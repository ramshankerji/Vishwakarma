---
title: "Snapping (2D and 3D)"
weight: 100110
---
This page is the Design Document for snapping in the 2D CAD page and the 3D modelling scene. To be referred by AI for coding as well as humans. It is a plan; sections marked as decisions are locked, defaults are changeable until first implementation. Nothing here is implemented yet.

Snapping is the mechanism that converts an imprecise cursor pixel into an **exact** engineering coordinate. It is the difference between a drawing that looks right and a model that *is* right. Selection (`selection.md`) answers "which object is under the cursor"; snapping answers "which exact point did the user mean". They share machinery but are different questions and must not be conflated.

## 1. Vocabulary

| Term | Meaning |
|---|---|
| **Snap kind** | The rule that produced a point: End, Mid, Center, Grid, Perpendicular … |
| **Priority** | 0–15, exported per snap point by the object. 0 is coarsest, 15 finest. |
| **Aperture** | Screen radius in logical pixels within which a snap of a given priority is eligible. |
| **Anchor** | The previous point of the running command (line start, move base point). Required by all *relative* snaps. |
| **Ambient snap** | The always-on fallback: rounds the raw pixel to a zoom-appropriate grid increment. Never fails. |
| **Work plane** | The 3D plane on which a screen ray is resolved when nothing is hit. |

## 2. Locked design decisions

1. **The GPU answers "which object", the CPU answers "which coordinate".** In 3D the pick pass returns `matrixIndex` and a float32 NDC depth. With `nearZ = 0.1` / `farZ = 1000`, reconstructing a world point from that depth carries millimetre-to-centimetre error at plant scale — unusable as an engineering coordinate. So the GPU pick is used **only to narrow the candidate object set**; the snapped coordinate is always recomputed on the CPU, in `double`, from the object's own engineering parameters. This is the single most important decision on this page.
2. **The latency is hidden, not fought.** The GPU pick has ~1 frame of readback latency. That is fine because the *candidate set* changes slowly as the mouse moves, while the *snap point* is recomputed from that set at full input rate on the CPU. A stale candidate set costs nothing visible; a stale coordinate would be a bug.
3. **Two stages, symmetric in 2D and 3D.** Stage A = broad phase (which objects are near the cursor). Stage B = narrow phase (exact analytic snap points from those objects, scored by priority). Only Stage A differs: GPU pick in 3D, CPU pre-filter in 2D — matching the existing selection split.
4. **Snap resolution runs on the engineering thread**, at the existing input-loop chokepoints, and publishes a hover result for the render thread under the same lock-free, one-frame-stale contract as `CameraState`. No new mutex between engineering and render threads.
5. **Ambient snap is always on and always succeeds.** Every click therefore lands on a defined value, never on 16 digits of float noise. It is priority 0 with an unbounded aperture, so it is the guaranteed fallback of the resolution rule.
6. **Priority 0 is the coarsest level, 15 the finest.** 0 is the ambient grid (widest aperture, always eligible); 15 is a member end or a piping connection point (narrowest aperture, wins outright when in range). Aperture decreases monotonically as priority rises.
7. **Objects export their own snap points.** A single dispatch `SnapPointsForObject(objectType, object, out)` mirrors the existing `GeometryForObject(...)`. Adding a new intelligent object means adding one case there — no changes to the snap engine.
8. **The 3D work plane is an explicit axis-aligned plane, defaulting to horizontal.** The plane is named by its normal axis and an offset along that normal; the default is normal Z, i.e. the horizontal plane. **The view/focal plane is discarded** — a plane that rotates with the camera is not a construction reference. An oblique plane picked off a future object surface is planned but not in the first implementation.
9. **The work plane belongs to the view (sub-tab slot), not to the Scene3D, and is not persisted.** A view will eventually be able to load *several* Scene3D containers at once, so the plane cannot belong to any one of them. It is working context like the camera — which already lives per view in `InternalSubTab` — and it is rebuilt from the default on every open.
10. **Right-click opens the snap palette** in both worlds. It is not AutoCAD's "repeat last command"; **Space** carries that role instead.
11. **Ortho ships in both idioms.** A persistent CAD-style toggle on **F8**, and Blender-style per-operation axis constraints on **X / Y / Z**. They are two front-ends onto the same direction-constraint mechanism.
12. **Snap-from-object is never implicit.** It is entered only by explicitly choosing it from the right-click palette. With it off — the normal case — resolution is always the priority-then-distance rule of §5 across every candidate.
13. **Snapping never silently moves an existing object.** It only ever affects the *point being entered*. Editing an existing coordinate through the properties pane bypasses snapping entirely.

