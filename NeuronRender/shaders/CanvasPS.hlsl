// CanvasPS.hlsl — 2D canvas pixel shader.
//   mode 0  solid              → tint only
//   mode 1  textured (linear)  → chrome gradient strip × tint
//   mode 2  textured (point)   → font atlas glyph (white+alpha) × tint
// The per-vertex colour is a tint multiplied onto the sampled texel.

cbuffer PerPass : register(b0)
{
    float2 invScreenSize;
    float  mode;
    float  _pad;
};

Texture2D    g_tex    : register(t0);
SamplerState g_linear : register(s0);
SamplerState g_point  : register(s1);

struct PSIn
{
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR;
};

float4 main(PSIn p) : SV_TARGET
{
    if (mode < 0.5f)
        return p.color;                       // solid

    float4 texel = (mode < 1.5f) ? g_tex.Sample(g_linear, p.uv)
                                 : g_tex.Sample(g_point, p.uv);
    return texel * p.color;
}
