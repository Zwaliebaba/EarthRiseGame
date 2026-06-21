// BlurPS.hlsl — separable 9-tap Gaussian blur.
// Run twice per bloom level: once horizontal, once vertical. The blur direction
// and the source texel size arrive in the params cbuffer (g_texelX/Y carry the
// per-axis step; the inactive axis is zero).

Texture2D    g_src  : register(t0);
SamplerState g_samp : register(s0);

cbuffer Params : register(b0)
{
    float g_threshold; // unused here
    float g_intensity; // unused here
    float g_texelX;    // blur step on X (0 for the vertical pass)
    float g_texelY;    // blur step on Y (0 for the horizontal pass)
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSIn p) : SV_TARGET
{
    // Normalised 9-tap Gaussian (sigma ~2).
    const float w[5] = { 0.227027, 0.194595, 0.121622, 0.054054, 0.016216 };
    float2 dir = float2(g_texelX, g_texelY);

    float3 acc = g_src.Sample(g_samp, p.uv).rgb * w[0];
    [unroll] for (int i = 1; i < 5; ++i)
    {
        float2 off = dir * (float)i;
        acc += g_src.Sample(g_samp, p.uv + off).rgb * w[i];
        acc += g_src.Sample(g_samp, p.uv - off).rgb * w[i];
    }
    return float4(acc, 1.0);
}
