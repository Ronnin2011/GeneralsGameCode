// Ronin @feature 14/08/2026 DX9: §29 terrain shadow receiver. Outputs a GREY shadow factor that the
// caller blends multiplicatively (SRCBLEND=ZERO, DESTBLEND=SRCCOLOR), so the framebuffer is darkened
// where the terrain is occluded. Own pass, own sampler — no contention with terrainpermaterial_ps.
//
// Ronin @feature 17/08/2026 DX9: §29h-6. Three additions, each aimed at a different artifact:
//   * 3x3 PCF — the bare hardware 2x2 tap gave a hard one-texel edge, and made thin casters band on
//     and off between texel rows (a tree trunk is about one texel wide seen from the light).
//   * Normal-offset — moves the LOOKUP along the surface normal rather than pushing depth along the
//     light, so it adapts to slope without detaching the shadow from its caster.
//   * N.L fade — a face turned away from the sun is already dark from lighting, and the map holds
//     nothing useful for it; sampling there is what streaked the steep hillsides.
//
// Compile with: fxc /T ps_3_0 /Fo TerrainShadow.pso TerrainShadow_ps.hlsl

sampler2D g_ShadowSampler : register(s0);
// Ronin @feature 03/09/2026 DX9: §29j.13h. Last frame's accumulated shadow term, screen-space.
sampler2D g_AccumPrev : register(s1);

float4x4 g_LightViewProj : register(c0); // c0..c3, TRANSPOSED like g_ViewProj (Windowednew.md §29d)
float4 g_ShadowParams : register(c4);    // x = EMA HISTORY WEIGHT (§29j.13h), y = bias, zw = half-texel
float4 g_ShadowParams2 : register(c5);   // xyz = direction light TRAVELS, w = texel size in world units
float4 g_ScreenInvSize : register(c6);   // xy = 1/render-target size, zw = render-target size
float4x4 g_PrevViewProj : register(c7);  // c7..c10, TRANSPOSED — §29j.13l
float4 g_ViewportRect : register(c11);   // xy = viewport origin, zw = viewport size, target pixels
float4x4 g_CurViewProj : register(c12);  // c12..c15, TRANSPOSED — §29j.13q

struct PS_INPUT
{
    float3 worldPos : TEXCOORD0;
    float4 color : COLOR0;
    float2 vpos : VPOS;           // §29j.13h — exact pixel coordinate, ps_3_0
};


