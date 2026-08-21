const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '..', '..');

test('external sliders reject the false minimum sample emitted on touch release', () => {
  const runtime = fs.readFileSync(
    path.join(
      root,
      'main/display/agent_ui/apps/external_apps/external_app_runtime.cc',
    ),
    'utf8',
  );

  assert.match(runtime, /void GuardExternalSliderRelease\(lv_event_t\* event\)/);
  assert.match(runtime, /lv_indev_get_state\(indev\) == LV_INDEV_STATE_RELEASED/);
  assert.match(runtime, /slider->last_pressed_value/);
  assert.match(runtime, /LV_EVENT_PRESS_LOST, slider\.get\(\)/);
  assert.match(
    runtime,
    /slider == nullptr \|\| slider->updating \|\| slider->restoring/,
  );
});
