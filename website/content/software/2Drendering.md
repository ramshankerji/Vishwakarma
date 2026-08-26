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

Shipped 2026-08-25, as step 2 of the object-model sequencing recorded further down this page under
*The 2D object model*. That section carries the surrounding migration; this one is the renderer-side
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

## The 2D object model — one model, and the migration that made it one

**Moved here from `id.md` in August 2026**, when that page was narrowed to ids and references only.
This is the record of how the 2D world's engineering records became the same kind of object as the
3D world's, what it cost, and the half that has not happened yet. Identity, references and
resolution are `id.md`'s subject; residency, sizes and the record types are this page's.

**Status, 2026-08-26.** Steps 0, 1 and 2 are shipped. Step 3 is half done: all nine 2D record types
now derive `META_DATA`, so there is ONE object model — but they still live by value in
`std::vector` on the heap, not in the arena, which is the half `Optional64` waits on.

### The question, and the answer

3D engineering objects derive `META_DATA` and are allocated out of the CPU RAM arena (`राम`, in
`MemoryManagerCPU.h`). The nine Page2D record types did neither: they were plain structs living by
value in `std::vector` members of `TabCad2DStorage`.

That split cost twice. The Properties Pane had to widen its whole accessor contract to `void*`
because the two worlds had no common base (`propertiesPane.md`, amendment), and `snapping.md` §14
was about to plan a second candidate gatherer for the same reason — there was no one type to
dispatch on.

**Answered in two halves.** All nine types now derive `META_DATA`, so there is one object model and
the second snapping gatherer was never written. They are still value types in `std::vector`, not
arena objects, which is the half that remains.

### What 2D records are today — value records in contiguous vectors

**STILL TRUE, and it is the half of step 3 that has not happened.** The records derive `META_DATA`
now, but deriving it changed only their identity: they are still value types in `std::vector`, on
the CRT heap, and all three consequences below still hold. That is why `Optional64` is still out of
reach — it is residency, not inheritance, that the arena question is about.

`std::vector<Cad2DLineRecordCPU>`, `…Circle…`, and seven more, all members of `TabCad2DStorage`.
They go to the CRT heap, not the arena. Three consequences follow from that, and they are the real
argument for the current design:

1. **The rebuild is a streaming scan.** `RenderPage2D-DirectX12.cpp` walks every record of every
   type to rebuild a page. Contiguous records stream; arena pointers would not.
2. **The copy queue ships records by value.** `CommandToCopyThread2D` carries the record itself, so
   what crosses the engineering→copy thread boundary is a snapshot: trivially safe, no lifetime
   question, no fence discipline.
3. **`upsert` is one assignment.** The copy-thread ingest is literally
   `existing = std::move(updated)`.

Per-tab isolation still holds, but structurally (membership in a per-tab struct) rather than through
the arena's `memoryGroupNo`. Nothing leaks — the vectors die with the tab's `cad2d`.

Because they are not arena objects, **2D has no id directory and is not reachable by
`ResolveObject`** (`id.md` §2.2). When residency lands, 2D adopts `id.md` §3.4's sorted per-tab
directory unchanged — there is no second id mechanism for 2D and no separate 2D id space.

### `Optional64` — built, and why 2D cannot use it yet

Real code in `OptionalProperties.h`, in the vcxproj, driver-tested — and with **no consumer**. The
`LINE_MEMBER` pilot property was removed again in step 1, so nothing in the application declares an
optional property today. Say so rather than letting it look wired.

**40 bytes inline, and 0 arena bytes until something is set.** Allocation is lazy: `x` and `y` stay
null until the first `set`, which matters because the eager version would have cost a 100k-element
DXF import ~9.6 MB and 200k allocations for properties nobody set. The schema is declared through
an x-macro list per type, generating the enumerators, the byte-size table, `has/get/set/unset` and
a schema hash.

It needed four additions to the arena, all deriving their answer from an address alone:

```text
राम::MemoryGroupOf(anyPointer)        which tab owns the chunk this address is in
राम::ChunkIndexOf(anyPointer)         pool-relative chunk index, or kInvalidChunkIndex
राम::OffsetInChunkOf(anyPointer)      byte offset within that chunk
राम::AddressOf(chunkIndex, offset)    the inverse
```

`MemoryGroupOf` is what keeps the class at 40 bytes: an **interior** pointer reports the enclosing
object's group, so `Optional64` finds its own tab from `this` and stores neither a group nor a
chunk index. The other three let `ByteArrayData` stay position-independent — it holds
`{chunkIndex, offset}` rather than a pointer, so `DefragmentRAMChunks` can move a dynamic
property's bytes and rewrite the pair instead of hunting down every copy of an address. Same
reasoning as `id.md` §3.3's id-not-pointer rule, one level down.

