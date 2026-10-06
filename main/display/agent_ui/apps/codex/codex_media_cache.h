#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include "cJSON.h"

namespace agent_ui::codex_media {
class PixelBuffer {
public:
    PixelBuffer() = default;
    PixelBuffer(const PixelBuffer&) = delete;
    PixelBuffer& operator=(const PixelBuffer&) = delete;

    bool Allocate(size_t size) noexcept {
        if (size == 0 || size > 640u * 640u * 2u) return false;
        std::unique_ptr<uint8_t[]> allocation(new (std::nothrow) uint8_t[size]);
        if (!allocation) return false;
        storage_ = std::move(allocation);
        size_ = size;
        return true;
    }
    void Reset() noexcept { storage_.reset(); size_ = 0; }
    uint8_t* data() noexcept { return storage_.get(); }
    const uint8_t* data() const noexcept { return storage_.get(); }
    uint8_t* begin() noexcept { return storage_.get(); }
    size_t size() const noexcept { return size_; }

private:
    std::unique_ptr<uint8_t[]> storage_;
    size_t size_ = 0;
};

struct Image {
    int width = 0, height = 0;
    PixelBuffer pixels;
    size_t budget_charge_bytes = 0;

    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image();
};
struct Entry {
    std::string thread_id, media_id, variant, request_id, error;
    std::shared_ptr<Image> image;
    size_t received = 0;
    bool pending = false, ready = false;
    uint64_t touched = 0;
};
struct ImageMemoryStats {
    size_t current_bytes = 0;
    size_t peak_bytes = 0;
    size_t rejected_allocations = 0;
};
class Cache {
public:
    static constexpr size_t kGlobalImageBudgetBytes = 3u * 1024u * 1024u;
    static std::string Key(const std::string& thread, const std::string& media, const std::string& variant);
    static ImageMemoryStats GetImageMemoryStats();
#if defined(CODEX_MEDIA_CACHE_TESTING)
    static void SetImageAllocationFailureForTest(bool fail);
#endif
    std::shared_ptr<Entry> Get(const std::string& key);
    bool Request(const std::string& thread, const std::string& media, const std::string& variant, const std::string& request_id);
    bool Receive(const cJSON* root);
    bool Receive(const std::string& json);
    void Fail(const std::string& key, const std::string& error);
    void CancelPending();
    void Clear();
private:
    bool PruneOneUnreferencedImage(const std::string& except_key);
    std::map<std::string, std::shared_ptr<Entry>> entries_;
    uint64_t tick_ = 0;
};
}  // namespace agent_ui::codex_media
