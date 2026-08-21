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
    METALIO_APP_FONT_LARGE = 3,
    METALIO_APP_FONT_LARGE_BOLD = 4,
    METALIO_APP_FONT_SMALL_BOLD = 5,
} metalio_app_font_t;

typedef uint32_t metalio_app_widget_t;

typedef enum {
    METALIO_APP_ACTION_INFO = 0,
    METALIO_APP_ACTION_WIND = 1,
    METALIO_APP_ACTION_HEART = 2,
    METALIO_APP_ACTION_REFRESH = 3,
    METALIO_APP_ACTION_PLAY = 4,
    METALIO_APP_ACTION_PREVIOUS = 5,
    METALIO_APP_ACTION_PAUSE = 6,
    METALIO_APP_ACTION_NEXT = 7,
    METALIO_APP_ACTION_VOLUME_DOWN = 8,
    METALIO_APP_ACTION_VOLUME_UP = 9,
} metalio_app_action_icon_t;

typedef void (*metalio_app_callback_t)(void* app_context);
typedef void (*metalio_app_value_callback_t)(void* app_context,
                                              int32_t value);
typedef void (*metalio_app_selection_callback_t)(void* app_context,
                                                  uint32_t index);

typedef enum {
    METALIO_APP_TEXT_ALIGN_LEFT = 0,
    METALIO_APP_TEXT_ALIGN_CENTER = 1,
    METALIO_APP_TEXT_ALIGN_RIGHT = 2,
} metalio_app_text_align_t;

typedef uint64_t metalio_app_capabilities_t;

#define METALIO_APP_CAP_HAPTICS (UINT64_C(1) << 0)
#define METALIO_APP_CAP_MOTION_ACCELEROMETER (UINT64_C(1) << 1)
#define METALIO_APP_CAP_MOTION_TILT (UINT64_C(1) << 2)
#define METALIO_APP_CAP_MEDIA_HLS (UINT64_C(1) << 3)
#define METALIO_APP_CAP_MEDIA_SPECTRUM (UINT64_C(1) << 4)
#define METALIO_APP_CAP_DEVICE_INFO (UINT64_C(1) << 5)
#define METALIO_APP_CAP_DATE_TIME (UINT64_C(1) << 6)
#define METALIO_APP_CAP_UI_BUTTONS (UINT64_C(1) << 7)
#define METALIO_APP_CAP_UI_GRID (UINT64_C(1) << 8)
#define METALIO_APP_CAP_UI_DRAW (UINT64_C(1) << 9)
#define METALIO_APP_CAP_UI_SWIPE (UINT64_C(1) << 10)
#define METALIO_APP_CAP_HTTP (UINT64_C(1) << 11)
#define METALIO_APP_CAP_UI_LIST (UINT64_C(1) << 12)
#define METALIO_APP_CAP_UI_ICONS (UINT64_C(1) << 13)
#define METALIO_APP_CAP_MAGNETOMETER (UINT64_C(1) << 14)
#define METALIO_APP_CAP_AUDIO_RECORDING (UINT64_C(1) << 15)
#define METALIO_APP_CAP_UI_THEME (UINT64_C(1) << 16)
#define METALIO_APP_CAP_APP_STORAGE (UINT64_C(1) << 17)
#define METALIO_APP_CAP_UI_CONTROLS (UINT64_C(1) << 18)

typedef enum {
    METALIO_APP_STORAGE_OK = 0,
    METALIO_APP_STORAGE_ERROR_INVALID = -1,
    METALIO_APP_STORAGE_ERROR_NOT_FOUND = -2,
    METALIO_APP_STORAGE_ERROR_IO = -3,
    METALIO_APP_STORAGE_ERROR_TOO_LARGE = -4,
    METALIO_APP_STORAGE_ERROR_BUFFER_TOO_SMALL = -5,
} metalio_app_storage_result_t;

#define METALIO_APP_MEDIA_SPECTRUM_BANDS 12U

