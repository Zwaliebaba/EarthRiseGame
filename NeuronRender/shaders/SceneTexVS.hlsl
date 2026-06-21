// SceneTexVS.hlsl — textured 3D scene vertex shader (SM6, instanced).
//
// Same transform as SceneVS (see that file for the column-major-cbuffer note),
// plus it passes the per-vertex UV (CMO vertex offset 44 → TEXCOORD4) through to
// the pixel shader for diffuse sampling. Per-instance world rows arrive on
// stream 1 as TEXCOORD0-3 + COLOR0.

cbuffer PerFrame : register(b0)
{
    float4x4 viewProj; // column-major; receives un-transposed view*proj from C++
};

struct VSIn
{
    // Stream 0: per-vertex geometry (CMO layout: pos@0, normal@12, uv@44)
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD4;
    // Stream 1: per-instance (world matrix rows + color)
    float4 w0     : TEXCOORD0;
    float4 w1     : TEXCOORD1;
    float4 w2     : TEXCOORD2;
    float4 w3     : TEXCOORD3;
    float4 color  : COLOR;
};

struct VSOut
{
    float4 pos    : SV_POSITION;
    float3 normal : TEXCOORD0;
    float4 color  : TEXCOORD1;
    float2 uv     : TEXCOORD2;
};

VSOut main(VSIn v)
{
    float4x4 world = float4x4(v.w0, v.w1, v.w2, v.w3);
    float4 worldPos = mul(world, float4(v.pos, 1.0));

    VSOut o;
    o.pos    = mul(viewProj, worldPos);
    o.normal = mul((float3x3)world, v.normal);
    o.color  = v.color;
    o.uv     = v.uv;
    return o;
}
