#include "codex_ws_pipeline.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codex_remote::pipeline {
namespace {
constexpr size_t kMaxJsonDepth = 64;
constexpr size_t kNodeDomCharge = 72;
constexpr size_t kNodeInternalCharge = 64;
constexpr size_t kAllocatorOverhead = 8;
constexpr size_t kMallocInternalThreshold = 4096;

bool IsWhitespace(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool Add(size_t* target, size_t value) {
    if (target == nullptr || value > std::numeric_limits<size_t>::max() - *target) return false;
    *target += value;
    return true;
}

bool ParseString(std::string_view json, size_t* offset, size_t* decoded_bytes) {
    if (offset == nullptr || decoded_bytes == nullptr || *offset >= json.size() ||
        json[*offset] != '"') return false;
    ++*offset;
    *decoded_bytes = 0;
    while (*offset < json.size()) {
        const char value = json[(*offset)++];
        if (value == '"') return true;
        if (static_cast<unsigned char>(value) < 0x20) return false;
        if (value != '\\') {
            if (!Add(decoded_bytes, 1)) return false;
            continue;
        }
        if (*offset >= json.size()) return false;
        const char escaped = json[(*offset)++];
        if (escaped == 'u') {
            if (json.size() - *offset < 4) return false;
            for (size_t i = 0; i < 4; ++i) {
                const char digit = json[*offset + i];
                if (!((digit >= '0' && digit <= '9') ||
                      (digit >= 'a' && digit <= 'f') ||
                      (digit >= 'A' && digit <= 'F'))) return false;
            }
            *offset += 4;
            // One escaped UTF-16 code unit decodes to at most three UTF-8 bytes.
            // Surrogate pairs are conservatively charged as six bytes.
            if (!Add(decoded_bytes, 3)) return false;
        } else if (escaped == '"' || escaped == '\\' || escaped == '/' ||
                   escaped == 'b' || escaped == 'f' || escaped == 'n' ||
                   escaped == 'r' || escaped == 't') {
            if (!Add(decoded_bytes, 1)) return false;
        } else {
            return false;
        }
    }
    return false;
}

bool AddStringFootprint(size_t decoded_bytes, Footprint* footprint) {
    size_t allocation = decoded_bytes;
    if (!Add(&allocation, 1 + kAllocatorOverhead) ||
        !Add(&footprint->strings, allocation) ||
        (allocation <= kMallocInternalThreshold &&
         !Add(&footprint->internal_preferred_bytes, allocation))) return false;
    return true;
}

bool AddNode(Footprint* footprint) {
    if (footprint == nullptr || !Add(&footprint->nodes, 1) ||
        !Add(&footprint->internal_preferred_bytes, kNodeInternalCharge)) return false;
    return true;
}

bool ParseDefault(const char* data, size_t length, cJSON** root) {
    if (data == nullptr || root == nullptr) return false;
    *root = cJSON_ParseWithLength(data, length);
    return *root != nullptr;
}

bool MeasureItem(const cJSON* item, size_t depth, Footprint* footprint) {
    if (item == nullptr || footprint == nullptr || depth > kMaxJsonDepth) return false;
    footprint->max_depth = std::max(footprint->max_depth, depth);
    if (!AddNode(footprint)) return false;
    if (item->string != nullptr &&
        !AddStringFootprint(std::strlen(item->string), footprint)) return false;
    if (cJSON_IsString(item) && item->valuestring != nullptr &&
        !AddStringFootprint(std::strlen(item->valuestring), footprint)) return false;
    for (const cJSON* child = item->child; child != nullptr; child = child->next) {
        if (!MeasureItem(child, depth + 1, footprint)) return false;
    }
    return true;
}

bool SameOrSmaller(const Footprint& candidate, const Footprint& original) {
    return candidate.nodes <= original.nodes && candidate.strings <= original.strings &&
           candidate.dom_bytes <= original.dom_bytes &&
           candidate.internal_preferred_bytes <= original.internal_preferred_bytes;
}

void SkipWhitespace(std::string_view json, size_t* offset) {
    while (*offset < json.size() && IsWhitespace(json[*offset])) ++*offset;
}

bool SkipStringToken(std::string_view json, size_t* offset) {
    if (offset == nullptr || *offset >= json.size() || json[*offset] != '"') return false;
    ++*offset;
    while (*offset < json.size()) {
        const unsigned char value = static_cast<unsigned char>(json[(*offset)++]);
        if (value == '"') return true;
        if (value < 0x20) return false;
        if (value != '\\') continue;
        if (*offset >= json.size()) return false;
        const char escaped = json[(*offset)++];
        if (escaped == 'u') {
            if (json.size() - *offset < 4) return false;
            for (size_t i = 0; i < 4; ++i) {
                const char digit = json[*offset + i];
                if (!((digit >= '0' && digit <= '9') ||
                      (digit >= 'a' && digit <= 'f') ||
                      (digit >= 'A' && digit <= 'F'))) return false;
            }
            *offset += 4;
        } else if (escaped != '"' && escaped != '\\' && escaped != '/' &&
                   escaped != 'b' && escaped != 'f' && escaped != 'n' &&
                   escaped != 'r' && escaped != 't') {
            return false;
        }
    }
    return false;
}

bool SkipJsonValue(std::string_view json, size_t* offset, size_t depth) {
    if (offset == nullptr || depth > kMaxJsonDepth) return false;
    SkipWhitespace(json, offset);
    if (*offset >= json.size()) return false;
    if (json[*offset] == '"') return SkipStringToken(json, offset);
    if (json[*offset] == '{') {
        ++*offset;
        SkipWhitespace(json, offset);
        if (*offset < json.size() && json[*offset] == '}') { ++*offset; return true; }
        while (*offset < json.size()) {
            SkipWhitespace(json, offset);
            if (!SkipStringToken(json, offset)) return false;
            SkipWhitespace(json, offset);
            if (*offset >= json.size() || json[(*offset)++] != ':') return false;
            if (!SkipJsonValue(json, offset, depth + 1)) return false;
            SkipWhitespace(json, offset);
            if (*offset >= json.size()) return false;
            const char separator = json[(*offset)++];
            if (separator == '}') return true;
            if (separator != ',') return false;
        }
        return false;
    }
    if (json[*offset] == '[') {
        ++*offset;
        SkipWhitespace(json, offset);
        if (*offset < json.size() && json[*offset] == ']') { ++*offset; return true; }
        while (*offset < json.size()) {
            if (!SkipJsonValue(json, offset, depth + 1)) return false;
            SkipWhitespace(json, offset);
            if (*offset >= json.size()) return false;
            const char separator = json[(*offset)++];
            if (separator == ']') return true;
            if (separator != ',') return false;
        }
        return false;
    }
    const size_t start = *offset;
    while (*offset < json.size() && !IsWhitespace(json[*offset]) &&
           json[*offset] != ',' && json[*offset] != ']' && json[*offset] != '}') ++*offset;
    return *offset > start;
}

bool FindTopLevelMember(std::string_view json, std::string_view key,
                        size_t* value_start, size_t* value_end) {
    if (json.empty() || json.size() > kMaxWireBytes || value_start == nullptr || value_end == nullptr) return false;
    size_t offset = 0;
    SkipWhitespace(json, &offset);
    if (offset >= json.size() || json[offset++] != '{') return false;
    const std::string expected = "\"" + std::string(key) + "\"";
    SkipWhitespace(json, &offset);
    if (offset < json.size() && json[offset] == '}') return false;
    while (offset < json.size()) {
        SkipWhitespace(json, &offset);
        const size_t key_start = offset;
        if (!SkipStringToken(json, &offset)) return false;
        const size_t key_end = offset;
        SkipWhitespace(json, &offset);
        if (offset >= json.size() || json[offset++] != ':') return false;
        SkipWhitespace(json, &offset);
        const size_t start = offset;
        if (!SkipJsonValue(json, &offset, 1)) return false;
        const size_t end = offset;
        if (json.substr(key_start, key_end - key_start) == expected) {
            *value_start = start;
            *value_end = end;
            return true;
        }
        SkipWhitespace(json, &offset);
        if (offset >= json.size()) return false;
        const char separator = json[offset++];
        if (separator == '}') return false;
        if (separator != ',') return false;
    }
    return false;
}
}  // namespace

bool RetainedBudget::TryReserve(const Footprint& footprint) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (footprint.dom_bytes > kMaxRetainedDomBytes - stats_.bytes ||
        footprint.nodes > kMaxRetainedNodes - stats_.nodes ||
        stats_.messages >= kMaxRetainedMessages) {
        ++stats_.rejected;
        return false;
    }
    stats_.bytes += footprint.dom_bytes;
    stats_.nodes += footprint.nodes;
    stats_.internal_preferred_bytes += footprint.internal_preferred_bytes;
    ++stats_.messages;
    stats_.high_water_bytes = std::max(stats_.high_water_bytes, stats_.bytes);
    stats_.high_water_nodes = std::max(stats_.high_water_nodes, stats_.nodes);
    stats_.high_water_messages = std::max(stats_.high_water_messages, stats_.messages);
    return true;
}

