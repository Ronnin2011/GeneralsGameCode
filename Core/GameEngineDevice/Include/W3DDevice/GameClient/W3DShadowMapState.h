/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 shadow-map terrain receiver — cross-tree state hand-off.
*/

// Ronin @feature 14/08/2026 DX9: §29h. Core/HeightMap.cpp compiles for both targets but W3DShadowMap is
// GeneralsMD-only, so the shadow map PUSHES state down into this Core-owned struct. Doc §3.
#pragma once

#ifndef __W3DSHADOWMAPSTATE_H_
#define __W3DSHADOWMAPSTATE_H_

#include "Lib/BaseType.h"

struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9;
struct IDirect3DBaseTexture9;
struct IDirect3DSurface9;
class CameraClass;

struct TerrainShadowPassState
{
	Bool enabled;						// FALSE => renderTerrainShadowPass does nothing
	// Ronin @bugfix 14/08/2026 DX9: §29h-4.3. TRUE only while WW3D::Render(scene, lightCamera) runs.
	// NOT the same as `enabled`. Subsystems that latch per-frame CAMERA state read this and skip.
	Bool inDepthPass;
	// Ronin @feature 14/08/2026 DX9: §29h-4.2. TRUE when a shadow map was produced this frame.
	// Subsystems that also draw a 2D blob (tree and infantry decals) read this and stand down.
	Bool active;
	// Ronin @bugfix 15/08/2026 DX9: §29h-4.3. The MAIN camera, published for the depth pass only.
	// Casters must be culled against the view, never the light — the light fit steps as the camera moves.
	const CameraClass *sceneCamera;
	// Ronin @bugfix 15/08/2026 DX9: §29h-4.4. Normalised direction the light TRAVELS. Casters are culled
	// against the VIEW, so cull() sweeps each sphere along this to keep off-screen casters that reach in.
	float lightTravelDir[3];
	// Ronin @feature 15/08/2026 DX9: §29i.2. Centre of the fitted light footprint in world space. Read by the
	// terrain tile cull and Visibility_Check, together with lightAxisX/Y and fitExtentX/Y.
	float fitCentre[3];


	IDirect3DVertexShader9 *vs;
	IDirect3DPixelShader9 *ps;
	IDirect3DBaseTexture9 *shadowTex;
	float lightViewProjT[16];			// already TRANSPOSED for SetPixelShaderConstantF (§29d)
	float depthBias;
	float texelOffset;
	// Ronin @feature 17/08/2026 DX9: §29h-6. World size of one shadow texel — the unit the receiver's
	// normal-offset bias works in. APPEND new fields, never insert: a stale object file on either side
	// of the Core/GeneralsMD boundary reads every later field at the wrong offset.
	float texelWorldSize;
	// Ronin @feature 03/09/2026 DX9: §29j.13h. Screen-space temporal accumulation of the shadow term.
	// `accumSurf` is this frame's target, `accumPrev` last frame's, `accumWeight` the EMA weight.
	// APPENDED — rebuild GeneralsMD and Core. Doc §8.
	IDirect3DBaseTexture9 *accumPrev;
	IDirect3DBaseTexture9 *accumCur;	// texture behind accumSurf — what the composite quad samples
	IDirect3DSurface9     *accumSurf;
	float accumWeight;
	// Ronin @feature 07/09/2026 DX9: §29j.13n. TREE receiver accumulation — its own pair, because trees
	// draw after the terrain composite. APPENDED — rebuild GeneralsMD and Core. Doc §8.4.
	IDirect3DBaseTexture9 *treeAccumPrev;
	IDirect3DBaseTexture9 *treeAccumCur;
	IDirect3DSurface9     *treeAccumSurf;
	// Ronin @feature 09/09/2026 DX9: §29i.3 step 2. FAR cascade. APPENDED — rebuild GeneralsMD and Core.
	// cascadeCount is 1 or 2; at 1 the receiver ignores everything below and behaves exactly as before.
	IDirect3DBaseTexture9 *shadowTexFar;
	float lightViewProjFarT[16];		// already TRANSPOSED, like lightViewProjT
	float depthBiasFar;
	float texelWorldSizeFar;
	float cascadeCount;
	// Ronin @feature 13/09/2026 DX9: §29i.3 step 2. The CURRENT split's box in its own light axes, for exact culls.
	// APPENDED — rebuild GeneralsMD and Core together, or a stale object reads these at the wrong offset.
	float lightAxisX[3];
	float lightAxisY[3];
	float fitExtentX;
	float fitExtentY;
	// Ronin @feature 13/09/2026 DX9: §29i.3. Light-space mask of where MOVING casters are, one per split — the terrain
	// receiver skips its history there.
	IDirect3DBaseTexture9 *movingMaskNear;
	IDirect3DBaseTexture9 *movingMaskFar;
};




extern TerrainShadowPassState TheTerrainShadowPass;


#endif  // __W3DSHADOWMAPSTATE_H_
