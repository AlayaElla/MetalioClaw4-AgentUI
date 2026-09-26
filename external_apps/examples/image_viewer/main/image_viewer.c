#include "metalio_app_api.h"
#include "metalio_app_json.h"

#include <stddef.h>
#include <string.h>

#define IMAGE_COUNT 5U

typedef struct {
    const metalio_app_host_api_t* api;
    void* host_context;
    metalio_app_widget_t image;
    metalio_app_widget_t title;
    metalio_app_widget_t counter;
    uint32_t index;
    uint8_t haptics;
} image_viewer_state_t;

static image_viewer_state_t s_viewer;

static const char* const kImagePaths[IMAGE_COUNT] = {
    "assets/city-night.png",
    "assets/coral-coast.png",
    "assets/autumn-valley.png",
    "assets/orbital-station.png",
    "assets/demo.png",
};

static const char* const kImageTitles[IMAGE_COUNT] = {
    "香港雨夜",
    "珊瑚海岸",
    "秋日山谷",
    "轨道空间站",
    "Demo 原图",
};

static const char* const kImageCounters[IMAGE_COUNT] = {
    "1 / 5", "2 / 5", "3 / 5", "4 / 5", "5 / 5",
};

static int show_image(uint32_t index) {
    if (index >= IMAGE_COUNT || s_viewer.api == 0) return 0;
    if (s_viewer.api->set_image_source(s_viewer.host_context, s_viewer.image,
                                       kImagePaths[index]) != 0) {
        return 0;
    }
    s_viewer.index = index;
    s_viewer.api->set_label_text(s_viewer.host_context, s_viewer.title,
                                 kImageTitles[index]);
    s_viewer.api->set_label_text(s_viewer.host_context, s_viewer.counter,
                                 kImageCounters[index]);
    if (s_viewer.haptics) {
        s_viewer.api->play_haptic(s_viewer.host_context,
                                  METALIO_APP_HAPTIC_TICK);
    }
    return 1;
}

static void show_previous(void* app_context) {
    image_viewer_state_t* viewer = (image_viewer_state_t*)app_context;
    const uint32_t index =
        viewer->index == 0 ? IMAGE_COUNT - 1U : viewer->index - 1U;
    show_image(index);
}

static void show_next(void* app_context) {
    image_viewer_state_t* viewer = (image_viewer_state_t*)app_context;
    const uint32_t index =
        viewer->index + 1U == IMAGE_COUNT ? 0U : viewer->index + 1U;
    show_image(index);
}

static void on_swipe(void* app_context,
                     metalio_app_swipe_direction_t direction) {
    if (direction == METALIO_APP_SWIPE_LEFT) {
        show_next(app_context);
    } else if (direction == METALIO_APP_SWIPE_RIGHT) {
        show_previous(app_context);
    }
}

static int ai_navigate(void* context, const metalio_app_ai_action_request_t* request,
                       metalio_app_ai_action_result_t* result) {
    image_viewer_state_t* viewer = (image_viewer_state_t*)context;
    if (viewer == 0 || request == 0 || result == 0 || request->arguments_json == 0) return -1;
    uint32_t index = viewer->index;
    char direction[16];
    if (metalio_app_json_get_string(request->arguments_json, "direction", direction,
                                    sizeof(direction))) {
        if (strcmp(direction, "next") == 0) index = (index + 1U) % IMAGE_COUNT;
        else if (strcmp(direction, "previous") == 0) index = index == 0 ? IMAGE_COUNT - 1U : index - 1U;
        else { result->status = METALIO_APP_AI_ACTION_FAILED; result->error = "invalid image direction"; return 0; }
    } else if (!metalio_app_json_get_uint(request->arguments_json, "index", &index) ||
               index >= IMAGE_COUNT) {
        result->status = METALIO_APP_AI_ACTION_FAILED; result->error = "image index is required and must be in range"; return 0;
    }
    if (!show_image(index)) { result->status = METALIO_APP_AI_ACTION_FAILED; result->error = "image could not be displayed"; return 0; }
    result->status = METALIO_APP_AI_ACTION_SUCCEEDED;
    result->result_json = index == 0 ? "{\"index\":0}" : index == 1 ? "{\"index\":1}" : index == 2 ? "{\"index\":2}" : index == 3 ? "{\"index\":3}" : "{\"index\":4}";
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc != 2 || argv == 0 || argv[0] == 0 || argv[1] == 0) return 1;

    const metalio_app_host_api_t* api =
        (const metalio_app_host_api_t*)argv[0];
    const metalio_app_launch_context_t* launch =
        (const metalio_app_launch_context_t*)argv[1];
    if (api->abi_version != METALIO_APP_ABI_VERSION ||
        api->struct_size < sizeof(metalio_app_host_api_t) ||
        launch->abi_version != METALIO_APP_ABI_VERSION ||
        launch->struct_size < sizeof(metalio_app_launch_context_t)) {
        return 2;
    }

    s_viewer.api = api;
    s_viewer.host_context = launch->host_context;
    s_viewer.index = 0;
    s_viewer.haptics =
        (api->get_capabilities(launch->host_context) & METALIO_APP_CAP_HAPTICS)
            ? 1U
            : 0U;

    api->set_background(launch->host_context, 0x0B0C0F);
    metalio_app_widget_t frame = 0;
    api->add_rect(launch->host_context, 16, 16, 688, 432, 0x15171C, 24,
                  &frame);
    api->set_rect_border(launch->host_context, frame, 0x292C33, 1);

    if (api->add_image_ex(launch->host_context, kImagePaths[0], 28, 28, 664,
                          408, &s_viewer.image) != 0) {
        api->add_label(launch->host_context, "图片资源读取失败", 28, 208, 664,
                       48, 0xFF7D7D, METALIO_APP_FONT_MEDIUM_BOLD);
        return 3;
    }

    if (api->add_label_ex(launch->host_context, kImageTitles[0], 24, 466, 520,
                          42, 0xF4F5F7, METALIO_APP_FONT_MEDIUM_BOLD,
                          &s_viewer.title) != 0 ||
        api->add_label_ex(launch->host_context, kImageCounters[0], 572, 466,
                          124, 42, 0xA3A8B2, METALIO_APP_FONT_MEDIUM,
                          &s_viewer.counter) != 0) {
        return 4;
    }
    api->set_label_alignment(launch->host_context, s_viewer.counter,
                             METALIO_APP_TEXT_ALIGN_RIGHT);

    api->add_action(launch->host_context, METALIO_APP_ACTION_PREVIOUS,
                    "上一张", show_previous, &s_viewer);
    api->add_action(launch->host_context, METALIO_APP_ACTION_NEXT, "下一张",
                    show_next, &s_viewer);
    api->set_swipe_handler(launch->host_context, on_swipe, &s_viewer);
    if ((api->get_capabilities(launch->host_context) & METALIO_APP_CAP_AI_ACTIONS) != 0 &&
        api->struct_size >= offsetof(metalio_app_host_api_t, ai_unregister_actions) +
                            sizeof(api->ai_unregister_actions)) {
        api->ai_register_action(launch->host_context, "com.metalio.image-viewer.navigate",
                                ai_navigate, 0, 0, &s_viewer);
    }
    return 0;
}
