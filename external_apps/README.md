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
degrees-per-second values. It also has no magnetometer: the magnetic sample API
is present for source compatibility with future boards, but this host does not
advertise `METALIO_APP_CAP_MAGNETOMETER` and returns unsupported. External apps
are native code, not a security sandbox, so only run apps from sources you
trust.
