// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once

#define _USE_MATH_DEFINES // For M_PI
#include <cmath>
#include <vector>
#include <iostream>
#include "डेटा.h"
#include <random>
constexpr float M_PI = 3.1415926535f; // TODO: Why it's not coming from cmath library ?

// Most generic 3D point structure with double precision.
// TODO: change float to double, and for return Replace with DirectXMath XMFLOAT3 or XMFLOAT4
struct xyz32 { float x=0, y=0, z=0; };

// Helper function to get a random number generator
inline std::mt19937& GetRNG() {
    static std::mt19937 rng(static_cast<unsigned>(std::time(nullptr)));
    return rng;
}

// The most basic 3D Shapes.: Pyramid, Cuboid, Cone, Cylinder, Parallelepiped, Sphere
struct PYRAMID :public META_DATA{
    //Mandatory Fields
    std::vector<XMFLOAT3> vertices; // Index 0,1,2 for base, 3 for apex
    std::vector<XMFLOAT4> colors; // RGBA format.

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
    std::vector<XMFLOAT3> vertices; // 8 corner vertices
    std::vector<XMFLOAT4> colors;   // 8 colors, one for each vertex

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    void Randomize();
    GeometryData GetGeometry();
};

struct CONE :public META_DATA {
    //Mandatory Fields
    XMFLOAT3 apex;
    XMFLOAT3 baseCenter;
    float radius;
    std::vector<XMFLOAT4> colors; // 38 colors: 1 for apex, 1 for base center, 36 for base edge

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    void Randomize();
    GeometryData GetGeometry();
};

struct CYLINDER :public META_DATA {
    //Mandatory Fields
    XMFLOAT3 p1, p2; // Center points of the two circular bases
    float radius;
    std::vector<XMFLOAT4> colors; // 74 colors: 1 for each base center, 36 for each base edge

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    void Randomize();
    GeometryData GetGeometry();
};

struct PARALLELEPIPED :public META_DATA {
    //Mandatory Fields
    std::vector<XMFLOAT3> vertices; // 8 vertices
    std::vector<XMFLOAT4> colors;   // 8 colors

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    void Randomize();
    GeometryData GetGeometry();
};

struct SPHERE :public META_DATA {
    //Mandatory Fields
    XMFLOAT3 center;
    float radius;
    std::vector<XMFLOAT4> colors;

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    void Randomize();
    GeometryData GetGeometry();
};

struct FRUSTUM_OF_PYRAMID :public META_DATA {
    //Mandatory Fields
    std::vector<XMFLOAT3> vertices; // 8 vertices: 4 for bottom base, 4 for top base
    std::vector<XMFLOAT4> colors;   // 8 colors

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    void Randomize();
    GeometryData GetGeometry();
};

struct  FRUSTUM_OF_CONE :public META_DATA {
    //Mandatory Fields
    XMFLOAT3 bottomCenter, topCenter;
    float bottomRadius, topRadius;
    std::vector<XMFLOAT4> colors; // 74 colors: 1 for each center, 36 for each edge

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    void Randomize();
    GeometryData GetGeometry();
};

/************************* IMPLEMENTATIONS *************************/
//PYRAMID
inline GeometryData PYRAMID::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.vertices.resize(4);
    geometry.vertices = {
        Vertex{ vertices[0], colors[0] }, Vertex{ vertices[1], colors[1] },
        Vertex{ vertices[2], colors[2] }, Vertex{ vertices[3], colors[3] }
    };
    geometry.indices.resize(12);
	geometry.indices = { 0, 1, 2, 0, 3, 1, 1, 3, 2, 2, 3, 0 }; //1st triangle is base, then 3 sides.
    //geometry.indices = { 0, 1, 3, 1, 2, 3, 2, 0, 3, 2, 1, 0 }; // Sides and base with correct winding
    return geometry;
}

