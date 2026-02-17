// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once

#define _USE_MATH_DEFINES // For M_PI
#include <cmath>
#include <vector>
#include <iostream>
#include <d3d12.h>
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
    std::vector<XMHALF4> colors; // RGBA format.

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
    XMHALF4 colors;   // Common color for all faces.

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
    XMHALF4 colorBase, colorIncline; // Cone has only 2 surface.

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
    XMHALF4 colorBase, colorTop, colorIncline; // 1 Unique color for each surface.

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
    XMHALF4 colors;   // common color for entire object. 

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
    XMHALF4 color; // SInce entire sphere is 1 surface, it has got only 1 color.

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
    XMHALF4 colorBase, colorTop, colorIncline; // There are 3 unique type of surfaces on a pyramid frustum.

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
    XMHALF4 colorBase, colorTop, colorIncline; // Cone has only 3 surface.

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    void Randomize();
    GeometryData GetGeometry();
};

struct PIPE : public META_DATA {

    // Mandatory Fields
    XMFLOAT3 center1;
    XMFLOAT3 center2;
    float outsideDiameter;
    float insideDiameter;

    XMHALF4 colorOuter;
    XMHALF4 colorInner;
    XMHALF4 colorCap;

    // Optional Fields
    uint64_t optionalFieldsFlags;
    uint32_t systemFlags;
    uint32_t objectLifeCycleFlags;
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

    // Calculate the geometric centroid of the pyramid.
    // We use this to calculate an outward-facing normal for each corner vertex.
    // This technique effectively simulates smooth shading. 
    // (For hard-edge flat shading, vertices would need to be split/duplicated).
    XMVECTOR v0 = XMLoadFloat3(&vertices[0]);
    XMVECTOR v1 = XMLoadFloat3(&vertices[1]);
    XMVECTOR v2 = XMLoadFloat3(&vertices[2]);
    XMVECTOR v3 = XMLoadFloat3(&vertices[3]);

    XMVECTOR centroidVec = (v0 + v1 + v2 + v3) / 4.0f;

    // Lambda helper to calculate and pack the normal
    auto GetCentroidNormal = [&](XMVECTOR vertexPos) -> XMUBYTE4 {
        XMFLOAT3 normalFloat;
        // Normal is the direction from centroid to the vertex
        XMStoreFloat3(&normalFloat, XMVector3Normalize(vertexPos - centroidVec));
        return PackNormal(normalFloat);
        };

    // Construct vertices with Position, Normal, and Color
    // Since we are using common vertex between different surfaces, Intentionally assigning colors[0] for uniformity.
    geometry.vertices[0] = Vertex{ vertices[0], GetCentroidNormal(v0), colors[0] };
    geometry.vertices[1] = Vertex{ vertices[1], GetCentroidNormal(v1), colors[0] };
    geometry.vertices[2] = Vertex{ vertices[2], GetCentroidNormal(v2), colors[0] };
    geometry.vertices[3] = Vertex{ vertices[3], GetCentroidNormal(v3), colors[0] };

    geometry.indices.resize(12);
    geometry.indices = { 0, 1, 2, 0, 3, 1, 1, 3, 2, 2, 3, 0 }; // //1st triangle is base, then 3 sides.
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

    colors = {XMHALF4 (colorDist(rng), colorDist(rng), colorDist(rng), 1.0f),
        XMHALF4 (colorDist(rng), colorDist(rng), colorDist(rng), 1.0f),
        XMHALF4 (colorDist(rng), colorDist(rng), colorDist(rng), 1.0f),
        XMHALF4 (colorDist(rng), colorDist(rng), colorDist(rng), 1.0f)
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
        XMFLOAT3(cx - sx, cy - sy, cz - sz), // 0
        XMFLOAT3(cx + sx, cy - sy, cz - sz), // 1
        XMFLOAT3(cx + sx, cy + sy, cz - sz), // 2
        XMFLOAT3(cx - sx, cy + sy, cz - sz), // 3
        XMFLOAT3(cx - sx, cy - sy, cz + sz), // 4
        XMFLOAT3(cx + sx, cy - sy, cz + sz), // 5
        XMFLOAT3(cx + sx, cy + sy, cz + sz), // 6
        XMFLOAT3(cx - sx, cy + sy, cz + sz)  // 7
    };

