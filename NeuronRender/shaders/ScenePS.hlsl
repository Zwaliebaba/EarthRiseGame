// ScenePS.hlsl — untextured 3D scene pixel shader (cube fallback / untextured
// meshes). Shades the per-instance colour as albedo with the shared camera-
// relative three-point rig (Lighting.hlsli), matching the textured path.

#include "Lighting.hlsli"

struct PSIn
{
    float4 pos    : SV_POSITION;
    float3 normal : TEXCOORD0;
    float4 color  : TEXCOORD1;
};

float4 main(PSIn p) : SV_TARGET
{
    return float4(ApplyThreePoint(p.normal, p.color.rgb), p.color.a);
}
