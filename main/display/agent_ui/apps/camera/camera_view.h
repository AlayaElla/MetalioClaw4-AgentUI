#pragma once

#include <functional>
#include <cstdint>
#include <memory>
#include <vector>

#include "camera_contract.h"
#include "camera_view_state.h"
#include "core/ui_utils.h"
#include "lvgl.h"

namespace agent_ui::camera {

class View {
public:
    using IntentSink = std::function<void(const Intent&)>;

    void BuildInto(lv_obj_t* parent, IntentSink intent_sink);
    void Render(const ViewState& state);
    void Reset();
    void LifecycleCallback(Lifecycle lifecycle);

private:
    static void OnCapture(lv_event_t* event);
    static void OnGallery(lv_event_t* event);
    static void OnBack(lv_event_t* event);
    static void OnReviewDelete(lv_event_t* event);
    static void OnReviewSave(lv_event_t* event);
    static void OnGalleryBack(lv_event_t* event);
    static void OnGalleryNew(lv_event_t* event);
    static void OnViewerBack(lv_event_t* event);
    static void OnViewerDelete(lv_event_t* event);
    static void OnSwipeBack();
    static void OnPreviewDrawPost(lv_event_t* event);
    static void OnGalleryItem(lv_event_t* event);
    static void OnFlashTimer(lv_timer_t* timer);
    static void SetFlashOpacity(void* object, int32_t opacity);
    static void OnFlashFadeInCompleted(lv_anim_t* animation);
    static void OnFlashFadeCompleted(lv_anim_t* animation);
    static void OnReviewExitAnimation(void* object, int32_t progress);
    static void OnReviewExitCompleted(lv_anim_t* animation);

    void Emit(const Intent& intent);
    void RenderCamera(const ViewState& state);
    void RenderReview(const ViewState& state);
    void RenderGallery(const ViewState& state);
    void RenderViewer(const ViewState& state);
    void RenderPreview(const ViewState& state);
    void RenderGalleryItems(const ViewState& state);
    void ApplyModeVisibility(ViewMode mode);
    bool CameraReady(const ViewState& state) const;
    void UpdateCaptureButton(const ViewState& state);
    void BeginReviewExit(IntentType intent_type);
    void CompleteReviewExit();
    void CancelReviewExit();
    void ResetReviewExitVisual();
    void SetReviewActionsEnabled(bool enabled);
    void RevealReviewAtFlashPeak();
    void StartReviewFlashAnimation();
    void StartReviewFlashFade(uint32_t duration_ms);
    void StopReviewFlashAnimation();
    void ClearContent();

    IntentSink intent_sink_;
    lv_obj_t* parent_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* camera_panel_ = nullptr;
    lv_obj_t* viewfinder_ = nullptr;
    lv_obj_t* preview_ = nullptr;
    lv_obj_t* review_mask_ = nullptr;
    lv_obj_t* review_image_ = nullptr;
    lv_obj_t* flash_ = nullptr;
    lv_obj_t* camera_strip_ = nullptr;
    lv_obj_t* review_strip_ = nullptr;
    lv_obj_t* gallery_ = nullptr;
    lv_obj_t* gallery_header_ = nullptr;
    lv_obj_t* viewer_ = nullptr;
    lv_obj_t* viewer_actions_ = nullptr;
    lv_obj_t* capture_button_ = nullptr;
    lv_obj_t* capture_icon_ = nullptr;
    lv_obj_t* capture_label_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* gallery_empty_ = nullptr;
    lv_obj_t* gallery_list_ = nullptr;
    lv_obj_t* viewer_image_ = nullptr;
    lv_obj_t* viewer_status_ = nullptr;
    lv_image_dsc_t preview_descriptor_{};
    lv_image_dsc_t review_descriptor_{};
    lv_image_dsc_t viewer_descriptor_{};
    std::vector<lv_image_dsc_t> thumbnail_descriptors_;
    std::vector<lv_obj_t*> thumbnail_images_;
    std::vector<lv_obj_t*> thumbnail_failure_labels_;
    std::vector<bool> thumbnail_requested_;
    std::vector<std::shared_ptr<const DecodedImage>> thumbnail_frames_;
    std::vector<GalleryPhoto> rendered_gallery_;
    bool gallery_structure_built_ = false;
    std::shared_ptr<const PreviewFrame> preview_frame_;
    std::shared_ptr<const DecodedImage> review_frame_;
    std::shared_ptr<const DecodedImage> viewer_frame_;
    ViewState state_{};
    int pending_buffer_index_ = -1;
    uint32_t flash_elapsed_ms_ = 0;
    lv_timer_t* flash_timer_ = nullptr;
    bool flash_fading_out_ = false;
    bool flash_timed_out_ = false;
    bool review_capture_pending_ = false;
    bool review_exit_active_ = false;
    IntentType pending_review_intent_ = IntentType::NavigateBack;
};

}  // namespace agent_ui::camera
