#include "metalio_app_api.h"

namespace {

constexpr float kCanvasScale = 400.0f / 480.0f;
constexpr uint32_t kPerformancePeriodMs = 40;
constexpr uint32_t kQualityPeriodMs = 33;

struct AppState {
    const metalio_app_host_api_t* api;
    const metalio_app_launch_context_t* launch;
    metalio_app_widget_t pet;
    metalio_app_widget_t stats_label;
    const metalio_pet_clip_t* current_clip;
    const char* current_name;
    uint32_t frame_period_ms;
    uint32_t last_frame_count;
    uint8_t debug_enabled;
};

AppState s_app{};

constexpr metalio_pet_vec2_t Scale(metalio_pet_vec2_t value) {
    return {value.x * kCanvasScale, value.y * kCanvasScale};
}

constexpr metalio_pet_pose_t NeutralPose() {
    metalio_pet_pose_t pose{};
    pose.root_scale = {1.0f, 1.0f};
    for (uint32_t index = 0; index < METALIO_PET_MAX_LIMBS; ++index) {
        pose.limbs[index].width_scale = 1.0f;
        pose.limbs[index].opacity = 1.0f;
    }
    return pose;
}

constexpr void ApplyBodyBulge(metalio_pet_pose_t& pose, float amount) {
    pose.body_vertex_offsets[5].x -= amount * 0.35f;
    pose.body_vertex_offsets[9].x += amount * 0.35f;
    pose.body_vertex_offsets[10].x -= amount;
    pose.body_vertex_offsets[11].x -= amount * 0.35f;
    pose.body_vertex_offsets[13].x += amount * 0.35f;
    pose.body_vertex_offsets[14].x += amount;
    pose.body_vertex_offsets[15].x -= amount * 0.7f;
    pose.body_vertex_offsets[19].x += amount * 0.7f;
}

constexpr metalio_pet_pose_t InhalePose() {
    auto pose = NeutralPose();
    pose.root_translation = {0.0f, -3.0f};
    pose.root_rotation_degrees = -0.2f;
    pose.root_scale = {0.993f, 1.016f};
    ApplyBodyBulge(pose, 4.5f);
    pose.body_vertex_offsets[2].y = -2.0f;
    pose.limbs[0].control2_offset = {-1.0f, -2.0f};
    pose.limbs[0].end_offset = {-2.0f, -3.0f};
    pose.limbs[1].control2_offset = {1.0f, -2.0f};
    pose.limbs[1].end_offset = {2.0f, -3.0f};
    return pose;
}

constexpr metalio_pet_pose_t ExhalePose() {
    auto pose = NeutralPose();
    pose.root_translation = {0.0f, 3.0f};
    pose.root_rotation_degrees = 0.2f;
    pose.root_scale = {1.007f, 0.984f};
    ApplyBodyBulge(pose, -3.5f);
    pose.body_vertex_offsets[2].y = 2.0f;
    pose.limbs[0].end_offset = {1.0f, 2.0f};
    pose.limbs[1].end_offset = {-1.0f, 2.0f};
    return pose;
}

constexpr metalio_pet_pose_t AnticipationPose() {
    auto pose = NeutralPose();
    pose.root_translation = {0.0f, 5.0f};
    pose.root_scale = {1.025f, 0.975f};
    ApplyBodyBulge(pose, 7.0f);
    pose.body_vertex_offsets[20].y = -2.0f;
    pose.body_vertex_offsets[24].y = -2.0f;
    return pose;
}

constexpr metalio_pet_pose_t TakeoffPose(float height) {
    auto pose = NeutralPose();
    pose.root_translation = {0.0f, height};
    pose.root_scale = {0.99f, 1.02f};
    ApplyBodyBulge(pose, -3.0f);
    pose.body_vertex_offsets[0].y = -2.0f;
    pose.body_vertex_offsets[4].y = -2.0f;
    pose.limbs[0].control2_offset = {-4.0f, -20.0f};
    pose.limbs[0].end_offset = {-8.0f, -32.0f};
    pose.limbs[1].control2_offset = {4.0f, -20.0f};
    pose.limbs[1].end_offset = {8.0f, -32.0f};
    pose.limbs[2].end_offset = {-4.0f, -7.0f};
    pose.limbs[3].end_offset = {4.0f, -7.0f};
    return pose;
}

constexpr metalio_pet_pose_t LandingPose() {
    auto pose = NeutralPose();
    pose.root_translation = {0.0f, 4.0f};
    pose.root_scale = {1.04f, 0.96f};
    ApplyBodyBulge(pose, 9.0f);
    pose.body_vertex_offsets[11].y = 2.0f;
    pose.body_vertex_offsets[12].y = 4.0f;
    pose.body_vertex_offsets[13].y = 2.0f;
    pose.limbs[0].end_offset = {-3.0f, 4.0f};
    pose.limbs[1].end_offset = {3.0f, 4.0f};
    return pose;
}

constexpr metalio_pet_pose_t WavePose(float direction) {
    auto pose = NeutralPose();
    pose.root_rotation_degrees = 2.4f;
    pose.root_scale = {1.012f, 0.992f};
    pose.body_vertex_offsets[0].x = 2.0f;
    pose.body_vertex_offsets[5].x = 3.0f;
    pose.body_vertex_offsets[10].x = 2.0f;
    pose.body_vertex_offsets[4].x = 1.0f;
    pose.limbs[0].end_offset = {-2.0f, -3.0f};
    pose.limbs[1].control1_offset = {-7.0f + direction * 2.0f, -34.0f};
    pose.limbs[1].control2_offset = {-17.0f + direction * 7.0f, -72.0f};
    pose.limbs[1].end_offset = {-12.0f + direction * 12.0f, -102.0f};
    return pose;
}

constexpr metalio_pet_pose_t ScalePose(metalio_pet_pose_t pose) {
    pose.root_translation = Scale(pose.root_translation);
    for (uint32_t index = 0; index < METALIO_PET_MAX_BODY_VERTICES; ++index) {
        pose.body_vertex_offsets[index] = Scale(pose.body_vertex_offsets[index]);
    }
    for (uint32_t index = 0; index < METALIO_PET_MAX_LIMBS; ++index) {
        pose.limbs[index].control1_offset = Scale(pose.limbs[index].control1_offset);
        pose.limbs[index].control2_offset = Scale(pose.limbs[index].control2_offset);
        pose.limbs[index].end_offset = Scale(pose.limbs[index].end_offset);
    }
    return pose;
}

constexpr metalio_pet_keyframe_t Frame(uint32_t time_ms,
                                       metalio_pet_pose_t pose,
                                       float y1 = 0.0f, float y2 = 1.0f) {
    return {time_ms, ScalePose(pose), y1, y2};
}

constexpr metalio_pet_keyframe_t kBreathingFrames[] = {
    Frame(0, NeutralPose()), Frame(550, InhalePose()),
    Frame(1100, NeutralPose()), Frame(1650, ExhalePose()),
    Frame(2200, NeutralPose()),
};

constexpr metalio_pet_keyframe_t kBounceFrames[] = {
    Frame(0, NeutralPose()), Frame(126, AnticipationPose()),
    Frame(288, TakeoffPose(-16.0f)), Frame(450, TakeoffPose(-24.0f)),
    Frame(684, LandingPose()), Frame(900, NeutralPose()),
};

constexpr metalio_pet_keyframe_t kWaveFrames[] = {
    Frame(0, NeutralPose()), Frame(162, WavePose(0.0f), 1.0f / 3.0f, 2.0f / 3.0f),
    Frame(243, WavePose(1.0f), 1.0f / 3.0f, 2.0f / 3.0f),
    Frame(405, WavePose(-1.0f), 1.0f / 3.0f, 2.0f / 3.0f),
    Frame(567, WavePose(1.0f), 1.0f / 3.0f, 2.0f / 3.0f),
    Frame(729, WavePose(-1.0f), 1.0f / 3.0f, 2.0f / 3.0f),
    Frame(891, WavePose(1.0f), 1.0f / 3.0f, 2.0f / 3.0f),
    Frame(1107, WavePose(0.0f)), Frame(1228, WavePose(-0.25f)),
    Frame(1350, NeutralPose()),
};

constexpr metalio_pet_clip_t kBreathingClip{
    "q_drop_breathing", kBreathingFrames, 5, 2200, 1};
constexpr metalio_pet_clip_t kBounceClip{
    "q_drop_bounce", kBounceFrames, 6, 900, 1};
constexpr metalio_pet_clip_t kWaveClip{
    "q_drop_wave", kWaveFrames, 10, 1350, 1};

constexpr metalio_pet_rig_t BuildRig() {
    metalio_pet_rig_t rig{};
    rig.columns = 5;
    rig.rows = 5;
    rig.destination_x = 82.0f * kCanvasScale;
    rig.destination_y = 28.0f * kCanvasScale;
    rig.destination_width = 316.0f * kCanvasScale;
    rig.destination_height = 388.0f * kCanvasScale;
    rig.uv_width = 1.0f;
    rig.uv_height = 1.0f;
    rig.root_pivot = Scale({240.0f, 222.0f});
    rig.limb_count = 4;

    rig.limbs[0] = {15, Scale({-10.0f, 10.0f}), Scale({-26.0f, 24.0f}),
                    Scale({-40.0f, 43.0f}), 6.0f * kCanvasScale,
                    0x141414FF, 8, 0};
    rig.limbs[1] = {19, Scale({10.0f, 10.0f}), Scale({26.0f, 24.0f}),
                    Scale({40.0f, 43.0f}), 6.0f * kCanvasScale,
                    0x141414FF, 8, 0};
    rig.limbs[2] = {21, Scale({-1.0f, 12.0f}), Scale({-2.0f, 26.0f}),
                    Scale({-2.0f, 43.0f}), 6.0f * kCanvasScale,
                    0x141414FF, 6, 1};
    rig.limbs[3] = {23, Scale({1.0f, 12.0f}), Scale({2.0f, 26.0f}),
                    Scale({2.0f, 43.0f}), 6.0f * kCanvasScale,
                    0x141414FF, 6, 1};
    return rig;
}

constexpr metalio_pet_rig_t kRig = BuildRig();

char* AppendText(char* cursor, char* end, const char* text) {
    while (cursor < end && *text != '\0') *cursor++ = *text++;
    return cursor;
}

char* AppendUnsigned(char* cursor, char* end, uint32_t value) {
    char digits[10];
    uint32_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10U);
        value /= 10U;
    } while (value != 0 && count < sizeof(digits));
    while (count > 0 && cursor < end) *cursor++ = digits[--count];
    return cursor;
}

