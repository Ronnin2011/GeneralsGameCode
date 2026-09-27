// Ronin @feature 20/09/2026 DX9: TAA resolve — reproject the history, clamp it, blend it.
//
// STEP 2b-ii: full resolve. The history is reprojected with the previous view-projection so a moving camera still
// lines up, then clamped to the current pixel's neighbourhood so movers do not trail, then blended with an EMA.
//
// THE REPROJECTION IS ANCHORED ON THIS PIXEL AND ADDS ONLY THE DELTA. Using the absolute reprojected UV jitters past
// half a pixel and bleeds — the shadow work paid for that lesson once already, docs/ShadowMap_Temporal_Work.md §8.7.
// Computing the current UV through the SAME matrix path means the float error is common to both and cancels in the
// subtraction, so a still camera reprojects to exactly this pixel instead of almost this pixel.
//
// Compile with: fxc /T ps_3_0 /Fo TaaResolve.pso TaaResolve_ps.hlsl   (pairs with ScreenQuad.vso)

sampler2D g_Scene   : register(s0);      // this frame, as rendered (W3DShaderManager's render-to-texture target)
sampler2D g_History : register(s1);      // last frame's resolved output
sampler2D g_Depth   : register(s2);      // INTZ scene depth, this frame
sampler2D g_PrevDepth: register(s4);     // last frame's LINEAR depth, packed RGBA8 by TaaDepthStore
sampler2D g_VelPrev  : register(s6);     // LAST frame's mesh velocity; its alpha = a mover covered this pixel
sampler2D g_Opaque   : register(s7);     // this frame BEFORE particles (W3DTaa::noteOpaqueDone) - auto-reactive
sampler2D g_Velocity : register(s5);     // mesh velocity, 12-bit packed by TaaVelMesh_ps; alpha = kind (W3DTaa.cpp)

float4   g_TaaParams : register(c0);     // x = history weight (0 = ignore history), yz = 1/texture size, w = reproject on
float4x4 g_Reproject : register(c1);     // c1..c4: current clip -> previous clip. Transposed, like every matrix here.
float4   g_Viewport  : register(c5);     // xy = 3D viewport min, zw = its size, both 0..1 of the texture
float4   g_TaaDebug  : register(c6);     // x = debug view, yz = clip planes (the depth views must linearise)
float4   g_TaaSharpen: register(c7);     // x = CAS strength (screen copy only), y = this frame's velocity is bound, w = clamp strength
float4   g_TaaExtra  : register(c10);    // x = last frame's velocity is bound, y = `taa disocc` mode, z = `taa reactive`
float4   g_TaaVel    : register(c11);    // z = `taa disoccv` px, w = `taa autoreact` threshold (0 = off or no snapshot)

// Ronin @feature 21/09/2026 DX9: Catmull-Rom history fetch, 9 taps.
//
// WHY NOT BILINEAR: the history is re-fetched at a subpixel position every frame and fed straight back in. Bilinear
// softens a little on each fetch and that loss COMPOUNDS through the feedback loop — which is why a lower history
// weight looked sharper (less feedback) while shimmering more (less accumulation). Catmull-Rom resamples without the
// softening, so a high weight can be used for stability without paying for it in blur.
// Ronin @feature 22/09/2026 DX9: inverse of TaaDepthStore's pack.
// Ronin @feature 26/09/2026 DX9: 24 bits in RGB now - alpha carries last frame's auto-reactive flag.
float unpackDepth(float4 enc)
{
    return dot(enc.rgb, float3(1.0f, 1.0f / 255.0f, 1.0f / 65025.0f));
}

