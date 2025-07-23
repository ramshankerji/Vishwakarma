// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

#include <vector>
#include <iostream>

#include "डेटा.h"

// The most basic 3D Shapes.: Pyramid, Cuboid, Cone, Cylinder, Parallelepiped, Sphere

struct STRUCTURAL_SIMPLE_MEMBER {
    META_DATA metaData;

    //Mandatory Fields
    double x1, y1, z1, x2, y2, z2;
    uint64_t profileID; //Pointer to the profile type

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    c_float profileParameters;
};

struct STRUCTURAL_CURVED_MEMBER {
    META_DATA metaData;

    //Mandatory Fields
    double x, y, z, radius;
    uint64_t profileID; //Pointer to the profile type

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    c_float profileParameters;
};

struct STRUCTURAL_POLY_MEMBER {
    META_DATA metaData;

    //Mandatory Fields
    double x1, y1, z1, x2, y2, z2;
    uint64_t profileID; //Pointer to the profile type

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    c_float profileParameters;
};

struct PLANER_2D { //SLABS, GRATINGS, PLATES, CHEQUERED PLATES, OR ANY PLATE  IN A SINGLE PLANE.
    META_DATA metaData;

    //Mandatory Fields
    double x1, y1, z1, x2, y2, z2;

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    c_float profileParameters;
};