typedef enum {
    METALIO_APP_MEDIA_IDLE = 0,
    METALIO_APP_MEDIA_CONNECTING = 1,
    METALIO_APP_MEDIA_PLAYING = 2,
    METALIO_APP_MEDIA_PAUSED = 3,
    METALIO_APP_MEDIA_ERROR = 4,
} metalio_app_media_state_t;

typedef enum {
    METALIO_APP_HAPTIC_TICK = 0,
    METALIO_APP_HAPTIC_CLICK = 1,
} metalio_app_haptic_effect_t;

/*
 * SC7A20H-backed motion data. Acceleration is expressed in milli-g and the
 * filtered board-relative tilt axes use [-1000, 1000]. This board does not
 * expose angular velocity, so these values must not be treated as gyroscope
 * degrees-per-second data.
 */
typedef struct {
    int16_t acceleration_x_mg;
    int16_t acceleration_y_mg;
    int16_t acceleration_z_mg;
    int16_t tilt_x_q10;
    int16_t tilt_y_q10;
    uint8_t acceleration_valid;
    uint8_t tilt_valid;
    uint8_t reserved[2];
} metalio_app_motion_sample_t;

typedef struct {
    char board_name[48];
    char chip_model[24];
    char firmware_version[32];
    uint32_t flash_size_bytes;
    uint32_t free_internal_bytes;
    uint32_t free_psram_bytes;
    uint32_t uptime_seconds;
    int16_t battery_percent;
    uint8_t battery_valid;
    uint8_t charging;
    uint8_t network_connected;
    uint8_t reserved[3];
} metalio_app_device_info_t;

typedef struct {
    int64_t unix_seconds;
    int16_t year;
    int8_t month;
    int8_t day;
    int8_t hour;
    int8_t minute;
    int8_t second;
    int8_t weekday;
    uint8_t valid;
    uint8_t reserved[7];
} metalio_app_date_time_t;

typedef struct {
    int32_t x_microtesla;
    int32_t y_microtesla;
    int32_t z_microtesla;
    uint8_t valid;
    uint8_t reserved[3];
} metalio_app_magnetic_sample_t;

typedef enum {
    METALIO_APP_SWIPE_LEFT = 0,
    METALIO_APP_SWIPE_RIGHT = 1,
    METALIO_APP_SWIPE_UP = 2,
    METALIO_APP_SWIPE_DOWN = 3,
} metalio_app_swipe_direction_t;

typedef void (*metalio_app_swipe_callback_t)(
    void* app_context, metalio_app_swipe_direction_t direction);

typedef enum {
    METALIO_APP_ICON_INFO = 0,
    METALIO_APP_ICON_CALENDAR = 1,
    METALIO_APP_ICON_CALCULATOR = 2,
    METALIO_APP_ICON_GAME = 3,
    METALIO_APP_ICON_WEATHER = 4,
    METALIO_APP_ICON_LEVEL = 5,
    METALIO_APP_ICON_MAGNETIC = 6,
    METALIO_APP_ICON_HAPTIC = 7,
    METALIO_APP_ICON_RADIO = 8,
    METALIO_APP_ICON_MUSIC = 9,
    METALIO_APP_ICON_MICROPHONE = 10,
    METALIO_APP_ICON_SETTINGS = 11,
    METALIO_APP_ICON_CHEVRON_RIGHT = 12,
} metalio_app_icon_t;

typedef struct {
    uint8_t dark;
    uint8_t reserved[3];
    uint32_t background;
    uint32_t surface;
    uint32_t raised;
    uint32_t border;
    uint32_t text;
    uint32_t muted;
    uint32_t accent;
    uint32_t accent_pressed;
    uint32_t accent_ink;
    uint32_t danger;
    uint32_t warning;
} metalio_app_theme_t;

/* Theme memory is host-owned and valid only during the callback. */
typedef void (*metalio_app_theme_callback_t)(
    void* app_context, const metalio_app_theme_t* theme);

