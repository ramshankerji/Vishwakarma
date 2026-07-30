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

#include "डेटा.h" // GeometryData: the vertex/index payload carried by CommandToCopyThread.

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

// Commands sent from Generator thread(s) to the Copy thread
enum class CommandToCopyThreadType { NONE = 0, ADD, MODIFY, REMOVE };
struct CommandToCopyThread
{
    CommandToCopyThreadType type;
    std::optional<GeometryData> geometry; // Present for ADD and MODIFY
    uint64_t id = 0; // Always present
    uint64_t tabID = 0; // NEW: We must know which tab this object belongs to!
    uint64_t containerMemoryId = 0; // Parent high-level container; pages never mix container IDs.
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

// Approximate GPU staging cost of one command. Used in two places (graphics.md, 10M plan Step 0):
// to cap the copy thread's CPU-side drain, and to size the chunks that must fit in the upload ring.
// REMOVE carries no payload and therefore no staging cost. ADD / MODIFY also stage the 64-byte
// instance record plus its 4-byte redirect entry, because both live in device-local memory since
// Step 2 and can no longer be written by a plain CPU store into a mapped upload heap. A
// transform-only MODIFY carries no vertices or indices, so those two terms fall to zero.
inline uint64_t EstimateStagingBytes(const CommandToCopyThread& command) {
    if (!command.geometry.has_value()) return 0;
    const GeometryData& geometry = *command.geometry;
    return geometry.vertices.size() * sizeof(Vertex) + geometry.indices.size() * sizeof(uint16_t)
        + kInstanceRecordBytes + kInstanceSlotBytes;
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
