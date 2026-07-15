// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once

#include <vector>
#include <iostream>

#include "डेटा.h"
#include "डेटा-सामान्य-3D.h" // For GeometryData, Vertex, PackNormal, GetRNG shared with the primitive shapes.

struct PIPE_SPOOL {
    META_DATA metaData;

    //Mandatory Fields

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};

// Hollow pipe bend. Parametric sweep angle lets one primitive represent 22.5/45/90/135/180 degree elbows.
struct ELBOW : public META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Elbow;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;

    //Mandatory Fields
    XMFLOAT3 center = {};          // Center of the bend arc.
    float bendRadius = 1;          // Distance from the arc center to the pipe centerline.
    float outsideDiameter = 0.3f;
    float insideDiameter = 0.2f;
    float sweepAngleRadians = XM_PIDIV2; // Portion of the bend to draw (default 90 degrees).
    XMHALF4 colorOuter = {}, colorInner = {}, colorCap = {};

    //Optional Fields
    uint64_t optionalFieldsFlags = 0;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags = 0;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags = 0; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;

    void Randomize();
    GeometryData GetGeometry();
};

// Pipe tee/branch. Branch diameters are independent of the main run, and the branch angle
// (restricted 45..135 degrees) lets the same primitive also represent a Y-branch strainer.
struct TEE : public META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Tee;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;

    //Mandatory Fields
    XMFLOAT3 center1 = {}, center2 = {}; // Main run endpoints.
    float mainOutsideDiameter = 0.35f;
    float mainInsideDiameter = 0.28f;
    float branchAngleDegrees = 90.0f;    // Angle between the main axis and the branch (45..135).
    float branchLength = 0.5f;           // Branch length measured from the main centerline.
    float branchOutsideDiameter = 0.25f;
    float branchInsideDiameter = 0.2f;
    XMHALF4 colorOuter = {}, colorInner = {}, colorCap = {};

    //Optional Fields
    uint64_t optionalFieldsFlags = 0;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags = 0;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags = 0; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;

    void Randomize();
    GeometryData GetGeometry();
};

// Plate flange: an annular disc with a bore, plus a raised face projecting from one side.
struct FLANGE : public META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::Flange;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;

    //Mandatory Fields
    XMFLOAT3 center1 = {}, center2 = {}; // The two flange faces (axis + thickness).
    float flangeOuterDiameter = 0.6f;
    float boreDiameter = 0.25f;
    float raisedFaceDiameter = 0.4f;     // Raised face outer diameter (bore..flange OD).
    float raisedFaceProjection = 0.03f;  // How far the raised face protrudes past the center2 face.
    XMHALF4 colorFace = {}, colorRim = {}, colorBore = {};

    //Optional Fields
    uint64_t optionalFieldsFlags = 0;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags = 0;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags = 0; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;

    void Randomize();
    GeometryData GetGeometry();
};

struct T_Y {
    META_DATA metaData;

    //Mandatory Fields

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};

struct REDUCER {
    META_DATA metaData;

    //Mandatory Fields

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};

struct VALVE { //All kinds of valve including MOVs.
    META_DATA metaData;

    //Mandatory Fields

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};

struct PIPELINE { // This is the main container object of piping activities.
    META_DATA metaData;

    //Mandatory Fields

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    c_string name;
};

/************************* IMPLEMENTATIONS *************************/

