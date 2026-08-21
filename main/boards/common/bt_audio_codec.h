#ifndef _NO_AUDIO_CODEC_H
#define _NO_AUDIO_CODEC_H

#include "audio_codec.h"

#include <driver/gpio.h>
#include <driver/i2s_pdm.h>
#include <driver/pulse_cnt.h>
#include <atomic>
#include <cstdint>
#include <mutex>

class BTAudioCodec : public AudioCodec {
protected:
    std::mutex data_if_mutex_;
    std::atomic_bool output_started_logged_{false};
    std::atomic_bool output_timeout_logged_{false};
    std::atomic_bool output_transport_requested_{true};
    std::atomic<int64_t> last_write_success_us_{0};
    pcnt_unit_handle_t ws_counter_unit_ = nullptr;
    pcnt_channel_handle_t ws_counter_channel_ = nullptr;
    gpio_num_t bclk_ = GPIO_NUM_NC;
    gpio_num_t ws_ = GPIO_NUM_NC;
    gpio_num_t dout_ = GPIO_NUM_NC;
    gpio_num_t din_ = GPIO_NUM_NC;
    i2s_role_t clock_role_ = I2S_ROLE_SLAVE;
    bool channels_started_ = false;

    bool ConfigureI2sChannels(i2s_role_t role, bool start_channels);
    void DeleteI2sChannels();
    bool SetI2sClockRole(i2s_role_t role);
    void InitializeWsClockProbe(gpio_num_t ws);
    void DeinitializeWsClockProbe();
    bool BeginWsClockProbe(int64_t& started_us);
    int ReadWsClockProbe(int64_t started_us, int64_t& window_us,
                         int64_t& observed_hz);

    virtual int Write(const int16_t* data, int samples) override;
    virtual int Read(int16_t* dest, int samples) override;

public:
    virtual ~BTAudioCodec();
    virtual void Start() override;
    virtual void SetOutputVolume(int volume) override;
    virtual void EnableOutput(bool enable) override;
    virtual bool SetOutputTransportEnabled(bool enabled) override;
};

class BTAudioCodecDuplex : public BTAudioCodec {
public:
    BTAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
};


#endif // _NO_AUDIO_CODEC_H
