/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 shadow-map path — §29 phases 0-1.
*/

// Ronin @feature 12/08/2026 DX9: shadow-map path (§29). Coexists with the stencil volume
// system; nothing here runs unless TheUseShadowMaps is TRUE.

#pragma once

#ifndef __W3DSHADOWMAP_H_
#define __W3DSHADOWMAP_H_

#include "Lib/BaseType.h"
#include "WWMath/matrix4.h"

class TextureBaseClass;
class TextureClass;
class ZTextureClass;
class FrustumClass;
class CameraClass;
class SceneClass;
class RenderObjClass;

// Forward-declared so this header does not drag in d3d9.h.
struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9;

// Which depth representation the card gave us. Decided once, by probeCapabilities().
enum ShadowMapMode
{
	SHADOWMAP_UNAVAILABLE = 0,	// nothing usable — caller must stay on stencil volumes
	SHADOWMAP_HW_DEPTH,			// D16/D24X8/DF16/DF24 as a texture: hardware PCF, cheapest
	SHADOWMAP_R32F				// NOT IMPLEMENTED (§29a): no receiver exists, never selected. Kept as the
								// slot for the INTZ / manual-compare path when an AMD card is available.
};

class W3DShadowMap
{
public:
	// Phase 0. Logs every probe result and picks m_mode. Safe to call before init().
	static ShadowMapMode probeCapabilities(void);

	// Ronin @feature 23/08/2026 DX9: §29i.2. The cost was expected to split three ways — draw calls
	// (CPU, MAX_SHADOW_DISTANCE), depth-map fill (GPU, resolution) and receiver pixel work (GPU, PCF).
	// MEASURED 23/08/2026: only resolution is a usable lever. PCF is worth 0.06-0.11 ms and is the
	// biggest visual win in §29, so it stays on at every tier; distance is refused because cutting it
	// makes shadows visibly STOP on ground you are looking at. See applyQuality for the numbers.
	// Ronin @cleanup 24/08/2026 DX9: LOW and MEDIUM were both 1024 and measured identical in cost AND
	// sharpness — one tier wearing two names. Merged into NORMAL; the enum renumbered with it.
	enum ShadowQuality
	{
		SHADOWQ_OFF = 0,
		SHADOWQ_NORMAL,
		SHADOWQ_HIGH,
		SHADOWQ_ULTRA,
		SHADOWQ_COUNT
	};

	// Reads TheGlobalData->m_shadowMapQuality, clamps it, sets TheUseShadowMaps, and rebuilds the
	// target at the matching resolution. Safe to call at ANY time — it tears the receivers down first
	// so nothing is left holding a released texture. Returns TRUE if the map path is live afterwards.
	static Bool applyQuality(void);
	static Int getQuality(void) { return m_quality; }
	static Real getMaxShadowDistance(void) { return m_maxShadowDistance; }

	// NO DEFAULT ARGUMENT ON PURPOSE: the tier table in applyQuality is the single source of truth for
	// resolution. A default here would be a second, silently-disagreeing answer.
	static Bool init(Int resolution);
	static void shutdown(void);
	static void releaseResources(void);		// device-lost hook
	static void reacquireResources(void);	// device-reset hook

	// Phase 1. Fits the light matrices to the visible ground, texel-snapped.
	static void updateLightMatrices(const FrustumClass &cameraFrustum);

	// Fit + fill, in one call. MUST run BEFORE the main render loop — you cannot render to a
	// texture while rendering to screen (W3DDisplay.cpp:2149).
	static void updateRenderTargetTexture(CameraClass *sceneCam, SceneClass *scene);

	// Binds the shadow map as the render target and clears it. Pair with endDepthPass().
	static Bool beginDepthPass(void);
	static void endDepthPass(void);

	// Debug: blits the light's COLOUR view to a screen corner. Validates direction/fit/coverage,
	// which is where the bugs are — a HW depth texture cannot be displayed directly.
	static void drawDebugOverlay(void);

	static ShadowMapMode getMode(void) { return m_mode; }
	static Bool isAvailable(void);
	// HW_DEPTH samples the DEPTH texture, R32F samples the COLOUR one — they are sibling types,
	// so the common base is the only safe return (§29).
	static TextureBaseClass *peekShadowTexture(void);
	static const Matrix4x4 &getLightViewProj(void) { return m_lightViewProj; }

