// BrightPassPS.hlsl — bloom bright-pass + downsample.
// Samples the HDR scene (t0) and keeps only the energy above a luminance
// threshold, with a soft knee so the bloom ramps in smoothly rather than
// popping. Run at half resolution (the viewport sets the downsample), so the
// bilinear sampler already averages a 2x2 footprint.

Texture2D    g_hdr  : register(t0);
SamplerState g_samp : register(s0);

cbuffer Params : register(b0)
{
    float g_threshold; // luminance below this is rejected
    float g_intensity; // unused here (composite uses it)
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
    float3 c = g_hdr.Sample(g_samp, p.uv).rgb;
    float  luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    // Soft-knee threshold: 0 below threshold, ramps over a small band above it.
    float  knee = 0.5;
    float  w = saturate((luma - g_threshold) / max(knee, 1e-4));
    return float4(c * w, 1.0);
}
