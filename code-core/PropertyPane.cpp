// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

// Property descriptor tables for the right-side properties pane.
// Design document: website/content/software/propertiesPane.md

#include "PropertyPane.h"

// डेटा-सामान्य-3D.h defines _USE_MATH_DEFINES then declares its own `constexpr float M_PI`, which only
// compiles when <math.h> was already pulled in (via DirectXMath in डेटा.h) *before* that define.
// Include डेटा.h first so this TU matches the working order in DataStorage.cpp / विश्वकर्मा.cpp.
#include "डेटा.h"
#include "डेटा-सामान्य-3D.h" // SPHERE, CYLINDER, ... and META_DATA
#include "डेटा-संरचना.h"     // LINE_MEMBER

#include <algorithm> // std::clamp, used by the CUBOID Euler extraction
#include <cmath>     // std::isfinite, std::atan2, std::asin

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

/* CUBOID's authored orientation is stored as a unit QUATERNION, which is not something a user types
into a properties field, so the pane shows XYZ Euler angles in DEGREES and solves back on write.
That is the same shape as the world-coordinate handling for point fields above - compose on read,
solve on write - rather than a new mechanism.

Writing one angle rebuilds the whole quaternion from all three, exactly as editing one world
component of a point rewrites all three authored components: the three angles are not independent
storage, they are a chart on one rotation. Reading is not a round-trip identity - a quaternion has
many Euler representations and this returns one of them - but it is a stable one, so a value the
user did not touch reads back as they left it.

Extraction is the standard XYZ (roll about X, then pitch about Y, then yaw about Z) decomposition of
the row-vector rotation matrix, with the gimbal-lock case (|m02| ~ 1, pitch at +/-90 degrees) folded
onto roll, because at that pole roll and yaw are the same rotation and only their sum is defined. */
void CuboidEulerDegrees(const CUBOID* box, float* out) {
    DirectX::XMFLOAT4X4 m;
    DirectX::XMStoreFloat4x4(&m,
        DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&box->orientation)));
    const float kToDegrees = 180.0f / M_PI;
    const float m02 = std::clamp(m._13, -1.0f, 1.0f);
    if (std::abs(m02) > 0.9999f) {
        out[0] = std::atan2(-m._32, m._22) * kToDegrees;
        out[1] = std::asin(m02) * kToDegrees;
        out[2] = 0.0f;
        return;
    }
    out[0] = std::atan2(-m._23, m._33) * kToDegrees;
    out[1] = std::asin(m02) * kToDegrees;
    out[2] = std::atan2(-m._12, m._11) * kToDegrees;
}

float CuboidEulerComponent(const META_DATA* o, int axis) {
    float euler[3];
    CuboidEulerDegrees(static_cast<const CUBOID*>(o), euler);
    return euler[axis];
}

void SetCuboidEulerComponent(META_DATA* o, int axis, float degrees) {
    CUBOID* box = static_cast<CUBOID*>(o);
    float euler[3];
    CuboidEulerDegrees(box, euler);
    euler[axis] = degrees;
    const float kToRadians = M_PI / 180.0f;
    DirectX::XMStoreFloat4(&box->orientation, DirectX::XMQuaternionRotationRollPitchYaw(
        euler[0] * kToRadians, euler[1] * kToRadians, euler[2] * kToRadians));
}

