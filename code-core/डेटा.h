// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

// This files defines our basic data types to be used by other domain specific data types.
#pragma once
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

/*Each data type will inherit this base struct.
STRICT WARNING: DO NOT ADD ANY MORE FIELDS TO THIS BASE STRUCT.
The sequence of fields in this struct has been specifically planned considering 
"compact struct packing" approach. DO NOT ALTER the sequence.

For our Data-Structure design approach, reach commentary on डेटा-CPURAM-Manager.h
*/
// Represents the meta-data stored at the beginning of each object in a RAM chunk.
struct META_DATA {
    uint64_t id;                  // 8 bytes 
    uint64_t parentId;      // 8 bytes 
    
    // In general our dataSize will be maximum few kilo bytes per object only.
    // The following will limit the maximum external data (ex: Image, PDF, other native files etc.)
    // TODO: Notice that our MEMORY manager is not capable to handle more then 4MB data per object.
    uint32_t dataSize; // Total size of data of the inherited struct, inclusive of optional fields.

    // For each loaded yyy/zzz file, we will have an index. To assist with saving things to disc.
    // This way we can handle a total of 4 Billion loaded files. This is our SYSTEM limit.
    uint32_t xxxFileIndex;

    uint16_t dataType; // Each unique class type. To assist with linear scan etc.

    uint16_t reserved1; //reserved for future use.
    uint16_t reserved2; //reserved for future use.
    uint16_t reserved3; //reserved for future use.
    
    // So we have minimum 32 Byte overhead per Object.

    // --- Architecture-specific fields ---
    std::atomic<uint64_t> dataVersion{ 1 }; // Incremented on each modification
    uint64_t lastProcessedVersion{ 0 };     // The version last seen by the GPU processing logic
    bool isDeleted{ false };                // Soft-delete flag
    // Padding to ensure 8-byte alignment can be added if necessary
};

// Following are some special data types designed to be dynamically allocated by our RAM Manager.

struct c_string {// System Limit: 4 GB for individual dynamically allocated properties.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;   //If used bytes is less than or equal to 8, we don't heap allocate,
    //i.e. store the byte directly in "bytes" variable. It's called Small String Optimization.
    std::byte* str; //utf8 encoded.
};

// System Limit: 4 GB for individual dynamically allocated properties.
struct c_double {
    // Used by Polylines, Contour Plates etc.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;
    double* d;
};

struct c_float {// System Limit: 4 GB for individual dynamically allocated properties.
    // Used by Polylines, Contour Plates etc.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;
    float* n;
};

struct c_uint64 {// System Limit: 4 GB for individual dynamically allocated properties.
    // Used by Polylines, Contour Plates etc.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;
    uint64_t* n;
};

struct c_uint32 {// System Limit: 4 GB for individual dynamically allocated properties.
    // Used by Polylines, Contour Plates etc.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;
    uint32_t* n;
};

struct c_uint16 {// System Limit: 4 GB for individual dynamically allocated properties.
    // Used by Polylines, Contour Plates etc.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;
    uint16_t* n;
};

struct c_uint8 {// System Limit: 4 GB for individual dynamically allocated properties.
    // Used by Polylines, Contour Plates etc.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;
    uint8_t* n;
};

struct FOLDER {
    META_DATA metaData;
    /*This is our administrative element used for selection tree organization.
    This will also provide the short-codes used for naming of various equipments & instruments
    buildings, or any way people may want to organize their information.*/

    //Mandatory Properties
    char name[128];     //Null terminated utf-8 encoded string.
    char shortCode[16]; //Short Prefix for naming use. utf-8 encoded.
    uint64_t previousSequenceNo, nextSequenceNo; //For display in selection tree.
    //TODO: Design an approach such that we can do sequencing using just 8 bytes instead of 16.

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
    c_string displayName; // Optionally 
};

//////////////////////////////////////////////////////////

// --- Thread-Safe Queue for Inter-Thread Communication ---

template<typename T>
class ThreadSafeQueue {
public:
    void push(T value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(value));
        m_cond.notify_one();
    }

    // Non-blocking pop
    bool try_pop(T& value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) {
            return false;
        }
        value = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }

    // Shuts down the queue, waking up any waiting threads
    void shutdown() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
        m_cond.notify_all();
    }

private:
    std::queue<T> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    bool m_shutdown = false;
};

// --- Command Definitions for the Input->MainLogic Queue ---

struct CreateObjectCmd {
    uint64_t desiredId;
    uint64_t parentId;
    std::vector<std::byte> data; // The actual object data
};

struct ModifyObjectCmd {
    uint64_t id;
    std::vector<std::byte> newData;
};

struct DeleteObjectCmd {
    uint64_t id;
};

// Using a variant for type-safe command storage
using InputCommand = std::variant<CreateObjectCmd, ModifyObjectCmd, DeleteObjectCmd>;


// --- GPU-related Definitions ---

// Information about a resource currently residing in VRAM
struct GpuResourceInfo {
    uint64_t vramOffset; // Simulated VRAM address
    size_t size;
    // In a real DX12 app, this would hold ID3D12Resource*, D3D12_VERTEX_BUFFER_VIEW, etc.
};

// Commands for the MainLogic -> GpuCopy Thread Queue
struct GpuUploadCmd {
    uint64_t objectId;
    uint64_t objectDataVersion;
    std::vector<std::byte> data; // Vertex/Index data to be uploaded
};

struct GpuFreeCmd {
    uint64_t objectId;
    GpuResourceInfo resourceToFree; // The specific VRAM resource to be reclaimed
};

using GpuCommand = std::variant<GpuUploadCmd, GpuFreeCmd>;

// Packet of work for a Render Thread for one frame
struct RenderPacket {
    uint64_t frameNumber;
    std::vector<uint64_t> visibleObjectIds;
};




