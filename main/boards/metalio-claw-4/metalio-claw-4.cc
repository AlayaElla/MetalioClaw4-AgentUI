#include "application.h"
#include "button.h"
#include "config.h"
#include "display/lv_adapter_display.h"
#include "dual_network_board.h"

#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <wifi_station.h>
#include "esp_lcd_touch_gt911.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_fl7707n.h"
#include "esp_lcd_nv3051f.h"
#include "mipi_dsi_power_control.h"

// ========== LCD 屏幕选择 ==========
// 通过此宏在两款 720x720 MIPI-DSI 屏之间切换：
//   0 = NV3051F (36MHz DPI, RGB888, 24bpp)  ← 默认，量产屏
//   1 = FL7707N (48MHz DPI, RGB888, 16bpp)  ← 新备选屏
// 两种屏的初始化代码分别在成员函数：
//   InitializeNV3051FLCD()  /  InitializeFL7707NLCD()
// 下面通过预处理器把构造函数中调用的 InitializeLCD() 替换为对应的版本。
#ifndef METALIO_CLAW_4_USE_FL7707N
#define METALIO_CLAW_4_USE_FL7707N 0
#endif

// 周期系统监控仅用于临时性能诊断。默认关闭，避免串口每秒输出
// CPU / 内存 / 温度 / 电池日志，也避免常驻采样任务带来的额外开销。
// 需要诊断时可在编译选项中显式设置为 1。
#ifndef METALIO_CLAW_4_ENABLE_SYSTEM_MONITOR
#define METALIO_CLAW_4_ENABLE_SYSTEM_MONITOR 0
#endif

#if METALIO_CLAW_4_USE_FL7707N
#define InitializeLCD InitializeFL7707NLCD
#else
#define InitializeLCD InitializeNV3051FLCD
#endif

#include <cstring>
#include <iostream>
#include "IOExpander.hpp"
#include "display/agent_ui/core/power_key.h"
#include "SimpleUart.hpp"
// #include "power_manager.h"
// #include "power_save_timer.h"

#include "SdCardManager.hpp"
#include "usb_virtual_disk.h"
#include "bq27220_gauge.h"
#include "cx25601n.h"
#include "sc7a20_motion.h"
#include "bt_audio_codec.h"
#include "display/agent_ui/apps/bluetooth/bluetooth_module.h"

#include "driver/temperature_sensor.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2c_device.h"

#define TAG "METALIO_CLAW_4"

static std::string uartBuffer;

// NV3051F panel IO 的全局句柄，供功能界面（如相机界面）在摄像头驱动
// 对共享 GPIO 3 复位线发出脉冲后重放厂商 DCS 初始化序列。
// 在 InitializeLCD() 中赋值。
static esp_lcd_panel_io_handle_t s_metalio_claw_4_panel_io = NULL;

extern "C" esp_lcd_panel_io_handle_t metalio_claw_4_get_panel_io() { return s_metalio_claw_4_panel_io; }

// 板载 I2C 主总线（端口 1，GPIO 7/8）的全局句柄。
// 摄像头 SCCB 必须复用此句柄，而不是在同一物理引脚上再分配控制器，
// 否则两个 I2C 外设会抢总线，导致 GT911 / TCA9555 通信失败。
static i2c_master_bus_handle_t s_metalio_claw_4_i2c_bus = NULL;

extern "C" i2c_master_bus_handle_t metalio_claw_4_get_i2c_bus() { return s_metalio_claw_4_i2c_bus; }

class Wxcho : public I2cDevice {
private:
public:
    Wxcho(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
        : I2cDevice(i2c_bus, addr, I2C_BUS_SPEED_HZ) {
        ESP_LOGW(TAG, "Device found at address 0x60,init");
    }

    /*
     * 无线充电
     * MTP_ILIM_SET 寄存器地址: 0x1E
     * 位段: [2:0]
     * 功能: 过流保护限流值设置
     *
     * 电流与寄存器值(低3位)对照表:
     *   0x00 : 1.4A
     *   0x01 : 1.65A
     *   0x02 : 1.1A
     *   0x03 : 0.74A
     *   0x04 : 0.365A
     *   0x05 : 0.45A
     *   0x06 : 0.29A
     *   0x07 : 0.215A
     *
     * 写入时注意: 仅修改低3位 [2:0]，高5位保持不变
     */

    bool write0x1e() {
        // Conservative commercial default: preserve reserved bits and set
        // MTP_ILIM_SET to 0.74 A. Do not overwrite 0x15; its power-on value
        // keeps the hardware temperature protection enabled.
        uint8_t current = 0;
        esp_err_t err = TryReadReg(0x1e, &current);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "charge limit read failed: %s", esp_err_to_name(err));
            return false;
        }
        const uint8_t limited = static_cast<uint8_t>((current & 0xF8) | 0x03);
        err = TryWriteReg(0x1e, limited);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "charge limit write failed: %s", esp_err_to_name(err));
            return false;
        }
        ESP_LOGW(TAG, "charge limit: 0x%02X -> 0x%02X (0.74A)", current,
                 limited);
        return true;
    }

    void read0x1e() {
        const uint8_t value = ReadReg(0x1e);
        ESP_LOGW(TAG, "read 0X1E reg: 0x%02X", value);
    }

    ~Wxcho() {}
};

