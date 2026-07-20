---
title: "Right Side Properties Pane"
weight: 100109
---
This page is the Design Document for the right-side icon bar, the object properties pane, and the
first editable text-field UI element. To be referred by AI for coding as well as humans. It is a
plan; sections marked as decisions are locked, defaults are changeable until first implementation.

The feature: a vertical icon bar occupies the right **8mm** of the screen (below the top ribbon).
It initially holds a single icon-only "Properties" button. Clicking it opens a **64mm** wide
properties pane immediately to the left of the icon bar. When an object is selected in the 3D
scene, the pane shows Object Type, Object ID (64-bit, shown as 13-character Crockford Base32),
and the object's stored geometric fields, each in an editable text field. Typing a new numeric value and pressing Enter
routes the change through the engineering thread, which updates the field and issues a geometry
MODIFY command to the GPU copy thread.

## Locked design decisions

1. **Raw stored fields.** The pane shows exactly what the struct stores — Sphere: `Center X/Y/Z,
   Radius`; Cylinder: `P1 X/Y/Z, P2 X/Y/Z, Radius`; etc. Edits map 1:1 to memory, no
   derived-field inverse mapping. Derived rows (center + orientation vector + length) are a
   documented future extension; the descriptor format reserves room for them.
2. **Overlay, not viewport shrink.** The icon bar and pane draw on top of the 3D scene like the
   rest of the UI overlay. Scene rendering, GPU picking and placement math stay untouched; input
   over the bar/pane is swallowed before it reaches the engineering thread's scene handling.
   Migrating to a shrunk viewport is a follow-up phase (touches
   `GetVisibleSceneViewportForTab`, scene viewport/scissor, pick targets, 2D page rendering).
3. **Compile-time descriptor tables** describe which fields each object type exposes — the same
   philosophy as the `AllUIControls[]` giant array in `UserInterface.h`. One generic pane
   renderer + one generic apply function; supporting a new type = adding one table. Fields are
   addressed through **typed get/set accessor function pointers, not `offsetof`** — the shape
   structs are not standard-layout (both `META_DATA` and the derived shapes declare data
   members), so `offsetof` on them is only conditionally supported by compilers. No virtual
   functions on the data structs (`META_DATA` layout is frozen; a vtable pointer would break it).