// Appends a hollow, capped circular tube between c1 and c2 to an existing GeometryData.
// Same tessellation as PIPE; reused by TEE (main + branch) and FLANGE (body + raised face).
inline void AppendPipeTube(GeometryData& geometry, const XMFLOAT3& c1, const XMFLOAT3& c2,
    float outsideDiameter, float insideDiameter,
    const XMHALF4& colorOuter, const XMHALF4& colorInner, const XMHALF4& colorCap) {
    const int numSegments = 36;
    const float outerR = outsideDiameter * 0.5f;
    const float innerR = insideDiameter * 0.5f;

    XMVECTOR p1 = XMLoadFloat3(&c1);
    XMVECTOR p2 = XMLoadFloat3(&c2);
    XMVECTOR axis = XMVector3Normalize(p2 - p1);

    // Build an orthonormal basis around the tube axis.
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    if (fabs(XMVectorGetX(XMVector3Dot(axis, up))) > 0.99f) up = XMVectorSet(1, 0, 0, 0);
    XMVECTOR tangent = XMVector3Normalize(XMVector3Cross(up, axis));
    XMVECTOR bitangent = XMVector3Cross(axis, tangent);

    auto AddQuad = [&](XMVECTOR a, XMVECTOR b, XMVECTOR c, XMVECTOR d,
        XMVECTOR normalVec, const XMHALF4& color) {
            XMFLOAT3 normalF;
            XMStoreFloat3(&normalF, XMVector3Normalize(normalVec));
            XMUBYTE4 packedNormal = PackNormal(normalF);
            uint16_t base = static_cast<uint16_t>(geometry.vertices.size());

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

    for (int i = 0; i < numSegments; ++i) {
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

        AddQuad(o1, o2, o3, o4, dir0, colorOuter);   // Outer wall
        AddQuad(i4, i3, i2, i1, -dir0, colorInner);  // Inner wall (reverse normal)
        AddQuad(i2, i1, o1, o2, -axis, colorCap);    // Start cap ring
        AddQuad(o4, o3, i3, i4, axis, colorCap);     // End cap ring
    }
}

// ELBOW — the outer/inner walls are torus arcs (same math as TORUS, but theta runs 0..sweep).
inline GeometryData ELBOW::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    const int majorSegments = 32; // Along the bend sweep.
    const int minorSegments = 24; // Around the tube cross-section.
    const float outerR = outsideDiameter * 0.5f;
    const float innerR = insideDiameter * 0.5f;
    geometry.vertices.reserve((majorSegments + 1) * minorSegments * 2 + minorSegments * 4);
    geometry.indices.reserve(majorSegments * minorSegments * 12 + minorSegments * 12);

    // Emits one shell (outer or inner). facingOutward flips normal sign + winding so the
    // visible side is lit and not back-face culled.
    auto AddShell = [&](float tubeRadius, bool facingOutward, const XMHALF4& color) {
        const uint16_t base = static_cast<uint16_t>(geometry.vertices.size());
        for (int i = 0; i <= majorSegments; ++i) {
            const float theta = sweepAngleRadians * i / majorSegments;
            const float ct = cosf(theta), st = sinf(theta);
            for (int j = 0; j < minorSegments; ++j) {
                const float phi = XM_2PI * j / minorSegments;
                const float cp = cosf(phi), sp = sinf(phi);
                const float ring = bendRadius + tubeRadius * cp;
                XMFLOAT3 pos = { center.x + ring * ct, center.y + tubeRadius * sp, center.z + ring * st };
                XMFLOAT3 nrm = { ct * cp, sp, st * cp };
                if (!facingOutward) { nrm = { -nrm.x, -nrm.y, -nrm.z }; }
                geometry.vertices.push_back(Vertex{ pos, PackNormal(nrm), color });
            }
        }
        for (int i = 0; i < majorSegments; ++i) {
            for (int j = 0; j < minorSegments; ++j) {
                const int nj = (j + 1) % minorSegments;
                const uint16_t a = base + static_cast<uint16_t>(i * minorSegments + j);
                const uint16_t b = base + static_cast<uint16_t>((i + 1) * minorSegments + j);
                const uint16_t c = base + static_cast<uint16_t>((i + 1) * minorSegments + nj);
                const uint16_t d = base + static_cast<uint16_t>(i * minorSegments + nj);
                if (facingOutward) geometry.indices.insert(geometry.indices.end(), { a, b, d, d, b, c });
                else               geometry.indices.insert(geometry.indices.end(), { a, d, b, d, c, b });
            }
        }
    };
    AddShell(outerR, true, colorOuter);
    AddShell(innerR, false, colorInner);

    // Annular end caps at theta = 0 and theta = sweep.
    auto AddCap = [&](float theta, bool atStart) {
        const float ct = cosf(theta), st = sinf(theta);
        // Cap faces along the centerline tangent (-sin, 0, cos); the start cap faces backward.
        XMFLOAT3 nrm = atStart ? XMFLOAT3{ st, 0.0f, -ct } : XMFLOAT3{ -st, 0.0f, ct };
        XMUBYTE4 packed = PackNormal(nrm);
        const uint16_t base = static_cast<uint16_t>(geometry.vertices.size());
        for (int j = 0; j < minorSegments; ++j) {
            const float phi = XM_2PI * j / minorSegments;
            const float cp = cosf(phi), sp = sinf(phi);
            const float ringO = bendRadius + outerR * cp;
            const float ringI = bendRadius + innerR * cp;
            geometry.vertices.push_back(Vertex{ { center.x + ringO * ct, center.y + outerR * sp, center.z + ringO * st }, packed, colorCap });
            geometry.vertices.push_back(Vertex{ { center.x + ringI * ct, center.y + innerR * sp, center.z + ringI * st }, packed, colorCap });
        }
        for (int j = 0; j < minorSegments; ++j) {
            const int nj = (j + 1) % minorSegments;
            const uint16_t o0 = base + static_cast<uint16_t>(j * 2 + 0);
            const uint16_t i0 = base + static_cast<uint16_t>(j * 2 + 1);
            const uint16_t o1 = base + static_cast<uint16_t>(nj * 2 + 0);
            const uint16_t i1 = base + static_cast<uint16_t>(nj * 2 + 1);
            if (atStart) geometry.indices.insert(geometry.indices.end(), { o0, i0, i1, o0, i1, o1 });
            else         geometry.indices.insert(geometry.indices.end(), { o0, i1, i0, o0, o1, i1 });
        }
    };
    AddCap(0.0f, true);
    AddCap(sweepAngleRadians, false);

    return geometry;
}

