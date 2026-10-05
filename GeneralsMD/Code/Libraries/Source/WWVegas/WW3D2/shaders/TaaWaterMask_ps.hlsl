// Ronin @bugfix 03/10/2026 DX9: TAA's water mask, copied into the velocity target. The water marks its own pixels while it
// draws (WaterSea_ps COLOR1, W3DTaa::beginWaterMask). docs/Water_Work.md §6, docs/AntiAliasing_Work.md.
//
// Compile with: fxc /T ps_3_0 /Fo TaaWaterMask.pso TaaWaterMask_ps.hlsl   (pairs with ScreenQuad.vso)

sampler2D g_Mask : register(s0);	// the water's marks, point sampled, 1:1 with the velocity target

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
	clip(tex2D(g_Mask, uv).r - 0.5f);
	// TaaVelMesh_ps's packing of zero velocity (2048 of 4095 a axis), and its kind 0.4 = reactive: the surface is still,
	// the camera's motion is the reprojection's.
	return float4(128.0f / 255.0f, 128.0f / 255.0f, 0.0f, 0.4f);
}