void Play(const metalio_pet_clip_t* clip, const char* name) {
    s_app.current_clip = clip;
    s_app.current_name = name;
    s_app.last_frame_count = 0;
    s_app.api->pet_play(s_app.launch->host_context, s_app.pet, clip,
                        s_app.frame_period_ms);
}

void PlayBreathing(void*) { Play(&kBreathingClip, "呼吸"); }
void PlayBounce(void*) { Play(&kBounceClip, "弹跳"); }
void PlayWave(void*) { Play(&kWaveClip, "挥手"); }

void ToggleDebug(void*) {
    s_app.debug_enabled = !s_app.debug_enabled;
    s_app.api->pet_set_debug(s_app.launch->host_context, s_app.pet,
                             s_app.debug_enabled);
}

void ToggleCadence(void*) {
    s_app.frame_period_ms = s_app.frame_period_ms == kPerformancePeriodMs
                                ? kQualityPeriodMs
                                : kPerformancePeriodMs;
    Play(s_app.current_clip, s_app.current_name);
}

void RefreshStats(void*) {
    metalio_pet_render_stats_t stats{};
    if (s_app.api->pet_get_stats(s_app.launch->host_context, s_app.pet,
                                 &stats) != 0) {
        return;
    }
    const uint32_t fps = (stats.frame_count - s_app.last_frame_count) * 2U;
    s_app.last_frame_count = stats.frame_count;
    char text[256];
    char* cursor = text;
    char* const end = text + sizeof(text) - 1;
    cursor = AppendText(cursor, end, s_app.current_name);
    cursor = AppendText(cursor, end, " · 外部 App\n目标 ");
    cursor = AppendUnsigned(cursor, end, 1000U / s_app.frame_period_ms);
    cursor = AppendText(cursor, end, " · 实际 ");
    cursor = AppendUnsigned(cursor, end, fps);
    cursor = AppendText(cursor, end, " FPS\nframe ");
    cursor = AppendUnsigned(cursor, end, stats.last_render_us / 1000U);
    cursor = AppendText(cursor, end, " · 光栅 ");
    cursor = AppendUnsigned(cursor, end, stats.last_raster_us / 1000U);
    cursor = AppendText(cursor, end, " ms\navg ");
    cursor = AppendUnsigned(cursor, end, stats.average_render_us / 1000U);
    cursor = AppendText(cursor, end, " · max ");
    cursor = AppendUnsigned(cursor, end, stats.max_render_us / 1000U);
    cursor = AppendText(cursor, end, " ms\n超时 ");
    cursor = AppendUnsigned(cursor, end, stats.budget_overrun_count);
    cursor = AppendText(cursor, end, " · 丢帧 ");
    cursor = AppendUnsigned(cursor, end, stats.missed_frame_count);
    *cursor = '\0';
    s_app.api->set_label_text(s_app.launch->host_context,
                              s_app.stats_label, text);
}

}  // namespace

