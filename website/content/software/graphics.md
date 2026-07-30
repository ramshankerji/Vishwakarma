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

### Lighting

Initially, **hemispheric ambient lighting**:

```
Factor       = (Normal.z × 0.5) + 0.5
AmbientLight = Lerp(GroundColor, SkyColor, Factor)
```

Screen Space Ambient Occlusion (SSAO) to darken creases and corners is planned for a future revision.

### World matrix

All vertices are positioned in object-local space; the world matrix is applied in the vertex shader. This lets us move even a 1000-vertex object with just a 48-byte world-matrix update per object. We use a **packed 48-byte** world matrix instead of 64 bytes to save bandwidth — the last row is always `0,0,0,1`, so we omit it and reconstruct it in the shader.

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

There are multiple views per tab. *As implemented*, the `ExecuteIndirect` argument buffer is per **page** (one, not per-view double-buffered) and is regenerated whenever a page is cloned; a delete soft-marks the placement record (`isDeleted`) and the next rebuild drops it. Per-view argument buffers proved unnecessary — a view filters at draw time by the page's container ID, issuing an argument count of 0 for inactive containers.

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

*Superseded at the top end:* the one-call-per-page model holds through Phase 5. The 10-million-object plan replaces it with GPU-compacted, per-Viewport command buffers and a single `ExecuteIndirect` per Viewport; of these four axes only index width remains a true page property there. See *10 Million Objects + 64 SubTab Draw Plan*, Step 7.

*To clarify later:* how to handle repeated geometry (e.g. bolts). They need only one set of vertex/index buffers and can be drawn with different world matrices.

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
| Page content | Single type only | Zero PSO switching during draw |
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

