---
name: snapping-ambient-grid
description: "Snapping stage 1 (2D ambient grid) shipped 2026-08-22 — what landed, the one tuning knob, and the resequencing advice Ram has not acted on yet"
metadata: 
  node_type: memory
  type: project
  originSessionId: 2b921006-4cb5-413a-ba0a-75fc02cb14e8
  modified: 2026-08-23T09:32:09.555Z
---

Spec: `website/content/software/snapping.md`, §15 stage 1 marked DONE. Code: `Snap.h` / `Snap.cpp`
(`NiceRound125`, `SnapToStep`, `Page2DAmbientStepCU`) plus `Page2DSnappedPointFromInput` in
`RenderPage2D.cpp`. Nine of the ten 2D chokepoint call sites switched; the tenth, the selection
fallback, deliberately stays on the raw mapping (locked decision 14 — a snapped point would
hit-test the wrong object).

Deliberately NOT built despite §4/§14 listing them: `SnapKind`, `SnapPoint`, `SnapContext`, the
aperture table and the settings mask. Nothing at stage 1 referenced them, and the wrapper's optional
`SnapResult*` can be added later without touching the nine call sites.

**The one tuning knob.** `kAmbientStepTargetPixels = 15`. A 1-2-5 ladder can only hit that within
sqrt(2), so the on-screen step lands anywhere in ~10.6-21.2 logical px. Measured in-app it sat at
the bottom of that band (~10 px). If it feels too fine in use, that constant is the only thing to
change — the ladder itself is verified correct across the whole zoom range (0.0001..5000 px/CU,
200000 mm down to the 0.001 mm floor).

**Advice given but not acted on: pull the 3D work plane forward, ahead of stages 5-6.**
`Scene3DPlacementPointFromInput` resolves onto the plane through `camera.target`, so placing the
same object from two camera angles gives two different depths — not merely imprecise, unpredictable,
and arguably the sharpest defect in the app. The minimal fix is a hardcoded horizontal plane at
z=0; the palette/offset UI can wait. Settings persistence (stage 5) and ribbon toggles (stage 6) are
machinery; this is correctness. Ram has not responded to this either way.

**A caveat on §2's "in double".** 3D objects store `XMFLOAT3` (`CYLINDER::p1` etc.) and the
placement function returns `XMFLOAT3`, so 3D snapping is exact-to-the-stored-value, not
double-precision — storage quantises at ~1 mm at 10 km extents. 2D genuinely is double.

**§8's deferred linear scan is reachable today, not hypothetical** — DXF import already ships, so a
100k-record drawing is one file open away. The once-per-frame coalescing is mandatory in stage 1,
not a nicety.

Verification recipe for the optical quantization measurement is in [[ribbon-command-recipe]].
Related: [[properties-pane-plan]] (the pane now reads 2D coordinates, which is how stage 1 got its
numeric readback), [[page2d-transforms]].
