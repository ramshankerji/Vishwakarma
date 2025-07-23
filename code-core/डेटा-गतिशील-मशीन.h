// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

#include <vector>
#include <iostream>

#include "डेटा.h"

struct PUMP_3D {
    META_DATA metaData;

    //Mandatory Fields

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};

struct AIR_COOLER_3D {
    META_DATA metaData;

    //Mandatory Fields

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};

struct COMPRESSOR_3D {
    META_DATA metaData;

    //Mandatory Fields

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};
