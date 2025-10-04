// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#include<cstdint>
#include <cstddef>
#include <bit>
class META_DATA;// Forward declaration to demonstrate creation of derived classes with optional properties.
class राम;
extern राम cpuRAMArena; // Forward declaration of RAM class, which is the memory manager for the RAM arena.

/* Think over it and Analyze the specification's intent, provide feedback. Do not implement the code.
This specification defines the optional properties that can be associated with 1000s of different derived data classes.
Self contained .h ( definition + implementation both), c++23 class named Optional64. 
We support little-endian process architecture / operating system only (Windows/Linux on x64, ARMv8, RISC-V servers/desktops).*/

// Miscellaneous information:
struct Byte16 { std::byte data[16]; }; // Byte16 and Byte32 are for Small String Optimization, Null Terminated UTF-8 strings.
struct Byte32 { std::byte data[32]; };
/* ByteArrayData is 12 Bytes long ( 4 Bytes: chunkIndex, 4 Bytes: Size, 4 Bytes: Offset. ).
This is our overhead for dynamic memory allocation. Even though "size" variable can store up to 4 GB, we will not allow it to grow >4 MB.*/
//struct ByteArrayData { uint32_t chunkIndex;  uint32_t size; std::byte* bytes;};//Discarded. Storing actual pointer.
struct ByteArrayData { uint32_t chunkIndex; uint32_t offset; uint32_t size; };//12 Bytes overhead for dynamic memory allocation.

// Some static assertions to ensure the correctness of the code.
static_assert(sizeof(bool) == 1);
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(Byte16) == 16 && sizeof(Byte32) == 32);
static_assert(sizeof(ByteArrayData) == 12 && alignof(ByteArrayData) == 4);

const static uint8_t MAX_PROPERTY_TYPES = 16; // Currently 14 implemented. 1 Reserved for future use. Ex: Bfloat16 for AI!
/* Intent is that Derived classes can define up to 64x2 optional properties, each with one of the following types.
bool, char, uint8_t, int8_t, uint16_t, int16_t, uint32_t, int32_t, float, uint64_t, int64_t, double, Byte16, Byte32. 
Byte16 and Byte32 are for Small String Optimization, Null Terminated UTF-8 strings.
For dynamic optional properties, we use ByteArrayData which is a pointer to a memory block managed by the RAM arena.
The Optional64 class manages these properties efficiently in memory, allowing for quick access and modification. 
A property takes space only when it is set, and the class provides methods to check if a property is set, get its value, set its value, 
unset it, and debug dump the current state of properties. */
class Optional64{
public:
    uint64_t flagsFixed; // To store 64 flags. For primitive c++ data types and Byte16/Byte32. i.e. Have predefined fixed size.
    uint64_t flagsDynamic; // For ByteArrayData member only. i.e. Can store up to 64 flags for ByteArrayData properties.
    /* Pointer to store the starting address of the memory where all these fields (which are present) are packed closely,
    without any space in between. If a flag is Off, No memory is allotted to it.
    To prevent frequent reallocations during new field insertions, the memory allocation (x & y both) grows in following sequence. 
    32 (initially), 64, 128, 256, 512, 1024, 2048 ( 64 x 32 for Byte32 = 2048 Max.
    Note that for variable ByteArrayData, corresponding pointer could be located in a different RAM Chunk, managed by RAMArena class राम.*/

    std::byte* x; // x is for Optional property of fixed size having standard c++ types.

    /* y is for Optional property, needing it's own memory allocation out of RAM Arena. y stores ByteArrayData only.
    Each pointer is uniquely owned by the Optional64 class, and is not shared with any other class. Hence no ref counting needed.*/
    std::byte* y;
    
    uint16_t xBytesAllocated, xBytesUsed; // xBytesUsed will always be less than or equal to xBytesAllocated. 
    uint16_t yCountAllocated, yCountUsed; // yCountUsed will always be less than or equal to yCountAllocated.
    
    // The RAM Chunk which is storing the pointer being pointed by x & y respectively. 
    // ChunkIndx is used by destructor to release the memory back to the RAM arena.
    uint32_t xChunkIndex, yChunkIndex;   
    
