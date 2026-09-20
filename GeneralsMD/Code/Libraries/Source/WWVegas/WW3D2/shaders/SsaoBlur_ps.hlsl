// Ronin @feature 13/09/2026 DX9: SSAO step 3a. One axis of a depth-aware Gaussian blur over the half-resolution AO.
// A tap only counts if its depth is close to the centre's, so occlusion does not smear across object edges.
// Run twice: across, then down.
//
// Compile with: fxc /T ps_3_0 /Fo SsaoBlur.pso SsaoBlur_ps.hlsl   (pairs with ScreenQuad.vso)

sampler2D g_AO         : register(s0);      // AO in R, POINT, CLAMP
sampler2D g_SceneDepth : register(s1);      // INTZ, POINT, CLAMP

float4 g_BlurParams : register(c0);         // x = camera near, y = camera far, z = depth tolerance as a fraction of distance
float4 g_BlurStep   : register(c1);         // xy = one AO texel along the blur axis, in UV
float4 g_DepthTexel : register(c2);         // xy = 1 / depth texture size, zw = its size

static const float WEIGHTS[5] = { 0.2270270f, 0.1945946f, 0.1216216f, 0.0540541f, 0.0162162f };

// Ronin @bugfix 13/09/2026 DX9: SSAO step 3a. Read depth at ONE definite texel — AO texel centres sit exactly on depth
// texel borders, and point sampling there picks a different side on different rows (see SsaoRaw_ps).
float linearDepth(float2 uv)
{
    float2 uvT = (floor(uv * g_DepthTexel.zw + 0.25f) + 0.5f) * g_DepthTexel.xy;
    float d = tex2Dlod(g_SceneDepth, float4(uvT, 0.0f, 0.0f)).r;
    float n = g_BlurParams.x;
    float f = g_BlurParams.y;
    return (n * f) / max(f - d * (f - n), 0.0001f);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float eCentre   = linearDepth(uv);
    float tolerance = max(eCentre * g_BlurParams.z, 0.0001f);

    float sum  = tex2Dlod(g_AO, float4(uv, 0.0f, 0.0f)).r * WEIGHTS[0];
    float wsum = WEIGHTS[0];

    [loop]
    for (int i = 1; i < 5; ++i)
    {
        float2 offset = g_BlurStep.xy * (float)i;

        float2 uvA = uv + offset;
        float  wA  = WEIGHTS[i] * saturate(1.0f - abs(linearDepth(uvA) - eCentre) / tolerance);
        sum  += tex2Dlod(g_AO, float4(uvA, 0.0f, 0.0f)).r * wA;
        wsum += wA;

        float2 uvB = uv - offset;
        float  wB  = WEIGHTS[i] * saturate(1.0f - abs(linearDepth(uvB) - eCentre) / tolerance);
        sum  += tex2Dlod(g_AO, float4(uvB, 0.0f, 0.0f)).r * wB;
        wsum += wB;
    }

    float ao = sum / wsum;
    return float4(ao, ao, ao, 1.0f);
}
