// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

/* GPU picking pixel shader. Writes the object's pick id into RT0 (R32_UINT) and the NDC depth
into RT1 (R32_FLOAT). The bound depth-stencil ensures the nearest surface wins per pixel, so RT0
ends up holding the id of the closest object under each rasterized pixel. */

struct PSInput {
    float4 position : SV_POSITION;
    nointerpolation uint id : PICKID;
};

struct PSOutput {
    uint  id    : SV_TARGET0;
    float depth : SV_TARGET1;
};

PSOutput main(PSInput input) {
    PSOutput o;
    o.id = input.id;
    o.depth = input.position.z; // Post-perspective-divide NDC depth in [0,1]
    return o;
}
