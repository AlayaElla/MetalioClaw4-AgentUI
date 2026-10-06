#pragma once

#include "components/render_snapshot_buffer.h"
#include "lvgl.h"

namespace agent_ui::home {

// Owns the settled Home carousel snapshot. The buffer remains caller-owned so
// its storage can be cache-line aligned in PSRAM and registered with the
// existing opaque-image PPA path.
class CarouselRenderCache {
public:
    CarouselRenderCache() = default;
    CarouselRenderCache(const CarouselRenderCache&) = delete;
    CarouselRenderCache& operator=(const CarouselRenderCache&) = delete;
    ~CarouselRenderCache();

    bool Capture(lv_obj_t* source, lv_obj_t* image);
    void Release();

    bool valid() const { return snapshot_.valid(); }
    const lv_draw_buf_t* buffer() const { return snapshot_.buffer(); }

private:
    RenderSnapshotBuffer snapshot_;
    lv_obj_t* image_ = nullptr;
};

}  // namespace agent_ui::home
