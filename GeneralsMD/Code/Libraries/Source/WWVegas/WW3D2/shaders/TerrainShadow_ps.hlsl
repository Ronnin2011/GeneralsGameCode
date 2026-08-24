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

float4x4 g_LightViewProj : register(c0); // c0..c3, TRANSPOSED like g_ViewProj (Windowednew.md §29d)
float4 g_ShadowParams : register(c4);    // x = unused here, y = depth bias, zw = half-texel offset
float4 g_ShadowParams2 : register(c5);   // xyz = direction light TRAVELS, w = texel size in world units

struct PS_INPUT
{
    float3 worldPos : TEXCOORD0;
    float4 color : COLOR0;
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
    float lit = 1.0f;
    if (shadowUV.x == saturate(shadowUV.x) && shadowUV.y == saturate(shadowUV.y))
    {
        // 3x3 at one-texel spacing. Each tap is itself a hardware 2x2 compare, so this is an
        // effective 4x4 blur for 9 taps. texelUV = 1/resolution = 2 * the half-texel offset.
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
        lit = sum * (1.0f / 9.0f);
    }

    // A surface turning away from the sun can only get DARKER, never brighter — min(), not lerp().
    lit = min(lit, geo);

    float shade = lerp(0.45f, 1.0f, lit);
    return float4(shade, shade, shade, 1.0f);
}
