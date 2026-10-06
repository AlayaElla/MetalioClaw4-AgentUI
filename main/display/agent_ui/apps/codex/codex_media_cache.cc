#include "codex_media_cache.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include "cJSON.h"

namespace agent_ui::codex_media {
namespace {
std::atomic_size_t g_image_bytes{0};
std::atomic_size_t g_image_peak_bytes{0};
std::atomic_size_t g_image_rejections{0};
#if defined(CODEX_MEDIA_CACHE_TESTING)
std::atomic_bool g_fail_image_allocation_for_test{false};
#endif

size_t ImageCharge(size_t payload_bytes) {
    constexpr size_t kCacheLineBytes = 128;
    if (payload_bytes > std::numeric_limits<size_t>::max() - (2 * kCacheLineBytes - 1)) return 0;
    return ((payload_bytes + kCacheLineBytes - 1) / kCacheLineBytes) * kCacheLineBytes + kCacheLineBytes;
}

void UpdatePeak(size_t current) {
    size_t peak = g_image_peak_bytes.load(std::memory_order_relaxed);
    while (peak < current && !g_image_peak_bytes.compare_exchange_weak(
               peak, current, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

bool TryReserveImageBytes(size_t charge) {
    if (charge == 0 || charge > Cache::kGlobalImageBudgetBytes) return false;
    size_t current = g_image_bytes.load(std::memory_order_relaxed);
    for (;;) {
        if (current > Cache::kGlobalImageBudgetBytes - charge) return false;
        if (g_image_bytes.compare_exchange_weak(current, current + charge,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            UpdatePeak(current + charge);
            return true;
        }
    }
}

bool AllocatePixels(PixelBuffer& pixels, size_t size) {
#if defined(CODEX_MEDIA_CACHE_TESTING)
    if (g_fail_image_allocation_for_test.load(std::memory_order_relaxed)) return false;
#endif
    return pixels.Allocate(size);
}

void ReleaseImageBytes(size_t charge) {
    if (charge != 0) g_image_bytes.fetch_sub(charge, std::memory_order_acq_rel);
}

class ImageReservation {
public:
    explicit ImageReservation(size_t charge) : charge_(charge) {}
    ~ImageReservation() { ReleaseImageBytes(charge_); }
    void Commit(Image* image) {
        image->budget_charge_bytes = charge_;
        charge_ = 0;
    }
private:
    size_t charge_;
};

size_t BoundedLength(const char* value, size_t maximum) {
    if (!value) return 0;
    size_t length = 0;
    while (length < maximum && value[length] != '\0') ++length;
    return length;
}
bool ReadBoundedString(const cJSON* root, const char* key, size_t maximum, std::string* output) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!output || !cJSON_IsString(value) || !value->valuestring) return false;
    const size_t length = BoundedLength(value->valuestring, maximum + 1);
    if (length > maximum) return false;
    output->assign(value->valuestring, length);
    return true;
}
int Number(const cJSON* root, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(value) && value->valuedouble == value->valueint ? value->valueint : -1;
}
int Base64Digit(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

bool MeasureBase64(const char* input, size_t size, size_t* decoded_size) {
    if (!input || !decoded_size || size > 10924 || size % 4 != 0) return false;
    size_t output_size = 0;
    for (size_t i = 0; i < size; i += 4) {
        const int a = Base64Digit(input[i]), b = Base64Digit(input[i + 1]);
        const bool pad_c = input[i + 2] == '=', pad_d = input[i + 3] == '=';
        const int c = pad_c ? 0 : Base64Digit(input[i + 2]);
        const int d = pad_d ? 0 : Base64Digit(input[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (pad_c && !pad_d) || ((pad_c || pad_d) && i + 4 != size)) return false;
        output_size += pad_c ? 1 : pad_d ? 2 : 3;
        if (output_size > 8192) return false;
    }
    *decoded_size = output_size;
    return true;
}

void DecodeBase64(const char* input, size_t size, uint8_t* output) {
    size_t offset = 0;
    for (size_t i = 0; i < size; i += 4) {
        const int a = Base64Digit(input[i]), b = Base64Digit(input[i + 1]);
        const bool pad_c = input[i + 2] == '=', pad_d = input[i + 3] == '=';
        const int c = pad_c ? 0 : Base64Digit(input[i + 2]);
        const int d = pad_d ? 0 : Base64Digit(input[i + 3]);
        output[offset++] = static_cast<uint8_t>((a << 2) | (b >> 4));
        if (!pad_c) output[offset++] = static_cast<uint8_t>((b << 4) | (c >> 2));
        if (!pad_d) output[offset++] = static_cast<uint8_t>((c << 6) | d);
    }
}
}  // namespace
Image::~Image() {
    pixels.Reset();
    ReleaseImageBytes(budget_charge_bytes);
}

std::string Cache::Key(const std::string& thread, const std::string& media, const std::string& variant) {
    // IDs originate on the PC but are still untrusted transport data.  A
    // length-prefixed key cannot alias when an ID itself contains ':' or any
    // other printable separator.
    return std::to_string(thread.size()) + ":" + thread +
           std::to_string(media.size()) + ":" + media +
           std::to_string(variant.size()) + ":" + variant;
}
ImageMemoryStats Cache::GetImageMemoryStats() {
    return {g_image_bytes.load(std::memory_order_relaxed),
            g_image_peak_bytes.load(std::memory_order_relaxed),
            g_image_rejections.load(std::memory_order_relaxed)};
}
#if defined(CODEX_MEDIA_CACHE_TESTING)
void Cache::SetImageAllocationFailureForTest(bool fail) {
    g_fail_image_allocation_for_test.store(fail, std::memory_order_relaxed);
}
#endif

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
    if (existing) {
        existing->request_id = request_id;
        existing->received = 0;
        existing->error.clear();
        existing->pending = true;
        existing->ready = false;
        existing->image.reset();
        existing->touched = ++tick_;
        return true;
    }
    while (!entries_.empty() && entries_.size() >= 16) {
        const auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) { return a.second->touched < b.second->touched; });
        entries_.erase(oldest);
    }
    auto entry = std::make_shared<Entry>();
    entry->thread_id = thread; entry->media_id = media; entry->variant = variant;
    entry->request_id = request_id; entry->pending = true; entry->touched = ++tick_;
    entries_[key] = entry;
    return true;
}
bool Cache::PruneOneUnreferencedImage(const std::string& except_key) {
    auto oldest = entries_.end();
    for (auto current = entries_.begin(); current != entries_.end(); ++current) {
        if (current->first == except_key || current->second.use_count() != 1 ||
            !current->second->image || current->second->image.use_count() != 1) continue;
        if (oldest == entries_.end() || current->second->touched < oldest->second->touched) oldest = current;
    }
    if (oldest == entries_.end()) return false;
    auto& entry = *oldest->second;
    entry.image.reset();
    entry.received = 0;
    entry.pending = false;
    entry.ready = false;
    entry.error = "图片缓存空间已回收，点击重试";
    entry.touched = ++tick_;
    return true;
}

void Cache::Fail(const std::string& key, const std::string& error) {
    auto entry = Get(key);
    if (!entry) return;
    entry->pending = false; entry->ready = false; entry->image.reset(); entry->received = 0;
    entry->error = error.empty() ? "图片加载失败，点击重试" : error;
}
bool Cache::Receive(const std::string& json) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(json.c_str()), cJSON_Delete);
    return Receive(root.get());
}
bool Cache::Receive(const cJSON* root) {
    if (!root) return false;
    std::string type, thread, media, variant, host, request;
    if (!ReadBoundedString(root, "type", 32, &type) ||
        !ReadBoundedString(root, "thread_id", 128, &thread) || thread.empty() ||
        !ReadBoundedString(root, "media_id", 192, &media) || media.empty() ||
        !ReadBoundedString(root, "variant", 5, &variant) ||
        !ReadBoundedString(root, "host_id", 16, &host) ||
        !ReadBoundedString(root, "request_id", 128, &request)) return false;
    const std::string key = Key(thread, media, variant);
    const auto found = entries_.find(key);
    auto entry = found == entries_.end() ? nullptr : found->second;
    if ((type != "codex_media_chunk" && type != "codex_media_error") || !entry || !entry->pending ||
        host != "local" || request != entry->request_id) return false;
    entry->touched = ++tick_;
    if (type == "codex_media_error") {
        Fail(key, "图片加载失败，点击重试"); return true;
    }
    const int width = Number(root, "width"), height = Number(root, "height"), total = Number(root, "total_bytes"), offset = Number(root, "offset");
    const int maximum = entry->variant == "thumb" ? 256 : 640;
    const cJSON* done = cJSON_GetObjectItemCaseSensitive(root, "done");
    const cJSON* encoded = cJSON_GetObjectItemCaseSensitive(root, "data");
    const char* data = cJSON_IsString(encoded) ? encoded->valuestring : nullptr;
    size_t encoded_size = 0;
    if (data) while (encoded_size <= 10924 && data[encoded_size] != '\0') ++encoded_size;
    size_t decoded_size = 0;
    std::string format;
    bool valid = cJSON_IsBool(done) && ReadBoundedString(root, "format", 16, &format) && format == "rgb565le" &&
        width > 0 && height > 0 && width <= maximum && height <= maximum &&
        total == width * height * 2 && offset >= 0 && static_cast<size_t>(offset) == entry->received &&
        data != nullptr && encoded_size <= 10924 && MeasureBase64(data, encoded_size, &decoded_size) &&
        decoded_size != 0 && entry->received + decoded_size <= static_cast<size_t>(total);
    if (valid && !entry->image) {
        const size_t charge = ImageCharge(static_cast<size_t>(total));
        bool reserved = TryReserveImageBytes(charge);
        while (!reserved && PruneOneUnreferencedImage(key)) reserved = TryReserveImageBytes(charge);
        if (!reserved) {
            g_image_rejections.fetch_add(1, std::memory_order_relaxed);
            Fail(key, "图片缓存已满，点击重试");
            return true;
        }
        ImageReservation reservation(charge);
        auto image = std::make_shared<Image>();
        bool allocated = AllocatePixels(image->pixels, static_cast<size_t>(total));
        while (!allocated && PruneOneUnreferencedImage(key))
            allocated = AllocatePixels(image->pixels, static_cast<size_t>(total));
        if (!allocated) {
            g_image_rejections.fetch_add(1, std::memory_order_relaxed);
            Fail(key, "图片内存不足，点击重试");
            return true;
        }
        reservation.Commit(image.get());
        image->width = width; image->height = height;
        entry->image = std::move(image);
    }
    valid = valid && entry->image->width == width && entry->image->height == height;
    if (!valid) { Fail(key, "图片数据不完整，点击重试"); return true; }
    DecodeBase64(data, encoded_size, entry->image->pixels.data() + offset);
    entry->received += decoded_size;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "done"))) {
        if (entry->received != static_cast<size_t>(total)) Fail(key, "图片数据不完整，点击重试");
        else { entry->pending = false; entry->ready = true; entry->error.clear(); }
    }
    return true;
}
void Cache::CancelPending() {
    for (auto& item : entries_) if (item.second->pending) Fail(item.first, "连接已变化，点击重试");
}
void Cache::Clear() { entries_.clear(); }
}  // namespace agent_ui::codex_media
