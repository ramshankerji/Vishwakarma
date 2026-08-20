// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

// Platform-agnostic Scene3D data layouts: camera, geometry-page metadata, the copy-thread
// command/queue types and the indirect-draw ABI. Every graphics backend (DirectX12 today,
// Vulkan / Metal later) consumes these same definitions; the static_asserts are the
// cross-platform ABI contract.

#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef> // offsetof, used by the VisibleIndirectCommand alignment asserts.
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

/* Per-draw SubTab bit for a Viewport that has no mask-addressable bit. Since MV_MAX_SUBTABS became
64 - equal to the mask width - no OPEN sub-tab can produce this any more; it is now reached only by a
draw with no sub-tab at all, such as the print path. The vertex and cull shaders skip the membership
test entirely for this value, so those views show everything rather than nothing. */
constexpr uint32_t kNoSubTabBit = 0xFFFFFFFFu;

/* Sub-tab slot -> VisibilityMask bit. MV_MAX_SUBTABS and the mask are both 64, so this is the
identity for every valid slot and the sentinel branch below is defensive only - it used to be
reachable when the slot array was 128 wide. Widening past 64 means adding a second mask word, not
just enlarging the array; a slot with no bit fails OPEN (shows everything) rather than closed. */
inline uint32_t SubTabVisibilityBit(int subTabSlot) {
    static_assert(MV_MAX_SUBTABS <= 64,
        "A sub-tab slot must have a VisibilityMask bit; widening past 64 needs a second mask word.");
    return subTabSlot >= 0 && subTabSlot < 64 ? static_cast<uint32_t>(subTabSlot) : kNoSubTabBit;
}

/* THE GLOBAL PRIMITIVE LIBRARY (website/software/graphics.md, "Shared geometry and the primitive
libraries"). One immutable mesh per (shape, LOD), built in code at startup, alive for the process and
shared by every tab. An eligible object emits a REFERENCE plus a transform instead of vertices, so a
scene of 100,000 spheres stores one sphere.

WHY IT IS NOT A GeometryPage. A compacted draw command carries the RAW GPU VIRTUAL ADDRESS of its
vertex buffer, and every GeometryPage is RCU-managed: any modify to any object in it clones the page
to a new buffer at a new address and retires the old one. A cross-tab reference into such a page
would hold an address the copy thread is free to invalidate. Tab 0 being un-closable does not help -
the CLONE is what breaks it, not tab lifetime. So the library is one plain committed resource with a
fixed address, deliberately outside the page system: no snapshot, no retirement, no fence gating.

The shape IDS live in डेटा.h beside GeometryData::libraryShapeId - that is the generators' half of
the contract ("which shape am I") and they never include this header. Everything below is the
renderer's half ("where are that shape's bytes"). */
// Always 8, even where fewer are meaningful - uniform array width keeps the Step 2 shader
// branch-free, and a duplicate table entry costs 24 bytes.
constexpr uint32_t kPrimitiveLodCount = 8;
/* Step 1 selects ONE level on the CPU; Step 2 replaces this with per-frame selection in the compute
pass. LOD 5 is 36x18 - byte-identical to the mesh SPHERE::GetGeometry emitted before the library
existed, which is what makes Step 1 verifiable as an unchanged image. */
constexpr uint32_t kPrimitiveFixedLod = 5;

// Where one (shape, LOD) mesh sits inside the single library buffer, plus its canonical-space
// bounds. The AABB is NOT decoration: an instanced object uploads no vertices, so this is the only
// source for the registry's world-centre shadow and for zoom-to-fit (graphics.md records both as
// silent failures otherwise).
struct PrimitiveLibraryEntry {
    uint32_t indexCount = 0;
    uint32_t startIndexLocation = 0; // Indices from the library's index-region base.
    int32_t  baseVertexLocation = 0; // Vertices from the library's vertex-region base.
    float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
    float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
};

/* The GPU's half of a library entry: just enough to fill in a draw command, 12 bytes. The AABB
above stays CPU-only - it feeds the registry's world-centre shadow and zoom-to-fit, neither of which
the shader can reach. Mirrored as `PrimitiveLod` in ShaderSceneCull.hlsl; keep the two in step. */
struct PrimitiveLibraryDrawRange {
    uint32_t indexCount = 0;
    uint32_t startIndexLocation = 0;
    int32_t  baseVertexLocation = 0;
};
static_assert(sizeof(PrimitiveLibraryDrawRange) == 12,
    "PrimitiveLibraryDrawRange is a StructuredBuffer element stride the cull shader indexes.");

