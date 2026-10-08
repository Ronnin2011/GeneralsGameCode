// Ronin @feature 28/09/2026 DX9: the WaterType 2 water, SM3: normal maps, Fresnel, sun, depth, swell, foam. Began as a port
// of Water/wave.nvp. Inputs bound by W3DWaterSea.h; the knobs are TheWaterSeaProfiles. docs/Water_Work.md.
//
// Compile with: fxc /T ps_3_0 /Fo WaterSea.pso WaterSea_ps.hlsl
// Ronin @feature 02/10/2026 DX9: four builds of this source: plain, /D WATER_VIEWS (debug views), /D WATER_RIVER, and both.
//
// Constants start at c10: the water draws before the TAA resolve, which keeps g_Reproject in c1-c4.

sampler2D s_Bump       : register(s0);	// caust frame, V8U8: signed du/dv in .rg
sampler2D s_Reflection : register(s1);	// renderMirror's target
sampler2D s_Water      : register(s2);	// the standing-water texture; white for the sea
sampler2D s_Edge       : register(s3);	// river edge fade (TWAlphaEdge); white elsewhere
sampler2D s_Normal     : register(s4);	// WaterNormal.tga, world-space normals, z up; alpha = the foam pattern
sampler2D s_Height     : register(s5);	// Ronin @feature 29/09/2026 DX9: terrain heights, one texel per terrain vertex (R32F or L8)
sampler2D s_Scene      : register(s6);	// Ronin @feature 29/09/2026 DX9: phase 3 - the frame just before the water

float4 g_BumpEnvMat : register(c10);	// (M00, M01, M10, M11), what D3DTSS_BUMPENVMAT00..11 held for texbem
float4 g_TexProj    : register(c11);	// ndc -> reflection uv: xy scale, zw offset
float4 g_Reflect    : register(c12);	// x: reflection strength; Fresnel scales it when on, y: how much the ripples move the
										// Fresnel weight (0 = the swell's normal only) - Ronin @bugfix 02/10/2026 DX9
float4 g_Eye        : register(c13);	// xyz: camera position, world
float4 g_SunDir     : register(c14);	// xyz: toward the sun (terrain light 0), w: glitter power (`water gloss`)
float4 g_SunColor   : register(c15);	// rgb: sun colour x glitter strength (`water spec`)
float4 g_Surface    : register(c16);	// x: ripple strength (0 = normal maps off, 1 = as authored), y: Fresnel on, z: reflection distortion, w: F0
float4 g_Shroud     : register(c17);	// x: 1 when the vertex colour carries shroud (rivers), y: 1 / the WaterSet alpha
// Ronin @feature 29/09/2026 DX9: what makes it read as water from an RTS camera, where Fresnel leaves little reflection.
float4 g_Look       : register(c18);	// x: body refraction, y: body shade, z: Fresnel power, w: debug view (`water view`)
float4 g_Sheen      : register(c19);	// rgb: sun colour x sheen strength, w: sheen power
// Ronin @feature 29/09/2026 DX9: phase 1 - water depth from the terrain heights (Water_Work.md §5b).
float4 g_HeightUV   : register(c20);	// world xy -> s5 uv: xy scale, zw offset
float4 g_HeightCfg  : register(c21);	// x: depth on, y: world z per sampled unit, z: foam tiles per world unit, w: seconds
float4 g_DepthCfg   : register(c22);	// x: 1 / tint depth, y: deep brightness, z: opacity, w: 1 / soft shore width, 0 = off
float4 g_FoamCfg    : register(c23);	// x: foam strength, y: 1 / foam width, z: water-texture amount, w: 1 = a river
float4 g_HeightStep : register(c24);	// xy: one height texel in uv, z: 1 / world units per texel, w: `shoredepth`
// Ronin @feature 29/09/2026 DX9: phase 2 - Gerstner swell (W3DWaterSea.h), per wave two rows.
float4 g_Wave[8]    : register(c25);	// [2i] (k dx, k dy, omega, this wave's height / the default sea's total),
										// [2i+1] (dx k A, dy k A - both x the normal boost, Q k A, 0)
float4 g_SwellCfg   : register(c33);	// x: seconds, y: on, z: a lake's shore slope 1 / (depth x MinOpacity) (0 = soft edge off),
										// w: the map's MinOpacity - Ronin @bugfix 01/10/2026 DX9
float4 g_SeaFoam    : register(c34);	// x: open-water lace strength, y: 1 / its depth, z: tiles per world unit, w: 1 = the profile's `opacity`, 0 = the map's own
float4 g_Refraction : register(c35);	// Ronin @feature 29/09/2026 DX9: phase 3 - xy: 1 / frame copy size, z: bend, w: on
// Ronin @feature 29/09/2026 DX9: phase 3b - the terrain's shadow map (TheTerrainShadowPass). Above c35: PS constants are
// device state, and the other receivers hold c12-c28.
float4x4 g_LightViewProj    : register(c36);	// c36-c39, TRANSPOSED
float4x4 g_LightViewProjFar : register(c40);	// c40-c43, TRANSPOSED
float4 g_ShadowCfg          : register(c44);	// x: on, y: near bias, z: half-texel offset, w: strength (`shadow`)
float4 g_CascadeCfg         : register(c45);	// x: far bias, y: cascade count
float4 g_RiverFlow          : register(c46);	// Ronin @feature 02/10/2026 DX9: phase 4 - x: 1 / flow cycle seconds, y: layer-A uv -> foam uv,
												// z: layer-A uv -> the current foam's uv (`seafoamtile`)