void RetainedBudget::Release(const Footprint& footprint) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.bytes -= std::min(stats_.bytes, footprint.dom_bytes);
    stats_.nodes -= std::min(stats_.nodes, footprint.nodes);
    stats_.internal_preferred_bytes -=
        std::min(stats_.internal_preferred_bytes, footprint.internal_preferred_bytes);
    if (stats_.messages != 0) --stats_.messages;
}

void RetainedBudget::ReleaseFootprint(const Footprint& footprint) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.bytes -= std::min(stats_.bytes, footprint.dom_bytes);
    stats_.nodes -= std::min(stats_.nodes, footprint.nodes);
    stats_.internal_preferred_bytes -=
        std::min(stats_.internal_preferred_bytes, footprint.internal_preferred_bytes);
}

ReservationStats RetainedBudget::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

Reservation::Reservation(RetainedBudget* budget, const Footprint& footprint)
    : budget_(budget), footprint_(footprint) {}

Reservation::~Reservation() { Reset(); }

Reservation::Reservation(Reservation&& other) noexcept
    : budget_(std::exchange(other.budget_, nullptr)),
      footprint_(std::exchange(other.footprint_, Footprint{})) {}

Reservation& Reservation::operator=(Reservation&& other) noexcept {
    if (this == &other) return *this;
    Reset();
    budget_ = std::exchange(other.budget_, nullptr);
    footprint_ = std::exchange(other.footprint_, Footprint{});
    return *this;
}