struct PrimitiveLibraryTable {
    PrimitiveLibraryEntry entries[kPrimitiveShapeCount * kPrimitiveLodCount];

    const PrimitiveLibraryEntry& At(int16_t shapeId, uint32_t lod) const {
        return entries[static_cast<uint32_t>(shapeId) * kPrimitiveLodCount + lod];
    }
    PrimitiveLibraryEntry& At(int16_t shapeId, uint32_t lod) {
        return entries[static_cast<uint32_t>(shapeId) * kPrimitiveLodCount + lod];
    }

    // Flatten to the 12-byte form the cull shader indexes, in the same (shapeId * 8 + lod) order.
    void ToDrawRanges(PrimitiveLibraryDrawRange* out) const {
        for (uint32_t i = 0; i < kPrimitiveShapeCount * kPrimitiveLodCount; ++i) {
            out[i].indexCount = entries[i].indexCount;
            out[i].startIndexLocation = entries[i].startIndexLocation;
            out[i].baseVertexLocation = entries[i].baseVertexLocation;
        }
    }
};

/* How a draw TEMPLATE says which library shape it draws: `StartInstanceLocation` carries
`libraryShapeId + 1`, so 0 means "bespoke, use my own offsets". The +1 is what keeps shape 0 (the
sphere) distinguishable from an ordinary template, which always writes 0 there.

That field is otherwise dead weight - we draw one instance starting at 0 - but it IS a real
D3D12 draw argument, and the legacy, pick and print paths execute these templates DIRECTLY. A
non-zero StartInstanceLocation offsets SV_InstanceID and per-instance vertex fetch, neither of which
any shader here uses, so those paths are unaffected today. **Adding a per-instance vertex stream or
reading SV_InstanceID would break that silently**, and the compacted output command therefore always
writes 0 rather than passing this value through. */
constexpr uint32_t kBespokeTemplateMarker = 0;
inline uint32_t TemplateShapeMarker(int16_t libraryShapeId) {
    return libraryShapeId < 0 ? kBespokeTemplateMarker : static_cast<uint32_t>(libraryShapeId) + 1;
}

/* Sphere tessellation per LOD, coarsest first: {slices, stacks}, giving slices*stacks*2 triangles.
LOD 0 is 16 triangles for a sub-pixel sphere; LOD 7 is 4096 for one covering 128 px or more. Every
slice count is a multiple of 4, so each level's vertices reach exactly +/-1 on all three axes.

The whole ladder is ~123 KB (4588 vertices, 26256 indices) ONCE for the process. Largest level is
2112 vertices, comfortably inside the 16-bit index limit, which is what lets every entry keep
object-relative indices resolved through BaseVertexLocation. */
constexpr uint32_t kSphereLodSlices[kPrimitiveLodCount] = { 4, 8, 12, 16, 24, 36, 48, 64 };
constexpr uint32_t kSphereLodStacks[kPrimitiveLodCount] = { 2, 4,  6,  8, 12, 18, 24, 32 };

/* Append one canonical UNIT sphere at the origin. Same ring/quad topology SPHERE::GetGeometry used
before the library took the mesh over - full latitude rings including both poles, quads between
stacks - so LOD 5 reproduces it exactly. On a unit sphere at the origin the outward normal IS the
position, already unit length. */
inline void AppendUnitSphereMesh(uint32_t sliceCount, uint32_t stackCount,
    std::vector<Vertex>& vertices, std::vector<uint16_t>& indices) {
    for (uint32_t i = 0; i <= stackCount; ++i) {
        const float phi = 3.14159265358979323846f * static_cast<float>(i) / static_cast<float>(stackCount);
        const float sinPhi = sinf(phi);
        const float cosPhi = cosf(phi);
        for (uint32_t j = 0; j < sliceCount; ++j) {
            const float theta = 6.28318530717958647692f * static_cast<float>(j) / static_cast<float>(sliceCount);
            const XMFLOAT3 position = { sinPhi * cosf(theta), cosPhi, sinPhi * sinf(theta) };
            vertices.push_back(Vertex{ position, PackNormal(position) });
        }
    }
    for (uint32_t i = 0; i < stackCount; ++i) {
        for (uint32_t j = 0; j < sliceCount; ++j) {
            const uint32_t nextJ = (j + 1) % sliceCount;
            const uint32_t r0 = i * sliceCount;
            const uint32_t r1 = (i + 1) * sliceCount;
            const uint16_t quad[6] = {
                static_cast<uint16_t>(r0 + j),     static_cast<uint16_t>(r1 + j),
                static_cast<uint16_t>(r0 + nextJ), static_cast<uint16_t>(r0 + nextJ),
                static_cast<uint16_t>(r1 + j),     static_cast<uint16_t>(r1 + nextJ),
            };
            indices.insert(indices.end(), quad, quad + 6);
        }
    }
}

