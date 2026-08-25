// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once

//DirectX 12 headers. Best Place to learn DirectX12 is original Microsoft documentation.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-12-graphics
// You need a good dose of prior C++ knowledge and Computer Fundamentals before learning DirectX12.
// Expect to read at least 2 times before you start grasping it !

#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // MUST be before d3d12.h
#include <d3d12.h> //Main DirectX12 API. Included from %WindowsSdkDir\Include%WindowsSDKVersion%\\um
//helper structures Library. MIT Licensed. Added to the project as git submodule.
//https://github.com/microsoft/DirectX-Headers/blob/main/include/directx/d3dx12.h
#include <d3dx12.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <wrl.h>
#include <d3dcompiler.h>
#include <DirectXMath.h> //Where from? https://github.com/Microsoft/DirectXMath ?
#include <vector>
#include <string>
#include <unordered_map>
#include <random>
#include <ctime>
#include <iostream>
#include <thread>
#include <chrono>
#include <map>
#include <list>
#include <deque>

#include "RenderScene3D.h"

#include "ConstantsApplication.h"
#include "MemoryManagerGPU.h"
#include "UserInterface-DirectX12.h"
#include "VirtualMemory.h"
#include "डेटा.h"
#include "Selection3D-DirectX12.h"
#include "RenderScene3D-DirectX12.h"


using namespace Microsoft::WRL;

//DirectX12 Libraries.
#pragma comment(lib, "d3d12.lib") //%WindowsSdkDir\Lib%WindowsSDKVersion%\\um\arch
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

/* Double buffering is preferred for CAD application due to low input lag.Caveat: If rendering time
exceeds frame refresh interval, than strutting distortion will appear. However
we low input latency outweighs the slight frame smoothness of triple buffering.
Double buffering (2x) is also 50% more memory efficient Triple Buffering (3x). */
const UINT FRAMES_PER_RENDERTARGETS = 2; //Initially we are going with double buffering.

// Represents complete geometry and index data associated with 1 engineering object..
// This structure holds information about a resource allocated in GPU memory (VRAM)
struct GpuResourceVertexIndexInfo {
    ComPtr<ID3D12Resource> vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_INDEX_BUFFER_VIEW indexBufferView;
    UINT indexCount;
    uint32_t matrixIndex = 0;

    //TODO: Latter on we will generalize this structure to hold textures, materials, shaders etc.
    // Currently we are letting the Drive manage the GPU memory fragmentation. Latter we will manage it ourselves.
    //uint64_t vramOffset; // Simulated VRAM address
    //uint64_t size;
    // In a real DX12 app, this would hold ID3D12Resource*, D3D12_VERTEX_BUFFER_VIEW, etc.
};

// IndirectCommand and GeometryPlacementRecordInPage are portable ABI layouts (RenderScene3D.h).
// Verify here that the portable drawArguments block matches D3D12_DRAW_INDEXED_ARGUMENTS exactly.
static_assert(sizeof(IndirectCommand::DrawIndexedArguments) == sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
    "IndirectCommand::DrawIndexedArguments must match D3D12_DRAW_INDEXED_ARGUMENTS bit for bit.");

/* Initial capacity of a page's ExecuteIndirect argument buffer, expressed in bytes because that is
what the VRAM budget cares about (graphics.md, Phase 6 "right-size the per-page indirect buffer").

It used to be a flat 65,536 commands = 1.5 MB on EVERY page, which was 27% of each page's 5.5 MB
footprint and sized for a pathological page of single triangles. Measured against a real scene - 56
pages holding ~178 objects each - that reserved 84 MB to hold 240 KB, a ~350x over-reservation, and
at 10M objects the reservation alone would have approached 2.4 GB.

256 KB is a deliberate middle, NOT the measured minimum: a page densely packed with cuboids holds
~6,470 objects and fits inside this, and instancing will raise objects-per-page further since shared
geometry lets one page back far more of them. Pages that genuinely need more grow - see
`indirectCapacity` - so this is a starting point rather than a ceiling. */
constexpr uint32_t kIndirectInitialBytes = 256 * 1024;

/* THE GLOBAL PRIMITIVE LIBRARY, DirectX12 side (graphics.md, "Shared geometry and the primitive
libraries"). One immutable committed resource holding every (shape, LOD) mesh: vertices at offset 0,
indices at `indexByteOffset`. Created and uploaded ONCE by InitD3DDeviceOnly, before any thread
exists, and read-only for the rest of the process - so it needs no snapshot, no retirement and no
fence gating, and its GPU virtual address can be handed straight to a draw command.

~123 KB for the whole 8-LOD sphere ladder. Note it is NOT a GeometryPage on purpose: see the comment
on PrimitiveLibraryTable in RenderScene3D.h for why an RCU-managed page cannot back a cross-tab
reference. */
struct PrimitiveLibrary {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    uint32_t vertexBytes = 0;
    uint32_t indexByteOffset = 0; // Where the index region starts inside `buffer`.
    uint32_t indexBytes = 0;
    PrimitiveLibraryTable table;

    /* The same table as `table`, in the 12-byte GPU form, so the cull pass can look up the level it
    picks per frame (Step 2). Tiny - 8 LODs per shape at 12 bytes - and as immutable as the mesh, so
    it is a plain committed resource bound as a root SRV with no descriptor heap involved. */
    Microsoft::WRL::ComPtr<ID3D12Resource> drawRangeBuffer;
    D3D12_GPU_VIRTUAL_ADDRESS drawRangeVA = 0;

    bool IsReady() const { return buffer != nullptr; }
    D3D12_VERTEX_BUFFER_VIEW VertexView() const {
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = va;
        vbv.SizeInBytes = vertexBytes;
        vbv.StrideInBytes = sizeof(Vertex);
        return vbv;
    }
    // Based at the index REGION, not the buffer base, so a template's startIndexLocation is a plain
    // element offset within that region - the same "absolute within the bound view" rule the
    // geometry pages follow.
    D3D12_INDEX_BUFFER_VIEW IndexView() const {
        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = va + indexByteOffset;
        ibv.SizeInBytes = indexBytes;
        ibv.Format = DXGI_FORMAT_R16_UINT;
        return ibv;
    }
};

struct GeometryPage {
    /* Bespoke pages own `buffer` below; INSTANCED pages leave it null and draw from the global
    primitive library instead, holding only placement records and draw templates (~256 KB against
    ~4.25 MB). Every predicate that reasons about a page's contents has to branch on this - and the
    three that infer "has geometry" from `vertexHead != 0 && indexTail != pageSize` are exactly the
    ones that would otherwise skip instanced geometry with no error anywhere. */
    GeometryPageKind kind = GeometryPageKind::Bespoke;

    // GPU RESOURCES. Single unified 4 MB buffer. NULL on an instanced page.
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;// Layout:[Vertex Region ↑ ][Free Space][ Index Region ↓ ]
    Microsoft::WRL::ComPtr<ID3D12Resource> indirectBuffer;// ExecuteIndirect argument buffer for this page
    uint32_t indirectCount = 0; // Number of valid indirect draw commands
    /* Commands the indirectBuffer can hold. Grown by RebuildIndirectBuffer when a page turns out to
    hold more objects than the initial reservation covers, which is safe precisely because that
    function only ever runs on clones and brand-new pages - never on a published one, so no render
    thread can be reading the buffer being replaced. `indirectCount <= indirectCapacity` always: all
    three readers (legacy draw, cull dispatch, pick/print) are bounded by indirectCount, and the cull
    path binds this as a ROOT SRV, which carries no bounds check at all. */
    uint32_t indirectCapacity = 0;
    uint64_t containerMemoryId = 0; // High-level Scene3D/Page2D/etc. owning every object in this page.

