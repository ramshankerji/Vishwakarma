// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

/* Selection highlight pixel shader. Reuses the scene vertex shader (ShaderSceneVertex) unchanged,
but overrides the surface color with a deep blue, keeping the same hemispherical shading so the
object still reads as a 3D form. Used to redraw the currently selected objects on top of the scene.
See website/software/selection.md. */

struct PSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float3 normal : NORMAL;
};

float4 main(PSInput input) : SV_TARGET {
    float3 norm = normalize(input.normal);
    float3 up = float3(0.0f, 0.0f, 1.0f);
    float t = 0.5f * (dot(norm, up) + 1.0f);         // [-1,1] -> [0,1]
    float3 deepBlue = float3(0.05f, 0.15f, 0.65f);   // selection override color
    float3 shaded = deepBlue * lerp(0.55f, 1.20f, t);
    return float4(shaded, 1.0f);
}
