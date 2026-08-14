/*
 * SPDX-FileCopyrightText: 2026 MetalioClaw4 contributors
 * SPDX-License-Identifier: MIT
 *
 * Stable C ABI exposed to external apps. External apps must only call the
 * function pointers supplied here; they must not link against firmware
 * internals such as LVGL or ESP-IDF.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define METALIO_APP_ABI_VERSION 1U
#define METALIO_APP_TARGET "esp32p4"

typedef enum {
    METALIO_APP_FONT_SMALL = 0,
    METALIO_APP_FONT_MEDIUM = 1,
    METALIO_APP_FONT_MEDIUM_BOLD = 2,
} metalio_app_font_t;

typedef uint32_t metalio_app_widget_t;

typedef enum {
    METALIO_APP_ACTION_INFO = 0,
    METALIO_APP_ACTION_WIND = 1,
    METALIO_APP_ACTION_HEART = 2,
    METALIO_APP_ACTION_REFRESH = 3,
    METALIO_APP_ACTION_PLAY = 4,
} metalio_app_action_icon_t;

typedef void (*metalio_app_callback_t)(void* app_context);

#define METALIO_PET_MAX_GRID_AXIS 5U
#define METALIO_PET_MAX_BODY_VERTICES 25U
#define METALIO_PET_MAX_LIMBS 4U

typedef struct {
    float x;
    float y;
} metalio_pet_vec2_t;

typedef struct {
    metalio_pet_vec2_t control1_offset;
    metalio_pet_vec2_t control2_offset;
    metalio_pet_vec2_t end_offset;
    float width_scale;
    float opacity;
} metalio_pet_limb_pose_t;

typedef struct {
    metalio_pet_vec2_t root_translation;
    float root_rotation_degrees;
    metalio_pet_vec2_t root_scale;
    metalio_pet_vec2_t body_vertex_offsets[METALIO_PET_MAX_BODY_VERTICES];
    metalio_pet_limb_pose_t limbs[METALIO_PET_MAX_LIMBS];
} metalio_pet_pose_t;

typedef struct {
    uint32_t time_ms;
    metalio_pet_pose_t pose;
    float curve_y1;
    float curve_y2;
} metalio_pet_keyframe_t;

typedef struct {
    const char* id;
    const metalio_pet_keyframe_t* keyframes;
    uint32_t keyframe_count;
    uint32_t duration_ms;
    uint8_t loop;
} metalio_pet_clip_t;

typedef struct {
    uint8_t anchor_vertex;
    metalio_pet_vec2_t control1;
    metalio_pet_vec2_t control2;
    metalio_pet_vec2_t end;
    float width;
    uint32_t rgba8888;
    uint8_t segments;
    uint8_t draw_behind_body;
} metalio_pet_limb_rig_t;

typedef struct {
    uint8_t columns;
    uint8_t rows;
    float destination_x;
    float destination_y;
    float destination_width;
    float destination_height;
    float uv_x;
    float uv_y;
    float uv_width;
    float uv_height;
    metalio_pet_vec2_t root_pivot;
    metalio_pet_limb_rig_t limbs[METALIO_PET_MAX_LIMBS];
    uint8_t limb_count;
} metalio_pet_rig_t;

typedef struct {
    uint32_t frame_count;
    uint32_t last_render_us;
    uint32_t last_raster_us;
    uint32_t average_render_us;
    uint32_t max_render_us;
    uint32_t budget_overrun_count;
    uint32_t missed_frame_count;
} metalio_pet_render_stats_t;

typedef struct metalio_app_host_api {
    uint32_t abi_version;
    uint32_t struct_size;

    int (*set_background)(void* host_context, uint32_t rgb888);
    int (*add_label)(void* host_context, const char* text, int16_t x,
                     int16_t y, int16_t width, int16_t height,
                     uint32_t rgb888, metalio_app_font_t font);
    int (*add_image)(void* host_context, const char* asset_relative_path,
                     int16_t x, int16_t y, int16_t width, int16_t height);

    /* ABI 1 additive extensions. Check struct_size before using them. */
    int (*add_label_ex)(void* host_context, const char* text, int16_t x,
                        int16_t y, int16_t width, int16_t height,
                        uint32_t rgb888, metalio_app_font_t font,
                        metalio_app_widget_t* widget);
    int (*set_label_text)(void* host_context, metalio_app_widget_t widget,
                          const char* text);
    int (*set_interval)(void* host_context, uint32_t period_ms,
                        metalio_app_callback_t callback, void* app_context);
    int (*add_action)(void* host_context, metalio_app_action_icon_t icon,
                      const char* label, metalio_app_callback_t callback,
                      void* app_context);
    int (*add_pet)(void* host_context, const char* texture_asset_relative_path,
                   int16_t x, int16_t y, int16_t width, int16_t height,
                   const metalio_pet_rig_t* rig, metalio_app_widget_t* widget);
    int (*pet_play)(void* host_context, metalio_app_widget_t widget,
                    const metalio_pet_clip_t* clip, uint32_t frame_period_ms);
    int (*pet_set_debug)(void* host_context, metalio_app_widget_t widget,
                         uint8_t enabled);
    int (*pet_get_stats)(void* host_context, metalio_app_widget_t widget,
                         metalio_pet_render_stats_t* stats);
} metalio_app_host_api_t;

typedef struct metalio_app_launch_context {
    uint32_t abi_version;
    uint32_t struct_size;
    void* host_context;
    uint16_t content_width;
    uint16_t content_height;
} metalio_app_launch_context_t;

#ifdef __cplusplus
}
#endif
