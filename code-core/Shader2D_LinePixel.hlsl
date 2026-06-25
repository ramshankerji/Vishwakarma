// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

struct PSInput {
    float4 position : SV_POSITION;
    float2 screenPositionPx : TEXCOORD0;
    nointerpolation float2 p0Px : TEXCOORD1;
    nointerpolation float2 p1Px : TEXCOORD2;
    nointerpolation float halfWidthPx : TEXCOORD3;
    nointerpolation uint colorABGR : COLOR0;
};

float4 DecodeABGR(uint colorABGR) {
    float r = ((colorABGR >> 0) & 0xFF) / 255.0;
    float g = ((colorABGR >> 8) & 0xFF) / 255.0;
    float b = ((colorABGR >> 16) & 0xFF) / 255.0;
    float a = ((colorABGR >> 24) & 0xFF) / 255.0;
    return float4(r, g, b, a);
}

float4 main(PSInput input) : SV_TARGET {
    float2 ba = input.p1Px - input.p0Px;
    float segmentLengthSquared = max(dot(ba, ba), 0.0001);
    float t = saturate(dot(input.screenPositionPx - input.p0Px, ba) / segmentLengthSquared);
    float2 closest = input.p0Px + ba * t;
    float distanceToSegment = length(input.screenPositionPx - closest);

    float aa = max(fwidth(distanceToSegment), 0.75);
    float coverage = saturate((input.halfWidthPx + aa - distanceToSegment) / aa);
    float4 color = DecodeABGR(input.colorABGR);
    return float4(color.rgb, color.a * coverage);
}
