// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once
#include <cstdint>
#include <atomic>
#include <vector>
#include <shared_mutex>
#include <unordered_map>

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

/* How one engineering object points at another - a pipe at the nozzle it connects to. Not used
anywhere yet; the only references in the model today are structural - META_DATA::memoryIDContainer
(which container holds it) and META_DATA::memoryIDGenerator (which template produced it).
Stores the ID and never a pointer, because the arena relocates objects and a stale pointer would
address whatever took the block. Resolution is a binary search over the owning tab's directory;
holding the result for one operation is fine, storing it back into a persisted object is not.
Reverse lookup is deliberately absent - back-references become an optional property instead.
Rationale: mv.ramshanker.in/software/id sections 3.3 and 3.6. */
struct ReferenceID {
	//WARNING: Do not add any more field to this struct. This is one of the core data types.
	//It is repeated all over the place, having very high memory implication. Consult Ram Shanker.

	/* memoryID: This is globally unique, across all threads under the current process.
	We need it separate from id2save because user may open 2 same file just copied to different folder.
	So essentially all id2save will be duplicate, hence we can't use that as primary key inside process memory.
	User expect to be able to modify both files (in different tabs) independently of each other.
	Hence every time a file is loaded, all objects inside the file are assigned a temporary,
	unique across process memory space, monotonically increasing, unique memoryID. */
	uint64_t memoryID;

	/* Above variable is temporary. Following 2 are persistent, to be saved to disc.
	realID could be persistent ID or temporary local ID. https://mv.ramshanker.in/software/id */
	uint64_t realID;
	/* Following is for memory size optimization. All files have unique 256 bit identity ID. 
	Storing reference to objects in external file is common use case.
	There is a separate table inside each file mapping this fileReferenceID to actual external file.
	0 value represents reference made to other object inside same file.
	Being 32 bit means, each file can refer up to 4 billion external files! More than sufficient. */
	uint32_t savedFileReference; 
	/* Every time a file is loaded, it is allocated a temporary memory ID.
	Being 32 bit means, our software can load 4 Billion files simultaneously max.
	TODO: Implement an optimization such that common files between tabs are loaded only once. How ?*/
	uint32_t loadedFileReference;
};

/* SUPERSEDED and never used: resolution is a binary search over each tab's own object directory,
not one process-wide map. See mv.ramshanker.in/software/id section 3.5. Delete once that ships. */

//ChatGPT Prompt:  Implement me an efficient mechanism such that I can quickly get pointer to the 
// location of actual data mapped from memoryID. memoryID could grow into trillions in number on
// larger servers. Here is my other codes in id.h file.
class MemoryIDMap {// Highly scalable mapping: memoryID -> data pointer
private:
	// Number of shards (power of 2). More shards = less contention.
	static constexpr size_t NUM_SHARDS = 256;
	struct Shard {
		mutable std::shared_mutex mtx;
		std::unordered_map<uint64_t, char*> table;
	};
	inline static std::vector<Shard> shards = [] {
		return std::vector<Shard>(NUM_SHARDS);
		}();
	static Shard& getShard(uint64_t id) {
		return shards[id & (NUM_SHARDS - 1)]; // cheap modulo (since NUM_SHARDS is power of 2)
	}

public:
	static void set(uint64_t memoryID, char* ptr) {// Insert or update mapping
		Shard& shard = getShard(memoryID);
		std::unique_lock lock(shard.mtx);
		shard.table[memoryID] = ptr;
	}
	static char* get(uint64_t memoryID) {// Retrieve pointer, nullptr if not found
		Shard& shard = getShard(memoryID);
		std::shared_lock lock(shard.mtx);
		auto it = shard.table.find(memoryID);
		return (it != shard.table.end()) ? it->second : nullptr;
	}
	static void erase(uint64_t memoryID) {// Remove mapping
		Shard& shard = getShard(memoryID);
		std::unique_lock lock(shard.mtx);
		shard.table.erase(memoryID);
	}
};

//TODO: Above code has tremendous scope of improvement. We need to benchmark it under real world load.
