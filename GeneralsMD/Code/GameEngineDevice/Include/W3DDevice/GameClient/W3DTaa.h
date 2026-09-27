/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Ronin @feature 20/09/2026 DX9: Temporal anti-aliasing. STEP 1 is the jitter only — no history buffer and no resolve
// yet, so switching it on looks like a faint shimmer. That shimmer IS the subpixel sampling TAA will later accumulate,
// and seeing it is how step 1 is verified. docs/AntiAliasing_Work.md.
//
// The jitter reaches the device through CameraClass::Apply and NOTHING else: Apply takes a local copy of the projection
// (camera.cpp:752-754), so ProjectionTransform itself stays clean and every consumer that reads the camera for picking,
// placement, culling or the view plane keeps the unjittered matrix. That separation is the whole safety argument.

#pragma once

#include "Lib/BaseType.h"

class CameraClass;
class MeshClass;
class Matrix3D;
class Matrix4x4;
class Vector3;

class W3DTaa
{
public:

	// Ronin @feature 20/09/2026 DX9: one step of the sample sequence, called once per frame before the views draw.
	// Does nothing while disabled, so the sequence does not advance and the jitter stays zero.
	static void beginFrame(void);

	// Ronin @feature 20/09/2026 DX9: wraps the 3D scene render so it lands in a texture we can read back. preRender
	// returns TRUE when it took the redirect, and ONLY then may postRender run. Both no-op when TAA is off or when a
	// script's view filter already owns the render-to-texture path - those are cinematic and win.
	static Bool preRender(const CameraClass &camera);
	static void postRender(void);

	// Ronin @feature 20/09/2026 DX9: reprojection needs the INTZ scene depth. SSAO owns that buffer when it is on; when
	// it is off TAA begins it, and then TAA must end it. Called beside W3DSsao's pair in W3DDisplay::draw.
	static void endFrame(void);

	// Frees shaders and targets while the device still exists. Called from W3DDisplay's shutdown, beside W3DSsao's.
	static void shutdown(void);

	// The current sample offset in NDC units, ready to add to the projection's off-centre terms. Zero when disabled.
	// CameraClass::Apply is the only caller.
	static void getJitterNDC(float *outX, float *outY);

	// Ronin @feature 20/09/2026 DX9: on/off - Options.ini `DX9TAA = yes|no` at start, `taa 0|1` on the debug panel after.
	// isActive = enabled AND MSAA off; TAA does nothing under MSAA (no readable depth).
	static Bool isEnabled(void) { return s_enabled; }
	static void setEnabled(Bool on);
	static Bool isActive(void);

	// Ronin @diagnostic 20/09/2026 DX9: runtime knobs (`taa <knob> <value>`).
	// weight - history cap; debug - 1 reproj delta, 2 depth, 3 samples, 4 history, 13 velocity/reactive mask;
	// sharpen - CAS strength 0..1 on the way to the screen, 0 = off; clamp - neighbourhood clamp strength where there is evidence of change.
	static void  setWeight(float w);
	static float getWeight(void);
	static void  setDebug(Int mode);
	static Int   getDebug(void);
	static void  setSharpen(float s);
	static float getSharpen(void);
	// Ronin @feature 26/09/2026 DX9: mipbias - texture mip LOD bias while TAA runs, -2..0; default -1 (Options.ini DX9TAAMipBias).
	static void  setMipBias(float b);
	static float getMipBias(void);
	static void  setClamp(float v);
	static float getClamp(void);

	// Ronin @feature 24/09/2026 DX9: §14 mesh motion vectors. MeshClass::Render reports every mesh it draws outside the
	// shadow depth pass; a mesh that moved since last frame is re-drawn into the velocity target. vel - on/off.
	static void noteMesh(MeshClass *mesh);
	static void setVelocity(Bool on);
	static Bool getVelocity(void);
	static Bool getVelocityOK(void);
	static Int  getMoverCount(void);		// meshes in the velocity pass this frame
	static Int  getMeshSeen(void);

	// Ronin @feature 24/09/2026 DX9: CameraClass::Apply reports the clean projection and view it sends for the TAA
	// camera; the velocity pass and the reprojection use them.
	static void  noteCameraApplied(const Matrix4x4 &d3dProjClean, const Matrix3D &view);

	// Ronin @feature 24/09/2026 DX9: reactive mask - scrolling textures and moving translucent meshes. 0 off, else the
	// history weight cap on those pixels (1 = clamp only). The count is this frame's reactive draws.
	static void  setReactive(float w);
	static float getReactive(void);
	static Int   getReactiveCount(void);
	static Int   getSkinDrawCount(void);	// Ronin @feature 26/09/2026 DX9: skinned meshes in the velocity pass this frame
	static Int   getTrackMisses(void);	// Ronin @diagnostic 25/09/2026 DX9: meshes with no slot this frame; should be 0

	// disocc - reset the history where a mover was last frame and is not now: 0 off, 1 this pixel, 2 plus the edge band.
	static void  setDisocc(Int m);
	static Int   getDisocc(void);
	// Ronin @bugfix 26/09/2026 DX9: disoccv - a mover vacates a pixel only if it moved at least this many px (0 = any).
	static void  setDisoccV(float px);
	static float getDisoccV(void);

	// Ronin @feature 26/09/2026 DX9: auto-reactive. RTS3DScene::Flush calls noteOpaqueDone just before particles; the
	// resolve clamps wherever the final frame differs from that copy by more than autoreact (0 = off).
	static void  noteOpaqueDone(void);
	static void  setAutoReact(float t);
	static float getAutoReact(void);
	static Bool  getOpaqueOK(void);
	// Ronin @bugfix 26/09/2026 DX9: W3DTerrainVisual::addFactionBibDrawable hands over the build-placement bib's 4 world
	// corners; TAA flags that quad reactive for a few frames so the bib leaves no trail.
	static void  noteCursorBib(const Vector3 *corners);

	// Samples in the sequence before it repeats. 8 is the usual choice: long enough to look supersampled, short enough
	// that a stopped camera settles quickly.
	enum { SAMPLE_COUNT = 8 };

private:

	static Bool s_enabled;
};