    // Base Class functions:
    // Constructor to initialize the class. Initially x shall have minimum size as recommended above. 
    // This will also call the allocateMemory() function to allocate 32 Byte initial memory for x.
    Optional64() { flagsFixed = 0; flagsDynamic = 0; 
        cpuRAMArena.allocate(&x, 32, xChunkIndex); xBytesAllocated=32; xBytesUsed=0;
        cpuRAMArena.allocate(&y, 4*sizeof(ByteArrayData), yChunkIndex); yCountAllocated=4; yCountUsed = 0;
    } 
    
    /* 1st Release all the memory pointed by ByteArrayData types stored in variable y.
    Than release memory pointed by x & y back to the arena. Note that this does not return memory to OS but to  RAM arena.*/
    ~Optional64(); 
    
    /* Generic get/set methods called by the derived class's specific getters/setters. 
    Always use std::memcpy to/from a properly aligned local T when reading/writing packed storage. 
    This is mandatory for ARMv8 and RISCV architectures. For x86/x64, std::bit_cast can be used for better performance.
    Returns a value of type dataType. This function should be used to get the value of a property by its offset.
    If the property is not set, it returns a default value of dataType. // This is a safety measure against undefined behavior.
    bool: false, char: null, uint8_t:0, int8_t:0, uint16_t:0, int16_t:0, uint32_t:0, int32_t:0, float:0, uint64_t:0, int64_t:0,
    double: 0, ByteArrayData: {0,0,0}, which is an invalid value, Byte16:{0 array}, Byte32:{0 array}. */
    template<typename dataType>
    dataType get(uint16_t offset) const; //Memcpy the data from x[offset] to a local variable of type dataType and return it.
    
    //To set a value, Check if it's flag is set. If not, create space for it in x, set the flag. Than memcpy the value to x[offset].
    template<typename dataType>
    void set(uint16_t offset, dataType value);

    void setDynamic(uint8_t propertyIndex, const void* src, uint32_t size); // For ByteArrayData properties.

    // To unset a property, clear the flag and shift bytes in x by byteSize at offset. Update xBytesUsed accordingly.
    template<typename dataType>
    void unsetFixed(uint8_t propertyIndex, const uint8_t* propertyByteSizes);

    // Specialized function shall be defined for ByteArrayData, since it has a pointer to external memory which needs to be freed.
    void unsetDynamic(uint8_t propertyIndex);

    bool isSetFixed(uint8_t propertyIndex) const { return (flagsFixed & (1ULL << propertyIndex)) != 0; }
    bool isSetDynamic(uint8_t propertyIndex) const { return (flagsDynamic & (1ULL << propertyIndex)) != 0; }

    /* Check if flag is set, if not, insert it in the middle of x in exact sequence. Shifting all subsequent bytes.
    Returns the offset of the property Index. If it is already set, it does nothing and returns the existing offset.
    This may trigger a reallocation of x or y if they are not large enough to accommodate the new property.*/
    uint32_t enableProperty(uint8_t propertyIndex, const uint8_t* propertyByteSizes);
    uint32_t enableDynamicProperty(uint8_t propertyIndex);

    void debugDump() const; //To the extent human readable format.
    
    /* When x needs to be grown. If x_extra = 0, follow the next number in [ 64, 128, 256, 512, 1024, 2048 ] Never exceed 2048.
    y will always be grown by 4 * sizeof(ByteArrayData) Bytes. yCountAllocated grows by 4 up to 64. Max 64.
    Up to a maximum of 64 properties of type ByteArrayData. Also called when preallocation is required for bulk insertions. */
    void allocateMemoryX(uint16_t x_extra = 0);
    void allocateMemoryY(uint16_t y_extra = 0);
    
    void releaseMemory(); // Release the memory allocated for x & y back to the RAM arena. This is called in destructor.
    
