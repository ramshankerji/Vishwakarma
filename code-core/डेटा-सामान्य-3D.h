// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once

#define _USE_MATH_DEFINES // For M_PI
#include <cmath>
#include <vector>
#include <iostream>
#include <d3d12.h>
#include "डेटा.h"
#include "CommonNamedNumbers.h"
#include "Snap.h"
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

// Regenerates the render geometry for a stored object (switch over all 11 supported 3D types).
// Defined once in DataStorage.cpp; reused by object creation and property-edit MODIFY paths.
// Also composes the object's placement into GeometryData::worldMatrix - see PlacementForObject.
bool GeometryForObject(VishwakarmaStorage::ObjectType objectType, META_DATA* object, GeometryData& geometry);

/* The object's rigid authored -> world placement, or nullptr when the type has none (2D, logical).
Mutable on purpose: this is also how a move writes a new placement, so the two directions cannot
drift apart into separate switches. Defined beside GeometryForObject in DataStorage.cpp. */
Placement3D* PlacementForObject(VishwakarmaStorage::ObjectType objectType, META_DATA* object);

/* The exact points an engineer may want to snap to on this object, APPENDED to `out`
(website/content/software/snapping.md section 9). Mirrors GeometryForObject deliberately: adding a
new intelligent object means adding one case here and nothing at all to the snap engine, which is
the whole reason the semantics live with the object rather than in a central table.

Coordinates are AUTHORED, exactly like the object's own defining parameters, and exactly like the
vertices GeometryForObject emits for the authored-space types. The caller composes the object's
Placement3D on top - one place, the same one PlacementForObject serves - so a moved object cannot
end up with snap points at its old location.

Doubles, per locked decision 1: the GPU pick narrows the candidate set, the coordinate is always
recomputed on the CPU from the object's own parameters. That the 3D types store XMFLOAT3 caps the
precision at float, but nothing is lost a second time on the way through here. */
bool SnapPointsForObject(VishwakarmaStorage::ObjectType objectType, META_DATA* object,
    std::vector<SnapPoint>& out);

/* THE object's full LOCAL -> WORLD matrix, and the ONLY place it is composed. This is what reaches
the GPU as the object's 64-byte InstanceRecord, so anything that disagrees with it draws the object
in the wrong place or at the wrong size (website/software/graphics.md, "World matrix and object
placement").

It is two transforms composed, in row-vector order:

  local -> authored   whatever frame the type's GetGeometry emits vertices in. IDENTITY for every
                      type that still bakes world coordinates into its vertices. For the three
                      canonical-frame types it is: SPHERE scale(radius) then translate(center);
                      CUBOID scale(size) then rotate(orientation) then translate(center); CYLINDER
                      an orthonormal basis about p2 - p1 with rows scaled (r, r, L) and the
                      translation row at the MIDPOINT, because the canonical rod is centred. A
                      LINE_MEMBER is the first type whose frame depends on its DATA rather than its
                      type - canonical only for a solid rectangular or circular profile, identity
                      for the rest - so its verdict comes from LineMemberCanonicalFrame, the single
                      function its generator asks too.
  authored -> world   the object's rigid Placement3D, i.e. where it has been moved to since.

WHY THIS IS A FUNCTION AND NOT INLINE IN GeometryForObject. There are TWO producers of an object's
world matrix and only one of them regenerates geometry: a geometry ADD/MODIFY goes through
GeometryForObject, while a MOVE emits a transform-only MODIFY carrying a matrix and NO vertices.
The move path used to build that matrix from the placement alone, which was indistinguishable from
correct only while every generator emitted authored-space vertices and left the local matrix at
identity. The moment SPHERE stopped doing that, moving a sphere dropped its radius and centre and
redrew it as a unit sphere at the placement origin. Both callers now go through here.

ADDING A CANONICAL-FRAME TYPE MEANS EDITING THIS SWITCH, not just its generator - the two must
describe the same frame, exactly as PlacementForObject and GeometryForObject must. */
DirectX::XMMATRIX WorldMatrixForObject(VishwakarmaStorage::ObjectType objectType, META_DATA* object);

