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

### Actual Code of our graphics engine.
{{< codefile src="code-core/MemoryManagerGPU-DirectX12.h" >}}
{{< codefile src="code-core/MemoryManagerGPU-DirectX12.cpp" >}}

## Detailed design: the memory manager & rendering core

The notes below capture the architecture of our GPU memory manager (the *Vishwakarma* core) and the rendering engine that sits on top of it. They were written as we implemented the engine and are the single source of truth for *why* the code looks the way it does.

At startup, pick up the GPU with the highest VRAM. All rendering happens on that one device only — exactly one device is supported for rendering. The OS may still send the finished frame to a monitor connected to another / integrated GPU.

### Vertex format

Vertex layout is common to all geometry:

- 3 × 4 bytes for **Position**, 4 bytes for **Normal**, 4 bytes for **Color** (RGBA — 8 bytes if an HDR monitor is present) = **20 / 24 bytes per vertex**.
- Always go with the **24-byte** format. Tone mapping (HDR → SDR) happens in the pixel shader.
- Initial development is on `R8G8B8A8`; when we implement HDR later we will upgrade. Some hardware may not support HDR, so keep both versions of the shaders.
- Whether to load HDR or SDR shaders is decided at application startup. If the graphics card supports HDR and at least one monitor is HDR-capable, switch to HDR. Once HDR is ON, the application keeps HDR shaders even if the HDR monitor disconnects — until the app closes.

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

There are multiple views per tab. Each view maintains a double-buffered pair of `ExecuteIndirect` command buffers. When an object is deleted, the copy thread receives the command from the engineering thread, updates the next double buffer and records the hole in the vertex/index buffer (except for the currently filling head buffer).

**Free-list allocator:** maintain a CPU-side segregated free list, per tab. The allocator knows, e.g., "I have a 12 KB middle gap in Page 3 and a 40 KB middle gap in Page 8." When a 10 KB request comes in, it immediately returns "Page 3" — no iterating through page objects. If the free list says no existing page can accommodate the geometry, create a new heap / placed-resource buffer. The free list tracks only middle empty space, not internal holes from deleted objects; aggregate holes are tracked per page and defragmented occasionally.

When a buffer accumulates more than 25% holes, it creates a new defragmented buffer and switches over once complete (for new geometry additions). At most one buffer is defragmented at a time (between two frames). Since the max page size is 64 MB, this does not produce a high-latency stall while running async with the copy thread.

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
| Growth logic | Chained doubling (4→8→16→32→64) | No moving old data |
| Max page size | 64 MB | Prevents fragmentation failure on low-VRAM GPUs |
| Allocation | Lazy (on demand) | Keeps "Hello World" tabs lightweight |
| Sub-allocation | Double-ended stack | Maximizes usage for varying vertex/index ratios |

New geometry is appended (in the middle) only if both the new vertex and index buffers fit inside; otherwise a new buffer is allocated. The copy thread also batches — it aggregates all objects that fit in the current buffer into a single GPU upload, coalescing updates into single `ExecuteCommandList` calls where possible to reduce API overhead.

**"Big buffer" fallback:** if `Allocation_Size > Max_Page_Size`, allocate a dedicated committed resource just for that object, bypassing the paging system. This handles large STL meshes or terrain maps. Treat big buffers as a special page type with a separate "large object list". Don't jam them into the standard EI logic if they need unique per-object resource bindings — one separate draw call per jumbo object. Keep a separate `std::vector<BigObject>` in the tab structure. Rendering: loop through pages (`ExecuteIndirect`), then loop through big objects (standard `DrawIndexedInstanced`, or EI with count 1).

### Defragmentation logic

The copy queue marks a page for defragmentation. All frames of that tab **freeze** (keep presenting the previous render output). Any one render thread/queue reads the mark, transitions the resource to `COMMON` and signals a fence. The copy queue picks it up and, once defragmented, returns the new resource. Freezing a few frames on screen is a recognized engineering trade-off, acceptable to CAD users.

