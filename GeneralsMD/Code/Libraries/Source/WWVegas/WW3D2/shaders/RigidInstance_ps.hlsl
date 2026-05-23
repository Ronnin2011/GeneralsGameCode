//   fxc /T ps_2_0 /E main /Fo RigidInstance.pso RigidInstance_ps.hlsl
// Ronin @feature 16/05/2026 DX9: sample the same terrain cloud field on rigid
// instanced meshes using sampler s1 when the fixed-function rigid path has
// installed the cloud texture there.
// Ronin @bugfix 17/05/2026 DX9: gate the tex2D call behind the cloud-enabled flag.
// An unconditional sample on an unbound stage-1 returns undefined values on some
// drivers, causing a bright multiply flash on all instanced meshes in the batch
// whenever any structure was selected and the cloud was inactive for that draw.
// Ronin @diagnostic 22/05/2026 DX9: add one-shot debug output modes for the
// armed suspicious cloud batch so the instanced cloud shader path can be
// inspected directly without broad global logging.

sampler2D g_DiffuseSampler : register(s0);
sampler2D g_CloudSampler : register(s1);

float4 g_CloudParams : register(c0);
float4 g_DebugParams : register(c1);
// g_DebugParams.x modes:
//   0 = normal
//   1 = vertex lighting only (input.diffuse)
//   2 = textured color before cloud multiply
//   3 = cloud sample only
//   4 = projected cloud UV visualization

struct PS_INPUT
{
    float4 diffuse : COLOR0;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

float4 main(PS_INPUT input) : COLOR0
{
    const int debugMode = (int) (g_DebugParams.x + 0.5f);

    float4 texel = tex2D(g_DiffuseSampler, input.uv0);
    float4 baseColor = texel * input.diffuse;

    float3 cloudSample = float3(1.0f, 1.0f, 1.0f);
    if (g_CloudParams.x > 0.0f)
    {
        cloudSample = tex2D(g_CloudSampler, input.uv1).rgb;
    }

    if (debugMode == 1)
    {
        return input.diffuse;
    }

    if (debugMode == 2)
    {
        return baseColor;
    }

    if (debugMode == 3)
    {
        return float4(cloudSample, 1.0f);
    }

    if (debugMode == 4)
    {
        return float4(frac(input.uv1.x), frac(input.uv1.y), 0.0f, 1.0f);
    }

    float4 color = baseColor;
    if (g_CloudParams.x > 0.0f)
    {
        color.rgb *= cloudSample;
    }

    return color;
}
