---
title: "Selection and Picking"
weight: 100108
---
This page is the Design Document for click-selection and picking in both the 3D scene and the 2D CAD page. To be referred by AI for coding as well as humans. It is a plan; sections marked as decisions are locked, defaults are changeable until first implementation.

The 3D world uses **GPU picking** (all render data already lives on the GPU); the 2D world uses **CPU analytic hit-testing** (all 2D records already live on the CPU). Selection highlight color is a **deep blue** override (`~RGB(0.05, 0.15, 0.65)`), chosen because the regular engineering colors are low-contrast.

## Locked design decisions

1. **3D pick technique:** GPU id + depth pass. A tiny offscreen pass renders object-id and depth under the cursor, read back on the CPU. Pixel-accurate; returns both the object and the exact surface point in one shot. ~1 frame readback latency, which is invisible for a discrete click. No literal render pause is required — the scene already renders to an offscreen texture, so the pick is folded into a normal frame.
2. **Highlight:** redraw the selected objects in deep blue with a dedicated PSO after the main scene. The hot scene shader is left untouched. Supports multi-select naturally.
3. **Code ownership:** 3D selection GPU code lives in a new API-specific module `Selection3D-DirectX12.h` / `Selection3D-DirectX12.cpp` (future `Selection3D-Vulkan.*` and `Selection3D-Metal.*` will mirror it). 2D selection is CPU-only and lives in API-agnostic `Selection2D.h` / `Selection2D.cpp`.
4. **Center of selection:** selecting a single object re-centers orbit/zoom on that object's **AABB center**; the mouse wheel re-picks the **nearest surface** under the cursor each gesture, so the user flies around the scene dynamically.

## Prerequisite: populate object AABBs

`GeometryPlacementRecordInPage` already reserves an AABB (`minX..maxZ`), but the copy thread currently builds the record value-initialized and never fills it (only `objectID`, byte offsets, `indexCount`, `matrixIndex` are set). The AABB-center decision depends on these fields, so the first task is to compute the local-space AABB from `geo.vertices` at the record-build site in the copy thread. The vertices are already in hand there, so this is a min/max loop and costs nothing extra.

## Core mechanism — the 3D pick

**Pick ID = `matrixIndex + 1`** (`0` = background / empty space). `matrixIndex` is allocated uniquely per live object through the per-tab free-list, so it is a natural 32-bit pick identity, and it is already available to the vertex shader as the `b1` root constant.

**As built:** rather than a `gluPickMatrix` sub-projection, the pick reuses the visible frame's exact `viewProj` and viewport and simply restricts rasterization to a small scissor box around the cursor, then copies just that box back. This guarantees pixel-for-pixel correspondence with what the user sees, at the cost of scene-viewport-sized pick targets (re-created only when the viewport size changes). The pick targets and readback buffers are the `PickPassContext`, held per tab in `DX12ResourcesPerTab`.

**Pick targets** (created to the scene-viewport size, reused across picks):

```text
RT0  R32_UINT   -> pick id (matrixIndex + 1; 0 = background)
RT1  R32_FLOAT  -> NDC depth (to reconstruct the world surface point)
DSV  D32_FLOAT  -> the pick's own depth buffer, so the nearest surface wins per pixel
```

An N×N scissor (default `kPickBoxSize` = 5) is rasterized around the cursor; that N×N region of RT0/RT1 is copied to two `READBACK` buffers (256-byte-aligned rows). On readback the CPU scans the box for the nearest (smallest-depth) non-zero id — this is where the click tolerance comes from.

**Request / response protocol (per tab / per view):**

```text
1. Logic thread (विश्वकर्मा input loop) sets PickRequest{ pending, x, y, purpose }
   on a plain left-click, and on mouse-wheel.
2. Render thread (GpuRenderThread, after PopulateCommandList, reusing the SAME viewProj
   it just built) sees the request, records the N×N pick pass + CopyTextureRegion into a
   readback buffer in the current command list, and tags it with the frame's fence value.
3. One frame later, when renderFence >= tag, it maps the readback, extracts
   (matrixIndex, ndcDepth), and publishes PickResult back to the tab (atomic / double-buffered).
4. Logic thread consumes PickResult:
   world surface point = inverse(viewProj) · (cursorNDC, ndcDepth).
```

