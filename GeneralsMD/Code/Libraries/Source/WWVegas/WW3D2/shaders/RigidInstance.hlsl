// Ronin @bugfix 21/02/2026 DX9: Respect D3DRS_DIFFUSEMATERIALSOURCE / AMBIENTMATERIALSOURCE
// Ronin @bugfix 22/02/2026 DX9: Fix light direction convention - InputLights points toward light
// Ronin @bugfix 28/02/2026 DX9: Use TEXCOORD1..3 for instance transform to avoid TEXCOORD gaps on AMD
// Ronin @feature 16/05/2026 DX9: pass terrain cloud projection UVs through TEXCOORD1.
// Ronin @feature 23/05/2026 DX9 R2: also pass world-space normal (TEXCOORD2) and world-space
// position (TEXCOORD3) so the PS can build a screen-space TBN for rigid normal mapping.
// Ronin @feature 07/06/2026 DX9: 4 directional lights, now sourced PER INSTANCE from
// stream-1 inputs TEXCOORD4..12 (ambient + numLights, then dir/diffuse x4) instead of
// draw-wide constants. Material/flags/cloud stay constants. Uses all 16 vs_3_0 inputs.
// Compile with: fxc /T vs_3_0 /Fo RigidInstance.vso RigidInstance.hlsl

float4x4 g_ViewProj : register(c0);
float4 g_MatDiffuse : register(c7);
float4 g_MatEmissive : register(c8);
float4 g_Flags : register(c9); // x = lightingEnabled, y = hasVertexColor
float4 g_MatAmbient : register(c10);
float4 g_MatSrcFlags : register(c13);
float4 g_CloudParams : register(c14);
float4 g_TexGenParams  : register(c19); // x = enable, y = sourceMode (0 = vertex UV)
float4 g_TexMatrixRow0 : register(c20);
float4 g_TexMatrixRow1 : register(c21);


struct VS_INPUT
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float4 diffuse : COLOR0;
    float2 uv0 : TEXCOORD0;
    float4 worldRow0 : TEXCOORD1;
    float4 worldRow1 : TEXCOORD2;
    float4 worldRow2 : TEXCOORD3;
    float4 instAmbient : TEXCOORD4; // rgb = ambient, w = numLights
    float4 instLightDir0 : TEXCOORD5;
    float4 instLightDiffuse0 : TEXCOORD6;
    float4 instLightDir1 : TEXCOORD7;
    float4 instLightDiffuse1 : TEXCOORD8;
    float4 instLightDir2 : TEXCOORD9;
    float4 instLightDiffuse2 : TEXCOORD10;
    float4 instLightDir3 : TEXCOORD11;
    float4 instLightDiffuse3 : TEXCOORD12;
};

struct VS_OUTPUT
{
    float4 pos : POSITION;
    float4 diffuse : COLOR0;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float3 worldNormal : TEXCOORD2; // R2: for PS-side normal mapping
    float3 worldPos : TEXCOORD3; // R2: for ddx/ddy TBN reconstruction
};

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;

    float4 localPos = float4(input.pos, 1.0f);

    float3 worldPos;
    worldPos.x = dot(input.worldRow0, localPos);
    worldPos.y = dot(input.worldRow1, localPos);
    worldPos.z = dot(input.worldRow2, localPos);

    float3 worldNormal;
    worldNormal.x = dot(input.worldRow0.xyz, input.normal);
    worldNormal.y = dot(input.worldRow1.xyz, input.normal);
    worldNormal.z = dot(input.worldRow2.xyz, input.normal);
    worldNormal = normalize(worldNormal);

    output.pos = mul(float4(worldPos, 1.0f), g_ViewProj);

    float lightingEnabled = g_Flags.x;
    float hasVertexColor = g_Flags.y;
    float numLights = input.instAmbient.w;

    float diffuseSrcVertex = g_MatSrcFlags.x;
    float ambientSrcVertex = g_MatSrcFlags.y;
    float emissiveSrcVertex = g_MatSrcFlags.z;

    float4 effectiveDiffuse = lerp(g_MatDiffuse, input.diffuse, diffuseSrcVertex * hasVertexColor);
    float3 effectiveAmbient = lerp(g_MatAmbient.rgb, input.diffuse.rgb, ambientSrcVertex * hasVertexColor);
    float3 effectiveEmissive = lerp(g_MatEmissive.rgb, input.diffuse.rgb, emissiveSrcVertex * hasVertexColor);

    float3 ambientTerm = effectiveAmbient * input.instAmbient.rgb;
    float3 diffuseAccum = float3(0.0f, 0.0f, 0.0f);

    if (numLights > 0.0f)
    {
        float NdotL0 = max(0.0f, dot(worldNormal, input.instLightDir0.xyz));
        diffuseAccum += NdotL0 * input.instLightDiffuse0.rgb;
    }

    if (numLights > 1.0f)
    {
        float NdotL1 = max(0.0f, dot(worldNormal, input.instLightDir1.xyz));
        diffuseAccum += NdotL1 * input.instLightDiffuse1.rgb;
    }

    if (numLights > 2.0f)
    {
        float NdotL2 = max(0.0f, dot(worldNormal, input.instLightDir2.xyz));
        diffuseAccum += NdotL2 * input.instLightDiffuse2.rgb;
    }

    if (numLights > 3.0f)
    {
        float NdotL3 = max(0.0f, dot(worldNormal, input.instLightDir3.xyz));
        diffuseAccum += NdotL3 * input.instLightDiffuse3.rgb;
    }

    float3 litColor = effectiveEmissive + ambientTerm + diffuseAccum * effectiveDiffuse.rgb;
    litColor = saturate(litColor);
    float litAlpha = effectiveDiffuse.a;

    float3 unlitColor = lerp(float3(1, 1, 1), input.diffuse.rgb, hasVertexColor);
    float unlitAlpha = lerp(1.0f, input.diffuse.a, hasVertexColor);

    output.diffuse.rgb = lerp(unlitColor, litColor, lightingEnabled);
    output.diffuse.a = lerp(unlitAlpha, litAlpha, lightingEnabled);

    // Ronin @feature 16/06/2026 DX9 Rigid parity (Phase 1: AFFINE_UV).
    float2 mappedUV = input.uv0;
    if (g_TexGenParams.x > 0.5f)
    {
        float4 src = float4(input.uv0, 0.0f, 1.0f); // sourceMode 0; env modes added later
        mappedUV.x = dot(g_TexMatrixRow0, src);
        mappedUV.y = dot(g_TexMatrixRow1, src);
    }
    output.uv0 = mappedUV;

    output.uv1 = worldPos.xy * g_CloudParams.y + g_CloudParams.zw;
    output.worldNormal = worldNormal;
    output.worldPos = worldPos;

    return output;
}
