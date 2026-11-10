// Ronin @feature 09/11/2026 DX9: waypoint ribbons (R3DWaypointLines) - a flat band on the ground: a faint fill, brighter rims,
// arrows flowing to the destination, brighter towards it; and the ring at a waypoint. Alpha blended over the opaque scene.
//
// Compile with: fxc /T ps_3_0 /Fo WaypointRibbon.pso WaypointRibbon_ps.hlsl

float4 g_flow  : register(c206);	// x = seconds, y = the arrows' speed (units/s), z = their spacing (units), w = overall alpha
float4 g_shape : register(c207);	// x = fill alpha, y = rim alpha, z = the end's glow (units), w = the start's fade-in (units)
float4 g_ring  : register(c208);	// x = the rings' opacity

float4 main(float4 col : COLOR0, float4 t : TEXCOORD0) : COLOR0
{
	if (t.w > 0.5f)		// a ring: t.xy runs -1..1 across its square. Ronin @feature 09/11/2026 DX9: faint, no centre dot
	{
		float r     = length(t.xy);
		float aa    = max(fwidth(r), 1e-4f);
		float ring  = smoothstep(0.66f - aa, 0.66f + aa, r) * (1.0f - smoothstep(0.86f - aa, 0.86f + aa, r));
		float pulse = 0.85f + 0.15f * sin(g_flow.x * 4.0f);
		return float4(col.rgb, ring * pulse * g_ring.x * g_flow.w);
	}

	float u    = abs(t.x);								// 0 on the centre line, 1 at the edge
	float aa   = max(fwidth(t.x), 1e-4f);
	float edge = 1.0f - smoothstep(1.0f - 2.0f * aa, 1.0f, u);	// the band's own anti-aliased edge
	float rim  = smoothstep(0.62f, 0.80f, u);
	float a    = lerp(g_shape.x, g_shape.y, rim);

	// arrows pointing and flowing to the destination: the centre leads the edges
	float ph    = frac((t.y - g_flow.x * g_flow.y) / g_flow.z + u * 0.35f);
	float arrow = smoothstep(0.0f, 0.06f, ph) * (1.0f - smoothstep(0.16f, 0.24f, ph));
	a += arrow * 0.35f * (1.0f - rim);

	a *= saturate(t.y / g_shape.w);								// fades in from under the unit, or out of the door
	a *= 1.0f + 0.8f * saturate(1.0f - (t.z - t.y) / g_shape.z);	// brighter towards the destination

	return float4(col.rgb * (1.0f + 0.5f * rim + 0.4f * arrow), saturate(a * edge) * g_flow.w);
}
