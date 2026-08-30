---
name: page2d-transforms
description: "Page2D selection transforms (Copy/Offset/Mirror/Rotate/Move) — architecture, click flows, and 2D geometry conventions discovered while implementing them"
metadata: 
  node_type: memory
  type: project
  originSessionId: 04e98f84-f24b-4bfd-9187-69e14a43f360
---

Implemented 2026-07-07: EDIT_COPY/OFFSET/MIRROR/ROTATE/MOVE for the 2D selection of the active
Page2D. Pattern for future EDIT_* tools (stretch/scale/trim...):

- `Cad2DTransformKind` (MemoryManagerGPU2D.h) + `BEGIN_TRANSFORM2D` ACTION_TYPE (x = kind).
  Chain: Main.cpp ProcessPendingUIActions → PushSystemTodoToTab → विश्वकर्मा.cpp todo loop →
  `Cad2DBeginTransform2D` (requires non-empty `selectedObjectIds`, else no-op). State lives in
  TabCad2DStorage (`transform2DKind/Step/P1/P2` atomics); kind atomic doubles as the render-thread
  cursor-icon flag (UserInterface-DirectX12.cpp Page2D block). One-shot; ESC cancels;
  `ClearLineCreationState` is the cancel-everything helper (add new modal fields there AND in
  `CleanupCad2DTabResources`). HandleZoomWindowInput's self-heal list must include new modes.
- Click flows: Copy/Move 2 clicks (delta p1→p2; Move in place, Copy duplicates), Rotate 3 clicks
  (center, start-dir, end-dir; in place), Mirror 2 clicks (line; creates mirrored copy, AutoCAD
  style), Offset 2 clicks (distance = |p1p2|, 2nd click picks side: left/right of line/polyline,
  outside/inside grows/shrinks closed shapes; creates new object; N/A for Text).
- Copy-thread `Add*` commands are UPSERTS by objectId → in-place edit = re-enqueue same objectId;
  duplicate = zero objectId AND persistedId/persistedParentId (Enqueue assigns fresh memory id,
  save assigns persisted ids; the save path reads the cad2d record vectors directly).
- 2D geometry conventions: arcs render CCW start→end (Shader2D_CurvePixel AngleInCCWSweep) →
  mirror must swap start/end. Polygon vertices at (center + (sin a, cos a)·r), a = rotationDegrees
  + i·step, i.e. param a = 90° − standard polar angle → rotate by θ: rot −= θdeg; mirror across
  line at standard angle φ: rot = 180 − 2φdeg − rot. Text mirror keeps glyphs readable:
  rotationRadians = 2φ − old; text rendered origin is (x+xOffsetCU, y+yOffsetCU) — map that
  effective point, then subtract offsets back. Related: [[ribbon-command-recipe]],
  [[commandline-build]].
- Ellipse/Arc `rotationRadians` added 2026-07-07 (schema v2, proto field rotation_radians; v1
  files decode as 0). Semantics: CCW rotation of the radius axes about the center; arc start/end
  stay WORLD points, sweep angles measured in the rotated local frame (reflection maps local
  param t → −t in the new frame θ' = 2φ−θ, so mirror = swap ends + θ' update). GPU: repurposed
  padding0 of the 64-byte Cad2DCurveGPURecord as float rotationRadians (C++ struct + HLSL struct
  in Shader2D_CurveVertex.hlsl must stay in sync); vertex shader computes rotated-AABB quad
  extents, pixel shader un-rotates into the local frame before radius-normalizing. Proto .pb.cc
  regenerate automatically each build via GenerateDataStorageProtobuf.ps1 (pre-build). DXF
  extension does NOT import ellipses/arcs (skipped), PrinterController reuses the same curve
  PSO/buffers, PropertyPane has no 2D descriptors — none needed changes.
