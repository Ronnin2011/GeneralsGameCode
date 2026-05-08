//
// Live per-material weighted terrain splat shader.
// Supports both single-page terrain atlases and multi-page atlas accumulation.
//
// Resource layout:
//   s0       = current terrain atlas page
//   s1..s8   = weight atlas pages 0..7
//   s9       = cloud texture (optional)
//   s10      = light/noise texture (optional)
//   s11      = low-frequency macro variation texture (optional, Route 2 / Appendix C.2)
//
// Constant layout:
//   c0       = (uvScaleX, uvScaleY, uvOriginX, uvOriginY)
//   c1..c32  = g_atlasRegionA[32] -- (originU, originV, extentU, extentV)
//   c33..c64 = g_atlasRegionB[32] -- (invPeriodU, invPeriodV, 0, 0)
//   c65      = (numActiveMaterials, 0, 0, 0)
//   c66      = g_noiseParams0 = (cloudScaleX, cloudScaleY, cloudOffsetX, cloudOffsetY)
//   c67      = g_noiseParams1 = (lightScaleX, lightScaleY, useCloud, useLight)
//   c68..c75 = g_slotEnableMask[8] -- 32 floats, one enable value per active slot
//   c76      = g_variationParams = (varFreqX, varFreqY, varStrength, 0)
//                varStrength = 0 disables the variation lookup entirely.
//
// Weight atlas channel mapping:
//   page p, slot s = p*4 + c, where c in {0..3}:
//     c == 0 -> dst byte 0 (B)  -> HLSL .b
//     c == 1 -> dst byte 1 (G)  -> HLSL .g
//     c == 2 -> dst byte 2 (R)  -> HLSL .r
//     c == 3 -> dst byte 3 (A)  -> HLSL .a
//
// Reuses terrainprimarycontrol_vs.vso unchanged.

#define SPLAT_MAX_ACTIVE_MATERIALS 32
#define MAX_S20_WEIGHT_ATLAS_PAGES 8

// 0 = production
// 1 = winning-slot heatmap
// 2 = sum-of-weights
// 3 = worldXY-continuity probe (frac of worldXY * 0.05 in R/G)
#define DEBUG_VIZ_MODE 0

float4 g_controlParams : register(c0);
float4 g_atlasRegionA[SPLAT_MAX_ACTIVE_MATERIALS] : register(c1);
float4 g_atlasRegionB[SPLAT_MAX_ACTIVE_MATERIALS] : register(c33);
float4 g_activeCount : register(c65);
float4 g_noiseParams0 : register(c66);
float4 g_noiseParams1 : register(c67);
float4 g_slotEnableMask[8] : register(c68);
float4 g_variationParams : register(c76);

sampler2D atlasSampler : register(s0);
sampler2D weightPage0 : register(s1);
sampler2D weightPage1 : register(s2);
sampler2D weightPage2 : register(s3);
sampler2D weightPage3 : register(s4);
sampler2D weightPage4 : register(s5);
sampler2D weightPage5 : register(s6);
sampler2D weightPage6 : register(s7);
sampler2D weightPage7 : register(s8);
sampler2D cloudSampler : register(s9);
sampler2D lightSampler : register(s10);
sampler2D variationSampler : register(s11);

struct PSInput
{
    float4 color : COLOR0;
    float2 uvBase : TEXCOORD0;
    float2 uvBlend : TEXCOORD1;
    float2 worldXY : TEXCOORD2;
};

float slotEnabled(int slot)
{
    int vecIdx = slot / 4;
    int comp = slot - vecIdx * 4;
    float4 maskVec = g_slotEnableMask[vecIdx];

    if (comp == 0)
        return maskVec.x;
    if (comp == 1)
        return maskVec.y;
    if (comp == 2)
        return maskVec.z;
    return maskVec.w;
}

// @feature Ronin 03/05/2026 Splat S20 / Appendix C.5: cheap deterministic value-noise
// used to perturb world-XY before per-slot atlas sampling so the source tile's natural
// content does not visibly repeat on a regular world-space grid. Same algebra as the
// CPU-side splat_value_noise / splat_hash2 helpers in WorldHeightMap.cpp, ported to PS.
float splat_hash2(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

float splat_value_noise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 w = f * f * (3.0f - 2.0f * f);
    float a = splat_hash2(i);
    float b = splat_hash2(i + float2(1.0f, 0.0f));
    float c = splat_hash2(i + float2(0.0f, 1.0f));
    float d = splat_hash2(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, w.x), lerp(c, d, w.x), w.y);
}

float3 sampleAtlasForSlot(int slot, float2 worldXY, float2 dWorldXYdx, float2 dWorldXYdy)
{
    float4 regA = g_atlasRegionA[slot];
    float4 regB = g_atlasRegionB[slot];

    // @bugfix Ronin 05/05/2026 Splat S20: reverted from triangle-wave wrap back to plain
    // sawtooth frac(). The triangle wave eliminated the per-tile-period seam line but
    // introduced a much more obvious mirror artifact (every other tile reflected across
    // the wrap axis -- the eye picks up bilateral symmetry instantly, far more than a
    // straight seam line). The proper fix for tile-period seams is either authored
    // seamless source art or the low-frequency variation map (g_variationParams in
    // applyTerrainNoiseLayers below); not a PS-side wrap trick on non-seamless tiles.
    float2 uvScale = regB.xy * regA.zw;
    float2 dudx = dWorldXYdx * uvScale;
    float2 dudy = dWorldXYdy * uvScale;

    float2 fracUV = frac(worldXY * regB.xy);

    // Small inset of the [0,1] sample range so that bilinear (and especially mip > 0
    // bilinear) footprints never reach the slot's atlas-region edge. Cheap belt-and-
    // braces against per-slot atlas-region bleed at very oblique camera angles.
    const float kEdgeInset = 0.008f;
    fracUV = lerp(kEdgeInset.xx, (1.0f - kEdgeInset).xx, fracUV);

    float2 uv = regA.xy + fracUV * regA.zw;
    return tex2Dgrad(atlasSampler, uv, dudx, dudy).rgb;
}

