// Ronin @feature 14/08/2026 DX9: §29 terrain shadow receiver. Separate pass, NOT a sampler stolen
// from terrainpermaterial_ps (all 16 are reserved — see Terrain_Normal_Map_Design_NEW.md decision log).
//
// Compile with: fxc /T vs_2_0 /Fo TerrainShadow.vso TerrainShadow_vs.hlsl
//
// The input struct MUST mirror DX8_FVF_XYZDUV2 (BaseHeightMap.h:82) even though only the position is
// used — the FVF-declared stream has to map onto declared inputs. Terrain VB positions are already
// TRUE WORLD SPACE (ADJUST_FROM_INDEX_TO_REAL at HeightMap.cpp:390 removes the border and scales by
// MAP_XY_FACTOR), and renderTerrainPass sets an identity world transform, so worldPos == input.pos.

float4x4 g_ViewProj : register(c0);

struct VSInput
{
    float3 pos : POSITION0;
    float4 color : COLOR0;
    float2 uvBase : TEXCOORD0;
    float2 uvBlend : TEXCOORD1;
};

struct VSOutput
{
    float4 pos : POSITION0;
    float3 worldPos : TEXCOORD0;
    // Ronin @feature 18/08/2026 DX9: §29h-6. The vertex COLOUR is the only smoothly interpolated
    // attribute this FVF carries, and doTheLight() builds it from the AVERAGED per-vertex normal, so
    // its brightness is a faceting-free stand-in for N.L. A ddx/ddy normal is flat per triangle and
    // stamped the terrain's triangulation onto the hilltops.
    float4 color : COLOR0;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    output.pos = mul(float4(input.pos, 1.0f), g_ViewProj);
    output.worldPos = input.pos;
    output.color = input.color;

    return output;
}
