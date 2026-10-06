#pragma once

/* Small, platform-independent planner for framebuffer cache ranges. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DISPLAY_CACHE_SYNC_MAX_RECTS 256U
#define DISPLAY_CACHE_SYNC_MAX_PLANNER_STEPS 4096U
#define DISPLAY_CACHE_SYNC_MAX_RANGE_CALLS 64U

typedef struct {
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
} display_cache_sync_rect_t;

typedef struct {
    uintptr_t address;
    size_t size;
} display_cache_sync_range_t;

typedef bool (*display_cache_sync_rect_getter_t)(
    const void *source, size_t index, display_cache_sync_rect_t *rect);

static inline bool display_cache_sync_rect_array_get(
    const void *source, size_t index, display_cache_sync_rect_t *rect)
{
    if (source == NULL || rect == NULL) return false;
    *rect = ((const display_cache_sync_rect_t *)source)[index];
    return true;
}

typedef struct {
    const void *rect_source;
    display_cache_sync_rect_getter_t rect_getter;
    size_t rect_count;
    size_t width;
    size_t height;
    size_t frame_capacity;
    size_t color_bytes;
    size_t cache_line_size;
    uintptr_t framebuffer;
    uint16_t order[DISPLAY_CACHE_SYNC_MAX_RECTS];
    int32_t first_row;
    int32_t next_row;
    int32_t last_row;
    size_t rect_cursor;
    size_t planner_steps;
    size_t init_steps;
    int32_t interval_x1;
    int32_t interval_x2;
    bool have_interval;
    bool have_pending_range;
    bool failed;
    bool exhausted;
    display_cache_sync_range_t pending_range;
} display_cache_sync_iterator_t;

typedef enum {
    DISPLAY_CACHE_SYNC_PLAN_INVALID = 0,
    DISPLAY_CACHE_SYNC_PLAN_PARTIAL,
    DISPLAY_CACHE_SYNC_PLAN_FULL_FRAME,
} display_cache_sync_plan_result_t;

static inline bool display_cache_sync_frame_valid(
    uintptr_t framebuffer, size_t frame_capacity, size_t width, size_t height,
    size_t color_bytes)
{
    if (framebuffer == 0 || frame_capacity == 0 || width == 0 || height == 0 ||
        width > INT32_MAX || height > INT32_MAX ||
        color_bytes == 0 || frame_capacity > UINTPTR_MAX - framebuffer ||
        width > SIZE_MAX / color_bytes) {
        return false;
    }

    const size_t row_stride = width * color_bytes;
    if (height > SIZE_MAX / row_stride) {
        return false;
    }
    return row_stride * height <= frame_capacity;
}

static inline bool display_cache_sync_rect_valid(
    size_t width, size_t height, int32_t x1, int32_t y1, int32_t x2,
    int32_t y2)
{
    return width != 0 && height != 0 && x1 >= 0 && y1 >= 0 && x2 >= x1 &&
           y2 >= y1 && (uint32_t)x2 < width && (uint32_t)y2 < height;
}

static inline bool display_cache_sync_geometry_valid(
    uintptr_t framebuffer, size_t frame_capacity, size_t width, size_t height,
    size_t color_bytes, size_t cache_line_size)
{
    return cache_line_size != 0 &&
           (cache_line_size & (cache_line_size - 1U)) == 0 &&
           display_cache_sync_frame_valid(framebuffer, frame_capacity, width,
                                          height, color_bytes);
}

static inline bool display_cache_sync_plan_row(
    uintptr_t framebuffer, size_t frame_capacity, size_t width, size_t height,
    size_t color_bytes, size_t cache_line_size, int32_t x1, int32_t y1,
    int32_t x2, int32_t y2, size_t row,
    display_cache_sync_range_t *range)
{
    if (range == NULL ||
        !display_cache_sync_geometry_valid(framebuffer, frame_capacity, width,
                                           height, color_bytes,
                                           cache_line_size) ||
        !display_cache_sync_rect_valid(width, height, x1, y1, x2, y2) ||
        row < (size_t)y1 || row > (size_t)y2) {
        return false;
    }

    const size_t row_stride = width * color_bytes;
    const size_t row_offset = row * row_stride;
    const size_t x_begin = (size_t)x1 * color_bytes;
    const size_t x_end = ((size_t)x2 + 1U) * color_bytes;
    if (row_offset > frame_capacity || x_end > row_stride ||
        x_begin >= x_end || row_offset > frame_capacity - x_end) {
        return false;
    }

    const uintptr_t frame_end = framebuffer + frame_capacity;
    const uintptr_t start = framebuffer + row_offset + x_begin;
    const uintptr_t end = framebuffer + row_offset + x_end;
    const uintptr_t mask = (uintptr_t)cache_line_size - 1U;
    const uintptr_t aligned_start = start & ~mask;
    if (end > UINTPTR_MAX - mask) {
        return false;
    }
    const uintptr_t aligned_end = (end + mask) & ~mask;
    if (aligned_start < framebuffer || aligned_end > frame_end ||
        aligned_end <= aligned_start ||
        aligned_end - aligned_start > SIZE_MAX) {
        return false;
    }

    range->address = aligned_start;
    range->size = (size_t)(aligned_end - aligned_start);
    return true;
}

static inline bool display_cache_sync_iterator_init(
    display_cache_sync_iterator_t *iterator, uintptr_t framebuffer,
    size_t frame_capacity, size_t width, size_t height, size_t color_bytes,
    size_t cache_line_size, const void *rect_source, size_t rect_count,
    display_cache_sync_rect_getter_t rect_getter)
{
    if (iterator == NULL || rect_source == NULL || rect_getter == NULL ||
        rect_count == 0 ||
        rect_count > DISPLAY_CACHE_SYNC_MAX_RECTS ||
        !display_cache_sync_geometry_valid(framebuffer, frame_capacity, width,
                                           height, color_bytes,
                                           cache_line_size)) {
        return false;
    }

    memset(iterator, 0, sizeof(*iterator));
    iterator->rect_source = rect_source;
    iterator->rect_getter = rect_getter;
    iterator->rect_count = rect_count;
    iterator->width = width;
    iterator->height = height;
    iterator->frame_capacity = frame_capacity;
    iterator->color_bytes = color_bytes;
    iterator->cache_line_size = cache_line_size;
    iterator->framebuffer = framebuffer;
    iterator->first_row = INT32_MAX;
    iterator->last_row = -1;

    for (size_t index = 0; index < rect_count; ++index) {
        if (iterator->planner_steps >= DISPLAY_CACHE_SYNC_MAX_PLANNER_STEPS) {
            iterator->failed = true;
            return false;
        }
        ++iterator->planner_steps;
        display_cache_sync_rect_t rect = {0};
        if (!rect_getter(rect_source, index, &rect) ||
            !display_cache_sync_rect_valid(width, height, rect.x1, rect.y1,
                                           rect.x2, rect.y2)) {
            iterator->failed = true;
            return false;
        }
        if (rect.y1 < iterator->first_row) iterator->first_row = rect.y1;
        if (rect.y2 > iterator->last_row) iterator->last_row = rect.y2;
    }
    iterator->next_row = iterator->first_row;

    /* Sort once by x. Every scanline can then union intervals in one pass. */
    for (size_t index = 0; index < rect_count; ++index) {
        size_t position = index;
        display_cache_sync_rect_t insert = {0};
        if (iterator->planner_steps >= DISPLAY_CACHE_SYNC_MAX_PLANNER_STEPS) {
            iterator->failed = true;
            return false;
        }
        ++iterator->planner_steps;
        if (!rect_getter(rect_source, index, &insert)) {
            iterator->failed = true;
            return false;
        }
        while (position > 0) {
            if (iterator->planner_steps >= DISPLAY_CACHE_SYNC_MAX_PLANNER_STEPS) {
                iterator->failed = true;
                return false;
            }
            ++iterator->planner_steps;
            const uint16_t previous_index = iterator->order[position - 1U];
            display_cache_sync_rect_t previous = {0};
            if (!rect_getter(rect_source, previous_index, &previous)) {
                iterator->failed = true;
                return false;
            }
            if (previous.x1 < insert.x1 ||
                (previous.x1 == insert.x1 && previous.x2 <= insert.x2)) {
                break;
            }
            iterator->order[position] = previous_index;
            --position;
        }
        iterator->order[position] = (uint16_t)index;
    }
    iterator->init_steps = iterator->planner_steps;
    return true;
}