    // ALLOCATION STATE (CPU-side only)
    uint32_t vertexHead = 0; // Vertex region grows upward from 0
    // Index region grows downward from pageSize
    uint32_t indexTail = 0;  // Initialized to pageSize
    uint32_t pageSize = 0;   // Typically 4 * 1024 * 1024
    static constexpr uint32_t SAFETY_GAP = 64; // alignment guard

    // FRAGMENTATION TRACKING
    uint32_t liveBytes  = 0;   // Actively used bytes
    uint32_t holeBytes  = 0;   // Deleted object space
    uint32_t objectCount = 0;  // Active objects

    // VERSIONING & LIFETIME CONTROL
    uint32_t version = 0;                // Incremented on rebuild
    std::atomic<bool> published = false; // Immutable once true
    uint64_t retireFence = 0; // Fence value after which this page is safe to destroy

    std::vector<GeometryPlacementRecordInPage> objects; // CPU METADATA (NO GEOMETRY STORED)

    // UTILITY
    bool IsFull(uint32_t incomingVertexBytes, uint32_t incomingIndexBytes) const  {
        /* An instanced page has no vertex or index region, so "largest middle gap" is meaningless
        for it; what bounds it is how many DRAW TEMPLATES its argument buffer holds. Note the bound
        is indirectCapacity rather than a fixed count: RebuildIndirectBuffer grows that buffer by
        doubling, so this is the page's current capacity, not a ceiling. */
        if (kind == GeometryPageKind::InstancedGlobal) {
            return objects.size() >= indirectCapacity;
        }
        //If: incomingIndexBytes > indexTail then : indexTail - incomingIndexBytes wraps to huge value.
        if (incomingIndexBytes > indexTail) return true;
        uint32_t alignedVertexHead = VertexAlign(vertexHead);
        uint32_t alignedIndexTail  = AlignDown(indexTail - incomingIndexBytes, 4);
		return (alignedVertexHead + incomingVertexBytes + SAFETY_GAP > alignedIndexTail);
    }

    /* Vertex offsets MUST be a whole number of vertices: RebuildIndirectBuffer derives
    BaseVertexLocation as vertexByteOffset / sizeof(Vertex), and the Selection3D highlight path
    repeats that division. Historically sizeof(Vertex) was 24 - not a power of two - so AlignUp's
    mask trick could not express this, and a 16-byte alignment landed on a whole vertex only while
    the page's running vertex total happened to stay even (graphics.md, live defect 1).

    The lean vertex is now 16 bytes, so AlignUp WOULD work - deliberately not switched. Multiplying
    is correct for any stride, costs one divide per object append, and is what the parked 24-byte
    variant needs back. When vertex format becomes a per-PAGE property, this takes the page's stride
    instead of sizeof(Vertex) and the choice matters again. */
    static uint32_t RoundUpToMultiple(uint32_t value, uint32_t multiple) {
        return ((value + multiple - 1) / multiple) * multiple;
    }

    static uint32_t VertexAlign(uint32_t value) {
        return RoundUpToMultiple(value, static_cast<uint32_t>(sizeof(Vertex)));
    }

    static uint32_t AlignUp(uint32_t value, uint32_t alignment) { // Power-of-two alignments only.
        return (value + alignment - 1) & ~(alignment - 1);
    }

    static uint32_t AlignDown(uint32_t value, uint32_t alignment) {
        return value & ~(alignment - 1);
    }
};

/* Global upload ring (graphics.md, 10M plan Step 0). ONE persistent-mapped UPLOAD buffer serves all
copy-thread staging, replacing the per-object committed staging resource that RecordGeometryUpload
used to create for every single object.

Allocation is a bump pointer over MONOTONIC byte cursors; the physical offset is cursor % capacity.
That makes free space simply capacity - (head - tail) with no wrap special-casing in the arithmetic,
and it never overflows in any realistic session. Each submitted chunk tags the region it consumed
with the copy-fence value that releases it; Reclaim advances the tail as those fences complete.

Copy-thread-owned: exactly one thread ever touches it, so nothing here is locked or atomic. */
struct GpuUploadRing {
    static constexpr uint64_t kCapacity  = 64ull * 1024 * 1024; // 64 MB.
    static constexpr uint64_t kAlignment = 256;                 // Also satisfies texture placement.

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    uint8_t* mapped = nullptr;
    uint64_t capacity = 0;
    uint64_t head = 0; // Next free byte.
    uint64_t tail = 0; // Oldest byte still referenced by a copy the GPU has not finished.

    // Everything below `end` is released once the copy fence reaches `fence`.
    struct InFlightRegion { uint64_t end; uint64_t fence; };
    std::deque<InFlightRegion> inFlight;

    uint64_t highWaterBytes = 0; // Peak simultaneous occupancy (telemetry).

    void Initialize();
    void Shutdown();
    void Reclaim(); // Advance the tail past every region whose copy fence has completed.
    // Bump-allocate `bytes` as one physically contiguous region. Returns false when the ring cannot
    // satisfy it right now (or ever, for a payload larger than the ring) - the caller then falls
    // back to a one-off committed staging buffer, so an oversize upload can never deadlock.
    bool Allocate(uint64_t bytes, uint8_t*& outCpu, uint64_t& outOffset);
    // Tag everything allocated since the previous tag with the fence value that will release it.
    void TagSubmission(uint64_t fenceValue);
    uint64_t UsedBytes() const { return head - tail; }
};

/* Copy-thread telemetry (graphics.md Phase 6 groundwork; the counters the 10M plan's four workload
budgets are actually measured against). Written by the copy thread, read by the debug heartbeat on
the render threads - hence atomic, all relaxed: these are diagnostics, never control flow. */
struct GpuCopyStats {
    std::atomic<uint64_t> batches{ 0 };         // Drained batches processed.
    std::atomic<uint64_t> chunks{ 0 };          // Chunks published == copy submits == fence waits.
    std::atomic<uint64_t> commands{ 0 };        // ADD / MODIFY / REMOVE commands applied.
    std::atomic<uint64_t> pagesCloned{ 0 };     // RCU clones performed.
    std::atomic<uint64_t> pagesCompacted{ 0 };  // Clones that packed live ranges instead of copying whole.
    std::atomic<uint64_t> clonedBytes{ 0 };     // Bytes moved by those clones.
    std::atomic<uint64_t> ringBytes{ 0 };       // Bytes staged through the ring.
    std::atomic<uint64_t> oversizeStaging{ 0 }; // Allocations that had to bypass the ring.
    std::atomic<uint64_t> ringHighWater{ 0 };   // Peak ring occupancy.
    std::atomic<uint64_t> queueDeferred{ 0 };   // Commands left queued by the drain cap.
    // Largest per-tab retiredPages + retiredSnapshots seen at the last sweep, and the all-time
    // high. The live value is what tells you retirement is keeping up right now; the peak is what
    // catches a transient stall that has since cleared.
    std::atomic<uint64_t> liveRetireBacklog{ 0 };
    std::atomic<uint64_t> peakRetireBacklog{ 0 };
    std::atomic<uint64_t> maxActivePages{ 0 };  // Largest active page count seen in one tab.
    // Times a page needed a bigger ExecuteIndirect argument buffer than the 256 KB starting
    // reservation. This is the number that says whether kIndirectInitialBytes was chosen well:
    // a steady non-zero rate means pages routinely hold more objects than it covers.
    std::atomic<uint64_t> indirectGrowths{ 0 };
    std::atomic<uint64_t> pendingIndexes{ 0 };  // gpuInstanceIndexes in their zombie interval (Step 3).
    std::atomic<uint64_t> freeIndexes{ 0 };     // gpuInstanceIndexes available for reuse.
    std::atomic<uint64_t> pendingSlots{ 0 };    // Arena slots waiting on a fence (Step 4).
    std::atomic<uint64_t> freeSlots{ 0 };       // Arena slots available for reuse.
    std::atomic<uint64_t> transformOnlyEdits{ 0 }; // Moves that cloned zero geometry pages (Step 4).
    std::atomic<uint64_t> maskWrites{ 0 };      // VisibilityMask entries written (Step 5).
    std::atomic<uint64_t> hiddenInstances{ 0 }; // Objects hidden in at least one SubTab right now.

