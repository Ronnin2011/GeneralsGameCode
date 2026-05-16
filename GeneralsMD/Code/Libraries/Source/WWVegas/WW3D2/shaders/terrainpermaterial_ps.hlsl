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
//   s12..s15 = per-material normal atlas pages 0..3 (Normal-map N2)
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
//   c77      = g_normalParams = (numNormalPages, strength, 0, 0)
//   c78      = g_lightDir -- direction TOWARD the sun, world space
//   c79      = g_sunColor  (declared; unused in production -- N3.1 revision)
//   c80      = g_ambientCol (declared; unused in production -- N3.1 revision)
//   c81      = (reserved gap between N3 and N6)
//   c82      = g_cameraPosWorld = (x, y, z, 0) -- N6 POM
//   c83      = g_pomParams = (heightScale, fadeStart, fadeEnd, enableFlag) -- N6 POM
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
// 4 = normal-map preview (Normal-map N2; weighted-sum tangent-space normal as RGB)
// 5 = POM visualization (Normal-map N6; R = height field, G = |displacement_cUV|*1000, B = POM enabled)
#define DEBUG_VIZ_MODE 0

float4 g_controlParams : register(c0);
float4 g_atlasRegionA[SPLAT_MAX_ACTIVE_MATERIALS] : register(c1);
float4 g_atlasRegionB[SPLAT_MAX_ACTIVE_MATERIALS] : register(c33);
float4 g_activeCount : register(c65);
float4 g_noiseParams0 : register(c66);
float4 g_noiseParams1 : register(c67);
float4 g_slotEnableMask[8] : register(c68);
float4 g_variationParams : register(c76);
// @feature Ronin 10/05/2026 Normal-map N2: c77 = (numNormalPages, strength, 0, 0).
//   .x  > 0 => sample the normal atlas at all
//   .y      => Lambert-delta strength scalar (1.0 = full effect, 0 = no perturbation)
float4 g_normalParams : register(c77);

// @feature Ronin 11/05/2026 Normal-map N3: per-pixel terrain lighting constants.
//   c78 = direction TOWARD the primary terrain sun, world-space, unit length.
//   c79 = g_sunColor  (declared; pushed but not used in production after N3.1 revision)
//   c80 = g_ambientCol (declared; pushed but not used in production after N3.1 revision)
float4 g_lightDir : register(c78);
float4 g_sunColor : register(c79);
float4 g_ambientCol : register(c80);

// @feature Ronin 12/05/2026 Normal-map N6: POM constants.
// c81 is a gap (reserved between N3 and N6 additions; never written by the CPU path).
// g_cameraPosWorld: world-space camera position, pushed each frame from m_lastCameraPos
//   (BaseHeightMapRenderObjClass, cached in updateCenter). Used to compute per-pixel
//   view direction V for the UV-space raymarch.
// g_pomParams:
//   .x = heightScale (MAX world-XY offset at full depth; in MAP_XY_FACTOR units, 0.5..2.0)
//   .y = fadeStart   (world distance at which POM starts fading out; typically 200)
//   .z = fadeEnd     (world distance at which POM is fully off; typically 800)
//   .w = enableFlag  (>0.5 => run the raymarch; set to 0 by CPU when UseTerrainPOM=No
//                    OR when no _NRM assets are present on disk)
float4 g_cameraPosWorld : register(c82);
float4 g_pomParams : register(c83);

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
// @feature Ronin 10/05/2026 Normal-map N2: per-material normal atlas pages. One page
// per diffuse atlas page; same dimensions, same per-source-tile rectangles, so the
// existing g_atlasRegionA/B tables address these without modification.
sampler2D normalPage0 : register(s12);
sampler2D normalPage1 : register(s13);
sampler2D normalPage2 : register(s14);
sampler2D normalPage3 : register(s15);

