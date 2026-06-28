// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

cbuffer Cad2DViewConstants : register(b0) {
    float2 viewCenterCU;
    float zoomPixelsPerCU;
    float dpiY;
    float2 viewportSizePx;
    float minLineWeightPx;
    float padding0;
};

struct Cad2DCurveRecord {
    float2 centerCU;
    float2 radiiCU;
    float2 startCU;
    float2 endCU;
    float lineWeight;
    uint lineWeightMode;
    uint colorABGR;
    uint curveType;
    uint flags;
    uint padding0;
    uint padding1;
    uint padding2;
};

StructuredBuffer<Cad2DCurveRecord> Curves : register(t0);

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

float2 ModelToScreen(float2 pCU) {
    float2 delta = pCU - viewCenterCU;
    return float2(
        viewportSizePx.x * 0.5 + delta.x * zoomPixelsPerCU,
        viewportSizePx.y * 0.5 - delta.y * zoomPixelsPerCU);
}

float4 ScreenToClip(float2 screenPx) {
    float2 safeViewport = max(viewportSizePx, float2(1.0, 1.0));
    return float4(
        screenPx.x / safeViewport.x * 2.0 - 1.0,
        1.0 - screenPx.y / safeViewport.y * 2.0,
        0.0,
        1.0);
}

float ResolveLineWidthPx(Cad2DCurveRecord rec) {
    float widthPx = rec.lineWeight;
    if (rec.lineWeightMode == 0u) {
        widthPx = rec.lineWeight * zoomPixelsPerCU;
    }
    else if (rec.lineWeightMode == 2u) {
        widthPx = rec.lineWeight * dpiY / 25.4;
    }
    return max(widthPx, minLineWeightPx);
}

float CurveAngle(float2 pointCU, float2 centerCU, float2 radiiCU) {
    float2 safeRadii = max(abs(radiiCU), float2(0.000001, 0.000001));
    float2 normalized = (pointCU - centerCU) / safeRadii;
    return atan2(normalized.y, normalized.x);
}

PSInput main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    Cad2DCurveRecord rec = Curves[instanceId];
    float2 centerPx = ModelToScreen(rec.centerCU);
    float2 radiiPx = max(abs(rec.radiiCU) * zoomPixelsPerCU, float2(0.0001, 0.0001));
    float halfWidth = ResolveLineWidthPx(rec) * 0.5;
    float expand = halfWidth + 2.0;
    float2 extent = radiiPx + expand;

    float2 corners[6] = {
        centerPx + float2(-extent.x, -extent.y),
        centerPx + float2( extent.x, -extent.y),
        centerPx + float2( extent.x,  extent.y),
        centerPx + float2(-extent.x, -extent.y),
        centerPx + float2( extent.x,  extent.y),
        centerPx + float2(-extent.x,  extent.y)
    };

    PSInput output;
    output.screenPositionPx = corners[vertexId];
    output.position = ScreenToClip(output.screenPositionPx);
    output.centerPx = centerPx;
    output.radiiPx = radiiPx;
    output.angleRange = float2(
        CurveAngle(rec.startCU, rec.centerCU, rec.radiiCU),
        CurveAngle(rec.endCU, rec.centerCU, rec.radiiCU));
    output.startPx = ModelToScreen(rec.startCU);
    output.endPx = ModelToScreen(rec.endCU);
    output.halfWidthPx = halfWidth;
    output.colorABGR = rec.colorABGR;
    output.curveType = rec.curveType;
    return output;
}