    /* Helper for offset calculation based on popcount, bit manipulation, SSE/AVX instructions etc. No Loops, no conditionals, 
    just bit manipulation. To do this, We will also have to unpack uint64_t to byte array [64] using AVX2.
    propertyByteSizes is an array of size 64. It sums the dot product of all flags smaller than propertyIndex and size array.*/
    uint32_t calculateOffsetX(uint8_t propertyIndex, const uint8_t* const propertyByteSizes) const;
    uint32_t calculateOffsetY(uint8_t propertyIndex) const; //Don't need propertyByteSizes, since ByteArrayData is fixed size.
    
    // This Optional class is not responsible for defragmentation of RAM arena memory.

    // Next: Copy constructor shall be implemented using deep copy semantics.
};

/* There will be 1000s of distinct derived class. Schema is always defined compile time and should preferably be assert checked. 
Saving the data to disc or IPC serialization is not specified in this specification.
That will be taken care in a separate Disc Write/Read function specification. */
class DerivedClass: public META_DATA {
    /* Some fields come to DerivedClass from MetaData class. Mandatory Fields specific to the derived class are declared first.
    These fields are not optional and must be present in every instance of the derived class.
    These are accessed using .x / .y / .z etc. syntax. */
    uint32_t x,y,z; 

    Optional64 opt; // This is the instance of the Optional64 class that will manage the optional properties for this derived class.
    static const uint8_t optionalFixedCount; // Computed compile time, to speedup other functions. Defined using a macro.
    static const uint8_t optionalDynamicCount; // Computed compile time, to speedup other functions. Defined using a macro.

    /* Create a macro to make derived classes Schema definition easier to implement.
    Using combination of macros, x-macros, templates, constexpr, consteval etc 
    BEGIN_OPTIONAL_PROPERTIES()
        ADD_OPTIONAL_PROPERTY(property1, uint32_t);
        ADD_OPTIONAL_PROPERTY(property2, char);
        ADD_OPTIONAL_PROPERTY(property3, float);
        ADD_OPTIONAL_PROPERTY(property4, int64_t);
        ADD_OPTIONAL_PROPERTY(property5, ByteArrayData);
        // Maximum 64 properties can be defined, in any sequence. in each x and y groups.
    END_OPTIONAL_PROPERTIES()

    Computed compile time. To store the count of number of variables of each type. Even though each value can store up-to 256, 
    sum of all the values should be maximum 64 only, since we have only 64 flags available. When a new property is inserted at runtime,
    Max shifting required can't be more than 2048 Bytes, a small number which is acceptable.
    */
    // static uint8_t propertyTypesCount[MAX_PROPERTY_TYPES]; // Doesn't seem to be needed now.
    
    /* To store the bytes taken by each stored property name. Generated at compile time. Latter in will be optimized to 32 bytes size only.
    This information, in conjunction with flags bit field shall speed up the get/set operations or lookups of the byte offset of a property.*/
    static const uint8_t propertyByteSizes[64];

    // Generated compile time from the ADD_OPTIONAL_PROPERTY() defined above. It's basically index of each property.
    enum PropertyName { property1, property2, etc } ; // All properties other than ByteArrayData are gathered here.
    enum DynamicPropertyName { property3, property8, etc}; //All ByteArrayData types are gathered here.
    /* For each optional property, getter/setter will be generated at compile time.
    Ex: ADD_OPTIONAL_PROPERTY(property1, uint32_t); generated following 2 functions:
    float getHeight() // Getter Function
    {
        if(!opt.isSet(propertyName::height)) {
            logToConsole("Height property is not set."); // Only in debug mode.
            return 0.0f; // Default value of float if not set
        }
        uint32_t offset = opt.calculateOffset(propertyName::height, propertyByteSizes);
        return opt.get<float>(offset); // Get the value of the property at the calculated offset
    }
    void setHeight(float value) // Setter Function
    {
        uint32_t offset;
        if(opt.isSet(propertyName::height)) {
            offset = opt.calculateOffset(propertyName::height, propertyByteSizes);
        } else {
            //The property is not set, we need to allocate space for i
            offset = opt.enableProperty(propertyName::height, propertyByteSizes);t.
        }
        opt.set<float>(offset, value); // Set the value of the property at the calculated offset
    }
    //Similar functions will be generated for all ByteArrayData properties as well, which refer y instead of x.
    
    This will ensure that the type of the property is checked at compile time, and the correct type is used for getting/setting the value.
    If the field is present, it's value is just updated. If not, corresponding flag is set true, 
    and all data bytes are moved to create space of this new bytes in the middle, maintaining the order of Property Types in schema. 
    Every set/get should validate that the PropertyName’s declared type matches the set/get operation. static_assert at compile time.*/

