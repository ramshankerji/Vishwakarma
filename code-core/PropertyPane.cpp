// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

// Property descriptor tables for the right-side properties pane.
// Design document: website/content/software/propertiesPane.md

#include "PropertyPane.h"

// डेटा-सामान्य-3D.h defines _USE_MATH_DEFINES then declares its own `constexpr float M_PI`, which only
// compiles when <math.h> was already pulled in (via DirectXMath in डेटा.h) *before* that define.
// Include डेटा.h first so this TU matches the working order in DataStorage.cpp / विश्वकर्मा.cpp.
#include "डेटा.h"
#include "डेटा-सामान्य-3D.h" // SPHERE, CYLINDER, ... and META_DATA

#include <cmath> // std::isfinite

using VishwakarmaStorage::ObjectType;

namespace {

// Each accessor is a captureless lambda that decays to a function pointer: portable, no casts at
// the call site, and type-checked at compile time.

// SPHERE: Center X/Y/Z, Radius.
const PropertyFieldDescriptor kSphereFields[] = {
    { UITextID::PropCenterX, [](const META_DATA* o) { return static_cast<const SPHERE*>(o)->center.x; },
        [](META_DATA* o, float v) { static_cast<SPHERE*>(o)->center.x = v; }, PropertyFieldKind::Float32, 0, false },
    { UITextID::PropCenterY, [](const META_DATA* o) { return static_cast<const SPHERE*>(o)->center.y; },
        [](META_DATA* o, float v) { static_cast<SPHERE*>(o)->center.y = v; }, PropertyFieldKind::Float32, 1, false },
    { UITextID::PropCenterZ, [](const META_DATA* o) { return static_cast<const SPHERE*>(o)->center.z; },
        [](META_DATA* o, float v) { static_cast<SPHERE*>(o)->center.z = v; }, PropertyFieldKind::Float32, 2, false },
    { UITextID::PropRadius, [](const META_DATA* o) { return static_cast<const SPHERE*>(o)->radius; },
        [](META_DATA* o, float v) { static_cast<SPHERE*>(o)->radius = v; }, PropertyFieldKind::Float32, 3, true },
};

// CYLINDER: P1 X/Y/Z, P2 X/Y/Z, Radius.
const PropertyFieldDescriptor kCylinderFields[] = {
    { UITextID::PropPoint1X, [](const META_DATA* o) { return static_cast<const CYLINDER*>(o)->p1.x; },
        [](META_DATA* o, float v) { static_cast<CYLINDER*>(o)->p1.x = v; }, PropertyFieldKind::Float32, 0, false },
    { UITextID::PropPoint1Y, [](const META_DATA* o) { return static_cast<const CYLINDER*>(o)->p1.y; },
        [](META_DATA* o, float v) { static_cast<CYLINDER*>(o)->p1.y = v; }, PropertyFieldKind::Float32, 1, false },
    { UITextID::PropPoint1Z, [](const META_DATA* o) { return static_cast<const CYLINDER*>(o)->p1.z; },
        [](META_DATA* o, float v) { static_cast<CYLINDER*>(o)->p1.z = v; }, PropertyFieldKind::Float32, 2, false },
    { UITextID::PropPoint2X, [](const META_DATA* o) { return static_cast<const CYLINDER*>(o)->p2.x; },
        [](META_DATA* o, float v) { static_cast<CYLINDER*>(o)->p2.x = v; }, PropertyFieldKind::Float32, 3, false },
    { UITextID::PropPoint2Y, [](const META_DATA* o) { return static_cast<const CYLINDER*>(o)->p2.y; },
        [](META_DATA* o, float v) { static_cast<CYLINDER*>(o)->p2.y = v; }, PropertyFieldKind::Float32, 4, false },
    { UITextID::PropPoint2Z, [](const META_DATA* o) { return static_cast<const CYLINDER*>(o)->p2.z; },
        [](META_DATA* o, float v) { static_cast<CYLINDER*>(o)->p2.z = v; }, PropertyFieldKind::Float32, 5, false },
    { UITextID::PropRadius, [](const META_DATA* o) { return static_cast<const CYLINDER*>(o)->radius; },
        [](META_DATA* o, float v) { static_cast<CYLINDER*>(o)->radius = v; }, PropertyFieldKind::Float32, 6, true },
};

// CONE: Apex X/Y/Z, Base Center X/Y/Z, Radius.
const PropertyFieldDescriptor kConeFields[] = {
    { UITextID::PropApexX, [](const META_DATA* o) { return static_cast<const CONE*>(o)->apex.x; },
        [](META_DATA* o, float v) { static_cast<CONE*>(o)->apex.x = v; }, PropertyFieldKind::Float32, 0, false },
    { UITextID::PropApexY, [](const META_DATA* o) { return static_cast<const CONE*>(o)->apex.y; },
        [](META_DATA* o, float v) { static_cast<CONE*>(o)->apex.y = v; }, PropertyFieldKind::Float32, 1, false },
    { UITextID::PropApexZ, [](const META_DATA* o) { return static_cast<const CONE*>(o)->apex.z; },
        [](META_DATA* o, float v) { static_cast<CONE*>(o)->apex.z = v; }, PropertyFieldKind::Float32, 2, false },
    { UITextID::PropBaseCenterX, [](const META_DATA* o) { return static_cast<const CONE*>(o)->baseCenter.x; },
        [](META_DATA* o, float v) { static_cast<CONE*>(o)->baseCenter.x = v; }, PropertyFieldKind::Float32, 3, false },
    { UITextID::PropBaseCenterY, [](const META_DATA* o) { return static_cast<const CONE*>(o)->baseCenter.y; },
        [](META_DATA* o, float v) { static_cast<CONE*>(o)->baseCenter.y = v; }, PropertyFieldKind::Float32, 4, false },
    { UITextID::PropBaseCenterZ, [](const META_DATA* o) { return static_cast<const CONE*>(o)->baseCenter.z; },
        [](META_DATA* o, float v) { static_cast<CONE*>(o)->baseCenter.z = v; }, PropertyFieldKind::Float32, 5, false },
    { UITextID::PropRadius, [](const META_DATA* o) { return static_cast<const CONE*>(o)->radius; },
        [](META_DATA* o, float v) { static_cast<CONE*>(o)->radius = v; }, PropertyFieldKind::Float32, 6, true },
};

// TORUS: Center X/Y/Z, Major Radius, Minor Radius.
const PropertyFieldDescriptor kTorusFields[] = {
    { UITextID::PropCenterX, [](const META_DATA* o) { return static_cast<const TORUS*>(o)->center.x; },
        [](META_DATA* o, float v) { static_cast<TORUS*>(o)->center.x = v; }, PropertyFieldKind::Float32, 0, false },
    { UITextID::PropCenterY, [](const META_DATA* o) { return static_cast<const TORUS*>(o)->center.y; },
        [](META_DATA* o, float v) { static_cast<TORUS*>(o)->center.y = v; }, PropertyFieldKind::Float32, 1, false },
    { UITextID::PropCenterZ, [](const META_DATA* o) { return static_cast<const TORUS*>(o)->center.z; },
        [](META_DATA* o, float v) { static_cast<TORUS*>(o)->center.z = v; }, PropertyFieldKind::Float32, 2, false },
    { UITextID::PropMajorRadius, [](const META_DATA* o) { return static_cast<const TORUS*>(o)->majorRadius; },
        [](META_DATA* o, float v) { static_cast<TORUS*>(o)->majorRadius = v; }, PropertyFieldKind::Float32, 3, true },
    { UITextID::PropMinorRadius, [](const META_DATA* o) { return static_cast<const TORUS*>(o)->minorRadius; },
        [](META_DATA* o, float v) { static_cast<TORUS*>(o)->minorRadius = v; }, PropertyFieldKind::Float32, 4, true },
};

// ELLIPSOID: Center X/Y/Z, Radius X/Y/Z.
const PropertyFieldDescriptor kEllipsoidFields[] = {
    { UITextID::PropCenterX, [](const META_DATA* o) { return static_cast<const ELLIPSOID*>(o)->center.x; },
        [](META_DATA* o, float v) { static_cast<ELLIPSOID*>(o)->center.x = v; }, PropertyFieldKind::Float32, 0, false },
    { UITextID::PropCenterY, [](const META_DATA* o) { return static_cast<const ELLIPSOID*>(o)->center.y; },
        [](META_DATA* o, float v) { static_cast<ELLIPSOID*>(o)->center.y = v; }, PropertyFieldKind::Float32, 1, false },
    { UITextID::PropCenterZ, [](const META_DATA* o) { return static_cast<const ELLIPSOID*>(o)->center.z; },
        [](META_DATA* o, float v) { static_cast<ELLIPSOID*>(o)->center.z = v; }, PropertyFieldKind::Float32, 2, false },
    { UITextID::PropRadiusX, [](const META_DATA* o) { return static_cast<const ELLIPSOID*>(o)->radiusX; },
        [](META_DATA* o, float v) { static_cast<ELLIPSOID*>(o)->radiusX = v; }, PropertyFieldKind::Float32, 3, true },
    { UITextID::PropRadiusY, [](const META_DATA* o) { return static_cast<const ELLIPSOID*>(o)->radiusY; },
        [](META_DATA* o, float v) { static_cast<ELLIPSOID*>(o)->radiusY = v; }, PropertyFieldKind::Float32, 4, true },
    { UITextID::PropRadiusZ, [](const META_DATA* o) { return static_cast<const ELLIPSOID*>(o)->radiusZ; },
        [](META_DATA* o, float v) { static_cast<ELLIPSOID*>(o)->radiusZ = v; }, PropertyFieldKind::Float32, 5, true },
};

// PIPE: Center1 X/Y/Z, Center2 X/Y/Z, Outside Diameter, Inside Diameter.
const PropertyFieldDescriptor kPipeFields[] = {
    { UITextID::PropPoint1X, [](const META_DATA* o) { return static_cast<const PIPE*>(o)->center1.x; },
        [](META_DATA* o, float v) { static_cast<PIPE*>(o)->center1.x = v; }, PropertyFieldKind::Float32, 0, false },
    { UITextID::PropPoint1Y, [](const META_DATA* o) { return static_cast<const PIPE*>(o)->center1.y; },
        [](META_DATA* o, float v) { static_cast<PIPE*>(o)->center1.y = v; }, PropertyFieldKind::Float32, 1, false },
    { UITextID::PropPoint1Z, [](const META_DATA* o) { return static_cast<const PIPE*>(o)->center1.z; },
        [](META_DATA* o, float v) { static_cast<PIPE*>(o)->center1.z = v; }, PropertyFieldKind::Float32, 2, false },
    { UITextID::PropPoint2X, [](const META_DATA* o) { return static_cast<const PIPE*>(o)->center2.x; },
        [](META_DATA* o, float v) { static_cast<PIPE*>(o)->center2.x = v; }, PropertyFieldKind::Float32, 3, false },
    { UITextID::PropPoint2Y, [](const META_DATA* o) { return static_cast<const PIPE*>(o)->center2.y; },
        [](META_DATA* o, float v) { static_cast<PIPE*>(o)->center2.y = v; }, PropertyFieldKind::Float32, 4, false },
    { UITextID::PropPoint2Z, [](const META_DATA* o) { return static_cast<const PIPE*>(o)->center2.z; },
        [](META_DATA* o, float v) { static_cast<PIPE*>(o)->center2.z = v; }, PropertyFieldKind::Float32, 5, false },
    { UITextID::PropOutsideDiameter, [](const META_DATA* o) { return static_cast<const PIPE*>(o)->outsideDiameter; },
        [](META_DATA* o, float v) { static_cast<PIPE*>(o)->outsideDiameter = v; }, PropertyFieldKind::Float32, 6, true },
    { UITextID::PropInsideDiameter, [](const META_DATA* o) { return static_cast<const PIPE*>(o)->insideDiameter; },
        [](META_DATA* o, float v) { static_cast<PIPE*>(o)->insideDiameter = v; }, PropertyFieldKind::Float32, 7, true },
};

// FRUSTUM_OF_CONE: Bottom Center X/Y/Z, Top Center X/Y/Z, Bottom Radius, Top Radius.
const PropertyFieldDescriptor kFrustumOfConeFields[] = {
    { UITextID::PropBottomCenterX, [](const META_DATA* o) { return static_cast<const FRUSTUM_OF_CONE*>(o)->bottomCenter.x; },
        [](META_DATA* o, float v) { static_cast<FRUSTUM_OF_CONE*>(o)->bottomCenter.x = v; }, PropertyFieldKind::Float32, 0, false },
    { UITextID::PropBottomCenterY, [](const META_DATA* o) { return static_cast<const FRUSTUM_OF_CONE*>(o)->bottomCenter.y; },
        [](META_DATA* o, float v) { static_cast<FRUSTUM_OF_CONE*>(o)->bottomCenter.y = v; }, PropertyFieldKind::Float32, 1, false },
    { UITextID::PropBottomCenterZ, [](const META_DATA* o) { return static_cast<const FRUSTUM_OF_CONE*>(o)->bottomCenter.z; },
        [](META_DATA* o, float v) { static_cast<FRUSTUM_OF_CONE*>(o)->bottomCenter.z = v; }, PropertyFieldKind::Float32, 2, false },
    { UITextID::PropTopCenterX, [](const META_DATA* o) { return static_cast<const FRUSTUM_OF_CONE*>(o)->topCenter.x; },
        [](META_DATA* o, float v) { static_cast<FRUSTUM_OF_CONE*>(o)->topCenter.x = v; }, PropertyFieldKind::Float32, 3, false },
    { UITextID::PropTopCenterY, [](const META_DATA* o) { return static_cast<const FRUSTUM_OF_CONE*>(o)->topCenter.y; },
        [](META_DATA* o, float v) { static_cast<FRUSTUM_OF_CONE*>(o)->topCenter.y = v; }, PropertyFieldKind::Float32, 4, false },
    { UITextID::PropTopCenterZ, [](const META_DATA* o) { return static_cast<const FRUSTUM_OF_CONE*>(o)->topCenter.z; },
        [](META_DATA* o, float v) { static_cast<FRUSTUM_OF_CONE*>(o)->topCenter.z = v; }, PropertyFieldKind::Float32, 5, false },
    { UITextID::PropBottomRadius, [](const META_DATA* o) { return static_cast<const FRUSTUM_OF_CONE*>(o)->bottomRadius; },
        [](META_DATA* o, float v) { static_cast<FRUSTUM_OF_CONE*>(o)->bottomRadius = v; }, PropertyFieldKind::Float32, 6, true },
    { UITextID::PropTopRadius, [](const META_DATA* o) { return static_cast<const FRUSTUM_OF_CONE*>(o)->topRadius; },
        [](META_DATA* o, float v) { static_cast<FRUSTUM_OF_CONE*>(o)->topRadius = v; }, PropertyFieldKind::Float32, 7, true },
};

// Copies the pre-edit field values and applies the edited value at editIndex, so cross-field rules
// evaluate the hypothetical post-edit state.
void CopyWithEdit(float* dst, const float* src, uint8_t count, uint8_t editIndex, float newValue) {
    for (uint8_t i = 0; i < count; ++i) dst[i] = src[i];
    if (editIndex < count) dst[editIndex] = newValue;
}

bool PointsDistinct(const float* v, int a, int b) {
    return !(v[a] == v[b] && v[a + 1] == v[b + 1] && v[a + 2] == v[b + 2]);
}

// CYLINDER / FRUSTUM_OF_CONE: the two axis end points must not coincide after the edit.
bool CrossTwoPoints(const float* values, uint8_t count, uint8_t editIndex, float newValue) {
    float f[8]; CopyWithEdit(f, values, count, editIndex, newValue);
    return PointsDistinct(f, 0, 3);
}

// TORUS: minor radius (field 4) must stay below the major radius (field 3).
bool CrossTorus(const float* values, uint8_t count, uint8_t editIndex, float newValue) {
    float f[8]; CopyWithEdit(f, values, count, editIndex, newValue);
    return f[4] < f[3];
}

// PIPE: inside diameter (field 7) < outside diameter (field 6) AND the two centers must not coincide.
bool CrossPipe(const float* values, uint8_t count, uint8_t editIndex, float newValue) {
    float f[8]; CopyWithEdit(f, values, count, editIndex, newValue);
    if (!(f[7] < f[6])) return false;
    return PointsDistinct(f, 0, 3);
}

} // namespace

