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
    std::vector<XMHALF4> colors;   // 8 colors, one for each vertex

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
    std::vector<XMHALF4> colors;   // 8 colors

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
    XMHALF4 colorBase, colorTop, colorIncline; // There are 3 uniqe type of surfaces on a pyramid frustum.

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
    // Since we are using commone vertex between different surfaces, Intentionally assigning colors[0] for uinformity.
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
        XMFLOAT3(cx - sx, cy - sy, cz - sz), XMFLOAT3(cx + sx, cy - sy, cz - sz),
        XMFLOAT3(cx + sx, cy + sy, cz - sz), XMFLOAT3(cx - sx, cy + sy, cz - sz),
        XMFLOAT3(cx - sx, cy - sy, cz + sz), XMFLOAT3(cx + sx, cy - sy, cz + sz),
        XMFLOAT3(cx + sx, cy + sy, cz + sz), XMFLOAT3(cx - sx, cy + sy, cz + sz)
    };

    colors.resize(8);
    for (int i = 0; i < 8; ++i) {
        colors[i] = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    }
}

/*Similar to the Pyramid implementation, calculating the centroid (geometric center) of the cuboid. 
The normal for each shared corner vertex is then defined as the normalized vector pointing from the center to that corner. 
This provides a "rounded" shading look, which is the mathematically suitable approach for shared vertices on a convex shape.*/

// CUBOID
inline GeometryData CUBOID::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.vertices.resize(8);

    // Calculate the geometric center (centroid) of the cuboid.
    // We use this to calculate an outward-facing normal for each corner vertex.
    XMVECTOR centroid = XMVectorZero();
    for (const auto& v : vertices) {
        centroid += XMLoadFloat3(&v);
    }
    centroid /= 8.0f; // Average of 8 vertices

    // Lambda helper to calculate and pack the normal
    auto GetCentroidNormal = [&](XMVECTOR vertexPos) -> XMUBYTE4 {
        XMFLOAT3 normalFloat; // Normal is the direction from centroid to the vertex
        XMStoreFloat3(&normalFloat, XMVector3Normalize(vertexPos - centroid));
        return PackNormal(normalFloat);
        };

    for (size_t i = 0; i < 8; ++i) {
        XMVECTOR vPos = XMLoadFloat3(&vertices[i]);
        geometry.vertices[i] = Vertex{ vertices[i], GetCentroidNormal(vPos), colors[i] };
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

    colorBase = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorIncline = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    
}

// CONE
/*Consistent with the previous shapes, using the centroid (geometric average) of all generated vertices to determine the
"suitable" normal for the shared vertices. This ensures that the apex points roughly up, the base center points down, 
and the rim vertices point outwards and slightly downwards, providing a smooth shading approximation for the single-mesh 
structure.*/
inline GeometryData CONE::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    const int numSegments = 36; // Must match the logic in Randomize regarding color count

    geometry.vertices.resize(numSegments + 2);

    // Generate positions first to calculate the Centroid.
    // We use the centroid to determine the outward-facing normal for shared vertices.
    std::vector<XMFLOAT3> tempPositions(numSegments + 2);
    tempPositions[0] = apex;
    tempPositions[1] = baseCenter;

    XMVECTOR centroidAcc = XMLoadFloat3(&apex) + XMLoadFloat3(&baseCenter);

    for (int i = 0; i < numSegments; ++i) {
        float angle = 2.0f * (float)M_PI * i / numSegments;
        tempPositions[i + 2] = {
            baseCenter.x + radius * cos(angle),
            baseCenter.y,
            baseCenter.z + radius * sin(angle)
        };
        centroidAcc += XMLoadFloat3(&tempPositions[i + 2]);
    }
    XMVECTOR centroid = centroidAcc / (float)(numSegments + 2);// Calculate average (centroid)

    // Lambda helper to calculate and pack the normal
    auto GetCentroidNormal = [&](XMVECTOR vertexPos) -> XMUBYTE4 {
        XMFLOAT3 normalFloat; // Normal is the direction from centroid to the vertex
        XMStoreFloat3(&normalFloat, XMVector3Normalize(vertexPos - centroid));
        return PackNormal(normalFloat);
        };

    for (int i = 0; i < numSegments + 2; ++i) { // Construct Vertices with Position, Normal, and Color
        XMVECTOR vPos = XMLoadFloat3(&tempPositions[i]);
        geometry.vertices[i] = Vertex{ tempPositions[i], GetCentroidNormal(vPos), colorIncline };
    }

    for (int i = 0; i < numSegments; ++i) {//Define Indices
        geometry.indices.push_back(0);// Side surface triangles
        geometry.indices.push_back(i + 2);
        geometry.indices.push_back(((i + 1) % numSegments) + 2);

        geometry.indices.push_back(1);// Base surface triangles
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

    colorBase    = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorTop     = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorIncline = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    
}

