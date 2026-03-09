// Ronin @bugfix 07/03/2026 DX9: Hardware instancing vertex shader variant for
// rigid meshes whose geometry FVF does NOT provide D3DFVF_DIFFUSE / COLOR0.
// Compile with: fxc /T vs_3_0 /Fo RigidInstance_NoColor.vso RigidInstance_NoColor.hlsl

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

struct VS_INPUT
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
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
    float numLights = g_Flags.z;

    float3 ambientTerm = g_MatAmbient.rgb * g_AmbientLight.rgb;

    float NdotL0 = max(0.0f, dot(worldNormal, g_LightDir0.xyz));
    float3 diffuseAccum = NdotL0 * g_LightDiffuse0.rgb;

    if (numLights > 1.0f)
    {
        float NdotL1 = max(0.0f, dot(worldNormal, g_LightDir1.xyz));
        diffuseAccum += NdotL1 * g_LightDiffuse1.rgb;
    }

    float3 litColor = g_MatEmissive.rgb + ambientTerm + diffuseAccum * g_MatDiffuse.rgb;
    litColor = saturate(litColor);
    float litAlpha = g_MatDiffuse.a;

    float3 unlitColor = g_MatDiffuse.rgb;
    float unlitAlpha = g_MatDiffuse.a;

    output.diffuse.rgb = lerp(unlitColor, litColor, lightingEnabled);
    output.diffuse.a = lerp(unlitAlpha, litAlpha, lightingEnabled);

    output.uv0 = input.uv0;
    output.uv1 = float2(0, 0);

    return output;
}
