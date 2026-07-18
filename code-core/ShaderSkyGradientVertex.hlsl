// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

// Scene3D sky background. Replaces the old 48 banded ClearRenderTargetView calls with one quad:
// no vertex buffer, the four corners come from SV_VertexID as a triangle strip. The quad spans
// NDC y from ndcTopY (the bottom edge of the reserved top-UI strip) down to -1, so the top-UI area
// keeps the flat sky-top colour the compositor already cleared it to.

cbuffer SkyGradientConstants : register(b0) {
    float3 skyTopColor;
    float  ndcTopY;
    float3 skyHorizonColor;
    float  padding0;
};

struct PSInput {
    float4 position  : SV_POSITION;
    float  gradientT : TEXCOORD0; // 0 at the top of the sky band, 1 at the horizon.
};

PSInput main(uint vertexId : SV_VertexID) {
    // Strip order: (left, top) (right, top) (left, bottom) (right, bottom).
    const float x = (vertexId & 1u) ? 1.0 : -1.0;
    const bool isBottom = (vertexId & 2u) != 0u;

    PSInput output;
    output.position = float4(x, isBottom ? -1.0 : ndcTopY, 0.0, 1.0);
    output.gradientT = isBottom ? 1.0 : 0.0;
    return output;
}
