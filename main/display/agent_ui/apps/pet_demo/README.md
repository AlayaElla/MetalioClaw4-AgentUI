# Pet source assets

The Pet demo is now the external package in `external_apps/examples/pet_demo`.
Firmware does not register or compile a built-in Pet screen. This directory
only retains the generated Q-drop texture sources and preview PNG used by the
external package build.

- Character source: `DanmukuPet/UnityProject/Assets/AssetArt/Generate/Q版小水滴`
- Body/anchors: `step2_body.png` and `step2_body_config.json`
- Neutral face reference: `step1_character.png`
- Firmware texture: generated RGB565A8, 256 x 313, 240384 bytes
- Runtime surface: 400 x 400 ARGB8888 in 128-byte-aligned PSRAM
- Rig: 5 x 5 body grid and four cubic-Bezier limbs

Regenerate the checked-in texture without modifying the Unity project:

```powershell
rtk python scripts/generate_pet_demo_asset.py `
  --source-dir "D:\GameProject\DanmukuPet\UnityProject\Assets\AssetArt\Generate\Q版小水滴" `
  --output-dir "main\display\agent_ui\apps\pet_demo\assets" `
  --width 256
```

The external app exposes breathing, bounce, and wave loops. Its mesh toggle
overlays deformed grid edges and limb anchor vertices. The 25/30 FPS control
provides a reversible cadence A/B, while the stats show actual FPS and raster
timing. A successful firmware or package build is not an FPS result.
