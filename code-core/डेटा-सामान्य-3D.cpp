// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

// Protobuf encode()/decode() members for the basic 3D geometry shapes declared in डेटा-सामान्य-3D.h.
// Split out of DataStorage.cpp so each shape's serialization sits beside its struct and geometry.
// Bodies are unchanged from the former EncodeXxx/DecodeXxx free functions; only the addressing
// moved from `object.field` to direct members. Shared proto helpers live in
// DataStorageProtoHelpers.h. Pipe/structure types (Elbow/Tee/Flange/LineMember) still keep their
// free codecs in DataStorage.cpp.

// Include डेटा.h first so <math.h> is pulled in (via DirectXMath) before डेटा-सामान्य-3D.h defines
// _USE_MATH_DEFINES and its own constexpr M_PI — matches the working order in DataStorage.cpp /
// विश्वकर्मा.cpp / PropertyPane.cpp.
#include "डेटा.h"
#include "डेटा-सामान्य-3D.h"

#include "DataStorageProtoHelpers.h"

#include "DataStorage_CONE.pb.h"
#include "DataStorage_CUBOID.pb.h"
#include "DataStorage_CYLINDER.pb.h"
#include "DataStorage_ELLIPSOID.pb.h"
#include "DataStorage_FRUSTUM_OF_CONE.pb.h"
#include "DataStorage_FRUSTUM_OF_PYRAMID.pb.h"
#include "DataStorage_PARALLELEPIPED.pb.h"
#include "DataStorage_PIPE.pb.h"
#include "DataStorage_PYRAMID.pb.h"
#include "DataStorage_SPHERE.pb.h"
#include "DataStorage_TORUS.pb.h"

// protobuf's runtime headers occupy the GLOBAL namespace `pb` (extension_set.h), so the storage
// namespace is aliased under a distinct name here. DataStorage.cpp can use plain `pb` only because
// its alias lives inside an anonymous namespace that shadows protobuf's global one.
namespace pbv = vishwakarma::storage;
using namespace VishwakarmaStorageCodec;

// ---------------------------------- encode() ----------------------------------

bool PYRAMID::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::Pyramid message;
    for (const XMFLOAT3& vertex : vertices) {
        WritePoint3(message.add_vertices(), vertex);
    }
    for (const XMHALF4& color : colors) {
        WriteColor4(message.add_colors(), color);
    }
    return SerializeMessage(message, payload, errorMessage);
}

bool CUBOID::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::Cuboid message;
    for (const XMFLOAT3& vertex : vertices) {
        WritePoint3(message.add_vertices(), vertex);
    }
    WriteColor4(message.mutable_color(), colors);
    return SerializeMessage(message, payload, errorMessage);
}

bool CONE::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::Cone message;
    WritePoint3(message.mutable_apex(), apex);
    WritePoint3(message.mutable_base_center(), baseCenter);
    message.set_radius(radius);
    WriteColor4(message.mutable_color_base(), colorBase);
    WriteColor4(message.mutable_color_incline(), colorIncline);
    return SerializeMessage(message, payload, errorMessage);
}

bool CYLINDER::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::Cylinder message;
    WritePoint3(message.mutable_p1(), p1);
    WritePoint3(message.mutable_p2(), p2);
    message.set_radius(radius);
    WriteColor4(message.mutable_color_base(), colorBase);
    WriteColor4(message.mutable_color_top(), colorTop);
    WriteColor4(message.mutable_color_incline(), colorIncline);
    return SerializeMessage(message, payload, errorMessage);
}

bool PARALLELEPIPED::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::Parallelepiped message;
    for (const XMFLOAT3& vertex : vertices) {
        WritePoint3(message.add_vertices(), vertex);
    }
    WriteColor4(message.mutable_color(), colors);
    return SerializeMessage(message, payload, errorMessage);
}

bool SPHERE::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::Sphere message;
    WritePoint3(message.mutable_center(), center);
    message.set_radius(radius);
    WriteColor4(message.mutable_color(), color);
    return SerializeMessage(message, payload, errorMessage);
}

bool TORUS::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::Torus message;
    WritePoint3(message.mutable_center(), center);
    message.set_major_radius(majorRadius);
    message.set_minor_radius(minorRadius);
    WriteColor4(message.mutable_color(), color);
    return SerializeMessage(message, payload, errorMessage);
}