    // Single common color for entire cuboid
    colors = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
}

// CUBOID
/*Vertex bifurcation applied:- No shared vertices- Each face has independent vertices
- Proper flat shading- No centroid-based approximation */
inline GeometryData CUBOID::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.vertices.clear();
    geometry.indices.clear();
    geometry.vertices.reserve(24);
    geometry.indices.reserve(36);

    // Lambda to add flat shaded quad
    auto AddFace = [&](int i0, int i1, int i2, int i3)
        {
            XMVECTOR v0 = XMLoadFloat3(&vertices[i0]);
            XMVECTOR v1 = XMLoadFloat3(&vertices[i1]);
            XMVECTOR v2 = XMLoadFloat3(&vertices[i2]);
            XMVECTOR normal = XMVector3Normalize( XMVector3Cross(v1 - v0, v2 - v0));// Face normal (flat)
            XMFLOAT3 normalFloat;
            XMStoreFloat3(&normalFloat, normal);
            XMUBYTE4 packedNormal = PackNormal(normalFloat);
            uint16_t base = static_cast<uint16_t>(geometry.vertices.size());

            geometry.vertices.push_back(Vertex{ vertices[i0], packedNormal, colors });
            geometry.vertices.push_back(Vertex{ vertices[i1], packedNormal, colors });
            geometry.vertices.push_back(Vertex{ vertices[i2], packedNormal, colors });
            geometry.vertices.push_back(Vertex{ vertices[i3], packedNormal, colors });

            geometry.indices.insert(geometry.indices.end(), {
                static_cast<uint16_t>(base + 0), static_cast<uint16_t>(base + 1),
                static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 0),
                static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)
            });
        };

    // Faces (consistent outward winding)
    AddFace(0, 3, 2, 1); // Front (-Z)
    AddFace(4, 5, 6, 7); // Back  (+Z)
    AddFace(4, 7, 3, 0); // Left  (-X)
    AddFace(1, 2, 6, 5); // Right (+X)
    AddFace(3, 7, 6, 2); // Top   (+Y)
    AddFace(0, 1, 5, 4); // Bottom(-Y)
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

    colorBase = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorIncline = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
}

// CONE
/*Vertex bifurcation applied:- No shared vertices- Each triangle has independent vertices
- Proper flat shading- No centroid-based approximation */
inline GeometryData CONE::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    const int numSegments = 36;
    geometry.vertices.clear();
    geometry.indices.clear();

    // Reserve enough space. Side: 36 triangles → 108 vertices. Base: 36 triangles → 108 vertices
    geometry.vertices.reserve(numSegments * 6);
    geometry.indices.reserve(numSegments * 6);

    // Lambda to add a flat shaded triangle
    auto AddTriangle = [&](const XMFLOAT3& a,
        const XMFLOAT3& b,
        const XMFLOAT3& c,
        const XMHALF4& color)
        {
            XMVECTOR v0 = XMLoadFloat3(&a);
            XMVECTOR v1 = XMLoadFloat3(&b);
            XMVECTOR v2 = XMLoadFloat3(&c);
            XMVECTOR normal = XMVector3Normalize( XMVector3Cross(v1 - v0, v2 - v0));
            XMFLOAT3 normalFloat;
            XMStoreFloat3(&normalFloat, normal);
            XMUBYTE4 packedNormal = PackNormal(normalFloat);

            uint16_t base = static_cast<uint16_t>(geometry.vertices.size());

            geometry.vertices.push_back(Vertex{ a, packedNormal, color });
            geometry.vertices.push_back(Vertex{ b, packedNormal, color });
            geometry.vertices.push_back(Vertex{ c, packedNormal, color });

            geometry.indices.push_back(base + 0);
            geometry.indices.push_back(base + 1);
            geometry.indices.push_back(base + 2);
        };

    // Generate geometry
    for (int i = 0; i < numSegments; ++i) {
        int next = (i + 1) % numSegments;

        float a0 = 2.0f * XM_PI * i / numSegments;
        float a1 = 2.0f * XM_PI * next / numSegments;

        float c0 = cosf(a0);
        float s0 = sinf(a0);
        float c1 = cosf(a1);
        float s1 = sinf(a1);

        // Rim points
        XMFLOAT3 r0 = { baseCenter.x + radius * c0, baseCenter.y, baseCenter.z + radius * s0 };
        XMFLOAT3 r1 = { baseCenter.x + radius * c1, baseCenter.y, baseCenter.z + radius * s1 };

        AddTriangle(apex, r0, r1, colorIncline);// Side surface triangle (flat shaded)
        AddTriangle(baseCenter, r1, r0, colorBase);// Base surface triangle (flat shaded, downward facing via winding)
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

    colorBase    = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorTop     = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorIncline = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    
}

