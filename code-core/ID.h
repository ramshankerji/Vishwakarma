// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once
#include <cstdint>
#include <atomic>
#include <vector>

// --- Constants Defining the ID Space ---
// Using ULL (unsigned long long) suffix to ensure constants are 64-bit.

// The top 16 bits are reserved (0), so the maximum valid ID is 2^48 - 1.
constexpr uint64_t ID_UPPER_BOUND = (1ULL << 48) - 1;

/* The range [0, 2^32) is PERMANENTLY INVALID. Nothing is ever assigned an ID below 2^32, in any
band, which means every valid ID has at least one bit set above the 31st. So if any code ever
truncates a 64 bit ID into a 32 bit integer, the result lands below this floor and fails the
validity check immediately, instead of silently addressing the wrong object. It costs 1/256th of
the catalogue band - see the July 2026 update, point 1, at mv.ramshanker.in/software/id. */
constexpr uint64_t MINIMUM_VALID_ID = (1ULL << 32);

// Range 1: Reserved for Mission Vishwakarma developers/catalogue items.
// Spans from 2^32 up to (but not including) 2^40 - the invalid floor above eats the bottom 2^32.
// Catalog/catalog_editor_v2.py and code-miscellaneous/steel_profile_embedder.py enforce the same
// band on the CSV catalogue; these three must agree.
constexpr uint64_t CATALOGUE_ID_START = MINIMUM_VALID_ID;
constexpr uint64_t CATALOGUE_ID_END = (1ULL << 40) - 1;

// Range 2: For local, temporary use on client machines during offline work.
// Spans from 2^40 up to (but not including) 2^41.
constexpr uint64_t LOCAL_ID_START = (1ULL << 40);
constexpr uint64_t LOCAL_ID_END = (1ULL << 41) - 1;

// Range 3: For permanent IDs assigned by the central server.
// Spans from 2^42 up to the maximum possible ID.
constexpr uint64_t SERVER_ID_START = (1ULL << 42);
// The end of the server range is implicitly ID_UPPER_BOUND.

struct MemoryID {
private:
	/*ensures lock-free, thread-safe increments on most modern CPUs.
	Single global atomic counter for the entire process memory space.
	We intentionally start from 1, so that 0 can be used as a special "null" value. */
	inline static std::atomic<uint64_t> counter{ 1 }; //inline (c++17) allows initialization here.
public: 
	/* Returns the current value. Atomically increments it by 1. memory_order_relaxed is
	sufficient here since we only care about unique monotonically increasing numbers,
	not memory ordering between threads. If hundreds of threads call next(),
	each gets a unique ID.*/
	static uint64_t next() {// fetch_add returns the current value and then increments atomically
		return counter.fetch_add(1, std::memory_order_relaxed);
	}

	/* Two guarantees other code depends on. Rationale: mv.ramshanker.in/software/id section 3.
	1. IDs are NEVER recycled, so a reference to a deleted object fails to resolve rather than
	   silently resolving to whatever object took its place.
	2. IDs are issued in increasing order, so a directory appended to by ONE thread stays sorted
	   and can be binary searched. Per directory only - which is why directories are per tab. */
};
