---
name: snapping-ambient-grid
description: "Snapping: stage 1 (2D ambient grid) 2026-08-22, then stages 2-4/6-8 + the Snap2D/Snap3D ribbon groups 2026-08-31 — what landed, the one tuning knob, what is still unbuilt, and the host-runnable tests"
metadata: 
  node_type: memory
  type: project
  originSessionId: 2b921006-4cb5-413a-ba0a-75fc02cb14e8
  modified: 2026-08-31T00:00:00.000Z
---

**Superseded in large part on 2026-08-31**: stages 2-4 and 6-8 landed, plus the closed-form half of
11 and F3/F8 of 13. The doc now carries a **§19 "What is built, and where it deviates"** — read that
first, it is the authoritative status. What follows is still true about stage 1 and about the
advice, and the new section at the bottom records what the big change actually cost.

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

**Advice given, and acted on 2026-08-31.** The 3D work plane came forward, ahead of stage 5.
`Scene3DPlacementPointFromInput` resolves onto the plane through `camera.target`, so placing the
same object from two camera angles gives two different depths — not merely imprecise, unpredictable,
and arguably the sharpest defect in the app. The minimal fix is a hardcoded horizontal plane at
z=0; the palette/offset UI can wait. Settings persistence (stage 5) and ribbon toggles (stage 6) are
machinery; this is correctness. Ram never responded either way, so it shipped as argued: placement
resolves onto `Viewport::workPlane` (default normal Z, offset 0) and the focal-plane math survives
only in `Scene3DFocalPointFromInput`, whose one caller is Zoom Window. Three ribbon buttons move the
plane; the offset text box and the oblique plane are still unbuilt.

**A caveat on §2's "in double".** 3D objects store `XMFLOAT3` (`CYLINDER::p1` etc.) and the
placement function returns `XMFLOAT3`, so 3D snapping is exact-to-the-stored-value, not
double-precision — storage quantises at ~1 mm at 10 km extents. 2D genuinely is double.

**§8's deferred linear scan is reachable today, not hypothetical** — DXF import already ships, so a
100k-record drawing is one file open away. The once-per-frame coalescing is mandatory in stage 1,
not a nicety.

Verification recipe for the optical quantization measurement is in [[ribbon-command-recipe]].
Related: [[properties-pane-plan]] (the pane now reads 2D coordinates, which is how stage 1 got its
numeric readback), [[page2d-transforms]].

---

## The 2026-08-31 change (stages 2-4, 6-8, part of 11, F3/F8)

**Where things live, and why not where §14 said.** `SnapPointsForObject` went into
`DataStorage.cpp` beside `GeometryForObject` / `PlacementForObject`, not into a new `Snap3D.cpp` —
all three switches over `ObjectType` stay in one file, and adding a type means editing them
together. 2D candidate gathering went into `RenderPage2D.cpp`, not `Snap.cpp`, because
`TabCad2DStorage` is declared in `RenderPage2D-DirectX12.h` and gathering there would have dragged a
graphics header into the one module §14 wants free of them. Neither needed a new `.vcxproj` entry,
which mattered: the vcxproj could not be verified from Linux.

**Settings are per DATASETTAB (Ram asked for that explicitly), the work plane is per Viewport.**
TWO masks — `snapMask2D` / `snapMask3D` — not one: an Endpoint is a polyline vertex in a drawing and
a cuboid corner in a model, and MemberEnd has no 2D meaning. Master switch and ortho are separate
atomics, NOT mask bits, so turning the master back on restores the user's kind selection rather than
a default. Nothing is persisted; that is still stage 5.

**The ribbon's first latched controls.** Every `AllUIControls[]` row was momentary. Added
`UIColors::actionLatchedBackground` (ABGR `0xFFE38409` = the `#0984E3` the snap icons are drawn with)
and a latched branch in the draw loop. The per-command meaning lives ONCE, in
`kSnapCommandBindings` (UserInterface.h), read by both `SnapTodoForCommand` (Main.cpp dispatch) and
`SnapControlLatched` (the highlight) — two dozen buttons spelled out twice would have drifted.
Snap3D is placed BEFORE the Views run, which already overflows the right window edge at 1080p.

**Icons are shared between the worlds** via explicit `SVGIconRenderer::IconForID(kIconSnap*)` in the
control rows rather than `UIIconForCommand`, because an End snap looks the same in both. The 25 snap
SVGs were authored in the preceding commit; their ids are named as constants in UserInterface.h.

**`validations/snap/` — three tests that run on Linux, no GPU, no Windows.** This is the reusable
part. (1) `snap_core_test.cpp` compiles `Snap.cpp` and asserts the §5 rule including the case the
naive implementation gets wrong (a NEARER coarse candidate must lose to a farther fine one when both
are inside their own apertures). (2) `snap_2d_geometry_test.cpp` `#include`s the 2D conic geometry
**verbatim** out of `RenderPage2D.cpp` via `extract_2d_geometry.py` — a hand-copied version drifts
and then passes against a stale copy. (3) `check_solid_topology.py` extracts the edge/face tables of
`SnapPointsForObject` and checks them as pure combinatorics. **That third one caught a real defect**:
the cuboid `kFaces` rows were vertex SETS, not cycles — harmless for the centroid they are used for,
a trap for anything later. Same verbatim-extraction technique as the mesh harness in
[[ribbon-command-recipe]], and much cheaper to set up.

**Watch for**: my first test of the §5 rule FAILED for a bad reason — I picked 8 px vs 20 px, but
level 14's aperture is only 11 px, so the far candidate was correctly rejected. Aperture is
`25 - level` px. Check the ladder before writing a case against it.

**Two mistakes worth not repeating.** (i) The ambient fallback read `gatherer.cursorXCU`, which was
only assigned inside the `if (tab.cad2d && container != 0)` block — an empty page rounded every
click onto the origin. (ii) The hover timestamp was `std::chrono::steady_clock` on the engineering
side and would have been `GetTickCount64()` on the render side; `GetTickCount64()` is what this
codebase already shares across those threads (`SelectionState::lastNavInteractionMs`) and both sides
now use it.

**Perf posture.** 3D Stage A is a CPU scan over `storageObjects3D` filtered by
`SubTabDrawsContainer` — correct but O(n); the GPU broad phase is stage 9 and unbuilt. The camera
basis is built ONCE per resolve (`Scene3DBuildProjection`), not per candidate point, which was most
of the cost before. Hover resolution is coalesced to once per input-queue drain in both worlds and
suspended while the camera is being driven, per §8.

**Could not build or run any of it.** The session was a Linux container; the app is Windows/MSVC/
DirectX12 and there is no msbuild, no MSVC, no wine. `freetype` also could not be fetched —
`gitlab.freedesktop.org` returns 403 through the agent proxy — so 9 of 10 submodules are initialized.
Everything above the three host tests is UNVERIFIED against a compiler.