The pick pass must use the exact `viewProj` of the visible frame to be accurate, so it reuses the matrix `PopulateCommandList` already computes for that frame.

## Resolving the pick into an object and a center

**As built:** no separate table is maintained. At readback the render thread scans the current live page snapshot for the object whose `matrixIndex` matches the picked id (selection is rare, so an O(objects) scan on click is fine), reading `objectID` and the local AABB straight from the immutable placement record. The world AABB center = `XMVector3Transform(localAABBcenter, worldMatrix[matrixIndex])`, where `pWorldMatrixDataBegin` holds the same transpose-stored matrix the vertex shader uses, so the transform mirrors the shader exactly. The surface point comes from the hit pixel's NDC + depth via `inverse(viewProj)`.

- **Select (left-click):** if an object resolved, it becomes the (single) selection and the camera pans so its CG is centered (`CenterCameraOnPoint`, preserving view direction and distance). Clicking empty space clears the selection.
- **Scroll re-center:** if geometry was hit, the camera pans so the surface point under the cursor is centered — but only when it lies more than 5 % of the view distance off the current pivot, so a stationary cursor doesn't jitter the view every notch. This is the tunable UX knob; empty space leaves the existing cursor-anchored zoom untouched.

## Highlight pass (deep blue), RCU-safe

A new highlight PSO reuses the scene root signature and vertex shader; its pixel shader outputs a constant deep blue. It runs after the main geometry inside `PopulateCommandList`.

No new lookup structure is needed: iterate the **existing page snapshot**, and for each object whose `objectID` is in the selection set, bind the page's whole vertex/index buffers and issue a `DrawIndexedInstanced` using the same `StartIndexLocation` / `BaseVertexLocation` arithmetic as the indirect path (`(indexByteOffset − indexTail) / 2`, `vertexByteOffset / sizeof(Vertex)`), with the object's `matrixIndex` as the `b1` root constant. The highlight PSO reuses the scene vertex shader and root signature; it tests depth `LESS_EQUAL` with depth-write off so it paints exactly over the selected surfaces. The selection set is copied under a small mutex each frame, so it is immune to page rebuilds.

## Rotation-center cube overlay

A small unit-cube vertex/index buffer (created once) drawn at `camera.target`, with world matrix `translate(target) · scale(k · distance)` so it holds a constant on-screen size. Visibility is driven by a `lastNavInteractionTime` set on orbit / pan / scroll: draw with a fade for ~0.6 s after the last navigation interaction. Rendered after the scene, before the UI. `camera.target` is already the orbit/pan pivot, so no extra state is needed.

## 2D selection (CPU only)

2D records already live on the CPU under `cpuRecordsMutex` in ComputerUnits, so 2D uses **CPU analytic hit-testing** — GPU picking would be overkill:

- On a plain left click (no creation tool active), `Cad2DHandleSelectionClick` maps the click to CU via `Page2DCoordinateFromInput`, then distance-tests against the line / polyline / polygon / circle / ellipse / arc records within a ~6 px tolerance and keeps the nearest object id in `TabCad2DStorage::selectedObjectIds`.
- Highlight rides the **existing spare `flags` field** on the GPU records (`Cad2DLineGPURecord`, `Cad2DCurveGPURecord`). A lightweight `CommandToCopyThread2DType::SelectionRefresh` command forces the copy thread to rebuild and republish the tab's pages; during the rebuild it stamps `kCad2DSelectedFlag` into the GPU records of selected objects (including every segment a polyline/polygon expands into), and the 2D line/curve **vertex** shaders override the color to deep blue (`0xFFA6260D` ABGR) when that bit is set. No new buffer.