// CUBOID: Center X/Y/Z, Size X/Y/Z (full edge lengths), Rotation X/Y/Z (degrees).
const PropertyFieldDescriptor kCuboidFields[] = {
    { UITextID::PropCenterX, [](const META_DATA* o) { return static_cast<const CUBOID*>(o)->center.x; },
        [](META_DATA* o, float v) { static_cast<CUBOID*>(o)->center.x = v; }, PropertyFieldKind::Float32, 0, false },
    { UITextID::PropCenterY, [](const META_DATA* o) { return static_cast<const CUBOID*>(o)->center.y; },
        [](META_DATA* o, float v) { static_cast<CUBOID*>(o)->center.y = v; }, PropertyFieldKind::Float32, 1, false },
    { UITextID::PropCenterZ, [](const META_DATA* o) { return static_cast<const CUBOID*>(o)->center.z; },
        [](META_DATA* o, float v) { static_cast<CUBOID*>(o)->center.z = v; }, PropertyFieldKind::Float32, 2, false },
    { UITextID::PropSizeX, [](const META_DATA* o) { return static_cast<const CUBOID*>(o)->size.x; },
        [](META_DATA* o, float v) { static_cast<CUBOID*>(o)->size.x = v; }, PropertyFieldKind::Float32, 3, true },
    { UITextID::PropSizeY, [](const META_DATA* o) { return static_cast<const CUBOID*>(o)->size.y; },
        [](META_DATA* o, float v) { static_cast<CUBOID*>(o)->size.y = v; }, PropertyFieldKind::Float32, 4, true },
    { UITextID::PropSizeZ, [](const META_DATA* o) { return static_cast<const CUBOID*>(o)->size.z; },
        [](META_DATA* o, float v) { static_cast<CUBOID*>(o)->size.z = v; }, PropertyFieldKind::Float32, 5, true },
    { UITextID::PropRotationX, [](const META_DATA* o) { return CuboidEulerComponent(o, 0); },
        [](META_DATA* o, float v) { SetCuboidEulerComponent(o, 0, v); }, PropertyFieldKind::Float32, 6, false },
    { UITextID::PropRotationY, [](const META_DATA* o) { return CuboidEulerComponent(o, 1); },
        [](META_DATA* o, float v) { SetCuboidEulerComponent(o, 1, v); }, PropertyFieldKind::Float32, 7, false },
    { UITextID::PropRotationZ, [](const META_DATA* o) { return CuboidEulerComponent(o, 2); },
        [](META_DATA* o, float v) { SetCuboidEulerComponent(o, 2, v); }, PropertyFieldKind::Float32, 8, false },
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

// LINE_MEMBER: P1 X/Y/Z, P2 X/Y/Z (meters), Parameter 1/2 (parametric section dims,
// millimeters; 0 = the catalog row's defaults, so zero stays editable-legal).
const PropertyFieldDescriptor kLineMemberFields[] = {
    { UITextID::PropPoint1X, [](const META_DATA* o) { return static_cast<const LINE_MEMBER*>(o)->point1.x; },
        [](META_DATA* o, float v) { static_cast<LINE_MEMBER*>(o)->point1.x = v; }, PropertyFieldKind::Float32, 0, false },
    { UITextID::PropPoint1Y, [](const META_DATA* o) { return static_cast<const LINE_MEMBER*>(o)->point1.y; },
        [](META_DATA* o, float v) { static_cast<LINE_MEMBER*>(o)->point1.y = v; }, PropertyFieldKind::Float32, 1, false },
    { UITextID::PropPoint1Z, [](const META_DATA* o) { return static_cast<const LINE_MEMBER*>(o)->point1.z; },
        [](META_DATA* o, float v) { static_cast<LINE_MEMBER*>(o)->point1.z = v; }, PropertyFieldKind::Float32, 2, false },
    { UITextID::PropPoint2X, [](const META_DATA* o) { return static_cast<const LINE_MEMBER*>(o)->point2.x; },
        [](META_DATA* o, float v) { static_cast<LINE_MEMBER*>(o)->point2.x = v; }, PropertyFieldKind::Float32, 3, false },
    { UITextID::PropPoint2Y, [](const META_DATA* o) { return static_cast<const LINE_MEMBER*>(o)->point2.y; },
        [](META_DATA* o, float v) { static_cast<LINE_MEMBER*>(o)->point2.y = v; }, PropertyFieldKind::Float32, 4, false },
    { UITextID::PropPoint2Z, [](const META_DATA* o) { return static_cast<const LINE_MEMBER*>(o)->point2.z; },
        [](META_DATA* o, float v) { static_cast<LINE_MEMBER*>(o)->point2.z = v; }, PropertyFieldKind::Float32, 5, false },
    { UITextID::PropParameter1, [](const META_DATA* o) { return static_cast<const LINE_MEMBER*>(o)->userParameter1; },
        [](META_DATA* o, float v) { static_cast<LINE_MEMBER*>(o)->userParameter1 = v; }, PropertyFieldKind::Float32, 6, false },
    { UITextID::PropParameter2, [](const META_DATA* o) { return static_cast<const LINE_MEMBER*>(o)->userParameter2; },
        [](META_DATA* o, float v) { static_cast<LINE_MEMBER*>(o)->userParameter2 = v; }, PropertyFieldKind::Float32, 7, false },
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

// LINE_MEMBER: the axis end points must not coincide; parameters (fields 6, 7) must not go
// negative (0 is legal — it means "use the catalog row's default dimensions").
bool CrossLineMember(const float* values, uint8_t count, uint8_t editIndex, float newValue) {
    float f[8]; CopyWithEdit(f, values, count, editIndex, newValue);
    if (f[6] < 0.0f || f[7] < 0.0f) return false;
    return PointsDistinct(f, 0, 3);
}

} // namespace

// The trailing { first fields } / count declare which field triples are POINTS and therefore get
// carried to and from world space by the placement (see PropertyTypeDescriptor). Single-point types
// list one triple; axis types list both ends. Everything after the points is a scalar.
const PropertyTypeDescriptor kPropertyTables[] = {
    { ObjectType::Sphere, kSphereFields, static_cast<uint8_t>(std::size(kSphereFields)), nullptr, { 0, 0 }, 1 },
    { ObjectType::Cylinder, kCylinderFields, static_cast<uint8_t>(std::size(kCylinderFields)), CrossTwoPoints, { 0, 3 }, 2 },
    { ObjectType::Cone, kConeFields, static_cast<uint8_t>(std::size(kConeFields)), nullptr, { 0, 3 }, 2 },
    { ObjectType::Torus, kTorusFields, static_cast<uint8_t>(std::size(kTorusFields)), CrossTorus, { 0, 0 }, 1 },
    { ObjectType::Ellipsoid, kEllipsoidFields, static_cast<uint8_t>(std::size(kEllipsoidFields)), nullptr, { 0, 0 }, 1 },
    { ObjectType::Pipe, kPipeFields, static_cast<uint8_t>(std::size(kPipeFields)), CrossPipe, { 0, 3 }, 2 },
    { ObjectType::FrustumOfCone, kFrustumOfConeFields, static_cast<uint8_t>(std::size(kFrustumOfConeFields)), CrossTwoPoints, { 0, 3 }, 2 },
    { ObjectType::LineMember, kLineMemberFields, static_cast<uint8_t>(std::size(kLineMemberFields)), CrossLineMember, { 0, 3 }, 2 },
    /* the type stores its own orientation is to keep "drawn at 30 degrees" distinct from
    "rotated by 30 degrees since". */
    { ObjectType::Cuboid, kCuboidFields, static_cast<uint8_t>(std::size(kCuboidFields)), nullptr, { 0, 0 }, 1 },
    // PYRAMID, PARALLELEPIPED, FRUSTUM_OF_PYRAMID are vertex-list types: no table in the MVP, so
    // FindPropertyTable() returns nullptr and the pane shows Type + ID only.
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

namespace {

// Which point triple a field belongs to, or kNoPointGroup when it is a scalar.
uint8_t PointGroupOfField(const PropertyTypeDescriptor& table, uint8_t fieldIndex) {
    for (uint8_t g = 0; g < table.pointGroupCount && g < kMaxPointGroups; ++g) {
        const uint8_t first = table.pointGroupFirstField[g];
        if (fieldIndex >= first && fieldIndex < static_cast<uint8_t>(first + 3)) return g;
    }
    return kNoPointGroup;
}

} // namespace

void ReadPropertyValuesForDisplay(const PropertyTypeDescriptor& table, const META_DATA* object,
    float* out) {
    if (!object || !out) return;
    for (uint8_t i = 0; i < table.fieldCount; ++i) out[i] = table.fields[i].get(object);

    // const_cast: PlacementForObject is the one switch over the 15 types and hands back a mutable
    // pointer because the move producer writes through it; this path only reads.
    const Placement3D* placement =
        PlacementForObject(table.objectType, const_cast<META_DATA*>(object));
    if (!placement || placement->IsIdentity()) return; // Authored == world; nothing to convert.

    for (uint8_t g = 0; g < table.pointGroupCount && g < kMaxPointGroups; ++g) {
        const uint8_t b = table.pointGroupFirstField[g];
        if (static_cast<uint8_t>(b + 3) > table.fieldCount) continue;
        DirectX::XMFLOAT3 world;
        DirectX::XMStoreFloat3(&world, placement->TransformPoint(
            DirectX::XMVectorSet(out[b], out[b + 1], out[b + 2], 1.0f)));
        out[b] = world.x; out[b + 1] = world.y; out[b + 2] = world.z;
    }
}

void ApplyPropertyValueFromDisplay(const PropertyTypeDescriptor& table, META_DATA* object,
    uint8_t fieldIndex, float newValue) {
    if (!object || fieldIndex >= table.fieldCount) return;

    Placement3D* placement = PlacementForObject(table.objectType, object);
    const uint8_t group = PointGroupOfField(table, fieldIndex);
    // A scalar, or an unplaced object: the displayed value IS the stored value.
    if (group == kNoPointGroup || !placement || placement->IsIdentity()) {
        table.fields[fieldIndex].set(object, newValue);
        return;
    }

    const uint8_t b = table.pointGroupFirstField[group];
    // Take the point to world space, replace the one component the user edited, come back.
    DirectX::XMFLOAT3 world;
    DirectX::XMStoreFloat3(&world, placement->TransformPoint(DirectX::XMVectorSet(
        table.fields[b].get(object), table.fields[b + 1].get(object),
        table.fields[b + 2].get(object), 1.0f)));
    float component[3] = { world.x, world.y, world.z };
    component[fieldIndex - b] = newValue;

    DirectX::XMFLOAT3 authored;
    DirectX::XMStoreFloat3(&authored, placement->InverseTransformPoint(
        DirectX::XMVectorSet(component[0], component[1], component[2], 1.0f)));
    // All three, not just the edited one: under a rotation each authored component depends on all
    // three world components, so writing one axis alone would skew the object.
    table.fields[b].set(object, authored.x);
    table.fields[b + 1].set(object, authored.y);
    table.fields[b + 2].set(object, authored.z);
}