class METALIO_CLAW_4 : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;

    Display* display_;

    esp_lcd_touch_handle_t touch_handle = NULL;
    esp_lcd_panel_io_handle_t panel_io_handle = NULL;
    esp_lcd_panel_handle_t panel_handle = NULL;

    Wxcho* wxcho;

    esp_err_t err;
    bool init0x60 = false;
    bool c_is_found_0x60 = false;
    bool l_is_found_0x60 = false;
    bool charge_limit_configured_ = false;

    void gpio_output_init(gpio_num_t gpio_num, uint8_t initial_level) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << gpio_num),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(gpio_num, initial_level);
    }

    // 屏幕初始化前对 RST 引脚做一次硬件复位，时序与 esp_lcd_panel_reset 一致。
    void ResetLcdBeforeInit() {
        gpio_output_init(PIN_NUM_LCD_RST, 1);
        gpio_set_level(PIN_NUM_LCD_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(PIN_NUM_LCD_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
        ESP_LOGI(TAG, "LCD hardware reset done (GPIO %d)", PIN_NUM_LCD_RST);
    }

    void InitializeI2C() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = I2C_SDA_PIN,
            .scl_io_num = I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
        s_metalio_claw_4_i2c_bus = i2c_bus_;
    }

    void InitializeCharger() {
        constexpr int kProbeAttempts = 5;
        constexpr int kProbeDelayMs = 100;
        esp_err_t probe_error = ESP_FAIL;
        for (int attempt = 1; attempt <= kProbeAttempts; ++attempt) {
            probe_error = i2c_master_probe(i2c_bus_, CX25601N_I2C_ADDR, 100);
            if (probe_error == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "CX25601N probe attempt %d/%d failed: %s", attempt,
                     kProbeAttempts, esp_err_to_name(probe_error));
            vTaskDelay(pdMS_TO_TICKS(kProbeDelayMs));
        }
        if (probe_error != ESP_OK) {
            ESP_LOGW(TAG, "CX25601N charger not found at I2C address 0x%02X",
                     CX25601N_I2C_ADDR);
            return;
        }
        const esp_err_t init_error = cx25601n_init(i2c_bus_);
        if (init_error != ESP_OK) {
            ESP_LOGE(TAG, "CX25601N init failed: %s", esp_err_to_name(init_error));
            return;
        }

        // Charging current is intentionally fixed; the Settings screen no
        // longer exposes a profile selector, so stale NVS values must not
        // continue to change hardware behavior after an upgrade.
        constexpr uint32_t kDefaultChargeMa = 1000;
        const esp_err_t apply_error = cx25601n_set_ichg_ma(kDefaultChargeMa);
        if (apply_error == ESP_OK) {
            ESP_LOGI(TAG, "Charge current initialized to fixed default: %u mA",
                     kDefaultChargeMa);
        } else {
            ESP_LOGE(TAG, "Failed to initialize charge current: %s",
                     esp_err_to_name(apply_error));
        }

        // Initialization programs the limits, but no later UI owner guarantees
        // that EN_CHG is asserted.  Explicitly enable charging here; this is
        // essential when USB is attached after the battery has entered deep
        // discharge.
        const esp_err_t enable_error = cx25601n_enable_charge(true);
        if (enable_error == ESP_OK) {
            ESP_LOGI(TAG, "Battery charging enabled");
        } else {
            ESP_LOGE(TAG, "Failed to enable battery charging: %s",
                     esp_err_to_name(enable_error));
        }
    }

    bool InitializeIOExpander() {
        // gpio_output_init(GPIO_NUM_22, 1);

        auto& iOExpander = IOExpander::getInstance();
        constexpr int kInitAttempts = 8;
        constexpr int kInitDelayMs = 250;
        esp_err_t init_error = ESP_FAIL;
        for (int attempt = 1; attempt <= kInitAttempts; ++attempt) {
            init_error = iOExpander.begin(i2c_bus_);
            if (init_error == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "TCA9555 init attempt %d/%d failed: %s", attempt,
                     kInitAttempts, esp_err_to_name(init_error));
            vTaskDelay(pdMS_TO_TICKS(kInitDelayMs));
        }
        if (init_error != ESP_OK) {
            ESP_LOGE(TAG,
                     "TCA9555 unavailable after %d attempts; continue in reduced mode",
                     kInitAttempts);
            return false;
        }
        iOExpander.setLevel(IOExpander::Pin::BT_POWER, true);
        // PA is owned by audio_output_route and only turns on while the local
        // speaker is selected and codec output is active.
        iOExpander.setLevel(IOExpander::Pin::PA, false);
        iOExpander.setLevel(IOExpander::Pin::PA_SWITCH, true);
        iOExpander.setLevel(IOExpander::Pin::RST_4G, true);
        // CAM_PWDN: 低电平通电；这里默认拉高 = 摄像头断电。
        // 只有进入相机 App 时（CameraScreen::LifecycleCallback LOAD）才拉低供电。
        iOExpander.setLevel(IOExpander::Pin::CAM_PWDN, true);
        iOExpander.setLevel(IOExpander::Pin::SD, false);

        return true;
    }

    void InitializeBTAudio() {
        SimpleUart& uart = SimpleUart::getInstance();
        if (uart.begin(BT_AUDIO_TX_PIN, BT_AUDIO_RX_PIN, 115200, UART_NUM_2)) {
            ESP_LOGI(TAG, "UART initialized successfully!");
        } else {
            ESP_LOGI(TAG, "UART initialization failed!");
            return;
        }
        // Restore the user-facing local/Bluetooth speaker choice.
        agent_ui::bluetooth::Module::InitializeHardware();
    }

    static esp_err_t bsp_enable_dsi_phy_power(void) {
#if MIPI_DSI_PHY_PWR_LDO_CHAN > 0
        // 为 MIPI DSI PHY 上电，使其从「无电源」状态进入「关闭」状态
        static esp_ldo_channel_handle_t phy_pwr_chan = NULL;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
            .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
        };
        esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan);
        ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