// The most basic 3D Shapes.: Pyramid, Cuboid, Cone, Cylinder, Parallelepiped, Sphere
struct PYRAMID :public META_DATA{
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Pyramid;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;
    //Mandatory Fields
    std::vector<XMFLOAT3> vertices; // Index 0,1,2 for base, 3 for apex
    std::vector<XMHALF4> colors; // RGBA format.

    //Optional Fields
    uint64_t optionalFieldsFlags = 0;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags = 0;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags = 0; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;

	// Rigid authored -> world placement (struct Placement3D, in the base data header).
	// Identity until this object is moved; a move rewrites it and emits a transform-only
	// MODIFY instead of regenerating geometry.
	Placement3D placement;

	void Randomize(); // Assign random position, size, colors etc.
    GeometryData GetGeometry(); // Simply returns the vertices with colors and indexes.
    void CalculateGeometry() {}; // Calculate the geometry, potentially taking into account cutouts.
    bool encode(std::vector<uint8_t>& payload, std::string* errorMessage) const;
    bool decode(const std::vector<uint8_t>& payload);
};

struct CUBOID :public META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Cuboid;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;
    XMFLOAT3 center = {};                     // Authored centre of the box.
    XMFLOAT3 size = { 1.0f, 1.0f, 1.0f };     // FULL edge lengths along the box's own X/Y/Z.
    XMFLOAT4 orientation = { 0.0f, 0.0f, 0.0f, 1.0f }; // Unit quaternion; identity = axis-aligned.
    XMHALF4 colors = {};   // Common color for all faces.

    //Optional Fields
    uint64_t optionalFieldsFlags = 0;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags = 0;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags = 0; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    // Rigid authored -> world placement (struct Placement3D, in the base data header).
    // Identity until this object is moved; a move rewrites it and emits a transform-only
    // MODIFY instead of regenerating geometry.
    Placement3D placement;

    void Randomize();
    GeometryData GetGeometry();
    bool encode(std::vector<uint8_t>& payload, std::string* errorMessage) const;
    bool decode(const std::vector<uint8_t>& payload);
};

struct CONE :public META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Cone;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;
    //Mandatory Fields
    XMFLOAT3 apex = {};
    XMFLOAT3 baseCenter = {};
    float radius = 1;
    XMHALF4 colorBase = {}, colorIncline = {}; // Cone has only 2 surface.

    //Optional Fields
    uint64_t optionalFieldsFlags = 0;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags = 0;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags = 0; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    // Rigid authored -> world placement (struct Placement3D, in the base data header).
    // Identity until this object is moved; a move rewrites it and emits a transform-only
    // MODIFY instead of regenerating geometry.
    Placement3D placement;

    void Randomize();
    GeometryData GetGeometry();
    bool encode(std::vector<uint8_t>& payload, std::string* errorMessage) const;
    bool decode(const std::vector<uint8_t>& payload);
};

struct CYLINDER :public META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Cylinder;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;
    //Mandatory Fields
    XMFLOAT3 p1 = {}, p2 = {}; // Center points of the two circular bases
    float radius = 1;
    XMHALF4 colorBase = {}, colorTop = {}, colorIncline = {}; // 1 Unique color for each surface.

    //Optional Fields
    uint64_t optionalFieldsFlags = 0;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags = 0;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags = 0; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    // Rigid authored -> world placement (struct Placement3D, in the base data header).
    // Identity until this object is moved; a move rewrites it and emits a transform-only
    // MODIFY instead of regenerating geometry.
    Placement3D placement;

    void Randomize();
    GeometryData GetGeometry();
    bool encode(std::vector<uint8_t>& payload, std::string* errorMessage) const;
    bool decode(const std::vector<uint8_t>& payload);
};

