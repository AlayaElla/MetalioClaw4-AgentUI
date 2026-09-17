#include "codex_media_cache.h"

#include <algorithm>
#include <cstring>
#include "cJSON.h"

namespace agent_ui::codex_media {
namespace {
std::string String(const cJSON* root, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}
int Number(const cJSON* root, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(value) && value->valuedouble == value->valueint ? value->valueint : -1;
}
bool Decode(const std::string& input, std::vector<uint8_t>* output) {
    if (input.size() > 10924 || input.size() % 4 != 0) return false;
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    for (size_t i = 0; i < input.size(); i += 4) {
        const int a = digit(input[i]), b = digit(input[i + 1]);
        const int c = input[i + 2] == '=' ? 0 : digit(input[i + 2]);
        const int d = input[i + 3] == '=' ? 0 : digit(input[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (input[i + 2] == '=' && input[i + 3] != '=') ||
            ((input[i + 2] == '=' || input[i + 3] == '=') && i + 4 != input.size())) return false;
        output->push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        if (input[i + 2] != '=') output->push_back(static_cast<uint8_t>((b << 4) | (c >> 2)));
        if (input[i + 3] != '=') output->push_back(static_cast<uint8_t>((c << 6) | d));
    }
    return output->size() <= 8192;
}
}  // namespace
std::string Cache::Key(const std::string& thread, const std::string& media, const std::string& variant) {
    // IDs originate on the PC but are still untrusted transport data.  A
    // length-prefixed key cannot alias when an ID itself contains ':' or any
    // other printable separator.
    return std::to_string(thread.size()) + ":" + thread +
           std::to_string(media.size()) + ":" + media +
           std::to_string(variant.size()) + ":" + variant;
}
std::shared_ptr<Entry> Cache::Get(const std::string& key) {
    const auto found = entries_.find(key);
    if (found == entries_.end()) return nullptr;
    found->second->touched = ++tick_;
    return found->second;
}
bool Cache::Request(const std::string& thread, const std::string& media, const std::string& variant, const std::string& request_id) {
    if (thread.empty() || thread.size() > 128 || media.empty() || media.size() > 192 || request_id.empty() || request_id.size() > 128 ||
        (variant != "thumb" && variant != "full")) return false;
    const auto key = Key(thread, media, variant);
    const auto existing = Get(key);
    if (existing && (existing->pending || existing->ready)) return false;
    size_t bytes = 0;
    for (const auto& entry : entries_) if (entry.second->image) bytes += entry.second->image->pixels.size();
    while (!entries_.empty() && (entries_.size() >= 16 || bytes > 3 * 1024 * 1024)) {
        const auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) { return a.second->touched < b.second->touched; });
        if (oldest->second->image) bytes -= oldest->second->image->pixels.size();
        entries_.erase(oldest);
    }
    auto entry = std::make_shared<Entry>();
    entry->thread_id = thread; entry->media_id = media; entry->variant = variant;
    entry->request_id = request_id; entry->pending = true; entry->touched = ++tick_;
    entries_[key] = entry;
    return true;
}
void Cache::Fail(const std::string& key, const std::string& error) {
    auto entry = Get(key);
    if (!entry) return;
    entry->pending = false; entry->ready = false; entry->image.reset(); entry->received = 0;
    entry->error = error.empty() ? "图片加载失败，点击重试" : error;
}
bool Cache::Receive(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return false;
    const std::string type = String(root, "type");
    const std::string key = Key(String(root, "thread_id"), String(root, "media_id"), String(root, "variant"));
    const auto found = entries_.find(key);
    auto entry = found == entries_.end() ? nullptr : found->second;
    if ((type != "codex_media_chunk" && type != "codex_media_error") || !entry || !entry->pending ||
        String(root, "host_id") != "local" || String(root, "request_id") != entry->request_id) { cJSON_Delete(root); return false; }
    entry->touched = ++tick_;
    if (type == "codex_media_error") {
        Fail(key, "图片加载失败，点击重试"); cJSON_Delete(root); return true;
    }
    const int width = Number(root, "width"), height = Number(root, "height"), total = Number(root, "total_bytes"), offset = Number(root, "offset");
    const int maximum = entry->variant == "thumb" ? 256 : 640;
    std::vector<uint8_t> chunk;
    const cJSON* done = cJSON_GetObjectItemCaseSensitive(root, "done");
    bool valid = cJSON_IsBool(done) && String(root, "format") == "rgb565le" && width > 0 && height > 0 && width <= maximum && height <= maximum &&
        total == width * height * 2 && offset >= 0 && static_cast<size_t>(offset) == entry->received &&
        Decode(String(root, "data"), &chunk) && !chunk.empty() && entry->received + chunk.size() <= static_cast<size_t>(total);
    if (valid && !entry->image) {
        entry->image = std::make_shared<Image>(); entry->image->width = width; entry->image->height = height;
        entry->image->pixels.resize(total);
    }
    valid = valid && entry->image->width == width && entry->image->height == height;
    if (!valid) { Fail(key, "图片数据不完整，点击重试"); cJSON_Delete(root); return true; }
    std::copy(chunk.begin(), chunk.end(), entry->image->pixels.begin() + offset);
    entry->received += chunk.size();
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "done"))) {
        if (entry->received != static_cast<size_t>(total)) Fail(key, "图片数据不完整，点击重试");
        else { entry->pending = false; entry->ready = true; entry->error.clear(); }
    }
    cJSON_Delete(root); return true;
}
void Cache::CancelPending() {
    for (auto& item : entries_) if (item.second->pending) Fail(item.first, "连接已变化，点击重试");
}
void Cache::Clear() { entries_.clear(); }
}  // namespace agent_ui::codex_media