// CYLINDER
/*Consistent with the other shapes, the centroid(midpoint between the two base centers) and using the direction from 
the centroid to each vertex as the normal.This ensures that the cylinder has consistent "smooth" shading for the shared 
vertices along the rim and caps.*/
inline GeometryData CYLINDER::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    const int numSegments = 36;
    geometry.vertices.resize(2 * numSegments + 2);// Define vertices for both caps

    // Calculate the geometric center (centroid) of the cylinder.
    // We use this to calculate an outward-facing normal for each vertex.
    XMVECTOR vP1 = XMLoadFloat3(&p1);
    XMVECTOR vP2 = XMLoadFloat3(&p2);
    XMVECTOR centroid = (vP1 + vP2) * 0.5f;

    // Lambda helper to calculate and pack the normal
    auto GetCentroidNormal = [&](XMVECTOR vertexPos) -> XMUBYTE4 {
        XMFLOAT3 normalFloat;
        // Normal is the direction from centroid to the vertex
        XMStoreFloat3(&normalFloat, XMVector3Normalize(vertexPos - centroid));
        return PackNormal(normalFloat);
        };

    geometry.vertices[0] = Vertex{ p1, GetCentroidNormal(vP1), colorBase };// Bottom center
    geometry.vertices[numSegments + 1] = Vertex{ p2, GetCentroidNormal(vP2), colorTop };// Top center

    for (int i = 0; i < numSegments; ++i) {
        float angle = 2.0f * (float)M_PI * i / numSegments;
        float cosA = cos(angle);
        float sinA = sin(angle);

        // Bottom cap rim vertex
        XMFLOAT3 posBottom = { p1.x + radius * cosA, p1.y, p1.z + radius * sinA };
        XMVECTOR vPosBottom = XMLoadFloat3(&posBottom);
        geometry.vertices[i + 1] = Vertex{ posBottom, GetCentroidNormal(vPosBottom), colorIncline };

        // Top cap rim vertex
        XMFLOAT3 posTop = { p2.x + radius * cosA, p2.y, p2.z + radius * sinA };
        XMVECTOR vPosTop = XMLoadFloat3(&posTop);
        geometry.vertices[i + numSegments + 2] = Vertex{ posTop, GetCentroidNormal(vPosTop), colorIncline };
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
        colors[i] = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    }
}

// PARALLELEPIPED
inline GeometryData PARALLELEPIPED::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.vertices.resize(8);

    // Calculate the geometric center (centroid) of the parallelepiped.
    // We use this to calculate an outward-facing normal for each vertex.
    XMVECTOR centroid = XMVectorZero();
    for (const auto& v : vertices) {
        centroid += XMLoadFloat3(&v);
    }
    centroid /= 8.0f; // Average of 8 vertices

    // Lambda helper to calculate and pack the normal
    auto GetCentroidNormal = [&](XMVECTOR vertexPos) -> XMUBYTE4 {
        XMFLOAT3 normalFloat; // Normal is the direction from centroid to the vertex
        XMStoreFloat3(&normalFloat, XMVector3Normalize(vertexPos - centroid));
        return PackNormal(normalFloat);
        };

    for (size_t i = 0; i < 8; ++i) {
        XMVECTOR vPos = XMLoadFloat3(&vertices[i]);
        geometry.vertices[i] = Vertex{ vertices[i], GetCentroidNormal(vPos), colors[i] };
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
    color = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
}