struct PARALLELEPIPED :public META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Parallelepiped;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;
    //Mandatory Fields
    std::vector<XMFLOAT3> vertices; // 8 vertices
    XMHALF4 colors = {};   // common color for entire object. 

    //Optional Fields
    uint64_t optionalFieldsFlags = 0;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags = 0;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags = 0; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    // Rigid authored -> world placement (struct Placement3D, in the base data header).
    // Identity until this object is moved; a move rewrites it and emits a transform-only
    // MODIFY instead of regenerating geometry.
    Placement3D placement;

    void Randomize();
    GeometryData GetGeometry();
    bool encode(std::vector<uint8_t>& payload, std::string* errorMessage) const;
    bool decode(const std::vector<uint8_t>& payload);
};

struct SPHERE :public META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Sphere;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;
    //Mandatory Fields
    XMFLOAT3 center;
    float radius;
    XMHALF4 color = {}; // SInce entire sphere is 1 surface, it has got only 1 color.

    //Optional Fields
    uint64_t optionalFieldsFlags = 0;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags = 0;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags = 0; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    // Rigid authored -> world placement (struct Placement3D, in the base data header).
    // Identity until this object is moved; a move rewrites it and emits a transform-only
    // MODIFY instead of regenerating geometry.
    Placement3D placement;

    void Randomize();
    GeometryData GetGeometry();
    bool encode(std::vector<uint8_t>& payload, std::string* errorMessage) const;
    bool decode(const std::vector<uint8_t>& payload);
};

struct TORUS :public META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Torus;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;
    XMFLOAT3 center = {};
    float majorRadius = 1;
    float minorRadius = 0.25f;
    XMHALF4 color = {};

    uint64_t optionalFieldsFlags = 0;
    uint32_t systemFlags = 0;
    uint32_t objectLifeCycleFlags = 0;
    c_string name;
    // Rigid authored -> world placement (struct Placement3D, in the base data header).
    // Identity until this object is moved; a move rewrites it and emits a transform-only
    // MODIFY instead of regenerating geometry.
    Placement3D placement;

    void Randomize();
    GeometryData GetGeometry();
    bool encode(std::vector<uint8_t>& payload, std::string* errorMessage) const;
    bool decode(const std::vector<uint8_t>& payload);
};

struct ELLIPSOID :public META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Ellipsoid;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;
    XMFLOAT3 center = {};
    float radiusX = 1;
    float radiusY = 1;
    float radiusZ = 1;
    XMHALF4 color = {};

    uint64_t optionalFieldsFlags = 0;
    uint32_t systemFlags = 0;
    uint32_t objectLifeCycleFlags = 0;
    c_string name;
    // Rigid authored -> world placement (struct Placement3D, in the base data header).
    // Identity until this object is moved; a move rewrites it and emits a transform-only
    // MODIFY instead of regenerating geometry.
    Placement3D placement;

    void Randomize();
    GeometryData GetGeometry();
    bool encode(std::vector<uint8_t>& payload, std::string* errorMessage) const;
    bool decode(const std::vector<uint8_t>& payload);
};

struct FRUSTUM_OF_PYRAMID :public META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::FrustumOfPyramid;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;
    //Mandatory Fields
    std::vector<XMFLOAT3> vertices; // 8 vertices: 4 for bottom base, 4 for top base
    XMHALF4 colorBase = {}, colorTop = {}, colorIncline = {}; // There are 3 unique type of surfaces on a pyramid frustum.

    //Optional Fields
    uint64_t optionalFieldsFlags = 0;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags = 0;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags = 0; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    // Rigid authored -> world placement (struct Placement3D, in the base data header).
    // Identity until this object is moved; a move rewrites it and emits a transform-only
    // MODIFY instead of regenerating geometry.
    Placement3D placement;

    void Randomize();
    GeometryData GetGeometry();
    bool encode(std::vector<uint8_t>& payload, std::string* errorMessage) const;
    bool decode(const std::vector<uint8_t>& payload);
};

