// Ronin @feature 09/11/2026 DX9: tread relief (R3DTreadMarks) - a track's grooves shaded into the ground under it. The relief
// map (scripts/treads/GenTreadMaps.py) gives the groove's normal and depth; the sun lights it against the flat ground. Output
// is half the shading ratio: the blend (DESTCOLOR, SRCCOLOR) doubles it onto the ground, so 0.5 leaves it as it was.
//
// Compile with: fxc /T ps_3_0 /Fo TreadRelief.pso TreadRelief_ps.hlsl

sampler2D reliefMap : register(s0);	// RGB = normal (x across, y along, z up), A = height (128/255 = the ground, lower = rut)

float4 g_sunDir : register(c200);	// xyz = towards the sun, world
float4 g_sunCol : register(c201);	// rgb = the sun's colour (terrain diffuse), w = relief strength
float4 g_ambCol : register(c202);	// rgb = the terrain ambient, w = how much darker the rut's floor is

// Ronin @feature 10/11/2026 DX9: the sun's shadow map, read as the terrain's receiver reads it (TerrainShadow_ps): in a shadow only
// the ambient lights the groove, so it shows no sunlit side. Depth textures: each tap is the hardware's 2x2 compare.
sampler2D g_SunMap    : register(s1);	// the near cascade
sampler2D g_SunMapFar : register(s2);	// the far cascade (the near one again with one map)
float4x4 g_SunVP      : register(c204);	// world -> the near map's clip, TRANSPOSED
float4x4 g_SunVPFar   : register(c208);	// world -> the far map's clip
float4 g_SunMapPrm    : register(c212);	// x = 1: a map this frame, y = near bias, z = half a texel (UV), w = maps (1 or 2)
float4 g_SunMapPrm2   : register(c213);	// x = far bias, y = near texel's world size, z = far texel's
sampler2D g_Clouds   : register(s3);	// Ronin @feature 10/11/2026 DX9: the ground's cloud shadow (TSCloudMed), WRAP
float4 g_CloudPrm    : register(c214);	// x = 1 clouds on, y = scale per world unit, zw = drift (the ground's own)

float pcf9(sampler2D smp, float2 uv, float depth, float texelUV)
{
	float sum = 0.0f;
	[unroll]
	for (int y = -1; y <= 1; ++y)
	{
		[unroll]
		for (int x = -1; x <= 1; ++x)
			sum += tex2Dproj(smp, float4(uv + float2(x, y) * texelUV, depth, 1.0f)).r;
	}
	return sum * (1.0f / 9.0f);
}

// 1 lit .. 0 in the sun's shadow. The strip lies 2 units over the ground (W3DTerrainTracks: 0.2 x MAP_XY_FACTOR): the shadow is
// the ground's, so it is read there. Cascade pick, normal offset and seam as TerrainShadow_ps; outside the fitted map = lit.
float sunVisible(float3 world, float3 N, float3 L)
{
	float lit = 1.0f;
	if (g_SunMapPrm.x > 0.5f)
	{
		float3 ground  = world - float3(0.0f, 0.0f, 2.0f);
		float4 selClip = mul(float4(ground, 1.0f), g_SunVP);
		float2 selUV   = selClip.xy * float2(0.5f, -0.5f) + 0.5f + g_SunMapPrm.zz;
		const float SEAM_TEXELS = 4.0f;
		float  seamUV  = g_SunMapPrm.z * 2.0f * SEAM_TEXELS;
		bool   inNear  = (g_SunMapPrm.w < 1.5f)
						 || (selUV.x >= seamUV && selUV.x <= 1.0f - seamUV && selUV.y >= seamUV && selUV.y <= 1.0f - seamUV);
		float  texelWorld = inNear ? g_SunMapPrm2.y : g_SunMapPrm2.z;
		float  ndotl   = saturate(dot(N, L));
		float3 p       = ground + N * (texelWorld * 2.0f * sqrt(saturate(1.0f - ndotl * ndotl)));
		float4 clipN   = mul(float4(p, 1.0f), g_SunVP);
		float4 clipF   = mul(float4(p, 1.0f), g_SunVPFar);
		float2 uvN     = clipN.xy * float2(0.5f, -0.5f) + 0.5f + g_SunMapPrm.zz;
		float2 uvF     = clipF.xy * float2(0.5f, -0.5f) + 0.5f + g_SunMapPrm.zz;
		float2 suv     = inNear ? uvN : uvF;
		// fxc cannot branch around tex2Dproj (X3528): both cascades are read, one is kept.
		float  litN    = pcf9(g_SunMap,    uvN, clipN.z - g_SunMapPrm.y,  g_SunMapPrm.z * 2.0f);
		float  litF    = pcf9(g_SunMapFar, uvF, clipF.z - g_SunMapPrm2.x, g_SunMapPrm.z * 2.0f);
		bool   inMap   = !(suv.x < 0.0f || suv.x > 1.0f || suv.y < 0.0f || suv.y > 1.0f);
		lit = inMap ? (inNear ? litN : litF) : 1.0f;
	}
	return lit;
}

