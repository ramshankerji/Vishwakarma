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
#include <optional>
#include <algorithm> // Required for std::lower_bound

#include "डेटा.h"

struct FREE_RAM_RANGES {
    // Simple struct to track all the free spaces in the RAM Chunks. RAM Chunks are also knows as areana.
    uint32_t startOffset = 0;
    uint32_t freeBytes = 0;
    //For our 4MB RAM Chunks, above variable will be maximum 2^22 only. Which can be accomodated in 
    //3 Bytes ( 2^24 ). We have 10x2=20 bits margin here for additional info if we impliment
    //bit packing in future.
};

struct CPU_RAM_4MB {
    //1 KB ( 1024 Bytes) out of 4 MB ( = 4 * 1024 * 1024 Bytes) is reserved for meta-data.
    // We must not use more than 1 KB (1024 Bytes) for meta-data of this CPU RAM Chunk.

    // "static const" are not per Object of this struct. There are global properties of the struct.
    static const uint32_t METADATA_SIZE = 1024;
    static const uint32_t DATA_BLOCK_SIZE = (4 * 1024 * 1024) - METADATA_SIZE;
    
    uint32_t tabIndex = 0;
    std::vector<FREE_RAM_RANGES> freeByteRangesList; // Track the free space within the 4 MB Range.
    uint32_t totalFreeSpace = 0;   // 4 Bytes
    uint32_t totalUsedSpace = 0;   // 4 Bytes
    bool isChunkAllocated = false; // 1 Bytes
    bool isChunkFull = false;      // 1 Bytes 
    std::byte dataBlock[DATA_BLOCK_SIZE]; // Our THE data range.

    CPU_RAM_4MB(uint32_t tabNo) { // Constructor Function. Initialize all the fields.
        tabIndex = tabNo;
        totalFreeSpace = DATA_BLOCK_SIZE;
        totalUsedSpace = 0;
        isChunkAllocated = true;
        isChunkFull = false;

        // The chunk starts with one single free block covering the entire data area.
        // Offsets are relative to the start of dataBlock, so the first block starts at 0.
        freeByteRangesList.push_back({.startOffset = 0, .freeBytes = DATA_BLOCK_SIZE });
        std::memset(dataBlock, 0, sizeof(dataBlock)); // Clear the data block (set all bytes to 0)
    }
};

struct DATALocation {
    uint32_t chunkIndex;   // It needs 3 Byte only.
    uint32_t offsetInChunk;// It needs 3 Byte only.
    //We have 2 bytes margin in above definitions. We can use it latter using bit numbering.
};

/*
There will be exactly 1 object of this class in entire application.Hence the special name.
The global host cpu's Memory (RAM) manager.
Basically it consists of a series of 4 MB Chunks. However 1 Chunks belong to 1 tab only.
भगवान राम की कृपा बानी रहे. Corresponding object is named "cpuRAMManager".
TODO: Improve this function such that 1 Chunk is dedicated to 1 tab only.
*/
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
    // Helper function to find space and allocate from the freelist
    std::optional<DATALocation> FindSpaceAndAllocate(uint32_t totalSpaceNeeded);

    // Helper function to return a memory block to the freelist
    void AddToFreelist(uint32_t chunkIndex, uint32_t offset, uint32_t size);

    // Using std::mutex for thread-safety, as this class will be called from the Main Logic thread,
    // which is the single writer to the data repository. A mutex provides safety if we ever
    // change this architecture.
    std::mutex mutex;

    std::vector<CPU_RAM_4MB*> RAMChunks; // NULL identifies the chunk has not been allocated.
    uint32_t RAMChunksAllocatedCount = 0; // Just a tracker. Whenever we reach up-to cpuRAMChunkCount, we soft-warn users.
    uint32_t activeChunkIndex = 0; //TODO: Move to tab scope, when tabs are implimented.

};