float3 sampleHistoryCatmullRom(float2 uv, float2 texSize, float2 invTexSize)
{
    float2 samplePos = uv * texSize;
    float2 texPos1   = floor(samplePos - 0.5f) + 0.5f;
    float2 f         = samplePos - texPos1;

    // Catmull-Rom weights, expanded so fxc keeps them in registers rather than a loop.
    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);

    // The middle pair is folded into ONE bilinear tap placed between them — 9 fetches instead of 16.
    float2 w12      = w1 + w2;
    float2 offset12 = w2 / max(w12, 0.0001f);

    float2 texPos0  = (texPos1 - 1.0f)  * invTexSize;
    float2 texPos3  = (texPos1 + 2.0f)  * invTexSize;
    float2 texPos12 = (texPos1 + offset12) * invTexSize;

    float3 result = 0.0f;
    result += tex2D(g_History, float2(texPos0.x,  texPos0.y )).rgb * w0.x  * w0.y;
    result += tex2D(g_History, float2(texPos12.x, texPos0.y )).rgb * w12.x * w0.y;
    result += tex2D(g_History, float2(texPos3.x,  texPos0.y )).rgb * w3.x  * w0.y;

    result += tex2D(g_History, float2(texPos0.x,  texPos12.y)).rgb * w0.x  * w12.y;
    result += tex2D(g_History, float2(texPos12.x, texPos12.y)).rgb * w12.x * w12.y;
    result += tex2D(g_History, float2(texPos3.x,  texPos12.y)).rgb * w3.x  * w12.y;

    result += tex2D(g_History, float2(texPos0.x,  texPos3.y )).rgb * w0.x  * w3.y;
    result += tex2D(g_History, float2(texPos12.x, texPos3.y )).rgb * w12.x * w3.y;
    result += tex2D(g_History, float2(texPos3.x,  texPos3.y )).rgb * w3.x  * w3.y;

    return result;
}

// Ronin @feature 21/09/2026 DX9: clip toward the box CENTRE along the ray, instead of clamping each channel on its
// own. Per-channel clamping can land on a colour that exists nowhere in the neighbourhood — it mixes the red of one
// corner with the blue of another — and that invented colour is itself a source of flicker. Standard clip_aabb.
float3 clipToAABB(float3 boxMin, float3 boxMax, float3 history)
{
    float3 centre = 0.5f * (boxMax + boxMin);
    float3 extent = 0.5f * (boxMax - boxMin) + 0.0001f;
    float3 ray    = history - centre;
    float3 unit   = ray / extent;
    float3 a      = abs(unit);
    float  maxA   = max(a.x, max(a.y, a.z));
    return (maxA > 1.0f) ? (centre + ray / maxA) : history;
}

// Ronin @bugfix 26/09/2026 DX9: TaaVelMesh_ps's 12-bit packing, inverted. px, screen Y down, current minus previous.
float2 decodeVel(float4 vm)
{
    float3 b   = floor(vm.rgb * 255.0f + 0.5f);
    float  loX = floor(b.z / 16.0f);
    float  loY = b.z - loX * 16.0f;
    float2 e   = float2(b.x * 16.0f + loX, b.y * 16.0f + loY);
    return (e / 4095.0f - 0.5f) * 128.0f;
}

// Ronin @feature 26/09/2026 DX9: AMD FidelityFX CAS (contrast adaptive sharpening), the non-scaling form. Sharpens less
// where the 3x3 is already near its limits, so edges do not ring and flat areas do not grain. Clamped to the 3D viewport so
// the border does not read the command bar's texels.
float3 casSharpen(float2 uv, float sharpness)
{
    float2 px = g_TaaParams.yz;
    float2 lo = g_Viewport.xy + 0.5f * px;
    float2 hi = g_Viewport.xy + g_Viewport.zw - 0.5f * px;
    #define CAS_TAP(OX, OY) tex2Dlod(g_Scene, float4(clamp(uv + float2(OX, OY) * px, lo, hi), 0.0f, 0.0f)).rgb
    float3 a = CAS_TAP(-1.0f, -1.0f);  float3 b = CAS_TAP(0.0f, -1.0f);  float3 c = CAS_TAP(1.0f, -1.0f);
    float3 d = CAS_TAP(-1.0f,  0.0f);  float3 e = CAS_TAP(0.0f,  0.0f);  float3 f = CAS_TAP(1.0f,  0.0f);
    float3 g = CAS_TAP(-1.0f,  1.0f);  float3 h = CAS_TAP(0.0f,  1.0f);  float3 i = CAS_TAP(1.0f,  1.0f);
    #undef CAS_TAP
    float3 mn = min(min(min(d, e), min(f, b)), h);
    mn += min(mn, min(min(a, c), min(g, i)));      // soft min: cross + all nine, 2x scale
    float3 mx = max(max(max(d, e), max(f, b)), h);
    mx += max(mx, max(max(a, c), max(g, i)));
    float3 amp = sqrt(saturate(min(mn, 2.0f - mx) / max(mx, 0.0001f)));
    float3 w   = amp * (-1.0f / lerp(8.0f, 5.0f, saturate(sharpness)));
    return saturate((b * w + d * w + f * w + h * w + e) / (1.0f + 4.0f * w));
}

