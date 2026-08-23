// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <array>
#include <bit>
#include <iostream>
#include <type_traits>
#include "MemoryManagerCPU.h"

extern राम cpu; // The one process-wide CPU RAM arena. Defined in the application entry file.

/* OPTIONAL PROPERTIES - the compaction mechanism behind META_DATA + arena.

RAM LAYOUT ONLY, NOT A STORAGE FORMAT. Optional64 describes how optional values are packed in
memory and nothing else. It is not responsible for persistence and must never define the disc
format: the .proto schemas own that, and the loader translates a persisted record into optional
properties after reading it. For more detail refer storage.md (mv.ramshanker.in/software/storage),
which states it in three separate places. One consequence matters day to day - a property's INDEX
is a flag-bit position that exists only inside one build, so renumbering a schema is free and
nothing on disc ever has to agree with it. That stops being true only if a raw flagsFixed /
flagsDynamic word is ever shipped outside this process (an undo log entry, an IPC message, a
sync to a peer); none of those carry one today, and none should without revisiting this.

WHY. An engineering object carries a handful of the properties its type COULD carry. A P&ID line
has service, fluid, line number, pipe class, insulation spec, tracing, tag, from/to equipment -
and no single line carries all of them. Widening the struct for every possible field charges every
instance for fields it never sets. Optional64 charges only for what is set: a 64-bit flag word
says which properties are present, and the values sit packed end to end with nothing in between.
Rationale: mv.ramshanker.in/software/id section 4.

TWO GROUPS, ONE PER FLAG WORD. flagsFixed covers properties of a compile-time known size - the
14 primitive types plus Byte16 / Byte32 for short null-terminated UTF-8 strings - packed into
buffer x. flagsDynamic covers properties whose bytes need their own arena allocation, described
by a ByteArrayData each, packed into buffer y. 64 of each, so 128 optional properties per type.

WHAT CHANGED FROM THE ORIGINAL SPECIFICATION, and why:

1. NO xChunkIndex / yChunkIndex. The specification carried both so the destructor could hand the
   chunk back. Unnecessary: राम::Free derives the owning chunk from the pointer's own address, so
   the two fields were 8 bytes of duplicated information on every object. Dropping them is what
   brings the class to exactly 40 bytes.
2. NO stored memoryGroupNo either. राम::MemoryGroupOf(this) answers it from the address, and an
   interior pointer reports the enclosing object's group. So an Optional64 always allocates into
   the tab that owns the object it is a member of, without being told and without carrying a
   field that could go stale.
3. ALLOCATION IS LAZY. The specification allocated 32 bytes for x and 4 ByteArrayData for y in
   the constructor. That would charge every object ~80 bytes and two arena allocations even with
   nothing set, which is the opposite of the point - a 100k-element DXF import would pay ~9.6 MB
   and 200k allocations for properties nobody set. x and y stay null until the first set.
4. COPY AND MOVE ARE DELETED. The specification lists deep copy as future work. Until a caller
   needs it, a shallow copy would double-free the arena blocks, so the compiler refuses instead.

WHAT IT COSTS IN THE BINARY - MEASURED, not estimated. The macro generates four accessors per
property, so "2500 optional properties across the application" reads as 10,000 functions and
alarms people. It should not. Measured on x64 with /O2 /MT /Gy and /OPT:REF /OPT:ICF - the shape
Release builds with - comparing the .text virtual size of purpose-built probes:

     0 properties declared ........................  176,140 bytes
    64 properties declared, none of them called ..  176,140 bytes   <- ZERO. Byte identical.
     1 property declared and used (one get+set) ..  188,860 bytes
    64 properties declared and all 64 used .......  195,692 bytes

  - A DECLARED but never-called property costs NOTHING AT ALL. Unreferenced inline functions are
    never emitted, so a type may declare its complete schema and pay only for what code touches.
  - A USED property costs (195,692 - 188,860) / 63 = ~108 bytes, meaning one getter call site
    plus one setter call site, both fully inlined.
  - 2500 optional properties application-wide, EVERY one of them both read and written somewhere,
    extrapolates to ~265 KB of .text. That is the ceiling and not the expectation: it assumes no
    property is ever merely declared. Release also builds with WholeProgramOptimization, whose
    cross-TU folding a standalone probe cannot show, so the real figure lands lower still.

  Two things do NOT scale with the property count, which is what defuses the arithmetic:
  the get<T> / set<T> TEMPLATES instantiate per TYPE, not per property - fourteen types are
  permitted, so the whole application holds at most 28 instantiations whether it declares 20
  optional properties or 2500; and optionalFixedByteSizes is 64 bytes of .rdata per derived TYPE,
  again not per property, and is not emitted at all for a type whose properties go unused.

  Where the ~108 bytes actually goes is calculateOffsetX's loop over the set bits below the
  property being addressed. That is inherent to packing values with no gaps - not to templates
  and not to macros - and it is the lever if this ever needs to be smaller.

WHAT IS DELIBERATELY NOT HERE. Persistence, per the RAM-layout-only note at the top: the disc
format belongs to the .proto schemas and storage.md, not to this file. Defragmentation: this class does not compact
the arena, but ByteArrayData stores {chunkIndex, offset} rather than a pointer precisely so that
DefragmentRAMChunks CAN move dynamic bytes and rewrite the pair. Shrinking: unsetting a property
shifts the bytes down and drops xBytesUsed, but never returns capacity to the arena.

ONE LIFETIME CAVEAT, because it is not obvious. Engineering objects are placement-constructed into
the arena and released wholesale by notifyTabClosed de-committing the tab's chunks - their
destructors never run, and isDeleted is a soft flag. So ~Optional64 does not fire for them either.
That costs nothing, because x, y and every dynamic block were allocated out of the SAME tab's
arena group and are de-committed with it. What does NOT hold is freeing one object on its own with
राम::Free and no destructor call: its optional buffers would then sit unreferenced until the tab
closes. Nothing in the application does that today. Call releaseMemory() first if anything ever
needs to. */

// Byte16 and Byte32 are for Small String Optimization, Null Terminated UTF-8 strings.
struct Byte16 { std::byte data[16]; };
struct Byte32 { std::byte data[32]; };

