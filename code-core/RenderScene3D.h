// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#pragma once
#include <DirectXMath.h>

struct CameraState { // Each view gets its own camera state. 
    //This is part of the "View" data structure, not the "Tab" data structure. Each tab can have multiple views.
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 target;
    DirectX::XMFLOAT3 up;
    float fov;
    float aspect;
    float nearZ;
    float farZ;

    CameraState() { Initialize(); }
    void Initialize() {
        position = { 0.0f, -10.0f, 2.0f };
        target = { 0.0f, 0.0f,  0.0f };
        up = { 0.0f, 0.0f,  1.0f }; // Z-Up is perfect for an XY orbit.

        fov = DirectX::XMConvertToRadians(60.0f);
        aspect = 1.0f; // SAFE DEFAULT
        nearZ = 0.1f;
        farZ = 1000.0f;
    }
};

inline void UpdateCameraOrbit(CameraState& cam)
{
    // Calculate the 2D radius from the target on the XY plane. We ignore Z here to prevent the "spiral away" bug.
    float dx = cam.position.x - cam.target.x;
    float dy = cam.position.y - cam.target.y;
    float radius = hypotf(dx, dy);
    if (radius < 0.001f) radius = 10.0f;// Safety check to prevent radius becoming 0 (which locks the camera)

    // Stateless: advance from the camera's own azimuth, so every view camera orbits independently.
    float rotationAngle = atan2f(dy, dx) + 0.002f; // per-frame speed

    float x = cam.target.x + cosf(rotationAngle) * radius; // Orbit in XY plane
    float y = cam.target.y + sinf(rotationAngle) * radius;
    float z = cam.position.z;// Z remains static (height)
    cam.position = { x, y, z };
}