## 3. What we build on (the existing chokepoints)

The codebase already funnels every "user clicked somewhere in the world" event through exactly two functions. This is what makes snapping a contained change rather than a rewrite.

| World | Function | Today | With snapping |
|---|---|---|---|
| 2D | `Page2DCoordinateFromInput(tab, input, outX, outY)` in `RenderPage2D.cpp` | maps pixel → ComputerUnit in `double` | resolves the snap and returns the snapped point + the fired snap record |
| 3D | `Scene3DPlacementPointFromInput(tab, input, outPoint)` in `विश्वकर्मा.cpp` | ray-casts onto the focal plane through `camera.target` | resolves against object snap points first, falls back to the work plane |

All eleven 2D creation/transform call sites already follow the pattern `if (Page2DCoordinateFromInput(tab, input, x, y)) { Handle…Click(tab, x, y); }`, so line/polyline/polygon/circle/ellipse/arc/text creation, asset insert, and all five `Cad2DTransformKind` tools inherit snapping at once. Likewise 3D primitive placement inherits it through the single call in `HandlePrimitive3DPlacementInput`.

Other reusable pieces:

- **2D records** live in `TabCad2DStorage` in `double`, already the exact engineering values — the ideal snap source.
- **3D engineering objects** live in `tab.storageObjects3D` (`META_DATA*` + `ObjectType`) and the engineering thread is their sole writer, so it may iterate them without a lock. Their defining parameters (`CYLINDER::p1/p2`, `LINE_MEMBER::point1/point2`, `CUBOID::vertices` …) are exactly the points engineers want to snap to.
- **The pick pass** (`ServicePick` / `PickPassContext`, `kPickBoxSize = 5`) already rasterizes an N×N box and scans it. Enlarging the box and returning the *set* of distinct ids instead of the nearest one is a small, local change.
- **`GeometryPlacementRecordInPage`** already carries a per-object AABB and `matrixIndex`, giving a cheap CPU cross-check without touching the GPU.
- **Cursor-trailing icons** in `BuildUIOverlay` (`UserInterface.cpp`) are the existing pattern for drawing a screen-space glyph at the cursor from tab state — the snap marker reuses it.
- **SVG icon embedding** already exists: an icon authored into `website/static/SVGIcons/` and listed in `SVGIconManifest.h` is compiled into the binary by the pre-build `svg_icon_embedder.py`. Every snap icon is new artwork, but the pipeline to ship it is already there.

## 4. The snap record and result

```cpp
enum class SnapKind : uint8_t {   // Also the marker glyph selector.
    None = 0, AmbientGrid, GridObject, Ortho, End, Mid, Center, Quadrant,
    Perpendicular, Parallel, Tangent, Intersection, Nearest, Insertion,
    TextBounds, EdgeMid, FaceCenter, MemberEnd, MemberMid, ObjectDefined
};

struct SnapPoint {              // What an object exports. Packed, cheap to generate.
    double   x, y, z;           // z unused in 2D.
    uint64_t objectId;
    SnapKind kind;
    uint8_t  priority;          // 0..15
};

struct SnapResult {
    bool     hit;               // false only when every snap incl. ambient is disabled.
    SnapKind kind;
    uint8_t  priority;
    double   x, y, z;
    uint64_t objectId;          // 0 for ambient grid / ortho-only results.
    uint64_t secondObjectId;    // Intersection: the other participant. 0 otherwise.
};
```

`secondObjectId` exists because an intersection is inferred from *two* objects and the caller (and the marker tooltip) needs both.

## 5. The resolution rule

This is the part of the specification that needs stating precisely, because the obvious implementation is wrong.

The rule is **descending priority, first level with a hit wins** — *not* global nearest.

```text
for level = 15 down to 0:
    if level is disabled in the effective snap mask: continue
    collect candidates of that level within aperturePx[level]
    if any: return the one nearest the cursor
return ambient grid   // level 0, unbounded aperture, always succeeds
```

Aperture **increases** as priority decreases: fine snaps must be approached closely, coarse snaps catch you from far away, and the ambient grid catches everything. With this ordering the radius table is coherent. With a naive "global nearest inside its own radius" comparison it is not — a grid point 20 px away would beat an endpoint 10 px away, and the finest snaps would become unreachable exactly when the drawing is dense. Evaluate priority-first; use distance only to break ties *within* a level.

