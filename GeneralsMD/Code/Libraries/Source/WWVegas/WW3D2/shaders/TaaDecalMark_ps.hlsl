// Ronin @bugfix 27/10/2026 DX9: a decal's own texels in the TAA velocity target, with the decal's motion, in TaaVelMesh_ps's
// encoding. Moving, its history follows it (a ghost reset made it shake); marked, the moving-shadow mask leaves it alone.
//
// Compile with: fxc /T ps_3_0 /Fo TaaDecalMark.pso TaaDecalMark_ps.hlsl

sampler2D g_Decal : register(s0);   // the decal's texture
sampler2D g_Depth : register(s2);   // INTZ scene depth, this frame (bound by the velocity pass)

float4 g_VpPx  : register(c10);     // x,y = 3D viewport size in px; z = zNear, w = zFar
float4 g_VpUv  : register(c11);     // xy = viewport origin in frame-buffer UV, zw = viewport size in frame-buffer UV
float4 g_Kind  : register(c12);     // x = the velocity alpha written; w = 1 for an additive decal (its shape is in its colour)

struct PS_OUTPUT
{
    float4 velocity : COLOR0;
    float4 flash    : COLOR1;
};

static const float VEL_RANGE = 64.0f;

float linearise(float raw)
{
    return (g_VpPx.z * g_VpPx.w) / max(g_VpPx.w - raw * (g_VpPx.w - g_VpPx.z), 0.0001f);
}

PS_OUTPUT main(float2 uv : TEXCOORD0, float4 cur : TEXCOORD1, float4 prev : TEXCOORD2)
{
    float4 t     = tex2D(g_Decal, uv);
    float  cover = (g_Kind.w > 0.5f) ? max(t.r, max(t.g, t.b)) : t.a;
    clip(cover - 0.1f);

    float2 nc = cur.xy  / cur.w;
    float2 np = prev.xy / prev.w;

    // Only where the ground under the decal is what shows: a unit standing on it keeps its own pixels. One-sided, with room for
    // the ground between the grid's points.
    float2 suv    = g_VpUv.xy + float2(nc.x * 0.5f + 0.5f, 0.5f - nc.y * 0.5f) * g_VpUv.zw;
    float  sceneZ = linearise(tex2Dlod(g_Depth, float4(suv, 0.0f, 0.0f)).r);
    float  fragZ  = linearise(cur.z / cur.w);
    clip(sceneZ + max(sceneZ * 0.01f, 2.0f) - fragZ);

    float2 v  = float2((nc.x - np.x) * 0.5f * g_VpPx.x,
                       (np.y - nc.y) * 0.5f * g_VpPx.y);
    float2 e  = floor(saturate(v / (2.0f * VEL_RANGE) + 0.5f) * 4095.0f + 0.5f);
    float2 hi = floor(e / 16.0f);
    float2 lo = e - hi * 16.0f;
    PS_OUTPUT o;
    o.velocity = float4(hi.x / 255.0f, hi.y / 255.0f, (lo.x * 16.0f + lo.y) / 255.0f, g_Kind.x);
    o.flash    = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return o;
}
