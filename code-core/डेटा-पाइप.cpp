// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

// Protobuf encode()/decode() members for the piping shapes declared in डेटा-पाइप.h (ELBOW, TEE,
// FLANGE). Split out of DataStorage.cpp so each shape's serialization sits beside its struct and
// geometry. Bodies are unchanged from the former EncodeXxx/DecodeXxx free functions; only the
// addressing moved from `object.field` to direct members. Shared proto helpers live in
// DataStorageProtoHelpers.h. The stub piping structs (PIPE_SPOOL/T_Y/REDUCER/VALVE/PIPELINE) are
// not wired into storage and have no codec.

// Include डेटा.h first so <math.h> is pulled in (via DirectXMath) before डेटा-सामान्य-3D.h (pulled in
// by डेटा-पाइप.h) defines _USE_MATH_DEFINES and its own constexpr M_PI — matches the working order
// in DataStorage.cpp / विश्वकर्मा.cpp / डेटा-सामान्य-3D.cpp.
#include "डेटा.h"
#include "डेटा-पाइप.h"

#include "DataStorageProtoHelpers.h"

#include "DataStorage_ELBOW.pb.h"
#include "DataStorage_FLANGE.pb.h"
#include "DataStorage_TEE.pb.h"

// protobuf's runtime headers occupy the GLOBAL namespace `pb` (extension_set.h), so the storage
// namespace is aliased under a distinct name here. DataStorage.cpp can use plain `pb` only because
// its alias lives inside an anonymous namespace that shadows protobuf's global one.
namespace pbv = vishwakarma::storage;
using namespace VishwakarmaStorageCodec;

// ---------------------------------- encode() ----------------------------------

bool ELBOW::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::Elbow message;
    WritePoint3(message.mutable_center(), center);
    message.set_bend_radius(bendRadius);
    message.set_outside_diameter(outsideDiameter);
    message.set_inside_diameter(insideDiameter);
    message.set_sweep_angle_radians(sweepAngleRadians);
    WriteColor4(message.mutable_color_outer(), colorOuter);
    WriteColor4(message.mutable_color_inner(), colorInner);
    WriteColor4(message.mutable_color_cap(), colorCap);
    return SerializeMessage(message, payload, errorMessage);
}

bool TEE::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::Tee message;
    WritePoint3(message.mutable_center1(), center1);
    WritePoint3(message.mutable_center2(), center2);
    message.set_main_outside_diameter(mainOutsideDiameter);
    message.set_main_inside_diameter(mainInsideDiameter);
    message.set_branch_angle_degrees(branchAngleDegrees);
    message.set_branch_length(branchLength);
    message.set_branch_outside_diameter(branchOutsideDiameter);
    message.set_branch_inside_diameter(branchInsideDiameter);
    WriteColor4(message.mutable_color_outer(), colorOuter);
    WriteColor4(message.mutable_color_inner(), colorInner);
    WriteColor4(message.mutable_color_cap(), colorCap);
    return SerializeMessage(message, payload, errorMessage);
}

bool FLANGE::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::Flange message;
    WritePoint3(message.mutable_center1(), center1);
    WritePoint3(message.mutable_center2(), center2);
    message.set_flange_outer_diameter(flangeOuterDiameter);
    message.set_bore_diameter(boreDiameter);
    message.set_raised_face_diameter(raisedFaceDiameter);
    message.set_raised_face_projection(raisedFaceProjection);
    WriteColor4(message.mutable_color_face(), colorFace);
    WriteColor4(message.mutable_color_rim(), colorRim);
    WriteColor4(message.mutable_color_bore(), colorBore);
    return SerializeMessage(message, payload, errorMessage);
}

// ---------------------------------- decode() ----------------------------------

bool ELBOW::decode(const std::vector<uint8_t>& payload) {
    pbv::Elbow message;
    if (!ParseMessage(payload, message)) return false;

    center = message.has_center() ? ReadPoint3(message.center()) : XMFLOAT3{};
    bendRadius = message.bend_radius();
    outsideDiameter = message.outside_diameter();
    insideDiameter = message.inside_diameter();
    sweepAngleRadians = message.sweep_angle_radians();
    colorOuter = message.has_color_outer() ? ReadColor4(message.color_outer()) : DefaultColor4();
    colorInner = message.has_color_inner() ? ReadColor4(message.color_inner()) : DefaultColor4();
    colorCap = message.has_color_cap() ? ReadColor4(message.color_cap()) : DefaultColor4();
    return true;
}

bool TEE::decode(const std::vector<uint8_t>& payload) {
    pbv::Tee message;
    if (!ParseMessage(payload, message)) return false;

    center1 = message.has_center1() ? ReadPoint3(message.center1()) : XMFLOAT3{};
    center2 = message.has_center2() ? ReadPoint3(message.center2()) : XMFLOAT3{};
    mainOutsideDiameter = message.main_outside_diameter();
    mainInsideDiameter = message.main_inside_diameter();
    branchAngleDegrees = message.branch_angle_degrees();
    branchLength = message.branch_length();
    branchOutsideDiameter = message.branch_outside_diameter();
    branchInsideDiameter = message.branch_inside_diameter();
    colorOuter = message.has_color_outer() ? ReadColor4(message.color_outer()) : DefaultColor4();
    colorInner = message.has_color_inner() ? ReadColor4(message.color_inner()) : DefaultColor4();
    colorCap = message.has_color_cap() ? ReadColor4(message.color_cap()) : DefaultColor4();
    return true;
}

bool FLANGE::decode(const std::vector<uint8_t>& payload) {
    pbv::Flange message;
    if (!ParseMessage(payload, message)) return false;

    center1 = message.has_center1() ? ReadPoint3(message.center1()) : XMFLOAT3{};
    center2 = message.has_center2() ? ReadPoint3(message.center2()) : XMFLOAT3{};
    flangeOuterDiameter = message.flange_outer_diameter();
    boreDiameter = message.bore_diameter();
    raisedFaceDiameter = message.raised_face_diameter();
    raisedFaceProjection = message.raised_face_projection();
    colorFace = message.has_color_face() ? ReadColor4(message.color_face()) : DefaultColor4();
    colorRim = message.has_color_rim() ? ReadColor4(message.color_rim()) : DefaultColor4();
    colorBore = message.has_color_bore() ? ReadColor4(message.color_bore()) : DefaultColor4();
    return true;
}
