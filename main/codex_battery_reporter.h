#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace codex_battery {

struct Sample {
    bool available = false;
    int percentage = 0;
    bool charging = false;
};

// Called from the application loop, independently of the visible page. The
// connection generation also catches disconnect/reconnect between clock ticks.
class Reporter {
public:
    template <typename Read, typename Send>
    void Poll(uint64_t now_ms, bool connected, uint32_t connection,
              Read read, Send send) {
        if (!connected) {
            connected_ = false;
            return;
        }
        if (!connected_ || connection != connection_) {
            connected_ = true;
            connection_ = connection;
            sampled_ = false;
            last_payload_.clear();
        }
        if (sampled_ && now_ms - last_sample_ms_ < 5000) return;
        sampled_ = true;
        last_sample_ms_ = now_ms;

        const Sample sample = read();
        std::string payload = "{\"type\":\"device_battery\",\"available\":false}";
        if (sample.available && sample.percentage >= 0 && sample.percentage <= 100) {
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer),
                          "{\"type\":\"device_battery\",\"available\":true,"
                          "\"percentage\":%d,\"isCharging\":%s}",
                          sample.percentage, sample.charging ? "true" : "false");
            payload = buffer;
        }
        if (payload == last_payload_ && now_ms - last_sent_ms_ < 30000) return;
        if (send(payload)) {
            last_payload_ = payload;
            last_sent_ms_ = now_ms;
        }
    }

private:
    bool connected_ = false;
    bool sampled_ = false;
    uint32_t connection_ = 0;
    uint64_t last_sample_ms_ = 0;
    uint64_t last_sent_ms_ = 0;
    std::string last_payload_;
};

}  // namespace codex_battery