inline void राम::InitializeMemorySystem() { // Initialize the system - should be called once at startup
    if (!RAMChunks.empty()) return; // Already initialized
    physicalRAMInstalled = 8ULL * 1024 * 1024 * 1024; // 8 GB
    cpuRAMChunkLimit = static_cast<uint32_t>(physicalRAMInstalled / sizeof(CPU_RAM_4MB));

    RAMChunks.assign(cpuRAMChunkLimit, nullptr); // Overhead: 4GB RAM=>8*1024=8KB , 4TB Server=8*1024*1024=8MB : Negligible.
    RAMChunks[0] = new CPU_RAM_4MB(0); // We allocate 1 chunk at the beginning itself.
    RAMChunksAllocatedCount = 1; // Even on 16 TB system, this count will not grow to more than 2^22. (16TB/4MB)
    activeChunkIndex = 0;

    std::lock_guard<std::mutex> lock(mutex);
    std::cout << "Memory Manager Initialized." << std::endl;
}

/* For our memory management system, since we're dealing with large chunks of RAM and is a core part of
the application that would run until program termination, we can safely skip the cleanup function.
The OS will handle it. However to satisfy Valgrind, AddressSanitizer, or Visual Studio's diagnostic tools,
and make our life easier catching relevant bugs there, we do the cleanup anyway.*/
inline void राम::CleanupMemorySystem() {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto chunk : RAMChunks) { delete chunk; }
    RAMChunks.clear();
    id2ChunkMap.clear();
    RAMChunksAllocatedCount = 0;
    activeChunkIndex = 0;
}

//void CPURAMManager::AllocateNewObject(META_DATA& metaData, void* data, uint32_t dataSize) {
inline bool राम::AllocateNewObject(const CreateObjectCmd& cmd) {
    std::lock_guard<std::mutex> lock(mutex);
    // Duplicate id will be silently discarded without crashing the application.
    if (id2ChunkMap.count(cmd.desiredId)) return false; // ID already exists

    // WARNING: We do not yet support more than 4MB dataSize. TODO: Develop Multi Chunk data storage logic.

    uint32_t payloadSize = static_cast<uint32_t>(cmd.data.size());
    // Calculate aligned next position. x86_64 (Intel/AMD) is OK, but ARM / RISCV force 8 byte alignment.
    uint32_t totalSpaceNeeded = (sizeof(META_DATA) + payloadSize + 7) & ~7U; // 8-byte alignment

    auto locationOpt = FindSpaceAndAllocate(totalSpaceNeeded);
    if (!locationOpt) return false;

    DATALocation location = *locationOpt;

    // Get pointer to the start of the allocation
    std::byte* destPtr = RAMChunks[location.chunkIndex]->dataBlock + location.offsetInChunk;

    // Construct the header in-place
    META_DATA header;
    header.tempId = cmd.desiredId;
    header.tempIdParent = cmd.parentId; //WARNING: Wrong. copy tempIdParent ?
    header.dataSize = payloadSize;
    header.xxxFileIndex = 0; // Placeholder
    header.dataVersion = 1;
    header.lastProcessedVersion = 0;
    header.isDeleted = false;

    // Now that we have ensured that sufficient space is available, Copy header and payload
    std::memcpy(destPtr, &header, sizeof(META_DATA));
    std::memcpy(destPtr + sizeof(META_DATA), cmd.data.data(), payloadSize);

    //Add the meta-data id to global map for fast retrieval.
    id2ChunkMap[cmd.desiredId] = location;
    return true;
}

inline std::unordered_map<uint64_t, DATALocation> राम::GetIdMapSnapshot() {
    std::lock_guard<std::mutex> lock(mutex);
    return id2ChunkMap;
}

