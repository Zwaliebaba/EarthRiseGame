// Lighting.hlsli — camera-relative three-point shading shared by the scene
// pixel shaders. The CPU computes the key/fill directions from the camera basis
// each frame (so every object gets the same flattering rig regardless of view)
// and uploads them as root constants in b1; see SceneRenderer::SetLighting.
//
//   Key  — brightest, hard Lambert (over the camera's upper-right shoulder).
//   Fill — dim, soft half-Lambert from the opposite side; lifts the shadow side.
//   Rim  — Fresnel silhouette highlight (the real-time stand-in for a back light),
//          which also feeds the bloom pass for a glowing edge.
//
// Cast shadows (the other half of a film key light) are a separate future feature
// (shadow maps); this is the shading half of three-point lighting.

#ifndef NEURON_LIGHTING_HLSLI
#define NEURON_LIGHTING_HLSLI

cbuffer Lighting : register(b1)
{
    float3 g_keyDir;   float g_keyIntensity;  // dir surface->key (world), intensity
    float3 g_fillDir;  float g_fillIntensity; // dir surface->fill (world), intensity
    float3 g_viewDir;  float g_ambient;       // dir surface->camera (rim), ambient floor
    float3 g_rimColor; float g_rimPower;      // rim tint, Fresnel exponent
};

float3 ApplyThreePoint(float3 normal, float3 albedo)
{
    float3 N = normalize(normal);

    float  key  = saturate(dot(N, g_keyDir)) * g_keyIntensity;       // hard key
    float  fill = (dot(N, g_fillDir) * 0.5 + 0.5) * g_fillIntensity; // soft fill
    float3 lit  = albedo * (g_ambient + key + fill);

    // Fresnel rim — brightest where the surface turns away from the camera.
    float rim = pow(1.0 - saturate(dot(N, g_viewDir)), g_rimPower);
    lit += g_rimColor * rim;

    return lit;
}

#endif // NEURON_LIGHTING_HLSLI
