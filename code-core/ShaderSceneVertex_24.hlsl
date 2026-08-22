// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

/* PARKED: the 24-byte per-vertex-color vertex shader (position 12 + normal 4 + FP16 color 8). The
engine draws with the lean 16-byte format today - see ShaderSceneVertex_16.hlsl, which is what every
PSO actually binds. This file is kept compiling so the per-vertex-color variants specified under
"Vertex format variants" in website/software/graphics.md (baked colors from imported meshes, FEA
scalar fields) have a maintained starting point rather than one recovered from git history.

Nothing includes its generated header yet. Keep it in step with the _16 twin on everything EXCEPT
where color originates: here it arrives per vertex, there it is unpacked from the instance record.

3D shader code with matrix transformations.
Shaders are like a mini sub-program, which runs on the GPU FOR EACH VERTEX. Massively parallel.
In the following shader code, we do only 1 transformation: Transform the vertex 3D co-ordinate
to screen co-ordinate. Color is passed forward as it is without change.
TODO: In future, we will implement index color system using some transformation here. */

cbuffer ConstantBuffer : register(b0) {
    // We will pack more data here in future, like time, animation parameters etc.
    float4x4 viewProj;
};

/* The per-tab instance arena and its redirect table (website/software/graphics.md, 10M plan
Steps 2-4). Mirrored in ShaderScenePickVertex_24.hlsl and defined on the CPU as InstanceRecord in
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
ShaderScenePickVertex_24.hlsl. */
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

float3 InstanceTransformPoint(InstanceRecord r, float3 p) {
    float4 h = float4(p, 1.0f);
    return float3(dot(h, r.transformA), dot(h, r.transformB), dot(h, r.transformC));
}

// Inverse-transpose normal transform for non-uniform scale, kept in step with the shipping
// ShaderSceneVertex_16.hlsl twin - that file carries the derivation and the scale -> rotate ->
// translate rule it depends on.
float3 InstanceTransformNormal(InstanceRecord r, float3 n) {
    float3 row0 = float3(r.transformA.x, r.transformB.x, r.transformC.x);
    float3 row1 = float3(r.transformA.y, r.transformB.y, r.transformC.y);
    float3 row2 = float3(r.transformA.z, r.transformB.z, r.transformC.z);
    float3 scaleSquared = max(float3(dot(row0, row0), dot(row1, row1), dot(row2, row2)), 1e-12f);
    float3 m = n / scaleSquared;
    return float3(dot(m, r.transformA.xyz), dot(m, r.transformB.xyz), dot(m, r.transformC.xyz));
}

struct PSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float3 normal : NORMAL;
};

PSInput main(float3 position : POSITION, float4 normal : NORMAL, float4 color : COLOR)
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
    result.color = color;
    return result;
}
