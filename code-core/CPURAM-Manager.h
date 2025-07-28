// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

/* Goal of our In-Memory data structure: 
The data set being handled by the application will have classes of approx. 
1000 to 5000 unique types. Each class type will have 10 to 50 fields. 
Certain mandatory fields (members) and around 2/3rd being optional values, 
some of which could be utf-8 string. These optional values could be set or unset. 
Design a memory data structure / algorithmic approach such that least amount of space 
is wasted storing unneeded fields, with flexibility to change 
the field values as and when required by the user. 
I will be persisting these classes using their protocol buffer representations. 
Each object shall be identified by a unique 64bit ID. 
I also need to be able to quickly retrieve any particular ID, or confirm it's absence. 
Not afraid to use state-of-art industry standards algorithms.
Could even consider small string optimizations etc.

Proposed solution:
┌───────────────────────────────────────────────────────────┐
│ uint64_t  id;     // 8 bytes (Part of meta-data)          │
│ bool/int/char/float/double mandatoryFeilds; // Fixed‑size │
│ uint64_t  presence_mask;         // bit-mask for up to 64 │
│ // followed by N small-value slots, in order:             │
│ // ┌───────────────────────────────────────────────────┐  │
│ // │ optional slot 0 (if present)                      │  │
│ // │ optional slot 1 (if present)                      │  │
│ // │   …                                               │  │
│ // │ optional slot F (if present)                      │  │
│ // └───────────────────────────────────────────────────┘  │
│ // followed by string storage area (packed back‑to‑back)  │
└───────────────────────────────────────────────────────────┘

We will not use std::optional<T> because It will consume min. 8 Bytes even if "T" is not present.
I also want to quickly access all objects of a particular class having another field 
equal to say "2"? It will be like in-memory indexing over the table.
We will have index only over certain fields. For others, we will do a linear scan.
*/
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <cstring> // for memcpy
#include <unordered_map>
#include <iostream>
#include <mutex>

#include "डेटा.h"

struct CPU_RAM_4MB {
    //1 KB ( 1024 Bytes) out of 4 MB ( = 4 * 1024 * 1024 Bytes) is reserved for meta-data.
    static const uint32_t METADATA_SIZE = 1024;
    static const uint32_t DATA_BLOCK_SIZE = (4 * 1024 * 1024) - METADATA_SIZE;

    // We must not use more than 1 KB (1024 Bytes) for meta-data of this CPU RAM Chunk.
    uint32_t newDataSpace = 0;     // 4 Bytes. How much space is left starting with nextDataPointer
    uint32_t totalFreeSpace = 0;   // 4 Bytes
    uint32_t totalUsedSpace = 0;   // 4 Bytes
    bool isChunkAllocated = false; // 1 Bytes
    bool isChunkFull = false;      // 1 Bytes 
    std::byte* nextDataLocation = nullptr;// 8 Byte. This is where new data will be appended.

    static const uint32_t dataBlockSize = 4 * 1024 * 1024 - 1024; // Not per Object. Global variable.
    
    std::byte dataBlock[DATA_BLOCK_SIZE];

    CPU_RAM_4MB() { // Constructor Function. Initialize all the fields.
        newDataSpace = dataBlockSize;
        totalFreeSpace = dataBlockSize;
        totalUsedSpace = 0;
        nextDataLocation = dataBlock;
        isChunkAllocated = true;
        isChunkFull = false;
    }
};

struct DATALocation {
    uint32_t chunkIndex;   // It needs 3 Byte only.
    uint32_t offsetInChunk;// It needs 3 Byte only.
    //We have 2 bytes margin in above definitions. We can use it latter using bit numbering.
};

// There will be exactly 1 object of this class in entire application. Hence the special name.
// भगवान राम की कृपा बानी रहे. Corresponding object is named "cpuRAMManager".
class राम {
public:
    // Global variables with proper initialization
    uint64_t physicalRAMInstalled = 8ULL * 1024 * 1024 * 1024; // 8 GB. TODO: Read system RAM.
    const uint64_t cpuRAMChunkSize = sizeof(CPU_RAM_4MB);
    uint32_t cpuRAMChunkLimit = static_cast<uint32_t>(physicalRAMInstalled / cpuRAMChunkSize);

    /* We try to restrict our memory limit to what is physically installed on machine.
    However our approach is of soft-limit. Users will get warning only when we have actually exceeded the system RAM limit.
    We do not take into account, other running applications, hence we may already be getting paged-out to disc when
    nearing the limit. It's not a hard limit. */

    //TODO: Standard Library unordered_map does not take advantage of SIMD capabilities.
    // Latter on we will have our own implementation. We want this performance to be extreme.
    // This is core operation in our application. Ask AI for better options.
    // We use a high-performance hash map for ID-to-location mapping.
    // NOTE: For extreme performance, consider replacing with a more specialized hash map
    // like absl::flat_hash_map or tsl::hopscotch_map which are more cache-friendly.
    std::unordered_map<uint64_t, DATALocation> id2ChunkMap;

    राम() { InitializeMemorySystem(); };
    ~राम() { CleanupMemorySystem();   };
    void InitializeMemorySystem();
    void CleanupMemorySystem();

    // The main interface for the logic thread
    bool AllocateNewObject(const CreateObjectCmd& cmd);
    bool ModifyObject(const ModifyObjectCmd& cmd);
    bool DeleteObject(uint64_t id);

    // Accessors for the logic thread to inspect objects
    std::optional<META_DATA*> GetMETA_DATA(uint64_t id);
    std::optional<std::byte*> GetObjectPayload(uint64_t id);

    // For iterating over all objects to find "dirty" ones
    // Returns a copy of the map to iterate over without holding the lock.
    std::unordered_map<uint64_t, DATALocation> GetIdMapSnapshot();

    void DefragmentRAMChunks(uint32_t chunkIndex);

private:
    // Helper function to find space and allocate
    std::optional<DATALocation> FindSpaceAndAllocate(uint32_t totalSpaceNeeded);

    // Using std::mutex for thread-safety, as this class will be called from the Main Logic thread,
    // which is the single writer to the data repository. A mutex provides safety if we ever
    // change this architecture.
    std::mutex mutex;

    std::vector<CPU_RAM_4MB*> RAMChunks; // NULL identifies the chunk has not been allocated.
    uint32_t RAMChunksAllocatedCount = 0; // Just a tracker. Whenever we reach up-to cpuRAMChunkCount, we soft-warn users.
    uint32_t activeChunkIndex = 0;

};