void Reservation::Reset() {
    if (budget_ != nullptr) budget_->Release(footprint_);
    budget_ = nullptr;
    footprint_ = {};
}

bool Reservation::ShrinkTo(const Footprint& footprint) {
    if (budget_ == nullptr || !SameOrSmaller(footprint, footprint_)) return false;
    Footprint released{
        .nodes = footprint_.nodes - footprint.nodes,
        .strings = footprint_.strings - footprint.strings,
        .dom_bytes = footprint_.dom_bytes - footprint.dom_bytes,
        .internal_preferred_bytes =
            footprint_.internal_preferred_bytes - footprint.internal_preferred_bytes,
        .max_depth = 0,
    };
    budget_->ReleaseFootprint(released);
    footprint_ = footprint;
    return true;
}

ParsedJson::~ParsedJson() {
    if (root != nullptr) cJSON_Delete(root);
}

ParsedJson::ParsedJson(ParsedJson&& other) noexcept
    : root(std::exchange(other.root, nullptr)),
      footprint(std::exchange(other.footprint, Footprint{})),
      reservation(std::move(other.reservation)) {}

ParsedJson& ParsedJson::operator=(ParsedJson&& other) noexcept {
    if (this == &other) return *this;
    if (root != nullptr) cJSON_Delete(root);
    reservation.Reset();
    root = std::exchange(other.root, nullptr);
    footprint = std::exchange(other.footprint, Footprint{});
    reservation = std::move(other.reservation);
    return *this;
}