// CYLINDER
/* Vertex bifurcation applied:- No shared vertices- Each triangle/quad has independent vertices
- Proper flat shading- No centroid-based fake normals */
inline GeometryData CYLINDER::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    const int numSegments = 36;
    geometry.vertices.clear();
    geometry.indices.clear();
    geometry.vertices.reserve(numSegments * (3 + 3 + 4));// Reserve enough space
    geometry.indices.reserve(numSegments * (3 + 3 + 6));

    // Axis direction (for correct cap orientation reference if needed)
    XMVECTOR vP1 = XMLoadFloat3(&p1);
    XMVECTOR vP2 = XMLoadFloat3(&p2);
    XMVECTOR axis = XMVector3Normalize(vP2 - vP1);

    // Lambda to add a flat-shaded triangle
    auto AddTriangle = [&](const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c,
        const XMHALF4& color)
        {
            XMVECTOR v0 = XMLoadFloat3(&a);
            XMVECTOR v1 = XMLoadFloat3(&b);
            XMVECTOR v2 = XMLoadFloat3(&c);

            XMVECTOR normal = XMVector3Normalize(
                XMVector3Cross(v1 - v0, v2 - v0));

            XMFLOAT3 normalFloat;
            XMStoreFloat3(&normalFloat, normal);
            XMUBYTE4 packedNormal = PackNormal(normalFloat);

            uint16_t base = static_cast<uint16_t>(geometry.vertices.size());

            geometry.vertices.push_back(Vertex{ a, packedNormal, color });
            geometry.vertices.push_back(Vertex{ b, packedNormal, color });
            geometry.vertices.push_back(Vertex{ c, packedNormal, color });

            geometry.indices.push_back(base + 0);
            geometry.indices.push_back(base + 1);
            geometry.indices.push_back(base + 2);
        };

    // Lambda to add a flat-shaded quad (two triangles)
    auto AddQuad = [&](const XMFLOAT3& a,
        const XMFLOAT3& b,
        const XMFLOAT3& c,
        const XMFLOAT3& d,
        const XMHALF4& color)
        {
            XMVECTOR v0 = XMLoadFloat3(&a);
            XMVECTOR v1 = XMLoadFloat3(&b);
            XMVECTOR v2 = XMLoadFloat3(&c);
            XMVECTOR normal = XMVector3Normalize( XMVector3Cross(v1 - v0, v2 - v0));
            XMFLOAT3 normalFloat;
            XMStoreFloat3(&normalFloat, normal);
            XMUBYTE4 packedNormal = PackNormal(normalFloat);
            uint16_t base = static_cast<uint16_t>(geometry.vertices.size());

            geometry.vertices.push_back(Vertex{ a, packedNormal, color });
            geometry.vertices.push_back(Vertex{ b, packedNormal, color });
            geometry.vertices.push_back(Vertex{ c, packedNormal, color });
            geometry.vertices.push_back(Vertex{ d, packedNormal, color });

            geometry.indices.insert(geometry.indices.end(), {
                static_cast<uint16_t>(base + 0), static_cast<uint16_t>(base + 1),
                static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 0),
                static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)
            });
        };

    // Generate geometry
    for (int i = 0; i < numSegments; ++i) {
        int next = (i + 1) % numSegments;

        float a0 = 2.0f * XM_PI * i / numSegments;
        float a1 = 2.0f * XM_PI * next / numSegments;

        float c0 = cosf(a0);
        float s0 = sinf(a0);
        float c1 = cosf(a1);
        float s1 = sinf(a1);

        XMFLOAT3 b0 = { p1.x + radius * c0, p1.y, p1.z + radius * s0 };// Bottom rim points
        XMFLOAT3 b1 = { p1.x + radius * c1, p1.y, p1.z + radius * s1 };
        XMFLOAT3 t0 = { p2.x + radius * c0, p2.y, p2.z + radius * s0 };// Top rim points
        XMFLOAT3 t1 = { p2.x + radius * c1, p2.y, p2.z + radius * s1 };

        AddTriangle(p1, b1, b0, colorBase);// Bottom cap (flat, downward facing via winding)
        AddTriangle(p2, t0, t1, colorTop);// Top cap (flat, upward facing via winding)
        AddQuad(b0, b1, t1, t0, colorIncline);// Side wall (flat quad per segment)
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

    // Single common color for entire object
    colors = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
}

