// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    uint   color    : COLOR0;
    nointerpolation uint atlasIndex : TEXCOORD1;
};

// Array of textures. SM 6.0 allows dynamic indexing via NonUniformResourceIndex
Texture2D atlases[10] : register(t0);
SamplerState samp : register(s0);

// Helper function to find the median of the MSDF channels
float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

float4 main(PSInput input) : SV_TARGET {
    // 1. Decode Vertex Color
    float r = ((input.color >> 0) & 0xFF)  / 255.0;
    float g = ((input.color >> 8) & 0xFF)  / 255.0;
    float b = ((input.color >> 16) & 0xFF) / 255.0;
    float a = ((input.color >> 24) & 0xFF) / 255.0;
    float4 vertexColor = float4(r, g, b, a);

    // 2. Sample the Texture dynamically
    // Dynamic indexing in SM 6.0 requires NonUniformResourceIndex
    float4 sampleColor = atlases[NonUniformResourceIndex(input.atlasIndex)].Sample(samp, input.uv);

    // If this is a standard R8 alpha atlas (like your icons), use red channel.
    // Assuming atlasIndex 0 is MSDF and others are standard coverage:
    if (input.atlasIndex != 0) {
        return float4(vertexColor.rgb, vertexColor.a * sampleColor.r);
    }

    // 3. Process MSDF Data (for MSDF atlas at index 0)
    float3 msd = sampleColor.rgb;
    float sd = median(msd.r, msd.g, msd.b);
    
    // Calculate screen-space pixel range for crisp anti-aliasing scaling
    float2 texSize;
    atlases[NonUniformResourceIndex(input.atlasIndex)].GetDimensions(texSize.x, texSize.y);
    
    // MSDF specific crispness derivative math
    float2 dx = ddx(input.uv * texSize);
    float2 dy = ddy(input.uv * texSize);
    float toPixels = 8.0 * rsqrt(dot(dx, dx) + dot(dy, dy)); // 8.0 accounts for pxRange scaling
    
    float sigDist = sd - 0.5;
    float alpha = clamp(sigDist * toPixels + 0.5, 0.0, 1.0);

    return float4(vertexColor.rgb, vertexColor.a * alpha);
}