inline void PYRAMID::Randomize() {
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> sizeDist(0.2f, 1.0f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);

    // Random properties for the new pyramid
    auto& rng = GetRNG();
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

// CUBOID
inline void CUBOID::Randomize() {
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> sizeDist(0.1f, 0.5f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);
    auto& rng = GetRNG();

    float cx = posDist(rng);
    float cy = posDist(rng);
    float cz = posDist(rng);
    float sx = sizeDist(rng) * 0.5f;
    float sy = sizeDist(rng) * 0.5f;
    float sz = sizeDist(rng) * 0.5f;

    vertices = {
        XMFLOAT3(cx - sx, cy - sy, cz - sz), XMFLOAT3(cx + sx, cy - sy, cz - sz),
        XMFLOAT3(cx + sx, cy + sy, cz - sz), XMFLOAT3(cx - sx, cy + sy, cz - sz),
        XMFLOAT3(cx - sx, cy - sy, cz + sz), XMFLOAT3(cx + sx, cy - sy, cz + sz),
        XMFLOAT3(cx + sx, cy + sy, cz + sz), XMFLOAT3(cx - sx, cy + sy, cz + sz)
    };

    colors.resize(8);
    for (int i = 0; i < 8; ++i) {
        colors[i] = XMFLOAT4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    }
}

inline GeometryData CUBOID::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.vertices.resize(8);
    for (size_t i = 0; i < 8; ++i) {
        geometry.vertices[i] = { vertices[i], colors[i] };
    }

    geometry.indices = {
        0, 3, 2, 0, 2, 1, // Front face
        4, 5, 6, 4, 6, 7, // Back face
        4, 7, 3, 4, 3, 0, // Left face
        1, 2, 6, 1, 6, 5, // Right face
        3, 7, 6, 3, 6, 2, // Top face
        0, 1, 5, 0, 5, 4  // Bottom face
    };
    return geometry;
}

// CONE
inline void CONE::Randomize() {
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> sizeDist(0.1f, 0.5f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);
    auto& rng = GetRNG();

    baseCenter = { posDist(rng), posDist(rng), posDist(rng) };
    radius = sizeDist(rng);
    float height = sizeDist(rng);
    apex = { baseCenter.x, baseCenter.y + height, baseCenter.z };

    colors.resize(38); // 1 apex + 1 base center + 36 base edge
    for (size_t i = 0; i < colors.size(); ++i) {
        colors[i] = XMFLOAT4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    }
}

inline GeometryData CONE::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    const int numSegments = 36;

    geometry.vertices.resize(numSegments + 2);
    geometry.vertices[0] = { apex, colors[0] };
    geometry.vertices[1] = { baseCenter, colors[1] };

    for (int i = 0; i < numSegments; ++i) {
        float angle = 2.0f * (float)M_PI * i / numSegments;
        XMFLOAT3 pos = { baseCenter.x + radius * cos(angle), baseCenter.y, baseCenter.z + radius * sin(angle) };
        geometry.vertices[i + 2] = { pos, colors[i + 2] };
    }

    for (int i = 0; i < numSegments; ++i) {
        // Side surface triangles
        geometry.indices.push_back(0);
        geometry.indices.push_back(i + 2);
        geometry.indices.push_back(((i + 1) % numSegments) + 2);

        // Base surface triangles
        geometry.indices.push_back(1);
        geometry.indices.push_back(((i + 1) % numSegments) + 2);
        geometry.indices.push_back(i + 2);
    }
    return geometry;
}

// CYLINDER
inline void CYLINDER::Randomize() {
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> sizeDist(0.1f, 0.5f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);
    auto& rng = GetRNG();

    p1 = { posDist(rng), posDist(rng), posDist(rng) };
    //p2 = { p1.x + sizeDist(rng), p1.y + sizeDist(rng), p1.z + sizeDist(rng) };
    p2 = { p1.x , p1.y + sizeDist(rng), p1.z  };
    radius = sizeDist(rng) * 0.5f;

    colors.resize(74); // 2 centers + 2 * 36 edges
    for (size_t i = 0; i < colors.size(); ++i) {
        colors[i] = XMFLOAT4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    }
}

