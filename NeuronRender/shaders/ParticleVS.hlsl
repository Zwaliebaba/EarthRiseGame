// ParticleVS.hlsl — additive billboard particles (SM6).
// Quads are expanded on the CPU into camera-facing world-space vertices, so the
// VS just projects them. viewProj uses the same column-major-cbuffer convention
// as SceneVS (C++ uploads view*proj un-transposed).

cbuffer PerFrame : register(b0)
{
    float4x4 viewProj;
};

struct VSIn
{
    float3 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR;
};

struct VSOut
{
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR;
};

VSOut main(VSIn v)
{
    VSOut o;
    o.pos   = mul(viewProj, float4(v.pos, 1.0));
    o.uv    = v.uv;
    o.color = v.color;
    return o;
}