    /* Page2D copy path (id.md §11, step 2a). ProcessCad2DCopyBatch rebuilds every container of the
    tab from scratch on every batch, so each counter below is O(records) today and each one is
    retired by a NAMED later sub-step. They are the BEFORE baseline the paging work has to beat,
    and they stay afterwards as the regression guard. The number that matters is a RATIO:
    `cad2dCommands` against the three record counters. Five commands moving a million records is
    the defect, stated in one line. */
    std::atomic<uint64_t> cad2dBatches{ 0 };        // ProcessCad2DCopyBatch invocations.
    std::atomic<uint64_t> cad2dCommands{ 0 };       // 2D commands in those batches.
    // These two were the per-batch index rebuild and the two full deep copies. Step 2b retired
    // both: `indexed` is now one hash lookup per COMMAND and `copied` is structurally zero. They
    // stay because "this number must not grow with the drawing" is the cheapest regression guard.
    std::atomic<uint64_t> cad2dRecordsIndexed{ 0 }; // Hash lookups into TabCad2DStorage::recordIndex.
    std::atomic<uint64_t> cad2dRecordsCopied{ 0 };  // Records deep-copied. Must stay 0.
    std::atomic<uint64_t> cad2dRecordsExpanded{ 0 };// GPU records produced by the expansion (2c).
    std::atomic<uint64_t> cad2dBytesStaged{ 0 };    // Bytes staged for 2D page buffers (2c).
    std::atomic<uint64_t> cad2dPagesBuilt{ 0 };     // Pages created and uploaded (2c).
    // Pages a container rebuild threw away. It is the count that says whether edits are landing
    // on the append path (built small, retired 0) or still forcing rebuilds (2c/2d).
    std::atomic<uint64_t> cad2dPagesRetired{ 0 };
    /* Records whose 4-byte `flags` word was overwritten in place - selection, deselection and the
    hide half of a modify (2d). It is the counter that says the cheap path was taken: a selection
    click on a line reads `pages=+0/-0 staged=4 B flags=1` where it used to retire and rebuild
    every page of the container. */
    std::atomic<uint64_t> cad2dFlagStores{ 0 };
    /* Pages packed by the compaction (2e), and the hole records resident across the tab AFTER it
    ran. The second is a GAUGE, stored not added: it is the number the step exists to bound, and a
    running total would only say how much churn there had been. Watch it flatten while
    `cad2dPagesBuilt` stays put - that pair is what "10k modifies leave the page count stable and
    holes bounded" looks like from outside. */
    std::atomic<uint64_t> cad2dPagesCompacted{ 0 };
    std::atomic<uint64_t> cad2dHoleRecords{ 0 };
    std::atomic<uint64_t> cad2dBatchMicros{ 0 };    // Cumulative wall time inside the function.
    std::atomic<uint64_t> cad2dLastBatchMicros{ 0 };// The last batch alone - what you read for "+5 lines".
};
extern GpuCopyStats gCopyStats;

struct BigGeometryObject {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> indirectBuffer;
    uint32_t indexCount = 0;
    uint32_t matrixIndex = 0;
    uint64_t retireFence = 0;
    std::atomic<bool> published = false;
};

struct GeometryPageSnapshot {// A lightweight, immutable snapshot of the current pages.
    // We use raw pointers here because the Render thread only needs to observe them.
    // Iterating over a contiguous array of pointers is extremely cache-friendly.
    std::vector<GeometryPage*> pages;

    /* containerMemoryId -> that container's pages (graphics.md, 10M plan Step 6, item 4). Built
    once by the copy thread when the snapshot is published, then strictly read-only - the same
    immutability the pages vector already has.

    Without it every view walked every page in the tab and paid two IA binds per page just to
    discover its ExecuteIndirect count was 0. With it a SubTab visits only the pages it can
    actually draw, so the container test happens ABOVE the binds rather than after them, and a
    tab holding thousands of pages across many containers stops costing every view the whole set. */
    std::unordered_map<uint64_t, std::vector<GeometryPage*>> pagesByContainer;
};

struct TabGeometryStorage {
    // THE RCU POINTER: Render threads read this, Copy thread writes to it.
    std::atomic<GeometryPageSnapshot*> activeSnapshot{ nullptr };
    // WRITER-ONLY STATE: Only the Copy thread touches these, so they need no locks/atomics.
    std::vector<std::unique_ptr<GeometryPage>> activePages; // Actually owns the memory

    // Cleanup queues for the Copy thread
    struct RetiredSnapshot { GeometryPageSnapshot* snapshot; uint64_t retireFence; };
    struct RetiredPage { std::unique_ptr<GeometryPage> page; uint64_t retireFence; };
    // There is no retired-buffer queue: the only resource that ever outgrew itself was the
    // world-matrix table, and the Step 2 instance arena grows by committing tiles behind a fixed
    // virtual address instead of allocating a bigger buffer and copying into it.
    std::vector<RetiredSnapshot> retiredSnapshots;
    std::vector<RetiredPage> retiredPages;

    /* TODO: RCU version of all of the following vectors need to be developed. Only 1st done so far.
    std::vector<std::unique_ptr<GeometryPage>> opaquePages; // Opaque geometry pages
    std::vector<std::unique_ptr<GeometryPage>> transparentPages; // Transparent geometry pages
    std::vector<std::unique_ptr<GeometryPage>> wireframePages; // Wireframe pages (if used)
    std::vector<std::unique_ptr<BigGeometryObject>> bigObjects; // Dedicated large objects
    std::atomic<uint32_t> currentVersion = 0;
    std::vector<std::unique_ptr<GeometryPage>> retiredPages;
    */
};

/* Instance arena geometry (graphics.md, 10M plan Step 2). D3D12 buffer tiles are always 64 KB, so a
tile holds a whole number of records (1024 at 64 B) or redirect entries (16384 at 4 B), and every
growth step lands on an element boundary. */
constexpr uint32_t kInstanceArenaTileBytes   = 65536;
constexpr uint32_t kInstanceInitialCapacity  = 4096; // 4 record tiles. First step the old table used.
constexpr uint32_t kInvalidInstanceIndex     = 0xFFFFFFFFu;
constexpr uint32_t kInvalidInstanceSlot      = 0xFFFFFFFFu;

