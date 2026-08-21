#include "bt_audio_codec.h"
#include "audio_output_route.h"

#include <algorithm>
#include <esp_log.h>
#include <esp_timer.h>
#include <cmath>
#include <cstring>
#include <inttypes.h>

#define TAG "BTAudioCodec"

namespace {
constexpr TickType_t kI2sIoTimeout = pdMS_TO_TICKS(200);
}

BTAudioCodec::~BTAudioCodec()
{
    DeinitializeWsClockProbe();
    DeleteI2sChannels();
}

void BTAudioCodec::Start()
{
    AudioCodec::Start();
    channels_started_ = true;
}

void BTAudioCodec::SetOutputVolume(int volume)
{
    AudioCodec::SetOutputVolume(volume);
    AudioOutput_NotifyVolumeChanged(output_volume_);
}

void BTAudioCodec::EnableOutput(bool enable)
{
    if (enable == output_enabled_) {
        return;
    }
    AudioCodec::EnableOutput(enable);
    AudioOutput_SetCodecEnabled(enable);
    if (!enable) {
        output_started_logged_.store(false, std::memory_order_release);
        output_timeout_logged_.store(false, std::memory_order_release);
    }
}

bool BTAudioCodec::SetOutputTransportEnabled(bool enabled)
{
    const bool previous = output_transport_requested_.exchange(
        false, std::memory_order_acq_rel);
    bool ready = true;
    if (enabled) {
        const i2s_role_t desired_role =
            AudioOutput_GetTarget() == AudioOutputTarget::BluetoothSpeaker
                ? I2S_ROLE_MASTER
                : I2S_ROLE_SLAVE;
        ready = SetI2sClockRole(desired_role);
        output_transport_requested_.store(ready, std::memory_order_release);
    }
    output_started_logged_.store(false, std::memory_order_release);
    output_timeout_logged_.store(false, std::memory_order_release);
    last_write_success_us_.store(0, std::memory_order_release);
    ESP_LOGI(TAG,
             "I2S output software gate %s -> %s; duplex channels remain "
             "enabled, expected_ws=%dHz expected_bclk=%dHz",
             previous ? "open" : "muted", ready && enabled ? "open" : "muted",
             output_sample_rate_, output_sample_rate_ * 2 * 32);
    return ready;
}

void BTAudioCodec::DeleteI2sChannels()
{
    auto stop_and_delete = [](i2s_chan_handle_t& handle) {
        if (handle == nullptr) return;
        const esp_err_t disable_error = i2s_channel_disable(handle);
        if (disable_error != ESP_OK && disable_error != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "I2S channel disable failed: %s",
                     esp_err_to_name(disable_error));
        }
        const esp_err_t delete_error = i2s_del_channel(handle);
        if (delete_error != ESP_OK) {
            ESP_LOGE(TAG, "I2S channel delete failed: %s",
                     esp_err_to_name(delete_error));
        }
        handle = nullptr;
    };
    stop_and_delete(rx_handle_);
    stop_and_delete(tx_handle_);
}

bool BTAudioCodec::ConfigureI2sChannels(i2s_role_t role,
                                        bool start_channels)
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = role,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    esp_err_t error = i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create %s I2S channels: %s",
                 role == I2S_ROLE_MASTER ? "master" : "slave",
                 esp_err_to_name(error));
        tx_handle_ = nullptr;
        rx_handle_ = nullptr;
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(
            static_cast<uint32_t>(output_sample_rate_)),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk_,
            .ws = ws_,
            .dout = dout_,
            .din = din_,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    error = i2s_channel_init_std_mode(tx_handle_, &std_cfg);
    if (error == ESP_OK) {
        error = i2s_channel_init_std_mode(rx_handle_, &std_cfg);
    }
    if (error == ESP_OK && start_channels) {
        error = i2s_channel_enable(tx_handle_);
        if (error == ESP_OK) error = i2s_channel_enable(rx_handle_);
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize %s I2S channels: %s",
                 role == I2S_ROLE_MASTER ? "master" : "slave",
                 esp_err_to_name(error));
        DeleteI2sChannels();
        return false;
    }

    clock_role_ = role;
    ESP_LOGI(TAG,
             "I2S clock role configured: role=%s sample_rate=%dHz "
             "expected_ws=%dHz expected_bclk=%dHz pins(bclk=%d ws=%d)",
             role == I2S_ROLE_MASTER ? "master" : "slave",
             output_sample_rate_, output_sample_rate_,
             output_sample_rate_ * 2 * 32, static_cast<int>(bclk_),
             static_cast<int>(ws_));
    return true;
}

bool BTAudioCodec::SetI2sClockRole(i2s_role_t role)
{
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (tx_handle_ != nullptr && rx_handle_ != nullptr &&
        clock_role_ == role) {
        return true;
    }

    const i2s_role_t previous_role = clock_role_;
    DeleteI2sChannels();
    if (ConfigureI2sChannels(role, channels_started_)) return true;

    ESP_LOGE(TAG, "Restoring previous I2S clock role after reconfiguration failure");
    if (!ConfigureI2sChannels(previous_role, channels_started_)) {
        ESP_LOGE(TAG, "Failed to restore previous I2S clock role");
    }
    return false;
}