float4 sampleWeightPage(int page, float2 controlUV)
{
    if (page == 0)
        return tex2D(weightPage0, controlUV);
    if (page == 1)
        return tex2D(weightPage1, controlUV);
    if (page == 2)
        return tex2D(weightPage2, controlUV);
    if (page == 3)
        return tex2D(weightPage3, controlUV);
    if (page == 4)
        return tex2D(weightPage4, controlUV);
    if (page == 5)
        return tex2D(weightPage5, controlUV);
    if (page == 6)
        return tex2D(weightPage6, controlUV);
    return tex2D(weightPage7, controlUV);
}

float weightForSlot(int slot, float2 controlUV)
{
    int page = slot / 4;
    int chan = slot - page * 4;
    float4 s = sampleWeightPage(page, controlUV);

    if (chan == 0)
        return s.b;
    if (chan == 1)
        return s.g;
    if (chan == 2)
        return s.r;
    return s.a;
}

float3 applyTerrainNoiseLayers(float3 color, float2 worldXY)
{
    if (g_noiseParams1.z > 0.5f)
    {
        float2 cloudUV = worldXY * g_noiseParams0.xy + g_noiseParams0.zw;
        color *= tex2D(cloudSampler, cloudUV).rgb;
    }

    if (g_noiseParams1.w > 0.5f)
    {
        float2 lightUV = worldXY * g_noiseParams1.xy;
        color *= tex2D(lightSampler, lightUV).rgb;
    }

    // @feature Ronin 05/05/2026 Splat S20 / Appendix C.2: low-frequency macro variation
    // map. Sampled at a much lower frequency than the per-slot atlas tile so its own
    // wrap period is far larger than any one camera view -- breaks the eye's ability to
    // spot identical tile micro-content without introducing any new geometric artifact
    // (no swirl, no mirror, no seam). Mid-gray (0.5) = neutral; brighter areas brighten
    // the underlying terrain, darker areas darken it. Strength == 0 makes this a no-op,
    // which is the default when TSTerrainVariation.tga is not present on disk.
    if (g_variationParams.z > 0.001f)
    {
        float2 varUV = worldXY * g_variationParams.xy;
        float3 variation = tex2D(variationSampler, varUV).rgb;
        variation = lerp(float3(1.0f, 1.0f, 1.0f), variation * 2.0f, g_variationParams.z);
        color *= variation;
    }

    return color;
}

float4 main(PSInput input) : COLOR0
{
    float2 controlUV = (input.worldXY - g_controlParams.zw) * g_controlParams.xy;
    int activeCount = (int) g_activeCount.x;

    float2 dWorldXYdx = ddx(input.worldXY);
    float2 dWorldXYdy = ddy(input.worldXY);

    float3 color = float3(0, 0, 0);

#if DEBUG_VIZ_MODE == 2
    float wSum = 0.0f;
#endif
#if DEBUG_VIZ_MODE == 1
    int winnerSlot = 0;
    float winnerW = -1.0f;
#endif

    for (int s = 0; s < SPLAT_MAX_ACTIVE_MATERIALS; ++s)
    {
        if (s < activeCount)
        {
            float enabled = slotEnabled(s);
            if (enabled > 0.5f)
            {
                float w = weightForSlot(s, controlUV);

#if DEBUG_VIZ_MODE == 2
                wSum += w;
#endif
#if DEBUG_VIZ_MODE == 1
                if (w > winnerW) {
                    winnerW = w;
                    winnerSlot = s;
                }
#endif

                if (w > (1.0f / 255.0f))
                {
                    color += w * sampleAtlasForSlot(s, input.worldXY, dWorldXYdx, dWorldXYdy);
                }
            }
        }
    }

#if DEBUG_VIZ_MODE == 1
    float h = (float)winnerSlot / max(1.0f, (float)activeCount);
    return float4(h, 1.0f - h, frac(h * 7.13f), 1.0f);
#elif DEBUG_VIZ_MODE == 2
    return float4(saturate(1.0f - wSum), saturate(wSum), 0.0f, 1.0f);
#elif DEBUG_VIZ_MODE == 3
    // worldXY continuity probe. Should show smooth 0->1 sawtooth ramps with snap lines
    // at the wrap boundaries -- expected with sawtooth wrap. If the snap pattern looks
    // wrong, worldXY itself has discontinuities (different bug, look in VS / VB writes).
    float2 phase = frac(input.worldXY * 0.05f);
    return float4(phase.x, phase.y, 0.0f, 1.0f);
#else
    float3 finalColor = color * input.color.rgb;
    finalColor = applyTerrainNoiseLayers(finalColor, input.worldXY);
    return float4(saturate(finalColor), 1.0f);
#endif
}