struct  FRUSTUM_OF_CONE :public META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::FrustumOfCone;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;
    //Mandatory Fields
    XMFLOAT3 bottomCenter = {}, topCenter = {};
    float bottomRadius = 1, topRadius = 1;
    XMHALF4 colorBase = {}, colorTop = {}, colorIncline = {}; // Cone has only 3 surface.

    //Optional Fields
    uint64_t optionalFieldsFlags = 0;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags = 0;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags = 0; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
    // Rigid authored -> world placement (struct Placement3D, in the base data header).
    // Identity until this object is moved; a move rewrites it and emits a transform-only
    // MODIFY instead of regenerating geometry.
    Placement3D placement;

    void Randomize();
    GeometryData GetGeometry();
    bool encode(std::vector<uint8_t>& payload, std::string* errorMessage) const;
    bool decode(const std::vector<uint8_t>& payload);
};

struct PIPE : public META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Pipe;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;

    // Mandatory Fields
    XMFLOAT3 center1 = {};
    XMFLOAT3 center2 = {};
    float outsideDiameter = 1;
    float insideDiameter = 1;

    XMHALF4 colorOuter = {}, colorInner = {}, colorCap = {};

    // Optional Fields
    uint64_t optionalFieldsFlags = 0;
    uint32_t systemFlags = 0;
    uint32_t objectLifeCycleFlags = 0;
    c_string name;

    // Rigid authored -> world placement (struct Placement3D, in the base data header).
    // Identity until this object is moved; a move rewrites it and emits a transform-only
    // MODIFY instead of regenerating geometry.
    Placement3D placement;

    void Randomize();
    GeometryData GetGeometry();
    bool encode(std::vector<uint8_t>& payload, std::string* errorMessage) const;
    bool decode(const std::vector<uint8_t>& payload);
};

/************************* IMPLEMENTATIONS *************************/
//PYRAMID
inline GeometryData PYRAMID::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.color = ToFloat4(colors[0]); // The generator already colored every face colors[0].
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
    geometry.vertices[0] = Vertex{ vertices[0], GetCentroidNormal(v0) };
    geometry.vertices[1] = Vertex{ vertices[1], GetCentroidNormal(v1) };
    geometry.vertices[2] = Vertex{ vertices[2], GetCentroidNormal(v2) };
    geometry.vertices[3] = Vertex{ vertices[3], GetCentroidNormal(v3) };

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

    center = { posDist(rng), posDist(rng), posDist(rng) };
    size = { sizeDist(rng), sizeDist(rng), sizeDist(rng) };
    orientation = { 0.0f, 0.0f, 0.0f, 1.0f }; // Axis-aligned; nothing authors a rotation yet.

    // Single common color for entire cuboid
    colors = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
}

