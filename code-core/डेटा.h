// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

// This files defines our basic data types to be used by other domain specific data types.
#pragma once // Further to this, Global variables defined here need to be defined with "inline" prefix.
#include <cstdint>
#include <vector>
#include <string>
#include <variant>
#include <any>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <optional>
#include <new> // Required for std::align_val_t
#include <d3d12.h>
#include "CommonNamedNumbers.h"
#include "ID.h"
#include "MemoryManagerCPU.h"
#include "OptionalProperties.h" // Optional64 + MV_DECLARE_OPTIONAL_PROPERTIES, for derived types.
//#include "MemoryManagerGPU.h" // This file must not depend on GPU manager.
#include <d3dx12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <d3dcompiler.h>
#include <DirectXMath.h> //Where from? https://github.com/Microsoft/DirectXMath ?
#include <DirectXPackedVector.h>
#include <unordered_map>

using namespace Microsoft::WRL;
using namespace DirectX;
using namespace DirectX::PackedVector;

/*Each data type will inherit this base struct.
STRICT WARNING: DO NOT ADD ANY MORE FIELDS TO THIS BASE STRUCT. DO NOT ALTER the sequence.
The sequence of fields in this struct has been specifically planned considering "compact struct packing" approach.
For our Data-Structure design approach, read commentary on MemoryManagerCPU.h

One field HAS been added since that warning was written: memoryIDGenerator, 2026-08-23, settling
parenthood across 2D and 3D (mv.ramshanker.in/software/id section 7.2). In the same pass
memoryIDParent was RENAMED to memoryIDContainer - it always meant the container, and sitting next
to a second parent-ish id it had to say so. Anything written before that date, git history and
older design notes included, uses the old name. It took the struct from 56
to 64 bytes - a uint64_t cannot fit the 7 padding bytes that follow isDeleted, whatever the field
order. Those 7 bytes are STILL free and are the only room left: a small field (a flag word, a
uint32_t) costs nothing, a second pointer-sized one costs 8 bytes on every object in the model.
sizeof is asserted below so a casual addition fails the build rather than the memory budget.
*/
// Forward declaration of the global memory manager.

extern राम cpu;

// A tab is assigned a unique memoryGroupNo, which is shared with the downstream analysis thread and so on.
extern uint32_t memoryGroupNo;

// Data for a single geometry object. All 3D entities will generate one for their own graphics representation.
// Currently we are using common heap, latter on we will transition them to our own heap allocator.
/* The LEAN 16-byte vertex (website/software/graphics.md, "Vertex format"). The 8-byte FP16 color
this used to carry moved into the object's 64-byte InstanceRecord as a single packed RGBA8 - a 33%
cut across the bulk of a model, since the overwhelming majority of CAD geometry is one color per
object anyway.

WHAT IT COST: per-FACE color. A cylinder still STORES colorBase / colorTop / colorIncline and always
will, but it now draws in one of them (the dominant surface - see each type's GetGeometry). Per-face
color returns with per-face disaggregation of an object, as a geometry variation rather than as 8
bytes charged to every vertex of every object in the model.

16 is a power of two, unlike the 24 it replaces, so GeometryPage::VertexAlign could now use the
cheap AlignUp mask - deliberately left on RoundUpToMultiple, which is correct for both and is what
the 24-byte variant needs back. */
struct Vertex { // Struct for vertex data
    XMFLOAT3 position; // 12 Bytes
    XMUBYTE4 normal; // 4 Bytes: Packed X, Y, Z, W (padding/0). Uses DXGI_FORMAT_R8G8B8A8_SNORM
}; // Total Stride = 16 Bytes (Perfect alignment!)
static_assert(sizeof(Vertex) == 16,
    "Vertex is the vertex-buffer byte stride: it feeds VertexAlign, IsFull, BaseVertexLocation "
    "(vertexByteOffset / sizeof(Vertex), computed independently in three places) and the two input "
    "layouts. Changing it silently misplaces draws rather than failing.");

/* Shapes in the global primitive library (website/software/graphics.md, "Shared geometry and the
primitive libraries"). A generator names one of these instead of emitting vertices; the mesh itself,
its eight LODs and the table locating them live renderer-side in RenderScene3D.h, which generators
never include. Only shapes whose sole freedom is SIZE belong here - one canonical mesh plus a
non-uniform scale must cover every instance - so anything with an internal ratio (elbow, flange)
stays bespoke rather than becoming an entry. */
enum : int16_t {
    kPrimitiveShapeSphere = 0,
    kPrimitiveShapeCuboid = 1,   // Unit cube, edges 1, centred at the origin.
    kPrimitiveShapeCylinder = 2, // Unit solid rod: axis +Z, radius 1, length 1, CENTRED on z.
};
constexpr uint32_t kPrimitiveShapeCount = 3;

