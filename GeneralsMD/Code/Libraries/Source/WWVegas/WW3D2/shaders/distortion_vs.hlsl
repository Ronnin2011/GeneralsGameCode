// distortion_vs.hlsl - Pass-through vertex shader for smudge distortion
// @feature Ronin 05/03/2026 DX9 requires a VS when using ps_3_0. This VS transforms
// view-space vertices by the projection matrix and forwards UVs + diffuse to the PS.
//
// Compile with: fxc /T vs_3_0 /Fo distortion_vs.vso distortion_vs.hlsl
//
// Constant registers (set by CPU):
//   c0..c3 = Projection matrix (transposed for mul(vector, matrix))
//
// The CPU sets World=Identity, View=Identity, so vertices arrive in view space.
// We only need to apply the projection matrix.

float4x4 g_Projection : register(c0);

struct VS_INPUT
{
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float4 diffuse : COLOR0;
    float2 uv0     : TEXCOORD0;
    float2 uv1     : TEXCOORD1;
};

struct VS_OUTPUT
{
    float4 pos      : POSITION;
    float2 sceneUV  : TEXCOORD0;
    float2 noiseUV  : TEXCOORD1;
    float4 color    : COLOR0;
};

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;

    // Transform view-space position by projection (World and View are identity)
    output.pos = mul(float4(input.pos, 1.0f), g_Projection);

    // Pass through UVs and vertex color directly to pixel shader
    output.sceneUV = input.uv0;
    output.noiseUV = input.uv1;
    output.color   = input.diffuse;

    return output;
}