Consequences worth noting:

- Priority is a property of the **point**, not of the object, so one object can export a priority-14 endpoint and a priority-6 nearest-on-curve point.
- Apertures are **logical pixels scaled by the monitor's DPI**. On a 4K panel a fixed device-pixel aperture feels half the size; the DPI plumbing already exists per monitor.

**Priority → aperture is linear for now.** The right curve can only be found by trial and error on real drawings, so the first implementation uses the simplest mapping that satisfies the monotonic requirement, expressed as two constants rather than a hand-tuned sixteen-entry table:

```text
aperturePx[0]     = unbounded            // ambient grid: the guaranteed fallback
aperturePx[level] = kApertureMaxPx - (level - 1) * (kApertureMaxPx - kApertureMinPx) / 14
                                         // level 1..15, i.e. 24 px down to 10 px at 1 px per level
```

Keep this as a computed function, not a literal table, until measurement justifies otherwise — then the table can replace it without touching any caller.

## 6. Snap kinds — Phase 1

The set implemented by this design. Each is one bit in the settings mask.

**Always on**

- **Ambient Grid** — rounds the raw cursor point to a zoom-derived decimal increment, so every click lands on an exact value rather than on float noise. Priority 0, unbounded aperture, cannot be switched off.

**Point snaps on geometry (both worlds)**

- **End** — the endpoints of a line, of each polyline segment, of an arc's sweep, and each polygon corner. The most-used snap in drafting; priority 14.
- **Mid** — the midpoint of any bounded segment or arc sweep, and each polygon edge midpoint.
- **Center** — the center of a circle, arc, ellipse or polygon, and the body center of a 3D solid.
- **Quadrant** — the 0°/90°/180°/270° points of a circle, arc or ellipse, measured in the curve's own rotated frame. Unavoidable in vessel, flange and piping drawings.
- **Nearest** — any point lying *on* the curve itself, used when no distinguished point is wanted. Off by default, because it fires almost everywhere and drowns the finer snaps.
- **Intersection** — the crossing point of two curves both inside the aperture. Line×line and line×circle/arc are closed form and ship in phase 1; conic×conic is deferred.
- **Insertion** — the stored insertion coordinate of a text record or an `Asset2DInsert`. Nearly free, because those records already carry an explicit insertion point.
- **TextBounds** — the four corners and four edge midpoints of a text record's bounding box. Blocked on 2D text hit-testing, which does not exist yet.

**Relative snaps — need an anchor**

- **Perpendicular** — the foot of the perpendicular dropped from the anchor onto a candidate curve. Closed form for line, circle and arc; ellipse falls back to a bounded Newton iteration.
- **Parallel** — locks the direction from the anchor parallel to a hovered reference line. A direction constraint that yields a point on the constrained ray, not a point of its own.
- **Tangent** — from the anchor to a circle, arc or ellipse. Two solutions exist; the nearer is taken.

**3D-specific**

- **Member End** — the axis endpoints of a `LINE_MEMBER`, cylinder or pipe. Priority 15, so when a beam end coincides with a cuboid corner the beam wins — which is what the engineer meant.
- **Member Mid** — the axis midpoint of those same members.
- **Edge Mid** — the midpoint of a solid's edge, for cuboids, parallelepipeds and pyramids.
- **Face Center** — the centroid of a planar face of a solid.
- **Object-defined** — whatever semantic points an intelligent object chooses to export: piping connection faces, nozzle positions, bolt centers. This is the extension point through which every future intelligent object contributes, with no change to the snap engine.

**Grid objects**

- **Grid Object** — snaps to an explicitly placed grid entity (the building/column grid), as distinct from the ambient grid. Blocked on the grid object itself: `CREATE_GRID2D` and `CREATE_GRID3D` exist as commands but have no implementation yet.

**Direction constraints — not point snaps**

- **Ortho (F8)** — a persistent mode that locks the vector from the anchor onto the nearest work-plane axis, then slides the cursor's projection along it. CAD convention.
- **Axis constraint (X / Y / Z)** — Blender convention: during an active placement or transform, pressing an axis key restricts motion to that global axis. Same mechanism as ortho, different front-end; see §9.

## 7. Snap sources — 2D