inline GeometryData CYLINDER::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    const int numSegments = 36;

    // Define vertices for both caps
    geometry.vertices.resize(2 * numSegments + 2);
    geometry.vertices[0] = { p1, colors[0] }; // Bottom center
    geometry.vertices[numSegments + 1] = { p2, colors[1] }; // Top center

    for (int i = 0; i < numSegments; ++i) {
        float angle = 2.0f * (float)M_PI * i / numSegments;
        // Bottom cap vertices
        geometry.vertices[i + 1] = { XMFLOAT3(p1.x + radius * cos(angle), p1.y, p1.z + radius * sin(angle)), colors[i + 2] };
        // Top cap vertices
        geometry.vertices[i + numSegments + 2] = { XMFLOAT3(p2.x + radius * cos(angle), p2.y, p2.z + radius * sin(angle)), colors[i + 38] };
    }

    // Define indices
    for (int i = 0; i < numSegments; ++i) {
        int next_i = (i + 1) % numSegments;
        // Bottom cap
        geometry.indices.push_back(0);
        geometry.indices.push_back(next_i + 1);
        geometry.indices.push_back(i + 1);
        // Top cap
        geometry.indices.push_back(numSegments + 1);
        geometry.indices.push_back(i + numSegments + 2);
        geometry.indices.push_back(next_i + numSegments + 2);
        // Side wall
        geometry.indices.push_back(i + 1);
        geometry.indices.push_back(next_i + 1);
        geometry.indices.push_back(i + numSegments + 2);
        geometry.indices.push_back(next_i + 1);
        geometry.indices.push_back(next_i + numSegments + 2);
        geometry.indices.push_back(i + numSegments + 2);
    }
    return geometry;
}

// PARALLELEPIPED
inline void PARALLELEPIPED::Randomize() {
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> vecDist(-0.5f, 0.5f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);
    auto& rng = GetRNG();

    XMFLOAT3 origin = { posDist(rng), posDist(rng), posDist(rng) };
    XMFLOAT3 vecA = { vecDist(rng), vecDist(rng), vecDist(rng) };
    XMFLOAT3 vecB = { vecDist(rng), vecDist(rng), vecDist(rng) };
    XMFLOAT3 vecC = { vecDist(rng), vecDist(rng), vecDist(rng) };

    vertices.resize(8);
    vertices[0] = origin;
    vertices[1] = { origin.x + vecA.x, origin.y + vecA.y, origin.z + vecA.z };
    vertices[2] = { origin.x + vecB.x, origin.y + vecB.y, origin.z + vecB.z };
    vertices[3] = { origin.x + vecC.x, origin.y + vecC.y, origin.z + vecC.z };
    vertices[4] = { origin.x + vecA.x + vecB.x, origin.y + vecA.y + vecB.y, origin.z + vecA.z + vecB.z };
    vertices[5] = { origin.x + vecA.x + vecC.x, origin.y + vecA.y + vecC.y, origin.z + vecA.z + vecC.z };
    vertices[6] = { origin.x + vecB.x + vecC.x, origin.y + vecB.y + vecC.y, origin.z + vecB.z + vecC.z };
    vertices[7] = { origin.x + vecA.x + vecB.x + vecC.x, origin.y + vecA.y + vecB.y + vecC.y, origin.z + vecA.z + vecB.z + vecC.z };

    colors.resize(8);
    for (int i = 0; i < 8; ++i) {
        colors[i] = XMFLOAT4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    }
}

