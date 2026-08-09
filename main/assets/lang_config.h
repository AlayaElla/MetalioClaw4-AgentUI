// Auto-generated language config
// Language: zh-CN with en-US fallback
#pragma once

#include <string_view>

#ifndef zh_cn
    #define zh_cn  // 預設語言
#endif

namespace Lang {
    // 语言元数据
    constexpr const char* CODE = "zh-CN";

    // 字符串资源 (en-US as fallback for missing keys)
    namespace Strings {
        constexpr const char* ACCESS_VIA_BROWSER = "，浏览器访问 ";
        constexpr const char* ACTIVATION = "激活设备";
        constexpr const char* BATTERY_CHARGING = "正在充电";
        constexpr const char* BATTERY_FULL = "电量已满";
        constexpr const char* BATTERY_LOW = "电量不足";
        constexpr const char* BATTERY_NEED_CHARGE = "电量低，请充电";
        constexpr const char* CHECKING_NEW_VERSION = "检查新版本...";
        constexpr const char* CHECK_NEW_VERSION_FAILED = "检查新版本失败，将在 %d 秒后重试：%s";
        constexpr const char* CONNECTED_TO = "已连接 ";
        constexpr const char* CONNECTING = "连接中...";
        constexpr const char* CONNECTION_SUCCESSFUL = "Connection Successful";
        constexpr const char* CONNECT_TO = "连接 ";
        constexpr const char* CONNECT_TO_HOTSPOT = "手机连接热点 ";
        constexpr const char* DETECTING_MODULE = "检测模组...";
        constexpr const char* DOWNLOAD_ASSETS_FAILED = "下载资源失败";
        constexpr const char* ENTERING_WIFI_CONFIG_MODE = "进入配网模式...";
        constexpr const char* ERROR = "错误";
        constexpr const char* FOUND_NEW_ASSETS = "发现新资源: %s";
        constexpr const char* HELLO_MY_FRIEND = "你好，我的朋友！";
        constexpr const char* INFO = "信息";
        constexpr const char* INITIALIZING = "正在初始化...";
        constexpr const char* LISTENING = "聆听中...";
        constexpr const char* LOADING_ASSETS = "加载资源...";
        constexpr const char* LOADING_PROTOCOL = "登录服务器...";
        constexpr const char* MAX_VOLUME = "最大音量";
        constexpr const char* MUTED = "已静音";
        constexpr const char* NEW_VERSION = "新版本 ";
        constexpr const char* OTA_UPGRADE = "OTA 升级";
        constexpr const char* PIN_ERROR = "请插入 SIM 卡";
        constexpr const char* PLEASE_WAIT = "请稍候...";
        constexpr const char* REGISTERING_NETWORK = "等待网络...";
        constexpr const char* REG_ERROR = "无法接入网络，请检查流量卡状态";
        constexpr const char* RTC_MODE_OFF = "AEC 关闭";
        constexpr const char* RTC_MODE_ON = "AEC 开启";
        constexpr const char* SCANNING_WIFI = "扫描 Wi-Fi...";
        constexpr const char* SERVER_ERROR = "发送失败，请检查网络";
        constexpr const char* SERVER_NOT_CONNECTED = "无法连接服务，请稍后再试";
        constexpr const char* SERVER_NOT_FOUND = "正在寻找可用服务";
        constexpr const char* SERVER_TIMEOUT = "等待响应超时";
        constexpr const char* SPEAKING = "说话中...";
        constexpr const char* STANDBY = "待命";
        constexpr const char* SWITCH_TO_4G_NETWORK = "切换到 4G...";
        constexpr const char* SWITCH_TO_WIFI_NETWORK = "切换到 Wi-Fi...";
        constexpr const char* UPGRADE_FAILED = "升级失败";
        constexpr const char* UPGRADING = "正在升级系统...";
        constexpr const char* VERSION = "版本 ";
        constexpr const char* VOLUME = "音量 ";
        constexpr const char* WARNING = "警告";
        constexpr const char* WIFI_CONFIG_MODE = "配网模式";
    }

    // 音效资源 (en-US as fallback for missing audio files)
    namespace Sounds {

        extern const char ogg_ratchet_detent_start[] asm("_binary_ratchet_detent_ogg_start");
        extern const char ogg_ratchet_detent_end[] asm("_binary_ratchet_detent_ogg_end");
        static const std::string_view OGG_RATCHET_DETENT {
        static_cast<const char*>(ogg_ratchet_detent_start),
        static_cast<size_t>(ogg_ratchet_detent_end - ogg_ratchet_detent_start)
        };

        // Optional voice prompts are distributed separately.  Keep the
        // symbols available for builds that do not include those assets;
        // Application::Alert skips playback for an empty sound view.
        inline constexpr std::string_view OGG_0{};
        inline constexpr std::string_view OGG_1{};
        inline constexpr std::string_view OGG_2{};
        inline constexpr std::string_view OGG_3{};
        inline constexpr std::string_view OGG_4{};
        inline constexpr std::string_view OGG_5{};
        inline constexpr std::string_view OGG_6{};
        inline constexpr std::string_view OGG_7{};
        inline constexpr std::string_view OGG_8{};
        inline constexpr std::string_view OGG_9{};
        inline constexpr std::string_view OGG_ACTIVATION{};
        inline constexpr std::string_view OGG_ERR_PIN{};
        inline constexpr std::string_view OGG_ERR_REG{};
        inline constexpr std::string_view OGG_EXCLAMATION{};
        inline constexpr std::string_view OGG_LOW_BATTERY{};
        inline constexpr std::string_view OGG_POPUP{};
        inline constexpr std::string_view OGG_SUCCESS{};
        inline constexpr std::string_view OGG_UPGRADE{};
        inline constexpr std::string_view OGG_VIBRATION{};
        inline constexpr std::string_view OGG_WELCOME{};
        inline constexpr std::string_view OGG_WIFICONFIG{};
    }
}
