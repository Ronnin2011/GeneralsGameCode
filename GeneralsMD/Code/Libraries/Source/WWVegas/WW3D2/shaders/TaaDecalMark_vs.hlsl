// Ronin @bugfix 27/10/2026 DX9: a decal's own texels in the TAA velocity target (R3DTaa, the decal marks), with the decal's own
// motion: the ground grid under it now and last frame, through the clean and the previous projections, as TaaVelMesh_vs.
//
// Compile with: fxc /T vs_3_0 /Fo TaaDecalMark.vso TaaDecalMark_vs.hlsl

float4x4 g_CurVP  : register(c0);   // TRANSPOSED on upload, as TaaVelMesh_vs
float4x4 g_PrevVP : register(c4);

struct VS_IN
{
    float3 pos     : POSITION;     // the grid now
    float2 uv      : TEXCOORD0;    // the decal's UV, 0..1 from its top-left corner
    float3 prevPos : TEXCOORD1;    // the same grid point last frame
};

struct VS_OUT
{
    float4 pos  : POSITION;
    float2 uv   : TEXCOORD0;
    float4 cur  : TEXCOORD1;
    float4 prev : TEXCOORD2;
};

VS_OUT main(VS_IN i)
{
    VS_OUT o;
    o.cur  = mul(float4(i.pos, 1.0f), g_CurVP);
    o.prev = mul(float4(i.prevPos, 1.0f), g_PrevVP);
    o.pos  = o.cur;
    o.uv   = i.uv;
    return o;
}
