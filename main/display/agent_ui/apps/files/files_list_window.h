#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace agent_ui::files_list_window {

constexpr std::size_t kNoItem = std::numeric_limits<std::size_t>::max();

constexpr bool IsBindingCurrent(std::uint32_t bound_epoch,
                                std::uint32_t current_epoch) {
    return bound_epoch == current_epoch;
}

struct Range {
    std::size_t first = 0;
    std::size_t count = 0;
};

struct Slot {
    std::size_t item = kNoItem;
    bool pressed = false;
    bool available = true;
};

constexpr Range CalculateRange(std::size_t item_count, int scroll_y,
                               int viewport_height, int row_height,
                               std::size_t capacity) {
    if (item_count == 0 || viewport_height <= 0 || row_height <= 0 ||
        capacity == 0) {
        return {};
    }
    const std::size_t visible =
        (static_cast<std::size_t>(viewport_height) + row_height - 1) /
        static_cast<std::size_t>(row_height);
    const std::size_t count = visible + 2 < capacity ? visible + 2 : capacity;
    std::size_t first = static_cast<std::size_t>(scroll_y > 0 ? scroll_y : 0) /
                        static_cast<std::size_t>(row_height);
    if (first > 0) --first;
    if (first + count > item_count) {
        first = item_count > count ? item_count - count : 0;
    }
    return {first, item_count - first < count ? item_count - first : count};
}

template <std::size_t Capacity>
std::array<std::size_t, Capacity> Assign(const Range& range,
                                         const std::array<Slot, Capacity>& slots) {
    std::array<std::size_t, Capacity> result{};
    result.fill(kNoItem);
    std::array<std::size_t, Capacity> held{};
    held.fill(kNoItem);
    for (std::size_t slot = 0; slot < Capacity; ++slot) {
        if (!slots[slot].available || !slots[slot].pressed ||
            slots[slot].item == kNoItem) continue;
        result[slot] = slots[slot].item;
        held[slot] = slots[slot].item;
    }
    const auto is_held = [&](std::size_t item) {
        for (std::size_t held_item : held) {
            if (held_item == item) return true;
        }
        return false;
    };
    const auto is_assigned = [&](std::size_t item) {
        for (std::size_t assigned : result) {
            if (assigned == item) return true;
        }
        return false;
    };
    std::size_t next = range.first;
    const std::size_t end = range.first + range.count;
    for (std::size_t slot = 0; slot < Capacity; ++slot) {
        if (!slots[slot].available || slots[slot].pressed ||
            slots[slot].item < range.first ||
            slots[slot].item >= end || is_assigned(slots[slot].item)) {
            continue;
        }
        result[slot] = slots[slot].item;
    }
    for (std::size_t slot = 0; slot < Capacity; ++slot) {
        if (!slots[slot].available || result[slot] != kNoItem ||
            slots[slot].pressed) continue;
        while (next < end && (is_held(next) || is_assigned(next))) ++next;
        if (next >= end) break;
        result[slot] = next++;
    }
    return result;
}

}  // namespace agent_ui::files_list_window
