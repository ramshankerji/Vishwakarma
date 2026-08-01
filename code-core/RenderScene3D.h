// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

// Platform-agnostic Scene3D data layouts: camera, geometry-page metadata, the copy-thread
// command/queue types and the indirect-draw ABI. Every graphics backend (DirectX12 today,
// Vulkan / Metal later) consumes these same definitions; the static_asserts are the
// cross-platform ABI contract.

#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <DirectXMath.h>

#include "ConstantsApplication.h" // MV_MAX_CONTAINERS_PER_SUBTAB
#include "डेटा.h" // GeometryData: the vertex/index payload carried by CommandToCopyThread.

/* The set of containers one SubTab draws (graphics.md, 10M plan Step 6, item 3). A SubTab holds a
SET of containers of a single type - never a mix, because a mixed SubTab would have ambiguous
renderer and interaction semantics.

Inline storage, not a vector: render threads read this every frame without a lock, so a heap buffer
the engineering thread could reallocate underneath them is precisely the hazard to avoid. Same
benign-staleness contract as the per-view camera next to it.

The set is a COMPACT RULE, deliberately: "this SubTab shows container X" is expressed by one entry
here, never by setting a per-object VisibilityMask bit for every member. The mask is for per-object
hide; conflating the two would make opening a SubTab an O(objects) write. */
struct SubTabContainerSet {
    uint64_t ids[MV_MAX_CONTAINERS_PER_SUBTAB] = {};
    uint8_t count = 0;

    bool Empty() const { return count == 0; }
    bool Contains(uint64_t containerMemoryId) const {
        for (uint8_t i = 0; i < count; ++i) {
            if (ids[i] == containerMemoryId) return true;
        }
        return false;
    }
    // Silently ignores 0, duplicates, and overflow past the fixed capacity.
    void Add(uint64_t containerMemoryId) {
        if (containerMemoryId == 0 || Contains(containerMemoryId)) return;
        if (count < MV_MAX_CONTAINERS_PER_SUBTAB) ids[count++] = containerMemoryId;
    }
    // Remove one id, compacting the inline array; silently ignores an absent id. Same benign-
    // staleness contract as Add: a render thread copying the set mid-edit sees old-or-new, and the
    // worst case is one frame of a container drawn twice or dropped a frame early - both visually
    // identical, since a container's pages rasterise to the same pixels regardless of set position.
    void Remove(uint64_t containerMemoryId) {
        for (uint8_t i = 0; i < count; ++i) {
            if (ids[i] != containerMemoryId) continue;
            for (uint8_t j = static_cast<uint8_t>(i + 1); j < count; ++j) ids[j - 1] = ids[j];
            --count;
            return;
        }
    }
    void Clear() { count = 0; }
};

struct CameraState { // Each view gets its own camera state. 
    //This is part of the "View" data structure, not the "Tab" data structure. Each tab can have multiple views.
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 target;
    DirectX::XMFLOAT3 up;
    float fov;
    float aspect;
    float nearZ;
    float farZ;

    CameraState() { Initialize(); }
    void Initialize() {
        position = { 0.0f, -10.0f, 2.0f };
        target = { 0.0f, 0.0f,  0.0f };
        up = { 0.0f, 0.0f,  1.0f }; // Z-Up is perfect for an XY orbit.

        fov = DirectX::XMConvertToRadians(60.0f);
        aspect = 1.0f; // SAFE DEFAULT
        nearZ = 0.1f;
        farZ = 1000.0f;
    }
};

inline void UpdateCameraOrbit(CameraState& cam)
{
    // Calculate the 2D radius from the target on the XY plane. We ignore Z here to prevent the "spiral away" bug.
    float dx = cam.position.x - cam.target.x;
    float dy = cam.position.y - cam.target.y;
    float radius = hypotf(dx, dy);
    if (radius < 0.001f) radius = 10.0f;// Safety check to prevent radius becoming 0 (which locks the camera)

    // Stateless: advance from the camera's own azimuth, so every view camera orbits independently.
    float rotationAngle = atan2f(dy, dx) + 0.002f; // per-frame speed

    float x = cam.target.x + cosf(rotationAngle) * radius; // Orbit in XY plane
    float y = cam.target.y + sinf(rotationAngle) * radius;
    float z = cam.position.z;// Z remains static (height)
    cam.position = { x, y, z };
}

