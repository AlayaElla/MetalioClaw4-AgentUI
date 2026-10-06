# Agent UI firmware architecture

The firmware UI is organized as one runtime entry point, shared layers, and
vertical app slices. The directory map is:

```text
display/agent_ui/
├── agent_ui_runtime.*   runtime registration and global lifecycle
├── core/                 navigation, shell, theme, fonts, and device-wide UI
├── components/           reusable LVGL components and expression playback
├── apps/<app>/           app-owned contract, state, view, and integrations
└── sources.cmake         aggregate source and include-directory manifest
```

Each UI app is a vertical slice under `apps/<app>/`:

- `*_contract.h`: intents, events, commands, and shared app types;
- `*_view_state.h`: complete render state with no LVGL or hardware handles;
- `*_controller.*`: state transitions and intent/event handling;
- `*_adapter.*`: access to existing business and hardware services;
- `*_view.*`: LVGL creation, rendering, and intent emission;
- `*_module.*`: wiring between the controller, adapter, view, and navigation.

The Network slice is embedded in Settings rather than registered as a
standalone navigation screen:

```text
apps/network/
├── network_contract.h       intents, events, and commands
├── network_view_state.h     renderable network state
├── network_controller.*     state transitions and user intent handling
├── network_adapter.*        Wi-Fi, cellular, and storage service access
├── network_module.*         Settings integration boundary
├── network_view.*           LVGL composition and rendering
└── network_*_ui.*           reusable settings panels and dialogs
```

`network_view.*` and the `network_*_ui.*` helpers only render state and emit
callbacks. Hardware, storage, network, and modem operations belong in the
adapter and are coordinated by the controller and module.

The Bluetooth slice is also embedded in Settings and keeps the external audio
module behind an explicit boundary:

```text
apps/bluetooth/
├── bluetooth_contract.h       intents, commands, events, and device types
├── bluetooth_view_state.h     renderable connection and scan state
├── bluetooth_controller.*     intent and lifecycle coordination
├── bluetooth_adapter.*        UART protocol, NVS, audio route, and tasks
├── bluetooth_view.*           LVGL rendering and intent emission
├── bluetooth_settings_ui.*    Bluetooth settings controls
└── bluetooth_module.*         lifecycle and dependency wiring
```

Mode changes follow the audio module protocol sequence and are completed by
the matching `SET MODE` response. Mode 2 accepts both a device selected from
the scan results and a reconnect reported by the module. Other uncorrelated
success responses do not change the current route. Once SLC confirms the
control link, the app selects A2DP music mode by default for ordinary Bluetooth
speakers. Call mode remains an explicit option that requests the SCO path for
compatible headset playback and microphone input. `SETUP SCO` confirms the
call audio path.
A device-level disconnect returns the module to Mode 1, restores the local
audio route, and resumes wake-word processing after the local I2S
acknowledgement.

The Camera slice is a top-level app with a complete vertical boundary:

```text
apps/camera/
├── camera_contract.h              intents, events, and commands
├── camera_view_state.h            renderable camera state
├── camera_controller.*            state transitions and intent handling
├── camera_adapter.*               command execution and service integration
├── camera_capture_backend.*       sensor capture and preview frame delivery
├── camera_gallery_repository.*    photo storage, metadata, and gallery entries
├── camera_image_codec.*           RGB565 and JPEG conversion
├── camera_image_decoder.*         asynchronous JPEG and viewer decoding
├── camera_view.*                  LVGL composition and user intent emission
└── camera_module.*                lifecycle and dependency wiring
```

`camera_view.*` owns LVGL objects, review and gallery layout, and draw-post
acknowledgements. Capture hardware, preview buffers, storage, JPEG encoding,
and asynchronous decoding stay behind the backend, repository, codec, decoder,
and adapter boundaries. Camera runtime registration uses `camera::Module`; the
runtime does not construct `CameraView` directly.

The Camera gallery lists the newest 48 regular `.jpg`/`.jpeg` files on the SD
card (case-insensitive extension matching). Older entries are intentionally
omitted to bound LVGL card and thumbnail memory; a missing older file is not a
decode failure.

Dependencies must flow in these directions:

```text
View -> shared components -> design tokens
Controller -> contract <- Adapter
Module -> View + Controller + Adapter
```

Controllers must not include LVGL. Adapters must not retain View or LVGL
pointers. Views must not access `Application`, NVS, network, storage, camera,
AT commands, or protocol clients directly. App-specific rendering helpers stay
under their app directory; only behavior that has no app ownership belongs in
`components/`.