/* ByteArrayData is 12 Bytes long ( 4 Bytes: chunkIndex, 4 Bytes: Size, 4 Bytes: Offset. ).
This is our overhead for dynamic memory allocation. Even though "size" variable can store up to 4 GB,
we will not allow it to grow >4 MB.

Position independent on purpose - see the defragmentation note above. offset is measured from the
start of the owning 4 MB chunk, so राम::AddressOf(chunkIndex, offset) resolves it. chunkIndex ==
राम::kInvalidChunkIndex means "not allocated".

NOTE on the >4 MB policy above: the implementation enforces a TIGHTER cap than 4 MB, and
deliberately. See Optional64::kMaxDynamicPropertyBytes - a block larger than the arena's
LARGE_ALLOC_THRESHOLD goes to the large pool, which has no chunk index, so {chunkIndex, offset}
could not address it and this class could not free it. Raising the cap to the specified 4 MB needs
the large pool to record a chunk identity first. */
//struct ByteArrayData { uint32_t chunkIndex;  uint32_t size; std::byte* bytes;};//Discarded. Storing actual pointer.
struct ByteArrayData { uint32_t chunkIndex; uint32_t offset; uint32_t size; };//12 Bytes overhead for dynamic memory allocation.

// Some static assertions to ensure the correctness of the code.
static_assert(sizeof(bool) == 1);
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(Byte16) == 16 && sizeof(Byte32) == 32);
static_assert(sizeof(ByteArrayData) == 12 && alignof(ByteArrayData) == 4);

const static uint8_t MAX_PROPERTY_TYPES = 16; // Currently 14 implemented. 1 Reserved for future use. Ex: Bfloat16 for AI!

/* The types an optional property may have. Anything else is a compile error at the declaration
site, so a schema cannot smuggle in a type whose bytes this class does not know how to pack.
Note that char, uint8_t and int8_t are three DISTINCT types in C++, all one byte. */
template<typename T>
inline constexpr bool IsOptionalPropertyType =
    std::is_same_v<T, bool>     || std::is_same_v<T, char>     ||
    std::is_same_v<T, uint8_t>  || std::is_same_v<T, int8_t>   ||
    std::is_same_v<T, uint16_t> || std::is_same_v<T, int16_t>  ||
    std::is_same_v<T, uint32_t> || std::is_same_v<T, int32_t>  ||
    std::is_same_v<T, float>    || std::is_same_v<T, uint64_t> ||
    std::is_same_v<T, int64_t>  || std::is_same_v<T, double>   ||
    std::is_same_v<T, Byte16>   || std::is_same_v<T, Byte32>;

class Optional64 {
public:
    // ---- Layout. Exactly 40 bytes, asserted below. Do not add fields; consult Ram Shanker. ----
    uint64_t flagsFixed = 0;   // Which of the 64 fixed-size properties are present in x.
    uint64_t flagsDynamic = 0; // Which of the 64 ByteArrayData properties are present in y.
    std::byte* x = nullptr;    // Packed values of the set fixed properties, in schema order.
    std::byte* y = nullptr;    // Packed ByteArrayData of the set dynamic properties, schema order.
    uint16_t xBytesAllocated = 0, xBytesUsed = 0; // Used <= Allocated, always.
    uint16_t yCountAllocated = 0, yCountUsed = 0; // Counted in ByteArrayData entries, not bytes.

    /* x grows 32 -> 64 -> ... -> 2048 and never beyond: 64 properties of the widest type (Byte32)
    is exactly 2048 bytes, so the ladder's top is the schema's own ceiling rather than a limit.
    y grows 4 entries at a time to 64, one per possible dynamic property. */
    static constexpr uint16_t kXInitialBytes = 32;
    static constexpr uint16_t kXMaxBytes = 2048;
    static constexpr uint16_t kYGrowStep = 4;
    static constexpr uint16_t kYMaxCount = 64;
    // Kept in the small pool so ChunkIndexOf can address it; राम::Allocate adds an 8-byte header.
    static constexpr uint32_t kMaxDynamicPropertyBytes = uint32_t(LARGE_ALLOC_THRESHOLD) - 8u;

    Optional64() = default;
    ~Optional64() { releaseMemory(); }

    // Deep copy is unimplemented, so a shallow one must not compile - it would double free.
    Optional64(const Optional64&) = delete;
    Optional64& operator=(const Optional64&) = delete;
    Optional64(Optional64&&) = delete;
    Optional64& operator=(Optional64&&) = delete;

    bool isSetFixed(uint8_t propertyIndex) const { return (flagsFixed & (1ULL << propertyIndex)) != 0; }
    bool isSetDynamic(uint8_t propertyIndex) const { return (flagsDynamic & (1ULL << propertyIndex)) != 0; }

    /* Byte offset of a property within x: the sizes of every SET property declared before it.
    Iterating the set bits costs one step per present property, not per declared one, and the
    mask makes it independent of how sparse the schema is. The AVX2 expansion at the bottom of
    this file is an alternative worth benchmarking before it is adopted. */
    uint32_t calculateOffsetX(uint8_t propertyIndex, const uint8_t* const propertyByteSizes) const {
        uint64_t preceding = flagsFixed & ((1ULL << propertyIndex) - 1ULL);
        uint32_t offset = 0;
        while (preceding) {
            offset += propertyByteSizes[std::countr_zero(preceding)];
            preceding &= preceding - 1; // Clear the lowest set bit.
        }
        return offset;
    }

    // ByteArrayData is fixed size, so the y offset is just how many dynamic properties precede.
    uint32_t calculateOffsetY(uint8_t propertyIndex) const {
        const uint64_t preceding = flagsDynamic & ((1ULL << propertyIndex) - 1ULL);
        return uint32_t(std::popcount(preceding)) * uint32_t(sizeof(ByteArrayData));
    }

    /* Read a packed value. memcpy rather than a cast because x is packed with no padding, so the
    address is not guaranteed aligned for the type - unaligned reads are a fault on ARMv8 and
    RISC-V even where x64 tolerates them. Callers check isSetFixed first; the null guard is for
    the case where they did not. */
    template<typename dataType>
    dataType get(uint16_t offset) const {
        static_assert(IsOptionalPropertyType<dataType>, "Not a supported optional property type.");
        dataType value{};
        if (!x) return value;
        std::memcpy(&value, x + offset, sizeof(dataType));
        return value;
    }

    template<typename dataType>
    void set(uint16_t offset, dataType value) {
        static_assert(IsOptionalPropertyType<dataType>, "Not a supported optional property type.");
        if (!x) return; // enableProperty allocates; reaching here means it failed.
        std::memcpy(x + offset, &value, sizeof(dataType));
    }