// PARALLELEPIPED
inline GeometryData PARALLELEPIPED::GetGeometry() {

    GeometryData geometry;
    geometry.id = memoryID;

    geometry.vertices.clear();
    geometry.indices.clear();

    geometry.vertices.reserve(24);
    geometry.indices.reserve(36);

    auto AddFace = [&](int i0, int i1, int i2, int i3)
    {
        XMVECTOR v0 = XMLoadFloat3(&vertices[i0]);
        XMVECTOR v1 = XMLoadFloat3(&vertices[i1]);
        XMVECTOR v2 = XMLoadFloat3(&vertices[i2]);

        XMVECTOR edge1 = v1 - v0;
        XMVECTOR edge2 = v2 - v0;
        XMVECTOR normal = XMVector3Normalize(XMVector3Cross(edge1, edge2));

        XMFLOAT3 normalFloat;
        XMStoreFloat3(&normalFloat, normal);
        XMUBYTE4 packedNormal = PackNormal(normalFloat);

        uint16_t baseIndex = static_cast<uint16_t>(geometry.vertices.size());

        geometry.vertices.push_back(Vertex{ vertices[i0], packedNormal, colors });
        geometry.vertices.push_back(Vertex{ vertices[i1], packedNormal, colors });
        geometry.vertices.push_back(Vertex{ vertices[i2], packedNormal, colors });
        geometry.vertices.push_back(Vertex{ vertices[i3], packedNormal, colors });

        geometry.indices.insert(geometry.indices.end(), {
            static_cast<uint16_t>(baseIndex + 0), static_cast<uint16_t>(baseIndex + 1),
            static_cast<uint16_t>(baseIndex + 2), static_cast<uint16_t>(baseIndex + 0),
            static_cast<uint16_t>(baseIndex + 2), static_cast<uint16_t>(baseIndex + 3)
        });
    };

    // Faces (consistent winding for outward normals)
    AddFace(0, 2, 4, 1); // vecA + vecB
    AddFace(0, 3, 6, 2); // vecB + vecC
    AddFace(0, 1, 5, 3); // vecA + vecC
    AddFace(7, 5, 1, 4); // opposite vecB+vecC
    AddFace(7, 6, 3, 5); // opposite vecA+vecB
    AddFace(7, 4, 2, 6); // opposite vecA+vecC

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
    color = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
}