inline std::optional<DATALocation> राम::FindSpaceAndAllocate(uint32_t totalSpaceNeeded) {
    if (totalSpaceNeeded > CPU_RAM_4MB::DATA_BLOCK_SIZE) return std::nullopt; // Object too large

    // --- Search Active Chunk's Free List ---
    // First-fit: find the first block in the free list that is large enough.
    CPU_RAM_4MB* activeChunk = RAMChunks[activeChunkIndex];
    uint32_t foundOffset = 0;

    for (auto it = activeChunk->freeByteRangesList.begin(); it != activeChunk->freeByteRangesList.end(); ++it) {
        if (it->freeBytes >= totalSpaceNeeded) {
            // Found a suitable block.
            foundOffset = it->startOffset;

            // Update the free list entry.
            if (it->freeBytes == totalSpaceNeeded) {
                // The block is used up exactly, so remove it from the list.
                activeChunk->freeByteRangesList.erase(it);
            }
            else {
                // The block is larger than needed, so shrink it from the start.
                it->startOffset += totalSpaceNeeded;
                it->freeBytes -= totalSpaceNeeded;
            }

            // Update chunk metadata and return the location.
            activeChunk->totalFreeSpace -= totalSpaceNeeded;
            activeChunk->totalUsedSpace += totalSpaceNeeded;
            return DATALocation{ activeChunkIndex, foundOffset };
        }
    }

    // --- If no space in active chunk, allocate a new one ---
    // Not enough contiguous space in the active chunk's free list.
    // As per design, we do not search prior chunks. We just allocate a new one.
    activeChunkIndex++;
    if (activeChunkIndex >= RAMChunksAllocatedCount) {
        if (RAMChunksAllocatedCount >= cpuRAMChunkLimit) {
            std::cerr << "WARNING: Exceeding physical RAM soft limit." << std::endl;
            // Allow allocation to continue, but we're now likely swapping to disk.
        }
        if (activeChunkIndex >= RAMChunks.size()) {
            RAMChunks.resize(RAMChunks.size() + 16, nullptr); // Grow vector by 16 slots 
        }
        RAMChunks[activeChunkIndex] = new CPU_RAM_4MB(0);
        RAMChunksAllocatedCount++;
    }
    else if (RAMChunks[activeChunkIndex] == nullptr) {
        // Re-using a slot that was part of the initial vector but not yet allocated
        RAMChunks[activeChunkIndex] = new CPU_RAM_4MB(0);
    }

    // --- Allocate from the newly prepared chunk ---
    // The new chunk is guaranteed to have one large free block at the start of its list.
    CPU_RAM_4MB* newActiveChunk = RAMChunks[activeChunkIndex];
    auto& firstBlock = newActiveChunk->freeByteRangesList.front();
    foundOffset = firstBlock.startOffset;

    // Adjust the single free block in the new chunk.
    firstBlock.startOffset += totalSpaceNeeded;
    firstBlock.freeBytes -= totalSpaceNeeded;

    // Update chunk metadata.
    newActiveChunk->totalFreeSpace -= totalSpaceNeeded;
    newActiveChunk->totalUsedSpace += totalSpaceNeeded;

    return DATALocation{ activeChunkIndex, foundOffset };
}

