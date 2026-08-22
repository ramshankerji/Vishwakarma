// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once

#include <algorithm>
#include <cstring>
#include <vector>
#include <iostream>

#include "डेटा.h"
#include "डेटा-सामान्य-3D.h" // For GeometryData, Vertex, PackNormal, GetRNG shared with the primitive shapes.
#include "डेटा-पाइप.h"       // For AppendPipeTube, reused by hollow circular (CHS) members.
#include "SteelProfileCatalog.h"

// Straight structural member between two 3D points. The cross-section comes from the
// compile-time embedded steel profile catalog (SteelProfileCatalog.h) via profileId.
struct LINE_MEMBER : public META_DATA {
    static constexpr VishwakarmaStorage::ObjectType storageObjectType = VishwakarmaStorage::ObjectType::LineMember;
    static constexpr uint16_t storageSchemaVersion = VishwakarmaStorage::kGeometry3DLineMemberSchemaVersion;

    //Mandatory Fields
    XMFLOAT3 point1 = {}, point2 = {}; // Member axis end points, SI meters.
    uint64_t profileId = 0;            // Steel profile catalog id, [2^32, 2^40) band.
    XMHALF4 colorMain = {}, colorInner = {}, colorCap = {}; // Inner shows on hollow sections (CHS).
    // Parametric-family section dimensions, millimeters; 0 = use the catalog row's defaults.
    // RECT: 1 = depth h, 2 = width b. CIRC: 1 = diameter. OCT/HEX: 1 = across flats.
    float userParameter1 = 0.0f, userParameter2 = 0.0f;

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

struct STRUCTURAL_CURVED_MEMBER {
    META_DATA metaData;

    //Mandatory Fields
    double x, y, z, radius;
    uint64_t profileID; //Pointer to the profile type

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    CustomString name;
    //c_float profileParameters;
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
    CustomString name;
    //c_float profileParameters;
};

struct PLANER_2D { //SLABS, GRATINGS, PLATES, CHEQUERED PLATES, OR ANY PLATE  IN A SINGLE PLANE.
    META_DATA metaData;

    //Mandatory Fields
    double x1, y1, z1, x2, y2, z2;

    //Optional Fields
    uint64_t optionalFieldsFlags;  // Bit-mask for up to 64 Optional Fields - 8 Bytes.
    uint32_t systemFlags;          // 32 booleans for internal use only. Not persisted.
    uint32_t objectLifeCycleFlags; // 32 booleans used as compact stored object properties. Persisted.
    CustomString name;
    //c_float profileParameters;
};

/************************* IMPLEMENTATIONS *************************/

// Section-plane basis for a member axis. Local +y (v) is world +Z projected perpendicular
// to the axis, so section heights stand vertical by default (the STAAD BETA = 0 convention);
// vertical members fall back to +X. (u, v, axis) is right-handed: a CCW outline in (x, y)
// section coordinates is CCW when viewed against the axis.
inline void MemberSectionBasis(FXMVECTOR axis, XMVECTOR& u, XMVECTOR& v) {
    XMVECTOR up = XMVectorSet(0, 0, 1, 0);
    if (fabs(XMVectorGetX(XMVector3Dot(axis, up))) > 0.99f) up = XMVectorSet(1, 0, 0, 0);
    u = XMVector3Normalize(XMVector3Cross(up, axis));
    v = XMVector3Cross(axis, u);
}

// Extrudes one CONVEX cross-section polygon (CCW, section coordinates in meters) from c1
// to c2: one flat-shaded quad per outline edge plus fan-triangulated end caps. A profile
// decomposes into a few convex pieces that may touch or interpenetrate — the same "no CSG"
// convention TEE already uses; shared faces stay hidden inside the closed outer surfaces.
inline void AppendExtrudedConvexPrism(GeometryData& geometry,
    const XMFLOAT3& c1, const XMFLOAT3& c2,
    const XMFLOAT2* sectionPoints, int pointCount) {

    XMVECTOR p1 = XMLoadFloat3(&c1);
    XMVECTOR p2 = XMLoadFloat3(&c2);
    XMVECTOR axis = XMVector3Normalize(p2 - p1);
    XMVECTOR u, v;
    MemberSectionBasis(axis, u, v);

    auto WorldPoint = [&](XMVECTOR base, int i) {
        return base + u * sectionPoints[i].x + v * sectionPoints[i].y;
    };
    auto PushVertex = [&](XMVECTOR position, XMVECTOR normal) {
        XMFLOAT3 p, n;
        XMStoreFloat3(&p, position);
        XMStoreFloat3(&n, normal);
        geometry.vertices.push_back(Vertex{ p, PackNormal(n) });
    };

    // Side walls. The outward normal of a CCW outline edge (ex, ey) is (ey, -ex).
    for (int i = 0; i < pointCount; ++i) {
        const int next = (i + 1) % pointCount;
        const float ex = sectionPoints[next].x - sectionPoints[i].x;
        const float ey = sectionPoints[next].y - sectionPoints[i].y;
        XMVECTOR normal = XMVector3Normalize(u * ey - v * ex);
        const uint16_t base = static_cast<uint16_t>(geometry.vertices.size());
        PushVertex(WorldPoint(p1, i), normal);
        PushVertex(WorldPoint(p1, next), normal);
        PushVertex(WorldPoint(p2, next), normal);
        PushVertex(WorldPoint(p2, i), normal);
        geometry.indices.insert(geometry.indices.end(), {
            base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2),
            base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3) });
    }

    // End caps as triangle fans (valid because each piece is convex).
    auto AddCap = [&](XMVECTOR base, XMVECTOR normal, bool atStart) {
        const uint16_t first = static_cast<uint16_t>(geometry.vertices.size());
        for (int i = 0; i < pointCount; ++i) PushVertex(WorldPoint(base, i), normal);
        for (int i = 1; i + 1 < pointCount; ++i) {
            if (atStart) geometry.indices.insert(geometry.indices.end(),
                { first, static_cast<uint16_t>(first + i + 1), static_cast<uint16_t>(first + i) });
            else geometry.indices.insert(geometry.indices.end(),
                { first, static_cast<uint16_t>(first + i), static_cast<uint16_t>(first + i + 1) });
        }
    };
    AddCap(p1, -axis, true);
    AddCap(p2, axis, false);
}

