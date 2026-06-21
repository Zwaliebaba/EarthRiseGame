// FullscreenVS.hlsl — generates a single screen-covering triangle from the
// vertex id (no vertex/index buffer bound). Shared by every post-process pass.
//
//   id=0 -> (-1,-1) uv(0,0)   id=1 -> (-1, 3) uv(0,2)   id=2 -> ( 3,-1) uv(2,0)
//
// The oversized triangle is clipped to the viewport, covering the whole screen
// with no diagonal seam. UV origin is top-left (DirectX convention).

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);          // (0,0) (2,0) (0,2)
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
