#include "metalio_app_api.h"

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

    api->set_background(launch->host_context, 0xF5F2EB);
    api->add_label(launch->host_context, "图片查看器\nSD 卡资源测试", 28, 28,
                   170, 92, 0x171717, METALIO_APP_FONT_MEDIUM_BOLD);
    api->add_label(launch->host_context,
                   "这张 PNG 来自 App 自身资源，\n由宿主直接从 SD 卡读取。",
                   28, 142, 174, 126, 0x68645E, METALIO_APP_FONT_SMALL);
    if (api->add_image(launch->host_context, "assets/demo.png", 220, 22,
                       464, 452) != 0) {
        api->add_label(launch->host_context, "无法读取 assets/demo.png",
                       220, 196, 440, 48, 0xB42318,
                       METALIO_APP_FONT_MEDIUM);
        return 3;
    }
    api->add_label(launch->host_context, "com.metalio.image-viewer · ABI 1",
                   28, 440, 174, 52, 0x8A857E, METALIO_APP_FONT_SMALL);
    return 0;
}
