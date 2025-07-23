// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

#include <vector>
#include <iostream>

#include "डेटा.h"

// The most basic 3D Shapes.: Pyramid, Cuboid, Cone, Cylinder, Parallelepiped, Sphere
struct VERTICAL_VESSEL_3D { //Vertical pressure vessel, Including Reactors.
    META_DATA metaData;

    //Mandatory Fields

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bit-mask.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    std::string name;
};

struct HORIZONTAL_VESSEL_3D {
    META_DATA metaData;

    //Mandatory Fields
    
    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bit-mask.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    std::string name;
};

struct VERTICAL_EXCHANGER_3D {
    META_DATA metaData;

    //Mandatory Fields
    
    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bit-mask.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    std::string name;
};

struct HORIZONTAL_EXCHANGER_3D {
    META_DATA metaData;

    //Mandatory Fields
    
    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bit-mask.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    std::string name;
};

struct FILTERS_VERTICAL_3D {
    META_DATA metaData;

    //Mandatory Fields
    
    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bit-mask.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    std::string name;
};
