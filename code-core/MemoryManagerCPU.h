// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

/* Why our own memory allocator? Ask this question to any AI Chat: 
When I create multiple pointers in c++ and assign to it the location of memory allocated using "new" operator, 
than there must be something in the c/c++ runtime maintaining how much memory is allocated and where it is located,
so that when free is called, appropriate memory is released?
Is this memory allocator managing the stack / bulk memory requested from the OS?
This information also seems to be necessary to de-fragment this latter.
If the answer is yes, than I would like to know what is the memory overhead of tracking all the dynamic memory allocation.

Here is the few important details I found: 
1. When you call >> int* p = new int[20]; << The C++ runtime allocates a block of memory large enough to hold 20 integers,
plus some overhead for managing that memory immediately before the block. This preceding header is overhead.
2. Overhead Size: glibc malloc: 16 bytes on 64 bit systems, Windows HeapAlloc: 8-16 bytes, jemalloc, tcmalloc: 8-16 bytes,
Debug Builds: 32 to 64 bytes ! So in general, 16 bytes is a good estimate for overhead per allocation.
3. The inbuilt C++ allocator (std::allocator) can't do memory defragmentation, otherwise the row pointers would get invalidated. 
Overtime, this leads to fragmentation, where free memory is split into small chunks that can't be used for larger allocations.
4. True defragmentation (compaction) requires an indirection layer (e.g., handles instead of raw pointers) — 
which is exactly the kind of thing a custom arena could give you.

Maximum RAM on a single motherboard: (as of 2025) : Hence the maximum RAM we support is 32 TB in a single system !
AMD : 2 (2P) x 12 (Channel) x 2 (DIMM/channel) x 512 GB (DDR5 RAM Stick) = 24576 GB (24 TB) :
https://www.amd.com/en/products/processors/server/epyc/9005-series/amd-epyc-9965.html
Intel : 8 (8P) x 4 TB = 32 TB : 
https://www.intel.com/content/www/us/en/products/sku/241837/intel-xeon-6788p-processor-336m-cache-2-00-ghz/specifications.html

Goal of our In-Memory data structure:
The data set being handled by the application will have classes of approx. 1000 to 5000 unique types.
Each class type will have 10 to 50 fields. 
Certain mandatory fields (members) and around 2/3rd being optional values, some of which could be utf-8 string.
These optional values could be set or unset. 
Design a memory data structure / algorithmic approach such that least amount of space is wasted storing unneeded fields,
with flexibility to change the field values as and when required by the user. 
I will be persisting these classes using their protocol buffer representations. Each object shall be identified by a unique 64bit ID.
I also need to be able to quickly retrieve any particular ID, or confirm it's absence. 
Not afraid to use state-of-art industry standards algorithms. Could even consider small string optimizations etc.

We will not use std::optional<T> because It will consume min. 8 Bytes even if "T" is not present.
I also want to quickly access all objects of a particular class having another field equal to say "2"? 
It will be like in-memory indexing over the table.
We will have index only over certain fields. For others, we will do a linear scan.

Additional Info: We do not use void*, but rather use modern std::byte*.
*/
#pragma once
#include <cstdint>
#include <cstddef>
#include <iostream>
#include <vector>
#include <cstring> // for memcpy
#include <unordered_map>
#include <mutex>
#include <optional>
#include <algorithm> // Required for std::lower_bound
#include <list> // For freeChunks pool
#include <new>  // Required for placement new
#include <map>
#include <thread>
#include <numeric>

#include "VirtualMemory.h"

// Total virtual space to reserve. 16 TB is a reasonable amount for a 64-bit app.
constexpr uint64_t TOTAL_RESERVED_SPACE = 16ULL * 1024 * 1024 * 1024 * 1024;
constexpr uint64_t SMALL_ALLOCATOR_CHUNK_SIZE = 4 * 1024 * 1024; // 4MB. Size of small allocator chunks.
// The threshold for an allocation to be considered "large".
constexpr uint64_t LARGE_ALLOC_THRESHOLD = SMALL_ALLOCATOR_CHUNK_SIZE / 4; // e.g., 1 MB
// A thread_local variable to hold the ID of the current tab/thread.
// This is crucial for the memory manager to associate allocations with the correct tab.
//uint32_t memoryGroupNo;