/*For the sphere, the "suitable" normal for every vertex is exactly the normalized vector from the sphere's 
center to that vertex. This results in perfectly smooth shading across the surface. 
Color randomization integrated directly into the vertex generation loop to accommodate the new Vertex structure.*/
inline GeometryData SPHERE::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;

    // Using 36 longitude slices and 18 latitude stacks for a reasonable vertex count
    const int sliceCount = 36;
    const int stackCount = 18;

    XMVECTOR vCenter = XMLoadFloat3(&center);// Calculate the center vector for normal calculations

    auto GetSphericalNormal = [&](XMVECTOR vertexPos) -> XMUBYTE4 {// Lambda helper to calculate and pack the normal
        XMFLOAT3 normalFloat;
        // Normal is the direction from center to the vertex (result is already unit length for sphere surface)
        XMStoreFloat3(&normalFloat, XMVector3Normalize(vertexPos - vCenter));
        return PackNormal(normalFloat);
        };

    // Add top pole vertex
    XMFLOAT3 topPos = { center.x, center.y + radius, center.z };
    geometry.vertices.push_back(Vertex{ topPos, GetSphericalNormal(XMLoadFloat3(&topPos)), color });

    for (int i = 1; i < stackCount; ++i) {// Add middle stacks
        float phi = (float)M_PI * i / stackCount;
        float sinPhi = sin(phi);
        float cosPhi = cos(phi);

        for (int j = 0; j < sliceCount; ++j) {
            float theta = 2.0f * (float)M_PI * j / sliceCount;
            XMFLOAT3 pos = {
                center.x + radius * sinPhi * cos(theta),
                center.y + radius * cosPhi,
                center.z + radius * sinPhi * sin(theta)
            };

            XMVECTOR vPos = XMLoadFloat3(&pos);
            geometry.vertices.push_back(Vertex{ pos, GetSphericalNormal(vPos), color });
        }
    }

    // Add bottom pole vertex
    XMFLOAT3 bottomPos = { center.x, center.y - radius, center.z };
    geometry.vertices.push_back(Vertex{ bottomPos, GetSphericalNormal(XMLoadFloat3(&bottomPos)), color });

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

            // Quad split into two triangles
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
inline GeometryData FRUSTUM_OF_CONE::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    const int numSegments = 36;

    // Define vertices
    geometry.vertices.resize(2 * numSegments + 2);

    // Calculate the geometric center (centroid) of the frustum.
    // We use this to calculate an outward-facing normal for each vertex.
    XMVECTOR vBottom = XMLoadFloat3(&bottomCenter);
    XMVECTOR vTop = XMLoadFloat3(&topCenter);
    XMVECTOR centroid = (vBottom + vTop) * 0.5f;

    // Lambda helper to calculate and pack the normal
    auto GetCentroidNormal = [&](XMVECTOR vertexPos) -> XMUBYTE4 {
        XMFLOAT3 normalFloat;
        // Normal is the direction from centroid to the vertex
        XMStoreFloat3(&normalFloat, XMVector3Normalize(vertexPos - centroid));
        return PackNormal(normalFloat);
        };

    // Bottom center
    geometry.vertices[0] = Vertex{ bottomCenter, GetCentroidNormal(vBottom), colorBase };

    // Top center
    geometry.vertices[numSegments + 1] = Vertex{ topCenter, GetCentroidNormal(vTop), colorTop };

    for (int i = 0; i < numSegments; ++i) {
        float angle = 2.0f * (float)M_PI * i / numSegments;
        float cosA = cos(angle);
        float sinA = sin(angle);

        // Bottom cap vertices
        XMFLOAT3 posBottom = { bottomCenter.x + bottomRadius * cosA, bottomCenter.y, bottomCenter.z + bottomRadius * sinA };
        XMVECTOR vPosBottom = XMLoadFloat3(&posBottom);
        geometry.vertices[i + 1] = Vertex{ posBottom, GetCentroidNormal(vPosBottom), colorIncline };

        // Top cap vertices
        XMFLOAT3 posTop = { topCenter.x + topRadius * cosA, topCenter.y, topCenter.z + topRadius * sinA };
        XMVECTOR vPosTop = XMLoadFloat3(&posTop);
        geometry.vertices[i + numSegments + 2] = Vertex{ posTop, GetCentroidNormal(vPosTop), colorIncline };
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