CheckResult EstimateJson(std::string_view json, Footprint* out) {
    if (out == nullptr) return CheckResult::InvalidJson;
    *out = {};
    if (json.empty()) return CheckResult::Empty;
    if (json.size() > kMaxWireBytes) return CheckResult::TooLarge;

    size_t offset = 0;
    size_t depth = 0;
    bool have_root = false;
    while (offset < json.size()) {
        if (IsWhitespace(json[offset]) || json[offset] == ',' || json[offset] == ':') {
            ++offset;
            continue;
        }
        const char value = json[offset];
        if (value == '}' || value == ']') {
            if (depth == 0) return CheckResult::InvalidJson;
            --depth;
            ++offset;
            continue;
        }
        if (value == '{' || value == '[') {
            if (!AddNode(out)) return CheckResult::DomLimit;
            if (!have_root) have_root = true;
            ++depth;
            out->max_depth = std::max(out->max_depth, depth);
            if (depth > kMaxJsonDepth) return CheckResult::TooDeep;
            ++offset;
        } else if (value == '"') {
            size_t decoded = 0;
            if (!ParseString(json, &offset, &decoded)) return CheckResult::InvalidJson;
            size_t after = offset;
            while (after < json.size() && IsWhitespace(json[after])) ++after;
            const bool is_key = after < json.size() && json[after] == ':';
            if (!AddStringFootprint(decoded, out)) return CheckResult::DomLimit;
            if (!is_key) {
                if (!AddNode(out)) return CheckResult::DomLimit;
                if (!have_root) have_root = true;
            }
        } else {
            const size_t start = offset;
            while (offset < json.size() && !IsWhitespace(json[offset]) &&
                   json[offset] != ',' && json[offset] != ']' && json[offset] != '}') ++offset;
            if (offset == start || !AddNode(out)) return CheckResult::InvalidJson;
            if (!have_root) have_root = true;
        }
        if (out->nodes > kMaxFrameNodes) return CheckResult::TooManyNodes;
        if (out->nodes > (std::numeric_limits<size_t>::max() / kNodeDomCharge)) return CheckResult::DomLimit;
        out->dom_bytes = out->nodes * kNodeDomCharge;
        if (!Add(&out->dom_bytes, out->strings) || out->dom_bytes > kMaxFrameDomBytes) {
            return CheckResult::DomLimit;
        }
    }
    if (!have_root || depth != 0) return CheckResult::InvalidJson;
    return CheckResult::Ok;
}

CheckResult MeasureDom(const cJSON* root, Footprint* out) {
    if (root == nullptr || out == nullptr) return CheckResult::InvalidJson;
    *out = {};
    if (!MeasureItem(root, 1, out)) {
        return out->max_depth > kMaxJsonDepth ? CheckResult::TooDeep : CheckResult::DomLimit;
    }
    if (out->nodes > kMaxFrameNodes) return CheckResult::TooManyNodes;
    if (out->nodes > (std::numeric_limits<size_t>::max() / kNodeDomCharge)) return CheckResult::DomLimit;
    out->dom_bytes = out->nodes * kNodeDomCharge;
    if (!Add(&out->dom_bytes, out->strings) || out->dom_bytes > kMaxFrameDomBytes) return CheckResult::DomLimit;
    return CheckResult::Ok;
}

