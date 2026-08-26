---
title: "2D Rendering"
weight: 100107
---
This page is the design document for the Page2D renderer — the classic, traditional CAD surface.
It began as a ChatGPT-drafted specification and has since been rewritten against what was actually
built. Subsequently, extensively modified manually by Ram to fully synchronize with application architecture. 
**What is here is either as-built or an open decision; the speculative first draft has been
removed.** Read it before touching the 2D renderer.

The premise has held: this is a **GPU parametric renderer**, not a CPU tessellator. The CPU stores
CAD objects and uploads compact primitive records; the GPU expands strokes and computes
anti-aliased coverage. The payload is CAD primitive records, never pre-built triangle geometry.

## Conventions that stuck

Decided for the first pass, and still true. These are settled — treat them as constraints, not
preferences.

1. **`1.0` ComputerUnit is `1 mm`.** CPU-side Page2D coordinates are `double`; the GPU records are
   `float32` (see *Precision*, which is the one part of this that is not finished).
2. **Lower-left origin**, matching the intended print-layout child coordinate system.
3. **The canvas is infinite**, not a paper sheet preview — dulled white, black default geometry.
4. **One `memoryID` generator** for the whole application. There is no separate 2D id space.
5. **Lineweight is day-0 behaviour**, not a later refinement. One line segment is one instance,
   the vertex shader expands `SV_VertexID` 0..5 into two triangles, and the pixel shader computes
   coverage — so cost is proportional to segments, not to pixels covered.
6. **Text height is in ComputerUnits**, so text zooms with the drawing and can vanish when zoomed
   far out. That is correct CAD behaviour. Font `0` is Noto Sans.
7. **2D shaders are prefixed `Shader2D_`** and built through the existing `FxCompile` pattern.
8. **All 2D rendering code lives in** `RenderPage2D.h/.cpp` and `RenderPage2D-DirectX12.h/.cpp`.
   The nine `Cad2D*RecordCPU` engineering records do **not**: they live in `डेटा-सामान्य-2D.h`,
   because they are the object model, not a rendering concern.

## What is built

**Page2D is a flat structure.** Each 2D element is owned by exactly one Page2D — or by another 2D
container such as a P&ID, SLD or 3D-to-2D sheet. Elements appear in the hierarchy and open in their
own sub-tab on double click. Unlike the 3D world, a VRAM page belongs to exactly one container.

The renderer is a **parallel subsystem**, deliberately not merged into the 3D triangle pipeline. It
currently draws straight into the existing scene RTT; a dedicated CAD render target and separate
composition pass were deferred and have not been built.

### The GPU record ABI

Three fixed-stride, self-contained streams. Colour and lineweight are inline — there is no
attribute-index indirection, no layer table and no z-order field.

| Record | Size | Carries |
|---|---:|---|
| `Cad2DLineGPURecord` | 32 B | two endpoints, lineweight + mode, colour, `flags` |
| `Cad2DCurveGPURecord` | 64 B | centre, two radii, start/end, rotation, curve type, `flags` |
| `Cad2DTextVertex` | 24 B | position, UV, colour, atlas index |

The `flags` word is load-bearing and is what the paging section below is built on: `kCad2DSelectedFlag`
makes the vertex shader override the stroke colour, and `kCad2DHiddenFlag` collapses the quad to
zero area. Both are single aligned 4-byte stores into a published page. Glyph quads have no flags
word, which is why text is the exception throughout.

Binding is small: `b0` view constants plus one root SRV holding the current page's record buffer,
per draw. Text additionally binds the monitor's UI SRV heap and the sampler heap. Drawing is one
`ExecuteIndirect` per page.

### Strokes

Lines, polylines and polygons all expand to line records; circles, ellipses and arcs to curve
records. **No D3D line primitives** — CAD lineweight needs real width, so a segment is expanded in
screen space perpendicular to the transformed segment and the pixel shader computes anti-aliased
coverage from the signed distance. Curves are analytic: the pixel shader evaluates distance to the
arc or ellipse, so they stay crisp at any zoom with no CPU tessellation and no flattening pass.

Lineweight has three modes, and all three ship: model-space thickness zooms with the drawing,
screen-space stays fixed in pixels, and paper-space is `mm * DPI / 25.4`.

Polylines currently draw each segment as an independent thick segment. Joins and caps are not
implemented — see *Not built yet*.

### Anti-aliasing and text

Analytic, not MSAA: coverage comes from the signed distance to the stroke edge in the pixel shader.
Text reuses the UI's MSDF atlas infrastructure rather than carrying a second text renderer.

