// Modern per-pixel environment reflection for the reflective pass (pass 0) of multi-pass
// rigid surfaces. Replaces the FFP per-vertex env mapper. Outputs the reflection opaquely;
// the existing pass-1 alpha-diffuse overlay composites it: lerp(reflection, diffuse, diffuse.a).
// Compile: fxc /T ps_3_0 /Fo ReflectiveRigid.pso ReflectiveRigid_ps.hlsl
sampler2D g_EnvSampler : register(s0); // this pass's stage-0 texture (lakedusk)
float4    g_CameraPos  : register(c0); // world-space camera position (xyz)
float4x4  g_View       : register(c1); // world->view; uploaded transposed (mirrors the VS g_ViewProj convention)
// Ronin @feature 12/10/2026 DX9: lighting step 5. The map's own sky (W3DSkyCube) by the WORLD reflection vector, in place of the
// art's one picture. g_SkyMode.x: 0 = the picture as before, 1 = the cube, 2 = the cube with the sky in every pane.
samplerCUBE g_SkyCube  : register(s6);
float4    g_SkyMode    : register(c6);
// Ronin @feature 18/10/2026 DX9: reflections of the ground. The lit ground map (W3DSkyCube: rgb = colour x its level light, halved;
// a = height) and its constants (Set_Reflection_Ground).
sampler2D g_GroundMap  : register(s7);
float4    g_GroundUV   : register(c12);  // xy = the map's corner in the world, zw = 1 / its size
float4    g_ReflKnobs  : register(c13);  // x = `reflectground`, y = `fresnel`
float4    g_GroundLit  : register(c14);  // rgb = the level light the map was lit with, w = its height span
float4    g_GroundZ    : register(c15);  // x = its lowest height, y = how far a reflection may run to the ground
// Ronin @feature 18/10/2026 DX9: screen-space reflections from LAST frame - TAA's history and its depth (Set_Screen_Reflection):
// their clean view-projection (c16..c19, transposed), where their 3D view sits in them, and the march's settings.
sampler2D g_PrevColour : register(s8);
sampler2D g_PrevDepth  : register(s9);  // linear depth over the far plane, packed into RGB (TaaDepthStore)
float4x4  g_PrevViewProj : register(c16);
float4    g_PrevView   : register(c20);  // xy = the 3D view's corner in those textures, zw = its size (0..1)
float4    g_Ssr        : register(c21);  // x = on, y = the far plane, z = how far a ray runs, w = how far behind a hit may end
// Ronin @feature 18/10/2026 DX9: the reflections' change mask - the frame before last (TAA's other history) and its camera. Where
// the hit changed between the two, COLOR1.r marks the pane for TAA (W3DTaa::beginReflectMask). c69..c74: no other shader's.
sampler2D g_OlderColour   : register(s10);
float4x4  g_OlderViewProj : register(c69);
float4    g_OlderView     : register(c73);  // xy = the 3D view's corner in it, zw = its size (0..1)
float4    g_ReflMark      : register(c74);  // x = the mask is bound, y = the older picture is there, z = least change, w = gain
// Ronin @feature 19/10/2026 DX9: the mover mark - last frame's velocity and the frame before's (TAA's; alpha = what drew there),
// bound by W3DTaa::beginReflectMask round this draw. A unit at the hit in either frame: COLOR1.b, and TAA restarts the pane.
sampler2D g_PrevVel       : register(s11);
sampler2D g_OlderVel      : register(s12);
float4    g_ReflVel       : register(c75);  // x = both are there, yz = one texel (uv)
// Ronin @bugfix 19/09/2026 DX9: this pass used to rely on the FFP shroud overlay reaching it (see the note below). That
// overlay is depth-compared and blackened alpha meshes at the fog edge, so it no longer runs on programmable meshes — the
// shroud is sampled here instead, same meaning as in RigidInstance_ps. zw == 0 means the map has no shroud.
sampler2D g_ShroudSampler : register(s5);
float4    g_ShroudMap  : register(c5);  // xy = world offset, zw = 1/(cellSize * textureSize)

struct PS_INPUT {
    float4 col         : COLOR0;
    float2 uv0         : TEXCOORD0;
    float2 uv1         : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
    float3 worldPos    : TEXCOORD3;
};

