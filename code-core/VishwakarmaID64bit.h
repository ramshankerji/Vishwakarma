// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

#pragma once

// VishwakarmaId.h
// ID Generation System for Mission Vishwakarma
//


#include <cstdint> // For uint64_t
#include <atomic>  // For std::atomic, ensuring thread-safe local ID generation
#include <stdexcept> // For std::runtime_error
#include <string>    // For exception messages

// All Mission Vishwakarma data structures are placed within this namespace.
namespace vishwakarma {

/**
 * @struct VId
 * @brief A type-safe representation of a Mission Vishwakarma 64-bit Unique ID.
 *
 * Using a struct instead of a raw uint64_t prevents accidental mixing of
 * IDs with other integer types and allows for extending functionality directly
 * on the ID type in the future.
 */
struct VId {
    uint64_t value;

    // Default constructor creates an invalid ID (or a commonly known null ID).
    VId() : value(0) {}

    // Explicit constructor to create an ID from a raw 64-bit integer.
    explicit VId(uint64_t val) : value(val) {}

    // --- Operator Overloads ---
    // These make the VId struct easy to use in standard containers and algorithms.

    bool operator==(const VId& other) const { return value == other.value; }
    bool operator!=(const VId& other) const { return value != other.value; }
    bool operator<(const VId& other) const { return value < other.value; } // For std::map, std::set, etc.
};

// --- Constants Defining the ID Space ---
// Using ULL (unsigned long long) suffix to ensure constants are 64-bit.

// The top 16 bits are reserved (0), so the maximum valid ID is 2^48 - 1.
constexpr uint64_t ID_UPPER_BOUND = (1ULL << 48) - 1;

// Range 1: Reserved for Mission Vishwakarma developers/catalogue items.
// Spans from 0 up to (but not including) 2^40.
constexpr uint64_t CATALOGUE_ID_START = 0ULL;
constexpr uint64_t CATALOGUE_ID_END = (1ULL << 40) - 1;

// Range 2: For local, temporary use on client machines during offline work.
// Spans from 2^40 up to (but not including) 2^41.
constexpr uint64_t LOCAL_ID_START = (1ULL << 40);
constexpr uint64_t LOCAL_ID_END = (1ULL << 41) - 1;

// Range 3: For permanent IDs assigned by the central server.
// Spans from 2^42 up to the maximum possible ID.
constexpr uint64_t SERVER_ID_START = (1ULL << 42);
// The end of the server range is implicitly ID_UPPER_BOUND.

// --- ID Classification Functions ---
// These are small, inline helper functions for performance and to avoid
// linker errors when the header is included in multiple source files.

/**
 * @brief Checks if an ID is a valid Mission Vishwakarma ID.
 * @param id The ID to check.
 * @return True if the ID is within the overall valid range [0, 2^48 - 1].
 */
inline bool IsValid(VId id) {
    return id.value <= ID_UPPER_BOUND;
}

/**
 * @brief Checks if an ID belongs to the developer/catalogue range.
 * @param id The ID to check.
 * @return True if the ID is a catalogue ID.
 */
inline bool IsCatalogueId(VId id) {
    return id.value >= CATALOGUE_ID_START && id.value <= CATALOGUE_ID_END;
}

/**
 * @brief Checks if an ID belongs to the local/temporary range.
 * @param id The ID to check.
 * @return True if the ID is a local ID.
 */
inline bool IsLocalId(VId id) {
    return id.value >= LOCAL_ID_START && id.value <= LOCAL_ID_END;
}

/**
 * @brief Checks if an ID belongs to the server-assigned range.
 * @param id The ID to check.
 * @return True if the ID is a server ID.
 */
inline bool IsServerId(VId id) {
    // Also implicitly ensures it's within the overall valid range by checking the start.
    return id.value >= SERVER_ID_START && id.value <= ID_UPPER_BOUND;
}

/**
 * @class LocalIdGenerator
 * @brief A thread-safe generator for creating new local/temporary IDs.
 *
 * This class is designed to be used as a singleton or a shared instance
 * within an application to generate unique temporary IDs for new objects
 * created on a client machine.
 */
class LocalIdGenerator {
public:
    /**
     * @brief Default constructor. Initializes the generator to the start of the local range.
     */
    LocalIdGenerator() : next_id_(LOCAL_ID_START) {}

    // Disable copy and move semantics to prevent accidental duplication.
    LocalIdGenerator(const LocalIdGenerator&) = delete;
    LocalIdGenerator& operator=(const LocalIdGenerator&) = delete;
    LocalIdGenerator(LocalIdGenerator&&) = delete;
    LocalIdGenerator& operator=(LocalIdGenerator&&) = delete;

    /**
     * @brief Generates a new, unique local ID in a thread-safe manner.
     * @return A new VId from the local range.
     * @throws std::runtime_error if the local ID space is exhausted for this session.
     */
    VId Generate() {
        // Atomically fetch the current value and then add 1 for the next call.
        // std::memory_order_relaxed is sufficient here because we only need atomicity
        // for the counter, not for synchronizing other memory operations.
        uint64_t new_id_value = next_id_.fetch_add(1, std::memory_order_relaxed);

        // Check if we have exhausted the available local ID range. This is a critical
        // failure condition, indicating the application has created over 2^40 (approx. 1 trillion)
        // new local objects in a single session.
        if (new_id_value > LOCAL_ID_END) {
            // We have gone past the end. Revert the counter to prevent overflow
            // and throw an exception to signal the fatal error.
            next_id_.store(LOCAL_ID_END + 1, std::memory_order_relaxed);
            throw std::runtime_error("Local ID range exhausted. Cannot generate new IDs.");
        }

        return VId(new_id_value);
    }

private:
    // Atomic counter to ensure that even in a multi-threaded application,
    // each call to Generate() returns a unique ID.
    std::atomic<uint64_t> next_id_;
};

} // namespace vishwakarma