## Page2D memory paging — as built

Shipped 2026-08-25, as step 2 of the object-model sequencing in `id.md` §7.
That page carries the sub-step-by-sub-step history — six sub-steps, each with its own verify
criterion, and the decisions that were reversed along the way. This section is the renderer-side
account: what the mechanism is, what each operation costs, and the measured before and after.

The first pass deferred optimisation, and correctly. This is the work that came due when the
deferral ran out.

### The problem it removed

Before paging, one `Cad2DPageGPU` was one whole Page2D container, so there was no paging at all —
just a buffer per container that had to be correct in full. Every drained copy-thread batch
therefore rebuilt **every container of the tab from scratch**. Appending one line to an N-object
sheet paid seven O(N) stages: seven `objectId -> index` hash maps rebuilt, a fresh id set, two full
deep copies of the record vectors, a full re-expansion into GPU records, a full re-upload, and the
retirement of every page of every container. The first three stages held `cpuRecordsMutex`, so the
engineering thread blocked behind them.

And a **selection click paid exactly the same price**, on a batch carrying no geometry at all: the
only way to re-stamp the selection flags was to rebuild the drawing that carried them.

Measured on a 1,000,111-line sheet, Debug x64: appending five lines cost **3,386 ms, four million
record touches and 30.5 MB re-uploaded**. The five lines themselves are 160 bytes. A selection click
on the same sheet cost 3,437 ms. Frame rate, meanwhile, held at 34-60 FPS with the million lines on
screen — **paging is a write-path problem, not a draw-path one**, which is worth stating plainly
because section 7 below assumes the opposite.

### The page model

**A page is (container, class, 1 MB).** The three classes are the three GPU record streams: line
records at 32 B, curve records at 64 B, and text glyph quads at 24 B vertices plus indices. So a
line page holds 32,768 records, a curve page 16,384, and a million-line sheet is about 32 pages.
1 MB was chosen so that compaction copies stay cheap and a small Page2D wastes little; 4 MB
(mirroring the 3D `GeometryPage`) and chained doubling were both considered.

**The load-bearing simplification is that no object emits more than one class.** Lines, polylines
and polygons all expand to line records; circles, ellipses and arcs to curve records; text to glyph
quads. An object therefore lives in exactly one page, its GPU location is one run
`{page, firstRecord, count}`, and there are no multi-page objects to reconcile. The filler enforces
the other half of that: **an object never straddles a page** — the tail page takes as many
consecutive objects as fit and the remainder opens a new one — because modify and delete have to
reach every record of an object from that single entry.

**2D barely needs page clones, and that is the non-obvious part.** The 3D side clones a geometry
page on every change because its objects are variable-size and its argument buffer holds one command
per object. A 2D page holds fixed-stride records and **one** indirect command whose `InstanceCount`
is the record count. That changes what is safe to mutate in place:

| Operation | Mechanism | Cost |
|---|---|---|
| **Append** | write records into the unpublished tail `[count, capacity)`, which no drawing frame reads; fence; then patch `InstanceCount` — one aligned 4-byte store whose old and new values are both valid | O(k). No clone, no new snapshot |
| **Modify** | append the new records, then set `kCad2DHiddenFlag` on the old run — `flags` is a 4-byte aligned field the vertex shader already reads | O(1) |
| **Delete** | the hide store alone | O(1) |
| **Select / deselect** | `kCad2DSelectedFlag` store, one staged word per object | 4 bytes |
| **Compaction** | GPU-to-GPU copy of the survivors into a fresh page | O(1 page) |

`kCad2DHiddenFlag` is what makes a modify O(1): the vertex shader collapses a hidden record's quad
to zero area, so the old geometry leaves the drawing without anything being moved or rebuilt. The
hide is issued **after** the new records' fence wait, beside the `InstanceCount` patch, not before
it. Both orderings are correct; this one is kinder to the eye. Hiding first blanks the old records
while the new ones are still uncounted, so a modify flashes a one-frame **hole**; hiding after
leaves the object drawn where it was until the patch lands, so it reads as a one-frame lag.

**Text is the one exception, throughout.** Glyph quads carry no flags word, so a text object is
never hidden and never registered, and a text modify re-lays-out its container's **text pages only**
— leaving the line and curve pages of the same container, and the selection flags on them,
untouched.

### Compaction

