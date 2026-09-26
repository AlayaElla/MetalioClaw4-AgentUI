#include "camera_controller.h"
#include <cassert>
#include <iostream>

using namespace agent_ui;
using namespace agent_ui::camera;

int main() {
    Controller controller;
    int saves = 0;
    controller.Activate([](const ViewState&) {}, [&](const Command& command) {
        if (command.type == CommandType::SaveReview) ++saves;
    });
    controller.HandleLifecycle(AppLifecycleEvent::Load);
    auto image = std::make_shared<DecodedImage>();
    image->pixels = std::shared_ptr<uint8_t>(new uint8_t[2], std::default_delete<uint8_t[]>());
    image->width = image->height = 1;
    Event ready;
    ready.type = EventType::ReviewReady;
    ready.generation = controller.generation();
    ready.success = true;
    ready.review_image = image;
    controller.HandleEvent(ready);
    assert(controller.state().review_ready);

    controller.HandleIntent(Intent::SaveReview());
    assert(saves == 1 && controller.state().saving);
    Event started;
    started.type = EventType::SaveStarted;
    started.generation = controller.generation();
    controller.HandleEvent(started);
    controller.HandleIntent(Intent::SaveReview());
    controller.HandleIntent(Intent::DeleteReview());
    assert(saves == 1 && controller.state().review_image == image);

    Event failed;
    failed.type = EventType::SaveFinished;
    failed.generation = controller.generation();
    failed.status_code = StatusCode::SaveFailed;
    failed.text = "save_task_allocation_failed";
    controller.HandleEvent(failed);
    assert(!controller.state().saving && controller.state().frozen);
    assert(controller.state().mode == ViewMode::Review);
    assert(controller.state().review_ready && controller.state().review_image == image);

    controller.HandleIntent(Intent::SaveReview());
    assert(saves == 2 && controller.state().status_code == StatusCode::None);
    assert(controller.state().status.empty());
    Event saved = failed;
    saved.success = true;
    saved.status_code = StatusCode::SaveSucceeded;
    saved.text.clear();
    saved.generation += 1;
    controller.HandleEvent(saved);
    assert(controller.state().saving && controller.state().review_ready);
    saved.generation = controller.generation();
    controller.HandleEvent(saved);
    assert(!controller.state().saving && !controller.state().frozen);
    assert(controller.state().mode == ViewMode::Camera && !controller.state().review_ready);
    assert(!controller.state().review_image);

    controller.HandleEvent(ready);
    controller.HandleIntent(Intent::SaveReview());
    controller.HandleEvent(failed);
    controller.HandleIntent(Intent::DeleteReview());
    assert(controller.state().mode == ViewMode::Camera);
    assert(controller.state().status_code == StatusCode::None);
    controller.HandleLifecycle(AppLifecycleEvent::Unload);
    controller.HandleEvent(failed);
    assert(!controller.state().mounted && !controller.state().review_image);
    std::cout << "camera controller: failure/retry/success/duplicate/stale/unload passed\n";
}