const PropertyTypeDescriptor kPropertyTables[] = {
    { ObjectType::Sphere, kSphereFields, static_cast<uint8_t>(std::size(kSphereFields)), nullptr },
    { ObjectType::Cylinder, kCylinderFields, static_cast<uint8_t>(std::size(kCylinderFields)), CrossTwoPoints },
    { ObjectType::Cone, kConeFields, static_cast<uint8_t>(std::size(kConeFields)), nullptr },
    { ObjectType::Torus, kTorusFields, static_cast<uint8_t>(std::size(kTorusFields)), CrossTorus },
    { ObjectType::Ellipsoid, kEllipsoidFields, static_cast<uint8_t>(std::size(kEllipsoidFields)), nullptr },
    { ObjectType::Pipe, kPipeFields, static_cast<uint8_t>(std::size(kPipeFields)), CrossPipe },
    { ObjectType::FrustumOfCone, kFrustumOfConeFields, static_cast<uint8_t>(std::size(kFrustumOfConeFields)), CrossTwoPoints },
    // PYRAMID, CUBOID, PARALLELEPIPED, FRUSTUM_OF_PYRAMID are vertex-list types: no table in the
    // MVP, so FindPropertyTable() returns nullptr and the pane shows Type + ID only.
};

const size_t kPropertyTableCount = std::size(kPropertyTables);

const PropertyTypeDescriptor* FindPropertyTable(ObjectType objectType) {
    for (size_t i = 0; i < kPropertyTableCount; ++i) {
        if (kPropertyTables[i].objectType == objectType) return &kPropertyTables[i];
    }
    return nullptr;
}

bool ValidatePropertyEdit(const PropertyTypeDescriptor& table, const float* values, uint8_t count,
    uint8_t editIndex, float newValue) {
    if (editIndex >= table.fieldCount) return false;
    if (!std::isfinite(newValue)) return false;
    if (table.fields[editIndex].mustBePositive && newValue <= 0.0f) return false;
    if (table.validateCrossField && !table.validateCrossField(values, count, editIndex, newValue)) {
        return false;
    }
    return true;
}
