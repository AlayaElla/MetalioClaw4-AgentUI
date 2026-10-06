# Metalio external apps (MVP)

Users install external apps without an installer UI: copy `.eapp` packages to
`/metalio/apps` on the SD card and restart the device. During boot, the firmware
validates and extracts each package into its managed directory at
`/metalio/.installed_apps/<app-id>`. Valid apps are added directly to the home
carousel; there is no separate **外部 App** launcher.

## Package layout

`.eapp` is an uncompressed USTAR archive:

```text
manifest.json
elf/esp32p4.elf
assets/...                 # optional App-owned resources
```

Only regular files and directories are accepted. Absolute paths, `..`, links,
the wrong target/API, oversized entries and invalid ELF locations are rejected.
Packaging also rejects unresolved native symbols outside the host's narrow
`sdk/metalio_app_host_imports.def` allowlist. This catches an ABI mismatch before
an App is copied to the SD card.
The extraction directory is firmware-managed and should not be edited by users.

Every package present at boot atomically replaces the extracted copy with the
same app ID, including same-version development builds, through a staging
directory and rollback backup. After activation succeeds, the source `.eapp`
file is deleted automatically. The managed extracted copy
remains installed and continues to appear after later restarts.

## Build the samples

From an exported ESP-IDF PowerShell environment:

```powershell
./external_apps/build-image-viewer.ps1
./external_apps/build-pet-demo.ps1
./external_apps/build-radio.ps1
./external_apps/build-calculator.ps1
./external_apps/build-voice-recorder.ps1
```

Copy `external_apps/dist/image-viewer-1.0.0.eapp` to `/metalio/apps/` on the SD
card and restart. **图片查看器** then appears directly in the home App carousel.
Its five display-sized PNGs are read from the extracted app's own `assets/`
directory through the host API. The viewer keeps one real image widget and
switches its source with the previous/next actions or a horizontal swipe.

The Pet build produces `external_apps/dist/pet-demo-1.0.0.eapp`. Copy it to
the same SD-card folder and restart. **Pet 动画** is installed from the package;
there is no built-in Pet entry in the firmware. The package owns the Q-drop
texture, rig and animation keyframes, while the firmware provides the reusable
LVGL Pet renderer through the host ABI.

The Radio build produces `external_apps/dist/radio-1.0.0.eapp`. **收音机** owns
the redesigned station picker, large current-station display, volume slider and
12-band spectrum. Its editable station list remains in the App-private
`stations.json`; the firmware only provides generic storage, UI controls and a
single-owner HLS media service. When the screen is covered or closed, the host
suspends or releases its audio session and restores the normal system audio
path.

The Calculator build produces `external_apps/dist/calculator-1.0.0.eapp`.
It owns both the standard and scientific keypads, expression parser, history
line and result display. Calculator and Radio read the current AgentUI palette
and subscribe to theme changes, so their backgrounds, surfaces, text, buttons,
spectrum and action controls follow light or dark appearance and the selected
accent color.

## ABI boundary

The Voice Recorder build produces `external_apps/dist/voice-recorder-1.0.0.eapp`.
**录音机** shows live microphone level, elapsed time and received audio size, records
PCM16 WAV to `/sdcard/Recordings`, and plays the latest saved recording through
`media_start`. It follows the `design/agent` layout and theme, with recording,
playback and cancel controls between the timer and waveform. It requires firmware
with recording-file playback support.

**待机功耗** (`standby-power-1.0.0.eapp`) displays live CPU frequency, battery
voltage/current and timed black-screen measurements. Its UI runs in the external
App; the host samples during display suspension and restores the lock screen.
Install it under `/metalio/apps/` on the SD card. See
`examples/standby_power/README.md` and `build-standby-power.ps1`.

The public ABI is `sdk/metalio_app_api.h`. ABI 1 contains a small LVGL-backed
surface plus additive label, bar, timer, action-bar, Pet renderer, media,
haptics and motion functions. Apps must inspect both `struct_size` and the
capability bitmask before calling optional functions. Older ABI-1 apps remain
loadable because new calls are appended to a sized function table.