inline GeometryData PARALLELEPIPED::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.vertices.resize(8);
    for (size_t i = 0; i < 8; ++i) {
        geometry.vertices[i] = { vertices[i], colors[i] };
    }
    // Indices are based on vertex combinations
    geometry.indices = {
        0,2,4, 0,4,1, // Face defined by vecA, vecB
        0,3,2,       // Face defined by vecB, vecC (part 1)
        3,6,2,       // Face defined by vecB, vecC (part 2)
        0,1,5, 0,5,3, // Face defined by vecA, vecC
        7,5,1, 7,1,4, // Opposite of vecB, vecC
        7,6,3, 7,3,5, // Opposite of vecA, vecB
        7,4,2, 7,2,6  // Opposite of vecA, vecC
    };
    return geometry;
}

// SPHERE
inline void SPHERE::Randomize() {
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> sizeDist(0.1f, 0.5f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);
    auto& rng = GetRNG();

    center = { posDist(rng), posDist(rng), posDist(rng) };
    radius = sizeDist(rng);

    // Color randomization is handled in GetGeometry as vertex count is determined there
}

inline GeometryData SPHERE::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;

    // Using 36 longitude slices and 18 latitude stacks for a reasonable vertex count
    const int sliceCount = 36;
    const int stackCount = 18;

    // Add top pole vertex
    geometry.vertices.push_back({ XMFLOAT3(center.x, center.y + radius, center.z), XMFLOAT4(1,1,1,1) });

    for (int i = 1; i < stackCount; ++i) {
        float phi = (float)M_PI * i / stackCount;
        for (int j = 0; j < sliceCount; ++j) {
            float theta = 2.0f * (float)M_PI * j / sliceCount;
            XMFLOAT3 pos = {
                center.x + radius * sin(phi) * cos(theta),
                center.y + radius * cos(phi),
                center.z + radius * sin(phi) * sin(theta)
            };
            geometry.vertices.push_back({ pos, XMFLOAT4(1,1,1,1) });
        }
    }
    // Add bottom pole vertex
    geometry.vertices.push_back({ XMFLOAT3(center.x, center.y - radius, center.z), XMFLOAT4(1,1,1,1) });

    // Randomize colors for all generated vertices
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);
    auto& rng = GetRNG();
    for (auto& v : geometry.vertices) {
        v.color = XMFLOAT4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    }

    // Top cap indices
    for (int i = 0; i < sliceCount; ++i) {
        geometry.indices.push_back(0);
        geometry.indices.push_back(i + 1);
        geometry.indices.push_back(((i + 1) % sliceCount) + 1);
    }

    // Middle stacks indices
    for (int i = 0; i < stackCount - 2; ++i) {
        for (int j = 0; j < sliceCount; ++j) {
            int next_j = (j + 1) % sliceCount;
            int r0 = 1 + i * sliceCount;
            int r1 = 1 + (i + 1) * sliceCount;
            // Quad
            geometry.indices.push_back(r0 + j);
            geometry.indices.push_back(r0 + next_j);
            geometry.indices.push_back(r1 + j);

            geometry.indices.push_back(r0 + next_j);
            geometry.indices.push_back(r1 + next_j);
            geometry.indices.push_back(r1 + j);
        }
    }

    // Bottom cap indices
    int bottomPoleIndex = (int)geometry.vertices.size() - 1;
    int lastStackStartIndex = 1 + (stackCount - 2) * sliceCount;
    for (int i = 0; i < sliceCount; ++i) {
        geometry.indices.push_back(bottomPoleIndex);
        geometry.indices.push_back(((i + 1) % sliceCount) + lastStackStartIndex);
        geometry.indices.push_back(i + lastStackStartIndex);
    }

    return geometry;
}