Files uses a Controller/Adapter/Module boundary. The existing `FilesView::Create` and
lifecycle entry points remain compatibility facades; the Controller owns
directory and preview policy, the Adapter owns storage/USB/worker access, and
the Module wires their lifecycle. Shared capacity tickets can be consumed by
their owner across Files suspend and unload.

The shared StatusBar keeps LVGL widgets, animation, and asset selection in
`core/status_bar.*`. `status_bar_data_provider.*` gathers a value snapshot and
preserves the network dirty-cache and battery callback cadence;
`status_bar_state.*` returns semantic icon and visibility/battery policy values
without owning LVGL objects or theme assets. The system data source reads the
existing AI, Bluetooth, time, activation, network, and battery services.

`CodexCaptureCoordinator` coordinates voice capture and realtime audio-uplink
lifecycle through an Application-owned service port. It does not extract the
whole Codex screen or every protocol path. `apps/codex/codex_protocol_service.*`
owns Codex command admission, transport context checks, and value-event delivery;
the View still owns Codex-specific presentation. Its `on_sent` callback runs on
the transport worker only after a complete local WebSocket write succeeds, so
callers must keep it thread-safe and free of LVGL work. Send-failure UI events
are retried through the main loop up to three times; exhaustion is logged.
Application owns Xiaozhi protocol integration,
activation, and special interaction services.

## Runtime integration

`agent_ui::Runtime` owns top-level app registration and lifecycle dispatch. The
Home slice uses the complete Controller, Adapter, Module, and View boundary.
Its View composes `agent_ui::home::Renderer`, which only draws LVGL state and
emits intents; it does not call navigation or `Application` directly. Network
is owned by Settings and is consumed through the Network module's
`BuildInto`, `ResetUi`, and `LifecycleCallback` interface instead of a
`ScreenId` registration. Bluetooth uses the same embedded Module boundary.
Camera is registered through its Module mount factory
and receives lifecycle events through the same runtime boundary.

Home presentation operations from the status bar, idle power, Application, and
Standby are routed through Runtime and Home Module/View. Core services receive
runtime-owned callbacks rather than including the Home renderer. Home's tick
sound follows the existing intent/command/adapter path and keeps the device's
speaking-state guard. Battery sampling intervals and power state transitions
remain owned by the existing services.

Each top-level app is registered by `agent_ui::Runtime`; embedded apps are
owned by their host module. Every app keeps its source manifest in
`apps/<app>/sources.cmake`. A View-only app may remain a View-only slice until
its business behavior needs a Controller or Adapter; it must still obey the
same dependency directions. Add new sources to the app manifest and include
that manifest from `display/agent_ui/sources.cmake`.

## Shared render resources

`components/render_snapshot_buffer.*` owns reusable snapshot memory, the stable
LVGL descriptor, image-cache invalidation, and acceleration registration. Home,
the Codex menu, and the keyboard keep their own capture timing, source-state
checks, asynchronous work, native-widget fallback, and statistics. The owner is
noncopyable and nonmovable because acceleration registration uses descriptor
identity. Home replaces snapshots using an unregistered candidate and `Adopt`.
Release unregisters the buffer before freeing it. Use these operations on the
LVGL owner thread or under the existing display lock.

`components/opaque_render_acceleration.*` owns RGB565/RGB888 PPA SRM copies,
registration, bounds/cache checks, and copy telemetry. The public functions in
`expression_acceleration.h` remain the compatibility facade;
`expression_acceleration.cc` retains A8 blending and its dispatch policy. Every
unsupported or failed copy continues through the software rendering path.

`display/display_render_telemetry.*` owns refresh/dirty-area/flush accounting.
The display adapter selects the mode and its log name together, then attaches
the collector once. LVGL callbacks retain the collector's address, so its owner
must outlive callback delivery. The existing adapter FPS statistics are retained
because they report different information.

## Source integration

- Edit `expression-spec.json` in the Demo and regenerate
  `components/expression_spec.generated.h` when expression timing changes.
- Keep hardware, storage, networking, and protocol work in app integrations or
  the existing device services. Shared widget policy must remain app-independent;
  platform render helpers may use ESP-specific acceleration behind explicit
  capability checks and software fallback.
- Register firmware sources in the owning app manifest and build from the
  repository root with `npm run package`.

The module guide, resource contracts, and development commands are described in
[`docs/esp32-ui-engineering.md`](../../../docs/esp32-ui-engineering.md).