The general ABI capability groups are:

| Capability | Host API | Intended Apps |
| --- | --- | --- |
| Device info | `get_device_info` | System information |
| Date/time | `get_date_time` | Calendar and clocks |
| Buttons/grid | `add_button`, `add_grid`, `grid_add_button` | Calculator and keypads |
| Drawing/text | `add_rect`, `set_rect_color`, labels | 2048 and local games |
| Swipe | `set_swipe_handler` | 2048 and gesture navigation |
| HTTP | `http_request`, `http_cancel` | Weather and read-only data clients |
| Lists/icons | `add_list`, `list_add_item`, `add_icon` | Weather, settings and catalogs |
| Theme | `get_theme`, `set_theme_callback`, color setters | Light/dark adaptive Apps |
| Motion/haptics | `get_motion_sample`, `play_haptic` | Level and vibration tools |
| Advanced controls | sliders, segments, inertial picker | Radio and Calculator |
| Media | `media_*` | Radio |
| Recording | `recording_*` | Voice recorder |
| Magnetic field | `get_magnetic_sample`, `get_magnetic_sample_ex` | Magnetic instruments |
| Tone synthesis | `synth_start`, `synth_set`, `synth_stop`, `synth_get_state` | Theremin |

HTTP runs asynchronously and delivers its callback on the UI thread. Requests
are limited to GET/POST, 16 KiB request bodies, 64 KiB responses, a 30 second
timeout and two concurrent requests per App. The callback response body is
host-owned and valid only until the callback returns. Outstanding callbacks are
cancelled when the App unloads.

Recording uses the host's processed 16 kHz mono PCM route and writes PCM16 WAV
files under `/sdcard/Recordings`. `recording_start` accepts an optional base
file name and a duration limit; the host sanitizes the name, never overwrites an
existing file, and clamps recordings to 1 second through 10 minutes (5 minutes
by default). Apps poll `recording_get_status` for duration, current peak level,
dropped-frame count, final path and errors. `recording_stop` finalizes the WAV
asynchronously, while `recording_cancel` removes the partial file. Covering or
unloading the App stops and finalizes an active recording. The host owns audio
focus: Radio/Music playback is stopped, wake-word capture is suppressed during
recording, and the latest normal input-route requests are restored afterward.

Motion data comes from the board's SC7A20H three-axis accelerometer. The host
provides raw acceleration in milli-g and filtered board-relative tilt; the
board has no angular-rate gyroscope, so the API deliberately does not report
degrees-per-second values. External apps are native code, not a security sandbox,
so only run apps from sources you trust.

## Power diagnostics (ABI 1)

`METALIO_APP_CAP_POWER_DIAGNOSTICS` gates the appended `get_power_reading`,
`standby_start`, `standby_get_result` and `standby_cancel` calls. Check both the
function-table size and capability before calling them. `standby_start` accepts
0 for manual standby, or 15000/60000 ms for timed sampling and wake. The host
creates its reusable timed worker only when first requested. Results belong to
the requesting App instance; unload invalidates its pending wake and callbacks.
Standby overlay suspension pauses the App UI while the host completes the test.
Other App suspension cancels its scheduled test.

Timed samples retain the normal network grace period. They describe connected
black-screen power, not the later offline standby phase or measured battery life.

## Magnetic instruments (ABI 1)

The instrument extension follows `ai_get_availability` in this exact order:
`get_magnetic_sample_ex`, `synth_start`, `synth_set`, `synth_stop`,
`synth_get_state`. All earlier fields and ABI version 1 are retained. The
manifest and native import allowlist are unchanged. Check both the function
table size and the relevant capability before accessing any optional pointer:

```c
#include <stddef.h>
#define HOST_HAS(api, field) \
    ((api)->struct_size >= offsetof(metalio_app_host_api_t, field) + \
                          sizeof((api)->field))

/* Check get_capabilities's own size before obtaining caps. */
if (HOST_HAS(api, get_capabilities) && api->get_capabilities) {
    uint64_t caps = api->get_capabilities(host);
    if ((caps & METALIO_APP_CAP_AUDIO_SYNTH) &&
        HOST_HAS(api, synth_get_state) && api->synth_start && api->synth_set &&
        api->synth_stop && api->synth_get_state) {
        /* This host supports all four synthesizer calls. */
    }
    if ((caps & METALIO_APP_CAP_MAGNETOMETER) &&
        HOST_HAS(api, get_magnetic_sample_ex) && api->get_magnetic_sample_ex) {
        metalio_app_magnetic_sample_ex_t sample = {0};
        api->get_magnetic_sample_ex(host, &sample);
    }
}
```

QMC6309 discovery uses the board's shared I2C bus (GPIO 7/8), address `0x7C`
and chip ID `0x90`. Only a successful probe enables bit 14. Discovery runs
outside the UI thread; the first sample request starts periodic measurement.
The [manufacturer's Rev C datasheet](https://www.qstcorp.com/upload/pdf/202512/7A7DE8DCC625401FBB333322DD87E567.pdf)
defines the selected CR2 `0x40` / CR1 `0x61` setup as +/-32 G, 200 Hz and
OSR1/OSR2 of 8 in Normal mode. At this range, 1000 LSB/G gives exactly 100 nT per raw count.
The host applies no calibration or software filtering.
Sample calls only copy the latest snapshot, so an initializing sensor returns
success with `valid=0`. `sequence` advances on a new data-ready sample and
`timestamp_ms` records its readout time from `esp_timer`. Overflow is reported
without discarding the saturated field. Read failures invalidate the snapshot;
unavailable hardware returns `-2`, invalid arguments return `-1`.

The legacy microtesla call reads the same snapshot and rounds each axis to the
nearest integer (half values away from zero). Covering or unloading the App
stops measurements and requests sensor suspend. An App returning to the
foreground starts sampling again on its next read.

Axis mapping is identity: App X = chip X (registers 0x01/0x02), App Y = chip Y
(0x03/0x04), App Z = chip Z (0x05/0x06). This preserves the factory driver's
raw sensor axes. The physical rotation relative to screen-right/screen-up/
screen-out and the motion API has not been verified; use vector magnitude for
instrument v1, and verify PCB orientation before relying on screen axes.

The synthesizer advertises bit 20 only when the board has an audio codec.
`synth_set` latches parameters in every state. `synth_start` immediately enters
`STARTING` and the audio worker acquires focus before reporting `RUNNING`;
repeated starts are idempotent. Poll state to detect asynchronous audio errors.
It renders mono PCM16 at 16 kHz in blocks of at most 160 samples, with continuous
oscillator phase, frequency glide, a minimum 5 ms level ramp, vibrato and a
one-pole brightness filter. The App level scales the codec's system volume.

`synth_stop` requests fade using the latest `level_ramp_ms` (at least 5 ms) and
releases focus after the ramp. Assistant interaction, occlusion and unload
also release the synthesizer with an interruption ramp limited to 40 ms; it stays
`IDLE` until the App explicitly starts it again. Media and recording share the
same audio route: starting synthesis stops existing media in the worker.
Calling the App's `media_start`, `media_resume` or `recording_start` while
synthesis is active requests its stop and returns busy (`-2`); retry after
`synth_get_state` reports `IDLE`. Active recording makes `synth_start` return
`METALIO_APP_SYNTH_ERROR_BUSY`.

For on-device acceptance, log a successful QMC6309 `CHIP_ID=0x90` probe, verify
new sample sequences at least 100 times/second, move a magnet through the
overflow threshold, and poll `synth_set` every 20 ms during a 220 to 880 Hz
sweep. Check click-free glides, system volume, all waveforms, assistant
interruption, screen occlusion and unload. Host tests and a firmware build do
not establish speaker latency or physical I2C performance.