/* A reserved (tiled) DEVICE-LOCAL buffer whose virtual address never moves: the address range is
reserved in full at tab creation and physical 64 KB tiles are committed on demand as it grows. Three
per tab (10M plan Steps 2, 4 and 5):

  instanceArena  - 64-byte InstanceRecords, addressed by instanceSlot
  instanceSlotOf - 4-byte slot ids,         addressed by gpuInstanceIndex
  visibilityMask - 8-byte SubTab membership words, addressed by gpuInstanceIndex

A fixed VA is the whole point. It deletes the grow-copy, the retired-buffer queue and the atomic
address mirrors that a doubling committed buffer forces on every reader.

Copy-thread-owned. `va` is written once before any thread can see the tab and read lock-free by the
render threads thereafter; `capacity` is copy-thread-only. */
struct TiledInstanceBuffer {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    // One heap per growth step, and the step doubles, so a dozen heaps back the whole range. One
    // heap per 64 KB tile would be thousands of allocations, against WDDM guidance.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Heap>> heaps;
    uint32_t elementBytes = 0;
    uint32_t capacity = 0;    // Elements backed by committed tiles.
    uint32_t maxCapacity = 0; // Elements covered by the reservation.

    void Initialize(uint32_t elementBytes, uint32_t maxCapacity, const wchar_t* debugName);
    void Shutdown();
    // Commit tiles until at least minimumCapacity elements are backed. Returns true when tiles were
    // actually committed (the caller may then need to refresh a descriptor). Throws HrException on
    // allocation failure or when the reservation is exhausted.
    bool Grow(uint32_t minimumCapacity);
    uint32_t ElementsPerTile() const { return kInstanceArenaTileBytes / elementBytes; }
};

/* One entry of a tab's identity registry, addressed by the same dense gpuInstanceIndex as the
instance arena (graphics.md, 10M plan Step 3). The design lists the reverse direction as three
separate flat arrays; one array of 32-byte entries is the same bytes with one commit path and one
cache line per lookup, which is what a pick actually does, so that is what is built here. */
struct InstanceRegistryEntry {
    uint64_t memoryID = 0;        // 0 = the index is free, or inside its post-REMOVE zombie interval.
    GeometryPage* page = nullptr; // Geometry page currently holding the object - a movable LOCATION,
    uint32_t pageSlot = 0;        // and its record index inside that page. Never an identity.
    // Arena slot currently holding this object's 64-byte InstanceRecord, i.e. the CPU's copy of
    // what InstanceSlotOf[gpuInstanceIndex] says on the GPU. Kept here so a transform edit knows
    // which slot to hand back once its fence clears (10M plan Step 4).
    uint32_t instanceSlot = kInvalidInstanceSlot;
    // World-space AABB centre, written by the copy thread on every transform edit. This is the
    // "CPU-side transform shadow" Step 2 asks for: the arena is device-local now, so the pick
    // resolve can no longer read a mapped matrix pointer and transform a local AABB centre itself.
    float worldCenterX = 0.0f, worldCenterY = 0.0f, worldCenterZ = 0.0f;
    /* CPU shadow of the object's InstanceRecord::packedColor, and it is REQUIRED, not a convenience.
    A transform-only MODIFY allocates a FRESH arena slot and writes all 64 bytes of a new record,
    but its GeometryData arrives with empty vertex/index vectors - carrying only a matrix - and the
    arena is device-local, so the copy thread cannot read the old record back. Without this field a
    drag would rewrite the record with packedColor = 0 and the object would silently turn black.
    (graphics.md flagged exactly this defect under 10M plan Step 4, waiting for the first producer
    of the appearance bytes; the 16-byte vertex is that producer.)

    Only packedColor is shadowed. materialIndex / renderFlags / packedParams still have no producer,
    so they are still written as zero and there is nothing to preserve across a move yet. */
    uint32_t packedColor = 0;
};
static_assert(sizeof(InstanceRegistryEntry) == 40,
    "InstanceRegistryEntry must stay 40 bytes - one cache line covers any single lookup.");

/* Copy-thread-owned identity registry for one tab. memoryID -> gpuInstanceIndex is the ONLY hash
map; everything keyed BY the dense index is the flat array above, because three unordered_maps at
10M entries would cost well over a gigabyte in node overhead alone.

The array is a VirtualMemory reservation with 64 KB blocks committed as the index space grows. That
is not only about sparseness: the RENDER threads read entries (the pick resolve), so the base
address must never move under them - a std::vector would relocate. Entries are written by the copy
thread and read by render threads without a lock, the same benign staleness contract the mapped
matrix table used to have. */
struct InstanceRegistry {
    static constexpr uint64_t kCommitBlockBytes = 64 * 1024; // 2048 entries per commit.

    InstanceRegistryEntry* entries = nullptr; // Fixed for the tab's lifetime. Render threads read it.
    uint64_t reservedBytes = 0;
    uint64_t committedBytes = 0;
    std::atomic<uint32_t> committedCount{ 0 }; // Entries addressable right now. Only ever grows.
    std::unordered_map<uint64_t, uint32_t> indexOfMemoryId; // The one hash map. Copy thread only.

    void Initialize(uint32_t maxInstances);
    void Shutdown();
    void Commit(uint32_t count); // Make [0, count) addressable. Copy thread only.
    // Drop every mapping and every page pointer. The committed pages stay - the tab is being torn
    // down and its VA reservation is released by Shutdown.
    void Clear();

    // Copy-thread accessors. The caller has already ensured the index was allocated.
    InstanceRegistryEntry& operator[](uint32_t index) { return entries[index]; }
    uint32_t Find(uint64_t memoryID) const {
        auto it = indexOfMemoryId.find(memoryID);
        return it == indexOfMemoryId.end() ? kInvalidInstanceIndex : it->second;
    }

    // Render-thread accessor: bounds-checked against the committed range, and copies the entry out
    // so the caller never holds a reference into memory the copy thread is writing.
    bool TryRead(uint32_t index, InstanceRegistryEntry& out) const {
        if (!entries || index >= committedCount.load(std::memory_order_acquire)) return false;
        out = entries[index];
        return true;
    }
};

/* DirectX 12 resources are organized at 3 levels:
1. The Data   : Per Tab (Jumbo Buffers for geometry data, materials, textures,
    Pipeline State Object, Root Signature, Command Signature etc.)
2. The Target : Per Window (Swap Chain, Render Targets, Depth Stencil Buffer etc.)
3. The Worker : Per Render Thread. 1 For each monitor. (Command Queue, Command List etc.
    Resources shared across multiple windows on the same monitor) */

struct DX12ResourcesPerTab { // (The Data) Geometry Data

    // No per-tab upload heaps: ALL copy-thread staging goes through the one global GpuUploadRing
    // (gpu.uploadRing). The 64 MB + 16 MB per-tab heaps that used to live here were committed and
    // persistently mapped by InitD3DPerTab and never written by anything - 80 MB of VRAM per tab,
    // straight against the "Hello-World tabs stay lightweight" rule (graphics.md, 10M plan Step 0).

	// TODO: We will generalize this to hold materials, shaders, textures etc. unique to this project/tab
    ComPtr<ID3D12DescriptorHeap> srvHeap;

    mutable std::mutex objectsOnGPUMutex;// Make mutex mutable so const references can lock it in rendering paths.
    // Copy thread will update the following map whenever it adds/removes/modifies an object on GPU.
    std::map<uint64_t, GpuResourceVertexIndexInfo> objectsOnGPU;