inline void ELBOW::Randomize() {
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> bendDist(0.4f, 0.9f);
    std::uniform_real_distribution<float> odDist(0.15f, 0.3f);
    std::uniform_real_distribution<float> thickDist(0.03f, 0.08f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);
    // Standard elbow angles: 22.5, 45, 90, 135, 180 degrees.
    static const float kAngles[] = { XM_PI / 8, XM_PIDIV4, XM_PIDIV2, 3.0f * XM_PI / 4, XM_PI };
    std::uniform_int_distribution<int> angleDist(0, 4);
    auto& rng = GetRNG();

    center = { posDist(rng), posDist(rng), posDist(rng) };
    bendRadius = bendDist(rng);
    outsideDiameter = odDist(rng);
    insideDiameter = outsideDiameter - thickDist(rng);
    sweepAngleRadians = kAngles[angleDist(rng)];
    colorOuter = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorInner = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorCap = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
}

// TEE — main run plus an angled branch. No CSG: the two hollow tubes interpenetrate and the
// overlap is hidden inside the closed outer surfaces.
inline GeometryData TEE::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.vertices.reserve(36 * 32);
    geometry.indices.reserve(36 * 48);

    AppendPipeTube(geometry, center1, center2, mainOutsideDiameter, mainInsideDiameter,
        colorOuter, colorInner, colorCap);

    // Branch leaves the main-run midpoint at branchAngleDegrees from the main axis.
    XMVECTOR p1 = XMLoadFloat3(&center1);
    XMVECTOR p2 = XMLoadFloat3(&center2);
    XMVECTOR axis = XMVector3Normalize(p2 - p1);
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    if (fabs(XMVectorGetX(XMVector3Dot(axis, up))) > 0.99f) up = XMVectorSet(1, 0, 0, 0);
    XMVECTOR side = XMVector3Normalize(XMVector3Cross(up, axis)); // Perpendicular to the main axis.
    const float angle = branchAngleDegrees * (XM_PI / 180.0f);
    XMVECTOR branchDir = XMVector3Normalize(cosf(angle) * axis + sinf(angle) * side);
    XMVECTOR mid = 0.5f * (p1 + p2);

    XMFLOAT3 branchStart, branchEnd;
    XMStoreFloat3(&branchStart, mid);
    XMStoreFloat3(&branchEnd, mid + branchDir * branchLength);
    AppendPipeTube(geometry, branchStart, branchEnd, branchOutsideDiameter, branchInsideDiameter,
        colorOuter, colorInner, colorCap);

    return geometry;
}

