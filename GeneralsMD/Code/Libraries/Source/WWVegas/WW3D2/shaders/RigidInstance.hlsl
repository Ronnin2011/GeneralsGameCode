// Ronin @feature 18/02/2026 DX9: Hardware instancing vertex shader for rigid meshes
// Ronin @bugfix 21/02/2026 DX9: Match FFP lighting model (material ambient, multi-light)
// Ronin @bugfix 21/02/2026 DX9: Respect D3DRS_DIFFUSEMATERIALSOURCE / AMBIENTMATERIALSOURCE
// Ronin @bugfix 22/02/2026 DX9: Fix light direction convention — InputLights points toward light
//
// Compile with: fxc /T vs_3_0 /Fo RigidInstance.vso RigidInstance.hlsl
//
// Constant registers:
//   c0..c3  = ViewProjection matrix (transposed for mul(vector, matrix))
//   c4      = Ambient light color (RGB) from LightEnv OutputAmbient
//   c5      = Light0 direction (world-space, pointing TOWARD light source)
//   c6      = Light0 diffuse color (RGB)
//   c7      = Material diffuse color (RGBA)
//   c8      = Material emissive color (RGB)
//   c9      = Flags: (lightingEnabled, hasVertexColor, numLights, 0)
//   c10     = Material ambient color (RGB)
//   c11     = Light1 direction (world-space, pointing TOWARD light source)
//   c12     = Light1 diffuse color (RGB)
//   c13     = Material source flags: (diffuseSrcVertex, ambientSrcVertex, emissiveSrcVertex, 0)

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
    float4 diffuse : COLOR0;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;

    float4 worldRow0 : TEXCOORD4;
    float4 worldRow1 : TEXCOORD5;
    float4 worldRow2 : TEXCOORD6;
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
    float hasVertexColor = g_Flags.y;
    float numLights = g_Flags.z;

    // Material source selection flags (1.0 = use vertex color, 0.0 = use material)
    float diffuseSrcVertex = g_MatSrcFlags.x;
    float ambientSrcVertex = g_MatSrcFlags.y;
    float emissiveSrcVertex = g_MatSrcFlags.z;

    // Select diffuse color based on D3DRS_DIFFUSEMATERIALSOURCE
    float4 effectiveDiffuse = lerp(g_MatDiffuse, input.diffuse, diffuseSrcVertex * hasVertexColor);

    // Select ambient color based on D3DRS_AMBIENTMATERIALSOURCE
    float3 effectiveAmbient = lerp(g_MatAmbient.rgb, input.diffuse.rgb, ambientSrcVertex * hasVertexColor);

    // Select emissive color based on D3DRS_EMISSIVEMATERIALSOURCE
    float3 effectiveEmissive = lerp(g_MatEmissive.rgb, input.diffuse.rgb, emissiveSrcVertex * hasVertexColor);

    // FFP lighting model:
    //   color = emissive + ambient * ambientLight
    //         + sum_i( max(0, dot(N, L_i)) * lightDiffuse_i ) * diffuse
    //
    // g_LightDir0/1 point TOWARD the light (from LightEnvironmentClass::InputLights).
    // dot(N, toLight) > 0 means the surface faces the light. No negation needed.

    // Ambient term: effectiveAmbient * globalAmbientLight
    float3 ambientTerm = effectiveAmbient * g_AmbientLight.rgb;

    // Diffuse term: accumulate per-light contributions
    // Ronin @bugfix 22/02/2026 DX9: Remove negation — InputLights.Direction already points toward light
    float NdotL0 = max(0.0f, dot(worldNormal, g_LightDir0.xyz));
    float3 diffuseAccum = NdotL0 * g_LightDiffuse0.rgb;

    // Second light (if present)
    if (numLights > 1.0f)
    {
        float NdotL1 = max(0.0f, dot(worldNormal, g_LightDir1.xyz));
        diffuseAccum += NdotL1 * g_LightDiffuse1.rgb;
    }

    float3 litColor = effectiveEmissive + ambientTerm + diffuseAccum * effectiveDiffuse.rgb;
    litColor = saturate(litColor);
    float litAlpha = effectiveDiffuse.a;

    // Unlit path: use vertex color if available, else white
    float3 unlitColor = lerp(float3(1, 1, 1), input.diffuse.rgb, hasVertexColor);
    float unlitAlpha = lerp(1.0f, input.diffuse.a, hasVertexColor);

    output.diffuse.rgb = lerp(unlitColor, litColor, lightingEnabled);
    output.diffuse.a = lerp(unlitAlpha, litAlpha, lightingEnabled);

    output.uv0 = input.uv0;
    output.uv1 = input.uv1;

    return output;
}
