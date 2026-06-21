// CompositePS.hlsl — final post-process resolve into the LDR swap-chain buffer.
// Adds the blurred bloom (t1, half-res, bilinearly upsampled by the sampler) to
// the HDR scene (t0), applies a simple exposure + tone-map, and writes sRGB-ish
// gamma. This is the one pass that converts HDR -> displayable LDR.

Texture2D    g_hdr   : register(t0);
Texture2D    g_bloom : register(t1);
SamplerState g_samp  : register(s0);

cbuffer Params : register(b0)
{
    float g_threshold; // unused here
    float g_intensity; // bloom add strength
    float g_texelX;    // unused here
    float g_texelY;    // unused here
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSIn p) : SV_TARGET
{
    float3 hdr   = g_hdr.Sample(g_samp, p.uv).rgb;
    float3 bloom = g_bloom.Sample(g_samp, p.uv).rgb;

    // Additive glow over the scene. The base image is passed through unchanged
    // (saturate only clamps >1, which the LDR back buffer did anyway), so the
    // approved scene tone is preserved and the bloom adds a halo on top.
    float3 c = saturate(hdr) + bloom * g_intensity;
    return float4(saturate(c), 1.0);
}
