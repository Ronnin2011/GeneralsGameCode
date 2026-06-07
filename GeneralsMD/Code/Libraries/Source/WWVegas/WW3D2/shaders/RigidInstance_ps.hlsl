// Ronin @feature 16/05/2026 DX9: sample the terrain cloud field on rigid instanced meshes.
// Ronin @bugfix 17/05/2026 DX9: gate the tex2D call behind the cloud-enabled flag.
// Ronin @diagnostic 22/05/2026 DX9: one-shot debug output modes for the cloud path.
// Ronin @feature 23/05/2026 DX9 R2: rigid normal mapping (phase 2).
//   * Upgraded to ps_3_0 so we can use ddx/ddy to reconstruct a tangent basis
//     directly from interpolated worldPos + uv0 (no authored tangents needed yet).
//   * Optional normal map on s2, controlled by g_NormalMapParams.x.
//   * Adds a per-pixel Lambert delta against the perturbed normal on top of the
//     existing VS Gouraud term, mirroring the terrain N3 approach documented in
//     docs/Terrain_Normal_Map_Design_NEW.md.
//   * Falls back to identical previous behavior when g_NormalMapParams.x == 0.
//
// Compile with: fxc /T ps_3_0 /Fo RigidInstance.pso RigidInstance_ps.hlsl

sampler2D g_DiffuseSampler : register(s0);
sampler2D g_CloudSampler : register(s1);
sampler2D g_NormalSampler : register(s2);

float4 g_CloudParams : register(c0); // x = cloud enable
float4 g_DebugParams : register(c1);
float4 g_NormalMapParams : register(c2); // x = enable, y = intensity
float4 g_PS_LightDir0 : register(c3);
float4 g_PS_LightDiffuse0 : register(c4);
float4 g_PS_LightDir1 : register(c5);
float4 g_PS_LightDiffuse1 : register(c6);
float4 g_PS_NumLights : register(c7); // x = number of lights (0/1/2)

struct PS_INPUT
{
    float4 diffuse : COLOR0;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
    float3 worldPos : TEXCOORD3;
};

float4 main(PS_INPUT input) : COLOR0
{
    const int debugMode = (int) (g_DebugParams.x + 0.5f);

    float4 texel = tex2D(g_DiffuseSampler, input.uv0);
    float4 baseColor = texel * input.diffuse;

    // ---- Optional rigid normal mapping (R2) -----------------------------
    float3 lambertDelta = float3(0.0f, 0.0f, 0.0f);
    if (g_NormalMapParams.x > 0.0f)
    {
        // Screen-space tangent basis from interpolants.
        float3 dPdx = ddx(input.worldPos);
        float3 dPdy = ddy(input.worldPos);
        float2 dUVdx = ddx(input.uv0);
        float2 dUVdy = ddy(input.uv0);

        float3 N = normalize(input.worldNormal);

        // Cotangent frame (Mikktspace-style approximation without per-vertex tangents).
        float3 dPdy_cross_N = cross(dPdy, N);
        float3 N_cross_dPdx = cross(N, dPdx);
        float3 T = dPdy_cross_N * dUVdx.x + N_cross_dPdx * dUVdy.x;
        float3 B = dPdy_cross_N * dUVdx.y + N_cross_dPdx * dUVdy.y;

        float tLenSq = dot(T, T);
        float bLenSq = dot(B, B);

        // Ronin @bugfix 23/05/2026 DX9 R3: Guard against degenerate screen-space
        // tangent frames. Some rigid meshes produce near-zero UV derivatives on a few
        // pixels; the old rsqrt(0) path caused NaNs / flickering artifacts.
        if (tLenSq > 1.0e-8f && bLenSq > 1.0e-8f)
        {
            float invMax = rsqrt(max(tLenSq, bLenSq));
            T *= invMax;
            B *= invMax;

            float3 nm = tex2D(g_NormalSampler, input.uv0).rgb * 2.0f - 1.0f;
            nm.xy *= g_NormalMapParams.y;
            float3 perturbedN = normalize(T * nm.x + B * nm.y + N * nm.z);

            // Lambert delta vs. the geometric normal so we only add the *bump-induced*
            // shading on top of the existing VS Gouraud term — never double-count base lighting.
            float numLights = g_PS_NumLights.x;
            if (numLights > 0.0f)
            {
                float dN0 = saturate(dot(perturbedN, g_PS_LightDir0.xyz)) -
                            saturate(dot(N, g_PS_LightDir0.xyz));
                lambertDelta += dN0 * g_PS_LightDiffuse0.rgb;
            }
            if (numLights > 1.0f)
            {
                float dN1 = saturate(dot(perturbedN, g_PS_LightDir1.xyz)) -
                            saturate(dot(N, g_PS_LightDir1.xyz));
                lambertDelta += dN1 * g_PS_LightDiffuse1.rgb;
            }

            // Modulate the delta by the diffuse texel so the bump never lights up
            // areas that are otherwise dark in the diffuse map (e.g. black decals).
            baseColor.rgb = saturate(baseColor.rgb + lambertDelta * texel.rgb);
        }
    }
    // ---------------------------------------------------------------------

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
