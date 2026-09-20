// Ronin @feature 13/09/2026 DX9: SSAO step 1. Debug view of the scene depth (INTZ). Grey = distance from the camera,
// black near to white at g_DepthParams.z world units. RED = depth still at the clear value: nothing wrote there.
//
// Compile with: fxc /T ps_3_0 /Fo SceneDepthDebug.pso SceneDepthDebug_ps.hlsl   (pairs with ScreenQuad.vso)

sampler2D g_SceneDepth : register(s0);
float4 g_DepthParams : register(c0);    // x = camera near, y = camera far, z = grey range in world units

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float d = tex2D(g_SceneDepth, uv).r;
    float n = g_DepthParams.x;
    float f = g_DepthParams.y;

    // D3D perspective depth is d = f/(f-n) * (1 - n/z) (camera.cpp Get_D3D_Projection_Matrix), so z = n*f / (f - d*(f-n)).
    float z = (n * f) / max(f - d * (f - n), 0.0001f);
    float g = saturate(z / g_DepthParams.z);

    float cleared = step(0.99999f, d);
    return float4(lerp(float3(g, g, g), float3(1.0f, 0.0f, 0.0f), cleared), 1.0f);
}
