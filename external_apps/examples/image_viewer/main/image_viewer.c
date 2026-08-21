#include "metalio_app_api.h"

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

static void show_image(uint32_t index) {
    if (index >= IMAGE_COUNT || s_viewer.api == 0) return;
    if (s_viewer.api->set_image_source(s_viewer.host_context, s_viewer.image,
                                       kImagePaths[index]) != 0) {
        return;
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
    return 0;
}
