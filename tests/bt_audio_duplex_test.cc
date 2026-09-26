#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <inttypes.h>
using namespace std::chrono_literals;
using gpio_num_t = int;
using i2s_chan_handle_t = void*;
using pcnt_unit_handle_t = void*;
using pcnt_channel_handle_t = void*;
using esp_err_t = int;
enum i2s_role_t { I2S_ROLE_SLAVE, I2S_ROLE_MASTER };
constexpr int GPIO_NUM_NC = -1, ESP_OK = 0, ESP_ERR_TIMEOUT = 1, kI2sIoTimeout = 200;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
int64_t esp_timer_get_time() { return 1; }
const char* esp_err_to_name(int) { return "mock"; }
enum class AudioOutputTarget { LocalSpeaker, BluetoothSpeaker };
AudioOutputTarget target = AudioOutputTarget::LocalSpeaker;
AudioOutputTarget AudioOutput_GetTarget() { return target; }
class AudioCodec {
protected:
    i2s_chan_handle_t tx_handle_ = reinterpret_cast<void*>(1);
    i2s_chan_handle_t rx_handle_ = reinterpret_cast<void*>(2);
    int output_volume_ = 70, output_sample_rate_ = 16000;
    virtual int Write(const int16_t*, int) = 0;
    virtual int Read(int16_t*, int) = 0;
public:
    virtual ~AudioCodec() = default;
    virtual void Start() {}
    virtual void SetOutputVolume(int) {}
    virtual void EnableOutput(bool) {}
    virtual bool SetOutputTransportEnabled(bool) { return true; }
};
#include "bt_audio_codec_host.h"

struct DriverGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool blocked = false, entered = false;
    void Reset(bool block) { std::lock_guard<std::mutex> lock(mutex); blocked = block; entered = false; }
    void Enter() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return !blocked; });
    }
    bool Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, 500ms, [&] { return entered; });
    }
    void Release() { std::lock_guard<std::mutex> lock(mutex); blocked = false; cv.notify_all(); }
} input, output;
std::atomic<int> active_io{0}, rebuilds{0}, unsafe_rebuilds{0};
std::atomic<bool> timeout_output{false};
esp_err_t i2s_channel_read(void*, void* data, size_t size, size_t* read, int) {
    ++active_io;
    input.Enter();
    std::fill_n(static_cast<int32_t*>(data), size / sizeof(int32_t), 4096);
    *read = size;
    --active_io;
    return ESP_OK;
}
esp_err_t i2s_channel_write(void*, const void*, size_t size, size_t* written, int) {
    ++active_io;
    output.Enter();
    if (timeout_output) std::this_thread::sleep_for(5ms);
    *written = timeout_output ? 0 : size;
    --active_io;
    return timeout_output ? ESP_ERR_TIMEOUT : ESP_OK;
}
BTAudioCodec::~BTAudioCodec() = default;
void BTAudioCodec::Start() {}
void BTAudioCodec::SetOutputVolume(int) {}
void BTAudioCodec::EnableOutput(bool) {}
bool BTAudioCodec::BeginWsClockProbe(int64_t&) { return false; }
int BTAudioCodec::ReadWsClockProbe(int64_t, int64_t&, int64_t&) { return -1; }
void BTAudioCodec::DeleteI2sChannels() {
    if (active_io != 0) ++unsafe_rebuilds;
    ++rebuilds;
    tx_handle_ = rx_handle_ = nullptr;
}
bool BTAudioCodec::ConfigureI2sChannels(i2s_role_t role, bool) {
    tx_handle_ = reinterpret_cast<void*>(1);
    rx_handle_ = reinterpret_cast<void*>(2);
    clock_role_ = role;
    return true;
}
#include "bt_audio_codec_host.inc"
class TestCodec : public BTAudioCodec {
public:
    int Capture() { int16_t data[16]; return Read(data, 16); }
    int Play() { const int16_t data[16] = {1}; return Write(data, 16); }
    bool Reconfigure() { return SetI2sClockRole(I2S_ROLE_MASTER); }
};
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
    if (!ok) ++failures;
}
int main() {
    {
        TestCodec codec;
        input.Reset(true); output.Reset(false);
        auto capture = std::async(std::launch::async, [&] { return codec.Capture(); });
        Check(input.Wait(), "capture entered I2S");
        auto playback = std::async(std::launch::async, [&] { return codec.Play(); });
        const bool concurrent = playback.wait_for(300ms) == std::future_status::ready;
        input.Release();
        Check(capture.get() == 16 && playback.get() == 16 && concurrent,
              "playback progresses while microphone read waits");
    }
    {
        TestCodec codec;
        input.Reset(false); output.Reset(true);
        auto playback = std::async(std::launch::async, [&] { return codec.Play(); });
        Check(output.Wait(), "playback entered I2S");
        auto capture = std::async(std::launch::async, [&] { return codec.Capture(); });
        const bool concurrent = capture.wait_for(300ms) == std::future_status::ready;
        output.Release();
        Check(capture.get() == 16 && playback.get() == 16 && concurrent,
              "microphone progresses while playback waits");
    }
    for (bool release_input_first : {true, false}) {
        TestCodec codec;
        input.Reset(true); output.Reset(true);
        auto capture = std::async(std::launch::async, [&] { return codec.Capture(); });
        input.Wait();
        auto playback = std::async(std::launch::async, [&] { return codec.Play(); });
        if (!output.Wait()) {
            input.Release(); output.Release();
            capture.get(); playback.get();
            Check(false, "both I2S directions can be active");
        } else {
            const int before = rebuilds;
            auto route = std::async(std::launch::async, [&] { return codec.Reconfigure(); });
            Check(route.wait_for(100ms) == std::future_status::timeout && rebuilds == before,
                  "route change waits for active I/O");
            if (release_input_first) { input.Release(); capture.get(); }
            else { output.Release(); playback.get(); }
            Check(route.wait_for(100ms) == std::future_status::timeout && rebuilds == before,
                  "route change also waits for the other I/O direction");
            if (release_input_first) { output.Release(); playback.get(); }
            else { input.Release(); capture.get(); }
            Check(route.get() && rebuilds == before + 1 && unsafe_rebuilds == 0,
                  "route rebuild occurs only after both directions finish");
        }
    }
    {
        TestCodec codec;
        input.Reset(false); output.Reset(false); timeout_output = true;
        auto playback = std::async(std::launch::async, [&] { return codec.Play(); });
        output.Wait();
        auto capture = std::async(std::launch::async, [&] { return codec.Capture(); });
        const bool concurrent = capture.wait_for(300ms) == std::future_status::ready;
        codec.SetOutputTransportEnabled(false);
        Check(playback.get() == 0 && capture.get() == 16 && concurrent,
              "missing output clock preserves input and route mute cancels retry");
        timeout_output = false;
        target = AudioOutputTarget::BluetoothSpeaker;
        Check(codec.SetOutputTransportEnabled(true) && codec.Play() == 16,
              "playback resumes after clock role switch");
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
