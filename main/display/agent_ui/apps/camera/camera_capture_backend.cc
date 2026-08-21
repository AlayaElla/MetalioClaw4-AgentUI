#include "camera_capture_backend.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "IOExpander.hpp"
#include "effects/camera_effects.h"
#include "driver/i2c_master.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#if CONFIG_SOC_PPA_SUPPORTED
#include "esp_memory_utils.h"
#include "esp_private/esp_cache_private.h"
#endif
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_caps.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#if CONFIG_SOC_PPA_SUPPORTED
#include "driver/ppa.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

extern "C" esp_err_t esp_lcd_nv3051f_replay_vendor_init(
    esp_lcd_panel_io_handle_t io);
extern "C" esp_lcd_panel_io_handle_t metalio_claw_4_get_panel_io();
extern "C" i2c_master_bus_handle_t metalio_claw_4_get_i2c_bus();

namespace agent_ui::camera {
namespace {

constexpr char kTag[] = "CameraBackend";
constexpr int kCameraBufferNum = 2;
constexpr int kStillCameraBufferNum = 2;
constexpr int kStillWarmupFrameCount = 2;
constexpr int kPreviewBufferNum = 2;
constexpr int kCameraAreaW = 720;
constexpr int kCameraAreaH = 526;
constexpr int kPreviewFrameW = 720;
constexpr int kPreviewFrameH = 526;
constexpr int kPreviewSensorW = 720;
constexpr int kPreviewSensorH = 720;
constexpr int kPreviewCropOffsetY =
    (kPreviewSensorH - kPreviewFrameH) / 2;
constexpr int kStillSensorW = 1920;
constexpr int kStillSensorH = 1080;
constexpr int kReviewBufferNum = 2;
constexpr int kReviewImageW = 600;
constexpr int kReviewImageH = 394;
constexpr int kReviewFramePad = 14;
constexpr int kReviewFrameBottom = 40;
constexpr int kReviewFrameW = kReviewImageW + 2 * kReviewFramePad;
constexpr int kReviewFrameH = kReviewFramePad + kReviewImageH + kReviewFrameBottom;
constexpr int kCamPowerOnSettleMs = 200;
constexpr int kCamXclkSettleMs = 50;
constexpr int kCamResetRecoverMs = 120;
constexpr int kCamXclkPin = 32;
constexpr int kCamXclkFreq = 24000000;
constexpr int kSccbI2cFreq = 100000;

// The review surface is composed off the LVGL thread.  These RGB565 values
// mirror the light theme defaults; keeping them here avoids a hardware worker
// dependency on LVGL/theme state while retaining the framed-card layout.
constexpr uint16_t kReviewSurfaceRgb565 = 0xFFFF;
constexpr uint16_t kReviewBorderRgb565 = 0xB5B6;
constexpr size_t kPpaFallbackAlignment = 64;

size_t PpaCacheAlignment(uint32_t caps) {
#if CONFIG_SOC_PPA_SUPPORTED
    size_t alignment = 0;
    if (esp_cache_get_alignment(caps, &alignment) == ESP_OK && alignment != 0) {
        return alignment;
    }
#else
    (void)caps;
#endif
    return kPpaFallbackAlignment;
}

size_t PpaAlignedSize(uint32_t caps, size_t size) {
    const size_t alignment = PpaCacheAlignment(caps);
    return ((size + alignment - 1) / alignment) * alignment;
}

struct CameraDev {
    int fd = -1;
    uint8_t* buffer[kCameraBufferNum] = {};
    uint32_t buffer_size[kCameraBufferNum] = {};
    int buffer_count = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pixel_format = 0;
    uint32_t bytes_per_pixel = 0;
    uint32_t stride = 0;
};

using effects::PackRgb565;

bool PpaRotateCropRgb565ToRgb888(const uint8_t* src, const CameraDev& cam,
                                 uint8_t* dst, void*& handle) {
#if CONFIG_SOC_PPA_SUPPORTED
    auto client = reinterpret_cast<ppa_client_handle_t>(handle);
    if (client == nullptr) {
        ppa_client_config_t client_config = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        if (ppa_register_client(&client_config, &client) != ESP_OK) {
            ESP_LOGE(kTag, "register preview PPA client failed");
            return false;
        }
        handle = reinterpret_cast<void*>(client);
    }

    ppa_srm_oper_config_t config = {};
    config.in.buffer = const_cast<uint8_t*>(src);
    config.in.pic_w = cam.width;
    config.in.pic_h = cam.height;
    config.in.block_w = kPreviewFrameW;
    config.in.block_h = kPreviewFrameH;
    config.in.block_offset_x = (cam.width - kPreviewFrameW) / 2;
    config.in.block_offset_y = kPreviewCropOffsetY;
    config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    config.out.buffer = dst;
    config.out.buffer_size = static_cast<uint32_t>(PpaAlignedSize(
        MALLOC_CAP_SPIRAM,
        static_cast<size_t>(kPreviewFrameW) * kPreviewFrameH * 3));
    config.out.pic_w = kPreviewFrameW;
    config.out.pic_h = kPreviewFrameH;
    config.out.block_offset_x = 0;
    config.out.block_offset_y = 0;
    config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
    config.rotation_angle = PPA_SRM_ROTATION_ANGLE_180;
    config.scale_x = 1.0f;
    config.scale_y = 1.0f;
    config.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(client, &config) == ESP_OK;
#else
    (void)src;
    (void)cam;
    (void)dst;
    (void)handle;
    return false;
#endif
}

bool PpaCaptureRgb565(const uint8_t* src, const CameraDev& cam,
                      uint8_t* dst, size_t dst_size, void*& handle) {
#if CONFIG_SOC_PPA_SUPPORTED
    auto client = reinterpret_cast<ppa_client_handle_t>(handle);
    if (client == nullptr) return false;

    ppa_srm_oper_config_t config = {};
    config.in.buffer = const_cast<uint8_t*>(src);
    config.in.pic_w = cam.width;
    config.in.pic_h = cam.height;
    config.in.block_w = kPreviewSensorW;
    config.in.block_h = kPreviewSensorH;
    config.in.block_offset_x = 0;
    config.in.block_offset_y = 0;
    config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    config.out.buffer = dst;
    config.out.buffer_size = static_cast<uint32_t>(dst_size);
    config.out.pic_w = kPreviewSensorW;
    config.out.pic_h = kPreviewSensorH;
    config.out.block_offset_x = 0;
    config.out.block_offset_y = 0;
    config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    config.rotation_angle = PPA_SRM_ROTATION_ANGLE_180;
    config.scale_x = 1.0f;
    config.scale_y = 1.0f;
    config.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(client, &config) == ESP_OK;
#else
    (void)src;
    (void)cam;
    (void)dst;
    (void)dst_size;
    (void)handle;
    return false;
#endif
}

void Rgb888FrameToRgb565(const uint8_t* src, uint8_t* dst_bytes) {
    auto* dst = reinterpret_cast<uint16_t*>(dst_bytes);
    for (int y = 0; y < kCameraAreaH; ++y) {
        for (int x = 0; x < kCameraAreaW; ++x) {
            // ISP, PPA and LVGL all expose RGB888 as BGR24 in memory.
            *dst++ = PackRgb565(src[2], src[1], src[0]);
            src += 3;
        }
    }
}

void FillReviewSurface(uint8_t* destination) {
    auto* pixels = reinterpret_cast<uint16_t*>(destination);
    std::fill(pixels, pixels + static_cast<size_t>(kReviewFrameW) * kReviewFrameH,
              kReviewSurfaceRgb565);
    for (int x = 0; x < kReviewFrameW; ++x) {
        pixels[x] = kReviewBorderRgb565;
        pixels[(kReviewFrameH - 1) * kReviewFrameW + x] = kReviewBorderRgb565;
    }
    for (int y = 1; y < kReviewFrameH - 1; ++y) {
        pixels[y * kReviewFrameW] = kReviewBorderRgb565;
        pixels[y * kReviewFrameW + kReviewFrameW - 1] = kReviewBorderRgb565;
    }
}

void ScaleReviewRgb888ToRgb565(const uint8_t* source, uint8_t* destination) {
    auto* dst = reinterpret_cast<uint16_t*>(destination);
    const uint32_t x_step = (static_cast<uint32_t>(kCameraAreaW) << 16) /
                            kReviewImageW;
    const uint32_t y_step = (static_cast<uint32_t>(kCameraAreaH) << 16) /
                            kReviewImageH;
    uint32_t y_acc = 0;
    for (int y = 0; y < kReviewImageH; ++y) {
        const int source_y = std::min<int>(y_acc >> 16, kCameraAreaH - 1);
        uint32_t x_acc = 0;
        for (int x = 0; x < kReviewImageW; ++x) {
            const int source_x = std::min<int>(x_acc >> 16, kCameraAreaW - 1);
            const uint8_t* pixel =
                source + (source_y * kPreviewFrameW +
                          source_x) * 3;
            dst[(y + kReviewFramePad) * kReviewFrameW + x + kReviewFramePad] =
                PackRgb565(pixel[2], pixel[1], pixel[0]);
            x_acc += x_step;
        }
        y_acc += y_step;
    }
}

}  // namespace

struct CaptureBackend::Impl : std::enable_shared_from_this<CaptureBackend::Impl> {
    EventSink sink;
    mutable std::mutex sink_mutex;
    std::atomic<bool> running{false};
    std::atomic<bool> frozen{false};
    std::atomic<EffectStyle> effect_style{EffectStyle::Original};
    std::atomic<bool> effect_dark_mode{false};
    std::atomic<uint32_t> generation{0};
    std::atomic<int> front_index{0};
    std::atomic<int> pending_index{-1};
    std::atomic<bool> frame_pending{false};
    std::atomic<int64_t> pending_submitted_us{0};
    std::mutex performance_mutex;
    uint32_t ack_count = 0;
    uint32_t ack_total_us = 0;
    uint32_t ack_max_us = 0;
    std::atomic<bool> review_pending{false};
    std::atomic<int> review_source_index{-1};
    std::atomic<uint32_t> review_generation{0};
    std::atomic<TaskHandle_t> task_handle{nullptr};
    SemaphoreHandle_t worker_slot = nullptr;
    CameraDev camera{};
    esp_cam_sensor_xclk_handle_t xclk = nullptr;
    bool video_initialized = false;
    void* ppa_handle = nullptr;
    uint32_t ppa_hits = 0;
    uint32_t ppa_failures = 0;
    uint8_t* display_buffers[kPreviewBufferNum] = {};
    std::shared_ptr<uint8_t> display_owners[kPreviewBufferNum];
    uint8_t* effect_scratch = nullptr;
    std::shared_ptr<uint8_t> effect_scratch_owner;
    uint8_t* review_buffers[kReviewBufferNum] = {};
    std::shared_ptr<DecodedImage> review_images[kReviewBufferNum] = {};
    mutable std::mutex captured_frame_mutex;
    std::shared_ptr<const PreviewFrame> captured_frame;
    int review_slot = 0;