struct PS_OUTPUT {
    float4 color : COLOR0;
    float4 mark  : COLOR1;  // r = how much the pane's reflection changed (TAA's light mask; only red is written)
};

// Ronin @feature 18/10/2026 DX9: screen-space reflections - a world point in a frame's textures (xy) and its depth from that eye
// (z; <= 0: behind it), and the depth last frame drew at a place in them.
float3 framePoint(float3 Q, float4x4 viewProj, float4 view)
{
    float4 c   = mul(float4(Q, 1.0f), viewProj);
    float2 ndc = c.xy / max(c.w, 0.0001f);
    return float3(view.xy + float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * view.zw, c.w);
}
float3 prevPoint(float3 Q)
{
    return framePoint(Q, g_PrevViewProj, g_PrevView);
}
float prevDepth(float2 uv)
{
    return dot(tex2Dlod(g_PrevDepth, float4(uv, 0.0f, 0.0f)).rgb, float3(1.0f, 1.0f / 255.0f, 1.0f / 65025.0f)) * g_Ssr.y;
}
// Ronin @feature 19/10/2026 DX9: the mover mark - a unit (velocity: alpha > 0.5) or a build preview (0.3) on uv or one texel
// from it: the velocity's outline is unjittered, the picture's is not.
float moverNear(sampler2D vel, float2 uv)
{
    const float2 o[5] = { float2(0.0f, 0.0f), float2(g_ReflVel.y, 0.0f), float2(-g_ReflVel.y, 0.0f), float2(0.0f, g_ReflVel.z),
                          float2(0.0f, -g_ReflVel.z) };
    float m = 0.0f;
    [unroll]
    for (int k = 0; k < 5; ++k)
    {
        float a = tex2Dlod(vel, float4(uv + o[k], 0.0f, 0.0f)).a;
        m = max(m, (a > 0.5f || (a > 0.25f && a < 0.35f)) ? 1.0f : 0.0f);
    }
    return m;
}

