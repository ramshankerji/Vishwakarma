---
name: properties-pane-plan
description: "Properties pane (implemented 2026-07-05) — the design rules new object types must follow, plus two include-order traps"
metadata:
  node_type: memory
  type: project
  originSessionId: 06246d65-142e-4980-b3a6-9fd8e0da449e
  modified: 2026-08-02T00:16:39.797Z
---

Right-side properties pane, designed and implemented 2026-07-05. Architecture doc:
`website/content/software/propertiesPane.md`. Code: `code-core/PropertyPane.{h,cpp}`.

**Rules to follow when adding an object type's fields:**
- Compile-time descriptor table per ObjectType using typed **get/set accessor function pointers**,
  not `offsetof` — the shapes are not standard-layout (META_DATA + derived both declare members).
  No virtuals; META_DATA layout is frozen.
- Show raw stored struct fields, not derived values (no computed orientation/length) — **amended
  2026-08-02**: POINT fields are now composed to world space, because objects gained a placement and
  the stored coordinates became authored space. Scalars (radius, diameter, parameters) are still raw;
  a rigid placement never touches them. Do not "simplify" this back to raw display — a moved object
  would then report where it was drawn. Read via `ReadPropertyValuesForDisplay`, write via
  `ApplyPropertyValueFromDisplay`; which field triples are points is declared per TYPE
  (`pointGroupFirstField[]`), and an edit to one world component rewrites all three authored ones.
  See [[hot-drag-placement-design]]. If the parked local-space refactor lands, this layer should be
  DELETED rather than maintained.
- Validation ships with the commit: `isfinite`, `mustBePositive` flags, a per-type cross-field
  validate fn. Run on the UI thread pre-commit, re-run authoritatively on the engineering thread.
- float↔text is a **single** round-trip format — fast_float for parsing, `std::to_chars` shortest for
  formatting. Ram rejected separate display/edit formats.
- Text-edit state is owned by the UI/render thread; a draft-validation channel is reserved so the
  engineering thread can validate intermediate values later.
- `storageObjectsMutex` and `toCopyThreadMutex` are never nested (matches AppendObjectToTab).
- Object IDs display as 13-char Crockford Base32 via `code-core/CrockfordBase32.h` (Ram's header-only
  lib; decode tolerates i/l/o).
- Input swallowing happens both WndProc-side (`IsClientPointOverRightOverlay`) and engineering-side
  as backup.
- Commit path reuses `CommandToCopyThreadType::MODIFY`; UI side is `UIActionEntry.p3` +
  `ACTION_TYPE::MODIFY_OBJECT_PROPERTY`.

**Two traps that cost real time:**
- `GeometryForObject` lives in DataStorage.cpp's **anonymous namespace**. It was lifted to external
  linkage (close/reopen the anon namespace around just it) and declared in डेटा-सामान्य-3D.h.
  AppendObjectToTab stayed internal — don't duplicate either. Same trap bit again in 2026-08:
  `DefaultSchemaVersionForObjectType` was file-local there, so object creation could not reach it and
  hardcoded its own version, mislabelling LINE_MEMBER. Fixed by moving it to CommonNamedNumbers.h as
  `constexpr` (it is pure). **Before duplicating a helper out of that anon namespace, check whether
  it belongs in a header instead.**
- **M_PI include-order trap**: डेटा-सामान्य-3D.h does `#define _USE_MATH_DEFINES` / `#include <cmath>`
  then `constexpr float M_PI=...`. This only compiles if `<math.h>` was already pulled in (via
  DirectXMath in डेटा.h) *before* that define. A new TU must `#include "डेटा.h"` first — a bare
  `#include <cmath>` does NOT fix it on MSVC.

**THE `void*` REASON EXPIRED ON 2026-08-26** and nobody has revisited the contract: all nine
`Cad2D*RecordCPU` types now derive `META_DATA`, so the two worlds DO have a common base and the
accessors could narrow to `const META_DATA*`. Not urgent, and not free either - the records are
still the storage, so a narrowing buys type safety rather than any new capability. Flagged so the
`void*` is not treated as a permanent constraint. See [[object-model-unification]]. The original
reasoning follows.

**The pane serves BOTH worlds since 2026-08-22, and the descriptor contract changed to allow it.**
Accessors are now `double (*get)(const void*)` / `void (*set)(void*, double)` - `void*` because 2D
objects had NO `META_DATA` representation at the time (there is no `LINE2D` struct; the
`Cad2D*RecordCPU` record IS the storage), and `double` because Page2D ComputerUnits are millimetres.
That second half is load-bearing, not tidiness: measured at a 0.005 mm ambient grid step the pane
shows `-5075.295` where float32 would render `-5075.294921875`, and an edit would commit that
perturbation back. The commit transport was ALWAYS a double (`p3 = double value bits`); only the
descriptor and display layers were narrow. `PropertyFieldKind::Float32` was renamed `Real`.

Things that will bite when extending this:
- **Which selection set is authoritative follows the VIEW.** 3D = `tab.selection.selectedObjectIds`
  (a vector, `selectedMutex`); Page2D = `tab.cad2d->selectedObjectIds` (an unordered_SET,
  `selection2DMutex`). Nothing joins them. `Cad2DReadPaneSelection` returns false for "not a 2D
  view" (fall back to 3D) and true with `count == 0` for "2D view, nothing selected" - collapsing
  those two is exactly the bug that made a selected 2D line report "0 objects selected".
- **2D is READ-ONLY**: every 2D `set` is nullptr and `ApplyPropertyValueFromDisplay` refuses. To
  wire the write, mutate the record and re-enqueue via `EnqueueCad2D*` - the copy-thread ingest
  function is literally named `upsert`, and `ApplyTransform2DToSelection` is the working precedent.
- **2D declares no point groups.** No placement to undo: a hit on an asset member selects the WHOLE
  instance, so the single-object path only ever sees a plain page object in page coordinates.
- Only fixed-arity types get tables (LINE2D/CIRCLE2D/ELLIPSE2D/ARC2D). POLYLINE2D/POLYGON2D/TEXT2D
  fall through to Type + ID, same as the vertex-list solids.
- `ReadPropertyValuesRaw` is the accessor loop without the placement; `ReadPropertyValuesForDisplay`
  is that plus the 3D world-space conversion and still takes `META_DATA*`.
- The definition of anything the header declares must sit OUTSIDE RenderPage2D.cpp's second
  anonymous namespace (lines ~862-1392) - `Cad2DHandleSelectionClick` lives inside it and is
  file-local, so "next to it" silently gives you internal linkage and an LNK2019.

Ids/assets: `Commands::PROPERTIES_PANE = 3150246791`; UITextID labels 1400–1433 in
`UserInterfaceTranslation.csv` (regenerate manually with UserInterfaceTranslationCompiler.py — it is
NOT a pre-build step). fast_float include is `fast_float/fast_float.h`.
Related: [[ribbon-command-recipe]], [[line-member-object]], [[commandline-build]].
