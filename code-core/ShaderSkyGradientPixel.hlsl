// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

cbuffer SkyGradientConstants : register(b0) {
    float3 skyTopColor;
    float  ndcTopY;
    float3 skyHorizonColor;
    float  padding0;
};

struct PSInput {
    float4 position  : SV_POSITION;
    float  gradientT : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET {
    // Same smoothstep the CPU band loop used, now evaluated per pixel instead of per band.
    const float t = saturate(input.gradientT);
    const float smoothT = t * t * (3.0 - 2.0 * t);
    return float4(lerp(skyTopColor, skyHorizonColor, smoothT), 1.0);
}