float4 main(PS_INPUT input) : COLOR0
{
    // Terrain carries no vertex normal (DX8_FVF_XYZDUV2), so rebuild one from the screen-space
    // derivatives of worldPos — the same idiom RigidInstance_ps uses for its tangent basis.
    float3 N = normalize(cross(ddx(input.worldPos), ddy(input.worldPos)));
    if (N.z < 0.0f) N = -N;                     // derivative winding can flip it; terrain faces up

    float3 toLight = -g_ShadowParams2.xyz;      // the constant points the way light TRAVELS
    float  ndotl   = dot(N, toLight);
    
    // Ronin @feature 18/08/2026 DX9: §29h-6. The fade keys on the terrain's OWN vertex lighting, not
    // on a derivative normal: ddx/ddy of worldPos is constant within a triangle, so that normal was
    // flat per face and stamped the triangulation onto the hilltops. doTheLight() builds the vertex
    // colour from the averaged per-vertex normal, so its luminance interpolates smoothly. Subtracting
    // the map's ambient floor leaves the sun's N.L contribution, which is what the fade wants.
    // AMBIENT_LEVEL is the only thing to tune: raise it until slopes facing away from the sun fade,
    // lower it if ground that IS lit starts fading. BACKFACE_SHADOW = 1.0 disables the fade entirely.
    const float BACKFACE_SHADOW = 0.35f;
    const float AMBIENT_LEVEL   = 0.35f;
    float lum    = dot(input.color.rgb, float3(0.299f, 0.587f, 0.114f));
    float sunLit = saturate((lum - AMBIENT_LEVEL) / max(1.0f - AMBIENT_LEVEL, 0.05f));
    float geo    = lerp(BACKFACE_SHADOW, 1.0f, smoothstep(0.0f, 0.35f, sunLit));

    // NORMAL-OFFSET: push the LOOKUP off the surface, not the depth along the light. Scaled by
    // sin(angle from face-on), so it grows exactly where a constant bias runs out.
    const float NORMAL_OFFSET_TEXELS = 2.0f;    // must cover the PCF kernel radius
    float  slope     = sqrt(saturate(1.0f - ndotl * ndotl));
    float3 samplePos = input.worldPos + N * (g_ShadowParams2.w * NORMAL_OFFSET_TEXELS * slope);

    // ORTHO light, so w == 1 and the projective divide is a no-op; tex2Dproj still performs the
    // hardware depth compare + PCF.
    float4 lightClip  = mul(float4(samplePos, 1.0f), g_LightViewProj);
    float2 shadowUV   = lightClip.xy * float2(0.5f, -0.5f) + 0.5f + g_ShadowParams.zw;
    float  lightDepth = lightClip.z - g_ShadowParams.y;

    // Outside the fitted map = fully lit, i.e. white, i.e. a multiply that changes nothing.
    float lit    = 1.0f;
    // §29j.13j: the darkest and brightest values the shadow map supports AT THIS POINT, used to reject
    // stale history. Both default to 1.0 so an off-map pixel clamps history to "lit", which is correct.
    float litMin = 1.0f;
    float litMax = 1.0f;
    if (shadowUV.x == saturate(shadowUV.x) && shadowUV.y == saturate(shadowUV.y))
    {
        // 3x3 at one-texel spacing. Each tap is itself a hardware 2x2 compare, so this is an
        // effective 4x4 blur for 9 taps. texelUV = 1/resolution = 2 * the half-texel offset.
        float texelUV = g_ShadowParams.z * 2.0f;
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
                float2 uv = shadowUV + float2(x, y) * texelUV;
                float  t  = tex2Dproj(g_ShadowSampler, float4(uv, lightDepth, 1.0f)).r;
                sum   += t;
                sumSq += t * t;
                litMin = min(litMin, t);
                litMax = max(litMax, t);
            }
        }
        lit = sum * (1.0f / 9.0f);

        // VARIANCE CLIPPING, intersected with the min/max box. Min/max alone is too permissive for a
        // SMALL shadow: 8 shadowed taps and 1 lit still spans [0,1], so an infantry shadow would keep
        // ghosting. Mean +/- sd is [0, 0.43] for that case and rejects it.
        // Ronin @bugfix 06/09/2026 DX9: §29j.13j. CLIP_FLOOR — sd is 0 wherever the 9 taps agree, so the
        // box collapsed to a point and rejected all history there, whatever CLIP_GAMMA was. lit is sum/9
        // and cannot resolve finer than one tap, so a floor of 1/9 is the estimator's own precision.
        const float CLIP_GAMMA = 2.0f;
        const float CLIP_FLOOR = 1.0f / 9.0f;
        float sd = sqrt(max(sumSq * (1.0f / 9.0f) - lit * lit, 0.0f));
        litMin = max(litMin, lit - CLIP_GAMMA * sd);
        litMax = min(litMax, lit + CLIP_GAMMA * sd);
        litMin = min(litMin, lit - CLIP_FLOOR);
        litMax = max(litMax, lit + CLIP_FLOOR);

    }

    // A surface turning away from the sun can only get DARKER, never brighter — min(), not lerp().
    // min() is monotone, so applying it to the bounds keeps them a valid bracket around lit.
    lit    = min(lit,    geo);
    litMin = min(litMin, geo);
    litMax = min(litMax, geo);

    float shade    = lerp(0.45f, 1.0f, lit);
    float shadeMin = lerp(0.45f, 1.0f, litMin);
    float shadeMax = lerp(0.45f, 1.0f, litMax);


    // Ronin @feature 03/09/2026 DX9: §29j.13h. Temporal EMA on the shadow term. Doc §8.
    // Ronin @feature 06/09/2026 DX9: §29j.13l. REPROJECTION. NDC from the previous view-projection spans
    // the VIEWPORT; this texture spans the RENDER TARGET. Convert through pixels — measured 1600x720
    // against 1600x900, and assuming they match is what broke the first two attempts.
    // Ronin @bugfix 08/09/2026 DX9: §29j.13q. VPOS anchors the lookup; reprojection supplies only the
    // DELTA, whose precision errors cancel. Absolute reprojection repeats §9.13. Doc §8.7.
    float4 prevClip = mul(float4(input.worldPos, 1.0f), g_PrevViewProj);
    float4 curClip  = mul(float4(input.worldPos, 1.0f), g_CurViewProj);
    float2 prevVPuv = (prevClip.xy / prevClip.w) * float2(0.5f, -0.5f) + 0.5f;
    float2 curVPuv  = (curClip.xy  / curClip.w)  * float2(0.5f, -0.5f) + 0.5f;
    float2 myPix    = floor(input.vpos) + 0.5f;
    float2 histUV   = (myPix + (prevVPuv - curVPuv) * g_ViewportRect.zw) * g_ScreenInvSize.xy;


    // Off-screen or behind the eye last frame = newly revealed, so there is no history to blend.
    float onScreen = (prevClip.w > 0.0f
                      && prevVPuv.x == saturate(prevVPuv.x)
                      && prevVPuv.y == saturate(prevVPuv.y)) ? 1.0f : 0.0f;

    float hist = tex2D(g_AccumPrev, histUV).r;

    // Ronin @bugfix 06/09/2026 DX9: §29j.13j. Neighbourhood clamp — rejects history the shadow map no
    // longer supports at this point. Doc §8.
    hist  = clamp(hist, shadeMin, shadeMax);
    shade = lerp(shade, hist, g_ShadowParams.x * onScreen);
    return float4(shade, shade, shade, 1.0f);

}

