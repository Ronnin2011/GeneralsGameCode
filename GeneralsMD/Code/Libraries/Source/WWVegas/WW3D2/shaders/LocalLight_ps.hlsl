// Ronin @feature 09/10/2026 DX9: local lights in screen space. One draw per light over the screen rectangle its sphere
// covers; position and facing come from the scene depth, the falloff is the engine's own (doTheDynamicLight).

// Ronin @build 09/10/2026 DX9: fxc /T ps_3_0 /Fo LocalLight.pso LocalLight_ps.hlsl   (pairs with ScreenQuad.vso)

sampler2D g_SceneDepth : register(s0);      // INTZ, POINT, CLAMP
sampler2D g_Frame      : register(s1);      // the frame as it stood before any light, POINT, CLAMP
sampler2D g_SunShade   : register(s2);      // what the sun's shadow left of each mesh pixel (RigidInstance_ps COLOR1); 0 = not written
sampler2D g_GroundShade : register(s3);     // the same for the terrain, as its own shadow pass composited it; white = none taken

float4 g_Camera     : register(c0);         // x = camera near, y = camera far, z = view mode, w = overbright
float4 g_ViewPlane  : register(c1);         // x = view-plane min X, y = view-plane max Y, z = width, w = height, at depth 1
float4 g_Viewport   : register(c2);         // xy = 3D viewport min, zw = its size - both 0..1 of the render target
float4 g_TexelSize  : register(c3);         // xy = 1 / target size, z = how much of the sun's shade the lights undo (0 = none known), w = how fast a lamp restores it
float4 g_SceneLight : register(c4);         // rgb = 1 / the map's own light on open ground: frame * this ~ surface colour; w = how much the lights' shadows take
float4 g_LightPos   : register(c5);         // xyz = the light in view space, w = 1 / (outer - inner)
float4 g_LightDiff  : register(c6);         // rgb = diffuse * intensity, w = inner radius
float4 g_LightAmb   : register(c7);         // rgb = ambient * intensity, w = the cone's edge: 1 / (cos inner - cos outer)
// Ronin @bugfix 09/10/2026 DX9: the same light as it was LAST frame (zero colour = it was not there), for TAA's change mask.
float4 g_PrevPos    : register(c8);         // xyz in THIS frame's view space, w = 1 / (outer - inner)
float4 g_PrevDiff   : register(c9);         // rgb = diffuse * intensity, w = inner radius
float4 g_PrevAmb    : register(c10);        // rgb = ambient * intensity, w = the cone's edge
// Ronin @feature 09/10/2026 DX9: a spot light (the beams, Lighting_Work.md 4.2): its axis in view space and the cosine of its outer half-angle.
// A point light uploads cosine -2 and edge 1, so its cone term is 1 everywhere.
float4 g_LightAxis  : register(c11);        // xyz = the axis, away from the lamp; w = cos outer
float4 g_PrevAxis   : register(c12);        // the same, last frame, in THIS frame's view space
float4 g_Shaft      : register(c13);        // the beam in the air: x = its strength now, y = last frame's (0 = none; below 0 = a light that casts no shadow); z = the spill's strength, w = 1 / its width
float4 g_Noise      : register(c15);        // x = this frame's shift of the shadow rays' dither (TAA averages it; below 0 = none), y = taps in a row a thing must cover to cast, z = how far towards the lamp the ray is walked
float4 g_Corona     : register(c14);        // a lamp's own glow: x = radius, y = strength; z, w = last frame's (0 = none). A beam: x, z = where its shaft ends

struct PS_OUTPUT
{
    float4 color : COLOR0;                  // added to the frame
    float4 mask  : COLOR1;                  // W3DTaa::beginLightMask - how much this light changed the pixel since last frame
};

// Ronin @feature 09/10/2026 DX9: W3D view space: camera looks down -Z, +Y up. Same reconstruction as SsaoRaw_ps.
float3 viewPosition(float2 uv)
{
    float  d = tex2Dlod(g_SceneDepth, float4(uv, 0.0f, 0.0f)).r;
    float  n = g_Camera.x;
    float  f = g_Camera.y;
    float  e = (n * f) / max(f - d * (f - n), 0.0001f);
    float2 s = (uv - g_Viewport.xy) / g_Viewport.zw;
    float  px = g_ViewPlane.x + s.x * g_ViewPlane.z;
    float  py = g_ViewPlane.y - s.y * g_ViewPlane.w;
    return float3(px * e, py * e, -e);
}