**Copy and move are deleted**, deliberately — a shallow copy would double-free. That single fact is
why the residency half exists: a type containing an `Optional64` cannot live in a `std::vector`,
because growth moves its elements.

**AND IT CANNOT BE REACHED FROM OUTSIDE THE ARENA — a correctness constraint, not a preference.**
`Optional64::set` allocates with `cpu.Allocate(size, cpu.MemoryGroupOf(this))`, and
`राम::MemoryGroupOf` **returns 0 for any pointer outside the chunk pool**. So an `Optional64` on a
record living by value in a `std::vector` would not fail — it would silently allocate into **group
0**, the Application Tab, which never closes. The properties would outlive their tab and
`notifyTabClosed` would never de-commit them: a slow leak with cross-tab ownership, and no crash to
find it by.

Two consequences:

1. **2D gets `Optional64` only after 2D objects live in the arena**, and after there is a lookup
   mechanism to reach them by id. Until both exist, no 2D type declares an optional property.
   Decided 2026-08-26.
2. **`memoryGroupNo == tabNo` is load-bearing.** `CPU_RAM_4MB::reset(tabNo)` makes the group the
   tab, and that identity is the whole reason the address-derived lookup works. Partitioning the
   arena any other way — per world, per container — means `Optional64` needs a group-to-tab map and
   stops being 40 bytes. This is the argument against giving 2D its own arena group when it moves:
   separate accounting is worth a counter, not a partition. It is also what `id.md` §5.6 names as
   the bill for a process-wide shared catalog.

### Measured sizes

Compiled against the real headers, x64 MSVC, 2026-08-26.

```text
META_DATA base                              64 B   (was 56 before step 1)
arena per-allocation overhead                8 B
Optional64                                  40 B   (inline; 0 arena bytes until a property is set)

2D CPU records, deriving META_DATA, still by value in std::vector on the heap
  Cad2DLineRecordCPU       112  (was  88)    Cad2DArcRecordCPU              152  (was 128)
  Cad2DCircleRecordCPU     104  (was  80)    Cad2DPolylineRecordCPU         112  (was  80)
  Cad2DEllipseRecordCPU    120  (was  96)    Cad2DTextRecordCPU             160  (was 136)
  Cad2DPolygonRecordCPU    120  (was  96)    Cad2DAssetDefinitionRecordCPU   88
                                             Cad2DAssetInsertRecordCPU      112
  (polyline and text carry heap allocations on top, for points and string)

3D arena objects, for scale
  SPHERE 128    CYLINDER 160    CUBOID 152
```

+24 bytes per record almost everywhere; polyline paid +32 because its old field order padded
differently. `sizeof(Cad2DLineRecordCPU) == 112` is static_asserted, the way `META_DATA`'s 64 is.

**The bytes were never the problem** — +2.4 MB on a 100k-line DXF, noise on the largest data set the
application handles, and currently zero extra allocations because the records are not in the arena.
What the residency half has to answer for is the 100k separate arena allocations it would add, which
is what the risk list below still calls untested and what the fixed-block idea in step 3 is meant to
avoid.

### Why the residency half still matters

Not memory. **Optional properties on intelligent 2D objects.**

Page2D today is dumb geometry, so it has no optional properties and the arena buys it nothing. That
ends with P&ID, SLD, PFD and interlock diagrams. A P&ID line is not a line: it carries service,
fluid, line number, pipe class, insulation spec, tracing, tag, and from/to equipment. Most lines
carry a few of those; no line carries all of them. That is precisely the case `Optional64` was
specified for — and the arena constraint above is why it cannot be reached until the records live
in the arena.

The hazard if that half is skipped is not fat records, it is **a second, parallel optional-property
mechanism for the 2D side**. Two of everything is the pain the Properties Pane already felt; a
second `Optional64` would make it structural.

### The options, and which one landed

Four were weighed: **A** leave the split; **B** a common identity POD header in both types;
**C** a separate `META_DATA2D`; **D** full unification, 2D deriving `META_DATA` and living in the
arena behind a directory.

