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
constexpr int kPreviewBufferNum = 2;
constexpr int kCameraAreaW = 720;
constexpr int kCameraAreaH = 526;
constexpr int kReviewBufferNum = 2;
constexpr int kReviewImageW = 600;
constexpr int kReviewImageH = 394;
constexpr int kReviewFramePad = 14;
constexpr int kReviewFrameBottom = 40;
constexpr int kReviewFrameW = kReviewImageW + 2 * kReviewFramePad;
constexpr int kReviewFrameH = kReviewFramePad + kReviewImageH + kReviewFrameBottom;
constexpr int kSaveW = 360;
constexpr int kSaveH = 236;
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

// The framed review dimensions require CPU composition because SRM quantizes
// scale factors to 1/16 and cannot represent 600/720 exactly.
constexpr bool kReviewPpaEnabled = false;

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

bool IsPpaAligned(const void* address, uint32_t caps) {
    if (address == nullptr) return false;
    return reinterpret_cast<uintptr_t>(address) % PpaCacheAlignment(caps) == 0;
}

struct CameraDev {
    int fd = -1;
    uint8_t* buffer[kCameraBufferNum] = {};
    uint32_t buffer_size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pixel_format = 0;
    uint32_t bytes_per_pixel = 0;
    uint32_t crop_offset_x = 0;
    uint32_t crop_offset_y = 0;
};

inline uint16_t PackRgb565(int r, int g, int b) {
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    return static_cast<uint16_t>(((r & 0xF8) << 8) |
                                 ((g & 0xFC) << 3) | (b >> 3));
}

void Rgb565ToRgb565(const uint8_t* src, const CameraDev& cam, uint8_t* dst_bytes) {
    const uint16_t* src_base = reinterpret_cast<const uint16_t*>(src) +
                               cam.crop_offset_y * cam.width + cam.crop_offset_x;
    uint32_t* dst = reinterpret_cast<uint32_t*>(dst_bytes);
    const uint16_t* src_row = src_base +
                              (kCameraAreaH - 1) * cam.width +
                              (kCameraAreaW - 1);
    for (int y = 0; y < kCameraAreaH; ++y) {
        const uint16_t* sp = src_row;
        for (int x = 0; x < kCameraAreaW; x += 2) {
            const uint16_t first = *sp--;
            const uint16_t second = *sp--;
            *dst++ = static_cast<uint32_t>(first) |
                     (static_cast<uint32_t>(second) << 16);
        }
        src_row -= cam.width;
    }
}

void Yuv422ToRgb565(const uint8_t* src, const CameraDev& cam,
                    uint8_t* dst_bytes, bool u_first) {
    uint16_t* dst = reinterpret_cast<uint16_t*>(dst_bytes);
    const uint8_t* src_base = src +
                              cam.crop_offset_y * cam.width * 2 +
                              cam.crop_offset_x * 2;
    const int row_bytes = static_cast<int>(cam.width * 2);
    const uint8_t* src_row = src_base + (kCameraAreaH - 1) * row_bytes +
                             (kCameraAreaW - 2) * 2;
    for (int y = 0; y < kCameraAreaH; ++y) {
        const uint8_t* sp = src_row;
        for (int x = 0; x < kCameraAreaW; x += 2) {
            uint8_t y0, y1, u, v;
            if (u_first) {
                u = sp[0];
                y0 = sp[1];
                v = sp[2];
                y1 = sp[3];
            } else {
                y0 = sp[0];
                u = sp[1];
                y1 = sp[2];
                v = sp[3];
            }
            sp -= 4;
            const int c0 = static_cast<int>(y0) - 16;
            const int c1 = static_cast<int>(y1) - 16;
            const int d = static_cast<int>(u) - 128;
            const int e = static_cast<int>(v) - 128;
            const int r0 = (298 * c0 + 409 * e + 128) >> 8;
            const int g0 = (298 * c0 - 100 * d - 208 * e + 128) >> 8;
            const int b0 = (298 * c0 + 516 * d + 128) >> 8;
            const int r1 = (298 * c1 + 409 * e + 128) >> 8;
            const int g1 = (298 * c1 - 100 * d - 208 * e + 128) >> 8;
            const int b1 = (298 * c1 + 516 * d + 128) >> 8;
            *dst++ = PackRgb565(r1, g1, b1);
            *dst++ = PackRgb565(r0, g0, b0);
        }
        src_row -= row_bytes;
    }
}

