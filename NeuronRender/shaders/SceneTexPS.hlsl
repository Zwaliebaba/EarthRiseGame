// SceneTexPS.hlsl — textured 3D scene pixel shader.
// Samples the material diffuse (t0) and shades it with the shared camera-relative
// three-point rig (Lighting.hlsli): key + fill + Fresnel rim. The diffuse is the
// albedo; the per-instance colour is unused here (textured shapes show their art).

#include "Lighting.hlsli"

Texture2D    g_diffuse : register(t0);
SamplerState g_samp    : register(s0);

struct PSIn
{
    float4 pos    : SV_POSITION;
    float3 normal : TEXCOORD0;
    float4 color  : TEXCOORD1;
    float2 uv     : TEXCOORD2;
};

float4 main(PSIn p) : SV_TARGET
{
    float3 albedo = g_diffuse.Sample(g_samp, p.uv).rgb;
    return float4(ApplyThreePoint(p.normal, albedo), 1.0f);
}
