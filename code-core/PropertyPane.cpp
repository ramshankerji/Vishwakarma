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
#include "RenderPage2D.h"    // Cad2DLineRecordCPU, Cad2DCircleRecordCPU, ... the 2D records

#include <algorithm> // std::clamp, used by the CUBOID Euler extraction
#include <cmath>     // std::isfinite, std::atan2, std::asin

using VishwakarmaStorage::ObjectType;

namespace {

// Each accessor is a captureless lambda that decays to a function pointer: portable, no casts at
// the call site, and type-checked at compile time.

// SPHERE: Center X/Y/Z, Radius.
const PropertyFieldDescriptor kSphereFields[] = {
    { UITextID::PropCenterX, [](const void* o) -> double { return static_cast<const SPHERE*>(o)->center.x; },
        [](void* o, double v) { static_cast<SPHERE*>(o)->center.x = static_cast<float>(v); }, PropertyFieldKind::Real, 0, false },
    { UITextID::PropCenterY, [](const void* o) -> double { return static_cast<const SPHERE*>(o)->center.y; },
        [](void* o, double v) { static_cast<SPHERE*>(o)->center.y = static_cast<float>(v); }, PropertyFieldKind::Real, 1, false },
    { UITextID::PropCenterZ, [](const void* o) -> double { return static_cast<const SPHERE*>(o)->center.z; },
        [](void* o, double v) { static_cast<SPHERE*>(o)->center.z = static_cast<float>(v); }, PropertyFieldKind::Real, 2, false },
    { UITextID::PropRadius, [](const void* o) -> double { return static_cast<const SPHERE*>(o)->radius; },
        [](void* o, double v) { static_cast<SPHERE*>(o)->radius = static_cast<float>(v); }, PropertyFieldKind::Real, 3, true },
};

// CYLINDER: P1 X/Y/Z, P2 X/Y/Z, Radius.
const PropertyFieldDescriptor kCylinderFields[] = {
    { UITextID::PropPoint1X, [](const void* o) -> double { return static_cast<const CYLINDER*>(o)->p1.x; },
        [](void* o, double v) { static_cast<CYLINDER*>(o)->p1.x = static_cast<float>(v); }, PropertyFieldKind::Real, 0, false },
    { UITextID::PropPoint1Y, [](const void* o) -> double { return static_cast<const CYLINDER*>(o)->p1.y; },
        [](void* o, double v) { static_cast<CYLINDER*>(o)->p1.y = static_cast<float>(v); }, PropertyFieldKind::Real, 1, false },
    { UITextID::PropPoint1Z, [](const void* o) -> double { return static_cast<const CYLINDER*>(o)->p1.z; },
        [](void* o, double v) { static_cast<CYLINDER*>(o)->p1.z = static_cast<float>(v); }, PropertyFieldKind::Real, 2, false },
    { UITextID::PropPoint2X, [](const void* o) -> double { return static_cast<const CYLINDER*>(o)->p2.x; },
        [](void* o, double v) { static_cast<CYLINDER*>(o)->p2.x = static_cast<float>(v); }, PropertyFieldKind::Real, 3, false },
    { UITextID::PropPoint2Y, [](const void* o) -> double { return static_cast<const CYLINDER*>(o)->p2.y; },
        [](void* o, double v) { static_cast<CYLINDER*>(o)->p2.y = static_cast<float>(v); }, PropertyFieldKind::Real, 4, false },
    { UITextID::PropPoint2Z, [](const void* o) -> double { return static_cast<const CYLINDER*>(o)->p2.z; },
        [](void* o, double v) { static_cast<CYLINDER*>(o)->p2.z = static_cast<float>(v); }, PropertyFieldKind::Real, 5, false },
    { UITextID::PropRadius, [](const void* o) -> double { return static_cast<const CYLINDER*>(o)->radius; },
        [](void* o, double v) { static_cast<CYLINDER*>(o)->radius = static_cast<float>(v); }, PropertyFieldKind::Real, 6, true },
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

float CuboidEulerComponent(const void* o, int axis) {
    float euler[3];
    CuboidEulerDegrees(static_cast<const CUBOID*>(o), euler);
    return euler[axis];
}

void SetCuboidEulerComponent(void* o, int axis, float degrees) {
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
    { UITextID::PropCenterX, [](const void* o) -> double { return static_cast<const CUBOID*>(o)->center.x; },
        [](void* o, double v) { static_cast<CUBOID*>(o)->center.x = static_cast<float>(v); }, PropertyFieldKind::Real, 0, false },
    { UITextID::PropCenterY, [](const void* o) -> double { return static_cast<const CUBOID*>(o)->center.y; },
        [](void* o, double v) { static_cast<CUBOID*>(o)->center.y = static_cast<float>(v); }, PropertyFieldKind::Real, 1, false },
    { UITextID::PropCenterZ, [](const void* o) -> double { return static_cast<const CUBOID*>(o)->center.z; },
        [](void* o, double v) { static_cast<CUBOID*>(o)->center.z = static_cast<float>(v); }, PropertyFieldKind::Real, 2, false },
    { UITextID::PropSizeX, [](const void* o) -> double { return static_cast<const CUBOID*>(o)->size.x; },
        [](void* o, double v) { static_cast<CUBOID*>(o)->size.x = static_cast<float>(v); }, PropertyFieldKind::Real, 3, true },
    { UITextID::PropSizeY, [](const void* o) -> double { return static_cast<const CUBOID*>(o)->size.y; },
        [](void* o, double v) { static_cast<CUBOID*>(o)->size.y = static_cast<float>(v); }, PropertyFieldKind::Real, 4, true },
    { UITextID::PropSizeZ, [](const void* o) -> double { return static_cast<const CUBOID*>(o)->size.z; },
        [](void* o, double v) { static_cast<CUBOID*>(o)->size.z = static_cast<float>(v); }, PropertyFieldKind::Real, 5, true },
    { UITextID::PropRotationX, [](const void* o) -> double { return CuboidEulerComponent(o, 0); },
        [](void* o, double v) { SetCuboidEulerComponent(o, 0, static_cast<float>(v)); }, PropertyFieldKind::Real, 6, false },
    { UITextID::PropRotationY, [](const void* o) -> double { return CuboidEulerComponent(o, 1); },
        [](void* o, double v) { SetCuboidEulerComponent(o, 1, static_cast<float>(v)); }, PropertyFieldKind::Real, 7, false },
    { UITextID::PropRotationZ, [](const void* o) -> double { return CuboidEulerComponent(o, 2); },
        [](void* o, double v) { SetCuboidEulerComponent(o, 2, static_cast<float>(v)); }, PropertyFieldKind::Real, 8, false },
};

// CONE: Apex X/Y/Z, Base Center X/Y/Z, Radius.
const PropertyFieldDescriptor kConeFields[] = {
    { UITextID::PropApexX, [](const void* o) -> double { return static_cast<const CONE*>(o)->apex.x; },
        [](void* o, double v) { static_cast<CONE*>(o)->apex.x = static_cast<float>(v); }, PropertyFieldKind::Real, 0, false },
    { UITextID::PropApexY, [](const void* o) -> double { return static_cast<const CONE*>(o)->apex.y; },
        [](void* o, double v) { static_cast<CONE*>(o)->apex.y = static_cast<float>(v); }, PropertyFieldKind::Real, 1, false },
    { UITextID::PropApexZ, [](const void* o) -> double { return static_cast<const CONE*>(o)->apex.z; },
        [](void* o, double v) { static_cast<CONE*>(o)->apex.z = static_cast<float>(v); }, PropertyFieldKind::Real, 2, false },
    { UITextID::PropBaseCenterX, [](const void* o) -> double { return static_cast<const CONE*>(o)->baseCenter.x; },
        [](void* o, double v) { static_cast<CONE*>(o)->baseCenter.x = static_cast<float>(v); }, PropertyFieldKind::Real, 3, false },
    { UITextID::PropBaseCenterY, [](const void* o) -> double { return static_cast<const CONE*>(o)->baseCenter.y; },
        [](void* o, double v) { static_cast<CONE*>(o)->baseCenter.y = static_cast<float>(v); }, PropertyFieldKind::Real, 4, false },
    { UITextID::PropBaseCenterZ, [](const void* o) -> double { return static_cast<const CONE*>(o)->baseCenter.z; },
        [](void* o, double v) { static_cast<CONE*>(o)->baseCenter.z = static_cast<float>(v); }, PropertyFieldKind::Real, 5, false },
    { UITextID::PropRadius, [](const void* o) -> double { return static_cast<const CONE*>(o)->radius; },
        [](void* o, double v) { static_cast<CONE*>(o)->radius = static_cast<float>(v); }, PropertyFieldKind::Real, 6, true },
};

// TORUS: Center X/Y/Z, Major Radius, Minor Radius.
const PropertyFieldDescriptor kTorusFields[] = {
    { UITextID::PropCenterX, [](const void* o) -> double { return static_cast<const TORUS*>(o)->center.x; },
        [](void* o, double v) { static_cast<TORUS*>(o)->center.x = static_cast<float>(v); }, PropertyFieldKind::Real, 0, false },
    { UITextID::PropCenterY, [](const void* o) -> double { return static_cast<const TORUS*>(o)->center.y; },
        [](void* o, double v) { static_cast<TORUS*>(o)->center.y = static_cast<float>(v); }, PropertyFieldKind::Real, 1, false },
    { UITextID::PropCenterZ, [](const void* o) -> double { return static_cast<const TORUS*>(o)->center.z; },
        [](void* o, double v) { static_cast<TORUS*>(o)->center.z = static_cast<float>(v); }, PropertyFieldKind::Real, 2, false },
    { UITextID::PropMajorRadius, [](const void* o) -> double { return static_cast<const TORUS*>(o)->majorRadius; },
        [](void* o, double v) { static_cast<TORUS*>(o)->majorRadius = static_cast<float>(v); }, PropertyFieldKind::Real, 3, true },
    { UITextID::PropMinorRadius, [](const void* o) -> double { return static_cast<const TORUS*>(o)->minorRadius; },
        [](void* o, double v) { static_cast<TORUS*>(o)->minorRadius = static_cast<float>(v); }, PropertyFieldKind::Real, 4, true },
};

// ELLIPSOID: Center X/Y/Z, Radius X/Y/Z.
const PropertyFieldDescriptor kEllipsoidFields[] = {
    { UITextID::PropCenterX, [](const void* o) -> double { return static_cast<const ELLIPSOID*>(o)->center.x; },
        [](void* o, double v) { static_cast<ELLIPSOID*>(o)->center.x = static_cast<float>(v); }, PropertyFieldKind::Real, 0, false },
    { UITextID::PropCenterY, [](const void* o) -> double { return static_cast<const ELLIPSOID*>(o)->center.y; },
        [](void* o, double v) { static_cast<ELLIPSOID*>(o)->center.y = static_cast<float>(v); }, PropertyFieldKind::Real, 1, false },
    { UITextID::PropCenterZ, [](const void* o) -> double { return static_cast<const ELLIPSOID*>(o)->center.z; },
        [](void* o, double v) { static_cast<ELLIPSOID*>(o)->center.z = static_cast<float>(v); }, PropertyFieldKind::Real, 2, false },
    { UITextID::PropRadiusX, [](const void* o) -> double { return static_cast<const ELLIPSOID*>(o)->radiusX; },
        [](void* o, double v) { static_cast<ELLIPSOID*>(o)->radiusX = static_cast<float>(v); }, PropertyFieldKind::Real, 3, true },
    { UITextID::PropRadiusY, [](const void* o) -> double { return static_cast<const ELLIPSOID*>(o)->radiusY; },
        [](void* o, double v) { static_cast<ELLIPSOID*>(o)->radiusY = static_cast<float>(v); }, PropertyFieldKind::Real, 4, true },
    { UITextID::PropRadiusZ, [](const void* o) -> double { return static_cast<const ELLIPSOID*>(o)->radiusZ; },
        [](void* o, double v) { static_cast<ELLIPSOID*>(o)->radiusZ = static_cast<float>(v); }, PropertyFieldKind::Real, 5, true },
};

// PIPE: Center1 X/Y/Z, Center2 X/Y/Z, Outside Diameter, Inside Diameter.
const PropertyFieldDescriptor kPipeFields[] = {
    { UITextID::PropPoint1X, [](const void* o) -> double { return static_cast<const PIPE*>(o)->center1.x; },
        [](void* o, double v) { static_cast<PIPE*>(o)->center1.x = static_cast<float>(v); }, PropertyFieldKind::Real, 0, false },
    { UITextID::PropPoint1Y, [](const void* o) -> double { return static_cast<const PIPE*>(o)->center1.y; },
        [](void* o, double v) { static_cast<PIPE*>(o)->center1.y = static_cast<float>(v); }, PropertyFieldKind::Real, 1, false },
    { UITextID::PropPoint1Z, [](const void* o) -> double { return static_cast<const PIPE*>(o)->center1.z; },
        [](void* o, double v) { static_cast<PIPE*>(o)->center1.z = static_cast<float>(v); }, PropertyFieldKind::Real, 2, false },
    { UITextID::PropPoint2X, [](const void* o) -> double { return static_cast<const PIPE*>(o)->center2.x; },
        [](void* o, double v) { static_cast<PIPE*>(o)->center2.x = static_cast<float>(v); }, PropertyFieldKind::Real, 3, false },
    { UITextID::PropPoint2Y, [](const void* o) -> double { return static_cast<const PIPE*>(o)->center2.y; },
        [](void* o, double v) { static_cast<PIPE*>(o)->center2.y = static_cast<float>(v); }, PropertyFieldKind::Real, 4, false },
    { UITextID::PropPoint2Z, [](const void* o) -> double { return static_cast<const PIPE*>(o)->center2.z; },
        [](void* o, double v) { static_cast<PIPE*>(o)->center2.z = static_cast<float>(v); }, PropertyFieldKind::Real, 5, false },
    { UITextID::PropOutsideDiameter, [](const void* o) -> double { return static_cast<const PIPE*>(o)->outsideDiameter; },
        [](void* o, double v) { static_cast<PIPE*>(o)->outsideDiameter = static_cast<float>(v); }, PropertyFieldKind::Real, 6, true },
    { UITextID::PropInsideDiameter, [](const void* o) -> double { return static_cast<const PIPE*>(o)->insideDiameter; },
        [](void* o, double v) { static_cast<PIPE*>(o)->insideDiameter = static_cast<float>(v); }, PropertyFieldKind::Real, 7, true },
};

// FRUSTUM_OF_CONE: Bottom Center X/Y/Z, Top Center X/Y/Z, Bottom Radius, Top Radius.
const PropertyFieldDescriptor kFrustumOfConeFields[] = {
    { UITextID::PropBottomCenterX, [](const void* o) -> double { return static_cast<const FRUSTUM_OF_CONE*>(o)->bottomCenter.x; },
        [](void* o, double v) { static_cast<FRUSTUM_OF_CONE*>(o)->bottomCenter.x = static_cast<float>(v); }, PropertyFieldKind::Real, 0, false },
    { UITextID::PropBottomCenterY, [](const void* o) -> double { return static_cast<const FRUSTUM_OF_CONE*>(o)->bottomCenter.y; },
        [](void* o, double v) { static_cast<FRUSTUM_OF_CONE*>(o)->bottomCenter.y = static_cast<float>(v); }, PropertyFieldKind::Real, 1, false },
    { UITextID::PropBottomCenterZ, [](const void* o) -> double { return static_cast<const FRUSTUM_OF_CONE*>(o)->bottomCenter.z; },
        [](void* o, double v) { static_cast<FRUSTUM_OF_CONE*>(o)->bottomCenter.z = static_cast<float>(v); }, PropertyFieldKind::Real, 2, false },
    { UITextID::PropTopCenterX, [](const void* o) -> double { return static_cast<const FRUSTUM_OF_CONE*>(o)->topCenter.x; },
        [](void* o, double v) { static_cast<FRUSTUM_OF_CONE*>(o)->topCenter.x = static_cast<float>(v); }, PropertyFieldKind::Real, 3, false },
    { UITextID::PropTopCenterY, [](const void* o) -> double { return static_cast<const FRUSTUM_OF_CONE*>(o)->topCenter.y; },
        [](void* o, double v) { static_cast<FRUSTUM_OF_CONE*>(o)->topCenter.y = static_cast<float>(v); }, PropertyFieldKind::Real, 4, false },
    { UITextID::PropTopCenterZ, [](const void* o) -> double { return static_cast<const FRUSTUM_OF_CONE*>(o)->topCenter.z; },
        [](void* o, double v) { static_cast<FRUSTUM_OF_CONE*>(o)->topCenter.z = static_cast<float>(v); }, PropertyFieldKind::Real, 5, false },
    { UITextID::PropBottomRadius, [](const void* o) -> double { return static_cast<const FRUSTUM_OF_CONE*>(o)->bottomRadius; },
        [](void* o, double v) { static_cast<FRUSTUM_OF_CONE*>(o)->bottomRadius = static_cast<float>(v); }, PropertyFieldKind::Real, 6, true },
    { UITextID::PropTopRadius, [](const void* o) -> double { return static_cast<const FRUSTUM_OF_CONE*>(o)->topRadius; },
        [](void* o, double v) { static_cast<FRUSTUM_OF_CONE*>(o)->topRadius = static_cast<float>(v); }, PropertyFieldKind::Real, 7, true },
};

// LINE_MEMBER: P1 X/Y/Z, P2 X/Y/Z (meters), Parameter 1/2 (parametric section dims,
// millimeters; 0 = the catalog row's defaults, so zero stays editable-legal).
const PropertyFieldDescriptor kLineMemberFields[] = {
    { UITextID::PropPoint1X, [](const void* o) -> double { return static_cast<const LINE_MEMBER*>(o)->point1.x; },
        [](void* o, double v) { static_cast<LINE_MEMBER*>(o)->point1.x = static_cast<float>(v); }, PropertyFieldKind::Real, 0, false },
    { UITextID::PropPoint1Y, [](const void* o) -> double { return static_cast<const LINE_MEMBER*>(o)->point1.y; },
        [](void* o, double v) { static_cast<LINE_MEMBER*>(o)->point1.y = static_cast<float>(v); }, PropertyFieldKind::Real, 1, false },
    { UITextID::PropPoint1Z, [](const void* o) -> double { return static_cast<const LINE_MEMBER*>(o)->point1.z; },
        [](void* o, double v) { static_cast<LINE_MEMBER*>(o)->point1.z = static_cast<float>(v); }, PropertyFieldKind::Real, 2, false },
    { UITextID::PropPoint2X, [](const void* o) -> double { return static_cast<const LINE_MEMBER*>(o)->point2.x; },
        [](void* o, double v) { static_cast<LINE_MEMBER*>(o)->point2.x = static_cast<float>(v); }, PropertyFieldKind::Real, 3, false },
    { UITextID::PropPoint2Y, [](const void* o) -> double { return static_cast<const LINE_MEMBER*>(o)->point2.y; },
        [](void* o, double v) { static_cast<LINE_MEMBER*>(o)->point2.y = static_cast<float>(v); }, PropertyFieldKind::Real, 4, false },
    { UITextID::PropPoint2Z, [](const void* o) -> double { return static_cast<const LINE_MEMBER*>(o)->point2.z; },
        [](void* o, double v) { static_cast<LINE_MEMBER*>(o)->point2.z = static_cast<float>(v); }, PropertyFieldKind::Real, 5, false },
    { UITextID::PropParameter1, [](const void* o) -> double { return static_cast<const LINE_MEMBER*>(o)->userParameter1; },
        [](void* o, double v) { static_cast<LINE_MEMBER*>(o)->userParameter1 = static_cast<float>(v); }, PropertyFieldKind::Real, 6, false },
    { UITextID::PropParameter2, [](const void* o) -> double { return static_cast<const LINE_MEMBER*>(o)->userParameter2; },
        [](void* o, double v) { static_cast<LINE_MEMBER*>(o)->userParameter2 = static_cast<float>(v); }, PropertyFieldKind::Real, 7, false },
};

// Copies the pre-edit field values and applies the edited value at editIndex, so cross-field rules
// evaluate the hypothetical post-edit state.
void CopyWithEdit(double* dst, const double* src, uint8_t count, uint8_t editIndex, double newValue) {
    for (uint8_t i = 0; i < count; ++i) dst[i] = src[i];
    if (editIndex < count) dst[editIndex] = newValue;
}

bool PointsDistinct(const double* v, int a, int b) {
    return !(v[a] == v[b] && v[a + 1] == v[b + 1] && v[a + 2] == v[b + 2]);
}

// CYLINDER / FRUSTUM_OF_CONE: the two axis end points must not coincide after the edit.
bool CrossTwoPoints(const double* values, uint8_t count, uint8_t editIndex, double newValue) {
    double f[8]; CopyWithEdit(f, values, count, editIndex, newValue);
    return PointsDistinct(f, 0, 3);
}

// TORUS: minor radius (field 4) must stay below the major radius (field 3).
bool CrossTorus(const double* values, uint8_t count, uint8_t editIndex, double newValue) {
    double f[8]; CopyWithEdit(f, values, count, editIndex, newValue);
    return f[4] < f[3];
}

// PIPE: inside diameter (field 7) < outside diameter (field 6) AND the two centers must not coincide.
bool CrossPipe(const double* values, uint8_t count, uint8_t editIndex, double newValue) {
    double f[8]; CopyWithEdit(f, values, count, editIndex, newValue);
    if (!(f[7] < f[6])) return false;
    return PointsDistinct(f, 0, 3);
}

// LINE_MEMBER: the axis end points must not coincide; parameters (fields 6, 7) must not go
// negative (0 is legal — it means "use the catalog row's default dimensions").
bool CrossLineMember(const double* values, uint8_t count, uint8_t editIndex, double newValue) {
    double f[8]; CopyWithEdit(f, values, count, editIndex, newValue);
    if (f[6] < 0.0 || f[7] < 0.0) return false;
    return PointsDistinct(f, 0, 3);
}

/* ---- Page2D records ------------------------------------------------------------------------
Read-only for now: every `set` is nullptr, so ApplyPropertyValueFromDisplay refuses the write and
the pane renders these as static rows. The 2D commit path (mutate the record, re-enqueue through
EnqueueCad2D* - which the copy thread ingests as an upsert) is deliberately not wired yet.

No point groups: 2D coordinates are already page coordinates, so there is nothing to convert. The
pointGroup machinery is X/Y/Z triples anyway, which a 2D X/Y pair does not fit. */

// LINE2D: two end points.
const PropertyFieldDescriptor kLine2DFields[] = {
    { UITextID::PropPoint1X, [](const void* o) -> double { return static_cast<const Cad2DLineRecordCPU*>(o)->x1; },
        nullptr, PropertyFieldKind::Real, 0, false },
    { UITextID::PropPoint1Y, [](const void* o) -> double { return static_cast<const Cad2DLineRecordCPU*>(o)->y1; },
        nullptr, PropertyFieldKind::Real, 1, false },
    { UITextID::PropPoint2X, [](const void* o) -> double { return static_cast<const Cad2DLineRecordCPU*>(o)->x2; },
        nullptr, PropertyFieldKind::Real, 2, false },
    { UITextID::PropPoint2Y, [](const void* o) -> double { return static_cast<const Cad2DLineRecordCPU*>(o)->y2; },
        nullptr, PropertyFieldKind::Real, 3, false },
};

// CIRCLE2D: center + radius.
const PropertyFieldDescriptor kCircle2DFields[] = {
    { UITextID::PropCenterX, [](const void* o) -> double { return static_cast<const Cad2DCircleRecordCPU*>(o)->centerX; },
        nullptr, PropertyFieldKind::Real, 0, false },
    { UITextID::PropCenterY, [](const void* o) -> double { return static_cast<const Cad2DCircleRecordCPU*>(o)->centerY; },
        nullptr, PropertyFieldKind::Real, 1, false },
    { UITextID::PropRadius, [](const void* o) -> double { return static_cast<const Cad2DCircleRecordCPU*>(o)->radius; },
        nullptr, PropertyFieldKind::Real, 2, true },
};

// ELLIPSE2D: center, the two radii, and the CCW rotation of the radius axes (shown in degrees,
// stored in radians - the one derived field here, and the reason it is read-only twice over).
const PropertyFieldDescriptor kEllipse2DFields[] = {
    { UITextID::PropCenterX, [](const void* o) -> double { return static_cast<const Cad2DEllipseRecordCPU*>(o)->centerX; },
        nullptr, PropertyFieldKind::Real, 0, false },
    { UITextID::PropCenterY, [](const void* o) -> double { return static_cast<const Cad2DEllipseRecordCPU*>(o)->centerY; },
        nullptr, PropertyFieldKind::Real, 1, false },
    { UITextID::PropRadiusX, [](const void* o) -> double { return static_cast<const Cad2DEllipseRecordCPU*>(o)->radiusX; },
        nullptr, PropertyFieldKind::Real, 2, true },
    { UITextID::PropRadiusY, [](const void* o) -> double { return static_cast<const Cad2DEllipseRecordCPU*>(o)->radiusY; },
        nullptr, PropertyFieldKind::Real, 3, true },
    { UITextID::PropRotationZ, [](const void* o) -> double {
            return static_cast<const Cad2DEllipseRecordCPU*>(o)->rotationRadians * 180.0 / M_PI; },
        nullptr, PropertyFieldKind::Real, 4, false },
};

// ARC2D: the ellipse fields plus the two sweep end points, which are stored in page coordinates.
const PropertyFieldDescriptor kArc2DFields[] = {
    { UITextID::PropCenterX, [](const void* o) -> double { return static_cast<const Cad2DArcRecordCPU*>(o)->centerX; },
        nullptr, PropertyFieldKind::Real, 0, false },
    { UITextID::PropCenterY, [](const void* o) -> double { return static_cast<const Cad2DArcRecordCPU*>(o)->centerY; },
        nullptr, PropertyFieldKind::Real, 1, false },
    { UITextID::PropRadiusX, [](const void* o) -> double { return static_cast<const Cad2DArcRecordCPU*>(o)->radiusX; },
        nullptr, PropertyFieldKind::Real, 2, true },
    { UITextID::PropRadiusY, [](const void* o) -> double { return static_cast<const Cad2DArcRecordCPU*>(o)->radiusY; },
        nullptr, PropertyFieldKind::Real, 3, true },
    { UITextID::PropRotationZ, [](const void* o) -> double {
            return static_cast<const Cad2DArcRecordCPU*>(o)->rotationRadians * 180.0 / M_PI; },
        nullptr, PropertyFieldKind::Real, 4, false },
    { UITextID::PropStartX, [](const void* o) -> double { return static_cast<const Cad2DArcRecordCPU*>(o)->startX; },
        nullptr, PropertyFieldKind::Real, 5, false },
    { UITextID::PropStartY, [](const void* o) -> double { return static_cast<const Cad2DArcRecordCPU*>(o)->startY; },
        nullptr, PropertyFieldKind::Real, 6, false },
    { UITextID::PropEndX, [](const void* o) -> double { return static_cast<const Cad2DArcRecordCPU*>(o)->endX; },
        nullptr, PropertyFieldKind::Real, 7, false },
    { UITextID::PropEndY, [](const void* o) -> double { return static_cast<const Cad2DArcRecordCPU*>(o)->endY; },
        nullptr, PropertyFieldKind::Real, 8, false },
};

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

    // Page2D. Read-only, no point groups. POLYLINE2D / POLYGON2D / TEXT2D are left out for the same
    // reason as the vertex-list solids above: a variable-length point list (or a string) does not
    // fit a fixed field table, so they fall through to Type + ID.
    { ObjectType::Line2D, kLine2DFields, static_cast<uint8_t>(std::size(kLine2DFields)), nullptr, { 0, 0 }, 0 },
    { ObjectType::Circle2D, kCircle2DFields, static_cast<uint8_t>(std::size(kCircle2DFields)), nullptr, { 0, 0 }, 0 },
    { ObjectType::Ellipse2D, kEllipse2DFields, static_cast<uint8_t>(std::size(kEllipse2DFields)), nullptr, { 0, 0 }, 0 },
    { ObjectType::Arc2D, kArc2DFields, static_cast<uint8_t>(std::size(kArc2DFields)), nullptr, { 0, 0 }, 0 },
};

const size_t kPropertyTableCount = std::size(kPropertyTables);

const PropertyTypeDescriptor* FindPropertyTable(ObjectType objectType) {
    for (size_t i = 0; i < kPropertyTableCount; ++i) {
        if (kPropertyTables[i].objectType == objectType) return &kPropertyTables[i];
    }
    return nullptr;
}

bool ValidatePropertyEdit(const PropertyTypeDescriptor& table, const double* values, uint8_t count,
    uint8_t editIndex, double newValue) {
    if (editIndex >= table.fieldCount) return false;
    if (!std::isfinite(newValue)) return false;
    if (table.fields[editIndex].mustBePositive && newValue <= 0.0) return false;
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

void ReadPropertyValuesRaw(const PropertyTypeDescriptor& table, const void* object, double* out) {
    if (!object || !out) return;
    for (uint8_t i = 0; i < table.fieldCount; ++i) out[i] = table.fields[i].get(object);
}

void ReadPropertyValuesForDisplay(const PropertyTypeDescriptor& table, const META_DATA* object,
    double* out) {
    if (!object || !out) return;
    ReadPropertyValuesRaw(table, object, out);

    // const_cast: PlacementForObject is the one switch over the 15 types and hands back a mutable
    // pointer because the move producer writes through it; this path only reads.
    const Placement3D* placement =
        PlacementForObject(table.objectType, const_cast<META_DATA*>(object));
    if (!placement || placement->IsIdentity()) return; // Authored == world; nothing to convert.

    for (uint8_t g = 0; g < table.pointGroupCount && g < kMaxPointGroups; ++g) {
        const uint8_t b = table.pointGroupFirstField[g];
        if (static_cast<uint8_t>(b + 3) > table.fieldCount) continue;
        DirectX::XMFLOAT3 world;
        DirectX::XMStoreFloat3(&world, placement->TransformPoint(DirectX::XMVectorSet(
            static_cast<float>(out[b]), static_cast<float>(out[b + 1]),
            static_cast<float>(out[b + 2]), 1.0f)));
        out[b] = world.x; out[b + 1] = world.y; out[b + 2] = world.z;
    }
}

void ApplyPropertyValueFromDisplay(const PropertyTypeDescriptor& table, META_DATA* object,
    uint8_t fieldIndex, double newValue) {
    if (!object || fieldIndex >= table.fieldCount) return;
    // A read-only table (every Page2D one today) declares no setter. Refuse rather than crash, so
    // "read-only" is enforced at the lowest level and not only by the pane refusing to focus.
    if (!table.fields[fieldIndex].set) return;

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
        static_cast<float>(table.fields[b].get(object)),
        static_cast<float>(table.fields[b + 1].get(object)),
        static_cast<float>(table.fields[b + 2].get(object)), 1.0f)));
    float component[3] = { world.x, world.y, world.z };
    component[fieldIndex - b] = static_cast<float>(newValue);

    DirectX::XMFLOAT3 authored;
    DirectX::XMStoreFloat3(&authored, placement->InverseTransformPoint(
        DirectX::XMVectorSet(component[0], component[1], component[2], 1.0f)));
    // All three, not just the edited one: under a rotation each authored component depends on all
    // three world components, so writing one axis alone would skew the object.
    table.fields[b].set(object, authored.x);
    table.fields[b + 1].set(object, authored.y);
    table.fields[b + 2].set(object, authored.z);
}
