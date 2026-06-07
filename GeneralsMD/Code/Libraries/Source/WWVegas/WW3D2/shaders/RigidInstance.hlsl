// Ronin @bugfix 21/02/2026 DX9: Match FFP lighting model (material ambient, multi-light)
// Ronin @bugfix 21/02/2026 DX9: Respect D3DRS_DIFFUSEMATERIALSOURCE / AMBIENTMATERIALSOURCE
// Ronin @bugfix 22/02/2026 DX9: Fix light direction convention - InputLights points toward light
// Ronin @bugfix 28/02/2026 DX9: Use TEXCOORD1..3 for instance data to avoid TEXCOORD gaps on AMD
// Ronin @feature 16/05/2026 DX9: pass terrain cloud projection UVs through TEXCOORD1.
// Ronin @feature 23/05/2026 DX9 R2: also pass world-space normal (TEXCOORD2) and world-space
// position (TEXCOORD3) so the PS can build a screen-space TBN for rigid normal mapping.
// Compile with: fxc /T vs_3_0 /Fo RigidInstance.vso RigidInstance.hlsl

float4x4 g_ViewProj : register(c0);
float4 g_AmbientLight : register(c4);
float4 g_LightDir0 : register(c5);
float4 g_LightDiffuse0 : register(c6);
float4 g_MatDiffuse : register(c7);
float4 g_MatEmissive : register(c8);
float4 g_Flags : register(c9);
float4 g_MatAmbient : register(c10);
float4 g_LightDir1 : register(c11);
float4 g_LightDiffuse1 : register(c12);
float4 g_MatSrcFlags : register(c13);
float4 g_CloudParams : register(c14);

struct VS_INPUT
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float4 diffuse : COLOR0;
    float2 uv0 : TEXCOORD0;
    float4 worldRow0 : TEXCOORD1;
    float4 worldRow1 : TEXCOORD2;
    float4 worldRow2 : TEXCOORD3;
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
    float numLights = g_Flags.z;

    float diffuseSrcVertex = g_MatSrcFlags.x;
    float ambientSrcVertex = g_MatSrcFlags.y;
    float emissiveSrcVertex = g_MatSrcFlags.z;

    float4 effectiveDiffuse = lerp(g_MatDiffuse, input.diffuse, diffuseSrcVertex * hasVertexColor);
    float3 effectiveAmbient = lerp(g_MatAmbient.rgb, input.diffuse.rgb, ambientSrcVertex * hasVertexColor);
    float3 effectiveEmissive = lerp(g_MatEmissive.rgb, input.diffuse.rgb, emissiveSrcVertex * hasVertexColor);

    float3 ambientTerm = effectiveAmbient * g_AmbientLight.rgb;

    float NdotL0 = max(0.0f, dot(worldNormal, g_LightDir0.xyz));
    float3 diffuseAccum = NdotL0 * g_LightDiffuse0.rgb;

    if (numLights > 1.0f)
    {
        float NdotL1 = max(0.0f, dot(worldNormal, g_LightDir1.xyz));
        diffuseAccum += NdotL1 * g_LightDiffuse1.rgb;
    }

    float3 litColor = effectiveEmissive + ambientTerm + diffuseAccum * effectiveDiffuse.rgb;
    litColor = saturate(litColor);
    float litAlpha = effectiveDiffuse.a;

    float3 unlitColor = lerp(float3(1, 1, 1), input.diffuse.rgb, hasVertexColor);
    float unlitAlpha = lerp(1.0f, input.diffuse.a, hasVertexColor);

    output.diffuse.rgb = lerp(unlitColor, litColor, lightingEnabled);
    output.diffuse.a = lerp(unlitAlpha, litAlpha, lightingEnabled);

    output.uv0 = input.uv0;
    output.uv1 = worldPos.xy * g_CloudParams.y + g_CloudParams.zw;
    output.worldNormal = worldNormal;
    output.worldPos = worldPos;

    return output;
}