void BTAudioCodec::InitializeWsClockProbe(gpio_num_t ws)
{
    pcnt_unit_config_t unit_config = {
        .low_limit = -1,
        .high_limit = 32767,
        .intr_priority = 0,
        .flags = {},
    };
    esp_err_t error = pcnt_new_unit(&unit_config, &ws_counter_unit_);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "WS clock probe unavailable: pcnt_new_unit failed: %s",
                 esp_err_to_name(error));
        ws_counter_unit_ = nullptr;
        return;
    }

    pcnt_chan_config_t channel_config = {
        .edge_gpio_num = static_cast<int>(ws),
        .level_gpio_num = -1,
        .flags = {},
    };
    error = pcnt_new_channel(ws_counter_unit_, &channel_config,
                             &ws_counter_channel_);
    if (error == ESP_OK) {
        error = pcnt_channel_set_edge_action(
            ws_counter_channel_, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
            PCNT_CHANNEL_EDGE_ACTION_HOLD);
    }
    if (error == ESP_OK) {
        error = pcnt_channel_set_level_action(
            ws_counter_channel_, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
            PCNT_CHANNEL_LEVEL_ACTION_KEEP);
    }
    if (error == ESP_OK) {
        error = pcnt_unit_enable(ws_counter_unit_);
    }
    if (error == ESP_OK) {
        error = pcnt_unit_clear_count(ws_counter_unit_);
    }
    if (error == ESP_OK) {
        error = pcnt_unit_start(ws_counter_unit_);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "WS clock probe initialization failed: %s",
                 esp_err_to_name(error));
        if (ws_counter_unit_ != nullptr) {
            pcnt_unit_stop(ws_counter_unit_);
            pcnt_unit_disable(ws_counter_unit_);
        }
        if (ws_counter_channel_ != nullptr) {
            pcnt_del_channel(ws_counter_channel_);
            ws_counter_channel_ = nullptr;
        }
        if (ws_counter_unit_ != nullptr) {
            pcnt_del_unit(ws_counter_unit_);
            ws_counter_unit_ = nullptr;
        }
        return;
    }

    ESP_LOGI(TAG,
             "WS clock probe ready on GPIO %d; counting rising edges during "
             "each I2S write window",
             static_cast<int>(ws));
}

void BTAudioCodec::DeinitializeWsClockProbe()
{
    if (ws_counter_unit_ == nullptr) return;
    pcnt_unit_stop(ws_counter_unit_);
    pcnt_unit_disable(ws_counter_unit_);
    if (ws_counter_channel_ != nullptr) {
        pcnt_del_channel(ws_counter_channel_);
        ws_counter_channel_ = nullptr;
    }
    pcnt_del_unit(ws_counter_unit_);
    ws_counter_unit_ = nullptr;
}

bool BTAudioCodec::BeginWsClockProbe(int64_t& started_us)
{
    started_us = esp_timer_get_time();
    return ws_counter_unit_ != nullptr &&
           pcnt_unit_clear_count(ws_counter_unit_) == ESP_OK;
}

int BTAudioCodec::ReadWsClockProbe(int64_t started_us, int64_t& window_us,
                                   int64_t& observed_hz)
{
    window_us = esp_timer_get_time() - started_us;
    observed_hz = -1;
    if (ws_counter_unit_ == nullptr) return -1;

    int edges = -1;
    if (pcnt_unit_get_count(ws_counter_unit_, &edges) != ESP_OK) return -1;
    if (window_us > 0) {
        observed_hz = static_cast<int64_t>(edges) * 1000000 / window_us;
    }
    return edges;
}


BTAudioCodecDuplex::BTAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din)
{
    duplex_ = true;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    input_reference_ = true;
    input_channels_ = input_reference_ ? 2 : 1;
    bclk_ = bclk;
    ws_ = ws;
    dout_ = dout;
    din_ = din;
    ESP_ERROR_CHECK(ConfigureI2sChannels(I2S_ROLE_SLAVE, false) ? ESP_OK
                                                                : ESP_FAIL);
    InitializeWsClockProbe(ws);
    ESP_LOGI(TAG,
             "I2S config: port=0 role=slave duplex=1 sample_rate=%dHz "
             "data_bits=32 slots=2 slot_bits=32 expected_bclk=%dHz "
             "mclk=unused pins(bclk=%d ws=%d dout=%d din=%d)",
             output_sample_rate_, output_sample_rate_ * 2 * 32,
             static_cast<int>(bclk), static_cast<int>(ws),
             static_cast<int>(dout), static_cast<int>(din));
}

