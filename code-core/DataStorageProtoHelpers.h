// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

// Shared protobuf <-> in-memory conversion helpers for the geometry codecs. Used by both
// DataStorage.cpp (pipe/structure/logical/2D codecs) and डेटा-सामान्य-3D.cpp (the basic-3D shape
// encode()/decode() members that were split out to sit beside their structs). Header-only
// (inline / templates) so both translation units share a single definition without ODR clashes.

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <DirectXMath.h>
#include <DirectXPackedVector.h>

#include "DataStorage_Common3D.pb.h" // vishwakarma::storage::Point3F / Color4F / Placement
#include "डेटा.h" // struct Placement3D. Every consumer of this header already pulls डेटा.h in.

namespace VishwakarmaStorageCodec {

inline void SetError(std::string* errorMessage, const std::string& value) {
    if (errorMessage) *errorMessage = value;
}

inline void WritePoint3(vishwakarma::storage::Point3F* message, const DirectX::XMFLOAT3& point) {
    message->set_x(point.x);
    message->set_y(point.y);
    message->set_z(point.z);
}

inline DirectX::XMFLOAT3 ReadPoint3(const vishwakarma::storage::Point3F& message) {
    return DirectX::XMFLOAT3(message.x(), message.y(), message.z());
}

inline void WriteColor4(vishwakarma::storage::Color4F* message, const DirectX::PackedVector::XMHALF4& color) {
    message->set_r(DirectX::PackedVector::XMConvertHalfToFloat(color.x));
    message->set_g(DirectX::PackedVector::XMConvertHalfToFloat(color.y));
    message->set_b(DirectX::PackedVector::XMConvertHalfToFloat(color.z));
    message->set_a(DirectX::PackedVector::XMConvertHalfToFloat(color.w));
}

inline DirectX::PackedVector::XMHALF4 ReadColor4(const vishwakarma::storage::Color4F& message) {
    return DirectX::PackedVector::XMHALF4(message.r(), message.g(), message.b(), message.a());
}

inline DirectX::PackedVector::XMHALF4 DefaultColor4() {
    return DirectX::PackedVector::XMHALF4(0.8f, 0.8f, 0.8f, 1.0f);
}

// Rigid authored -> world placement, field 20 of every 3D geometry message. Callers skip this
// entirely for an identity placement (Placement3D::IsIdentity), so files of never-moved objects
// keep the byte layout the previous schema version produced.
inline void WritePlacement(vishwakarma::storage::Placement* message, const Placement3D& placement) {
    WritePoint3(message->mutable_origin(), placement.origin);
    message->set_qx(placement.rotation.x);
    message->set_qy(placement.rotation.y);
    message->set_qz(placement.rotation.z);
    message->set_qw(placement.rotation.w);
}

inline Placement3D ReadPlacement(const vishwakarma::storage::Placement& message) {
    Placement3D placement; // Identity until proven otherwise.
    if (message.has_origin()) placement.origin = ReadPoint3(message.origin());

    /* proto3 scalars default to 0, so a placement message that was written without a quaternion -
    or truncated - arrives as (0,0,0,0). That is NOT identity: XMMatrixRotationQuaternion turns a
    zero quaternion into a matrix with a zero 3x3 block, which collapses every vertex of the object
    onto the origin. Silent, total, and it would look like the geometry generator failed. Anything
    without a usable length therefore stays identity. */
    const DirectX::XMFLOAT4 quaternion(message.qx(), message.qy(), message.qz(), message.qw());
    const float lengthSquared = quaternion.x * quaternion.x + quaternion.y * quaternion.y +
        quaternion.z * quaternion.z + quaternion.w * quaternion.w;
    if (lengthSquared > 1e-12f) {
        DirectX::XMStoreFloat4(&placement.rotation,
            DirectX::XMQuaternionNormalize(DirectX::XMLoadFloat4(&quaternion)));
    }
    return placement;
}

template <typename Message>
bool SerializeMessage(const Message& message, std::vector<uint8_t>& payload, std::string* errorMessage) {
    std::string bytes;
    if (!message.SerializeToString(&bytes)) {
        SetError(errorMessage, "Could not serialize protobuf payload.");
        return false;
    }

    payload.assign(bytes.begin(), bytes.end());
    return true;
}

template <typename Message>
bool ParseMessage(const std::vector<uint8_t>& payload, Message& message) {
    if (payload.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }

    const void* data = payload.empty()
        ? static_cast<const void*>("")
        : static_cast<const void*>(payload.data());
    return message.ParseFromArray(data, static_cast<int>(payload.size()));
}

} // namespace VishwakarmaStorageCodec