typedef enum {
    METALIO_APP_HTTP_GET = 0,
    METALIO_APP_HTTP_POST = 1,
} metalio_app_http_method_t;

typedef enum {
    METALIO_APP_HTTP_OK = 0,
    METALIO_APP_HTTP_ERROR_INVALID = -1,
    METALIO_APP_HTTP_ERROR_NETWORK = -2,
    METALIO_APP_HTTP_ERROR_CREATE = -3,
    METALIO_APP_HTTP_ERROR_OPEN = -4,
    METALIO_APP_HTTP_ERROR_READ = -5,
    METALIO_APP_HTTP_ERROR_CANCELLED = -6,
} metalio_app_http_error_t;

typedef struct {
    metalio_app_http_method_t method;
    const char* url;
    const char* body;
    uint32_t body_size;
    const char* content_type;
    uint32_t timeout_ms;
    uint32_t max_response_bytes;
} metalio_app_http_request_t;

/* Body memory is owned by the host and is valid only during the callback. */
typedef struct {
    uint32_t request_id;
    int32_t status_code;
    metalio_app_http_error_t error;
    const uint8_t* body;
    uint32_t body_size;
    uint8_t truncated;
    uint8_t reserved[3];
} metalio_app_http_response_t;

typedef void (*metalio_app_http_callback_t)(
    void* app_context, const metalio_app_http_response_t* response);

#define METALIO_APP_RECORDING_PATH_BYTES 160U

typedef enum {
    METALIO_APP_RECORDING_IDLE = 0,
    METALIO_APP_RECORDING_RECORDING = 1,
    METALIO_APP_RECORDING_STOPPING = 2,
    METALIO_APP_RECORDING_COMPLETED = 3,
    METALIO_APP_RECORDING_ERROR = 4,
    METALIO_APP_RECORDING_CANCELLED = 5,
} metalio_app_recording_state_t;

typedef enum {
    METALIO_APP_RECORDING_OK = 0,
    METALIO_APP_RECORDING_ERROR_INVALID = -1,
    METALIO_APP_RECORDING_ERROR_BUSY = -2,
    METALIO_APP_RECORDING_ERROR_STORAGE = -3,
    METALIO_APP_RECORDING_ERROR_AUDIO = -4,
    METALIO_APP_RECORDING_ERROR_WRITE = -5,
    METALIO_APP_RECORDING_ERROR_CANCELLED = -6,
} metalio_app_recording_error_t;

/*
 * The host records processed 16 kHz mono PCM16 into a WAV file under
 * /sdcard/Recordings. file_name is an optional base name, not a path. The
 * host sanitizes it, appends .wav and never overwrites an existing file.
 */
typedef struct {
    const char* file_name;
    uint32_t max_duration_ms;
} metalio_app_recording_config_t;