static const int SHAFT_STEPS  = 6;
static const int SHADOW_STEPS = 32;

// Ronin @feature 10/10/2026 DX9: local lights cast shadows from what the frame itself shows: the line from the surface to the lamp is walked
// through the scene depth. 1 = the lamp is seen from here. A sample counts when it lies BEHIND the frame, but not far behind.
float lampSeen(float3 P, float3 N, float3 L, float2 uv, out float where)
{
    where = 0.0f;       // `lights view 4`: how far along the ray the longest run of covered taps began
    float3 start = P + N * (0.25f - 0.0015f * P.z);
    float3 d     = L - start;
    float  dist  = length(d);
    float  thick = max(3.0f, 0.25f * dist);
    // Ronin @feature 10/10/2026 DX9: each pixel starts its taps at its own place along the ray, moved every frame: fixed taps drew a post's
    // shadow as dashes (scratchpad light/ss_shadow_sim.py). Interleaved gradient noise.
    float2 px    = uv / g_TexelSize.xy + g_Noise.x;
    float  shift = (g_Noise.x < 0.0f) ? 0.5f : frac(52.9829189f * frac(0.06711056f * px.x + 0.00583715f * px.y));
    // Ronin @feature 10/10/2026 DX9: a thing casts only if it covers g_Noise.y taps in a row: fences, poles and the flag caught one tap here
    // and there, which was the speckle round them. At 32 taps a vehicle still covers several.
    float  run   = 0.0f;
    float  occ   = 0.0f;
    for (int i = 0; i < SHADOW_STEPS; ++i)
    {
        float  t    = (i + shift) * (g_Noise.z / SHADOW_STEPS);          // not the last stretch: the lamp's own housing
        float3 S    = start + d * t;
        float  e    = max(-S.z, 0.001f);
        float2 s    = float2((S.x / e - g_ViewPlane.x) / g_ViewPlane.z, (g_ViewPlane.y - S.y / e) / g_ViewPlane.w);
        float  raw  = tex2Dlod(g_SceneDepth, float4(g_Viewport.xy + s * g_Viewport.zw, 0.0f, 0.0f)).r;
        float  se   = (g_Camera.x * g_Camera.y) / max(g_Camera.y - raw * (g_Camera.y - g_Camera.x), 0.0001f);
        float  diff = e - se;
        float  in01 = (s.x > 0.0f && s.x < 1.0f && s.y > 0.0f && s.y < 1.0f) ? 1.0f : 0.0f;    // off the view: nothing known
        float  hit  = (diff > 0.15f + 0.0015f * e + 0.01f * t * dist && diff < thick) ? in01 : 0.0f;
        run = hit * (run + hit);
        where = (run > occ) ? (t - (run - 1.0f) * (g_Noise.z / SHADOW_STEPS)) / g_Noise.z : where;
        occ = max(occ, run);
    }
    return 1.0f - saturate(occ - (g_Noise.y - 1.0f));
}

// Ronin @bugfix 09/10/2026 DX9: every falloff ends through this - no slope and no curvature where the light runs out. A ramp
// that stops dead reads as a dark band round the pool, and the sharpening deepens it.
float ease(float x)
{
    return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
}

// Ronin @feature 09/10/2026 DX9: a spot's cone: the art's at full strength, inside a dim, wider spill (g_Shaft.zw) - a lamp's light does not
// stop at a line (a hard disc showed on whatever a headlight touched). A point light (cosine -2) comes out as 1.
float coneTerm(float cosA, float4 axis, float edge)
{
    float core  = ease(saturate((cosA - axis.w) * edge));
    float c     = max(axis.w, 0.001f);
    float ca    = max(cosA, 0.001f);
    float u     = sqrt((max(1.0f - ca * ca, 0.0f) * c * c) / max((1.0f - c * c) * ca * ca, 0.000001f));    // tangent off the axis, in cone widths
    // Ronin @bugfix 09/10/2026 DX9: the spill is ONE curve from the axis to its end, under the core. As a second S-curve it began flat
    // exactly where the core ended: pool, shelf, edge - which read as a dark ring round the pool.
    float spill = (1.0f - ease(saturate(u * g_Shaft.w))) * step(0.0f, cosA);
    return (axis.w < -1.5f) ? 1.0f : lerp(spill * g_Shaft.z, 1.0f, core);
}