**Growth logic** needs no special path either: a container simply gets more pages — new geometry lands in the page with the largest middle gap, else in a fresh page. Pages are 4 MB today; once chained doubling lands (Phase 7), successive new pages of a fast-growing container double up to 64 MB, but the rule is unchanged: nothing grows in place, so nothing freezes. The old plan (grow a heap in place while the tab's views freeze) is gone.

### Freeze logic

Use **Render To Texture (RTT)** to implement frame freezes, since the swap chain is `FLIP_DISCARD`. RTT is now the standard frame path (draw → RTT → copy into backbuffer → present), so a freeze is simply "keep presenting the last RTT". With RCU pages, defragmentation no longer needs freezes at all (see above); the mechanism stays valuable for eviction safety, device-loss handling, HDR tonemapping (the copy becomes a draw), UI composition and multi-monitor flexibility.

### Known issues / limitations (to be resolved in a later revision)

Everything formerly tracked here now has a roadmap slot: transparency sorting, the hot-drag / active-mutation path and instanced repeated geometry sit in Phase 5, the telemetry counters in Phase 6, and evict/residency logic, compute-shader frustum culling, mesh shaders and instance-based LOD in Phase 7.

Resolved since this list was written: selection highlighter methodology — shipped as the GPU pick pass + highlight overlay + rotation cube (Selection3D module); vertex page offsets misaligned against the 24-byte stride — see the alignment rule under *Page structure*; world-matrix slots recycled before the frames referencing them retired — see Step 1 of the 10-million-object plan; and the pick resolve's world-space AABB centre, which was transforming a local centre by the *transposed* matrix (harmless only because every generator bakes positions into vertices and leaves `worldMatrix` identity) — Step 2's registry shadow computes it with the same row-vector convention the vertex shader uses.

### Miscellaneous specification

- A uniform 64-bit object ID, unique across all objects in the entire process memory. The renderer maps exactly one `gpuInstanceIndex` to one such ID; if multiple simultaneous geometry variations per object are ever wanted, the key becomes `(memoryID, variation)` — see the 10M plan's terminology.
- We expect roughly 1000–5000 draw calls per frame at present. That budget is what the one-`ExecuteIndirect`-per-page model is sized for; at 10M objects the same budget is met instead by one compacted `ExecuteIndirect` per Viewport (Step 7).
- Multiple partially-overlapping windows, each independently resizable / maximize / minimize.
- The lowest distance between an object and *all* the different view-camera positions is used by the logic threads to decide the Level of Detail.
- A mechanism to manage memory over-pressure, signalling the logic threads to reduce level of detail within some distance.
- The GPU memory manager is a **singleton** — exactly one instance manages all GPU memory.

On a desktop PC with two discrete GPUs and one integrated GPU, each driving one active monitor, we still use exactly one device for rendering all monitors. Windows 10/11 WDDM supports heterogeneous multi-adapter: when a window moves, DWM composites surfaces and copies the frame across adapters if needed. This works but is slow, since every such frame must traverse the PCIe bus.

### Roadmap (to-do list)

As items complete, they move out of this pending list and into the design document proper.

**Phases 1–3 — complete.** The visual baseline (lit 24-byte vertices, hemispherical lighting, mouse navigation), the RTT infrastructure and the API pivot (structured-buffer world matrices, `DrawIndexedInstanced` → `ExecuteIndirect`) are all live; their designs are described in the sections above.

**Phase 4 — the memory manager (the Vishwakarma core)**

Done so far: 4 MB double-ended VRAM pages with RCU clone → mutate → atomic-snapshot publish, per-container page ownership, fence-gated retirement (snapshots, pages, instance indices, instance slots), empty-page GC, the reserved-tile instance arena + redirect table with their copy-thread identity registry (10M plan Steps 2–4, which retired the doubling world-matrix table and its retired-buffer queue outright), tab/view management, basic ribbon UI, and the global upload ring with ring-gated chunked submission.

**Global upload ring buffer + ring-gated submission — done.** One persistent-mapped 64 MB ring with fence-tracked reclaim now serves Scene3D geometry staging and the argument-buffer rebuilds, replacing the per-object committed staging resources in `RecordGeometryUpload`, and the dead per-tab upload heaps (64 MB vertex + 16 MB index per tab, committed and mapped but never written) are gone. Submission is chunk-driven: one recording, one submit, one fence wait and one publish per chunk, collapsing the three record/execute/CPU-wait cycles per tab into one. The CPU-side drain is capped so a lakh-object import cannot materialise hundreds of megabytes of `GeometryData` before a byte reaches the GPU, and an oversize payload falls back to a one-off committed buffer. Specified in full as Step 0 of the 10-million-object plan below. **Still to migrate onto the ring:** Page2D record uploads (`ProcessCad2DCopyBatch`, which also creates its own allocator and command list per batch) and texture uploads.

Remaining, in build order — every later feature funnels through the copy thread, so each item here multiplies the value of everything after it:
- [ ] **Page compaction during RCU clone** — replaces the old freeze-based "VRAM defragmentation" item; see the rewritten *Defragmentation logic* section. When a page's `holeBytes` cross ~25%, its Pass-2 clone copies live ranges packed (per-object `CopyBufferRegion` from the placement records) instead of whole-page `CopyResource`. The argument-buffer rebuild — already mandatory on every clone — picks up the remapped offsets for free. At most one page compacts per batch.
- **CPU-side segregated free-list allocator — demoted (telemetry-gated, Phase 6).** The implemented per-batch scan picks the page with the largest middle gap in O(active pages), which is fine below ~1000 pages, and compaction removes most fragmentation pressure. Build the free list only if Phase 6 telemetry shows the scan actually costing something.

**Phase 5 — structural features first, then polish**

Done so far: Shader Model 6; click / window selection — built as a GPU pick pass (object-ID render + fence-gated readback, Selection3D module) instead of the originally planned CPU raycast, plus selection-highlight and rotation-cube overlays.

- [ ] **Page-type axes — the "16 × N" object representation.** Pages are currently keyed by container only and everything draws with one PSO (opaque triangles, 16-bit indices, back-culled). Add a page kind — {opaque | transparent | wireframe} × {16-bit | 32-bit index} — next to `containerMemoryId`, a small PSO table, and a per-kind loop in `RenderScene3D`. This unlocks wireframe display modes, transparency and large meshes; none of the items below can start without it.
- [ ] **Big-object fallback.** Wire the dormant `BigGeometryObject` path: dedicated committed resource, 32-bit indices, own draw call. Today one object above 65,536 vertices silently wraps its 16-bit indices — must land before STL / terrain import ships.
- [ ] **Hot-drag / active-mutation path** (promoted from the known-issues list). An interactive drag would once have been a MODIFY per mouse-move — a 4 MB page clone per frame. The GPU side of this is **done**: Steps 1–4 make a whole-object move write one new instance slot plus one atomic redirect, cloning nothing. What remains is entirely on the producer side — every geometry generator currently bakes world positions into its vertices and leaves `GeometryData::worldMatrix` identity, so nothing can emit the transform-only command the copy thread already understands. Give the generators a real world matrix and the fast path lights up. (The per-frame matrix double-buffer once listed here as a prerequisite was *not* the right fix — no fixed frame depth is correct when render threads run at different refresh rates, and a 64-byte record cannot be written in place without tearing.) Must land before interactive 3D move/rotate tooling.
- [ ] **Transparency sorting** (needs the page-type axes): accept imperfect order during camera motion; CPU sort + argument rebuild when the camera stops.
- [ ] **Instanced rendering for repeated geometry** (pipes, bolts, standard sections): `InstanceCount > 1` + per-instance matrix indirection in the vertex shader. This finally answers the "repeated geometry" open question above, and is the single biggest VRAM lever for plant models.
- [ ] SSAO.
- [ ] **HDR output pipeline** (reworded — the vertex side is already done, colors are FP16): `rttFormat` → `R16G16B16A16_FLOAT`, tonemap draw replacing the RTT→backbuffer `CopyResource` (the full-screen-quad path returns), HDR swap chain + the startup detection rules from the *Vertex format* section.

**Phase 6 — performance & telemetry** (wire into the existing ImprovementData pipeline; this phase also settles the demoted items)

A first set of copy-thread counters already exists (`GpuCopyStats`, printed on the debug FPS heartbeat): batches, chunks, commands, pages cloned, clones-per-chunk, clone bytes, ring bytes and high-water, oversize-staging count, deferred-queue depth, max active pages, pending/free instance indices (`idx`, which moves only on REMOVE — a MODIFY keeps its index), pending/free arena slots (`slots`, which every MODIFY churns), the transform-only edit count (`moves`), and retire backlog as both a live gauge and an all-time peak. They are the numbers the four workload budgets above are measured against. Note which counters are cumulative, which are live and which are high-water marks — a peak reads like a stuck value when it is only recording a stall that has since cleared. Wiring them into the ImprovementData pipeline and the Application Tab stats pane is the remaining work.

- [ ] Per-tab VRAM usage graphs: page count, `liveBytes`, `holeBytes`, matrix-table size, big-object list size.
- [ ] Page fragmentation heatmap + compaction-trigger counters.
- [ ] Copy-thread health: batch latency and stall time at the publish fence wait (the rest is covered by `GpuCopyStats`).
- [x] Retire-backlog depth — promoted out of `_DEBUG` to a real counter. It catches the frozen-monitor-fence → unbounded-retirement failure mode, and it is what caught the per-chunk-publish VRAM exhaustion described in Step 0.
- [ ] Eviction frequency counters (ground work for Phase 7 residency).
- [ ] Right-size the per-page indirect buffer from measured object counts: today every 4 MB page reserves a fixed 65,536 × 24 B = 1.5 MB argument buffer, while the densest possible page holds ~45k objects. At 10M objects the fixed reservation alone would be ~2.4 GB, which is the sharpest justification for retiring persistent per-page argument buffers entirely (10M plan, Step 7) — do this only as an interim measure if Step 7 is still distant.
- [ ] Decision gate: segregated free-list allocator (from Phase 4) — build only if the page-selection scan shows up in these numbers.

**Phase 7 — extreme performance (only after everything above is done and stable)**

- [ ] **Chained page doubling (4→8→16→32→64 MB).** Fixed 4 MB pages are right while a model's geometry sits in the low GBs, but the arithmetic fails at the top end: filling a 48 GB professional card would need ~12,000 pages — ~24,600 committed resources (each page = geometry buffer + argument buffer, plus today's fixed 1.5 MB argument reservation each) and ~37,000 bind/`ExecuteIndirect` calls per frame, an order of magnitude past our 1000–5000 draw-call budget and against WDDM guidance to keep allocation counts in the low thousands (there is no hard API cap; creation cost, residency tracking and per-draw binding are what bite). Doubling each container's next-page size up to 64 MB puts the same card at ~800 pages. The code is half-ready: `GeometryPage::pageSize` is already per-page and only `CreateNewPage` hardcodes 4 MB — but `IsFull`, clone/compaction volume and argument-buffer sizing must all follow the variable size. Trigger from Phase 6 page-count telemetry; schedule before (or with) residency management below, which also gets cheaper with fewer, larger allocations.
- [ ] **Residency management** (promoted from the known-issues list): `Evict` a tab's pages a few seconds after tab switch, `MakeResident` on return, budget-driven via `IDXGIAdapter3::QueryVideoMemoryInfo` + budget-change notifications. Tab 0 always resident.
- [ ] Replace the final copy-batch CPU fence wait with GPU-side cross-queue waits (fence-tagged snapshots).
- [ ] LOD optimization (instancing or compute shaders, based on camera distance).
- [ ] Compute-shader frustum culling.
- [ ] Mesh-shader implementation (supported hardware, pipes only).
- [ ] GPU-based defragmentation (compute compaction — the CPU-driven clone compaction from Phase 4 should carry us a long way first).
- [ ] Asynchronous resource creation (reduce stalls during page allocation bursts).
- [ ] Page-level optimization: static pages → single draw, semi-dynamic → EI, highly dynamic → EI + GPU compaction.

