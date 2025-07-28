// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#include "CPURAM-Manager.h"

void राम::InitializeMemorySystem() { // Initialize the system - should be called once at startup
    if (!RAMChunks.empty()) return; // Already initialized
    physicalRAMInstalled = 8ULL * 1024 * 1024 * 1024; // 8 GB
    cpuRAMChunkLimit = static_cast<uint32_t>(physicalRAMInstalled / sizeof(CPU_RAM_4MB));

    RAMChunks.assign(cpuRAMChunkLimit, nullptr); // Overhead: 4GB RAM=>8*1024=8KB , 4TB Server=8*1024*1024=8MB : Negligible.
    //RAMChunks.reserve(cpuRAMChunkLimit, nullptr); 
    RAMChunks[0] = new CPU_RAM_4MB(); // We allocate 1 chunk at the beginning itself.
    RAMChunksAllocatedCount = 1; // Even on 16 TB system, this count will not grow to more than 2^22. (16TB/4MB)
    activeChunkIndex = 0;

    std::lock_guard<std::mutex> lock(mutex);
    std::cout << "Memory Manager Initialized." << std::endl;
}

/* For our memory management system, since we're dealing with large chunks of RAM and is a core part of
the application that would run until program termination, we can safely skip the cleanup function.
The OS will handle it. However to satisfy Valgrind, AddressSanitizer, or Visual Studio's diagnostic tools,
and make our life easier catching relevant bugs there, we do the cleanup anyway.*/
void राम::CleanupMemorySystem() {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto chunk : RAMChunks) { delete chunk; }
    RAMChunks.clear();
    id2ChunkMap.clear();
    RAMChunksAllocatedCount = 0;
    activeChunkIndex = 0;
}