// Ronin @feature 09/10/2026 DX9: the beam in the air, with no cone texture: how much of the line of sight to this pixel lies inside the
// spot's cone before it meets the surface, weighted by the light there. Camera at the origin; entry and exit are exact.
float shaft(float3 P, float3 lamp, float end, float4 axis, float edge)
{
    float  tMax  = length(P);
    float3 v     = P / max(tMax, 0.001f);
    float  outer = end;                 // its own end, not the light's: the light on surfaces runs further
    float  inner = 0.5f * end;
    float  span  = 2.0f / max(end, 0.001f);
    float3 co    = -lamp;
    float  c2    = axis.w * axis.w;
    float  vd    = dot(v, axis.xyz);
    float  cod   = dot(co, axis.xyz);
    float  vco   = dot(v, co);
    float  coco  = dot(co, co);
    float  a     = vd * vd - c2;
    float  b     = 2.0f * (vd * cod - c2 * vco);
    float  c     = cod * cod - c2 * coco;
    a = (abs(a) < 0.00001f) ? ((a >= 0.0f) ? 0.00001f : -0.00001f) : a;
    float  disc  = b * b - 4.0f * a * c;
    float  sq    = sqrt(max(disc, 0.0f));
    float  r0    = (-b - sq) / (2.0f * a);
    float  r1    = (-b + sq) / (2.0f * a);
    float  t0    = min(r0, r1);
    float  t1    = max(r0, r1);
    float  ok    = (disc > 0.0f) ? 1.0f : 0.0f;
    if (a < 0.0f)
        ok *= (cod + 0.5f * (t0 + t1) * vd > 0.0f) ? 1.0f : 0.0f;     // not the other half of the double cone
    else if (vd > 0.0f)
    {
        t0 = t1;                    // looking along the beam: inside from the far root on
        t1 = 1.0e6f;
    }
    else
    {
        t1 = t0;                    // looking into the beam: inside up to the near root
        t0 = -1.0e6f;
    }
    float  ds    = vco * vco - (coco - outer * outer);                 // and inside the light's reach
    ok *= (ds > 0.0f) ? 1.0f : 0.0f;
    float  sds   = sqrt(max(ds, 0.0f));
    t0 = max(t0, max(-vco - sds, 0.0f));
    t1 = min(t1, min(-vco + sds, tMax));
    ok *= (t1 > t0) ? 1.0f : 0.0f;
    // Ronin @feature 09/10/2026 DX9: brightest near the lamp: the light thins as 1/d^2 while the cone widens as d.
    float  s     = 0.12f * outer;
    float  tanO  = sqrt(max(1.0f - c2, 0.0f)) / max(axis.w, 0.001f);
    float  sum   = 0.0f;
    for (int i = 0; i < SHAFT_STEPS; ++i)
    {
        float  t     = t0 + (t1 - t0) * ((i + 0.5f) / SHAFT_STEPS);
        float3 to    = v * t - lamp;
        float  dist  = length(to);
        float  cone  = saturate((dot(to, axis.xyz) / max(dist, 0.001f) - axis.w) * edge);
        cone = cone * cone * (3.0f - 2.0f * cone);
        float  reach = saturate(1.0f - (dist - inner) * span);
        sum += cone * reach * (s * s) / (dist * dist + s * s);
    }
    return ok * (sum / SHAFT_STEPS) * (t1 - t0) / max(tanO * s, 0.0001f);
}

