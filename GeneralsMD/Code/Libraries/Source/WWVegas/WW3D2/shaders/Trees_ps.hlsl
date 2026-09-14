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
sampler2D g_ShadowSamplerFar : register(s5); // §29i.3 step 2 — far cascade (s4 is the accum history)
// Ronin @feature 07/09/2026 DX9: §29j.13n. Last frame's tree shadow term, screen-space. Alpha is the
// validity mask: 0 means no canopy wrote that pixel, so there is nothing to blend.
sampler2D g_TreeAccumPrev  : register(s4);

float4 g_ShroudParams : register(c0);        // x = shroud bound (0 or 1)
float4 g_CloudParams  : register(c1);        // x = cloud enabled (0 or 1)
// c12..c17 mirror RigidInstance_ps exactly so the receiver block stays comparable.
float4x4 g_LightViewProj  : register(c12);   // TRANSPOSED on upload (§29d)
float4 g_ShadowParams     : register(c16);   // x = enable, y = depth bias, zw = half-texel offset
float4 g_ShadowParams2    : register(c17);   // xyz = direction light TRAVELS, w = texel world size
// §29j.13n accumulation.
float4x4 g_PrevViewProj   : register(c18);   // c18..c21, TRANSPOSED
float4 g_ScreenInvSize    : register(c22);   // xy = 1/render-target size, z = EMA weight, w unused
float4 g_ViewportRect     : register(c23);   // xy = viewport origin, zw = viewport size, target pixels
// Ronin @feature 09/09/2026 DX9: §29i.3 step 2. c24+ deliberately — c18..c23 are already shared with
// RigidInstance_ps, and PS constants are global device state, not per-shader.
float4x4 g_LightViewProjFar : register(c24); // c24..c27, TRANSPOSED
float4 g_CascadeParams : register(c28);      // x = far bias, y = far texel world size, z = cascade count

struct PS_INPUT
{
    float4 diffuse  : COLOR0;
    float2 uv0      : TEXCOORD0;
    float2 uv1      : TEXCOORD1;
    float3 worldPos : TEXCOORD2;   // shadow receiver
    float2 uv2      : TEXCOORD3;   // cloud projection
};

// §29j.13n: COLOR1 carries the shadow term for next frame. Alpha test still uses COLOR0's alpha, so a
// discarded canopy pixel writes to neither target — which is exactly what the validity mask needs.
struct PS_OUTPUT
{
    float4 color : COLOR0;
    float4 term  : COLOR1;
};

// 3x3 at one-texel spacing; each tap is a hardware 2x2 compare. Returns the term plus the
// history-rejection bracket (§29j.13j), floored so a one-texel canopy step keeps its history.
void pcf9(sampler2D smp, float2 uv, float depth, float texelUV,
          out float lit, out float litMin, out float litMax)
{
    float sum   = 0.0f;
    float sumSq = 0.0f;
    litMin = 1.0f;
    litMax = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 tapUV = uv + float2(x, y) * texelUV;
            float  t     = tex2Dproj(smp, float4(tapUV, depth, 1.0f)).r;
            sum   += t;
            sumSq += t * t;
            litMin = min(litMin, t);
            litMax = max(litMax, t);
        }
    }
    lit = sum * (1.0f / 9.0f);
    const float CLIP_GAMMA = 2.0f;
    // Ronin @bugfix 13/09/2026 DX9: §29i.3. 0.34, not 1/9 — a one-texel step moves a pixel ~1/3, and 1/9 threw that
    // history away, which is what let the canopy step. Same fix as TerrainShadow_ps.
    const float CLIP_FLOOR = 0.34f;
    float sd = sqrt(max(sumSq * (1.0f / 9.0f) - lit * lit, 0.0f));
    litMin = max(litMin, lit - CLIP_GAMMA * sd);
    litMax = min(litMax, lit + CLIP_GAMMA * sd);
    litMin = min(litMin, lit - CLIP_FLOOR);
    litMax = max(litMax, lit + CLIP_FLOOR);
}

PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT o;
    float4 texel = tex2D(g_DiffuseSampler, input.uv0);

    float4 color;
    color.rgb = texel.rgb * input.diffuse.rgb;   // stage 0 MODULATE
    color.a   = texel.a   * input.diffuse.a;     // stage 0 MODULATE, feeds the 0x60 test

    // §29j.13n: lit and INVALID until the receiver runs. Alpha is the validity mask the next frame
    // reads — trees cover a fraction of the screen, so "no history here" must be distinguishable.
    o.term = float4(1.0f, 1.0f, 1.0f, 0.0f);

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
    // the NORMAL slot carries packed sway data.
    // Caller must clear c16.x during the depth pass — the map is the render target there.
    if (g_ShadowParams.x > 0.0f)
    {
        // Ronin @tweak 25/08/2026 DX9: offset along the LIGHT, not +Z. A billboard is near-parallel
        // to a high sun — the worst case for depth bias — and +Z pushed the sample INTO its own
        // canopy. Reduces sway-driven self-shadow shimmer; raising it trades that for peter-panning.
        const float LIGHT_OFFSET_TEXELS = 4.0f;
        float3 toLight = -g_ShadowParams2.xyz;

        // Ronin @feature 09/09/2026 DX9: §29i.3 step 2. CASCADE SELECT from the UN-OFFSET position —
        // the light offset is scaled by that cascade's own texel size, so pick the cascade first.
        float4 selClip = mul(float4(input.worldPos, 1.0f), g_LightViewProj);
        float2 selUV   = selClip.xy * float2(0.5f, -0.5f) + 0.5f + g_ShadowParams.zw;
        // Ronin @bugfix 14/09/2026 DX9: §29i.3. Near only when SEAM_TEXELS inside the near map — the light offset (4 texels) and
        // the PCF left the map at its edge and read as lit: the bright seam.
        const float SEAM_TEXELS = 6.0f;             // LIGHT_OFFSET_TEXELS (4) + PCF radius (1) + the 2x2 compare, rounded up
        float  seamUV  = g_ShadowParams.z * 2.0f * SEAM_TEXELS;
        bool   inNear  = (g_CascadeParams.z < 1.5f)
                         || (selUV.x >= seamUV && selUV.x <= 1.0f - seamUV && selUV.y >= seamUV && selUV.y <= 1.0f - seamUV);
        float texelWorld = inNear ? g_ShadowParams2.w : g_CascadeParams.y;

        float3 samplePos = input.worldPos + toLight * (texelWorld * LIGHT_OFFSET_TEXELS);

        // Each cascade carries its OWN bias — a shared constant stripes the far split (doc §2).
        float4 clipN = mul(float4(samplePos, 1.0f), g_LightViewProj);
        float4 clipF = mul(float4(samplePos, 1.0f), g_LightViewProjFar);
        float2 uvN   = clipN.xy * float2(0.5f, -0.5f) + 0.5f + g_ShadowParams.zw;
        float2 uvF   = clipF.xy * float2(0.5f, -0.5f) + 0.5f + g_ShadowParams.zw;
        float2 shadowUV = inNear ? uvN : uvF;

        // Outside the fitted map = lit.
        if (shadowUV.x == saturate(shadowUV.x) && shadowUV.y == saturate(shadowUV.y))
        {
            float texelUV = g_ShadowParams.z * 2.0f;
            // fxc cannot branch around tex2Dproj (X3528), so both cascades are sampled — 18 taps.
            float litN, minN, maxN, litF, minF, maxF;
            pcf9(g_ShadowSampler,    uvN, clipN.z - g_ShadowParams.y,  texelUV, litN, minN, maxN);
            pcf9(g_ShadowSamplerFar, uvF, clipF.z - g_CascadeParams.x, texelUV, litF, minF, maxF);
            float lit    = inNear ? litN : litF;
            float litMin = inNear ? minN : minF;
            float litMax = inNear ? maxN : maxF;

            float shade    = lerp(0.45f, 1.0f, lit);
            float shadeMin = lerp(0.45f, 1.0f, litMin);
            float shadeMax = lerp(0.45f, 1.0f, litMax);

            // §29j.13l reprojection. NDC spans the VIEWPORT, the accum texture spans the RENDER
            // TARGET — convert through pixels, never straight from NDC to UV.
            float4 prevClip = mul(float4(input.worldPos, 1.0f), g_PrevViewProj);
            float2 prevVPuv = (prevClip.xy / prevClip.w) * float2(0.5f, -0.5f) + 0.5f;
            float2 histUV   = (g_ViewportRect.xy + prevVPuv * g_ViewportRect.zw) * g_ScreenInvSize.xy;

            float4 histT = tex2D(g_TreeAccumPrev, histUV);
            // Off-screen last frame, behind the eye, or no canopy wrote there = no history.
            float valid = (prevClip.w > 0.0f
                           && prevVPuv.x == saturate(prevVPuv.x)
                           && prevVPuv.y == saturate(prevVPuv.y)
                           && histT.a > 0.5f) ? 1.0f : 0.0f;

            float hist = clamp(histT.r, shadeMin, shadeMax);
            shade = lerp(shade, hist, g_ScreenInvSize.z * valid);

            color.rgb *= shade;
            o.term = float4(shade, shade, shade, 1.0f);
        }
    }

    o.color = color;
    return o;
}

