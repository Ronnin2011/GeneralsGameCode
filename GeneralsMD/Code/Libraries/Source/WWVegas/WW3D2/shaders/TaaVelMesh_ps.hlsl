// Ronin @feature 24/09/2026 DX9: TAA motion vectors drawn FROM THE MESH - the pixel half. docs/AntiAliasing_Work.md §14.
//
// Two jobs. First, only the VISIBLE surface may write: this pass draws with no depth buffer, so a fragment is
// discarded when it lies behind the scene depth at its pixel. That is the ownership test the rectangle approach never
// had - the geometry itself says which pixels are the unit, and ground is never touched.
// Second, write the screen-space velocity (current minus previous, in pixels, screen Y down) at 12 bits per axis.
// 8 bits over +/-64 px is 0.5 px a step, and a velocity error compounds roughly x9 through a 0.9 history.
//
// Packing: x and y each become 0..4095. R = x >> 4, G = y >> 4, B = (x & 15) << 4 | (y & 15), A = 1 = written.
// The resolve decodes the exact inverse. Point-sampled only: a filtered packed value is meaningless.
//
// Compile with: fxc /T ps_3_0 /Fo TaaVelMesh.pso TaaVelMesh_ps.hlsl   (pairs with TaaVelMesh.vso)

sampler2D g_Depth : register(s2);   // INTZ scene depth, this frame

// Ronin @bugfix 24/09/2026 DX9: c10/c11, NOT c0/c1. This pass runs BEFORE the resolve's pass 1, and the resolve keeps
// g_Reproject at c1..c4 - uploaded earlier and never again - so using c1 corrupted the reprojection for the whole
// screen: the second time this exact collision happened (TaaVelocity_ps, 23/09). Anything drawn before pass 1 must
// stay out of the resolve's c0..c9.
float4 g_VpPx  : register(c10);     // x,y = 3D viewport size in px; z = zNear, w = zFar
float4 g_VpUv  : register(c11);     // xy = viewport origin in frame-buffer UV, zw = viewport size in frame-buffer UV
// Ronin @feature 24/09/2026 DX9: reactive mask. x = alpha out (1 velocity, 0.7 velocity + reactive, 0.4 reactive),
// y = depth test: 0 two-sided, 1 one-sided (translucent: writes no depth), 2 none (the placement bib quad);
// z = write zero velocity.
float4 g_Kind  : register(c12);
// Ronin @bugfix 09/10/2026 DX9: this mesh's selection flash (rgb, a = 1 while it flashes), written to a second target when one is
// bound: the screen copy adds the flash there, after the resolve.
float4 g_Flash : register(c13);

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

PS_OUTPUT main(float4 cur : TEXCOORD0, float4 prev : TEXCOORD1)
{
    float2 nc = cur.xy  / cur.w;
    float2 np = prev.xy / prev.w;

    // Visible surface only. The fragment's own depth against the scene's at this pixel; a small tolerance absorbs
    // the sub-pixel jitter between the scene render and this clean-projection redraw.
    float2 uv      = g_VpUv.xy + float2(nc.x * 0.5f + 0.5f, 0.5f - nc.y * 0.5f) * g_VpUv.zw;
    float  sceneZ  = linearise(tex2Dlod(g_Depth, float4(uv, 0.0f, 0.0f)).r);
    float  fragZ   = linearise(cur.z / cur.w);
    // Ronin @bugfix 24/09/2026 DX9: TWO-SIDED. Write velocity only where this mesh IS the depth surface. Rejecting only
    // fragments BEHIND the scene let a mesh that writes no depth - a helicopter's translucent rotor - stamp its
    // spinning velocity over the whole factory under it, which then fetched history from the wrong place and fell back
    // to the raw frame: the sharp, un-antialiased patch under the blades. A fragment much NEARER than the scene is
    // over something else, not part of it. At silhouettes this is also more correct: where the jittered scene shows
    // background, the background owns the pixel.
    float  tolZ    = max(sceneZ * 0.005f, 0.5f);
    if (g_Kind.y > 1.5f)
        ;		// Ronin @bugfix 26/09/2026 DX9: no test - a flat quad over terrain that need not be flat
    else if (g_Kind.y > 0.5f)
        clip(sceneZ + tolZ - fragZ);		// translucent: anything not behind the scene is visible
    else
        clip(tolZ - abs(fragZ - sceneZ));

    // ndc -> pixels; ndc Y is up, screen Y down.
    float2 v = float2((nc.x - np.x) * 0.5f * g_VpPx.x,
                      (np.y - nc.y) * 0.5f * g_VpPx.y);
    if (g_Kind.z > 0.5f)
        v = 0.0f;

    float2 e  = floor(saturate(v / (2.0f * VEL_RANGE) + 0.5f) * 4095.0f + 0.5f);
    float2 hi = floor(e / 16.0f);
    float2 lo = e - hi * 16.0f;
    PS_OUTPUT o;
    o.velocity = float4(hi.x / 255.0f, hi.y / 255.0f, (lo.x * 16.0f + lo.y) / 255.0f, g_Kind.x);
    o.flash    = g_Flash;
    return o;
}