	// Phase 1b. ORTHO camera fitted to the light — also what phase (b) will render the scene with.
	static CameraClass *getLightCamera(void) { return m_lightCamera; }

	// Terrain receiver runs as its OWN pass (the terrain PS has no free sampler). HeightMap.cpp
	// installs these, calls renderTerrainPass, then restores.
	static IDirect3DVertexShader9 *getTerrainShadowVS(void) { return m_terrainShadowVS; }
	static IDirect3DPixelShader9  *getTerrainShadowPS(void) { return m_terrainShadowPS; }
	// Ronin @bugfix 17/08/2026 DX9: §29h-6/§29h-10. COMPUTED, not tuned. Acne appears when the depth
	// error across one shadow texel exceeds the bias, and that error is texel * cot(sun elevation),
	// expressed in the normalised depth the shader compares in.
	// Ronin @feature 23/08/2026 DX9: §29i.3. Written out in full. It used to reduce to cot/(3*res)
	// because depthHalf was 2*radius, so the radius CANCELLED — that was a property of the SPHERE fit,
	// not a law, and the box fit decouples depth range from texel size. This is the general form and it
	// collapses back to cot/(3*res) in the old geometry, so nothing regresses.
	static Real getDepthBias(void)
	{
		// Dimensionless. 1.0 = one texel of depth error, which is what cleared the ship deck. Raise
		// slightly if stripes reappear — it is the only knob left in the whole bias path.
		const Real TEXEL_SPAN = 1.0f;
		const Real res = (m_resolution > 0) ? (Real)m_resolution : 1024.0f;
		const Real depthRange = 3.0f * m_lastDepthHalf;		// near 0.1 .. far 3*depthHalf+1
		if (depthRange <= 0.0f)
			return 0.0f;
		return (TEXEL_SPAN * ((2.0f * m_lastRadius) / res) * m_lastSunCot) / depthRange;
	}
	static Real getTexelOffset(void) { return (m_resolution > 0) ? (0.5f / (Real)m_resolution) : 0.0f; }
	static Int getResolution(void) { return m_resolution; }
	// World size of one shadow texel. Unused today; it is the natural unit for the receiver-side
	// normal-offset bias that comes next.
	static Real getTexelWorldSize(void)
	{
		return (m_resolution > 0) ? (2.0f * m_lastRadius / (Real)m_resolution) : 0.0f;
	}

	// Ronin @perf 19/08/2026 DX9: §29j.7 static caster bake.
	static Bool isStaticCaster(RenderObjClass *robj);
	static void rebuildStaticCasters(SceneClass *scene);
	static void drawStaticCasters(void);
	static void invalidateStaticCasters(void);
	static void ensureStaticCasters(SceneClass *scene);

private:

	static ShadowMapMode	m_mode;
	static Int				m_resolution;
	// Ronin @feature 23/08/2026 DX9: §29i.2. Was a local const in updateLightMatrices. It is the ONLY
	// lever on depth-pass DRAW COUNT (§29j.7): 1200 -> 400 removed 397 draws and 0.81 ms.
	static Real				m_maxShadowDistance;
	static Int				m_quality;
	static TextureClass		*m_colorTarget;
	static ZTextureClass	*m_depthTarget;
	static Matrix4x4		m_lightViewProj;
	static Real				m_lastRadius;	// quantised HALF-EXTENT of the fitted square (§29i.3 box fit)
	static Real				m_lastSunCot;	// cot(sun elevation), clamped — see updateLightMatrices
	// Ronin @feature 23/08/2026 DX9: §29i.3. Half the ortho depth range. Once the fit became a BOX this
	// stopped being proportional to m_lastRadius, so getDepthBias needs it explicitly.
	static Real				m_lastDepthHalf;
	static CameraClass		*m_lightCamera;

	// Terrain receiver pass. Non-fatal if either fails to load — terrain simply gets no shadows.
	static IDirect3DVertexShader9	*m_terrainShadowVS;
	static IDirect3DPixelShader9	*m_terrainShadowPS;
};

// ONE switch for the whole path. FALSE = stencil volumes only, exactly as before.
extern Bool TheUseShadowMaps;

// Debug: keeps COLOUR writes on in the light pass and blits the result to a screen corner.
extern Bool TheShowShadowMapDebug;

// Run stencil volumes alongside the shadow map. Comparison only — double-shadows everything.
extern Bool TheKeepShadowVolumesWithMaps;

#endif  // __W3DSHADOWMAP_H_