    /* THE INSTANCE ARENA AND ITS REDIRECT TABLE (graphics.md, 10M plan Steps 2 and 4).

        instanceSlotOf[gpuInstanceIndex] -> instanceSlot        4 bytes, mutated in place
        instanceArena[instanceSlot]      -> InstanceRecord     64 bytes, immutable while published

    The indirection is what makes a move cost nothing. A transform edit allocates a FRESH slot,
    writes the record where no published frame can reach it, then flips one naturally-aligned
    4-byte redirect entry. A concurrent reader sees the old slot or the new slot - both hold valid
    transforms - so the worst case is one frame of staleness on one monitor. No geometry page is
    cloned, no argument buffer is rebuilt, no snapshot is published. Rewriting the 64-byte record
    in place instead (which is what Step 3 shipped, knowingly) can be observed half-written: a
    garbage matrix, not a stale one.

    Both buffers have a virtual address fixed for the tab's lifetime - see TiledInstanceBuffer for
    what that deleted. Copy thread owns every field below; render threads read only the two VAs and
    the registry's committed range. */
    TiledInstanceBuffer instanceArena;  // 64-byte records, indexed by instanceSlot.
    TiledInstanceBuffer instanceSlotOf; // 4-byte slot ids, indexed by gpuInstanceIndex.

    /* THE VISIBILITY MASK (10M plan Step 5). 8 bytes per gpuInstanceIndex - a 64-bit SubTab
    membership word, bit N = visible in the sub-tab occupying slot N. Hiding or showing an object is
    ONE aligned write here: no geometry clone, no argument rebuild, no snapshot publish, no arena
    slot burned. It is grown in lockstep with instanceSlotOf because both are addressed by the same
    dense index, and both are bound as ROOT descriptors, which carry no bounds check - a read past
    the committed tiles is undefined, not clamped. */
    TiledInstanceBuffer visibilityMask; // 8-byte membership words, indexed by gpuInstanceIndex.

    /* CPU shadow of every index whose mask is NOT kVisibleInAllSubTabs, i.e. exactly the objects
    hidden somewhere. Two jobs, both of which would otherwise need a compute dispatch over all 10M
    entries (and the shader-visible descriptor heap that does not exist until Step 7):

      - a SET_VISIBILITY names one sub-tab bit, so the copy thread needs the object's CURRENT word
        to compute the new one;
      - retiring a sub-tab slot must clear that bit everywhere before the slot is reused, and this
        bounds that sweep by the number of hidden objects instead of by the index space.

    Copy-thread-owned, like the registry. Entries are erased on REMOVE and when a mask returns to
    the all-visible default, so an unhidden scene costs nothing. */
    std::unordered_map<uint32_t, uint64_t> hiddenInstanceMasks;

    uint32_t instanceCount = 0;    // Highest gpuInstanceIndex ever handed out (dense high-water).
    uint32_t instanceSlotCount = 0;// Highest instanceSlot ever handed out.
    std::vector<uint32_t> freeInstanceIndexes; // Past their zombie interval, reusable.
    std::vector<uint32_t> freeInstanceSlots;   // Ditto for arena slots.

    /* Indices vacated by REMOVE and slots vacated by REMOVE / MODIFY, held until every monitor's
    render fence has passed the value tagged after the chunk's copies completed - the zombie
    interval of Step 3, and the same mechanism Step 1 shipped for matrix slots. Handing either back
    immediately would let a later edit in the SAME batch take it and overwrite live data while
    render threads are still drawing frames that reference it: for an index, the pre-publish
    snapshot's indirect buffer still names it; for a slot, an in-flight frame may still be reading
    the pre-flip redirect value. The safeRetireFence sweep does both handovers.

    Note the asymmetry: MODIFY burns a SLOT but never an INDEX, because gpuInstanceIndex is the
    object's stable identity and survives every edit. */
    struct PendingInstanceIndex { uint32_t index; uint64_t retireFence; };
    std::vector<PendingInstanceIndex> pendingFreeInstanceIndexes;
    std::vector<PendingInstanceIndex> pendingFreeInstanceSlots;

    // memoryID <-> gpuInstanceIndex and the object's current geometry location (Step 3).
    InstanceRegistry registry;

	// Initially rootSignature & pipelineState were in PerWindow, but now moved here,
    // when adding commandSignature and indirect drawing infrastructure.
    // Since Root Signature and Pipeline State are closely tied to the command signature, 
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;

    ComPtr<ID3D12CommandSignature> commandSignature;// Indirect Drawing
    /* The 56-byte signature the ONE-per-Viewport ExecuteIndirect uses (10M plan Step 7). Same root
    signature and same root constant as `commandSignature`, plus a vertex and index buffer view per
    command - which is what allows one call to draw templates originating in different pages. The
    24-byte signature above is still what the legacy draw path, the GPU pick pass and the print path
    execute, since those read a page's templates directly. */
    ComPtr<ID3D12CommandSignature> visibleCommandSignature;

    // 3D click-selection: GPU picking + highlight + rotation-cube resources (Selection3D module).
    Selection3DResources selection3D;
    PickPassContext pickCtx; // Render-thread pick scratch (targets, readback, in-flight state).

    // NO camera here. Cameras are per view (InternalSubTab::camera, tab.camera as fallback) and are
    // resolved per window per frame by ResolveWindowViewTarget, then passed down as a parameter. A
    // camera field here would be shared by every window/monitor showing this tab, so two render
    // threads drawing two views of one tab would race on it.
};

struct DX12ResourcesPerWindow {// Presentation Logic
    int WindowWidth = 800;//Current ViewPort ( Rendering area ) size. excluding task-bar etc.
    int WindowHeight = 600;
    // True for extracted view windows: no top ribbon / bands, the scene fills the whole client area.
    bool contentOnly = false;
    ID3D12CommandQueue* creatorQueue = nullptr; // Track which queue this windows was created with.
    //To assist with migrations.
    
    ComPtr<IDXGISwapChain3>         swapChain; // The link to the OS Window
	//ComPtr<ID3D12CommandQueue>    commandQueue; // Moved to OneMonitorController
    ComPtr<ID3D12DescriptorHeap>    rtvHeap;
    ComPtr<ID3D12Resource>          renderTargets[FRAMES_PER_RENDERTARGETS];

    // Render To Texture Infrastructure
    ComPtr<ID3D12Resource>          renderTextures[FRAMES_PER_RENDERTARGETS];
    ComPtr<ID3D12DescriptorHeap>    rttRtvHeap;
    ComPtr<ID3D12DescriptorHeap>    rttSrvHeap;
    
    // TODO: When we will implement HDR support, we wil have change above format to following.
    //DXGI_FORMAT                     rttFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // HDR ready

    ComPtr<ID3D12Resource> depthStencilBuffer;// Depth Buffer (Sized to the window dimensions)
    ComPtr<ID3D12DescriptorHeap> dsvHeap;

    D3D12_VIEWPORT viewport;// Viewport & Scissor (Dependent on Window Size).
    D3D12_RECT scissorRect;

    ComPtr<ID3D12Resource> constantBuffer;
    ComPtr<ID3D12DescriptorHeap> cbvHeap;
    UINT8* cbvDataBegin = nullptr;