CheckResult ParseBoundedJson(std::string_view json, RetainedBudget* budget,
                             ParsedJson* out,
                             ParseFunction parse) {
    if (out == nullptr || budget == nullptr) return CheckResult::InvalidJson;
    *out = ParsedJson{};
    Footprint estimated{};
    const CheckResult estimate = EstimateJson(json, &estimated);
    if (estimate != CheckResult::Ok) return estimate;
    if (!budget->TryReserve(estimated)) return CheckResult::DomLimit;
    Reservation reservation(budget, estimated);
    cJSON* root = nullptr;
    const bool parsed = parse == nullptr
        ? ParseDefault(json.data(), json.size(), &root)
        : ((root = parse(json.data(), json.size())) != nullptr);
    if (!parsed || root == nullptr) return CheckResult::InvalidJson;

    Footprint actual{};
    const CheckResult measured = MeasureDom(root, &actual);
    if (measured != CheckResult::Ok || !SameOrSmaller(actual, estimated) ||
        !reservation.ShrinkTo(actual)) {
        cJSON_Delete(root);
        return measured == CheckResult::Ok ? CheckResult::DomLimit : measured;
    }
    out->root = root;
    out->footprint = actual;
    out->reservation = std::move(reservation);
    return CheckResult::Ok;
}

bool ReadTopLevelStringMember(std::string_view json, std::string_view key,
                              std::string_view* value) {
    if (value == nullptr) return false;
    size_t start = 0, end = 0;
    if (!FindTopLevelMember(json, key, &start, &end) || end - start < 2 ||
        json[start] != '"' || json[end - 1] != '"') return false;
    const auto contents = json.substr(start + 1, end - start - 2);
    if (contents.find('\\') != std::string_view::npos) return false;
    *value = contents;
    return true;
}

bool ElideTopLevelMember(std::string_view json, std::string_view key,
                         std::string* output) {
    if (output == nullptr) return false;
    size_t start = 0, end = 0;
    if (!FindTopLevelMember(json, key, &start, &end)) return false;
    output->clear();
    output->reserve(json.size() - (end - start) + 4);
    output->append(json.data(), start);
    output->append("null");
    output->append(json.data() + end, json.size() - end);
    return output->size() <= kMaxWireBytes;
}

bool DecodeBase64(std::string_view encoded, size_t max_output_bytes,
                  std::vector<uint8_t>* output) {
    if (output == nullptr || encoded.empty() || (encoded.size() % 4) != 0 ||
        encoded.size() > ((max_output_bytes + 2) / 3) * 4) return false;
    size_t padding = 0;
    if (encoded.back() == '=') {
        padding = 1;
        if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') padding = 2;
    }
    const size_t decoded_size = (encoded.size() / 4) * 3 - padding;
    if (decoded_size == 0 || decoded_size > max_output_bytes) return false;
    auto digit = [](char value) -> int {
        if (value >= 'A' && value <= 'Z') return value - 'A';
        if (value >= 'a' && value <= 'z') return value - 'a' + 26;
        if (value >= '0' && value <= '9') return value - '0' + 52;
        if (value == '+') return 62;
        if (value == '/') return 63;
        return -1;
    };
    output->clear();
    output->reserve(decoded_size);
    for (size_t i = 0; i < encoded.size(); i += 4) {
        const bool last = i + 4 == encoded.size();
        const int a = digit(encoded[i]);
        const int b = digit(encoded[i + 1]);
        const bool pad_c = encoded[i + 2] == '=';
        const bool pad_d = encoded[i + 3] == '=';
        const int c = pad_c ? 0 : digit(encoded[i + 2]);
        const int d = pad_d ? 0 : digit(encoded[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (pad_c && (!last || !pad_d)) || (pad_d && !last)) return false;
        if (pad_c && (b & 0x0f) != 0) return false;
        if (!pad_c && pad_d && (c & 0x03) != 0) return false;
        output->push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        if (!pad_c) output->push_back(static_cast<uint8_t>((b << 4) | (c >> 2)));
        if (!pad_d) output->push_back(static_cast<uint8_t>((c << 6) | d));
    }
    return output->size() == decoded_size;
}

}  // namespace codex_remote::pipeline