Append-plus-hide leaves the superseded records resident. They still occupy their slot **and** cost a
vertex-shader invocation apiece, because `InstanceCount` cannot skip them — the shader can only make
them draw nothing. So each page counts its holes, and once a quarter of its **capacity** is holes,
its next touch packs it: a fresh page, the survivors copied in, the stale page retired.

Three properties are worth knowing:

- **Packing stages zero bytes.** The survivors are already in VRAM, correct down to their selection
  flags, so packing is a `CopyBufferRegion` per contiguous run of survivors — nothing is re-expanded
  from the CPU records, nothing goes through the upload ring, and no lock is taken. That is why the
  measurements below report `packed=` separately from `staged=`.
- **The packed page goes back in the stale one's slot**, not at the end of the list. Otherwise every
  object on a page would jump the draw order because some *other* object on it was edited.
- **A page with no survivors is retired outright**, with no replacement. That is the only path that
  gives memory back.

The threshold is measured against capacity rather than fill, so a nearly-empty page is left alone:
90 holes among 100 records waste 2.8 KB and are not worth a 1 MB page copy, while 8,192 holes in a
line page are worth exactly that. Compaction runs **first** in a batch, before that batch's hides
and appends, so the same batch's appends can land in the room it just freed — which is the mechanism
that keeps a repeatedly-edited object inside one page. Holes made by a batch are packed by the next
one: the threshold is a bound, not a promise of immediacy.

### Measured, before and after

Debug x64, one Page2D holding **1,000,111 line records** on a 10 CU grid. The event is the same
throughout — five lines appended — and each row is one `[cad2d][perf]` line from the copy thread.

| After | cmds | indexed | copied | expanded | staged | pages | time |
|---|---:|---:|---:|---:|---:|---:|---:|
| baseline, no paging | 5 | 2,000,222 | 2,000,150 | 1,000,162 | 32,004,096 B | 1 built | 3,386 ms |
| persistent record index | 5 | **5** | **0** | 1,000,352 | 32,010,240 B | 1 built | 921 ms |
| **paged GPU store** | 5 | 5 | 0 | **5** | **164 B** | **+0 / -0** | **1.29 ms** |

The record index removed four million record touches and 2.5 seconds of the 3.4: `indexed` stopped
depending on the drawing's size at all and became the command count. Paging removed what was left.
Five appended lines now expand five records and stage 164 bytes — 160 of records plus the 4-byte
`InstanceCount` patch that publishes them — and touch no page and no snapshot. Against the baseline
that is **2,600x faster and 195,000x less staged**.

Interactive editing, measured on sheets of 2,000,000 and 100,000 lines respectively:

| Event | cmds | expanded | staged | pages | flags | time |
|---|---:|---:|---:|---:|---:|---:|
| Selection click, 2M sheet | 1 | 0 | **4 B** | +0 / -0 | 1 | **0.51 ms** |
| Click selecting nothing | 1 | 0 | 0 B | +0 / -0 | 0 | 0.075 ms |
| 1,000 objects modified | 1,000 | 1,000 | 36,004 B | +0 / -0 | 1,000 | 8-25 ms |
| 100,032 objects modified | 100,032 | 100,032 | 3,601,096 B | +3 / -0 | 100,032 | 1,310 ms |
| 100,000 lines appended | 100,000 | 100,000 | 3,200,052 B | +3 / -0 | 0 | 468 ms |

`staged=36004` is the whole of what a thousand edits costs: 32,000 bytes of new line records, 4,000
of hide flag words, and the 4-byte patch that publishes them. The compaction closes the loop — nine
rounds of 1,000 modifies put 9,000 holes in a 32,768-record page, and the next batch read
`pages=+1/-1 packed=1/1760 holes=0`: one page built, one retired, 1,760 records moved by GPU copy,
holes back to zero, in 10.5 ms. Moving *every* line at once instead leaves three full pages holding
nothing alive, and those are handed back outright: `pages=+0/-3 packed=3/0`.

Loading a `.yyy` was fixed in the same step and for the same reason — the loader was resolving each
incoming record by scanning the record vector. Through the index its own work is flat per record:
13.5 microseconds at 100,093 records, 14.8 at 300,106.

### One known behaviour change

Draw order within a container moved from "all lines, then all polylines, then all polygons" to
**insertion order**, and a modified object is appended like a new one, so **editing an object moves
it to the front of the overlap order**. Invisible for opaque strokes on white; real if coloured 2D
geometry overlaps, where dragging an object can change what it hides. Accepted. The fallback, if it
ever matters, is to keep per-class ordering *within* a page rather than across the container.