bool ELLIPSOID::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::Ellipsoid message;
    WritePoint3(message.mutable_center(), center);
    message.set_radius_x(radiusX);
    message.set_radius_y(radiusY);
    message.set_radius_z(radiusZ);
    WriteColor4(message.mutable_color(), color);
    return SerializeMessage(message, payload, errorMessage);
}

bool FRUSTUM_OF_PYRAMID::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::FrustumOfPyramid message;
    for (const XMFLOAT3& vertex : vertices) {
        WritePoint3(message.add_vertices(), vertex);
    }
    WriteColor4(message.mutable_color_base(), colorBase);
    WriteColor4(message.mutable_color_top(), colorTop);
    WriteColor4(message.mutable_color_incline(), colorIncline);
    return SerializeMessage(message, payload, errorMessage);
}

bool FRUSTUM_OF_CONE::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::FrustumOfCone message;
    WritePoint3(message.mutable_bottom_center(), bottomCenter);
    WritePoint3(message.mutable_top_center(), topCenter);
    message.set_bottom_radius(bottomRadius);
    message.set_top_radius(topRadius);
    WriteColor4(message.mutable_color_base(), colorBase);
    WriteColor4(message.mutable_color_top(), colorTop);
    WriteColor4(message.mutable_color_incline(), colorIncline);
    return SerializeMessage(message, payload, errorMessage);
}

bool PIPE::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::Pipe message;
    WritePoint3(message.mutable_center1(), center1);
    WritePoint3(message.mutable_center2(), center2);
    message.set_outside_diameter(outsideDiameter);
    message.set_inside_diameter(insideDiameter);
    WriteColor4(message.mutable_color_outer(), colorOuter);
    WriteColor4(message.mutable_color_inner(), colorInner);
    WriteColor4(message.mutable_color_cap(), colorCap);
    return SerializeMessage(message, payload, errorMessage);
}

// ---------------------------------- decode() ----------------------------------

bool PYRAMID::decode(const std::vector<uint8_t>& payload) {
    pbv::Pyramid message;
    if (!ParseMessage(payload, message)) return false;

    vertices.clear();
    vertices.reserve(message.vertices_size());
    for (int i = 0; i < message.vertices_size(); ++i) {
        vertices.push_back(ReadPoint3(message.vertices(i)));
    }

    colors.clear();
    colors.reserve(message.colors_size());
    for (int i = 0; i < message.colors_size(); ++i) {
        colors.push_back(ReadColor4(message.colors(i)));
    }
    if (colors.empty()) colors.push_back(DefaultColor4());

    return vertices.size() >= 4;
}

bool CUBOID::decode(const std::vector<uint8_t>& payload) {
    pbv::Cuboid message;
    if (!ParseMessage(payload, message)) return false;

    vertices.clear();
    vertices.reserve(message.vertices_size());
    for (int i = 0; i < message.vertices_size(); ++i) {
        vertices.push_back(ReadPoint3(message.vertices(i)));
    }
    colors = message.has_color() ? ReadColor4(message.color()) : DefaultColor4();
    return vertices.size() >= 8;
}

bool CONE::decode(const std::vector<uint8_t>& payload) {
    pbv::Cone message;
    if (!ParseMessage(payload, message)) return false;

    apex = message.has_apex() ? ReadPoint3(message.apex()) : XMFLOAT3{};
    baseCenter = message.has_base_center() ? ReadPoint3(message.base_center()) : XMFLOAT3{};
    radius = message.radius();
    colorBase = message.has_color_base() ? ReadColor4(message.color_base()) : DefaultColor4();
    colorIncline = message.has_color_incline() ? ReadColor4(message.color_incline()) : DefaultColor4();
    return true;
}

bool CYLINDER::decode(const std::vector<uint8_t>& payload) {
    pbv::Cylinder message;
    if (!ParseMessage(payload, message)) return false;

    p1 = message.has_p1() ? ReadPoint3(message.p1()) : XMFLOAT3{};
    p2 = message.has_p2() ? ReadPoint3(message.p2()) : XMFLOAT3{};
    radius = message.radius();
    colorBase = message.has_color_base() ? ReadColor4(message.color_base()) : DefaultColor4();
    colorTop = message.has_color_top() ? ReadColor4(message.color_top()) : DefaultColor4();
    colorIncline = message.has_color_incline() ? ReadColor4(message.color_incline()) : DefaultColor4();
    return true;
}