/* We choose 4 MB as the chunk size due to following 3 considerations:
1. Operating system pages are typically 4 KB, usually OS is able to provide 1024 contiguous memory located pages. Hence 4 MB is a good size.
2. For any memory manager, reclaiming memory is a costly operation. We want our defragmentation to be efficient. 
4 MB is a good size for defragmentation.
3. What if user opens all 500 odd files in a directory in a singly click ( Hint: Ram Shanker does this often in projects, 
while studying/searching drawings of entire unit).If these files are small enough, than we allocate 500 * 4 MB = 2 GB of RAM. 
Still manageable in most of the systems. Note that each chunk is exclusive to a particular tab.
*/

struct FREE_RAM_RANGES {
    // Simple struct to track all the free spaces in the RAM Chunks. RAM Chunks are also knows as arena.
    uint32_t startOffset = 0;
    uint32_t freeBytes = 0;
    //For our 4MB RAM Chunks, above variable will be maximum 2^22 only. Which can be accommodated in 3 Bytes ( 2^24 ). 
    // We have 10x2=20 bits margin here for additional info if we implement bit packing in future.
};

// The whole purpose of defining the following struct is that we can accurately calculate it's size to deduct from 4MB.
struct CHUNK_METADATA { // Should not be more than 4KB in size.
    uint32_t memoryGroupNo = 0; // The owner of the current chunk.
    uint32_t totalFreeSpace = 0; //TODO: To be implemented.
    uint32_t maxContiguousFreeSpace = 0; // Not being used due to performance penalty of keeping it updated.
    uint32_t freeByteRangesCount = 0;
    uint32_t currentFreeListIndex = 0;

    /*Each chunk has its own mutex to allow for concurrent allocations across different chunks
    without blocking on a single global mutex. NOTE: std::mutex is non-copyable and non-movable. This is safe here because
    CPU_RAM_4MB chunks are constructed in-place with placement new and are never moved or copied.
    During defragmentation, elements will be copied manually, by creating a new mutex in destination Object. */
    std::mutex chunkMutex;

    static const uint32_t freeByteRangesMaxSize = 498;
    FREE_RAM_RANGES freeByteRangesList[freeByteRangesMaxSize]; //Track the free space within the 4 MB Range.
    std::byte headerPadding[4]; //Reserve bytes. Deduct from here if new variable required inside this struct.
};
static_assert(sizeof(CHUNK_METADATA) == 4096, "CHUNK_METADATA must be exactly 4KB (4096 bytes)");
struct CPU_RAM_4MB : CHUNK_METADATA {
    // Inheriting CHUNK_METADATA, causes all it's member to be present inside this class as well.
    static const uint32_t DATA_BLOCK_SIZE = 4194304 - 4096; // (2^22-2^12) i.e. (4MB - 4KB)
    alignas(16) std::byte dataBytes[DATA_BLOCK_SIZE];  //Our THE data range.

    CPU_RAM_4MB(uint32_t tabNo) {reset(tabNo);}// Constructor Function.