struct GeometryData
{
    uint64_t id = 0; // Unique identifier for the geometry. It is the memoryID of the corresponding engineering object.
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    /* The object's single surface color, set by each generator from its dominant face. This is the
    producer of InstanceRecord::packedColor now that the vertex carries no color: the copy thread
    packs it to RGBA8 on ADD and on a geometry MODIFY, and shadows it in the InstanceRegistryEntry
    so a transform-only MODIFY - which arrives with empty vertex/index vectors - can rewrite the
    64-byte record without losing it. */
    XMFLOAT4 color;
    DirectX::XMFLOAT4X4 worldMatrix;
    /* SHARED GEOMETRY (website/software/graphics.md, "Shared geometry and the primitive libraries").
    >= 0 means this object draws from the global primitive library instead of owning any geometry:
    `vertices` and `indices` are EMPTY and stay that way, and size/position/orientation are carried
    entirely by worldMatrix. -1 is an ordinary bespoke object that brings its own vertices.

    It is also the discriminator that keeps IsTransformOnlyEdit honest. A move and an instanced edit
    BOTH arrive with an empty payload, so "empty" alone cannot tell them apart - and guessing wrong
    either clones a page for nothing or silently keeps the old library entry, so a radius change
    never appears on screen. A move comes from TranslateSelectedSceneObjects, which knows nothing
    about libraries and leaves this at -1; an instanced edit comes through GeometryForObject. */
    int16_t libraryShapeId = -1;
	GeometryData() {
        color = XMFLOAT4(0.8f, 0.8f, 0.8f, 1.0f); // Default color: light gray
        worldMatrix = {
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f
        };
    }
};

/* Rigid placement of one 3D object: the transform from its AUTHORED coordinates - whatever its own
point fields hold (SPHERE::center, CYLINDER::p1/p2, CUBOID::center, ...) - to WORLD coordinates.
Identity for a newly created object and for every object authored before this field existed.

This is what lets an interactive move cost nothing (website/software/graphics.md, 10M plan Step 4).
Moving an object rewrites its placement and emits a TRANSFORM-ONLY MODIFY - one fresh 64-byte
instance record plus one 4-byte redirect flip - instead of regenerating and re-uploading geometry.
GeometryForObject composes it into GeometryData::worldMatrix above, and that is the SINGLE point
where a placement takes effect; every ADD, geometry MODIFY, file load and import inherits it there.

RIGID ONLY, deliberately: the rotation is a unit quaternion and there is no scale factor, matching
the uniform-scale assumption the InstanceRecord shader path already documents. Being rigid also
means every scalar dimension (radius, length, diameter) is unaffected by a placement, so only POINT
fields ever need composing when converting between authored and world coordinates. */
struct Placement3D {
    XMFLOAT3 origin = { 0.0f, 0.0f, 0.0f };
    XMFLOAT4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f }; // Unit quaternion (x, y, z, w). Identity.

    // Encoders skip writing an identity placement entirely, so a file of never-moved objects stays
    // byte-identical to what the previous schema version produced.
    bool IsIdentity() const {
        return origin.x == 0.0f && origin.y == 0.0f && origin.z == 0.0f &&
            rotation.x == 0.0f && rotation.y == 0.0f && rotation.z == 0.0f && rotation.w == 1.0f;
    }

    // Row-vector convention throughout the engine - the vertex shader computes `pos * W` - so this
    // reads left to right as rotate, then translate.
    XMMATRIX ToMatrix() const {
        return XMMatrixRotationQuaternion(XMLoadFloat4(&rotation)) *
            XMMatrixTranslation(origin.x, origin.y, origin.z);
    }

    // Authored -> world for a single point, and its exact inverse. Used by the Properties Pane to
    // show and accept WORLD coordinates while the object keeps storing authored ones. Being rigid
    // is what makes the inverse a plain un-rotate-then-un-translate rather than a matrix inversion.
    XMVECTOR TransformPoint(FXMVECTOR authored) const {
        return XMVectorAdd(XMVector3Rotate(authored, XMLoadFloat4(&rotation)),
            XMLoadFloat3(&origin));
    }

    XMVECTOR InverseTransformPoint(FXMVECTOR world) const {
        return XMVector3InverseRotate(XMVectorSubtract(world, XMLoadFloat3(&origin)),
            XMLoadFloat4(&rotation));
    }
};