inline void TEE::Randomize() {
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> lenDist(0.6f, 1.2f);
    std::uniform_real_distribution<float> odDist(0.2f, 0.35f);
    std::uniform_real_distribution<float> thickDist(0.03f, 0.08f);
    std::uniform_real_distribution<float> branchRatioDist(0.4f, 0.9f);
    std::uniform_real_distribution<float> branchLenDist(0.3f, 0.6f);
    std::uniform_real_distribution<float> angleDist(45.0f, 135.0f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);
    auto& rng = GetRNG();

    center1 = { posDist(rng), posDist(rng), posDist(rng) };
    const float length = lenDist(rng);
    center2 = { center1.x, center1.y + length, center1.z };
    mainOutsideDiameter = odDist(rng);
    mainInsideDiameter = mainOutsideDiameter - thickDist(rng);
    branchAngleDegrees = angleDist(rng);
    branchLength = branchLenDist(rng);
    branchOutsideDiameter = mainOutsideDiameter * branchRatioDist(rng); // Independent of the main run.
    branchInsideDiameter = branchOutsideDiameter - thickDist(rng) * 0.5f;
    colorOuter = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorInner = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorCap = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
}

// FLANGE — a hollow disc (body) plus a shorter hollow disc (raised face) projecting from one side.
inline GeometryData FLANGE::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    geometry.vertices.reserve(36 * 32);
    geometry.indices.reserve(36 * 48);

    // Flange body: outer rim, bore wall, and two annular faces.
    AppendPipeTube(geometry, center1, center2, flangeOuterDiameter, boreDiameter,
        colorRim, colorBore, colorFace);

    // Raised face: a shorter disc projecting past the center2 face.
    if (raisedFaceProjection > 0.0f && raisedFaceDiameter > boreDiameter) {
        XMVECTOR c1 = XMLoadFloat3(&center1);
        XMVECTOR c2 = XMLoadFloat3(&center2);
        XMVECTOR axis = XMVector3Normalize(c2 - c1);
        XMFLOAT3 rfStart, rfEnd;
        XMStoreFloat3(&rfStart, c2);
        XMStoreFloat3(&rfEnd, c2 + axis * raisedFaceProjection);
        AppendPipeTube(geometry, rfStart, rfEnd, raisedFaceDiameter, boreDiameter,
            colorFace, colorBore, colorFace);
    }

    return geometry;
}

inline void FLANGE::Randomize() {
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> thickDist(0.05f, 0.12f);
    std::uniform_real_distribution<float> odDist(0.4f, 0.7f);
    std::uniform_real_distribution<float> boreDist(0.15f, 0.3f);
    std::uniform_real_distribution<float> rfProjDist(0.02f, 0.05f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);
    auto& rng = GetRNG();

    center1 = { posDist(rng), posDist(rng), posDist(rng) };
    const float thickness = thickDist(rng);
    center2 = { center1.x, center1.y + thickness, center1.z };
    flangeOuterDiameter = odDist(rng);
    boreDiameter = boreDist(rng);
    raisedFaceDiameter = (flangeOuterDiameter + boreDiameter) * 0.5f; // Between the bore and the rim.
    raisedFaceProjection = rfProjDist(rng);
    colorFace = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorRim = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorBore = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
}
