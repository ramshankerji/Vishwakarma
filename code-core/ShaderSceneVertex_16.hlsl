// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

/* 3D scene vertex shader for the LEAN 16-BYTE vertex: position 12 + normal 4, no per-vertex color
(website/software/graphics.md, "Vertex format" and "Vertex format variants").

WHERE COLOR COMES FROM NOW. The 8-byte FP16 per-vertex color this format drops is replaced by ONE
color per object, read from the instance record's packedColor as RGBA8. That is a 33% cut across the
bulk of a model, and it costs per-FACE color: a cylinder that stored a separate base / top / incline
color now draws in its dominant surface color everywhere. Storage keeps all the face colors; they
come back when per-face disaggregation of an object lands, as a variation rather than as a tax on
every vertex.

ShaderSceneVertex_24.hlsl is the parked per-vertex-color twin, kept compiling for the future 24-byte
variant. The two must stay in step on everything EXCEPT where color originates.

Shaders are like a mini sub-program, which runs on the GPU FOR EACH VERTEX. Massively parallel. */

cbuffer ConstantBuffer : register(b0) {
    // We will pack more data here in future, like time, animation parameters etc.
    float4x4 viewProj;
};

/* The per-tab instance arena and its redirect table (website/software/graphics.md, 10M plan
Steps 2-4). Mirrored in ShaderScenePickVertex_16.hlsl and defined on the CPU as InstanceRecord in
RenderScene3D.h - change all three together.

transformA/B/C are the first three ROWS of transpose(world), i.e. the COLUMNS of the row-vector
world matrix. The implicit fourth row is (0,0,0,1), which is why 48 bytes suffice - and why this
must NOT be handed to a generic float4x4 mul. */
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
/* Per-object SubTab membership, 64 bits carried as uint2 for broad compatibility (10M plan
Step 5). Bit N set = visible in the sub-tab occupying slot N. Mirrored in
ShaderScenePickVertex_16.hlsl. */
StructuredBuffer<uint2> VisibilityMask : register(t2);
cbuffer PerDraw : register(b1) { uint gpuInstanceIndex; };
cbuffer PerView : register(b2) { uint subTabBit; };

/* One bit test against one 32-bit half of the membership word. Because a reader only ever touches
ONE of the two halves, a mask observed part-way through an 8-byte write still cannot be read
inconsistently - which is what lets the copy thread mutate it in place under invariant 2.

subTabBit >= 64 means this Viewport has no mask-addressable bit. Since MV_MAX_SUBTABS became 64 -
equal to the mask width - that is reached only by a draw with no sub-tab at all, such as the print
path; every open sub-tab has a bit. Those views show everything rather than nothing. */
bool IsVisibleInSubTab(uint instanceIndex, uint bit) {
    if (bit >= 64u) return true;
    uint2 mask = VisibilityMask[instanceIndex];
    uint word = bit < 32u ? mask.x : mask.y;
    return (word & (1u << (bit & 31u))) != 0u;
}

/* RGBA8, red in the LOW byte - the XMUBYTE4 convention the CPU packs with (PackColorRGBA8 in
डेटा.h). Alpha is the object's opacity, so no separate transparency field is needed.

Note the range consequence, since the vertex color this replaces was FP16: a per-object color can no
longer exceed 1.0. HDR is unaffected - tonemapping is on the OUTPUT side (Phase 5), and CAD surface
colors are authored in [0,1] - but an emissive / overbright base color is not expressible any more. */
float4 UnpackColorRGBA8(uint packed) {
    return float4((packed        ) & 0xFFu,
                  (packed >>  8u ) & 0xFFu,
                  (packed >> 16u ) & 0xFFu,
                  (packed >> 24u ) & 0xFFu) * (1.0f / 255.0f);
}

float3 InstanceTransformPoint(InstanceRecord r, float3 p) {
    float4 h = float4(p, 1.0f);
    return float3(dot(h, r.transformA), dot(h, r.transformB), dot(h, r.transformC));
}

float3 InstanceTransformNormal(InstanceRecord r, float3 n) {
    // Uniform-scale assumption stands; inverse-transpose is a later revision.
    return float3(dot(n, r.transformA.xyz), dot(n, r.transformB.xyz), dot(n, r.transformC.xyz));
}

/* Deliberately NOT nointerpolation, even though the color is one value for the whole object now.
ShaderScenePixel.hlsl and ShaderSceneHighlightPixel.hlsl are shared with the parked 24-byte twin,
whose color is genuinely per-vertex and MUST interpolate; a flat modifier here would have to be
matched over there and would silently defeat that variant. Interpolating three identical corner
values costs nothing and is what keeps one pixel shader serving both vertex formats. */
struct PSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float3 normal : NORMAL;
};

PSInput main(float3 position : POSITION, float4 normal : NORMAL)
{
    PSInput result;
    /* Hidden objects collapse to a degenerate primitive. Every vertex of the object takes this same
    branch, so all three corners of every triangle land on the identical position and nothing is
    rasterised. Vertex shading still runs for hidden geometry - the accepted interim cost until
    Step 7's compute compaction drops hidden objects before they ever become draw commands. */
    if (!IsVisibleInSubTab(gpuInstanceIndex, subTabBit)) {
        result.position = float4(0.0f, 0.0f, 0.0f, 0.0f);
        result.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
        result.normal = float3(0.0f, 0.0f, 1.0f);
        return result;
    }

    // Two loads, not one. The redirect is what lets a move rewrite 4 bytes instead of 64: a reader
    // sees the old slot or the new one, both holding a valid transform, never a torn record.
    uint slot = InstanceSlotOf[gpuInstanceIndex];
    InstanceRecord instance = Instances[slot];

    float3 worldPos = InstanceTransformPoint(instance, position);

    // Transform position to homogeneous clip space
    result.position = mul(float4(worldPos, 1.0f), viewProj);

    result.normal = InstanceTransformNormal(instance, normal.xyz);
    // The whole point of the 16-byte vertex: color is per OBJECT, not per vertex.
    result.color = UnpackColorRGBA8(instance.packedColor);
    return result;
}