4. **UI/render thread owns text-edit state** (focus, buffer, caret). Keystrokes never mutate
   engineering data. The engineering thread receives the committed value on Enter. The protocol
   additionally reserves a *draft-value* message so that future engineering-side validation
   (e.g. "length may not exceed 50m") can run while the user types — see
   [Draft validation (future)](#draft-validation-future). A minimal geometric validator ships
   with the MVP, not only in the future — see [MVP validation](#mvp-validation-commit-time-not-future-only).
5. **Number/text conversion (single format):** text→float parsing uses **fast_float**
   (`code-external/fast_float` git submodule, header-only); float→text uses `std::to_chars`
   shortest round-trip form. One format for both display and edit — the seeded string parses
   back to the identical bits, so clicking a field and pressing Enter without typing is a
   value-level no-op. No separate display vs edit formatting layer.
6. **Object ID display:** the 64-bit `memoryID` renders as its fixed 13-character Crockford
   Base32 form via `vishwakarma::crockford_base32::EncodeUInt64` (`CrockfordBase32.h`,
   header-only). `TryDecodeUInt64` already tolerates i/l/o typos, ready for future ID
   search/entry fields.

## UI geometry (all sizes in mm, converted per-monitor DPI like the top ribbon)

```text
              |<--- 64mm --->|<-8mm->|
  ------------+--------------+-------+
              | PROPERTIES   | I     |   <- both start below topUITotalHeightPx
   3D         | PANE         | C     |
   VIEWPORT   | (open/close) | O     |
              |              | N     |
              |              | S     |
  ------------+--------------+-------+
```

- New constants in `UserInterface.h`: `UI_RIGHT_ICONBAR_WIDTH_MM = 8.0f`,
  `UI_RIGHT_PANE_WIDTH_MM = 64.0f`. Vertical extent: from `topRibbonLayout.topUITotalHeightPx`
  to window bottom.
- Icon bar buttons are square, `UI_ICON_SIZE_MM` icons centered in the 8mm column, stacked from
  the top. Only one button for now: **Properties**.
- Pane rows use the standard 2.5mm text (`UI_TEXT_HEIGHT_MM`), `UI_BUTTON_HEIGHT_MM`-tall rows:
  label on the left ~24mm, value field on the right ~36mm, 2mm padding.
- Colors reuse `UIColors` (pane background = `actionGroupBackground`, field background =
  distinct light rectangle, focused field border = `0xFF3399FF` accent already used by the data
  tree).

## Components and ownership

### 1. Properties icon + pane visibility (render thread, per window)

- `SingleUIWindow` gains `bool rightPaneOpen = false;` (same class of UI-only state as
  `activeDropdownAction`). Toggled directly in the immediate-mode hit test — no engineering
  round-trip, because it changes nothing but UI.
- Properties icon: add `Commands::PROPERTIES_PANE = <random 10-digit id>` to `ListOfCommands.h`,
  a `UITextID` entry ("Properties" — not "Settings", which would imply app preferences), and an
  `icon_<id>_properties_pane.svg` in `website/static/SVGIcons` + one line in
  `SVGIconManifest.h` (sliders/list glyph rather than a gear). The icon renders via the existing
  `PushIcon`/`iconGlyphLookup` path — no new atlas work.
- The click is *not* pushed to `g_actionQueue`; it only flips `rightPaneOpen`. (We still record
  usage statistics via `ImprovementData::RecordRibbonAction` for parity with ribbon buttons.)

### 2. Property descriptor tables (compile time, shared)

New header `PropertyPane.h` (API-agnostic, like `DataTreeView.h`):

```cpp
enum class PropertyFieldKind : uint8_t { Float32 /*, Float64, Int, Text, Derived... future*/ };

struct PropertyFieldDescriptor {
    UITextID  labelStringID;         // e.g. UITextID::Radius, UITextID::CenterX
    float (*get)(const META_DATA*);  // typed accessors — see the layout note below
    void  (*set)(META_DATA*, float); // called by the engineering thread only
    PropertyFieldKind kind;          // MVP: Float32 only
    uint8_t   fieldIndex;            // Stable per-type index, used in the edit protocol.
    bool      mustBePositive;        // MVP validation hint (radii, diameters).
};

struct PropertyTypeDescriptor {
    VishwakarmaStorage::ObjectType objectType;
    const PropertyFieldDescriptor* fields;
    uint8_t fieldCount;
    // Optional cross-field rule (nullptr if none), e.g. PIPE inside < outside diameter,
    // CYLINDER p1 != p2. The same function serves commits now and drafts later.
    bool (*validate)(const META_DATA* object, uint8_t fieldIndex, float newValue);
};

extern const PropertyTypeDescriptor kPropertyTables[]; // Sphere, Cylinder, Cone, Torus,
                                                       // Ellipsoid, Pipe, FrustumOfCone ...
```

- **Why accessors, not `offsetof`:** the shape structs are *not* standard-layout — both the
  base `META_DATA` and the derived shapes declare data members, and C++ only guarantees
  `offsetof` for standard-layout types (on others it is "conditionally supported", i.e. a
  compiler-layout coupling rather than a language guarantee — a real concern with GCC/Clang
  ports planned). Each accessor is a tiny stateless lambda that decays to a function pointer:
  `[](const META_DATA* o){ return static_cast<const SPHERE*>(o)->radius; }` — fully portable,
  no casts at the call site, and type-checked at compile time. (Virtual `GetProperties()`
  methods remain rejected: `META_DATA` layout is frozen and a vtable pointer would break it.)
- Types whose geometry is a vertex list (`PYRAMID`, `CUBOID`, `PARALLELEPIPED`,
  `FRUSTUM_OF_PYRAMID`) get an **empty field table** in the MVP — the pane shows Type + ID only.
  Editing individual vertices is a future extension (repeating row groups).
- Reading a field for display: `fields[i].get(object)`. Writing (engineering thread only):
  `fields[i].set(object, value)`.

### 3. Rendering the pane (render thread, in `RenderUIOverlay`)

Order of work inside the existing function, after the data-tree block:

1. Draw the 8mm icon bar background + Settings button (hover/pressed tints like
   `PushInteractiveRect`).
2. If `window.rightPaneOpen`, draw the 64mm pane background.
3. Snapshot the selection: copy `tab.selection.selectedObjectIds` under `selectedMutex`
   (the render thread already does this pattern for the highlight pass).
4. If exactly one object is selected, find its `StoredGeometryObject3D` in
   `tab.storageObjects3D` under `*tab.storageObjectsMutex` and, still under that lock, copy out:
   `objectType`, `memoryID`, and the ≤ N floats named by the type's descriptor table into a
   small stack array. Lock is held for microseconds; this is the same lock/copy discipline the
   data tree uses each frame.
   - *Torn-read note:* the engineering thread mutates fields without this mutex today. A torn
     float read would only mis-display for one frame, but for correctness the plan makes the
     engineering thread take `storageObjectsMutex` for the few stores of a property apply
     (see §5) — cheap and removes the race entirely.
5. Render rows: `Object Type` (via `VishwakarmaStorage::ObjectTypeDisplayName`), `Object ID`
   as its 13-character Crockford Base32 form (`vishwakarma::crockford_base32::EncodeUInt64`
   from `CrockfordBase32.h` — fixed width, no ambiguous I/L/O characters), then one label +
   text field per descriptor.
   Multi-selection or empty selection shows a static "0 / N objects selected" line (field
   editing for multi-select is out of scope).
6. Selection change (different `memoryID` than last frame) cancels any in-progress edit.

### 4. The text field element (new reusable UI element)

State lives in a small per-window struct (`SingleUIWindow.textEditState`):

```cpp
struct UITextEditState {
    uint64_t focusedFieldKey = 0; // 0 = none. Key = hash(objectId, fieldIndex) — or the
                                  // control's stable id for future ribbon/search reuse.
    char32_t buffer[32] = {};     // Numeric entry never needs more.
    uint8_t  length = 0;
    uint8_t  caret = 0;
    uint64_t editingObjectId = 0; // Guard: commit only to the object the edit started on.
};
```

Behaviour (immediate mode, evaluated every frame in the pane renderer):

- **Click on a field** → focus it and seed the buffer from the currently displayed value
  (caret at end, full-text implicitly "selected" is a nicety we skip in MVP).
- **Number text conversion (single format):** the displayed/seeded string is produced by
  `std::to_chars` in its default shortest-round-trip form; parsing uses
  `fast_float::from_chars` (`code-external/fast_float` git submodule, header-only, faster and
  locale-independent vs `strtod`). Round-trip means the seeded string parses back to the
  identical float bits — an untouched Enter can never silently alter the stored value.
- **While focused**, the field renders the buffer instead of the live value, plus a blinking
  caret (`GetTickCount64()/500 % 2`, same idiom as the 2D text tool).
- **Character source:** `UIInput.textInputThisFrame[32]` already exists and is reset each
  frame by the render thread — but WndProc never fills it. Change in `Main.cpp WM_CHAR`:
  *in addition to* queueing to the tab, append the char into
  `currentWindow->uiInput.textInputThisFrame` (main thread and render thread already share
  `uiInput` under the existing snapshot-per-frame discipline). Accepted chars for numeric
  fields: `0-9 . - + e E`, Backspace (`\b`), Escape, Enter (`\r`).
- **Focus routing / suppression:** while `focusedFieldKey != 0`, the engineering thread must
  not interpret those keystrokes as shortcuts ('P' creates a pyramid today!). Add
  `std::atomic<uint64_t> uiKeyboardCaptureCount{0}` (or a bool) on `SingleUIWindow`; WndProc
  checks it and simply *does not queue* `WM_CHAR`/`WM_KEYDOWN` printable keys to the tab while
  a UI field has focus. Modifier tracking (`SyncModifiersForWindow`) continues unaffected.
- **Escape** → revert buffer, drop focus. **Click elsewhere** → drop focus without committing
  (MVP; commit-on-blur is a later UX decision).
- **Enter** → parse with `fast_float::from_chars`, then run the shared MVP validator (below)
  against the field snapshot. If parse or validation fails, flash the field red and keep
  focus. If it succeeds, push the commit action (below) and drop focus. The pane keeps
  rendering the *old* live value until the engineering thread's apply lands (1–2 frames) —
  acceptable and honest: the field shows engineering truth, not UI hope.

This element is deliberately generic (`UITextEditState` + accepted-charset parameter) so the
ribbon `SEARCH_BOX` (`ctrl.type == 3`, currently a dead rectangle) can adopt it later.

### 5. Edit protocol: UI → engineering → copy thread

A committed edit must carry: tab, 64-bit object id, field index, and a double value —
`UIActionEntry {uint32 id; uint64 p1; uint64 p2}` cannot hold all of that.
**Decision: extend `UIActionEntry` with one more `uint64_t p3`** rather than heap-allocating a
payload (the `IMPORT_STD_FILE` heap-pointer precedent works but is overkill for 3 words, and a
pointer would leak if a tab dies with queued actions). All existing `PushUIAction` call sites
keep working via the default argument.

```cpp
// UserInterface.h
constexpr uint32_t kPropertyCommitUIAction = 0xE0000020u; // UI action id namespace
// Encode (render thread):  p1 = (uint64_t(tabIndex) << 8) | fieldIndex
//                          p2 = objectMemoryId
//                          p3 = std::bit_cast<uint64_t>(double(value))
// Decode (main thread):    tabIndex   = uint32_t(p1 >> 8)  — REJECT if >= MV_MAX_TABS;
//                          fieldIndex = uint8_t(p1 & 0xFF) — forwarded as-is; the
//                          engineering thread bounds-checks it against the resolved type's
//                          fieldCount (the object type is unknown until resolution there).

// UserInputProcessing.h
MODIFY_OBJECT_PROPERTY = 30022, // ACTION_TYPE. objectId = memoryID, x = fieldIndex,
                                // auxValue = std::bit_cast<uint64_t>(double value).
```

`ACTION_DETAILS` transport: add one `uint64_t auxValue = 0;` field (the struct is an internal
queue element, not persisted; growing it is free — today it carries no value payload beyond
`objectId`). `PushSystemTodoToTab` keeps its current shape and gains the trailing default:
`PushSystemTodoToTab(DATASETTAB*, ACTION_TYPE, int x = 0, int y = 0, int delta = 0,
uint64_t objectId = 0, uint64_t auxValue = 0)` — all existing call sites compile unchanged.

Flow:

1. Render thread (Enter pressed):
   `PushUIAction(kPropertyCommitUIAction, p1, p2, p3)`.
2. Main thread `ProcessPendingUIActions()`: route to
   `PushSystemTodoToTab(&allTabs[tabIndex], ACTION_TYPE::MODIFY_OBJECT_PROPERTY, fieldIndex,
   0, 0, objectMemoryId, valueBits)`.
3. Engineering thread `todoCPUQueue` handler `ModifyObjectProperty(myTab, objectId,
   fieldIndex, value)`. **Lock discipline: the two mutexes are taken strictly one after the
   other, never nested** — matching `AppendObjectToTab` and `RegisterGeneratedGeometryElement`,
   which never hold `storageObjectsMutex` and `toCopyThreadMutex` simultaneously. Nesting them
   would create a deadlock ordering hazard, and holding `storageObjectsMutex` through geometry
   generation would stall the render thread, which takes it every frame:
   - find the `StoredGeometryObject3D` by `memoryId` (linear scan, same as elsewhere);
   - look up the type's `PropertyTypeDescriptor`; bounds-check `fieldIndex`;
   - re-run the MVP validator against live values (authoritative gate); on rejection, drop the
     commit — the UI thread pre-validated, so this only fires on races or bugs;
   - under `*myTab->storageObjectsMutex`: `fields[i].set(object, value)` and
     `object->dataVersion++` — nothing else; release the lock;
   - with **no lock held**, regenerate: `GeometryData geo; GeometryForObject(objectType,
     object, geo);` — this helper already exists in `DataStorage.cpp` doing exactly this
     switch over all 11 types; declare it in a header and reuse it, do not write a second copy;
   - push `{CommandToCopyThreadType::MODIFY, std::move(geo), object->memoryID, myTab->tabID,
     object->memoryIDParent}` under `toCopyThreadMutex` alone, then
     `toCopyThreadCV.notify_one()`.
4. Copy thread: **already handles MODIFY** (in-place when it fits, grow/ADD path otherwise).
   Nothing to build here.
5. Next frame the pane re-reads the stored field and displays the applied value.

### MVP validation (commit-time, not future-only)

Raw-field editing can still poison `GetGeometry()` — a NaN center, a zero radius — so a
minimal geometric validator ships with Phase D. It is one pure function over the descriptor
tables, reused verbatim by the future draft channel:

- **All fields:** reject NaN and ±Inf (`std::isfinite`).
- **Fields flagged `mustBePositive`** (radii, diameters, torus radii): reject values `<= 0`.
- **Per-type cross-field rules** via `PropertyTypeDescriptor::validate`:
  `PIPE` inside diameter < outside diameter; `CYLINDER` / `PIPE` / `FRUSTUM_OF_CONE` axis end
  points must not coincide after the edit; `TORUS` minor radius < major radius.
- The **UI thread runs the same checks before pushing the commit** (it holds the field
  snapshot), so rejection feedback is immediate — red flash, keep focus. The **engineering
  thread re-runs them against live values** as the authoritative gate and silently drops
  failures. One function, two call sites, no divergence.

### 6. Input swallowing over the bar/pane (fixes click-through)

Today WndProc queues every mouse event to the tab, and the engineering thread issues a GPU pick
for any left-click not over the top UI. With an overlay pane, clicks on the pane would also
select/deselect objects underneath — the classic click-through bug. Guard on **both sides of
the queue**:

- `SingleUIWindow` gains `std::atomic<uint32_t> rightOverlayWidthPx{0}` — written once per
  frame by the render thread (8mm, or 72mm when open, DPI-scaled). Both guards below read it.
- **WndProc side (primary):** `Main.cpp` already suppresses tab-queueing for UI regions via
  `IsClientPointOverTopRibbon` and `IsClientPointOverDataTree`. Add the sibling
  `IsClientPointOverRightOverlay(window, pt)` — x within `rightOverlayWidthPx` of the client
  right edge, y below the ribbon — and skip queueing clicks and wheel events to the tab when
  it hits, exactly like the existing data-tree paths.
- **Engineering-thread side (backup):** in the `LBUTTONDOWN` / placement / 2D handlers, ignore
  scene interaction when `input.x >= viewportWidth - rightOverlayWidthPx` (helper
  `IsOverRightOverlay(tab, x)` that resolves the window the same way
  `GetVisibleSceneViewportForTab` does). This covers events already sitting in the queue when
  the pane opens, and any future non-WndProc input source. Mouse wheel over the pane likewise
  skips the camera-zoom path (the pane will want wheel-scrolling once content exceeds the
  window height — MVP just swallows it).

## Draft validation (future — designed for, not built now)

Per the locked decision, edit state stays on the UI thread, but drafts can flow to engineering:

- On each buffer change (or throttled to ~10 Hz), the render thread pushes
  `kPropertyDraftUIAction` with the same payload as a commit. The engineering thread runs the
  *validation* half of `ModifyObjectProperty` only (no store, no MODIFY command) and writes the
  verdict into a small per-tab `std::atomic<uint64_t> propertyDraftVerdict` (packed: field key +
  ok/fail + reason enum). The render thread reads it next frame and tints the field
  red/normal. Enter then sends the normal commit, which re-validates authoritatively.
- Because drafts and commits travel the same route and are validated by the same function,
  there is no divergence between "what looked valid while typing" and "what commit accepts".

## Phased build order

| Phase | Deliverable | Verify |
|---|---|---|
| A | Icon bar + Properties SVG + `rightPaneOpen` toggle, empty pane, both input guards of §6 | Bar renders at 8mm on 96/144 DPI; icon toggles pane; clicks/wheel over pane no longer pick/deselect/zoom the 3D scene |
| B | Descriptor tables (accessors) + read-only pane rows | Select sphere/cylinder in 3D → Type, Base32 ID and correct live values appear; values track `Randomize()`d objects; multi/empty selection shows count line |
| C | Text field element + WndProc `WM_CHAR` routing + keyboard capture flag | Click field → type → caret/backspace/escape behave; 'P' no longer spawns pyramids while typing; seeded string is `to_chars` round-trip form |
| D | Commit path (`p3`, `MODIFY_OBJECT_PROPERTY`, validator, `ModifyObjectProperty`, MODIFY push) | Edit sphere radius + Enter → geometry visibly changes; pane re-reads applied value; `NaN`, `-1` radius, `inf` rejected with red flash; untouched Enter is a value no-op; undo/redo explicitly out of scope |

## Explicit non-goals of this iteration

- Multi-select editing, vertex-list editing (CUBOID/PYRAMID families), derived fields
  (orientation vector / length), units display & conversion, pane scrolling, commit-on-blur,
  undo/redo integration, persistence of `rightPaneOpen`, localization of new labels beyond
  registering `UITextID`s, and the viewport-shrink layout. Each is listed so it is a conscious
  cut, not an oversight.

## Open questions (defaults chosen, change before implementation if desired)

1. **Where the Properties icon sits when more right-bar icons arrive:** stacked from top,
   Properties stays first.
2. **Pane state per window vs per tab:** per window (matches `activeDropdownAction`); switching
   tabs keeps the pane open and simply shows the new tab's selection.
3. **Friendlier display formatting** (fixed decimals per unit category) may come later — if it
   does, the *edit seed* must stay the `to_chars` round-trip string; only the at-rest display
   may be shortened.

---

## Next iteration (planned): proto-numbered property descriptors

Everything below this line is the design for the NEXT property-pane iteration. Decisions are
locked; implementation has not started. It streamlines the descriptor system above so that
**every property carries a numeric ID equal to its field number in the type's `.proto` file**,
a full property type system, and an optional numeric range — so a non-CS developer can add an
object type or property by editing one file section and following one mechanical rule.

Architecture fact this design builds on (verified): the in-memory object definitions are the
C++ structs (`डेटा-सामान्य-3D.h`, `डेटा-पाइप.h`, `डेटा-संरचना.h`) — **not** the protos. Protos are
storage-only: `DataStorage.cpp` hand-copies struct fields into generated protobuf-lite messages
and stores the blob in SQLite. Today proto field numbers appear nowhere in C++; the struct↔proto
correspondence exists only in the statement order of the hand-written `EncodeXxx`/`DecodeXxx`
functions, with nothing enforcing it. This iteration makes proto field numbers the stable,
compile-time-enforced property IDs.

### Locked decisions of this iteration

1. **Unroll composite proto fields in the .proto files.** Singular `Point3F`/`Color4F` message
   fields become flat scalars, each with its own field number → property ID ↔ proto number is
   exactly 1:1. This is a BREAKING storage change, accepted pre-release: no migration; old
   `.yyy` files fail to load with a clear error (a load gate turns silent mis-decode into an
   explicit message).
2. **Property ID = proto field number**, `uint16_t` in the descriptor, enforced by
   `static_assert` against the protoc-generated `k<Field>FieldNumber` constants.
3. **Property tables move next to their structs** in the data headers, as `inline constexpr`
   arrays — struct + properties visible in one place. `PropertyPane.cpp` shrinks to the
   registry, lookup, validation, and the static_assert block.
4. **Full `PropertyKind` enum now, phased activation**: `Bool, Uint, Int, Float, Double,
   Utf8String`. This iteration keeps only Float editable end-to-end (pixel-identical UX);
   the enum and accessor signatures are final so tables never churn when later kinds activate.
5. **Numeric range replaces `mustBePositive`**: inclusive `minValue`/`maxValue` doubles per
   field (radii use a "smallest positive" constant). Cross-field validators stay.
6. **Labels stay `UITextID`** (localized via `UserInterfaceTranslation.csv` +
   `UserInterfaceTranslationCompiler.py`).
7. **The edit protocol keys on propertyID** (stable identity end-to-end) instead of the array
   index; the `fieldIndex` descriptor member is deleted.

### Proto unroll rule and new field layouts

Mechanical rule, applied to all 3D/fitting protos in one pass (a single storage break): keep
each message's declaration order; `Point3F foo` → `float foo_x, foo_y, foo_z` (3 numbers);
`Color4F bar` → `float bar_r, bar_g, bar_b, bar_a` (4 numbers); plain scalars keep 1 number;
`repeated` fields stay composite (vertex lists); renumber sequentially from 1. Colors unroll
too — one uniform rule; `Point3F`/`Color4F` in `DataStorage_Common3D.proto` remain only for
`repeated` use. Each proto gains the comment: *"Field numbers are permanent property IDs:
never renumber or reuse them; only append new fields."*

New layouts (field = number):

- **Sphere**: center_x/y/z = 1-3, radius = 4, color_r/g/b/a = 5-8
- **Cylinder**: p1 1-3, p2 4-6, radius = 7, color_base 8-11, color_top 12-15, color_incline 16-19
- **Cone**: apex 1-3, base_center 4-6, radius = 7, color_base 8-11, color_incline 12-15
- **Torus**: center 1-3, major_radius = 4, minor_radius = 5, color 6-9
- **Ellipsoid**: center 1-3, radius_x = 4, radius_y = 5, radius_z = 6, color 7-10
- **Pipe**: center1 1-3, center2 4-6, outside_diameter = 7, inside_diameter = 8,
  color_outer 9-12, color_inner 13-16, color_cap 17-20
- **FrustumOfCone**: bottom_center 1-3, top_center 4-6, bottom_radius = 7, top_radius = 8,
  color_base 9-12, color_top 13-16, color_incline 17-20
- **Elbow**: center 1-3, bend_radius = 4, outside_diameter = 5, inside_diameter = 6,
  sweep_angle_radians = 7, color_outer 8-11, color_inner 12-15, color_cap 16-19
- **Tee**: center1 1-3, center2 4-6, main_outside_diameter = 7, main_inside_diameter = 8,
  branch_angle_degrees = 9, branch_length = 10, branch_outside_diameter = 11,
  branch_inside_diameter = 12, color_outer 13-16, color_inner 17-20, color_cap 21-24
- **Flange**: center1 1-3, center2 4-6, flange_outer_diameter = 7, bore_diameter = 8,
  raised_face_diameter = 9, raised_face_projection = 10, color_face 11-14, color_rim 15-18,
  color_bore 19-22
- **LineMember**: point1 1-3, point2 4-6, profile_id = 7 (uint64), color_main 8-11,
  color_inner 12-15, color_cap 16-19, user_parameter1 = 20, user_parameter2 = 21
- **Cuboid**: vertices = 1 (repeated, unchanged), color_r/g/b/a = 2-5
- **Parallelepiped**: vertices = 1, color 2-5
- **FrustumOfPyramid**: vertices = 1, color_base 2-5, color_top 6-9, color_incline 10-13
- **Pyramid** (all repeated), **Folder/Page2D/Scene3D** (no composites), and all 2D protos:
  unchanged.

Protos whose singular composites disappear drop the now-unused `DataStorage_Common3D.proto`
import (11 files); the three vertex-list types above keep it.

### The new descriptor

```cpp
enum class PropertyKind : uint8_t { Bool, Uint, Int, Float, Double, Utf8String };
// This iteration renders/edits only Float; the enum is complete so tables never churn.

inline constexpr double kNoMinLimit = -std::numeric_limits<double>::infinity();
inline constexpr double kNoMaxLimit =  std::numeric_limits<double>::infinity();
inline constexpr double kMinPositive = FLT_MIN; // "> 0" for float-backed fields.

struct PropertyFieldDescriptor {
    uint16_t  propertyID;             // == field number in DataStorage_<TYPE>.proto. Stable forever.
    UITextID  labelStringID;
    PropertyKind kind;
    double (*get)(const META_DATA*);  // numeric kinds; set() casts to the stored type
    void   (*set)(META_DATA*, double);// engineering thread only
    double    minValue;               // inclusive range (replaces mustBePositive)
    double    maxValue;
};
```

- Accessors become `double`-based: one migration covers Bool/Uint/Int/Float/Double later
  (`Utf8String` gets its own accessor pair as trailing members in its phase). Documented limit:
  uint64 values above 2^53 lose precision through a double — revisit if a real ID field ever
  exceeds it (`profile_id` catalog ids do not).
- `fieldIndex` is deleted — array position serves the snapshot arrays; the protocol uses
  `propertyID`.
- `PropertyTypeDescriptor` keeps its shape; `validateCrossField` becomes
  `(const double* values, uint8_t count, uint8_t editIndex, double newValue)`.
- `ValidatePropertyEdit`: `std::isfinite(v) && minValue <= v && v <= maxValue` + cross-field
  rule. `CrossLineMember` drops its `>= 0` part (now per-field `minValue = 0.0`).
- `UserInterfaceTranslationCompiled.h` is enum-only (~350 lines) — safe to reach the data
  headers via `PropertyPane.h`, which keeps forward-declaring `META_DATA` (no include cycle).

### Tables live next to their structs

Declared `inline constexpr` immediately below each struct (C++20 is already the project
standard; captureless-lambda → function-pointer conversion is constexpr). Example, in
`डेटा-सामान्य-3D.h` below `SPHERE`:

```cpp
// Properties pane declaration. propertyID = field number in DataStorage_SPHERE.proto
// (enforced by static_asserts in PropertyPane.cpp).
inline constexpr PropertyFieldDescriptor kSphereProperties[] = {
    { 1, UITextID::PropCenterX, PropertyKind::Float,
      [](const META_DATA* o) -> double { return static_cast<const SPHERE*>(o)->center.x; },
      [](META_DATA* o, double v) { static_cast<SPHERE*>(o)->center.x = static_cast<float>(v); },
      kNoMinLimit, kNoMaxLimit },
    // ... center_y = 2, center_z = 3 ...
    { 4, UITextID::PropRadius, PropertyKind::Float, /* get/set */, kMinPositive, kNoMaxLimit },
};
```

Cross-field validators (`CrossTwoPoints`, `CrossTorus`, `CrossPipe`, `CrossLineMember`) move
as `inline` functions next to the tables they serve, so a type's whole property story sits in
one file section. Affected headers: `डेटा-सामान्य-3D.h` (Sphere, Cylinder, Cone, Torus,
Ellipsoid, Pipe, FrustumOfCone), `डेटा-संरचना.h` (LineMember). `PropertyPane.cpp` retains only:
`kPropertyTables[]` registry (one line per type), `FindPropertyTable`, `ValidatePropertyEdit`,
and the static_assert block.

### Compile-time ID enforcement

`PropertyPane.cpp` includes the generated pb headers it needs (bare-name include —
`$(IntDir)GeneratedProtobuf` is already on the include path, exactly as `DataStorage.cpp`
does) and asserts every entry:

```cpp
static_assert(kSphereProperties[0].propertyID == pb::Sphere::kCenterXFieldNumber);
static_assert(kSphereProperties[3].propertyID == pb::Sphere::kRadiusFieldNumber);
```

Contingency: if MSVC rejects constexpr-lambda tables, fall back to `inline const` tables plus
a parallel `constexpr uint16_t kSpherePropertyIDs[]` used by the asserts.

### Edit protocol switches to propertyID

- `UserInterface.cpp` commit: `p1 = (uint64_t(tabIndex) << 16) | propertyID` (was `<< 8 |
  fieldIndex`); `focusedFieldKey` hashes propertyID.
- `Main.cpp` routing unpacks the `uint16` and forwards it in the same
  `MODIFY_OBJECT_PROPERTY` slot.
- `विश्वकर्मा.cpp ModifyObjectProperty` resolves propertyID → table entry (short scan of ≤ 24
  entries), then the flow is unchanged: re-validate, `set()`, `dataVersion++`,
  `GeometryForObject`, MODIFY push.

### Storage changes

- `CommonNamedNumbers.h`: `kGeometry3DMvpSchemaVersion` 1 → 2,
  `kGeometry3DLineMemberSchemaVersion` 2 → 3 (struct `storageSchemaVersion` constants follow).
- `DataStorage.cpp`: the 14 affected Encode/Decode pairs become per-scalar
  `message.set_center_x(...)` / `message.center_x()` calls; colors keep the half↔float
  per-component conversion. `WritePoint3/ReadPoint3/WriteColor4/ReadColor4/DefaultColor4`
  remain only for the repeated-field paths (Pyramid/Cuboid/Parallelepiped/FrustumOfPyramid).
- Load gate in `DeserializeGeometryObject`: stored `schema_version` older than the type's
  current constant → fail the load through the existing `errorMessage` path with a clear
  "saved by an older pre-release build; format no longer supported" message.
- `GenerateDataStorageProtobuf.ps1` and the vcxproj need no change (the proto file set is
  unchanged).

### Renderer notes

- Snapshot array becomes `double[]`.
- **Float-kind values must be formatted via `std::to_chars` on `static_cast<float>(value)`** —
  formatting a float-backed value as double would print noise digits (`0.30000001192…`) and
  break the seeded-string round-trip guarantee.
- Parse path is unchanged: buffer → double → validate range → commit payload already carries
  `std::bit_cast<uint64_t>(double)`; `set()` casts to the stored type.

### Explicitly unchanged (behavior-preservation checklist)

- Pane visuals, row set, and Float-only editing UX (same rows, same formatting, same
  validation outcomes).
- C++ struct memory layouts — zero struct changes; `META_DATA` untouched.
- Selection flow, draft-validation reservation, input guards, copy-thread MODIFY protocol.
- SQLite schema, 2D record model, logical objects.
- `optionalFieldsFlags` / Optional64: untouched — per-proto-field property IDs are exactly the
  key the future presence system needs, so "many properties Optional, highly compact layout"
  plugs into this ID scheme without rework.

### Implementation order

1. Rewrite the 14 protos per the layouts above.
2. Bump the two schema-version constants in `CommonNamedNumbers.h`.
3. Rewrite the Encode/Decode pairs in `DataStorage.cpp`; add the load gate.
4. Redesign `PropertyPane.h` (kind enum, range constants, new descriptor, double-based
   signatures).
5. Add tables + validators next to the structs; delete the old tables from `PropertyPane.cpp`.
6. Rebuild `PropertyPane.cpp`: registry + lookup + validation + static_asserts.
7. Update `UserInterface.cpp` (double snapshots, float-cast formatting, propertyID key),
   `Main.cpp` routing, `UserInterface.h` payload comment, `विश्वकर्मा.cpp ModifyObjectProperty`.
8. Update this document (move this section into the main body as-built) and add a one-line
   note in `storage.md` §12 that 3D payload protos use flat scalars.

### Verification (Windows build)

- Build regenerates protos automatically (pre-build protoc step).
- Create all 15 wired types → save → reopen → geometry identical.
- Edit Sphere radius + center end-to-end: geometry updates, pane re-reads the applied value.
- Reject 0/negative radius, NaN, `inside >= outside` on Pipe (red flash).
- A `.yyy` saved by a previous build fails to load with the clear version message.
- Typing in a field still suppresses shortcuts ('P' spawns no pyramid).

### Phased roadmap (after this iteration)

| Phase | Deliverable |
|---|---|
| 2 | Bool/Uint/Int/Double editable end-to-end (the u64 payload already fits); LineMember `profile_id` row; Elbow/Tee/Flange tables + UITextIDs; grow the 8-slot validator/snapshot buffers (Tee needs 12) |
| 3 | Utf8String kind: `name` proto fields + persistence (append new field numbers, schema bump), string commit channel UI→engineering, string accessor pair on the descriptor |
| 4 | Logical objects (Folder name/short_code) — pane reacts to data-tree selection, not just 3D picking |
| 5 | Optional64 presence integration: presence bit per proto-field ID, pane renders unset state |

### Risks / notes

- Old `.yyy` files become unreadable — accepted (pre-release); the load gate turns silent
  mis-decode into an explicit error.
- Devanagari headers must remain UTF-8 (repo rule) — edits touch `डेटा-सामान्य-3D.h` /
  `डेटा-संरचना.h`.
- `double` accessors cap exact integers at 2^53 — irrelevant while only floats are editable;
  documented for phase 2's `profile_id`.
- constexpr-lambda tables are standard C++20; the fallback above covers a compiler objection.
