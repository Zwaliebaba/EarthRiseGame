// ScenePS.hlsl — 3D scene pixel shader.
// Single directional light with ambient term; emissive tint from per-instance color.
// M2 adds bloom pre-pass and additive particles.

struct PSIn
{
    float4 pos    : SV_POSITION;
    float3 normal : TEXCOORD0;
    float4 color  : TEXCOORD1;
};

float4 main(PSIn p) : SV_TARGET
{
    float3 L    = normalize(float3(0.3f, 1.0f, 0.5f));
    float  ndotl = saturate(dot(normalize(p.normal), L));
    // Darwinia look: dark ambient + bright emissive highlight.
    float3 lit  = p.color.rgb * (0.12f + 0.88f * ndotl);
    return float4(lit, p.color.a);
}
