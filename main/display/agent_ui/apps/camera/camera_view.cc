#include "camera_view.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include <font_awesome.h>

#include "components/ui_components.h"
#include "core/fonts.h"
#include "core/theme.h"
#include "i18n.h"

namespace agent_ui::camera {
namespace {

constexpr int kPanelW = 720;
constexpr int kPanelH = 658;
constexpr int kCameraAreaW = 720;
constexpr int kCameraAreaH = 526;
constexpr int kButtonStripY = kCameraAreaH;
constexpr int kReviewW = 600;
constexpr int kReviewH = 394;
constexpr int kReviewFramePad = 14;
constexpr int kReviewFrameBottom = 40;
constexpr int kReviewFrameW = kReviewW + 2 * kReviewFramePad;
constexpr int kReviewFrameH = kReviewFramePad + kReviewH + kReviewFrameBottom;
constexpr int kGalleryThumbW = 220;
constexpr int kGalleryThumbH = 144;
constexpr int kGalleryImageW = kGalleryThumbW - 10;
constexpr int kGalleryImageH = kGalleryThumbH;
constexpr int kMaxGalleryItems = 48;
constexpr int kGallerySectionHeight = 238;
constexpr int kGalleryRailHeight = 196;
constexpr int kPreviewWidth = kCameraAreaW;
constexpr int kPreviewHeight = kCameraAreaH;
constexpr int kReviewWidth = kReviewFrameW;
constexpr int kReviewHeight = kReviewFrameH;
constexpr int kReviewImageWidth = kReviewW;
constexpr int kReviewImageHeight = kReviewH;
constexpr int kReviewPad = kReviewFramePad;
constexpr int kGalleryCardWidth = kGalleryThumbW;
constexpr int kGalleryCardHeight = kGalleryThumbH + 38;
constexpr int kGalleryThumbWidth = kGalleryImageW;
constexpr int kGalleryThumbHeight = kGalleryImageH;
constexpr uint32_t kFlashFadeInMs = 30;
constexpr uint32_t kFlashFadeOutMs = 220;
constexpr uint32_t kFlashTotalMs = kFlashFadeInMs + kFlashFadeOutMs;
constexpr uint32_t kFlashTickMs = 16;
constexpr uint32_t kReviewExitDurationMs = 200;
constexpr int32_t kReviewExitOffsetY = 12;
static_assert(kFlashTotalMs == 250);

View* Owner(lv_event_t* event) {
    if (event == nullptr) return nullptr;
    return static_cast<View*>(lv_event_get_user_data(event));
}

View* s_active_view = nullptr;

void Hide(lv_obj_t* object) {
    if (object != nullptr) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

void Show(lv_obj_t* object) {
    if (object != nullptr) lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
}

const char* StatusText(const ViewState& state) {
    switch (state.status_code) {
        case StatusCode::SaveSucceeded:
            return nullptr;
        case StatusCode::SaveFailed:
            return I18n::T("保存失败");
        case StatusCode::DeleteFailed:
            return I18n::T("删除失败");
        case StatusCode::CameraStartupFailed:
            return I18n::T("摄像头启动失败");
        case StatusCode::BackendMessage:
        case StatusCode::None:
            return state.status.empty() ? nullptr : state.status.c_str();
    }
    return nullptr;
}

void SetDescriptor(lv_image_dsc_t& descriptor, const uint8_t* data,
                   std::size_t data_size, std::size_t width,
                   std::size_t height, std::size_t stride) {
    descriptor = {};
    descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    descriptor.header.w = static_cast<uint32_t>(width);
    descriptor.header.h = static_cast<uint32_t>(height);
    descriptor.header.stride = static_cast<uint32_t>(stride);
    descriptor.data_size = static_cast<uint32_t>(data_size);
    descriptor.data = data;
}

lv_obj_t* AddFinderMark(lv_obj_t* parent, int x, int y, int width, int height) {
    lv_obj_t* mark = lv_obj_create(parent);
    StripObjectChrome(mark);
    lv_obj_set_size(mark, width, height);
    lv_obj_set_pos(mark, x, y);
    lv_obj_set_style_bg_color(mark, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mark, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(mark, 2, LV_PART_MAIN);
    MakeInputPassive(mark);
    return mark;
}

}  // namespace

void View::BuildInto(lv_obj_t* parent, IntentSink intent_sink) {
    Reset();
    intent_sink_ = std::move(intent_sink);
    parent_ = parent;
    if (parent_ == nullptr) return;
    s_active_view = this;
    AttachSwipeBack(parent_, OnSwipeBack);

    content_ = lv_obj_create(parent_);
    StripObjectChrome(content_);
    lv_obj_set_size(content_, kPanelW, kPanelH);
    lv_obj_set_pos(content_, 0, metrics::kStatusBarHeight);
    lv_obj_set_style_bg_color(content_,
                              lv_color_hex(Theme::Get().colors().background),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(content_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(content_, LV_OBJ_FLAG_SCROLLABLE);

    camera_panel_ = lv_obj_create(content_);
    StripObjectChrome(camera_panel_);
    lv_obj_set_size(camera_panel_, kCameraAreaW, kCameraAreaH);
    lv_obj_set_pos(camera_panel_, 0, 0);
    lv_obj_set_style_bg_color(camera_panel_, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(camera_panel_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(camera_panel_, LV_OBJ_FLAG_SCROLLABLE);
    MakeInputPassive(camera_panel_);

    preview_ = lv_image_create(camera_panel_);
    lv_obj_set_size(preview_, kPreviewWidth, kPreviewHeight);
    lv_obj_set_pos(preview_, 0, 0);
    lv_obj_add_event_cb(preview_, OnPreviewDrawPost, LV_EVENT_DRAW_POST_END,
                        this);
    MakeInputPassive(preview_);

    viewfinder_ = lv_obj_create(camera_panel_);
    StripObjectChrome(viewfinder_);
    constexpr int kFinderW = 600;
    constexpr int kFinderH = 390;
    constexpr int kCornerLength = 54;
    constexpr int kLineThickness = 4;
    lv_obj_set_size(viewfinder_, kFinderW, kFinderH);
    lv_obj_align(viewfinder_, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_bg_opa(viewfinder_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(viewfinder_, LV_OBJ_FLAG_SCROLLABLE);
    AddFinderMark(viewfinder_, 0, 0, kCornerLength, kLineThickness);
    AddFinderMark(viewfinder_, 0, 0, kLineThickness, kCornerLength);
    AddFinderMark(viewfinder_, kFinderW - kCornerLength, 0, kCornerLength,
                  kLineThickness);
    AddFinderMark(viewfinder_, kFinderW - kLineThickness, 0, kLineThickness,
                  kCornerLength);
    AddFinderMark(viewfinder_, 0, kFinderH - kLineThickness, kCornerLength,
                  kLineThickness);
    AddFinderMark(viewfinder_, 0, kFinderH - kCornerLength, kLineThickness,
                  kCornerLength);
    AddFinderMark(viewfinder_, kFinderW - kCornerLength,
                  kFinderH - kLineThickness, kCornerLength, kLineThickness);
    AddFinderMark(viewfinder_, kFinderW - kLineThickness,
                  kFinderH - kCornerLength, kLineThickness, kCornerLength);
    AddFinderMark(viewfinder_, kFinderW / 2 - 30, kFinderH / 2 - 1, 18, 3);
    AddFinderMark(viewfinder_, kFinderW / 2 + 12, kFinderH / 2 - 1, 18, 3);
    AddFinderMark(viewfinder_, kFinderW / 2 - 1, kFinderH / 2 - 30, 3, 18);
    AddFinderMark(viewfinder_, kFinderW / 2 - 1, kFinderH / 2 + 12, 3, 18);
    MakeInputPassive(viewfinder_);

    status_ = lv_label_create(camera_panel_);
    lv_label_set_text(status_, "");
    lv_obj_set_style_text_color(status_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(status_, fonts::Medium(), LV_PART_MAIN);
    lv_obj_align(status_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(status_, LV_OBJ_FLAG_HIDDEN);
    MakeInputPassive(status_);

    review_mask_ = lv_obj_create(camera_panel_);
    StripObjectChrome(review_mask_);
    lv_obj_set_size(review_mask_, kCameraAreaW, kCameraAreaH);
    lv_obj_set_pos(review_mask_, 0, 0);
    lv_obj_set_style_bg_color(review_mask_, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(review_mask_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(review_mask_, LV_OBJ_FLAG_SCROLLABLE);
    IgnoreSwipeBack(review_mask_, true);
    lv_obj_add_flag(review_mask_, LV_OBJ_FLAG_HIDDEN);

    review_image_ = lv_image_create(review_mask_);
    lv_obj_set_size(review_image_, kReviewFrameW, kReviewFrameH);
    lv_image_set_inner_align(review_image_, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(review_image_, LV_ALIGN_CENTER, 0, 0);
    MakeInputPassive(review_image_);

    flash_ = lv_obj_create(camera_panel_);
    StripObjectChrome(flash_);
    lv_obj_set_size(flash_, kCameraAreaW, kCameraAreaH);
    lv_obj_set_pos(flash_, 0, 0);
    lv_obj_set_style_bg_color(flash_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(flash_, LV_OPA_TRANSP, LV_PART_MAIN);
    MakeInputPassive(flash_);
    lv_obj_add_flag(flash_, LV_OBJ_FLAG_HIDDEN);

    gallery_ = lv_obj_create(content_);
    StripObjectChrome(gallery_);
    lv_obj_set_size(gallery_, kPanelW, kPanelH);
    lv_obj_set_pos(gallery_, 0, 0);
    lv_obj_set_style_bg_color(gallery_,
                              lv_color_hex(Theme::Get().colors().background),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(gallery_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(gallery_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(gallery_, LV_OBJ_FLAG_SCROLLABLE);

    gallery_header_ = ui_components::CreateBottomActionBar(
        gallery_, metrics::kBottomActionContentHeight);
    ui_components::AddBottomActionButton(
        gallery_header_, FONT_AWESOME_ARROW_LEFT, I18n::T("返回"),
        OnGalleryBack, this);
    ui_components::AddBottomPrimaryButton(
        gallery_header_, FONT_AWESOME_CAMERA, I18n::T("拍摄新照片"),
        OnGalleryNew, this);
    lv_obj_t* gallery_balance = ui_components::AddBottomActionSpacer(
        gallery_header_);
    if (gallery_balance != nullptr) {
        lv_obj_set_flex_grow(gallery_balance, 0);
        lv_obj_set_width(gallery_balance, 72);
    }

    gallery_list_ = lv_obj_create(gallery_);
    StripObjectChrome(gallery_list_);
    lv_obj_set_size(gallery_list_, kPanelW,
                    metrics::kBottomActionContentHeight);
    lv_obj_set_pos(gallery_list_, 0, 0);
    lv_obj_set_style_bg_color(gallery_list_,
                              lv_color_hex(Theme::Get().colors().background),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(gallery_list_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(gallery_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(gallery_list_, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(gallery_list_, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(gallery_list_, 18, LV_PART_MAIN);
    lv_obj_set_scroll_dir(gallery_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(gallery_list_, LV_SCROLLBAR_MODE_AUTO);
    IgnoreSwipeBack(gallery_list_);

    gallery_empty_ = lv_label_create(gallery_);
    lv_label_set_text(gallery_empty_, I18n::T("暂无照片"));
    lv_obj_set_style_text_color(gallery_empty_,
                                lv_color_hex(Theme::Get().colors().muted),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(gallery_empty_, fonts::Medium(), LV_PART_MAIN);
    lv_obj_align(gallery_empty_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(gallery_empty_, LV_OBJ_FLAG_HIDDEN);
    MakeInputPassive(gallery_empty_);
    lv_obj_move_foreground(gallery_header_);

    viewer_ = lv_obj_create(content_);
    StripObjectChrome(viewer_);
    lv_obj_set_size(viewer_, kPanelW, kPanelH);
    lv_obj_set_pos(viewer_, 0, 0);
    lv_obj_set_style_bg_color(viewer_,
                              lv_color_hex(Theme::Get().colors().background),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(viewer_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(viewer_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(viewer_, LV_OBJ_FLAG_SCROLLABLE);
    IgnoreSwipeBack(viewer_, true);

    viewer_actions_ = ui_components::CreateBottomActionBar(
        viewer_, metrics::kBottomActionContentHeight);
    ui_components::AddBottomActionButton(
        viewer_actions_, FONT_AWESOME_ARROW_LEFT, I18n::T("返回"),
        OnViewerBack, this);
    ui_components::AddBottomActionSpacer(viewer_actions_);
    ui_components::AddBottomActionButton(
        viewer_actions_, FONT_AWESOME_TRASH, I18n::T("删除"),
        OnViewerDelete, this, true);

    viewer_image_ = lv_image_create(viewer_);
    lv_obj_set_size(viewer_image_, kPanelW,
                    metrics::kBottomActionContentHeight);
    lv_obj_set_pos(viewer_image_, 0, 0);
    lv_image_set_inner_align(viewer_image_, LV_IMAGE_ALIGN_CONTAIN);
    MakeInputPassive(viewer_image_);

    viewer_status_ = lv_label_create(viewer_);
    lv_obj_set_size(viewer_status_, kPanelW,
                    LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(viewer_status_,
                                lv_color_hex(Theme::Get().colors().danger),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(viewer_status_, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_style_text_align(viewer_status_, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_label_set_long_mode(viewer_status_, LV_LABEL_LONG_WRAP);
    lv_obj_align(viewer_status_, LV_ALIGN_TOP_MID, 0, 240);
    lv_obj_add_flag(viewer_status_, LV_OBJ_FLAG_HIDDEN);
    MakeInputPassive(viewer_status_);

    camera_strip_ = ui_components::CreateBottomActionBar(content_, kButtonStripY);
    ui_components::AddBottomActionButton(
        camera_strip_, FONT_AWESOME_ARROW_LEFT, I18n::T("返回"), OnBack,
        this);
    const auto capture = ui_components::AddBottomPrimaryButton(
        camera_strip_, FONT_AWESOME_CAMERA, I18n::T("拍照"), OnCapture,
        this);
    capture_button_ = capture.root;
    capture_icon_ = capture.icon;
    capture_label_ = capture.label;
    ui_components::AddBottomActionButton(
        camera_strip_, FONT_AWESOME_IMAGE, I18n::T("相册"), OnGallery, this);

    review_strip_ = ui_components::CreateBottomActionBar(content_, kButtonStripY);
    lv_obj_set_flex_align(review_strip_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(review_strip_, 54, LV_PART_MAIN);
    auto review_delete = ui_components::AddBottomActionButton(
        review_strip_, FONT_AWESOME_TRASH, I18n::T("删除"), OnReviewDelete,
        this, true);
    auto review_save = ui_components::AddBottomActionButton(
        review_strip_, FONT_AWESOME_CHECK, I18n::T("保存"), OnReviewSave,
        this);
    lv_obj_set_width(review_delete.root, 230);
    lv_obj_set_width(review_save.root, 230);
    lv_obj_set_style_bg_color(review_delete.root,
                              lv_color_hex(Theme::Get().colors().raised),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(review_delete.root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(review_save.root,
                              lv_color_hex(Theme::Get().colors().accent),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(review_save.root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(review_save.icon,
                                lv_color_hex(Theme::Get().colors().accent_ink),
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(review_save.label,
                                lv_color_hex(Theme::Get().colors().accent_ink),
                                LV_PART_MAIN);
    lv_obj_add_flag(review_strip_, LV_OBJ_FLAG_HIDDEN);

    ApplyModeVisibility(state_.mode);
    Render(state_);
}

void View::Emit(const Intent& intent) {
    if (intent_sink_) intent_sink_(intent);
}

bool View::CameraReady(const ViewState& state) const {
    const auto& frame = state.preview_frame;
    return state.preview_running && frame != nullptr && frame->data != nullptr &&
           frame->width > 0 && frame->height > 0 &&
           state.status_code != StatusCode::CameraStartupFailed;
}

void View::UpdateCaptureButton(const ViewState& state) {
    if (capture_button_ == nullptr || capture_icon_ == nullptr ||
        capture_label_ == nullptr) {
        return;
    }
    const auto& colors = Theme::Get().colors();
    if (CameraReady(state)) {
        lv_obj_clear_state(capture_button_, LV_STATE_DISABLED);
        lv_obj_set_style_opa(capture_button_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(capture_button_, lv_color_hex(colors.accent),
                                  LV_PART_MAIN);
        lv_label_set_text(capture_icon_, FONT_AWESOME_CAMERA);
        lv_label_set_text(capture_label_, I18n::T("拍照"));
        lv_obj_set_style_text_color(capture_icon_,
                                    lv_color_hex(colors.accent_ink),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(capture_label_,
                                    lv_color_hex(colors.accent_ink),
                                    LV_PART_MAIN);
        return;
    }

    lv_obj_add_state(capture_button_, LV_STATE_DISABLED);
    lv_obj_set_style_opa(capture_button_, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_bg_color(capture_button_, lv_color_hex(colors.raised),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(capture_button_, LV_OPA_COVER, LV_PART_MAIN);
    lv_label_set_text(capture_icon_, FONT_AWESOME_ARROWS_ROTATE);
    lv_label_set_text(capture_label_, I18n::T("启动中"));
    lv_obj_set_style_text_color(capture_icon_, lv_color_hex(colors.muted),
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(capture_label_, lv_color_hex(colors.muted),
                                LV_PART_MAIN);
}

void View::SetReviewActionsEnabled(bool enabled) {
    if (review_strip_ == nullptr) return;
    if (enabled) {
        lv_obj_clear_state(review_strip_, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(review_strip_, LV_STATE_DISABLED);
    }
    const uint32_t child_count = lv_obj_get_child_count(review_strip_);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t* child = lv_obj_get_child(review_strip_, static_cast<int32_t>(i));
        if (child == nullptr) continue;
        if (enabled) {
            lv_obj_clear_state(child, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(child, LV_STATE_DISABLED);
        }
    }
}

void View::ResetReviewExitVisual() {
    if (review_mask_ != nullptr) {
        lv_obj_set_style_opa(review_mask_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_translate_y(review_mask_, 0, LV_PART_MAIN);
    }
    if (review_strip_ != nullptr) {
        lv_obj_set_style_opa(review_strip_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_translate_y(review_strip_, 0, LV_PART_MAIN);
    }
}

void View::BeginReviewExit(IntentType intent_type) {
    if (state_.mode != ViewMode::Review || review_exit_active_ ||
        (intent_type != IntentType::SaveReview &&
         intent_type != IntentType::DeleteReview)) {
        return;
    }
    lv_anim_delete(this, OnReviewExitAnimation);
    pending_review_intent_ = intent_type;
    review_exit_active_ = true;
    ResetReviewExitVisual();
    SetReviewActionsEnabled(false);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, this);
    lv_anim_set_exec_cb(&animation, OnReviewExitAnimation);
    lv_anim_set_values(&animation, 0, LV_OPA_COVER);
    lv_anim_set_duration(&animation, kReviewExitDurationMs);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&animation, OnReviewExitCompleted);
    lv_anim_start(&animation);
}

void View::CompleteReviewExit() {
    if (!review_exit_active_) return;
    const IntentType intent_type = pending_review_intent_;
    review_exit_active_ = false;
    pending_review_intent_ = IntentType::NavigateBack;
    if (intent_type == IntentType::SaveReview) {
        Emit(Intent::SaveReview());
    } else if (intent_type == IntentType::DeleteReview) {
        Emit(Intent::DeleteReview());
    }
    if (state_.mode == ViewMode::Review && !state_.saving) {
        ResetReviewExitVisual();
        SetReviewActionsEnabled(true);
    }
}

void View::CancelReviewExit() {
    lv_anim_delete(this, OnReviewExitAnimation);
    review_exit_active_ = false;
    pending_review_intent_ = IntentType::NavigateBack;
    ResetReviewExitVisual();
    SetReviewActionsEnabled(true);
}

void View::OnReviewExitAnimation(void* object, int32_t progress) {
    auto* self = static_cast<View*>(object);
    if (self == nullptr || !self->review_exit_active_) return;
    const int32_t clamped = std::clamp(
        progress, static_cast<int32_t>(0), static_cast<int32_t>(LV_OPA_COVER));
    const lv_opa_t opacity = static_cast<lv_opa_t>(LV_OPA_COVER - clamped);
    const int32_t offset =
        (clamped * kReviewExitOffsetY) / static_cast<int32_t>(LV_OPA_COVER);
    if (self->review_mask_ != nullptr) {
        lv_obj_set_style_opa(self->review_mask_, opacity, LV_PART_MAIN);
        lv_obj_set_style_translate_y(self->review_mask_, offset, LV_PART_MAIN);
    }
    if (self->review_strip_ != nullptr) {
        lv_obj_set_style_opa(self->review_strip_, opacity, LV_PART_MAIN);
        lv_obj_set_style_translate_y(self->review_strip_, offset, LV_PART_MAIN);
    }
}

void View::OnReviewExitCompleted(lv_anim_t*) {
    if (s_active_view != nullptr) s_active_view->CompleteReviewExit();
}

void View::Render(const ViewState& state) {
    if (content_ == nullptr) {
        state_ = state;
        return;
    }
    const ViewMode previous_mode = state_.mode;
    state_ = state;
    Hide(camera_panel_);
    Hide(gallery_);
    Hide(viewer_);
    ApplyModeVisibility(state.mode);
    const char* status_text = StatusText(state);
    if (status_ != nullptr) {
        if (status_text != nullptr) {
            lv_label_set_text(status_, status_text);
            Show(status_);
        } else {
            Hide(status_);
        }
    }
    switch (state.mode) {
        case ViewMode::Camera:
            RenderCamera(state);
            break;
        case ViewMode::Review:
            RenderReview(state);
            break;
        case ViewMode::Gallery:
            RenderGallery(state);
            break;
        case ViewMode::Viewer:
            RenderViewer(state);
            break;
    }
    if (state.mode == ViewMode::Review && previous_mode != ViewMode::Review) {
        CancelReviewExit();
        ResetReviewExitVisual();
        // Capture starts the flash before dispatching the asynchronous command.
        // A ReviewReady state only supplies the already-composed descriptor.
        review_capture_pending_ = false;
        if (flash_timer_ == nullptr && state.review_image != nullptr &&
            !flash_timed_out_) {
            StartReviewFlashAnimation();
        }
        // A late ready result is shown directly after the bounded wait; never
        // restart a second full flash cycle.
        flash_timed_out_ = false;
    }
    if (state.mode != ViewMode::Review && review_capture_pending_) {
        // A rejected/stale capture or an unload must never leave an opaque
        // flash covering the camera indefinitely.
        StopReviewFlashAnimation();
        review_capture_pending_ = false;
    }
    if (state.mode != ViewMode::Review && previous_mode == ViewMode::Review) {
        CancelReviewExit();
        StopReviewFlashAnimation();
        review_frame_.reset();
    }
}

void View::RenderCamera(const ViewState& state) {
    Show(camera_panel_);
    Show(preview_);
    UpdateCaptureButton(state);
    RenderPreview(state);
}

void View::RenderReview(const ViewState& state) {
    Show(camera_panel_);
    lv_obj_t* review_ = review_image_;
    const bool flash_is_covering = flash_timer_ != nullptr && !flash_fading_out_;
    if (!flash_is_covering) Show(review_mask_);
    if (state.review_image && state.review_image->pixels &&
        state.review_image->width > 0 && state.review_image->height > 0) {
        review_frame_ = state.review_image;
        SetDescriptor(review_descriptor_, review_frame_->pixels.get(),
                      review_frame_->data_size, review_frame_->width,
                      review_frame_->height, review_frame_->stride);
        lv_image_set_src(review_, &review_descriptor_);
        Show(review_);
    } else {
        review_frame_.reset();
        lv_image_set_src(review_, nullptr);
    }
}

void View::RenderGallery(const ViewState& state) {
    Show(gallery_);
    if (gallery_empty_ != nullptr) {
        if (state.gallery_loading) {
            Hide(gallery_empty_);
        } else if (!state.gallery_available) {
            lv_label_set_text(gallery_empty_, I18n::T("请插入 SD 卡"));
            Show(gallery_empty_);
        } else if (state.gallery.empty()) {
            lv_label_set_text(gallery_empty_, I18n::T("暂无照片"));
            Show(gallery_empty_);
        } else {
            Hide(gallery_empty_);
        }
    }
    RenderGalleryItems(state);
}

void View::RenderViewer(const ViewState& state) {
    Show(viewer_);
    lv_obj_move_foreground(viewer_);
    Hide(viewer_status_);
    if (state.viewer_image && state.viewer_image->pixels) {
        viewer_frame_ = state.viewer_image;
        SetDescriptor(viewer_descriptor_, viewer_frame_->pixels.get(),
                      viewer_frame_->data_size, viewer_frame_->width,
                      viewer_frame_->height, viewer_frame_->stride);
        lv_image_set_src(viewer_image_, &viewer_descriptor_);
        Show(viewer_image_);
    } else if (state.viewer_index < thumbnail_descriptors_.size() &&
               thumbnail_descriptors_[state.viewer_index].data != nullptr) {
        // Keep the gallery thumbnail visible until the full-resolution decoder
        // publishes the viewer image.
        viewer_frame_.reset();
        lv_image_set_src(viewer_image_,
                         &thumbnail_descriptors_[state.viewer_index]);
        Show(viewer_image_);
    } else if (state.viewer_loading) {
        viewer_frame_.reset();
        lv_image_set_src(viewer_image_, nullptr);
        if (viewer_status_ != nullptr) {
            lv_label_set_text(viewer_status_, I18n::T("正在加载照片…"));
            Show(viewer_status_);
        }
    } else if (viewer_image_ != nullptr) {
        viewer_frame_.reset();
        lv_image_set_src(viewer_image_, nullptr);
    }
    const char* status_text = StatusText(state);
    if (!state.viewer_loading && status_text != nullptr &&
        viewer_status_ != nullptr) {
        lv_label_set_text(viewer_status_, status_text);
        Show(viewer_status_);
    }
}

void View::ApplyModeVisibility(ViewMode mode) {
    Hide(camera_panel_);
    Hide(gallery_);
    Hide(viewer_);
    Hide(camera_strip_);
    Hide(review_strip_);
    Hide(gallery_header_);
    Hide(viewer_actions_);
    if (mode == ViewMode::Camera) {
        Show(camera_strip_);
    }
    const bool show_camera = mode == ViewMode::Camera || mode == ViewMode::Review;
    if (show_camera) {
        Show(camera_panel_);
    } else {
        Hide(camera_panel_);
    }
    if (mode == ViewMode::Gallery) {
        Show(gallery_);
        lv_obj_move_foreground(gallery_);
    } else {
        Hide(gallery_);
    }
    if (mode == ViewMode::Viewer) {
        Show(viewer_);
        lv_obj_move_foreground(viewer_);
    } else {
        Hide(viewer_);
    }
    if (mode == ViewMode::Gallery) {
        Show(gallery_header_);
    } else {
        Hide(gallery_header_);
    }
    if (mode == ViewMode::Viewer) {
        Show(viewer_actions_);
    } else {
        Hide(viewer_actions_);
    }
    Hide(review_mask_);
    if (mode == ViewMode::Review &&
        (flash_timer_ == nullptr || flash_fading_out_)) {
        Show(review_mask_);
    }
    if (mode == ViewMode::Review &&
        (flash_timer_ == nullptr || flash_fading_out_)) {
        Show(review_strip_);
    }
}

void View::RenderPreview(const ViewState& state) {
    if (!state.preview_frame || state.preview_frame->data == nullptr ||
        preview_ == nullptr) {
        return;
    }
    preview_frame_ = state.preview_frame;
    pending_buffer_index_ = preview_frame_->buffer_index;
    SetDescriptor(preview_descriptor_, preview_frame_->data,
                  static_cast<std::size_t>(preview_frame_->width) *
                      preview_frame_->height * 2,
                  preview_frame_->width, preview_frame_->height,
                  static_cast<std::size_t>(preview_frame_->width) * 2);
    lv_image_set_src(preview_, &preview_descriptor_);
}

void View::RenderGalleryItems(const ViewState& state) {
    if (gallery_list_ == nullptr) return;
    const auto set_thumbnail_failure = [this](std::size_t index, bool failed) {
        if (index >= thumbnail_images_.size()) return;
        if (failed) {
            lv_image_set_src(thumbnail_images_[index], nullptr);
        }
        if (index < thumbnail_failure_labels_.size() &&
            thumbnail_failure_labels_[index] != nullptr) {
            if (failed) {
                Show(thumbnail_failure_labels_[index]);
            } else {
                Hide(thumbnail_failure_labels_[index]);
            }
        }
    };
    const bool structure_changed = !gallery_structure_built_ ||
                                   rendered_gallery_ != state.gallery;
    if (!structure_changed) {
        // ThumbnailReady only swaps the target image source. Keep the existing
        // rails and their scroll offsets intact while the decoder catches up.
        for (std::size_t i = 0; i < thumbnail_images_.size() &&
                                i < state.thumbnails.size(); ++i) {
            const auto& decoded = state.thumbnails[i];
            if (decoded && decoded->pixels) {
                thumbnail_frames_[i] = decoded;
                SetDescriptor(thumbnail_descriptors_[i], decoded->pixels.get(),
                              decoded->data_size, decoded->width, decoded->height,
                              decoded->stride);
                lv_image_set_src(thumbnail_images_[i], &thumbnail_descriptors_[i]);
                set_thumbnail_failure(i, false);
            } else if (i < state.thumbnail_failures.size() &&
                       state.thumbnail_failures[i]) {
                set_thumbnail_failure(i, true);
            } else {
                set_thumbnail_failure(i, false);
                if (i < thumbnail_requested_.size() &&
                    !thumbnail_requested_[i] && i < state.gallery.size()) {
                    thumbnail_requested_[i] = true;
                    Emit(Intent::LoadThumbnail(i));
                }
            }
        }
        return;
    }

    gallery_structure_built_ = true;
    rendered_gallery_ = state.gallery;
    lv_obj_clean(gallery_list_);
    thumbnail_images_.clear();
    thumbnail_failure_labels_.clear();
    thumbnail_descriptors_.clear();
    thumbnail_descriptors_.resize(state.gallery.size());
    thumbnail_frames_.assign(state.gallery.size(), nullptr);
    thumbnail_requested_.assign(state.gallery.size(), false);

    if (state.gallery.empty()) {
        lv_obj_set_style_pad_top(gallery_list_, 0, LV_PART_MAIN);
        return;
    }

    std::vector<std::pair<std::size_t, std::size_t>> groups;
    std::size_t begin = 0;
    while (begin < state.gallery.size()) {
        const std::string date_key = state.gallery[begin].date_key;
        std::size_t end = begin + 1;
        while (end < state.gallery.size() &&
               state.gallery[end].date_key == date_key) {
            ++end;
        }
        groups.emplace_back(begin, end);
        begin = end;
    }
    const int centered_pad = groups.size() <= 2
                                 ? std::max(
                                       18, (metrics::kBottomActionContentHeight -
                                                static_cast<int>(groups.size()) *
                                                    kGallerySectionHeight -
                                                static_cast<int>(groups.size() - 1) *
                                                    2) /
                                            2)
                                 : 18;
    lv_obj_set_style_pad_top(gallery_list_, centered_pad, LV_PART_MAIN);

    const auto& colors = Theme::Get().colors();
    for (const auto& group : groups) {
        begin = group.first;
        const std::size_t end = group.second;
        lv_obj_t* section = lv_obj_create(gallery_list_);
        StripObjectChrome(section);
        lv_obj_set_width(section, kPanelW);
        lv_obj_set_height(section, kGallerySectionHeight);
        lv_obj_set_style_bg_opa(section, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_clear_flag(section, LV_OBJ_FLAG_SCROLLABLE);

        const auto& first_photo = state.gallery[begin];
        const std::string date_label = first_photo.date_label.empty()
                                           ? first_photo.date_key
                                           : first_photo.date_label;
        const std::string day_text =
            date_label + "   " + std::to_string(end - begin) + "张";
        lv_obj_t* date = lv_label_create(section);
        lv_label_set_text(date, day_text.c_str());
        lv_obj_set_style_text_font(date, fonts::MediumBold(), LV_PART_MAIN);
        lv_obj_set_style_text_color(date, lv_color_hex(colors.text), LV_PART_MAIN);
        lv_obj_set_pos(date, 28, 0);
        MakeInputPassive(date);

        lv_obj_t* rail = lv_obj_create(section);
        StripObjectChrome(rail);
        lv_obj_set_size(rail, lv_pct(100), kGalleryRailHeight);
        lv_obj_set_pos(rail, 0, 38);
        lv_obj_set_style_bg_opa(rail, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_ROW);
        lv_obj_set_scroll_dir(rail, LV_DIR_HOR);
        if (end - begin <= 3) {
            lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        } else {
            // Centering an overflowing flex track gives the first card a
            // negative x coordinate that LTR scrolling cannot recover from.
            lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        }
        lv_obj_set_style_pad_left(rail, 30, LV_PART_MAIN);
        lv_obj_set_style_pad_right(rail, 30, LV_PART_MAIN);
        lv_obj_set_style_pad_column(rail, -10, LV_PART_MAIN);
        lv_obj_set_scrollbar_mode(rail, LV_SCROLLBAR_MODE_OFF);
        IgnoreSwipeBack(rail);

        for (std::size_t i = begin; i < end; ++i) {
            const auto& photo = state.gallery[i];
            lv_obj_t* card = ui_components::CreateButton(rail);
            lv_obj_set_size(card, kGalleryCardWidth, kGalleryCardHeight);
            lv_obj_set_style_bg_color(card, lv_color_hex(colors.surface), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(card, 3, LV_PART_MAIN);
            lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(card, lv_color_hex(colors.border), LV_PART_MAIN);
            lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_pad_all(card, 5, LV_PART_MAIN);
            const int stagger_step = static_cast<int>(i % 3) - 1;
            lv_obj_set_style_translate_y(card, stagger_step * 4, LV_PART_MAIN);
            lv_obj_add_event_cb(card, OnGalleryItem, LV_EVENT_CLICKED, this);

            lv_obj_t* image = lv_image_create(card);
            lv_obj_set_size(image, kGalleryThumbWidth, kGalleryThumbHeight);
            lv_obj_set_style_bg_color(image, lv_color_hex(colors.raised), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(image, LV_OPA_COVER, LV_PART_MAIN);
            lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
            lv_obj_align(image, LV_ALIGN_TOP_MID, 0, 0);
            MakeInputPassive(image);
            thumbnail_images_.push_back(image);
            lv_obj_t* failure = lv_label_create(card);
            lv_label_set_text(failure, I18n::T("图片加载失败"));
            lv_obj_set_style_text_color(failure, lv_color_hex(colors.muted),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_font(failure, fonts::Small(), LV_PART_MAIN);
            lv_obj_align(failure, LV_ALIGN_CENTER, 0, 0);
            MakeInputPassive(failure);
            Hide(failure);
            thumbnail_failure_labels_.push_back(failure);
            if (i < state.thumbnails.size() && state.thumbnails[i] &&
                state.thumbnails[i]->pixels) {
                const auto& decoded = state.thumbnails[i];
                thumbnail_frames_[i] = decoded;
                SetDescriptor(thumbnail_descriptors_[i], decoded->pixels.get(),
                              decoded->data_size, decoded->width, decoded->height,
                              decoded->stride);
                lv_image_set_src(image, &thumbnail_descriptors_[i]);
                set_thumbnail_failure(i, false);
            } else if (i < state.thumbnail_failures.size() &&
                       state.thumbnail_failures[i]) {
                set_thumbnail_failure(i, true);
            } else {
                thumbnail_requested_[i] = true;
                Emit(Intent::LoadThumbnail(i));
            }

            const std::string time_label = photo.time_label.empty()
                                               ? "--:--"
                                               : photo.time_label;
            const std::string size_label = photo.size_label.empty()
                                               ? I18n::T("大小未知")
                                               : photo.size_label;
            const std::string meta = time_label + "        " + size_label;
            lv_obj_t* caption = lv_label_create(card);
            lv_label_set_text(caption, meta.c_str());
            lv_obj_set_width(caption, kGalleryCardWidth - 14);
            lv_obj_set_style_text_color(caption, lv_color_hex(colors.text), LV_PART_MAIN);
            lv_obj_set_style_text_font(caption, fonts::Small(), LV_PART_MAIN);
            lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -1);
            MakeInputPassive(caption);
        }
    }
}

void View::StartReviewFlashAnimation() {
    if (flash_ == nullptr) return;
    StopReviewFlashAnimation();
    flash_timed_out_ = false;
    Hide(review_mask_);
    Hide(review_strip_);
    Show(flash_);
    lv_obj_move_foreground(flash_);
    lv_obj_set_style_bg_opa(flash_, LV_OPA_TRANSP, LV_PART_MAIN);
    flash_elapsed_ms_ = 0;
    flash_fading_out_ = false;
    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, flash_);
    lv_anim_set_user_data(&fade_in, this);
    lv_anim_set_exec_cb(&fade_in, SetFlashOpacity);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade_in, kFlashFadeInMs);
    lv_anim_set_path_cb(&fade_in, lv_anim_path_linear);
    lv_anim_set_completed_cb(&fade_in, OnFlashFadeInCompleted);
    lv_anim_start(&fade_in);
    flash_timer_ = lv_timer_create(OnFlashTimer, kFlashTickMs, this);
}

void View::StartReviewFlashFade(uint32_t duration_ms) {
    if (flash_ == nullptr) return;
    if (duration_ms == 0) {
        OnFlashFadeCompleted(nullptr);
        return;
    }
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, flash_);
    lv_anim_set_user_data(&animation, this);
    lv_anim_set_exec_cb(&animation, SetFlashOpacity);
    lv_anim_set_values(&animation, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&animation, duration_ms);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_set_completed_cb(&animation, OnFlashFadeCompleted);
    lv_anim_start(&animation);
}

void View::SetFlashOpacity(void* object, int32_t opacity) {
    auto* flash = static_cast<lv_obj_t*>(object);
    if (flash == nullptr) return;
    const int32_t clamped = std::clamp(
        opacity, static_cast<int32_t>(LV_OPA_TRANSP),
        static_cast<int32_t>(LV_OPA_COVER));
    lv_obj_set_style_bg_opa(flash, static_cast<lv_opa_t>(clamped), LV_PART_MAIN);
}

void View::OnFlashFadeInCompleted(lv_anim_t* animation) {
    View* self = animation != nullptr
                     ? static_cast<View*>(lv_anim_get_user_data(animation))
                     : s_active_view;
    if (self != nullptr && self->flash_ != nullptr) {
        SetFlashOpacity(self->flash_, LV_OPA_COVER);
    }
}

void View::OnFlashFadeCompleted(lv_anim_t* animation) {
    View* self = animation != nullptr
                     ? static_cast<View*>(lv_anim_get_user_data(animation))
                     : s_active_view;
    if (self == nullptr) return;
    if (self->flash_ != nullptr) {
        SetFlashOpacity(self->flash_, LV_OPA_TRANSP);
        Hide(self->flash_);
    }
    self->flash_fading_out_ = false;
    self->flash_elapsed_ms_ = 0;
    self->review_capture_pending_ = false;
}

void View::StopReviewFlashAnimation() {
    if (flash_timer_ != nullptr) {
        lv_timer_delete(flash_timer_);
        flash_timer_ = nullptr;
    }
    if (flash_ != nullptr) {
        lv_anim_delete(flash_, SetFlashOpacity);
        SetFlashOpacity(flash_, LV_OPA_TRANSP);
    }
    flash_fading_out_ = false;
    flash_elapsed_ms_ = 0;
    Hide(flash_);
}

void View::RevealReviewAtFlashPeak() {
    if (review_mask_ != nullptr) {
        Show(review_mask_);
        lv_obj_move_foreground(review_mask_);
    }
    if (review_strip_ != nullptr) {
        Show(review_strip_);
        lv_obj_move_foreground(review_strip_);
    }
    if (flash_ != nullptr) lv_obj_move_foreground(flash_);
    if (flash_ != nullptr) {
        lv_obj_set_style_bg_opa(flash_, LV_OPA_COVER, LV_PART_MAIN);
    }
}

void View::ClearContent() {
    CancelReviewExit();
    StopReviewFlashAnimation();
    if (content_ != nullptr) lv_obj_clean(content_);
    parent_ = nullptr;
    if (s_active_view == this) s_active_view = nullptr;
    content_ = nullptr;
    camera_panel_ = nullptr;
    viewfinder_ = nullptr;
    preview_ = nullptr;
    review_mask_ = nullptr;
    review_image_ = nullptr;
    flash_ = nullptr;
    camera_strip_ = nullptr;
    review_strip_ = nullptr;
    gallery_ = nullptr;
    gallery_header_ = nullptr;
    viewer_ = nullptr;
    viewer_actions_ = nullptr;
    capture_button_ = nullptr;
    capture_icon_ = nullptr;
    capture_label_ = nullptr;
    status_ = nullptr;
    gallery_empty_ = nullptr;
    gallery_list_ = nullptr;
    viewer_image_ = nullptr;
    viewer_status_ = nullptr;
    thumbnail_images_.clear();
    thumbnail_failure_labels_.clear();
    thumbnail_descriptors_.clear();
    thumbnail_requested_.clear();
    thumbnail_frames_.clear();
    rendered_gallery_.clear();
    gallery_structure_built_ = false;
    preview_frame_.reset();
    review_frame_.reset();
    viewer_frame_.reset();
    flash_timed_out_ = false;
    review_capture_pending_ = false;
}

void View::Reset() {
    ClearContent();
    intent_sink_ = nullptr;
    state_ = {};
    pending_buffer_index_ = -1;
    flash_elapsed_ms_ = 0;
    flash_fading_out_ = false;
    flash_timed_out_ = false;
}

void View::LifecycleCallback(Lifecycle lifecycle) {
    if (lifecycle == Lifecycle::Unload) Reset();
}

void View::OnCapture(lv_event_t* event) {
    View* self = Owner(event);
    if (self == nullptr) return;
    switch (self->state_.mode) {
        case ViewMode::Camera:
            if (!self->CameraReady(self->state_)) return;
            self->review_capture_pending_ = true;
            self->StartReviewFlashAnimation();
            self->Emit(Intent::Capture());
            break;
        case ViewMode::Review:
            self->BeginReviewExit(IntentType::SaveReview);
            break;
        case ViewMode::Gallery:
            self->Emit(Intent::GalleryBack());
            break;
        case ViewMode::Viewer:
            self->Emit(Intent::DeleteViewer());
            break;
    }
}

void View::OnGallery(lv_event_t* event) {
    if (View* self = Owner(event)) self->Emit(Intent::OpenGallery());
}

void View::OnBack(lv_event_t* event) {
    View* self = Owner(event);
    if (self == nullptr) return;
    switch (self->state_.mode) {
        case ViewMode::Camera:
            self->Emit(Intent::NavigateBack());
            break;
        case ViewMode::Review:
            self->BeginReviewExit(IntentType::DeleteReview);
            break;
        case ViewMode::Gallery:
            self->Emit(Intent::NavigateBack());
            break;
        case ViewMode::Viewer:
            self->Emit(Intent::ViewerBack());
            break;
    }
}

void View::OnReviewDelete(lv_event_t* event) {
    if (View* self = Owner(event)) {
        self->BeginReviewExit(IntentType::DeleteReview);
    }
}

void View::OnReviewSave(lv_event_t* event) {
    if (View* self = Owner(event)) {
        self->BeginReviewExit(IntentType::SaveReview);
    }
}

void View::OnGalleryBack(lv_event_t* event) {
    if (View* self = Owner(event)) self->Emit(Intent::NavigateBack());
}

void View::OnGalleryNew(lv_event_t* event) {
    if (View* self = Owner(event)) self->Emit(Intent::GalleryBack());
}

void View::OnViewerBack(lv_event_t* event) {
    if (View* self = Owner(event)) self->Emit(Intent::ViewerBack());
}

void View::OnViewerDelete(lv_event_t* event) {
    if (View* self = Owner(event)) self->Emit(Intent::DeleteViewer());
}

void View::OnSwipeBack() {
    if (s_active_view != nullptr) s_active_view->Emit(Intent::NavigateBack());
}

void View::OnPreviewDrawPost(lv_event_t* event) {
    View* self = Owner(event);
    if (self != nullptr && self->pending_buffer_index_ >= 0) {
        self->Emit(Intent::PreviewDrawn(self->pending_buffer_index_));
    }
}

void View::OnGalleryItem(lv_event_t* event) {
    View* self = Owner(event);
    if (self == nullptr) return;
    lv_obj_t* target = lv_event_get_current_target_obj(event);
    for (std::size_t i = 0; i < self->thumbnail_images_.size(); ++i) {
        if (lv_obj_get_parent(self->thumbnail_images_[i]) == target) {
            self->Emit(Intent::OpenViewer(i));
            return;
        }
    }
}

void View::OnFlashTimer(lv_timer_t* timer) {
    View* self = timer != nullptr ? static_cast<View*>(lv_timer_get_user_data(timer))
                                  : nullptr;
    if (self == nullptr || self->flash_ == nullptr) return;
    self->flash_elapsed_ms_ = std::min(
        kFlashTotalMs, self->flash_elapsed_ms_ + kFlashTickMs);
    if (self->flash_elapsed_ms_ < kFlashFadeInMs) {
        // The native LVGL fade-in owns opacity; this timer only waits for its
        // peak before polling ReviewReady.
        return;
    }
    if (self->state_.mode != ViewMode::Review ||
        !self->state_.review_ready || self->review_frame_ == nullptr) {
        // Keep the cover opaque while the worker composes ReviewReady.  A
        // bounded timeout restores the camera; a later ready result is shown
        // directly by Render without restarting the flash.
        if (self->flash_elapsed_ms_ >= kFlashTotalMs) {
            self->flash_timed_out_ = true;
            self->StopReviewFlashAnimation();
            self->review_capture_pending_ = false;
        }
        return;
    }
    self->RevealReviewAtFlashPeak();
    if (self->flash_timer_ != nullptr) {
        lv_timer_delete(self->flash_timer_);
        self->flash_timer_ = nullptr;
    }
    self->flash_fading_out_ = true;
    self->StartReviewFlashFade(kFlashFadeOutMs);
}

}  // namespace agent_ui::camera
