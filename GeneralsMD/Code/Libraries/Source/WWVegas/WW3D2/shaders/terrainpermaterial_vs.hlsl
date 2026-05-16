// Compile to shaders\terrainprimarycontrol_vs.vso (vs_2_0).
//
// @feature Ronin 19/04/2026 Splat-map redesign (docs/Terrain_Splat_Map_Design.md §6).
// Outputs uvBase, uvBlend and worldXY. uvControl from the vertex declaration is read but
// ignored in v1; v2 will repurpose it for cell-grid sampling.
//
// worldXY is taken straight from the input position. The terrain render object's transform
// is effectively identity (HeightMapRenderObjClass::Transform is the world transform of the
// height map), and the vertex buffer is already in world space minus a constant border
// offset, which is fine because the noise texture wraps. If the terrain ever gains a
// non-identity world transform this needs to multiply by g_world.

float4x4 g_worldViewProj : register(c0);

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
    float4 color : COLOR0;
    float2 uvBase : TEXCOORD0;
    float2 uvBlend : TEXCOORD1;
    float2 worldXY : TEXCOORD2;
    // @feature Ronin 11/05/2026 Normal-map N3: full world position (xyz) so the PS
    // can ddx/ddy it to derive the analytical heightfield normal per pixel. No VB or
    // FVF change -- the position is already in the input stream as input.pos.
    float3 worldPos : TEXCOORD3;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    output.pos = mul(float4(input.pos, 1.0f), g_worldViewProj);
    output.color = input.color;
    output.uvBase = input.uvBase;
    output.uvBlend = input.uvBlend;
    output.worldXY = input.pos.xy;
    output.worldPos = input.pos.xyz;

    return output;
}