/* Build every LOD of every library shape into one vertex array and one index array, filling in the
table. Platform-agnostic: the caller uploads the two arrays into whatever a backend calls a buffer.
Indices are OBJECT-RELATIVE (they start at 0 for each entry) and resolved per draw through
BaseVertexLocation, exactly as an ordinary geometry page's are. */
inline void BuildPrimitiveLibraryMesh(std::vector<Vertex>& vertices, std::vector<uint16_t>& indices,
    PrimitiveLibraryTable& table) {
    vertices.clear();
    indices.clear();
    for (uint32_t lod = 0; lod < kPrimitiveLodCount; ++lod) {
        PrimitiveLibraryEntry& entry = table.At(kPrimitiveShapeSphere, lod);
        entry.baseVertexLocation = static_cast<int32_t>(vertices.size());
        entry.startIndexLocation = static_cast<uint32_t>(indices.size());
        const size_t firstVertex = vertices.size();
        AppendUnitSphereMesh(kSphereLodSlices[lod], kSphereLodStacks[lod], vertices, indices);
        entry.indexCount = static_cast<uint32_t>(indices.size()) - entry.startIndexLocation;

        // Measured rather than assumed to be the unit cube: a coarse level's vertices only reach
        // +/-1 because every slice count here is a multiple of 4, and that is a property of the
        // table above rather than of the topology.
        float minX = FLT_MAX, minY = FLT_MAX, minZ = FLT_MAX;
        float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;
        for (size_t v = firstVertex; v < vertices.size(); ++v) {
            const XMFLOAT3& p = vertices[v].position;
            minX = (std::min)(minX, p.x); maxX = (std::max)(maxX, p.x);
            minY = (std::min)(minY, p.y); maxY = (std::max)(maxY, p.y);
            minZ = (std::min)(minZ, p.z); maxZ = (std::max)(maxZ, p.z);
        }
        entry.minX = minX; entry.minY = minY; entry.minZ = minZ;
        entry.maxX = maxX; entry.maxY = maxY; entry.maxZ = maxZ;
    }
}

/* Which source a GeometryPage's vertices and indices come from. An INSTANCED page owns NO geometry
buffer at all - it holds only placement records and draw templates, ~256 KB against an ordinary
page's ~4.25 MB - and names the global primitive library as its source instead.

The kinds are kept in SEPARATE pages rather than mixed because the cull dispatch passes ONE vertex
and index buffer view per page as root constants. A page mixing both would have to carry views per
COMMAND in the persistent 24-byte template, taking it to 56 bytes - 320 MB at 10M objects - or pay a
second dispatch per page. Keeping them apart costs one branch in the four page walks instead. */
enum class GeometryPageKind : uint8_t {
    Bespoke = 0,        // Owns a geometry buffer; vertices uploaded per object.
    InstancedGlobal = 1 // Draws from the global primitive library.
};

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

/* Output ABI of the GPU draw-command compaction pass - ONE ExecuteIndirect per Viewport
(graphics.md, 10M plan Step 7). The 24-byte IndirectCommand above stays the persistent per-page
TEMPLATE format; this is the transient compacted form the compute shader writes and the single
per-Viewport ExecuteIndirect consumes.

WHY IT CARRIES THE BUFFER VIEWS AGAIN. A template is drawable only against its own page's vertex and
index buffers, which is exactly why the draw loop used to bind them per page and issue one
ExecuteIndirect per page. Putting the two views back INTO the command lets commands from different
pages sit in one buffer, so the whole Viewport collapses to a single ExecuteIndirect with no IA binds
at all. The 32 bytes are paid only for VISIBLE commands in a per-monitor scratch, never per object in
VRAM - the templates stay 24 bytes.

MEMBER ORDER IS LOAD-BEARING, not stylistic. D3D12 packs indirect arguments tightly in the order of
the command signature's argument descs, and the two D3D12_GPU_VIRTUAL_ADDRESS fields must land on
8-byte boundaries. Views first puts them at offsets 0 and 16, and the 56-byte stride is a multiple of
8, so every command in the buffer keeps that alignment. Leading with the 4-byte root constant would
put the first address at offset 4 and every one after it on an odd 4-byte boundary.

Three definitions must stay in lockstep: this struct, the command signature built in InitD3DPerTab,
and the VisibleCommand struct in ShaderSceneCull.hlsl. */
struct VisibleIndirectCommand {
    // D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW (slot 0) == D3D12_VERTEX_BUFFER_VIEW.
    uint64_t vertexBufferLocation;
    uint32_t vertexSizeInBytes;
    uint32_t vertexStrideInBytes;
    // D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW == D3D12_INDEX_BUFFER_VIEW. Note that the
    // index FORMAT rides along per command here, so 16-bit and 32-bit pages could be drawn by one
    // call - the page-kind axis no longer has to keep them apart on this path (Phase 5 item).
    uint64_t indexBufferLocation;
    uint32_t indexSizeInBytes;
    uint32_t indexFormat; // DXGI_FORMAT_R16_UINT today.
    uint32_t gpuInstanceIndex; // Root Constant b1, same meaning as in IndirectCommand.
    IndirectCommand::DrawIndexedArguments drawArguments;
};
static_assert(sizeof(VisibleIndirectCommand) == 56,
    "VisibleIndirectCommand must be exactly 56 bytes - it is the ExecuteIndirect byte stride, and "
    "the shader writes it as 14 tightly packed uints.");
