// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

/* 3D shader code with matrix transformations.
Shaders are like a mini sub-program, which runs on the GPU FOR EACH VERTEX. Massively parallel.
In the following shader code, we do only 1 transformation: Transform the vertex 3D co-ordinate
to screen co-ordinate. Color is passed forward as it is without change.
TODO: In future, we will implement index color system using some transformation here. */

cbuffer ConstantBuffer : register(b0) {
    // We will pack more data here in future, like time, animation parameters etc.
    float4x4 viewProj;
};

StructuredBuffer<float4x4> WorldMatrices : register(t0);
cbuffer PerDraw : register(b1) { uint matrixIndex; };

struct PSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float3 normal : NORMAL;
};

PSInput main(float3 position : POSITION, float4 normal : NORMAL, float4 color : COLOR)
{
    PSInput result;
    float4x4 world = WorldMatrices[matrixIndex];
    //float4x4 world = float4x4( 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1); //Used for debugging.
    float4 worldPos = mul(float4(position, 1.0f), world);

    // Transform position to homogeneous clip space
    result.position = mul(worldPos, viewProj); // correct order with transposed matrices

    // Transform normal to world space
    // Note: If 'world' contains non-uniform scaling, we should use the inverse-transpose.
    // For now, assuming uniform scaling, casting to float3x3 works.
    // Normal (good enough for CAD; inverse-transpose later if non-uniform scale)
    result.normal = mul(normal.xyz, (float3x3)world);
    result.color = color;
    return result;
}
