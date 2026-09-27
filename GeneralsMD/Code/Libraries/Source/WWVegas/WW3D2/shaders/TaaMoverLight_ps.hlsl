// Ronin @bugfix 27/09/2026 DX9: TAA moving-shadow mask - the pixel half. docs/AntiAliasing_Work.md.
//
// W3DTaa renderMoverMask draws every mesh that MOVED this frame from the sun's view (the shadow map's far cascade, which
// covers the whole view) with TaaVelMesh.vso, whose TEXCOORD0 is then the sun-clip position. This writes that depth, 16 bits
// packed into two channels; TaaResolve_ps unpack16 is the exact inverse. The colour write mask picks the pair: RG = where
// the mesh is now, BA = where it was last frame. The pass's own depth buffer keeps the surface nearest the sun.
//
// Compile with: fxc /T ps_3_0 /Fo TaaMoverLight.pso TaaMoverLight_ps.hlsl   (pairs with TaaVelMesh.vso)

float4 main(float4 sunClip : TEXCOORD0) : COLOR0
{
    float  z  = saturate(sunClip.z / sunClip.w);
    float  v  = floor(z * 65535.0f + 0.5f);
    float  hi = floor(v / 256.0f);
    float  lo = v - hi * 256.0f;
    float2 e  = float2(hi, lo) / 255.0f;
    return float4(e, e);
}