static inline void display_cache_sync_iterator_reset(
    display_cache_sync_iterator_t *iterator)
{
    if (iterator == NULL) return;
    iterator->next_row = iterator->first_row;
    iterator->rect_cursor = 0;
    iterator->planner_steps = iterator->init_steps;
    iterator->interval_x1 = 0;
    iterator->interval_x2 = 0;
    iterator->have_interval = false;
    iterator->have_pending_range = false;
    iterator->failed = false;
    iterator->exhausted = false;
    iterator->pending_range.address = 0;
    iterator->pending_range.size = 0;
}

static inline bool display_cache_sync_append_range(
    display_cache_sync_iterator_t *iterator,
    const display_cache_sync_range_t *range,
    display_cache_sync_range_t *ready)
{
    if (!iterator->have_pending_range) {
        iterator->pending_range = *range;
        iterator->have_pending_range = true;
        return false;
    }
    const uintptr_t pending_end = iterator->pending_range.address +
                                  iterator->pending_range.size;
    const uintptr_t range_end = range->address + range->size;
    if (range->address <= pending_end) {
        if (range_end > pending_end) {
            iterator->pending_range.size =
                (size_t)(range_end - iterator->pending_range.address);
        }
        return false;
    }
    *ready = iterator->pending_range;
    iterator->pending_range = *range;
    return true;
}

