// ParticlePS.hlsl — sample the glow sprite × per-particle colour. The PSO uses
// additive blending (src.a, ONE), so transparent texels add nothing and bright
// cores accumulate — feeding the HDR bloom for the Darwinia glow.

Texture2D    g_tex  : register(t0);
SamplerState g_samp : register(s0);

struct PSIn
{
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR;
};

float4 main(PSIn p) : SV_TARGET
{
    return g_tex.Sample(g_samp, p.uv) * p.color;
}
