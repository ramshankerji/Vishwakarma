// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#pragma once

#include <cstdint>

namespace VishwakarmaStorage {

// C++ is the runtime source of truth for these MVP storage numbers.
// The .proto payloads store the object fields; SQLite object_store.object_type stores these IDs.
enum class ObjectType : uint32_t {
    Unknown = 0,
    Pyramid = 1,
    Cuboid = 2,
    Cone = 3,
    Cylinder = 4,
    Parallelepiped = 5,
    Sphere = 6,
    FrustumOfPyramid = 7,
    FrustumOfCone = 8,
    Pipe = 9,
    Folder = 10,
    Page2D = 11,
    Scene3D = 12,
    Line2D = 13,
    Polyline2D = 14,
    Polygon2D = 15,
    Text2D = 16,
};

enum class LifecycleState : uint32_t {
    Live = 0,
    SoftDeleted = 1,
    TombstoneRetained = 2,
    PurgedStubOrArchiveBoundary = 3,
};

constexpr uint16_t kGeometry3DMvpSchemaVersion = 1;
constexpr uint16_t kLogicalElementSchemaVersion = 1;
constexpr uint16_t kGeometry2DLineSchemaVersion = 1;
constexpr uint16_t kGeometry2DPolylineSchemaVersion = 1;
constexpr uint16_t kGeometry2DPolygonSchemaVersion = 1;
constexpr uint16_t kGeometry2DTextSchemaVersion = 1;
constexpr uint64_t kMaxLocalObjectId = (1ULL << 40) - 1ULL;

constexpr uint32_t ToNumber(ObjectType value) {
    return static_cast<uint32_t>(value);
}

constexpr uint32_t ToNumber(LifecycleState value) {
    return static_cast<uint32_t>(value);
}

constexpr bool IsGeometry3DObjectType(ObjectType value) {
    return value >= ObjectType::Pyramid && value <= ObjectType::Pipe;
}

constexpr bool IsLogicalObjectType(ObjectType value) {
    return value == ObjectType::Folder ||
        value == ObjectType::Page2D ||
        value == ObjectType::Scene3D;
}

constexpr bool IsGeometry2DObjectType(ObjectType value) {
    return value == ObjectType::Line2D ||
        value == ObjectType::Polyline2D ||
        value == ObjectType::Polygon2D ||
        value == ObjectType::Text2D;
}

inline const char* ObjectTypeDisplayName(ObjectType value) {
    switch (value) {
    case ObjectType::Pyramid: return "Pyramid";
    case ObjectType::Cuboid: return "Cuboid";
    case ObjectType::Cone: return "Cone";
    case ObjectType::Cylinder: return "Cylinder";
    case ObjectType::Parallelepiped: return "Parallelepiped";
    case ObjectType::Sphere: return "Sphere";
    case ObjectType::FrustumOfPyramid: return "Frustum of Pyramid";
    case ObjectType::FrustumOfCone: return "Frustum of Cone";
    case ObjectType::Pipe: return "Pipe";
    case ObjectType::Folder: return "Folder";
    case ObjectType::Page2D: return "Page2D";
    case ObjectType::Scene3D: return "Scene3D";
    case ObjectType::Line2D: return "Line2D";
    case ObjectType::Polyline2D: return "Polyline2D";
    case ObjectType::Polygon2D: return "Polygon2D";
    case ObjectType::Text2D: return "Text2D";
    default: return "Unknown";
    }
}

} // namespace VishwakarmaStorage
