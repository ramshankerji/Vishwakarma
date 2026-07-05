// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

/* GPU picking vertex shader. Identical transform to ShaderSceneVertex, but instead of forwarding
color/normal it forwards the per-draw matrixIndex (+1) as the object's pick id. The pixel shader
writes this id and the NDC depth into the pick render targets so the CPU can identify the object
and reconstruct the surface point under the cursor. See website/software/selection.md. */

cbuffer ConstantBuffer : register(b0) {
    float4x4 viewProj;
};

StructuredBuffer<float4x4> WorldMatrices : register(t0);
cbuffer PerDraw : register(b1) { uint matrixIndex; };

struct PSInput {
    float4 position : SV_POSITION;
    nointerpolation uint id : PICKID; // matrixIndex + 1 (0 is reserved for background)
};

PSInput main(float3 position : POSITION, float4 normal : NORMAL, float4 color : COLOR) {
    PSInput result;
    float4x4 world = WorldMatrices[matrixIndex];
    float4 worldPos = mul(float4(position, 1.0f), world);
    result.position = mul(worldPos, viewProj);
    result.id = matrixIndex + 1u;
    return result;
}