/* CUBOID emits NO GEOMETRY AT ALL - it names a shape in the global primitive library and lets the
world matrix carry `center`, `size` and `orientation` (website/software/graphics.md, "Shared
geometry and the primitive libraries"). Every cuboid in the process shares ONE 12-triangle mesh.

worldMatrix is left at identity here: WorldMatrixForObject composes it, and is the only place that
does, because a MOVE needs the same matrix without regenerating anything. */
inline GeometryData CUBOID::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.color = ToFloat4(colors);
    geometry.libraryShapeId = kPrimitiveShapeCuboid;
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
    geometry.color = ToFloat4(colorIncline); // Incline dominates; colorBase stays stored.
    const int numSegments = 36;
    geometry.vertices.clear();
    geometry.indices.clear();

    // Reserve enough space. Side: 36 triangles → 108 vertices. Base: 36 triangles → 108 vertices
    geometry.vertices.reserve(numSegments * 6);
    geometry.indices.reserve(numSegments * 6);

    // Lambda to add a flat shaded triangle
    auto AddTriangle = [&](const XMFLOAT3& a,
        const XMFLOAT3& b,
        const XMFLOAT3& c)
        {
            XMVECTOR v0 = XMLoadFloat3(&a);
            XMVECTOR v1 = XMLoadFloat3(&b);
            XMVECTOR v2 = XMLoadFloat3(&c);
            XMVECTOR normal = XMVector3Normalize( XMVector3Cross(v1 - v0, v2 - v0));
            XMFLOAT3 normalFloat;
            XMStoreFloat3(&normalFloat, normal);
            XMUBYTE4 packedNormal = PackNormal(normalFloat);

            uint16_t base = static_cast<uint16_t>(geometry.vertices.size());

            geometry.vertices.push_back(Vertex{ a, packedNormal });
            geometry.vertices.push_back(Vertex{ b, packedNormal });
            geometry.vertices.push_back(Vertex{ c, packedNormal });

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

        AddTriangle(apex, r0, r1);// Side surface triangle (flat shaded)
        AddTriangle(baseCenter, r1, r0);// Base surface triangle (flat shaded, downward facing via winding)
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

/* CYLINDER emits NO GEOMETRY AT ALL - it names a shape in the global primitive library and lets the
world matrix carry `p1`, `p2` and `radius` (website/software/graphics.md, "Shared geometry and the
primitive libraries"). The canonical mesh is a unit rod along +Z, radius 1, length 1, CENTRED on z.

TWO DEFECTS DIE WITH THE OLD GENERATOR, and both were silent:

- IT IGNORED ITS OWN AXIS. It computed `axis = normalize(p2 - p1)` and never used it, building both
  rims flat in the XZ plane at p1.y / p2.y. A cylinder whose axis was not parallel to Y therefore
  drew SHEARED - rims staying axis-aligned while their centres moved. PIPE always got this right;
  the world matrix built by WorldMatrixForObject now uses the same orthonormal-basis construction.
- ITS NORMALS POINTED INWARD. Every cap and wall normal came out of a cross product wound the wrong
  way, so cylinders were lit as though from inside while spheres and cuboids were lit from outside.
  Wrong lighting is not a crash, which is exactly why it survived. The library mesh is outward.

Stored data is untouched: p1, p2 and radius stay this object's own authored fields, deliberately the
OPPOSITE choice from cuboid - a cylinder's endpoints are engineering data, whereas a box's eight
corners were only ever an encoding of something simpler. The midpoint the centred mesh needs is
derived inside WorldMatrixForObject and nowhere else.

worldMatrix is left at identity here: WorldMatrixForObject composes it, and is the only place that
does, because a MOVE needs the same matrix without regenerating anything. */
inline GeometryData CYLINDER::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.color = ToFloat4(colorIncline); // Incline dominates; colorBase / colorTop stay stored.
    geometry.libraryShapeId = kPrimitiveShapeCylinder;
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
    geometry.color = ToFloat4(colors);

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

        geometry.vertices.push_back(Vertex{ vertices[i0], packedNormal });
        geometry.vertices.push_back(Vertex{ vertices[i1], packedNormal });
        geometry.vertices.push_back(Vertex{ vertices[i2], packedNormal });
        geometry.vertices.push_back(Vertex{ vertices[i3], packedNormal });

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

/* SPHERE emits NO GEOMETRY AT ALL - it names a shape in the global primitive library and lets the
world matrix carry `center` and `radius` (website/software/graphics.md, "Shared geometry and the
primitive libraries"). Every sphere in the process therefore shares ONE mesh, at eight levels of
detail; a scene of 100,000 spheres stores a sphere once.

That became possible only when the mesh stopped depending on the object's own parameters. The
canonical-local-frame step did that - a UNIT sphere at the ORIGIN - and the ring/quad tessellation
that used to live here now lives in AppendUnitSphereMesh, parameterised by slice and stack count so
the library can build all eight levels from it. LOD 5 is 36x18, exactly what this function emitted.

Eligibility is unconditional: an instanced object costs ZERO geometry bytes, so instancing a one-off
still beats not instancing it, and there is no repeat-count threshold to tune.

Stored data is untouched. `center` and `radius` are still this object's own authored fields, still
persisted exactly as before, and the Properties Pane still reads and writes them - `libraryShapeId`
is runtime-only and never reaches disk, so files written before the library existed load unchanged.
worldMatrix is left at identity here: WorldMatrixForObject composes it, and is the only place that
does, because a MOVE needs the same matrix without regenerating anything. */
inline GeometryData SPHERE::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.color = ToFloat4(color);
    geometry.libraryShapeId = kPrimitiveShapeSphere;
    return geometry;
}