static inline bool display_cache_sync_iterator_next(
    display_cache_sync_iterator_t *iterator,
    display_cache_sync_range_t *range)
{
    if (iterator == NULL || range == NULL || iterator->exhausted) {
        return false;
    }

    for (;;) {
        if (iterator->next_row > iterator->last_row) {
            if (iterator->have_pending_range) {
                *range = iterator->pending_range;
                iterator->have_pending_range = false;
                iterator->exhausted = true;
                return true;
            }
            iterator->exhausted = true;
            return false;
        }

        if (iterator->rect_cursor < iterator->rect_count) {
            if (iterator->planner_steps >= DISPLAY_CACHE_SYNC_MAX_PLANNER_STEPS) {
                iterator->failed = true;
                iterator->exhausted = true;
                return false;
            }
            display_cache_sync_rect_t rect = {0};
            const uint16_t rect_index = iterator->order[iterator->rect_cursor++];
            if (!iterator->rect_getter(iterator->rect_source, rect_index, &rect)) {
                iterator->failed = true;
                iterator->exhausted = true;
                return false;
            }
            ++iterator->planner_steps;
            if (rect.y1 > iterator->next_row || rect.y2 < iterator->next_row) {
                continue;
            }
            if (!iterator->have_interval) {
                iterator->interval_x1 = rect.x1;
                iterator->interval_x2 = rect.x2;
                iterator->have_interval = true;
                continue;
            }
            if ((int64_t)rect.x1 <= (int64_t)iterator->interval_x2 + 1) {
                if (rect.x2 > iterator->interval_x2) {
                    iterator->interval_x2 = rect.x2;
                }
                continue;
            }

            display_cache_sync_range_t planned = {0};
            if (!display_cache_sync_plan_row(
                    iterator->framebuffer, iterator->frame_capacity,
                    iterator->width, iterator->height, iterator->color_bytes,
                    iterator->cache_line_size, iterator->interval_x1,
                    iterator->next_row, iterator->interval_x2,
                    iterator->next_row, (size_t)iterator->next_row,
                    &planned)) {
                iterator->failed = true;
                iterator->exhausted = true;
                return false;
            }
            iterator->interval_x1 = rect.x1;
            iterator->interval_x2 = rect.x2;
            if (display_cache_sync_append_range(iterator, &planned, range)) {
                return true;
            }
            continue;
        }

        if (iterator->have_interval) {
            display_cache_sync_range_t planned = {0};
            if (!display_cache_sync_plan_row(
                    iterator->framebuffer, iterator->frame_capacity,
                    iterator->width, iterator->height, iterator->color_bytes,
                    iterator->cache_line_size, iterator->interval_x1,
                    iterator->next_row, iterator->interval_x2,
                    iterator->next_row, (size_t)iterator->next_row,
                    &planned)) {
                iterator->failed = true;
                iterator->exhausted = true;
                return false;
            }
            iterator->have_interval = false;
            iterator->rect_cursor = 0;
            ++iterator->next_row;
            if (display_cache_sync_append_range(iterator, &planned, range)) {
                return true;
            }
            continue;
        }

        iterator->rect_cursor = 0;
        ++iterator->next_row;
    }
}