int BTAudioCodec::Write(const int16_t *data, int samples)
{
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (!output_transport_requested_.load(std::memory_order_acquire)) {
        return 0;
    }
    std::vector<int32_t> buffer(samples * 2);

    int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536;
    int pcm_peak = 0;

    for (int i = 0; i < samples; i++)
    {
        const int sample = data[i];
        pcm_peak = std::max(pcm_peak, sample < 0 ? -sample : sample);
        int64_t temp = int64_t(data[i]) * volume_factor;
        int32_t processed_sample;

        if (temp > INT32_MAX)
        {
            processed_sample = INT32_MAX;
        }
        else if (temp < INT32_MIN)
        {
            processed_sample = INT32_MIN;
        }
        else
        {
            processed_sample = static_cast<int32_t>(temp);
        }

        buffer[i * 2] = processed_sample;
        buffer[i * 2 + 1] = processed_sample;
    }

    while (output_transport_requested_.load(std::memory_order_acquire)) {
        int64_t probe_started_us = 0;
        const bool probe_active = BeginWsClockProbe(probe_started_us);
        size_t bytes_written = 0;
        const esp_err_t error = i2s_channel_write(
            tx_handle_, buffer.data(), samples * 2 * sizeof(int32_t),
            &bytes_written, kI2sIoTimeout);
        int64_t probe_window_us = -1;
        int64_t observed_ws_hz = -1;
        const int observed_ws_edges = probe_active
            ? ReadWsClockProbe(probe_started_us, probe_window_us,
                               observed_ws_hz)
            : -1;
        if (error == ESP_OK) {
            const int64_t now_us = esp_timer_get_time();
            const bool clock_was_paused = output_timeout_logged_.exchange(
                false, std::memory_order_acq_rel);
            last_write_success_us_.store(now_us, std::memory_order_release);
            if (clock_was_paused) {
                ESP_LOGI(TAG,
                         "I2S output clock resumed: %u bytes written, "
                         "expected_ws=%dHz expected_bclk=%dHz",
                         static_cast<unsigned>(bytes_written),
                         output_sample_rate_, output_sample_rate_ * 2 * 32);
            }
            if (!output_started_logged_.exchange(
                    true, std::memory_order_acq_rel)) {
                ESP_LOGI(TAG,
                         "I2S output active: %u bytes written, pcm_peak=%d "
                         "volume=%d expected_ws=%dHz expected_bclk=%dHz "
                         "observed_ws_edges=%d observed_window_us=%" PRIi64
                         " observed_ws_hz=%" PRIi64,
                         static_cast<unsigned>(bytes_written), pcm_peak,
                         output_volume_, output_sample_rate_,
                         output_sample_rate_ * 2 * 32, observed_ws_edges,
                         probe_window_us, observed_ws_hz);
            }
            return bytes_written / (2 * sizeof(int32_t));
        }
        if (error == ESP_ERR_TIMEOUT) {
            if (!output_timeout_logged_.exchange(
                    true, std::memory_order_acq_rel)) {
                const int64_t now_us = esp_timer_get_time();
                const int64_t last_success_us = last_write_success_us_.load(
                    std::memory_order_acquire);
                const int64_t clock_absent_ms = last_success_us > 0
                    ? (now_us - last_success_us) / 1000
                    : -1;
                ESP_LOGW(TAG,
                         "I2S output stalled; PCM may remain queued in DMA "
                         "until the route changes: copied=%u requested=%u "
                         "bytes clock_absent_ms=%" PRIi64 " expected_ws=%dHz "
                         "expected_bclk=%dHz observed_ws_edges=%d "
                         "observed_window_us=%" PRIi64 " observed_ws_hz=%" PRIi64
                         " clock_state=%s",
                         static_cast<unsigned>(bytes_written),
                         static_cast<unsigned>(samples * 2 * sizeof(int32_t)),
                         clock_absent_ms, output_sample_rate_,
                         output_sample_rate_ * 2 * 32, observed_ws_edges,
                         probe_window_us, observed_ws_hz,
                         observed_ws_edges < 0 ? "probe_unavailable" :
                         observed_ws_edges == 0 ? "absent" :
                         "present_dma_stalled");
              }
            continue;
        }
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(error));
        return 0;
    }
    return 0;
}

int BTAudioCodec::Read(int16_t *dest, int samples)
{
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    size_t bytes_read;

    std::vector<int32_t> bit32_buffer(samples);
    const esp_err_t error = i2s_channel_read(
        rx_handle_, bit32_buffer.data(), samples * sizeof(int32_t), &bytes_read,
        kI2sIoTimeout);
    if (error != ESP_OK)
    {
        if (error != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(error));
        }
        return 0;
    }

    samples = bytes_read / sizeof(int32_t);
    for (int i = 0; i < samples; i++)
    {
        int32_t value = bit32_buffer[i] >> 12;
        dest[i] = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? -INT16_MAX
                                                                         : (int16_t)value;
    }
    return samples;
}