// TORUS
inline void TORUS::Randomize() {
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> majorDist(0.15f, 0.5f);
    std::uniform_real_distribution<float> minorRatioDist(0.2f, 0.4f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);
    auto& rng = GetRNG();

    center = { posDist(rng), posDist(rng), posDist(rng) };
    majorRadius = majorDist(rng);
    minorRadius = majorRadius * minorRatioDist(rng);
    color = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
}

inline GeometryData TORUS::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.color = ToFloat4(color);
    const int majorSegments = 36;
    const int minorSegments = 18;
    geometry.vertices.clear();
    geometry.indices.clear();
    geometry.vertices.reserve(majorSegments * minorSegments);
    geometry.indices.reserve(majorSegments * minorSegments * 6);

    for (int i = 0; i < majorSegments; ++i) {
        const float theta = XM_2PI * i / majorSegments;
        const float cosTheta = cosf(theta);
        const float sinTheta = sinf(theta);

        for (int j = 0; j < minorSegments; ++j) {
            const float phi = XM_2PI * j / minorSegments;
            const float cosPhi = cosf(phi);
            const float sinPhi = sinf(phi);
            const float ringRadius = majorRadius + minorRadius * cosPhi;
            XMFLOAT3 position = {
                center.x + ringRadius * cosTheta,
                center.y + minorRadius * sinPhi,
                center.z + ringRadius * sinTheta
            };
            XMFLOAT3 normal = { cosTheta * cosPhi, sinPhi, sinTheta * cosPhi };
            geometry.vertices.push_back(Vertex{ position, PackNormal(normal) });
        }
    }

    for (int i = 0; i < majorSegments; ++i) {
        const int nextI = (i + 1) % majorSegments;
        for (int j = 0; j < minorSegments; ++j) {
            const int nextJ = (j + 1) % minorSegments;
            const uint16_t a = static_cast<uint16_t>(i * minorSegments + j);
            const uint16_t b = static_cast<uint16_t>(nextI * minorSegments + j);
            const uint16_t c = static_cast<uint16_t>(nextI * minorSegments + nextJ);
            const uint16_t d = static_cast<uint16_t>(i * minorSegments + nextJ);
            geometry.indices.insert(geometry.indices.end(), { a, b, d, d, b, c });
        }
    }

    return geometry;
}

// ELLIPSOID
inline void ELLIPSOID::Randomize() {
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> sizeDist(0.1f, 0.5f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);
    auto& rng = GetRNG();

    center = { posDist(rng), posDist(rng), posDist(rng) };
    radiusX = sizeDist(rng);
    radiusY = sizeDist(rng);
    radiusZ = sizeDist(rng);
    color = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
}

