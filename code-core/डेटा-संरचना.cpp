// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

// Protobuf encode()/decode() members for the structural shapes declared in डेटा-संरचना.h
// (LINE_MEMBER). Split out of DataStorage.cpp so each shape's serialization sits beside its struct
// and geometry. Bodies are unchanged from the former EncodeLineMember/DecodeLineMember free
// functions; only the addressing moved from `object.field` to direct members. Shared proto helpers
// live in DataStorageProtoHelpers.h. The stub structs (STRUCTURAL_CURVED_MEMBER /
// STRUCTURAL_POLY_MEMBER) are not wired into storage and have no codec.

// Include डेटा.h first so <math.h> is pulled in (via DirectXMath) before डेटा-सामान्य-3D.h (pulled in
// by डेटा-संरचना.h) defines _USE_MATH_DEFINES and its own constexpr M_PI — matches the working order
// in DataStorage.cpp / विश्वकर्मा.cpp / डेटा-सामान्य-3D.cpp / डेटा-पाइप.cpp.
#include "डेटा.h"
#include "डेटा-संरचना.h"

#include "DataStorageProtoHelpers.h"

#include "DataStorage_LINE_MEMBER.pb.h"

// protobuf's runtime headers occupy the GLOBAL namespace `pb` (extension_set.h), so the storage
// namespace is aliased under a distinct name here. DataStorage.cpp can use plain `pb` only because
// its alias lives inside an anonymous namespace that shadows protobuf's global one.
namespace pbv = vishwakarma::storage;
using namespace VishwakarmaStorageCodec;

bool LINE_MEMBER::encode(std::vector<uint8_t>& payload, std::string* errorMessage) const {
    pbv::LineMember message;
    WritePoint3(message.mutable_point1(), point1);
    WritePoint3(message.mutable_point2(), point2);
    message.set_profile_id(profileId);
    WriteColor4(message.mutable_color_main(), colorMain);
    WriteColor4(message.mutable_color_inner(), colorInner);
    WriteColor4(message.mutable_color_cap(), colorCap);
    message.set_user_parameter1(userParameter1);
    message.set_user_parameter2(userParameter2);
    return SerializeMessage(message, payload, errorMessage);
}

bool LINE_MEMBER::decode(const std::vector<uint8_t>& payload) {
    pbv::LineMember message;
    if (!ParseMessage(payload, message)) return false;

    point1 = message.has_point1() ? ReadPoint3(message.point1()) : XMFLOAT3{};
    point2 = message.has_point2() ? ReadPoint3(message.point2()) : XMFLOAT3{};
    profileId = message.profile_id();
    colorMain = message.has_color_main() ? ReadColor4(message.color_main()) : DefaultColor4();
    colorInner = message.has_color_inner() ? ReadColor4(message.color_inner()) : DefaultColor4();
    colorCap = message.has_color_cap() ? ReadColor4(message.color_cap()) : DefaultColor4();
    userParameter1 = message.user_parameter1(); // proto3 default 0 = catalog defaults (v1 files).
    userParameter2 = message.user_parameter2();
    return true;
}
