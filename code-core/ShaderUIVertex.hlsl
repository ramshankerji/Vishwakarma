// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

cbuffer OrthoConstantBuffer : register(b0) {
    float4x4 ortho;
};

struct VSInput {
    float2 position : POSITION;
    float2 uv       : TEXCOORD0;
    uint   color    : COLOR0;
    uint   atlasIndex : TEXCOORD1;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    uint   color    : COLOR0;
    nointerpolation uint atlasIndex : TEXCOORD1;
};

PSInput main(VSInput input) {
    PSInput output;
    float4 pos = float4(input.position, 0, 1);
    output.position = mul(pos, ortho);
    output.uv = input.uv;
    output.color = input.color;
    output.atlasIndex = input.atlasIndex;
    return output;
}