float luminance(float3 c)
{
    return dot(c, float3(0.2127f, 0.7152f, 0.0722f));
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float3 scene = tex2D(g_Scene, uv).rgb;

    // No usable history: first frame after a toggle, a resize, a device reset, or a target that could not be made.
    // Degrade to a straight copy rather than blend into a buffer nobody wrote — the mistake that cost the terrain its
    // shadows under MSAA (Windowednew §31b).
    // Alpha 0, not 1: alpha is the motion memory now, and marking a fresh frame as "moving" would reject the history
    // it is about to start building.
    if (g_TaaParams.x <= 0.0f)
    {
        // DEBUG 3 belongs HERE, in the copy-to-screen pass. Returning it from the resolve pass writes the debug value
        // into the HISTORY, and the next frame computes `reject` against that corrupted history — the measurement
        // destroys the thing it measures. Alpha already carries the motion memory the real path wrote, so reading it
        // back here shows the signal without disturbing it.
        if (g_TaaDebug.x > 2.5f && g_TaaDebug.x < 3.5f)
        {
            float a = tex2D(g_Scene, uv).a;		// stage 0 IS the history in this pass
            return float4(a, a, a, 0.0f);
        }
        // Ronin @feature 26/09/2026 DX9: CAS on the way to the screen (`taa sharpen`). This pass does not feed the history,
        // so the sharpening cannot compound frame over frame. c7.x is 0 in pass 1.
        if (g_TaaSharpen.x > 0.0f)
            return float4(casSharpen(uv, g_TaaSharpen.x), 0.0f);
        return float4(scene, 0.0f);
    }



    // ---- reprojection ------------------------------------------------------------------------------------------
    float2 histUV   = uv;
    float2 camHistUV = uv;	// the CAMERA-reprojected position, kept before any velocity replaces histUV
    float  velTaken = 0.0f;
    float  speedPx  = 0.0f;		// how far this pixel moved since last frame, in pixels — drives the weight below
    float  prevW    = 1.0f;		// w of the reprojected position: <= 0 means it was BEHIND the eye last frame
    if (g_TaaParams.w > 0.0f)
    {
        // This pixel's position in the 3D view, as NDC. The viewport is a sub-rect of the texture (the command bar
        // takes the rest), so normalise through it before going to clip space.
        float2 s   = (uv - g_Viewport.xy) / g_Viewport.zw;
        float2 ndc = float2(s.x * 2.0f - 1.0f, 1.0f - s.y * 2.0f);

        // tex2DLOD, not tex2D. This sits inside dynamic flow control, where gradient-based sampling is UNDEFINED in
        // HLSL — and on an INTZ depth texture "undefined" came back as a flat 1.0, so every pixel reprojected as if it
        // were on the far plane and the whole frame smeared. SsaoRaw_ps reads the same buffer with tex2Dlod for the
        // same reason.
        float  d   = tex2Dlod(g_Depth, float4(uv, 0.0f, 0.0f)).r;
        float4 cur = float4(ndc, d, 1.0f);

        float4 prev = mul(cur, g_Reproject);
        prevW = prev.w;
        if (prev.w > 0.0001f)
        {
            float2 prevNDC = prev.xy / prev.w;

            // DELTA, not absolute — see the note at the top. Both sides come from the same ndc, so the error cancels.
            float2 deltaNDC = prevNDC - ndc;
            float2 deltaUV  = float2(deltaNDC.x * 0.5f, -deltaNDC.y * 0.5f) * g_Viewport.zw;
            histUV  = uv + deltaUV;
            camHistUV = histUV;
            speedPx = length(deltaUV / max(g_TaaParams.yz, 0.000001f));

            // Ronin @feature 24/09/2026 DX9: MOTION VECTORS, drawn from the meshes (§14). Written only where a moving mesh is
            // the visible surface. It is the full screen motion of that surface point - camera included - so it REPLACES
            // the camera reprojection above: the fetch is relocated, and the mover keeps its accumulation.
            if (g_TaaSharpen.y > 0.5f)
            {
                float4 vm = tex2Dlod(g_Velocity, float4(uv, 0.0f, 0.0f));
                // Ronin @bugfix 26/09/2026 DX9: DILATION. The velocity silhouette is the CLEAN projection and
                // the scene the JITTERED one, so up to half a pixel of a moving unit's edge had no velocity: reset, or fed
                // the old position's history unclamped - the ugly perimeter. Borrow a neighbour's velocity, but only from
                // the SAME surface (depth within 1%), so it never spreads onto the ground beside the unit.
                if (vm.a <= 0.5f)
                {
                    float zn0   = g_TaaDebug.y;
                    float zf0   = g_TaaDebug.z;
                    float zc    = (zn0 * zf0) / max(zf0 - d * (zf0 - zn0), 0.0001f);
                    float bestD = max(zc * 0.01f, 1.0f);
                    float2 offs[4] = { float2(g_TaaParams.y, 0.0f), float2(-g_TaaParams.y, 0.0f),
                                       float2(0.0f, g_TaaParams.z), float2(0.0f, -g_TaaParams.z) };
                    [unroll] for (int i = 0; i < 4; ++i)
                    {
                        float4 vn = tex2Dlod(g_Velocity, float4(uv + offs[i], 0.0f, 0.0f));
                        if (vn.a > 0.5f)
                        {
                            float rn = tex2Dlod(g_Depth, float4(uv + offs[i], 0.0f, 0.0f)).r;
                            float dz = abs((zn0 * zf0) / max(zf0 - rn * (zf0 - zn0), 0.0001f) - zc);
                            if (dz < bestD)
                            {
                                bestD = dz;
                                vm    = vn;
                            }
                        }
                    }
                }
                if (vm.a > 0.5f)
                {
                    float2 v  = decodeVel(vm);		// px, screen Y down, current minus previous
                    histUV    = uv - v * g_TaaParams.yz;			// the surface point was here last frame
                    velTaken  = 1.0f;
                }
            }

            // DEBUG 1: the delta itself, scaled so a one-pixel shift is clearly visible. Grey = no motion; a smooth
            // gradient that follows the terrain = correct parallax; a flat colour = depth is not reaching us; wild
            // banding = the matrix is wrong. This is the picture that says which of those it is.
            if (g_TaaDebug.x > 0.5f && g_TaaDebug.x < 1.5f)
                return float4(saturate(0.5f + deltaUV.x * 40.0f), saturate(0.5f + deltaUV.y * 40.0f), 0.5f, 1.0f);
        }
    }

    // DEBUG 2: depth as grey, LINEARISED. Raw D3D depth is f/(f-n)*(1-n/z), which sits above 0.99 for nearly the whole
    // scene — showing it directly is a white screen no matter how healthy the buffer is. Linearise to camera distance
    // and normalise by the far plane, and the terrain's shape appears.
    if (g_TaaDebug.x > 1.5f && g_TaaDebug.x < 2.5f)	// BOUNDED: unbounded, this ate 3, 4, 5 and 6 and returned alpha=1
    {
        float dd = tex2Dlod(g_Depth, float4(uv, 0.0f, 0.0f)).r;
        float n  = g_TaaDebug.y;
        float f  = g_TaaDebug.z;
        float linZ = (n * f) / max(f - dd * (f - n), 0.0001f);	// `linear` is a reserved HLSL interpolation modifier
        float g = saturate(linZ / f);
        return float4(g, g, g, 1.0f);
    }


    // Off screen OR behind the eye last frame = newly revealed, so there is no history to blend. The w test is the
    // half TerrainShadow_ps has that this was missing: a pixel can reproject to a valid-looking UV while actually
    // having been behind the camera, and that history is garbage. Alpha 0 — newly revealed is not a mover, and
    // marking it would reject the perfectly good history this pixel has NEXT frame.
    if (prevW <= 0.0f ||
        histUV.x < g_Viewport.x || histUV.y < g_Viewport.y ||
        histUV.x > g_Viewport.x + g_Viewport.z || histUV.y > g_Viewport.y + g_Viewport.w)
        return float4(scene, 0.0f);

    float2 texSize = float2(1.0f / g_TaaParams.y, 1.0f / g_TaaParams.z);
    float3 history = sampleHistoryCatmullRom(histUV, texSize, g_TaaParams.yz);




    // The motion memory rides in alpha. A plain bilinear tap is enough for it — it is a decaying flag, not detail.
    float  histAlpha = tex2D(g_History, histUV).a;

    // Ronin @feature 24/09/2026 DX9: REACTIVE. A scrolling texture or a moving translucent layer keeps its history's
    // POSITION but not its CONTENT, so no motion vector can fix it: clamp the history and cap its weight (c10.z).
    // Velocity alpha: 0.4 reactive, 0.7 velocity + reactive, 1.0 velocity only.
    float reactive = 0.0f;
    float autoR    = 0.0f;
    if (g_TaaExtra.z > 0.0f)
    {
        float ra = (g_TaaSharpen.y > 0.5f) ? tex2Dlod(g_Velocity, float4(uv, 0.0f, 0.0f)).a : 0.0f;
        // Ronin @bugfix 25/09/2026 DX9: reactive LAST frame too. A light that blinks off is not drawn, so it had no flag
        // the frame after and its history faded over ~10 frames: the trail along the runway lights.
        float rp = (g_TaaExtra.x > 0.5f) ? tex2Dlod(g_VelPrev, float4(camHistUV, 0.0f, 0.0f)).a : 0.0f;
        reactive = ((ra > 0.25f && ra < 0.85f) || (rp > 0.25f && rp < 0.85f)) ? 1.0f : 0.0f;
        // Ronin @feature 26/09/2026 DX9: AUTO-REACTIVE (c11.w). An effect drew here if the final frame differs from the copy
        // taken just before particles; LAST frame's flag rides in the depth history's alpha, so the pixel a flame just
        // left is clamped too. Particles had no velocity and no clamp: the beaded missile trail, the smeared fire.
        if (g_TaaVel.w > 0.0f)
        {
            float3 dd = abs(scene - tex2Dlod(g_Opaque, float4(uv, 0.0f, 0.0f)).rgb);
            float  pa = tex2Dlod(g_PrevDepth, float4(camHistUV, 0.0f, 0.0f)).a;
            autoR     = (max(dd.r, max(dd.g, dd.b)) > g_TaaVel.w || pa > 0.5f) ? 1.0f : 0.0f;
            reactive  = max(reactive, autoR);
        }
    }

    // DEBUG 13: the mesh velocity target. Dim = nothing, GREEN = velocity, YELLOW = velocity + reactive, MAGENTA =
    // reactive only, CYAN = stopped this frame, ORANGE = auto-reactive (an effect drew over the scene, now or last frame).
    // Returned from pass 1; back to debug 0 and it recovers in a frame.
    if (g_TaaDebug.x > 12.5f && g_TaaDebug.x < 13.5f)
    {
        float da = (g_TaaSharpen.y > 0.5f) ? tex2Dlod(g_Velocity, float4(uv, 0.0f, 0.0f)).a : 0.0f;
        if (da < 0.1f)
            return (autoR > 0.5f) ? float4(1.0f, 0.5f, 0.0f, 0.0f) : float4(scene * 0.25f, 0.0f);
        if (da < 0.25f)
            return float4(0.0f, 1.0f, 1.0f, 0.0f);
        if (da < 0.35f)
            return float4(0.2f, 0.4f, 1.0f, 0.0f);		// BLUE: ghost - moving preview or placement bib, no history kept
        if (da < 0.55f)
            return float4(1.0f, 0.0f, 1.0f, 0.0f);
        if (da < 0.85f)
            return float4(1.0f, 1.0f, 0.0f, 0.0f);
        return float4(0.0f, 1.0f, 0.0f, 0.0f);
    }

    // ---- neighbourhood clamp ------------------------------------------------------------------------------------
    // Reprojection fixes the camera; this fixes everything else. History that falls outside the colours actually
    // present around this pixel cannot be correct — a unit moved, a shadow swept past — so pull it back to the
    // nearest plausible value instead of trusting it. Same idea as the shadow EMA's history rejection.
    float2 du = float2(g_TaaParams.y, 0.0f);
    float2 dv = float2(0.0f, g_TaaParams.z);

    // VARIANCE CLIPPING (Salvi), not a min/max box. The raw min/max of a 3x3 is set by its two most extreme pixels,
    // so one bright subpixel sliver stretches the box wide enough to admit almost any history — and on the frames the
    // sliver is absent it collapses and rejects everything. Mean +/- gamma*sigma tracks what the neighbourhood is
    // actually made of, so it stays steady while a thin feature blinks in and out of it.
    float3 m1 = scene;
    float3 m2 = scene * scene;
    float3 lo = scene;
    float3 hi = scene;
    float3 s2;
    #define TAA_TAP(OFF) s2 = tex2D(g_Scene, OFF).rgb; m1 += s2; m2 += s2 * s2; lo = min(lo, s2); hi = max(hi, s2);
    TAA_TAP(uv - du)
    TAA_TAP(uv + du)
    TAA_TAP(uv - dv)
    TAA_TAP(uv + dv)
    TAA_TAP(uv - du - dv)
    TAA_TAP(uv + du - dv)
    TAA_TAP(uv - du + dv)
    TAA_TAP(uv + du + dv)
    #undef TAA_TAP

    // NOT intersected with the min/max. Intersecting can only ever SHRINK the bracket, and a tighter bracket rejects
    // more history — the opposite of what a thin feature needs. The whole point of the variance form is that when one
    // subpixel sliver makes the neighbourhood volatile, sigma grows and the bracket WIDENS to accommodate it.
    const float GAMMA = 1.25f;
    float3 mu    = m1 / 9.0f;
    float3 sigma = sqrt(abs(m2 / 9.0f - mu * mu));
    lo = mu - GAMMA * sigma;
    hi = mu + GAMMA * sigma;

    float3 clamped = clipToAABB(lo, hi, history);
    // Clamp only where there is EVIDENCE the history describes different geometry: camera motion, a sustained
    // disagreement, or a known mover. NOT merely because this frame disagrees - on a still pixel that disagreement
    // IS the subpixel sampling, and averaging it is the whole point. That is what destroyed the flagpole.
    // Camera motion only: depthBad is computed below, and the clamp is no longer the primary mechanism - the
    // sample counter is. This just handles disocclusions during a pan.
    // Ronin @bugfix 24/09/2026 DX9: ALSO clamp where a motion vector was applied. The velocity is per-OBJECT and rigid,
    // so a turret, wheels or a unit turning all carry residual error, and through a 0.9 history that compounds roughly
    // x9 into a smear. Motion vectors get the history approximately right and the clamp bounds the rest - the standard
    // pairing. A static pole never gets a velocity, so this does not bring its flicker back. `taa clamp 0` for an A/B.
    float clampNeed = saturate(max(max(speedPx * 0.5f, velTaken), reactive));
    history = lerp(history, clamped, g_TaaSharpen.w * clampNeed);

    // ---- history weight: a per-pixel sample counter ---------------------------------------------------------------
    // Ronin @feature 22/09/2026 DX9: CONFIDENCE ACCUMULATION, per the literature. Two parts, and both matter:
    //
    // 1. The history is invalidated on EVIDENCE THAT IT DESCRIBES DIFFERENT GEOMETRY - here a depth discontinuity
    //    between this pixel and the reprojected previous depth - never on colour disagreement. On a still pixel a
    //    colour disagreement IS the subpixel sampling, and averaging it is the entire point; collapsing on it is
    //    what destroyed the flagpole and what every mechanism tried on 22/09 got wrong.
    // 2. The blend is 1/n off a per-pixel SAMPLE COUNTER, not a fixed weight. That is the part a lowered weight can
    //    never emulate: on invalidation n resets to 1, the pixel takes the current frame ENTIRELY in one frame - no
    //    ghost, no 2.5-frame smear - and then n climbs again at full rate so antialiasing rebuilds immediately.
    //    A fixed low weight instead starves the pixel forever, which is exactly the shimmer on movers.
    float curLinZ  = 0.0f;
    float prevLinZ = 0.0f;
    float depthBad = 0.0f;
    if (g_TaaParams.w > 0.0f)
    {
        float dRaw = tex2Dlod(g_Depth, float4(uv, 0.0f, 0.0f)).r;
        float zn   = g_TaaDebug.y;
        float zf   = g_TaaDebug.z;
        curLinZ    = (zn * zf) / max(zf - dRaw * (zf - zn), 0.0001f);
        // Ronin @bugfix 22/09/2026 DX9: ask whether the geometry that WAS here is STILL here - not whether what is
        // here now was nearby before. The first version had it backwards and spared every trail: a trail pixel shows
        // ground now, and a pixel one over was ground last frame too, so it matched and never invalidated.
        //   * Static pole: last frame this pixel was pole; the pole is still a pixel away in THIS frame, so it
        //     matches and the history is kept. That is the jitter, not motion.
        //   * Tank trail: last frame this pixel was tank; there is no tank anywhere near it now, so nothing matches
        //     and the history is correctly thrown away. This is the case colour could never see.
        float2 dpx = g_TaaParams.yz;
        prevLinZ   = unpackDepth(tex2D(g_PrevDepth, histUV)) * zf;

        float4 nd;
        nd.x = tex2Dlod(g_Depth, float4(uv + float2(dpx.x, 0.0f), 0.0f, 0.0f)).r;
        nd.y = tex2Dlod(g_Depth, float4(uv - float2(dpx.x, 0.0f), 0.0f, 0.0f)).r;
        nd.z = tex2Dlod(g_Depth, float4(uv + float2(0.0f, dpx.y), 0.0f, 0.0f)).r;
        nd.w = tex2Dlod(g_Depth, float4(uv - float2(0.0f, dpx.y), 0.0f, 0.0f)).r;
        float4 nlin = (zn * zf) / max(zf - nd * (zf - zn), 0.0001f);

        // REVERSE: is the geometry that WAS here still here - prev depth against the CURRENT neighbourhood.
        float bestRev = abs(prevLinZ - curLinZ);
        bestRev = min(bestRev, min(min(abs(prevLinZ - nlin.x), abs(prevLinZ - nlin.y)),
                                   min(abs(prevLinZ - nlin.z), abs(prevLinZ - nlin.w))));

        float tol  = max(curLinZ * 0.01f, 0.5f);
        depthBad   = saturate((bestRev - tol) / max(tol, 0.0001f));
    }

    // n lives in alpha, normalised by TAA_MAX_N. Camera motion counts as evidence too, the same as before.
    float motion   = saturate(speedPx / 16.0f);
    const float TAA_MAX_N = 32.0f;
    // Ronin @feature 24/09/2026 DX9: DISOCCLUSION. A mover covered this pixel last frame and no mover covers it now, so the
    // history here is the mover and the pixel is ground: reset it. This is the trail behind a moving unit - the ground
    // it just uncovered gets no velocity (the ground did not move) and the depth test misses a low vehicle, whose depth
    // differs from the ground's by less than its tolerance. Exact: last frame's velocity alpha says where a mover was.
    // Sampled at the CAMERA-reprojected position, so it follows a pan. A static pole never carries a velocity.
    float disocc = 0.0f;
    // Ronin @bugfix 27/09/2026 DX9: GHOST (velocity alpha 0.3) - a moving build preview or the placement bib. Its pixels mix
    // preview and ground and change every frame, so no history is right under it.
    if (g_TaaExtra.y > 0.5f && g_TaaSharpen.y > 0.5f)
    {
        float gh = tex2Dlod(g_Velocity, float4(uv, 0.0f, 0.0f)).a;
        if (gh > 0.25f && gh < 0.35f)
            disocc = 1.0f;
    }
    if (g_TaaExtra.x > 0.5f && g_TaaExtra.y > 0.5f && velTaken < 0.75f)
    {
        // Exact rule (`taa disocc 1`): a mover covered THIS pixel last frame and nothing covers it now.
        // Ronin @bugfix 25/09/2026 DX9: "nothing" = no tracked mesh at all (a > 0.1), not just no mover - a unit that
        // stopped is drawn one frame as still (0.15), so its own pixels are not taken for vacated ground.
        float isHere = (g_TaaSharpen.y > 0.5f) ? tex2Dlod(g_Velocity, float4(uv, 0.0f, 0.0f)).a : 0.0f;
        // Ronin @bugfix 26/09/2026 DX9: only a mover that MOVED vacates a pixel (`taa disoccv`, px). A breathing
        // infantryman's clean silhouette wobbles by a fraction of a pixel every frame, and each edge pixel it slipped
        // off was reset: the dark outline in debug 3, the popping edges. Object motion = its velocity minus the camera's.
        float2 camPx    = (uv - camHistUV) / max(g_TaaParams.yz, 0.000001f);
        float4 wm       = tex2Dlod(g_VelPrev, float4(camHistUV, 0.0f, 0.0f));
        float  wasMover = (wm.a > 0.5f && length(decodeVel(wm) - camPx) >= g_TaaVel.z) ? 1.0f : 0.0f;
        if (wasMover > 0.5f && isHere < 0.1f)
            disocc = 1.0f;
        // Ronin @bugfix 27/09/2026 DX9: a ghost left this pixel. It has no velocity, so the mover test above never fires.
        if (wm.a > 0.25f && wm.a < 0.35f && isHere < 0.1f)
            disocc = 1.0f;

        // Ronin @bugfix 24/09/2026 DX9: `taa disocc 2` adds the one-pixel EDGE BAND, with the right question: a mover
        // was NEAR here last frame and is NOT near here now. The velocity silhouette uses the CLEAN projection while
        // the history came from JITTERED frames, so the band just outside the old outline holds some of the object and
        // was never marked - the dotted outline a flag left behind. The first version asked only "was a mover near",
        // and a pole pixel beside its flag is near it on EVERY frame, so it was reset every frame: the flicker at the
        // flag-pole joint. Near on both frames keeps its history; near before and not now is the vacated band.
        if (g_TaaExtra.y > 1.5f && disocc < 0.5f)
        {
            float2 tx = float2(g_TaaParams.y, 0.0f);
            float2 ty = float2(0.0f, g_TaaParams.z);
            float4 n0 = tex2Dlod(g_VelPrev, float4(camHistUV + tx, 0.0f, 0.0f));
            float4 n1 = tex2Dlod(g_VelPrev, float4(camHistUV - tx, 0.0f, 0.0f));
            float4 n2 = tex2Dlod(g_VelPrev, float4(camHistUV + ty, 0.0f, 0.0f));
            float4 n3 = tex2Dlod(g_VelPrev, float4(camHistUV - ty, 0.0f, 0.0f));
            float wasNear = 0.0f;		// a neighbour that MOVED at least `taa disoccv` px, as above
            if (n0.a > 0.5f && length(decodeVel(n0) - camPx) >= g_TaaVel.z) wasNear = 1.0f;
            if (n1.a > 0.5f && length(decodeVel(n1) - camPx) >= g_TaaVel.z) wasNear = 1.0f;
            if (n2.a > 0.5f && length(decodeVel(n2) - camPx) >= g_TaaVel.z) wasNear = 1.0f;
            if (n3.a > 0.5f && length(decodeVel(n3) - camPx) >= g_TaaVel.z) wasNear = 1.0f;
            // Ronin @bugfix 27/09/2026 DX9: and a ghost's edge band, the preview's outline left in the jittered history.
            if ((n0.a > 0.25f && n0.a < 0.35f) || (n1.a > 0.25f && n1.a < 0.35f) ||
                (n2.a > 0.25f && n2.a < 0.35f) || (n3.a > 0.25f && n3.a < 0.35f)) wasNear = 1.0f;
            float isNear = 0.0f;
            if (g_TaaSharpen.y > 0.5f)		// this frame's velocity is bound
            {
                isNear = max(max(tex2Dlod(g_Velocity, float4(uv + tx, 0.0f, 0.0f)).a,
                                 tex2Dlod(g_Velocity, float4(uv - tx, 0.0f, 0.0f)).a),
                             max(tex2Dlod(g_Velocity, float4(uv + ty, 0.0f, 0.0f)).a,
                                 tex2Dlod(g_Velocity, float4(uv - ty, 0.0f, 0.0f)).a));
            }
            if (wasNear > 0.5f && isNear < 0.1f)	// a still or reactive neighbour counts as present
                disocc = 1.0f;
        }
    }
    float invalid  = saturate(max(max(depthBad, motion), disocc));
    float n        = max(histAlpha * TAA_MAX_N, 1.0f);
    n              = lerp(min(n + 1.0f, TAA_MAX_N), 1.0f, invalid);
    // The caller's weight caps how much history a converged pixel may keep, so `taa weight` still means something.
    float effWeight = min(1.0f - 1.0f / n, g_TaaParams.x);
    if (reactive > 0.5f)
        effWeight = min(effWeight, g_TaaExtra.z);


    // DEBUG 4: the history WITHOUT the current frame mixed in — effWeight forced to 1. If the pole is steady here but
    // flickers with the blend on, the accumulated history is fine and the current frame is what disturbs it. If it
    // flickers HERE too, the history itself never converges and the fault is upstream, in the reprojection or the
    // resample. Safe to return from this pass: it writes a real history value, just a fully-accumulated one.
    if (g_TaaDebug.x > 3.5f && g_TaaDebug.x < 4.5f)	// BOUNDED: unbounded, this ate 5 and 6
        return float4(history, n / TAA_MAX_N);

    // ---- tonemapped blending (Karis) -----------------------------------------------------------------------------
    // For thin geometry — a flagpole, a mast — which is narrower than a pixel, so the jitter rasterises it on some
    // frames and misses it on others and a plain lerp passes that full-amplitude blink straight through.
    //
    // The blend happens in TONEMAPPED space and is then inverted. Weighting the two sides by 1/(1+luminance) instead
    // is the form usually quoted, but it does almost nothing here: that form assumes HDR, where a highlight is 10x or
    // 100x and the attenuation bites. This engine renders LDR into A8R8G8B8, luminance sits around 0.3-0.8, and the
    // weights barely differ. Compressing the VALUES and inverting afterwards works at these magnitudes, and it costs
    // no highlight brightness because the inverse restores it.
    float3 sceneT = scene   / (1.0f + luminance(scene));
    float3 histT  = history / (1.0f + luminance(history));
    float3 mixed  = lerp(sceneT, histT, effWeight);
    float3 result = mixed / max(1.0f - luminance(mixed), 0.0001f);

    // Ronin @cleanup 26/09/2026 DX9: the unsharp mask that sat here is gone - it wrote into the history and compounded.
    // Sharpening is CAS in the screen copy now (casSharpen, `taa sharpen`).

    float storeAlpha = n / TAA_MAX_N;		// the sample counter, reprojected with the colour; debug 3 reads it back

    return float4(result, storeAlpha);
}
