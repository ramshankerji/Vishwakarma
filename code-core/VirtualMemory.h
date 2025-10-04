// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once

#include <cstddef>
#include <iostream>

// Platform-specific includes
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace VirtualMemory {

    /**
     * @brief Reserves a large contiguous block of virtual address space.
     * This function only reserves the address range; it does not commit any physical memory.
     * @param size The total number of bytes to reserve.
     * @return A pointer to the base of the reserved address space, or nullptr on failure.
     */
    inline void* reserve_address_space(size_t size) {
        void* ptr = nullptr;
#ifdef _WIN32
        ptr = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
#else
        // MAP_PRIVATE | MAP_ANONYMOUS creates a private mapping not backed by a file.
        // PROT_NONE makes the pages inaccessible until we commit them.
        ptr = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) {
            ptr = nullptr;
        }
#endif
        if (!ptr) {
            std::cerr << "Failed to reserve virtual address space!" << std::endl;
        }
        return ptr;
    }

    /**
     * @brief Commits physical memory for a portion of a reserved address space.
     * @param address A pointer within a previously reserved block.
     * @param size The number of bytes to commit.
     * @return true on success, false on failure.
     */
    inline bool commit_memory(void* address, size_t size) {
#ifdef _WIN32
        return VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
        // Change protection to allow read/write access, which implicitly commits the pages.
        return mprotect(address, size, PROT_READ | PROT_WRITE) == 0;
#endif
    }

    /**
     * @brief Decommits physical memory, returning it to the OS.
     * The virtual address space remains reserved.
     * @param address The base address of the memory block to decommit.
     * @param size The size of the block.
     */
    inline void decommit_memory(void* address, size_t size) {
#ifdef _WIN32
        VirtualFree(address, size, MEM_DECOMMIT);
#else
        // To "decommit", we can advise the kernel we don't need the pages.
        madvise(address, size, MADV_DONTNEED);
        // And reset protection to no access.
        mprotect(address, size, PROT_NONE);
#endif
    }

    /**
     * @brief Releases an entire reserved virtual address space back to the OS.
     * @param address The base address of the reserved space.
     * @param size The total size of the reserved space (must match the original reservation).
     */
    inline void release_address_space(void* address, size_t size) {
#ifdef _WIN32
        // For reserved memory, size must be 0 and type must be MEM_RELEASE.
        VirtualFree(address, 0, MEM_RELEASE);
#else
        munmap(address, size);
#endif
    }

} // namespace VirtualMemory
