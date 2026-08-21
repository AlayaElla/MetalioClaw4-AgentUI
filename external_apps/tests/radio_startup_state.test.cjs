const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '..', '..');
const read = (relativePath) =>
  fs.readFileSync(path.join(root, relativePath), 'utf8');

test('radio reads system volume before acquiring the media session', () => {
  const media = read(
    'main/display/agent_ui/apps/external_apps/external_media_service.cc',
  );
  const getter = media.slice(
    media.indexOf('int MediaService::GetVolume'),
    media.indexOf('int MediaService::SetVolume'),
  );

  assert.match(getter, /codec->output_volume\(\)/);
  assert.doesNotMatch(getter, /pending_owner|impl_->owner/);
});

test('radio restores the last successfully playing station by URL', () => {
  const radio = read('external_apps/examples/radio/main/radio.cc');
  const playingTransition = radio.slice(
    radio.indexOf('if (state == METALIO_APP_MEDIA_PLAYING)'),
    radio.indexOf('} else if (state == METALIO_APP_MEDIA_CONNECTING)'),
  );

  assert.match(radio, /kSavedStationPath = "last-station-url\.txt"/);
  assert.match(radio, /void RestoreLastStation\(uint32_t\* selected\)/);
  assert.match(radio, /TextEquals\(s_app\.stations\[index\]\.url, saved_url\)/);
  assert.match(radio, /RestoreLastStation\(&selected\)/);
  assert.match(playingTransition, /SavePlayingStation\(s_app\.playing_station\)/);
  assert.match(radio, /config_write\([^;]+kSavedStationPath/s);
});