// FRUSTUM_OF_PYRAMID
inline void FRUSTUM_OF_PYRAMID::Randomize() {
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> sizeDist(0.1f, 0.5f);
    std::uniform_real_distribution<float> ratioDist(0.2f, 0.8f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);
    auto& rng = GetRNG();

    float cx = posDist(rng);
    float cy = posDist(rng);
    float cz = posDist(rng);
    float height = sizeDist(rng);
    float bottomSize = sizeDist(rng);
    float topSize = bottomSize * ratioDist(rng);

    vertices.resize(8);
    // Bottom base (counter-clockwise)
    vertices[0] = { cx - bottomSize, cy, cz - bottomSize };
    vertices[1] = { cx + bottomSize, cy, cz - bottomSize };
    vertices[2] = { cx + bottomSize, cy, cz + bottomSize };
    vertices[3] = { cx - bottomSize, cy, cz + bottomSize };
    // Top base
    vertices[4] = { cx - topSize, cy + height, cz - topSize };
    vertices[5] = { cx + topSize, cy + height, cz - topSize };
    vertices[6] = { cx + topSize, cy + height, cz + bottomSize };
    vertices[7] = { cx - topSize, cy + height, cz + bottomSize };

    colors.resize(8);
    for (int i = 0; i < 8; ++i) {
        colors[i] = XMFLOAT4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    }
}

inline GeometryData FRUSTUM_OF_PYRAMID::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.vertices.resize(8);
    for (size_t i = 0; i < 8; ++i) {
        geometry.vertices[i] = { vertices[i], colors[i] };
    }

    geometry.indices = {
        // Bottom face
        0, 2, 1, 0, 3, 2,
        // Top face
        4, 5, 6, 4, 6, 7,
        // Side faces
        0, 1, 5, 0, 5, 4,
        1, 2, 6, 1, 6, 5,
        2, 3, 7, 2, 7, 6,
        3, 0, 4, 3, 4, 7
    };
    return geometry;
}


// FRUSTUM_OF_CONE
inline void FRUSTUM_OF_CONE::Randomize() {
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> sizeDist(0.1f, 0.5f);
    std::uniform_real_distribution<float> ratioDist(0.2f, 0.8f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);
    auto& rng = GetRNG();

    bottomCenter = { posDist(rng), posDist(rng), posDist(rng) };
    float height = sizeDist(rng);
    topCenter = { bottomCenter.x, bottomCenter.y + height, bottomCenter.z };
    bottomRadius = sizeDist(rng);
    topRadius = bottomRadius * ratioDist(rng);

    colors.resize(74); // 2 centers + 2 * 36 edges
    for (size_t i = 0; i < colors.size(); ++i) {
        colors[i] = XMFLOAT4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    }
}

inline GeometryData FRUSTUM_OF_CONE::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    const int numSegments = 36;

    // Define vertices
    geometry.vertices.resize(2 * numSegments + 2);
    geometry.vertices[0] = { bottomCenter, colors[0] }; // Bottom center
    geometry.vertices[numSegments + 1] = { topCenter, colors[1] }; // Top center

    for (int i = 0; i < numSegments; ++i) {
        float angle = 2.0f * (float) M_PI * i / numSegments;
        // Bottom cap vertices
        geometry.vertices[i + 1] = { XMFLOAT3(bottomCenter.x + bottomRadius * cos(angle), bottomCenter.y, bottomCenter.z + bottomRadius * sin(angle)), colors[i + 2] };
        // Top cap vertices
        geometry.vertices[i + numSegments + 2] = { XMFLOAT3(topCenter.x + topRadius * cos(angle), topCenter.y, topCenter.z + topRadius * sin(angle)), colors[i + 38] };
    }

    // Define indices
    for (int i = 0; i < numSegments; ++i) {
        int next_i = (i + 1) % numSegments;
        // Bottom cap
        geometry.indices.push_back(0);
        geometry.indices.push_back(next_i + 1);
        geometry.indices.push_back(i + 1);
        // Top cap
        geometry.indices.push_back(numSegments + 1);
        geometry.indices.push_back(i + numSegments + 2);
        geometry.indices.push_back(next_i + numSegments + 2);
        // Side wall
        geometry.indices.push_back(i + 1);
        geometry.indices.push_back(next_i + 1);
        geometry.indices.push_back(i + numSegments + 2);

        geometry.indices.push_back(next_i + 1);
        geometry.indices.push_back(next_i + numSegments + 2);
        geometry.indices.push_back(i + numSegments + 2);
    }
    return geometry;
}
