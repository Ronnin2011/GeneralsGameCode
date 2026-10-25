// Ronin @feature 13/09/2026 DX9: SSAO step 3a. Debug view of an AO render target: its R channel as grey.
// Ronin @feature 12/10/2026 DX9: and the frame's multiply. AO shades the map's ambient, not its sun: where the sun reaches, a pixel
// takes only the ambient's share of it - the shade's floor over its shade. g_Sun.w = 0: plain grey, as before.
//
// Compile with: fxc /T ps_3_0 /Fo SsaoShow.pso SsaoShow_ps.hlsl   (pairs with ScreenQuad.vso)

sampler2D g_AO          : register(s0);
sampler2D g_SunShade    : register(s1);     // what the sun's shadow left of each mesh pixel (R3DLocalLights); a = 1 where a mesh wrote
sampler2D g_GroundShade : register(s2);     // the same for the terrain, from its own shadow pass

float4 g_Sun  : register(c0);               // rgb = the shade's floor on this map, w = how far to follow it (`lights aosun`)
float4 g_Have : register(c1);               // x = a mesh record is bound, y = a ground record is
// Ronin @feature 24/10/2026 DX9: COLOUR BLEED. c2 = rgb: 1 / the map's light (frame -> surface colour), w: the bounce's strength;
// c3 = x: coloured AO's strength, y: 0 grey AO (as before), 1 the composite (2x multiply: out = factor / 2), 2 the bounce alone.
float4 g_Bounce : register(c2);
float4 g_AOMode : register(c3);
sampler2D g_Frame : register(s3);           // the frame before SSAO

// Ronin @feature 24/10/2026 DX9: multi-bounce AO (Jimenez et al. 2016): the light bouncing inside a crevice, by the surface's albedo.
float3 multiBounce(float3 v, float3 rho)
{
    float3 a = 2.0404f * rho - 0.3324f;
    float3 b = 4.7951f * rho - 0.6417f;
    float3 c = 2.7552f * rho + 0.6903f;
    return max(v, ((a * v - b) * v + c) * v);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 aoB  = tex2D(g_AO, uv);
    float  ao   = aoB.a;
    if (g_AOMode.y > 1.5f)
        return float4(saturate(aoB.rgb * 4.0f), 1.0f);		// the bounce alone, x4
    float3 keep = float3(1.0f, 1.0f, 1.0f);
    [branch]
    if (g_Sun.w > 0.0f)
    {
        float4 shadeM = tex2Dlod(g_SunShade, float4(uv, 0.0f, 0.0f));
        float3 shadeG = tex2Dlod(g_GroundShade, float4(uv, 0.0f, 0.0f)).rgb;
        float3 shade  = g_Sun.rgb;          // no record of this pixel: all of the AO, as before
        if (g_Have.y > 0.0f && max(shadeG.r, max(shadeG.g, shadeG.b)) > 0.05f)
            shade = shadeG;
        if (g_Have.x > 0.0f && shadeM.a > 0.5f)
            shade = shadeM.rgb;             // a mesh's own first, as the light pass reads them
        keep = saturate(1.0f - g_Sun.w * (1.0f - g_Sun.rgb / max(shade, g_Sun.rgb)));
    }
    float3 v = 1.0f - keep * (1.0f - ao);
    if (g_AOMode.y < 0.5f)
        return float4(v, 1.0f);
    // Ronin @feature 24/10/2026 DX9: the frame x (the AO in the surface's colour + the bounce over the surface's light); halved for the 2x.
    float3 rho = saturate(tex2Dlod(g_Frame, float4(uv, 0.0f, 0.0f)).rgb * g_Bounce.rgb);
    float3 vis = lerp(v, multiBounce(v, rho), g_AOMode.x);
    return float4(saturate((vis + aoB.rgb * g_Bounce.rgb * g_Bounce.w) * 0.5f), 1.0f);
}