extern "C" int main(int argc, char* argv[]) {
    if (argc != 2 || argv == nullptr || argv[0] == nullptr || argv[1] == nullptr) {
        return 1;
    }
    s_app.api = reinterpret_cast<const metalio_app_host_api_t*>(argv[0]);
    s_app.launch = reinterpret_cast<const metalio_app_launch_context_t*>(argv[1]);
    if (s_app.api->abi_version != METALIO_APP_ABI_VERSION ||
        s_app.api->struct_size < sizeof(metalio_app_host_api_t) ||
        s_app.launch->abi_version != METALIO_APP_ABI_VERSION ||
        s_app.launch->struct_size < sizeof(metalio_app_launch_context_t)) {
        return 2;
    }

    s_app.frame_period_ms = kPerformancePeriodMs;
    s_app.api->set_background(s_app.launch->host_context, 0xFFFFFF);
    s_app.api->add_label(s_app.launch->host_context, "Pet\n动画测试", 28, 18,
                         150, 76, 0x171717, METALIO_APP_FONT_MEDIUM_BOLD);
    if (s_app.api->add_label_ex(s_app.launch->host_context, "正在初始化…",
                                28, 104, 160, 280, 0x68645E,
                                METALIO_APP_FONT_SMALL,
                                &s_app.stats_label) != 0 ||
        s_app.api->add_pet(s_app.launch->host_context, "assets/demo.png",
                           206, 18, 400, 400, &kRig, &s_app.pet) != 0) {
        return 3;
    }

    s_app.api->add_action(s_app.launch->host_context, METALIO_APP_ACTION_WIND,
                          "呼吸", PlayBreathing, nullptr);
    s_app.api->add_action(s_app.launch->host_context, METALIO_APP_ACTION_HEART,
                          "弹跳", PlayBounce, nullptr);
    s_app.api->add_action(s_app.launch->host_context, METALIO_APP_ACTION_REFRESH,
                          "挥手", PlayWave, nullptr);
    s_app.api->add_action(s_app.launch->host_context, METALIO_APP_ACTION_PLAY,
                          "25/30", ToggleCadence, nullptr);
    s_app.api->add_action(s_app.launch->host_context, METALIO_APP_ACTION_INFO,
                          "网格", ToggleDebug, nullptr);
    PlayBreathing(nullptr);
    s_app.api->set_interval(s_app.launch->host_context, 500,
                            RefreshStats, nullptr);
    RefreshStats(nullptr);
    return 0;
}
