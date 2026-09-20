// Ronin @feature 13/09/2026 DX9: SSAO step 2. Raw ambient occlusion from the scene depth alone — no normals buffer.
// Normal-oriented hemisphere: rebuild the view-space position and normal, then count kernel samples buried behind the
// depth buffer. White = open, black = occluded. Unblurred on purpose: this step verifies the maths.
//
// Compile with: fxc /T ps_3_0 /Fo SsaoRaw.pso SsaoRaw_ps.hlsl   (pairs with ScreenQuad.vso)

sampler2D g_SceneDepth : register(s0);      // INTZ, POINT, CLAMP

float4 g_AOParams   : register(c0);         // x = camera near, y = camera far, z = radius (world units), w = intensity
float4 g_ViewPlane  : register(c1);         // x = view-plane min X, y = view-plane max Y, z = width, w = height, at depth 1
float4 g_Viewport   : register(c2);         // xy = 3D viewport min, zw = its size — both 0..1 of the render target
float4 g_TexelSize  : register(c3);         // xy = 1 / target size, zw = target size
// Ronin @feature 20/09/2026 DX9: SSAO quality tiers. STRIDE, not a prefix: the kernel's lengths grow with index, so the
// first N entries would all be close-contact and the tier would lose wide occlusion instead of just gaining noise.
float4 g_AOQuality  : register(c4);         // x = sample count, y = kernel stride, z = 1 / sample count

// Hemisphere kernel, tangent space (+z = along the normal). Golden-angle azimuths, lengths growing toward the rim so
// most samples test close contact.
static const float3 KERNEL[12] =
{
    float3( 0.087f,  0.000f, 0.050f), float3(-0.047f,  0.043f, 0.085f), float3( 0.010f, -0.119f, 0.038f),
    float3( 0.041f,  0.054f, 0.140f), float3(-0.181f, -0.032f, 0.080f), float3( 0.154f, -0.098f, 0.179f),
    float3(-0.082f,  0.304f, 0.081f), float3(-0.150f, -0.288f, 0.244f), float3( 0.441f,  0.160f, 0.175f),
    float3(-0.468f,  0.194f, 0.333f), float3( 0.162f, -0.346f, 0.616f), float3( 0.230f,  0.729f, 0.385f)
};

// Distance from the camera along the view axis. D3D depth d = f/(f-n) * (1 - n/z), camera.cpp Get_D3D_Projection_Matrix.
float linearDepth(float2 uv)
{
    float d = tex2Dlod(g_SceneDepth, float4(uv, 0.0f, 0.0f)).r;
    float n = g_AOParams.x;
    float f = g_AOParams.y;
    return (n * f) / max(f - d * (f - n), 0.0001f);
}

// W3D view space: camera looks down -Z, +Y up. The screen's Y grows downward, the view plane's upward.
float3 viewPosition(float2 uv)
{
    float  e = linearDepth(uv);
    float2 s = (uv - g_Viewport.xy) / g_Viewport.zw;
    float  px = g_ViewPlane.x + s.x * g_ViewPlane.z;
    float  py = g_ViewPlane.y - s.y * g_ViewPlane.w;
    return float3(px * e, py * e, -e);
}

float2 viewToUV(float3 p)
{
    float  e = max(-p.z, 0.0001f);
    float2 s = float2((p.x / e - g_ViewPlane.x) / g_ViewPlane.z,
                      (g_ViewPlane.y - p.y / e) / g_ViewPlane.w);
    return g_Viewport.xy + s * g_Viewport.zw;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    // Outside the 3D viewport (the command bar) there is no scene: RED, as in the depth view.
    // Ronin @bugfix 13/09/2026 DX9: SSAO step 2. Plain comparisons — `s != saturate(s)` made fxc compute s twice (mul, then
    // mad), and when the viewport is not full-screen the two round apart: red on most rows in a match, stripes on the rest.
    float2 s = (uv - g_Viewport.xy) / g_Viewport.zw;
    if (s.x < 0.0f || s.x > 1.0f || s.y < 0.0f || s.y > 1.0f)
        return float4(1.0f, 0.0f, 0.0f, 1.0f);

    // Ronin @bugfix 13/09/2026 DX9: SSAO step 3a. Snap to ONE depth texel. At half resolution every pixel centre sits exactly
    // on a full-resolution texel border, and point sampling picked a different side on different rows: horizontal bands.
    float2 uvT = (floor(uv * g_TexelSize.zw + 0.25f) + 0.5f) * g_TexelSize.xy;

    // Nothing drew here (sky, off-map): fully open.
    if (tex2Dlod(g_SceneDepth, float4(uvT, 0.0f, 0.0f)).r >= 0.99999f)
        return float4(1.0f, 1.0f, 1.0f, 1.0f);

    // NORMAL from neighbouring depth texels, taking the smaller step on each axis so an edge does not bend it.
    float3 P  = viewPosition(uvT);
    float3 Pr = viewPosition(uvT + float2(g_TexelSize.x, 0.0f));
    float3 Pl = viewPosition(uvT - float2(g_TexelSize.x, 0.0f));
    float3 Pd = viewPosition(uvT + float2(0.0f, g_TexelSize.y));
    float3 Pu = viewPosition(uvT - float2(0.0f, g_TexelSize.y));
    float3 dx = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? (Pr - P) : (P - Pl);
    float3 dy = (abs(Pd.z - P.z) < abs(P.z - Pu.z)) ? (Pd - P) : (P - Pu);
    float3 N  = normalize(cross(dy, dx));
    if (dot(N, -P) < 0.0f)
        N = -N;                             // face the camera

    // Per-pixel rotation of the kernel about the normal — trades banding for noise, which step 3's blur removes.
    float2 pix   = floor(uvT * g_TexelSize.zw);

    float  angle = frac(sin(dot(pix, float2(12.9898f, 78.233f))) * 43758.5453f) * 6.2831853f;
    float3 rnd   = float3(cos(angle), sin(angle), 0.0f);
    float3 T     = normalize(rnd - N * dot(rnd, N));
    float3 B     = cross(N, T);

    const float radius = g_AOParams.z;
    const float bias   = radius * 0.02f;
    const float eP     = -P.z;

    float occlusion = 0.0f;
    const int sampleCount  = (int)g_AOQuality.x;
    const int kernelStride = (int)g_AOQuality.y;
    [loop]
    for (int i = 0; i < sampleCount; ++i)
    {
        float3 k       = KERNEL[i * kernelStride];
        float3 S       = P + (T * k.x + B * k.y + N * k.z) * radius;
        float  sceneE  = linearDepth(viewToUV(S));
        float  sampleE = -S.z;
        // Only geometry within the radius counts, or a tower far in front would darken the ground behind it.
        float  inRange = saturate(radius / max(abs(eP - sceneE), 0.0001f));
        occlusion += ((sceneE <= sampleE - bias) ? 1.0f : 0.0f) * inRange;
    }

    float ao = saturate(1.0f - (occlusion * g_AOQuality.z) * g_AOParams.w);
    return float4(ao, ao, ao, 1.0f);
}