/*For the sphere, the "suitable" normal for every vertex is exactly the normalized vector from the sphere's 
center to that vertex. This results in perfectly smooth shading across the surface. 
Color randomization integrated directly into the vertex generation loop to accommodate the new Vertex structure.*/
// SPHERE
/*
Updated implementation:
- No explicit pole vertices
- Full latitude rings generated (including top and bottom)
- Uniform quad-based indexing across stacks
- Smooth shading via true spherical normals
*/
inline GeometryData SPHERE::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    const int sliceCount = 36;   // Longitude
    const int stackCount = 18;   // Latitude
    geometry.vertices.clear();
    geometry.indices.clear();
    geometry.vertices.reserve((stackCount + 1) * sliceCount);
    geometry.indices.reserve(stackCount * sliceCount * 6);

    XMVECTOR vCenter = XMLoadFloat3(&center);

    // Helper for smooth spherical normal
    auto GetSphericalNormal = [&](XMVECTOR vertexPos) -> XMUBYTE4 {
        XMFLOAT3 normalFloat;
        XMStoreFloat3(&normalFloat, XMVector3Normalize(vertexPos - vCenter));
        return PackNormal(normalFloat);
    };

    // Generate full latitude rings (including top and bottom)
    for (int i = 0; i <= stackCount; ++i) {

        float phi = XM_PI * i / stackCount; // 0 → PI
        float sinPhi = sinf(phi);
        float cosPhi = cosf(phi);

        for (int j = 0; j < sliceCount; ++j) {
            float theta = XM_2PI * j / sliceCount; // 0 → 2PI

            XMFLOAT3 pos = {
                center.x + radius * sinPhi * cosf(theta),
                center.y + radius * cosPhi,
                center.z + radius * sinPhi * sinf(theta)
            };

            XMVECTOR vPos = XMLoadFloat3(&pos);
            geometry.vertices.push_back( Vertex{ pos, GetSphericalNormal(vPos), color } );
        }
    }

    // Connect stacks with quads (2 triangles each)
    for (int i = 0; i < stackCount; ++i) {
        for (int j = 0; j < sliceCount; ++j) {
            int next_j = (j + 1) % sliceCount;
            int r0 = i * sliceCount;
            int r1 = (i + 1) * sliceCount;

            geometry.indices.push_back(r0 + j);
            geometry.indices.push_back(r1 + j);
            geometry.indices.push_back(r0 + next_j);
            geometry.indices.push_back(r0 + next_j);
            geometry.indices.push_back(r1 + j);
            geometry.indices.push_back(r1 + next_j);
        }
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

    colorBase = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorTop = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorIncline = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    
}

// FRUSTUM_OF_PYRAMID
inline GeometryData FRUSTUM_OF_PYRAMID::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.vertices.resize(8);

    // Calculate the geometric center (centroid) of the frustum.
    // We use this to calculate an outward-facing normal for each corner vertex.
    XMVECTOR centroid = XMVectorZero();
    for (const auto& v : vertices) {
        centroid += XMLoadFloat3(&v);
    }
    centroid /= 8.0f; // Average of 8 vertices

    // Lambda helper to calculate and pack the normal
    auto GetCentroidNormal = [&](XMVECTOR vertexPos) -> XMUBYTE4 {
        XMFLOAT3 normalFloat;
        // Normal is the direction from centroid to the vertex
        XMStoreFloat3(&normalFloat, XMVector3Normalize(vertexPos - centroid));
        return PackNormal(normalFloat);
        };

    for (size_t i = 0; i < 8; ++i) {
        XMVECTOR vPos = XMLoadFloat3(&vertices[i]);
        //Since currently we are sharing vertices between faces, we can assign only 1 color.
        geometry.vertices[i] = Vertex{ vertices[i], GetCentroidNormal(vPos), colorBase };
    }

    geometry.indices = {
        0, 2, 1, 0, 3, 2,// Bottom face
        4, 5, 6, 4, 6, 7,// Top face
        0, 1, 5, 0, 5, 4,// Side faces
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

    colorBase    = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorTop     = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorIncline = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
}

