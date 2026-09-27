// Ronin @feature 26/09/2026 DX9: TAA motion vectors for SKINNED meshes (infantry). docs/AntiAliasing_Work.md §14 stage 2.
//
// W3D deforms skins on the CPU into WORLD space, so there is no world matrix to diff: TAA keeps each skin's deformed
// positions from last frame and feeds both per vertex - POSITION = this frame, TEXCOORD0 = last frame. Same outputs as
// TaaVelMesh_vs, so TaaVelMesh_ps writes the velocity unchanged.
//
// View-projections built on the CPU in the row-vector convention, uploaded TRANSPOSED, consumed with mul(rowVec, M).
//
// Compile with: fxc /T vs_3_0 /Fo TaaVelSkin.vso TaaVelSkin_vs.hlsl

float4x4 g_CurVP  : register(c0);
float4x4 g_PrevVP : register(c4);

struct VS_IN
{
    float3 pos     : POSITION;     // world, this frame
    float3 prevPos : TEXCOORD0;    // world, last frame
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
    o.cur  = mul(float4(i.pos, 1.0f), g_CurVP);
    o.prev = mul(float4(i.prevPos, 1.0f), g_PrevVP);
    o.pos  = o.cur;
    return o;
}
