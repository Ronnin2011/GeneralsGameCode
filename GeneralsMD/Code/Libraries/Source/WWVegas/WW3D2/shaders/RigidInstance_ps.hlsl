// Ronin @feature 08/03/2026 DX9: Minimal instancing pixel shader used to bypass
// fixed-function pixel combiner differences during AMD instancing diagnosis.
// Ronin @build 08/03/2026 DX9: Compile with:
//   fxc /T ps_2_0 /E main /Fo RigidInstance.pso RigidInstance_ps.hlsl

sampler2D g_DiffuseSampler : register(s0);

struct PS_INPUT
{
    float4 diffuse : COLOR0;
    float2 uv0 : TEXCOORD0;
};

float4 main(PS_INPUT input) : COLOR0
{
    float4 texel = tex2D(g_DiffuseSampler, input.uv0);
    return texel * input.diffuse;
}   
