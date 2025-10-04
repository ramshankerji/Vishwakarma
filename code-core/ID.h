// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once
#include <cstdint>
#include <atomic>
#include <vector>
#include <shared_mutex>
#include <unordered_map>

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
};

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
	/* Following is difference the location where the referred object is actually loaded in memory.
	null pointer indicates that referred value has not been loaded to memory. Or it is missing.*/
	char* data; //48 bits. 2^48Bytes = 256TB
	
	/* Above 2 variables are temporary. Following 2 are persistent, to be saved to disc.
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
	uint32_t loadedFileReferene;
};

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
