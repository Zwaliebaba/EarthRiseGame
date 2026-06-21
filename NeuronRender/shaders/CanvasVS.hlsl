// CanvasVS.hlsl — 2D canvas vertex shader for the HUD/UI pass (SM6).
// Converts screen-pixel positions to NDC; passes UV + tint through.

cbuffer PerPass : register(b0)
{
    float2 invScreenSize; // 1/width, 1/height
    float  mode;          // 0 solid, 1 textured-linear, 2 textured-point
    float  _pad;
};

struct VSIn
{
    float2 pos   : POSITION;
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
    // Pixel → NDC: [0, W] x [0, H]  →  [-1, +1] x [+1, -1]
    float2 ndc = v.pos * invScreenSize * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    VSOut o;
    o.pos   = float4(ndc, 0.0f, 1.0f);
    o.uv    = v.uv;
    o.color = v.color;
    return o;
}
