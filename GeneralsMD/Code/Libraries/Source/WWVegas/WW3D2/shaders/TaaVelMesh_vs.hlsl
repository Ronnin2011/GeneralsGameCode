// Ronin @feature 24/09/2026 DX9: TAA motion vectors drawn FROM THE MESH. docs/AntiAliasing_Work.md §14.
//
// Each moving mesh is re-drawn from its own vertex array with two full transforms: where it is now (this frame's
// world x this frame's camera) and where it was (last frame's world x last frame's camera). The pixel shader writes
// the screen difference. Per VERTEX, so translation, rotation, a turret turning on a still hull and a bone-driven
// part are all exact - the per-object rectangle could represent none of those, and could not tell unit from ground.
//
// Both matrices are world-view-projection, built on the CPU in the row-vector convention and uploaded TRANSPOSED,
// consumed with mul(rowVec, M) - the pairing every matrix in this project's shaders uses.
//
// Compile with: fxc /T vs_3_0 /Fo TaaVelMesh.vso TaaVelMesh_vs.hlsl

float4x4 g_CurWVP  : register(c0);
float4x4 g_PrevWVP : register(c4);

struct VS_IN
{
    float3 pos : POSITION;
};

struct VS_OUT
{
    float4 pos  : POSITION;
    float4 cur  : TEXCOORD0;
    float4 prev : TEXCOORD1;
};

VS_OUT main(VS_IN i)
{
    VS_OUT o;
    float4 p = float4(i.pos, 1.0f);
    o.cur  = mul(p, g_CurWVP);
    o.prev = mul(p, g_PrevWVP);
    o.pos  = o.cur;
    return o;
}
