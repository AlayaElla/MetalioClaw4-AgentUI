#pragma once

#include <functional>
#include <memory>
#include <string>
#include "cJSON.h"
#include "lvgl.h"
#include "codex_menu_state.h"

namespace agent_ui {
class CodexConversationUi {
public:
    using Send = std::function<bool(const std::string&)>;
    using Markdown = std::function<void(lv_obj_t*, const char*, bool)>;
    CodexConversationUi(lv_obj_t* parent, lv_obj_t* root, Send send, Markdown markdown);
    ~CodexConversationUi();
    void Update(const codex_menu::Conversation& conversation, bool connected, bool new_task_draft = false,
                bool draft_binding = false, const std::string& draft_error = {},
                std::function<void()> retry_draft = {}, const std::string& draft_status = {});
    bool HandleMessage(const cJSON* root);
    bool HandleMessage(const std::string& json);
    void Disconnected();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace agent_ui