#endif  // 当 MIPI_DSI_PHY_PWR_LDO_CHAN > 0 时

        return ESP_OK;
    }

    // 开机就把 SD 卡挂到 /sdcard。失败不致命（卡没插 / 没格式化都会失败），
    // Files App 通过 SdCardManager::IsMounted() 判断状态。
    void InitializeSdCard() {
        if (!SdCardManager::GetInstance().Mount()) {
            ESP_LOGW(TAG, "SD card not mounted at boot (card may be absent)");
        }
        // 虚拟 U 盘 worker：默认保持 USB Serial/JTAG，启用时再切 MSC。
        UsbVirtualDisk::GetInstance().Init();
    }

    // ---------- NV3051F (TRULY HE396-040T2BZZ, 36MHz DPI, RGB888) ----------
    // 量产屏初始化。构造函数中调用 InitializeLCD()，预处理器在
    // METALIO_CLAW_4_USE_FL7707N == 0 时把它替换为本函数。
    void InitializeNV3051FLCD() {
        bsp_enable_dsi_phy_power();

        esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
        esp_lcd_dsi_bus_config_t bus_config = {
            .bus_id = 0,
            .num_data_lanes = 2,
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = 1000,
        };
        ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

        ESP_LOGI(TAG, "Install MIPI DSI LCD control panel (NV3051F)");
        // 使用 DBI 接口发送 LCD 命令和参数
        esp_lcd_dbi_io_config_t dbi_config = NV3051F_PANEL_IO_DBI_CONFIG();
        ESP_ERROR_CHECK(
            esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &panel_io_handle));

        esp_lcd_dpi_panel_config_t dpi_config = {};
        // 1. 时钟源配置
        dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
        dpi_config.dpi_clock_freq_mhz = 36;  // NV3051F_DCLK_MHZ

        // 2. 虚拟通道
        dpi_config.virtual_channel = 0;

        // 3. 像素格式

        // 4. 帧缓冲数量
        // LVGL adapter 跑 TEAR_AVOID_MODE_TRIPLE_FULL，需要 3 张 panel FB
        // 当 LVGL 的 draw buffer，避免回退到 partial 模式抢内部 SRAM。
        dpi_config.num_fbs = 3;

        // 5. 视频时序参数 (TRULY HE396-040T2BZZ + NV3051F, 20250708 datasheet)
        dpi_config.video_timing.h_size = 720;            // NV3051F_LCD_H_RES (HDP)
        dpi_config.video_timing.v_size = 720;            // NV3051F_LCD_V_RES (VDP)
        dpi_config.video_timing.hsync_back_porch = 44;   // NV3051F_HBP (HBPD)
        dpi_config.video_timing.hsync_pulse_width = 2;   // NV3051F_HSW (HSPW)
        dpi_config.video_timing.hsync_front_porch = 46;  // NV3051F_HFP (HFPD)
        dpi_config.video_timing.vsync_back_porch = 14;   // NV3051F_VBP (VBPD)
        dpi_config.video_timing.vsync_pulse_width = 2;   // NV3051F_VSW (VSPW)
        dpi_config.video_timing.vsync_front_porch = 16;  // NV3051F_VFP (VFPD)

        // 6. 颜色格式
        dpi_config.in_color_format = LCD_COLOR_FMT_RGB888;
        dpi_config.out_color_format = LCD_COLOR_FMT_RGB888;

        // 7. 功能标志

        nv3051f_vendor_config_t vendor_config = {
            .mipi_config =
                {
                    .dsi_bus = mipi_dsi_bus,
                    .dpi_config = &dpi_config,
                },
        };

        const esp_lcd_panel_dev_config_t lcd_dev_config = {
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 24,
            .reset_gpio_num = PIN_NUM_LCD_RST,
            .vendor_config = &vendor_config,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_nv3051f(panel_io_handle, &lcd_dev_config, &panel_handle));
        ESP_ERROR_CHECK(esp_lcd_dpi_panel_enable_dma2d(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
        // ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

        // 暴露 panel IO 句柄，供其他组件（相机界面）在 GPIO 3 摄像头
        // 复位脉冲后重放厂商 DCS 初始化序列。
        s_metalio_claw_4_panel_io = panel_io_handle;
    }

    // ---------- FL7707N (48MHz DPI, RGB888) ----------
    // 备选屏初始化。参数源自厂商 example (esp32-p4-fl7707n-gt911)。
    // 构造函数中调用 InitializeLCD()，预处理器在
    // METALIO_CLAW_4_USE_FL7707N == 1 时把它替换为本函数。
    void InitializeFL7707NLCD() {
        bsp_enable_dsi_phy_power();

        esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
        esp_lcd_dsi_bus_config_t bus_config = {
            .bus_id = 0,
            .num_data_lanes = 2,
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = 1000,
        };
        ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

        ESP_LOGI(TAG, "Install MIPI DSI LCD control panel (FL7707N)");
        // 使用 DBI 接口发送 LCD 命令和参数
        esp_lcd_dbi_io_config_t dbi_config = FL7707N_PANEL_IO_DBI_CONFIG();
        ESP_ERROR_CHECK(
            esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &panel_io_handle));

        esp_lcd_dpi_panel_config_t dpi_config = {};
        // 1. 时钟源配置
        dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
        dpi_config.dpi_clock_freq_mhz = 48;  // FL7707N_DCLK_MHZ

        // 2. 虚拟通道
        dpi_config.virtual_channel = 0;

        // 3. 像素格式 (RGB888, 16bpp)

        // 4. 帧缓冲数量
        // LVGL adapter 跑 TEAR_AVOID_MODE_TRIPLE_FULL，需要 3 张 panel FB。
        // 厂商 example 用 2 张，但本工程的 LVGL 通路必须 3 张，否则会回退到
        // partial 模式抢内部 SRAM，导致初始化失败。
        dpi_config.num_fbs = 3;

        // 5. 视频时序参数 (FL7707N 厂商 example)
        dpi_config.video_timing.h_size = 720;             // FL7707N_LCD_H_RES
        dpi_config.video_timing.v_size = 720;             // FL7707N_LCD_V_RES
        dpi_config.video_timing.hsync_back_porch = 120;   // FL7707N_HBP
        dpi_config.video_timing.hsync_pulse_width = 60;   // FL7707N_HSW
        dpi_config.video_timing.hsync_front_porch = 106;  // FL7707N_HFP
        dpi_config.video_timing.vsync_back_porch = 20;    // FL7707N_VBP
        dpi_config.video_timing.vsync_pulse_width = 4;    // FL7707N_VSW
        dpi_config.video_timing.vsync_front_porch = 20;   // FL7707N_VFP

        // 6. 颜色格式
        dpi_config.in_color_format = LCD_COLOR_FMT_RGB888;
        dpi_config.out_color_format = LCD_COLOR_FMT_RGB888;

        // 7. 功能标志

        fl7707n_vendor_config_t vendor_config = {
            .mipi_config =
                {
                    .dsi_bus = mipi_dsi_bus,
                    .dpi_config = &dpi_config,
                },
        };

        const esp_lcd_panel_dev_config_t lcd_dev_config = {
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
            .reset_gpio_num = PIN_NUM_LCD_RST,
            .vendor_config = &vendor_config,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_fl7707n(panel_io_handle, &lcd_dev_config, &panel_handle));
        ESP_ERROR_CHECK(esp_lcd_dpi_panel_enable_dma2d(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
        // ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

        // 暴露 panel IO 句柄，供其他组件（相机界面）在 GPIO 3 摄像头
        // 复位脉冲后重放厂商 DCS 初始化序列。
        // 注意：camera_screen 当前调用的是 esp_lcd_nv3051f_replay_vendor_init，
        // replay 函数并在 camera_screen 里按宏分发。
        s_metalio_claw_4_panel_io = panel_io_handle;
    }

    void InitializeDisplay() {
        display_ = new LVAdapterDisplay(panel_handle, panel_io_handle, touch_handle, DISPLAY_WIDTH,
                                        DISPLAY_HEIGHT);
    }

    uint8_t ProbeGT911I2CAddress() {
        const uint8_t addrs[] = {
            ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
            ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP,
        };
        for (uint8_t addr : addrs) {
            if (i2c_master_probe(i2c_bus_, addr, 100) == ESP_OK) {
                ESP_LOGI(TAG, "GT911 found at I2C address 0x%02X", addr);
                return addr;
            }
        }
        ESP_LOGW(TAG, "GT911 I2C probe failed, fallback to default 0x%02X",
                 ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS);
        return ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    }

    void InitializeTouch() {
        if (touch_handle != NULL) {
            return;
        }

        const uint8_t dev_addr = ProbeGT911I2CAddress();

        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {};
        tp_io_config.dev_addr = dev_addr;
        tp_io_config.scl_speed_hz = I2C_BUS_SPEED_HZ;
        tp_io_config.control_phase_bytes = 1;
        tp_io_config.dc_bit_offset = 0;
        tp_io_config.lcd_cmd_bits = 16;
        tp_io_config.flags.disable_control_phase = 1;

        esp_err_t err = esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Touch panel IO create failed: 0x%x", err);
            return;
        }

        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
            .levels =
                {
                    .reset = 0,
                    .interrupt = 0,
                },
            .flags =
                {
                    .swap_xy = 0,
                    .mirror_x = 0,
                    .mirror_y = 0,
                },
        };

        ESP_LOGI(TAG, "Initialize GT911 touch controller");
        err = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &touch_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GT911 init failed: 0x%x", err);
            esp_lcd_panel_io_del(tp_io_handle);
            touch_handle = NULL;
            return;
        }
    }
    static void I2cWxchoTask(void* arg) {
        METALIO_CLAW_4* board = static_cast<METALIO_CLAW_4*>(arg);
        if (board == nullptr) {
            ESP_LOGE(TAG, "I2cWxchoTask: invalid board pointer");
            vTaskDelete(NULL);
            return;
        }

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(board->c_is_found_0x60 ? 1000 : 2000));
            board->err = i2c_master_probe(board->i2c_bus_, 0x60, 100);
            if (board->err == ESP_OK) {
                board->c_is_found_0x60 = true;
                if (!board->init0x60) {
                    board->init0x60 = true;
                    board->wxcho = new Wxcho(board->i2c_bus_, 0x60);
                }

                if (!board->charge_limit_configured_) {
                    board->charge_limit_configured_ = board->wxcho->write0x1e();
                }
            } else {
                board->c_is_found_0x60 = false;
                board->charge_limit_configured_ = false;
            }
            board->l_is_found_0x60 = board->c_is_found_0x60;
        }
    }

    void InitializeI2cWxcho() {
        BaseType_t ret =
            xTaskCreatePinnedToCore(I2cWxchoTask, "i2c_wxcho_task", 4 * 1024, this, 1, NULL, 0);

        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create I2cWxchoTask");
        } else {
            ESP_LOGI(TAG, "I2cWxchoTask created successfully");
        }
    }

    // 开机电量保护：读到 0% 且未在充电时，发 PWR_KEY_PULSE 序列强制关机。
    void CheckBatteryLevelAtBoot() {
        auto& gauge = Bq27220Gauge::GetInstance();
        int level = 0;
        bool charging = false;
        bool discharging = false;
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (gauge.GetBatteryLevel(level, charging, discharging)) {
                ESP_LOGI(TAG, "Boot battery check: level=%d%%, charging=%s", level,
                         charging ? "true" : "false");
                if (level == 0 && !charging) {
                    ESP_LOGW(TAG, "Battery 0%%, forcing power off");
                    auto& io = IOExpander::getInstance();
                    constexpr int kPulseHalfMs = 100;
                    constexpr int kPulseCount = 10;
                    for (int i = 0; i < kPulseCount; ++i) {
                        io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, true);
                        vTaskDelay(pdMS_TO_TICKS(kPulseHalfMs));
                        io.setLevel(IOExpander::Pin::PWR_KEY_PULSE, false);
                        vTaskDelay(pdMS_TO_TICKS(kPulseHalfMs));
                    }
                    while (true) {
                        vTaskDelay(portMAX_DELAY);
                    }
                }
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        ESP_LOGW(TAG, "Boot battery check: gauge unavailable, skip shutdown");
    }

    void InitializeNoLCD() { display_ = new NoDisplay(); }