    // Resets the chunk to a pristine state for reuse. Initialize all the fields.
    // This function should only be called when exclusive access to the chunk is guaranteed
    // (e.g., when it's being recycled by the main manager, which holds a global lock).
    void reset(uint32_t tabNo) {
        memoryGroupNo = tabNo;
        totalFreeSpace = DATA_BLOCK_SIZE;
        maxContiguousFreeSpace = DATA_BLOCK_SIZE;
        // The chunk starts with one single free block covering the entire data area.
        // Offsets are relative to the start of dataBytes, so the first block starts at 0.
        freeByteRangesList[0] = {.startOffset = 0, .freeBytes = DATA_BLOCK_SIZE };
        std::memset(dataBytes, 0, sizeof(dataBytes)); // Clear the data block (set all bytes to 0)
        freeByteRangesCount = 1;
        currentFreeListIndex = 0; // Initialize the new index.
    }
    // Finds space, allocates it, and updates the free list. Returns a pointer within dataBytes.
    // This operation is now thread-safe at the chunk level.
	std::byte* Allocate(uint32_t size); //uint32_t because maximum allocation size is 4MB only.
    // Frees a previously allocated block of memory, coalescing with adjacent free blocks.
    void Free(std::byte* ptrToFree);
    
};
static_assert(sizeof(CPU_RAM_4MB) == 4194304, "CPU_RAM_4MB must be exactly 4MB (4194304 bytes)");
inline std::byte* CPU_RAM_4MB::Allocate(uint32_t size) {
    if (size > DATA_BLOCK_SIZE) return nullptr;  // Safety against overflow.
    std::lock_guard<std::mutex> lock(chunkMutex); // Lock only this chunk
    if (freeByteRangesCount == 0) { return nullptr; }
    //Fast Path: First, check the block at the cached index `currentFreeListIndex`.
    if (currentFreeListIndex >= freeByteRangesCount) {
        currentFreeListIndex = 0; // Sanity check: reset if index is out of bounds.
    }


    if (freeByteRangesList[currentFreeListIndex].freeBytes >= size) {
        uint32_t offset = freeByteRangesList[currentFreeListIndex].startOffset;
        if (freeByteRangesList[currentFreeListIndex].freeBytes == size) { // Exact fit
            // Remove this entry by shifting subsequent entries left.
            for (uint32_t j = currentFreeListIndex; j < freeByteRangesCount - 1; ++j) {
                freeByteRangesList[j] = freeByteRangesList[j + 1];
            }
            freeByteRangesCount--;
            // The index now points to the next block, a good candidate for the next allocation.
            // If we removed the last item, wrap around to the beginning.
            if (currentFreeListIndex >= freeByteRangesCount && freeByteRangesCount > 0) {
                currentFreeListIndex = 0;
            }
        }
        else { // Block is larger, so shrink it from the front.
            freeByteRangesList[currentFreeListIndex].startOffset += size;
            freeByteRangesList[currentFreeListIndex].freeBytes -= size;
            // This block is still a good candidate, so we don't change the index.
        }
        totalFreeSpace -= size;
        // TODO: Update maxContiguousFreeSpace if necessary.
        return &dataBytes[offset];
    }

    // Slow Path : If the hinted block was too small, search the entire list.
    for (uint32_t i = 0; i < freeByteRangesCount; ++i) {
        if (freeByteRangesList[i].freeBytes >= size) {
            uint32_t offset = freeByteRangesList[i].startOffset;
            if (freeByteRangesList[i].freeBytes == size) { // Exact fit
                // Remove this entry by shifting subsequent entries left.
                for (uint32_t j = i; j < freeByteRangesCount - 1; ++j) {
                    freeByteRangesList[j] = freeByteRangesList[j + 1];
                }
                freeByteRangesCount--;
                currentFreeListIndex = i;// Update the hint to the location of the next block.
                if (currentFreeListIndex >= freeByteRangesCount && freeByteRangesCount > 0) {
                    currentFreeListIndex = 0;
                }
                else { // Block is larger, so shrink it.
                    freeByteRangesList[i].startOffset += size;
                    freeByteRangesList[i].freeBytes -= size;
                    currentFreeListIndex = i;// Update the hint** to this block, as it still has free space.
                }
                totalFreeSpace -= size;
                return &dataBytes[offset];
            }
        }
    }
    return nullptr; // No suitable block found.
}