// Ronin @feature 09/10/2026 DX9: a lamp's own glow, with no texture: by how near the line of sight passes the lamp, so it is round from
// any side; gone where the surface at this pixel stands in front of the lamp.
float corona(float3 P, float3 lamp, float radius)
{
    float  tMax = length(P);
    float3 v    = P / max(tMax, 0.001f);
    float  t    = dot(lamp, v);
    float  r    = max(radius, 0.001f);
    float  g    = saturate(1.0f - length(lamp - v * t) / r);
    float  seen = saturate((tMax - t) / r + 1.0f);
    return g * g * seen * step(0.0f, t);
}

PS_OUTPUT main(float2 uv : TEXCOORD0)
{
    PS_OUTPUT o;
    o.mask = float4(0.0f, 0.0f, 0.0f, 0.0f);
    const int view = (int)(g_Camera.z + 0.5f);
    if (view == 3)
    {
        o.color = float4(0.10f, 0.03f, 0.0f, 0.0f);    // the rectangle each light costs
        return o;
    }

    clip(0.99999f - tex2Dlod(g_SceneDepth, float4(uv, 0.0f, 0.0f)).r);     // nothing drew here (sky, off-map)

    float3 P     = viewPosition(uv);
    float3 toL   = g_LightPos.xyz - P;
    float  dist  = length(toL);
    float  reach = ease(saturate(1.0f - (dist - g_LightDiff.w) * g_LightPos.w));
    reach *= coneTerm(dot(-toL / max(dist, 0.001f), g_LightAxis.xyz), g_LightAxis, g_LightAmb.w);
    float3 toP    = g_PrevPos.xyz - P;
    float  distP  = length(toP);
    float  reachP = ease(saturate(1.0f - (distP - g_PrevDiff.w) * g_PrevPos.w));
    reachP *= coneTerm(dot(-toP / max(distP, 0.001f), g_PrevAxis.xyz), g_PrevAxis, g_PrevAmb.w);

    float3 result  = float3(0.0f, 0.0f, 0.0f);
    float3 resultP = float3(0.0f, 0.0f, 0.0f);     // what the light added as it was last frame
    [branch]
    if (reach + reachP > 0.0f)
    {
        // Ronin @feature 09/10/2026 DX9: facing from the neighbouring depth texels, the smaller step on each axis so an edge does not bend it.
        float3 Pr = viewPosition(uv + float2(g_TexelSize.x, 0.0f));
        float3 Pl = viewPosition(uv - float2(g_TexelSize.x, 0.0f));
        float3 Pd = viewPosition(uv + float2(0.0f, g_TexelSize.y));
        float3 Pu = viewPosition(uv - float2(0.0f, g_TexelSize.y));
        float3 dx = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? (Pr - P) : (P - Pl);
        float3 dy = (abs(Pd.z - P.z) < abs(P.z - Pu.z)) ? (Pd - P) : (P - Pu);
        float3 N  = normalize(cross(dy, dx));
        if (dot(N, -P) < 0.0f)
            N = -N;                             // face the camera

        // Ronin @feature 10/10/2026 DX9: the shadow goes on this frame's light AND last frame's, so the change mask says nothing about it
        // and TAA is free to average the dithered rays into a soft shadow (marked, it could not: the sizzle of the first form).
        float  seen  = 1.0f;
        [branch]
        float  where = 0.0f;
        if ((g_SceneLight.w > 0.0f || view == 4) && g_Shaft.x >= 0.0f && reach > 0.0f && view != 2)
            seen = lerp(1.0f, lampSeen(P, N, g_LightPos.xyz, uv, where), (view == 4) ? 1.0f : g_SceneLight.w);
        float  ndl   = saturate(dot(N, toL / max(dist, 0.001f)));
        float3 light = (g_LightDiff.rgb * ndl + g_LightAmb.rgb) * (reach * seen);
        float  ndlP   = saturate(dot(N, toP / max(distP, 0.001f)));
        float3 lightP = (g_PrevDiff.rgb * ndlP + g_PrevAmb.rgb) * (reachP * seen);

        if (view == 1)
            result = light * 0.5f;              // the light alone, on a grey world
        else if (view == 2)
            result = N * 0.5f + 0.5f;           // the facing it was given
        else if (view == 4)
        {
            // Ronin @diagnostic 10/10/2026 DX9: where a light's shadow was caught: red = by the surface the ray starts from, green = by something
            // between, blue = close to the lamp (hidden from the camera). Grey = lit. The lights' shadows, localised.
            float3 at = (where < 0.2f) ? float3(1.0f, 0.1f, 0.1f) : ((where > 0.7f) ? float3(0.2f, 0.3f, 1.0f) : float3(0.1f, 0.9f, 0.2f));
            result = lerp(float3(0.25f, 0.25f, 0.25f), at, 1.0f - seen) * saturate(reach * 4.0f);
        }
        else
        {
            // Ronin @feature 09/10/2026 DX9: the frame is surface colour x the map's light: undo that light, light the surface, and stop at
            // the surface's own colour x overbright (the old vertex path stopped at 1).
            float3 frame   = tex2Dlod(g_Frame, float4(uv, 0.0f, 0.0f)).rgb;
            // Ronin @feature 10/10/2026 DX9: lighting part 3 - the frame is darker where the sun's shadow fell; the mesh's own draw recorded by
            // how much, so the lamp lights the surface as it is, not as the shadow left it.
            float  shadeM  = tex2Dlod(g_SunShade, float4(uv, 0.0f, 0.0f)).r;
            float  shadeG  = tex2Dlod(g_GroundShade, float4(uv, 0.0f, 0.0f)).r;
            float  shade   = (shadeM > 0.2f) ? shadeM : ((shadeG > 0.2f) ? shadeG : 1.0f);    // a mesh's own first, else the ground's
            shade = lerp(1.0f, min(shade, 1.0f), g_TexelSize.z);
            float3 surface = saturate(frame * g_SceneLight.rgb / shade);
            float3 ceiling = max(frame, surface * g_Camera.w);
            // Ronin @feature 10/10/2026 DX9: and the lamp gives back what the sun's shadow took, in step with its own strength here: the
            // shadow was still to be seen at the pool's rim, where the lamp is weak (`lights sunerase`).
            float3 lost    = frame / shade - frame;
            float  lamp    = saturate(max(light.r, max(light.g, light.b)) * g_TexelSize.w);
            float  lampP   = saturate(max(lightP.r, max(lightP.g, lightP.b)) * g_TexelSize.w);
            result  = min(frame + lost * lamp + surface * light, ceiling) - frame;
            resultP = min(frame + lost * lampP + surface * lightP, ceiling) - frame;
        }
    }

    // Ronin @feature 09/10/2026 DX9: the beam in the air - also over ground the light itself does not reach. Not in the facing view.
    [branch]
    if (g_Shaft.x + g_Shaft.y > 0.0f && view != 2)
    {
        float3 air  = (g_LightDiff.rgb + g_LightAmb.rgb) * (shaft(P, g_LightPos.xyz, g_Corona.x, g_LightAxis, g_LightAmb.w) * g_Shaft.x);
        float3 airP = (g_PrevDiff.rgb + g_PrevAmb.rgb) * (shaft(P, g_PrevPos.xyz, g_Corona.z, g_PrevAxis, g_PrevAmb.w) * g_Shaft.y);
        result  += (view == 1) ? air * 0.5f : air;
        resultP += airP;
    }

    // Ronin @feature 09/10/2026 DX9: the lamp's own glow, in the light's colour.
    [branch]
    if (g_Corona.y + g_Corona.w > 0.0f && view != 2)
    {
        result  += (g_LightDiff.rgb + g_LightAmb.rgb) * (corona(P, g_LightPos.xyz, g_Corona.x) * g_Corona.y);
        resultP += (g_PrevDiff.rgb + g_PrevAmb.rgb) * (corona(P, g_PrevPos.xyz, g_Corona.z) * g_Corona.w);
    }

    // Ronin @bugfix 09/10/2026 DX9: what the light added now against what it added last frame: 2/255 of change marks the pixel in
    // full for TAA. A steady light marks nothing, so its pool and its beam keep their whole history.
    if (view == 0)
    {
        float3 change = abs(result - resultP);
        o.mask = saturate(max(change.r, max(change.g, change.b)) * 128.0f);
    }
    o.color = float4(result, 0.0f);
    return o;
}