// FRUSTUM_OF_CONE
// Ram: After reading most of the previous Shapes code, this one is on blind faith. This code was not read before commit !
inline GeometryData FRUSTUM_OF_CONE::GetGeometry()
{
    GeometryData geometry;
    geometry.id = memoryID;
    const int numSegments = 36;
    geometry.vertices.clear();
    geometry.indices.clear();
    geometry.vertices.reserve(numSegments * (3 + 3 + 4));
    geometry.indices.reserve(numSegments * (3 + 3 + 6));

    auto AddTriangle = [&](const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMHALF4& color) {
        XMVECTOR v0 = XMLoadFloat3(&p0);
        XMVECTOR v1 = XMLoadFloat3(&p1);
        XMVECTOR v2 = XMLoadFloat3(&p2);
        XMVECTOR normal = XMVector3Normalize( XMVector3Cross(v1 - v0, v2 - v0));
        XMFLOAT3 normalFloat;
        XMStoreFloat3(&normalFloat, normal);
        XMUBYTE4 packedNormal = PackNormal(normalFloat);
        uint16_t base = static_cast<uint16_t>(geometry.vertices.size());

        geometry.vertices.push_back(Vertex{ p0, packedNormal, color });
        geometry.vertices.push_back(Vertex{ p1, packedNormal, color });
        geometry.vertices.push_back(Vertex{ p2, packedNormal, color });

        geometry.indices.push_back(base + 0);
        geometry.indices.push_back(base + 1);
        geometry.indices.push_back(base + 2);
    };

    auto AddQuad = [&](const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3, 
        const XMHALF4& color){

        XMVECTOR v0 = XMLoadFloat3(&p0);
        XMVECTOR v1 = XMLoadFloat3(&p1);
        XMVECTOR v2 = XMLoadFloat3(&p2);
        XMVECTOR normal = XMVector3Normalize( XMVector3Cross(v1 - v0, v2 - v0));
        XMFLOAT3 normalFloat;
        XMStoreFloat3(&normalFloat, normal);
        XMUBYTE4 packedNormal = PackNormal(normalFloat);
        uint16_t base = static_cast<uint16_t>(geometry.vertices.size());

        geometry.vertices.push_back(Vertex{ p0, packedNormal, color });
        geometry.vertices.push_back(Vertex{ p1, packedNormal, color });
        geometry.vertices.push_back(Vertex{ p2, packedNormal, color });
        geometry.vertices.push_back(Vertex{ p3, packedNormal, color });

        geometry.indices.insert(geometry.indices.end(), {
            static_cast<uint16_t>(base + 0),
            static_cast<uint16_t>(base + 1),
            static_cast<uint16_t>(base + 2),
            static_cast<uint16_t>(base + 0),
            static_cast<uint16_t>(base + 2),
            static_cast<uint16_t>(base + 3)
        });
    };

    for (int i = 0; i < numSegments; ++i) {
        int next = (i + 1) % numSegments;

        float a0 = 2.0f * XM_PI * i / numSegments;
        float a1 = 2.0f * XM_PI * next / numSegments;

        float c0 = cosf(a0);
        float s0 = sinf(a0);
        float c1 = cosf(a1);
        float s1 = sinf(a1);

        XMFLOAT3 b0 = { bottomCenter.x + bottomRadius * c0, bottomCenter.y, bottomCenter.z + bottomRadius * s0 };
        XMFLOAT3 b1 = { bottomCenter.x + bottomRadius * c1, bottomCenter.y, bottomCenter.z + bottomRadius * s1 };
        XMFLOAT3 t0 = { topCenter.x + topRadius * c0, topCenter.y, topCenter.z + topRadius * s0 };
        XMFLOAT3 t1 = { topCenter.x + topRadius * c1, topCenter.y, topCenter.z + topRadius * s1 };

        AddTriangle(bottomCenter, b1, b0, colorBase);// Bottom cap (normal automatically downward via winding)
        AddTriangle(topCenter, t0, t1, colorTop);// Top cap (normal automatically upward)
        AddQuad(b0, b1, t1, t0, colorIncline);// Side wall (flat quad)
    }

    return geometry;
}

