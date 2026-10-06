#include "codex_conversation_ui.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <font_awesome.h>
#include "cJSON.h"
#include "codex_media_cache.h"
#include "core/fonts.h"
#include "core/theme.h"
#include "components/system_keyboard.h"
#include "misc/cache/instance/lv_image_cache.h"

namespace agent_ui {
namespace {
std::string String(const cJSON* root, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}
std::string Json(cJSON* value) {
    char* text = cJSON_PrintUnformatted(value);
    const std::string result = text ? text : "";
    if (text) cJSON_free(text);
    cJSON_Delete(value); return result;
}
void Text(lv_obj_t* parent, const std::string& text, bool muted = false) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, fonts::Medium(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(muted ? Theme::Get().colors().muted : Theme::Get().colors().text), 0);
    lv_label_set_text(label, text.c_str());
}
bool SetLabelTextIfChanged(lv_obj_t* label, const char* text) {
    if (label != nullptr && text != nullptr &&
        std::strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
        return true;
    }
    return false;
}
struct Callback { std::function<void(lv_event_t*)> function; lv_event_code_t code; };
void OnEvent(lv_event_t* event) {
    auto* callback = static_cast<Callback*>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        if (callback->code == LV_EVENT_DELETE) callback->function(event);
        delete callback;
    }
    else callback->function(event);
}
void Bind(lv_obj_t* object, lv_event_code_t code, std::function<void(lv_event_t*)> function) {
    auto* callback = new Callback{std::move(function), code};
    lv_obj_add_event_cb(object, OnEvent, code, callback);
    // A separate callback allocation would double-own the function. The same
    // data is freed exactly once by the object's deletion event.
    if (code != LV_EVENT_DELETE) lv_obj_add_event_cb(object, OnEvent, LV_EVENT_DELETE, callback);
}
lv_obj_t* Button(lv_obj_t* parent, const std::string& label, std::function<void()> click, bool enabled = true, bool accent = false, bool selected = false) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(button, 14, 0);
    lv_obj_set_style_radius(button, 16, 0);
    const auto& colors = Theme::Get().colors();
    lv_obj_set_style_bg_color(button, lv_color_hex(accent ? colors.accent : colors.surface), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(colors.accent_pressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_border_width(button, selected ? 3 : 0, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(colors.accent_ink), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    Text(button, label);
    if (accent) lv_obj_set_style_text_color(lv_obj_get_child(button, 0), lv_color_hex(colors.accent_ink), 0);
    if (!enabled) lv_obj_add_state(button, LV_STATE_DISABLED);
    Bind(button, LV_EVENT_CLICKED, [click = std::move(click)](lv_event_t*) { click(); });
    return button;
}
struct ImageSource {
    lv_image_dsc_t descriptor{};
    std::shared_ptr<codex_media::Image> image;
    explicit ImageSource(std::shared_ptr<codex_media::Image> value) : image(std::move(value)) {
        descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
        descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
        descriptor.header.w = image->width; descriptor.header.h = image->height;
        descriptor.header.stride = image->width * 2;
        descriptor.data_size = image->pixels.size(); descriptor.data = image->pixels.data();
    }
};
}  // namespace

struct CodexConversationUi::Impl {
    struct Draft {
        std::map<std::string, std::vector<std::string>> answers;
        std::string request_id, error;
        uint32_t submitted_at = 0;
    };
    struct ImageWidget { std::string key; lv_obj_t *button = nullptr, *label = nullptr, *image = nullptr; };
    struct Row { lv_obj_t* object = nullptr; std::string signature; std::vector<ImageWidget> images; };
    lv_obj_t *parent, *root, *viewer = nullptr;
    Send send;
    Markdown markdown;
    codex_menu::Conversation conversation;
    bool connected = false;
    bool new_task_draft = false;
    bool draft_binding = false;
    std::string draft_error;
    std::string draft_status;
    std::function<void()> retry_draft;
    std::map<std::string, Row> rows;
    std::vector<std::string> rendered_order;
    std::map<std::string, Draft> drafts;
    std::map<std::string, uint32_t> image_started;
    codex_media::Cache media;
    std::vector<ImageWidget> viewer_images;
    uint64_t sequence = 0;
    lv_timer_t* timer = nullptr;
    bool render_scheduled = false;
    int32_t layout_width = -1;
    int32_t layout_height = -1;

    Impl(lv_obj_t* parent, lv_obj_t* root, Send send, Markdown markdown)
        : parent(parent), root(root), send(std::move(send)), markdown(std::move(markdown)) {
        timer = lv_timer_create([](lv_timer_t* timer) { static_cast<Impl*>(lv_timer_get_user_data(timer))->Tick(); }, 1000, this);
    }
    ~Impl() {
        lv_async_call_cancel(DeferredRender, this);
        if (timer) lv_timer_delete(timer);
    }
    std::string RequestId() { return "esp-dialog-" + std::to_string(lv_tick_get()) + "-" + std::to_string(++sequence); }
    std::string DraftKey(const std::string& id) const {
        return std::to_string(conversation.host_id.size()) + ":" + conversation.host_id +
               std::to_string(conversation.thread_id.size()) + ":" + conversation.thread_id +
               std::to_string(id.size()) + ":" + id;
    }
    const codex_menu::Interaction* Find(const std::string& id) {
        for (const auto& request : conversation.interactions) if (request.id == id) return &request;
        return nullptr;
    }
    bool Pending(const codex_menu::Interaction& request) const { return request.status == "pending" || request.status == "error"; }
    static void DeferredRender(void* user_data) {
        auto* self = static_cast<Impl*>(user_data);
        if (!self) return;
        self->render_scheduled = false;
        self->Render();
    }
    void ScheduleRender() {
        if (render_scheduled) return;
        render_scheduled = lv_async_call(DeferredRender, this) == LV_RESULT_OK;
    }
    void Tick() {
        bool changed = false;
        for (auto it = image_started.begin(); it != image_started.end();) {
            auto entry = media.Get(it->first);
            if (!entry || !entry->pending) it = image_started.erase(it);
            else if (lv_tick_elaps(it->second) > 20000) {
                media.Fail(it->first, "图片加载超时，点击重试"); it = image_started.erase(it); changed = true;
            } else ++it;
        }
        for (auto& item : drafts) if (!item.second.request_id.empty() && lv_tick_elaps(item.second.submitted_at) > 15000) {
            item.second.request_id.clear(); item.second.error = "尚未收到处理回执，可重试"; changed = true;
        }
        if (changed) Render();
    }
    lv_obj_t* Bubble(Row& row, bool user) {
        row.object = lv_obj_create(parent);
        lv_obj_remove_style_all(row.object); lv_obj_set_size(row.object, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_remove_flag(row.object, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_bottom(row.object, 10, 0);
        lv_obj_t* bubble = lv_obj_create(row.object);
        lv_obj_set_size(bubble, user ? 590 : 620, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(bubble, 28, 0); lv_obj_set_style_pad_ver(bubble, 22, 0);
        lv_obj_set_style_radius(bubble, 24, 0); lv_obj_set_style_border_width(bubble, 0, 0);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(user ? Theme::Get().colors().accent : Theme::Get().colors().raised), 0);
        lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(bubble, 14, 0);
        lv_obj_align(bubble, user ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, user ? -4 : 4, 0);
        return bubble;
    }
    void RequestImage(const std::string& media_id, const std::string& variant) {
        if (!connected) return;
        const auto id = RequestId();
        if (!media.Request(conversation.thread_id, media_id, variant, id)) return;
        const auto key = codex_media::Cache::Key(conversation.thread_id, media_id, variant);
        cJSON* request = cJSON_CreateObject();
        cJSON_AddStringToObject(request, "type", "codex_media_request");
        cJSON_AddStringToObject(request, "host_id", conversation.host_id.c_str());
        cJSON_AddStringToObject(request, "thread_id", conversation.thread_id.c_str());
        cJSON_AddStringToObject(request, "media_id", media_id.c_str());
        cJSON_AddStringToObject(request, "variant", variant.c_str());
        cJSON_AddStringToObject(request, "request_id", id.c_str());
        if (send(Json(request))) image_started[key] = lv_tick_get();
        else media.Fail(key, "发送失败，点击重试");
    }
    ImageWidget AddImage(lv_obj_t* parent, const std::string& media_id, const std::string& variant) {
        ImageWidget widget;
        widget.key = codex_media::Cache::Key(conversation.thread_id, media_id, variant);
        widget.button = lv_button_create(parent);
        lv_obj_set_size(widget.button, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(widget.button, 10, 0);
        lv_obj_set_style_bg_color(widget.button, lv_color_hex(Theme::Get().colors().surface), 0);
        lv_obj_set_style_shadow_width(widget.button, 0, 0);
        lv_obj_set_flex_flow(widget.button, LV_FLEX_FLOW_COLUMN);
        widget.label = lv_label_create(widget.button);
        lv_obj_set_style_text_font(widget.label, fonts::Medium(), 0);
        lv_obj_set_style_text_color(widget.label, lv_color_hex(Theme::Get().colors().muted), 0);
        lv_label_set_text(widget.label, "正在加载图片…");
        Bind(widget.button, LV_EVENT_CLICKED, [this, media_id, variant](lv_event_t*) {
            auto entry = media.Get(codex_media::Cache::Key(conversation.thread_id, media_id, variant));
            if (entry && entry->ready && variant == "thumb") OpenImage(media_id);
            else { RequestImage(media_id, variant); RefreshImages(); }
        });
        RequestImage(media_id, variant);
        return widget;
    }
    bool RefreshImage(ImageWidget& widget) {
        auto entry = media.Get(widget.key);
        if (!entry || !entry->ready) {
            return SetLabelTextIfChanged(
                widget.label,
                !connected ? "已断开，重连后点击加载"
                                 : entry && !entry->error.empty()
                                       ? entry->error.c_str()
                                       : "正在加载图片…");
        }
        if (widget.image) return false;
        auto* source = new ImageSource(entry->image);
        widget.image = lv_image_create(widget.button);
        lv_image_set_src(widget.image, &source->descriptor);
        Bind(widget.image, LV_EVENT_DELETE, [source](lv_event_t*) {
            // Stride/post-processing can make LVGL cache a decoded copy under
            // the descriptor address. Drop it before that address is reused.
            lv_image_cache_drop(&source->descriptor);
            delete source;
        });
        lv_obj_add_flag(widget.label, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
    bool RefreshImages() {
        bool changed = false;
        for (auto& row : rows) {
            for (auto& image : row.second.images) {
                changed = RefreshImage(image) || changed;
            }
        }
        for (auto& image : viewer_images) {
            changed = RefreshImage(image) || changed;
        }
        return changed;
    }
    void CloseImage() { viewer_images.clear(); if (viewer) { lv_obj_delete(viewer); viewer = nullptr; } }
    void OpenImage(const std::string& id) {
        CloseImage(); Keyboard::Get().Hide();
        viewer = lv_obj_create(root); lv_obj_set_size(viewer, LV_PCT(100), LV_PCT(100)); lv_obj_center(viewer);
        lv_obj_set_style_bg_color(viewer, lv_color_hex(Theme::Get().colors().background), 0);
        lv_obj_set_style_pad_all(viewer, 24, 0); lv_obj_set_flex_flow(viewer, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_top(viewer, metrics::kStatusBarHeight + 16, 0);
        viewer_images.push_back(AddImage(viewer, id, "full"));
        // Keep the close control over the image's upper-right corner, below
        // the global status bar and independent of the image's scroll offset.
        lv_obj_t* close = lv_button_create(viewer);
        lv_obj_remove_style_all(close);
        lv_obj_set_size(close, 64, 64);
        lv_obj_add_flag(close, static_cast<lv_obj_flag_t>(
            LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_IGNORE_LAYOUT));
        lv_obj_remove_flag(close, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -12, 12);
        const auto& colors = Theme::Get().colors();
        lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(close, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(close, lv_color_hex(colors.accent), 0);
        lv_obj_set_style_bg_color(close, lv_color_hex(colors.accent_pressed), LV_STATE_PRESSED);
        lv_obj_t* icon = lv_label_create(close);
        lv_label_set_text(icon, FONT_AWESOME_XMARK);
        lv_obj_set_style_text_font(icon, fonts::IconLarge(), 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(colors.accent_ink), 0);
        lv_obj_center(icon);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        Bind(close, LV_EVENT_CLICKED, [this](lv_event_t*) {
            auto* image_viewer = viewer;
            viewer = nullptr;
            viewer_images.clear();
            // Delete after LVGL finishes dispatching the close-button event.
            if (image_viewer) lv_obj_delete_async(image_viewer);
        });
        lv_obj_move_foreground(viewer);
        RefreshImages();
        lv_obj_update_layout(viewer);
    }
    void Submit(const std::string& id, const std::string& decision = {}) {
        const auto* interaction = Find(id);
        if (!interaction || !interaction->can_respond || !Pending(*interaction) || !connected) return;
        auto& draft = drafts[DraftKey(id)];
        if (!draft.request_id.empty()) return;
        cJSON* request = cJSON_CreateObject();
        cJSON_AddStringToObject(request, "type", "codex_interaction_response");
        cJSON_AddStringToObject(request, "host_id", conversation.host_id.c_str());
        cJSON_AddStringToObject(request, "thread_id", conversation.thread_id.c_str());
        cJSON_AddStringToObject(request, "id", id.c_str());
        const std::string request_id = RequestId();
        cJSON_AddStringToObject(request, "request_id", request_id.c_str());
        if (interaction->kind == "approval") cJSON_AddStringToObject(request, "decision", decision.c_str());
        else {
            cJSON* answers = cJSON_AddObjectToObject(request, "answers");
            for (const auto& question : interaction->questions) {
                auto selected = draft.answers[question.id];
                if (selected.empty()) { cJSON_Delete(request); draft.error = "请完成每道问题后再提交"; ScheduleRender(); return; }
                cJSON* values = cJSON_AddArrayToObject(answers, question.id.c_str());
                for (const auto& value : selected) cJSON_AddItemToArray(values, cJSON_CreateString(value.c_str()));
            }
        }
        draft.error.clear(); draft.request_id = request_id; draft.submitted_at = lv_tick_get();
        Keyboard::Get().Hide();
        if (!send(Json(request))) { draft.request_id.clear(); draft.error = "发送失败，请重试"; }
        ScheduleRender();
    }
    void Interaction(Row& row, const codex_menu::Interaction& request) {
        lv_obj_t* bubble = Bubble(row, false);
        const auto key = DraftKey(request.id);
        auto& draft = drafts[key];
        Text(bubble, request.title.empty() ? request.kind == "approval" ? "需要你的确认" : "等待你的回答" : request.title);
        if (!request.body.empty()) {
            if (request.body.size() <= 768) markdown(bubble, request.body.c_str(), false);
            else {
                lv_obj_t* details = lv_obj_create(bubble);
                lv_obj_set_size(details, LV_PCT(100), LV_SIZE_CONTENT);
                lv_obj_set_flex_flow(details, LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_all(details, 0, 0);
                lv_obj_set_style_border_width(details, 0, 0); lv_obj_add_flag(details, LV_OBJ_FLAG_HIDDEN);
                Button(bubble, "展开操作详情", [details] { if (lv_obj_has_flag(details, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(details, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(details, LV_OBJ_FLAG_HIDDEN); });
                markdown(details, request.body.c_str(), false);
            }
        }
        if (!Pending(request) && request.status != "submitting") {
            Text(bubble, !request.response_text.empty() ? request.response_text : request.status == "expired" ? "请求已过期" : "已在电脑端处理", true);
            return;
        }
        if (!request.can_respond) {
            Text(bubble, "此请求只能在电脑端处理", true);
            return;
        }
        const bool enabled = connected && draft.request_id.empty() && request.status != "submitting";
        if (request.kind == "approval") {
            for (const auto& option : request.options) Button(bubble, option.label, [this, key, id = request.id, decision = option.id] {
                if (DraftKey(id) == key) Submit(id, decision);
            }, enabled, true);
            if (request.options.empty()) Text(bubble, "请在电脑端处理此确认", true);
        } else {
            if (!request.blocking) Text(bubble, "Codex 正在继续工作，你仍可回答", true);
            const bool choices_available = !request.questions.empty() && std::all_of(request.questions.begin(), request.questions.end(),
                [](const auto& question) { return !question.options.empty(); });
            const bool direct = request.questions.size() == 1 && !request.questions.front().multiple;
            for (const auto& question : request.questions) {
                if (!question.header.empty()) Text(bubble, question.header);
                Text(bubble, question.question);
                for (const auto& option : question.options) {
                    const auto& answers = draft.answers[question.id];
                    const bool selected = std::find(answers.begin(), answers.end(), option.label) != answers.end();
                    Button(bubble, option.label, [this, key, id = request.id, qid = question.id, value = option.label, multiple = question.multiple, direct] {
                        if (DraftKey(id) != key || !connected || !drafts[key].request_id.empty()) return;
                        auto& answers = drafts[key].answers[qid];
                        const bool selected = std::find(answers.begin(), answers.end(), value) != answers.end();
                        if (!multiple) answers.clear();
                        answers.erase(std::remove(answers.begin(), answers.end(), value), answers.end());
                        if (!multiple || !selected) answers.push_back(value);
                        if (direct) { Submit(id); return; }
                        InvalidateRequest(key); ScheduleRender();
                    }, enabled && choices_available, true, selected);
                    if (!option.description.empty()) Text(bubble, option.description, true);
                }
                if (question.options.empty()) Text(bubble, "请在电脑端回答此问题", true);
            }
            if (choices_available && !direct) Button(bubble, "提交回答", [this, key, id = request.id] {
                if (DraftKey(id) == key) Submit(id);
            }, enabled, true);
        }
        if (!enabled) Text(bubble, connected ? "正在提交…" : "已断开，重连后可继续", true);
        if (!draft.error.empty() || !request.error.empty()) Text(bubble, !draft.error.empty() ? draft.error : request.error, true);
    }
    void InvalidateRequest(const std::string& key) {
        auto found = rows.find("request:" + key);
        if (found != rows.end()) found->second.signature.clear();
    }
    void Render() {
        const bool bottom = lv_obj_get_scroll_bottom(parent) < 40;
        const int32_t scroll = lv_obj_get_scroll_y(parent);
        const int32_t width = lv_obj_get_width(parent);
        const int32_t height = lv_obj_get_height(parent);
        bool layout_changed = width != layout_width || height != layout_height;
        std::vector<std::string> order;
        auto request = [&](const codex_menu::Interaction& value) {
            const auto key = "request:" + DraftKey(value.id);
            auto& draft = drafts[DraftKey(value.id)];
            const auto signature = value.signature + draft.request_id + draft.error + (connected ? "1" : "0");
            auto& row = rows[key]; order.push_back(key);
            if (row.object && row.signature == signature) return;
            layout_changed = true;
            if (row.object) lv_obj_delete(row.object);
            row = {}; row.signature = signature; Interaction(row, value);
        };
        std::set<std::string> ids;
        for (const auto& value : conversation.messages) ids.insert(value.id);
        for (const auto& value : conversation.interactions) if (!ids.count(value.after_message_id)) request(value);
        for (const auto& value : conversation.messages) {
            const auto key = "message:" + value.id;
            std::string signature = value.role + value.text;
            for (const auto& part : value.content) signature += part.type + part.text + part.media_id;
            auto& row = rows[key]; order.push_back(key);
            if (!row.object || row.signature != signature) {
                layout_changed = true;
                if (row.object) lv_obj_delete(row.object);
                row = {}; row.signature = signature;
                lv_obj_t* bubble = Bubble(row, value.role == "user");
                if (value.content.empty()) markdown(bubble, value.text.c_str(), value.role == "user");
                else for (const auto& part : value.content) {
                    if (part.type == "text") markdown(bubble, part.text.c_str(), value.role == "user");
                    else if (part.type == "image") row.images.push_back(AddImage(bubble, part.media_id, "thumb"));
                }
            }
            for (const auto& pending : conversation.interactions) if (pending.after_message_id == value.id) request(pending);
        }
        if (order.empty()) {
            const auto key = "placeholder"; order.push_back(key);
            const char* placeholder = conversation.thread_id.empty() ?
                (!draft_error.empty() ? draft_error.c_str() : draft_status == "preparing" ? "正在准备新任务…" :
                 draft_binding ? "已发送，正在同步任务…" :
                 new_task_draft ? "按住下方按钮，开始新对话" : "请从菜单选择任务") :
                (conversation.ready ? "等待新消息" : "正在同步消息…");
            const std::string signature = conversation.thread_id + placeholder + (retry_draft ? "1" : "0");
            auto& row = rows[key];
            if (!row.object || row.signature != signature) {
                layout_changed = true;
                if (row.object) lv_obj_delete(row.object);
                row = {}; row.signature = signature;
                auto* bubble = Bubble(row, false);
                Text(bubble, placeholder);
                if (!draft_error.empty() && retry_draft) Button(bubble, "重试连接", [this] { retry_draft(); });
            }
        }
        const std::set<std::string> wanted(order.begin(), order.end());
        for (auto it = rows.begin(); it != rows.end();) {
            if (!wanted.count(it->first)) {
                layout_changed = true;
                lv_obj_delete(it->second.object);
                it = rows.erase(it);
            } else ++it;
        }
        if (order != rendered_order) layout_changed = true;
        // Media may complete independently of message signatures, so image
        // state is checked on every render even when transcript layout is stable.
        if (RefreshImages()) layout_changed = true;
        if (layout_changed) {
            for (size_t index = 0; index < order.size(); ++index) {
                lv_obj_move_to_index(rows[order[index]].object, index);
            }
            lv_obj_update_layout(parent);
            lv_obj_scroll_to_y(parent, bottom ? LV_COORD_MAX : scroll,
                               LV_ANIM_OFF);
            rendered_order = order;
            layout_width = width;
            layout_height = height;
        }
    }
};

CodexConversationUi::CodexConversationUi(lv_obj_t* parent, lv_obj_t* root, Send send, Markdown markdown)
    : impl_(std::make_unique<Impl>(parent, root, std::move(send), std::move(markdown))) {}
CodexConversationUi::~CodexConversationUi() = default;
void CodexConversationUi::Update(const codex_menu::Conversation& conversation, bool connected, bool new_task_draft,
                                 bool draft_binding, const std::string& draft_error,
                                 std::function<void()> retry_draft, const std::string& draft_status) {
    if (impl_->conversation.thread_id != conversation.thread_id || impl_->conversation.host_id != conversation.host_id) {
        Keyboard::Get().Hide(); impl_->CloseImage(); impl_->media.CancelPending();
        for (auto& row : impl_->rows) lv_obj_delete(row.second.object);
        impl_->rows.clear();
        impl_->rendered_order.clear();
        impl_->layout_width = -1;
        impl_->layout_height = -1;
        lv_obj_scroll_to_y(impl_->parent, 0, LV_ANIM_OFF);
    }
    impl_->conversation = conversation; impl_->connected = connected;
    if (impl_->conversation.messages.size() > 10) {
        impl_->conversation.messages.erase(impl_->conversation.messages.begin(), impl_->conversation.messages.end() - 10);
    }
    impl_->new_task_draft = new_task_draft && conversation.thread_id.empty();
    impl_->draft_binding = draft_binding && conversation.thread_id.empty();
    impl_->draft_error = impl_->new_task_draft ? draft_error : std::string{};
    impl_->retry_draft = std::move(retry_draft);
    impl_->draft_status = impl_->new_task_draft ? draft_status : std::string{};
    for (const auto& request : conversation.interactions) if (!impl_->Pending(request) && request.status != "submitting") {
        auto& draft = impl_->drafts[impl_->DraftKey(request.id)]; draft = {};
    }
    while (impl_->drafts.size() > 64) impl_->drafts.erase(impl_->drafts.begin());
    impl_->Render();
}
bool CodexConversationUi::HandleMessage(const std::string& json) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(json.c_str()), cJSON_Delete);
    return HandleMessage(root.get());
}
bool CodexConversationUi::HandleMessage(const cJSON* root) {
    if (impl_->media.Receive(root)) {
        const bool bottom = lv_obj_get_scroll_bottom(impl_->parent) < 40;
        const int32_t scroll = lv_obj_get_scroll_y(impl_->parent);
        if (impl_->RefreshImages()) {
            lv_obj_update_layout(impl_->parent);
            lv_obj_scroll_to_y(impl_->parent, bottom ? LV_COORD_MAX : scroll,
                               LV_ANIM_OFF);
            if (impl_->viewer != nullptr) lv_obj_update_layout(impl_->viewer);
        }
        return true;
    }
    if (!root) return false;
    const bool matched = String(root, "type") == "codex_interaction_result" && String(root, "host_id") == impl_->conversation.host_id &&
        String(root, "thread_id") == impl_->conversation.thread_id;
    if (matched) {
        auto& draft = impl_->drafts[impl_->DraftKey(String(root, "id"))];
        if (draft.request_id == String(root, "request_id")) {
            draft.request_id.clear();
            draft.error = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "success")) ? "" : String(root, "error");
            impl_->Render();
        }
    }
    return matched;
}
void CodexConversationUi::Disconnected() { impl_->connected = false; impl_->media.CancelPending(); impl_->Render(); }
}  // namespace agent_ui
