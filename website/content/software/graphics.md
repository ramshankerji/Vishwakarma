---
title: "Graphics API"
weight: 100102
---
API stands for Application Programming Interface. Basically a set of conventions / standards, compute engineers have come up with to write the software into. We need to pick sides here.

Choosing a graphics API to base our software upon is one of the most fundamental design we are going to make. For all practical purpose (read sunk man-month reasons) once we choose an API we will be "stuck" with it forever. This is one of the topics where I intentionally choose Performance over Development velocity. We could speed up software development by choosing a ready built engines such as open source ImGUI, GoDot, QT etc. Though, "engines" isolate the software from underlying APIs, we may get constrained by the engine itself at some point in future. We rule out closed source engines such as Unity and Unreal Engine for political reasons ! Fun Fact: This attitude is sometimes called NIH Syndrome i.e. Not-Invented-Here Syndrome. ;) So coming back to lower level APIs, we have limited APIs on each of the Operating Systems. 

On windows, we have DirectX 9 / 10 / 11 / 12, OpenGL and Vulkan. OpenGL has been deprecated long back and newer graphics features such as Ray Tracing aren't supported by it. Vulkan is generally a 2nd class citizen in windows compared to DirectX. Hence we choose the most modern flavor DirectX12. Remember, DirectX12 itself was 1st released in 2014. Hence setting it as a baseline requirement for our software is a reasonable decision. Hence DirectX12 is our ONLY graphics API for Windows Operating System. We support Windows 10 and 11 both for now (2025). This covers perhaps 90% of our target worldwide users. We also presume support of Heap_Tier_2 inside DirectX12. Note: Heap_Tier_2 started appearing in 2015/2016 timeline. What ShaderModel Level ? To be figured out. If you are feeling over-hyped to get deep down, read the 1st ( of 4 ) tutorial on DirectX12 [here](https://www.3dgep.com/learning-directx-12-1/). It is ~100 pages !

Next most "market-share" operating system is MacOS on Apple Devices. In Apple world, Metal APIs are the only recommended ( non-deprecated ) APIs, hence we go with Metal. Even Vulkan works though a translation layer such as MoltenVK etc. Still for performance and 1st party support, we choose Metal API. Mac Graphics / Metal API shall also be partially reusable on iPhone / iPad devices, since they also have Metal as the preferred API.

Next up is Linux ( Ubuntu ) Operating System. This being open source operating system, open standard Vulkan is preferred here. We want our software to be available on even free operating systems. Hence we must have a Vulkan based US as well. Another reason for keeping this Vulkan interface is due to overlap with Android Mobile Operating System. For Android Phones, we have only 2 options, deprecated OpenGL or modern Vulkan. Hence we choose Vulkan. The within last 10 year version ! i.e. Vulkan 1.1.

Above 3 APIs are for desktop application. Next up is Browser based engine. Here upcoming ( as on 2025) API named WebGPU is chosen-one. This is supported by all major web-browser vendors i.e. Google Chrome, Apple Safari and Mozilla.

Having made above decisions, we have to be realistic about our core-engineering-degree-holder software developers. We can't expect a chemical / civil / electrical / instrumentation / mechanical background people/developers to be familiar with such deep computer science concepts. Hence we structure our code in sort of mini-engine (NIH?), where adding a new UI element doesn't involve fiddling deep down in graphics APIs. This will be sorted out progressively as our software matures.

Our software installer will verify that all the relevant APIs are present on the system, before installation. So this way, inside application, we don't check every time whether a particular feature is supported by available hardware. Unless the initial installed-hardware itself changes. By default this check shouldn't take more than a few micro-seconds during application startups.

## Detailed design: the memory manager & rendering core

The notes below capture the architecture of our GPU memory manager (the *Vishwakarma* core) and the rendering engine that sits on top of it. They were written as we implemented the engine and are the single source of truth for *why* the code looks the way it does.

At startup, pick up the GPU with the highest VRAM. All rendering happens on that one device only — exactly one device is supported for rendering. The OS may still send the finished frame to a monitor connected to another / integrated GPU.

### Vertex format

Vertex layout is common to all geometry:

- 3 × 4 bytes for **Position**, 4 bytes for **Normal**, 4 bytes for **Color** (RGBA — 8 bytes if an HDR monitor is present) = **20 / 24 bytes per vertex**.
- Always go with the **24-byte** format. Tone mapping (HDR → SDR) happens in the pixel shader.
- Initial development is on `R8G8B8A8`; when we implement HDR later we will upgrade. Some hardware may not support HDR, so keep both versions of the shaders.
- Whether to load HDR or SDR shaders is decided at application startup. If the graphics card supports HDR and at least one monitor is HDR-capable, switch to HDR. Once HDR is ON, the application keeps HDR shaders even if the HDR monitor disconnects — until the app closes.

*As implemented:* the 24-byte layout is live — position `R32G32B32_FLOAT` (12) + normal `R8G8B8A8_SNORM` (4) + color `R16G16B16A16_FLOAT` (8). Vertex color has been FP16 from day one, so the vertex format needs **no** change for HDR; the pending HDR work is entirely on the output side (render-target format, tonemap pass, swap chain) — see Phase 5.

*Planned:* once colour moves into the per-object instance record, those 8 bytes become dead weight for ordinary geometry and the base vertex drops to **16 bytes** — a 33% cut across the bulk of a model. Per-vertex colour is still genuinely needed (FEA contours, imported meshes with baked colour), but as separate *variants* with their own pipelines and their own pages rather than as a tax on every vertex. See *Appearance, variations and display state*.

### Lighting

Initially, **hemispheric ambient lighting**:

```
Factor       = (Normal.z × 0.5) + 0.5
AmbientLight = Lerp(GroundColor, SkyColor, Factor)
```

Screen Space Ambient Occlusion (SSAO) to darken creases and corners is planned for a future revision.

### World matrix

All vertices are positioned in object-local space; the world matrix is applied in the vertex shader. This lets us move even a 1000-vertex object with just a 48-byte world-matrix update per object. We use a **packed 48-byte** world matrix instead of 64 bytes to save bandwidth — the last row is always `0,0,0,1`, so we omit it and reconstruct it in the shader.

*As implemented, with one important qualification:* the shader side is exactly as described, but "object-local space" is currently **authored space**, not a canonical local frame. Every generator emits vertices at the coordinates the object was drawn at (a sphere at `center`, a cylinder between `p1` and `p2`), and the world matrix carries only the object's *placement* — the rigid transform accumulated since it was drawn (see *Object placement*). So a freshly created object has an identity world matrix and world-coordinate vertices; only a moved one has a non-identity matrix.

That is enough for the move path to be free, because a move never changes the vertices. It is **not** enough for two of the things the local-space claim would otherwise buy, and both are worth stating plainly since the sentence above reads as though we already have them:

- **Geometry can never be shared between two identical objects in this form.** Two bolts at different places produce different vertex bytes. *This no longer blocks instancing*, which is the single biggest VRAM lever for plant models: 10M plan Step 8 gets its canonical meshes from a **library built in code at startup**, and an eligible object emits a reference plus a transform rather than vertices at all — so no generator has to be rewritten to emit a canonical local frame. What the authored-space form still costs is the point below.
- **Float32 precision is spent on the offset.** A Ø300 flange at world (4000, 2000, −300) stores absolute coordinates, burning most of the mantissa on position and leaving little for shape. Kilometre-scale sites with millimetre features are where this bites.

Moving the generators to a canonical local frame — shape around the origin, position entirely in the placement — is the natural follow-on. The axis-defined types (cylinder, cone, pipe, tee, line member) are the awkward ones: their two endpoints are engineering data, so for those the endpoints should stay the stored truth and the placement should be *derived* from them rather than replacing them.

### Threading model

- **Separate render threads (one per monitor)** and a **single copy thread**. The copy thread is the ringmaster of VRAM!
- Each render thread is in VSync with its monitor's unique refresh rate, and has its own render queue (e.g. one at 60 Hz, one at 144 Hz, one at 30 Hz), command queue, allocator and command list.
- We use `ExecuteIndirect` with a start-vertex location instead of `DrawIndexedInstanced` per object.

### Per-tab VRAM isolation

Each tab has its own completely separate VRAM, except for the un-closeable tab 0 which stores common textures and UI elements.

To support hundreds of simultaneous tabs, we start with a small heap (say 4 MB per tab) and grow it only when necessary. Each page can be a mixture of geometry types (cylinders, cubes, I-beams, …) instead of one giant 256 MB buffer. Don't manually destroy heaps on tab switch — use **Evict** and let the OS handle caching. If the user clicks back to a heavy tab, `MakeResident` is faster than re-creating heaps. Tab 0 is always resident. Eviction happens with a lag of a few seconds. A more advanced, system-memory-budget-based eviction strategy comes after the rest of the spec is implemented.

Each page carries a corresponding `ExecuteIndirect` argument buffer, and each tab has its own world-matrix buffer. When we defragment a page, we must simultaneously rebuild its argument buffer.

### Lock-free VRAM management

We use `ExecuteIndirect` + **versioned geometry pages** (max page size 4 MB initially). On geometry modify (Add / Modify / Delete):

- If the new geometry (plus the filled-up last active page) exceeds the 4 MB page threshold, **create new pages** — do not touch existing ones — and then publish.
- Otherwise allocate a new page via the copy queue. The copy queue makes a **read-only** copy of the existing page to create a `newPage` (not yet published for rendering). `DirectQueue0/1/2…` keep rendering as usual. Leave the old page in `COMMON` state permanently; never explicitly transition it to `VERTEX`. Both render and copy-source are allowed on their respective queues by implicit promotion from `COMMON`.
- The copy queue finalizes the copied `newPage` and uploads the delta. For additions, just add; for modify / delete, if page free space drops below threshold → rebuild / defragment the page.
- Publish the pointer swap **atomically**. Once all render threads have passed a fence, retire the old page later by releasing its buffers.

Geometry is **NOT** kept in CPU RAM once uploaded to VRAM (memory efficiency, keeping iGPU systems in mind). Objects are generated on demand by the engineering thread and handed to the copy queue. To be able to defragment, the copy queue stores the byte/index ranges of every object loaded into a page.

The copy queue prepares `newPage` (vertex buffer, index buffer, `ExecuteIndirect` buffers) and uploads it to VRAM. This PCIe transfer happens in parallel while the render threads keep running. Iteration over all objects has been removed from the engine entirely. There are two levels of batching: the engineering thread batches changes to some extent, and the copy thread batches further by draining the submission list.

**Geometry page lifecycle:** created in `COMMON` • never explicitly transitioned • only used in read-only states • copied from (`COPY_SOURCE`) • copied into (`COPY_DEST`) only before publishing (once published there is no write) • drawn from (`VERTEX` / `INDEX` / `INDIRECT`).

**Strict invariants:**

- Geometry pages are immutable after publish.
- No explicit state transitions for page buffers.
- All page swaps are atomic.
- Old pages are destroyed only after all queues retire.

There are multiple views per tab. *As implemented*, the `ExecuteIndirect` argument buffer is per **page** (one, not per-view double-buffered) and is regenerated whenever a page is cloned; a delete soft-marks the placement record (`isDeleted`) and the next rebuild drops it. Per-view argument buffers proved unnecessary — a view draws only the pages of the containers in its SubTab set, reached through the snapshot's container directory (Step 6). The earlier form of this, issuing an argument count of 0 for pages of inactive containers, is gone: those pages are no longer visited at all.

**Free-list allocator** *(designed, not built — demoted to a telemetry-gated decision in Phase 6; the implemented path is a per-batch largest-gap scan)***:** maintain a CPU-side segregated free list, per tab. The allocator knows, e.g., "I have a 12 KB middle gap in Page 3 and a 40 KB middle gap in Page 8." When a 10 KB request comes in, it immediately returns "Page 3" — no iterating through page objects. If the free list says no existing page can accommodate the geometry, create a new heap / placed-resource buffer. The free list tracks only middle empty space, not internal holes from deleted objects; aggregate holes are tracked per page and defragmented occasionally.

When a buffer accumulates more than 25% holes, it creates a new defragmented buffer and switches over once complete (for new geometry additions). At most one buffer is defragmented at a time (between two frames). Since pages are 4 MB, this does not produce a high-latency stall while running async with the copy thread.

**Root signature:** the constants (View/Proj matrix) go in root constants or a very fast descriptor table, as these don't change between pages. Only the VBV/IBV and the EI argument buffer change per batch/page.

### Object representation

The realistic "worst case" hierarchy for a CAD frame:

- **Index depth:** 16-bit vs 32-bit (hardware requirement) — e.g. nuts/bolts (16) vs engine blocks (32).
- **Transparency:** opaque vs transparent (sorting requirement) — transparent objects must be drawn last for alpha blending.
- **Topology:** triangles (solid) vs lines (wireframe) (PSO requirement) — we cannot draw lines and triangles in the same call.
- **Culling:** single-sided vs double-sided (PSO requirement) — sheet metal vs solids. Since sectioning is a common use case, we may make all geometry double-sided; to be ascertained later.
- **Buffer pages (N):** how many 4 MB pages are in use.

Total unique batches = 2 × 2 × 2 × 2 × N = **16 × N**. This ensures no pipeline-state reset while rendering a single page — one `ExecuteIndirect` call per page.

*Superseded at the top end — and this has now shipped:* the one-call-per-page model is what the legacy draw path still does, but the primary path is GPU-compacted, per-Viewport command buffers and a **single `ExecuteIndirect` per Viewport**. See *10 Million Objects + 64 SubTab Draw Plan*, Step 7.

#### The axes do not behave alike any more

Once draw commands are compacted per Viewport rather than issued per page, the four axes above split into two kinds — and the split, not the count, is what should drive the design:

- **PSO-only axes — transparency, topology, culling.** These change *pipeline state* and nothing else, so each can be a per-object bit that the compute pass reads to route commands into separate output regions: one `ExecuteIndirect` per non-empty bucket, with **pages free to mix them**. 2 × 2 × 2 = 8 buckets worst case, independent of N. *(Planned — today everything draws with one PSO; specified under* Appearance, variations and display state*.)*
- **Layout axes — vertex format.** These change the *byte layout of the page itself*: stride drives `vertexByteOffset` alignment, `IsFull`, and `BaseVertexLocation = vertexByteOffset / stride`. No amount of bucketing helps, so vertex format must stay a **page** property. *(Planned.)*
- **Index depth is neither — it stays a page property but is no longer a bucket key.** *(Implemented.)* This is the axis that has already changed. `StartIndexLocation` is an *element* offset in units of the command's own format, and each compacted command carries its own `D3D12_INDEX_BUFFER_VIEW` including `Format`; index format is not PSO state for triangle lists. So a 16-bit page and a 32-bit page draw in **one** call. Uniformity *within* a page is not enforced by the API — but keep it anyway, because the call-sharing benefit is already had between pages and mixing would put a per-object divisor back into the three sites that independently compute these offsets.