// Ronin @feature 03/10/2026 DX9: phase 5 - whitecaps (W3DWaterSea.h).
float4 g_Cap[4]             : register(c47);	// per wave: its height share x (cos, sin) of the phase of two trail lags (xy, zw)
float4 g_CapCfg             : register(c51);	// x: `whitecapat`, y: 1 / the foam edge's softness, z: `whitecaps`, w: edge noise
float4 g_CapTrail           : register(c52);	// xy: the two trails' weights, z: `swelltint`, w: 1 = shore breakers (`breakers`)
// Ronin @bugfix 03/10/2026 DX9: the fog of war, read at the displaced surface (W3DWaterSea.h).
float4 g_ShroudMap          : register(c53);	// xy: world xy -> shroud uv scale (0 = no shroud here), zw: the offset added first
// Ronin @feature 03/10/2026 DX9: phase 5 - wakes (W3DWater.cpp renderWakes, WaterWake_ps.hlsl).
float4 g_WakeMap            : register(c54);	// world xy -> wake uv: xy scale, zw offset
float4 g_WakeCfg            : register(c55);	// x: foam (`wakefoam`), y: its slope -> the normal, z: its height -> crest units (all 0 = none)
// Ronin @feature 08/10/2026 DX9: `mirrorsoft` (W3DWaterSea.h). c62: WaterWake_ps holds c56-c61.
float4 g_MirrorSoft         : register(c62);	// xy: the extra taps' offset in mirror uv, z: 1 = on

sampler2D s_ShadowNear : register(s7);	// Ronin @feature 29/09/2026 DX9: phase 3b - depth textures, hardware compare
sampler2D s_ShadowFar  : register(s8);
sampler2D s_Shroud     : register(s9);	// Ronin @bugfix 03/10/2026 DX9: the fog of war (W3DShroud's texture)
sampler2D s_Wake       : register(s10);	// Ronin @feature 03/10/2026 DX9: phase 5 - the wakes: xy slope, z foam, w height

// Ronin @feature 29/09/2026 DX9: phase 3b - 2x2 taps half a texel out, each a hardware 2x2 compare: a 3x3-texel footprint.
float shadowPcf4(sampler2D smp, float2 uv, float depth, float texelUV)
{
	const float h = 0.5f * texelUV;
	return 0.25f * (tex2Dproj(smp, float4(uv + float2(-h, -h), depth, 1.0f)).r +
	                tex2Dproj(smp, float4(uv + float2( h, -h), depth, 1.0f)).r +
	                tex2Dproj(smp, float4(uv + float2(-h,  h), depth, 1.0f)).r +
	                tex2Dproj(smp, float4(uv + float2( h,  h), depth, 1.0f)).r);
}

struct PSInput
{
	float2 vpos     : VPOS;			// Ronin @feature 29/09/2026 DX9: phase 3 - the pixel, for its own texel in the frame copy
	float2 bumpUV   : TEXCOORD0;
	float2 texUV    : TEXCOORD1;
	float2 edgeUV   : TEXCOORD2;
	float4 clipPos  : TEXCOORD3;
	float3 worldPos : TEXCOORD4;
	float4 normalUV : TEXCOORD5;
	float3 swell    : TEXCOORD6;	// Ronin @feature 29/09/2026 DX9: xy the undisplaced world xy, z the swell weight
	float4 flowUV   : TEXCOORD7;	// Ronin @feature 02/10/2026 DX9: a river's drift over one flow cycle - xy layer A, zw layer B
	float4 color    : COLOR0;
};

// Ronin @bugfix 03/10/2026 DX9: COLOR1 marks the pixel for TAA's water mask (W3DTaa::beginWaterMask binds the target while
// TAA runs; unbound, the write goes nowhere). White: it survives whatever blend the draw uses.
struct PSOutput
{
	float4 color   : COLOR0;
	float4 taaMask : COLOR1;
};

