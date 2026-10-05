// Ronin @feature 03/10/2026 DX9: phase 5 - wakes. One shape per texel of the wake texture, added: xy slope, z foam, w the
// mesh's height. Three shapes by the rim: a unit's trail, a ring leaving a hull (the surge), the rings round one at rest.
//
// Compile with: fxc /T ps_3_0 /Fo WaterWake.pso WaterWake_ps.hlsl

float4 g_Arm     : register(c56);	// x: the ridge's half-width, y: its height, z: the swell's half-width, w: its height
float4 g_Wash    : register(c57);	// x: the strip's widening per unit astern, y: its foam, z: the arms' foam, w: 1 / the arms' foam length
float4 g_Feather : register(c58);	// x: how far the wavelets break the ridge (0 = a line), y: their phase per unit astern, z: per unit out,
									// w: 1 / the units astern of the hull over which the ridge and the arms' foam come in
float4 g_Ring    : register(c59);	// x: the foam on a ring that runs ahead only (the surge); Ronin @feature 04/10/2026 DX9: y: the V's
									// half-width at the bow, z: 1 / its gain per unit astern, w: the hollow's depth x the swell's height
// Ronin @feature 04/10/2026 DX9: the rings round a hull at rest, per draw: three waves' phases now, the long one's height.
float4 g_Idle    : register(c60);	// xyz: the two ripples' and the long wave's phase, w: the long wave's height
float4 g_IdleK   : register(c61);	// xyz: their 2 pi / length

struct PSInput
{
	float4 across : TEXCOORD0;	// trail: x distance from the track (signed), y the V's half-width here, z age 0..1, w strength
								// ring:  xy the texel in the hull's frame (x ahead), z the ring's half-width, w its height now
	float4 along  : TEXCOORD1;	// trail: xy the ribbon's left normal, z distance astern of the stern, w the hull's half beam
								// ring:  xy the hull's heading, z the ring's distance from the hull now, w half the hull's straight part
	float  rim    : TEXCOORD2;	// trail: 0 at the ribbon's edge, 1 or more at the track. ring: -1 - how far it runs ahead only (0..1)
								// rest:  -3. Its across: xy as a ring's, z the hull's half beam, w the ripples' height;
								//        along: xy the heading, z 1 / the units the rings run, w half the hull's straight part
};