Enabling 32-bit pages is therefore small: a `page.indexFormat`, and `RebuildIndirectBuffer` dividing by the page's element size instead of `sizeof(uint16_t)`. Index offsets are already 4-byte aligned, which satisfies `R32_UINT` unchanged. Note that 32-bit indices and the big-object fallback are *different* thresholds — 65,536 indices is only 128 KB, so a 100k-index mesh needs 32-bit indices while still fitting easily in a 4 MB page.

*Answered by 10M plan Step 8:* repeated geometry (e.g. bolts) needs only one set of vertex/index buffers, drawn with different world matrices. Step 8 stores that one set in a shared library and points every instance's command at it.

### Normals

The industry-standard solution for normals is not 16-bit floats but **packed 10-bit integers** — format `DXGI_FORMAT_R10G10B10A2_UNORM`:

- X: 10 bits (0–1023), Y: 10 bits, Z: 10 bits, padding: 2 bits (unused). Total: 32 bits (4 bytes).
- **Size:** 3× smaller than a 12-byte normal.
- **Precision:** 10 bits gives 2¹⁰ = 1024 steps. Since normals lie between −1.0 and 1.0, that's ~0.002 precision — visually indistinguishable from 32-bit floats for lighting, even in high-end CAD.
- Vertex-shader normalization: `Normal = Input.Normal * 2.0 - 1.0`.

*As implemented:* we shipped `DXGI_FORMAT_R8G8B8A8_SNORM` instead — same 4 bytes, signed, zero shader remap (SNORM unpacks straight to −1..1). 8 bits (~0.008 steps) has shown no banding on CAD lighting so far; the 10-bit layout above remains the documented upgrade path if it ever does.

### Page structure

Putting the vertex and index buffer in the **same page** is the superior architectural choice for three reasons:

1. **Halves allocation overhead** — one heap/resource per 4 MB page instead of two.
2. **Cache locality** — the GPU fetches vertices and indices from physically close VRAM (same page), slightly improving cache hit rates.
3. **Double-ended layout** — vertices start at offset 0 and grow **up**; indices start at offset max (4 MB) and grow **down**. Free space is always the gap in the middle. The page is full when the vertex head pointer meets or crosses the index tail pointer. A mandatory 64-byte gap in the middle handles alignment concerns.

**Vertex offsets are a whole number of vertices, always.** Every object's `vertexByteOffset` is rounded up to a multiple of `sizeof(Vertex)` (24), never to a power-of-two boundary, because the draw path addresses vertices by `BaseVertexLocation = vertexByteOffset / sizeof(Vertex)` in units of stride — and the Selection3D highlight path recomputes the same division independently. 24 already satisfies `CopyBufferRegion`, so a separate alignment buys nothing. Index offsets stay 4-byte aligned. Note the trap: 24 is not a power of two, so the usual `(v + a - 1) & ~(a - 1)` helper cannot express this and a separate round-up-to-multiple is required. An earlier revision aligned to 16 and survived only because every generator happened to emit an even vertex count; the first odd-count object — an imported mesh of N triangles gives 3N vertices — would have silently misaligned the *next* object in that page by 8 bytes, with no error or warning.

**Lazy creation:**

- New tab → allocated memory = 0 MB.
- User draws a bolt (solid) → allocate `Solid_Page_0` (4 MB).
- User draws a glass window → allocate `Transparent_Page_0` (4 MB).
- User never draws a wireframe → `Wireframe_Page` stays null.

Pages are created in `COMMON` and never explicitly transitioned; implicit promotion covers every read-only use they have. (An earlier revision of this section specified a combined `VERTEX_AND_CONSTANT_BUFFER | INDEX_BUFFER` state — that contradicted the page lifecycle above and was never built.)

| Feature | Decision | Benefit |
|---|---|---|
| Page content | One container, one vertex format, one index width. PSO class is *not* a page property — it is a per-object bit resolved into draw buckets *(planned)* | Container keeps the coarse reject and O(1) teardown; vertex format and index width are byte-layout facts a page cannot mix; PSO class does not need to be a page fact once commands are compacted per Viewport |
| Growth logic | Fixed-size pages; a container grows by adding pages | No moving old data; uniform pages retire / recycle cleanly |
| Page size | 4 MB fixed today; chained doubling 4→8→…→64 MB queued (Phase 7) | Cheap RCU clones now; page count stays in the low thousands even on a 48 GB card later; jumbo objects still bypass via the big-buffer fallback |
| Allocation | Lazy (on demand) | Keeps "Hello World" tabs lightweight |
| Sub-allocation | Double-ended stack | Maximizes usage for varying vertex/index ratios |

New geometry is appended (in the middle) only if both the new vertex and index buffers fit inside; otherwise a new buffer is allocated. The copy thread also batches — it aggregates all objects that fit in the current buffer into a single GPU upload, coalescing updates into single `ExecuteCommandList` calls where possible to reduce API overhead.

**"Big buffer" fallback:** if `Allocation_Size > Max_Page_Size`, allocate a dedicated committed resource just for that object, bypassing the paging system. This handles large STL meshes or terrain maps. Treat big buffers as a special page type with a separate "large object list". Don't jam them into the standard EI logic if they need unique per-object resource bindings — one separate draw call per jumbo object. Keep a separate `std::vector<BigObject>` in the tab structure. Rendering: loop through pages (`ExecuteIndirect`), then loop through big objects (standard `DrawIndexedInstanced`, or EI with count 1).

### Defragmentation logic

*(Rewritten July 2026: the original freeze-based design predated the RCU page system that was actually built. Same requirements, simpler mechanism.)*

Defragmentation rides the existing RCU clone path and needs **no frame freeze** and no resource-state gymnastics:

- Every ADD / MODIFY / REMOVE batch already clones the affected pages on the copy queue, applies changes to the clones, rebuilds each clone's `ExecuteIndirect` argument buffer and publishes the new snapshot atomically. Render threads keep drawing the old pages until the swap, so nothing ever freezes.
- When a page's `holeBytes` cross the ~25% threshold, its clone step switches from whole-page `CopyResource` to per-live-object `CopyBufferRegion`, driven by the `GeometryPlacementRecordInPage` table, packing survivors tight. Offsets are remapped in CPU metadata; the argument-buffer rebuild — mandatory on every clone anyway — picks up the new offsets for free.
- Relocating an object never touches byte contents: indices are object-relative, resolved per draw through `BaseVertexLocation` / `StartIndexLocation`.
- At most one page compacts per batch (bounds the extra copy volume); a clone that ends up empty is dropped instead of published (empty-page GC — already implemented).
- EI argument buffers stay tightly coupled to pages: regenerate per clone, never patch. (Unchanged rule.)

*As implemented:*

- **The punch and the compact can never be the same batch, by construction.** Holes are added to the *clone* during Pass 3, while the compaction decision is made in Pass 2 against the *old* page. So a batch that creates holes always copies whole-page, and only a later batch that re-clones that page packs it. This is not a limitation to design around — it falls out of RCU and keeps the decision on immutable data.
- **Compaction competes with the empty-page GC, and the GC usually wins.** A page whose objects *all* leave drains to `objectCount == 0` and is dropped rather than compacted, which is strictly better. Compaction therefore only ever fires on pages that keep survivors — exactly the case the GC cannot handle. A test that modifies every object measures nothing; it must modify a fraction and then come back for the rest.
- **The clone no longer copies the old argument buffer.** Every clone rebuilds it unconditionally a few lines later, so the `CopyResource` was moving 1.5 MB that was immediately overwritten — and it quietly contradicted the "regenerate, never patch" rule above. Removing it took a clone from ~5.5 MB to ~4.0 MB, a 27% cut in *all* clone traffic, compacted or not. Only the first `indirectCount` commands are ever read, so the untouched tail of a fresh buffer is harmless.
- Survivors keep their relative order, so a packed offset can never exceed the original and the page cannot overflow. Placement follows the same rules as a fresh append — whole multiples of `sizeof(Vertex)` up, 4-byte-aligned indices down — because the draw path derives `BaseVertexLocation` by dividing, and Selection3D repeats that division independently.
- Measured on a 10,000-object scene: page count stabilised at 56 with compaction against 340 and still climbing without it, and the `compacted` counter is on the heartbeat next to `clones`.