void Rgb888ToRgb565(const uint8_t* src, const CameraDev& cam, uint8_t* dst_bytes) {
    uint16_t* dst = reinterpret_cast<uint16_t*>(dst_bytes);
    const uint8_t* src_base = src +
                              (cam.crop_offset_y * cam.width + cam.crop_offset_x) * 3;
    const uint8_t* src_row = src_base + (kCameraAreaH - 1) * cam.width * 3 +
                             (kCameraAreaW - 1) * 3;
    for (int y = 0; y < kCameraAreaH; ++y) {
        const uint8_t* sp = src_row;
        for (int x = 0; x < kCameraAreaW; ++x) {
            *dst++ = PackRgb565(sp[0], sp[1], sp[2]);
            sp -= 3;
        }
        src_row -= cam.width * 3;
    }
}

bool PpaRotateCropRgb565(const uint8_t* src, const CameraDev& cam,
                         uint8_t* dst, void*& handle,
                         uint32_t& hits, uint32_t& fallbacks,
                         bool& wide_source_logged) {
#if CONFIG_SOC_PPA_SUPPORTED
    if (!IsPpaAligned(dst, MALLOC_CAP_SPIRAM)) {
        ++fallbacks;
        return false;
    }
    if (cam.width > 1024) {
        if (!wide_source_logged) {
            ESP_LOGI(kTag,
                     "Camera source SRM skipped for %" PRIu32 "x%" PRIu32
                     " stride; use CPU crop/rotate (RGB565 accelerator remains active)",
                     cam.width, cam.height);
            wide_source_logged = true;
        }
        return false;
    }
    auto ppa_handle = reinterpret_cast<ppa_client_handle_t>(handle);
    if (ppa_handle == nullptr) {
        ppa_client_config_t client_config = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        if (ppa_register_client(&client_config, &ppa_handle) != ESP_OK) {
            ESP_LOGW(kTag, "camera PPA SRM register failed");
            return false;
        }
        handle = reinterpret_cast<void*>(ppa_handle);
    }
    ppa_srm_oper_config_t config = {};
    config.in.buffer = const_cast<uint8_t*>(src);
    config.in.pic_w = cam.width;
    config.in.pic_h = cam.height;
    config.in.block_w = kCameraAreaW;
    config.in.block_h = kCameraAreaH;
    config.in.block_offset_x = cam.crop_offset_x;
    config.in.block_offset_y = cam.crop_offset_y;
    config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    config.out.buffer = dst;
    config.out.buffer_size = static_cast<uint32_t>(PpaAlignedSize(
        MALLOC_CAP_SPIRAM,
        static_cast<size_t>(kCameraAreaW) * kCameraAreaH * 2));
    config.out.pic_w = kCameraAreaW;
    config.out.pic_h = kCameraAreaH;
    config.out.block_offset_x = 0;
    config.out.block_offset_y = 0;
    config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    config.rotation_angle = PPA_SRM_ROTATION_ANGLE_180;
    config.scale_x = 1.0f;
    config.scale_y = 1.0f;
    config.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t result = ppa_do_scale_rotate_mirror(ppa_handle, &config);
    if (result == ESP_OK) {
        ++hits;
        return true;
    }
    ++fallbacks;
#else
    (void)src;
    (void)cam;
    (void)dst;
    (void)handle;
    (void)hits;
    (void)fallbacks;
    (void)wide_source_logged;
#endif
    return false;
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

void ScaleReviewCpu(const uint8_t* source, uint8_t* destination) {
    const auto* src = reinterpret_cast<const uint16_t*>(source);
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
            dst[(y + kReviewFramePad) * kReviewFrameW + x + kReviewFramePad] =
                src[source_y * kCameraAreaW + source_x];
            x_acc += x_step;
        }
        y_acc += y_step;
    }
}