static inline display_cache_sync_plan_result_t display_cache_sync_choose_plan(
    uintptr_t framebuffer, size_t frame_capacity, size_t width, size_t height,
    size_t color_bytes, size_t cache_line_size,
    const void *rect_source, size_t rect_count,
    display_cache_sync_rect_getter_t rect_getter,
    size_t *range_bytes, size_t *range_calls, size_t *planner_steps,
    display_cache_sync_iterator_t *iterator)
{
    if (range_bytes != NULL) *range_bytes = 0;
    if (range_calls != NULL) *range_calls = 0;
    if (planner_steps != NULL) *planner_steps = 0;
    if (iterator == NULL || rect_source == NULL || rect_getter == NULL || rect_count == 0 ||
        rect_count > DISPLAY_CACHE_SYNC_MAX_RECTS ||
        !display_cache_sync_geometry_valid(framebuffer, frame_capacity, width,
                                           height, color_bytes,
                                           cache_line_size)) {
        return DISPLAY_CACHE_SYNC_PLAN_INVALID;
    }

    int32_t first_row = INT32_MAX;
    int32_t last_row = -1;
    const size_t full_frame_threshold = frame_capacity - frame_capacity / 4U;
    bool full_frame_by_rect = false;
    size_t validation_steps = 0;
    for (size_t index = 0; index < rect_count; ++index) {
        ++validation_steps;
        display_cache_sync_rect_t rect = {0};
        if (!rect_getter(rect_source, index, &rect) ||
            !display_cache_sync_rect_valid(width, height, rect.x1, rect.y1,
                                           rect.x2, rect.y2)) {
            if (planner_steps != NULL) *planner_steps = validation_steps;
            return DISPLAY_CACHE_SYNC_PLAN_INVALID;
        }
        if (rect.y1 < first_row) first_row = rect.y1;
        if (rect.y2 > last_row) last_row = rect.y2;
        const size_t rect_width = (size_t)(rect.x2 - rect.x1 + 1);
        const size_t rect_height = (size_t)(rect.y2 - rect.y1 + 1);
        if (rect_height > SIZE_MAX / rect_width ||
            rect_width * rect_height > SIZE_MAX / color_bytes) {
            if (planner_steps != NULL) *planner_steps = validation_steps;
            return DISPLAY_CACHE_SYNC_PLAN_INVALID;
        }
        if (rect_width * rect_height * color_bytes >= full_frame_threshold) {
            full_frame_by_rect = true;
        }
    }
    if (planner_steps != NULL) *planner_steps = validation_steps;
    if (full_frame_by_rect) {
        if (range_bytes != NULL) *range_bytes = frame_capacity;
        if (range_calls != NULL) *range_calls = 1;
        return DISPLAY_CACHE_SYNC_PLAN_FULL_FRAME;
    }

    const size_t scan_rows = (size_t)(last_row - first_row + 1);
    const size_t sort_comparisons = rect_count * (rect_count - 1U) / 2U;
    const size_t fixed_steps = 3U * rect_count + sort_comparisons;
    if (fixed_steps > DISPLAY_CACHE_SYNC_MAX_PLANNER_STEPS ||
        scan_rows >
            (DISPLAY_CACHE_SYNC_MAX_PLANNER_STEPS - fixed_steps) / rect_count) {
        if (range_bytes != NULL) *range_bytes = frame_capacity;
        if (range_calls != NULL) *range_calls = 1;
        return DISPLAY_CACHE_SYNC_PLAN_FULL_FRAME;
    }

    if (!display_cache_sync_iterator_init(
            iterator, framebuffer, frame_capacity, width, height,
            color_bytes, cache_line_size, rect_source, rect_count,
            rect_getter)) {
        if (planner_steps != NULL) {
            *planner_steps = validation_steps + iterator->planner_steps;
        }
        return DISPLAY_CACHE_SYNC_PLAN_INVALID;
    }
    iterator->planner_steps += validation_steps;
    iterator->init_steps += validation_steps;
    display_cache_sync_range_t range = {0};
    size_t bytes = 0;
    size_t calls = 0;
    while (display_cache_sync_iterator_next(iterator, &range)) {
        if (range.size > SIZE_MAX - bytes) {
            return DISPLAY_CACHE_SYNC_PLAN_INVALID;
        }
        bytes += range.size;
        ++calls;
        if (planner_steps != NULL) *planner_steps = iterator->planner_steps;
        if (calls > DISPLAY_CACHE_SYNC_MAX_RANGE_CALLS ||
            bytes >= full_frame_threshold) {
            if (range_bytes != NULL) *range_bytes = frame_capacity;
            if (range_calls != NULL) *range_calls = 1;
            return DISPLAY_CACHE_SYNC_PLAN_FULL_FRAME;
        }
    }
    if (planner_steps != NULL) *planner_steps = iterator->planner_steps;
    if (iterator->failed) {
        return DISPLAY_CACHE_SYNC_PLAN_INVALID;
    }
    if (calls == 0) {
        return DISPLAY_CACHE_SYNC_PLAN_INVALID;
    }
    if (range_bytes != NULL) *range_bytes = bytes;
    if (range_calls != NULL) *range_calls = calls;
    display_cache_sync_iterator_reset(iterator);
    return DISPLAY_CACHE_SYNC_PLAN_PARTIAL;
}