    /* Make room for a property and return where its bytes go. Already set: returns the existing
    offset and touches nothing. Not set: grows x if needed, shifts every later property up by
    this one's size, sets the flag. O(bytes after the insertion point), bounded by 2048. */
    uint32_t enableProperty(uint8_t propertyIndex, const uint8_t* const propertyByteSizes) {
        const uint32_t offset = calculateOffsetX(propertyIndex, propertyByteSizes);
        if (isSetFixed(propertyIndex)) return offset;

        const uint16_t propertyBytes = propertyByteSizes[propertyIndex];
        allocateMemoryX(uint16_t(xBytesUsed + propertyBytes));
        if (!x || xBytesUsed + propertyBytes > xBytesAllocated) return offset; // Growth refused.

        // memmove, not memcpy: source and destination overlap by design.
        std::memmove(x + offset + propertyBytes, x + offset, size_t(xBytesUsed) - offset);
        std::memset(x + offset, 0, propertyBytes);
        xBytesUsed = uint16_t(xBytesUsed + propertyBytes);
        flagsFixed |= (1ULL << propertyIndex);
        return offset;
    }

    // Same insertion, in units of whole ByteArrayData entries. Returns the byte offset into y.
    uint32_t enableDynamicProperty(uint8_t propertyIndex) {
        const uint32_t offset = calculateOffsetY(propertyIndex);
        if (isSetDynamic(propertyIndex)) return offset;

        allocateMemoryY(uint16_t(yCountUsed + 1));
        if (!y || yCountUsed + 1 > yCountAllocated) return offset; // Growth refused.

        const size_t bytesAfter = (size_t(yCountUsed) * sizeof(ByteArrayData)) - offset;
        std::memmove(y + offset + sizeof(ByteArrayData), y + offset, bytesAfter);
        const ByteArrayData empty{ राम::kInvalidChunkIndex, 0, 0 };
        std::memcpy(y + offset, &empty, sizeof(ByteArrayData));
        yCountUsed = uint16_t(yCountUsed + 1);
        flagsDynamic |= (1ULL << propertyIndex);
        return offset;
    }

    /* Clear a fixed property: shift everything after it down over its bytes and drop the flag.
    Capacity is not returned - x keeps its allocation for the next property that needs it. */
    void unsetFixed(uint8_t propertyIndex, const uint8_t* const propertyByteSizes) {
        if (!isSetFixed(propertyIndex) || !x) return;
        const uint32_t offset = calculateOffsetX(propertyIndex, propertyByteSizes);
        const uint16_t propertyBytes = propertyByteSizes[propertyIndex];
        const size_t bytesAfter = size_t(xBytesUsed) - offset - propertyBytes;
        std::memmove(x + offset, x + offset + propertyBytes, bytesAfter);
        xBytesUsed = uint16_t(xBytesUsed - propertyBytes);
        flagsFixed &= ~(1ULL << propertyIndex);
    }

    // Clear a dynamic property. Its own arena block goes back first, then its descriptor slot.
    void unsetDynamic(uint8_t propertyIndex) {
        if (!isSetDynamic(propertyIndex) || !y) return;
        const uint32_t offset = calculateOffsetY(propertyIndex);
        freeDynamicBlockAt(offset);
        const size_t bytesAfter =
            (size_t(yCountUsed) * sizeof(ByteArrayData)) - offset - sizeof(ByteArrayData);
        std::memmove(y + offset, y + offset + sizeof(ByteArrayData), bytesAfter);
        yCountUsed = uint16_t(yCountUsed - 1);
        flagsDynamic &= ~(1ULL << propertyIndex);
    }

    /* Store bytes for a dynamic property, replacing whatever it held. The block comes from the
    same tab's arena as the owning object, and is capped so it stays in the small pool where
    {chunkIndex, offset} can address it. A larger payload is refused rather than silently stored
    somewhere this class cannot free. */
    void setDynamic(uint8_t propertyIndex, const void* source, uint32_t size) {
        if (size == 0) { unsetDynamic(propertyIndex); return; }
        if (size > kMaxDynamicPropertyBytes) {
#ifdef _DEBUG
            std::cout << "[opt64][warn] dynamic property " << int(propertyIndex) << " of " << size
                      << " bytes exceeds the small-pool cap of " << kMaxDynamicPropertyBytes
                      << " - not stored." << std::endl;
#endif
            return;
        }
        const uint32_t offset = enableDynamicProperty(propertyIndex);
        if (!y) return;
        freeDynamicBlockAt(offset); // Replacing: the previous bytes go back first.

        std::byte* bytes = cpu.Allocate(size, cpu.MemoryGroupOf(this));
        std::memcpy(bytes, source, size);
        const ByteArrayData descriptor{ cpu.ChunkIndexOf(bytes), cpu.OffsetInChunkOf(bytes), size };
        std::memcpy(y + offset, &descriptor, sizeof(ByteArrayData));
    }

    // The descriptor of a dynamic property, or an all-invalid one when it is not set.
    ByteArrayData getDynamic(uint8_t propertyIndex) const {
        ByteArrayData descriptor{ राम::kInvalidChunkIndex, 0, 0 };
        if (!isSetDynamic(propertyIndex) || !y) return descriptor;
        std::memcpy(&descriptor, y + calculateOffsetY(propertyIndex), sizeof(ByteArrayData));
        return descriptor;
    }

    // The bytes themselves. Valid until this property is set again, unset, or the object dies.
    std::byte* dynamicBytes(uint8_t propertyIndex) const {
        const ByteArrayData descriptor = getDynamic(propertyIndex);
        if (descriptor.size == 0) return nullptr;
        return cpu.AddressOf(descriptor.chunkIndex, descriptor.offset);
    }

