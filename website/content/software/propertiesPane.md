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