inline XMUBYTE4 PackNormal(XMFLOAT3 n) {
    // Normalize first to be safe
    XMVECTOR v = XMLoadFloat3(&n);
    v = XMVector3Normalize(v);
    XMFLOAT3 norm;
    XMStoreFloat3(&norm, v);

    // Compress float (-1.0 to 1.0) to byte (0 to 255) representing SNORM
    // simple packing: (val * 127.0f) 
    // Note: C++ casting to int8_t handles the bit representation for SNORM usually, 
    // but XMUBYTE4 is unsigned char, so we rely on the specific casting or manual mapping.
    // DXGI_FORMAT_R8G8B8A8_SNORM interprets 0x7F as 1.0 and 0x81 as -1.0. 

    // Easier approach: Use XMNORMAL helper from library if available, but manual here:
    auto toSNORM = [](float f) -> uint8_t {
        return (uint8_t)(int8_t)(std::clamp(f, -1.0f, 1.0f) * 127.0f);
        };

    return XMUBYTE4(toSNORM(norm.x), toSNORM(norm.y), toSNORM(norm.z), 0);
}

/* The stored per-face colors are XMHALF4; GeometryData::color is XMFLOAT4. Each generator uses this
to nominate one of its faces as the object's color, which is the only colour that reaches the GPU
now that the vertex carries none. */
inline XMFLOAT4 ToFloat4(const XMHALF4& color) {
    XMFLOAT4 result;
    XMStoreFloat4(&result, XMLoadHalf4(&color));
    return result;
}