**Growth logic** needs no special path either: a container simply gets more pages — new geometry lands in the page with the largest middle gap, else in a fresh page. Pages are 4 MB today; once chained doubling lands (Phase 7), successive new pages of a fast-growing container double up to 64 MB, but the rule is unchanged: nothing grows in place, so nothing freezes. The old plan (grow a heap in place while the tab's views freeze) is gone.

### Freeze logic

Use **Render To Texture (RTT)** to implement frame freezes, since the swap chain is `FLIP_DISCARD`. RTT is now the standard frame path (draw → RTT → copy into backbuffer → present), so a freeze is simply "keep presenting the last RTT". With RCU pages, defragmentation no longer needs freezes at all (see above); the mechanism stays valuable for eviction safety, device-loss handling, HDR tonemapping (the copy becomes a draw), UI composition and multi-monitor flexibility.

### Known issues / limitations (to be resolved in a later revision)

Everything formerly tracked here now has a roadmap slot: transparency sorting and the hot-drag / active-mutation path sit in Phase 5, instanced repeated geometry in 10M plan Step 8, the telemetry counters in Phase 6, and evict/residency logic, compute-shader frustum culling, mesh shaders and instance-based LOD in Phase 7.

Resolved since this list was written: selection highlighter methodology — shipped as the GPU pick pass + highlight overlay + rotation cube (Selection3D module); vertex page offsets misaligned against the 24-byte stride — see the alignment rule under *Page structure*; world-matrix slots recycled before the frames referencing them retired — see Step 1 of the 10-million-object plan; and the pick resolve's world-space AABB centre, which was transforming a local centre by the *transposed* matrix (harmless only because every generator bakes positions into vertices and leaves `worldMatrix` identity) — Step 2's registry shadow computes it with the same row-vector convention the vertex shader uses.

### Miscellaneous specification

- A uniform 64-bit object ID, unique across all objects in the entire process memory. The renderer maps exactly one `gpuInstanceIndex` to one such ID; multiple simultaneous geometry variations per object make the key `(memoryID, variation)`, packed into the id's free high 8 bits at no storage cost — see *Appearance, variations and display state*.
- We expect roughly 1000–5000 draw calls per frame at present. That budget is what the one-`ExecuteIndirect`-per-page model was sized for; the shipped path meets it with **one** compacted `ExecuteIndirect` per Viewport regardless of page count (Step 7), which is what makes the budget survive to 10M objects.
- Multiple partially-overlapping windows, each independently resizable / maximize / minimize.
- The lowest distance between an object and *all* the different view-camera positions is used by the logic threads to decide the Level of Detail.
- A mechanism to manage memory over-pressure, signalling the logic threads to reduce level of detail within some distance.
- The GPU memory manager is a **singleton** — exactly one instance manages all GPU memory.

On a desktop PC with two discrete GPUs and one integrated GPU, each driving one active monitor, we still use exactly one device for rendering all monitors. Windows 10/11 WDDM supports heterogeneous multi-adapter: when a window moves, DWM composites surfaces and copies the frame across adapters if needed. This works but is slow, since every such frame must traverse the PCIe bus.

### Roadmap (to-do list)

As items complete, they move out of this pending list and into the design document proper.

**Phases 1–3 — complete.** The visual baseline (lit 24-byte vertices, hemispherical lighting, mouse navigation), the RTT infrastructure and the API pivot (structured-buffer world matrices, `DrawIndexedInstanced` → `ExecuteIndirect`) are all live; their designs are described in the sections above.

**Phase 4 — the memory manager (the Vishwakarma core)**

Done so far: 4 MB double-ended VRAM pages with RCU clone → mutate → atomic-snapshot publish, per-container page ownership, fence-gated retirement (snapshots, pages, instance indices, instance slots), empty-page GC, the reserved-tile instance arena + redirect table + visibility mask with their copy-thread identity registry (10M plan Steps 2–5, which retired the doubling world-matrix table and its retired-buffer queue outright), tab/view management, basic ribbon UI, and the global upload ring with ring-gated chunked submission.

**Global upload ring buffer + ring-gated submission — done.** One persistent-mapped 64 MB ring with fence-tracked reclaim now serves Scene3D geometry staging and the argument-buffer rebuilds, replacing the per-object committed staging resources in `RecordGeometryUpload`, and the dead per-tab upload heaps (64 MB vertex + 16 MB index per tab, committed and mapped but never written) are gone. Submission is chunk-driven: one recording, one submit, one fence wait and one publish per chunk, collapsing the three record/execute/CPU-wait cycles per tab into one. The CPU-side drain is capped so a lakh-object import cannot materialise hundreds of megabytes of `GeometryData` before a byte reaches the GPU, and an oversize payload falls back to a one-off committed buffer. Specified in full as Step 0 of the 10-million-object plan below. 

**Page2D record uploads are now on the ring too.** `ProcessCad2DCopyBatch` takes the copy thread's allocator and command list as parameters instead of creating a pair per batch, and `CreateUploadWithData` / `UploadVector` — which committed a fresh UPLOAD resource per vector, up to six per container page times every container, on every batch — are gone. It cannot pre-chunk the way the Scene3D path does, because a 2D batch rebuilds every container's page wholesale and the total is not known until the records are expanded; submission is therefore driven from the other side, by the ring itself, with `AcquireStaging` flushing and retrying when it cannot satisfy a request. That reaches the same one-submit-per-ring-full rule. Flushing mid-rebuild is safe because every destination is a freshly created page nothing can reach until `PublishCad2DPages` at the end.

**Texture uploads are the last holdout, and they are blocked on a fence defect rather than on effort.** `ProcessTextureUpload` still commits an upload buffer and creates its own allocator and command list per texture. It cannot simply join the ring, because it signals a fence value **reserved on another thread before the copy thread reaches the request** (three sites in `UserInterface-DirectX12.cpp`), while the copy thread bumps `copyFenceValue` once per loop iteration and once per published chunk. A reserved value can therefore be *lower* than one the copy queue has already signalled — the icon-atlas rebuild on a DPI or monitor-topology change is exactly when that happens. `GpuUploadRing::TagSubmission` pushes onto a deque that `Reclaim` pops front-first while `front().fence <= completed`, which is correct only for monotonically increasing fence values; feed it an out-of-order one and reclaim releases ring bytes the GPU is still reading, with no debug-layer warning. **Prerequisite:** move fence-value allocation to signal time on the copy thread and publish it back through `req.completionFence` — the non-UI fallback branch in `ProcessTextureUpload` already does exactly this — then have the three UI callers wait on the written-back value. That also fixes the latent non-monotonic signal on its own. Texture staging joins the ring after that, and needs an alignment argument on `Allocate` (`UpdateSubresources` places footprints at 512, the ring aligns to 256).

Remaining, in build order — every later feature funnels through the copy thread, so each item here multiplies the value of everything after it:
- [x] **Page compaction during RCU clone** — replaces the old freeze-based "VRAM defragmentation" item; see the rewritten *Defragmentation logic* section. When a page's `holeBytes` cross ~25%, its Pass-2 clone copies live ranges packed (per-object `CopyBufferRegion` from the placement records) instead of whole-page `CopyResource`. The argument-buffer rebuild — already mandatory on every clone — picks up the remapped offsets for free. At most one page compacts per chunk.
- **CPU-side segregated free-list allocator — demoted (telemetry-gated, Phase 6).** The implemented per-batch scan picks the page with the largest middle gap in O(active pages), which is fine below ~1000 pages, and compaction removes most fragmentation pressure. Build the free list only if Phase 6 telemetry shows the scan actually costing something.

**Phase 5 — structural features first, then polish**

Done so far: Shader Model 6; click / window selection — built as a GPU pick pass (object-ID render + fence-gated readback, Selection3D module) instead of the originally planned CPU raycast, plus selection-highlight and rotation-cube overlays.

- [ ] **Page-type axes — no longer a page *kind*, but PSO buckets.** Pages are currently keyed by container only and everything draws with one PSO (opaque triangles, 16-bit indices, back-culled). The shape of the fix changed with Step 7: instead of a page kind next to `containerMemoryId`, the three PSO-only axes (transparent / double-sided / line topology) become per-object `renderFlags` bits that the compute pass routes into separate output regions — one `ExecuteIndirect` per non-empty bucket, pages free to mix. Only *vertex format* stays a page property, and index width needs nothing beyond a `page.indexFormat`. This unlocks wireframe display modes, transparency and large meshes; none of the items below can start without it. Specified under *The axes do not behave alike any more* and *Appearance, variations and display state*.
- [ ] **Big-object fallback.** Wire the dormant `BigGeometryObject` path: dedicated committed resource, 32-bit indices, own draw call. Today one object above 65,536 vertices silently wraps its 16-bit indices — must land before STL / terrain import ships.
- [x] **Hot-drag / active-mutation path — the data path is done** (promoted from the known-issues list). An interactive drag would once have been a MODIFY per mouse-move — a 4 MB page clone per frame. Steps 1–4 made a whole-object move write one new instance slot plus one atomic redirect, cloning nothing; *Object placement* gave objects somewhere to record the move, so the transform-only command now has a real producer. A move persists, survives save/reload, and shows up correctly in the Properties Pane. (The per-frame matrix double-buffer once listed here as a prerequisite was *not* the right fix — no fixed frame depth is correct when render threads run at different refresh rates, and a 64-byte record cannot be written in place without tearing.) **What is left is tooling, not engine:** a translate/rotate gizmo, screen-ray-to-drag-plane projection, and an undo story (undo/redo is designed only). Rotation is also untested end to end — nothing yet produces a non-identity quaternion.
- [ ] **Transparency sorting** (needs the PSO buckets). **The original plan here — accept imperfect order during camera motion, then CPU sort + argument rebuild when the camera stops — no longer describes a path that exists.** Draw commands are GPU-authored now: the compaction pass assigns slots by `InterlockedAdd` across overlapping dispatches, so there is no CPU-side argument buffer to sort and the order varies frame to frame even with a still camera. Two viable replacements: a GPU sort pass over the transparent bucket only (it is small — the bucket holds just the transparent survivors of one Viewport), or order-independent transparency. Note that the common CAD case, **ghosting**, needs neither: drawn with depth-write off it is order-independent by construction, and gets click-through in the pick pass for free. See *Per-Viewport display state*.
- [ ] SSAO.
- [ ] **HDR output pipeline** (reworded — the vertex side is already done, colors are FP16): `rttFormat` → `R16G16B16A16_FLOAT`, tonemap draw replacing the RTT→backbuffer `CopyResource` (the full-screen-quad path returns), HDR swap chain + the startup detection rules from the *Vertex format* section.

**Phase 6 — performance & telemetry** (wire into the existing ImprovementData pipeline; this phase also settles the demoted items)

A first set of copy-thread counters already exists (`GpuCopyStats`, printed on the debug FPS heartbeat): batches, chunks, commands, pages cloned, clones-per-chunk, clone bytes, ring bytes and high-water, oversize-staging count, deferred-queue depth, max active pages, pending/free instance indices (`idx`, which moves only on REMOVE — a MODIFY keeps its index), pending/free arena slots (`slots`, which every MODIFY churns), the transform-only edit count (`moves`), visibility-mask writes and the live hidden-object count (`mask`), and retire backlog as both a live gauge and an all-time peak. They are the numbers the four workload budgets above are measured against. Note which counters are cumulative, which are live and which are high-water marks — a peak reads like a stuck value when it is only recording a stall that has since cleared. Wiring them into the ImprovementData pipeline and the Application Tab stats pane is the remaining work.

A **render-thread** counterpart now sits beside it (`GpuRenderStats`, `[gpu][cull]`): the active draw path, `drawn` — commands the last completed frame actually issued on this monitor after compaction, a live gauge fed by the fence-gated `visibleCount` read-back (Step 7) — and `commandOverflows`. Together with the copy line these read as a pair: a hide should drop `drawn` while leaving `clones` and `cloneMB` flat, and a move should leave *both* flat while `moves` climbs. That pairing is how each workload budget is actually checked.

- [ ] Per-tab VRAM usage graphs: page count, `liveBytes`, `holeBytes`, matrix-table size, big-object list size.
- [ ] Page fragmentation heatmap + compaction-trigger counters.
- [ ] Copy-thread health: batch latency and stall time at the publish fence wait (the rest is covered by `GpuCopyStats`).
- [x] Retire-backlog depth — promoted out of `_DEBUG` to a real counter. It catches the frozen-monitor-fence → unbounded-retirement failure mode, and it is what caught the per-chunk-publish VRAM exhaustion described in Step 0.
- [ ] Eviction frequency counters (ground work for Phase 7 residency).
- [x] **Right-size the per-page indirect buffer — done.** Every 4 MB page used to reserve a flat 65,536 × 24 B = 1.5 MB argument buffer, sized for a pathological page of single triangles: **27% of each page's 5.5 MB footprint**, and ~2.4 GB of pure reservation at 10M objects. Measured against a real scene — 56 pages holding ~178 objects each — that was 84 MB reserved to hold 240 KB, a ~350× over-reservation. The starting reservation is now **256 KB** (`kIndirectInitialBytes`), taking a page from 5.5 MB to ~4.25 MB.

  256 KB is deliberately *not* the measured minimum. A page densely packed with cuboids holds ~6,470 objects and fits comfortably inside it, and instancing will push objects-per-page higher still once shared geometry lets one page back many more of them — so the figure leaves room rather than tracking today's numbers. Pages that genuinely need more **grow**: `AllocateIndirectBuffer` doubles the capacity, and `RebuildIndirectBuffer` is the single growth point. That is safe there and nowhere else, because every page reaching it is a clone or a brand-new page — never a published one — so replacing the buffer cannot be observed by a render thread, and the rebuild rewrites the whole buffer anyway. A new `argGrow` counter on the heartbeat says whether the 256 KB was chosen well; a steady non-zero rate means pages routinely outgrow it. Measured 0 in testing.
- [ ] Decision gate: segregated free-list allocator (from Phase 4) — build only if the page-selection scan shows up in these numbers.

**Phase 7 — extreme performance (only after everything above is done and stable)**

- [ ] **Chained page doubling (4→8→16→32→64 MB).** Fixed 4 MB pages are right while a model's geometry sits in the low GBs, but the arithmetic fails at the top end: filling a 48 GB professional card would need ~12,000 pages — ~24,600 committed resources (each page = geometry buffer + argument buffer) and ~37,000 bind/`ExecuteIndirect` calls per frame, an order of magnitude past our 1000–5000 draw-call budget and against WDDM guidance to keep allocation counts in the low thousands (there is no hard API cap; creation cost, residency tracking and per-draw binding are what bite). Doubling each container's next-page size up to 64 MB puts the same card at ~800 pages. The code is half-ready: `GeometryPage::pageSize` is already per-page and only `CreateNewPage` hardcodes 4 MB — but `IsFull`, clone/compaction volume and argument-buffer sizing must all follow the variable size. Trigger from Phase 6 page-count telemetry; schedule before (or with) residency management below, which also gets cheaper with fewer, larger allocations. **The same variable-size machinery is also the fix in the *shrinking* direction:** a container's first page can *start* small (say 64 KB) and grow toward 4 MB, so a tab holding many tiny Scene3D containers — the case that would otherwise pay a whole page per near-empty container and tempt a flat shared-page pool (see the composition note under Step 6) — costs bytes proportional to content while keeping per-container page ownership. Right-sizing the argument buffer already took the per-container floor from 5.5 MB to ~4.25 MB; the 4 MB geometry buffer is now what dominates a near-empty container, and only variable page size addresses that.
- [ ] **Residency management** (promoted from the known-issues list): `Evict` a tab's pages a few seconds after tab switch, `MakeResident` on return, budget-driven via `IDXGIAdapter3::QueryVideoMemoryInfo` + budget-change notifications. Tab 0 always resident.
- [ ] Replace the final copy-batch CPU fence wait with GPU-side cross-queue waits (fence-tagged snapshots).
- [ ] LOD optimization based on camera distance. The instanced case is already specified in 10M plan Step 8, which selects LOD per frame in the compute pass from a library's 8 levels; what is left here is LOD for *bespoke* geometry, which has no equivalent level set to choose from.
- [ ] Compute-shader frustum culling.
- [ ] Mesh-shader implementation (supported hardware, pipes only).
- [ ] GPU-based defragmentation (compute compaction — the CPU-driven clone compaction from Phase 4 should carry us a long way first).
- [ ] Asynchronous resource creation (reduce stalls during page allocation bursts).
- [ ] Page-level optimization: static pages → single draw, semi-dynamic → EI, highly dynamic → EI + GPU compaction.

**Not to do:**

- Multi-GPU rendering (too complex for now; Windows' multi-adapter support is limited).
- Face-wise geometry colors (implementation detail; maybe needed later for mechanical parts). This is not contradicted by the per-vertex colour *variants* under *Appearance, variations and display state*: those exist for computed scalar fields and for imported meshes that arrive with colour baked in, neither of which is an authoring feature on our own primitives.

## Rendering architecture (control flow)

The rendering code is organized into three clearly separated groups, plus the two layers they sit on:

| Group | Responsibility | One-line contract |
|---|---|---|
| **Scene3D Renderer** | Render one specific 3D scene into an already-bound render target | "Given a Scene3D container + camera + viewport, record its draw commands" |
| **Page2D Renderer** | Render one specific 2D page into an already-bound render target | "Given a Page2D container + pan/zoom view + viewport, record its draw commands" |
| **Compositor** | Decide which views / active tabs appear in which window, stitch the results, present | Owns render threads, windows, swap chains, RTT→backbuffer copy, present, migration/resize |
| **UI Overlay** | Ribbon / tab bands / data tree / property pane, always drawn on top | The `UserInterface*` module — its own group, independent of the two renderers |
| **GPU Foundation** | Device, queues, copy thread, VRAM paging, upload ring | Shared singleton (`शंकर`) serving both renderers — neither 3D nor 2D specific |

Second axis: **platform separation**. Platform-agnostic code lives in one place (headers where possible). Each platform (DirectX12/Windows now; Vulkan/Linux+Android and Metal/iOS+Mac later) provides its own definitions of the *same function names*, selected at build time by compiling exactly one platform `.cpp` per module. No virtual interfaces, no `#ifdef` forests. The naming convention: `<Module>.h/.cpp` is platform-agnostic; `<Module>-<Platform>.h/.cpp` is per-platform (e.g. `RenderPage2D.cpp` vs `RenderPage2D-DirectX12.cpp`), and `GPUPlatformSelector.h` is the only file that names a platform header.

One render thread per monitor drives the whole flow top-to-bottom:

```
┌────────────────────────────────┐
│ COMPOSITOR - one render thread  │
│ per monitor:                    │
│   pick window -> tab -> view    │
│   bind that window's RTT        │
│   dispatch to a renderer  ↓     │
│   draw UI overlay on top        │
│   RTT -> backbuffer -> present  │
└───────────────┬────────────────┘
    ┌───────────┼───────────┐
    ▼           ▼           ▼
┌────────┐  ┌────────┐  ┌────────┐
│Scene3D │  │Page2D  │  │  UI    │
│camera  │  │pan/zoom│  │ribbon  │
│pick    │  │tools   │  │bands   │
│cube    │  │select  │  │tree    │
└───┬────┘  └───┬────┘  └───┬────┘
    └───────────┼───────────┘
                ▼
┌────────────────────────────────┐
│ GPU FOUNDATION (shared)         │
│   device, queues, fences        │
│   VRAM pages + upload ring      │
│   copy thread = sole VRAM       │
│   writer                        │
└────────────────────────────────┘
```

**Key boundary rule:** the two renderers (Scene3D, Page2D) never touch a swap chain, never present, and never decide *what* to draw. They receive a container `memoryId`, a view state (camera or pan/zoom) and a viewport, and record commands into a command list handed to them by the compositor. The compositor never touches geometry pages or PSOs directly. The UI overlay is always recorded **last**, by the compositor, on top of whichever renderer ran.

## 10 Million Objects + 64 SubTab Draw Plan

### Goal and workload

One tab holds up to 10 million simultaneously GPU-resident Scene3D **engineering** objects, presented through up to 64 independently filtered SubTabs. The objective is not to redraw 10 million objects in every SubTab at 60 Hz; it is to build the identity, memory and command-generation architecture that makes that scale possible, then progressively reduce each SubTab to what its camera needs.

**The figure is aspirational, and it counts engineering objects — not the graphics objects the renderer actually addresses.** The two are not the same: one engineering object can emit several graphics objects (a pipe as walls plus end caps, an object plus its centerline or its stress contour), each with its own `gpuInstanceIndex`, transform, appearance and visibility. `MV_MAX_INSTANCES_PER_TAB` bounds the *graphics* count, so the engineering count it supports is that figure divided by the average parts per object.

What a given machine actually achieves varies with its hardware, and the goal is to maximise it rather than to guarantee a number: the arena is reserved GPU address space committed on demand, the registry is committed host RAM at 40 bytes per graphics object, and geometry is whatever the model needs. Two hardware limits bound the reservation itself and belong in the same install-time check as Heap Tier 2 and `TiledResourcesTier` — `MaxGPUVirtualAddressBitsPerResource`, which can be as low as **31 bits (2 GB)** on the lowest tier and therefore caps a single tab's arena at ~33M records, and `MaxGPUVirtualAddressBitsPerProcess` across all open tabs. Sizing the reservation from the queried value rather than from a compile-time constant is what turns "varies with hardware" into behaviour instead of a caveat.

Every decision below is driven by four concrete workloads. They are the acceptance criteria:

| Workload | Must cost | Must **not** cost |
|---|---|---|
| Insert ~100 objects into a live 10M scene | Clone the 1–2 append-target geometry pages plus their argument rebuild | Anything proportional to 10M |
| Move 10–1000 objects scattered anywhere in the 10M | One new 64-byte instance record and one 4-byte redirect write per object | **Any geometry page clone.** 1000 objects spread over 1000 pages would be ~4 GB of clone traffic |
| Hide / show any subset; 64 SubTabs showing different subsets | One atomic mask write per object | Any clone, any argument rebuild, any upload |
| Several monitors at different refresh rates, one render thread each | Nothing extra | Any scheme needing a fixed number of frames in flight |

The insert path already meets its budget: today's RCU clone is O(pages touched), not O(scene). **Move and hide are what drive the rest of this design.**

### Terminology

| Term | Lifetime / responsibility |
|---|---|
| **Persistent engineering object ID** | Durable file/project identity, resolved again after loading a project. |
| **`memoryID`** | 64-bit, process-local, monotonically assigned CPU identity (`MemoryID::next`). Never reused during one process lifetime; remains the engineering-thread ↔ copy-thread command key. |
| **`gpuInstanceIndex`** | Dense 32-bit renderer identity. **Stable for the object's whole GPU lifetime** — unchanged when the object is modified, moves geometry page, or is relocated by defragmentation. Reusable only after fence-safe retirement, paired with a CPU-side generation counter. |
| **`instanceSlot`** | Physical location of the object's 64-byte record in the instance arena. **Changes on every transform edit.** Known only to the copy thread and the redirect table. |
| **SubTab** | Content selection: a set of Scene3D containers, or a set of Page2D containers, plus filter and display state. Replaces the older overloaded use of “view”. |
| **Viewport** | Camera, input ownership, render-target rectangle, presentation state. References a SubTab; several Viewports may show one SubTab with different cameras. |

64 SubTabs per tab is the design limit, represented directly by a 64-bit membership word (`uint2` in shaders). `MV_MAX_SUBTABS` is **also 64**, deliberately: the slot array used to be 128 and the mismatch bought nothing except a second class of slot that could not be mask-addressed and had to fall back to showing everything. Equalising them makes every open SubTab addressable and that fallback unreachable. Going past 64 therefore means adding a second mask word *and* enlarging the array — not one or the other.

Every tab owns its own stores and snapshots because tabs are independent. Device, copy queue, upload ring, heap arena, memory budget and fence retirement stay global, so the application keeps one coherent VRAM budget.

### Invariants

These four rules are load-bearing. Every step below preserves them.

1. **Geometry pages and instance data are never associated.** An object's geometry page is a movable location; its `gpuInstanceIndex` is stable identity. Tying them together would mean a transform edit clones geometry, and a page change breaks identity.
2. **Nothing a published frame can read is ever mutated — with exactly one exception:** a single naturally-aligned store of ≤ 8 bytes whose old and new values are *both valid*. Reads are then old-or-new, never torn. This is a hardware guarantee, not a D3D12 API guarantee; D3D12 is silent on the question and its debug layer will not flag the race either way.
3. **Everything shared per-tab retires on `min(all monitor render fences)`.** Render threads run at their monitors' refresh rates and never coordinate, so no fixed frame depth is correct for a shared resource.
4. **Per-window / per-Viewport resources have exactly one owning render thread**, so classic N-deep buffering against that thread's own fence *is* correct for them.

Why instance data is not paged alongside geometry: a 4 MB geometry page holds anywhere between ~170 objects (a 36×18 sphere) and ~6,470 (a cuboid), so the two page systems can never line up; objects change geometry page on modify and on defragmentation, which would destroy index stability, and the GPU pick id *is* the instance identity; and a per-page instance buffer forces one `ExecuteIndirect` per page forever, forfeiting the single-call-per-Viewport goal of Step 7.

### Per-tab stores

| Store | Unit | Bytes at 10M | Mutability |
|---|---|---:|---|
| GeometryStore | 4 MB double-ended pages | ~6.5 GB for trivial solids | Immutable after publish; RCU clone on change |
| InstanceArena | 64-byte records, slot-allocated | 640 MB live + hole overhead | **Records immutable while published**; edits write a new slot |
| InstanceRegistry (CPU) | 40-byte entry per `gpuInstanceIndex` | 400 MB host RAM | Copy-thread owned, read lock-free by the pick resolve |
| InstanceSlotOf | `uint` per `gpuInstanceIndex` | 40 MB | Mutated in place, one aligned 4-byte store |
| VisibilityMask | `uint2` per `gpuInstanceIndex` | 80 MB | Mutated in place, aligned stores |
| DrawTemplates | one per object | ~320 MB | Immutable; rebuilt when its geometry page is cloned |
| VisibleIndirect | per active Viewport | capped, see Step 7 | Transient GPU output |

The arena, the redirect table and the mask are all indexed by the same dense `gpuInstanceIndex`, and none of them knows anything about geometry pages. The registry is the CPU-side half of that same index space — it is the *only* store here that maps back to `memoryID` and to a geometry page, and it never reaches the GPU. Both the arena and the registry are reserve-then-commit allocations (GPU tiles, host pages), so those byte figures are the fully-populated ceiling, not what an open tab costs.

### The write model

This is the heart of the design; everything else follows from it.

```hlsl
uint slot = InstanceSlotOf[gpuInstanceIndex];  // 4 B, atomically rewritten on edit
InstanceRecord r = Instances[slot];            // immutable while any frame can read it
uint2 vis = VisibilityMask[gpuInstanceIndex];  // aligned, old-or-new
```

- **Transform edit:** allocate a fresh `instanceSlot`, write the record there (no frame can reference it yet), then flip `InstanceSlotOf[idx]` with one aligned 4-byte store. The old slot goes on the min-fence-gated free list. A concurrent reader sees the old or the new slot — both are valid transforms, so the worst case is one frame of staleness on one monitor. **No geometry page is touched and nothing is cloned.**
- **Hide / show and SubTab membership:** one aligned write to the mask. Each Viewport tests one bit, which lives in one word, so even a torn 8-byte mask cannot be observed inconsistently by any single reader.
- **Why records are never mutated in place:** the arena is write-combined memory, so a 64-byte `memcpy` can be observed half-old and half-new — a garbage matrix, not a stale one. The redirect exists precisely to turn a 64-byte update into a 4-byte atomic one.

Dragging burns one slot per edit, but steady state ping-pongs between 2–3 slots per object: a fresh slot is needed only while the previous one is still referenced by an in-flight frame. Slots freed by moves accumulate as holes and are reclaimed by the defragmentation pass in Step 9. Index contiguity is deliberately **not** maintained during normal operation.

### Step 0 — Upload ring and ring-gated submission *(implemented)*

Two Phase 4 items are hard prerequisites for everything below, and they are really one mechanism. This step is the authoritative specification for both; the Phase 4 entries point here.

**The ring.** One persistent-mapped upload buffer per process — 64 MB to start — serving *all* copy-thread staging: Scene3D geometry, indirect-argument and draw-template rebuilds, Page2D record uploads, texture uploads. (Everything but textures is there today; see the Phase 4 entry for the fence defect that gates the last one.) Allocation is a bump pointer with wraparound; every allocation is tagged with the copy-fence value that will release it, and a region becomes reusable once `copyFence->GetCompletedValue()` has passed that value. Today every object upload creates its own committed staging resource in `RecordGeometryUpload`, which is exactly the stall the original roadmap predicted. The dead per-tab upload heaps go at the same time: `InitD3DPerTab` commits and persistently maps 64 MB (vertex) + 16 MB (index) that nothing ever writes — ~80 MB per tab, directly against the "Hello-World tabs stay lightweight" rule.

**Submission is driven by ring capacity, not by pass boundaries.** This is what makes bulk import work. Model loading hands the copy thread lakhs of objects at once, and the ring must never be asked to hold more than it has:

```text
open command list
for each command in the drained batch:
    need = vertexBytes + indexBytes          // or record / template / texture bytes
    if the ring cannot satisfy `need`:
        Close + ExecuteCommandLists + Signal(copyFence)   // flush what is staged
        wait until the oldest tagged region's fence releases enough space
        Reset allocator + command list                    // rotate 2-3 allocators
    write into the ring; record CopyBufferRegion
Close + ExecuteCommandLists + Signal + wait; publish
```

The rule is therefore **one submit per ring-full**. For an ordinary interactive edit — a handful of objects — that degenerates to exactly one submit per batch, which was the original intent. The CPU fence wait at a flush is not a stall to be optimised away: it *is* the back-pressure that throttles the engineering thread's production to the GPU's ingestion rate, and it is the only thing keeping staging memory bounded during import.

Within one chunk, clone → upload → argument rebuild is a **single** recording. The copy queue executes strictly in order, so a clone completes before the copies that write into it; the three record / execute / CPU-wait cycles the batch path performs today are unnecessary. One CPU wait remains, immediately before publish. (Removing even that, via GPU-side cross-queue waits, stays Phase 7.)

**Publish per chunk, not per batch.** Each chunk ends with fully uploaded pages and rebuilt arguments, so publishing it is safe — and the user watches the model appear progressively instead of waiting through a multi-second freeze. A page left partially filled at a chunk boundary is simply re-cloned by the next chunk: 4 MB of extra copy per chunk, noise against 64 MB. Preferring to flush at the moment the current append page fills avoids even that.

**Retirement must be swept between chunks — this is a correctness requirement, not an optimisation.** Publishing per chunk retires the append-target page on *every* chunk, but the fence-gated reclaim sweep historically ran once per copy-thread iteration, i.e. only after the whole batch returned. A batch producing many chunks therefore accumulated retired 5.5 MB pages (geometry + argument buffer) with nothing freeing them, and exhausted VRAM outright — `E_OUTOFMEMORY` partway through a bulk import, observed in testing before the fix. The sweep is now a callable function invoked both per copy-thread iteration and after each published chunk. Because reclaim is gated on `min(all monitor render fences)`, a copy thread that outruns the monitors can still accumulate; so when the backlog stays above a cap (~64 pages) the chunk loop takes a few short bounded waits before continuing, which throttles the copy thread against the renderer instead of letting retention grow without bound. The waits are bounded so a frozen monitor degrades to the old behaviour — visible in the retire-backlog counter — rather than hanging the copy thread.

**Oversize fallback.** An upload larger than the entire ring — a jumbo STL mesh — gets a one-off committed staging buffer, so it can never deadlock waiting for space that will never exist.

**Cap the CPU-side drain too.** The ring bounds GPU staging; it does nothing for the `std::vector<CommandToCopyThread>` that `GpuCopyThread` fills with `while (!commandToCopyThreadQueue.empty())`. Each command carries a `GeometryData` holding two heap vectors, so lakhs of objects materialise as hundreds of megabytes and millions of small allocations before a single byte reaches the GPU. Drain until an estimated-bytes cap (a small multiple of the ring) instead of until the queue is empty, and leave the remainder queued — the queue then supplies upstream back-pressure. This closes the open throttling TODO in `GpuCopyThread`.

**Sizing sanity check**, taking one ring fill as the unit of work:

| Object | Bytes each | Objects per 64 MB fill |
|---|---:|---:|
| Cuboid (24 vertices, 36 indices) | 648 B | ~100,000 |
| Sphere (36 × 18) | ~24 KB | ~2,700 |

A lakh of cuboids is roughly one ring fill; a lakh of spheres is ~24 fills and ~1.5 GB of geometry. Argument and template staging is small beside this — even a densely packed 4 MB page rebuilds in ~155 KB.

### Step 1 — Fence-gated slot retirement *(implemented)*

The bug this fixes: `REMOVE` and `MODIFY` returned the matrix slot to `freeMatrixSlots` immediately, so a later `ADD` in the same batch could take it and overwrite the transform while in-flight frames still drew the pre-publish snapshot — one frame of an object wearing a stranger's transform. Highly visible, unlike the benign one-frame staleness discussed elsewhere.

Slots vacated during a chunk now collect in a per-chunk list, are tagged at publish time with the global render fence, and are handed back to the free list only by the `safeRetireFence` sweep — the same rule and the same sweep that governs retired pages and snapshots. Small, self-contained, and the discipline the redirect table in Step 4 depends on. (Step 3 renamed this machinery from matrix slots to instance indices and made it REMOVE-only; outgrown matrix tables, the third thing the sweep used to free, no longer exist at all.)

*Done when:* a REMOVE + ADD in one batch cannot reuse a slot until every monitor's fence has passed it. The `slots(pending/free)` telemetry counter shows the holding list draining.

### Step 2 — Reserved-tile instance arena *(implemented)*

The per-tab world-matrix buffer is now one **reserved (tiled) device-local resource** per tab: `MV_MAX_INSTANCES_PER_TAB` (10,485,760) records × 64 B = 640 MB of GPU virtual address reserved at tab creation, with physical 64 KB tiles committed on demand through `UpdateTileMappings` as the arena grows. 64 KB = 1024 records, so growth is fine-grained and an empty tab costs zero physical bytes.

The point is not merely avoiding a reallocation. The virtual address never changes for the tab's lifetime, which deleted:

- the whole-table copy in `GrowMatrixTable`, unaffordable at 640 MB;
- the `retiredBuffers` path for matrix tables, and with it the "640 MB pinned until the slowest monitor catches up" hazard — `TabGeometryStorage` no longer has a retired-buffer queue at all;
- the `worldMatrixVAShared` / `worldMatrixDataShared` / `matrixCapacityShared` mirrors and their publish ordering, which existed *only* because the buffer address moved. All four readers (scene, pick, highlight, `PrinterController`) now bind a plain `instanceArenaVA` written once at creation.

*As implemented:*

- **One heap per growth step, not one per tile.** The step doubles from 4096 records (4 tiles, 256 KB), so ~12 heaps back the whole 640 MB range. One 64 KB heap per tile would be 10,240 allocations, against the same WDDM low-thousands guidance that motivates chained page doubling in Phase 7.
- **`UpdateTileMappings` is issued on the copy queue.** It is a queue operation, not a command-list one; the copy thread owns arena growth, and the mapping is enqueued ahead of the `ExecuteCommandLists` that writes records into the new tiles. Growth needs no fence gating either, because newly committed tiles back indices beyond `instanceCount` that no published snapshot can reference. Verified against `TiledResourcesTier 3` hardware.
- **Records now ride the upload ring.** A device-local arena is not CPU-mappable, so the plain `XMStoreFloat4x4` into a persistently mapped upload heap became a 64-byte ring staging plus one `CopyBufferRegion` per record, in the same chunk recording as the geometry. `EstimateStagingBytes` accounts for it.
- **The pick resolve's world-space AABB centre moved onto a CPU-side transform shadow** — three floats per instance inside the Step 3 registry entry, written by the copy thread at upload time. Putting it in the pick shader was the alternative; the shader has no access to an object's AABB, so the shadow is both cheaper and the only one of the two that actually works without a second per-object GPU buffer.

`TiledResourcesTier >= 1` (buffers) is a baseline requirement, to be verified at install time alongside the existing Heap Tier 2 requirement; `InitD3DDeviceOnly` queries and logs the tier as a startup backstop.

### Step 3 — Stable `gpuInstanceIndex` and the copy-thread registry *(implemented)*

Identity is separated from location. The copy thread is the sole owner of a **per-tab** registry:

```text
memoryID          -> gpuInstanceIndex     the only hash map
gpuInstanceIndex  -> { memoryID, GeometryLocation(page, pageSlot),
                       instanceSlot, worldCentre }                 flat array
```

Only the first is a hash map; because the index is dense, everything else is a flat array. This matters concretely — three `unordered_map`s at 10M entries would cost well over a gigabyte of CPU node overhead, and the single global `objectLocation` map this replaced was already on that trajectory.

*As implemented*, the reverse direction is **one array of 40-byte entries** rather than the three separate arrays sketched above (32 bytes until Step 4 added the object's current `instanceSlot` to it): the same bytes, one commit path, and one cache line per lookup — which is exactly what a pick does. The array is a `VirtualMemory` reservation with 64 KB blocks committed as the index space grows, mirroring the GPU arena. That is not only about sparseness: the *render* threads read entries, so the base address must never move under them, which a `std::vector` cannot promise. The `worldCentre` field is Step 2's transform shadow.

The `gpuInstanceIndex -> generation` array is **deliberately not built yet.** Nothing caches a GPU handle to validate against it, and the zombie interval below is already enforced by the fence-gated free list. Add the counter with the first handle cache.

`gpuInstanceIndex` is identity, never a page location: it does not change when the object is modified or when an RCU clone moves it between GeometryPages. The clone step therefore re-points the registry with a direct array write per live record — the identity lookup this used to need is gone. Engineering code keeps addressing objects by `memoryID` and never learns page coordinates or GPU indices. A transient handle may be cached in `META_DATA` but is never persisted to disk.

The GPU pick id is now `gpuInstanceIndex + 1` (it was `matrixIndex + 1`), so a pick resolves in O(1) through the registry instead of a linear scan over every object of every page. `IndirectCommand` carries `gpuInstanceIndex` in place of `matrixIndex` and stays 24 bytes.

Deletion uses a zombie interval:

```text
active -> absent from newly published snapshots -> zombie -> all old frames retire -> reusable index
```

Only once every snapshot that could reference the index is past its retire fence may the index return to the free list. This prevents a stale command or an old page from addressing a newly assigned object. Mechanically it is the same holding list and the same `safeRetireFence` sweep that Step 1 built for matrix slots; what changed is that **MODIFY no longer vacates anything**, because identity survives a modify. Only REMOVE starts a zombie interval.

**A regression this step opened, and Step 4 closed.** With the index stable but no redirect table yet, the arena was addressed directly by `gpuInstanceIndex`, so a MODIFY rewrote the record *in place*. A frame still drawing the pre-publish snapshot read that index while the copy queue overwrote it — one frame of a stale, or if the 64-byte copy were observed mid-write garbled, transform. That is invariant 2's hazard, and it was accepted for exactly one step because interactive 3D drag does not exist yet, so MODIFY fires at property-edit rates. Step 4's redirect removed it: a fresh `instanceSlot` plus one aligned 4-byte flip.

### Step 4 — Instance redirect table (the move path) *(implemented)*

`InstanceSlotOf[gpuInstanceIndex]` is a second reserved-tile buffer alongside the arena (4 bytes per index, 40 MB of reserved VA, one 64 KB tile = 16384 entries), and the scene and pick vertex shaders now use the two-load form shown in *The write model*. Besides being the move path, this closed the in-place-record-write window Step 3 knowingly opened. `MODIFY` splits into two paths:

- **transform-only** → new slot, write record, atomic redirect flip. No geometry page clone, no argument rebuild, nothing published.
- **geometry changed** → the existing path (relocate into a cloned page, rebuild that page's arguments) plus a new slot.

This is the step that pays for the whole design: "move 1000 scattered objects" drops from up to 4 GB of page cloning to ~68 KB of writes. *Done when:* moving N scattered objects clones zero geometry pages.

*As implemented:*

- **The flip is a 4-byte `CopyBufferRegion` on the copy queue, not a CPU store.** Both buffers are device-local, so the record write and the flip share one ring allocation and one command list; the copy queue's strict in-order execution is what guarantees the record lands before the redirect that publishes it — the same guarantee the page clone already relies on. The 4-byte write is naturally aligned and both its old and new values are valid slots, which is invariant 2 exactly.
- **A transform-only chunk publishes nothing, and that turned out to be a trap.** The per-chunk handover of vacated identities sat *after* the "nothing to publish → continue" early exit, so a chunk of pure moves would have leaked one slot per edit. The handover is now a lambda called from both exits. It still has to run *after* `PublishPages` on the publishing path: the fence it reads must be at or beyond the one the retired pages were tagged with, or a frame submitted in between still draws the pre-publish snapshot naming a just-freed index.
- **Two free lists, two different churn rates.** An index is vacated only by REMOVE; a slot is vacated by every MODIFY. Both ride the same `safeRetireFence` sweep, and the heartbeat now reports them separately (`idx(pending/free)`, `slots(pending/free)`, `moves`).
- **The transform-only path now has a producer.** It is keyed off a MODIFY whose payload carries a world matrix but empty vertex/index vectors (`IsTransformOnlyEdit`). That is emitted by `TranslateSelectedSceneObjects`, which writes each selected object's rigid **placement** and lets `GeometryForObject` compose it into `GeometryData::worldMatrix` — see *Object placement* below. Two debug keys drive the path: `m` (a raw matrix for every object in the container, predating the placement and touching no stored state) and `v` (the real producer, on the selection). Measured: `moves` climbs while `clones` and `cloneMB` stay flat.

  *What this cost, against the estimate.* An earlier revision of this section claimed the only remaining prerequisite was "give the generators a real world matrix". That badly understated it: `META_DATA` had **no transform field at all**, and all 15 3D types store absolute world coordinates in their own members (`SPHERE::center`, `CYLINDER::p1/p2`, `CUBOID::vertices`, …). Persistence is one type-specific proto blob per object with **no shared envelope**, so the placement had to be added to all 15 `.proto` files. It was a data-model and schema change, not a generator tweak.

- **The "clones zero geometry pages" criterion was not actually met until later, and it took two fixes, not one.** The chunk's *Pass 1* built its affected-page set from every non-ADD command, so a move dragged its own page in and cloned ~5.5 MB to publish a byte-identical replacement. Less obviously, the append-candidate scan that follows gates on `cmd.geometry.has_value()` — which is **true** for a transform-only edit, because carrying the world matrix is precisely what the otherwise-empty `GeometryData` is for — so even after Pass 1 was fixed, a chunk of pure moves still force-cloned one append page per container. Both scans now skip `IsTransformOnlyEdit`. Measured on the heartbeat: 27 moves, `clones` and `cloneMB` unchanged, scene visibly translated. The lesson generalises to anything else that rides the MODIFY command — *two* independent scans decide what gets cloned, and an edit that touches no bytes must opt out of both.

An InstanceRecord is exactly 64 bytes:

```cpp
struct InstanceRecord {
    float4 transformA;     // 48 bytes across transformA/B/C: affine object transform
    float4 transformB;
    float4 transformC;
    uint   materialIndex;  // 16-byte object render payload
    uint   packedColor;
    uint   renderFlags;
    uint   packedParams;   // e.g. opacity plus a future scalar parameter
}; // 64 bytes
```

The final affine row is implicit, so shaders must use an explicit affine-transform helper — the record is no longer safe to hand to a generic `float4x4` `mul`. The convention, settled byte-for-byte: **`transformA/B/C` are the first three rows of `transpose(world)`, i.e. the three columns of the row-vector world matrix.**

```text
transformA = (W00, W10, W20, tx)      point  : dot(float4(p,1), transformA/B/C)
transformB = (W01, W11, W21, ty)      normal : dot(n, transformA/B/C .xyz)
transformC = (W02, W12, W22, tz)      dropped row is always (0,0,0,1)
```

That is byte-identical to the leading 48 bytes of the `XMFLOAT4X4` the arena stored before this step, so the CPU writer is still one `XMMatrixTranspose` — it just stops copying the last row. Consumers that changed together: `ShaderSceneVertex`, `ShaderScenePickVertex` (which duplicates the struct — there is no `.hlsli` include mechanism in the build, so the two definitions carry cross-reference comments) and the highlight path, which reuses the scene vertex shader. The rotation-cube overlay did **not** change: it has its own root signature and never reads instance data. The CPU pick resolve did not change either — since Step 2 it reads the registry's world-centre shadow rather than a matrix.

The three transform vectors retain the 3×3 part used for normals; the uniform-scale assumption stands, with an inverse-transpose or separate normal-transform policy left to a later revision.

Appearance is packed into the record for authored, infrequently changed state: material, base colour, opaque/transparent classification, opacity, render flags. High-frequency interaction state (hover, selection, SubTab membership, temporary hide) stays **out** of it — that state lives in Step 5's mask, so an interaction never allocates a slot.

**Nothing writes or reads those 16 bytes yet, and there is a defect waiting there for whoever does.** A transform-only MODIFY allocates a *fresh* slot and writes all 64 bytes of a new record — but the copy thread has no source for the appearance half. `InstanceRegistryEntry` carries `memoryID`, page, pageSlot, `instanceSlot` and `worldCentre`, and no appearance; the arena is device-local and cannot be read back. So the moment appearance becomes real, **dragging an object silently resets its colour and opacity**. The fix belongs in the same change: carry the 16 bytes in the registry entry as the CPU shadow, 40 → 56 bytes, still one cache line. See *Appearance, variations and display state*.

### Object placement — the producer side of the move path *(implemented)*

Steps 1–4 make the GPU able to move an object for the price of one instance record and one redirect flip. Nothing could *ask* for that until an object had somewhere to record where it had been moved to. `Placement3D` is that field:

```cpp
struct Placement3D {
    XMFLOAT3 origin;    // translation
    XMFLOAT4 rotation;  // unit quaternion; identity = never moved
};
```

One per 3D object, on all 15 types, persisted as `Placement placement = 20` — **field number 20 in every 3D proto message**, by convention, so the rule is stateable rather than fifteen separate facts. Absent means identity, which is what every object written before the field existed decodes to.

**It is rigid on purpose.** No scale factor: the `InstanceRecord` already documents a uniform-scale assumption, and rigidity is what makes every scalar dimension (radius, diameter, section parameter) invariant under a placement, so only *point* fields ever need converting between spaces.

**`GeometryForObject` is the single point where a placement takes effect** — it composes the placement into `GeometryData::worldMatrix` after the per-type geometry switch. Every ADD, geometry MODIFY, file load and import inherits it there without knowing it exists. Three creation paths in `विश्वकर्मा.cpp` still call `GetGeometry()` directly and bypass this; harmless while new objects are unplaced, but an object *created* with a placement will not show it until reload.

**A move writes the placement and emits a transform-only MODIFY.** No geometry is regenerated, so no page is cloned — measured on the heartbeat as `moves` climbing while `clones` and `cloneMB` stay flat.

*Consequences that are easy to miss, and both were live defects until fixed:*

- **Stored coordinates are authored, not world.** Anything reading `SPHERE::center` or `CYLINDER::p1` directly gets where the object was *drawn*. The Properties Pane therefore composes on read and solves back on write, so the user still sees and types world coordinates; an edit to one world component rewrites **all three** authored components, because under a rotation each depends on all three. Point triples are declared per type (`pointGroupFirstField`), not inferred from "the first three fields are a point".
- **Any consumer of `geometry.vertices` for spatial reasoning must transform first.** Zoom-to-fit computed camera extents from authored vertices and framed moved objects at their old positions until it was fixed. Clash detection, export and bounds will each have the same obligation.

### Step 5 — Visibility mask *(implemented)*

`VisibilityMask[gpuInstanceIndex]` is a third reserved-tile buffer alongside the arena and the redirect table — one 64-bit SubTab membership word, `uint2` in shaders for broad compatibility — plus a per-draw SubTab bit index. CPU-side membership changes stay keyed by `memoryID`; the copy thread resolves them to dense indices and writes the mask directly under invariant 2.

Two consumption paths, in this order:

- **Interim, with today's per-page `ExecuteIndirect`:** the vertex shader tests the bit and emits a degenerate position when it is clear. Toggling is one atomic write — no rebuild, no clone, no upload. Vertex shading still runs for hidden geometry, which is the accepted interim cost.
- **Final, in Step 7:** compute compaction drops hidden objects before they ever become draw commands.

Per-object hide therefore ships well before the compute path exists. Reusing a SubTab bit requires clearing its prior membership first.

Note that whole-page container filtering — `ExecuteIndirect` argument count 0 when `page.containerMemoryId` is not the active container — is a *page* mechanism, not per-object hide. It stays as a cheap first-level reject.

*As implemented:*

- **The bit index is the sub-tab slot**, passed as a root constant (`b2`) in its own range rather than widened into `b1` — `b1` is what the command signature rewrites per command, so a render-thread value sharing that range would be overwritten by `ExecuteIndirect`. A view with no bit passes a sentinel (`kNoSubTabBit`) that disables the test and shows everything. That used to catch sub-tab slots past 63, back when `MV_MAX_SUBTABS` was 128 against a 64-bit mask; with both now 64 the only remaining caller is the print path, which draws with no sub-tab at all.
- **All four readers must bind it, not just the scene.** Scene, selection highlight, GPU pick and `PrinterController` share one root signature, and `t2` is a *root* descriptor — no bounds check, no null check — so a path that skips the bind reads garbage rather than failing loudly. The pick pass applies the identical bit: an object the user cannot see must not be clickable, and the print path uses the on-screen sub-tab's bit so a print matches what is displayed, hides included.
- **The default is written explicitly on every ADD.** A freshly committed D3D12 tile has undefined contents, so an unwritten mask is garbage rather than zero — and a recycled `gpuInstanceIndex` would otherwise inherit the hides of whatever owned it before. That costs 8 bytes of ring staging per object added, accounted for in `EstimateStagingBytes`.
- **Mask commands must stay out of the batch deduplication pass.** That pass keys on `memoryID` alone, so an ADD and a hide of the same object in one batch would collapse to whichever came last — and if that were the hide, the geometry would silently never be uploaded. They are also excluded from the affected-page scan, since pulling an object's page in there would clone 4 MB to change 8 bytes, which is precisely the cost this step exists to avoid. Each mask command names one bit rather than a whole word, so they are applied in order and never collapsed.
- **A hidden-object shadow replaces the compute dispatch.** The copy thread keeps a map of only those indices whose mask is not all-ones. It answers "what is this object's current word" without reading back from device-local memory, and it bounds the clear-on-slot-reuse sweep by the number of hidden objects instead of by the index space — so the "one compute dispatch over the mask array" this section used to require is not needed, and neither is the shader-visible descriptor heap that is a Step 7 prerequisite. The sweep is capped per chunk and re-queues its remainder, because a single command that fans out over millions of entries would otherwise overrun the upload ring and fall back to a committed buffer *per entry*.
- **The bit is restored at the fence-gated FREE transition, not at close.** Frames still drawing a closing view keep their hides until they retire, instead of objects popping back mid-flight. It also has to happen there for a locking reason: the retire path runs under `storageObjectsMutex`, and enqueueing there would nest it inside `toCopyThreadMutex` — against the never-nested discipline the geometry producers follow, and a way to stall every render thread (they take `storageObjectsMutex` each frame resolving the window's view) behind a copy-thread drain.
- **Producers:** the `HIDE_SELECTED` / `HIDE_UNSELECTED` / `HIDE_RESET` ribbon buttons, which existed as unwired rows. Each touches only the objects it names — "Hide Selected" does not silently un-hide everything else — so the three compose the way a user expects, and an empty selection makes the two hide actions no-ops rather than blanking the view. Hide state is session-only; nothing is persisted.
- Appearance state stays split as designed: authored, infrequently changed state (material, colour, opacity) lives in the 64-byte `InstanceRecord`; hover, selection and hide live here, so an interaction never allocates an arena slot.

### Step 6 — SubTabs, Viewports and the container-set directory *(implemented)*

1. Rename the content-level “view” concept to **SubTab**. A SubTab has exactly one content type — Scene3D or Page2D, never mixed, because a mixed SubTab would have ambiguous renderer and interaction semantics — and holds a *set* of containers of that type. `VIEW_INSIDE_DATASETTAB`, `tab.views` and `activeViewIndex` are dead code referenced only from a comment; delete them rather than renaming them.
2. Introduce **Viewport** as a separate object owning the Scene3D camera (or Page2D pan/zoom), input/pick state, render-target rectangle and update scheduling. Per-view cameras and Page2D pan/zoom already exist per sub-tab slot; this step lifts them out so several Viewports can show one SubTab with different cameras, and so a window can later host several side by side. The shared `DX12ResourcesPerTab::camera` write-through is already gone — the camera is passed down as a parameter.
3. Replace one-container-only render selection with a SubTab container set. When the set alone defines the subset, store it as a compact rule; do not set a per-object mask bit for every member merely to express "this whole container".
4. Add a snapshot-level `containerMemoryId -> GeometryPage list` directory, so a SubTab that selects a few containers stops walking every page in the tab. Independently of that directory, hoist the container test above the vertex/index buffer binds in `RenderScene3D`: today every page pays two IA binds before its argument count is discovered to be zero.

*As implemented:*

- **`SubTabContainerSet` is inline storage, not a vector.** Render threads read it lock-free every frame, so a heap buffer the engineering thread could reallocate underneath them is exactly the hazard to avoid; the compositor copies the set by value once per frame and the render thread owns that copy for the frame. It is also the "compact rule" the step asks for — container membership is one entry here, never a per-object mask bit, so opening a SubTab stays O(1) instead of O(objects).
- **The directory subsumed the hoist.** Items 3 and 4 turned out to be one change: once the draw loop iterates *the set's* containers through `pagesByContainer`, pages of other containers are never visited, so there is nothing left to hoist above the IA binds — and the `ExecuteIndirect`-with-count-0 trick that used to express "wrong container" is gone entirely. The pick and highlight paths share one `ForEachSubTabPage` helper. The scene draw loop (`RenderScene3D`) and the print collector (`Collect3DPages`) each carry their own copy of the same walk — three copies of the predicate, not one, because the print path takes `ComPtr` copies rather than visiting pages in place. They agree today; `RenderScene3D`'s copy is the one that has since grown a compute-cull branch, so it is where a divergence would appear first.
- **`PageIsRenderable` lost its container argument.** Reaching pages through the directory means a page a caller can see already belongs to the SubTab; leaving the test in would have been a second, redundant source of truth.
- **The Viewport lift was small because the accessors already existed.** Camera access had already been funnelled through `ActiveSceneCamera` and Page2D pan/zoom through `Cad2DInputView`, so moving both into `Viewport` touched about ten call sites rather than the sprawl it would have been a few phases ago. `TabCad2DStorage::views[]` moved out wholesale: pan/zoom is *view* state, and it now sits beside the camera in the object that owns both.
- **`RenderPage2D` now takes a `const Cad2DViewState&` instead of a slot index**, mirroring how the Scene3D renderer takes a camera. That is the *Key boundary rule* above finally holding on both sides: each renderer receives a container, a view state and a viewport, and neither reaches for view state itself.
- **Viewport-to-SubTab is 1:1 today** (`viewports[i]` drives `subTabs[i]`), recorded in `Viewport::subTabSlot` so the mapping is a stored fact rather than an assumption baked into every reader. Nothing outside the open/close path assumes it any more, which is the whole point — a second Viewport onto one SubTab now needs a slot allocator, not a refactor.
- Deliberately **not** built: the render-target rectangle and update scheduling fields named in item 2. The rectangle is still derived per frame by the compositor and scheduling does not exist yet — those belong with Step 11's Viewport classes, and adding empty fields now would only invite them to drift out of date.

*Composing multiple containers into one Viewport (drag-to-compose):* item 3's container set was seeded with exactly one container until now — a Building (Civil) and its Plumbing (Mechanical) could coexist in a tab but not in one view. The set is now populated at runtime: dragging a Scene3D out of the data tree and dropping it on the inline scene appends its `containerMemoryId` (`ADD_CONTAINER_TO_SUBTAB`), and a top-centre chip strip removes composed containers again (`REMOVE_CONTAINER_FROM_SUBTAB`); the home container is never removable. It is **home-plus-composable** — a Scene3D keeps at most one home SubTab (double-click opens/focuses it) and is composed into others by reference. New geometry still parents to the home container.

- **Composition is a container-SET operation, not a 64-bit-mask one, and this is the load-bearing decision.** Adding a whole Scene3D to a view is one O(1) append; its objects show by default because the mask is all-ones, so no per-object write is needed. The mask stays what Step 5 made it: per-*object* hide *within* the composed set. Expressing whole-container membership through the mask instead would be O(objects) per compose and would burn one of the 64 bits per view — exactly what item 3 warns against.
- **Geometry is never duplicated by showing one Scene3D in several SubTabs.** Pages are stored once per container and referenced by `containerMemoryId` from each SubTab's set; several SubTabs listing the same container draw the *same* GPU buffers with different cameras. (Repeated *placements* of one Scene3D reuse geometry too — that is what the instance arena is for.) This is why per-container page ownership was kept rather than switching to a flat shared page pool: flat pages would force every active Viewport to cull *all* tab objects each frame (cost ∝ active-SubTabs × total objects, with no spatial acceleration yet) and make closing / deleting / evicting a whole Scene3D O(objects) + defrag instead of dropping its pages. Flat pages + mask-only filtering is the **Step 10 end-state** (once a spatial hierarchy provides the coarse reject the container set provides today), not an interim option. The tiny-many-containers page-waste that motivates flat packing is addressed separately — see the page-sizing note under Phase 7 — not by going flat.
- **The two Step 4/5 producers were left behind by composition — fixed.** `TranslateSelectedSceneObjects` (move) and `ApplySceneVisibilityAction` (hide) both filtered objects by `subTabs[slot].containerMemoryId`, the *home* container, and skipped anything whose `memoryIDParent` differed. The pick pass did not: it walks the whole set through `ForEachSubTabPage`. So an object in a *composed* container could be selected and would then silently refuse to move or hide. Both now go through one `SubTabDrawsContainer` predicate that tests the container SET, falling back to the home container when the set is empty exactly as `ResolveWindowViewTarget` does — verified by moving an object belonging to a composed container and watching the producer report `1 object(s) translated` where it previously reported none. This is the general shape of the hazard, worth stating once: **composition made "the view's container" a set everywhere except in code written before it**, so every consumer that resolves a Viewport to a single `containerMemoryId` is suspect.
- Deliberately **not** built (deferred): dropping onto an extracted `WINDOW_KIND_VIEW` window (cross-window mouse tracking); several side-by-side Viewports in one window (needs the Viewport slot allocator noted above); more than `MV_MAX_CONTAINERS_PER_SUBTAB` (8) containers per SubTab; and the "fully independent copies" multiplicity where double-click always spawns a fresh SubTab of an already-open container.

### Step 7 — SceneEpoch and GPU indirect generation *(draw path implemented; SceneEpoch and template retirement deferred)*

The two halves of this step turned out to be separable, which the design did not anticipate. **The draw-call half has shipped: a Viewport is one `ExecuteIndirect`.** The publishing half — the `SceneEpoch` directory, the revision-keyed cull cache and the retirement of the per-page indirect buffers — has not, and is no longer a prerequisite for it. Read the design below as written, then the *As implemented* notes for what that reordering cost and bought.

Geometry pages, draw templates and later spatial pages cannot be published as unrelated “latest” resources. Publish one atomic directory instead:

```text
SceneEpoch
  GeometryPage directory
  DrawTemplate directory
  container -> page directory
  revision numbers
```

A render thread acquires one `SceneEpoch` and binds only what is reachable from it. The instance arena, redirect table and mask are deliberately **not** in the epoch: they are mutated under invariant 2 rather than republished, and that is exactly what keeps moves and hides free of clone traffic.

Then retire the *persistent executable* per-page indirect buffers — without removing draw information from the GPU:

```text
DrawTemplateBuffer
  Persistent, GPU-readable: gpuInstanceIndex, index count, start index,
  base vertex, source page, flags.

VisibleIndirectBuffer
  Per-Viewport GPU output: compacted commands plus a count buffer.
```

```text
SubTab container/filter/membership -> scan relevant draw templates
                                  -> compact matching commands on GPU
                                  -> VisibleIndirectBuffer + count
                                  -> one ExecuteIndirect
```

Four constraints must be designed in rather than discovered:

- **`StartIndexLocation` must be absolute — *done*.** It used to be `(indexByteOffset - page.indexTail) / 2`, and `indexTail` moves down on every append, so appending one object silently invalidated every other object's start index in that page. All three draw paths now bind the index buffer view at the page base over the whole page and `RebuildIndirectBuffer` emits `indexByteOffset / 2`, stable for the object's stay in the page. The low-byte overlap with the vertex region is harmless: no draw references indices down there. This was the hard prerequisite for a persistent draw template, and it is no longer in the way. Note that a full rebuild per clone is still performed — that is now a choice (the offsets change under compaction anyway), not a constraint.
- **~~Index width stays a page property.~~ *Overtaken by the implementation.*** This said 16-bit versus 32-bit index format is set by `IASetIndexBuffer` per page, so the page-kind axis had to survive for it. Once the compacted command carries its own `D3D12_INDEX_BUFFER_VIEW`, the **format rides along per command**, and one `ExecuteIndirect` can mix 16-bit and 32-bit pages freely. The page-kind axis is still wanted for PSO-level concerns (transparency, wireframe topology, culling) and for the big-object fallback, which has no page at all — but index width is no longer one of its reasons on this path. Transparency can still be a per-object `renderFlags` bit routed to a separate output buffer (classification only; ordering remains a later concern).
- **`MaxCommandCount` is a CPU-side constant — *done*.** `SceneCullScratch::kMaxCommands` is 65,536 per Viewport, the shader clamps against it, and overflows are counted in telemetry. The buffer is persistent rather than drawn from a per-frame ring, as required. Two parts remain open: it is allocated per **monitor** rather than per active visible Viewport (correct today, and see the invariant-4 note under *Still deferred*), and the cull cache that motivated "persistent, not a ring" is not built yet.
- **~~Compute needs descriptor infrastructure that does not exist yet.~~ *Avoided, not met.*** A shader-visible heap and a global descriptor allocator were listed as prerequisites. Passing each page's buffer views as **root constants** kept the whole compaction path on root descriptors, so neither was needed. They come back only with the page directory, i.e. with the deferred items. What did hold exactly as written: the cull dispatch is recorded on the same per-monitor direct command list as the draws, with a barrier between the last dispatch and `ExecuteIndirect`, so no cross-queue synchronisation is involved.

*As implemented (`ShaderSceneCull.hlsl` + the compute branch in `RenderScene3D`):* **a Viewport is one `ExecuteIndirect`, whatever its page count.** The compute pass still dispatches per page — one thread per template — but every page writes into the *same* output buffer under the *same* atomic counter, and a single call then draws the lot.

**What made that affordable was putting the buffer views back into the command.** A 24-byte template is drawable only against its own page's vertex and index buffers, which is precisely why the draw loop used to bind them and issue a call per page. The compacted output is therefore a **56-byte `VisibleIndirectCommand`** — `{VBV, IBV, root-constant b1, DRAW_INDEXED}`, a second command signature alongside the 24-byte one — so commands originating in different pages, and different *containers*, can sit side by side in one buffer. The extra 32 bytes are paid only for **visible** commands in a per-monitor scratch, never per object in VRAM: the persistent templates stay 24 bytes.

The CPU passes each page's views to its dispatch as **root constants** (`SceneCullConstants`, 12 DWORDs), which is what keeps the whole path on root descriptors only. That is the load-bearing simplification, and it is why the fourth constraint above did not have to be met: with the views travelling in the constants rather than in a GPU-side page directory the shader looks them up in, there is still **no shader-visible descriptor heap and no global descriptor allocator**, and the `SceneEpoch` directory is not a prerequisite for one call per Viewport after all. The design listed a page directory as the alternative to per-command views; it is in fact only needed to collapse the remaining per-page *dispatches*, not the draws.

Three things the per-page form did that this deliberately does not:

- **No barrier between page dispatches.** They touch the count only through `InterlockedAdd`, so they are order-independent and free to overlap. Command order within the buffer therefore varies between frames, which is immaterial for depth-tested opaque draws. The old form barriered `UNORDERED_ACCESS ↔ INDIRECT_ARGUMENT` around *every page*, serialising the entire loop.
- **No count reset per page** — one 4-byte reset per Viewport, before the first dispatch.
- **No `IASetVertexBuffers` / `IASetIndexBuffer` at all.** Two IA binds per page are simply gone.

Overflow is bounded twice over and cannot corrupt anything: the shader drops a command whose `InterlockedAdd` slot lands past `kMaxCommands` (65,536) rather than wrapping, and `ExecuteIndirect` independently executes `min(MaxCommandCount, count)`. The count is left deliberately *over*-counted so the read-back below can report the overflow.

**A fence-gated `visibleCount` read-back is wired**, closing the last item on the deferred list this step started with. Each Viewport's surviving-command count is copied into a per-monitor `READBACK` buffer between the dispatches and the draw (the count is reset by the next Viewport, so it has to happen there), tagged with the frame's fence, and consumed one frame later. It surfaces on the debug heartbeat as `[gpu][cull] path= drawn= overflow=`. `drawn` is a **live gauge**, not a running total — it is the number the plan's "reduce each SubTab to what its camera needs" is ultimately measured by — and `overflow` must stay 0.

**The default is now ON, and the legacy path stays maintained.** Before this step the toggle was a lateral move — per-page draws either way — so it defaulted off. Now the compute path draws a whole view in one call with no IA binds and no per-page barriers, which the legacy path cannot match, so `gUseComputeCull` defaults to `true` and the `k` key is for A/B comparison rather than for bring-up. The legacy per-page path is kept as the A/B reference and as the fallback should per-command buffer views ever misbehave on some driver. Treat any change to the draw loop as a change to **both** paths.

*Measured, on a 182-object scene composed with a second 6-object container:*

| Check | Result |
|---|---|
| Compute vs legacy, static scene | **0 differing pixels** of 131,520 sampled |
| `drawn` against objects uploaded | 182 vs `commands=182`, `hidden=0` |
| Hide Unselected (1 selected) | `drawn` 182 → **1**, `hidden=181`, `clones`/`cloneMB` **unchanged** |
| Hide Reset | `drawn` back to 182, `hidden=0` |
| Two containers composed into one Viewport | `drawn=188` — **two pages, two containers, one `ExecuteIndirect`** |
| `overflow` | 0 throughout |

The hide row is the one worth reading twice: 181 objects left the frame for 181 mask writes and *zero* geometry-clone traffic, and they were dropped before becoming draw commands rather than vertex-shaded into degenerates.

**What "compaction" moves here — commands, not geometry.** This is the point most likely to be misread, because the word also names the geometry defragmentation in *Defragmentation logic* / Step 9. The two are unrelated:

- **GPU draw-command compaction (this step, render thread).** Reads the array of 24-byte `IndirectCommand` templates and writes the survivors out as 56-byte `VisibleIndirectCommand`s. Vertex and index bytes are never read, copied or moved; the geometry pages stay exactly where they are — only the *addresses* of those pages are copied, into each command's buffer views. There is **no temporary allocation**: the output lands in a **persistent per-monitor scratch** (`SceneCullScratch`: a 65,536-command `visibleIndirect` buffer, 3.5 MB, plus a 4-byte `visibleCount`), created once when the render thread starts and reused every frame. Per **Viewport** the render thread resets the count once with a 4-byte `CopyBufferRegion` from a shared zero buffer, dispatches one thread per template of each of the Viewport's pages with nothing between them, barriers the scratch `UNORDERED_ACCESS → INDIRECT_ARGUMENT`, and issues **one** `ExecuteIndirect` whose command count comes from `visibleCount`. The scratch is reused by the next Viewport on that monitor — safely, because they are recorded in order into one command list and the next Viewport's barrier back to `UNORDERED_ACCESS` drains the previous one's draw first.
- **Page compaction (Step 9 / *Defragmentation logic*, copy thread).** *Does* allocate — a fresh 4 MB page via `CreateNewPage` — and copies only the live objects' vertex/index ranges into it with `CopyBufferRegion`, dropping the holes left by deleted geometry, then publishes the clone and retires the old page. That is the one that "creates a temporary allocation by copying only the valid buffers"; the command compaction above never does.

**Where the 64-bit visibility flag is processed.** Inside the compute shader, and that test *is* the compaction filter. Each thread calls `IsVisibleInSubTab(cmd.gpuInstanceIndex, subTabBit)`, which loads the object's `VisibilityMask[gpuInstanceIndex]` (the 64-bit SubTab-membership word from Step 5, carried as `uint2`) and tests the single bit for the SubTab this Viewport is drawing (`subTabBit`, a root constant; `>= 64` means "show all"). Bit set → the command is appended via one `InterlockedAdd` on the count; bit clear → the object is dropped and never becomes a draw. This is the same predicate the scene and pick *vertex* shaders apply on the legacy path (collapsing a hidden object to a degenerate primitive), moved upstream so hidden and cross-SubTab-filtered objects cost nothing past this dispatch instead of being vertex-shaded and discarded. The mask is authored by the copy thread's `WriteVisibilityMask` (from the `HIDE_*` ribbon rows via `SET_VISIBILITY` / `CLEAR_SUBTAB_HIDES`); the compute shader only reads it, as SRV `t1`. Note the coarser, container-level SubTab filter (Step 6's container-set directory) still runs first on the CPU — it decides *which pages* a Viewport visits at all; the 64-bit mask is the finer, per-object level within those pages.

*Still deferred, in rough build order:*

- **The `SceneEpoch` atomic directory and the revision-keyed cull cache.** The compaction still regenerates every frame, so a Viewport nobody is touching re-culls needlessly. The cache is keyed by `(SceneEpoch, SubTab filter revision, visibility revision)` — three revision counters that do not exist yet — and its output buffer must be persistent rather than drawn from a ring. Note one thing the cache cannot simply be: **per Viewport**. Two windows on two monitors can resolve to the same tab's active sub-tab, so a per-Viewport buffer would have two owning render threads and break invariant 4. The scratch is per **monitor** today for exactly that reason, and a cache must be keyed per monitor per Viewport, not per Viewport alone.
- **Retiring the persistent per-page indirect buffers.** Much less urgent than it was: right-sizing the reservation to 256 KB (Phase 6) removed the ~2.4 GB of pure waste that made this the sharpest VRAM item, so what remains is the structural cleanup rather than a memory emergency. The templates become an SRV-only `DrawTemplateBuffer`, which in turn forces the GPU pick pass *and* the print path off the 24-byte per-page `ExecuteIndirect` they still execute, and needs the page directory so the shader can expand a `sourcePage` reference into buffer views. A template could also shed `InstanceCount` and `StartInstanceLocation` once it stops being executable, 24 → 16 bytes.
- **Collapsing the per-page dispatches into one.** The draws are already one call; the dispatches are not. This is what the page directory actually buys, and it is worth little until page counts are large.

### Appearance, variations and display state *(planned — nothing below is built)*

Steps 1–7 built identity, memory and command generation. This is the layer that decides what an object *looks like* and which of several forms of it a given Viewport shows. It is designed as one piece because the four parts share machinery: the bucket keys live in the same 16 bytes, the variants are addressed by the same `memoryID` space, and per-Viewport display state is the same mask mechanism Step 5 already built.

#### The 16-byte appearance payload

The `InstanceRecord`'s second half finally acquires producers and consumers:

| Field | Holds |
|---|---|
| `packedColor` | RGBA8 — base colour **and** 0–255 opacity. No separate transparency field is needed. |
| `materialIndex` | Index into a material table (texture ids, roughness, …), not a raw texture id — same 4 bytes, far more headroom. |
| `renderFlags` | The three bucket keys — transparent, double-sided, line topology — plus depth-write behaviour. |
| `packedParams` | Spare. |

The split Step 4 established holds: authored, infrequently changed state here; hover, selection, SubTab membership and hide in the masks, so an interaction never allocates an arena slot. The transform-edit defect this opens is described under Step 4 and must be fixed in the same change.

#### Geometry variations — `(memoryID, variation)` packed into the ID

A centerline, an origin marker and a stress contour are all *different geometry for the same engineering object*. The Miscellaneous specification already anticipates the key becoming `(memoryID, variation)`; this is the packing that costs nothing.

`memoryID` is assigned monotonically from 1 and is process-local. 2⁵⁶ is ~7×10¹⁶ — at ten million allocations a second, 228 years — so the **high 8 bits are free** and hold a variation number, giving 256 variations per object with **no storage change at all**. Every existing map, command struct and staging estimate is untouched, and persistence is unaffected because `memoryID` never reaches disk. To the GPU a variation is simply an independent object: its own `InstanceRecord`, redirect entry, visibility mask, registry entry and geometry.

Three costs to price in:

- **It multiplies the one remaining hash map.** `indexOfMemoryId` is the single `unordered_map` the registry kept, already flagged as costing well over a gigabyte of node overhead at 10M entries; three variations per object triples it. If that bites, allocate a variation's `gpuInstanceIndex` contiguously with its base and store a variation count in the registry entry, so lookup becomes `Find(base) + v` and the map stays at 10M — at the cost of needing contiguous runs from the free list.
- **Variations spend the same index space as real objects.** 10M objects with centerlines and origins is 30M identities, which exceeds `MV_MAX_INSTANCES_PER_TAB` (10,485,760) outright and puts the registry at ~1.2 GB of host RAM. Raising the constant costs nothing but address space on the GPU side, but the registry is real committed memory. **10M is the identity ceiling, not the object ceiling.**
- **Selection must mask the low 56 bits.** Clicking a centerline should select its parent, so the pick resolve strips the variation before writing `selectedObjectIds`. Otherwise the lookup against the base id silently fails and the object refuses to move — the same shape as the composed-container gap fixed in Step 6.

**Additive versus substitutive is a required property, not an emergent one.** A centerline or origin marker is *additive*: drawn alongside the base. A contour is *substitutive*: the same triangles at the same positions, so drawing both is guaranteed z-fighting. A substitutive variation must therefore hide its base in that Viewport. The hide mechanism already exists; what must be added is the declaration, and a **rule** to drive it ("this Viewport shows variation N of container X") rather than a mask write per object.

**Lifecycle is deliberately asymmetric.** Centerlines, origin markers and similar are generated **eagerly** alongside the base object — they are small and always potentially wanted. Contours are generated **on demand** per load case when a Viewport asks for them, and **discarded eagerly** when the user closes that view: they are the large ones, and most of the time nobody is looking at them. That asymmetry is the argument for packing variations into the id rather than inventing a separate object type — a transient contour rides the ordinary ADD / REMOVE path with no lifecycle machinery of its own, closing the view is a REMOVE per variation, and the empty-page GC drops its pages.

#### Vertex format variants

Per-object colour makes the 8-byte per-vertex colour dead weight for ordinary geometry. It does not disappear — it moves into dedicated variants, each with its own pipeline and its own pages:

| Variant | Vertex | Lifetime |
|---|---|---|
| Base (lean) | pos 12 + normal 4 = **16 B** | authored, persistent |
| Scalar field (FEA results) | pos 12 + normal 4 + FP16 scalar = 18 B (pad 20) | computed, transient, per load case |
| Baked vertex colour (imported PLY/OBJ/scan) | pos 12 + normal 4 + RGBA8 = **20 B** | authored, persistent |

**Storing the scalar rather than a baked RGBA is the decision that matters.** A contour is a scalar field pushed through a colormap; baking the colour means a full re-upload to change the colormap, the legend range, or linear↔log scaling. Keeping the scalar and putting colormap and range in a per-Viewport root constant plus a small 1D texture makes all three **free** — and dragging the legend range is the most-used interaction in results post-processing. Imported colours are almost always 8-bit at source, so RGBA8 rather than FP16; the shader expands, and HDR is unaffected since tonemapping is on the output side.

Note the lean vertex is 16 bytes, a power of two, so lean pages get the cheap `AlignUp` mask; the non-power-of-two alignment problem moves to the variant strides rather than disappearing. `GeometryPage` gains a `vertexStride` and `VertexAlign` / `IsFull` stop being static — and **three independent sites** compute `vertexByteOffset / sizeof(Vertex)` (`RebuildIndirectBuffer`, the Selection3D highlight path, the compaction relocation), every one of which must switch to the page's stride. That is verbatim the trap that produced the 16-byte-alignment defect under *Page structure*; one of the three being missed is a silently misplaced draw, not a crash.

**The duplication is accepted knowingly.** A contour variation repeats position and normal — 16 bytes per vertex, ~8 MB for a 500k-vertex model. The alternative, a parallel colour *stream* bound as vertex slot 1 (the command signature can carry a second `VERTEX_BUFFER_VIEW` with a different `Slot`), duplicates nothing but couples two page systems through byte offsets: defragmenting a geometry page would have to relocate the colour page in lockstep, forever. That is precisely the association invariant 1 forbids, and 8 MB is a cheap price for keeping the two independent. Separate pages also mean re-solving a load case re-uploads only the scalar pages and clones nothing of the base.

#### Per-Viewport display state

Display class resolves at three levels, cheapest first:

1. **Per object, Viewport-independent** — the `InstanceRecord` (this window is glass at 40%).
2. **Per Viewport, object-independent** — a root constant (this whole view is wireframe; ghost everything at 15%). No per-object storage at all.
3. **Per object per Viewport** — a **second 8-byte mask array**, `DisplayModeMask[gpuInstanceIndex]` alongside `VisibilityMask`, one bit per SubTab.

**Level 3 must be a second array, not a wider mask.** Widening to `uint4` (2 bits per SubTab) breaks invariant 2, which permits exactly one naturally-aligned store of **≤ 8 bytes** whose old and new values are both valid. Two independent 8-byte arrays are each individually safe, and the one-frame skew between them — a reader seeing new hide with old display mode — is visually meaningless. It also stays free for tabs that never ghost anything, being another reserved-tile buffer.

Pair it with a container-level rule, exactly as Step 6 did for composition: "ghost this whole container in this Viewport" is one entry, never a write per object. Per-object bits are the escape hatch for "ghost everything except these three bolts".

*Worked example — an engine block inside a truck, the truck near-transparent so the engine's position reads.* The engine's bit is clear, so it routes to the opaque bucket; the truck body's bit is set and the Viewport override supplies a fixed alpha, so it routes to the ghosted bucket. **Two `ExecuteIndirect`s, regardless of page count.** Level 1 alone could not express it (the truck would be ghosted everywhere) and level 2 alone could not either (the engine would be ghosted too), which is what makes this the case that justifies level 3.

Two things fall out for free, and they agree with each other: ghosting drawn with **depth-write off is order-independent**, so it survives the nondeterministic command order `InterlockedAdd` produces across overlapping dispatches; and the same depth-write-off makes the **pick pass click straight through** the ghosted body to the engine, which is what a user expects.

**Not solved by any of this:** genuine sorted transparency — two glass panels with different per-object alpha, one behind the other — flickers under GPU-generated command order and needs either a sort pass over the transparent bucket or order-independent transparency. See the Phase 5 item, which this section rewrites the plan for.

### Step 8 — Shared geometry and the primitive libraries *(planned — nothing below is built)*

Prioritised ahead of defragmentation because it is the single biggest VRAM lever for plant models, and because Step 7 removed its hardest prerequisite.

#### What this is, and what it deliberately is not

Phase 5 promises *"Instanced rendering: `InstanceCount > 1` + per-instance matrix indirection in the vertex shader."* **That plan is superseded.** What is built here is **shared geometry**: one draw command *per object* with `InstanceCount = 1`, many commands pointing at the same `BaseVertexLocation` / `StartIndexLocation` inside a library buffer that stores the mesh **once**.

`InstanceCount > 1` was the wrong target. It needs per-instance data contiguous and indexed by `SV_InstanceID`, which fights every property Steps 1–5 established — you could not hide one bolt of a batch, move one, or recolour one without breaking the run. Shared geometry gives the identical VRAM saving and the identical draw-call count (one `ExecuteIndirect` per Viewport either way), while every object keeps its own `gpuInstanceIndex`, `InstanceRecord`, redirect entry and mask bit. **Colour, transparency, hide, move and pick all keep working with no new machinery.**

**Step 7 is what made this cheap.** Because the compacted command carries its own VBV/IBV, a command drawing from a library buffer sits in the same output buffer as one drawing from an ordinary geometry page. Under the per-page model, library geometry would have needed its own `ExecuteIndirect`.

#### Two libraries

**`gpu.primitiveLibrary` — global, immutable, LOD'd.** One buffer, built at startup, alive for the process, shared by every tab. **Deliberately outside the RCU/page system**: it never changes, so it needs no snapshot, no container directory, no retirement and no fence gating. Tab 0 owns it conceptually; it lives in the `शंकर` singleton so the draw loop never has to walk two tabs' snapshots.

It holds the shapes whose *only* freedom is size, so one canonical mesh plus a non-uniform scale covers every instance: sphere (uniform), cuboid (3-axis), cylinder outward-facing and inward-facing (radius + length), disc (radius), and one or two tori at *default* tube ratios. The two cylinder entries exist because a bore is the same mesh with inverted winding — a second entry rather than a render-state change, unless double-sided rendering becomes universal for sectioning.

Every entry carries **8 LOD levels, always 8**, even where fewer are meaningful: a cuboid's eight slots all point at the same 12-triangle mesh. Uniform array width keeps the shader branch-free and duplicate table entries cost 12 bytes each. The table is `(shapeId, lod) → { indexCount, startIndexLocation, baseVertexLocation }` — ~12 shapes × 8 LODs × 12 bytes ≈ 1.2 KB, a root SRV.

**Tab-owned template library — created on demand, no LOD.** Everything whose shape has a parametric ratio, plus non-parametric imported templates. Created on first use, lives until the tab closes, **no reference counting** in the first implementation — so a tab that creates many templates and then deletes their users holds that space until it closes. Acceptable initially; worth a telemetry counter to find out whether it matters.

**The split falls on *ratios*, not on parametric versus non-parametric.** Scaling changes size, never shape ratios, so a shape with an internal ratio needs one mesh per ratio — a catalog, not a scale. Elbows and flanges are therefore ASME catalog entries in the tab library; pipes, bolts and nuts are composed from primitives.

#### Pipes as composed primitives

A pipe decomposes into an outer wall, an inner wall and two annular end caps. The walls are scale-instanced cylinders; the caps are catalog items. Bolts and nuts compose the same way. This works because each part is its own graphics object with its own transform, so the engineering thread derives four transforms from the pipe's own `p1`, `p2` and radii and each lands in an ordinary `InstanceRecord` — no part-transform table, no matrix multiply per vertex, no second template struct.

**Decomposition does not remove the wall ratio, it relocates it.** The two walls become ratio-free, each a plain cylinder scaled by `(r, r, L)`. But the annular cap is `disc(ro) − disc(ri)`, and no affine transform can move two concentric radii independently, so the cap stays a catalog item keyed by schedule.

**Why this still beats one capped-tube mesh per schedule:** nearly all the triangles are in the walls — a 36-segment wall is 72 triangles against an annulus's 36 — so putting the walls in the global library gives pipe LOD for the expensive part automatically, where a capped-tube entry would need all 8 LODs *per schedule* in the tab library. Caps are also only needed at *free* ends; most pipe ends join a flange, elbow or another pipe.

#### The template-only page

An instanced object contributes **no vertex or index bytes anywhere**, so its page needs no 4 MB geometry buffer — only a template buffer and placement records. An ordinary page is 4 MB + 256 KB ≈ 4.25 MB; a template-only page is ~256 KB.

**The page names a source buffer rather than owning one.** That single decision keeps every existing draw path working: the legacy per-page path, the GPU pick pass and the print path all bind VBV/IBV per page, and for a template-only page those come from the referenced library instead of from `page.buffer`. Nothing is forced onto the compute path. The rule is that **one template-only page draws from exactly one source**, so a container has at most two (global, tab) beside its ordinary pages.

Second-order win: adding one bolt to a scene of 100,000 bolts clones a **256 KB** page, not 4.25 MB. Instancing improves the *edit* path, not only VRAM.

**Two template formats, one output buffer.** The *template* formats genuinely differ — a bespoke template names byte offsets in its own page, an instanced one names a shape id and needs a LOD lookup. The *compacted output* format does not differ at all: both emit the same 56-byte `VisibleIndirectCommand`, one with views pointing at a geometry page and the other at a library, same PSO and same vertex shader. So the split belongs in the **compute pass, not the draw** — a template-only page dispatches the instanced shader, an ordinary page the bespoke one, both `InterlockedAdd` into one count and one output buffer. Splitting the draw as well would double the scratch regions once PSO buckets land (16 instead of 8, each with its own headroom) for no pipeline-level gain, and instanced-versus-bespoke is not a PSO distinction the way transparency, culling, topology and vertex format are. Cheaply reversible if a reason appears.

Placement records in such a page have no meaningful byte offsets; those fields carry `(libraryEntryId, lod)` instead, and `GeometryPlacementRecordInPage` has 7 spare bytes in its 64.

#### Emission and mutation

`GetGeometry` for an eligible object emits a **library reference** instead of vertices and indices: shape id, LOD hint, and the transform mapping the canonical mesh onto the object's engineering parameters. **Eligibility is always-instance-when-possible** — no repeat-count threshold, because an instanced object costs zero geometry bytes, so instancing a one-off still beats not instancing it.

An object stays eligible while nothing modifies its geometry. Drill a hole and it stops being: `GetGeometry` emits real vertices and the copy thread relocates it into an ordinary page. **That is an ordinary geometry MODIFY on the same graphics object**, not a change of identity — the `gpuInstanceIndex` survives, and with it the object's colour, hide state and current selection. Had the two forms carried different ids, drilling a hole would silently reset all three. A composed object can be *partly* instanced, since the parts were independent graphics objects to begin with.

#### One engineering object, up to 256 graphics objects

The 8 free high bits of `memoryID` key a **graphics object**: one engineering object maps to as many as 256, each with a fully independent `gpuInstanceIndex`, `InstanceRecord`, redirect entry, mask word and registry entry. That independence is deliberate — it is what lets a composed pipe give each part its own transform with no new machinery.

**This unifies with the variation key.** Composition parts and alternate representations are the same question — *which* GPU object of this engineering object — so they share one field rather than competing for it. A pipe's four walls and caps, plus its centerline and a stress contour, are six graphics objects of one pipe. `0` is the whole object; the rest of the structure is deliberately left open for now, with the caveat that it hardens by accident once several composed types have baked their own assignments in.

Three prices, accepted knowingly:

- **Interactions cost N writes, not one.** Hiding a pipe is four mask writes; moving it is four arena slots and four redirect flips. Still ~270 bytes against a 4 MB clone, so the Step 4 budget holds — but the workload table's *"one atomic mask write per object"* means **per graphics object**.
- **The identity ceiling counts graphics objects.** `MV_MAX_INSTANCES_PER_TAB` bounds those, so the engineering count it supports is that figure divided by the average part count. See *Goal and workload*.
- **Selection and pick must mask the low 56 bits.** Clicking any part selects the pipe; highlighting the pipe highlights all four. Both paths compare engineering ids, not graphics ids — the same rule the variation key already required.

**The engineering thread expands a composed object**, through a method on the object's own class, and the graphics engine is **oblivious to composition entirely**: it receives four independent ADDs with four sub-ids, exactly as if four unrelated objects had been created. The batch deduplication pass therefore needs no change (it keys on the full 64-bit id, and the four are distinct), and the composition method extends the *Object placement* rule rather than bypassing it. The one piece of bookkeeping required is the **emitted part count, stored on the engineering object** — re-deriving it from current parameters is wrong once those parameters have changed.

#### LOD

LOD is selected **per frame, in the compute pass**, from camera distance. Camera position is three more root constants in `CullParams`, which has a spare `cullPadding` — 12 DWORDs becomes 16. Object position comes from the instance record's `transformA/B/C.w`, so the shader binds the redirect table and arena, two more root SRVs with tab-lifetime fixed addresses — the same two-load pattern the vertex shaders already perform. The shader writes the chosen LOD's offsets into the output command; VBV/IBV are unchanged, since all 8 LODs live in the same library buffer.

An instanced template carries its shape id in `StartInstanceLocation`, otherwise always 0, and the compute pass writes 0 into the output command — no template format change and no second template struct.

Two consequences: **the legacy path draws instanced geometry at a CPU-fixed LOD**, since its templates are static (correct, just not adaptive — acceptable for a reference path); and **camera motion must join the cull-cache key**, because the deferred `(epoch, filter revision, visibility revision)` cache from Step 7 no longer holds once command *content* is camera-dependent. That cache exists for static Viewports, so it is a fair trade — but it has to be designed in, not discovered.

#### Non-uniform scale and normals

Instancing a cylinder needs `scale(r, r, L)`. Positions are fine — the vertex shader computes `p · M` and does not care what M contains. **Normals are the whole issue**, and it is wrong lighting rather than a crash, so it would ship unnoticed.

Normals transform by `inverse(transpose(M₃ₓ₃))`, not by M. Under *uniform* scale the two differ only by a scalar that `normalize()` cancels — which is exactly why the shader is correct today and why the `InstanceRecord`'s uniform-scale assumption was safe to write down. Under non-uniform scale it does not cancel: scale a sphere to (1, 1, 10) and the shading bands stop matching the silhouette.

The fix costs no storage. For `M = S·R`, row *i* of M is `sᵢ · Rᵢ`, so `sᵢ = length(rowᵢ)` and `inverse(transpose(M)) = S⁻¹·R`, whose row *i* is `Mᵢ / sᵢ²` — three lengths, three divides, then normalize, on a transform the shader already loads. It imposes one rule: **composed transforms must stay scale → rotate → translate, never sheared.** It touches `ShaderSceneVertex.hlsl` and `ShaderScenePickVertex.hlsl` (which duplicates the struct), and the highlight path inherits it by reusing the scene vertex shader.

The scale is *derived*, never stored — radius and length are the object's own engineering fields, composed by `GeometryForObject`. `Placement3D` stays rigid and nothing in the schema changes.

#### Arena reservation sizing

The reservation is sized from the hardware rather than from a compile-time constant, which is what turns "capability varies with hardware" into behaviour instead of a caveat:

```text
reserve = min(MV_MAX_INSTANCES_PER_TAB, allowedByHardware × 0.75)
```

`allowedByHardware` comes from `MaxGPUVirtualAddressBitsPerResource` — as low as **31 bits (2 GB)** on the lowest tier, capping one tab's arena at ~33M records — with `MaxGPUVirtualAddressBitsPerProcess` bounding the sum across open tabs. Both are queried at startup beside the existing Heap Tier 2 and `TiledResourcesTier` checks. The **25% margin** is deliberate headroom for everything else competing for the same address space: geometry pages, template buffers, textures, swap chains, the upload ring, and the redirect and mask buffers. A first estimate, to be tuned once there is real data.

#### Build order, and four places the current code fails quietly

The library buffer is a VBV source, so its stride must match the PSO input layout. It is therefore built **only after the vertex format migration**, in the lean 16-byte layout, so it is never built twice. The sequence is: appearance payload and registry shadow → vertex 24 → 16 with the material table → this step.

Four things in the current implementation break *silently* under this design, which is why they are recorded here rather than discovered:

- **`PageIsRenderable` rejects every template-only page.** It requires `vertexHead != 0 && indexTail != pageSize`, and a template-only page has neither — so the draw loop, pick pass and print path all skip instanced geometry with no error anywhere. The predicate must branch on page kind.
- **`IsTransformOnlyEdit` misclassifies every instanced MODIFY.** It infers "transform-only" from an empty vertex/index payload — and an instanced object's payload is *always* empty. Moving a pipe and changing its schedule become indistinguishable: the first is correctly a redirect flip, the second silently keeps the old library entry, so the wall thickness never changes on screen. The instanced-MODIFY case needs an explicit marker. This is Step 4's own lesson reappearing from the other direction.
- **The registry's `worldCentre` has no source.** It is computed at upload time from the geometry's AABB, and an instanced object uploads none. It must come from the library entry's AABB transformed by the instance transform, or every instanced object reports (0,0,0) and silently breaks zoom-to-fit.
- **Append-target page selection has no meaning for a template-only page.** "Largest middle gap" is a vertex/index-region concept; the criterion there is template capacity (`indirectCapacity`).

### Step 9 — Defragmentation

Two arenas accumulate holes. Neither is compacted eagerly; both wait until a hole threshold is crossed.

- **Geometry pages** — as already specified in *Defragmentation logic* above: when a page's `holeBytes` crosses ~25%, its next RCU clone copies live ranges packed (per-object `CopyBufferRegion` from the placement records) instead of a whole-page `CopyResource`, at most one page per batch. The argument/template rebuild is mandatory on every clone anyway, so it picks up the remapped offsets for free. **Cross-page** compaction is the new part, and it needs a deliberate pass rather than riding a clone.
- **Instance arena** — every move burns a slot, so holes accumulate at edit rate. Note that the fence-gated free list already recycles them, so the arena does not *grow* without bound; what a pass buys is index contiguity, by relocating live records and rewriting the affected redirect entries. `gpuInstanceIndex` is untouched throughout, which is precisely what the redirect indirection buys.

Index contiguity is deliberately not maintained between passes, and no allocation policy attempts to preserve it. None of this is in the first implementation.

### Step 10 — Spatial data and real GPU culling (deferred)

Bounds and spatial data are deliberately deferred until the previous steps are correct. Design the draw template and page headers so they can later acquire world bounds, a spatial-cell/BVH reference and LOD data.

The culling path then evolves without changing object identity or paging:

```text
SubTab filter -> container-page rejection -> spatial-page rejection
              -> frustum culling -> LOD selection -> optional Hi-Z occlusion
              -> compact visible indirect commands
```

The Step 7 filter/compaction path remains useful as a correctness baseline and fallback.

### Step 11 — Viewport scheduling and scale limits

64 independent SubTabs do not imply 64 equal-rate full-resolution renders. The compositor schedules Viewports according to user value:

| Viewport class | Typical policy |
|---|---|
| Focused interactive | 30–60 Hz, highest LOD budget |
| Visible secondary | Budgeted refresh rate |
| Static background | Render only when dirty |
| Dashboard/thumbnail | Lower resolution and low refresh rate |
| Occluded/minimized | No rendering |

This policy, together with cached cull output and the later spatial hierarchy, is what turns the 10-million-object / 64-SubTab requirement into bounded GPU work rather than 640 million draw commands every frame.