inline GeometryData ELLIPSOID::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.color = ToFloat4(color);
    const int sliceCount = 36;
    const int stackCount = 18;
    geometry.vertices.clear();
    geometry.indices.clear();
    geometry.vertices.reserve((stackCount + 1) * sliceCount);
    geometry.indices.reserve(stackCount * sliceCount * 6);

    for (int i = 0; i <= stackCount; ++i) {
        const float phi = XM_PI * i / stackCount;
        const float sinPhi = sinf(phi);
        const float cosPhi = cosf(phi);

        for (int j = 0; j < sliceCount; ++j) {
            const float theta = XM_2PI * j / sliceCount;
            const float cosTheta = cosf(theta);
            const float sinTheta = sinf(theta);
            XMFLOAT3 position = {
                center.x + radiusX * sinPhi * cosTheta,
                center.y + radiusY * cosPhi,
                center.z + radiusZ * sinPhi * sinTheta
            };
            XMFLOAT3 normal = {
                (position.x - center.x) / (radiusX * radiusX),
                (position.y - center.y) / (radiusY * radiusY),
                (position.z - center.z) / (radiusZ * radiusZ)
            };
            geometry.vertices.push_back(Vertex{ position, PackNormal(normal) });
        }
    }

    for (int i = 0; i < stackCount; ++i) {
        for (int j = 0; j < sliceCount; ++j) {
            const int nextJ = (j + 1) % sliceCount;
            const int r0 = i * sliceCount;
            const int r1 = (i + 1) * sliceCount;
            geometry.indices.push_back(static_cast<uint16_t>(r0 + j));
            geometry.indices.push_back(static_cast<uint16_t>(r1 + j));
            geometry.indices.push_back(static_cast<uint16_t>(r0 + nextJ));
            geometry.indices.push_back(static_cast<uint16_t>(r0 + nextJ));
            geometry.indices.push_back(static_cast<uint16_t>(r1 + j));
            geometry.indices.push_back(static_cast<uint16_t>(r1 + nextJ));
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
    geometry.color = ToFloat4(colorIncline); // Incline dominates; colorBase / colorTop stay stored.
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
        geometry.vertices[i] = Vertex{ vertices[i], GetCentroidNormal(vPos) };
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
    geometry.color = ToFloat4(colorIncline); // Incline dominates; colorBase / colorTop stay stored.
    const int numSegments = 36;
    geometry.vertices.clear();
    geometry.indices.clear();
    geometry.vertices.reserve(numSegments * (3 + 3 + 4));
    geometry.indices.reserve(numSegments * (3 + 3 + 6));

    auto AddTriangle = [&](const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2) {
        XMVECTOR v0 = XMLoadFloat3(&p0);
        XMVECTOR v1 = XMLoadFloat3(&p1);
        XMVECTOR v2 = XMLoadFloat3(&p2);
        XMVECTOR normal = XMVector3Normalize( XMVector3Cross(v1 - v0, v2 - v0));
        XMFLOAT3 normalFloat;
        XMStoreFloat3(&normalFloat, normal);
        XMUBYTE4 packedNormal = PackNormal(normalFloat);
        uint16_t base = static_cast<uint16_t>(geometry.vertices.size());

        geometry.vertices.push_back(Vertex{ p0, packedNormal });
        geometry.vertices.push_back(Vertex{ p1, packedNormal });
        geometry.vertices.push_back(Vertex{ p2, packedNormal });

        geometry.indices.push_back(base + 0);
        geometry.indices.push_back(base + 1);
        geometry.indices.push_back(base + 2);
    };

    auto AddQuad = [&](const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3) {

        XMVECTOR v0 = XMLoadFloat3(&p0);
        XMVECTOR v1 = XMLoadFloat3(&p1);
        XMVECTOR v2 = XMLoadFloat3(&p2);
        XMVECTOR normal = XMVector3Normalize( XMVector3Cross(v1 - v0, v2 - v0));
        XMFLOAT3 normalFloat;
        XMStoreFloat3(&normalFloat, normal);
        XMUBYTE4 packedNormal = PackNormal(normalFloat);
        uint16_t base = static_cast<uint16_t>(geometry.vertices.size());

        geometry.vertices.push_back(Vertex{ p0, packedNormal });
        geometry.vertices.push_back(Vertex{ p1, packedNormal });
        geometry.vertices.push_back(Vertex{ p2, packedNormal });
        geometry.vertices.push_back(Vertex{ p3, packedNormal });

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

        AddTriangle(bottomCenter, b1, b0);// Bottom cap (normal automatically downward via winding)
        AddTriangle(topCenter, t0, t1);// Top cap (normal automatically upward)
        AddQuad(b0, b1, t1, t0);// Side wall (flat quad)
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
    geometry.color = ToFloat4(colorOuter); // Outer wall dominates; colorInner / colorCap stay stored.
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
        XMVECTOR normalVec)
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

            geometry.vertices.push_back(Vertex{ pa, packedNormal });
            geometry.vertices.push_back(Vertex{ pb, packedNormal });
            geometry.vertices.push_back(Vertex{ pc, packedNormal });
            geometry.vertices.push_back(Vertex{ pd, packedNormal });

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

        AddQuad(o1, o2, o3, o4, dir0);// Outer wall
        AddQuad(i4, i3, i2, i1, -dir0);// Inner wall (reverse normal)
        AddQuad(i2, i1, o1, o2, -axis);// Start cap ring
        AddQuad(o4, o3, i3, i4, axis);// End cap ring
    }

    return geometry;
}