struct PSInput
{
    float4 color : COLOR0;
    float2 uvBase : TEXCOORD0;
    float2 uvBlend : TEXCOORD1;
    float2 worldXY : TEXCOORD2;
    // @feature Ronin 11/05/2026 Normal-map N3: full xyz world position from VS.
    float3 worldPos : TEXCOORD3;
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
// content does not visibly repeat on a regular world-space grid.
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

    float2 uvScale = regB.xy * regA.zw;
    float2 dudx = dWorldXYdx * uvScale;
    float2 dudy = dWorldXYdy * uvScale;

    float2 fracUV = frac(worldXY * regB.xy);

    const float kEdgeInset = 0.008f;
    fracUV = lerp(kEdgeInset.xx, (1.0f - kEdgeInset).xx, fracUV);

    float2 uv = regA.xy + fracUV * regA.zw;
    return tex2Dgrad(atlasSampler, uv, dudx, dudy).rgb;
}

float4 sampleWeightPage(int page, float2 controlUV, float2 dControlUVdx, float2 dControlUVdy)
{
    if (page == 0)
        return tex2Dgrad(weightPage0, controlUV, dControlUVdx, dControlUVdy);
    if (page == 1)
        return tex2Dgrad(weightPage1, controlUV, dControlUVdx, dControlUVdy);
    if (page == 2)
        return tex2Dgrad(weightPage2, controlUV, dControlUVdx, dControlUVdy);
    if (page == 3)
        return tex2Dgrad(weightPage3, controlUV, dControlUVdx, dControlUVdy);
    if (page == 4)
        return tex2Dgrad(weightPage4, controlUV, dControlUVdx, dControlUVdy);
    if (page == 5)
        return tex2Dgrad(weightPage5, controlUV, dControlUVdx, dControlUVdy);
    if (page == 6)
        return tex2Dgrad(weightPage6, controlUV, dControlUVdx, dControlUVdy);
    return tex2Dgrad(weightPage7, controlUV, dControlUVdx, dControlUVdy);
}

float weightForSlot(int slot, float2 controlUV, float2 dControlUVdx, float2 dControlUVdy)
{
    int page = slot / 4;
    int chan = slot - page * 4;
    float4 s = sampleWeightPage(page, controlUV, dControlUVdx, dControlUVdy);

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

    // @feature Ronin 05/05/2026 Splat S20 / Appendix C.2: low-frequency macro variation map.
    if (g_variationParams.z > 0.001f)
    {
        float2 varUV = worldXY * g_variationParams.xy;
        float3 variation = tex2D(variationSampler, varUV).rgb;
        variation = lerp(float3(1.0f, 1.0f, 1.0f), variation * 2.0f, g_variationParams.z);
        color *= variation;
    }

    return color;
}

// @feature Ronin 10/05/2026 Normal-map N2: per-page sampler dispatch for the normal atlas.
float4 sampleNormalPage(int page, float2 uv, float2 dudx, float2 dudy)
{
    if (page == 0)
        return tex2Dgrad(normalPage0, uv, dudx, dudy);
    if (page == 1)
        return tex2Dgrad(normalPage1, uv, dudx, dudy);
    if (page == 2)
        return tex2Dgrad(normalPage2, uv, dudx, dudy);
    return tex2Dgrad(normalPage3, uv, dudx, dudy);
}

// @feature Ronin 10/05/2026 Normal-map N2: sample the per-material normal atlas for
// active slot `slot`. Returns unit-length tangent-space normal in [-1,1].
float3 sampleNormalForSlot(int slot, int terrainPage, float2 worldXY, float2 dWorldXYdx, float2 dWorldXYdy)
{
float4 regA = g_atlasRegionA[slot];
float4 regB = g_atlasRegionB[slot];

float2 uvScale = regB.xy * regA.zw;
float2 dudx = dWorldXYdx * uvScale;
float2 dudy = dWorldXYdy * uvScale;

float2 fracUV = frac(worldXY * regB.xy);
const float kEdgeInset = 0.008f;
    fracUV = lerp(kEdgeInset.xx, (1.0f - kEdgeInset).xx, fracUV);

float2 uv = regA.xy + fracUV * regA.zw;

float4 packed = sampleNormalPage(terrainPage, uv, dudx, dudy);

float3 n;
    n.x = packed.r * 2.0f - 1.0f;
    n.y = packed.g * 2.0f - 1.0f;
    n.z = packed.b * 2.0f - 1.0f;
    return
n;
}

