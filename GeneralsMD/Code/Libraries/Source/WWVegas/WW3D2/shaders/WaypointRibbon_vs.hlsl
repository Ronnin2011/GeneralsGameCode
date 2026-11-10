// Ronin @feature 09/11/2026 DX9: waypoint ribbons (R3DWaypointLines) - the band's and the rings' world vertices to clip space;
// their colour and band coordinates to the pixel shader as they are.
//
// Compile with: fxc /T vs_3_0 /Fo WaypointRibbon.vso WaypointRibbon_vs.hlsl

float4x4 g_ViewProj : register(c248);	// TRANSPOSED on upload; high registers: other passes' constants stay theirs

struct VS_IN
{
	float3 pos : POSITION;
	float4 col : COLOR0;
	float4 t   : TEXCOORD0;		// x across -1..1, y along (units), z the path's length, w 1 on a ring
};

struct VS_OUT
{
	float4 pos : POSITION;
	float4 col : COLOR0;
	float4 t   : TEXCOORD0;
};

VS_OUT main(VS_IN i)
{
	VS_OUT o;
	o.pos = mul(float4(i.pos, 1.0f), g_ViewProj);
	o.col = i.col;
	o.t   = i.t;
	return o;
}