    ~Impl() {
        running.store(false, std::memory_order_release);
        if (worker_slot != nullptr) {
            vSemaphoreDelete(worker_slot);
            worker_slot = nullptr;
        }
#if CONFIG_SOC_PPA_SUPPORTED
        if (ppa_handle != nullptr) {
            ppa_unregister_client(
                reinterpret_cast<ppa_client_handle_t>(ppa_handle));
            ppa_handle = nullptr;
        }
#endif
        for (int i = 0; i < kPreviewBufferNum; ++i) {
            display_owners[i].reset();
            display_buffers[i] = nullptr;
        }
        effect_scratch_owner.reset();
        effect_scratch = nullptr;
        // DecodedImage pixel owners release review slots after any queued
        // ReviewReady/ViewState references are gone.  Do not free the raw
        // pointers here while those shared owners may still be live.
        for (auto& image : review_images) image.reset();
        for (auto& buffer : review_buffers) buffer = nullptr;
    }

    void Emit(Event event) {
        if (event.generation != 0 &&
            event.generation != generation.load(std::memory_order_acquire)) {
            return;
        }
        EventSink callback;
        {
            std::lock_guard<std::mutex> lock(sink_mutex);
            callback = sink;
        }
        if (callback) callback(event);
    }

    void EmitStatus(const char* text,
                    StatusCode code = StatusCode::BackendMessage) {
        Emit(Event::Status(generation.load(std::memory_order_acquire), text,
                           code));
    }