inline void CPU_RAM_4MB::Free(std::byte* ptrToFree) {
    if (ptrToFree == nullptr) return; //Safety in case there is error in upstream logic and we received null to be released.
    std::lock_guard<std::mutex> lock(chunkMutex); // Lock only this chunk
    uint64_t size = reinterpret_cast<uint64_t> (ptrToFree + 8);
    
    uint32_t offset = static_cast<std::byte*>(ptrToFree) - dataBytes;
    // Find the insertion point to keep the list sorted by offset.
    uint32_t insertIndex = 0;
    while (insertIndex < freeByteRangesCount && freeByteRangesList[insertIndex].startOffset < offset) {
        //Currently we are doing a linear scan, however we will optimize it latter to use binary search,
        //since our freeList is in sorted order. This will improve complexity from O(N) to O(logN). Measure and implement.
        insertIndex++;
    }
    // Check if we can merge with the previous block.
    bool mergedWithPrevious = false;
    if (insertIndex > 0 && (freeByteRangesList[insertIndex - 1].startOffset + freeByteRangesList[insertIndex - 1].freeBytes) == offset) {
        mergedWithPrevious = true;
    }
    // Check if we can merge with the next block.
    bool mergedWithNext = false;
    if (insertIndex < freeByteRangesCount && (offset + size) == freeByteRangesList[insertIndex].startOffset) {
        mergedWithNext = true;
    }
    // Apply Merging Strategy 
    if (mergedWithPrevious && mergedWithNext) {
        // Case 1: Merge with both previous and next blocks (3-way merge).
        freeByteRangesList[insertIndex - 1].freeBytes += size + freeByteRangesList[insertIndex].freeBytes;
        // Remove the now-redundant 'next' block by shifting the rest of the list left.
        for (uint32_t j = insertIndex; j < freeByteRangesCount - 1; ++j) {
            freeByteRangesList[j] = freeByteRangesList[j + 1];
        }
        freeByteRangesCount--;
    }
    else if (mergedWithPrevious) {
        // Case 2: Merge with the previous block only.
        freeByteRangesList[insertIndex - 1].freeBytes += size;
    }
    else if (mergedWithNext) {
        // Case 3: Merge with the next block only.
        freeByteRangesList[insertIndex].startOffset = offset;
        freeByteRangesList[insertIndex].freeBytes += size;
    }
    else {
        // Case 4: No coalescing. Insert a new free block into the list.
        if (freeByteRangesCount >= freeByteRangesMaxSize) { // Safety check for list capacity
            // Cannot free, the free list is full. This indicates extreme fragmentation.
            // A real system might trigger defragmentation or log a critical error.
            return;
        }
        // Shift elements to the right to make space for the new entry.
        for (uint32_t j = freeByteRangesCount; j > insertIndex; --j) {
            freeByteRangesList[j] = freeByteRangesList[j - 1];
        }
        freeByteRangesList[insertIndex] = { .startOffset = offset, .freeBytes = (uint32_t)size };
        freeByteRangesCount++;
    }

    totalFreeSpace += size;
}

/* There will be exactly 1 object of this class across the application,
However 1 Chunk belongs to exactly 1 tab. So that when a tab is closed, we can free up it's memory quickly.
This way our defragmentation boundary is also per tab (in addition to per chunk).
भगवान राम की कृपा बानी रहे. Corresponding object is named "cpuRAMManager".
*/
class राम {
public:
    // Global variables with proper initialization
    uint64_t physicalRAMInstalled = 8ULL * 1024 * 1024 * 1024; // 8 GB. TODO: Read system RAM.
    const uint64_t cpuRAMChunkSize = sizeof(CPU_RAM_4MB);
    uint32_t cpuRAMChunkLimit = 0;// static_cast<uint32_t>(physicalRAMInstalled / cpuRAMChunkSize);

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
    std::unordered_map<uint64_t, std::byte*> id2MemoryMap;

    राम(); // The constructor. Initialize the memory sub-system.
    ~राम(); // The destructor.Do all the cleanup.
    // Non-copyable, non-movable singleton
    राम(const राम&) = delete;
    राम& operator=(const राम&) = delete;

    const uint32_t POINTER_OVERHEAD_BYTES = 8; // We store the bytes allocated, just preceding the bytes.
    