    // Per-window dynamic UI overlay buffers (created lazily by RenderUIOverlay). They must not
    // be shared between windows: one monitor command list records all its windows before
    // executing, so a shared upload buffer would show the last-recorded window's UI everywhere.
    ComPtr<ID3D12Resource> uiVertexBuffer;
    ComPtr<ID3D12Resource> uiIndexBuffer;
    ComPtr<ID3D12Resource> uiOrthoConstantBuffer;
    UINT8* pUIVertexDataBegin = nullptr;
    UINT8* pUIIndexDataBegin = nullptr;
    UINT8* pUIOrthoDataBegin = nullptr;

    // Per-window Page2D view constant buffer (created lazily by RenderCad2DPage). Per window for
    // the same reason as the UI overlay buffers: two windows can display two different Page2Ds of
    // one tab, and a shared per-tab buffer would render both with the last-recorded window's view.
    ComPtr<ID3D12Resource> cad2dViewConstantBuffer;
    UINT8* pCad2DViewConstantDataBegin = nullptr;

	UINT frameIndex = 0; // Remember this is different from allocatorIndex in Render Thread.
    // It can change even during windows resize.
};

// Opaque platform aliases (selected in GPUPlatformSelector.h). Core data structures
// (DATASETTAB, SingleUIWindow) use these so they never name a graphics-API type directly.
// On other platforms the same alias names bind to the Vulkan / Metal resource structs.
using PlatformTabGpu    = DX12ResourcesPerTab;
using PlatformWindowGpu = DX12ResourcesPerWindow;

/* Per-render-thread scratch for GPU draw-command compaction (graphics.md, 10M plan Step 7). Every
page of a Viewport dispatches into the SAME `visibleIndirect` and the SAME `visibleCount`, and one
ExecuteIndirect then draws the lot - which is why each command carries its page's buffer views.

One scratch per MONITOR, not per Viewport, and that is deliberate: two windows on two monitors can
resolve to the same tab's active sub-tab (ResolveWindowViewTarget), so a per-Viewport buffer would
have two owning render threads and break invariant 4. Per monitor, the owning thread is unique.

Several Viewports on ONE monitor share this scratch safely because they are recorded in order into
one command list: the next Viewport's barrier back to UNORDERED_ACCESS drains the previous one's
ExecuteIndirect before its dispatches can overwrite anything.

The two state fields track the buffers across pages/windows WITHIN a frame; they reset to COMMON each
frame after the command list is reset, because a buffer decays to COMMON at every
ExecuteCommandLists boundary. */
struct SceneCullScratch {
    ComPtr<ID3D12Resource> visibleIndirect; // kMaxCommands * sizeof(VisibleIndirectCommand) = 3.5 MB.
    ComPtr<ID3D12Resource> visibleCount;    // One uint at byte 0, written via InterlockedAdd.
    D3D12_RESOURCE_STATES visibleState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES countState = D3D12_RESOURCE_STATE_COMMON;
    // Cap on commands per Viewport - a CPU-side constant by design (10M plan Step 7). The shader
    // drops anything past it rather than wrapping, and ExecuteIndirect's MaxCommandCount enforces
    // the same bound independently, so an overflow costs dropped draws and a telemetry count,
    // never a buffer overrun.
    static constexpr uint32_t kMaxCommands = 65536;

    /* Fence-gated read-back of the surviving command count, purely for telemetry. Written on the
    GPU timeline, so it can only be read once the frame that produced it has retired - hence a
    fence value and a one-frame delay rather than a Map right after recording.

    One slot per Viewport recorded this frame: the count is reset per Viewport, so a single slot
    would report only the last one. A monitor showing more Viewports than there are slots simply
    stops recording the surplus - this is a counter, not a correctness mechanism. */
    static constexpr uint32_t kCountReadbackSlots = 8;
    ComPtr<ID3D12Resource> countReadback;   // READBACK heap, kCountReadbackSlots uints.
    uint32_t readbackSlotsRecorded = 0;     // Viewports whose count this frame recorded.
    uint32_t readbackSlotsPending = 0;      // Same, for the frame `readbackFence` refers to.
    uint64_t readbackFence = 0;             // 0 = nothing in flight.
};

/* Render-thread telemetry, the counterpart to the copy thread's GpuCopyStats and printed beside it
on the debug heartbeat. Written by render threads, read by whichever one prints - hence atomic, all
relaxed: diagnostics, never control flow. */
struct GpuRenderStats {
    // Commands actually drawn by the last completed frame's compaction pass, summed over that
    // monitor's Viewports. A LIVE gauge, not a running total: it is what the 10M plan's "reduce
    // each SubTab to what its camera needs" is ultimately measured by.
    std::atomic<uint64_t> drawnCommands{ 0 };
    // Times a Viewport's surviving commands exceeded SceneCullScratch::kMaxCommands and were
    // dropped. Cumulative, and a non-zero value means the view was visibly incomplete.
    std::atomic<uint64_t> commandOverflows{ 0 };
};
extern GpuRenderStats gRenderStats;

struct DX12ResourcesPerRenderThread { // This one is created 1 for each monitor.
    // For convenience only. It simply points to OneMonitorController.commandQueue
	ComPtr<ID3D12CommandQueue> commandQueue;

    // Note that there are as many render thread as number of monitors attached.
    // Command Allocators MUST be unique to the thread.
    // We need one per frame-in-flight to avoid resetting while GPU is reading.
    ComPtr<ID3D12CommandAllocator> commandAllocators[FRAMES_PER_RENDERTARGETS];
	UINT allocatorIndex = 0; // Remember this is different from frameIndex available per Window.

    // The Command List (The recording pen). Can be reset and reused for multiple windows within the same frame.
    ComPtr<ID3D12GraphicsCommandList> commandList;

    // GPU draw-command compaction scratch, one per monitor (10M plan Step 7 slice).
    SceneCullScratch cullScratch;

    // Synchronization (Per Window VSync)
    HANDLE fenceEvent = nullptr;
    ComPtr<ID3D12Fence> fence; // TODO: Discard this. use the fence inside monitor.
};

struct OneMonitorController { // Variables stored per monitor.
    // System Fetched information.
    bool isScreenInitalized = false;
    int screenPixelWidth = 800;
    int screenPixelHeight = 600;
    int screenPhysicalWidth = 0; // in mm
    int screenPhysicalHeight = 0; // in mm
    int WindowWidth = 800;//Current ViewPort ( Rendering area ) size. excluding task-bar etc.
    int WindowHeight = 600;

    HMONITOR hMonitor = NULL; // Monitor handle. Remains fixed as long as monitor is not disconnected / disabled.
    std::wstring monitorName;            // Monitor device name (e.g., "\\\\.\\DISPLAY1")
    std::wstring friendlyName;           // Human readable name (e.g., "Dell U2720Q")
    RECT monitorRect;                    // Full monitor rectangle
    RECT workAreaRect;                   // Work area (excluding task bar)
    int dpiX = 96;                       // Effective DPI used by Windows scaling
    int dpiY = 96;                       // Effective DPI used by Windows scaling
    int rawDpiX = 96;                    // Native monitor DPI (raw hardware DPI)
    int rawDpiY = 96;                    // Native monitor DPI (raw hardware DPI)
    int physicalDpiX = 96;               // Calculated physical DPI from pixel size and physical size
    int physicalDpiY = 96;               // Calculated physical DPI from pixel size and physical size
    double scaleFactor = 1.0;            // Scale factor (100% = 1.0, 125% = 1.25, etc.)
    bool isPrimary = false;              // Is this the primary monitor?
    DWORD orientation = DMDO_DEFAULT;    // Monitor orientation
    int refreshRate = 60;                // Refresh rate in Hz
    int colorDepth = 32;                 // Color depth in bits per pixel