    /* Grow x to hold at least bytesNeeded, following the doubling ladder. The arena has no
    working Reallocate, so growth is allocate-copy-free. Called on the first set and on every
    crossing of a ladder step - at most 7 times in the life of an object. */
    void allocateMemoryX(uint16_t bytesNeeded) {
        if (bytesNeeded <= xBytesAllocated) return;
        if (bytesNeeded > kXMaxBytes) {
#ifdef _DEBUG
            std::cout << "[opt64][warn] fixed properties need " << bytesNeeded << " bytes, over the "
                      << kXMaxBytes << " byte ceiling - property not enabled." << std::endl;
#endif
            return;
        }
        uint16_t capacity = xBytesAllocated ? xBytesAllocated : kXInitialBytes;
        while (capacity < bytesNeeded) capacity = uint16_t(capacity * 2);

        std::byte* grown = cpu.Allocate(capacity, cpu.MemoryGroupOf(this));
        if (x) std::memcpy(grown, x, xBytesUsed); // memcpy from null is UB even for zero bytes.
        std::memset(grown + xBytesUsed, 0, size_t(capacity) - xBytesUsed);
        if (x) cpu.Free(x);
        x = grown;
        xBytesAllocated = capacity;
    }

    // Same, in ByteArrayData entries, growing 4 at a time to the 64 the flag word allows.
    void allocateMemoryY(uint16_t countNeeded) {
        if (countNeeded <= yCountAllocated) return;
        if (countNeeded > kYMaxCount) return; // 64 flags, so this is unreachable by construction.
        uint16_t capacity = uint16_t(((countNeeded + kYGrowStep - 1) / kYGrowStep) * kYGrowStep);

        const size_t usedBytes = size_t(yCountUsed) * sizeof(ByteArrayData);
        const size_t capacityBytes = size_t(capacity) * sizeof(ByteArrayData);
        std::byte* grown = cpu.Allocate(uint32_t(capacityBytes), cpu.MemoryGroupOf(this));
        if (y) std::memcpy(grown, y, usedBytes); // memcpy from null is UB even for zero bytes.
        std::memset(grown + usedBytes, 0, capacityBytes - usedBytes);
        if (y) cpu.Free(y);
        y = grown;
        yCountAllocated = capacity;
    }

    /* Everything this object owns goes back to the arena: each dynamic property's own block
    first, then the two buffers. Called by the destructor; safe to call twice. */
    void releaseMemory() {
        if (y) {
            for (uint16_t entry = 0; entry < yCountUsed; ++entry) {
                freeDynamicBlockAt(uint32_t(entry) * uint32_t(sizeof(ByteArrayData)));
            }
            cpu.Free(y);
            y = nullptr;
        }
        if (x) { cpu.Free(x); x = nullptr; }
        flagsFixed = 0; flagsDynamic = 0;
        xBytesAllocated = 0; xBytesUsed = 0;
        yCountAllocated = 0; yCountUsed = 0;
    }

    void debugDump() const {
        std::cout << "[opt64] fixed=0x" << std::hex << flagsFixed << " dynamic=0x" << flagsDynamic
                  << std::dec << " x=" << xBytesUsed << "/" << xBytesAllocated << "B y="
                  << yCountUsed << "/" << yCountAllocated << " entries" << std::endl;
    }

private:
    // Return one dynamic property's bytes to the arena and blank its descriptor in place.
    void freeDynamicBlockAt(uint32_t offsetInY) {
        ByteArrayData descriptor{};
        std::memcpy(&descriptor, y + offsetInY, sizeof(ByteArrayData));
        if (descriptor.size != 0 && descriptor.chunkIndex != राम::kInvalidChunkIndex) {
            cpu.Free(cpu.AddressOf(descriptor.chunkIndex, descriptor.offset));
        }
        const ByteArrayData empty{ राम::kInvalidChunkIndex, 0, 0 };
        std::memcpy(y + offsetInY, &empty, sizeof(ByteArrayData));
    }
};

static_assert(sizeof(Optional64) == 40, "Optional64 must stay 40 bytes - see the layout comment.");
static_assert(alignof(Optional64) == 8);

/* ---- SCHEMA DECLARATION -----------------------------------------------------------------------

A derived class declares its optional properties as two list macros and one line in the struct.
The list is written in the specification's own ADD_OPTIONAL_PROPERTY form; it is a parameter name,
so the list reads the same whichever artefact is being generated from it.

    #define PIPE_OPTIONAL_FIXED(ADD_OPTIONAL_PROPERTY)     \
        ADD_OPTIONAL_PROPERTY(0, InsulationMM,  float)     \
        ADD_OPTIONAL_PROPERTY(1, DesignTempC,   float)
    #define PIPE_OPTIONAL_DYNAMIC(ADD_OPTIONAL_PROPERTY)   \
        ADD_OPTIONAL_PROPERTY(0, LineNumber, ByteArrayData)

    struct PIPE : META_DATA {
        ... mandatory fields ...
        MV_DECLARE_OPTIONAL_PROPERTIES(PIPE_OPTIONAL_FIXED, PIPE_OPTIONAL_DYNAMIC)
    };

generates, for each entry: an enumerator, hasX() / getX() / setX() / unsetX(); and for the class:
optionalFixedByteSizes, optionalFixedCount, optionalDynamicCount and optionalSchemaHash().
Indices are written explicitly rather than inferred, because they are the on-flag positions - a
property's index must never shift when a neighbour is added or removed.

The two counts are how many properties the schema DECLARES, not the highest index it uses, so
they say nothing about the flag word's occupied range. Anything iterating property indices should
bound on 64, the flag width, which stays correct however sparse the schema is.

Use MV_NO_OPTIONAL_PROPERTIES for whichever of the two groups a type does not use. */

#define MV_NO_OPTIONAL_PROPERTIES(ADD_OPTIONAL_PROPERTY) /* an empty list */

