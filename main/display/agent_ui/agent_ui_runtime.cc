#include "agent_ui_runtime.h"

#include <array>
#include <string>

#include <esp_random.h>
#include <esp_log.h>

#include "agent_ui/apps/camera/camera_module.h"
#include "agent_ui/apps/codex/codex_view.h"
#include "agent_ui/apps/display_debug/display_debug_view.h"
#include "agent_ui/apps/external_apps/external_apps_view.h"
#include "agent_ui/apps/external_apps/external_app_manager.h"
#include "agent_ui/apps/files/files_view.h"
#include "agent_ui/apps/phone/phone_view.h"
#include "agent_ui/apps/settings/settings_view.h"
#include "agent_ui/apps/home/home_renderer.h"
#include "agent_ui/core/app_mcp_tools.h"
#include "agent_ui/core/idle_power.h"
#include "agent_ui/core/navigation.h"
#include "agent_ui/core/status_bar.h"
#include "agent_ui/core/theme.h"
#include "agent_ui/components/system_keyboard.h"
#include "touch_feed.h"
#include "ui_dispatcher.h"

namespace agent_ui {
namespace {

constexpr char kTag[] = "AgentRuntime";

constexpr std::array<const char*, 8> kGreetings = {{
    "我在，\n随时可以开始。",
    "今天想先完成\n哪件事？",
    "有什么想法，\n随时告诉我。",
    "需要我帮你\n处理什么？",
    "新的任务，\n从一句话开始。",
    "准备好了，\n我们开始吧。",
    "有问题，\n就问我吧。",
    "今天也一起做点\n有意思的事吧。",
}};

const char* RandomGreeting() {
    return kGreetings[esp_random() % kGreetings.size()];
}

}  // namespace

Runtime& Runtime::Get() {
    static Runtime instance;
    return instance;
}

void Runtime::Initialize() {
    if (initialized_) return;
    initialized_ = true;

    const char* initial_message = home_module_.state().message.empty() ? RandomGreeting() : nullptr;
    home_module_.Initialize(
        initial_message,
        [](ScreenId target) { Navigation::Get().Open(target); });

    Theme::Get().Initialize();
    Navigation::Get().Register(ScreenId::Home, CreateHomeView);
    Navigation::Get().Register(ScreenId::Codex, CodexView::Create);
    Navigation::Get().Register(ScreenId::Camera, CreateCameraView);
    Navigation::Get().Register(ScreenId::Phone, PhoneView::Create);
    Navigation::Get().Register(ScreenId::Files, FilesView::Create);
    Navigation::Get().Register(ScreenId::Settings, SettingsView::Create);
    Navigation::Get().Register(ScreenId::ExternalAppHost,
                               external_apps::HostView::Create);
    Navigation::Get().Register(ScreenId::DisplayDebug, DisplayDebugView::Create);
    UiDispatcher::Init();
    RegisterAppMcpTools();
    StatusBar::Get().Initialize();
    StatusBar::Get().SetAgentState(home_module_.state().agent_state);
    StatusBar::Get().SetVisible(false);
    Keyboard::Get().Initialize();
}

void Runtime::OnBoardReady(Board& board) {
    bool expected = false;
    if (!board_ready_posted_.compare_exchange_strong(expected, true)) return;

    Board* board_ptr = &board;
    if (!UiDispatcher::Post([this, board_ptr]() {
            if (power_runtime_started_) return;
            IdlePower::Get().Initialize(*board_ptr);
            touch_feed_set_activity_callback([]() {
                IdlePower::Get().NotifyActivity();
            });
            power_runtime_started_ = true;
            ESP_LOGI(kTag, "Board ready; runtime power management started");
        })) {
        board_ready_posted_.store(false);
        ESP_LOGE(kTag, "Failed to post board-ready initialization");
    }
}

void Runtime::Start() {
    std::string external_apps_error;
    if (!external_apps::Manager::Get().Refresh(&external_apps_error)) {
        ESP_LOGW(kTag, "External app discovery failed: %s",
                 external_apps_error.c_str());
    }
    Navigation::Get().Start();
}

void Runtime::SetAgentState(AgentState state) {
    // Connecting/listening/answering are active operations. In particular,
    // voice wake reaches the UI through this path without a touch event, so it
    // must explicitly start Wake before the new state is rendered.
    if (state != AgentState::Idle) IdlePower::Get().NotifyActivity();
    home_module_.HandleEvent(home::Event::AgentStateChanged(state));
    StatusBar::Get().SetAgentState(state);
}

void Runtime::SetConversationMessage(const char* role, const char* content) {
    home_module_.HandleEvent(home::Event::ConversationMessage(role, content));
}

void Runtime::SetSystemStatus(const char* status) {
    global_state_.system_status = status != nullptr ? status : "";
}

void Runtime::PlayDizzyExpression() {
    home::Renderer::PlayDizzy();
}

lv_obj_t* Runtime::CreateHomeView() {
    return Get().home_module_.Mount();
}

lv_obj_t* Runtime::CreateCameraView() {
    return Get().camera_module_.Mount();
}

}  // namespace agent_ui