public:
    METALIO_CLAW_4() : DualNetworkBoard(NT26_TX_PIN, NT26_RX_PIN, NT26_MRDY_PIN, NT26_SRDY_PIN, 1) {
        InitializeI2C();

        // Bring the charger up before optional peripherals.  A deeply
        // discharged battery can leave the TCA9555 rail temporarily unable to
        // acknowledge I2C; charging must still start instead of entering an
        // ESP_ERROR_CHECK reboot loop.
        InitializeCharger();
        const bool io_expander_ready = InitializeIOExpander();
        // 把 BQ27220 电量计绑定到 i2c_bus_ 上；返回值表示「这次是否挂上」，
        // 开机失败 Bq27220Gauge::GetBatteryLevel 内部会节流自愈，这里不需要
        // ESP_ERROR_CHECK。
        (void)Bq27220Gauge::GetInstance().Begin(i2c_bus_);
        if (io_expander_ready) {
            CheckBatteryLevelAtBoot();
            InitializeBTAudio();
        } else {
            ESP_LOGW(TAG,
                     "Skipping IO-expander-dependent battery shutdown and Bluetooth init");
        }
        // SD 卡的 LDO（chan 4）在 InitializeSDWIFIPower() 里已经打开，这里
        // 直接挂载，进入 Files App 时可立即读取，不需要再次 mount。
        InitializeSdCard();
        // InitializeNoLCD();
        /* 顺序：LCD 上电稳定后再初始化 GT911，最后构造 LVGL 显示（触摸已就绪） */
        ResetLcdBeforeInit();
        InitializeLCD();
        ESP_ERROR_CHECK(metalio_mipi_dsi_power_init());
        vTaskDelay(pdMS_TO_TICKS(100));
        InitializeTouch();
        InitializeDisplay();
        // SC7A20 sampling starts only after LVGL exists, so a detected shake
        // can safely schedule the dizzy expression on the application loop.
        (void)Sc7a20MotionService::GetInstance().Start(i2c_bus_);
        // 电源键回调会调用 LVGL。冷启动时用户可能仍按着开机键，因此必须
        // 等 LVGL 初始化完成后再开始检测松手事件。
        if (io_expander_ready) {
            agent_ui::PowerKey::Initialize();
        }
        InitializeI2cWxcho();
        GetBacklight()->RestoreBrightness();

#if METALIO_CLAW_4_ENABLE_SYSTEM_MONITOR
        xTaskCreate(
            [](void* pvParameters) {
                (void)pvParameters;  // 单例已经在外部 Begin 过，task 不再需要 board 指针
                auto& gauge = Bq27220Gauge::GetInstance();

                // ---- ESP32-P4 双核 CPU 占用率采样 ----
                // 依赖 sdkconfig（已在 sdkconfig.defaults / .esp32p4 开启）：
                //   CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
                //   CONFIG_FREERTOS_USE_TRACE_FACILITY=y
                //   CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y
                // 思路：每个核都有一个 Idle Task，它只在没别的 task 跑的时候才
                // 会被调度。ESP-IDF 提供 ulTaskGetIdleRunTimeCounterForCore()
                // 直接拿到该核的 IDLE 累计运行时间（单位 us，因为
                // RUN_TIME_STATS_USING_ESP_TIMER 已开）。两次采样做差即得这一
                // 秒内核空闲微秒数：
                //   usage% = 100 - idle_delta_us * 100 / total_delta_us
                constexpr int kCoreCount = portNUM_PROCESSORS;
                configRUN_TIME_COUNTER_TYPE prev_idle[kCoreCount] = {0};
                for (int c = 0; c < kCoreCount; ++c) {
                    prev_idle[c] = ulTaskGetIdleRunTimeCounterForCore(c);
                }
                uint64_t prev_us = (uint64_t)esp_timer_get_time();

                // ---- ESP32-P4 内置温度传感器 ----
                temperature_sensor_handle_t temp_handle = NULL;
                const temperature_sensor_config_t temp_sensor_config =
                    TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 80);
                const bool temp_sensor_ok =
                    (temperature_sensor_install(&temp_sensor_config, &temp_handle) == ESP_OK &&
                     temperature_sensor_enable(temp_handle) == ESP_OK);
                if (!temp_sensor_ok) {
                    ESP_LOGW(TAG, "Temperature sensor init failed");
                }

                while (1) {
                    vTaskDelay(pdMS_TO_TICKS(1000));

                    // ---- CPU 占用率 ----
                    uint64_t now_us = (uint64_t)esp_timer_get_time();
                    uint64_t dt_us = now_us - prev_us;
                    int usage[kCoreCount] = {0};
                    int total_usage = 0;
                    if (dt_us > 0) {
                        for (int c = 0; c < kCoreCount; ++c) {
                            configRUN_TIME_COUNTER_TYPE now_idle =
                                ulTaskGetIdleRunTimeCounterForCore(c);
                            configRUN_TIME_COUNTER_TYPE didle = now_idle - prev_idle[c];
                            uint64_t idle_pct = (uint64_t)didle * 100ULL / dt_us;
                            if (idle_pct > 100)
                                idle_pct = 100;
                            usage[c] = 100 - (int)idle_pct;
                            total_usage += usage[c];
                            prev_idle[c] = now_idle;
                        }
                    }
                    prev_us = now_us;
                    const int avg_usage = (kCoreCount > 0) ? (total_usage / kCoreCount) : 0;
                    const int core1_usage = (kCoreCount > 1) ? usage[1] : 0;

                    constexpr const char* kMonitorTag = "系统监控";
                    ESP_LOGI(kMonitorTag,
                             "@@@CPU   | 内核0: %3d%% | 内核1: %3d%% | 平均: %3d%%",
                             usage[0], core1_usage, avg_usage);

                    const unsigned free_kb = static_cast<unsigned>(
                        heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
                    const unsigned min_free_kb = static_cast<unsigned>(
                        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
                    ESP_LOGI(kMonitorTag,
                             "@@@内存  | 剩余: %6u KB | 历史最小: %6u KB",
                             free_kb, min_free_kb);

                    if (temp_sensor_ok) {
                        float chip_temp_c = 0.0f;
                        if (temperature_sensor_get_celsius(temp_handle, &chip_temp_c) == ESP_OK) {
                            ESP_LOGI(kMonitorTag, "@@@温度  | 芯片: %5.1f °C", chip_temp_c);
                        } else {
                            ESP_LOGW(kMonitorTag, "@@@温度  | 芯片: 读取失败");
                        }
                    }

                    // ---- 电池电量 ----
                    int battery_level;
                    bool charging, discharging;
                    if (gauge.GetBatteryLevel(battery_level, charging, discharging)) {
                        uint16_t mv = 0;
                        const bool mv_ok = gauge.GetVoltageMv(mv);
                        if (mv_ok) {
                            ESP_LOGI(kMonitorTag,
                                     "@@@电池  | 电量: %3d%% | 电压: %5u mV | "
                                     "充电: %s | 放电: %s",
                                     battery_level, mv,
                                     charging ? "是" : "否",
                                     discharging ? "是" : "否");
                        } else {
                            ESP_LOGI(kMonitorTag,
                                     "@@@电池  | 电量: %3d%% | 电压: 读取失败 | "
                                     "充电: %s | 放电: %s",
                                     battery_level,
                                     charging ? "是" : "否",
                                     discharging ? "是" : "否");
                        }
                    }
                    // ---- 板子信息 JSON（OTA / 协议握手用的实时快照） ----
                    // 仅在网络真正连上后才打印，避免还没连通时刷一堆
                    // 半成品 JSON（WiFi 没拿到 IP / 4G 没注册的字段都是空）。

                    // WiFi  -> WifiStation::GetInstance().IsConnected()
                    //          (esp-wifi-connect 内部跟踪 IP_EVENT_STA_GOT_IP)
                    // 4G    -> Nt26Board::GetRegistrationState().stat
                    //          (AT+CEREG 上报的注册状态：1=本网、5=漫游为已注册)

                    // GetCurrentBoard() 在 ML307 模式下返回的就是 Nt26Board
                    // （metalio-claw-4 的 DualNetworkBoard 这一路只接 NT26），
                    // 所以 static_cast 是安全的；WiFi 模式根本不会走到这个分支。
                    {
                        // auto& dual = static_cast<DualNetworkBoard&>(Board::GetInstance());
                        // const NetworkType net_type = dual.GetNetworkType();
                        // bool connected = false;
                        // const char* net_name = "?";
                        // if (net_type == NetworkType::WIFI) {
                        //     net_name = "WiFi";
                        //     connected = WifiStation::GetInstance().IsConnected();
                        // } else {
                        //     net_name = "4G";
                        //     auto& nt26 = static_cast<Nt26Board&>(dual.GetCurrentBoard());
                        //     const int stat = nt26.GetRegistrationState().stat;
                        //     connected = (stat == 1 || stat == 5);
                        // }
                        // if (connected) {
                        //     std::string board_json = dual.GetBoardJson();
                        //     ESP_LOGI(TAG, "@@@[%s] BoardJson: %s", net_name, board_json.c_str());
                        // }
                    }
                    // if (WifiStation::GetInstance().IsConnected()) {
                    //     ESP_LOGI(TAG, "@@@rssi:%d", wifi_station.GetRssi());
                    // }
                }
            },
            "system_monitor", 8192, this, 5, NULL);
#endif
    }

    void SetLowPowerStandby(bool enabled) override {
        Sc7a20MotionService::GetInstance().SetSuspended(enabled);
    }

    void SetPerformanceMaxMhz(int max_freq_mhz) override {
        const esp_err_t error =
            metalio_mipi_dsi_power_set_frequency(max_freq_mhz);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Failed to switch CPU frequency tier: %s",
                     esp_err_to_name(error));
        }
    }

    int GetActiveDisplayMinMhz() const override {
        // The production RGB888 MIPI-DPI stream underruns below this floor.
        return 360;
    }

    int GetScreenOffMinMhz() const override {
        // Panel sleep leaves RGB888 GDMA reading the framebuffer from PSRAM.
        // Hardware testing underruns at 180 MHz, so the continuous DSI stream
        // must retain the same 360 MHz floor used while the panel is visible.
        return 360;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BTAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                              AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_MIC_GPIO_WS,
                                              AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    // 电量来自 BQ27220；是否连接外部电源来自充电芯片的 VBUS 状态。不能用
    // BQ27220 的瞬时电流当成插拔状态：接近满电时 top-off 会周期性启停，
    // 从而制造假的 charging false -> true 边沿。
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (!Bq27220Gauge::GetInstance().GetBatteryLevel(level, charging, discharging)) {
            return false;
        }

        uint8_t vbus_stat = 0;
        if (cx25601n_get_vbus_stat(&vbus_stat) == ESP_OK) {
            // 0 = no input; 7 = OTG output. Values 1..6 represent an
            // externally powered USB/adapter source.
            const bool input_present = vbus_stat != 0 && vbus_stat != 7;
            if (!charger_input_initialized_) {
                charger_input_present_ = input_present;
                charger_input_initialized_ = true;
                charger_input_candidate_samples_ = 0;
                ESP_LOGI(TAG, "External power: present=%d vbus_stat=%u",
                         input_present, static_cast<unsigned>(vbus_stat));
            } else if (input_present == charger_input_present_) {
                charger_input_candidate_samples_ = 0;
            } else {
                // VBUS status can briefly drop while the charger IC is
                // servicing I2C or switching between CC/CV/top-off. Require
                // a longer absence confirmation so one physical session
                // cannot generate repeated charging edges.
                ++charger_input_candidate_samples_;
                const uint8_t required_samples = input_present ? 2 : 8;
                if (charger_input_candidate_samples_ >= required_samples) {
                    charger_input_present_ = input_present;
                    charger_input_candidate_samples_ = 0;
                    ESP_LOGI(TAG, "External power: present=%d vbus_stat=%u",
                             input_present, static_cast<unsigned>(vbus_stat));
                }
            }
        }
        if (charger_input_initialized_) {
            charging = charger_input_present_;
        }
        return true;
    }

private:
    bool charger_input_initialized_ = false;
    bool charger_input_present_ = false;
    uint8_t charger_input_candidate_samples_ = 0;

    // virtual void SetPowerSaveMode(bool enabled) override {
    //     if (!enabled) {
    //         power_save_timer_->WakeUp();
    //     }
    //     DualNetworkBoard::SetPowerSaveMode(enabled);
    // }
};

DECLARE_BOARD(METALIO_CLAW_4);
