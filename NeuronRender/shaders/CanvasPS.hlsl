// CanvasPS.hlsl — 2D canvas pixel shader.
// M1b: solid-color quads.
// M2: texture lookup in a fixed-grid monospace font atlas.

struct PSIn
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
};

float4 main(PSIn p) : SV_TARGET
{
    return p.color;
}
