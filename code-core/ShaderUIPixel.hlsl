// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    uint   color    : COLOR0;
    nointerpolation uint atlasIndex : TEXCOORD1;
};

Texture2D atlases[10] : register(t0);
SamplerState samp : register(s0);

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
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

    bool boldText = (input.atlasIndex & 0x80000000u) != 0;
    uint atlasIndex = min(input.atlasIndex & 0xFFu, 9u);
    float4 sampleColor = atlases[NonUniformResourceIndex(atlasIndex)].Sample(samp, input.uv);
    float coverage = sampleColor.r;

    if (atlasIndex == 0) {
        uint atlasWidth;
        uint atlasHeight;
        atlases[0].GetDimensions(atlasWidth, atlasHeight);

        float signedDistance = median(sampleColor.r, sampleColor.g, sampleColor.b) - 0.5;
        if (boldText) {
            signedDistance += 0.075;
        }
        float2 unitRange = float2(4.0 / atlasWidth, 4.0 / atlasHeight);
        float2 screenTexSize = 1.0 / fwidth(input.uv);
        float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);
        coverage = saturate(signedDistance * screenPxRange + 0.5);
        return float4(baseColor.rgb, baseColor.a * coverage);
    }

    return float4(sampleColor.rgb * baseColor.rgb, sampleColor.a * baseColor.a);
}
