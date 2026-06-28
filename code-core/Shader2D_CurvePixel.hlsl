// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

struct PSInput {
    float4 position : SV_POSITION;
    float2 screenPositionPx : TEXCOORD0;
    nointerpolation float2 centerPx : TEXCOORD1;
    nointerpolation float2 radiiPx : TEXCOORD2;
    nointerpolation float2 angleRange : TEXCOORD3;
    nointerpolation float2 startPx : TEXCOORD4;
    nointerpolation float2 endPx : TEXCOORD5;
    nointerpolation float halfWidthPx : TEXCOORD6;
    nointerpolation uint colorABGR : COLOR0;
    nointerpolation uint curveType : TEXCOORD7;
};

float4 DecodeABGR(uint colorABGR) {
    float r = ((colorABGR >> 0) & 0xFF) / 255.0;
    float g = ((colorABGR >> 8) & 0xFF) / 255.0;
    float b = ((colorABGR >> 16) & 0xFF) / 255.0;
    float a = ((colorABGR >> 24) & 0xFF) / 255.0;
    return float4(r, g, b, a);
}

float NormalizeAngle(float angle) {
    static const float Tau = 6.28318530718;
    return fmod(fmod(angle, Tau) + Tau, Tau);
}

bool AngleInCCWSweep(float angle, float startAngle, float endAngle) {
    static const float Tau = 6.28318530718;
    angle = NormalizeAngle(angle);
    startAngle = NormalizeAngle(startAngle);
    endAngle = NormalizeAngle(endAngle);
    if (endAngle < startAngle) endAngle += Tau;
    if (angle < startAngle) angle += Tau;
    return angle <= endAngle;
}

float EllipseStrokeDistance(float2 screenPx, float2 centerPx, float2 radiiPx, out float curveAngle) {
    float2 safeRadii = max(radiiPx, float2(0.0001, 0.0001));
    float2 localPx = screenPx - centerPx;
    float2 normalizedModel = float2(localPx.x / safeRadii.x, -localPx.y / safeRadii.y);

    if (dot(normalizedModel, normalizedModel) <= 0.00000001) {
        curveAngle = 0.0;
    }
    else {
        curveAngle = atan2(normalizedModel.y, normalizedModel.x);
    }

    float2 closestPx = centerPx + float2(cos(curveAngle) * safeRadii.x, -sin(curveAngle) * safeRadii.y);
    return length(screenPx - closestPx);
}

float4 main(PSInput input) : SV_TARGET {
    float curveAngle = 0.0;
    float distanceToStroke = EllipseStrokeDistance(input.screenPositionPx, input.centerPx,
        input.radiiPx, curveAngle);

    if (input.curveType == 2u) {
        const bool inSweep = AngleInCCWSweep(curveAngle, input.angleRange.x, input.angleRange.y);
        const float capDistance = min(length(input.screenPositionPx - input.startPx),
            length(input.screenPositionPx - input.endPx));
        distanceToStroke = inSweep ? min(distanceToStroke, capDistance) : capDistance;
    }

    float aa = max(fwidth(distanceToStroke), 0.0001);
    float coverage = saturate((input.halfWidthPx + aa * 0.5 - distanceToStroke) / aa);
    float4 color = DecodeABGR(input.colorABGR);
    return float4(color.rgb, color.a * coverage);
}