/* THE ONE PLACE that decides whether a member draws from the global primitive library, and what
its canonical -> authored transform is. Returns the library shape id, or -1 for a profile that has
no canonical form; `world` is written only on a non-negative return and may be null when the caller
only wants the verdict.

WHY ONE FUNCTION AND NOT TWO. Both LINE_MEMBER::GetGeometry and WorldMatrixForObject need this
answer, and they run at different times: a geometry edit goes through the generator, a MOVE goes
through the matrix builder with no geometry at all. If the two ever disagreed about whether a member
is instanced, moving it would draw it at the wrong size or in the wrong place - exactly the defect
that bit SPHERE when its local frame was composed in two places (website/software/graphics.md, "The
canonical local frame, and its single composition point"). Asking the same function twice makes them
agree by construction, and it keeps the `userParameter > 0 ? userParameter : catalog default`
fallback in one place instead of two.

WHAT IS ELIGIBLE, AND WHY IT IS A PROPERTY OF THE PROFILE ROW RATHER THAN OF THE TYPE. A solid
rectangular section extruded along the axis IS a scaled, oriented unit cube - same 24 vertices and
36 indices the bespoke path emitted - and a solid circular one is the unit cylinder, which also buys
it real LOD in place of a fixed 24-gon. That covers five series across two families:

    PARAMETRIC RECT   cuboid,   scale (width b, depth h, length)   RC beams and columns
    PARAMETRIC CIRC   cylinder, scale (d/2, d/2, length)           RC circular columns
    BAR SQUARE        cuboid,   scale (a, a, length)
    BAR FLAT          cuboid,   scale (width a, thickness b, length)
    BAR ROUND         cylinder, scale (a/2, a/2, length)

Everything else is bespoke, for one of three reasons. OCT, HEX and BAR HEX would each need their own
prism entry. The multi-piece families - I, channel, tee, angle, RHS, bulb, rail - emit two to five
convex pieces from ONE GeometryData, and `libraryShapeId` is a single field, so they wait for one
engineering object to be able to own several graphics objects. CHS waits on the inward-facing bore
deferred with PIPE.

THE SECTION BASIS IS NOT THE CYLINDER'S. MemberSectionBasis stands the section's +y along world +Z
(the STAAD BETA = 0 convention) and falls back to +X for vertical members; CYLINDER's frame in
WorldMatrixForObject uses +Y. The two differ by a ROLL about the axis, which a circle does not
notice and a rectangle very much does, so this must go through MemberSectionBasis. */
inline int16_t LineMemberCanonicalFrame(const LINE_MEMBER& member, XMMATRIX* world) {
    const SteelProfileRecord* profile = FindSteelProfileById(member.profileId);
    if (!profile) return -1; // Unset or unknown: the generator draws its fallback tube.

    constexpr float s = 0.001f; // Catalog dimensions are millimeters; model space is SI meters.
    int16_t shapeId = -1;
    float scaleU = 0.0f, scaleV = 0.0f; // Section +x and +y extents, in the canonical mesh's units.
    if (profile->family == SteelProfileFamily::Parametric) {
        /* RC sections. These are the ONLY family whose dimensions come from the member's own
        userParameter fields (0 = fall back to the catalog row) - see LINE_MEMBER's field comment. */
        if (std::strcmp(profile->series, "RECT") == 0) {
            // Canonical cube spans +/-0.5, so the scale is the FULL section dimension.
            const float h = (member.userParameter1 > 0.0f ? member.userParameter1 : profile->h) * s;
            const float b = (member.userParameter2 > 0.0f ? member.userParameter2 : profile->b) * s;
            if (!(h > 0.0f && b > 0.0f)) return -1;
            shapeId = kPrimitiveShapeCuboid;
            scaleU = b; scaleV = h;
        } else if (std::strcmp(profile->series, "CIRC") == 0) {
            // Canonical cylinder has radius 1, so the scale is the RADIUS on both section axes.
            const float d = (member.userParameter1 > 0.0f ? member.userParameter1 : profile->d) * s;
            if (!(d > 0.0f)) return -1;
            shapeId = kPrimitiveShapeCylinder;
            scaleU = scaleV = d * 0.5f;
        } else {
            return -1; // OCT and HEX would each need their own prism entry.
        }
    } else if (profile->family == SteelProfileFamily::Bar) {
        /* Solid bar stock, sized by the catalog row alone - the userParameter fields are declared
        parametric-family only, and reading them here would silently resize every bar a user had
        ever typed a parameter into. */
        const float a = profile->a * s;
        if (!(a > 0.0f)) return -1;
        if (std::strcmp(profile->series, "ROUND") == 0) {
            shapeId = kPrimitiveShapeCylinder;
            scaleU = scaleV = a * 0.5f; // a = diameter.
        } else if (std::strcmp(profile->series, "SQUARE") == 0) {
            shapeId = kPrimitiveShapeCuboid;
            scaleU = scaleV = a;        // a = side.
        } else if (std::strcmp(profile->series, "FLAT") == 0) {
            const float b = profile->b * s; // a = width (section +x), b = thickness (section +y).
            if (!(b > 0.0f)) return -1;
            shapeId = kPrimitiveShapeCuboid;
            scaleU = a; scaleV = b;
        } else {
            /* HEX needs a hexagonal prism entry. Note the generator's BAR branch treats anything
            that is not ROUND / HEX / SQUARE as FLAT, where this names FLAT explicitly: a series
            neither has heard of therefore draws bespoke-as-flat instead of instanced-as-flat, which
            is the same picture and the safe direction to disagree in. */
            return -1;
        }
    } else {
        return -1;
    }
    if (!world) return shapeId;

    XMVECTOR p1 = XMLoadFloat3(&member.point1), p2 = XMLoadFloat3(&member.point2);
    XMVECTOR delta = XMVectorSubtract(p2, p1);
    const float length = XMVectorGetX(XMVector3Length(delta));
    if (length < 1e-6f) return -1; // No axis; the generator draws nothing for this member either.
    XMVECTOR axis = XMVector3Normalize(delta);
    XMVECTOR u, v;
    MemberSectionBasis(axis, u, v);

    // Row-vector order, and the translation row is the MIDPOINT because both canonical meshes are
    // centred on their own local z - the same construction CYLINDER uses.
    world->r[0] = XMVectorScale(u, scaleU);
    world->r[1] = XMVectorScale(v, scaleV);
    world->r[2] = XMVectorScale(axis, length);
    world->r[3] = XMVectorSetW(XMVectorScale(XMVectorAdd(p1, p2), 0.5f), 1.0f);
    return shapeId;
}

