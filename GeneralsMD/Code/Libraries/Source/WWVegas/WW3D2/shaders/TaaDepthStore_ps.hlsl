// Ronin @feature 22/09/2026 DX9: TAA depth history. Stores this frame's LINEAR depth so the next frame can ask the
// only question that actually matters: does the history at this pixel describe the same geometry?
//
// WHY: 22/09 established that colour disagreement cannot answer it. The signal is strong on high-contrast STATIC
// detail (poles, fences) and weak on the low-contrast ghosts it was meant to catch - anti-correlated with what is
// needed, so no threshold on it works. Depth has the opposite character: a tank against grass is almost invisible
// in colour and a large step in depth. That is the "velocity discontinuity" the literature calls for, obtained
// without per-object motion vectors.
//
// Packed into RGBA8 because this DX9 path has no float render target format. 24 bits is far more than a
// disocclusion test needs.
//
// Compile with: fxc /T ps_3_0 /Fo TaaDepthStore.pso TaaDepthStore_ps.hlsl   (pairs with ScreenQuad.vso)

sampler2D g_Scene    : register(s0);     // this frame, final
sampler2D g_Depth    : register(s2);     // INTZ scene depth, this frame
sampler2D g_Opaque   : register(s7);     // this frame before particles (W3DTaa::noteOpaqueDone)
float4    g_DepthCfg : register(c0);     // y = zNear, z = zFar (c0 yz, matching the resolve's g_TaaDebug layout),
                                         // w = `taa autoreact` threshold, 0 = off; x = 1 / (`autofull` - `autoreact`)

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float d = tex2D(g_Depth, uv).r;
    float n = g_DepthCfg.y;
    float f = g_DepthCfg.z;

    // Raw D3D depth is f/(f-n)*(1-n/z) and sits above 0.99 for nearly the whole scene, so it must be linearised
    // before it can be compared at all - the same trap the depth debug view fell into on 21/09.
    float linZ = (n * f) / max(f - d * (f - n), 0.0001f);
    float norm = saturate(linZ / f);

    // Ronin @feature 26/09/2026 DX9: 24 bits in RGB - still far past what a 1% depth test needs - and ALPHA = this
    // frame's auto-reactive flag, which next frame's resolve reads at the reprojected position. The resolve's unpack
    // is the exact inverse.
    float3 enc = frac(float3(1.0f, 255.0f, 65025.0f) * norm);
    enc -= enc.yzz * float3(1.0f / 255.0f, 1.0f / 255.0f, 0.0f);
    float flag = 0.0f;
    if (g_DepthCfg.w > 0.0f)
    {
        float3 dd = abs(tex2Dlod(g_Scene, float4(uv, 0.0f, 0.0f)).rgb - tex2Dlod(g_Opaque, float4(uv, 0.0f, 0.0f)).rgb);
        // Ronin @bugfix 08/10/2026 DX9: proportional, as the resolve computes it this frame (c0.x).
        flag = saturate((max(dd.r, max(dd.g, dd.b)) - g_DepthCfg.w) * g_DepthCfg.x);
    }
    return float4(enc, flag);
}
