// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

/* Rotation-center overlay cube pixel shader. Simple directional shading, alpha taken from the
constant buffer so the render thread can fade the cube in/out around navigation. */

cbuffer CubeConstants : register(b0) {
    float4x4 mvp;
    float4   cubeColor;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
};

float4 main(PSInput input) : SV_TARGET {
    float3 n = normalize(input.normal);
    float shade = 0.6f + 0.4f * saturate(dot(n, normalize(float3(0.4f, 0.5f, 0.75f))));
    return float4(cubeColor.rgb * shade, cubeColor.a);
}