**C was rejected outright** — everything it would hold is common to both worlds, so it would have
guaranteed two of everything forever. **D is the destination.** What has actually been built is
**B's outcome by D's mechanism**: the records derive `META_DATA` (D's inheritance, not a separate
header) but stayed value types in `std::vector` (B's residency). That was not a compromise so much
as the discovery that step 3 is two migrations wearing one number — and it leaves the route to D
open, because finishing it is now "change where they are allocated" rather than "change the type
again".

### Sequencing — and what it came to

Steps 0, 1 and 2 are shipped. Step 3 is half done. What follows is the state of each and the facts
that constrain what is built next; the blow-by-blow of how each landed is in git history.

**Step 0 — `Optional64`. DONE.** Built, driver-tested, and with no consumer — see above for what it
is, what it cost the arena, and the constraint that governs when 2D can use it.

**Step 1 — parenthood. DONE.** `memoryIDContainer` is the container and `META_DATA::memoryIDGenerator`
is the owner, in both worlds. `META_DATA` went 56 → 64 bytes, now static_asserted; a `uint64_t`
cannot fit the 7 trailing pad bytes in any field order, so those 7 survive and the next *small*
field is still free. **There is no `persistedGeneratorId`** — the file stores ONE parent id and the
loader rebuilds the split from the stored parent's TYPE; `id.md` §2.4 carries that rule. 3D needed
no change at all: every existing read of `memoryIDContainer` was already container semantics. The 3D
save path still does not perform the collapse, because no 3D type can generate anything until
templates exist; a `_DEBUG` `[3d][warn]` fires if one ever sets the field.

**Step 2 — page the Page2D world. DONE.** **+5 lines on a 1M-line sheet: 3,386 ms → 1.29 ms,
30.5 MB staged → 164 bytes. A selection click on a 2M-line sheet costs 4 bytes and 0.51 ms.** The
design, the cost table, compaction and the full before/after numbers are in *Page2D memory paging —
as built*, above.

The RCU clone this step was named after never appeared: a 2D page holds fixed-stride records under
ONE indirect command, so appends write the unpublished tail and patch `InstanceCount`, edits are
≤8-byte flag stores, and the only copy is compaction's GPU-to-GPU pack. The word "clone" does not
occur in the 2D code.

Four things that constrain anything built on top of it:

- **The `objectId -> page` registry is copy-thread-private and deliberately NOT a CPU object
  directory.** It maps to a GPU location, which is what `id.md` §2.5 says `InstanceRegistry` is.
  Same for `gpuLocation`, the per-page `placements` and `stampedSelection`: all unlocked, so
  anything touching them lives inside `ProcessCad2DCopyBatch`.
- **An object never straddles a page** — the registry names one run, so the filler places whole
  objects and opens a new page for the remainder.
- **Two commands for one object in one batch used to draw it twice.** The hide pre-pass cannot
  catch it structurally, so `classify` drops a repeated MODIFY of an id it has already queued and
  `RegisterPlacement` uses `insert` rather than `operator[]`. Reachable in practice — two clicks of
  a polyline draining together.
- **Editing an object moves it to the front of the overlap order**, because a modify is an append.
  Invisible for opaque strokes on white; real for overlapping coloured geometry. Accepted.

What it cost that is new and resident: `recordIndex` ~50 MB at a million objects, `gpuLocation`
another ~50 MB, per-page placement lists ~16 MB. All three are revisited by step 3's residency half.
The `recordIndex` figure is also the measured datum behind `id.md` §3.4's rejection of a hash map
as the id directory: ~50 bytes per entry is ~50 GB at a billion objects.

**Step 3 — migrate the 2D object model. IDENTITY DONE, RESIDENCY NOT STARTED.** It is two migrations
wearing one number, and only the second needs the arena:

*(A) Identity — DONE 2026-08-26.* All nine `Cad2D*RecordCPU` types derive `META_DATA`. One object
model, one type to dispatch on, `dataVersion` in 2D, `dataType` set by every constructor (which
retired the nine-overload `Cad2DKindOf` table). Records stay value types in `std::vector`. The
round-trip oracle (`validations/yyy_roundtrip`) stayed green throughout.

Two things worth not re-deriving:

- **A phased migration needs an accessor bridge, and it is what a mechanical rename destroys.**
  Migrating one type first meant generic code could name neither spelling, so getters and setters
  overloaded on `const META_DATA&` versus each unmigrated type carried it — the overload set phases
  itself. Eight generic sites needed it. The bridge was deleted the moment the ninth type landed.
- **"0 means unassigned" stops being expressible** for memoryID, because `META_DATA`'s constructor
  issues an id. Every `objectId == 0` sentinel went unreachable and was retired, and three lambdas
  that set the id to 0 for later assignment now take a fresh one eagerly. No compiler error points
  at any of it. `persistedId == 0` still means unassigned — `id.md` §2.3 has why that asymmetry is
  correct.

*(B) Residency — not started.* Payloads into the arena behind a directory; `CommandToCopyThread2D`
stops shipping records by value and becomes a variant; `Optional64` becomes reachable, but only
under the group-0 constraint above and once `ResolveObject` reaches 2D. **Worth deciding before
committing to option D as written: arena-allocated fixed BLOCKS of records rather than one
allocation per record.** It keeps streaming contiguity inside a block, keeps `MemoryGroupOf` correct
because the blocks come from the tab's own group, and turns a 100k-object DXF import into ~100
allocations rather than 100,000 — which is exactly the allocation pattern the risks below still call
untested.

Left over, cheap, unblocking nothing: decide whether upsert should *increment* `dataVersion` rather
than overwrite it, and whether `META_DATA` wants a constructor that issues no id — the 912-byte
`CommandToCopyThread2D` holds all seven geometry types as members and so burns seven ids per
command constructed.

### Risks

- **Page2D is currently the more complete half of the application** — transforms, assets, DXF
  import, printing, persistence all live there. Step 3 is weeks, not days.
  *RETIRED for the identity half, which took hours rather than weeks — 234 mechanical renames the
  compiler located. It stands for the residency half, which is where the weeks are.*
- **It runs through the save/load path**, where a defect corrupts user files silently rather than
  crashing. That path needs round-trip assertions before the migration, not after.
  *DONE, and it earned its keep: `validations/yyy_roundtrip` was built first and stayed green
  through every step. Two of its three initial red rounds were the TEST misunderstanding the
  object model, not defects — which is exactly the confusion worth having before a migration
  rather than during one.*
- **Step 2 must justify itself on its own performance numbers** before step 3 is committed to.
  *SATISFIED — the numbers are above.*
- **The arena has no defragmentation in service yet** (`DefragmentRAMChunks` exists; the chunk
  compaction path is not exercised at 2D volumes). A DXF import creating 100k arena objects is a
  much heavier allocation pattern than anything the arena has carried so far.
  *STILL LIVE, and now the main risk left in step 3, because it belongs entirely to the residency
  half. It is also the argument for arena-allocated fixed BLOCKS of records rather than one
  allocation per record: the same import becomes ~100 allocations instead of 100,000.*

### Incidental findings

Recorded because they surfaced during this analysis, not because they are part of the proposal.

- **`CommandToCopyThread2D` is 912 bytes** — it was 736, and grew by 176 when the seven geometry
  records it holds as simultaneous members each gained a `META_DATA` base. Six of the seven are dead
  on every command, so a 100k-record DXF import now pushes ~91 MB through the queue instead of
  ~11 MB. A variant or union fixes it; step 3's residency half forces the change anyway, because a
  record that lives in the arena cannot be shipped by value.
- **The Application Tab's Stats view under-reports.** It reads the arena's `liveChunkCount`, and 2D
  records are not in the arena — so a 100k-line drawing is invisible to it today.
- **The 2D object model used to live in a rendering header, and the reason it did is structural.**
  The nine `Cad2D*RecordCPU` types were declared in `RenderPage2D.h`; they have since moved to
  `डेटा-सामान्य-2D.h`, which is where the 3D half keeps its equivalents — `RenderScene3D.h` defines
  no engineering object at all. But the *cause* outlives the file move: the two copy threads are fed
  differently. `CommandToCopyThread` carries `GeometryData` — vertices baked by `GeometryForObject`
  on the **engineering** thread — so 3D's render layer never learns what a `CUBOID` is.
  `CommandToCopyThread2D` carries the engineering records themselves and the copy thread generates
  the geometry, so 2D's render layer structurally must know the object model. That is also why the
  command is so large. **Any fix that makes 2D's render layer stop depending on the object model has
  to move geometry generation to the engineering thread first.**
- **`डेटा-सामान्य-2D.h` holds an earlier, unbuilt 2D schema** — 18 types, referenced nowhere. Kept
  rather than deleted, because it is not superseded junk: it carries **layers**, **line types**,
  indexed colour palettes and explicit draw order, none of which exist in the shipped records, plus
  `DIMENSION`, `LEADER`, `NURBS`, `TABLE`, `HATCH_STYLE`, `POINT2D` and a true `RECTANGLE`, which
  have no counterpart at all. Neither generation is a superset of the other, so merging them is a
  per-field decision belonging with step 3, not a refactor. Each dead type now names its live
  replacement in a comment.

### Settled — do not re-litigate

1. **No `META_DATA2D`.** One object model. The dividing line is containment, not dimensionality —
   `id.md` §2.4.
2. **No 2D type declares an optional property** until 2D objects are in the arena and a lookup
   mechanism exists.
3. **2D adopts `id.md`'s directory and resolution unchanged** when residency lands. No second id
   mechanism, no separate 2D id space.

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