    /*Called by the overloaded `new` operator in META_DATA. return A pointer to the allocated memory.*/
    std::byte* Allocate(uint64_t size, uint32_t memoryGroupNo);
    void Free(std::byte* userPtr);//Free the memory allocated at the pointer.
    void notifyTabClosed(uint32_t memoryGroupNo);//De-commits all chunks associated with a closed tab.

    // New Interface. Their success is guaranteed. Calling class must not have error handling logic.
    // Error handling if any to be done by this राम class itself.
    //new_size can be higher or lower than the old size. old_size is already stored before pointer.
    void Reallocate(std::byte* old_loc, uint32_t new_size);
    void DefragmentRAMChunks(uint32_t chunkIndex);

private:
    void* baseAddress = nullptr;
    // Pointers defining the boundaries of the segregated pools
    std::byte* chunkPoolStart = nullptr;
    std::byte* largeBlockPoolStart = nullptr;
    std::byte* endOfReservedSpace = nullptr;
    std::byte* nextLargeAllocPtr = nullptr; // "Bump pointer" for new large allocations
    // Using std::mutex for thread-safety, since multiple threads will be calling this class.
    // It will be used only when either New Chunk is being created, Or when a new Large Allocation is being made. 
    std::mutex globalMemoryAllocationMutex;

    // Small Pool State
    uint64_t nextChunkOffset = 0;// Tracks where to commit the next new chunk
    std::unordered_map<uint32_t, CPU_RAM_4MB*> activeChunks;// Maps memoryGroupNo to its current chunk for allocation
    std::unordered_map<uint32_t, std::vector<CPU_RAM_4MB*>> tabToChunksMap;// All chunks for a tab
    std::list<CPU_RAM_4MB*> freeChunks; // Recycled chunks, i.e. Chunks that are committed but not in use
    // Large Pool State
    std::map<std::byte*, uint64_t> largeFreeBlocks; // Maps address -> size for coalescing
    std::unordered_map<std::byte*, uint64_t> largeAllocatedBlocks; // Maps ptr -> size for freeing

    CPU_RAM_4MB* getNewChunkForTab(uint32_t memoryGroupNo);
    std::byte* allocateFromSmallPool(uint64_t size, uint32_t memoryGroupNo);
    std::byte* allocateFromLargePool(uint64_t size, uint32_t memoryGroupNo);
    void freeInLargePool(std::byte* ptr);

    uint32_t RAMChunksAllocatedCount = 0; // Just a tracker. Whenever we reach up-to cpuRAMChunkCount, we soft-warn users.
    uint32_t activeChunkIndex = 0; //TODO: Move to tab scope, when tabs are implemented.
};

inline राम::राम() { // Initialize the system - should be called once at startup
    //Initializes the memory manager by reserving the virtual address space.
    std::lock_guard<std::mutex> lock(globalMemoryAllocationMutex);
    if (baseAddress != nullptr) return;  // Already initialized

    baseAddress = VirtualMemory::reserve_address_space(TOTAL_RESERVED_SPACE);
    if (baseAddress == nullptr) {
        throw std::runtime_error("Fatal: Could not reserve virtual address space.");
    }

    // Correctly initialize pool boundaries *after* baseAddress is valid.
    chunkPoolStart = static_cast<std::byte*>(baseAddress);
    largeBlockPoolStart = chunkPoolStart + (TOTAL_RESERVED_SPACE / 2);
    endOfReservedSpace = chunkPoolStart + TOTAL_RESERVED_SPACE;
    nextLargeAllocPtr = largeBlockPoolStart;

    physicalRAMInstalled = 8ULL * 1024 * 1024 * 1024; // TODO: Implement actual system RAM detection.
    cpuRAMChunkLimit = static_cast<uint32_t>(physicalRAMInstalled / sizeof(CPU_RAM_4MB));
  
    // TODO: We allocate 1 chunk at the beginning itself.
    RAMChunksAllocatedCount = 1; // Even on 16 TB system, this count will not grow to more than 2^22. (16TB/4MB)
    activeChunkIndex = 0;

    std::cout << "Memory Manager Initialized. Reserved " << TOTAL_RESERVED_SPACE / (1024ULL * 1024 * 1024) <<
        " GB of virtual space." << std::endl;
}