float4 main(PSInput input) : COLOR0
{
	// Ronin @feature 04/10/2026 DX9: a ring leaving a hull at rest, or the surge ahead of one that stops: a crest with a
	// trough behind it, at one distance all round the hull's straight part. 16 units wide: the 10-unit grid carries it.
	const float  ringW  = max(input.across.z, 0.01f);
	const float2 q      = float2(max(abs(input.across.x) - input.along.w, 0.0f), input.across.y);
	const float  qd     = max(length(q), 0.001f);
	const float  rx     = (qd - input.along.z) / ringW;
	const float  re     = exp(-rx * rx) * input.across.w * 2.3316f;	// x e^(-x x) peaks at 0.4289
	const float  rh     = rx * re;
	const float  rdh    = (1.0f - 2.0f * rx * rx) * re / ringW;
	const float2 out0   = float2(q.x * ((input.across.x >= 0.0f) ? 1.0f : -1.0f), q.y) / qd;	// outward, in the hull's frame
	const float  only   = -input.rim - 1.0f;
	const float  ahead  = saturate(out0.x * 1.3f - 0.15f);
	const float  part   = 1.0f - only + only * ahead * ahead;
	const float2 head   = input.along.xy;
	const float2 rslope = (out0.x * head + out0.y * float2(-head.y, head.x)) * (rdh * part);
	const float4 ring   = float4(rslope, g_Ring.x * only * max(rh, 0.0f) * part, rh * part);

	// Ronin @feature 04/10/2026 DX9: a hull at rest: a train leaving its outline and fading with distance - two close
	// ripples that beat (the shading), one long wave (the height: the mesh). Under the hull the water only heaves.
	const float  id     = max(qd - input.across.z, 0.0f);
	float ienv = saturate(1.0f - id * input.along.z);
	ienv *= ienv;
	float s1, c1, s2, c2, sM, cM;
	sincos(g_IdleK.x * id - g_Idle.x, s1, c1);
	sincos(g_IdleK.y * id - g_Idle.y, s2, c2);
	sincos(g_IdleK.z * id - g_Idle.z, sM, cM);
	const float  idh    = (qd > input.across.z) ? -(input.across.w * (g_IdleK.x * s1 + 0.6f * g_IdleK.y * s2) + g_Idle.w * g_IdleK.z * sM) * ienv : 0.0f;
	const float4 rest   = float4((out0.x * head + out0.y * float2(-head.y, head.x)) * idh, 0.0f, g_Idle.w * cM * ienv);

	// the trail
	const float ad    = abs(input.across.x);
	const float w     = input.across.y;
	const float young = saturate(1.0f - input.across.z);
	const float fade  = young * sqrt(young) * input.across.w;
	const float stern = input.along.z;
	const float beam  = max(input.along.w, 0.01f);

	// Ronin @bugfix 04/10/2026 DX9: the sharp ridge, its wavelets and the arms' foam come in astern of the hull: alongside
	// it they read as sharp chips under the boat.
	const float astern = saturate(stern * g_Feather.w);
	float fs, fc;
	sincos(g_Feather.y * stern - g_Feather.z * ad, fs, fc);
	const float mod  = 1.0f - g_Feather.x * (0.5f - 0.5f * fc);	// 1 on a wavelet
	const float modD = g_Feather.x * 0.5f * g_Feather.z * fs;	// its change per unit out
	const float modS = g_Feather.x * 0.5f * g_Feather.y * fs;	// and per unit forward

	// the sharp ridge: a crest on the arm, a trough just inside it
	const float xf     = (ad - w) / g_Arm.x;
	const float crest  = exp(-xf * xf);
	const float xt     = (ad - w + 1.7f * g_Arm.x) / (1.3f * g_Arm.x);
	const float trough = exp(-xt * xt);
	const float hFine  = (crest - 0.45f * trough) * g_Arm.y * astern;
	const float dFine  = (-2.0f * xf / g_Arm.x * crest + 0.9f * xt / (1.3f * g_Arm.x) * trough) * g_Arm.y * astern;

	// Ronin @bugfix 04/10/2026 DX9: the broad swell is ALL of the height the mesh takes, and smooth: wavelets and the
	// strip's trough were finer than the 10-unit grid can carry, and the mesh came out in sharp facets (py: mesh error).
	const float xb     = (ad - w) / g_Arm.z;
	const float hBroad = exp(-xb * xb) * g_Arm.w;
	const float dBroad = -2.0f * xb / g_Arm.z * hBroad;

	// Ronin @feature 04/10/2026 DX9: the hollow a moving hull leaves: along the track from amidships aft, between the two
	// arms. From an RTS camera height reads as light against dark, not as a bump.
	const float behind  = (w - g_Ring.y) * g_Ring.z;	// astern of the bow; under 0 in the nose
	const float hullL   = max(behind - stern, 1.0f);
	const float xh      = ad / g_Arm.z;
	const float hHollow = exp(-xh * xh) * g_Arm.w * g_Ring.w * saturate((behind - 0.3f * hullL) / (0.5f * hullL));
	const float dHollow = 2.0f * xh / g_Arm.z * hHollow;

	// the churned strip astern, as wide as the hull
	const float washW = beam * (0.9f + g_Wash.x * max(stern, 0.0f));
	float wash = saturate(1.0f - ad / washW);
	wash = wash * wash * (3.0f - 2.0f * wash) * saturate(stern / beam);
	const float armFoam = crest * mod * saturate(0.5f - stern * g_Wash.w) * astern;
	const float foam    = (wash * g_Wash.y * young + armFoam * g_Wash.z) * fade;

	const float  out1    = (dFine * mod + hFine * modD + dBroad + dHollow) * fade * ((input.across.x >= 0.0f) ? 1.0f : -1.0f);
	const float  fwd     = hFine * modS * fade;
	const float2 normal  = input.along.xy;
	const float2 forward = float2(normal.y, -normal.x);
	// Ronin @bugfix 04/10/2026 DX9: the ribbon stops short on the inside of a turn (it would fold over itself): whatever
	// reaches that edge fades into it.
	const float rim  = saturate(input.rim);
	const float edge = rim * rim * (3.0f - 2.0f * rim);
	const float4 trail = float4(out1 * normal + fwd * forward, foam, (hBroad - hHollow) * fade) * edge;

	return (input.rim < -2.5f) ? rest : (input.rim < -0.5f) ? ring : trail;
}
