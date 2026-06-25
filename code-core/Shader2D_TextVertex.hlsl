// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

cbuffer Cad2DViewConstants : register(b0) {
    float2 viewCenterCU;
    float zoomPixelsPerCU;
    float dpiY;
    float2 viewportSizePx;
    float minLineWeightPx;
    float padding0;
};

struct VSInput {
    float2 positionCU : POSITION;
    float2 uv : TEXCOORD0;
    uint colorABGR : COLOR0;
    uint atlasIndex : TEXCOORD1;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    uint colorABGR : COLOR0;
    nointerpolation uint atlasIndex : TEXCOORD1;
};

float4 ModelToClip(float2 pCU) {
    float2 delta = pCU - viewCenterCU;
    float2 screenPx = float2(
        viewportSizePx.x * 0.5 + delta.x * zoomPixelsPerCU,
        viewportSizePx.y * 0.5 - delta.y * zoomPixelsPerCU);
    float2 safeViewport = max(viewportSizePx, float2(1.0, 1.0));
    return float4(
        screenPx.x / safeViewport.x * 2.0 - 1.0,
        1.0 - screenPx.y / safeViewport.y * 2.0,
        0.0,
        1.0);
}

PSInput main(VSInput input) {
    PSInput output;
    output.position = ModelToClip(input.positionCU);
    output.uv = input.uv;
    output.colorABGR = input.colorABGR;
    output.atlasIndex = input.atlasIndex;
    return output;
}
