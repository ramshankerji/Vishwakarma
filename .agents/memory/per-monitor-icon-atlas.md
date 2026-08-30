---
name: per-monitor-icon-atlas
description: UI icon atlas is per-monitor (DPI-floored cell size); MSDF text -size 48 -pxrange 6; rebuild rides RestartRenderThreads
metadata: 
  node_type: memory
  type: project
  originSessionId: be7ddf77-8cae-484d-acb3-cf79edcd4338
---

Low-DPI blur fix (done 2026-07-18). UI icons are runtime-rasterised SVGs (lunasvg → RGBA atlas), NOT MSDF — only the NotoSans font is MSDF. Blur cause: a fixed 24px icon atlas minified on coarse monitors because UI lays out in physical mm from `physicalDpiX/Y`.

What changed:
- **Per-monitor icon atlas.** `OneMonitorController` (MemoryManagerGPU-DirectX12.h) gained `uiIconAtlasTexture` + `uiSrvHeap`. English MSDF stays shared in `uiRes.uiAtlasTextures[0]`; each monitor's `uiSrvHeap` has English SRV@0 + its own icon SRV@1. Icon vertices still use `UI_ICON_ATLAS_SLOT`(=1); the per-monitor heap resolves it. Built by `BuildMonitorIconAtlas(uiRes, device, monitorId)` (UserInterface-DirectX12.cpp).
- **De-globalised icon CPU data.** Old globals `iconGlyphLookup`/`gIconAtlasMetadata` → `struct IconAtlasCPU` (in UserInterface.cpp), one per monitor via `gMonitorIconAtlas[MV_MAX_MONITORS]` + accessor `MonitorIconAtlasCPU(int)`. `UIDrawContext.iconData` points at the frame's monitor bundle; `PushIcon`/`PushRoundedRectangle` read it (null-guarded). `BuildIconAtlas(int cellSizePx, IconAtlasCPU& out)` — cell size = `layout.iconSizePx`.
- **Global min-DPI floor** = `UI_MIN_LAYOUT_DPI = 127.0f` (UserInterface.h). `PrecomputeTopRibbonLayout` and `BuildUIOverlay` clamp `monitorDPI = max(dpi,127)` at entry so on coarse monitors the WHOLE ribbon (icons+text+bands) scales up together (4mm→20px at 127). Monitors >127 DPI unaffected.
- **Pixel-snap** in `PushIcon` (std::round x/y/w/h).
- **MSDF text**: msdf_atlas_generator.bat now `-size 48 -pxrange 6` (was 32/4); shader `ShaderUIPixelMSDF.hlsl` keeps factor 8.0 (≈ pxRange·√2 = 8.49, near-ideal for 6). Atlas is a PreBuildEvent, bakes unconditionally → NotoSansMSDF_Compiled.h (now Size=48, 1992²).

Key gotchas:
- **Rebuild point = `RestartRenderThreads()`** (fires on WM_DPICHANGED/DISPLAYCHANGE + startup). It joins all render threads (stop-the-world), so retiring old atlases is easy — BUT the render loop only waits 2 frames back, so a **drain loop** (wait each monitor's `renderFence`→`renderFenceValue`, then Reset atlas+heap) runs right after join, before re-enumerate. Then per monitor: PrecomputeTopRibbonLayout → BuildMonitorIconAtlas. Drain resets ALL current-count slots each time, so no stale atlas survives topology churn.
- Render threads run **concurrently, one per monitor** → per-monitor `uiSrvHeap` avoids descriptor races (don't swap a shared heap slot).
- `RenderUIOverlay` and `RenderPage2D` (both bind the UI srvHeap) now take/use `monitorId` and bind `gpu.screens[monitorId].uiSrvHeap`; both skip the frame if it's null (not built yet).
- Debug dump: `icon_atlas_debug_<monitorId>.bmp` in cwd; startup log prints `Icon atlas built for monitor N (cell X px)`.
- Verified: 2 monitors built cells 25px(157dpi)/26px(163dpi), no crash/GPU-validation over concurrent render. 92-DPI floor→20px not runtime-tested (no such monitor here) but same code path.

Related: [[multi-window-subtabs]] (per-monitor screens/threads), [[properties-pane-plan]].
