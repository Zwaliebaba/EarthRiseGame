// SceneVS.hlsl — 3D scene vertex shader (SM6, instanced geometry).
//
// Root constants at b0 carry the 4x4 viewProj matrix (16 floats). The cbuffer
// is HLSL-default COLUMN-MAJOR, and the C++ side uploads `view*proj` row-major
// WITHOUT transposing: the column-major load reinterprets those bytes as the
// transpose, which is exactly what the column-vector `mul(viewProj, worldPos)`
// below needs. (Do not also transpose on the C++ side — that cancels out.)
// Per-instance world transform arrives via a second vertex buffer stream:
// four float4 rows + one float4 emissive color (80 bytes per instance).

cbuffer PerFrame : register(b0)
{
    float4x4 viewProj;  // column-major; receives un-transposed view*proj from C++
};

struct VSIn
{
    // Stream 0: per-vertex geometry
    float3 pos    : POSITION;
    float3 normal : NORMAL;
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
};

VSOut main(VSIn v)
{
    // Reconstruct the world matrix from per-instance row vectors.
    // float4x4(a,b,c,d) sets rows, so this is a row-major construction —
    // mul(world, pos) = world * pos (standard linear algebra).
    float4x4 world = float4x4(v.w0, v.w1, v.w2, v.w3);
    float4 worldPos = mul(world, float4(v.pos, 1.0));

    VSOut o;
    o.pos   = mul(viewProj, worldPos);
    // Upper-left 3x3 gives the rotation+scale part; normalising in the PS
    // removes the scale, leaving the correct surface normal direction.
    o.normal = mul((float3x3)world, v.normal);
    o.color  = v.color;
    return o;
}
