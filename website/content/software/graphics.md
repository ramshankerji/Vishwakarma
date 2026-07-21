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

**Free-list allocator:** maintain a CPU-side segregated free list, per tab. The allocator knows, e.g., "I have a 12 KB middle gap in Page 3 and a 40 KB middle gap in Page 8." When a 10 KB request comes in, it immediately returns "Page 3" — no iterating through page objects. If the free list says no existing page can accommodate the geometry, create a new heap / placed-resource buffer. The free list tracks only middle empty space, not internal holes from deleted objects; aggregate holes are tracked per page and defragmented occasionally.

When a buffer accumulates more than 25% holes, it creates a new defragmented buffer and switches over once complete (for new geometry additions). At most one buffer is defragmented at a time (between two frames). Since pages are 4 MB, this does not produce a high-latency stall while running async with the copy thread.

**Root signature:** the constants (View/Proj matrix) go in root constants or a very fast descriptor table, as these don't change between pages. Only the VBV/IBV and the EI argument buffer change per batch/page.

### Object representation

The realistic "worst case" hierarchy for a CAD frame:

- **Index depth:** 16-bit vs 32-bit (hardware requirement) — e.g. nuts/bolts (16) vs engine blocks (32).
- **Transparency:** opaque vs transparent (sorting requirement) — transparent objects must be drawn last for alpha blending.
- **Topology:** triangles (solid) vs lines (wireframe) (PSO requirement) — we cannot draw lines and triangles in the same call.
- **Culling:** single-sided vs double-sided (PSO requirement) — sheet metal vs solids. Since sectioning is a common use case, we may make all geometry double-sided; to be ascertained later.
- **Buffer pages (N):** how many 256 MB blocks are in use.

Total unique batches = 2 × 2 × 2 × 2 × N = **16 × N**. This ensures no pipeline-state reset while rendering a single page — one `ExecuteIndirect` call per page.

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

**Lazy creation:**

- New tab → allocated memory = 0 MB.
- User draws a bolt (solid) → allocate `Solid_Page_0` (4 MB).
- User draws a glass window → allocate `Transparent_Page_0` (4 MB).
- User never draws a wireframe → `Wireframe_Page` stays null.

Resource state is combined, i.e. `D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER`.

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

Everything formerly tracked here now has a roadmap slot: transparency sorting, the hot-drag / active-mutation path and instanced repeated geometry sit in Phase 5, the telemetry counters in Phase 6, and evict/residency logic, compute-shader frustum culling, mesh shaders and instance-based LOD in Phase 7. Resolved since this list was written: selection highlighter methodology — shipped as the GPU pick pass + highlight overlay + rotation cube (Selection3D module).

### Miscellaneous specification

- A uniform 64-bit object ID, unique across all objects in the entire process memory. Each object can have up to ~16 simultaneous variations of vertex geometry / graphics representation.
- We expect roughly 1000–5000 draw calls per frame.
- Multiple partially-overlapping windows, each independently resizable / maximize / minimize.
- The lowest distance between an object and *all* the different view-camera positions is used by the logic threads to decide the Level of Detail.
- A mechanism to manage memory over-pressure, signalling the logic threads to reduce level of detail within some distance.
- The GPU memory manager is a **singleton** — exactly one instance manages all GPU memory.

On a desktop PC with two discrete GPUs and one integrated GPU, each driving one active monitor, we still use exactly one device for rendering all monitors. Windows 10/11 WDDM supports heterogeneous multi-adapter: when a window moves, DWM composites surfaces and copies the frame across adapters if needed. This works but is slow, since every such frame must traverse the PCIe bus.

### Roadmap (to-do list)

As items complete, they move out of this pending list and into the design document proper.

**Phases 1–3 — complete.** The visual baseline (lit 24-byte vertices, hemispherical lighting, mouse navigation), the RTT infrastructure and the API pivot (structured-buffer world matrices, `DrawIndexedInstanced` → `ExecuteIndirect`) are all live; their designs are described in the sections above.

**Phase 4 — the memory manager (the Vishwakarma core)**

Done so far: 4 MB double-ended VRAM pages with RCU clone → mutate → atomic-snapshot publish, per-container page ownership, fence-gated retirement (snapshots, pages, outgrown matrix tables), empty-page GC, world-matrix table with slot free-list and doubling growth, tab/view management, basic ribbon UI.

Remaining, in build order — every later feature funnels through the copy thread, so each item here multiplies the value of everything after it:

