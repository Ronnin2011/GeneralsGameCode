// Ronin @feature 03/10/2026 DX9: phase 5 - wakes. Draws a moving unit's trail (a ribbon built by W3DWater.cpp renderWakes)
// into the wake texture: world xy straight to the texture's clip space. docs/Water_Work.md.
//
// Compile with: fxc /T vs_3_0 /Fo WaterWake.vso WaterWake_vs.hlsl

float4 g_Map : register(c25);	// world xy -> clip: xy scale, zw offset (the half texel is in it)

struct VSInput
{
	float4 pos    : POSITION0;	// z: Ronin @bugfix 04/10/2026 DX9: the rim - 0 at the ribbon's edge, 1 or more at the track
	float4 across : TEXCOORD0;	// x: distance from the track (signed), y: the V's half-width here, z: age 0..1, w: strength
	float4 along  : TEXCOORD1;	// xy: the ribbon's left normal, z: distance astern of the stern, w: the hull's half beam
};

struct VSOutput
{
	float4 pos    : POSITION0;
	float4 across : TEXCOORD0;
	float4 along  : TEXCOORD1;
	float  rim    : TEXCOORD2;
};

VSOutput main(VSInput input)
{
	VSOutput output;
	output.pos    = float4(input.pos.xy * g_Map.xy + g_Map.zw, 0.5f, 1.0f);
	output.across = input.across;
	output.along  = input.along;
	output.rim    = input.pos.z;
	return output;
}