inline bool राम::ModifyObject(const ModifyObjectCmd& cmd) {
    std::lock_guard<std::mutex> lock(mutex);

    auto it = id2ChunkMap.find(cmd.id);
    if (it == id2ChunkMap.end()) return false; // Object not found

    DATALocation location = it->second;
    CPU_RAM_4MB* chunk = RAMChunks[location.chunkIndex];
    META_DATA* header = reinterpret_cast<META_DATA*>(chunk->dataBlock + location.offsetInChunk);

    uint32_t oldPayloadSize = header->dataSize;
    uint32_t oldTotalSpace = (sizeof(META_DATA) + oldPayloadSize + 7) & ~7U;

    uint32_t newPayloadSize = static_cast<uint32_t>(cmd.newData.size());
    uint32_t newTotalSpace = (sizeof(META_DATA) + newPayloadSize + 7) & ~7U;

    if (newTotalSpace <= oldTotalSpace) {
        // New data fits in the old spot. Overwrite it.
        std::memcpy(reinterpret_cast<std::byte*>(header) + sizeof(META_DATA), cmd.newData.data(), newPayloadSize);
        header->dataSize = newPayloadSize;
        header->dataVersion.fetch_add(1, std::memory_order_relaxed); // Increment version

        // If the new data is smaller, there is leftover space. Add it back to the freelist.
        uint32_t leftoverSpace = oldTotalSpace - newTotalSpace;
        if (leftoverSpace > 64) { // Only add back if the fragment is reasonably large.
            chunk->totalFreeSpace += leftoverSpace;
            chunk->totalUsedSpace -= leftoverSpace;

            // Add the leftover block to the freelist (with merging).
            uint32_t freedOffset = location.offsetInChunk + newTotalSpace;
            auto& freeList = chunk->freeByteRangesList;
            auto insertPos = std::lower_bound(freeList.begin(), freeList.end(), freedOffset,
                [](const FREE_RAM_RANGES& range, uint32_t value) { return range.startOffset < value; });
            auto newBlockIt = freeList.insert(insertPos, { freedOffset, leftoverSpace });

            // Try to merge with previous and next blocks.
            if (newBlockIt != freeList.begin()) {
                auto prevBlockIt = std::prev(newBlockIt);
                if (prevBlockIt->startOffset + prevBlockIt->freeBytes == newBlockIt->startOffset) {
                    prevBlockIt->freeBytes += newBlockIt->freeBytes;
                    newBlockIt = freeList.erase(newBlockIt);
                }
            }
            if (newBlockIt != freeList.end() && std::next(newBlockIt) != freeList.end()) {
                auto nextBlockIt = std::next(newBlockIt);
                if (newBlockIt->startOffset + newBlockIt->freeBytes == nextBlockIt->startOffset) {
                    newBlockIt->freeBytes += nextBlockIt->freeBytes;
                    freeList.erase(nextBlockIt);
                }
            }
        }
    }
    else {
        // New data is larger. We must move the object.
        auto newLocationOpt = FindSpaceAndAllocate(newTotalSpace);
        if (!newLocationOpt) return false; // Out of memory

        DATALocation newLocation = *newLocationOpt;

        // Mark old space as free and add it to the freelist for reuse.
        chunk->totalFreeSpace += oldTotalSpace;
        chunk->totalUsedSpace -= oldTotalSpace;
        uint32_t freedOffset = location.offsetInChunk;
        auto& freeList = chunk->freeByteRangesList;
        auto insertPos = std::lower_bound(freeList.begin(), freeList.end(), freedOffset,
            [](const FREE_RAM_RANGES& range, uint32_t value) { return range.startOffset < value; });
        auto newBlockIt = freeList.insert(insertPos, { freedOffset, oldTotalSpace });

        // Try to merge with previous and next blocks.
        if (newBlockIt != freeList.begin()) {
            auto prevBlockIt = std::prev(newBlockIt);
            if (prevBlockIt->startOffset + prevBlockIt->freeBytes == newBlockIt->startOffset) {
                prevBlockIt->freeBytes += newBlockIt->freeBytes;
                newBlockIt = freeList.erase(newBlockIt);
            }
        }
        if (newBlockIt != freeList.end() && std::next(newBlockIt) != freeList.end()) {
            auto nextBlockIt = std::next(newBlockIt);
            if (newBlockIt->startOffset + newBlockIt->freeBytes == nextBlockIt->startOffset) {
                newBlockIt->freeBytes += nextBlockIt->freeBytes;
                freeList.erase(nextBlockIt);
            }
        }

        // Copy data to the new location
        std::byte* destPtr = RAMChunks[newLocation.chunkIndex]->dataBlock + newLocation.offsetInChunk;

        META_DATA newHeader;
        newHeader.tempId = header->tempId;
        newHeader.tempIdParent = header->tempIdParent;
        newHeader.xxxFileIndex = header->xxxFileIndex;
        newHeader.dataType = header->dataType;
        newHeader.lastProcessedVersion = header->lastProcessedVersion;
        newHeader.isDeleted = header->isDeleted;
        // reserved fields are not copied as they are unused
        // Atomically load the version from the old header and store it in the new one.
        newHeader.dataVersion.store(header->dataVersion.load(std::memory_order_relaxed));
        newHeader.dataSize = newPayloadSize;// Now, update the fields for the new object state
        newHeader.dataVersion.fetch_add(1, std::memory_order_relaxed); // Increment version for the modification
        // Copy the new header and payload data to the destination
        std::memcpy(destPtr, &newHeader, sizeof(META_DATA));
        std::memcpy(destPtr + sizeof(META_DATA), cmd.newData.data(), newPayloadSize);

        // Update the map to point to the new location
        it->second = newLocation;
    }
    return true;
}