- [ ] **Global upload ring buffer.** One persistent-mapped ring (16–32 MB, fence-tracked reclaim) serving ALL copy-thread staging: Scene3D geometry, indirect-argument rebuilds, Page2D uploads, texture uploads. Today every object upload creates its own committed staging resource (`RecordGeometryUpload`), which is exactly the stall the original roadmap predicted. Sub-tasks:
  - [ ] Remove the dead per-tab upload heaps: `InitD3DPerTab` creates + persistently maps 64 MB (vertex) + 16 MB (index) upload buffers that nothing ever writes — ~80 MB of committed memory per tab, directly against the "Hello-World tabs stay lightweight" rule.
  - [ ] Oversize fallback: an upload bigger than the ring gets a one-off committed staging buffer.
  - [ ] Ring capacity becomes the natural batch throttle: a 100k-object import chunks by ring space instead of materializing all staging buffers at once (today's batch memory is unbounded — the open TODO in `GpuCopyThread`).
- [ ] **One submit per copy batch.** The batch path today records + executes + CPU-fence-waits up to three times per tab (page clones, geometry uploads, argument rebuilds). The copy queue executes a command list strictly in order, so clone → upload → rebuild can be one recording, one `ExecuteCommandLists`, one wait just before publish. (Removing even that last CPU wait via GPU-side cross-queue waits is Phase 7 material.)
- [ ] **Page compaction during RCU clone** — replaces the old freeze-based "VRAM defragmentation" item; see the rewritten *Defragmentation logic* section. When a page's `holeBytes` cross ~25%, its Pass-2 clone copies live ranges packed (per-object `CopyBufferRegion` from the placement records) instead of whole-page `CopyResource`. The argument-buffer rebuild — already mandatory on every clone — picks up the remapped offsets for free. At most one page compacts per batch.
- **CPU-side segregated free-list allocator — demoted (telemetry-gated, Phase 6).** The implemented per-batch scan picks the page with the largest middle gap in O(active pages), which is fine below ~1000 pages, and compaction removes most fragmentation pressure. Build the free list only if Phase 6 telemetry shows the scan actually costing something.

**Phase 5 — structural features first, then polish**

Done so far: Shader Model 6; click / window selection — built as a GPU pick pass (object-ID render + fence-gated readback, Selection3D module) instead of the originally planned CPU raycast, plus selection-highlight and rotation-cube overlays.

- [ ] **Page-type axes — the "16 × N" object representation.** Pages are currently keyed by container only and everything draws with one PSO (opaque triangles, 16-bit indices, back-culled). Add a page kind — {opaque | transparent | wireframe} × {16-bit | 32-bit index} — next to `containerMemoryId`, a small PSO table, and a per-kind loop in `RenderScene3D`. This unlocks wireframe display modes, transparency and large meshes; none of the items below can start without it.
- [ ] **Big-object fallback.** Wire the dormant `BigGeometryObject` path: dedicated committed resource, 32-bit indices, own draw call. Today one object above 65,536 vertices silently wraps its 16-bit indices — must land before STL / terrain import ships.
- [ ] **Hot-drag / active-mutation path** (promoted from the known-issues list). An interactive drag today would be a MODIFY per mouse-move — a 4 MB page clone per frame. Fast path: whole-object move/rotate rewrites only the object's world-matrix slot. Prerequisite: the per-frame matrix double-buffer (open TODO on `worldMatrixBuffer`) so in-place matrix writes cannot tear against in-flight frames. Must land before interactive 3D move/rotate tooling.
- [ ] **Transparency sorting** (needs the page-type axes): accept imperfect order during camera motion; CPU sort + argument rebuild when the camera stops.
- [ ] **Instanced rendering for repeated geometry** (pipes, bolts, standard sections): `InstanceCount > 1` + per-instance matrix indirection in the vertex shader. This finally answers the "repeated geometry" open question above, and is the single biggest VRAM lever for plant models.
- [ ] SSAO.
- [ ] **HDR output pipeline** (reworded — the vertex side is already done, colors are FP16): `rttFormat` → `R16G16B16A16_FLOAT`, tonemap draw replacing the RTT→backbuffer `CopyResource` (the full-screen-quad path returns), HDR swap chain + the startup detection rules from the *Vertex format* section.

**Phase 6 — performance & telemetry** (wire into the existing ImprovementData pipeline; this phase also settles the demoted items)

- [ ] Per-tab VRAM usage graphs: page count, `liveBytes`, `holeBytes`, matrix-table size, big-object list size.
- [ ] Page fragmentation heatmap + compaction-trigger counters.
- [ ] Copy-thread health: batch size, batch latency, stall time at the publish fence wait, upload-ring high-water mark.
- [ ] Retire-backlog depth (promote today's `_DEBUG`-only sentinel to a real counter — it catches the frozen-monitor-fence → unbounded-retirement failure mode).
- [ ] Eviction frequency counters (ground work for Phase 7 residency).
- [ ] Right-size the per-page indirect buffer from measured object counts: today every 4 MB page reserves a fixed 65,536 × 24 B = 1.5 MB argument buffer, while the densest possible page holds ~45k objects.
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
