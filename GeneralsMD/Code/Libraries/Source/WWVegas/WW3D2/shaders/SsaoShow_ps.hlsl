// Ronin @feature 13/09/2026 DX9: SSAO step 3a. Debug view of an AO render target: its R channel as grey.
//
// Compile with: fxc /T ps_3_0 /Fo SsaoShow.pso SsaoShow_ps.hlsl   (pairs with ScreenQuad.vso)

sampler2D g_AO : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float ao = tex2D(g_AO, uv).r;
    return float4(ao, ao, ao, 1.0f);
}