### What this step did not do

Named as decisions rather than left as omissions: no per-page frustum reject on draw (pages make
it cheap, but it stays deferred — see *Not built yet*); no eviction of inactive containers' pages, which stay
resident; no spatial index, so the CPU hit-test, snapping-candidate and zoom-to-extents scans are
all still O(N); no user-facing 2D delete, though the hidden-flag and hole plumbing a delete needs is
now complete; and `CommandToCopyThread2D` still carries all seven record types as simultaneous
members, six of them dead per command.

Unexercised rather than decided: the printing path, which was updated for per-kind pages and
compiles but has not been run against a paged drawing; compaction of a **curve** page, since only
line pages were ever driven past the threshold; and a pixel diff bracketing a single compaction
event. There is also one unreproduced crash on record — seen twice, both times after the window had
been **minimised** for about a minute with a large 2D drawing loaded — whose leading hypothesis is
that a monitor which stops presenting stops advancing its fence, so nothing deferred is ever freed.

## Selection and hit testing — as built

Selection ships, and it did **not** take the GPU picking path this page originally proposed. What
is in service is a CPU hit test that produces an id, plus a 4-byte GPU store that draws the
highlight. The reason the GPU-picking argument lost is not that CPU picking turned out to be fast —
it is that the highlight never needed a pass of its own, so the only thing a pick shader would have
bought is the hit test itself.

**Picking — CPU, analytic, per record type.** `Cad2DHandleSelectionClick` (`RenderPage2D.cpp`)
converts the click to ComputerUnits, takes a **6-pixel tolerance divided by the current zoom** so the
pick radius is constant on screen at any zoom, and walks the active container's records under
`cpuRecordsMutex` computing a true distance per type — point-to-segment for lines, polyline spans
and polygon edges, point-to-circle for circles, point-to-ellipse for ellipses and arcs. Nearest
inside tolerance wins. Thin geometry and lineweight are handled by the tolerance being a
screen-space constant, which is the property this section wanted compute picking for.

**Asset instances select as a unit.** If the hit record's `parentObjectId` names an
`Asset2DInsert`, the click expands to every record sharing that parent, across all seven geometry
types. A placed asset therefore behaves as one object to the user without being one object in
storage.

**Highlight — one 4-byte store per object, and no overlay pass.** The hit ids go into
`TabCad2DStorage::selectedObjectIds`; a `SelectionRefresh` command wakes the copy thread, which
diffs that set against its own copy-thread-private `stampedSelection` and writes
`kCad2DSelectedFlag` into the `flags` word of each affected object's GPU records. The line and curve
vertex shaders read the bit and override the stroke colour. So there is no selection overlay pass,
no second draw and no ID render target: the highlight is a property of records that are already
being drawn.

The flag word is staged **once per object**, not once per record — every record of an object carries
the same flags — so selecting a 16-segment polygon stages 4 bytes, not 64. Because the copy thread
diffs rather than rebuilds, a click that moves the selection from one container to another correctly
un-highlights the container it left. Measured on a 2,000,000-line sheet, a selection click reads
`staged=4 B pages=+0/-0 flags=1 in 0.51 ms`; a click that selects nothing short-circuits before the
command list is opened at all, at 0.075 ms.

**What is still O(N), deliberately.** The hit test itself scans the active container's records. That
is a spatial-index problem rather than a paging one and was out of scope by decision,
along with the equivalent scans behind snapping candidates and zoom-to-extents. The properties pane
is no longer among them: `Cad2DReadPaneSelection` runs once per frame per monitor and now resolves
the selected id through `TabCad2DStorage::recordIndex` in a single lookup instead of scanning up to
seven vectors.

**Not implemented, and no longer obviously wanted:** the `R32_UINT` ID target and the pick compute
shader this section proposed. The case that would still earn them is hover-highlight at interactive
rates over a very dense drawing, where the CPU scan above is the thing in the way.

## Precision — NOT implemented

Only the CPU half is done. Page2D coordinates are `double` end to end on the CPU, but **the GPU
records hold absolute `float32` model coordinates** and `Cad2DViewConstants::viewCenterCU` is
`float` too. This is a live limitation, not a theoretical one: it is what bounds how far a
large-coordinate DXF can be zoomed into before strokes visibly quantise.

**The chosen fix is page-local coordinates: a double-precision origin per page on the CPU, float
locals on the GPU, and the view constant carrying the camera origin so the shader works in rebased
space.** It aligns with the page architecture that now exists, which is what makes it the right
option. A high/low float pair, and fixed-point integer coordinates, were both considered and
rejected as more shader work for no better result at CAD zoom levels.