/* One record in a tab's instance arena, addressed by instanceSlot (NOT by gpuInstanceIndex - the
redirect table sits in between; see graphics.md, 10M plan Step 4). 64 bytes divides the 64 KB D3D12
buffer tile exactly, which is what makes the arena's commit-on-demand growth land on whole records.

TRANSFORM CONVENTION - get this wrong and everything silently draws in the wrong place. The engine
is row-vector throughout: the vertex shader computes `pos * W`, and DirectXMath's XMFLOAT4X4/XMMATRIX
put the translation in row 3. The three float4s here are the first three ROWS OF transpose(W), i.e.
the first three COLUMNS of W:

    transformA = (W00, W10, W20, tx)
    transformB = (W01, W11, W21, ty)
    transformC = (W02, W12, W22, tz)

The dropped fourth row of transpose(W) is always (0,0,0,1), which is exactly why 48 bytes suffice -
and exactly why a shader may NOT hand this to a generic float4x4 `mul` any more. Point transform is
three dots against float4(pos,1); normal transform is three dots against the .xyz parts (uniform
scale assumed, inverse-transpose left to a later revision).

Byte-for-byte this is the leading 48 bytes of the XMFLOAT4X4 the arena stored before Step 4, so the
CPU writer is still one XMMatrixTranspose - it just stops copying the last row. */
struct InstanceRecord {
    float transformA[4];
    float transformB[4];
    float transformC[4];
    // 16-byte render payload. Authored, infrequently changed state only. High-frequency
    // interaction state (hover, selection, SubTab membership, temporary hide) deliberately stays
    // OUT of here and lives in Step 5's visibility mask, so an interaction never allocates a slot.
    // Nothing produces or consumes these four yet; the bytes exist because the record is 64 bytes.
    uint32_t materialIndex;
    uint32_t packedColor;
    uint32_t renderFlags;
    uint32_t packedParams;
};
constexpr uint32_t kInstanceRecordBytes = 64;
static_assert(sizeof(InstanceRecord) == kInstanceRecordBytes,
    "InstanceRecord must be exactly 64 bytes - the arena's tile math depends on it.");

// One entry of InstanceSlotOf[gpuInstanceIndex]: the arena slot currently holding the object's
// record. This is the 4-byte value a transform edit atomically flips (10M plan Step 4).
constexpr uint32_t kInstanceSlotBytes = 4;

/* One entry of VisibilityMask[gpuInstanceIndex]: a 64-bit SubTab MEMBERSHIP word, read as uint2 in
shaders for broad compatibility (10M plan Step 5). Bit N set = this object is visible in the SubTab
occupying slot N. Toggling is one aligned write under invariant 2 - no clone, no argument rebuild,
no upload, no arena slot. That is the whole reason high-frequency interaction state (hover,
selection, temporary hide) lives here instead of inside the 64-byte InstanceRecord. */
constexpr uint32_t kVisibilityMaskBytes = 8;

/* Default for every newly added object: visible in every SubTab. This MUST be written explicitly on
each ADD - a freshly committed D3D12 tile has UNDEFINED contents, so a mask can never be inherited
from the tile the way a zero-initialised allocation could be. Note also that per-object membership
is deliberately NOT how "this whole container belongs to that SubTab" is expressed: the container
test on the geometry page stays the cheap first-level reject (10M plan Step 6, item 3). */
constexpr uint64_t kVisibleInAllSubTabs = ~0ull;

/* Per-draw SubTab bit for a Viewport that has no mask-addressable bit - a sub-tab in slot >= 64
(MV_MAX_SUBTABS is 128, the mask is 64 bits), or a draw with no sub-tab at all such as the print
path. The vertex shader skips the membership test entirely for this value, so those views show
everything rather than nothing. */
constexpr uint32_t kNoSubTabBit = 0xFFFFFFFFu;

/* Sub-tab slot -> VisibilityMask bit. MV_MAX_SUBTABS is 128 while the mask holds 64 SubTabs, which
is the design limit rather than an oversight: if more than 64 SIMULTANEOUSLY MASKED SubTabs are ever
needed the remedy is a second mask word, not a change here. Slots past 63 therefore show everything
instead of failing closed. */
inline uint32_t SubTabVisibilityBit(int subTabSlot) {
    return subTabSlot >= 0 && subTabSlot < 64 ? static_cast<uint32_t>(subTabSlot) : kNoSubTabBit;
}