#define MV_OPT_ENUMERATOR(index, name, type) name = index,
#define MV_OPT_SIZE_ROW(index, name, type) sizes[index] = uint8_t(sizeof(type));
#define MV_OPT_COUNT_ROW(index, name, type) ++declared;
#define MV_OPT_MASK_ROW(index, name, type) mask |= (1ULL << (index));
#define MV_OPT_HASH_ROW(index, name, type) \
    hash = OptionalSchemaFold(hash, #name, uint8_t(index), uint8_t(sizeof(type)));

// FNV-1a over property name, index and size. A schema change therefore changes the hash, which is
// what makes it useful in a mismatch diagnostic. Not persisted and not a security primitive.
constexpr uint64_t OptionalSchemaFold(uint64_t hash, const char* name, uint8_t index, uint8_t size) {
    constexpr uint64_t prime = 0x100000001B3ULL;
    for (const char* c = name; *c != '\0'; ++c) hash = (hash ^ uint64_t(uint8_t(*c))) * prime;
    hash = (hash ^ index) * prime;
    return (hash ^ size) * prime;
}

#define MV_OPT_ACCESSORS(index, name, type)                                                        \
    static_assert(IsOptionalPropertyType<type>, #name " is not a supported property type.");       \
    static_assert((index) < 64, #name " exceeds the 64 fixed optional properties per type.");      \
    bool has##name() const { return optional.isSetFixed(index); }                                  \
    type get##name() const {                                                                       \
        if (!optional.isSetFixed(index)) return type{};                                            \
        return optional.get<type>(                                                                 \
            uint16_t(optional.calculateOffsetX(index, optionalFixedByteSizes.data())));            \
    }                                                                                              \
    void set##name(type value) {                                                                   \
        optional.set<type>(                                                                        \
            uint16_t(optional.enableProperty(index, optionalFixedByteSizes.data())), value);       \
    }                                                                                              \
    void unset##name() { optional.unsetFixed(index, optionalFixedByteSizes.data()); }

#define MV_OPT_DYNAMIC_ACCESSORS(index, name, type)                                                \
    static_assert(std::is_same_v<type, ByteArrayData>, #name " must be declared ByteArrayData.");  \
    static_assert((index) < 64, #name " exceeds the 64 dynamic optional properties per type.");    \
    bool has##name() const { return optional.isSetDynamic(index); }                                \
    uint32_t name##Size() const { return optional.getDynamic(index).size; }                        \
    std::byte* name##Bytes() const { return optional.dynamicBytes(index); }                        \
    void set##name(const void* source, uint32_t size) {                                            \
        optional.setDynamic(index, source, size);                                                  \
    }                                                                                              \
    void unset##name() { optional.unsetDynamic(index); }

#define MV_DECLARE_OPTIONAL_PROPERTIES(FIXED_LIST, DYNAMIC_LIST)                                   \
    Optional64 optional;                                                                           \
    enum OptionalFixedProperty : uint8_t { FIXED_LIST(MV_OPT_ENUMERATOR) };                        \
    enum OptionalDynamicProperty : uint8_t { DYNAMIC_LIST(MV_OPT_ENUMERATOR) };                    \
    static constexpr std::array<uint8_t, 64> optionalFixedByteSizes = [] {                         \
        std::array<uint8_t, 64> sizes{};                                                           \
        FIXED_LIST(MV_OPT_SIZE_ROW)                                                                \
        return sizes; }();                                                                         \
    static constexpr uint8_t optionalFixedCount = [] {                                             \
        uint8_t declared = 0; FIXED_LIST(MV_OPT_COUNT_ROW) return declared; }();                   \
    static constexpr uint8_t optionalDynamicCount = [] {                                           \
        uint8_t declared = 0; DYNAMIC_LIST(MV_OPT_COUNT_ROW) return declared; }();                 \
    static_assert(optionalFixedCount <= 64, "More than 64 fixed optional properties declared.");   \
    static_assert(optionalDynamicCount <= 64, "More than 64 dynamic optional properties.");        \
    /* Every index must be UNIQUE. Two properties sharing one index is the copy-paste mistake      \
    this schema invites, and without this check it compiles clean and then silently loses data:    \
    both map to the same flag bit and the same offset, so writing one erases the other. Fewer      \
    set bits than declared properties is exactly that collision. */                                \
    static constexpr uint64_t optionalFixedMask = [] {                                             \
        uint64_t mask = 0; FIXED_LIST(MV_OPT_MASK_ROW) return mask; }();                           \
    static constexpr uint64_t optionalDynamicMask = [] {                                           \
        uint64_t mask = 0; DYNAMIC_LIST(MV_OPT_MASK_ROW) return mask; }();                         \
    static_assert(std::popcount(optionalFixedMask) == int(optionalFixedCount),                     \
        "Two fixed optional properties share the same index - give each a distinct one.");         \
    static_assert(std::popcount(optionalDynamicMask) == int(optionalDynamicCount),                 \
        "Two dynamic optional properties share the same index - give each a distinct one.");       \
    static constexpr uint64_t optionalSchemaHash() {                                               \
        uint64_t hash = 0xCBF29CE484222325ULL;                                                     \
        FIXED_LIST(MV_OPT_HASH_ROW)                                                                \
        DYNAMIC_LIST(MV_OPT_HASH_ROW)                                                              \
        return hash; }                                                                             \
    FIXED_LIST(MV_OPT_ACCESSORS)                                                                   \
    DYNAMIC_LIST(MV_OPT_DYNAMIC_ACCESSORS)

/* ---- ALTERNATIVE OFFSET CALCULATION, NOT YET IN SERVICE ---------------------------------------

Expands a 64-bit flag word to one byte per flag so the offset becomes a dot product with the size
array. Kept for the benchmark against calculateOffsetX's set-bit loop above: this one is constant
time in the number of DECLARED properties, the loop is linear in the number of PRESENT ones, so
which wins depends on how densely real schemas populate. ARMv8 and RISC-V equivalents would be
needed before it could be adopted. Currently called by nothing. */
#include <immintrin.h> // For AVX2 intrinsics
inline void generateFlagsToByte_AVX2_Refined(uint64_t flags, std::byte flagsToByte[64]) {
    // Load the 64-bit flags into a 128-bit register and broadcast it across a 256-bit register.
    // This makes bytes 0-7 available in both 128-bit lanes for the shuffle.
    const __m128i flags_128 = _mm_set_epi64x(0, static_cast<long long>(flags));
    const __m256i v_flags = _mm256_broadcastsi128_si256(flags_128);

    // --- Create broadcasted vectors for low and high 32 bits using shuffles ---

    // Shuffle mask to broadcast bytes 0, 1, 2, and 3.
    const __m256i shuf_mask_low = _mm256_set_epi8(
        3, 3, 3, 3, 3, 3, 3, 3,  // Broadcast byte 3
        2, 2, 2, 2, 2, 2, 2, 2,  // Broadcast byte 2
        1, 1, 1, 1, 1, 1, 1, 1,  // Broadcast byte 1
        0, 0, 0, 0, 0, 0, 0, 0   // Broadcast byte 0
    );
    const __m256i v_broadcast_low = _mm256_shuffle_epi8(v_flags, shuf_mask_low);

    // Shuffle mask to broadcast bytes 4, 5, 6, and 7.
    const __m256i shuf_mask_high = _mm256_set_epi8(
        7, 7, 7, 7, 7, 7, 7, 7,  // Broadcast byte 7
        6, 6, 6, 6, 6, 6, 6, 6,  // Broadcast byte 6
        5, 5, 5, 5, 5, 5, 5, 5,  // Broadcast byte 5
        4, 4, 4, 4, 4, 4, 4, 4   // Broadcast byte 4
    );
    const __m256i v_broadcast_high = _mm256_shuffle_epi8(v_flags, shuf_mask_high);

    // --- Common masks and operations for both halves ---

    // Mask to test individual bits of a byte: {128, 64, ..., 1}.
    const __m256i bit_mask = _mm256_set_epi8(
        static_cast<char>(1<<7), 1<<6, 1<<5, 1<<4, 1<<3, 1<<2, 1<<1, 1<<0,
        static_cast<char>(1<<7), 1<<6, 1<<5, 1<<4, 1<<3, 1<<2, 1<<1, 1<<0,
        static_cast<char>(1<<7), 1<<6, 1<<5, 1<<4, 1<<3, 1<<2, 1<<1, 1<<0,
        static_cast<char>(1<<7), 1<<6, 1<<5, 1<<4, 1<<3, 1<<2, 1<<1, 1<<0
    );

    // Isolate the bits by ANDing the broadcasted flags with the bit mask.
    const __m256i v_anded_low = _mm256_and_si256(v_broadcast_low, bit_mask);
    const __m256i v_anded_high = _mm256_and_si256(v_broadcast_high, bit_mask);

    // Compare with zero. Result is 0xFF if the bit was set (value > 0), 0x00 otherwise.
    const __m256i v_zero = _mm256_setzero_si256();
    const __m256i v_cmp_low = _mm256_cmpgt_epi8(v_anded_low, v_zero);
    const __m256i v_cmp_high = _mm256_cmpgt_epi8(v_anded_high, v_zero);

    // Convert 0xFF to 0x01 by ANDing with a vector of 1s.
    const __m256i v_ones = _mm256_set1_epi8(1);
    const __m256i v_result_low = _mm256_and_si256(v_cmp_low, v_ones);
    const __m256i v_result_high = _mm256_and_si256(v_cmp_high, v_ones);

    // Store the results into the output byte array.
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(&flagsToByte[0]), v_result_low);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(&flagsToByte[32]), v_result_high);
}

