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
    float3 N      = normalize(p.normal);
    float3 L      = normalize(float3(0.3f, 1.0f, 0.5f));
    float3 albedo = g_diffuse.Sample(g_samp, p.uv).rgb;

    // Half-Lambert wrap: a hard N.L leaves every camera-facing surface (most of
    // what we see) at the ambient floor, so the textured hull reads near-black.
    // Wrapping maps N.L from [-1,1] to [0,1] so angled surfaces still catch the
    // key light, and a generous ambient keeps the dark side readable.
    float wrap = dot(N, L) * 0.5f + 0.5f;
    float3 lit = albedo * (0.35f + 0.9f * wrap);
    return float4(lit, 1.0f);
}
