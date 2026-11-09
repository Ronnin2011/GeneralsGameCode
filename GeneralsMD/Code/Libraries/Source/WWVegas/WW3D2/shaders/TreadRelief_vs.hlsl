// Ronin @feature 09/11/2026 DX9: tread relief (R3DTreadMarks) - the track strips TerrainTracksRenderObjClassSystem::flush fills
// (XYZ, DIFFUSE, UV1), to clip space; their world position, UV and fade (the decal's alpha) to the pixel shader.
//
// Compile with: fxc /T vs_3_0 /Fo TreadRelief.vso TreadRelief_vs.hlsl

float4x4 g_WorldViewProj : register(c240);	// TRANSPOSED on upload; high registers: other passes' constants stay theirs

struct VS_IN
{
	float3 pos : POSITION;
	float4 col : COLOR0;		// .a = the track's fade with age and length
	float2 uv  : TEXCOORD0;	// u across the vehicle's width, v 0/1 alternating every edge (10 units)
};

struct VS_OUT
{
	float4 pos   : POSITION;
	float2 uv    : TEXCOORD0;
	float3 world : TEXCOORD1;
	float  fade  : TEXCOORD2;
};

VS_OUT main(VS_IN i)
{
	VS_OUT o;
	o.pos   = mul(float4(i.pos, 1.0f), g_WorldViewProj);
	o.uv    = i.uv;
	o.world = i.pos;
	o.fade  = i.col.a;
	return o;
}
