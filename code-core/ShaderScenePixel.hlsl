// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

/* Simple pass - through shader that outputs the interpolated vertex color for each pixel
No lighting calculations or texture sampling - just renders solid colors */

struct PSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float3 normal : NORMAL;
};

float4 main(PSInput input) : SV_TARGET {
    float3 norm = normalize(input.normal);// Re-normalize interpolants

    // Hemispherical Lighting Settings
    float3 up = float3(0.0f, 0.0f, 1.0f); // World Up
    // Sky: Bright, slightly bluish white (multiplies your vertex color by ~0.95)
    float3 skyColor = float3(0.9f, 0.95f, 1.0f);
    // Ground: Mid-grey (multiplies by ~0.4). This ensures the shadowed parts are still visible, not pitch black.
    float3 groundColor = float3(0.4f, 0.4f, 0.45f);

    // Calculate blend factor [-1, 1] -> [0, 1] . Dot product gives 1.0 facing up, -1.0 facing down.
    float t = 0.5f * (dot(norm, up) + 1.0f);
    float3 ambientLight = lerp(groundColor, skyColor, t); // Interpolate lighting
    return float4(input.color.rgb * ambientLight, input.color.a); // Apply lighting to surface color
}