//void CPURAMManager::AllocateNewObject(META_DATA& metaData, void* data, uint32_t dataSize) {
bool राम::AllocateNewObject(const CreateObjectCmd& cmd) {
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
    header.id = cmd.desiredId;
    header.parentId = cmd.parentId;
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

std::unordered_map<uint64_t, DATALocation> राम::GetIdMapSnapshot() {
    std::lock_guard<std::mutex> lock(mutex);
    return id2ChunkMap;
}

std::optional<DATALocation> राम::FindSpaceAndAllocate(uint32_t totalSpaceNeeded) {
    if (totalSpaceNeeded > CPU_RAM_4MB::DATA_BLOCK_SIZE) return std::nullopt; // Object too large

    if (RAMChunks[activeChunkIndex]->newDataSpace < totalSpaceNeeded) {
        // Not enough contiguous space at the end of the active chunk.
        // TODO: Could search prior chunks for free space (from deletions). For now, just allocate a new one.

        activeChunkIndex++;
        if (activeChunkIndex >= RAMChunksAllocatedCount) {
            if (RAMChunksAllocatedCount >= cpuRAMChunkLimit) {
                std::cerr << "WARNING: Exceeding physical RAM soft limit." << std::endl;
                // Allow allocation to continue, but we're now likely swapping to disk.
            }
            if (RAMChunksAllocatedCount >= RAMChunks.size()) {
                RAMChunks.resize(RAMChunks.size() + 16, nullptr); // Grow vector by 16 slots 
                cpuRAMChunkLimit += 16; // and update limit.
            }
            RAMChunks[activeChunkIndex] = new CPU_RAM_4MB();
            RAMChunksAllocatedCount++;
        }
        else if (RAMChunks[activeChunkIndex] == nullptr) {
            // Re-using a slot that was part of the initial vector but not yet allocated
            RAMChunks[activeChunkIndex] = new CPU_RAM_4MB();
        }
    }

    // We have a chunk with enough space at the end
    uint32_t offset = static_cast<uint32_t>(RAMChunks[activeChunkIndex]->nextDataLocation
        - RAMChunks[activeChunkIndex]->dataBlock);

    // Update chunk metadata
    RAMChunks[activeChunkIndex]->nextDataLocation += totalSpaceNeeded;
    RAMChunks[activeChunkIndex]->newDataSpace -= totalSpaceNeeded;
    RAMChunks[activeChunkIndex]->totalFreeSpace -= totalSpaceNeeded;

    return DATALocation{ activeChunkIndex, offset };
}

bool राम::ModifyObject(const ModifyObjectCmd& cmd) {
    std::lock_guard<std::mutex> lock(mutex);

    auto it = id2ChunkMap.find(cmd.id);
    if (it == id2ChunkMap.end()) return false; // Object not found

    DATALocation location = it->second;
    META_DATA* header = reinterpret_cast<META_DATA*>(RAMChunks[location.chunkIndex]->dataBlock + location.offsetInChunk);

    uint32_t oldPayloadSize = header->dataSize;
    uint32_t oldTotalSpace = (sizeof(META_DATA) + oldPayloadSize + 7) & ~7U;

    uint32_t newPayloadSize = static_cast<uint32_t>(cmd.newData.size());
    uint32_t newTotalSpace = (sizeof(META_DATA) + newPayloadSize + 7) & ~7U;

    if (newTotalSpace <= oldTotalSpace) {
        // New data fits in the old spot. Overwrite it.
        std::memcpy(reinterpret_cast<std::byte*>(header) + sizeof(META_DATA), cmd.newData.data(), newPayloadSize);
        header->dataSize = newPayloadSize;
        header->dataVersion.fetch_add(1, std::memory_order_relaxed); // Increment version
    }
    else {
        // New data is larger. We must move the object.
        auto newLocationOpt = FindSpaceAndAllocate(newTotalSpace);
        if (!newLocationOpt) return false; // Out of memory

        DATALocation newLocation = *newLocationOpt;

        // Mark old space as free
        RAMChunks[location.chunkIndex]->totalFreeSpace += oldTotalSpace;
        // In a real system, add this free block to a freelist for reuse.

        // Copy data to the new location
        std::byte* destPtr = RAMChunks[newLocation.chunkIndex]->dataBlock + newLocation.offsetInChunk;

        META_DATA newHeader;
        newHeader.id = header->id;
        newHeader.parentId = header->parentId;
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

bool राम::DeleteObject(uint64_t id) {
    //Find the chunkIndex using id2ChunkMap.
    //Update Relevant details in RAMChunks and mark for deletion. Delegated till GPU link is freeded.
    //Remove id from id2ChunkMap.
    std::lock_guard<std::mutex> lock(mutex);

    auto it = id2ChunkMap.find(id);
    if (it == id2ChunkMap.end()) return false;

    DATALocation location = it->second;
    META_DATA* header = reinterpret_cast<META_DATA*>(RAMChunks[location.chunkIndex]->dataBlock + location.offsetInChunk);

    // Soft delete: just mark the flag. The memory is now considered "free" but fragmented.
    // De-fragmentation would reclaim it.
    header->isDeleted = true;
    header->dataVersion.fetch_add(1, std::memory_order_relaxed); // Signal change to other systems

    uint32_t totalSpace = (sizeof(META_DATA) + header->dataSize + 7) & ~7U;
    RAMChunks[location.chunkIndex]->totalFreeSpace += totalSpace;

    // IMPORTANT: We do NOT remove from id2ChunkMap here. The Main Logic thread needs to see the
    // "isDeleted" flag to command the GPU to free its resources. The object will be fully
    // pruned from the map by the Main Logic thread after it has processed the deletion.

    return true;
}

std::optional<META_DATA*> राम::GetMETA_DATA(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = id2ChunkMap.find(id);
    if (it == id2ChunkMap.end()) return std::nullopt;

    DATALocation loc = it->second;
    return reinterpret_cast<META_DATA*>(RAMChunks[loc.chunkIndex]->dataBlock + loc.offsetInChunk);
}

std::optional<std::byte*> राम::GetObjectPayload(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = id2ChunkMap.find(id);
    if (it == id2ChunkMap.end()) return std::nullopt;

    DATALocation loc = it->second;
    std::byte* headerPtr = RAMChunks[loc.chunkIndex]->dataBlock + loc.offsetInChunk;
    return headerPtr + sizeof(META_DATA);
}

void राम::DefragmentRAMChunks(uint32_t chunkIndex) {
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