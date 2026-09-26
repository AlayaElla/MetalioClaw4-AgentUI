#pragma once

#include <cmath>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "cJSON.h"

namespace agent_ui::codex_realtime {

// Follow the live text tail within 150 ms, independent of utterance length.
// Retarget from the current position so streaming updates never restart a pass.
struct CaptionScroll {
    int from = 0;
    int target = 0;
    uint32_t started = 0;

    int Offset(uint32_t now) const {
        const uint32_t elapsed = now - started;
        if (elapsed >= 150) return target;
        return from + static_cast<int>(static_cast<int64_t>(target - from) * elapsed / 150);
    }

    void Follow(int overflow, uint32_t now) {
        const int next = overflow > 0 ? overflow : 0;
        if (next == target) return;
        const int current = Offset(now);
        from = current < next ? current : next;
        target = next;
        started = now;
    }
};

// A complete, bounded snapshot. The caller checks call/task/stream identity first.
struct Captions {
    uint32_t sequence = 0;
    std::string text;
    std::array<std::string, 2> lines;

    void Clear() { sequence = 0; text.clear(); lines = {}; }

    bool Apply(const cJSON* message) {
        const auto* revision = cJSON_GetObjectItemCaseSensitive(message, "sequence");
        const auto* entries = cJSON_GetObjectItemCaseSensitive(message, "entries");
        if (!cJSON_IsNumber(revision) || !std::isfinite(revision->valuedouble) ||
            revision->valuedouble <= sequence || revision->valuedouble > UINT32_MAX ||
            std::floor(revision->valuedouble) != revision->valuedouble || !cJSON_IsArray(entries)) return false;
        const int count = cJSON_GetArraySize(entries);
        if (count < 1 || count > 2) return false;
        std::string next;
        std::array<std::string, 2> next_lines;
        size_t line_count = 0;
        for (int i = 0; i < count; ++i) {
            const auto* entry = cJSON_GetArrayItem(entries, i);
            const auto* role = cJSON_GetObjectItemCaseSensitive(entry, "role");
            const auto* value = cJSON_GetObjectItemCaseSensitive(entry, "text");
            if (!cJSON_IsString(role) || !cJSON_IsString(value) || std::strlen(value->valuestring) > 2048) return false;
            const bool user = std::strcmp(role->valuestring, "user") == 0;
            if (!user && std::strcmp(role->valuestring, "assistant") != 0) return false;
            if (value->valuestring[0] == '\0') continue;
            std::string line = user ? "你：" : "AI：";
            for (const char* p = value->valuestring; *p; ++p) {
                line += (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
            }
            if (!next.empty()) next += '\n';
            next += line;
            next_lines[line_count++] = std::move(line);
        }
        sequence = static_cast<uint32_t>(revision->valuedouble);
        text = std::move(next);
        lines = std::move(next_lines);
        return true;
    }
};

}  // namespace agent_ui::codex_realtime