## Not built yet

Listed with what each would take, because the reasoning is worth keeping even where the code is not
written. Nothing here is blocking; each is a feature, not a debt.

**Polyline joins, caps and linetypes.** Today every segment is an independent quad, which overlaps
correctly for opaque strokes but shows at corners. Proper joins need the vertex shader to fetch
`p[i-1]`, `p[i]`, `p[i+1]` and compute miter/bevel/round locally. Linetypes want cumulative distance
along the polyline, evaluated in the shader.

**Hatches.** Two separate problems. *Pattern hatches*: render the boundary into a stencil or
coverage mask, draw a procedural pattern over the bounding box in model space
(`frac(dot(p, direction) / spacing)`), and clip by the mask — compact, zoom-independent and very
CAD-like, with boundary clipping as the hard part. *Solid fills*: start with stencil/mask clipping
rather than building a polygon triangulator, since CPU tessellation is explicitly not wanted.
A `HATCH` ribbon command id exists with nothing behind it.

**NURBS.** Do not attempt analytic pixel evaluation. The CPU uploads control points, weights, knots
and degree; a compute pass flattens adaptively against the current model-to-screen transform into a
transient segment buffer and generates indirect args; the existing thick-line renderer draws the
result. Subdivision must be **screen-error** based — stop when projected deviation is under 0.25 px
— not model-error based. Cubic Bézier is the easier first target, with NURBS spans converted to
rational Bézier spans on the CPU. A `NURBS` ribbon command id exists with nothing behind it.

**GPU culling and tile binning.** There is no compute pass anywhere in the 2D path. Paging made the
cheap version cheap — a per-page bounding box and a frustum reject on draw — and that is the thing
to do first. Full tile binning (32×32 or 64×64 tiles, compute assigning primitives to tiles) is a
scale answer for very dense drawings and should wait for a drawing that needs it.

**A dedicated CAD render target.** 2D currently draws into the scene RTT. A separate target would
allow composing 2D over 3D, and multi-view/multi-window composition of the same Page2D.

**Eviction of inactive containers' pages.** They stay resident today.

**A user-facing 2D delete.** The hidden-flag and hole-accounting plumbing a delete needs is
complete; the command and soft-delete flag are not written. See `undo-redo.md`.

## Known constraints worth remembering

- **The 2D render layer structurally depends on the 2D object model**, because
  `CommandToCopyThread2D` carries engineering records and the copy thread generates the geometry.
  The 3D half does not have this coupling: `CommandToCopyThread` carries vertices already baked by
  the engineering thread. Any fix has to move 2D geometry generation to the engineering thread
  first. It is also why that command is 912 bytes with six of its seven records dead on every one.
- **`डेटा-सामान्य-2D.h` still holds an earlier, unbuilt 2D schema** alongside the live records. It
  carries layers, line types, indexed colour palettes and explicit draw order — none of which the
  shipped records have — plus `DIMENSION`, `LEADER`, `TABLE` and a true `RECTANGLE`. Neither
  generation is a superset of the other, so merging is a per-field decision, not a refactor.

## GPU Tesselation Pros and cons
### Pros

| Advantage                   | Result                                                             |
| --------------------------- | ------------------------------------------------------------------ |
| Compact CAD records         | Much less memory than permanent tessellated triangles              |
| GPU culling + indirect draw | Scales to huge drawings                                            |
| Analytic strokes            | Crisp zooming, excellent lineweight control                        |
| Shared D3D12 architecture   | Reuses Our queues, fences, render threads, RTT flow               |
| Compute curve expansion     | NURBS/arcs can adapt to current zoom                               |
| Good CAD plotting model     | Screen-space, model-space, and paper-space widths are all possible |

### Cons
| Problem                                | Impact                                                        |
| -------------------------------------- | ------------------------------------------------------------- |
| Much harder than CPU tessellation      | More shaders, more debug complexity                           |
| Hatches are difficult                  | Boundary clipping, holes, islands, pattern phase              |
| NURBS are difficult                    | Adaptive GPU subdivision needed                               |
| Precision needs planning               | Float32 is not enough for large CAD coordinates               |
| Picking/snapping needs separate system | Rendering and selection are not automatically solved together |
| Many PSOs                              | Requires careful batching by primitive type/style             |
| GPU synchronization complexity         | UAV barriers, resource states, counters, indirect args        |