    bool PrepareBuffers() {
        const size_t buffer_alignment = PpaCacheAlignment(MALLOC_CAP_SPIRAM);
        const size_t display_size = PpaAlignedSize(
            MALLOC_CAP_SPIRAM,
            static_cast<size_t>(kPreviewFrameW) * kPreviewFrameH * 3);
        for (int i = 0; i < kPreviewBufferNum; ++i) {
            if (display_buffers[i] == nullptr) {
                display_buffers[i] = static_cast<uint8_t*>(
                    heap_caps_aligned_alloc(
                        buffer_alignment, display_size,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (display_buffers[i] == nullptr) {
                    ESP_LOGE(kTag,
                             "alloc RGB888 display buffer failed (%u bytes)",
                             static_cast<unsigned>(display_size));
                    return false;
                }
                display_owners[i] = std::shared_ptr<uint8_t>(
                    display_buffers[i],
                    [](uint8_t* value) {
                        if (value != nullptr) heap_caps_free(value);
                    });
                std::memset(display_buffers[i], 0, display_size);
            }
        }
        if (effect_scratch == nullptr) {
            effect_scratch = static_cast<uint8_t*>(heap_caps_malloc(
                effects::kEffectScratchBytes,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (effect_scratch == nullptr) {
                ESP_LOGE(kTag, "alloc effect scratch failed (%u bytes)",
                         static_cast<unsigned>(effects::kEffectScratchBytes));
                return false;
            }
            effect_scratch_owner = std::shared_ptr<uint8_t>(
                effect_scratch, [](uint8_t* value) {
                    if (value != nullptr) heap_caps_free(value);
                });
            std::memset(effect_scratch, 0, effects::kEffectScratchBytes);
        }
        const size_t review_size = PpaAlignedSize(
            MALLOC_CAP_SPIRAM,
            static_cast<size_t>(kReviewFrameW) * kReviewFrameH * 2);
        const size_t review_alignment = PpaCacheAlignment(MALLOC_CAP_SPIRAM);
        for (int i = 0; i < kReviewBufferNum; ++i) {
            if (review_buffers[i] == nullptr) {
                review_buffers[i] = static_cast<uint8_t*>(heap_caps_aligned_alloc(
                    review_alignment, review_size,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (review_buffers[i] == nullptr) {
                    ESP_LOGE(kTag, "alloc review buffer failed (%u bytes)",
                             static_cast<unsigned>(review_size));
                    return false;
                }
                std::memset(review_buffers[i], 0, review_size);
            }
            if (review_images[i] == nullptr) {
                // Keep each preallocated slot alive while an Event/ViewState
                // still references its composed image.
                auto pixels = std::shared_ptr<uint8_t>(
                    review_buffers[i], [](uint8_t* value) {
                        if (value != nullptr) heap_caps_free(value);
                    });
                review_images[i] = std::make_shared<DecodedImage>(DecodedImage{
                    .pixels = std::move(pixels),
                    .data_size = review_size,
                    .width = kReviewFrameW,
                    .height = kReviewFrameH,
                    .stride = static_cast<size_t>(kReviewFrameW) * 2,
                });
            }
        }
        front_index.store(0, std::memory_order_release);
        pending_index.store(-1, std::memory_order_release);
        frame_pending.store(false, std::memory_order_release);
        pending_submitted_us.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(performance_mutex);
            ack_count = 0;
            ack_total_us = 0;
            ack_max_us = 0;
        }
        review_pending.store(false, std::memory_order_release);
        review_source_index.store(-1, std::memory_order_release);
        review_generation.store(0, std::memory_order_release);
        ppa_hits = 0;
        ppa_failures = 0;
        review_slot = 0;
        return true;
    }

    esp_err_t InitVideo() {
        if (video_initialized) return ESP_OK;
        esp_cam_sensor_xclk_config_t xclk_config = {};
        xclk_config.esp_clock_router_cfg.xclk_pin =
            static_cast<gpio_num_t>(kCamXclkPin);
        xclk_config.esp_clock_router_cfg.xclk_freq_hz = kCamXclkFreq;
        ESP_RETURN_ON_ERROR(
            esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &xclk),
            kTag, "xclk allocate failed");
        ESP_RETURN_ON_ERROR(esp_cam_sensor_xclk_start(xclk, &xclk_config),
                            kTag, "xclk start failed");
        vTaskDelay(pdMS_TO_TICKS(kCamXclkSettleMs));

        esp_video_init_csi_config_t csi_config = {};
        csi_config.reset_pin = static_cast<gpio_num_t>(-1);
        csi_config.pwdn_pin = static_cast<gpio_num_t>(-1);
        i2c_master_bus_handle_t bus = metalio_claw_4_get_i2c_bus();
        if (bus == nullptr) {
            esp_cam_sensor_xclk_stop(xclk);
            esp_cam_sensor_xclk_free(xclk);
            xclk = nullptr;
            return ESP_ERR_INVALID_STATE;
        }
        csi_config.sccb_config.init_sccb = false;
        csi_config.sccb_config.i2c_handle = bus;
        csi_config.sccb_config.freq = kSccbI2cFreq;
        const esp_video_init_config_t config = {.csi = &csi_config};
        const esp_err_t error = esp_video_init(&config);
        if (error != ESP_OK) {
            esp_cam_sensor_xclk_stop(xclk);
            esp_cam_sensor_xclk_free(xclk);
            xclk = nullptr;
            return error;
        }
        video_initialized = true;
        return ESP_OK;
    }

    void DeinitVideo() {
        if (video_initialized) {
            esp_video_deinit();
            video_initialized = false;
        }
        if (xclk != nullptr) {
            esp_cam_sensor_xclk_stop(xclk);
            esp_cam_sensor_xclk_free(xclk);
            xclk = nullptr;
        }
    }

    esp_err_t ConfigureSensorFormat(int fd, bool preview) {
        esp_cam_sensor_format_t requested = {};
        requested.format = ESP_CAM_SENSOR_PIXFORMAT_RAW10;
        requested.port = ESP_CAM_SENSOR_MIPI_CSI;
        requested.xclk = kCamXclkFreq;
        requested.width = preview ? kPreviewSensorW : kStillSensorW;
        requested.height = preview ? kPreviewSensorH : kStillSensorH;
        requested.fps = 25;
        if (ioctl(fd, VIDIOC_S_SENSOR_FMT, &requested) != 0) {
            ESP_LOGE(kTag, "set %s sensor mode %ux%u failed",
                     preview ? "preview" : "still", requested.width,
                     requested.height);
            return ESP_FAIL;
        }

        esp_cam_sensor_format_t actual = {};
        if (ioctl(fd, VIDIOC_G_SENSOR_FMT, &actual) != 0 ||
            actual.width != requested.width ||
            actual.height != requested.height ||
            actual.format != requested.format) {
            ESP_LOGE(kTag,
                     "unexpected %s sensor mode: requested=%ux%u RAW10 "
                     "actual=%ux%u format=%d",
                     preview ? "preview" : "still", requested.width,
                     requested.height, actual.width, actual.height,
                     actual.format);
            return ESP_FAIL;
        }
        ESP_LOGI(kTag, "%s sensor mode %ux%u RAW10@%u",
                 preview ? "preview" : "still", actual.width, actual.height,
                 actual.fps);
        return ESP_OK;
    }

    esp_err_t ConfigureSensorOrientation(int fd, bool preview) {
        // OV2710's runtime HFLIP+VFLIP controls produce a black/noisy RAW
        // stream on this MIPI mode. Keep the sensor orientation at its
        // validated default while isolating the no-PPA preview path.
        const int enabled = 0;
        v4l2_ext_control control[2] = {};
        control[0].id = V4L2_CID_HFLIP;
        control[0].value = enabled;
        control[1].id = V4L2_CID_VFLIP;
        control[1].value = enabled;
        v4l2_ext_controls controls = {};
        controls.ctrl_class = V4L2_CTRL_CLASS_USER;
        controls.count = 2;
        controls.controls = control;
        if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
            ESP_LOGE(kTag,
                     "set sensor orientation hmirror=%d vflip=%d failed "
                     "at control=%u",
                     enabled, enabled, controls.error_idx);
            return ESP_FAIL;
        }
        ESP_LOGI(kTag, "%s sensor orientation: hmirror=%d vflip=%d",
                 preview ? "preview" : "still", enabled, enabled);
        return ESP_OK;
    }

    esp_err_t ConfigureSelection(int fd, bool preview, bool* applied) {
        if (applied == nullptr) return ESP_ERR_INVALID_ARG;
        *applied = false;
#if defined(ESP_VIDEO_ISP_DEVICE_CROP) && ESP_VIDEO_ISP_DEVICE_CROP
        esp_cam_sensor_format_t sensor_format = {};
        if (ioctl(fd, VIDIOC_G_SENSOR_FMT, &sensor_format) != 0 ||
            sensor_format.width == 0 || sensor_format.height == 0) {
            ESP_LOGE(kTag, "query sensor format failed");
            return ESP_FAIL;
        }

        v4l2_selection selection = {};
        selection.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        selection.target = V4L2_SEL_TGT_CROP;
        if (preview) {
            if (sensor_format.width < kPreviewFrameW ||
                sensor_format.height < kPreviewFrameH) {
                ESP_LOGE(kTag, "sensor format %dx%d is smaller than preview",
                         static_cast<int>(sensor_format.width),
                         static_cast<int>(sensor_format.height));
                return ESP_ERR_INVALID_SIZE;
            }
            // Keep the RAW Bayer crop origin even so the color phase remains
            // unchanged when ISP demosaics the cropped sensor window.
            selection.r.left = ((sensor_format.width - kPreviewFrameW) / 2) & ~1;
            selection.r.top = ((sensor_format.height - kPreviewFrameH) / 2) & ~1;
            selection.r.width = kPreviewFrameW;
            selection.r.height = kPreviewFrameH;
        } else {
            selection.r.left = 0;
            selection.r.top = 0;
            selection.r.width = sensor_format.width;
            selection.r.height = sensor_format.height;
        }
        if (ioctl(fd, VIDIOC_S_SELECTION, &selection) != 0) {
            ESP_LOGE(kTag, "set %s selection failed", preview ? "preview" : "still");
            return ESP_FAIL;
        }
        ESP_LOGI(kTag, "%s selection %dx%d at (%d,%d), sensor %dx%d",
                 preview ? "preview" : "still", selection.r.width,
                 selection.r.height, selection.r.left, selection.r.top,
                 static_cast<int>(sensor_format.width),
                 static_cast<int>(sensor_format.height));
        *applied = true;
        return ESP_OK;
#else
        (void)fd;
        (void)preview;
        static bool capability_logged = false;
        if (!capability_logged) {
            ESP_LOGW(kTag,
                     "CSI crop unavailable for configured ESP32-P4 revision; "
                     "using PPA center crop and 180-degree rotation");
            capability_logged = true;
        }
        return ESP_OK;
#endif
    }

    esp_err_t ConfigureFormat(int fd, bool preview) {
        v4l2_format format = {};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_G_FMT, &format) != 0) return ESP_FAIL;

        const uint32_t requested_format = V4L2_PIX_FMT_RGB565;
        format.fmt.pix.pixelformat = requested_format;
        if (ioctl(fd, VIDIOC_S_FMT, &format) != 0 ||
            format.fmt.pix.pixelformat != requested_format) {
            ESP_LOGE(kTag, "set %s pixel format failed (requested=0x%08" PRIx32
                           ", returned=0x%08" PRIx32 ")",
                     preview ? "preview RGB565" : "still RGB565",
                     requested_format, format.fmt.pix.pixelformat);
            return ESP_FAIL;
        }
        camera.width = format.fmt.pix.width;
        camera.height = format.fmt.pix.height;
        camera.pixel_format = format.fmt.pix.pixelformat;
        switch (camera.pixel_format) {
            case V4L2_PIX_FMT_RGB565:
            case V4L2_PIX_FMT_YUYV:
            case V4L2_PIX_FMT_UYVY:
                camera.bytes_per_pixel = 2;
                break;
            case V4L2_PIX_FMT_RGB24:
                camera.bytes_per_pixel = 3;
                break;
            default:
                return ESP_FAIL;
        }
        camera.stride = format.fmt.pix.bytesperline != 0
                            ? format.fmt.pix.bytesperline
                            : camera.width * camera.bytes_per_pixel;
        const uint32_t packed_stride =
            camera.width * camera.bytes_per_pixel;
        if (camera.stride != packed_stride) {
            ESP_LOGE(kTag,
                     "%s stream has unsupported padded stride=%" PRIu32
                     " packed=%" PRIu32,
                     preview ? "preview" : "still", camera.stride,
                     packed_stride);
            return ESP_ERR_NOT_SUPPORTED;
        }
        ESP_LOGI(kTag,
                 "%s stream %" PRIu32 "x%" PRIu32 " %s stride=%" PRIu32
                 " size=%" PRIu32,
                 preview ? "preview" : "still", camera.width, camera.height,
                 "RGB565", camera.stride,
                 format.fmt.pix.sizeimage);
        return ESP_OK;
    }

    esp_err_t OpenDevice(bool preview) {
        camera.fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
        if (camera.fd < 0) return ESP_FAIL;
        if (ConfigureSensorFormat(camera.fd, preview) != ESP_OK) {
            close(camera.fd);
            camera.fd = -1;
            return ESP_FAIL;
        }
        if (ConfigureSensorOrientation(camera.fd, preview) != ESP_OK) {
            close(camera.fd);
            camera.fd = -1;
            return ESP_FAIL;
        }
        bool selection_applied = false;
        if (ConfigureSelection(camera.fd, preview, &selection_applied) != ESP_OK) {
            close(camera.fd);
            camera.fd = -1;
            return ESP_FAIL;
        }
        if (ConfigureFormat(camera.fd, preview) != ESP_OK) {
            close(camera.fd);
            camera.fd = -1;
            return ESP_FAIL;
        }
        if (preview &&
            (camera.width < kPreviewFrameW ||
             camera.height < kPreviewFrameH)) {
            ESP_LOGE(kTag,
                     "preview stream %" PRIu32 "x%" PRIu32
                     " is smaller than %dx%d",
                     camera.width, camera.height, kPreviewFrameW,
                     kPreviewFrameH);
            close(camera.fd);
            camera.fd = -1;
            return ESP_FAIL;
        }
        if (preview && selection_applied &&
            (camera.width != kPreviewFrameW ||
             camera.height != kPreviewFrameH)) {
            ESP_LOGE(kTag, "unexpected preview format %" PRIu32 "x%" PRIu32,
                     camera.width, camera.height);
            close(camera.fd);
            camera.fd = -1;
            return ESP_FAIL;
        }
        v4l2_requestbuffers request = {};
        request.count = preview ? kCameraBufferNum : kStillCameraBufferNum;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (ioctl(camera.fd, VIDIOC_REQBUFS, &request) != 0) {
            ESP_LOGE(
                kTag,
                "%s buffer allocation failed: count=%u frame=%" PRIu32
                " bytes, PSRAM free=%u largest=%u",
                preview ? "preview" : "still", request.count,
                camera.width * camera.height * camera.bytes_per_pixel,
                static_cast<unsigned>(
                    heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                static_cast<unsigned>(
                    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
            close(camera.fd);
            camera.fd = -1;
            return ESP_FAIL;
        }
        camera.buffer_count = std::min<int>(request.count, kCameraBufferNum);
        const int minimum_buffer_count = preview ? 2 : 1;
        if (camera.buffer_count < minimum_buffer_count) {
            ESP_LOGE(kTag,
                     "%s returned only %d buffers; preview requires %d",
                     preview ? "preview" : "still", camera.buffer_count,
                     minimum_buffer_count);
            CloseDevice();
            return ESP_FAIL;
        }
        for (int i = 0; i < camera.buffer_count; ++i) {
            v4l2_buffer buffer = {};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = i;
            if (ioctl(camera.fd, VIDIOC_QUERYBUF, &buffer) != 0) {
                CloseDevice();
                return ESP_FAIL;
            }
            camera.buffer[i] = static_cast<uint8_t*>(mmap(
                nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                camera.fd, buffer.m.offset));
            if (camera.buffer[i] == MAP_FAILED) {
                camera.buffer[i] = nullptr;
                CloseDevice();
                return ESP_FAIL;
            }
            camera.buffer_size[i] = buffer.length;
            if (ioctl(camera.fd, VIDIOC_QBUF, &buffer) != 0) {
                CloseDevice();
                return ESP_FAIL;
            }
        }
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(camera.fd, VIDIOC_STREAMON, &type) != 0) {
            CloseDevice();
            return ESP_FAIL;
        }
        ESP_LOGI(kTag, "%s capture started: buffers=%d io=%s",
                 preview ? "preview" : "still", camera.buffer_count,
                 "blocking");
        if (preview) {
            front_index.store(0, std::memory_order_release);
            pending_index.store(-1, std::memory_order_release);
            frame_pending.store(false, std::memory_order_release);
        }
        return ESP_OK;
    }

    void CloseDevice() {
        if (camera.fd < 0) return;
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(camera.fd, VIDIOC_STREAMOFF, &type);
        for (int i = 0; i < kCameraBufferNum; ++i) {
            if (camera.buffer[i] != nullptr) {
                munmap(camera.buffer[i], camera.buffer_size[i]);
                camera.buffer[i] = nullptr;
                camera.buffer_size[i] = 0;
            }
        }
        close(camera.fd);
        camera.fd = -1;
        camera.buffer_count = 0;
    }

    std::shared_ptr<const PreviewFrame> FrameFor(int index,
                                                 uint32_t value) const {
        if (index < 0 || index >= kPreviewBufferNum ||
            display_buffers[index] == nullptr) {
            return nullptr;
        }
        return std::make_shared<PreviewFrame>(PreviewFrame{
            .data = display_buffers[index],
            .owned_data = display_owners[index],
            .width = kPreviewFrameW,
            .height = kPreviewFrameH,
            .buffer_index = index,
            .generation = value,
        });
    }

    int FindReviewSlot() {
        const int preferred = (review_slot + 1) % kReviewBufferNum;
        for (int offset = 0; offset < kReviewBufferNum; ++offset) {
            const int candidate = (preferred + offset) % kReviewBufferNum;
            if (review_images[candidate] != nullptr &&
                review_images[candidate].use_count() == 1) {
                review_slot = candidate;
                return candidate;
            }
        }
        return -1;
    }

    bool ComposeReview(int source_index, int* output_slot) {
        if (output_slot == nullptr || source_index < 0 ||
            source_index >= kPreviewBufferNum ||
            display_buffers[source_index] == nullptr) {
            return false;
        }
        const int slot = FindReviewSlot();
        if (slot < 0 || review_buffers[slot] == nullptr ||
            review_images[slot] == nullptr) {
            return false;
        }
        FillReviewSurface(review_buffers[slot]);
        // The exact 600x394 inset is composed on the capture worker so the
        // LVGL descriptor receives a stable RGB565 review surface. This
        // one-time conversion is outside the live preview loop.
        ScaleReviewRgb888ToRgb565(display_buffers[source_index],
                                  review_buffers[slot]);
        *output_slot = slot;
        return true;
    }

    void EmitReviewFailure(uint32_t value, const char* text) {
        frozen.store(false, std::memory_order_release);
        Event event;
        event.type = EventType::ReviewReady;
        event.generation = value;
        event.success = false;
        event.text = text != nullptr ? text : "review_compose_failed";
        Emit(std::move(event));
    }

    void RunWorker(uint32_t worker_generation) {
        xSemaphoreTake(worker_slot, portMAX_DELAY);
        if (!running.load(std::memory_order_acquire)) {
            xSemaphoreGive(worker_slot);
            task_handle.store(nullptr, std::memory_order_release);
            vTaskDelete(nullptr);
            return;
        }

        auto& io = IOExpander::getInstance();
        io.setLevel(IOExpander::Pin::CAM_PWDN, false);
        vTaskDelay(pdMS_TO_TICKS(kCamPowerOnSettleMs));
        if (!running.load(std::memory_order_acquire)) {
            io.setLevel(IOExpander::Pin::CAM_PWDN, true);
            task_handle.store(nullptr, std::memory_order_release);
            xSemaphoreGive(worker_slot);
            return;
        }
        if (InitVideo() != ESP_OK) {
            EmitStatus("Camera initialization failed",
                       StatusCode::CameraStartupFailed);
        } else {
            vTaskDelay(pdMS_TO_TICKS(kCamResetRecoverMs));
            if (running.load(std::memory_order_acquire)) {
                if (auto panel = metalio_claw_4_get_panel_io(); panel != nullptr) {
                    esp_lcd_nv3051f_replay_vendor_init(panel);
                }
                if (OpenDevice(true) != ESP_OK) {
                    EmitStatus("Camera device open failed",
                               StatusCode::CameraStartupFailed);
                } else {
                    int64_t stats_window_started_us = esp_timer_get_time();
                    uint32_t dq_window_frames = 0;
                    uint32_t submit_window_frames = 0;
                    uint32_t pending_drop_window_frames = 0;
                    uint32_t frozen_window_frames = 0;
                    uint32_t transform_window_count = 0;
                    uint64_t transform_window_total_us = 0;
                    uint32_t transform_window_max_us = 0;
                    uint32_t effect_window_count = 0;
                    uint64_t effect_window_total_us = 0;
                    uint32_t effect_window_max_us = 0;
                    uint32_t print_stage_window_count = 0;
                    uint64_t print_radii_window_total_us = 0;
                    uint64_t print_base_window_total_us = 0;
                    uint64_t print_dots_window_total_us = 0;
                    while (running.load(std::memory_order_acquire) &&
                           generation.load(std::memory_order_acquire) ==
                               worker_generation) {
                        v4l2_buffer buffer = {};
                        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                        buffer.memory = V4L2_MEMORY_MMAP;
                        if (ioctl(camera.fd, VIDIOC_DQBUF, &buffer) != 0) {
                            vTaskDelay(pdMS_TO_TICKS(10));
                            continue;
                        }
                        if (!running.load(std::memory_order_acquire) ||
                            generation.load(std::memory_order_acquire) !=
                                worker_generation) {
                            ioctl(camera.fd, VIDIOC_QBUF, &buffer);
                            break;
                        }
                        ++dq_window_frames;
                        bool capture_handled = false;
                        if (review_pending.exchange(false, std::memory_order_acq_rel)) {
                            const uint32_t request_generation =
                                review_generation.load(std::memory_order_acquire);
                            if (running.load(std::memory_order_acquire) &&
                                generation.load(std::memory_order_acquire) ==
                                    worker_generation &&
                                request_generation == worker_generation &&
                                frozen.load(std::memory_order_acquire)) {
                                int output_slot = -1;
                                const int source_index = review_source_index.load(
                                    std::memory_order_acquire);
                                if (ComposeReview(source_index, &output_slot)) {
                                    const size_t still_data_size =
                                        static_cast<size_t>(kPreviewSensorW) *
                                        kPreviewSensorH * sizeof(uint16_t);
                                    const size_t still_alloc_size =
                                        PpaAlignedSize(MALLOC_CAP_SPIRAM,
                                                       still_data_size);
                                    auto* still_pixels =
                                        static_cast<uint8_t*>(
                                            heap_caps_aligned_alloc(
                                                PpaCacheAlignment(
                                                    MALLOC_CAP_SPIRAM),
                                                still_alloc_size,
                                                MALLOC_CAP_SPIRAM |
                                                    MALLOC_CAP_8BIT));
                                    const int64_t still_started_us =
                                        esp_timer_get_time();
                                    const bool captured =
                                        still_pixels != nullptr &&
                                        buffer.index < static_cast<uint32_t>(
                                            camera.buffer_count) &&
                                        PpaCaptureRgb565(
                                            camera.buffer[buffer.index], camera,
                                            still_pixels, still_alloc_size,
                                            ppa_handle);
                                    if (captured) {
                                        const EffectStyle captured_style =
                                            effect_style.load(
                                                std::memory_order_acquire);
                                        const bool captured_dark_mode =
                                            effect_dark_mode.load(
                                                std::memory_order_acquire);
                                        const int64_t effect_started_us =
                                            esp_timer_get_time();
                                        effects::EffectStageTiming stage_timing;
                                        effects::Rgb565Pixels captured_pixels{
                                            reinterpret_cast<uint16_t*>(
                                                still_pixels)};
                                        const effects::EffectContext
                                            effect_context{
                                                kPreviewSensorW,
                                                kPreviewSensorH,
                                                0,
                                                captured_dark_mode,
                                                static_cast<uint32_t>(
                                                    effect_started_us /
                                                    effects::
                                                        kAnimationFrameIntervalUs),
                                                effect_scratch,
                                                effects::kEffectScratchBytes,
                                                &stage_timing,
                                            };
                                        effects::ApplyEffect(
                                            captured_pixels, captured_style,
                                            effect_context);
                                        const int64_t effect_elapsed_us =
                                            esp_timer_get_time() -
                                            effect_started_us;
                                        auto still_owner =
                                            std::shared_ptr<uint8_t>(
                                                still_pixels,
                                                [](uint8_t* value) {
                                                    heap_caps_free(value);
                                                });
                                        auto still =
                                            std::make_shared<PreviewFrame>(
                                                PreviewFrame{
                                                    .data = still_owner.get(),
                                                    .owned_data =
                                                        std::move(still_owner),
                                                    .width = kPreviewSensorW,
                                                    .height = kPreviewSensorH,
                                                    .buffer_index = -1,
                                                    .generation =
                                                        request_generation,
                                                });
                                        ESP_LOGI(
                                            kTag,
                                            "captured current preview still %dx%d in %lld ms",
                                            still->width, still->height,
                                            static_cast<long long>(
                                                (esp_timer_get_time() -
                                                 still_started_us) /
                                                1000));
                                        ESP_LOGI(
                                            kTag,
                                            "captured effect=%s processing=%lld ms "
                                            "print_radii/base/dots=%.2f/%.2f/%.2f ms",
                                            effects::EffectName(captured_style),
                                            static_cast<long long>(
                                                effect_elapsed_us / 1000),
                                            stage_timing.capture_radii_us /
                                                1000.0,
                                            stage_timing.render_base_us /
                                                1000.0,
                                            stage_timing.render_dots_us /
                                                1000.0);
                                        {
                                            std::lock_guard<std::mutex> lock(
                                                captured_frame_mutex);
                                            captured_frame = still;
                                        }
                                        Event event;
                                        event.type = EventType::ReviewReady;
                                        event.generation = request_generation;
                                        event.success = true;
                                        event.frame = FrameFor(
                                            source_index, request_generation);
                                        event.review_image =
                                            review_images[output_slot];
                                        Emit(std::move(event));
                                        ioctl(camera.fd, VIDIOC_QBUF, &buffer);
                                        capture_handled = true;
                                    } else {
                                        if (still_pixels != nullptr) {
                                            heap_caps_free(still_pixels);
                                        } else {
                                            ESP_LOGE(
                                                kTag,
                                                "allocate 720x720 still buffer failed: "
                                                "PSRAM free=%u largest=%u",
                                                static_cast<unsigned>(
                                                    heap_caps_get_free_size(
                                                        MALLOC_CAP_SPIRAM)),
                                                static_cast<unsigned>(
                                                    heap_caps_get_largest_free_block(
                                                        MALLOC_CAP_SPIRAM)));
                                        }
                                        EmitReviewFailure(
                                            request_generation,
                                            "preview_still_capture_failed");
                                    }
                                } else {
                                    EmitReviewFailure(request_generation,
                                                      "review_compose_failed");
                                }
                            }
                        }
                        if (capture_handled) {
                            while (running.load(std::memory_order_acquire) &&
                                   generation.load(std::memory_order_acquire) ==
                                       worker_generation &&
                                   frozen.load(std::memory_order_acquire)) {
                                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
                            }
                            if (!running.load(std::memory_order_acquire) ||
                                generation.load(std::memory_order_acquire) !=
                                    worker_generation) {
                                break;
                            }
                            stats_window_started_us = esp_timer_get_time();
                            dq_window_frames = 0;
                            submit_window_frames = 0;
                            pending_drop_window_frames = 0;
                            frozen_window_frames = 0;
                            transform_window_count = 0;
                            transform_window_total_us = 0;
                            transform_window_max_us = 0;
                            effect_window_count = 0;
                            effect_window_total_us = 0;
                            effect_window_max_us = 0;
                            print_stage_window_count = 0;
                            print_radii_window_total_us = 0;
                            print_base_window_total_us = 0;
                            print_dots_window_total_us = 0;
                            {
                                std::lock_guard<std::mutex> lock(
                                    performance_mutex);
                                ack_count = 0;
                                ack_total_us = 0;
                                ack_max_us = 0;
                            }
                            continue;
                        }
                        const bool preview_frozen =
                            frozen.load(std::memory_order_acquire);
                        const bool preview_pending =
                            frame_pending.load(std::memory_order_acquire);
                        if (!preview_frozen && !preview_pending &&
                            buffer.index <
                                static_cast<uint32_t>(camera.buffer_count)) {
                            const int back =
                                front_index.load(std::memory_order_acquire) ^ 1;
                            const int64_t transform_started_us =
                                esp_timer_get_time();
                            const bool transformed = PpaRotateCropRgb565ToRgb888(
                                camera.buffer[buffer.index], camera,
                                display_buffers[back], ppa_handle);
                            const uint32_t transform_us = static_cast<uint32_t>(
                                esp_timer_get_time() - transform_started_us);
                            ++transform_window_count;
                            transform_window_total_us += transform_us;
                            transform_window_max_us =
                                std::max(transform_window_max_us, transform_us);
                            if (transformed) {
                                ++ppa_hits;
                                const EffectStyle active_effect =
                                    effect_style.load(
                                        std::memory_order_acquire);
                                if (active_effect != EffectStyle::Original) {
                                    const int64_t effect_started_us =
                                        esp_timer_get_time();
                                    effects::EffectStageTiming stage_timing;
                                    effects::Rgb888Pixels effect_pixels{
                                        display_buffers[back]};
                                    const effects::EffectContext
                                        effect_context{
                                            kPreviewFrameW,
                                            kPreviewFrameH,
                                            kPreviewCropOffsetY,
                                            effect_dark_mode.load(
                                                std::memory_order_acquire),
                                            static_cast<uint32_t>(
                                                effect_started_us /
                                                effects::
                                                    kAnimationFrameIntervalUs),
                                            effect_scratch,
                                            effects::kEffectScratchBytes,
                                            &stage_timing,
                                        };
                                    effects::ApplyEffect(
                                        effect_pixels, active_effect,
                                        effect_context);
                                    const uint32_t effect_us =
                                        static_cast<uint32_t>(
                                            esp_timer_get_time() -
                                            effect_started_us);
                                    ++effect_window_count;
                                    effect_window_total_us += effect_us;
                                    effect_window_max_us = std::max(
                                        effect_window_max_us, effect_us);
                                    if (active_effect ==
                                        EffectStyle::PrintComic) {
                                        ++print_stage_window_count;
                                        print_radii_window_total_us +=
                                            stage_timing.capture_radii_us;
                                        print_base_window_total_us +=
                                            stage_timing.render_base_us;
                                        print_dots_window_total_us +=
                                            stage_timing.render_dots_us;
                                    }
                                }
                                pending_index.store(back,
                                                    std::memory_order_release);
                                frame_pending.store(true,
                                                    std::memory_order_release);
                                pending_submitted_us.store(
                                    esp_timer_get_time(),
                                    std::memory_order_release);
                                Event event;
                                event.type = EventType::PreviewFrameReady;
                                event.generation = worker_generation;
                                event.frame = FrameFor(back, event.generation);
                                Emit(std::move(event));
                                ++submit_window_frames;
                            } else {
                                ++ppa_failures;
                            }
                        } else if (preview_frozen) {
                            ++frozen_window_frames;
                        } else if (preview_pending) {
                            ++pending_drop_window_frames;
                        }
                        ioctl(camera.fd, VIDIOC_QBUF, &buffer);
                        if (frozen.load(std::memory_order_acquire)) {
                            vTaskDelay(pdMS_TO_TICKS(30));
                        }
                        const int64_t now_us = esp_timer_get_time();
                        const int64_t elapsed_us =
                            now_us - stats_window_started_us;
                        if (elapsed_us >= 2000000) {
                            uint32_t window_ack_count = 0;
                            uint32_t window_ack_total_us = 0;
                            uint32_t window_ack_max_us = 0;
                            {
                                std::lock_guard<std::mutex> lock(
                                    performance_mutex);
                                window_ack_count = ack_count;
                                window_ack_total_us = ack_total_us;
                                window_ack_max_us = ack_max_us;
                                ack_count = 0;
                                ack_total_us = 0;
                                ack_max_us = 0;
                            }
                            const double seconds =
                                static_cast<double>(elapsed_us) / 1000000.0;
                            const double transform_avg_ms =
                                transform_window_count > 0
                                    ? static_cast<double>(
                                          transform_window_total_us) /
                                          transform_window_count / 1000.0
                                    : 0.0;
                            const double ack_avg_ms =
                                window_ack_count > 0
                                    ? static_cast<double>(window_ack_total_us) /
                                          window_ack_count / 1000.0
                                    : 0.0;
                            const double effect_avg_ms =
                                effect_window_count > 0
                                    ? static_cast<double>(
                                          effect_window_total_us) /
                                          effect_window_count / 1000.0
                                    : 0.0;
                            const double print_radii_avg_ms =
                                print_stage_window_count > 0
                                    ? static_cast<double>(
                                          print_radii_window_total_us) /
                                          print_stage_window_count / 1000.0
                                    : 0.0;
                            const double print_base_avg_ms =
                                print_stage_window_count > 0
                                    ? static_cast<double>(
                                          print_base_window_total_us) /
                                          print_stage_window_count / 1000.0
                                    : 0.0;
                            const double print_dots_avg_ms =
                                print_stage_window_count > 0
                                    ? static_cast<double>(
                                          print_dots_window_total_us) /
                                          print_stage_window_count / 1000.0
                                    : 0.0;
                            const EffectStyle active_effect =
                                effect_style.load(std::memory_order_acquire);
                            ESP_LOGI(
                                kTag,
                                "preview pipeline: dq=%.1f fps submit=%.1f fps "
                                "pending_drop=%.1f fps frozen=%" PRIu32
                                " PPA_avg/max=%.2f/%.2f ms "
                                "effect=%s avg/max=%.2f/%.2f ms "
                                "print_radii/base/dots=%.2f/%.2f/%.2f ms "
                                "ack_avg/max=%.2f/%.2f ms "
                                "PPA RGB565->RGB888 rotate_crop=%" PRIu32
                                "/%" PRIu32,
                                dq_window_frames / seconds,
                                submit_window_frames / seconds,
                                pending_drop_window_frames / seconds,
                                frozen_window_frames, transform_avg_ms,
                                transform_window_max_us / 1000.0,
                                effects::EffectName(active_effect), effect_avg_ms,
                                effect_window_max_us / 1000.0,
                                print_radii_avg_ms, print_base_avg_ms,
                                print_dots_avg_ms,
                                ack_avg_ms, window_ack_max_us / 1000.0,
                                ppa_hits, ppa_failures);
                            stats_window_started_us = now_us;
                            dq_window_frames = 0;
                            submit_window_frames = 0;
                            pending_drop_window_frames = 0;
                            frozen_window_frames = 0;
                            transform_window_count = 0;
                            transform_window_total_us = 0;
                            transform_window_max_us = 0;
                            effect_window_count = 0;
                            effect_window_total_us = 0;
                            effect_window_max_us = 0;
                            print_stage_window_count = 0;
                            print_radii_window_total_us = 0;
                            print_base_window_total_us = 0;
                            print_dots_window_total_us = 0;
                        }
                    }
                    CloseDevice();
                }
            }
            DeinitVideo();
        }
        io.setLevel(IOExpander::Pin::CAM_PWDN, true);
        const bool restart = running.load(std::memory_order_acquire);
        task_handle.store(nullptr, std::memory_order_release);
        xSemaphoreGive(worker_slot);
        if (restart) StartWorker();
    }

    struct WorkerContext {
        std::shared_ptr<Impl> owner;
        uint32_t generation = 0;
    };

    static void WorkerEntry(void* arg) {
        std::unique_ptr<WorkerContext> context(static_cast<WorkerContext*>(arg));
        if (context == nullptr || !context->owner) {
            vTaskDelete(nullptr);
            return;
        }
        auto owner = std::move(context->owner);
        owner->RunWorker(context->generation);
        owner.reset();
        vTaskDelete(nullptr);
    }

    void StartWorker() {
        if (task_handle.load(std::memory_order_acquire) != nullptr) return;
        TaskHandle_t handle = nullptr;
        auto* context = new WorkerContext{
            .owner = shared_from_this(),
            .generation = generation.load(std::memory_order_acquire),
        };
        if (xTaskCreatePinnedToCore(WorkerEntry, "cam_screen", 8 * 1024, context,
                                    tskIDLE_PRIORITY + 4, &handle, 0) == pdPASS) {
            task_handle.store(handle, std::memory_order_release);
        } else {
            delete context;
            running.store(false, std::memory_order_release);
            EmitStatus("Camera worker start failed",
                       StatusCode::CameraStartupFailed);
        }
    }
};

CaptureBackend::CaptureBackend() : impl_(std::make_shared<Impl>()) {}

CaptureBackend::~CaptureBackend() {
    if (impl_ != nullptr) {
        impl_->running.store(false, std::memory_order_release);
    }
    impl_.reset();
}

void CaptureBackend::SetEventSink(EventSink sink) {
    if (impl_ == nullptr) return;
    std::lock_guard<std::mutex> lock(impl_->sink_mutex);
    impl_->sink = std::move(sink);
}

bool CaptureBackend::PrepareBuffers() {
    return impl_ != nullptr && impl_->PrepareBuffers();
}

void CaptureBackend::Start(uint32_t generation) {
    if (impl_ == nullptr || !impl_->PrepareBuffers()) return;
    impl_->generation.store(generation, std::memory_order_release);
    impl_->frozen.store(false, std::memory_order_release);
    impl_->review_pending.store(false, std::memory_order_release);
    impl_->review_source_index.store(-1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(impl_->captured_frame_mutex);
        impl_->captured_frame.reset();
    }
    impl_->running.store(true, std::memory_order_release);
    Event event;
    event.type = EventType::PreviewStarted;
    event.generation = generation;
    impl_->Emit(std::move(event));
    if (impl_->worker_slot == nullptr) {
        impl_->worker_slot = xSemaphoreCreateMutex();
    }
    if (impl_->worker_slot != nullptr) impl_->StartWorker();
}

void CaptureBackend::Stop() {
    if (impl_ == nullptr) return;
    impl_->running.store(false, std::memory_order_release);
    impl_->frozen.store(false, std::memory_order_release);
    impl_->frame_pending.store(false, std::memory_order_release);
    impl_->pending_submitted_us.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(impl_->performance_mutex);
        impl_->ack_count = 0;
        impl_->ack_total_us = 0;
        impl_->ack_max_us = 0;
    }
    impl_->pending_index.store(-1, std::memory_order_release);
    impl_->review_pending.store(false, std::memory_order_release);
    impl_->review_source_index.store(-1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(impl_->captured_frame_mutex);
        impl_->captured_frame.reset();
    }
    if (TaskHandle_t worker = impl_->task_handle.load(std::memory_order_acquire);
        worker != nullptr) {
        xTaskNotifyGive(worker);
    }
    Event event;
    event.type = EventType::PreviewStopped;
    event.generation = impl_->generation.load(std::memory_order_acquire);
    impl_->Emit(std::move(event));
}

void CaptureBackend::Capture() {
    if (impl_ == nullptr || !impl_->running.load(std::memory_order_acquire)) return;
    const uint32_t generation = impl_->generation.load(std::memory_order_acquire);
    const int pending_source =
        impl_->pending_index.load(std::memory_order_acquire);
    const int source_index = pending_source >= 0
                                 ? pending_source
                                 : impl_->front_index.load(
                                       std::memory_order_acquire);
    if (impl_->task_handle.load(std::memory_order_acquire) == nullptr ||
        source_index < 0 || source_index >= kPreviewBufferNum ||
        impl_->display_buffers[source_index] == nullptr) {
        impl_->EmitReviewFailure(generation, "review_compose_unavailable");
        return;
    }
    impl_->frozen.store(true, std::memory_order_release);
    bool request_was_empty = false;
    if (!impl_->review_pending.compare_exchange_strong(
            request_was_empty, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        // Keep the first request intact; report the duplicate without
        // publishing a competing ReviewReady failure that could race it.
        impl_->EmitStatus("review_compose_busy");
        return;
    }
    impl_->review_source_index.store(source_index, std::memory_order_release);
    impl_->review_generation.store(generation, std::memory_order_release);
    if (TaskHandle_t worker = impl_->task_handle.load(std::memory_order_acquire);
        worker != nullptr) {
        // The worker normally wakes on the next camera frame; notify also
        // covers a paused/frozen stream without creating a capture task.
        xTaskNotifyGive(worker);
    }
}

void CaptureBackend::Resume() {
    if (impl_ == nullptr) return;
    impl_->review_pending.store(false, std::memory_order_release);
    impl_->review_source_index.store(-1, std::memory_order_release);
    impl_->frozen.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(impl_->captured_frame_mutex);
        impl_->captured_frame.reset();
    }
    if (TaskHandle_t worker = impl_->task_handle.load(std::memory_order_acquire);
        worker != nullptr) {
        xTaskNotifyGive(worker);
    }
}

void CaptureBackend::SetEffect(EffectStyle style, bool dark_mode) {
    if (impl_ == nullptr) return;
    const EffectStyle previous = impl_->effect_style.exchange(
        style, std::memory_order_acq_rel);
    const bool previous_dark = impl_->effect_dark_mode.exchange(
        dark_mode, std::memory_order_acq_rel);
    if (previous != style || previous_dark != dark_mode) {
        ESP_LOGI(kTag, "camera effect=%s mosaic_gap=%s",
                 effects::EffectName(style), dark_mode ? "dark" : "light");
    }
}

void CaptureBackend::AcknowledgeFrame(uint32_t generation, int buffer_index) {
    if (impl_ == nullptr) return;
    if (generation != impl_->generation.load(std::memory_order_acquire)) return;
    if (impl_->pending_index.load(std::memory_order_acquire) != buffer_index) return;
    const int64_t submitted_us =
        impl_->pending_submitted_us.exchange(0, std::memory_order_acq_rel);
    if (submitted_us > 0) {
        const int64_t elapsed_us = esp_timer_get_time() - submitted_us;
        if (elapsed_us > 0) {
            const uint32_t bounded_us = static_cast<uint32_t>(
                std::min<int64_t>(elapsed_us, UINT32_MAX));
            std::lock_guard<std::mutex> lock(impl_->performance_mutex);
            ++impl_->ack_count;
            impl_->ack_total_us += bounded_us;
            impl_->ack_max_us = std::max(impl_->ack_max_us, bounded_us);
        }
    }
    impl_->front_index.store(buffer_index, std::memory_order_release);
    impl_->pending_index.store(-1, std::memory_order_release);
    impl_->frame_pending.store(false, std::memory_order_release);
}

std::shared_ptr<const PreviewFrame> CaptureBackend::CurrentFrame(uint32_t generation) const {
    if (impl_ == nullptr) return nullptr;
    return impl_->FrameFor(
        impl_->front_index.load(std::memory_order_acquire), generation);
}

std::shared_ptr<const PreviewFrame> CaptureBackend::CopyCurrentFrame(uint32_t generation) const {
    if (impl_ == nullptr) return nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->captured_frame_mutex);
        if (impl_->captured_frame != nullptr &&
            impl_->captured_frame->generation == generation) {
            return impl_->captured_frame;
        }
    }
    const int source_index =
        impl_->front_index.load(std::memory_order_acquire);
    if (source_index < 0 || source_index >= kPreviewBufferNum ||
        impl_->display_buffers[source_index] == nullptr) {
        return nullptr;
    }
    const size_t size = static_cast<size_t>(kCameraAreaW) * kCameraAreaH * 2;
    uint8_t* copy = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) return nullptr;
    Rgb888FrameToRgb565(impl_->display_buffers[source_index], copy);
    auto owner = std::shared_ptr<uint8_t>(copy, [](uint8_t* value) {
        heap_caps_free(value);
    });
    return std::make_shared<PreviewFrame>(PreviewFrame{
        .data = owner.get(),
        .owned_data = std::move(owner),
        .width = kCameraAreaW,
        .height = kCameraAreaH,
        .buffer_index = -1,
        .generation = generation,
    });
}

}  // namespace agent_ui::camera
