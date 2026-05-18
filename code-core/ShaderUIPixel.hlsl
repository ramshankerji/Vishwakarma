// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    uint   color    : COLOR0;
    nointerpolation uint atlasIndex : TEXCOORD1;
};

Texture2D atlases[10] : register(t0);
SamplerState samp : register(s0);

float SampleCoverage(uint atlasIndex, float2 uv) {
    // Shader Model 5.0 does not permit dynamic resource-array indexing.
    // Keep the descriptor array future-ready while sampling through static cases.
    // TODO: Shader Model 6.0 can simplify this with dynamic indexing and bounds checking.
    switch (atlasIndex) {
    case 0: return atlases[0].Sample(samp, uv).r;
    case 1: return atlases[1].Sample(samp, uv).r;
    case 2: return atlases[2].Sample(samp, uv).r;
    case 3: return atlases[3].Sample(samp, uv).r;
    case 4: return atlases[4].Sample(samp, uv).r;
    case 5: return atlases[5].Sample(samp, uv).r;
    case 6: return atlases[6].Sample(samp, uv).r;
    case 7: return atlases[7].Sample(samp, uv).r;
    case 8: return atlases[8].Sample(samp, uv).r;
    case 9: return atlases[9].Sample(samp, uv).r;
    default: return atlases[0].Sample(samp, uv).r;
    }
}

float4 main(PSInput input) : SV_TARGET {
    float r = ((input.color >> 0) & 0xFF) / 255.0;
    float g = ((input.color >> 8) & 0xFF) / 255.0;
    float b = ((input.color >> 16) & 0xFF) / 255.0;
    float a = ((input.color >> 24) & 0xFF) / 255.0;
    float4 baseColor = float4(r, g, b, a);

    if (input.uv.x == 0.0 && input.uv.y == 0.0) {
        return baseColor;
    }

    float coverage = SampleCoverage(input.atlasIndex, input.uv);
    return float4(baseColor.rgb, baseColor.a * coverage);
}
