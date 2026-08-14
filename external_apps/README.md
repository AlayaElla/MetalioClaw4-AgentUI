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
assets/demo.png
```

Only regular files and directories are accepted. Absolute paths, `..`, links,
the wrong target/API, oversized entries and invalid ELF locations are rejected.
The extraction directory is firmware-managed and should not be edited by users.

If a package has the same app ID and version as the extracted copy, boot keeps
the current copy. A package with the same ID and a different version replaces it
through a staging directory and a rollback backup. After either case succeeds,
the source `.eapp` file is deleted automatically. The managed extracted copy
remains installed and continues to appear after later restarts.

## Build the samples

From an exported ESP-IDF PowerShell environment:

```powershell
./external_apps/build-image-viewer.ps1
./external_apps/build-pet-demo.ps1
```

Copy `external_apps/dist/image-viewer-0.1.0.eapp` to `/metalio/apps/` on the SD
card and restart. **图片查看器** then appears directly in the home App carousel.
Its PNG is read from the extracted app's own `assets/demo.png` through the host
API.

The Pet build produces `external_apps/dist/pet-demo-0.1.0.eapp`. Copy it to
the same SD-card folder and restart. **Pet 动画** is installed from the package;
there is no built-in Pet entry in the firmware. The package owns the Q-drop
texture, rig and animation keyframes, while the firmware provides the reusable
LVGL Pet renderer through the host ABI.

## ABI boundary

The public ABI is `sdk/metalio_app_api.h`. ABI 1 contains a small LVGL-backed
surface plus additive label, timer, action-bar and Pet renderer functions.
Older ABI-1 apps remain loadable because calls are supplied through a sized
function table. External apps are native code, not a security sandbox, so only
run apps from sources you trust.