    bool isVirtualMonitor = false;       // To support headless mode.

    UITopRibbonLayout topRibbonLayout;   // DPI-specific cached geometry for top UI ribbon.

    // DirectX12 Resources.
	// TODO: Move these to per render thread structure.
	ComPtr<ID3D12CommandQueue> commandQueue;    // Persistent. Survives thread restarts.
    bool hasActiveThread = false;// We need to know if this specific monitor is currently being serviced by a thread
    ComPtr<ID3D12Fence> renderFence; // Signalled each frame by GpuRenderThread
    uint64_t renderFenceValue = 0; // Last value signalled (written by render thread)
    // Above is intentionally NOT std::atomic since gpu.renderFenceValue is the std::atomic serving all monitors.
    HANDLE renderFenceEvent = nullptr;

    // Per-monitor UI icon atlas. The icon cell size scales with this monitor's (DPI-floored) layout, so
    // each monitor owns its own icon texture plus a shader-visible SRV heap (slot 0 = shared English MSDF
    // texture, slot 1 = this monitor's icon texture). (Re)built by BuildMonitorIconAtlas and released +
    // rebuilt on DPI / topology change. Bound by RenderUIOverlay and RenderPage2D for this monitor.
    ComPtr<ID3D12Resource>       uiIconAtlasTexture;
    ComPtr<ID3D12DescriptorHeap> uiSrvHeap;
};

// CommandToCopyThreadType / CommandToCopyThread moved to RenderScene3D.h (portable).

extern std::atomic<bool> pauseRenderThreads; // Defined in Main.cpp

// Packet of work for a Render Thread for one frame
struct RenderPacket {
    uint64_t frameNumber;
    std::vector<uint64_t> visibleObjectIds;
};

class HrException : public std::runtime_error// Simple exception helper for HRESULT checks
{
public:
    HrException(HRESULT hr) : std::runtime_error("HRESULT Exception"), hr(hr) {}
    HRESULT Error() const { return hr; }
private:
    const HRESULT hr;
};

inline void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) { throw HrException(hr); }
}


// ThreadSafeQueueGPU / g_gpuCommandQueue moved to RenderScene3D.h (portable).

enum class UploadType : uint8_t {
    Texture2D,
    Buffer
};

struct TextureUploadDesc {
    uint32_t width;
    uint32_t height;
    DXGI_FORMAT format;
    const uint8_t* pixels;
    uint32_t rowPitch; // CPU row pitch
};

struct UploadRequest {
    UploadType type;
    union {
        TextureUploadDesc texture;
        // future: buffer uploads
    };
    ComPtr<ID3D12Resource>* outResource;// OUTPUT (written by copy thread)
    std::atomic<uint64_t>* completionFence;// completion tracking (lock-free)
};

constexpr uint32_t MAX_UPLOAD_REQUESTS = 1024;
struct UploadQueue {
    std::atomic<uint32_t> writeIndex = 0;
    std::atomic<uint32_t> readIndex = 0;
    UploadRequest requests[MAX_UPLOAD_REQUESTS];
};
extern UploadQueue gUploadQueue;

// VRAM Manager : This class handles the GPU memory dynamically.
// There will be exactly 1 object of this class in entire application. Hence the special name.
// भगवान शंकर की कृपा बनी रहे. Corresponding object is named "gpu".
class शंकर {
public:
    OneMonitorController screens[MV_MAX_MONITORS];
    int currentMonitorCount = 0; // Global monitor count. It can be 0 when no monitors are found (headless mode)

    // IDXGIFactory6 / IDXGIAdapter4 Prerequisite : Windows 10 1803+ / Windows 11
    ComPtr<IDXGIFactory6> factory6; //The OS-level display system manager. Can iterate over GPUs.
    ComPtr<IDXGIAdapter4> hardwareAdapter;// Represents a physical GPU device.
    //Represents 1 logical GPU device on above GPU adapter. Helps create all DirectX12 memory / resources / comments etc.

	ComPtr<ID3D12Device> device; //Very Important: We support EXACTLY 1 GPU device only in this version.
    bool isGPUEngineInitialized = false; //TODO: To be implemented.
    DXGI_FORMAT rttFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    /* Tier 1 (buffers) is what the per-tab instance arena needs - a reserved buffer whose 64 KB
    tiles are committed on demand. It is a baseline requirement alongside Heap Tier 2 and belongs in
    the installer's hardware check; the query here is what tells us at startup if that check was
    skipped. Every GPU shipped since ~2015 reports Tier 1 or better. */
    D3D12_TILED_RESOURCES_TIER tiledResourcesTier = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;

    DX12ResourcesUI uiResources;

    // Scene3D sky background pipeline. Device-wide (no per-window or per-tab state): created once by
    // InitSkyGradientResources before the render threads start, then only read while recording.
    ComPtr<ID3D12RootSignature> skyGradientRootSignature;
    ComPtr<ID3D12PipelineState> skyGradientPSO;

    /* GPU draw-command compaction pipeline (graphics.md, 10M plan Step 7 - vertical slice). Compute
    root signature is all root descriptors - no shader-visible descriptor heap - because the per-page
    draw structure is kept in this slice. Created once by InitSceneCullResources before render threads
    start, then only read while recording. cullZeroBuffer is a 4-byte zero source used to reset each
    page's visible-command count via CopyBufferRegion (keeps the reset root-descriptor-only). */
    ComPtr<ID3D12RootSignature> sceneCullRootSignature;
    ComPtr<ID3D12PipelineState> sceneCullPSO;
    ComPtr<ID3D12Resource> cullZeroBuffer;

    /* Shared geometry for every tab: one immutable mesh per (shape, LOD). Built by
    InitD3DDeviceOnly before any thread starts and never written again, so render threads read its
    address and table lock-free forever. Tab 0 owns it conceptually - it lives here rather than in a
    tab so the draw loop never has to walk two tabs' snapshots to find it. */
    PrimitiveLibrary primitiveLibrary;

    //Following to be added latter.
    //ID3D12DescriptorHeapMgr    ← Global descriptor allocator
    //Shader& PSO Cache         ← Shared by all threads
    //AdapterInfo                ← For device selection / VRAM stats

    /* We will have 1 Render Queue per monitor, which is local to Render Thread.
    IMPORTANT: All GPU have only 1 physical hardware engine, and can execute 1 command at a time only.
    Even if 4 commands list are submitted to 4 independent queue, graphics driver / WDDM serializes them.
    Still we need to have 4 separate queue to properly handle different refresh rate.

    Ex: If we put all 4 window on same queue: Window A (60Hz) submits a Present command. The Queue STALLS
    waiting for Monitor A's VSync interval. Window B (144Hz) submits draw comand. 
    Window B cannot be processed because the Queue is blocked by Windows A's VSync wait. 
    By using 4 Queues, Queue A can sit blocked waiting for VSync, 
    while Queue B immediately push work work to the GPU for the faster monitor.*/

    std::atomic<uint64_t> renderFenceValue = 0; // Global. This is in addition to per monitor render fence value.

