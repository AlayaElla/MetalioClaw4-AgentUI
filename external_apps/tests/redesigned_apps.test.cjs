const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '..', '..');
const read = (relative) => fs.readFileSync(path.join(root, relative), 'utf8');

test('all external app manifests and package names use version 1.0.0', () => {
  const apps = [
    ['image_viewer', 'image-viewer', 'build-image-viewer.ps1'],
    ['pet_demo', 'pet-demo', 'build-pet-demo.ps1'],
    ['radio', 'radio', 'build-radio.ps1'],
    ['calculator', 'calculator', 'build-calculator.ps1'],
  ];
  for (const [directory, packageName, builder] of apps) {
    const manifest = JSON.parse(
      read(`external_apps/examples/${directory}/manifest.json`),
    );
    assert.equal(manifest.version, '1.0.0', directory);
    assert.match(
      read(`external_apps/${builder}`),
      new RegExp(`${packageName}-1\\.0\\.0\\.eapp`),
      builder,
    );
  }
});

test('radio package uses the redesigned name, version and editable station config', () => {
  const manifest = JSON.parse(read('external_apps/examples/radio/manifest.json'));
  const source = read('external_apps/examples/radio/main/radio.cc');

  assert.equal(manifest.name, '收音机');
  assert.equal(manifest.version, '1.0.0');
  assert.match(source, /config_read\([^;]+"stations\.json"/s);
  assert.match(source, /kMaximumStations = 48/);
  assert.match(source, /add_action_picker/);
  assert.match(source, /add_slider/);
  assert.match(source, /METALIO_APP_MEDIA_SPECTRUM_BANDS/);
  assert.doesNotMatch(source, /std::(?:snprintf|strncmp|strcmp|strlen)/);
  assert.doesNotMatch(source, /add_action\(/);
  assert.doesNotMatch(source, /"LIVE"|"HLS"|METALIO_APP_ACTION_(PREVIOUS|PLAY|PAUSE|NEXT)/);
});

test('calculator owns standard and scientific layouts behind one expression engine', () => {
  const manifest = JSON.parse(read('external_apps/examples/calculator/manifest.json'));
  const source = read('external_apps/examples/calculator/main/calculator.cc');

  assert.equal(manifest.name, '计算器');
  assert.equal(manifest.version, '1.0.0');
  assert.match(source, /kStandardKeys/);
  assert.match(source, /kScientificKeys/);
  assert.match(source, /class ScientificParser/);
  assert.match(source, /"sin"/);
  assert.match(source, /"reciprocal"/);
  assert.match(source, /"mod"/);
  assert.match(source, /add_action_segment/);
  assert.match(source, /history_label/);
  assert.doesNotMatch(source, /"CALCULATOR"|"LOCAL LOGIC"|"EXTERNAL APP"|"READY"/);
});

test('host ABI exposes reusable controls and an inertial picker', () => {
  const api = read('external_apps/sdk/metalio_app_api.h');
  const runtime = read('main/display/agent_ui/apps/external_apps/external_app_runtime.cc');

  for (const call of ['add_slider', 'add_action_segment', 'add_action_picker']) {
    assert.match(api, new RegExp(`\\(\\*${call}\\)`));
  }
  assert.match(api, /METALIO_APP_CAP_UI_CONTROLS/);
  assert.match(runtime, /kPickerMaxVelocity = 3\.1f/);
  assert.match(runtime, /kPickerFriction = 0\.95f/);
  assert.match(runtime, /kPickerSnapDurationMs = 240\.0f/);
  assert.match(runtime, /LV_EVENT_PRESSING/);
  assert.match(runtime, /LV_EVENT_RELEASED/);
});

test('boot installation reports real progress without blocking the UI thread', () => {
  const manager = read(
    'main/display/agent_ui/apps/external_apps/external_app_manager.cc',
  );
  const runtime = read('main/display/agent_ui/agent_ui_runtime.cc');
  const boot = read('main/display/agent_ui/apps/boot/boot_view.cc');

  assert.match(manager, /kPreferredCopyBufferBytes = 32 \* 1024/);
  assert.match(manager, /EmitInstallProgress/);
  assert.match(manager, /bytes_completed/);
  assert.doesNotMatch(manager, /installed\.version == candidate\.version/);
  assert.match(runtime, /xTaskCreate\(StartTask/);
  assert.match(runtime, /BootView::SetInstallProgress/);
  assert.match(boot, /正在安装 App/);
});

test('external app launch paints status before running ELF and decoding assets', () => {
  const view = read(
    'main/display/agent_ui/apps/external_apps/external_apps_view.cc',
  );
  const create = view.slice(view.indexOf('lv_obj_t* HostView::Create()'));

  assert.match(view, /正在启动 /);
  assert.match(view, /正在载入界面/);
  assert.match(view, /StartExternalAppAfterFirstFrame/);
  assert.match(view, /LV_EVENT_SCREEN_LOADED/);
  assert.match(view, /lv_timer_create\(LaunchExternalApp, kLaunchDelayMs/);
  assert.match(view, /lv_obj_move_foreground\(state->launch_overlay\)/);
  assert.doesNotMatch(create, /Runtime::Get\(\)\.Launch/);
});

test('image viewer switches one real image widget with actions and swipes', () => {
  const api = read('external_apps/sdk/metalio_app_api.h');
  const runtime = read(
    'main/display/agent_ui/apps/external_apps/external_app_runtime.cc',
  );
  const viewer = read(
    'external_apps/examples/image_viewer/main/image_viewer.c',
  );

  assert.match(api, /\(\*add_image_ex\)/);
  assert.match(api, /\(\*set_image_source\)/);
  assert.match(runtime, /lv_image_set_src\(found->object, nullptr\)/);
  assert.match(viewer, /set_image_source/);
  assert.match(viewer, /METALIO_APP_ACTION_PREVIOUS/);
  assert.match(viewer, /METALIO_APP_ACTION_NEXT/);
  assert.match(viewer, /set_swipe_handler/);
});

test('radio switching paints connecting state before non-blocking media work', () => {
  const radio = read('external_apps/examples/radio/main/radio.cc');
  const media = read('main/display/agent_ui/apps/external_apps/external_media_service.cc');

  const queueTune = radio.slice(
    radio.indexOf('void QueueTune('),
    radio.indexOf('void OnStationSelected('),
  );
  assert.ok(queueTune.indexOf('tune_pending = true') <
            queueTune.indexOf('SetStationText("接收中..."'));
  assert.doesNotMatch(queueTune, /media_start/);
  const refresh = radio.slice(
    radio.indexOf('void Refresh('),
    radio.indexOf('void ApplyTheme('),
  );
  assert.match(refresh, /if \(s_app\.tune_pending\)/);
  assert.match(refresh, /media_start/);

  assert.match(media, /TaskHandle_t player_stop_task = nullptr/);
  assert.match(media, /xTaskCreateWithCaps\(\s*PlayerStopTask,[\s\S]*MALLOC_CAP_SPIRAM/);
  assert.match(media, /xTaskNotifyGive\(player_stop_task\)/);
  const requestStop = media.slice(
    media.indexOf('void RequestPlayerStop('),
    media.indexOf('static void PlayTaskEntry('),
  );
  assert.doesNotMatch(requestStop, /esp_audio_simple_player_stop/);
  assert.doesNotMatch(requestStop, /xTaskCreate/);
});
