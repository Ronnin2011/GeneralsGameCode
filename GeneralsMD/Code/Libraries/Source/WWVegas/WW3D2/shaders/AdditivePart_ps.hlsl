// Ronin @perf 07/11/2026 DX9: additive model parts (rocket flames, muzzle flashes) drawn instanced after the sorting pool.
// The texture times the lit colour RigidInstance.hlsl computes: the fixed-function MODULATE their pooled draw used.
// Compile with: fxc /T ps_3_0 /Fo AdditivePart.pso AdditivePart_ps.hlsl

sampler2D g_DiffuseSampler : register(s0);

struct PS_INPUT
{
    float4 diffuse : COLOR0;
    float2 uv0 : TEXCOORD0;
};

float4 main(PS_INPUT input) : COLOR0
{
    return tex2D(g_DiffuseSampler, input.uv0) * input.diffuse;
}