PS_OUTPUT main(PS_INPUT i)
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
    // Ronin @feature 12/10/2026 DX9: lighting step 5. Both are read (no gradient inside a branch). Seen from above a pane mirrors
    // the ground, so mode 2 folds the vector up to the sky.
    float3 env = tex2D(g_EnvSampler, envUV).rgb;
    float3 sky = texCUBE(g_SkyCube, float3(R.xy, (g_SkyMode.x > 1.5f) ? abs(R.z) : R.z)).rgb;
    float3 seen = (g_SkyMode.x > 0.5f) ? sky : env;
    // Ronin @feature 18/10/2026 DX9: reflections of the ground - looking down, the ground it meets: from the pixel's height over the
    // ground (map alpha) along the reflection, the map's colour there over its level light (its own colour). Faded in at the horizon.
    [branch]
    if (g_ReflKnobs.x > 0.0f && R.z < 0.0f)
    {
        float  under  = g_GroundZ.x + tex2Dlod(g_GroundMap, float4((i.worldPos.xy - g_GroundUV.xy) * g_GroundUV.zw, 0.0f, 0.0f)).a * g_GroundLit.w;
        float  run    = min(max(i.worldPos.z - under, 0.0f) / max(-R.z, 0.02f), g_GroundZ.y);
        float2 hit    = (i.worldPos.xy + R.xy * run - g_GroundUV.xy) * g_GroundUV.zw;
        float3 ground = tex2Dlod(g_GroundMap, float4(hit, 0.0f, 0.0f)).rgb * 2.0f / max(g_GroundLit.rgb, 0.02f);
        seen = lerp(seen, saturate(ground), g_ReflKnobs.x * saturate(-R.z * 20.0f));
    }
    float3 reflection = seen * i.col.rgb;
    float  changed    = 0.0f;   // Ronin @feature 18/10/2026 DX9: how much of it changed since the frame before (the change mask)
    float  moved      = 0.0f;   // Ronin @feature 19/10/2026 DX9: 1 = a unit was at the hit (the mover mark)
    // Ronin @feature 18/10/2026 DX9: screen-space reflections - what the reflection meets in last frame's picture: 16 widening steps
    // through its depth, then 4 halvings. Lit already: not x the vertex light. Off screen, hidden or too far behind: the one above.
    [branch]
    if (g_Ssr.x > 0.0f)
    {
        float t0 = 0.0f, t1 = 0.0f, met = 0.0f;
        [loop]
        for (int s = 1; s <= 16; ++s)
        {
            t1 = g_Ssr.z * (float)(s * s) / 256.0f;
            float3 p = prevPoint(i.worldPos + R * t1);
            if (p.z <= 0.0f || p.x < g_PrevView.x || p.y < g_PrevView.y || p.x > g_PrevView.x + g_PrevView.z ||
                p.y > g_PrevView.y + g_PrevView.w)
                break;
            if (p.z > prevDepth(p.xy))
            {
                met = 1.0f;
                break;
            }
            t0 = t1;
        }
        [branch]
        if (met > 0.0f)
        {
            [unroll]
            for (int h = 0; h < 4; ++h)
            {
                float  tm = 0.5f * (t0 + t1);
                float3 q  = prevPoint(i.worldPos + R * tm);
                if (q.z > prevDepth(q.xy))
                    t1 = tm;
                else
                    t0 = tm;
            }
            float3 hitQ = i.worldPos + R * t1;
            float3 hitP = prevPoint(hitQ);
            float2 edge = min(hitP.xy - g_PrevView.xy, g_PrevView.xy + g_PrevView.zw - hitP.xy) / g_PrevView.zw;
            float  take = (hitP.z - prevDepth(hitP.xy) < g_Ssr.w) ? saturate(min(edge.x, edge.y) * 10.0f) : 0.0f;
            float3 hitC = tex2Dlod(g_PrevColour, float4(hitP.xy, 0.0f, 0.0f)).rgb;
            reflection = lerp(reflection, hitC, take);
            // Ronin @feature 18/10/2026 DX9: the change mask - the same world point in the frame before. Off that frame, or no such
            // frame: taken as changed.
            [branch]
            if (g_ReflMark.x > 0.0f && take > 0.0f)
            {
                float3 oldP  = framePoint(hitQ, g_OlderViewProj, g_OlderView);
                float  diff  = 1.0f;
                bool   onOld = g_ReflMark.y > 0.0f && oldP.z > 0.0f && all(oldP.xy >= g_OlderView.xy) &&
                               all(oldP.xy <= g_OlderView.xy + g_OlderView.zw);
                if (onOld)
                {
                    float3 d = abs(hitC - tex2Dlod(g_OlderColour, float4(oldP.xy, 0.0f, 0.0f)).rgb);
                    diff = max(d.r, max(d.g, d.b));
                }
                changed = take * diff;
                // Ronin @feature 19/10/2026 DX9: the mover mark - a unit at the hit last frame, or in the frame before (it left).
                [branch]
                if (g_ReflVel.x > 0.0f)
                    moved = max(moverNear(g_PrevVel, hitP.xy), onOld ? moverNear(g_OlderVel, oldP.xy) : 0.0f);
            }
        }
    }
    // Ronin @feature 18/10/2026 DX9: `fresnel` - a reflection stronger where a pane turns from the eye, weaker facing it (the physical
    // curve hardly moves under a camera looking down).
    float lift = max(0.0f, 1.0f + g_ReflKnobs.y * 1.5f * ((1.0f - saturate(dot(N, -V))) - 0.3f));
    reflection *= lift;
    changed    *= lift;

    // Ronin @bugfix 19/09/2026 DX9: shroud in-shader now that the overlay pass no longer reaches programmable meshes.
    if (g_ShroudMap.z > 0.0f)
    {
        float2 shroudUV = (i.worldPos.xy + g_ShroudMap.xy) * g_ShroudMap.zw;
        float3 shroud   = tex2D(g_ShroudSampler, shroudUV).rgb;
        reflection *= shroud;
        changed    *= max(shroud.r, max(shroud.g, shroud.b));
    }

    // Ronin @feature 18/10/2026 DX9: the change mask (COLOR1), by how much the pane visibly changed (target 1: TAA's mask, or none).
    PS_OUTPUT o;
    o.color = float4(reflection, 1.0f);
    float m = saturate((changed - g_ReflMark.z) * g_ReflMark.w);
    o.mark  = float4(m, 0.0f, m * moved, 0.0f);	// Ronin @feature 19/10/2026 DX9: blue = the mover mark
    return o;

    // PBR/Fresnel extension point (later): weight by pow(1 - saturate(dot(-V,N)), 5).
}