struct IndirectCommand { // OPTIMIZED Indirect Command
    // Dense renderer identity of the object this command draws (Root Constant b1). STABLE for the
    // object's whole GPU lifetime: unchanged by MODIFY and by an RCU clone relocating the object to
    // another GeometryPage. It indexes the tab's instance arena AND is the GPU pick id (+1), so a
    // pick resolves in O(1) through the copy thread's registry (graphics.md, 10M plan Step 3).
    uint32_t gpuInstanceIndex; // 4 Bytes
	// Since Jumbo buffer ( or pages in future ) remains same, we bind it once.
    // REMOVED: D3D12_VERTEX_BUFFER_VIEW vbv (Saved 16 Bytes)
    // REMOVED: D3D12_INDEX_BUFFER_VIEW  ibv (Saved 16 Bytes)
    // Same 20-byte layout as D3D12_DRAW_INDEXED_ARGUMENTS and Vulkan's VkDrawIndexedIndirectCommand,
    // spelled portably so this header stays graphics-API free (checked by static_assert in the
    // platform header).
    struct DrawIndexedArguments {
        uint32_t IndexCountPerInstance;
        uint32_t InstanceCount;
        uint32_t StartIndexLocation;
        int32_t  BaseVertexLocation;
        uint32_t StartInstanceLocation;
    } drawArguments;// 20 Bytes
}; // Total size: 24 Bytes (down from 56 Bytes!)
static_assert(sizeof(IndirectCommand) == 24, "IndirectCommand must be exactly 24 bytes.");

/* Page Metadata: GeometryPlacementRecordInPage (CPU-side only).
One entry per geometry object inside a GeometryPage. Used by Copy Thread for defragmentation,
rebuilds, and future features. (frustum culling, ray-cast selection, LOD, etc.).
Total size = 56 bytes (tightly packed, cache-friendly). */
struct GeometryPlacementRecordInPage {
    uint64_t objectID;           // Unique 64-bit ID across entire process (unchanged)

    // Byte offsets into this page's vertex/index buffers (page max = 4 MB → uint32_t is safe)
    // Vertex region (grows upward)
    uint32_t vertexByteOffset; // Start of this object's vertices in the page (bytes)
    uint32_t vertexSize;       // In bytes

    // Index region (grows downward)
    uint32_t indexByteOffset;    // Start of this object's indices in the page (bytes)
    uint32_t indexSize;          // In bytes

    uint32_t indexCount;         // Number of indices (not bytes) For ExecuteIndirect
    uint32_t gpuInstanceIndex;   // Stable renderer identity; indexes the per-tab instance arena

    // Axis-Aligned Bounding Box (AABB) – stored as float32 only (24 bytes total)
    // Always present for future use (frustum culling, selection, etc.).
    // Set to {0,0,0} / {0,0,0} if we don't need it yet – costs nothing extra.
    float minX, minY, minZ, maxX, maxY, maxZ; // Minimum corner (X,Y,Z) Maximum corner (X,Y,Z)

    // Optional padding for perfect 8-byte alignment (not needed – compiler will pad anyway)
	bool isDeleted = false; // Marked for deletion (soft delete, for defragmentation)
};

static_assert(sizeof(GeometryPlacementRecordInPage) == 64,
    "GeometryPlacementRecordInPage must be exactly 64 bytes for optimal cache/line usage.");

/* Commands sent from Generator thread(s) to the Copy thread.

SET_VISIBILITY / CLEAR_SUBTAB_HIDES carry no geometry and touch no geometry page: they are pure
VisibilityMask writes (10M plan Step 5), which is exactly why hide/show costs nothing proportional
to the scene. Note that they must be kept OUT of the per-object deduplication pass in
ProcessScene3DCopyBatch - that pass keys on `id` alone, so an ADD and a hide of the same object in
one batch would collapse to whichever came last and the geometry would silently never be uploaded. */
enum class CommandToCopyThreadType { NONE = 0, ADD, MODIFY, REMOVE, SET_VISIBILITY,
    CLEAR_SUBTAB_HIDES };
struct CommandToCopyThread
{
    CommandToCopyThreadType type;
    std::optional<GeometryData> geometry; // Present for ADD and MODIFY
    uint64_t id = 0; // Always present
    uint64_t tabID = 0; // NEW: We must know which tab this object belongs to!
    uint64_t containerMemoryId = 0; // Parent high-level container; pages never mix container IDs.
    /* Which SubTab bit a mask command addresses, as a pre-shifted word (1ull << subTabSlot).
       SET_VISIBILITY    : change this bit on the object named by `id`.
       CLEAR_SUBTAB_HIDES: force this bit back ON for every object the tab currently hides (`id` is
       unused), so a retired sub-tab slot does not hand its hides to whatever reuses the slot.
       The producer sends a BIT, not a whole word, because only the copy thread knows an object's
       current membership - keeping that state in one place is what stops the two threads from
       having to agree on a shared shadow. */
    uint64_t visibilityBits = 0;
    bool visibilityVisible = true; // SET_VISIBILITY only: set the bit (show) or clear it (hide).
};

