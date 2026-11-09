// Ronin @feature 09/11/2026 DX9: tread relief (R3DTreadMarks) - a track's grooves shaded into the ground under it. The relief
// map (scripts/treads/GenTreadMaps.py) gives the groove's normal and depth; the sun lights it against the flat ground. Output
// is half the shading ratio: the blend (DESTCOLOR, SRCCOLOR) doubles it onto the ground, so 0.5 leaves it as it was.
//
// Compile with: fxc /T ps_3_0 /Fo TreadRelief.pso TreadRelief_ps.hlsl

sampler2D reliefMap : register(s0);	// RGB = normal (x across, y along, z up), A = height (128/255 = the ground, lower = rut)

float4 g_sunDir : register(c200);	// xyz = towards the sun, world
float4 g_sunCol : register(c201);	// rgb = the sun's colour (terrain diffuse), w = relief strength
float4 g_ambCol : register(c202);	// rgb = the terrain ambient, w = how much darker the rut's floor is

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
	float sunI = dot(g_sunCol.rgb, float3(0.299f, 0.587f, 0.114f));
	float ambI = dot(g_ambCol.rgb, float3(0.299f, 0.587f, 0.114f));
	float flatLit = ambI + sunI * saturate(dot(N, L));
	float rutLit  = ambI + sunI * saturate(dot(n, L));
	float ratio = rutLit / max(flatLit, 0.05f);
	ratio *= 1.0f + g_ambCol.w * min(h, 0.0f);

	float k = saturate(fade) * g_sunCol.w;
	float m = lerp(1.0f, ratio, k);
	return float4(0.5f * m.xxx, 1.0f);
}