	ComPtr<ID3D12CommandQueue> copyCommandQueue; // There is only 1 across the application.
    // The one staging buffer every copy-thread upload passes through. Created by InitD3DDeviceOnly
    // (before any thread starts) and touched only by the copy thread thereafter.
    GpuUploadRing uploadRing;
    ComPtr<ID3D12Fence> copyFence;// Synchronization for Copy Queue
	std::atomic<uint64_t> copyFenceValue = 1; // thread safe.
    //Start from 1 to avoid confusion with default fence value of 0.
    HANDLE copyFenceEvent = nullptr;

public:
    // Maps our CPU ObjectID to its resource info in VRAM
    std::unordered_map<uint64_t, GpuResourceVertexIndexInfo> resourceMap;

    // Simulates a simple heap allocator with 16MB chunks
    uint64_t m_nextFreeOffset = 0;
    const uint64_t CHUNK_SIZE = 16 * 1024 * 1024;
    uint64_t m_vram_capacity = 4 * CHUNK_SIZE; // Simulate 64MB VRAM

    // When an object is updated, the old VRAM is put here to be freed later.
    struct DeferredFree {
        uint64_t frameNumber; // The frame it became obsolete
        GpuResourceVertexIndexInfo resource;
    };
    std::list<DeferredFree> deferredFreeQueue;

	// Allocate space in VRAM. Returns the handle. What is this used for?
    // std::optional<GpuResourceVertexIndexInfo> Allocate(size_t size);

	// Descriptor sizes for RTV and CBV/SRV/UAV. We need these to calculate offsets in descriptor heaps.
	// These are initialized during device creation and remain constant. i.e. They are hardware properties of GPU.
    // We store them here for easy access across threads.
	UINT rtvDescriptorSize = 0, cbvSrvUavDescriptorSize = 0; //Initialized during device creation.

    void ProcessDeferredFrees(uint64_t lastCompletedRenderFrame);

	//शंकर() {}; // Our Main function initializes DirectX12 global resources by calling InitD3DDeviceOnly().
    void InitD3DDeviceOnly();
    void InitD3DPerTab(DX12ResourcesPerTab& tabRes); // Call this when a new Tab is created
    void InitD3DPerWindow(DX12ResourcesPerWindow& dx, HWND hwnd, ID3D12CommandQueue* commandQueue);
    // monitorId: index into gpu.screens[] for DPI/physical info used by UI layout calculations
    // containers: the SubTab's container set - only their pages are visited (Step 6).
    // subTabBit: which of the 64 VisibilityMask bits this view tests, i.e. the sub-tab slot being
    // drawn, or kNoSubTabBit when the draw has no sub-tab at all (the print path). Every open
    // sub-tab has a bit, since MV_MAX_SUBTABS equals the 64-bit mask width.
    // cullScratch: this render thread's compaction scratch, used only when gUseComputeCull is on
    // (10M plan Step 7 slice). Its state fields are reset to COMMON by the caller each frame.
    void RenderScene3D(ID3D12GraphicsCommandList* cmdList, //Called by per monitor render thread.
        DX12ResourcesPerWindow& winRes, const DX12ResourcesPerTab& tabRes, TabGeometryStorage& storage,
        const CameraState& camera, int monitorId, const SubTabContainerSet& containers,
        uint32_t subTabBit, SceneCullScratch& cullScratch);
    void WaitForPreviousFrame(const DX12ResourcesPerRenderThread& dx);
    void ResizeD3DWindow(DX12ResourcesPerWindow& dx, UINT newWidth, UINT newHeight);

    // Called when a monitor is unplugged or window is destroyed. Destroys SwapChain/RTVs but KEEPS Geometry.
    void CleanupWindowResources(DX12ResourcesPerWindow& winRes);
    // Called when a TAB is closed by the user. Destroys the Jumbo Vertex/Index Buffers.
    void CleanupTabResources(DX12ResourcesPerTab& tabRes);
    // Called ONLY at application exit (wWinMain end).Destroys the Device, Factory, and Global Copy Queue.
	// Thread resources are cleaned up by the Render Thread itself before exit.
    void CleanupD3DGlobal();
};

void FetchAllMonitorDetails();
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData);

/*
IID_PPV_ARGS is a MACRO used in DirectX (and COM programming in general) to help safely and correctly
retrieve interface pointers during object creation or querying. It helps reduce repetitive typing of codes.
COM interfaces are identified by unique GUIDs. Than GUID pointer is converted to appropriate pointer type.

Ex: IID_PPV_ARGS(&device) expands to following:
IID iid = __uuidof(ID3D12Device);
void** ppv = reinterpret_cast<void**>(&device);
*/

// Structure to hold transformation matrices
struct ConstantBuffer {
    DirectX::XMFLOAT4X4 viewProj;   // 64 bytes
};

// Externs for communication 
extern std::atomic<bool> shutdownSignal;

// Logic Thread "Fence"
extern std::mutex g_logicFenceMutex;
extern std::condition_variable g_logicFenceCV;
extern uint64_t g_logicFrameCount;

// Copy Thread "Fence"
extern std::mutex g_copyFenceMutex;
extern std::condition_variable g_copyFenceCV;
extern uint64_t g_copyFrameCount;

//TODO: Implement this. In a real allocator, we would manage free lists and possibly defragment memory.
/*
std::optional<GpuResourceVertexIndexInfo> शंकर::Allocate(size_t size) {

    if (nextFreeOffset + size > m_vram_capacity) {
        std::cerr << "VRAM MANAGER: Out of memory!" << std::endl;
        // Here, the Main Logic thread would be signaled to reduce LOD.
        return std::nullopt;
    }
    GpuResourceVertexIndexInfo info{ nextFreeOffset, size };
    nextFreeOffset += size; // Simple bump allocator
    return info;
}*/

// Utility Functions

// Waits for the previous frame to complete rendering.
inline void WaitForGpu(DX12ResourcesPerWindow dx)
{   //Where are we using this function?
    /*
    dx.commandQueue->Signal(dx.fence.Get(), dx.fenceValue);
    dx.fence->SetEventOnCompletion(dx.fenceValue, dx.fenceEvent);
    WaitForSingleObjectEx(dx.fenceEvent, INFINITE, FALSE);
    dx.fenceValue++;*/
}

// Waits for a specific fence value to be reached
inline void WaitForFenceValue(DX12ResourcesPerWindow dx, UINT64 fenceValue)
{ // Where are we using this?
    /*
    if (dx.fence->GetCompletedValue() < fenceValue)
    {
        ThrowIfFailed(dx.fence->SetEventOnCompletion(fenceValue, dx.fenceEvent));
        WaitForSingleObjectEx(dx.fenceEvent, INFINITE, FALSE);
    }*/
}

// Thread Functions
// toCopyThreadMutex / toCopyThreadCV / commandToCopyThreadQueue moved to RenderScene3D.h (portable).

// Copy-thread-only. Frees every retired snapshot / page / matrix buffer (and Cad2D resource) whose
// retire fence all live monitors have passed, and returns fence-cleared matrix slots to the free
// list. Returns the largest remaining per-tab retire backlog, or SIZE_MAX when no monitor fence
// could be read (nothing is safe to free; *outSafeRetireFence is then left untouched).
// Called once per copy-thread iteration AND once per published chunk - see the definition.
size_t PruneRetiredGpuResources(uint64_t* outSafeRetireFence = nullptr);

// Thread Functions - Just Declaration!
void GpuCopyThread();
void GpuRenderThread(int monitorId, int refreshRate);
