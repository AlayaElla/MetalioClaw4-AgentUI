#include "bt_audio_codec.h"
#include "audio_output_route.h"

#include <algorithm>
#include <esp_log.h>
#include <cmath>
#include <cstring>

#define TAG "BTAudioCodec"

namespace {
constexpr TickType_t kI2sIoTimeout = pdMS_TO_TICKS(200);
}

BTAudioCodec::~BTAudioCodec()
{
    if (rx_handle_ != nullptr)
    {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    if (tx_handle_ != nullptr)
    {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
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
        output_started_logged_ = false;
        output_timeout_logged_ = false;
    }
}


BTAudioCodecDuplex::BTAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din)
{
    duplex_ = true;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    input_reference_ = true;
    input_channels_ = input_reference_ ? 2 : 1;

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_SLAVE,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(
            static_cast<uint32_t>(output_sample_rate_)),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Duplex channels created");
}

int BTAudioCodec::Write(const int16_t *data, int samples)
{
    std::lock_guard<std::mutex> lock(data_if_mutex_);
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

    size_t bytes_written = 0;
    const esp_err_t error = i2s_channel_write(
        tx_handle_, buffer.data(), samples * 2 * sizeof(int32_t),
        &bytes_written, kI2sIoTimeout);
    if (error != ESP_OK) {
        if (error == ESP_ERR_TIMEOUT) {
            if (!output_timeout_logged_) {
                ESP_LOGW(TAG,
                         "I2S output timed out; external audio clock is "
                         "unavailable");
                output_timeout_logged_ = true;
            }
        } else {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(error));
        }
        return 0;
    }
    output_timeout_logged_ = false;
    if (!output_started_logged_) {
        ESP_LOGI(TAG,
                 "I2S output active: %u bytes written, pcm_peak=%d volume=%d",
                 static_cast<unsigned>(bytes_written), pcm_peak,
                 output_volume_);
        output_started_logged_ = true;
    }
    return bytes_written / (2 * sizeof(int32_t));
}

int BTAudioCodec::Read(int16_t *dest, int samples)
{
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