    // Function to query if a property is present. Needs to be generated at compile time for each property defined in the derived class.
    bool isPropertySet(PropertyName p) const;
    // clear flag and shift bytes. Needs to be generated at compile time for each property defined in the derived class.
    void unset(PropertyName p);

    /* method to print which properties are set and their values. 
    Calls the debugDump() method of the base class Optional64 to print the current state of properties. */
    void debugDump() const; // To the extent, human readable format.
    uint64_t schemaHash() const; // Helpful for diagnostics if schema mismatch occurs. Generated compile time. Any algorithm can be used.

    /* Memory allocation shall not be handled by this class itself. This is for RAM memory layout only. 
    Not for persistence on disc. Hence reordering of fields can be done between compile versions. 
    Only 1 thread shall update the RAM Arena. Hence memory safe. Single writer, multiple reader shall be implemented by Derived classes. 
    Not Optional64. The implementation should be portable for x64, ARMv8, RISCV (use std::memmove / std::bit_cast if required.)
    Time Complexity:  get(i) is O(1) (after a fast O(k) offset calculation, where k is the number of property types, not properties). 
    set(i, value) on an existing property is O(1). set(i, value) on a new property is O(N) due to the data shift in x.
    unset(i) is O(N) due to the data shift in x.

    Use c++23. Use static_assert to ensure that the derived class is not empty and has at least one property defined.
    Compile time assertion that not more than 64 optional properties are defined in each group x and y.
    Make the implementation optimized production ready.

    Either we can have a macro to define the properties, or we can use a template function to add properties. 
    Both case need to have option to specify types, and we should be able to define the functions declared above in the derived class.
    
    Here is my logic to implement the byteOffset calculation. suggest an alternative / improvement if possible.
    We are going to calculate offset of i'th property. i is the sequence number of property defined at compile time.
    uint8_t propertyTypeByteSizes[64]; // This is the array of byte sizes of each property type.
    uint16_t offset = 0; // This is the offset we are going to calculate
    uint8_t index i; //Calculating the offset of i'th property. It comes from PropertyName enum.
    uint64_t preceding_mask = (1ULL << i) - 1;
    uint64_t set_preceding_flags = flagsFixed & preceding_mask; // Bitwise AND operation. 
    // Next Expand set_preceding_flags to a byte array of size 64.
    offset = sum ( Dot product of set_preceding_flags expanded array and propertyTypeByteSizes array );
    */

};



/* A helper function for possible use. Currently only AVX2 is implemented. In future we will have ARMv8 and RISCV implementation as well.
We will also have a few alternate implementation using popcount and bit manipulation.
Alternate implementation shall be benchmarked against each other for performance and integration into release code. */
#include <immintrin.h> // For AVX2 intrinsics
void generateFlagsToByte_AVX2_Refined(uint64_t flags, std::byte flagsToByte[64]) {
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
        1<<7, 1<<6, 1<<5, 1<<4, 1<<3, 1<<2, 1<<1, 1<<0,
        1<<7, 1<<6, 1<<5, 1<<4, 1<<3, 1<<2, 1<<1, 1<<0,
        1<<7, 1<<6, 1<<5, 1<<4, 1<<3, 1<<2, 1<<1, 1<<0,
        1<<7, 1<<6, 1<<5, 1<<4, 1<<3, 1<<2, 1<<1, 1<<0
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

// Report if there is anything missing in specification for implementation of all the functions declared in Optional64 class.
// Do not implement the specified functions. Just report if there is anything missing in the specification.
// The specification should be complete and self-contained for the implementation of the Optional64 class and its derived classes.
// Think over it and reply.