bool PARALLELEPIPED::decode(const std::vector<uint8_t>& payload) {
    pbv::Parallelepiped message;
    if (!ParseMessage(payload, message)) return false;

    vertices.clear();
    vertices.reserve(message.vertices_size());
    for (int i = 0; i < message.vertices_size(); ++i) {
        vertices.push_back(ReadPoint3(message.vertices(i)));
    }
    colors = message.has_color() ? ReadColor4(message.color()) : DefaultColor4();
    return vertices.size() >= 8;
}

bool SPHERE::decode(const std::vector<uint8_t>& payload) {
    pbv::Sphere message;
    if (!ParseMessage(payload, message)) return false;

    center = message.has_center() ? ReadPoint3(message.center()) : XMFLOAT3{};
    radius = message.radius();
    color = message.has_color() ? ReadColor4(message.color()) : DefaultColor4();
    return true;
}

bool TORUS::decode(const std::vector<uint8_t>& payload) {
    pbv::Torus message;
    if (!ParseMessage(payload, message)) return false;

    center = message.has_center() ? ReadPoint3(message.center()) : XMFLOAT3{};
    majorRadius = message.major_radius() > 0.0f ? message.major_radius() : 0.5f;
    minorRadius = message.minor_radius() > 0.0f ? message.minor_radius() : 0.125f;
    color = message.has_color() ? ReadColor4(message.color()) : DefaultColor4();
    return true;
}

bool ELLIPSOID::decode(const std::vector<uint8_t>& payload) {
    pbv::Ellipsoid message;
    if (!ParseMessage(payload, message)) return false;

    center = message.has_center() ? ReadPoint3(message.center()) : XMFLOAT3{};
    radiusX = message.radius_x() > 0.0f ? message.radius_x() : 0.5f;
    radiusY = message.radius_y() > 0.0f ? message.radius_y() : 0.5f;
    radiusZ = message.radius_z() > 0.0f ? message.radius_z() : 0.5f;
    color = message.has_color() ? ReadColor4(message.color()) : DefaultColor4();
    return true;
}

bool FRUSTUM_OF_PYRAMID::decode(const std::vector<uint8_t>& payload) {
    pbv::FrustumOfPyramid message;
    if (!ParseMessage(payload, message)) return false;

    vertices.clear();
    vertices.reserve(message.vertices_size());
    for (int i = 0; i < message.vertices_size(); ++i) {
        vertices.push_back(ReadPoint3(message.vertices(i)));
    }
    colorBase = message.has_color_base() ? ReadColor4(message.color_base()) : DefaultColor4();
    colorTop = message.has_color_top() ? ReadColor4(message.color_top()) : DefaultColor4();
    colorIncline = message.has_color_incline() ? ReadColor4(message.color_incline()) : DefaultColor4();
    return vertices.size() >= 8;
}

bool FRUSTUM_OF_CONE::decode(const std::vector<uint8_t>& payload) {
    pbv::FrustumOfCone message;
    if (!ParseMessage(payload, message)) return false;

    bottomCenter = message.has_bottom_center() ? ReadPoint3(message.bottom_center()) : XMFLOAT3{};
    topCenter = message.has_top_center() ? ReadPoint3(message.top_center()) : XMFLOAT3{};
    bottomRadius = message.bottom_radius();
    topRadius = message.top_radius();
    colorBase = message.has_color_base() ? ReadColor4(message.color_base()) : DefaultColor4();
    colorTop = message.has_color_top() ? ReadColor4(message.color_top()) : DefaultColor4();
    colorIncline = message.has_color_incline() ? ReadColor4(message.color_incline()) : DefaultColor4();
    return true;
}

bool PIPE::decode(const std::vector<uint8_t>& payload) {
    pbv::Pipe message;
    if (!ParseMessage(payload, message)) return false;

    center1 = message.has_center1() ? ReadPoint3(message.center1()) : XMFLOAT3{};
    center2 = message.has_center2() ? ReadPoint3(message.center2()) : XMFLOAT3{};
    outsideDiameter = message.outside_diameter();
    insideDiameter = message.inside_diameter();
    colorOuter = message.has_color_outer() ? ReadColor4(message.color_outer()) : DefaultColor4();
    colorInner = message.has_color_inner() ? ReadColor4(message.color_inner()) : DefaultColor4();
    colorCap = message.has_color_cap() ? ReadColor4(message.color_cap()) : DefaultColor4();
    return true;
}