// @feature Ronin 12/05/2026 Normal-map N6: weighted height sample for POM raymarch.
//
// Returns the blended terrain height [0,1] at (worldXY_h, controlUV_h), where
// height = weighted average of the A channel of normalPage0 across all enabled slots.
//
// IMPORTANT -- page-0 limitation (v1):
//   All slots, regardless of which diffuse/normal atlas page they live on, are sampled
//   from normalPage0. This is consistent with the existing `activeNormalPage = 0` in
//   sampleNormalForSlot() (N3 v1 limitation). For terrain page 0 passes this is exact.
//   For page 1+ passes, slots whose art lives on page 1+ sample from page 0 with
//   those slots' atlas-region UVs -- which addresses the wrong sub-rect in page 0 and
//   gives approximate height. In practice: most terrain maps have <= 8 active materials
//   (all on page 0), and materials without authored _NRM have A=1 (full-surface default)
//   baked in by ensurePerMaterialNormalAtlasTextures(), so the approximation is safe.
//   Fix: pass current terrainPage in g_normalParams.z (currently 0) and dispatch to the
//   correct normalPage sampler -- deferred as a follow-up to keep v1 scope small.
//
// Budget: 1 weight-atlas tap + up to activeCount normal-atlas taps per call.
// POM uses 16 linear + 4 binary calls = 20 total. With typical activeCount 4-8,
// that is 80-160 texture instructions for the whole POM path -- well within ps_3_0.
float sampleHeightWeighted(
    float2 worldXY_h,
    float2 controlUV_h,
    float2 dWorldXYdx_h,
    float2 dWorldXYdy_h,
    float2 dControlUVdx_h,
    float2 dControlUVdy_h,
    int activeCount_h)
{
    float weightedH = 0.0f;
    float totalWeight = 0.0f;

    [loop]
    for (int s = 0; s < SPLAT_MAX_ACTIVE_MATERIALS; ++s)
    {
        if (s < activeCount_h)
        {
            if (slotEnabled(s) >= 0.5f)
            {
                float w = weightForSlot(s, controlUV_h, dControlUVdx_h, dControlUVdy_h);
                if (w > (1.0f / 255.0f))
                {
                    float4 regA = g_atlasRegionA[s];
                    float4 regB = g_atlasRegionB[s];

                    float2 uvScale = regB.xy * regA.zw;
                    float2 dudx = dWorldXYdx_h * uvScale;
                    float2 dudy = dWorldXYdy_h * uvScale;

                    const float kEdgeInset = 0.008f;
                    float2 fracUV = frac(worldXY_h * regB.xy);
                    fracUV = lerp(kEdgeInset.xx, (1.0f - kEdgeInset).xx, fracUV);
                    float2 uv = regA.xy + fracUV * regA.zw;

                    float h = tex2Dgrad(normalPage0, uv, dudx, dudy).a;
                    weightedH += w * h;
                    totalWeight += w;
                }
            }
        }
    }

    return (totalWeight > 0.001f) ? weightedH / totalWeight : 1.0f;
}


