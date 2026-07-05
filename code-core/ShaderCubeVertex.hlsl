// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

/* Rotation-center overlay cube vertex shader. Transforms a unit cube by a single precomputed MVP
(model = translate(orbit target) * scale(distance-proportional), so the cube keeps a constant
on-screen size). Independent of the scene pipeline. See website/software/selection.md. */

cbuffer CubeConstants : register(b0) {
    float4x4 mvp;
    float4   cubeColor; // rgb = tint, a = overall opacity (fade)
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
};

PSInput main(float3 position : POSITION, float3 normal : NORMAL) {
    PSInput r;
    r.position = mul(float4(position, 1.0f), mvp);
    r.normal = normal;
    return r;
}