/* A MODIFY carrying a world matrix but NO vertices or indices is a TRANSFORM-ONLY edit: the copy
thread writes a fresh instance record and flips one redirect entry, cloning no geometry page and
publishing no snapshot (10M plan Step 4). This is the encoding rather than a new command type
because GeometryData already carries the matrix, and an empty payload is otherwise a no-op.

ADD is never transform-only - a new object needs geometry. */
inline bool IsTransformOnlyEdit(const CommandToCopyThread& command) {
    return command.type == CommandToCopyThreadType::MODIFY && command.geometry.has_value() &&
        command.geometry->vertices.empty() && command.geometry->indices.empty();
}

// A pure VisibilityMask write: touches no geometry page and must be kept out of the per-object
// deduplication pass (see CommandToCopyThreadType above).
inline bool IsVisibilityCommand(const CommandToCopyThread& command) {
    return command.type == CommandToCopyThreadType::SET_VISIBILITY ||
        command.type == CommandToCopyThreadType::CLEAR_SUBTAB_HIDES;
}

// Approximate GPU staging cost of one command. Used in two places (graphics.md, 10M plan Step 0):
// to cap the copy thread's CPU-side drain, and to size the chunks that must fit in the upload ring.
// REMOVE carries no payload and therefore no staging cost. ADD / MODIFY also stage the 64-byte
// instance record plus its 4-byte redirect entry, because both live in device-local memory since
// Step 2 and can no longer be written by a plain CPU store into a mapped upload heap. A
// transform-only MODIFY carries no vertices or indices, so those two terms fall to zero.
inline uint64_t EstimateStagingBytes(const CommandToCopyThread& command) {
    // A mask write is one 8-byte staging region and nothing else - no record, no redirect, no
    // geometry (10M plan Step 5). CLEAR_SUBTAB_HIDES fans out over the tab's hidden objects, whose
    // count only the copy thread knows; it is charged one entry here and re-checked against the
    // ring as it writes, the same way an oversize geometry payload is.
    if (command.type == CommandToCopyThreadType::SET_VISIBILITY ||
        command.type == CommandToCopyThreadType::CLEAR_SUBTAB_HIDES) {
        return kVisibilityMaskBytes;
    }
    if (!command.geometry.has_value()) return 0;
    const GeometryData& geometry = *command.geometry;
    return geometry.vertices.size() * sizeof(Vertex) + geometry.indices.size() * sizeof(uint16_t)
        + kInstanceRecordBytes + kInstanceSlotBytes + kVisibilityMaskBytes;
}

class ThreadSafeQueueGPU {
public:
    void push(CommandToCopyThread value) {
        std::lock_guard<std::mutex> lock(mutex);
        fifoQueue.push(std::move(value));
        cond.notify_one();
    }

    // Non-blocking pop
    bool try_pop(CommandToCopyThread& value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (fifoQueue.empty()) { return false; }
        value = std::move(fifoQueue.front());
        fifoQueue.pop();
        return true;
    }

    // Shuts down the queue, waking up any waiting threads
    void shutdownQueue() {
        std::lock_guard<std::mutex> lock(mutex);
        shutdown = true;
        cond.notify_all();
    }

private:
    std::queue<CommandToCopyThread> fifoQueue; // fifo = First-In First-Out
    std::mutex mutex;
    std::condition_variable cond;
    bool shutdown = false;
};

inline ThreadSafeQueueGPU g_gpuCommandQueue;

// Thread synchronization between Main Logic thread and Copy thread
inline std::mutex toCopyThreadMutex;
inline std::condition_variable toCopyThreadCV;
inline std::queue<CommandToCopyThread> commandToCopyThreadQueue;

// Number of closed tabs whose GPU teardown is pending on the copy thread (fence-gated).
// UI thread increments (CleanupReleasedTabs) and notifies the CV; GpuCopyThread tags each
// request with the global render fence and decrements after performing the release.
inline std::atomic<uint32_t> gPendingTabGpuReleases{ 0 };
