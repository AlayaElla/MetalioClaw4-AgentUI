#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agent_ui::codex_media {
struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;
};
struct Entry {
    std::string thread_id, media_id, variant, request_id, error;
    std::shared_ptr<Image> image;
    size_t received = 0;
    bool pending = false, ready = false;
    uint64_t touched = 0;
};
class Cache {
public:
    static std::string Key(const std::string& thread, const std::string& media, const std::string& variant);
    std::shared_ptr<Entry> Get(const std::string& key);
    bool Request(const std::string& thread, const std::string& media, const std::string& variant, const std::string& request_id);
    bool Receive(const std::string& json);
    void Fail(const std::string& key, const std::string& error);
    void CancelPending();
    void Clear();
private:
    std::map<std::string, std::shared_ptr<Entry>> entries_;
    uint64_t tick_ = 0;
};
}  // namespace agent_ui::codex_media
