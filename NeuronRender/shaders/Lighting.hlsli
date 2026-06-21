// Lighting.hlsli — scene shading shared by the scene pixel shaders.
//
// Natural space lighting = ONE dominant world-fixed "sun" (the key), the shadow
// side lifted by a cool ambient/bounce (the fill), and a view-based Fresnel rim
// for silhouette separation. The key/fill directions are WORLD-FIXED (not
// camera-relative) so every object is lit consistently from the same star — that
// consistent lit-side/shadow-side across the scene is what reads as natural and
// gives depth. The warm-key / cool-fill split is the core Darwinia colour cue.
// Only the rim's view direction tracks the camera (the CPU updates b1 each frame;
// see SceneRenderer::SetLighting).
//
// Cast shadows (shadow maps) and IBL/ambient probes remain future upgrades.

#ifndef NEURON_LIGHTING_HLSLI
#define NEURON_LIGHTING_HLSLI

cbuffer Lighting : register(b1)
{
    float3 g_keyDir;    float _pad0;     // dir surface->sun (world)
    float3 g_keyColor;  float _pad1;     // warm key radiance (colour * intensity)
    float3 g_fillDir;   float _pad2;     // dir surface->fill (world, opposite side)
    float3 g_fillColor; float _pad3;     // cool fill radiance (half-Lambert)
    float3 g_ambient;   float _pad4;     // cool ambient floor (never pitch black)
    float3 g_rimColor;  float g_rimPower; // rim tint + Fresnel exponent
    float3 g_viewDir;   float _pad5;     // dir surface->camera (rim, per-frame)
};

float3 ApplyThreePoint(float3 normal, float3 albedo)
{
    float3 N = normalize(normal);

    float  key  = saturate(dot(N, g_keyDir));            // hard sun key
    float  fill = dot(N, g_fillDir) * 0.5 + 0.5;         // soft fill (half-Lambert)
    float3 lit  = albedo * (g_ambient + g_keyColor * key + g_fillColor * fill);

    // Fresnel rim — brightest where the surface turns away from the camera.
    float rim = pow(1.0 - saturate(dot(N, g_viewDir)), g_rimPower);
    lit += g_rimColor * rim;

    return lit;
}

#endif // NEURON_LIGHTING_HLSLI
