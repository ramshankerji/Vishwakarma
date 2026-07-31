// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

/* GPU picking vertex shader. Identical transform to ShaderSceneVertex, but instead of forwarding
color/normal it forwards the per-draw gpuInstanceIndex (+1) as the object's pick id. The pixel
shader writes this id and the NDC depth into the pick render targets so the CPU can identify the
object and reconstruct the surface point under the cursor. See website/software/selection.md.

Because the id IS the stable dense instance index, the CPU resolves a hit with a single indexed
read of the copy thread's registry (graphics.md, 10M plan Step 3) - no scan over pages. */

cbuffer ConstantBuffer : register(b0) {
    float4x4 viewProj;
};

// Mirrors ShaderSceneVertex.hlsl (and InstanceRecord in RenderScene3D.h) so the pick pass consumes
// the exact same transform as the visible scene - change all three together. See that file for the
// transformA/B/C row convention.
struct InstanceRecord {
    float4 transformA;
    float4 transformB;
    float4 transformC;
    uint   materialIndex;
    uint   packedColor;
    uint   renderFlags;
    uint   packedParams;
};
StructuredBuffer<InstanceRecord> Instances : register(t0);
StructuredBuffer<uint> InstanceSlotOf : register(t1);
StructuredBuffer<uint2> VisibilityMask : register(t2);
cbuffer PerDraw : register(b1) { uint gpuInstanceIndex; };
cbuffer PerView : register(b2) { uint subTabBit; };

// Mirrors IsVisibleInSubTab in ShaderSceneVertex.hlsl - see there for the bit convention. The pick
// pass MUST apply the identical test: an object the user cannot see must not be clickable either.
bool IsVisibleInSubTab(uint instanceIndex, uint bit) {
    if (bit >= 64u) return true;
    uint2 mask = VisibilityMask[instanceIndex];
    uint word = bit < 32u ? mask.x : mask.y;
    return (word & (1u << (bit & 31u))) != 0u;
}

struct PSInput {
    float4 position : SV_POSITION;
    nointerpolation uint id : PICKID; // gpuInstanceIndex + 1 (0 is reserved for background)
};

PSInput main(float3 position : POSITION, float4 normal : NORMAL, float4 color : COLOR) {
    PSInput result;
    if (!IsVisibleInSubTab(gpuInstanceIndex, subTabBit)) {
        result.position = float4(0.0f, 0.0f, 0.0f, 0.0f);
        result.id = 0u; // Background id, so a stray fragment could never resolve to this object.
        return result;
    }

    uint slot = InstanceSlotOf[gpuInstanceIndex];
    InstanceRecord instance = Instances[slot];
    float4 h = float4(position, 1.0f);
    float3 worldPos = float3(dot(h, instance.transformA), dot(h, instance.transformB),
                             dot(h, instance.transformC));
    result.position = mul(float4(worldPos, 1.0f), viewProj);
    result.id = gpuInstanceIndex + 1u;
    return result;
}