float4 main(PSInput input) : COLOR0
{
    float2 controlUV = (input.worldXY - g_controlParams.zw) * g_controlParams.xy;
    int activeCount = (int) g_activeCount.x;

    float2 dWorldXYdx = ddx(input.worldXY);
    float2 dWorldXYdy = ddy(input.worldXY);

    float2 dControlUVdx = dWorldXYdx * g_controlParams.xy;
    float2 dControlUVdy = dWorldXYdy * g_controlParams.xy;

    // @feature Ronin 12/05/2026 Normal-map N6: Parallax Occlusion Mapping.
    //
    // UV displacement is computed BEFORE any albedo or normal sampling so that all
    // downstream samples (diffuse, normal, weights) use the displaced surface position.
    // The displacement is in world-XY space and exactly mirrors the controlUV offset
    // (controlUV = (worldXY - zw) * xy, so delta_cUV = delta_world * xy).
    //
    // Algorithm (doc N6.2):
    //   1. Compute view direction V (camera -> surface) in world space.
    //   2. Project onto terrain plane: maxOffset_world = -V.xy / |V.z| * heightScale.
    //   3. Convert to controlUV space.  Apply distance fade factor.
    //   4. Linear search (16 steps): walk from surface (height=1) toward depth (height=0)
    //      until the height field crosses the ray.  Each step samples sampleHeightWeighted.
    //   5. Binary refinement (4 iterations): bisect the crossing interval for sub-step
    //      precision without extra linear steps.
    //   6. Use (displacedControlUV, displacedWorldXY) for all subsequent sampling.
    //
    // Gate: g_pomParams.w > 0.5 (set to 0 by CPU when UseTerrainPOM=No OR normalPageCount==0).
    // Distance fade: full strength within g_pomParams.y (fadeStart), zero beyond g_pomParams.z
    // (fadeEnd).  Saves ~80 ALU and all texture taps on far terrain.

    float2 displacedControlUV = controlUV;
    float2 displacedWorldXY = input.worldXY;

    // @bugfix Ronin 12/05/2026 Normal-map N6: NO `[branch]` here. fxc rejects forced
    // dynamic branches that wrap gradient instructions when the branch condition is
    // not a shader input (X3528: "can't force branch with gradients on non-inputs"),
    // and predicated/flattened execution is what we actually want anyway -- it keeps
    // control flow uniform across the 2x2 pixel quad so tex2Dgrad / tex2D inside
    // sampleAtlasForSlot, sampleNormalForSlot, and sampleHeightWeighted stay legal.
    // Cost is one ALU mask per pixel for the disabled path; for far terrain (POM
    // off via fadeFactor) the height taps still run but their results are masked
    // out -- still cheaper than the diffuse + normal taps the slot loop would do
    // anyway, and well inside the ps_3_0 budget.
    if (g_pomParams.w > 0.5f)
    {
        float3 V = normalize(input.worldPos - g_cameraPosWorld.xyz);

        // 3-D camera distance drives the distance fade.
        float distToCamera = length(input.worldPos - g_cameraPosWorld.xyz);
        float fadeFactor = saturate(
            (g_pomParams.z - distToCamera) /
            max(g_pomParams.z - g_pomParams.y, 1.0f));

        if (fadeFactor > 0.001f)
        {
            // World-space horizontal offset at full depth.
            // Clamp abs(V.z) to prevent the offset blowing up at extreme grazing angles.
            // RTS camera pitch is always well above 0.1 (~5.7 deg), so this is a safety rail.
            float2 maxOffset_world = -V.xy * (g_pomParams.x / max(0.1f, abs(V.z))) * fadeFactor;

            // Convert to controlUV space: delta_cUV = delta_world * g_controlParams.xy
            // (g_controlParams.xy = uvScaleX/Y = texelsPerCell / (atlasSize * MAP_XY_FACTOR))
            float2 maxOffset_cUV = maxOffset_world * g_controlParams.xy;

            const int kSteps = 16;
            const float stepSize = 1.0f / (float) kSteps;
            float2 step_cUV = maxOffset_cUV * stepSize;
            float2 step_world = maxOffset_world * stepSize;

            // Ray state: two adjacent bracketing positions (prev = below surface, curr = above).
            float2 prev_cUV = float2(0.0f, 0.0f);
            float2 curr_cUV = float2(0.0f, 0.0f);
            float2 prev_world = float2(0.0f, 0.0f);
            float2 curr_world = float2(0.0f, 0.0f);
            float prevRayH = 1.0f;
            float currRayH = 1.0f;

            // Initial height sample at the undisplaced UV.
            float h = sampleHeightWeighted(
                input.worldXY,
                controlUV,
                dWorldXYdx,
                dWorldXYdy,
                dControlUVdx,
                dControlUVdy,
                activeCount);

            // Linear search: advance until the height field is at or above the ray.
            // @bugfix Ronin 12/05/2026 Normal-map N6: flag-based termination instead of
            // `break`, to keep fxc from poisoning the gradient-instruction analysis for
            // sampleAtlasForSlot() in the slot loop below (error X3526). After the
            // crossing is found, `searching` flips false and all remaining iterations
            // skip both the state update and the height tap, so the cost is unchanged.
            bool searching = true;
            [loop]
            for (int i = 0; i < kSteps; ++i)
            {
                if (searching)
                {
                    if (h >= currRayH)
                    {
                        searching = false;
                    }
                    else
                    {
                        prev_cUV = curr_cUV;
                        prev_world = curr_world;
                        prevRayH = currRayH;

                        currRayH -= stepSize;
                        curr_cUV += step_cUV;
                        curr_world += step_world;

                        h = sampleHeightWeighted(
                            input.worldXY + curr_world,
                            controlUV + curr_cUV,
                            dWorldXYdx,
                            dWorldXYdy,
                            dControlUVdx,
                            dControlUVdy,
                            activeCount);
                    }
                }
            }

            // Binary refinement: bisect the (prev, curr) interval 4 times.
            // prev = last UV where surface was below ray.
            // curr = first UV where surface was at/above ray (or maxOffset if no crossing).
            [loop]
            for (int j = 0; j < 4; ++j)
            {
                float2 mid_cUV = (curr_cUV + prev_cUV) * 0.5f;
                float2 mid_world = (curr_world + prev_world) * 0.5f;
                float midRayH = (currRayH + prevRayH) * 0.5f;

                float midH = sampleHeightWeighted(
                    input.worldXY + mid_world,
                    controlUV + mid_cUV,
                    dWorldXYdx,
                    dWorldXYdy,
                    dControlUVdx,
                    dControlUVdy,
                    activeCount);

                if (midH < midRayH)
                {
                    // Surface still below ray at midpoint: push prev forward.
                    prev_cUV = mid_cUV;
                    prev_world = mid_world;
                    prevRayH = midRayH;
                }
                else
                {
                    // Surface at or above ray at midpoint: pull curr back.
                    curr_cUV = mid_cUV;
                    curr_world = mid_world;
                    currRayH = midRayH;
                }
            }

            displacedControlUV = controlUV + curr_cUV;
            displacedWorldXY = input.worldXY + curr_world;
        }
    }

    // All albedo, weight, and normal sampling below uses the POM-displaced coordinates.
    // Cloud / light / variation layers stay on the original worldXY (they are low-frequency
    // large-scale effects; displacing them produces no visible benefit and breaks continuity).

    float3 color = float3(0, 0, 0);

    // @feature Ronin 10/05/2026 Normal-map N2: weighted-sum tangent-space normal.
    float3 nSum = float3(0, 0, 0);
    bool sampleNormals = (g_normalParams.x > 0.5f);

    // Active terrain page = the same page the diffuse atlas is bound for. The current
    // pass renders one terrain page at a time (HeightMap.cpp's outer loop), and for the
    // normal atlas we use the matching page index. Because slots that don't live on
    // this page are masked out by g_slotEnableMask, we can pass terrainPage = 0 for
    // any slot that is enabled (the per-slot region table already addresses the right
    // sub-rect). For multi-page support the host code should pass a per-pass page
    // index in g_normalParams.z (reserved); v1 only supports normal page 0 actively
    // sampling and assumes 1:1 with the bound diffuse page.
    int activeNormalPage = 0;

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
                // @feature Ronin 12/05/2026 Normal-map N6: weight lookup at displaced UV.
                float w = weightForSlot(s, displacedControlUV, dControlUVdx, dControlUVdy);
                
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
                    // @feature Ronin 12/05/2026 Normal-map N6: diffuse + normal at displaced UV.
                    color += w * sampleAtlasForSlot(s, displacedWorldXY, dWorldXYdx, dWorldXYdy);

                    if (sampleNormals)
                    {
                        nSum += w * sampleNormalForSlot(s, activeNormalPage,
                            displacedWorldXY, dWorldXYdx, dWorldXYdy);
                    }
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
    float2 phase = frac(input.worldXY * 0.05f);
    return float4(phase.x, phase.y, 0.0f, 1.0f);
#elif DEBUG_VIZ_MODE == 4
    // Normal-map N2 preview: weighted-sum tangent normal mapped to RGB.
    // Flat default (0,0,1) -> solid blue-purple (0.5, 0.5, 1.0).
    // Tiles with a real _NRM asset show colored variation that follows surface detail.
    // If you see ALL solid blue-purple, no normal art is being sampled -- check
    // [NRM] log lines confirm `kind=tga` or `kind=dds` for at least one class.
    float3 nViz = normalize(nSum + float3(0.0001f, 0.0001f, 1.0f));
    return float4(nViz * 0.5f + 0.5f, 1.0f);
#elif DEBUG_VIZ_MODE == 5
    // @feature Ronin 12/05/2026 Normal-map N6: POM debug visualization.
    //   R = sampled heightfield at the displaced UV (1.0 = flat surface, < 1 = bumpy)
    //   G = magnitude of POM UV displacement, scaled x1000 so even tiny offsets show
    //   B = POM-enabled flag (full blue if g_pomParams.w > 0.5, else 0)
    // What you should see:
    //   - With NO _NRM height data:  uniform red, zero green, blue if POM is on.
    //   - With real _NRM height data: red varies across terrain, green lights up on
    //     close terrain that's actually being raymarched.
    //   - With POM gated off (UseTerrainPOM=No or no _NRM pages): black except R.
    float dbgH = sampleHeightWeighted(
        displacedWorldXY, displacedControlUV,
        dWorldXYdx, dWorldXYdy, dControlUVdx, dControlUVdy,
        activeCount);
    float dbgDispMag = length(displacedControlUV - controlUV) * 1000.0f;
    float dbgEnable  = (g_pomParams.w > 0.5f) ? 1.0f : 0.0f;
    return float4(dbgH, saturate(dbgDispMag), dbgEnable, 1.0f);
#else
    // @feature Ronin 12/05/2026 Normal-map N3 (revised): hybrid lighting.
    // input.color.rgb = per-vertex Gouraud (smooth macro slope, TOD-aware).
    // sampled tangent normal = per-pixel multiplicative Lambert delta on top.
    float3 finalColor = color * input.color.rgb;

    if (sampleNormals && g_normalParams.y > 0.001f)
    {
        float3 N = normalize(nSum + float3(0.0f, 0.0f, 1.0f));

        N.xy *= g_normalParams.y;
        N = normalize(N);

        float NdotL_flat = saturate(dot(float3(0.0f, 0.0f, 1.0f), g_lightDir.xyz));
        float NdotL_pert = saturate(dot(N, g_lightDir.xyz));

        // Floor on the divisor so a flat normal pointing away from the light
        // doesn't blow the ratio up. 0.15 corresponds to ~80 deg from the light.
        float ratio = NdotL_pert / max(NdotL_flat, 0.15f);

        // Clamp the correction range: 0.5x..1.6x. Keeps the effect visible without
        // crushing shadows to black or blowing highlights to pure white.
        ratio = clamp(ratio, 0.5f, 1.6f);

        finalColor *= ratio;
    }

    // Cloud / light / variation applied against the UNDISPLACED worldXY intentionally:
    // these are large-scale low-frequency layers where screen-space continuity matters
    // more than sub-texel UV accuracy. Displacing them would produce minor swimming
    // in clouds / lightmap at close range with no visible gain.
    finalColor = applyTerrainNoiseLayers(finalColor, input.worldXY);
    return float4(saturate(finalColor), 1.0f);
#endif
}