inline void PIPE::Randomize() {

    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> sizeDist(0.2f, 0.6f);
    std::uniform_real_distribution<float> thickDist(0.02f, 0.1f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);

    auto& rng = GetRNG();

    center1 = { posDist(rng), posDist(rng), posDist(rng) };

    float length = sizeDist(rng);
    center2 = { center1.x, center1.y + length, center1.z };

    outsideDiameter = sizeDist(rng);
    insideDiameter = outsideDiameter - thickDist(rng);

    colorOuter = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorInner = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorCap = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
}

inline GeometryData PIPE::GetGeometry()
{
    GeometryData geometry;
    geometry.id = memoryID;
    const int numSegments = 36;
    geometry.vertices.clear();
    geometry.indices.clear();
    geometry.vertices.reserve(numSegments * 16);
    geometry.indices.reserve(numSegments * 24);

    float outerR = outsideDiameter * 0.5f;
    float innerR = insideDiameter * 0.5f;

    XMVECTOR p1 = XMLoadFloat3(&center1);
    XMVECTOR p2 = XMLoadFloat3(&center2);

    XMVECTOR axis = XMVector3Normalize(p2 - p1);

    // Build orthonormal basis
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    if (fabs(XMVectorGetX(XMVector3Dot(axis, up))) > 0.99f) up = XMVectorSet(1, 0, 0, 0);

    XMVECTOR tangent = XMVector3Normalize(XMVector3Cross(up, axis));
    XMVECTOR bitangent = XMVector3Cross(axis, tangent);

    auto AddQuad = [&](XMVECTOR a, XMVECTOR b, XMVECTOR c, XMVECTOR d,
        XMVECTOR normalVec, const XMHALF4& color)
        {
            XMFLOAT3 normalF;
            XMStoreFloat3(&normalF, XMVector3Normalize(normalVec));
            XMUBYTE4 packedNormal = PackNormal(normalF);

            uint16_t base = (uint16_t)geometry.vertices.size();

            XMFLOAT3 pa, pb, pc, pd;
            XMStoreFloat3(&pa, a);
            XMStoreFloat3(&pb, b);
            XMStoreFloat3(&pc, c);
            XMStoreFloat3(&pd, d);

            geometry.vertices.push_back(Vertex{ pa, packedNormal, color });
            geometry.vertices.push_back(Vertex{ pb, packedNormal, color });
            geometry.vertices.push_back(Vertex{ pc, packedNormal, color });
            geometry.vertices.push_back(Vertex{ pd, packedNormal, color });

            geometry.indices.insert(geometry.indices.end(), {
                static_cast<uint16_t>(base + 0), static_cast<uint16_t>(base + 1),
                static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 0),
                static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)
            });
        };

    for (int i = 0; i < numSegments; ++i){
        int next = (i + 1) % numSegments;
        float a0 = XM_2PI * i / numSegments;
        float a1 = XM_2PI * next / numSegments;

        XMVECTOR dir0 = cosf(a0) * tangent + sinf(a0) * bitangent;
        XMVECTOR dir1 = cosf(a1) * tangent + sinf(a1) * bitangent;
        XMVECTOR o1 = p1 + dir0 * outerR;
        XMVECTOR o2 = p1 + dir1 * outerR;
        XMVECTOR o3 = p2 + dir1 * outerR;
        XMVECTOR o4 = p2 + dir0 * outerR;
        XMVECTOR i1 = p1 + dir0 * innerR;
        XMVECTOR i2 = p1 + dir1 * innerR;
        XMVECTOR i3 = p2 + dir1 * innerR;
        XMVECTOR i4 = p2 + dir0 * innerR;

        AddQuad(o1, o2, o3, o4, dir0, colorOuter);// Outer wall
        AddQuad(i4, i3, i2, i1, -dir0, colorInner);// Inner wall (reverse normal)
        AddQuad(i2, i1, o1, o2, -axis, colorCap);// Start cap ring
        AddQuad(o4, o3, i3, i4, axis, colorCap);// End cap ring
    }

    return geometry;
}