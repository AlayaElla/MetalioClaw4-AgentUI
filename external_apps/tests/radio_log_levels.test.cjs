const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '..', '..');
const media = fs.readFileSync(
  path.join(
    root,
    'main/display/agent_ui/apps/external_apps/external_media_service.cc',
  ),
  'utf8',
);

test('radio HLS routine logs are limited to errors for the media session', () => {
  for (const tag of [
    'ESP_GMF_TASK',
    'ESP_GMF_BLOCK',
    'ESP_GMF_HTTP',
    'HLS_IO',
    'HLS_IO_HELPER',
    'HLS_Playlist',
  ]) {
    assert.match(media, new RegExp(`"${tag}"`));
  }

  const limit = media.slice(
    media.indexOf('void LimitHlsLogsToErrors()'),
    media.indexOf('void RestoreHlsLogLevels()'),
  );
  assert.match(limit, /esp_log_level_get/);
  assert.match(limit, /esp_log_level_set[\s\S]*ESP_LOG_ERROR/);

  const restore = media.slice(
    media.indexOf('void RestoreHlsLogLevels()'),
    media.indexOf('bool Lock('),
  );
  assert.match(restore, /saved_hls_log_levels/);
  assert.match(media, /LimitHlsLogsToErrors\(\);[\s\S]*xTaskCreate\(StartTaskEntry/);
  assert.match(media, /if \(!restart_after_stop\) RestoreHlsLogLevels\(\);/);
});