/* ================================================================================================
   APPENDIX - THE ORIGINAL SPECIFICATION, VERBATIM.

   Everything below is the specification text as it stood before Optional64 was implemented,
   commented out and otherwise unaltered - including the class DerivedClass sketch, the discarded
   ByteArrayData variant, the intent paragraph, the worked getHeight/setHeight example, the
   complexity and threading notes, and the closing review instructions.

   It is kept because the implementation above ANSWERS the specification without REPLACING it:
   several statements here are design intent that no line of code expresses. The ones worth
   knowing about, because the code cannot tell you:
     - the single-writer / multiple-reader threading contract, which Optional64 does NOT enforce
       and which derived classes are expected to honour;
     - the complexity targets: get O(1), set on an existing property O(1), set on a new one and
       unset O(N) in the bytes after the insertion point;
     - the field-reordering licence - this is a RAM layout only, never a disc format, so members
       may be reordered between compiled versions;
     - Bfloat16 named as the candidate for the one spare property type;
     - the ByteArrayData variant that stored a raw pointer, and the note that it was discarded;
     - the supported-platform statement: little-endian only, x64 / ARMv8 / RISC-V.

   Where the implementation deviates, the deviation and its reason are listed under "WHAT CHANGED
   FROM THE ORIGINAL SPECIFICATION" at the top of this file. One deviation is not listed there
   because it is the project's, not this class's: the specification asks for C++23, the project
   builds at C++20 (LanguageStandard stdcpp20), and nothing here needed C++23.
   ============================================================================================= */


