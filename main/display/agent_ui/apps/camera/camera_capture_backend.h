#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "camera_contract.h"

namespace agent_ui::camera {

class CaptureBackend {
public:
    using EventSink = std::function<void(const Event&)>;

    CaptureBackend();
    ~CaptureBackend();

    void SetEventSink(EventSink sink);
    bool PrepareBuffers();
    void Start(uint32_t generation);
    void Stop();
    void Capture();
    void Resume();
    void AcknowledgeFrame(uint32_t generation, int buffer_index);

    std::shared_ptr<const PreviewFrame> CurrentFrame(uint32_t generation) const;
    std::shared_ptr<const PreviewFrame> CopyCurrentFrame(uint32_t generation) const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace agent_ui::camera
