// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

/*
Think over it and Analyze the specification's intent, provide feedback and answer the question asked at the end of the specification. Do not implement the code.
This specification defines the optional properties that can be associated with various data derived data classes. Self contained .h ( definition + implementation both), c++23 class named Optional64.

// Miscellaneous information:
struct ByteArrayData { uint32_t offset;  uint32_t size; }; //ByteArrayData is 8 Bytes long ( 4 Bytes for Offset, 4 Bytes for Size ).
struct Byte16 { char data[16]; }; // Byte16 and Byte32 are for Small String Optimization, Null Terminated UTF-8 strings.
struct Byte32 { char data[32]; };
const uint8 MAX_PROPERTY_TYPES = 16; // Currently 15 implemented. 1 Reserved for future use.

Intent is that Derived classes can define up to 64 optional properties, each with either native c++ types ( bool, char, uint8_t, int8_t, uint16_t, int16_t, uint32_t, int32_t, float, uint64_t, int64_t, double) or custom ( ByteArrayData, Byte16, Byte32) types. The Optional64 class manages these properties efficiently in memory, allowing for quick access and modification. A property takes space only when it is set, and the class provides methods to check if a property is set, get its value, set its value, unset it, and debug dump the current state of properties.

//////////////////////////////////////////////////////////////////////////////
Base Class:
1: uint64t flags : to store 64 flags.
2: byte* x : pointer to store the starting address of the memory where all these fields (which are present) are packed closely without any space in between. If a flag is Off, No memory is allotted to it. To prevent frequent reallocations during new field insertions, the memory allocated to the Bytes pointed by this pointer grows in this sequence. [ 32 (initially), 64, 128, 256, 512, 1024, 2048 ( 64 x 32 for Byte32 = 2048 Max ]

3: byte* y : To store data of ByteArray types. The ByteArrayData type stores 4 bytes for starting point offset and 4 bytes for size of bytes for that property. This array grows as [ 32 (initially), 64, 128, 256, 512, 1024, Than keeps adding 1024 Bytes in each allocation requirement, upto a max of upper limit 4 MB]. We do not maintain order in this variable. Whenever a new property is added, it is added at the end.

4: uint16t xBytesAllocated, xBytesUsed. // xBytesUsed will always be less than or equal to xBytesAllocated. 
6: uint32t yBytesAllocated, yBytesUsed. // The Maximum size permitted for this variable is 4 MB.

//////////////////////////////////////////////////////////////////////////////////
Derived Class: There will be 1000s of distinct derived class. Schema is always defined compile time and should preferably be assert checked. Saving the data to disc or IPC serialization is not specified in this specification. That will be taken care in a separate Disc Write/Read function specification. It will also generate an internal enum to map the property names to their respective indices. 0 for char, 1 for uint8_t, 2 for int8_t, 3 for uint16_t, 4 for int16_t, and so on. This enum may be used to quickly access the properties by their names.

8. static const uint8_t optionalPropertyCount; // The total number of optional fields defined in derived class. Computed compile time, to speedup other functions.
9: static uint8_t propertyTypesCount[MAX_PROPERTY_TYPES]; // Computed compile time. To store the count of number of variables of each type. Even though each value can store up-to 256, sum of all the values should be maximum 64 only, since we have only 64 flags available. Field ordering is STRICTLY in the order [char, uint8_t, int8_t, uint16_t, int16_t, int16_t, uint32_t, int32_t, float, uint64_t, int64_t, double, ByteArrayData, Byte16, Byte32, Bfloat16]. This strict ordering is required to ensure that we can calculate the bytes offset of any property using just BitManipulation operations, without any loop. Even if insertions will need to shift all subsequent bytes. Max shifting required can't be more than 512, a small number which is acceptable.

10: static uint8_t propertyTypeByteSizes[optionalPropertyCount]; // To store the bytes taken by each stored property name. ( in variable x  only, not accounting y). Generated at compile time. This information may speed up the get/set operations or lookups of the byte offset of a property.
11: PropertyTypes enum or type-to-ID mapping generated at compile time since some types share sizes (e.g., uint32_t vs. float).

It has following function.

1: Function to request memory from RAMArena (Initially 32 bytes for both x & y). Latter, if during insertion of a field, if more memory is required, than it requests new continuous memory chunks from RAMArena, and move the data from previous pointer to new pointer.
2: void reserveForBulkInsert(size_t x_extra, size_t y_extra); // Function to preAllocate / Reserve additional memory for x & y. This will be done before whenever bulk property insertions are required.
3: For each optional property, Ex: "float height", a getter function : float getHeight(); and a setter function : void setHeight(float value); will be generated at compile time. This will ensure that the type of the property is checked at compile time, and the correct type is used for getting/setting the value. If the field is present, it's value is just updated. If not, corresponding flag is set true, and all data bytes are moved to create space of this new bytes in the middle, maintaining the sort order of Property Types. Every set/get should validate that the PropertyName’s declared type matches the set/get operation. static_assert at compile time. 

4. bool isPropertySet(PropertyName) const; Function to query if a property is present.
5. void unset(PropertyName); // clear flag and shift bytes
6. void debugDump(std::ostream&) const; // method to print which properties are set and their values.
7. void defragmentY() // Over time, pointer y may become defragmented. A function to defragment this and compact the array using a transient memory array. unsetting a property won’t immediately reclaim space in y. Calling this function is left to higher level logic.
8. uint64_t schemaHash() const; // helpful for diagnostics if schema mismatch occurs

General Information:
Memory allocation shall not be handled by this class itself. This is for RAM memory layout only. Not for persistence on disc. Hence reordering of fields can be done between compile versions. Only 1 thread shall update the RAM Arena. Hence memory safe. Single writer, multiple reader shall be implemented by Derived classes. Not Optional64. I am expecting most get/set(without memory expansion) operations to be O(1) and Insert / Set (Memory expansion required) Operations to be max O(N). The implementation should be portable for x64, ARMv8, RISCV (use std::memmove / std::bit_cast if required.)

Addition Information: Create a macro to make derived classes easier to implement. Make the implementation optimized production ready. Use c++23. Create a sample derived class. Use static_assert to ensure that the derived class is not empty and has at least one property defined.

I expect the following API schematics. Or something similar for creators of derived class.

class Optional64 {
.....
}

class DerivedClass: public MetaData{
    // Some fields come to DerivedClass from MetaData class.

    // Mandatory Fields defined here, specific to the derived class.
    // These fields are not optional and must be present in every instance of the derived class. These are accessed using .x / .y / .z etc. syntax.
    uint32_t x,y,z; //All the mandatory fields of the derived class.

    Optional64 o;

    ADD_OPTIONAL_PROPERTY(property1, uint32_t;
    ADD_OPTIONAL_PROPERTY(property2, char);
    ADD_OPTIONAL_PROPERTY(property3, float);
    ADD_OPTIONAL_PROPERTY(property4, int64_t);
    ADD_OPTIONAL_PROPERTY(property5, ByteArrayData);
    // Maximum 64 properties can be defined, in any sequence.

    // Either we can have a macro to define the properties, or we can use a template function to add properties. Both case need to have option to specify types.
    // The macro will automatically map it to the correct storage bucket / offset based on sizeof().
    // Above macro should automatically create an enum PropertyNames with all the properties defined above. Like enum PropertyNames{property1, property2, ...}.
    // Compile time assertion that not more than 64 optional properties are defined.
}
*/