**Stage A — broad phase.** Today `Cad2DHandleSelectionClick` linearly scans every record of every type under `cpuRecordsMutex`. That is acceptable once per click. Phase 1 reuses that scan for snapping too — correctness first, and it is the same code path already proven for selection. The optimisation is deliberately deferred; see §8.

**Stage B — snap points per record type.** All coordinates are already `double` ComputerUnits.

| Record | Exported snap points |
|---|---|
| Line | endpoints (End, 14), midpoint (Mid, 12), nearest point on segment (Nearest, 6) |
| Polyline | every vertex (End, 14), every segment midpoint (Mid, 12), nearest on any segment (Nearest, 6) |
| Polygon | computed vertices (End, 14), edge midpoints (Mid, 12), center (Center, 13) |
| Circle | center (Center, 13), four quadrants (Quadrant, 11), nearest on circumference (Nearest, 6) |
| Ellipse | center (Center, 13), four axis endpoints in the rotated frame (Quadrant, 11), nearest (Nearest, 6) |
| Arc | center (Center, 13), start/end (End, 14), midpoint of sweep (Mid, 12), in-sweep quadrants (Quadrant, 11) |
| Text | insertion point (Insertion, 13), four bounding-box corners + edge midpoints (TextBounds, 9) |
| Asset2DInsert | the insert point (Insertion, 13); member records contribute their own points normally |

Perpendicular, Parallel, Tangent and Intersection are derived against the anchor or against pairs of candidates, not exported by a single record.

## 8. 2D performance — deliberately deferred

Snapping turns a per-*click* O(n) scan into a per-*mouse-move* one. An imported DXF can carry 100 000+ records, and holding `cpuRecordsMutex` at input rate also competes with the copy thread's ingest. This is understood and accepted for the MVP: **phase 1 ships the linear scan.**

The optimisation, when it comes, is **frame-to-frame coherence** rather than a new index structure: the cursor moves a few pixels between frames, so the previous frame's candidate set, its distances, and the resolved level are nearly always still valid. Carry that state forward, re-test only what the cursor movement could have invalidated, and rebuild fully only when the view pans/zooms or the records change. This is cheaper to build and to reason about than a spatial hash, and it exploits the one property that is always true of mouse input — continuity. A spatial index remains the fallback if measurement later shows coherence alone is not enough.

## 9. Snap sources — 3D, and the work plane

**Stage A — broad phase, GPU.** Reuse `ServicePick`: enlarge the scissor box from `kPickBoxSize = 5` to the largest enabled aperture (≈ 25 px, still a ~2.5 KB readback) and, instead of scanning for the single nearest non-zero id, collect the **distinct** `matrixIndex` values in the box. Resolving those against the page snapshot yields a handful of `objectId`s — occlusion-correct and **O(1) in model size**, which a CPU scan over `storageObjects3D` is not. A new `PickPurpose::SnapHover` distinguishes it from `Select` / `Recenter`. Requests are coalesced: one in flight at a time, newer requests overwrite the pending pixel, and stale results are dropped.

**Stage B — snap points per object, CPU.** A new dispatch mirroring `GeometryForObject`:

```cpp
// Declared next to GeometryForObject in डेटा-सामान्य-3D.h.
bool SnapPointsForObject(VishwakarmaStorage::ObjectType type, META_DATA* object,
                         std::vector<SnapPoint>& out);
```

Only the few objects named by Stage A are expanded, so the cost is bounded regardless of model size. Initial coverage:

| Object | Exported snap points |
|---|---|
| `CUBOID`, `PARALLELEPIPED` | 8 corners (End, 14), 12 edge midpoints (EdgeMid, 12), 6 face centers (FaceCenter, 10), body center (Center, 9) |
| `CYLINDER`, `PIPE` | axis ends `p1`/`p2` (MemberEnd, 15), axis midpoint (MemberMid, 13), base/top circle centers and quadrants (Center 13 / Quadrant 11) |
| `CONE`, `FRUSTUM_OF_CONE` | apex / base center (End, 14), base quadrants (Quadrant, 11) |
| `SPHERE`, `ELLIPSOID` | center (Center, 13), six axis extremes (Quadrant, 11) |
| `PYRAMID`, `FRUSTUM_OF_PYRAMID` | base vertices + apex (End, 14), base edge midpoints (EdgeMid, 12) |
| `TORUS` | center (Center, 13), tube-circle quadrants (Quadrant, 11) |
| `LINE_MEMBER` | `point1`/`point2` (MemberEnd, 15), axis midpoint (MemberMid, 13), nearest point on axis (Nearest, 7) |
| `ELBOW`, `TEE`, `FLANGE` | connection-point / face centers (Insertion, 15) — the semantically correct snaps for piping |