PSOutput main(PSInput input)
{
	// Surface normal: two scrolling layers, whiteout blend; flat (0,0,1) with normal maps off.
#ifdef WATER_RIVER
	// Ronin @feature 02/10/2026 DX9: phase 4 - the flow map: each layer read twice, dragged downstream by the vertex's drift
	// at phases half a cycle apart and cross-faded, so a read has no weight as it jumps back (Valve, Portal 2).
	const float  ph0  = frac(g_HeightCfg.w * g_RiverFlow.x);
	const float  ph1  = frac(ph0 + 0.5f);
	const float  wFlow = abs(2.0f * ph0 - 1.0f);	// 1 = all ph1 (ph0 resets at 0)
	const float2 uvA0 = input.normalUV.xy - input.flowUV.xy * ph0;
	const float2 uvA1 = input.normalUV.xy - input.flowUV.xy * ph1 + 0.5f;	// offset: the two reads must not match
	// Ronin @feature 03/10/2026 DX9: z rebuilt from xy - the river map's blue holds the bank-foam field.
	const float2 nAxy = lerp(tex2D(s_Normal, uvA0).xy, tex2D(s_Normal, uvA1).xy, wFlow) * 2.0f - 1.0f;
	const float3 nA   = float3(nAxy, sqrt(saturate(1.0f - dot(nAxy, nAxy))));
	const float2 uvB0 = input.normalUV.zw - input.flowUV.zw * ph0;
	const float2 uvB1 = input.normalUV.zw - input.flowUV.zw * ph1 + 0.5f;
	const float2 nBxy = lerp(tex2D(s_Normal, uvB0).xy, tex2D(s_Normal, uvB1).xy, wFlow) * 2.0f - 1.0f;
	const float3 nB   = float3(nBxy, sqrt(saturate(1.0f - dot(nBxy, nBxy))));
#else
	const float3 nA   = tex2D(s_Normal, input.normalUV.xy).xyz * 2.0f - 1.0f;
	const float3 nB   = tex2D(s_Normal, input.normalUV.zw).xyz * 2.0f - 1.0f;
#endif
	const float3 nMap = normalize(float3(nA.xy + nB.xy, nA.z * nB.z));
	const float3 nFine = normalize(lerp(float3(0.0f, 0.0f, 1.0f), nMap, g_Surface.x));

	// Ronin @feature 29/09/2026 DX9: phase 2 - the swell's normal per pixel at the undisplaced point, the normal maps on top.
	// N = (-sum dx k A cos(th), -sum dy k A cos(th), 1 - sum Q k A sin(th)), x the weight the VS moved the grid by.
	float3 nSwell = float3(0.0f, 0.0f, 1.0f);
	// Ronin @feature 03/10/2026 DX9: phase 5 - x: the swell's height in units of the default sea's total (crest tint,
	// whitecaps); yz: the same a moment earlier, twice - where a crest was, for the whitecaps' trail.
	float3 hSwell = float3(0.0f, 0.0f, 0.0f);
#ifndef WATER_RIVER		// Ronin @feature 02/10/2026 DX9: phase 4 - rivers never swell
	if (g_SwellCfg.y > 0.5f)
	{
		for (int i = 0; i < 4; i++)
		{
			float s, c;
			sincos(dot(g_Wave[2 * i].xy, input.swell.xy) - g_Wave[2 * i].z * g_SwellCfg.x, s, c);
			nSwell -= float3(g_Wave[2 * i + 1].xy * c, g_Wave[2 * i + 1].z * s) * input.swell.z;
			hSwell += float3(g_Wave[2 * i].w * s, g_Cap[i].x * s + g_Cap[i].y * c, g_Cap[i].z * s + g_Cap[i].w * c);
		}
		nSwell = normalize(nSwell);
		hSwell *= input.swell.z;
	}
	// Ronin @feature 03/10/2026 DX9: phase 5 - wakes, from a texture over the view: their slope tilts the normal, their height
	// counts as a crest's (tint, whitecaps), their foam joins the open water's below.
	const float4 wake = tex2D(s_Wake, input.worldPos.xy * g_WakeMap.xy + g_WakeMap.zw);
	nSwell.xy -= wake.xy * g_WakeCfg.y;
	hSwell.x  += wake.w * g_WakeCfg.z;
#endif
	const float3 n = normalize(float3(nFine.xy + nSwell.xy, nFine.z * nSwell.z));

	// Reflection offset: the normal's slope, or with normal maps off the caust texbem the port started from.
	// texbem: u' = u + M00*du + M10*dv,  v' = v + M01*du + M11*dv
	const float2 d = tex2D(s_Bump, input.bumpUV).rg;
	float2 caust;
	caust.x = g_BumpEnvMat.x * d.x + g_BumpEnvMat.z * d.y;
	caust.y = g_BumpEnvMat.y * d.x + g_BumpEnvMat.w * d.y;
	// Ronin @feature 02/10/2026 DX9: g_Surface.x is the ripple strength now (`ripple`, 0 = normal maps off): the reflection
	// bends with the flattened ripples - a calm lake's mirror stays straight.
	const float2 offset = (g_Surface.x > 0.0f) ? nFine.xy * g_Surface.z : caust;
	const float2 uv     = (input.clipPos.xy / input.clipPos.w) * g_TexProj.xy + g_TexProj.zw + offset;
	float4 refl         = tex2D(s_Reflection, uv);
	// Ronin @feature 08/10/2026 DX9: `mirrorsoft` - four more taps on a rotated square: the half-size mirror has no AA and
	// its cut-out edges crawl. tex2Dlod, so the branch is a real one (the mirror has one level).
	[branch] if (g_MirrorSoft.z > 0.5f)
	{
		const float2 o = g_MirrorSoft.xy;
		refl = 0.2f * (refl + tex2Dlod(s_Reflection, float4(uv + float2( o.x,  0.5f * o.y), 0.0f, 0.0f)) +
							  tex2Dlod(s_Reflection, float4(uv + float2(-o.x, -0.5f * o.y), 0.0f, 0.0f)) +
							  tex2Dlod(s_Reflection, float4(uv + float2(-0.5f * o.x,  o.y), 0.0f, 0.0f)) +
							  tex2Dlod(s_Reflection, float4(uv + float2( 0.5f * o.x, -o.y), 0.0f, 0.0f)));
	}

	// Ronin @feature 29/09/2026 DX9: the body bends with the surface: the water texture is read where the normals push it.
	// Light and shade from the normals, relative to flat water so the average brightness stays.
	const float4 water = tex2D(s_Water, input.texUV + n.xy * g_Look.x);
	const float  edge  = tex2D(s_Edge, input.edgeUV).a;
	const float  shade = 1.0f + (dot(n, g_SunDir.xyz) - g_SunDir.z) * g_Look.y;

	// Ronin @feature 29/09/2026 DX9: phase 1. Depth = this water point's height minus the terrain under it. Optional tint:
	// deeper water darker (`deep`, 1 = none) and the map's water texture reduced (`texture`, 1 = the map's own).
	const bool   depthOn = g_HeightCfg.x > 0.5f;
	const float2 huv     = input.worldPos.xy * g_HeightUV.xy + g_HeightUV.zw;
	const float  terrain = tex2Dlod(s_Height, float4(huv, 0.0f, 0.0f)).r * g_HeightCfg.y;
	const float  depth   = max(input.worldPos.z - terrain, 0.0f);
	const float  absorb  = 1.0f - exp(-depth * g_DepthCfg.x);

	// Ronin @bugfix 29/09/2026 DX9: distance to the waterline = depth / terrain slope (central differences, a texel each
	// way). Slopes under 1:20 count as 1:20, so a flat shallow shelf reads as open water, not all shoreline.
	const float gx    = tex2Dlod(s_Height, float4(huv + float2(g_HeightStep.x, 0.0f), 0.0f, 0.0f)).r
	                  - tex2Dlod(s_Height, float4(huv - float2(g_HeightStep.x, 0.0f), 0.0f, 0.0f)).r;
	const float gy    = tex2Dlod(s_Height, float4(huv + float2(0.0f, g_HeightStep.y), 0.0f, 0.0f)).r
	                  - tex2Dlod(s_Height, float4(huv - float2(0.0f, g_HeightStep.y), 0.0f, 0.0f)).r;
	const float slope = length(float2(gx, gy)) * g_HeightCfg.y * 0.5f * g_HeightStep.z;
	const float shore = depth / max(slope, 0.05f);

	// Ronin @bugfix 29/09/2026 DX9: shore effects stop in deep water (a steep slope under it read as a shore): a smooth depth
	// gate, full to `shoredepth` - 1 and gone by + 1, on foam, see-through and the reflection fade alike.
	const float nearSurface = saturate((g_HeightStep.w + 1.0f - depth) * 0.5f);
	const float3 detail = lerp(float3(1.0f, 1.0f, 1.0f), water.rgb, g_FoamCfg.z);
	// Ronin @feature 03/10/2026 DX9: phase 5 - crests lighter and greener, troughs darker (`swelltint`). From an RTS camera a
	// wave's slope barely changes the light; its height reads. Per pixel, not from the grid: the cell changes with zoom.
	const float  hTint  = hSwell.x * g_CapTrail.z;
	const float3 crest  = 1.0f + hTint + max(hTint, 0.0f) * float3(0.0f, 0.35f, 0.15f);
	const float3 body   = (depthOn ? detail * lerp(1.0f, g_DepthCfg.y, absorb) : water.rgb) * shade * crest;

	// Fresnel (Schlick): little reflection looking straight down, more toward the horizon.
	const float3 V       = normalize(g_Eye.xyz - input.worldPos);
	// Ronin @bugfix 02/10/2026 DX9: the Fresnel weight from a calmer normal - the swell plus `fresnelripple` of the ripples.
	// At RTS angles a pixel shows one ripple at full tilt, and the weight spiked from ~6% to ~36%: patches of opaque sky.
	const float3 nF      = normalize(lerp(nSwell, n, g_Reflect.y));
	const float  NdotV   = saturate(dot(nF, V));
	const float  fresnel = g_Surface.w + (1.0f - g_Surface.w) * pow(1.0f - NdotV, g_Look.z);
	const float  share   = g_Reflect.x * lerp(1.0f, fresnel, g_Surface.y);

	// Sun: sharp glitter plus a broad sheen, Blinn-Phong. A river carries its shroud in the vertex alpha; the flat water's
	// shroud pass darkens the rest.
	const float3 H      = normalize(g_SunDir.xyz + V);
	const float  NdotH  = saturate(dot(n, H));
	const float  shroud = lerp(1.0f, saturate(input.color.a * g_Shroud.y), g_Shroud.x);
	const float3 sun    = (pow(NdotH, g_SunDir.w) * g_SunColor.rgb + pow(NdotH, g_Sheen.w) * g_Sheen.rgb) * shroud;

	// Ronin @feature 29/09/2026 DX9: phase 3b - shadows on the surface: in shadow the sun's glitter and sheen go and the body
	// darkens by `shadow`. Both cascades are sampled (fxc cannot branch around tex2Dproj); outside the map = lit.
	float lit = 1.0f;
	if (g_ShadowCfg.x > 0.5f)
	{
		const float  texelUV = g_ShadowCfg.z * 2.0f;
		const float4 clipN   = mul(float4(input.worldPos, 1.0f), g_LightViewProj);
		const float4 clipF   = mul(float4(input.worldPos, 1.0f), g_LightViewProjFar);
		const float2 uvN     = clipN.xy * float2(0.5f, -0.5f) + 0.5f + g_ShadowCfg.zz;
		const float2 uvF     = clipF.xy * float2(0.5f, -0.5f) + 0.5f + g_ShadowCfg.zz;
		const float  seam    = texelUV * 3.0f;
		const bool   inNear  = (g_CascadeCfg.y < 1.5f) ||
		                       (uvN.x >= seam && uvN.x <= 1.0f - seam && uvN.y >= seam && uvN.y <= 1.0f - seam);
		const float  litN    = shadowPcf4(s_ShadowNear, uvN, clipN.z - g_ShadowCfg.y, texelUV);
		const float  litF    = shadowPcf4(s_ShadowFar,  uvF, clipF.z - g_CascadeCfg.x, texelUV);
		const float2 suv     = inNear ? uvN : uvF;
		if (suv.x >= 0.0f && suv.x <= 1.0f && suv.y >= 0.0f && suv.y <= 1.0f)
			lit = inNear ? litN : litF;
	}
	const float shadeLit = lerp(1.0f - g_ShadowCfg.w, 1.0f, lit);

	float4 c;
	// Ronin @bugfix 29/09/2026 DX9: the reflection is not tinted by the water's colour - it was, and a green lake dyed every
	// reflection dark green. The mirror already holds the lit scene.
	c.rgb = lerp(body * shadeLit * input.color.rgb, refl.rgb, share) + sun * lit;
	c.a   = water.a * edge * input.color.a;

	// Ronin @feature 29/09/2026 DX9: phase 1, flat water only (rivers keep their own edge fade). Opacity: the sea's `opacity`
	// faded in over `shore` world units from the waterline, or a lake's own (below). Soft edge off: the source alpha.
	float  foam    = 0.0f;
	float  sea     = 0.0f;
	float  aWater  = c.a;								// Ronin @bugfix 29/09/2026 DX9: the refraction blend's inputs
	float3 seaCol  = float3(0.0f, 0.0f, 0.0f);
	float3 foamCol = float3(0.0f, 0.0f, 0.0f);
	float  shoreFoam = shore;	// Ronin @bugfix 02/10/2026 DX9: the distance the shore foam uses (`view 13`); rivers measure it
#ifdef WATER_RIVER
	// Ronin @bugfix 02/10/2026 DX9: a river's distance to its banks, measured: four steps across the flow to each side, to
	// `foamwidth`, interpolated where the ground leaves the water. No flow direction: the slope estimate.
	if (depthOn)
	{
		const float2 fdir   = input.flowUV.xy;
		const float  flen   = length(fdir);
		const float2 across = (flen > 1.0e-6f) ? float2(-fdir.y, fdir.x) / flen : float2(0.0f, 0.0f);
		const float  width  = 1.0f / max(g_FoamCfg.y, 1.0e-3f);	// `foamwidth`
		float bank = 1000.0f;
		for (int side = 0; side < 2; side++)
		{
			const float2 dir    = across * ((side == 0) ? 1.0f : -1.0f);
			float        prevD  = 0.0f;
			float        prevW  = depth;	// water over the ground at prevD
			bool         found  = false;
			for (int k = 1; k <= 4; k++)
			{
				const float  d     = width * 0.25f * (float)k;
				const float2 puv   = (input.worldPos.xy + dir * d) * g_HeightUV.xy + g_HeightUV.zw;
				const float  wet   = input.worldPos.z - tex2Dlod(s_Height, float4(puv, 0.0f, 0.0f)).r * g_HeightCfg.y;
				if (!found && wet <= 0.0f && prevW > 0.0f)
				{
					bank  = min(bank, prevD + (d - prevD) * prevW / (prevW - wet));
					found = true;
				}
				prevD = d;
				prevW = wet;
			}
		}
		shoreFoam = (flen > 1.0e-6f) ? min(shore, bank) : shore;
	}
	// Ronin @bugfix 03/10/2026 DX9: foam along a river's banks, eroded from the map's blue (a bubble field, `foamtile`):
	// `foam` x the closeness to the bank is the share that passes. The reach varies along the bank; the field drifts slowly.
	if (depthOn)
	{
		const float  BANK_DRIFT = 0.35f;	// banks run slow
		const float2 fuv0  = (input.normalUV.xy - input.flowUV.xy * (ph0 * BANK_DRIFT)) * g_RiverFlow.y;
		const float2 fuv1  = (input.normalUV.xy - input.flowUV.xy * (ph1 * BANK_DRIFT)) * g_RiverFlow.y + 0.5f;
		const float  e     = 0.5f + (lerp(tex2D(s_Normal, fuv0).b, tex2D(s_Normal, fuv1).b, wFlow) - 0.5f) *
		                     rsqrt(wFlow * wFlow + (1.0f - wFlow) * (1.0f - wFlow));	// the blend's spread kept
		const float  reach = 0.15f + 0.85f * smoothstep(0.38f, 0.62f,
		                     tex2Dbias(s_Normal, float4(input.worldPos.xy * 0.002f, 0.0f, 2.0f)).r);
		const float  cov   = saturate(1.0f - shoreFoam * g_FoamCfg.y / reach) * g_FoamCfg.x;
		const float  t     = 1.0f - cov;
		const float  lum   = dot(input.color.rgb, float3(0.299f, 0.587f, 0.114f));
		foam    = smoothstep(t - 0.06f, t + 0.06f, e) * (0.45f + 0.55f * saturate((e - t) * 3.3f)) *
		          saturate(cov * 8.0f) * saturate(shoreFoam) * 0.95f;
		foamCol = saturate(lum * 1.2f + 0.35f).xxx * shadeLit;
		c.rgb   = lerp(c.rgb, foamCol, foam);
		c.a     = max(c.a, foam * smoothstep(0.0f, 0.15f, edge) * input.color.a);
	}
	// Ronin @feature 02/10/2026 DX9: phase 4 - the current's foam: lace on the flow map (`seafoamtile`), broken into rafts,
	// thicker where the river runs fast (swell.z from the VS). `seafoam` scales it; `view 8` shows it cyan.
	{
		const float lace  = lerp(tex2D(s_Normal, uvA0 * g_RiverFlow.z).a, tex2D(s_Normal, uvA1 * g_RiverFlow.z).a, wFlow);
		// Ronin @bugfix 03/10/2026 DX9: rafts ~500 units across, from the slope magnitude, on their own 12 s cycle; half the
		// river stays open.
		const float  phL0  = frac(g_HeightCfg.w * g_RiverFlow.x * 0.25f);
		const float  wL    = abs(2.0f * phL0 - 1.0f);
		const float4 m0    = tex2Dbias(s_Normal, float4((input.normalUV.xy - input.flowUV.xy * (4.0f * phL0)) * 0.27f, 0.0f, 2.0f));
		const float4 m1    = tex2Dbias(s_Normal, float4((input.normalUV.xy - input.flowUV.xy * (4.0f * frac(phL0 + 0.5f))) * 0.27f + 0.5f, 0.0f, 2.0f));
		const float rafts = smoothstep(0.13f, 0.21f, lerp(abs(m0.r - 0.5f) + abs(m0.g - 0.5f), abs(m1.r - 0.5f) + abs(m1.g - 0.5f), wL));
		const float fast  = input.swell.z * 2.0f;
		const float lum   = dot(input.color.rgb, float3(0.299f, 0.587f, 0.114f));
		sea    = saturate(lace * 2.0f * rafts * (0.5f + 0.5f * saturate(fast - 0.5f))) * g_SeaFoam.x;
		seaCol = saturate(lum * 1.3f + 0.15f).xxx * shadeLit;
		c.rgb  = lerp(c.rgb, seaCol, sea);
		c.a    = max(c.a, sea * smoothstep(0.0f, 0.15f, edge) * input.color.a);
	}
#endif
#ifndef WATER_RIVER		// Ronin @feature 02/10/2026 DX9: phase 4 - the standing water's block (it skipped rivers anyway)
	if (depthOn && g_FoamCfg.w < 0.5f)
	{
		// Ronin @bugfix 01/10/2026 DX9: only the sea takes `opacity`. A lake keeps the map's own see-through, vanilla's soft
		// shore per pixel - min(depth / (TransparentWaterDepth x MinOpacity), MinOpacity) - never more opaque.
		if (g_SeaFoam.w > 0.5f)
		{
			if (g_DepthCfg.w > 0.0f)
				c.a = g_DepthCfg.z * lerp(1.0f, saturate(shore * g_DepthCfg.w), nearSurface) * edge;
			else
				c.a = min(c.a, g_DepthCfg.z);
		}
		else if (g_SwellCfg.z > 0.0f)
			c.a = min(depth * g_SwellCfg.z, g_SwellCfg.w) * edge;
		aWater = c.a;

		const float t   = g_HeightCfg.w;
		const float lum = dot(input.color.rgb, float3(0.299f, 0.587f, 0.114f));

		// Ronin @feature 29/09/2026 DX9: the open-water lace - phase 1's first foam, restored as it was. Two
		// crossing layers at full resolution, fading out by `seafoamdepth`, a slow surge running through them.
		const float2 suv      = input.worldPos.xy * g_SeaFoam.z;
		const float  seaLace  = max(tex2D(s_Normal, suv + float2(t * 0.013f, t * 0.009f)).a,
		                            tex2D(s_Normal, suv * 1.7f + float2(0.37f, 0.61f) - float2(t * 0.011f, t * 0.007f)).a);
		// Ronin @bugfix 29/09/2026 DX9: the surge's peak travels across the water (about 480-unit bands); by depth alone a
		// shallow sea of one depth peaked everywhere at once and went white.
		const float  seaBand  = saturate(1.0f - depth * g_SeaFoam.y);
		const float  seaSurge = 0.6f + 0.4f * sin(depth * 0.9f - t * 1.4f + dot(input.worldPos.xy, float2(0.011f, 0.007f)));
		sea    = saturate(seaLace * 2.0f * seaBand * seaSurge) * g_SeaFoam.x;
		// Ronin @feature 03/10/2026 DX9: phase 5 - whitecaps: the same lace where the swell stands highest (`whitecapat`), fainter
		// where it stood a moment ago. The ripple maps fray the edge, only where the water is raised.
		const float3 capH = hSwell + (nA.x + nB.y) * g_CapCfg.w * saturate(hSwell * 3.0f);
		float3 capQ = saturate((capH - g_CapCfg.x) * g_CapCfg.y);
		capQ = capQ * capQ * (3.0f - 2.0f * capQ);
		const float  cap  = max(capQ.x, max(capQ.y * g_CapTrail.x, capQ.z * g_CapTrail.y));
		sea    = max(sea, cap * saturate(0.25f + seaLace * 2.0f) * g_CapCfg.z);
		// Ronin @feature 03/10/2026 DX9: phase 5 - a wake's foam through the same lace: solid where it is thick
		sea    = max(sea, saturate(wake.z * (0.35f + seaLace * 2.2f)) * g_WakeCfg.x);
		seaCol = saturate(lum * 1.3f + 0.15f).xxx * shadeLit;
		c.rgb  = lerp(c.rgb, seaCol, sea);
		c.a    = max(c.a, sea * saturate(depth * 2.0f) * edge * input.color.a);

		// Ronin @bugfix 29/09/2026 DX9: shore foam within `foamwidth` of the waterline: dense lace at the line, thinning out; the
		// wash breathes, and a line of foam comes in from the sea and spends itself at the line.
		const float2 fuv     = input.worldPos.xy * g_HeightCfg.z;
		const float  pattern = max(tex2Dbias(s_Normal, float4(fuv + float2(t * 0.013f, t * 0.009f), 0.0f, 1.0f)).a,
		                           tex2Dbias(s_Normal, float4(fuv * 1.6f + float2(0.37f, 0.61f) - float2(t * 0.011f, t * 0.007f), 0.0f, 1.0f)).a);
		// Ronin @feature 03/10/2026 DX9: phase 5 - shore breakers: the wash and the incoming front keep the SWELL's time (its
		// leading wave's phase here), so a front lands with each crest. `breakers` 0, or no swell: the old clocks.
		const float  th0     = dot(g_Wave[0].xy, input.swell.xy) - g_Wave[0].z * g_SwellCfg.x;
		const float  wash    = 0.75f + 0.25f * sin(lerp(t * 1.1f + dot(input.worldPos.xy, float2(0.021f, 0.017f)), th0, g_CapTrail.w));
		const float  reach   = shore * g_FoamCfg.y / wash;	// 0 at the waterline, 1 at the foam's outer edge
		const float  roll    = frac(lerp(t * 0.22f + dot(input.worldPos.xy, float2(0.004f, 0.003f)),
		                                 0.25f - th0 * 0.15915494f, g_CapTrail.w));	// 0 as the crest passes: a front sets out
		const float  front   = saturate(1.0f - abs(reach - 1.6f * (1.0f - roll)) / lerp(0.06f, 0.3f, roll))
		                     * smoothstep(0.0f, 0.2f, roll) * (1.0f - smoothstep(0.85f, 1.0f, roll));
		// Ronin @bugfix 29/09/2026 DX9: `foam` scales how much of the pattern passes, not the result - as a gain after the
		// threshold it saturated by about 5 and went no further.
		const float  cover   = max(saturate(1.0f - reach) * 1.2f, front) * g_FoamCfg.x;
		foam    = saturate((pattern - 1.0f + cover) * 2.0f) * saturate(shore) * 0.9f * nearSurface;
		foamCol = saturate(lum * 1.2f + 0.35f).xxx * shadeLit;
		c.rgb   = lerp(c.rgb, foamCol, foam);
		c.a     = max(c.a, foam);
	}
#endif

	// Ronin @feature 29/09/2026 DX9: phase 3 - true refraction: the frame under the water, read at this pixel's texel moved by
	// the normal (the bend grows over 4 units of depth). Reflection and sun sit ON the surface; the blend is done here.
#ifndef WATER_RIVER		// Ronin @feature 02/10/2026 DX9: phase 4 - rivers blend, they never take the frame copy
	if (g_Refraction.w > 0.5f)
	{
		const float  bend  = g_Refraction.z * saturate(depth * 0.25f);
		const float2 ruv   = (input.vpos + 0.5f) * g_Refraction.xy + n.xy * bend;
		const float3 under = tex2D(s_Scene, ruv).rgb;
		const float  rim   = lerp(1.0f, saturate(shore * 0.5f), nearSurface);
		float3 r = lerp(under, body * shadeLit * input.color.rgb, aWater);
		r = lerp(r, refl.rgb, share * rim) + sun * lit * rim;
		r = lerp(r, seaCol, sea * saturate(depth * 2.0f));
		r = lerp(r, foamCol, foam);
		c.rgb = r;
		c.a   = 1.0f;
	}
#endif

#ifndef WATER_RIVER		// a river carries its shroud in the vertex alpha
	// Ronin @bugfix 03/10/2026 DX9: the fog of war, here and not in a second pass: that pass drew the grid flat, so it lay
	// above the troughs like a sheet and missed the crests. As that pass did, it darkens the whole result.
	if (g_ShroudMap.x > 0.0f)
		c.rgb *= tex2D(s_Shroud, (input.worldPos.xy + g_ShroudMap.zw) * g_ShroudMap.xy).rgb;
#endif

#ifdef WATER_VIEWS	// Ronin @feature 02/10/2026 DX9: phase 4 - only the *Views.pso variants carry them (`water view`)
	// Ronin @bugfix 03/10/2026 DX9: debug views - one-sided tests, the last unconditional: this build only runs with `water
	// view` 1..14, so the water's own colour is dead here and the compiler drops it (the slot budget).
	const float view = g_Look.w;
	if (view < 1.5f)
		c.rgb = fresnel.xxx;						// 1: Fresnel - the reflection share before `refl`
	else if (view < 2.5f)
		c.rgb = NdotV.xxx;							// 2: N.V - white looking straight down
	else if (view < 3.5f)
		c.rgb = n * 0.5f + 0.5f;					// 3: the surface normal
	else if (view < 4.5f)
		c.rgb = sun;								// 4: sun glitter and sheen only
	else if (view < 5.5f)
		c.rgb = refl.rgb;							// 5: the reflection only
	else if (view < 6.5f)
		c.rgb = body * input.color.rgb;				// 6: the body only
	else if (view < 7.5f)
	{
		c.rgb = saturate(depth * 0.1f).xxx;			// 7: depth - black at the waterline, white from 10 units deep
		c.a   = 1.0f;
	}
	else if (view < 8.5f)
	{
		c.rgb = float3(saturate(foam), saturate(max(foam, sea)), saturate(sea));	// 8: foam - shore yellow, open water cyan
		c.a   = 1.0f;
	}
	else if (view < 9.5f)
	{
		c.rgb = saturate(lerp(50.0f, shore, nearSurface) * 0.02f).xxx;	// 9: distance to the waterline - white from 50 out or too deep
		c.a   = 1.0f;
	}
	else if (view < 10.5f)
	{
		// 10: Ronin @diagnostic 03/10/2026 DX9: the swell - red crests, blue troughs (full at 4 units), green its weight. It
		// showed the weight alone, a flat grey that could not tell waves from none.
		c.rgb = float3(saturate(input.flowUV.w * 0.25f), input.swell.z * 0.5f, saturate(input.flowUV.w * -0.25f));
		c.a   = 1.0f;
	}
	else if (view < 11.5f)
	{
		c.rgb = lit.xxx;							// 11: shadow - white lit, black in shadow
		c.a   = 1.0f;
	}
	else if (view < 12.5f)
	{
		// 12: Ronin @diagnostic 01/10/2026 DX9: opacity mode - blue the profile's `opacity` (sea), green the map's own (lake)
		c.rgb = (g_SeaFoam.w > 0.5f) ? float3(0.2f, 0.4f, 1.0f) : float3(0.2f, 0.9f, 0.3f);
		c.a   = 1.0f;
	}
	else if (view < 13.5f)
	{
		// 13: Ronin @diagnostic 02/10/2026 DX9: the shore foam's three gates - red the band (1 at the waterline, 0 at
		// `foamwidth`), green the depth gate (`shoredepth`), blue the line ramp (0 exactly at the line). Foam needs all three.
		c.rgb = float3(saturate(1.0f - shoreFoam * g_FoamCfg.y), nearSurface, saturate(shoreFoam));
		c.a   = 1.0f;
	}
	else
	{
#ifndef WATER_RIVER
		// 14: Ronin @diagnostic 03/10/2026 DX9: the wake texture - red foam, green height (grey = none, full at 2 units),
		// blue slope
		c.rgb = float3(saturate(wake.z), saturate(0.5f + wake.w * 0.25f), saturate(length(wake.xy) * 3.0f));
#else
		c.rgb = float3(0.0f, 0.5f, 0.0f);	// rivers take no wakes
#endif
		c.a   = 1.0f;
	}
#endif
	PSOutput o;
	o.color   = c;
	o.taaMask = float4(1.0f, 1.0f, 1.0f, 1.0f);
	return o;
}