**As built:** 2D selection lives in the existing 2D files (`MemoryManagerGPU2D.h/.cpp` for the state, hit-testing and refresh command; `MemoryManagerGPU2D-DirectX12.cpp` for the flag stamping) — there is no separate `Selection2D` module, matching the "2D stays where it is" decision. Page2D will adopt RCU-style immutable pages like the 3D world in the future; until then, selection reads the CPU record vectors directly.

## Module ownership

- `Selection3D-DirectX12.h` / `Selection3D-DirectX12.cpp` — `SelectionState` (selected ids, pick request/result, nav timer), `Selection3DResources` (pick / highlight PSOs + rotation-cube pipeline, held in `DX12ResourcesPerTab`), `PickPassContext` (pick targets + readback + in-flight state, also per tab), and the functions `InitSelection3DResources`, `RecordSelectionOverlays` (highlight + cube), `ServicePick` (pick pass + readback resolve), `FinalizePickFence`, plus cleanup. `SelectionState` is owned per tab on `DATASETTAB` (next to `CameraState`, ready for per-view later). Future `Selection3D-Vulkan.*` / `Selection3D-Metal.*` will provide equivalents behind the same `SelectionState`.
- 3D shaders: `ShaderScenePickVertex.hlsl` / `ShaderScenePickPixel.hlsl` (id + depth), `ShaderSceneHighlightPixel.hlsl` (deep-blue PS, reuses the scene VS), `ShaderCubeVertex.hlsl` / `ShaderCubePixel.hlsl` (rotation cube). All registered as `FxCompile` items.
- 2D selection: in the existing `MemoryManagerGPU2D.*` and `MemoryManagerGPU2D-DirectX12.cpp` (no separate module); the 2D line/curve vertex shaders gained the flag override.
- Small edits to existing files: AABB fill in the copy thread (`MemoryManagerGPU-DirectX12.cpp`); `InitSelection3DResources` / cleanup in the tab lifecycle; left-click + wheel hooks and the `camera.target` update in `विश्वकर्मा.cpp`; the overlay + pick calls (and pick-fence finalize) in `GpuRenderThread`.

## Staged implementation (each stage independently verifiable)

```text
1. AABB population        -> records show non-zero min/max for a known cube. (no visible change)
2. Pick pass + readback   -> click logs the correct objectID; clicking empty logs 0.
3. Highlight redraw       -> clicked object turns deep blue; clicking empty deselects.
4. Select re-centers      -> orbit now pivots on the selected object's AABB center.
5. Scroll re-picks surface-> camera.target follows the nearest surface under the cursor; fly-around works.
6. Rotation cube          -> appears during navigation, fades when idle.
7. 2D selection           -> CPU hit-test + flags-bit deep-blue highlight.
```

## Defaults (as built; tunable)

- **Single-select**; the set already supports multi-select via Ctrl+click later.
- **Plain left-click** selects (Alt+Left and Middle stay orbit/pan); clicking empty space **deselects**.
- Pick box **`kPickBoxSize` = 5×5**; rotation-cube visible **`kNavCubeVisibleMs` = 700 ms** with a 250 ms fade; cube on-screen size **`kNavCubePixels` ≈ 26 px** (warm orange, distinct from selection blue); deep blue ≈ `RGB(0.05, 0.15, 0.65)` (`0xFFA6260D` ABGR for 2D). Scroll re-center threshold **5 %** of view distance.

## Known limitations / follow-ups

- Selection state and the pick context are per **tab**; showing the same tab in two windows on two monitors would let two render threads touch one `PickPassContext` (a pre-existing per-tab-resource limitation, not selection-specific).
- Scroll re-center currently **pans** the surface point to the orbit center (translation, no rotation). Decoupling the orbit pivot from the look-at target (so the pivot can move with zero visible motion) is a future camera change.
- 2D highlight re-uploads the tab's pages on each selection change; fine at MVP sizes, worth revisiting once Page2D adopts RCU pages.
- Text selection (2D) is not implemented (text vertices carry no `flags`).
