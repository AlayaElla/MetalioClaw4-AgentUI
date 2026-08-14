# ESP32 Pet animation renderer

`pet_animation` and `pet_renderer` form a small data-driven Pet runtime for the
ESP32-P4 Agent UI. The component is independent of Home, Agent expressions,
LiveRoom, and character generation.

## Runtime model

- A body image is divided into a regular 2..5 by 2..5 grid. The default 5 by 5
  grid has 25 vertices and 32 triangles.
- Every `Pose` contains body vertex offsets plus root translation, rotation,
  and scale.
- Up to four limbs are cubic Bezier curves. Each limb is anchored to one body
  vertex and stores two control points plus an end point. Keyframes animate
  their offsets, width, and opacity.
- `AnimationPlayer` samples fixed-capacity keyframe data without per-frame heap
  allocation. `Curve::y1/y2` controls the interpolation to the next keyframe
  and may overshoot for squash or bounce.
- `Renderer` rasterizes the textured triangles and anti-aliased limb segments
  into one ARGB8888 PSRAM buffer. The existing RGB888 LVGL/PPA path composites
  that buffer onto the display.

All geometry is expressed in output-buffer pixels. Grid vertex indices are
row-major, starting at the top-left.

## Texture contract

`Renderer::SetTexture()` accepts caller-owned, uncompressed LVGL descriptors in
these formats:

- `RGB565` / `RGB565_SWAPPED`
- `RGB565A8`
- `RGB888`
- `ARGB8888`
- `XRGB8888`

Compressed PNG/RLE/LZ4 descriptors must be decoded before binding. The texture
and Clip arrays must remain alive while the renderer uses them.

## Minimal setup

```cpp
using namespace agent_ui::pet;

Renderer pet(parent, 256, 256);
pet.SetTexture(&pet_body_argb8888);

Rig rig;
rig.body.columns = 5;
rig.body.rows = 5;
rig.body.destination = {24, 16, 208, 224};
rig.root_pivot = {128, 128};
rig.limb_count = 4;

// Left arm starts at grid vertex 6. Rest points are relative to the anchor.
rig.limbs[0].anchor_vertex = 6;
rig.limbs[0].control1 = {-18, 8};
rig.limbs[0].control2 = {-34, 28};
rig.limbs[0].end = {-42, 50};
rig.limbs[0].width = 7;
rig.limbs[0].color = {40, 40, 48, 255};

pet.SetRig(rig);
pet.Play(&idle_clip);
```

For direct control, call `Render(pose)` on the LVGL thread. For automatic
playback, call `Play(clip)`; the internal LVGL timer defaults to about 30 FPS
and can be changed with `SetFramePeriodMs()`.

`Stats()` exposes frame count, last/average/maximum CPU raster time, render
budget overruns, and missed timer frames. Keep these counters when comparing
grid size, output resolution, or frame cadence on real hardware; a successful
build alone is not an FPS or PSRAM-bandwidth result.

The data structures are intentionally plain fixed-size C++ values so a Unity
exporter can later bake the existing Pet animation into generated `.h/.cc`
arrays without adding JSON parsing or runtime allocation to firmware.
