// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once

#include <vector>
#include <iostream>
#include "डेटा.h"
#include <random>

// Most generic 3D point structure with double precision.
// TODO: change float to double, and for return Replace with DirectXMath XMFLOAT3 or XMFLOAT4
struct xyz32 { float x=0, y=0, z=0; };

// The most basic 3D Shapes.: Pyramid, Cuboid, Cone, Cylinder, Parallelepiped, Sphere
struct PYRAMID :public META_DATA{
    //Mandatory Fields
    std::vector<XMFLOAT3> vertices; // Index 0,1,2 for base, 3 for apex
    std::vector<XMFLOAT4> colors; // RGBA format.
    std::vector<uint16_t> indexes;

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;

    PYRAMID() {};
	void Randomize(); // Assign random position, size, colors etc.
    GeometryData GetGeometry(); // Simply returns the vertices with colors and indexes.
    void CalculateGeometry() {}; // Calculate the geometry, potentially taking into account cutouts.
};

struct CUBOID :public META_DATA {
    //Mandatory Fields

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};

struct CONE :public META_DATA {
    //Mandatory Fields

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};

struct CYLINDER :public META_DATA {
    //Mandatory Fields
    double x1, y1, z1, x2, y2, z2;
    float radius;

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};

struct PARALLELEPIPED :public META_DATA {
    //Mandatory Fields

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};

struct SPHERE :public META_DATA {
    //Mandatory Fields
    double x, y, z, radius;

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};

struct FRUSTUM_OF_PYRAMID :public META_DATA {
    //Mandatory Fields

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};

struct  FRUSTUM_OF_CONE :public META_DATA {
    //Mandatory Fields

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};

/************************* IMLIMENTATIONS *************************/
inline GeometryData PYRAMID::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.vertices.resize(4);
    geometry.vertices = {
        Vertex{ vertices[0], colors[0] },Vertex{ vertices[1], colors[1] },
        Vertex{ vertices[2], colors[2] },Vertex{ vertices[3], colors[3] }
    };
    geometry.indices.resize(12);
	geometry.indices = { 0, 1, 2, 0, 3, 1, 1, 3, 2, 2, 3, 0 }; //1st triangle is base, then 3 sides.
    return geometry;
}

inline void PYRAMID::Randomize() {
    // Initialize random number generator (persistent)
    static std::mt19937 rng(static_cast<unsigned>(std::time(nullptr)));
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> sizeDist(0.2f, 1.0f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);

    // Random properties for the new pyramid
    float centerX = posDist(rng);
    float centerY = posDist(rng);
    float centerZ = posDist(rng);
    float pyramidSize = sizeDist(rng);

    vertices = { 
        XMFLOAT3(centerX - pyramidSize * 0.5f, centerY - pyramidSize * 0.5f, centerZ + pyramidSize * 0.5f),
        XMFLOAT3(centerX + pyramidSize * 0.5f, centerY - pyramidSize * 0.5f, centerZ + pyramidSize * 0.5f),
        XMFLOAT3(centerX, centerY - pyramidSize * 0.5f, centerZ - pyramidSize * 0.5f),
        XMFLOAT3(centerX, centerY + pyramidSize * 0.8f, centerZ)
    };

    colors = {XMFLOAT4 (colorDist(rng), colorDist(rng), colorDist(rng), 1.0f),
        XMFLOAT4 (colorDist(rng), colorDist(rng), colorDist(rng), 1.0f),
        XMFLOAT4 (colorDist(rng), colorDist(rng), colorDist(rng), 1.0f),
        XMFLOAT4 (colorDist(rng), colorDist(rng), colorDist(rng), 1.0f)
    };
}

