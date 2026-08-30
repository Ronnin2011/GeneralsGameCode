// Ronin @feature 25/08/2026 DX9: §29i.4 — tree pixel shader. Stages 2 (FFP parity),
// 3 (shadow receiver) and 4 (cloud map). Stage 2 was verified pixel-identical first.
//
// THE FFP COMBINE THIS REPLACES, as surveyed 25/08/2026:
//   stage0  Set_Shader(detailAlphaShader), GRADIENT_MODULATE + TEXTURING_ENABLE:
//             RGB = tex0.rgb * diffuse.rgb      A = tex0.a * diffuse.a
//   stage1  overwritten by W3DShaderManager::setShroudTex(1):
//             RGB = shroud.rgb * CURRENT.rgb    A = SELECTARG2 -> CURRENT.a, passthru
//   then ALPHATEST ref 0x60 GREATEREQUAL, no blend (SRCBLEND_ONE / DSTBLEND_ZERO).
//
// ALPHA IS LOAD-BEARING. Trees are alpha-tested billboards; if this stops producing
// meaningful alpha every tree becomes a solid card. Stage 1's ALPHAOP was a passthru,
// so alpha must be tex0.a * diffuse.a and the shroud must NOT touch it.
//
// SHROUD IS OPTIONAL. setShroudTex returns FALSE when TheTerrainRenderObject has no
// shroud, leaving s1 at the NULL set by Set_Texture(1, nullptr). The FFP survived that
// because stage 1 kept its DISABLE op; a PS would sample NULL and render black. Hence
// the c0.x gate, which the caller sets from setShroudTex's return value.
//
// DEPTH PASS SAFE. Trees also draw in the shadow depth pass because they cast. Colour
// writes are off there, so only alpha matters, and alpha is written before every branch.
// The caller MUST clear c16.x in the depth pass — the map is the render target there and
// reading a bound surface is undefined.
//
// Compile with: fxc /T ps_3_0 /Fo Trees.pso Trees_ps.hlsl

sampler2D g_DiffuseSampler : register(s0);   // tree texture atlas
sampler2D g_ShroudSampler  : register(s1);   // shroud, UV from the VS (c32/c33), not texgen
sampler2D g_CloudSampler   : register(s2);   // terrain cloud field, UV from the VS
sampler2D g_ShadowSampler  : register(s3);   // s3 to match RigidInstance_ps

float4 g_ShroudParams : register(c0);        // x = shroud bound (0 or 1)
float4 g_CloudParams  : register(c1);        // x = cloud enabled (0 or 1)
// c12..c17 mirror RigidInstance_ps exactly so the receiver block stays comparable.
float4x4 g_LightViewProj  : register(c12);   // TRANSPOSED on upload (§29d)
float4 g_ShadowParams     : register(c16);   // x = enable, y = depth bias, zw = half-texel offset
float4 g_ShadowParams2    : register(c17);   // xyz = direction light TRAVELS, w = texel world size

struct PS_INPUT
{
    float4 diffuse  : COLOR0;
    float2 uv0      : TEXCOORD0;
    float2 uv1      : TEXCOORD1;
    float3 worldPos : TEXCOORD2;   // shadow receiver
    float2 uv2      : TEXCOORD3;   // cloud projection
};

float4 main(PS_INPUT input) : COLOR0
{
    float4 texel = tex2D(g_DiffuseSampler, input.uv0);

    float4 color;
    color.rgb = texel.rgb * input.diffuse.rgb;   // stage 0 MODULATE
    color.a   = texel.a   * input.diffuse.a;     // stage 0 MODULATE, feeds the 0x60 test

    if (g_ShroudParams.x > 0.0f)
    {
        color.rgb *= tex2D(g_ShroudSampler, input.uv1).rgb;   // stage 1 MODULATE, RGB only
    }

    // Ronin @feature 25/08/2026 DX9: §29i.4 stage 4. Cloud, before the shadow — same order as
    // RigidInstance_ps. Gated: s2 is NULL when the cloud map is off.
    if (g_CloudParams.x > 0.0f)
    {
        color.rgb *= tex2D(g_CloudSampler, input.uv2).rgb;
    }

    // Ronin @feature 25/08/2026 DX9: §29i.4 stage 3. Receiver, ported from RigidInstance_ps.
    // NO N.L FADE AND NO SLOPE TERM. That block needs a real vertex normal; trees have none —
    // the NORMAL slot carries packed sway data. A synthetic normal would make ndotl a per-frame
    // constant, so the fade would dim every tree uniformly as the sun drops. The light-space
    // offset below and the depth bias do the anti-acne work instead.
    // Caller must clear c16.x during the depth pass — the map is the render target there.
    if (g_ShadowParams.x > 0.0f)
    {
        // Ronin @tweak 25/08/2026 DX9: offset along the LIGHT, not +Z. A billboard is near-parallel
        // to a high sun — the worst case for depth bias — and +Z pushed the sample INTO its own
        // canopy. Reduces sway-driven self-shadow shimmer; raising it trades that for peter-panning.
        const float LIGHT_OFFSET_TEXELS = 4.0f;
        float3 toLight = -g_ShadowParams2.xyz;
        float3 samplePos = input.worldPos + toLight * (g_ShadowParams2.w * LIGHT_OFFSET_TEXELS);

        float4 lightClip  = mul(float4(samplePos, 1.0f), g_LightViewProj);
        float2 shadowUV   = lightClip.xy * float2(0.5f, -0.5f) + 0.5f + g_ShadowParams.zw;
        float  lightDepth = lightClip.z - g_ShadowParams.y;

        // Outside the fitted map = lit.
        if (shadowUV.x == saturate(shadowUV.x) && shadowUV.y == saturate(shadowUV.y))
        {
            float texelUV = g_ShadowParams.z * 2.0f;
            float sum = 0.0f;
            [unroll]
            for (int y = -1; y <= 1; ++y)
            {
                [unroll]
                for (int x = -1; x <= 1; ++x)
                {
                    float2 uv = shadowUV + float2(x, y) * texelUV;
                    sum += tex2Dproj(g_ShadowSampler, float4(uv, lightDepth, 1.0f)).r;
                }
            }
            color.rgb *= lerp(0.45f, 1.0f, sum * (1.0f / 9.0f));
        }
    }

    return color;
}
