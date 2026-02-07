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
#include "ID.h"
#include "MemoryManagerCPU.h"
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
*/
// Forward declaration of the global memory manager.

extern राम cpu;

// A tab is assigned a unique memoryGroupNo, which is shared with the downstream analysis thread and so on.
extern uint32_t memoryGroupNo;

// Data for a single geometry object. All 3D entities will generate one for their own graphics representation.
// Currently we are using common heap, latter on we will transition them to our own heap allocator.
struct Vertex { // Struct for vertex data
    XMFLOAT3 position; // 12 Bytes
    XMUBYTE4 normal; // 4 Bytes: Packed X, Y, Z, W (padding/0). Uses DXGI_FORMAT_R8G8B8A8_SNORM
    XMHALF4  color;  // 8 Bytes
}; // Total Stride = 24 Bytes (Perfect alignment!)

struct GeometryData
{
    uint64_t id = 0; // Unique identifier for the geometry. It is the memoryID of the corresponding engineering object.
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    XMFLOAT4 color;
	GeometryData() { color = XMFLOAT4(0.8f, 0.8f, 0.8f, 1.0f); } // Default color: light gray
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

struct META_DATA {
    uint64_t memoryID = 0;// This is temporary CPU ID inside currently running software. Scoped within each tab.
    uint64_t memoryIDParent = 0; // This is temporary CPU ID. "0" simply means it has not been initialized.
    uint64_t persistedId = 0;      // This is the unique ID within the saved file.
    uint64_t persistedParentId = 0;// This is the unique ID within the saved file.

    // For each loaded yyy/zzz file, we will have an index. To assist with saving things to disc.
    // This way we can handle a total of 4 Billion loaded files. This is our SYSTEM limit.
    uint32_t xxxFileIndex = 0;
    uint16_t dataType = 0; // Each unique class type. Derived class will set this value. To assist with linear scan etc. 
    uint16_t schemaVersion = 0; // Derived class will set this value. To assist with versioning of data structure.

    // Every time a variable changes, we increment this to signal other threads.
    std::atomic<uint64_t> dataVersion{ 1 }; // Incremented on each modification
    std::atomic<uint64_t> geometryRenderedVersion{ 0 }; // The version last seen by the GPU processing logic
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
    // It's good practice to provide overloaded new/delete for arrays as well.
    void* operator new[](uint64_t size, uint32_t memoryGroupNo) {
        return cpu.Allocate(size, memoryGroupNo);
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

// Following are some special data types designed to be dynamically allocated by our RAM Manager.

class CustomString {// System Limit: 4 GB for individual dynamically allocated properties.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;   //If used bytes is less than or equal to 8, we don't heap allocate,
    //i.e. store the byte directly in "bytes" variable. It's called Small String Optimization.
    std::byte* str; //utf8 encoded.

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
    
    //Mandatory Properties
    char name[128];     //Null terminated utf-8 encoded string.
    char shortCode[16]; //Short Prefix for naming use. utf-8 encoded.
    uint64_t previousSequenceNo, nextSequenceNo; //For display in selection tree.
    //TODO: Design an approach such that we can do sequencing using just 8 bytes instead of 16.

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Variable Length Properties
    CustomString displayName; // Optionally 
};