// LINE_MEMBER — cross-section outline per catalog family, extruded point1 -> point2.
// v1 fidelity: sharp corners. flange_slope is honored (tf at the (b-tw)/4 gauge point);
// fillet radii r1/r2 and RHS corner radii are ignored; RAIL is an envelope approximation.
// Sections are centered on the bounding-box center, not the exact centroid.
inline GeometryData LINE_MEMBER::GetGeometry() {
    GeometryData geometry;
    geometry.id = memoryID;
    // The section walls are the whole member visually; colorInner (hollow CHS/RHS bores) and
    // colorCap stay stored but no longer reach the GPU (डेटा.h, Vertex).
    geometry.color = ToFloat4(colorMain);

    XMVECTOR p1 = XMLoadFloat3(&point1), p2 = XMLoadFloat3(&point2);
    if (XMVectorGetX(XMVector3LengthSq(p2 - p1)) < 1e-12f) return geometry; // Zero length: no axis.

    /* Solid rectangular and circular sections emit NO GEOMETRY AT ALL - they name a library shape
    and let the world matrix carry the section dimensions and the axis. See
    LineMemberCanonicalFrame, which is also what WorldMatrixForObject asks, so the two cannot
    disagree about whether this member owns vertices. */
    const int16_t libraryShape = LineMemberCanonicalFrame(*this, nullptr);
    if (libraryShape >= 0) {
        geometry.libraryShapeId = libraryShape;
        return geometry;
    }

    constexpr float s = 0.001f; // Catalog dimensions are millimeters; model space is SI meters.
    const SteelProfileRecord* profile = FindSteelProfileById(profileId);
    if (!profile) {
        // Unknown or unset profile: draw a fallback tube (114.3 x 5 CHS) so the member stays visible.
        AppendPipeTube(geometry, point1, point2, 114.3f * s, 104.3f * s);
        return geometry;
    }

    auto Piece = [&](const XMFLOAT2* points, int count) {
        AppendExtrudedConvexPrism(geometry, point1, point2, points, count);
    };
    auto Rect = [&](float x0, float y0, float x1, float y1) {
        const XMFLOAT2 points[4] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
        Piece(points, 4);
    };
    auto Quad = [&](XMFLOAT2 a, XMFLOAT2 b, XMFLOAT2 c, XMFLOAT2 d) { // CCW
        const XMFLOAT2 points[4] = { a, b, c, d };
        Piece(points, 4);
    };
    auto Polygon = [&](float circumRadius, int count, float startAngleRadians) {
        XMFLOAT2 points[24];
        for (int i = 0; i < count; ++i) {
            const float angle = startAngleRadians + XM_2PI * i / count;
            points[i] = { circumRadius * cosf(angle), circumRadius * sinf(angle) };
        }
        Piece(points, count);
    };
    // Flange outstand thicknesses: tf is given at the (b-tw)/4 gauge point, the middle of the
    // outstand; a linear inner-face slope adds/removes half the taper at the web face / toe.
    auto TaperedThickness = [&](float tf, float outstand, float slopePercent, float& tRoot, float& tToe) {
        const float halfTaper = slopePercent * 0.01f * outstand * 0.5f;
        tToe = (std::max)(tf - halfTaper, tf * 0.25f); // Clamp: survive bad slope data.
        tRoot = tf + halfTaper;
    };

    switch (profile->family) {
    case SteelProfileFamily::I: {
        const float h = profile->h * s, b = profile->b * s, tw = profile->tw * s, tf = profile->tf * s;
        float tRoot, tToe;
        TaperedThickness(tf, (b - tw) * 0.5f, profile->flangeSlope, tRoot, tToe);
        Rect(-tw * 0.5f, -h * 0.5f, tw * 0.5f, h * 0.5f); // Web slab, full height.
        Quad({ tw * 0.5f, h * 0.5f - tRoot }, { b * 0.5f, h * 0.5f - tToe }, { b * 0.5f, h * 0.5f }, { tw * 0.5f, h * 0.5f });
        Quad({ -b * 0.5f, h * 0.5f - tToe }, { -tw * 0.5f, h * 0.5f - tRoot }, { -tw * 0.5f, h * 0.5f }, { -b * 0.5f, h * 0.5f });
        Quad({ tw * 0.5f, -h * 0.5f }, { b * 0.5f, -h * 0.5f }, { b * 0.5f, -h * 0.5f + tToe }, { tw * 0.5f, -h * 0.5f + tRoot });
        Quad({ -b * 0.5f, -h * 0.5f }, { -tw * 0.5f, -h * 0.5f }, { -tw * 0.5f, -h * 0.5f + tRoot }, { -b * 0.5f, -h * 0.5f + tToe });
        break;
    }
    case SteelProfileFamily::Channel: {
        const float h = profile->h * s, b = profile->b * s, tw = profile->tw * s, tf = profile->tf * s;
        const float x0 = -b * 0.5f + tw; // Flange outstand starts at the web face.
        float tRoot, tToe;
        TaperedThickness(tf, b - tw, profile->flangeSlope, tRoot, tToe);
        Rect(-b * 0.5f, -h * 0.5f, x0, h * 0.5f); // Web at the left, full height; opens toward +x.
        Quad({ x0, h * 0.5f - tRoot }, { b * 0.5f, h * 0.5f - tToe }, { b * 0.5f, h * 0.5f }, { x0, h * 0.5f });
        Quad({ x0, -h * 0.5f }, { b * 0.5f, -h * 0.5f }, { b * 0.5f, -h * 0.5f + tToe }, { x0, -h * 0.5f + tRoot });
        break;
    }
    case SteelProfileFamily::Tee: {
        const float h = profile->h * s, b = profile->b * s, tw = profile->tw * s, tf = profile->tf * s;
        float tRoot, tToe;
        TaperedThickness(tf, (b - tw) * 0.5f, profile->flangeSlope, tRoot, tToe);
        Rect(-tw * 0.5f, -h * 0.5f, tw * 0.5f, h * 0.5f); // Stem, full height; flange on top.
        Quad({ tw * 0.5f, h * 0.5f - tRoot }, { b * 0.5f, h * 0.5f - tToe }, { b * 0.5f, h * 0.5f }, { tw * 0.5f, h * 0.5f });
        Quad({ -b * 0.5f, h * 0.5f - tToe }, { -tw * 0.5f, h * 0.5f - tRoot }, { -tw * 0.5f, h * 0.5f }, { -b * 0.5f, h * 0.5f });
        break;
    }
    case SteelProfileFamily::Angle: {
        const float a = profile->a * s, b = profile->b * s, t = profile->t * s;
        const float t2 = (profile->t2 > 0.0f ? profile->t2 : profile->t) * s; // JIS L-UT second thickness.
        Rect(-b * 0.5f, -a * 0.5f, -b * 0.5f + t, a * 0.5f);      // Vertical leg (length a), full height.
        Rect(-b * 0.5f + t, -a * 0.5f, b * 0.5f, -a * 0.5f + t2); // Horizontal leg from the vertical-leg face.
        break;
    }
    case SteelProfileFamily::CHS: {
        const float outside = profile->d * s;
        const float inside = (std::max)(profile->d - 2.0f * profile->t, 0.0f) * s;
        AppendPipeTube(geometry, point1, point2, outside, inside);
        break;
    }
    case SteelProfileFamily::RHS: {
        const float h = profile->h * s, b = profile->b * s, t = profile->t * s;
        Rect(-b * 0.5f, h * 0.5f - t, b * 0.5f, h * 0.5f);            // Top wall.
        Rect(-b * 0.5f, -h * 0.5f, b * 0.5f, -h * 0.5f + t);          // Bottom wall.
        Rect(-b * 0.5f, -h * 0.5f + t, -b * 0.5f + t, h * 0.5f - t);  // Left wall.
        Rect(b * 0.5f - t, -h * 0.5f + t, b * 0.5f, h * 0.5f - t);    // Right wall.
        break;
    }
    case SteelProfileFamily::Bar: {
        const float a = profile->a * s;
        if (std::strcmp(profile->series, "ROUND") == 0) {
            Polygon(a * 0.5f, 24, 0.0f);
        } else if (std::strcmp(profile->series, "HEX") == 0) {
            Polygon(a / sqrtf(3.0f), 6, 0.0f); // a = across flats; flats land top and bottom.
        } else if (std::strcmp(profile->series, "SQUARE") == 0) {
            Rect(-a * 0.5f, -a * 0.5f, a * 0.5f, a * 0.5f);
        } else { // FLAT: a = width, b = thickness.
            const float b = profile->b * s;
            Rect(-a * 0.5f, -b * 0.5f, a * 0.5f, b * 0.5f);
        }
        break;
    }
    case SteelProfileFamily::Bulb: {
        const float h = profile->h * s, t = profile->t * s, bulbH = profile->bulbH * s;
        Rect(-t * 0.5f, -h * 0.5f, t * 0.5f, h * 0.5f); // Web plate, full height.
        // Bulb approximated as one convex quad on the +x side of the top edge.
        Quad({ t * 0.5f, h * 0.5f - bulbH }, { t * 0.5f + 1.5f * t, h * 0.5f - 0.75f * bulbH },
             { t * 0.5f + 1.5f * t, h * 0.5f - 0.25f * bulbH }, { t * 0.5f, h * 0.5f });
        break;
    }
    case SteelProfileFamily::Rail: {
        const float h = profile->h * s, bHead = profile->bHead * s, bFoot = profile->bFoot * s, tw = profile->tw * s;
        const float footT = 0.20f * h, headT = 0.30f * h; // Envelope approximation (SteelTable.md).
        Rect(-bFoot * 0.5f, -h * 0.5f, bFoot * 0.5f, -h * 0.5f + footT);
        Rect(-bHead * 0.5f, h * 0.5f - headT, bHead * 0.5f, h * 0.5f);
        Rect(-tw * 0.5f, -h * 0.5f + footT, tw * 0.5f, h * 0.5f - headT);
        break;
    }
    case SteelProfileFamily::Parametric: {
        // RC beam/column sections: dimensions come from the member's user parameters
        // (millimeters; 0 = the catalog row's defaults), the series picks the outline.
        if (std::strcmp(profile->series, "RECT") == 0) {
            const float h = (userParameter1 > 0.0f ? userParameter1 : profile->h) * s; // Depth, section +y.
            const float b = (userParameter2 > 0.0f ? userParameter2 : profile->b) * s; // Width.
            Rect(-b * 0.5f, -h * 0.5f, b * 0.5f, h * 0.5f);
        } else if (std::strcmp(profile->series, "CIRC") == 0) {
            const float d = (userParameter1 > 0.0f ? userParameter1 : profile->d) * s;
            Polygon(d * 0.5f, 24, 0.0f); // Same fidelity as BAR ROUND.
        } else if (std::strcmp(profile->series, "OCT") == 0) {
            const float af = (userParameter1 > 0.0f ? userParameter1 : profile->a) * s; // Across flats.
            Polygon(af * 0.5f / cosf(XM_PI / 8.0f), 8, XM_PI / 8.0f); // Flats land top/bottom and left/right.
        } else if (std::strcmp(profile->series, "HEX") == 0) {
            const float af = (userParameter1 > 0.0f ? userParameter1 : profile->a) * s; // Across flats.
            Polygon(af / sqrtf(3.0f), 6, 0.0f); // Flats land top and bottom, like BAR HEX.
        }
        break;
    }
    }
    return geometry;
}

inline void LINE_MEMBER::Randomize() {
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> lengthDist(1.0f, 3.0f);
    std::uniform_real_distribution<float> dirDist(-1.0f, 1.0f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);
    std::uniform_int_distribution<uint32_t> profileDist(0, kSteelProfileCount - 1);
    auto& rng = GetRNG();

    profileId = kSteelProfiles[profileDist(rng)].id; // Every click exercises another catalog row.
    point1 = { posDist(rng), posDist(rng), posDist(rng) };
    XMVECTOR direction = XMVectorSet(dirDist(rng), dirDist(rng), dirDist(rng), 0);
    if (XMVectorGetX(XMVector3LengthSq(direction)) < 1e-4f) direction = XMVectorSet(1, 0, 0, 0);
    XMStoreFloat3(&point2, XMLoadFloat3(&point1) + XMVector3Normalize(direction) * lengthDist(rng));
    colorMain = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorInner = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    colorCap = XMHALF4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
}