**Not to do:**

- Multi-GPU rendering (too complex for now; Windows' multi-adapter support is limited).
- Face-wise geometry colors (implementation detail; maybe needed later for mechanical parts).

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

One tab holds up to 10 million simultaneously GPU-resident Scene3D objects, presented through up to 64 independently filtered SubTabs. The objective is not to redraw 10 million objects in every SubTab at 60 Hz; it is to build the identity, memory and command-generation architecture that makes that scale possible, then progressively reduce each SubTab to what its camera needs.

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

64 mask-addressable SubTabs per tab is the design limit, represented directly by a 64-bit membership word (`uint2` in shaders). The fixed slot capacity (`MV_MAX_SUBTABS = 128`) is larger; if more than 64 *simultaneously masked* SubTabs are ever needed, add a second mask word rather than changing anything else here.

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

Dragging burns one slot per edit, but steady state ping-pongs between 2–3 slots per object: a fresh slot is needed only while the previous one is still referenced by an in-flight frame. Slots freed by moves accumulate as holes and are reclaimed by the defragmentation pass in Step 8. Index contiguity is deliberately **not** maintained during normal operation.

### Step 0 — Upload ring and ring-gated submission *(implemented)*

Two Phase 4 items are hard prerequisites for everything below, and they are really one mechanism. This step is the authoritative specification for both; the Phase 4 entries point here.

**The ring.** One persistent-mapped upload buffer per process — 64 MB to start — serving *all* copy-thread staging: Scene3D geometry, indirect-argument and draw-template rebuilds, Page2D record uploads, texture uploads. Allocation is a bump pointer with wraparound; every allocation is tagged with the copy-fence value that will release it, and a region becomes reusable once `copyFence->GetCompletedValue()` has passed that value. Today every object upload creates its own committed staging resource in `RecordGeometryUpload`, which is exactly the stall the original roadmap predicted. The dead per-tab upload heaps go at the same time: `InitD3DPerTab` commits and persistently maps 64 MB (vertex) + 16 MB (index) that nothing ever writes — ~80 MB per tab, directly against the "Hello-World tabs stay lightweight" rule.

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
- **The transform-only path has no producer yet.** It is keyed off a MODIFY whose payload carries a world matrix but empty vertex/index vectors (`IsTransformOnlyEdit`). Nothing emits that today, because every geometry generator bakes world positions into its vertices and leaves `GeometryData::worldMatrix` identity — so a "move" currently regenerates geometry and takes the geometry-changed branch. Giving the generators a real world matrix is the remaining prerequisite for the Phase 5 hot-drag item, and it is now the *only* one.

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

### Step 5 — Visibility mask

Add `VisibilityMask[gpuInstanceIndex]` — one 64-bit SubTab membership word, `uint2` in shaders for broad compatibility — and a per-draw SubTab bit index. CPU-side membership changes stay keyed by `memoryID`; the copy thread resolves them to dense indices and writes the mask directly under invariant 2.

Two consumption paths, in this order:

- **Interim, with today's per-page `ExecuteIndirect`:** the vertex shader tests the bit and emits a degenerate position when it is clear. Toggling is one atomic write — no rebuild, no clone, no upload. Vertex shading still runs for hidden geometry, which is the accepted interim cost.
- **Final, in Step 7:** compute compaction drops hidden objects before they ever become draw commands.

Per-object hide therefore ships well before the compute path exists. Reusing a SubTab bit requires clearing its prior membership first — one compute dispatch over the mask array.

Note that whole-page container filtering — `ExecuteIndirect` argument count 0 when `page.containerMemoryId` is not the active container — is a *page* mechanism, not per-object hide. It stays as a cheap first-level reject.

### Step 6 — SubTabs, Viewports and the container-set directory

1. Rename the content-level “view” concept to **SubTab**. A SubTab has exactly one content type — Scene3D or Page2D, never mixed, because a mixed SubTab would have ambiguous renderer and interaction semantics — and holds a *set* of containers of that type. `VIEW_INSIDE_DATASETTAB`, `tab.views` and `activeViewIndex` are dead code referenced only from a comment; delete them rather than renaming them.
2. Introduce **Viewport** as a separate object owning the Scene3D camera (or Page2D pan/zoom), input/pick state, render-target rectangle and update scheduling. Per-view cameras and Page2D pan/zoom already exist per sub-tab slot; this step lifts them out so several Viewports can show one SubTab with different cameras, and so a window can later host several side by side. The shared `DX12ResourcesPerTab::camera` write-through is already gone — the camera is passed down as a parameter.
3. Replace one-container-only render selection with a SubTab container set. When the set alone defines the subset, store it as a compact rule; do not set a per-object mask bit for every member merely to express "this whole container".
4. Add a snapshot-level `containerMemoryId -> GeometryPage list` directory, so a SubTab that selects a few containers stops walking every page in the tab. Independently of that directory, hoist the container test above the vertex/index buffer binds in `RenderScene3D`: today every page pays two IA binds before its argument count is discovered to be zero.

### Step 7 — SceneEpoch and GPU indirect generation

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

- **`StartIndexLocation` must become absolute first.** It is `(indexByteOffset - page.indexTail) / 2` today, and `indexTail` moves down on every append — so appending one object silently invalidates every other object's start index in that page, which is why a full rebuild is currently mandatory. Bind the index buffer view at the page base and use `indexByteOffset / 2`, stable for the object's stay in the page. Without this, a persistent draw template cannot exist.
- **Index width stays a page property.** Transparency can be a per-object `renderFlags` bit routed to a separate output buffer (classification only; ordering remains a later concern), but 16-bit versus 32-bit index format is set by `IASetIndexBuffer` per page. The page-kind axis therefore survives for index width and for the big-object fallback, which has no page at all.
- **`MaxCommandCount` is a CPU-side constant.** Cap commands per Viewport, clamp in the compute shader, and count overflows in telemetry. A Viewport buffer is owned by exactly one render thread, so classic double buffering against that monitor's own fence is correct for it (invariant 4). It must be **persistent, not drawn from a per-frame ring**, because cull output is cached by `(SceneEpoch, SubTab filter revision, visibility revision)` and regenerated only when those inputs change — a ring would be overwritten before the cache could be reused. Allocate only for active, visible Viewports; never keep 64 full 10-million-command buffers resident.
- **Compute needs descriptor infrastructure that does not exist yet.** The scene root signature is entirely root descriptors today, and the per-tab heap holds one descriptor. A shader-visible heap and a global descriptor allocator are prerequisites. Record the cull dispatch on the same per-monitor direct command list as the draws, with a UAV barrier between dispatch and `ExecuteIndirect`, so no cross-queue synchronisation is needed.

### Step 8 — Defragmentation

Two arenas accumulate holes. Neither is compacted eagerly; both wait until a hole threshold is crossed.

- **Geometry pages** — as already specified in *Defragmentation logic* above: when a page's `holeBytes` crosses ~25%, its next RCU clone copies live ranges packed (per-object `CopyBufferRegion` from the placement records) instead of a whole-page `CopyResource`, at most one page per batch. The argument/template rebuild is mandatory on every clone anyway, so it picks up the remapped offsets for free. **Cross-page** compaction is the new part, and it needs a deliberate pass rather than riding a clone.
- **Instance arena** — every move burns a slot, so holes accumulate at edit rate. Note that the fence-gated free list already recycles them, so the arena does not *grow* without bound; what a pass buys is index contiguity, by relocating live records and rewriting the affected redirect entries. `gpuInstanceIndex` is untouched throughout, which is precisely what the redirect indirection buys.

Index contiguity is deliberately not maintained between passes, and no allocation policy attempts to preserve it. None of this is in the first implementation.

### Step 9 — Spatial data and real GPU culling (deferred)

Bounds and spatial data are deliberately deferred until the previous steps are correct. Design the draw template and page headers so they can later acquire world bounds, a spatial-cell/BVH reference and LOD data.

The culling path then evolves without changing object identity or paging:

```text
SubTab filter -> container-page rejection -> spatial-page rejection
              -> frustum culling -> LOD selection -> optional Hi-Z occlusion
              -> compact visible indirect commands
```

The Step 7 filter/compaction path remains useful as a correctness baseline and fallback.

### Step 10 — Viewport scheduling and scale limits

64 independent SubTabs do not imply 64 equal-rate full-resolution renders. The compositor schedules Viewports according to user value:

| Viewport class | Typical policy |
|---|---|
| Focused interactive | 30–60 Hz, highest LOD budget |
| Visible secondary | Budgeted refresh rate |
| Static background | Render only when dirty |
| Dashboard/thumbnail | Lower resolution and low refresh rate |
| Occluded/minimized | No rendering |

This policy, together with cached cull output and the later spatial hierarchy, is what turns the 10-million-object / 64-SubTab requirement into bounded GPU work rather than 640 million draw commands every frame.