/* For our memory management system, since we're dealing with large chunks of RAM and is a core part of
the application that would run until program termination, we can safely skip the cleanup function.
The OS will handle it. However to satisfy Valgrind, AddressSanitizer, or Visual Studio's diagnostic tools,
and make our life easier catching relevant bugs there, we do the cleanup anyway.*/
inline राम::~राम() {
    std::lock_guard<std::mutex> lock(globalMemoryAllocationMutex);
    if (baseAddress == nullptr) return;

    VirtualMemory::release_address_space(baseAddress, TOTAL_RESERVED_SPACE);
    baseAddress = nullptr;

    id2MemoryMap.clear();
    RAMChunksAllocatedCount = 0;
    activeChunkIndex = 0;
    activeChunks.clear();
    tabToChunksMap.clear();
    freeChunks.clear();
    largeFreeBlocks.clear();
    largeAllocatedBlocks.clear();
    std::cout << "Memory Manager Shut Down. Released virtual address space." << std::endl;
}

inline std::byte* राम::Allocate(uint64_t size, uint32_t memoryGroupNo) {
    if (size == 0) return nullptr; //TODO: Log Memory Dump and Abort Application !!
    // Total size needed includes our pointer-metadata header
    uint64_t totalSize = POINTER_OVERHEAD_BYTES + size; //memory allocation metadata.

    std::byte* ptr = nullptr;
    if (totalSize <= LARGE_ALLOC_THRESHOLD) {
        ptr = allocateFromSmallPool(totalSize, memoryGroupNo);
    }
    else {
        ptr = allocateFromLargePool(totalSize, memoryGroupNo);
    }

    if (!ptr) {
        throw std::bad_alloc();
    }

    *reinterpret_cast<uint64_t*>(ptr) = size;// Store the original requested size in the header.
    // Return pointer to the area *after* our header, which is what the user gets.
    return ptr + POINTER_OVERHEAD_BYTES;
}

inline void राम::Free(std::byte* userPtr) {
    if (!userPtr) return; // Safety against null pointers.
    // Get the actual base pointer by moving back to find our header.
    std::byte* actualPtr = static_cast<std::byte*>(userPtr) - POINTER_OVERHEAD_BYTES;
    // Validate pointer is within our managed range
    if (actualPtr < chunkPoolStart || actualPtr >= endOfReservedSpace) {
        std::cerr << "Error: Pointer not managed by this allocator" << std::endl;
        return;
    }

    std::lock_guard<std::mutex> lock(globalMemoryAllocationMutex);
    // Determine if it was a small or large allocation based on address range.
    if (actualPtr >= chunkPoolStart && actualPtr < largeBlockPoolStart) {
        ptrdiff_t offset_from_start = actualPtr - chunkPoolStart;
        uint64_t chunk_index = offset_from_start / SMALL_ALLOCATOR_CHUNK_SIZE;
        CPU_RAM_4MB* targetChunk = reinterpret_cast<CPU_RAM_4MB*>(chunkPoolStart + chunk_index * SMALL_ALLOCATOR_CHUNK_SIZE);
        uint64_t originalSize = *reinterpret_cast<uint64_t*>(actualPtr);
        uint64_t totalSize = POINTER_OVERHEAD_BYTES + originalSize;
        targetChunk->Free(actualPtr);
    }
    else if (actualPtr >= largeBlockPoolStart && actualPtr < endOfReservedSpace) {
        freeInLargePool(actualPtr);
    }
    else {
        std::cerr << "Warning: Attempting to free memory not managed by this allocator." << std::endl;
    }
}

inline void राम::notifyTabClosed(uint32_t memoryGroupNo) {
    std::lock_guard<std::mutex> lock(globalMemoryAllocationMutex);

    auto it = tabToChunksMap.find(memoryGroupNo);
    if (it != tabToChunksMap.end()) {
        for (CPU_RAM_4MB* chunk : it->second) {
            std::cout << "Tab " << memoryGroupNo << ": Decommitting chunk at " << chunk << std::endl;
            VirtualMemory::decommit_memory(chunk, SMALL_ALLOCATOR_CHUNK_SIZE);
            freeChunks.push_back(chunk); // Add back to the free pool for reuse
        }
        tabToChunksMap.erase(it);
    }
    // A similar loop would be needed for large allocations belonging to the tab.
}