`LINE_MEMBER` endpoints and piping connection points take priority 15, above generic geometry. **This ranking is the whole point of intelligent objects** — the snap engine stays generic and the semantics live in each object's own export function.

**The work plane.** A screen ray does not determine a 3D point, and the ambient grid can only round *within* a plane. The work plane is therefore explicit state, not inferred:

- It is **axis-aligned**, named by its **normal axis** (`x`, `y` or `z`) plus an **offset** along that normal. `z` with offset 0 is the horizontal plane through the origin.
- **Default: normal `z`, i.e. horizontal.** Structural and plant work is floor-based and the camera is Z-up, so this is the plane the user means the overwhelming majority of the time.
- **It is owned by the view, not by the Scene3D, and is never written to file.** A view will eventually host several Scene3D containers at once, so the plane cannot belong to any single one of them. It lives in `InternalSubTab` next to that view's `CameraState` — same ownership, same lifetime, same lock-free publication to the render thread — and resets to the default whenever a slot is (re)assigned, exactly as `Cad2DViewState::Reset` already does for 2D pan/zoom.
- **`o` (oblique)** — a plane picked off an existing object surface. Reserved in the UI, not implemented in phase 1.
- The **view/focal plane is discarded.** Today's `Scene3DPlacementPointFromInput` resolves onto the focal plane through `camera.target`, which rotates with the camera and is therefore useless as a construction reference. It is replaced by the work plane.
- The ambient grid rounds only the **two in-plane coordinates**. The third is the plane's own offset and is exact by definition — it is never rounded.

## 10. Ambient grid arithmetic

The increment is derived from zoom so that one grid step is always ≈ 15 logical pixels on screen, then rounded to a decimal-friendly value from the 1-2-5 sequence (`… 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100 …`). Powers of two are deliberately not used: engineers read and dimension in decimal.

```text
2D:  pixelsPerUnit = view.zoomPixelsPerCU            (1 CU = 1 mm)
3D:  pixelsPerUnit = viewportHeight / (2 * distanceToWorkPlane * tan(fov/2))   (1 unit = 1 m)

rawStep  = targetPixelsPerStep / pixelsPerUnit
step     = niceRound125(rawStep)
snapped  = round(coordinate / step) * step
```

**Watch the units.** Page2D ComputerUnits are millimetres (`2Drendering.md`, MVP decision 3); 3D geometry is SI metres (`LINE_MEMBER` comment). The same `niceRound125` helper is shared but the two worlds land on different absolute steps, and a nominal "1 unit" grid means 1 mm in one and 1 m in the other. Any shared constant expressed in "units" is a bug waiting to happen.

At high zoom the step goes below 1 mm; that is correct and intended ("nearest integer **or fraction**"). A configurable floor prevents absurd sub-micron steps.

## 11. Relative snaps, ortho and axis constraints

