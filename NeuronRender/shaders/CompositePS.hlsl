// CompositePS.hlsl — final post-process resolve into the LDR swap-chain buffer.
// Adds the blurred bloom (t1, half-res, bilinearly upsampled) to the HDR scene
// (t0), applies exposure + an ACES filmic tone-map (so over-bright bloom rolls
// off instead of clipping), then a gentle vignette + faint scanlines for the
// Darwinia CRT-ish frame. This pass converts HDR -> displayable LDR.

Texture2D    g_hdr   : register(t0);
Texture2D    g_bloom : register(t1);
SamplerState g_samp  : register(s0);

cbuffer Params : register(b0)
{
    float g_exposure;      // scene exposure multiplier
    float g_bloomIntensity; // bloom add strength
    float g_vignette;      // 0..1 vignette darkening at the corners
    float g_scanline;      // 0..1 scanline depth
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// ACES filmic tone-map (Narkowicz approximation).
float3 ACESFilm(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 main(PSIn p) : SV_TARGET
{
    float3 hdr   = g_hdr.Sample(g_samp, p.uv).rgb;
    float3 bloom = g_bloom.Sample(g_samp, p.uv).rgb;

    float3 col = ACESFilm(hdr * g_exposure + bloom * g_bloomIntensity);

    // Vignette: 1 at centre, darker toward the corners.
    float2 q = p.uv - 0.5;
    float vig = smoothstep(0.90, 0.35, length(q));
    col *= lerp(1.0, vig, g_vignette);

    // Faint scanlines (subtle; resolution-relative).
    float sl = 1.0 - g_scanline * (0.5 + 0.5 * sin(p.uv.y * 1400.0));
    col *= sl;

    return float4(col, 1.0);
}
