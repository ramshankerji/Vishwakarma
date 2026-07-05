// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

cbuffer Cad2DViewConstants : register(b0) {
    float2 viewCenterCU;
    float zoomPixelsPerCU;
    float dpiY;
    float2 viewportSizePx;
    float minLineWeightPx;
    float padding0;
};

struct Cad2DLineRecord {
    float2 p0CU;
    float2 p1CU;
    float lineWeight;
    uint lineWeightMode;
    uint colorABGR;
    uint flags;
};

StructuredBuffer<Cad2DLineRecord> Lines : register(t0);

struct PSInput {
    float4 position : SV_POSITION;
    float2 screenPositionPx : TEXCOORD0;
    nointerpolation float2 p0Px : TEXCOORD1;
    nointerpolation float2 p1Px : TEXCOORD2;
    nointerpolation float halfWidthPx : TEXCOORD3;
    nointerpolation uint colorABGR : COLOR0;
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

float ResolveLineWidthPx(Cad2DLineRecord rec) {
    float widthPx = rec.lineWeight;
    if (rec.lineWeightMode == 0u) {
        widthPx = rec.lineWeight * zoomPixelsPerCU;
    }
    else if (rec.lineWeightMode == 2u) {
        widthPx = rec.lineWeight * dpiY / 25.4;
    }
    return max(widthPx, minLineWeightPx);
}

PSInput main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    Cad2DLineRecord rec = Lines[instanceId];
    float2 p0 = ModelToScreen(rec.p0CU);
    float2 p1 = ModelToScreen(rec.p1CU);

    float lineWidthPx = ResolveLineWidthPx(rec);
    if (lineWidthPx <= 1.001) {
        float2 rawAxis = p1 - p0;
        if (abs(rawAxis.y) <= 0.0001 && abs(rawAxis.x) > 0.0001) {
            float y = floor((p0.y + p1.y) * 0.5) + 0.5;
            p0.y = y;
            p1.y = y;
        }
        else if (abs(rawAxis.x) <= 0.0001 && abs(rawAxis.y) > 0.0001) {
            float x = floor((p0.x + p1.x) * 0.5) + 0.5;
            p0.x = x;
            p1.x = x;
        }
    }

    float2 axis = p1 - p0;
    float len = max(length(axis), 0.0001);
    float2 dir = axis / len;
    float2 normal = float2(-dir.y, dir.x);

    float halfWidth = lineWidthPx * 0.5;
    float expand = halfWidth + 1.25;

    float2 corners[6] = {
        p0 - dir * 1.25 - normal * expand,
        p1 + dir * 1.25 - normal * expand,
        p1 + dir * 1.25 + normal * expand,
        p0 - dir * 1.25 - normal * expand,
        p1 + dir * 1.25 + normal * expand,
        p0 - dir * 1.25 + normal * expand
    };

    PSInput output;
    output.screenPositionPx = corners[vertexId];
    output.position = ScreenToClip(output.screenPositionPx);
    output.p0Px = p0;
    output.p1Px = p1;
    output.halfWidthPx = halfWidth;
    // Selection highlight: deep blue (ABGR 0xFFA6260D) when the SELECTED flag bit is set.
    output.colorABGR = (rec.flags & 1u) ? 0xFFA6260Du : rec.colorABGR;
    return output;
}