Ortho, Perpendicular, Parallel, Tangent and any future polar tracking are meaningless without a "from" point. The running tools already hold one (`lineCreationPreviousXCU/YCU`, `transform2DP1XCU/YCU`, the polyline's last vertex), but each holds it privately. Snapping needs it uniformly:

```cpp
struct SnapContext {
    bool     hasAnchor = false;
    double   anchorX = 0, anchorY = 0, anchorZ = 0;
    uint64_t focusObjectId = 0;   // SnapFromObject: restrict candidates to this object. 0 = all.
    uint32_t effectiveMask = 0;   // container settings, overridden by the temporary right-click mask.
    uint8_t  axisLock = 0;        // 0 = none, 1 = X, 2 = Y, 3 = Z, 4 = plane excluding X, …
};
```

The tool that is running fills `SnapContext` before resolving. When `hasAnchor` is false — the first click of a line, for instance — every relative snap and every direction constraint is simply skipped by the resolution loop, because there is nothing to be relative to.

**Ortho (F8)** is the persistent CAD mode: lock the vector from the anchor onto whichever work-plane axis it is closest to, and slide the cursor's projection along it. In 2D the axes are the page X and Y. In 3D they are the two work-plane axes plus the plane normal, so vertical members come out exactly vertical.

**The ambient grid still applies along the constrained direction.** A direction constraint fixes the *direction*; without rounding the distance the user gets an exact axis and a fuzzy length, which is half a result. So the projection along the locked axis is rounded to the ambient step, and a higher-priority snap point that lies on the constrained ray still wins over it as usual.

**Axis constraints (Blender convention)** are the per-operation front-end onto the same mechanism, active while a placement or transform is running:

| Key | Effect |
|---|---|
| `X` / `Y` / `Z` | Constrain motion to that global axis. |
| `Shift+X` / `Shift+Y` / `Shift+Z` | Constrain to the plane *excluding* that axis — i.e. the plane whose normal is that axis. |
| Same key again | Cancel the constraint. |

Blender's double-press "local axis" variant is **not** in phase 1: click-placement has no local coordinate frame to switch into. It becomes meaningful once objects carry their own orientation and can be transformed in place.

Ortho and an explicit axis constraint are mutually exclusive; the last one the user expressed wins, and both clear when the operation ends or ESC is pressed.

## 12. Settings: storage, propagation, threading

The snap set is a **32-bit mask, one bit per snap kind** — cheap to store, cheap to publish, and 32 kinds is comfortably more than §6 needs.

**Storage.** File-global defaults go in the existing `file_info` key/value table of the `.yyy` SQLite (`DataStorage.cpp`) as key `snap_defaults` — no schema change, forward compatible. Per-container values are new fields on `PAGE2D` / `SCENE3D` (डेटा.h) and their protos, with `kLogicalElementSchemaVersion` bumped; v1 files decode as 0 and fall back to the file default. Propagation is exactly as specified: file default → copied into each Page2D / Scene3D on creation and on load → thereafter local and independently editable.

> One consequence worth accepting knowingly: storing snap settings in the document means opening a colleague's file changes your snapping. Most CAD packages keep them in the user profile instead. The file-scoped choice is deliberate — a drawing carries its drafting conventions with it — and a user-level override can be layered on later without changing this design.

**Hot access.** Reading settings through `storageLogicalObjects` requires a locked linear scan — far too heavy for a per-mouse-move path. The authoritative value stays on the `PAGE2D` / `SCENE3D` object; a hot copy lives as an `std::atomic<uint32_t>` in `InternalSubTab`, right next to the per-view `CameraState` that already lives there, refreshed when the active sub-tab changes or a toggle fires.

**Two different scopes, do not conflate them.** The snap *mask* is per **container** and **persisted** (a drawing carries its drafting conventions). The work *plane* is per **view** and **not persisted** (working context, like the camera). `InternalSubTab` holds a cached copy of the first and the authoritative value of the second.

> Forward note: once a single view can host several Scene3D containers, a per-container mask becomes ambiguous for that view — two loaded scenes may disagree. The rule should then be that the **active** container's mask governs, which is already how `activeInternalSubTabMemoryId` resolves everything else. Worth knowing before the multi-scene work starts, not worth building for now.

**Temporary override.** The right-click palette writes a second atomic that shadows the container mask for the duration of the running command. It is cleared by `ClearLineCreationState`, by ESC, and on command completion — the same lifecycle as every other modal 2D state. Any new field added here must also be added to `CleanupCad2DTabResources` and to the self-heal list in `HandleZoomWindowInput`, per the established pattern.

**Publication to the render thread.** The engineering thread writes a `SnapHoverState` (snapped point, kind, screen position, the sub-tab slot it belongs to) as plain data; the render thread reads it each frame and draws the marker. One frame stale is fine, matching the camera contract stated in `UserInputProcessing.h`.

## 13. User interface

**Ribbon toggles.** A new "Snap" subgroup, one icon per snap kind. This needs a capability the ribbon does not have yet: `AllUIControls[]` controls are all momentary buttons with hover and pressed tints but **no latched "currently on" state** (`TOGGLE_AUTO_RANDOM` is a momentary button too). Add a control type or a state hook so a toggled-on snap renders with a persistent highlight. Each new button also needs the full eight-file treatment: `Commands` enum id, `UserInterfaceTranslation.csv` row plus regenerated `UserInterfaceTranslationCompiled.*`, `AllUIControls[]` row with `isEnabled = true`, a hand-authored `website/static/SVGIcons/icon_<id>_<slug>.svg`, its `SVGIconManifest.h` entry, and the `ProcessPendingUIActions` dispatch. Every snap icon is new artwork; the pre-build embedder compiles them into the binary automatically.

**Right-click palette.** Right-click is currently unused in both worlds (it only sets `mouseRightDown`; `rightButtonPressedThisFrame` is never consumed), so it is free to take. On right-click inside the viewport, draw rows of snap icons at the cursor; clicking one flips the temporary mask; ESC or a click elsewhere dismisses. Rendered in `BuildUIOverlay` from tab state, like every other overlay. The palette carries two rows:

1. **Snap kinds** — one toggle icon per enabled-able kind, plus **Snap from object**, which restricts all subsequent candidates to a single chosen object so a congested area can be worked precisely. It is never entered implicitly; only this button turns it on. The object it latches onto is the one that was under the cursor **at the moment of the right-click**, captured before the palette opened — by the time the button itself is clicked the cursor is over the palette, not over the drawing. It clears with the running command, like every other temporary override.
2. **Work plane (3D)** — buttons `o` / `x` / `y` / `z` selecting the plane by its normal axis, plus a text box holding the plane's offset along that normal. `z` is the default (horizontal); `o` (oblique, picked off an object surface) is drawn but inert in phase 1.

**Repeat last command** is **Space**, not right-click. The application has no last-command memory today; add one `Commands` value per tab, written whenever a command dispatches.

**Hover marker.** A distinct glyph per snap kind (square = end, triangle = mid, circle = center, X = intersection, ⟂ = perpendicular …) drawn at the snapped screen position, plus a short text label after a brief hover delay. Without this the user cannot tell *which* snap fired and will not trust the feature — it is not polish, it is the feature.

**Keyboard.** Adopt the industry-standard function keys so muscle memory transfers: **F3** master object-snap toggle, **F8** ortho, **F9** grid-object snap, **F10** polar/angle, **F11** object-snap tracking. Note that F9 governs the *grid object*, not the ambient grid — the ambient grid has no toggle by design (locked decision 5), so the master F3 switch turns off the object snaps and leaves ambient rounding in place. Plus `X`/`Y`/`Z` axis constraints during an operation (§11), and a hold-to-suppress modifier that temporarily disables all snapping — indispensable in congested areas, and the exact inverse of Snap-from-object.

## 14. Module ownership

- **`Snap.h`** — `SnapKind`, `SnapPoint`, `SnapResult`, `SnapContext`, the aperture table, the mask bit layout, `niceRound125`. Platform-agnostic, no graphics headers.
- **`Snap.cpp`** — the resolution loop, the ambient grid, the derived snaps (perpendicular / parallel / tangent / intersection), and the 2D candidate gathering over `TabCad2DStorage`. CPU only.
- **`Snap3D.cpp`** — `SnapPointsForObject` dispatch over `tab.storageObjects3D`, the work-plane state and math, and the ray/plane resolution. CPU only; deliberately does **not** include the geometry-page snapshot, so it stays free of `MemoryManagerGPU-DirectX12.h` and needs no `-DirectX12` variant.
- **Small edits elsewhere:** the id-set readback in `Selection3D-DirectX12.cpp` (`PickPurpose::SnapHover`); the two chokepoint functions in `RenderPage2D.cpp` and `विश्वकर्मा.cpp`; the mouse-move hooks in both input loops; settings fields in `डेटा.h` + the two protos + `DataStorage.cpp`; the marker, palette and ribbon toggles in `UserInterface.cpp`; `ClearLineCreationState` / `CleanupCad2DTabResources` for the temporary mask and axis lock.

## 15. Staged implementation

Each stage is independently verifiable and independently useful.

```text
1. Ambient grid, 2D only     -> draw a line at any zoom; both ends land on round mm.
                                Zoom in 10x; the step subdivides through the 1-2-5 sequence.
2. Hover marker + label      -> marker tracks the cursor and visibly jumps to the snapped point.
3. 2D End/Mid/Center/        -> each kind demonstrably wins near its own feature; priority
   Quadrant/Nearest             order observable by approaching a circle's center vs its edge.
4. Ortho (F8) + SnapContext  -> a line from an existing point is exactly axis-aligned; the
                                anchor comes from whichever tool is running.
5. Settings + persistence    -> toggle, save, reopen: state survives. New Page2D inherits the
                                file default; changing it afterwards does not affect the file.
6. Ribbon toggles + palette  -> latched button state renders; right-click palette changes
                                behaviour mid-command and reverts on ESC.
7. 3D work plane + ambient   -> the horizontal plane is the default; the palette's z/offset
                                controls move it; placement lands on round metres.
8. 3D SnapPointsForObject    -> place a cuboid whose corner exactly coincides with another's
                                corner; verify equality in the properties pane, not by eye.
9. 3D GPU broad phase        -> identical results with 10 000 objects; per-frame cost flat.
10. X/Y/Z axis constraints   -> during 3D placement, pressing Z restricts motion to vertical.
11. Perpendicular / Parallel -> each verified against a hand-computed reference case.
    / Tangent / Intersection
12. Snap from object         -> in a deliberately congested area, only the chosen object's
                                points are offered.
13. Function keys + Space    -> F3/F8/F9 toggle without touching the ribbon; Space repeats.
```

Stages 1–4 are the ones that make the application feel like CAD.

## 16. Defaults (proposed; tunable)

- Aperture by priority: linear, `kApertureMaxPx = 24` at level 1 down to `kApertureMinPx = 10` at level 15; level 0 unbounded. Logical pixels, DPI-scaled. Expected to be re-tuned by trial on real drawings (§5).
- Target ambient grid step ≈ 15 logical px on screen; nice sequence 1-2-5; floor 0.001 mm (2D) / 0.001 m (3D).
- Enabled by default: AmbientGrid, End, Mid, Center, Quadrant, Intersection, MemberEnd, MemberMid. Off by default: Nearest, Parallel, TextBounds (Nearest fires almost everywhere and drowns the others).
- Ortho off by default, F8 to toggle.
- Work plane: normal `z`, offset 0.
- Marker size 9 px, hover label after 400 ms.
- 3D pick box for snap hover: 25 px (vs 5 px for click selection).

## 17. Phase 2 — deferred snap kinds

Not in the first implementation, listed in rough order of how soon engineering drafting will miss them.

- **Extension** — hover an endpoint, then track along that edge's imaginary extension. Pairs with ortho and removes most construction lines.
- **From (offset base point)** — pick a base point, then enter or click a delta from it. The standard escape hatch when no snap point exists where you need one.
- **Deferred perpendicular** — perpendicular-*to-be-determined*, resolved only once the second point is known, as distinct from the perpendicular *from* the anchor. A real distinction in AutoCAD and the one users hit first.
- **Apparent intersection** — curves that cross in the screen projection but not in space. 3D-only, and a silent source of modelling errors if absent.
- **Face / plane snap in 3D** — project onto a picked face rather than onto a point on it. The natural 3D generalisation of Nearest, and the mechanism the oblique work plane (`o`) will reuse.
- **Node / point snap** — construction points must be snappable once `CREATE_POINT` is implemented.
- **Conic × conic intersection** — ellipse against ellipse or arc; needs a real solver, unlike the closed-form cases shipping in phase 1.
- **Grid Object snap** — blocked on `CREATE_GRID2D` / `CREATE_GRID3D` actually producing grid entities.
- **Local-axis constraint** (Blender's double-press) — meaningful only once objects carry their own orientation and can be transformed in place.
- **Oblique work plane (`o`)** — a work plane picked off an existing object surface. The palette button exists from phase 1 but is inert.

Two adjacent capabilities are deliberately **out of scope** here, because snapping alone cannot substitute for them:

- **Polar / object-snap tracking** — align along an acquired direction at a configurable angle increment. Ortho is its 90° special case, so designing the ortho path as an angle constraint rather than a hard-coded axis test makes polar nearly free later.
- **Typed coordinate entry** (`@1200,0`, `12.5<45`) — no snap system can express "exactly 1237.5 mm along this direction". A command/coordinate input line eventually must exist, and it should share `SnapContext`'s anchor.

## 18. Known risks

- **Mouse-move cost.** Snapping turns a per-click O(n) scan into a per-frame one, and phase 1 ships the linear scan knowingly (§8). A large imported DXF is the case that will expose it; frame-to-frame coherence is the planned answer.
- **Text bounds are not yet computable on the engineering thread** — glyph metrics live on the font/render side, and 2D text is not hit-testable at all today. TextBounds snap carries that prerequisite.
- **Per-tab pick context.** `PickPassContext` is per tab, so the same tab shown in two windows already lets two render threads touch one context. Snap hover raises the pick rate substantially, which makes that pre-existing limitation much easier to hit.
- **Snap settings in the document** change under the user when opening someone else's file (§12).
