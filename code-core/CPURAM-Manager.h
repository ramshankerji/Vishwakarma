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

#include <cstdint>
#include <cstddef>
#include <vector>
#include <cstring> // for memcpy
#include <unordered_map>
#include <iostream>

#include "डेटा.h"

struct CPU_RAM_4MB {
    // We must not use more than 1 KB (1024 Bytes) for meta-data of this CPU RAM Chunk.
    uint32_t newDataSpace = 0;     // 4 Bytes. How much space is left starting with nextDataPointer
    uint32_t totalFreeSpace = 0;   // 4 Bytes
    uint32_t totalUsedSpace = 0;   // 4 Bytes
    bool isChunkAllocated = false; // 1 Bytes
    bool isChunkFull = false;      // 1 Bytes 
    std::byte* nextDataLocation = nullptr;// 8 Byte. This is where new data will be appended.

    static const uint32_t dataBlockSize = 4 * 1024 * 1024 - 1024; // Not per Object. Global variable.
    //1 KB ( 1024 Bytes) out of 4 MB ( = 4 * 1024 * 1024 Bytes) is reserved for meta-data.
    std::byte dataBlock[dataBlockSize];

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

// Global variables with proper initialization
const uint64_t physicalRAMInstalled = 8ULL * 1024 * 1024 * 1024; // 8 GB.
const uint64_t cpuRAMChunkSize = sizeof(CPU_RAM_4MB);
uint32_t cpuRAMChunkLimit = static_cast<uint32_t>(physicalRAMInstalled / cpuRAMChunkSize);

/* We try to restrict our memory limit to what is physically installed on machine.
However our approach is of soft-limit. Users will get warning only when we have actually exceeded the system RAM limit.
We do not take into account, other running applications, hence we may already be getting paged-out to disc when
nearing the limit. It's not a hard limit. */

std::vector<CPU_RAM_4MB*> RAMChunks(cpuRAMChunkLimit, nullptr); // NULL identifies the chunk has not been allocated.
uint32_t RAMChunksAllocatedCount = 0; // Just a tracker. Whenever we reach up-to cpuRAMChunkCount, we soft-warn users.
uint32_t activeChunkIndex = 0;

//TODO: Standard Library unordered_map does not take advantage of SIMD capabilities.
// Latter on we will have our own implementation. We want this performance to be extreme.
// This is core operation in our application. Ask AI for better options.
std::unordered_map<uint64_t, DATALocation> id2ChunkMap;

void InitializeMemorySystem() { // Initialize the system - should be called once at startup
    RAMChunks.reserve(cpuRAMChunkLimit); // Overhead: 4GB RAM=>8*1024=8KB , 4TB Server=8*1024*1024=8MB : Negligible.
    RAMChunks[0] = new CPU_RAM_4MB; // We allocate 1 chunk at the beginning itself.
    RAMChunksAllocatedCount = 1; // Even on 16 TB system, this count will not grow to more than 2^22. (16TB/4MB)
    activeChunkIndex = 0;
}

void AllocateNewObject(META_DATA& metaData, void* data, uint32_t dataSize) {
    // WARNING: We do not yet support more than 4MB dataSize. TODO: Develop Multi Chunk data storage logic.
    // WARNING: This will not check if the provided id is already present in hash-map. 
    // Hence this function can be used for initial file load only.
    // Duplicate id will be silently discarded without crashing the application.
    if (dataSize > CPU_RAM_4MB::dataBlockSize) return;
    if (data == nullptr) return; //Safety check to prevent data corruption / crash.
    if (id2ChunkMap.find(metaData.id) != id2ChunkMap.end()) return;

    // metaData = { ID: 8 Byte, Parent ID: 8 Byte, dataSize: 4 Byte, fileID: 4 Bytes}
    uint32_t totalSpaceNeeded = sizeof(META_DATA) + dataSize;
    uint32_t alignedSpaceNeeded = (totalSpaceNeeded + 7) & ~0x7U;
    if (RAMChunks[activeChunkIndex]->newDataSpace < alignedSpaceNeeded)
    {   // Means we need to allocate a new RAMChunk. 1st Check if we need to grow the vector
        if (activeChunkIndex + 1 >= static_cast<uint32_t>(RAMChunks.size())) {
            RAMChunks.resize(RAMChunks.size() + 16, nullptr); // Grow vector by 16 slots 
            cpuRAMChunkLimit += 16; // and update limit.
        }
        RAMChunks[activeChunkIndex]->isChunkFull = true; // Mark the current RAM Chunk as full.
        RAMChunks[activeChunkIndex + 1] = new CPU_RAM_4MB;
        RAMChunksAllocatedCount++; // Add this line
        activeChunkIndex++;
    }
    // Now that we have ensured that sufficient space is available.
    // Copy metaData bytes starting with RAMChunks[activeChunkIndex]->nextDataLocation pointer.
    std::memcpy(RAMChunks[activeChunkIndex]->nextDataLocation, &metaData, sizeof(META_DATA));
    // Copy dataSize bytes starting with RAMChunks[activeChunkIndex]->nextDataLocation+24 pointer.
    std::memcpy(RAMChunks[activeChunkIndex]->nextDataLocation + sizeof(META_DATA), data, dataSize);

    //Add the meta-data id to global map for fast retrieval.
    id2ChunkMap[metaData.id] = DATALocation{ activeChunkIndex,
        static_cast<uint32_t>(RAMChunks[activeChunkIndex]->nextDataLocation 
            - RAMChunks[activeChunkIndex]->dataBlock) };

    // Calculate aligned next position. x86_64 (Intel/AMD) is OK, but ARM / RISCV force 8 byte alignment.
    uintptr_t unalignedPtr = reinterpret_cast<uintptr_t>(RAMChunks[activeChunkIndex]->nextDataLocation) +
        sizeof(META_DATA) + dataSize;
    uintptr_t alignedPtr = (unalignedPtr + 7) & ~0x7ULL; // Round up to next multiple of 8
    // Update pointer and counters
    RAMChunks[activeChunkIndex]->nextDataLocation = reinterpret_cast<std::byte*>(alignedPtr);
    RAMChunks[activeChunkIndex]->newDataSpace    -= alignedSpaceNeeded;
    RAMChunks[activeChunkIndex]->totalFreeSpace  -= alignedSpaceNeeded;
    RAMChunks[activeChunkIndex]->totalUsedSpace  += alignedSpaceNeeded;
}

void DeleteDataFromRAM(uint64_t id) {
    //Implement this function. Find the chunkIndex using id2ChunkMap.
    //Update Relevant details in RAMChunks and mark it as free.
    //Remove id from id2ChunkMap.
}

void DefragmentRAMChunks(uint32_t chunkIndex) {
    //Compress RAMChunks to free up CPU RAM. Update id2ChunkMap for all the IDs which have moved.
}

/* For our memory management system, since we're dealing with large chunks of RAM and is a core part of
the application that would run until program termination, we can safely skip the cleanup function.
The OS will handle it. However to satisfy Valgrind, AddressSanitizer, or Visual Studio's diagnostic tools,
and make our life easier catching relevant bugs there, we do the cleanup anyway.*/
void CleanupMemorySystem() {
    for (auto chunk : RAMChunks) { delete chunk; }
    RAMChunks.clear();
    id2ChunkMap.clear();
    RAMChunksAllocatedCount = 0;
    activeChunkIndex = 0;
}
