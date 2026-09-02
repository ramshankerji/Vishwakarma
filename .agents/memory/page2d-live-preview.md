---
name: page2d-live-preview
description: "Live preview of the in-progress 2D entity / transform in Page2D — what shipped, the invariant it is built on, and the two things it deliberately does not do"
metadata:
  node_type: memory
  type: project
---

Shipped 2026-09-02. Every armed 2D tool now draws what the next click would commit, tracking the
cursor, in amber. Full design is `website/content/software/2Drendering.md` → "Live tool preview —
as built"; do not restate it here, read it there. What is worth keeping in memory:

**The invariant everything hangs off: the preview must BE the thing that lands.** That is what
decided the split — the engineering thread builds it with the click path's own `Build*Record` /
`BuildTransform2DResult` functions (factored *out of* the click handlers, not written beside them),
the render thread converts it with the copy thread's own `ToGpu*` / `Append*` converters, and what
crosses between them is an engineering record (`Cad2DPreviewContent`). A parametric description, or
ready-made GPU records, would each have meant a second copy of the geometry somewhere. If a future
change makes the preview and the commit disagree, this is the invariant that broke.

**Renamed:** `Cad2DPublishSnapHover` → `Cad2DPublishHoverAndPreview`. One `Cad2DResolveSnap` call
now feeds both the snap marker and the preview; a second resolve would be a second candidate scan
for an answer that must be identical. Related: [[snapping-ambient-grid]], [[page2d-transforms]].

**Two live limitations, both deliberate:**
1. Transform previews are skipped above `kCad2DTransformPreviewMaxRecords` (100,000 live records),
   because `BuildTransform2DResult` finds the selection by SCANNING every record vector — fine once
   per click, tens of ms per mouse move on a big drawing. The proper fix resolves the selection
   through `recordIndex`, which is a change to the **commit** path, so it was left out of a preview
   change. See [[object-model-unification]] for that index.
2. The text half of a transform preview is implemented but was never exercised, because **2D text
   cannot be click-selected**: `Cad2DHandleSelectionClick` hit-tests lines, polylines, polygons,
   circles, ellipses and arcs and never touches `textRecords`. That gap predates this work; it is
   reachable only through an asset instance, whose selection expands to include text.

Not previewed at all: asset insert. Text creation needs none — its draft is already upserted on
every keystroke.
