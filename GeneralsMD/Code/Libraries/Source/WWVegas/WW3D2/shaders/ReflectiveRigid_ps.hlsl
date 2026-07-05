// Modern per-pixel environment reflection for the reflective pass (pass 0) of multi-pass
// rigid surfaces. Replaces the FFP per-vertex env mapper. Outputs the reflection opaquely;
// the existing pass-1 alpha-diffuse overlay composites it: lerp(reflection, diffuse, diffuse.a).
// Compile: fxc /T ps_3_0 /Fo ReflectiveRigid.pso ReflectiveRigid_ps.hlsl
sampler2D g_EnvSampler : register(s0); // this pass's stage-0 texture (lakedusk)
float4    g_CameraPos  : register(c0); // world-space camera position (xyz)
float4x4  g_View       : register(c1); // world->view; uploaded transposed (mirrors the VS g_ViewProj convention)

struct PS_INPUT {
    float4 col         : COLOR0;
    float2 uv0         : TEXCOORD0;
    float2 uv1         : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
    float3 worldPos    : TEXCOORD3;
};

float4 main(PS_INPUT i) : COLOR0
{
    float3 N = normalize(i.worldNormal);
    float3 V = normalize(i.worldPos - g_CameraPos.xyz); // camera -> surface
    float3 R = reflect(V, N);

    // Sphere-map in CAMERA space (view-relative), matching FFP TCI_CAMERASPACEREFLECTIONVECTOR.
    // World-space R.xy loses the reflection on camera-facing faces as the camera orbits.
    // If it looks mirrored/inverted as you orbit, flip the sign on Rv.xy below.
    float3 Rv = mul(float4(R, 0.0f), g_View).xyz;
    float2 envUV = Rv.xy * 0.5f + 0.5f;

    // Modulate the reflection by the VS lit vertex color so it sits at scene brightness like every
    // other surface (texture x lighting). Emitting it full-bright made it ignore lighting, so the
    // shroud/fog multiply (which DOES reach this mesh) left it the brightest object -> "stays bright
    // in fog". With the lighting factor it now dims with the scene and darkens under the shroud like
    // its neighbors, matching how the FFP env pass composited.
    return float4(tex2D(g_EnvSampler, envUV).rgb * i.col.rgb, 1.0f);

    // PBR/Fresnel extension point (later): weight by pow(1 - saturate(dot(-V,N)), 5).
}
