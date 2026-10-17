// Ronin @feature 13/09/2026 DX9: SSAO step 3a. Debug view of an AO render target: its R channel as grey.
// Ronin @feature 12/10/2026 DX9: and the frame's multiply. AO shades the map's ambient, not its sun: where the sun reaches, a pixel
// takes only the ambient's share of it - the shade's floor over its shade. g_Sun.w = 0: plain grey, as before.
//
// Compile with: fxc /T ps_3_0 /Fo SsaoShow.pso SsaoShow_ps.hlsl   (pairs with ScreenQuad.vso)

sampler2D g_AO          : register(s0);
sampler2D g_SunShade    : register(s1);     // what the sun's shadow left of each mesh pixel (W3DLocalLights); a = 1 where a mesh wrote
sampler2D g_GroundShade : register(s2);     // the same for the terrain, from its own shadow pass

float4 g_Sun  : register(c0);               // rgb = the shade's floor on this map, w = how far to follow it (`lights aosun`)
float4 g_Have : register(c1);               // x = a mesh record is bound, y = a ground record is

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float  ao   = tex2D(g_AO, uv).r;
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
    return float4(1.0f - keep * (1.0f - ao), 1.0f);
}