float4 main(float2 uv : TEXCOORD0, float3 world : TEXCOORD1, float fade : TEXCOORD2) : COLOR0
{
	float4 t = tex2D(reliefMap, uv);
	float3 nt = t.rgb * 2.0f - 1.0f;
	float h = (t.a * 255.0f - 128.0f) / 120.0f;	// -1 at the deepest

	// the strip's frame from the screen derivatives (u across, v along; v runs back and forth with the strip)
	float3 dpx = ddx(world);
	float3 dpy = ddy(world);
	float2 dux = ddx(uv);
	float2 duy = ddy(uv);
	float3 N = normalize(cross(dpx, dpy));
	float3 dp2perp = cross(dpy, N);
	float3 dp1perp = cross(N, dpx);
	float3 T = dp2perp * dux.x + dp1perp * duy.x;
	float3 B = dp2perp * dux.y + dp1perp * duy.y;
	float invLen = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20f));
	// T and B are the true directions of u and v whichever way N came out; only N is turned to face up (the ground's
	// side). Ronin @bugfix 09/11/2026 DX9: turning the whole normal also turned its tilt - the grooves showed as ridges.
	N *= (N.z < 0.0f) ? -1.0f : 1.0f;
	float3 n = normalize(T * (invLen * nt.x) + B * (invLen * nt.y) + N * nt.z);

	float3 L = g_sunDir.xyz;
	float sunI = dot(g_sunCol.rgb, float3(0.299f, 0.587f, 0.114f)) * sunVisible(world, N, L);	// Ronin @feature 10/11/2026 DX9: none in shade
	float ambI = dot(g_ambCol.rgb, float3(0.299f, 0.587f, 0.114f));
	// Ronin @feature 10/11/2026 DX9: a cloud darkens the ground by c (0.66..1); taken as sun it keeps off, what is left of the sun's
	// share with the ground's own light: c - (1 - c) x ambient / sun on flat ground - up to about half gone.
	float cloud = dot(tex2D(g_Clouds, world.xy * g_CloudPrm.y + g_CloudPrm.zw).rgb, float3(0.299f, 0.587f, 0.114f));
	sunI *= lerp(1.0f, saturate(cloud - (1.0f - cloud) * ambI / max(sunI * saturate(dot(N, L)), 0.05f)), g_CloudPrm.x);
	float flatLit = ambI + sunI * saturate(dot(N, L));
	float rutLit  = ambI + sunI * saturate(dot(n, L));
	float ratio = rutLit / max(flatLit, 0.05f);
	ratio *= 1.0f + g_ambCol.w * min(h, 0.0f);

	float k = saturate(fade) * g_sunCol.w;
	float m = lerp(1.0f, ratio, k);
	return float4(0.5f * m.xxx, 1.0f);
}