/* GeometryData::color -> InstanceRecord::packedColor. RGBA8 with red in the LOW byte, matching the
XMUBYTE4 convention PackNormal already uses and UnpackColorRGBA8 in ShaderSceneVertex_16.hlsl.
Alpha is the object's opacity, so no separate transparency field is needed.

Values are clamped to [0,1]: the FP16 vertex color this replaces could exceed 1.0 and RGBA8 cannot.
That costs nothing today - CAD surface colors are authored in range and HDR is an OUTPUT-side
concern (render-target format + tonemap, graphics.md Phase 5) - but an emissive / overbright base
color is no longer expressible, and would need the FP16 payload variant instead. */
inline uint32_t PackColorRGBA8(const XMFLOAT4& color) {
    auto toByte = [](float f) -> uint32_t {
        return static_cast<uint32_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
    return toByte(color.x) | (toByte(color.y) << 8) | (toByte(color.z) << 16) |
        (toByte(color.w) << 24);
}

struct META_DATA {
    uint64_t memoryID = 0;// This is temporary CPU ID inside currently running software process.
    /* THE CONTAINER this object lives in - the Scene3D, Page2D or FOLDER that holds it. Temporary
    CPU ID. "0" simply means it has not been initialized. This meaning is uniform across 2D and 3D
    (mv.ramshanker.in/software/id section 7.2): if you want to know WHERE an object is shown, this
    is the field. Do not overload it with ownership - that is the next one. */
    uint64_t memoryIDContainer = 0;
    /* THE GENERATOR that produced this object - the asset instance or template it was stamped out
    of. 0 for anything a user drew directly, which is most objects. Containment and generation are
    genuinely different questions: a member of a placed asset is DRAWN ON the page (memoryIDContainer)
    but BELONGS TO the insert (memoryIDGenerator), and deleting the insert takes the member with it
    while deleting the page does something else entirely.

    PERSISTENCE: there is no persistedGeneratorId, deliberately. The file stores ONE parent id -
    the generator if there is one, else the container - and the loader rebuilds the split by asking
    what TYPE the stored parent turned out to be. Page2D parent means it was the container; an
    Asset2DInsert or Asset2DDefinition parent means it was the generator, and the container is then
    the generator's own. That is what the 2D save/load path already does; see resolveLineParentId
    and resolve2DGeometryParent in DataStorage.cpp. */
    uint64_t memoryIDGenerator = 0;
    uint64_t persistedId = 0;      // This is the unique ID within the saved file.
    uint64_t persistedParentId = 0;// This is the unique ID within the saved file.

    // For each loaded yyy/zzz file, we will have an index. To assist with saving things to disc.
    // This way we can handle a total of 4 Billion loaded files. This is our SYSTEM limit.
    uint32_t xxxFileIndex = 0;
    uint16_t dataType = 0; // Each unique class type. Derived class will set this value. To assist with linear scan etc. 
    uint16_t schemaVersion = 0; // Derived class will set this value. To assist with versioning of data structure.

    // Every time a variable changes, we increment this to signal other threads.
    uint64_t dataVersion = 1; // Incremented on each modification. std::atomic discarded.
    bool isDeleted{ false };                // Soft-delete flag

	META_DATA() { memoryID = MemoryID::next(); }; // Assign a unique memoryID at creation
	GeometryData GenerateGeometry() { return GeometryData(); }; // Default implementation. Derived class will override.

    /* Overload the `new` operator. This is the magic that intercepts object creation.
    Delegate the allocation request to the global memory manager,
    passing the required size and the current thread's tab ID.
    The manager will handle the raw allocation. C++ runtime then calls the constructor. */
    void* operator new(uint64_t size, uint32_t memoryGroupNo) {
        return cpu.Allocate(size, memoryGroupNo);
    }
    void operator delete(void* ptr, uint32_t memoryGroupNo) {
        (void)memoryGroupNo;
        cpu.Free(reinterpret_cast<std::byte*>(ptr));
    }
    // It's good practice to provide overloaded new/delete for arrays as well.
    void* operator new[](uint64_t size, uint32_t memoryGroupNo) {
        return cpu.Allocate(size, memoryGroupNo);
    }
    void operator delete[](void* ptr, uint32_t memoryGroupNo) {
        (void)memoryGroupNo;
        cpu.Free(reinterpret_cast<std::byte*>(ptr));
    }
    /*No downstream derived class should create an object without specifying memoryGroupNo.
    This way we ensure more strict memory partitioning between isolated tabs.
    However in case it is missed anyway than we create on default tab 0. Memory may leak here.*/
    void* operator new(uint64_t size) { return cpu.Allocate(size, 0); };
    void* operator new[](uint64_t size) { return cpu.Allocate(size, 0);};

    // Overload the `delete` operator. Delegate the free request to the global memory manager.
    void operator delete(void* ptr) { cpu.Free(reinterpret_cast<std::byte*>(ptr)); }
    void operator delete[](void* ptr) { cpu.Free(reinterpret_cast<std::byte*>(ptr)); }

    // Disable default heap allocations to prevent accidental misuse.
    // Making these private and not defining them will cause a compile-time error
    // if someone tries to call `::new ArenaObject`.

private:
    static void* operator new(uint64_t size, void* ptr) = delete;
    static void operator delete(void* memory, void* ptr) = delete;
};

/* Every object in the model carries this, so a byte here is a byte times ten million. Making the
size an assertion rather than a comment is what turns "DO NOT ADD ANY MORE FIELDS" into something
the build enforces. If you meant to grow it, change the number here and say why above. */
static_assert(sizeof(META_DATA) == 64, "META_DATA grew - see the field-budget note above it.");

// Following are some special data types designed to be dynamically allocated by our RAM Manager.

class CustomString {// System Limit: 4 GB for individual dynamically allocated properties.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;   //If used bytes is less than or equal to 8, we don't heap allocate,
    //i.e. store the byte directly in "bytes" variable. It's called Small String Optimization.
    std::byte* str; //utf8 encoded.

public:
    CustomString() {
        allocatedBytes = 0;
        usedBytes = 0;
        str = nullptr;
    }
    //TODO: Improvised on this constructor to use our custom memory allocator.
};

struct c_string {};// To be deleted.

/* We don't have CustomDouble, CustomFloat, CustomInt, CustomLong etc. because they are fixed size variables.
Whenever we want them to optionally available as part of engineering data, and stored in memory when present,
it will be managed OptionalProperties Class as 1 boolean for each such dynamic property.
This way we avoid the high pointer overhead small dynamic properties.*/

/* Following struct demonstrate how various engineering Objects use the metaData.
The FOLDER is also an engineering data, helping with organization of other engineering data in User Interface.
This is our administrative element used for selection tree organization.
This will also provide the short-codes used for naming of various equipments & instruments
buildings, or any way people may want to organize their information.*/
struct FOLDER : META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Folder;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kLogicalElementSchemaVersion;
    
    //Mandatory Properties
    char name[128] = {};     //Null terminated utf-8 encoded string.
    char shortCode[16] = {}; //Short Prefix for naming use. utf-8 encoded.
    uint64_t previousSequenceNo = 0, nextSequenceNo = 0; //For display in selection tree.
    //TODO: Design an approach such that we can do sequencing using just 8 bytes instead of 16.

    uint16_t optionalFieldsFlags = 0;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags = 0;          // 32 booleans for internal use only. Not persisted.

    //Variable Length Properties
    CustomString displayName; // Optionally 
};

struct PAGE2D : META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Page2D;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kLogicalElementSchemaVersion;

    char name[128] = {};
    float widthMM = 841.0f;
    float heightMM = 594.0f;
    uint64_t previousSequenceNo = 0, nextSequenceNo = 0;
    uint16_t optionalFieldsFlags = 0;
    uint16_t systemFlags = 0;
};

struct SCENE3D : META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Scene3D;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kLogicalElementSchemaVersion;

    char name[128] = {};
    uint64_t previousSequenceNo = 0, nextSequenceNo = 0;
    uint16_t optionalFieldsFlags = 0;
    uint16_t systemFlags = 0;
};