bool PpaScaleReviewRgb565(const uint8_t* source, uint8_t* destination,
                          void*& handle, uint32_t& hits,
                          uint32_t& fallbacks) {
#if CONFIG_SOC_PPA_SUPPORTED
    if (!IsPpaAligned(destination, MALLOC_CAP_SPIRAM)) {
        ++fallbacks;
        return false;
    }
    auto ppa_handle = reinterpret_cast<ppa_client_handle_t>(handle);
    if (ppa_handle == nullptr) {
        ppa_client_config_t client_config = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        if (ppa_register_client(&client_config, &ppa_handle) != ESP_OK) {
            ESP_LOGW(kTag, "review PPA SRM register failed");
            return false;
        }
        handle = reinterpret_cast<void*>(ppa_handle);
    }
    ppa_srm_oper_config_t config = {};
    config.in.buffer = const_cast<uint8_t*>(source);
    config.in.pic_w = kCameraAreaW;
    config.in.pic_h = kCameraAreaH;
    config.in.block_w = kCameraAreaW;
    config.in.block_h = kCameraAreaH;
    config.in.block_offset_x = 0;
    config.in.block_offset_y = 0;
    config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    // The output picture is the complete framed surface; the scaled camera
    // block lands at the 14px inset, leaving the 1px border and 40px footer.
    config.out.buffer = destination;
    config.out.buffer_size = static_cast<uint32_t>(PpaAlignedSize(
        MALLOC_CAP_SPIRAM,
        static_cast<size_t>(kReviewFrameW) * kReviewFrameH * 2));
    config.out.pic_w = kReviewFrameW;
    config.out.pic_h = kReviewFrameH;
    config.out.block_offset_x = kReviewFramePad;
    config.out.block_offset_y = kReviewFramePad;
    config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    config.scale_x = static_cast<float>(kReviewImageW) / kCameraAreaW;
    config.scale_y = static_cast<float>(kReviewImageH) / kCameraAreaH;
    config.mirror_x = false;
    config.mirror_y = false;
    config.mode = PPA_TRANS_MODE_BLOCKING;
    if (ppa_do_scale_rotate_mirror(ppa_handle, &config) == ESP_OK) {
        ++hits;
        return true;
    }
    ++fallbacks;
#else
    (void)source;
    (void)destination;
    (void)handle;
    (void)hits;
    (void)fallbacks;
#endif
    return false;
}

}  // namespace

struct CaptureBackend::Impl : std::enable_shared_from_this<CaptureBackend::Impl> {
    EventSink sink;
    mutable std::mutex sink_mutex;
    std::atomic<bool> running{false};
    std::atomic<bool> frozen{false};
    std::atomic<uint32_t> generation{0};
    std::atomic<int> front_index{0};
    std::atomic<int> pending_index{-1};
    std::atomic<bool> frame_pending{false};
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
    uint32_t ppa_fallbacks = 0;
    uint32_t ppa_review_hits = 0;
    uint32_t ppa_review_fallbacks = 0;
    bool wide_source_logged = false;
    uint8_t* preview_buffers[kPreviewBufferNum] = {};
    uint8_t* review_buffers[kReviewBufferNum] = {};
    std::shared_ptr<DecodedImage> review_images[kReviewBufferNum] = {};
    int review_slot = 0;

    void ReleasePpa() {
#if CONFIG_SOC_PPA_SUPPORTED
        if (ppa_handle != nullptr) {
            ppa_unregister_client(
                reinterpret_cast<ppa_client_handle_t>(ppa_handle));
            ppa_handle = nullptr;
        }
#endif
    }

