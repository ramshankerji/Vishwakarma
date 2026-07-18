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

struct IndirectCommand { // OPTIMIZED Indirect Command
    uint32_t matrixIndex; // 4 Bytes (Root Constant b1)
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
    uint32_t matrixIndex;        // Index into the per-tab WorldMatrix structured buffer

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
