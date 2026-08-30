---
name: theme-design
description: Light/Dark theme design doc (theme.md) — DESIGNED ONLY; key invariants for implementing it
metadata: 
  node_type: memory
  type: project
  originSessionId: 4b373698-14c4-4e94-b52c-849d06075670
  modified: 2026-07-21T02:12:33.956Z
---

DESIGNED ONLY (2026-07-21): `website/content/software/theme.md`. Nothing implemented.

Single setting in tab 0 Settings view: System(1)/Light(2)/Dark(3), plus two background
overrides (scene3d/page2d, `#RRGGBB` in a new `%LOCALAPPDATA%\Mission Vishwakarma\Settings.txt`
— the app's FIRST settings file, key=value, unknown keys preserved). Both palettes constexpr in
colors.h (`kUIColorsLight/Dark`); `struct UIColors` moves there from UserInterface.h.

Key invariants locked in the doc:
- `uiActiveColors` + new `g_activeBackgrounds` mutate ONLY while render threads are joined;
  theme apply rides [[graphics-refactor-phases]]' RestartRenderThreads (adds a pending-apply
  step that also recreates every window's RTTs — baked optimized clear value must match the
  new Page2D background or fast clear is lost / debug warning).
- Page2D entity visibility: shader-side pure-black↔pure-white swap in the 3 2D vertex shaders,
  driven by `Cad2DViewConstants.padding0` renamed `backgroundIsDark` (32-byte ABI unchanged).
  PrinterController shares those constants → must pin 0 (print always light).
- Everything drawn OVER the canvas (data-tree text, cursor crosshair, entity remap) keys on
  effective background LUMINANCE (Rec.709 < 0.5), not theme name — overrides can invert polarity.
- System mode: registry AppsUseLightTheme + WM_SETTINGCHANGE "ImmersiveColorSet" (guard
  gpu.isGPUEngineInitialized like WM_DISPLAYCHANGE); DWMWA_USE_IMMERSIVE_DARK_MODE title bars
  (first dwmapi.lib use). Splash stays brand; icon atlases need no rebuild (tint-at-draw).
- Phase 1 is behavior-preserving literal migration (~25 hardcoded UI colors → UIColors fields,
  incl. the `TODO: theme-aware` in PushInteractiveRect); Phases 1-3 don't depend on
  [[application-tab-plan]]; the Settings UI (Phase 4) does.