EI argument buffers are tightly coupled to the memory pages. When we defragment a page we must simultaneously rebuild its argument buffer — don't try to "patch" it, regenerate it for that page.

**Growth logic** works similarly. To add a small 10 KB geometry to an existing 64 MB heap that's 50 MB full without blocking the render thread: all views/frames of *that* tab freeze while other tabs keep processing (no global stall). Transition the page to copy-destination, copy the new data, transition it back to render state for the render thread to pick up.

### Freeze logic

Use **Render To Texture (RTT)** to implement the frame freeze, since the swap chain is `FLIP_DISCARD`. Side benefits: HDR handling, UI composition, multi-monitor flexibility, eviction safety, clean defrag freezes.

### Known issues / limitations (to be resolved in a later revision)

- Transparency sorting — accept imperfect sorting for "glass" pages during rotation, and do a CPU sort + args rebuild only when the camera stops.
- Hot page for object drag / active mutation.
- Evict logic.
- Compute-shader frustum culling.
- Telemetry: per-tab VRAM usage graphs, page fragmentation heatmap, eviction frequency counters, copy-queue stall tracking.
- Selection highlighter methodology.
- Mesh shader on supported hardware (RTX 2000 onwards, RX 6000 onwards).
- Instance-based LOD optimization, optionally using a compute shader.

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

**Phase 1 — the visual baseline**
- [x] Vertex format includes normals (required for lighting).
- [x] Hemispherical lighting in shader.
- [x] Mouse zoom / pan / rotate (basic).

**Phase 2 — the "freeze" infrastructure** *(build the mechanism that hides the breakage before breaking the memory model)*
- [x] Render To Texture (RTT) & full-screen quad — detach "drawing" from "presenting".

**Phase 3 — the API pivot (the hardest part)** *(do this before custom heaps, to isolate variables)*
- [x] Structured buffer for world matrix (`StructuredBuffer<float4x4>` + a root-constant index).
- [x] `DrawIndexedInstanced` → `ExecuteIndirect`.

**Phase 4 — the memory manager (the Vishwakarma core)**
- [ ] **[MISSING]** Global upload ring buffer — the copy thread needs a staging area, else the "VRAM pages" step stalls on `CreateCommittedResource`.
- [x] VRAM pages per tab (the stack allocator) — the double-ended stack (vertex up, index down).
- [ ] CPU-side free-list allocator (tracks the holes).
- [x] Tab management / view management (integrating the heaps into the UI).
- [x] Basic ribbon UI.

**Phase 5 — advanced features & polish**
- [x] Migrated to Shader Model 6 (supported by hardware 2016 onwards).
- [ ] VRAM defragmentation (safe now that RTT exists).
- [x] Click / window selection (raycast against the CPU free list / data structures).
- [ ] Instanced optimization for pipes.
- [ ] SSAO.
- [ ] Upgrade vertices to HDR + tonemapping.
- [ ] Transparency sorting (CPU sort + args rebuild when the camera stops moving).

**Phase 6 — performance & telemetry**
- [ ] Per-tab VRAM usage graphs.
- [ ] Page fragmentation heatmap.
- [ ] Eviction frequency counters.
- [ ] Copy-queue stall tracking.

**Phase 7 — extreme performance (only after everything above is done and stable)**
- [ ] LOD optimization (instancing or compute shaders, based on camera distance).
- [ ] Compute-shader frustum culling.
- [ ] Mesh-shader implementation (supported hardware, pipes only).
- [ ] GPU-based defragmentation.
- [ ] Asynchronous resource creation (reduce stalls during heap growth / defragmentation).
- [ ] Page-level optimization: static pages → single draw, semi-dynamic → EI, highly dynamic → EI + GPU compaction.

**Not to do:**

- Multi-GPU rendering (too complex for now; Windows' multi-adapter support is limited).
- Face-wise geometry colors (implementation detail; maybe needed later for mechanical parts).

## Rendering architecture (control flow)

The rendering code is organized into three interchangeable renderer groups sitting on a shared GPU foundation, coordinated by a compositor. One render thread per monitor drives the whole flow top-to-bottom:

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
