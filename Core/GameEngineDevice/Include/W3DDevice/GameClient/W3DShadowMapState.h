/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 shadow-map terrain receiver — cross-tree state hand-off.
*/

// Ronin @feature 14/08/2026 DX9: §29h. Core/HeightMap.cpp is compiled for BOTH the Generals and
// GeneralsMD targets (Core CMakeLists:140; Generals' own copy is commented out), but W3DShadowMap
// lives in GeneralsMD and pulls in GeneralsMD-only dx8instancing.h. So the shadow-map system PUSHES
// its state down into this Core-owned struct instead of Core reaching up for it — same pattern as
// DX8InstanceManagerClass::Set_Shadow_Map on the WW3D2 side.

#pragma once

#ifndef __W3DSHADOWMAPSTATE_H_
#define __W3DSHADOWMAPSTATE_H_

#include "Lib/BaseType.h"

struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9;
struct IDirect3DBaseTexture9;
class CameraClass;

struct TerrainShadowPassState
{
	Bool enabled;						// FALSE => renderTerrainShadowPass does nothing
	// Ronin @bugfix 14/08/2026 DX9: §29h-4.3. TRUE only while WW3D::Render(scene, lightCamera) runs.
	// NOT the same as `enabled` — that is also FALSE when shadow maps are off entirely, and must not
	// gate this. Subsystems that latch per-frame CAMERA state read this and skip.
	Bool inDepthPass;
	// Ronin @feature 14/08/2026 DX9: §29h-4.2. TRUE when a shadow map was produced this frame, so the
	// scene's casters are already in it. Subsystems that ALSO draw a 2D blob (tree/infantry decals)
	// read this and stand down — that is the double shadow.
	Bool active;
	// Ronin @bugfix 15/08/2026 DX9: §29h-4.3. The MAIN camera, published for the depth pass only.
	// CASTERS must be culled against the view, never against the light — the light fit is radius-
	// quantised to 64 and texel-snapped (§29b), so it STEPS as the camera moves and trees at its edge
	// leave the caster set, blinking their shadows out. NULL outside the depth pass.
	const CameraClass *sceneCamera;
	// Ronin @bugfix 15/08/2026 DX9: §29h-4.4. Normalised direction the light TRAVELS (W3DShadow.cpp
	// negates m_terrainLightPos to place the source). Because casters are culled against the VIEW, a
	// caster just off-screen stops casting onto ground that IS visible — the gap at the screen edge.
	// cull() sweeps each bounding sphere along this vector to put those back in the caster set.
	float lightTravelDir[3];
	// Ronin @feature 15/08/2026 DX9: §29i.2. Centre and radius of the fitted light footprint in world
	// space. The WATER receiver draws one quad over exactly this square — outside it the map holds no
	// data, so the shader reads out of bounds, returns "lit", and the quad changes nothing.
	float fitCentre[3];
	float fitRadius;
	IDirect3DVertexShader9 *vs;
	IDirect3DPixelShader9 *ps;
	IDirect3DBaseTexture9 *shadowTex;
	float lightViewProjT[16];			// already TRANSPOSED for SetPixelShaderConstantF (§29d)
	float depthBias;
	float texelOffset;
	// Ronin @feature 17/08/2026 DX9: §29h-6. World size of one shadow texel — the unit the receiver's
	// normal-offset bias works in. APPENDED, not inserted: this struct crosses the Core/GeneralsMD
	// object boundary and a stale object file on either side reads every later field at the wrong
	// offset — garbage matrix, UVs out of [0,1], shadows vanish (§29h-11). ALWAYS rebuild both trees.
	float texelWorldSize;
};


extern TerrainShadowPassState TheTerrainShadowPass;


#endif  // __W3DSHADOWMAPSTATE_H_