typedef struct {
    metalio_app_recording_state_t state;
    metalio_app_recording_error_t error;
    uint32_t duration_ms;
    uint32_t data_bytes;
    uint32_t dropped_frames;
    uint8_t peak_percent;
    uint8_t reserved[3];
    char path[METALIO_APP_RECORDING_PATH_BYTES];
} metalio_app_recording_status_t;

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

    /* ABI 1 capability extensions. Check struct_size before using them. */
    metalio_app_capabilities_t (*get_capabilities)(void* host_context);
    int (*play_haptic)(void* host_context,
                       metalio_app_haptic_effect_t effect);
    int (*get_motion_sample)(void* host_context,
                             metalio_app_motion_sample_t* sample);

    int (*add_bar)(void* host_context, int16_t x, int16_t y, int16_t width,
                   int16_t height, uint32_t track_rgb888,
                   uint32_t indicator_rgb888, metalio_app_widget_t* widget);
    int (*set_bar_value)(void* host_context, metalio_app_widget_t widget,
                         uint16_t value_per_mille);

    int (*media_start)(void* host_context, const char* url);
    int (*media_pause)(void* host_context);
    int (*media_resume)(void* host_context);
    int (*media_stop)(void* host_context);
    int (*media_get_state)(void* host_context,
                           metalio_app_media_state_t* state);
    int (*media_get_spectrum)(
        void* host_context,
        uint8_t levels[METALIO_APP_MEDIA_SPECTRUM_BANDS]);
    int (*media_get_volume)(void* host_context, uint8_t* volume_percent);
    int (*media_set_volume)(void* host_context, uint8_t volume_percent);

    /* ABI 1 general App capability extensions. Check struct_size and caps. */
    int (*get_device_info)(void* host_context,
                           metalio_app_device_info_t* info);
    int (*get_date_time)(void* host_context,
                         metalio_app_date_time_t* date_time);
    int (*get_magnetic_sample)(void* host_context,
                               metalio_app_magnetic_sample_t* sample);

    int (*add_rect)(void* host_context, int16_t x, int16_t y, int16_t width,
                    int16_t height, uint32_t rgb888, uint16_t radius,
                    metalio_app_widget_t* widget);
    int (*set_rect_color)(void* host_context, metalio_app_widget_t widget,
                          uint32_t rgb888);
    int (*set_label_color)(void* host_context, metalio_app_widget_t widget,
                           uint32_t rgb888);

    int (*add_button)(void* host_context, const char* text, int16_t x,
                      int16_t y, int16_t width, int16_t height,
                      uint32_t background_rgb888, uint32_t text_rgb888,
                      metalio_app_font_t font,
                      metalio_app_callback_t callback, void* app_context,
                      metalio_app_widget_t* widget);
    int (*set_button_text)(void* host_context, metalio_app_widget_t widget,
                           const char* text);
    int (*set_button_enabled)(void* host_context,
                              metalio_app_widget_t widget, uint8_t enabled);

    int (*add_grid)(void* host_context, int16_t x, int16_t y, int16_t width,
                    int16_t height, uint8_t columns, uint8_t rows,
                    uint8_t column_gap, uint8_t row_gap,
                    metalio_app_widget_t* widget);
    int (*grid_add_button)(void* host_context, metalio_app_widget_t grid,
                           uint8_t column, uint8_t row, uint8_t column_span,
                           uint8_t row_span, const char* text,
                           uint32_t background_rgb888,
                           uint32_t text_rgb888, metalio_app_font_t font,
                           metalio_app_callback_t callback,
                           void* app_context, metalio_app_widget_t* widget);

    int (*set_swipe_handler)(void* host_context,
                             metalio_app_swipe_callback_t callback,
                             void* app_context);

    int (*add_icon)(void* host_context, metalio_app_icon_t icon, int16_t x,
                    int16_t y, int16_t width, int16_t height,
                    uint32_t rgb888, metalio_app_widget_t* widget);
    int (*set_icon)(void* host_context, metalio_app_widget_t widget,
                    metalio_app_icon_t icon);

    int (*add_list)(void* host_context, int16_t x, int16_t y, int16_t width,
                    int16_t height, metalio_app_widget_t* widget);
    int (*list_add_item)(void* host_context, metalio_app_widget_t list,
                         metalio_app_icon_t icon, const char* primary_text,
                         const char* secondary_text,
                         metalio_app_callback_t callback, void* app_context,
                         metalio_app_widget_t* widget);
    int (*list_clear)(void* host_context, metalio_app_widget_t list);

    int (*http_request)(void* host_context,
                        const metalio_app_http_request_t* request,
                        metalio_app_http_callback_t callback,
                        void* app_context, uint32_t* request_id);
    int (*http_cancel)(void* host_context, uint32_t request_id);

    int (*recording_start)(
        void* host_context, const metalio_app_recording_config_t* config);
    int (*recording_stop)(void* host_context);
    int (*recording_cancel)(void* host_context);
    int (*recording_get_status)(
        void* host_context, metalio_app_recording_status_t* status);

    int (*get_theme)(void* host_context, metalio_app_theme_t* theme);
    int (*set_theme_callback)(void* host_context,
                              metalio_app_theme_callback_t callback,
                              void* app_context);
    int (*set_bar_colors)(void* host_context, metalio_app_widget_t widget,
                          uint32_t track_rgb888,
                          uint32_t indicator_rgb888);
    int (*set_button_colors)(void* host_context,
                             metalio_app_widget_t widget,
                             uint32_t background_rgb888,
                             uint32_t pressed_rgb888,
                             uint32_t text_rgb888);
    int (*set_icon_color)(void* host_context, metalio_app_widget_t widget,
                          uint32_t rgb888);

    /*
     * ABI 1 app-private storage extension. Paths are safe relative paths.
     * config_read first uses persistent app data and, when absent, seeds it
     * from the package's assets/<relative_path>. config_write replaces the
     * persistent file without exposing an absolute SD-card path to the App.
     */
    int (*config_read)(void* host_context, const char* relative_path,
                       uint8_t* buffer, uint32_t capacity,
                       uint32_t* data_size);
    int (*config_write)(void* host_context, const char* relative_path,
                        const uint8_t* data, uint32_t data_size);

    /*
     * ABI 1 advanced reusable controls. These calls are appended so older
     * ABI-1 Apps keep working. Check struct_size and UI_CONTROLS first.
     */
    int (*set_label_alignment)(void* host_context,
                               metalio_app_widget_t widget,
                               metalio_app_text_align_t alignment);
    int (*set_label_font)(void* host_context, metalio_app_widget_t widget,
                          metalio_app_font_t font);
    int (*set_widget_bounds)(void* host_context,
                             metalio_app_widget_t widget, int16_t x,
                             int16_t y, int16_t width, int16_t height);
    int (*set_widget_visible)(void* host_context,
                              metalio_app_widget_t widget, uint8_t visible);
    int (*set_rect_border)(void* host_context, metalio_app_widget_t widget,
                           uint32_t rgb888, uint8_t width);
    int (*set_button_border)(void* host_context,
                             metalio_app_widget_t widget, uint32_t rgb888,
                             uint8_t width, uint16_t radius);

    int (*add_slider)(void* host_context, int16_t x, int16_t y,
                      int16_t width, int16_t height, int32_t minimum,
                      int32_t maximum, int32_t value,
                      uint32_t track_rgb888, uint32_t indicator_rgb888,
                      uint32_t knob_rgb888,
                      metalio_app_value_callback_t callback,
                      void* app_context, metalio_app_widget_t* widget);
    int (*set_slider_value)(void* host_context,
                            metalio_app_widget_t widget, int32_t value);
    int (*set_slider_colors)(void* host_context,
                             metalio_app_widget_t widget,
                             uint32_t track_rgb888,
                             uint32_t indicator_rgb888,
                             uint32_t knob_rgb888);

    int (*add_action_segment)(
        void* host_context, const char* const* labels, uint8_t count,
        uint8_t selected, metalio_app_selection_callback_t callback,
        void* app_context, metalio_app_widget_t* widget);
    int (*set_action_segment_selected)(void* host_context,
                                       metalio_app_widget_t widget,
                                       uint8_t selected);
    int (*add_action_picker)(
        void* host_context, const char* const* labels, uint32_t count,
        uint32_t selected, metalio_app_selection_callback_t callback,
        void* app_context, metalio_app_widget_t* widget);
    int (*set_action_picker_selected)(void* host_context,
                                      metalio_app_widget_t widget,
                                      uint32_t selected);

    /* ABI 1 mutable image extension. Appended to preserve older ABI-1 layouts. */
    int (*add_image_ex)(void* host_context, const char* asset_relative_path,
                        int16_t x, int16_t y, int16_t width, int16_t height,
                        metalio_app_widget_t* widget);
    int (*set_image_source)(void* host_context, metalio_app_widget_t widget,
                            const char* asset_relative_path);
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