inline bool राम::DeleteObject(uint64_t id) {
    //Find the chunkIndex using id2ChunkMap.
    //Update Relevant details in RAMChunks and mark for deletion. Delegated till GPU link is freed.
    //Remove id from id2ChunkMap.
    std::lock_guard<std::mutex> lock(mutex);

    auto it = id2ChunkMap.find(id);
    if (it == id2ChunkMap.end()) return false;

    DATALocation location = it->second;
    CPU_RAM_4MB* chunk = RAMChunks[location.chunkIndex];
    META_DATA* header = reinterpret_cast<META_DATA*>(chunk->dataBlock + location.offsetInChunk);

    // Soft delete: just mark the flag. The memory is now considered "free" but fragmented.
    // De-fragmentation would reclaim it.
    header->isDeleted = true;
    header->dataVersion.fetch_add(1, std::memory_order_relaxed); // Signal change to other systems

    uint32_t totalSpace = (sizeof(META_DATA) + header->dataSize + 7) & ~7U;
    chunk->totalFreeSpace += totalSpace;
    chunk->totalUsedSpace -= totalSpace;

    // Add the freed space back to the chunk's free list, maintaining sorted order and merging.
    auto& freeList = chunk->freeByteRangesList;
    uint32_t freedOffset = location.offsetInChunk;

    // Find the correct position to insert the new free block to maintain sorted order by offset.
    auto insertPos = std::lower_bound(freeList.begin(), freeList.end(), freedOffset,
        [](const FREE_RAM_RANGES& range, uint32_t value) {
            return range.startOffset < value;
        });

    auto newBlockIt = freeList.insert(insertPos, { freedOffset, totalSpace });

    // Try to merge with the previous block.
    if (newBlockIt != freeList.begin()) {
        auto prevBlockIt = std::prev(newBlockIt);
        if (prevBlockIt->startOffset + prevBlockIt->freeBytes == newBlockIt->startOffset) {
            prevBlockIt->freeBytes += newBlockIt->freeBytes;
            // Erase the current block (which has been merged into the previous one)
            // and update the iterator to point to the merged block.
            newBlockIt = freeList.erase(newBlockIt);
            newBlockIt--; // Point to the merged block (previous one)
        }
    }

    // Try to merge with the next block.
    if (newBlockIt != freeList.end() && std::next(newBlockIt) != freeList.end()) {
        auto nextBlockIt = std::next(newBlockIt);
        if (newBlockIt->startOffset + newBlockIt->freeBytes == nextBlockIt->startOffset) {
            newBlockIt->freeBytes += nextBlockIt->freeBytes;
            freeList.erase(nextBlockIt);
        }
    }

    // IMPORTANT: We do NOT remove from id2ChunkMap here. The Main Logic thread needs to see the
    // "isDeleted" flag to command the GPU to free its resources. The object will be fully
    // pruned from the map by the Main Logic thread after it has processed the deletion.

    return true;
}

inline std::optional<META_DATA*> राम::GetMETA_DATA(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = id2ChunkMap.find(id);
    if (it == id2ChunkMap.end()) return std::nullopt;

    DATALocation loc = it->second;
    return reinterpret_cast<META_DATA*>(RAMChunks[loc.chunkIndex]->dataBlock + loc.offsetInChunk);
}

inline std::optional<std::byte*> राम::GetObjectPayload(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = id2ChunkMap.find(id);
    if (it == id2ChunkMap.end()) return std::nullopt;

    DATALocation loc = it->second;
    std::byte* headerPtr = RAMChunks[loc.chunkIndex]->dataBlock + loc.offsetInChunk;
    return headerPtr + sizeof(META_DATA);
}

inline void राम::DefragmentRAMChunks(uint32_t chunkIndex) {
    // Compress RAMChunks to free up CPU RAM. Update id2ChunkMap for all the IDs which have moved.
    // This is a highly complex operation.
    // 1. Lock the chunk to prevent any modifications.
    // 2. Create a new, empty chunk.
    // 3. Iterate through the old chunk, identifying valid (not-deleted) objects.
    // 4. Copy each valid object contiguously into the new chunk.
    // 5. CRITICAL: For each object moved, update its DATALocation in the global id2ChunkMap.
    //    This requires a write lock on the map.
    // 6. Once all objects are moved, swap the new chunk in for the old one and delete the old chunk.
    std::cout << "De-fragmentation for chunk " << chunkIndex << " would run here." << std::endl;
}