inline CPU_RAM_4MB* राम::getNewChunkForTab(uint32_t memoryGroupNo) {
    // Assumes globalMemoryAllocationMutex is already held
    CPU_RAM_4MB* newChunk = nullptr;
    if (!freeChunks.empty()) {
        // Reuse a previously de-committed chunk
        newChunk = freeChunks.front();
        freeChunks.pop_front();
        newChunk->reset(memoryGroupNo); // Reset its metadata

        if (!VirtualMemory::commit_memory(newChunk, SMALL_ALLOCATOR_CHUNK_SIZE)) {
            freeChunks.push_front(newChunk); // Commit failed, put it back
            return nullptr;
        }
    } else { // Allocate a brand new chunk
        if ((chunkPoolStart + nextChunkOffset + SMALL_ALLOCATOR_CHUNK_SIZE) > largeBlockPoolStart) {
            std::cerr << "Error: Out of reserved space for small memory chunks." << std::endl;
            return nullptr;
        }
        void* chunkMem = chunkPoolStart + nextChunkOffset;
        if (!VirtualMemory::commit_memory(chunkMem, SMALL_ALLOCATOR_CHUNK_SIZE)) {
            std::cerr << "Error: Failed to commit new memory chunk." << std::endl;
            return nullptr;
        }
        nextChunkOffset += SMALL_ALLOCATOR_CHUNK_SIZE;
        newChunk = new (chunkMem) CPU_RAM_4MB(memoryGroupNo); // Placement new
    }
    tabToChunksMap[memoryGroupNo].push_back(newChunk);
    std::cout << "Tab " << memoryGroupNo << ": Acquired new chunk at " << newChunk << std::endl;
    return newChunk;
}

//A simple bump allocator for small objects within 4MB chunks.
inline std::byte* राम::allocateFromSmallPool(uint64_t size, uint32_t memoryGroupNo) {
    CPU_RAM_4MB* currentChunk = nullptr;
    /* Fast Path: First, we get a pointer to the current chunk for this tab.
    This read operation needs to be protected by the global lock to prevent race conditions
    where another thread is modifying the activeChunks map.*/
    {// Do not remove this seemingly un-necessary brace. It's to control mutex release.
        std::lock_guard<std::mutex> lock(globalMemoryAllocationMutex);
        auto it = activeChunks.find(memoryGroupNo);
        if (it != activeChunks.end()) { currentChunk = it->second; }
        // If a chunk exists, try to allocate from it. This call will use the CHUNK's
        // own local mutex, avoiding the global lock for the actual allocation attempt.
    } // Lock released here automatically. 

    if (currentChunk) {
        std::byte* ptr = currentChunk->Allocate(size);
        if (ptr) { return ptr;} // Success! The fast path worked.
    }

    // Slow Path: If we are here, it's because either (1) no chunk existed for the tab, or
    // (2) the existing chunk was full. We now need the global lock to modify the chunk list.
    std::lock_guard<std::mutex> lock(globalMemoryAllocationMutex);
    // We must re-fetch the active chunk. Another thread might have created a new one
    // for this tab while we were attempting the allocation on the old, full chunk.
    auto it = activeChunks.find(memoryGroupNo);
    currentChunk = (it != activeChunks.end()) ? it->second : nullptr;
    // Try allocating again from the potentially new active chunk.
    std::byte* ptr = nullptr;
    if (currentChunk) { ptr = currentChunk->Allocate(size); }
    // If allocation *still* fails, it's our job to create a brand new chunk.
    if (ptr == nullptr) {
        currentChunk = getNewChunkForTab(memoryGroupNo);
        if (!currentChunk) {return nullptr;}// Truly out of memory for the small pool.
        activeChunks[memoryGroupNo] = currentChunk;
        // Allocate from our newly created chunk.
        ptr = currentChunk->Allocate(size);
    }
    return ptr;
}