    ~Impl() {
        running.store(false, std::memory_order_release);
        if (worker_slot != nullptr) {
            vSemaphoreDelete(worker_slot);
            worker_slot = nullptr;
        }
#if CONFIG_SOC_PPA_SUPPORTED
        ReleasePpa();
#endif
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
        const size_t size = PpaAlignedSize(
            MALLOC_CAP_SPIRAM,
            static_cast<size_t>(kCameraAreaW) * kCameraAreaH * 2);
        const size_t buffer_alignment = PpaCacheAlignment(MALLOC_CAP_SPIRAM);
        for (auto& buffer : preview_buffers) {
            if (buffer == nullptr) {
                buffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(
                    buffer_alignment, size,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (buffer == nullptr) {
                    ESP_LOGE(kTag, "alloc preview buffer failed (%u bytes)",
                             static_cast<unsigned>(size));
                    return false;
                }
                std::memset(buffer, 0, size);
            }
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
        review_pending.store(false, std::memory_order_release);
        review_source_index.store(-1, std::memory_order_release);
        review_generation.store(0, std::memory_order_release);
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

    esp_err_t ConfigureFormat(int fd) {
        v4l2_format format = {};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_G_FMT, &format) != 0) return ESP_FAIL;

        v4l2_format try_format = format;
        try_format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        if (ioctl(fd, VIDIOC_S_FMT, &try_format) == 0 &&
            try_format.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565) {
            format = try_format;
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
        camera.crop_offset_x = camera.width > kCameraAreaW
                                    ? (camera.width - kCameraAreaW) / 2
                                    : 0;
        camera.crop_offset_y = camera.height > kCameraAreaH
                                    ? (camera.height - kCameraAreaH) / 2
                                    : 0;
        return ESP_OK;
    }

    esp_err_t OpenDevice() {
        camera.fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
        if (camera.fd < 0) return ESP_FAIL;
        if (ConfigureFormat(camera.fd) != ESP_OK) {
            close(camera.fd);
            camera.fd = -1;
            return ESP_FAIL;
        }
        v4l2_requestbuffers request = {};
        request.count = kCameraBufferNum;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (ioctl(camera.fd, VIDIOC_REQBUFS, &request) != 0) {
            close(camera.fd);
            camera.fd = -1;
            return ESP_FAIL;
        }
        for (int i = 0; i < kCameraBufferNum; ++i) {
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
            camera.buffer_size = buffer.length;
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
        return ESP_OK;
    }

    void CloseDevice() {
        if (camera.fd < 0) return;
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(camera.fd, VIDIOC_STREAMOFF, &type);
        for (auto& buffer : camera.buffer) {
            if (buffer != nullptr) {
                munmap(buffer, camera.buffer_size);
                buffer = nullptr;
            }
        }
        close(camera.fd);
        camera.fd = -1;
        camera.buffer_size = 0;
    }

    std::shared_ptr<const PreviewFrame> FrameFor(int index, uint32_t value) const {
        if (index < 0 || index >= kPreviewBufferNum || preview_buffers[index] == nullptr) {
            return nullptr;
        }
        return std::make_shared<PreviewFrame>(PreviewFrame{
            .data = preview_buffers[index],
            .width = kCameraAreaW,
            .height = kCameraAreaH,
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
            preview_buffers[source_index] == nullptr) {
            return false;
        }
        const int slot = FindReviewSlot();
        if (slot < 0 || review_buffers[slot] == nullptr ||
            review_images[slot] == nullptr) {
            return false;
        }
        FillReviewSurface(review_buffers[slot]);
        if (kReviewPpaEnabled &&
            PpaScaleReviewRgb565(preview_buffers[source_index],
                                 review_buffers[slot], ppa_handle,
                                 ppa_review_hits, ppa_review_fallbacks)) {
            *output_slot = slot;
            return true;
        }
        // The exact 600x394 inset is composed on the capture worker so the
        // LVGL descriptor never exposes the accelerator's quantized width.
        ScaleReviewCpu(preview_buffers[source_index], review_buffers[slot]);
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
                if (OpenDevice() != ESP_OK) {
                    EmitStatus("Camera device open failed",
                               StatusCode::CameraStartupFailed);
                } else {
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
                        if (review_pending.exchange(false, std::memory_order_acq_rel)) {
                            const uint32_t request_generation =
                                review_generation.load(std::memory_order_acquire);
                            const int source_index =
                                review_source_index.load(std::memory_order_acquire);
                            if (running.load(std::memory_order_acquire) &&
                                generation.load(std::memory_order_acquire) ==
                                    worker_generation &&
                                request_generation == worker_generation &&
                                frozen.load(std::memory_order_acquire)) {
                                int output_slot = -1;
                                if (ComposeReview(source_index, &output_slot)) {
                                    Event event;
                                    event.type = EventType::ReviewReady;
                                    event.generation = request_generation;
                                    event.success = true;
                                    event.frame = FrameFor(source_index,
                                                           request_generation);
                                    event.review_image = review_images[output_slot];
                                    // A Stop/Unload can race composition.  The
                                    // generation check in Emit drops any late
                                    // result and the frozen front remains owned
                                    // by the caller for SaveReview.
                                    Emit(std::move(event));
                                } else {
                                    EmitReviewFailure(request_generation,
                                                      "review_compose_failed");
                                }
                            }
                        }
                        if (!frozen.load(std::memory_order_acquire) &&
                            !frame_pending.load(std::memory_order_acquire) &&
                            buffer.index < kCameraBufferNum) {
                            const int back = front_index.load(std::memory_order_acquire) ^ 1;
                            const uint8_t* source = camera.buffer[buffer.index];
                            uint8_t* destination = preview_buffers[back];
                            switch (camera.pixel_format) {
                                case V4L2_PIX_FMT_RGB565:
                                    if (!PpaRotateCropRgb565(source, camera, destination, ppa_handle,
                                                             ppa_hits, ppa_fallbacks,
                                                             wide_source_logged)) {
                                        Rgb565ToRgb565(source, camera, destination);
                                    }
                                    break;
                                case V4L2_PIX_FMT_YUYV:
                                    Yuv422ToRgb565(source, camera, destination, false);
                                    break;
                                case V4L2_PIX_FMT_UYVY:
                                    Yuv422ToRgb565(source, camera, destination, true);
                                    break;
                                case V4L2_PIX_FMT_RGB24:
                                    Rgb888ToRgb565(source, camera, destination);
                                    break;
                                default:
                                    break;
                            }
                            pending_index.store(back, std::memory_order_release);
                            frame_pending.store(true, std::memory_order_release);
                            Event event;
                            event.type = EventType::PreviewFrameReady;
                            event.generation = worker_generation;
                            event.frame = FrameFor(back, event.generation);
                            Emit(std::move(event));
                        }
                        ioctl(camera.fd, VIDIOC_QBUF, &buffer);
                        if (frozen.load(std::memory_order_acquire)) {
                            vTaskDelay(pdMS_TO_TICKS(30));
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
        if (!restart) ReleasePpa();
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
    impl_->pending_index.store(-1, std::memory_order_release);
    impl_->review_pending.store(false, std::memory_order_release);
    impl_->review_source_index.store(-1, std::memory_order_release);
    Event event;
    event.type = EventType::PreviewStopped;
    event.generation = impl_->generation.load(std::memory_order_acquire);
    impl_->Emit(std::move(event));
}

void CaptureBackend::Capture() {
    if (impl_ == nullptr || !impl_->running.load(std::memory_order_acquire)) return;
    const uint32_t generation = impl_->generation.load(std::memory_order_acquire);
    const int source_index = impl_->front_index.load(std::memory_order_acquire);
    if (impl_->task_handle.load(std::memory_order_acquire) == nullptr ||
        source_index < 0 || source_index >= kPreviewBufferNum ||
        impl_->preview_buffers[source_index] == nullptr) {
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
}

void CaptureBackend::AcknowledgeFrame(uint32_t generation, int buffer_index) {
    if (impl_ == nullptr || generation != impl_->generation.load(std::memory_order_acquire)) {
        return;
    }
    if (impl_->pending_index.load(std::memory_order_acquire) != buffer_index) return;
    impl_->front_index.store(buffer_index, std::memory_order_release);
    impl_->pending_index.store(-1, std::memory_order_release);
    impl_->frame_pending.store(false, std::memory_order_release);
}

std::shared_ptr<const PreviewFrame> CaptureBackend::CurrentFrame(uint32_t generation) const {
    if (impl_ == nullptr) return nullptr;
    return impl_->FrameFor(impl_->front_index.load(std::memory_order_acquire), generation);
}

std::shared_ptr<const PreviewFrame> CaptureBackend::CopyCurrentFrame(uint32_t generation) const {
    if (impl_ == nullptr || !impl_->PrepareBuffers()) return nullptr;
    const auto source = impl_->FrameFor(impl_->front_index.load(std::memory_order_acquire),
                                        generation);
    if (source == nullptr || source->data == nullptr) return nullptr;
    const size_t size = static_cast<size_t>(kCameraAreaW) * kCameraAreaH * 2;
    uint8_t* copy = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) return nullptr;
    std::memcpy(copy, source->data, size);
    auto owner = std::shared_ptr<uint8_t>(copy, [](uint8_t* value) {
        heap_caps_free(value);
    });
    return std::make_shared<PreviewFrame>(PreviewFrame{
        .data = owner.get(),
        .owned_data = std::move(owner),
        .width = source->width,
        .height = source->height,
        .buffer_index = -1,
        .generation = generation,
    });
}

}  // namespace agent_ui::camera