static_assert(offsetof(VisibleIndirectCommand, vertexBufferLocation) % 8 == 0 &&
    offsetof(VisibleIndirectCommand, indexBufferLocation) % 8 == 0,
    "Both GPU virtual addresses must be 8-byte aligned within the command.");

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

    /* Shared geometry: >= 0 means this object draws from the global primitive library, and the four
    byte-offset/size fields above stay at LITERAL ZERO. graphics.md originally proposed overloading
    those fields to carry (shapeId, lod); putting them in the struct's spare bytes instead is what
    keeps `holeBytes += vertexSize + indexSize` and the compaction arithmetic correct BY
    CONSTRUCTION - an instanced object genuinely occupies no bytes - rather than needing a guard at
    every site that touches them. There were 7 spare bytes; this uses 3. */
    int16_t libraryShapeId = -1;
    uint8_t libraryLod = 0;
};

static_assert(sizeof(GeometryPlacementRecordInPage) == 64,
    "GeometryPlacementRecordInPage must be exactly 64 bytes for optimal cache/line usage.");

/* Where one object's indices and vertices live, in the units DrawIndexedInstanced wants: index
elements from the bound index view's base, and vertices from the bound vertex view's base.

This exists to stop a FOURTH copy of the same arithmetic appearing. graphics.md records that
`vertexByteOffset / sizeof(Vertex)` was already computed independently in three places -
RebuildIndirectBuffer, the Selection3D highlight path and the compaction relocation - and that one
of them being missed is a silently misplaced draw rather than a crash. Shared geometry adds a second
case to every one of them, so the resolution is centralised here instead.

BESPOKE: divide the byte offsets by the strides, exactly as before. INSTANCED: the record's byte
offsets are literal zero and the real location comes from the library table entry the record names.
The caller must have bound the matching views - page buffer for one, primitive library for the
other; BindPageBuffers and PageVertexView / PageIndexView are what guarantee that. */
inline void ResolveObjectDrawRange(const GeometryPlacementRecordInPage& record,
    const PrimitiveLibraryTable& library, uint32_t& indexCount, uint32_t& startIndexLocation,
    int32_t& baseVertexLocation) {
    if (record.libraryShapeId >= 0) {
        const PrimitiveLibraryEntry& entry = library.At(record.libraryShapeId, record.libraryLod);
        indexCount = entry.indexCount;
        startIndexLocation = entry.startIndexLocation;
        baseVertexLocation = entry.baseVertexLocation;
        return;
    }
    indexCount = record.indexCount;
    startIndexLocation = record.indexByteOffset / static_cast<uint32_t>(sizeof(uint16_t));
    baseVertexLocation = static_cast<int32_t>(record.vertexByteOffset / sizeof(Vertex));
}

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
        // An INSTANCED edit is also empty-payload, and the two must not be confused: a move is
        // correctly a redirect flip, while an instanced edit has to rewrite its page's draw
        // template or the shape silently keeps its old library entry. See GeometryData.
        command.geometry->libraryShapeId < 0 &&
        command.geometry->vertices.empty() && command.geometry->indices.empty();
}

// Draws from the global primitive library rather than owning geometry. Never transform-only: the
// template names a (shape, LOD) that an edit may have changed.
inline bool IsInstancedGeometry(const CommandToCopyThread& command) {
    return command.geometry.has_value() && command.geometry->libraryShapeId >= 0;
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