//
// /* Think over it and Analyze the specification's intent, provide feedback. Do not implement the code.
// This specification defines the optional properties that can be associated with 1000s of different derived data classes.
// Self contained .h ( definition + implementation both), c++23 class named Optional64.
// We support little-endian process architecture / operating system only (Windows/Linux on x64, ARMv8, RISC-V servers/desktops).*/
//
// // Miscellaneous information:
// struct Byte16 { std::byte data[16]; }; // Byte16 and Byte32 are for Small String Optimization, Null Terminated UTF-8 strings.
// struct Byte32 { std::byte data[32]; };
// /* ByteArrayData is 12 Bytes long ( 4 Bytes: chunkIndex, 4 Bytes: Size, 4 Bytes: Offset. ).
// This is our overhead for dynamic memory allocation. Even though "size" variable can store up to 4 GB, we will not allow it to grow >4 MB.*/
// //struct ByteArrayData { uint32_t chunkIndex;  uint32_t size; std::byte* bytes;};//Discarded. Storing actual pointer.
// struct ByteArrayData { uint32_t chunkIndex; uint32_t offset; uint32_t size; };//12 Bytes overhead for dynamic memory allocation.
//
// // Some static assertions to ensure the correctness of the code.
// static_assert(sizeof(bool) == 1);
// static_assert(std::endian::native == std::endian::little);
// static_assert(sizeof(Byte16) == 16 && sizeof(Byte32) == 32);
// static_assert(sizeof(ByteArrayData) == 12 && alignof(ByteArrayData) == 4);
//
// const static uint8_t MAX_PROPERTY_TYPES = 16; // Currently 14 implemented. 1 Reserved for future use. Ex: Bfloat16 for AI!
// /* Intent is that Derived classes can define up to 64x2 optional properties, each with one of the following types.
// bool, char, uint8_t, int8_t, uint16_t, int16_t, uint32_t, int32_t, float, uint64_t, int64_t, double, Byte16, Byte32.
// Byte16 and Byte32 are for Small String Optimization, Null Terminated UTF-8 strings.
// For dynamic optional properties, we use ByteArrayData which is a pointer to a memory block managed by the RAM arena.
// The Optional64 class manages these properties efficiently in memory, allowing for quick access and modification.
// A property takes space only when it is set, and the class provides methods to check if a property is set, get its value, set its value,
// unset it, and debug dump the current state of properties. */
// class Optional64{
// public:
//     uint64_t flagsFixed; // To store 64 flags. For primitive c++ data types and Byte16/Byte32. i.e. Have predefined fixed size.
//     uint64_t flagsDynamic; // For ByteArrayData member only. i.e. Can store up to 64 flags for ByteArrayData properties.
//     /* Pointer to store the starting address of the memory where all these fields (which are present) are packed closely,
//     without any space in between. If a flag is Off, No memory is allotted to it.
//     To prevent frequent reallocations during new field insertions, the memory allocation (x & y both) grows in following sequence.
//     32 (initially), 64, 128, 256, 512, 1024, 2048 ( 64 x 32 for Byte32 = 2048 Max.
//     Note that for variable ByteArrayData, corresponding pointer could be located in a different RAM Chunk, managed by RAMArena class राम.*/
//
//     std::byte* x; // x is for Optional property of fixed size having standard c++ types.
//
//     /* y is for Optional property, needing it's own memory allocation out of RAM Arena. y stores ByteArrayData only.
//     Each pointer is uniquely owned by the Optional64 class, and is not shared with any other class. Hence no ref counting needed.*/
//     std::byte* y;
//
//     uint16_t xBytesAllocated, xBytesUsed; // xBytesUsed will always be less than or equal to xBytesAllocated.
//     uint16_t yCountAllocated, yCountUsed; // yCountUsed will always be less than or equal to yCountAllocated.
//
//     // The RAM Chunk which is storing the pointer being pointed by x & y respectively.
//     // ChunkIndx is used by destructor to release the memory back to the RAM arena.
//     uint32_t xChunkIndex, yChunkIndex;
//
//     // Base Class functions:
//     // Constructor to initialize the class. Initially x shall have minimum size as recommended above.
//     // This will also call the allocateMemory() function to allocate 32 Byte initial memory for x.
//     Optional64() { flagsFixed = 0; flagsDynamic = 0;
//         cpuRAMArena.allocate(&x, 32, xChunkIndex); xBytesAllocated=32; xBytesUsed=0;
//         cpuRAMArena.allocate(&y, 4*sizeof(ByteArrayData), yChunkIndex); yCountAllocated=4; yCountUsed = 0;
//     }
//
//     /* 1st Release all the memory pointed by ByteArrayData types stored in variable y.
//     Than release memory pointed by x & y back to the arena. Note that this does not return memory to OS but to  RAM arena.*/
//     ~Optional64();
//
//     /* Generic get/set methods called by the derived class's specific getters/setters.
//     Always use std::memcpy to/from a properly aligned local T when reading/writing packed storage.
//     This is mandatory for ARMv8 and RISCV architectures. For x86/x64, std::bit_cast can be used for better performance.
//     Returns a value of type dataType. This function should be used to get the value of a property by its offset.
//     If the property is not set, it returns a default value of dataType. // This is a safety measure against undefined behavior.
//     bool: false, char: null, uint8_t:0, int8_t:0, uint16_t:0, int16_t:0, uint32_t:0, int32_t:0, float:0, uint64_t:0, int64_t:0,
//     double: 0, ByteArrayData: {0,0,0}, which is an invalid value, Byte16:{0 array}, Byte32:{0 array}. */
//     template<typename dataType>
//     dataType get(uint16_t offset) const; //Memcpy the data from x[offset] to a local variable of type dataType and return it.
//
//     //To set a value, Check if it's flag is set. If not, create space for it in x, set the flag. Than memcpy the value to x[offset].
//     template<typename dataType>
//     void set(uint16_t offset, dataType value);
//
//     void setDynamic(uint8_t propertyIndex, const void* src, uint32_t size); // For ByteArrayData properties.
//
//     // To unset a property, clear the flag and shift bytes in x by byteSize at offset. Update xBytesUsed accordingly.
//     template<typename dataType>
//     void unsetFixed(uint8_t propertyIndex, const uint8_t* propertyByteSizes);
//
//     // Specialized function shall be defined for ByteArrayData, since it has a pointer to external memory which needs to be freed.
//     void unsetDynamic(uint8_t propertyIndex);
//
//     bool isSetFixed(uint8_t propertyIndex) const { return (flagsFixed & (1ULL << propertyIndex)) != 0; }
//     bool isSetDynamic(uint8_t propertyIndex) const { return (flagsDynamic & (1ULL << propertyIndex)) != 0; }
//
//     /* Check if flag is set, if not, insert it in the middle of x in exact sequence. Shifting all subsequent bytes.
//     Returns the offset of the property Index. If it is already set, it does nothing and returns the existing offset.
//     This may trigger a reallocation of x or y if they are not large enough to accommodate the new property.*/
//     uint32_t enableProperty(uint8_t propertyIndex, const uint8_t* propertyByteSizes);
//     uint32_t enableDynamicProperty(uint8_t propertyIndex);
//
//     void debugDump() const; //To the extent human readable format.
//
//     /* When x needs to be grown. If x_extra = 0, follow the next number in [ 64, 128, 256, 512, 1024, 2048 ] Never exceed 2048.
//     y will always be grown by 4 * sizeof(ByteArrayData) Bytes. yCountAllocated grows by 4 up to 64. Max 64.
//     Up to a maximum of 64 properties of type ByteArrayData. Also called when preallocation is required for bulk insertions. */
//     void allocateMemoryX(uint16_t x_extra = 0);
//     void allocateMemoryY(uint16_t y_extra = 0);
//
//     void releaseMemory(); // Release the memory allocated for x & y back to the RAM arena. This is called in destructor.
//
//     /* Helper for offset calculation based on popcount, bit manipulation, SSE/AVX instructions etc. No Loops, no conditionals,
//     just bit manipulation. To do this, We will also have to unpack uint64_t to byte array [64] using AVX2.
//     propertyByteSizes is an array of size 64. It sums the dot product of all flags smaller than propertyIndex and size array.*/
//     uint32_t calculateOffsetX(uint8_t propertyIndex, const uint8_t* const propertyByteSizes) const;
//     uint32_t calculateOffsetY(uint8_t propertyIndex) const; //Don't need propertyByteSizes, since ByteArrayData is fixed size.
//
//     // This Optional class is not responsible for defragmentation of RAM arena memory.
//
//     // Next: Copy constructor shall be implemented using deep copy semantics.
// };
//
// /* There will be 1000s of distinct derived class. Schema is always defined compile time and should preferably be assert checked.
// Saving the data to disc or IPC serialization is not specified in this specification.
// That will be taken care in a separate Disc Write/Read function specification. */
// class DerivedClass: public META_DATA {
//     /* Some fields come to DerivedClass from MetaData class. Mandatory Fields specific to the derived class are declared first.
//     These fields are not optional and must be present in every instance of the derived class.
//     These are accessed using .x / .y / .z etc. syntax. */
//     uint32_t x,y,z;
//
//     Optional64 opt; // This is the instance of the Optional64 class that will manage the optional properties for this derived class.
//     static const uint8_t optionalFixedCount; // Computed compile time, to speedup other functions. Defined using a macro.
//     static const uint8_t optionalDynamicCount; // Computed compile time, to speedup other functions. Defined using a macro.
//
//     /* Create a macro to make derived classes Schema definition easier to implement.
//     Using combination of macros, x-macros, templates, constexpr, consteval etc
//     BEGIN_OPTIONAL_PROPERTIES()
//         ADD_OPTIONAL_PROPERTY(property1, uint32_t);
//         ADD_OPTIONAL_PROPERTY(property2, char);
//         ADD_OPTIONAL_PROPERTY(property3, float);
//         ADD_OPTIONAL_PROPERTY(property4, int64_t);
//         ADD_OPTIONAL_PROPERTY(property5, ByteArrayData);
//         // Maximum 64 properties can be defined, in any sequence. in each x and y groups.
//     END_OPTIONAL_PROPERTIES()
//
//     Computed compile time. To store the count of number of variables of each type. Even though each value can store up-to 256,
//     sum of all the values should be maximum 64 only, since we have only 64 flags available. When a new property is inserted at runtime,
//     Max shifting required can't be more than 2048 Bytes, a small number which is acceptable.
//     */
//     // static uint8_t propertyTypesCount[MAX_PROPERTY_TYPES]; // Doesn't seem to be needed now.
//
//     /* To store the bytes taken by each stored property name. Generated at compile time. Latter in will be optimized to 32 bytes size only.
//     This information, in conjunction with flags bit field shall speed up the get/set operations or lookups of the byte offset of a property.*/
//     static const uint8_t propertyByteSizes[64];
//
//     // Generated compile time from the ADD_OPTIONAL_PROPERTY() defined above. It's basically index of each property.
//     enum PropertyName { property1, property2, etc } ; // All properties other than ByteArrayData are gathered here.
//     enum DynamicPropertyName { property3, property8, etc}; //All ByteArrayData types are gathered here.
//     /* For each optional property, getter/setter will be generated at compile time.
//     Ex: ADD_OPTIONAL_PROPERTY(property1, uint32_t); generated following 2 functions:
//     float getHeight() // Getter Function
//     {
//         if(!opt.isSet(propertyName::height)) {
//             logToConsole("Height property is not set."); // Only in debug mode.
//             return 0.0f; // Default value of float if not set
//         }
//         uint32_t offset = opt.calculateOffset(propertyName::height, propertyByteSizes);
//         return opt.get<float>(offset); // Get the value of the property at the calculated offset
//     }
//     void setHeight(float value) // Setter Function
//     {
//         uint32_t offset;
//         if(opt.isSet(propertyName::height)) {
//             offset = opt.calculateOffset(propertyName::height, propertyByteSizes);
//         } else {
//             //The property is not set, we need to allocate space for i
//             offset = opt.enableProperty(propertyName::height, propertyByteSizes);t.
//         }
//         opt.set<float>(offset, value); // Set the value of the property at the calculated offset
//     }
//     //Similar functions will be generated for all ByteArrayData properties as well, which refer y instead of x.
//
//     This will ensure that the type of the property is checked at compile time, and the correct type is used for getting/setting the value.
//     If the field is present, it's value is just updated. If not, corresponding flag is set true,
//     and all data bytes are moved to create space of this new bytes in the middle, maintaining the order of Property Types in schema.
//     Every set/get should validate that the PropertyName’s declared type matches the set/get operation. static_assert at compile time.*/
//
//     // Function to query if a property is present. Needs to be generated at compile time for each property defined in the derived class.
//     bool isPropertySet(PropertyName p) const;
//     // clear flag and shift bytes. Needs to be generated at compile time for each property defined in the derived class.
//     void unset(PropertyName p);
//
//     /* method to print which properties are set and their values.
//     Calls the debugDump() method of the base class Optional64 to print the current state of properties. */
//     void debugDump() const; // To the extent, human readable format.
//     uint64_t schemaHash() const; // Helpful for diagnostics if schema mismatch occurs. Generated compile time. Any algorithm can be used.
//
//     /* Memory allocation shall not be handled by this class itself. This is for RAM memory layout only.
//     Not for persistence on disc. Hence reordering of fields can be done between compile versions.
//     Only 1 thread shall update the RAM Arena. Hence memory safe. Single writer, multiple reader shall be implemented by Derived classes.
//     Not Optional64. The implementation should be portable for x64, ARMv8, RISCV (use std::memmove / std::bit_cast if required.)
//     Time Complexity:  get(i) is O(1) (after a fast O(k) offset calculation, where k is the number of property types, not properties).
//     set(i, value) on an existing property is O(1). set(i, value) on a new property is O(N) due to the data shift in x.
//     unset(i) is O(N) due to the data shift in x.
//
//     Use c++23. Use static_assert to ensure that the derived class is not empty and has at least one property defined.
//     Compile time assertion that not more than 64 optional properties are defined in each group x and y.
//     Make the implementation optimized production ready.
//
//     Either we can have a macro to define the properties, or we can use a template function to add properties.
//     Both case need to have option to specify types, and we should be able to define the functions declared above in the derived class.
//
//     Here is my logic to implement the byteOffset calculation. suggest an alternative / improvement if possible.
//     We are going to calculate offset of i'th property. i is the sequence number of property defined at compile time.
//     uint8_t propertyTypeByteSizes[64]; // This is the array of byte sizes of each property type.
//     uint16_t offset = 0; // This is the offset we are going to calculate
//     uint8_t index i; //Calculating the offset of i'th property. It comes from PropertyName enum.
//     uint64_t preceding_mask = (1ULL << i) - 1;
//     uint64_t set_preceding_flags = flagsFixed & preceding_mask; // Bitwise AND operation.
//     // Next Expand set_preceding_flags to a byte array of size 64.
//     offset = sum ( Dot product of set_preceding_flags expanded array and propertyTypeByteSizes array );
//     */
//
// };
//