//Allocator for large objects, directly commits memory.
inline std::byte* राम::allocateFromLargePool(uint64_t size, uint32_t memoryGroupNo) {
    std::lock_guard<std::mutex> lock(globalMemoryAllocationMutex);

    // First-fit: Search the free list for a suitable block.
    for (auto it = largeFreeBlocks.begin(); it != largeFreeBlocks.end(); ++it) {
        if (it->second >= size) {
            std::byte* ptr = it->first;
            uint64_t blockSize = it->second;
            largeFreeBlocks.erase(it);
            const uint64_t minSplitSize = 1024; // Avoid creating tiny fragments
            if (blockSize > size + minSplitSize) {
                std::byte* remainderPtr = static_cast<std::byte*>(ptr) + size;
                largeFreeBlocks[remainderPtr] = blockSize - size;
                blockSize = size;
            }
            largeAllocatedBlocks[ptr] = blockSize;
            return ptr;
        }
    }

    // No suitable free block found, so commit new memory.
    uint64_t alignedSize = (size + 15) & ~15; // 16-byte alignment
    if (nextLargeAllocPtr + alignedSize > endOfReservedSpace) {
        return nullptr; // Out of reserved space
    }
    std::byte* ptr = nextLargeAllocPtr;
    if (!VirtualMemory::commit_memory(ptr, alignedSize)) {
        return nullptr;
    }
    nextLargeAllocPtr += alignedSize;
    largeAllocatedBlocks[ptr] = alignedSize;
    std::cout << "Tab " << memoryGroupNo << ": Allocated large block (" << size << " bytes) at " << ptr << std::endl;
    return ptr;
}

//Frees a large allocation by de-committing its memory.
inline void राम::freeInLargePool(std::byte* ptr) {
    std::lock_guard<std::mutex> lock(globalMemoryAllocationMutex);
    // Assumes global lock is held
    auto it = largeAllocatedBlocks.find(ptr);
    if (it != largeAllocatedBlocks.end()) {
        uint64_t size = it->second;
        largeAllocatedBlocks.erase(it);
        // Add the block to the free list and attempt to coalesce (merge) with neighbors.
        auto inserted_it = largeFreeBlocks.emplace(ptr, size).first;
        // Coalesce with the next block
        auto next_it = std::next(inserted_it);
        if (next_it != largeFreeBlocks.end() && static_cast<std::byte*>(ptr) + size == next_it->first) {
            inserted_it->second += next_it->second;
            largeFreeBlocks.erase(next_it);
        }
        // Coalesce with the previous block
        if (inserted_it != largeFreeBlocks.begin()) {
            auto prev_it = std::prev(inserted_it);
            if (static_cast<std::byte*>(prev_it->first) + prev_it->second == ptr) {
                prev_it->second += inserted_it->second;
                largeFreeBlocks.erase(inserted_it);
            }
        }
        std::cout << "Freed and coalesced large block at " << ptr << std::endl;
    }
}

inline void राम::Reallocate(std::byte* old_loc, uint32_t new_size) { /* ... */ }

inline void राम::DefragmentRAMChunks(uint32_t chunkIndex) {
    // Compress RAMChunks to free up CPU RAM. Update id2MemoryMap for all the IDs which have moved.
    // This is a highly complex operation.
    // 1. Lock the chunk to prevent any modifications.
    // 2. Create a new, empty chunk.
    // 3. Iterate through the old chunk, identifying valid (not-deleted) objects.
    // 4. Copy each valid object contiguously into the new chunk.
    // 5. CRITICAL: For each object moved, update its DATALocation in the global id2MemoryMap.
    //    This requires a write lock on the map.
    // 6. Once all objects are moved, swap the new chunk in for the old one and delete the old chunk.
    std::cout << "defragmentation for chunk " << chunkIndex << " would run here." << std::endl;
}

