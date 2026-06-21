// SceneTexPS.hlsl — textured 3D scene pixel shader.
// Samples the material diffuse (t0) and lights it with the same single
// directional + ambient term as ScenePS. The per-instance colour is treated as
// a tint (so an entity can still be coloured), defaulting to white-ish art when
// the tint is bright.

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
    float3 L     = normalize(float3(0.3f, 1.0f, 0.5f));
    float  ndotl = saturate(dot(normalize(p.normal), L));
    float3 albedo = g_diffuse.Sample(g_samp, p.uv).rgb;
    // Darwinia look: dark ambient + bright diffuse highlight.
    float3 lit = albedo * (0.12f + 0.88f * ndotl);
    return float4(lit, 1.0f);
}
