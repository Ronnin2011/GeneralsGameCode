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

// Ronin @feature 20/09/2026 DX9: TAA step 1 — the jitter. See W3DTaa.h for why this is the only thing step 1 does.

#include "WW3D2/dx8todx9.h"
#include "WWLib/always.h"
#include "Lib/BaseType.h"
#include "W3DDevice/GameClient/W3DTaa.h"
#include "WW3D2/ww3d.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/shader.h"
#include "WW3D2/texture.h"
#include "WW3D2/camera.h"
#include "WW3D2/mesh.h"		// Ronin @feature 24/09/2026 DX9: mesh motion vectors (§14)
#include "WW3D2/meshmdl.h"
#include "WW3D2/matinfo.h"		// Ronin @feature 24/09/2026 DX9: reactive mask - time-variant mappers
#include <d3dx9math.h>		// Ronin @bugfix 20/09/2026 DX9: D3DX matrix maths, to match the shadow map's convention
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "W3DDevice/GameClient/W3DSsao.h"
#include "Common/GlobalData.h"		// Ronin @feature 26/09/2026 DX9: Options.ini DX9TAA keys

Bool W3DTaa::s_enabled = FALSE;

static Int s_phase = 0;

// Ronin @feature 20/09/2026 DX9: TAA step 2b. Declared up here because beginFrame/setEnabled below touch them:
// toggling TAA mid-game must not blend the first new frame into whatever the buffer held from the last time it ran,
// and s_ownsDepth records that TAA — not SSAO — started the INTZ buffer, so TAA is the one that ends it.
static Bool s_historyValid = FALSE;
static Bool s_active       = FALSE;	// Ronin @feature 26/09/2026 DX9: enabled AND MSAA off, decided once per frame in beginFrame
static Bool s_ownsDepth    = FALSE;

// Ronin @diagnostic 20/09/2026 DX9: runtime knobs, driven by the `taa` panel command. Defaults are the shipping values.
static float s_weight      = 0.9f;		// ~8 frames of memory, matching the jitter sequence length
static Int   s_debugMode   = 0;
// Ronin @feature 26/09/2026 DX9: `taa sharpen` 0..1 = CAS strength, applied in the screen copy (pass 2). The unsharp
// mask it replaces ran in pass 1, so it was written into the history and compounded: +35% edge wobble at 0.35.
static float s_sharpen     = 0.0f;
// Ronin @feature 26/09/2026 DX9: `taa mipbias` - texture mip LOD bias while TAA runs; negative = sharper mips, whose extra
// aliasing the accumulation resolves. Nothing else in the engine sets D3DSAMP_MIPMAPLODBIAS, so TAA owns it.
static float s_mipBias     = -1.0f;	// Ronin @feature 26/09/2026 DX9: -1, user-tested; Options.ini DX9TAAMipBias overrides
static float s_lastMipBias = 0.0f;

// Ronin @feature 22/09/2026 DX9: last frame's LINEAR depth, packed RGBA8. The history is invalidated on a depth
// discontinuity - evidence the pixel shows DIFFERENT GEOMETRY - never on colour disagreement.
static TextureClass        *s_depthHist[2] = { NULL, NULL };
static IDirect3DPixelShader9 *s_depthStorePS = NULL;
// Frame stamp for the mesh tracking table. Slots untouched for TAA_STALE_FRAMES may be reused.
enum { TAA_STALE_FRAMES = 8 };
static UnsignedInt  s_moverFrame = 0;
static Bool     s_velocityOn = TRUE;
static TextureClass          *s_velTex = NULL;
// Ronin @feature 24/09/2026 DX9: LAST frame's velocity target, swapped with s_velTex at the end of each frame. Its alpha
// says where a mover WAS; a pixel covered by a mover last frame and not this one is DISOCCLUDED ground whose history
// still holds the mover - the trail behind a moving unit. The depth test cannot see it for a low vehicle: its depth
// differs from the ground's by less than the tolerance. `taa disocc` switches it.
static TextureClass          *s_velPrevTex = NULL;
static Bool                   s_prevVelOK  = FALSE;
static Int                    s_disocc     = 2;	// `taa disocc` - 0 off, 1 this pixel, 2 this pixel or a neighbour
static UnsignedInt s_velW = 0, s_velH = 0;
static Bool s_lastVelOK = FALSE;

// Ronin @feature 24/09/2026 DX9: MESH motion vectors, §14. Every mesh drawn in the main pass reports itself through
// MeshClass::s_TaaMeshNote; TAA keeps its previous WORLD TRANSFORM, and a mesh whose transform changed is re-drawn into
// the velocity target from its own vertex array. No classification: turrets, dishes and animated rigid parts qualify
// just by moving. Keyed by pointer, identity only; a pointer reused after a delete costs at most one wrong frame.
enum { TAA_MESH_BITS = 12, TAA_MESH_SLOTS = 1 << TAA_MESH_BITS, TAA_MAX_MESH_DRAWS = 1024 };
// Ronin @feature 26/09/2026 DX9: §14 stage 2 - a SKIN keeps its deformed WORLD positions for two frames in skin[]
// (skinCur = this frame's). W3D deforms skins on the CPU, so there is no transform to diff.
struct TaaMeshSlot { const void *key; Matrix3D prev; UnsignedInt stamp; UnsignedInt movedStamp;
                     Vector3 *skin[2]; Int skinCap; Int skinCount; Int skinCur; };
struct TaaMeshDraw { MeshClass *mesh; Matrix3D cur; Matrix3D prev; Int kind;
                     const Vector3 *skinCur; const Vector3 *skinPrev; };	// skinCur NULL = rigid
// Ronin @feature 24/09/2026 DX9: REACTIVE MASK. What a motion vector cannot describe: a texture scrolling on a still
// surface (treads, conveyors - the engine's own time-variant mappers) and a MOVING translucent layer (a rotor), which
// writes no depth. Flagged in the velocity alpha: 0 none, 0.4 reactive, 0.7 velocity + reactive, 1.0 velocity.
// "a > 0.5 = velocity" still holds for every existing reader.
enum { TAA_KIND_VEL = 0, TAA_KIND_VEL_REACT, TAA_KIND_REACT, TAA_KIND_TRANSLUCENT, TAA_KIND_STILL, TAA_KIND_GHOST };	// STILL: alpha 0.15
// Ronin @bugfix 27/09/2026 DX9: GHOST (alpha 0.3) = a MOVING runtime-translucent preview, or the placement bib. The resolve
// keeps no history under it and resets where it just was; REACTIVE's single clamp let busy grass admit its grey edges.
enum { TAA_MODEL_BITS = 10, TAA_MODEL_SLOTS = 1 << TAA_MODEL_BITS };
enum { TAA_MF_DONE = 1, TAA_MF_TIMEVAR = 2, TAA_MF_TRANSL = 4 };
struct TaaModelSlot { const void *key; Int verts; Int polys; UnsignedByte flags; };
static TaaModelSlot s_modelSlots[TAA_MODEL_SLOTS];
static float s_reactiveW  = 0.5f;	// `taa reactive` - 0 off, else the history weight cap there (1 = clamp only)
// Ronin @feature 26/09/2026 DX9: AUTO-REACTIVE, FSR2's generated mask. The frame is copied just before particles and
// sorted translucency draw; where the final frame differs from the copy an effect drew, and the history is clamped
// there. Particles have no velocity, so nothing else clamps them. `taa autoreact` = the threshold, 0 = off.
static float         s_autoReact   = 0.03f;
static TextureClass *s_opaqueTex   = NULL;
static UnsignedInt   s_opaqueW = 0, s_opaqueH = 0;
static Bool          s_opaqueOK    = FALSE;
static UnsignedInt   s_opaqueStamp = 0;
// Ronin @bugfix 26/09/2026 DX9: the build-placement bib (red while the spot is illegal). Terrain geometry that follows the
// cursor: no velocity, no depth change, so the pixels it left kept it in their history - the red trail. Its world corners
// come from W3DTerrainVisual::addFactionBibDrawable, refreshed every other frame; flagged reactive while fresh.
static Vector3      s_bibCorners[4];
static UnsignedInt  s_bibMovedFrame = 0;	// s_moverFrame when its corners last changed; 0 = never
// Ronin @bugfix 27/09/2026 DX9: flagged only while it MOVES (3 frames: InGameUI refreshes it every other frame). A bib
// parked on an illegal spot keeps normal TAA; one that just vanished is reset for those frames, clearing its red.
static inline Bool  cursorBibLive(void) { return s_bibMovedFrame != 0 && (s_moverFrame - s_bibMovedFrame) <= 3; }
static float s_disoccV    = 0.5f;	// Ronin @bugfix 26/09/2026 DX9: `taa disoccv` - px a mover must have moved to vacate a pixel
static Int   s_reactCount = 0;
// Ronin @diagnostic 25/09/2026 DX9: meshes that found no slot this frame, so no velocity at all. Should read 0.
static Int   s_trkFull = 0;
static TaaMeshSlot s_meshSlots[TAA_MESH_SLOTS];
static TaaMeshDraw s_meshDraws[TAA_MAX_MESH_DRAWS];
static Int  s_meshDrawCount = 0;
static Int  s_meshSeen      = 0;
static Int  s_skinDrawCount = 0;
static float *s_skinScratch    = NULL;	// interleaved { cur.xyz, prev.xyz } per vertex for the skin draw
static Int    s_skinScratchCap = 0;
// Ronin @feature 24/09/2026 DX9: the clean view and projection CameraClass::Apply sent for the 3D camera, captured
// inside Apply - the view-projection the velocity pass and the reprojection use.
static D3DXMATRIX s_appliedView, s_appliedProj;
static Bool  s_appliedValid = FALSE;
// Ronin @bugfix 24/09/2026 DX9: the viewport the SCENE drew with, read at the first noteMesh of the frame. s_viewport
// is taken in preRender, before the scene's Apply sets it: 1600x900 there, 1600x720 here (no command bar).
static D3DVIEWPORT9 s_meshVp;
static Bool       s_meshVpValid = FALSE;
static IDirect3DVertexShader9 *s_velMeshVS = NULL;
static IDirect3DVertexShader9 *s_velSkinVS = NULL;	// Ronin @feature 26/09/2026 DX9: §14 stage 2
static IDirect3DPixelShader9  *s_velMeshPS = NULL;


// Ronin @bugfix 23/09/2026 DX9: the 3D camera, cached in preRender. The view-projection is built from THIS, not
// read off the device - see the note at the capture site.
static const CameraClass *s_camera = NULL;
// Ronin @diagnostic 22/09/2026 DX9: `taa clamp` - clamp strength where there is evidence of change; 0 = off.
static float s_clamp     = 1.0f;

// Ronin @bugfix 27/09/2026 DX9: every float knob lands here, from Options.ini and the console alike. A NaN fails every
// comparison and would pass a plain clamp, so it is replaced by the default first.
static inline float taaKnob(float v, float lo, float hi, float ifNaN)
{
	if (!(v == v))
		return ifNaN;
	return (v < lo) ? lo : ((v > hi) ? hi : v);
}
void  W3DTaa::setSharpen(float s)     { s_sharpen = taaKnob(s, 0.0f, 1.0f, 0.0f); }
void  W3DTaa::setMipBias(float b)     { s_mipBias = taaKnob(b, -2.0f, 0.0f, -1.0f); }
float W3DTaa::getMipBias(void)        { return s_mipBias; }
float W3DTaa::getSharpen(void)        { return s_sharpen; }
void  W3DTaa::setClamp(float v)       { s_clamp = taaKnob(v, 0.0f, 1.0f, 1.0f); }
float W3DTaa::getClamp(void)          { return s_clamp; }


void  W3DTaa::setWeight(float w)    { s_weight = taaKnob(w, 0.0f, 0.99f, 0.9f); s_historyValid = FALSE; }
float W3DTaa::getWeight(void)       { return s_weight; }
void  W3DTaa::setDebug(Int mode)    { s_debugMode = mode; }
Int   W3DTaa::getDebug(void)        { return s_debugMode; }


// Ronin @feature 20/09/2026 DX9: Halton(2,3). A low-discrepancy sequence beats random offsets because 8 consecutive
// samples are guaranteed to spread evenly over the pixel — random ones clump, and a clump is a frame that adds no new
// information. Centred on 0 so the average offset over the sequence is zero and the image does not drift.
static float halton(Int index, Int base)
{
	float result = 0.0f;
	float f      = 1.0f / (float)base;
	Int   i      = index;
	while (i > 0)
	{
		result += f * (float)(i % base);
		i      /= base;
		f      /= (float)base;
	}
	return result;
}

static void currentJitterPixels(float *outX, float *outY)
{
	if (!s_active)
	{
		*outX = 0.0f;
		*outY = 0.0f;
		return;
	}
	// Halton is 1-based here: index 0 returns 0 for every base, which would waste a sample on the pixel centre.
	*outX = halton(s_phase + 1, 2) - 0.5f;
	*outY = halton(s_phase + 1, 3) - 0.5f;
}

void W3DTaa::beginFrame(void)
{
	// Ronin @feature 26/09/2026 DX9: Options.ini `DX9TAA`, `DX9TAASharpness`, `DX9TAAMipBias` apply at the first frame; the console
	// still overrides them for the session.
	static Bool s_iniApplied = FALSE;
	if (!s_iniApplied && TheGlobalData != NULL)
	{
		s_iniApplied = TRUE;
		setEnabled(TheGlobalData->m_taaEnabled);
		setSharpen(TheGlobalData->m_taaSharpness);	// Options.ini DX9TAASharpness, 0..1
		setMipBias(TheGlobalData->m_taaMipBias);		// Options.ini DX9TAAMipBias, -2..0
	}
	// Ronin @feature 26/09/2026 DX9: never under MSAA - INTZ has no multisampled form, so there would be no reprojection
	// and no velocity, only jitter. Checked every frame: the options menu can switch MSAA while the game runs.
	const Bool active = (s_enabled && DX8Wrapper::Get_Anti_Aliasing_Level() == 0) ? TRUE : FALSE;
	if (active && !s_active)
		s_historyValid = FALSE;		// the history is from before the pause
	s_active = active;

	// Ronin @feature 26/09/2026 DX9: the mip bias, on every sampler, only while TAA runs. Written every frame while non-zero
	// so a device reset (which restores 0) cannot drop it; once back at 0 it is left alone - zero cost with TAA off.
	const float wantBias = s_active ? s_mipBias : 0.0f;
	if (wantBias != 0.0f || s_lastMipBias != 0.0f)
	{
		IDirect3DDevice9 *biasDev = DX8Wrapper::_Get_D3D_Device8();
		if (biasDev != NULL)
			for (DWORD st = 0; st < 16; ++st)
				biasDev->SetSamplerState(st, D3DSAMP_MIPMAPLODBIAS, *(const DWORD *)&wantBias);
	}
	s_lastMipBias = wantBias;

	if (!s_active)
	{
		s_phase = 0;			// settle back to the start, so enabling always begins at the same sample
		return;
	}
	s_phase = (s_phase + 1) % SAMPLE_COUNT;

	// Ronin @feature 20/09/2026 DX9: reprojection needs the INTZ scene depth. SSAO begins it when its quality is above
	// Off and it runs BEFORE us in W3DDisplay::draw, so its isActive() already answers "did anyone take it". Only start
	// our own when nobody did — Begin_Scene_Depth refuses a second start, and two owners would fight over the end.
	s_ownsDepth = FALSE;
	if (!W3DSsao::isActive())
		s_ownsDepth = DX8Wrapper::Begin_Scene_Depth() ? TRUE : FALSE;
}

void W3DTaa::endFrame(void)
{
	if (!s_ownsDepth)
		return;					// SSAO started it, SSAO ends it
	s_ownsDepth = FALSE;
	DX8Wrapper::End_Scene_Depth();
}

Bool W3DTaa::isActive(void) { return s_active; }

void W3DTaa::setEnabled(Bool on)
{
	if (s_enabled == on)
		return;
	s_enabled      = on;
	s_phase        = 0;
	s_historyValid = FALSE;		// the accumulated image is meaningless across a toggle
}


// Ronin @feature 20/09/2026 DX9: pixels -> NDC. A shift of one pixel is 2/width in NDC because NDC spans -1..1, and the
// projection's off-centre terms (matrix4.h Init_Perspective: Row[0][2], Row[1][2]) are in exactly those units.
// The sign is NEGATED because this port's frustum is column-vector with w = -z: adding j to Row[0][2] contributes
// j*z to clip.x, and clip.x/w = j*z/(-z) = -j. Y flips again because NDC +Y is up while screen +Y is down.
void W3DTaa::getJitterNDC(float *outX, float *outY)
{
	if (outX != NULL) *outX = 0.0f;
	if (outY != NULL) *outY = 0.0f;
	if (!s_active)
		return;

	Int width = 0, height = 0, bits = 0;
	bool windowed = false;
	WW3D::Get_Render_Target_Resolution(width, height, bits, windowed);
	if (width <= 0 || height <= 0)
		return;

	float jx = 0.0f, jy = 0.0f;
	currentJitterPixels(&jx, &jy);

	if (outX != NULL) *outX = -(2.0f * jx / (float)width);
	if (outY != NULL) *outY =  (2.0f * jy / (float)height);
}

// ---------------------------------------------------------------------------------------------------------------
// Ronin @feature 20/09/2026 DX9: TAA step 2a — the render-to-texture wrap and an IDENTITY resolve.
// Nothing here blends yet. The point is to prove the scene can be redirected into a texture and drawn back through
// our own shader WITHOUT changing a single pixel. Step 2b replaces the resolve with the real history blend.
// ---------------------------------------------------------------------------------------------------------------

static IDirect3DPixelShader9  *s_resolvePS    = NULL;
static IDirect3DVertexShader9 *s_quadVS       = NULL;
static Bool                    s_shadersTried = FALSE;
static Bool                    s_redirected   = FALSE;

// Ronin @feature 20/09/2026 DX9: TAA step 2b-i. History ping-pong. POOL_DEFAULT render targets through TextureClass so
// DX8TextureManagerClass recreates them across a device Reset — the same rule SSAO's targets follow.
// s_historyValid is FALSE until a frame has actually been written: the resolve must never blend into an unwritten buffer.
static TextureClass *s_history[2]   = { NULL, NULL };
static UnsignedInt   s_historyW     = 0;
static UnsignedInt   s_historyH     = 0;
static Int           s_historyIndex  = 0;

Bool W3DTaa::getVelocityOK(void)   { return s_lastVelOK; }
Int  W3DTaa::getMoverCount(void)   { return s_meshDrawCount; }
Int  W3DTaa::getMeshSeen(void)     { return s_meshSeen; }
void  W3DTaa::setReactive(float w) { s_reactiveW = taaKnob(w, 0.0f, 1.0f, 0.5f); }
float W3DTaa::getReactive(void)    { return s_reactiveW; }
void  W3DTaa::setDisoccV(float px) { s_disoccV = taaKnob(px, 0.0f, 8.0f, 0.5f); }
float W3DTaa::getDisoccV(void)     { return s_disoccV; }
void  W3DTaa::setAutoReact(float t) { s_autoReact = taaKnob(t, 0.0f, 1.0f, 0.03f); }
float W3DTaa::getAutoReact(void)    { return s_autoReact; }
Bool  W3DTaa::getOpaqueOK(void)     { return s_opaqueOK; }
void  W3DTaa::noteCursorBib(const Vector3 *corners)
{
	if (!s_active || corners == NULL)
		return;
	Bool moved = FALSE;
	for (Int i = 0; i < 4; ++i)
	{
		if (fabsf(corners[i].X - s_bibCorners[i].X) > 0.01f || fabsf(corners[i].Y - s_bibCorners[i].Y) > 0.01f)
			moved = TRUE;
		s_bibCorners[i] = corners[i];
	}
	if (moved)
		s_bibMovedFrame = (s_moverFrame != 0) ? s_moverFrame : 1;
}

// Ronin @feature 26/09/2026 DX9: called by RTS3DScene::Flush in the main pass, just before particles and the sorted
// translucent flush. The first call of the frame is the one: later flushes draw on top of it.
void W3DTaa::noteOpaqueDone(void)
{
	if (!s_redirected || s_autoReact <= 0.0f || s_opaqueStamp == s_moverFrame)
		return;
	s_opaqueStamp = s_moverFrame;
	IDirect3DDevice9 *dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev == NULL)
		return;
	IDirect3DSurface9 *rt = NULL;
	if (FAILED(dev->GetRenderTarget(0, &rt)) || rt == NULL)
		return;
	D3DSURFACE_DESC desc;
	if (SUCCEEDED(rt->GetDesc(&desc)))
	{
		if (s_opaqueTex == NULL || s_opaqueW != desc.Width || s_opaqueH != desc.Height)
		{
			REF_PTR_RELEASE(s_opaqueTex);
			s_opaqueW = 0;
			s_opaqueH = 0;
			s_opaqueTex = NEW_REF(TextureClass, (desc.Width, desc.Height, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_1,
												  TextureClass::POOL_DEFAULT, true));
			if (s_opaqueTex->Peek_D3D_Base_Texture() == NULL)
			{
				REF_PTR_RELEASE(s_opaqueTex);
			}
			else
			{
				s_opaqueW = desc.Width;
				s_opaqueH = desc.Height;
			}
		}
		if (s_opaqueTex != NULL)
		{
			IDirect3DSurface9 *dst = s_opaqueTex->Get_D3D_Surface_Level();
			if (dst != NULL)
			{
				s_opaqueOK = SUCCEEDED(dev->StretchRect(rt, NULL, dst, NULL, D3DTEXF_NONE)) ? TRUE : FALSE;
				dst->Release();
			}
		}
	}
	rt->Release();
}

// The snapshot is usable only if it was taken this frame, of a target the size of the one being resolved, and last
// frame's depth history exists to carry the flag forward - an unbound stage reads white, which would flag everything.
static Bool autoReactUsable(Int w, Int h)
{
	return (s_autoReact > 0.0f && s_opaqueOK && s_opaqueTex != NULL &&
			s_opaqueW == (UnsignedInt)w && s_opaqueH == (UnsignedInt)h &&
			s_depthHist[0] != NULL && s_depthHist[1] != NULL) ? TRUE : FALSE;
}
Int   W3DTaa::getReactiveCount(void) { return s_reactCount; }
Int   W3DTaa::getTrackMisses(void) { return s_trkFull; }
Int   W3DTaa::getSkinDrawCount(void) { return s_skinDrawCount; }
void  W3DTaa::setDisocc(Int m)     { s_disocc = (m < 0) ? 0 : ((m > 2) ? 2 : m); }
Int   W3DTaa::getDisocc(void)      { return s_disocc; }

// Ronin @feature 24/09/2026 DX9: called from CameraClass::Apply for the TAA camera, BEFORE the jitter is added, with
// exactly the projection and view it is about to send. The last call before postRender is the scene's.
void W3DTaa::noteCameraApplied(const Matrix4x4 &d3dProjClean, const Matrix3D &view)
{
	s_appliedProj  = D3DXMATRIX(To_D3DMATRIX(d3dProjClean));
	s_appliedView  = D3DXMATRIX(To_D3DMATRIX(view));
	s_appliedValid = TRUE;
}

// Ronin @bugfix 26/09/2026 DX9: Fibonacci hashing takes the HIGH bits of the product. The low bits depend only on the
// key's low bits, which pool-allocated meshes share at a fixed stride - chains clustered and overflowed (trk=13).
static inline UnsignedInt taaHashPtr(const void *key, Int bits)
{
	return ((UnsignedInt)((size_t)key >> 3) * 2654435761u) >> (32 - bits);
}

// Ronin @bugfix 26/09/2026 DX9: search the WHOLE chain for the key before claiming a free slot. Taking the first freed
// slot met could leave the mesh's real slot further on, throwing away its previous transform.
static TaaMeshSlot *findMeshSlot(const void *key)
{
	const UnsignedInt h = taaHashPtr(key, TAA_MESH_BITS);
	TaaMeshSlot *freeSlot = NULL;
	for (Int probe = 0; probe < 32; ++probe)
	{
		TaaMeshSlot &sl = s_meshSlots[(h + probe) & (TAA_MESH_SLOTS - 1)];
		if (sl.key == key)
			return &sl;
		if (freeSlot == NULL && (sl.key == NULL || (s_moverFrame - sl.stamp) > TAA_STALE_FRAMES))
			freeSlot = &sl;
		if (sl.key == NULL)
			break;		// never used: nothing was ever placed past it
	}
	if (freeSlot == NULL)
	{
		++s_trkFull;
		return NULL;		// locally full; this mesh gets no velocity this frame
	}
	freeSlot->key        = key;
	freeSlot->stamp      = 0;		// 0 = no previous transform yet
	freeSlot->movedStamp = 0;
	freeSlot->skinCount  = 0;		// buffers kept for reuse, contents invalid
	return freeSlot;
}

static inline Bool taaShaderTranslucent(const ShaderClass &sh)
{
	return (sh.Get_Dst_Blend_Func() != ShaderClass::DSTBLEND_ZERO || sh.Get_Depth_Mask() == ShaderClass::DEPTH_WRITE_DISABLE);
}

// Ronin @feature 24/09/2026 DX9: classified once per model. Translucent = pass 0 blends or writes no depth, on most of
// its polygons; a later additive pass does not count. The counts guard against a freed model's address being reused.
static UnsignedByte classifyModel(MeshClass *mesh, MeshModelClass *mdl)
{
	const Int nv = mdl->Get_Vertex_Count();
	const Int np = mdl->Get_Polygon_Count();
	const UnsignedInt h = taaHashPtr(mdl, TAA_MODEL_BITS);
	TaaModelSlot *sl = NULL;
	for (Int probe = 0; probe < 16; ++probe)
	{
		TaaModelSlot &m = s_modelSlots[(h + probe) & (TAA_MODEL_SLOTS - 1)];
		if (m.key == mdl && m.verts == nv && m.polys == np)
			return m.flags;
		if (m.key == NULL || m.key == mdl)
		{
			sl = &m;
			break;
		}
	}
	if (sl == NULL)
		sl = &s_modelSlots[h];		// locally full: evict

	UnsignedByte f = TAA_MF_DONE;
	MaterialInfoClass *mi = mesh->Get_Material_Info();
	if (mi != NULL)
	{
		if (mi->Has_Time_Variant_Texture_Mappers())
			f |= TAA_MF_TIMEVAR;
		mi->Release_Ref();
	}
	if (mdl->Get_Pass_Count() > 0)
	{
		Int trans = 0, total = 1;
		if (mdl->Has_Shader_Array(0))
		{
			total = np;
			for (Int i = 0; i < np; ++i)
				if (taaShaderTranslucent(mdl->Get_Shader(i, 0)))
					++trans;
		}
		else if (taaShaderTranslucent(mdl->Get_Single_Shader(0)))
			trans = 1;
		if (trans * 2 > total)
			f |= TAA_MF_TRANSL;
	}
	sl->key   = mdl;
	sl->verts = nv;
	sl->polys = np;
	sl->flags = f;
	return f;
}

// Ronin @feature 24/09/2026 DX9: MeshClass::Render calls this for every mesh drawn outside the shadow depth pass. Only
// meshes inside the TAA-redirected 3D render count, and each is taken once a frame - the water reflection redraws the
// scene, and a second note would read back the transform the first one just stored.
void W3DTaa::noteMesh(MeshClass *mesh)
{
	if (!s_redirected || !s_velocityOn || mesh == NULL)
		return;
	++s_meshSeen;
	if (s_meshSeen == 1)
	{
		IDirect3DDevice9 *d = DX8Wrapper::_Get_D3D_Device8();
		s_meshVpValid = (d != NULL && SUCCEEDED(d->GetViewport(&s_meshVp))) ? TRUE : FALSE;
	}
	MeshModelClass *mdl = mesh->Peek_Model();
	if (mdl == NULL)
		return;
	// Ronin @feature 26/09/2026 DX9: §14 stage 2 - skins too. Get_Deformed_Vertices needs the container's HTree.
	const Bool isSkin = mdl->Get_Flag(MeshGeometryClass::SKIN) ? TRUE : FALSE;
	if (isSkin && (mesh->Get_Container() == NULL || mesh->Get_Container()->Get_HTree() == NULL))
		return;
	TaaMeshSlot *sl = findMeshSlot(mesh);
	if (sl == NULL || sl->stamp == s_moverFrame)
		return;

	Matrix3D cur  = mesh->Get_Transform();
	Matrix3D prev = sl->prev;
	// The previous transform must be LAST frame's - a mesh off screen for a while would otherwise get a velocity
	// spanning all the frames it was missing.
	const Bool hadPrev = (sl->stamp != 0) && (s_moverFrame - sl->stamp == 1);
	const Bool movedLast = (sl->movedStamp != 0) && (s_moverFrame - sl->movedStamp == 1);
	sl->prev  = cur;
	sl->stamp = s_moverFrame;

	Bool moved = FALSE;
	const Vector3 *skinCur = NULL, *skinPrev = NULL;
	if (isSkin)
	{
		// Deformed this frame into the buffer that is NOT last frame's; compared vertex by vertex, since an idle
		// infantryman's transform never changes while his mesh breathes.
		const Int nv = mdl->Get_Vertex_Count();
		if (nv <= 0 || nv > 65535)
			return;
		if (sl->skinCap < nv)
		{
			delete [] sl->skin[0];
			delete [] sl->skin[1];
			sl->skin[0]   = W3DNEWARRAY Vector3[nv];
			sl->skin[1]   = W3DNEWARRAY Vector3[nv];
			sl->skinCap   = nv;
			sl->skinCount = 0;
		}
		const Int ci = sl->skinCur ^ 1;
		mesh->Get_Deformed_Vertices(sl->skin[ci]);
		const Bool prevOK = hadPrev && (sl->skinCount == nv);
		if (prevOK)
		{
			const Vector3 *a = sl->skin[ci];
			const Vector3 *b = sl->skin[ci ^ 1];
			for (Int v = 0; v < nv && !moved; ++v)
				if (fabsf(a[v].X - b[v].X) > 1.0e-4f || fabsf(a[v].Y - b[v].Y) > 1.0e-4f || fabsf(a[v].Z - b[v].Z) > 1.0e-4f)
					moved = TRUE;
		}
		sl->skinCur   = ci;
		sl->skinCount = nv;
		skinCur  = sl->skin[ci];
		skinPrev = prevOK ? sl->skin[ci ^ 1] : sl->skin[ci];
	}
	else if (hadPrev)
		for (Int r = 0; r < 3 && !moved; ++r)
			for (Int c = 0; c < 4; ++c)
				if (fabsf(cur[r][c] - prev[r][c]) > 1.0e-4f) { moved = TRUE; break; }
	if (moved)
		sl->movedStamp = s_moverFrame;
	const Bool vel = hadPrev && moved;

	// Ronin @feature 24/09/2026 DX9: reactive meshes are drawn whether they moved or not - a conveyor never moves.
	// A STATIC translucent mesh is left alone: over still ground its history is valid.
	UnsignedByte f = (s_reactiveW > 0.0f) ? classifyModel(mesh, mdl) : 0;
	// Ronin @bugfix 26/09/2026 DX9: translucent at RUNTIME - an opacity override draws an opaque model through the alpha shader
	// (the build-placement preview, fading units); the model's shaders cannot say so. Moving, it is a GHOST (27/09).
	// Get_Alpha_Override is last frame's: Render sets it from rinfo after this note - one frame late at a change, no more.
	const Bool ghost = (s_reactiveW > 0.0f && mesh->Get_Alpha_Override() < 1.0f) ? TRUE : FALSE;
	Int kind = -1;
	if (ghost)
	{
		if (vel)
			kind = TAA_KIND_GHOST;
	}
	else if (f & TAA_MF_TRANSL)
	{
		if (vel || (f & TAA_MF_TIMEVAR))
			kind = TAA_KIND_TRANSLUCENT;
	}
	else if (vel)
		kind = (f & TAA_MF_TIMEVAR) ? TAA_KIND_VEL_REACT : TAA_KIND_VEL;
	else if (f & TAA_MF_TIMEVAR)
		kind = TAA_KIND_REACT;
	// Ronin @bugfix 25/09/2026 DX9: drawn ONE more frame after it stops, so the disocclusion test sees it is still here.
	// Without it every pixel of a unit that just stopped read as vacated ground and reset: a second of shimmer.
	else if (movedLast)
		kind = TAA_KIND_STILL;
	if (isSkin && kind == TAA_KIND_TRANSLUCENT)
		kind = -1;		// skins draw after the translucent pass; none are translucent in practice
	if (kind < 0 || s_meshDrawCount >= TAA_MAX_MESH_DRAWS)
		return;

	TaaMeshDraw &d = s_meshDraws[s_meshDrawCount++];
	d.mesh = mesh;
	d.cur  = cur;
	d.prev = hadPrev ? prev : cur;
	d.kind = kind;
	d.skinCur  = skinCur;
	d.skinPrev = skinPrev;
	if (isSkin)
		++s_skinDrawCount;
	if (kind != TAA_KIND_VEL && kind != TAA_KIND_STILL)
		++s_reactCount;
}
void W3DTaa::setVelocity(Bool on)  { s_velocityOn = on; }
Bool W3DTaa::getVelocity(void)     { return s_velocityOn; }



// Ronin @feature 20/09/2026 DX9: TAA step 2b-ii. This frame's and last frame's UNJITTERED view-projection, for the
// reprojection.
// Ronin @bugfix 20/09/2026 DX9: these are D3D-convention matrices read straight off the DEVICE, not built from
// Matrix4x4. Matrix4x4 is column-vector and D3D is row-vector, so building here and reasoning about HLSL's matrix
// packing on top gave two chances to get the handedness wrong — and it got it wrong. W3DShadowMap.cpp:1509-1518 is the
// convention that demonstrably works in this codebase: GetTransform, multiply view*proj, transpose, mul(rowVec, M).
// Both of these are JITTER-FREE. The device's projection carries this frame's subpixel offset; it is stripped before
// either is built, so a still camera produces an exactly zero reprojection delta. See the note at the D3DXMatrixInverse
// call for why cancelling the jitter here would destroy the accumulation rather than stabilise it.
static D3DXMATRIX s_curViewProjClean;	// this frame, jitter removed — also becomes next frame's s_prevViewProj
static D3DXMATRIX s_prevViewProj;		// last frame's, likewise clean
static Bool       s_curViewProjValid  = FALSE;
static Bool       s_prevViewProjValid = FALSE;

// Ronin @bugfix 20/09/2026 DX9: the 3D viewport, captured in preRender BEFORE the render target changes.
// SetRenderTarget resets the viewport to the whole target — this codebase says so in three places — so asking the
// device for it after endRenderToTexture returns the FULL FRAME BUFFER, not the 3D sub-rect. Reading it there made the
// shader map NDC across the whole screen: every reprojection delta wrong by the ratio between the two, scaling with
// distance from centre, which is exactly what the radial smear was.
static D3DVIEWPORT9 s_viewport  = { 0, 0, 0, 0, 0.0f, 1.0f };

static Bool         s_viewportOK = FALSE;
static float        s_zNear      = 1.0f;
static float        s_zFar       = 1000.0f;



// Ronin @feature 23/09/2026 DX9: the velocity target and last frame's, ping-ponged. Full resolution: per pixel and exact.
static Bool ensureVelocity(UnsignedInt width, UnsignedInt height)
{
	const UnsignedInt vw = (width  > 0) ? width  : 1;
	const UnsignedInt vh = (height > 0) ? height : 1;
	if (s_velTex != NULL && s_velPrevTex != NULL && s_velW == vw && s_velH == vh)
		return TRUE;

	REF_PTR_RELEASE(s_velTex);
	REF_PTR_RELEASE(s_velPrevTex);
	s_prevVelOK = FALSE;
	s_velW = 0;
	s_velH = 0;
	s_velTex     = NEW_REF(TextureClass, (vw, vh, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_1,
										  TextureClass::POOL_DEFAULT, true));
	s_velPrevTex = NEW_REF(TextureClass, (vw, vh, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_1,
										  TextureClass::POOL_DEFAULT, true));
	if (s_velTex->Peek_D3D_Base_Texture() == NULL || s_velPrevTex->Peek_D3D_Base_Texture() == NULL)
	{
		REF_PTR_RELEASE(s_velTex);
		REF_PTR_RELEASE(s_velPrevTex);
		return FALSE;
	}
	s_velW = vw;
	s_velH = vh;
	return TRUE;
}

static Bool ensureHistory(UnsignedInt width, UnsignedInt height)
{
	if (s_history[0] != NULL && s_history[1] != NULL && s_historyW == width && s_historyH == height)
		return TRUE;

	REF_PTR_RELEASE(s_history[0]);
	REF_PTR_RELEASE(s_history[1]);
	s_historyW     = 0;
	s_historyH     = 0;
	s_historyValid = FALSE;		// a resize throws the accumulated image away; it no longer lines up

	REF_PTR_RELEASE(s_depthHist[0]);
	REF_PTR_RELEASE(s_depthHist[1]);
	for (Int i = 0; i < 2; ++i)
	{
		s_depthHist[i] = NEW_REF(TextureClass, (width, height, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_1,
												TextureClass::POOL_DEFAULT, true));
		s_history[i] = NEW_REF(TextureClass, (width, height, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_1,
											  TextureClass::POOL_DEFAULT, true));
		if (s_history[i]->Peek_D3D_Base_Texture() == NULL)
		{
			REF_PTR_RELEASE(s_history[0]);
			REF_PTR_RELEASE(s_history[1]);
			return FALSE;
		}
	}
	// Ronin @bugfix 21/09/2026 DX9: CLEAR them. A fresh POOL_DEFAULT target holds whatever was in that memory, and
	// alpha is now the motion memory — an uncleared buffer would read back as "everything moved" and reject history
	// across the whole screen for the first several frames after a load or a device reset.
	IDirect3DDevice9 *dev = DX8Wrapper::_Get_D3D_Device8();
	IDirect3DSurface9 *oldRT = NULL;
	if (dev != NULL && SUCCEEDED(dev->GetRenderTarget(0, &oldRT)) && oldRT != NULL)
	{
		for (Int i = 0; i < 2; ++i)
		{
			IDirect3DSurface9 *surf = s_history[i]->Get_D3D_Surface_Level();
			if (surf != NULL)
			{
				if (SUCCEEDED(dev->SetRenderTarget(0, surf)))
					dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
				surf->Release();
			}
		}
		dev->SetRenderTarget(0, oldRT);
		oldRT->Release();
	}

	s_historyW = width;
	s_historyH = height;

	return TRUE;
}

// Same blob reader W3DSsao and W3DShadowMap use; file-static in each so no header has to carry it.
static DWORD *readShaderBlob(const char *path)
{
	HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return NULL;
	const DWORD fileSize = GetFileSize(hFile, NULL);
	if (fileSize == 0 || fileSize == INVALID_FILE_SIZE || fileSize < sizeof(DWORD))
	{
		CloseHandle(hFile);
		return NULL;
	}
	DWORD *blob = (DWORD *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, fileSize);
	if (blob == NULL)
	{
		CloseHandle(hFile);
		return NULL;
	}
	DWORD bytesRead = 0;
	const BOOL ok = ReadFile(hFile, blob, fileSize, &bytesRead, NULL);
	CloseHandle(hFile);
	if (!ok || bytesRead != fileSize)
	{
		HeapFree(GetProcessHeap(), 0, blob);
		return NULL;
	}
	return blob;
}

static void ensureShaders(IDirect3DDevice9 *dev)
{
	if (s_shadersTried)
		return;
	s_shadersTried = TRUE;

	DWORD *blob = readShaderBlob("shaders\\ScreenQuad.vso");
	if (blob != NULL)
	{
		if (FAILED(dev->CreateVertexShader(blob, &s_quadVS)))
			s_quadVS = NULL;
		HeapFree(GetProcessHeap(), 0, blob);
	}
	blob = readShaderBlob("shaders\\TaaResolve.pso");
	if (blob != NULL)
	{
		if (FAILED(dev->CreatePixelShader(blob, &s_resolvePS)))
			s_resolvePS = NULL;
		HeapFree(GetProcessHeap(), 0, blob);
	}
	blob = readShaderBlob("shaders\\TaaVelMesh.vso");
	if (blob != NULL)
	{
		if (FAILED(dev->CreateVertexShader(blob, &s_velMeshVS)))
			s_velMeshVS = NULL;
		HeapFree(GetProcessHeap(), 0, blob);
	}
	blob = readShaderBlob("shaders\\TaaVelSkin.vso");
	if (blob != NULL)
	{
		if (FAILED(dev->CreateVertexShader(blob, &s_velSkinVS)))
			s_velSkinVS = NULL;
		HeapFree(GetProcessHeap(), 0, blob);
	}
	blob = readShaderBlob("shaders\\TaaVelMesh.pso");
	if (blob != NULL)
	{
		if (FAILED(dev->CreatePixelShader(blob, &s_velMeshPS)))
			s_velMeshPS = NULL;
		HeapFree(GetProcessHeap(), 0, blob);
	}
	blob = readShaderBlob("shaders\\TaaDepthStore.pso");
	if (blob != NULL)
	{
		if (FAILED(dev->CreatePixelShader(blob, &s_depthStorePS)))
			s_depthStorePS = NULL;
		HeapFree(GetProcessHeap(), 0, blob);
	}
}

// Ronin @bugfix 20/09/2026 DX9: the filter is per stage and the difference matters.
//   POINT  — the scene and the depth. Both are read 1:1 at this pixel, and interpolating depth across an edge would
//            invent geometry that is not there.
//   LINEAR — the REPROJECTED HISTORY, and it must be. Its lookup lands between texels whenever the camera moves by a
//            fraction of a pixel; point sampling snaps that to the nearest texel, so slow panning quantises the
//            reprojection to whole pixels and the accumulated image ripples like water. Bilinear is what makes a
//            subpixel reprojection mean anything.
static void setResolveSampler(IDirect3DDevice9 *dev, DWORD stage, Bool linearFilter = FALSE)
{
	const DWORD filter = linearFilter ? D3DTEXF_LINEAR : D3DTEXF_POINT;
	dev->SetSamplerState(stage, D3DSAMP_MINFILTER, filter);
	dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, filter);
	dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	dev->SetSamplerState(stage, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
	dev->SetSamplerState(stage, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
}

void W3DTaa::shutdown(void)
{
	s_camera = NULL;
	if (s_resolvePS != NULL) { s_resolvePS->Release(); s_resolvePS = NULL; }
	if (s_quadVS    != NULL) { s_quadVS->Release();    s_quadVS    = NULL; }
	if (s_depthStorePS != NULL) { s_depthStorePS->Release(); s_depthStorePS = NULL; }
	if (s_velMeshVS != NULL) { s_velMeshVS->Release(); s_velMeshVS = NULL; }
	if (s_velSkinVS != NULL) { s_velSkinVS->Release(); s_velSkinVS = NULL; }
	REF_PTR_RELEASE(s_opaqueTex);
	s_opaqueW  = 0;
	s_opaqueH  = 0;
	s_opaqueOK = FALSE;
	for (Int i = 0; i < TAA_MESH_SLOTS; ++i)
	{
		delete [] s_meshSlots[i].skin[0];
		delete [] s_meshSlots[i].skin[1];
		s_meshSlots[i].skin[0]   = NULL;
		s_meshSlots[i].skin[1]   = NULL;
		s_meshSlots[i].skinCap   = 0;
		s_meshSlots[i].skinCount = 0;
	}
	delete [] s_skinScratch;
	s_skinScratch    = NULL;
	s_skinScratchCap = 0;
	if (s_velMeshPS != NULL) { s_velMeshPS->Release(); s_velMeshPS = NULL; }
	MeshClass::Set_Taa_Mesh_Note_Func(NULL);
	s_shadersTried = FALSE;

	REF_PTR_RELEASE(s_history[0]);
	REF_PTR_RELEASE(s_history[1]);
	s_historyW     = 0;
	s_historyH     = 0;
	s_historyValid = FALSE;
	REF_PTR_RELEASE(s_depthHist[0]);
	REF_PTR_RELEASE(s_depthHist[1]);
	REF_PTR_RELEASE(s_velTex);
	REF_PTR_RELEASE(s_velPrevTex);
	s_prevVelOK = FALSE;
	s_velW = 0;
	s_velH = 0;
}

Bool W3DTaa::preRender(const CameraClass &camera)
{
	s_redirected = FALSE;
	if (!s_active)
		return FALSE;
	s_camera = &camera;
	s_meshDrawCount = 0;
	s_meshSeen      = 0;
	s_reactCount    = 0;
	s_trkFull       = 0;
	s_skinDrawCount = 0;
	s_appliedValid  = FALSE;	// Apply runs during the scene render and sets it again
	s_meshVpValid   = FALSE;
	s_opaqueOK      = FALSE;
	MeshClass::Set_Taa_Mesh_Note_Func(&W3DTaa::noteMesh);	// §14; noteMesh itself checks the redirect window
	++s_moverFrame;
	if (s_moverFrame == 0) ++s_moverFrame;	// 0 is the "no previous position" marker

	{
		CameraClass &cam = const_cast<CameraClass &>(camera);
		cam.Get_Clip_Planes(s_zNear, s_zFar);
	}

	// The 3D viewport, while it is still the 3D viewport — see the note on s_viewport.
	{
		IDirect3DDevice9 *vpDev = DX8Wrapper::_Get_D3D_Device8();
		s_viewportOK = (vpDev != NULL && SUCCEEDED(vpDev->GetViewport(&s_viewport))) ? TRUE : FALSE;
	}

	IDirect3DDevice9 *dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev == NULL)
		return FALSE;
	ensureShaders(dev);
	if (s_quadVS == NULL || s_resolvePS == NULL)
		return FALSE;

	// A script's view filter (black & white, motion blur, crossfade) already redirects the scene and owns the target.
	// Those are cinematic and rare: stand down for the frame rather than fight over it.
	if (!W3DShaderManager::canRenderToTexture() || W3DShaderManager::isRenderingToTexture())
		return FALSE;

	W3DShaderManager::startRenderToTexture();
	if (!W3DShaderManager::isRenderingToTexture())
		return FALSE;

	s_redirected = TRUE;
	return TRUE;
}

void W3DTaa::postRender(void)
{
	if (!s_redirected)
		return;
	s_redirected = FALSE;

	IDirect3DDevice9 *dev = DX8Wrapper::_Get_D3D_Device8();
	IDirect3DTexture9 *sceneTex = W3DShaderManager::endRenderToTexture();
	if (dev == NULL || sceneTex == NULL || s_quadVS == NULL || s_resolvePS == NULL)
		return;

	// Ronin @feature 20/09/2026 DX9: take the INTZ buffer off the device so it can be SAMPLED. This must come AFTER
	// endRenderToTexture: that call rebinds the frame buffer's own depth, and Peek_Scene_Depth_Texture only hands the
	// texture over while ours is suspended. Resumed at the bottom — the rest of the frame keeps testing against it.
	const Bool depthSuspended = DX8Wrapper::Suspend_Scene_Depth() ? TRUE : FALSE;
	IDirect3DTexture9 *depthTex = depthSuspended ? DX8Wrapper::Peek_Scene_Depth_Texture() : NULL;
	// Ronin @bugfix 23/09/2026 DX9: build the view-projection from the CAMERA, not off the device. W3DShadowMap
	// records the hazard at its own capture site: "reading the device cold returned whatever drew last (particles set
	// VIEW to identity)". TAA had exactly that exposure - this runs after the scene, and Apply_Render_State_Changes
	// pushes whatever the wrapper last cached, which need not be the 3D camera. It looked fine only because the check
	// was `debug 1` flat grey AT REST, and a consistently wrong matrix also gives a zero delta at rest.
	// Get_D3D_Projection_Matrix returns ProjectionTransform, which is CLEAN - CameraClass::Apply jitters a local copy
	// only (camera.cpp:752-754) - so no jitter strip is needed here either.
	s_prevViewProj      = s_curViewProjClean;
	s_prevViewProjValid = s_curViewProjValid;
	if (s_camera != NULL)
	{
		CameraClass &cam = const_cast<CameraClass &>(*s_camera);
		Matrix4x4 projMtx;
		cam.Get_D3D_Projection_Matrix(&projMtx);
		const Matrix4x4 viewMtx(cam.Get_View_Matrix());

		const D3DMATRIX dv = To_D3DMATRIX(viewMtx);
		const D3DMATRIX dp = To_D3DMATRIX(projMtx);
		D3DXMATRIX dxView(dv), dxProj(dp);
		D3DXMatrixMultiply(&s_curViewProjClean, &dxView, &dxProj);	// row-vector: clip = pos * (view*proj)
		s_curViewProjValid = TRUE;

		// Ronin @feature 24/09/2026 DX9: prefer what Apply actually sent during the scene.
		if (s_appliedValid)
			D3DXMatrixMultiply(&s_curViewProjClean, &s_appliedView, &s_appliedProj);
	}
	else
	{
		s_curViewProjValid = FALSE;
	}

	const Bool canReproject = (depthTex != NULL) && s_prevViewProjValid && s_curViewProjValid;

	// The 3D viewport as it was BEFORE the render target changed. Asking the device now would give the whole frame
	// buffer, because endRenderToTexture's SetRenderTarget reset it — see the note on s_viewport.
	if (!s_viewportOK)
		return;
	const D3DVIEWPORT9 vp = s_viewport;
	dev->SetViewport(&vp);

	Int fbW = 0, fbH = 0, bits = 0;
	bool windowed = false;
	WW3D::Get_Render_Target_Resolution(fbW, fbH, bits, windowed);
	if (fbW <= 0 || fbH <= 0 || vp.Width == 0 || vp.Height == 0)
		return;

	DWORD oldZ = 0, oldZW = 0, oldAB = 0, oldAT = 0, oldCull = 0, oldCW = 0;
	dev->GetRenderState(D3DRS_ZENABLE,          &oldZ);
	dev->GetRenderState(D3DRS_ZWRITEENABLE,     &oldZW);
	dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAB);
	dev->GetRenderState(D3DRS_ALPHATESTENABLE,  &oldAT);
	dev->GetRenderState(D3DRS_CULLMODE,         &oldCull);
	dev->GetRenderState(D3DRS_COLORWRITEENABLE, &oldCW);

	dev->SetRenderState(D3DRS_ZENABLE,          FALSE);
	dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);
	dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
	// Ronin @bugfix 21/09/2026 DX9: ALPHA MUST BE WRITABLE. This engine's standing convention is COLORWRITEENABLE =
	// RGB only — the shoreline pass and W3DScene both restore it that way — so without this the resolve's alpha is
	// masked off. The motion memory lives in that alpha, which means it was never stored: histAlpha read back as zero
	// every frame and the "remember motion for a few frames" mechanism, the one taken from the shadow system, has
	// been dead since it was written. The `moving` debug view being pure black is what exposed it.
	dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
												D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);

	// The scene texture is the size of the WHOLE frame buffer and the viewport is a sub-rect of it (the command bar
	// takes the rest), so every quad below covers the viewport while its UVs address that same sub-rect — not 0..1.
	// The history targets are frame-buffer sized for exactly this reason: one UV convention for both passes.
	const float u0 = (float)vp.X / (float)fbW;
	const float v0 = (float)vp.Y / (float)fbH;
	const float u1 = (float)(vp.X + vp.Width)  / (float)fbW;
	const float v1 = (float)(vp.Y + vp.Height) / (float)fbH;

	// Clip space spans the VIEWPORT, so the quad is the full -1..1 rect, and the UVs address the viewport's sub-rect
	// of a frame-buffer-sized texture.
	//
	// Ronin @diagnostic 22/09/2026 DX9: this shift is CORRECT and it is now MEASURED, not argued. `taa debug 7` paints
	// the fractional sample offset: with the shift in, every component reads 0 or 1 - the same point modulo the texel
	// grid - and nothing reads 0.5, so uv lands exactly on texel centres and Catmull-Rom is an identity (f=0 gives
	// w1=1, f=1 gives w2=1 on the next texel over: the same tap). Removing it was tried on a derivation twice, 21/09
	// and 22/09, and blurred the image both times. The derivation is wrong; do not run it a third time.
	const float hx = 1.0f / (float)vp.Width;
	const float hy = 1.0f / (float)vp.Height;
	struct QuadVertex { float x, y, z; float u, v; } q[4];
	q[0].x = -1.0f - hx; q[0].y =  1.0f + hy; q[0].u = u0; q[0].v = v0;
	q[1].x =  1.0f - hx; q[1].y =  1.0f + hy; q[1].u = u1; q[1].v = v0;
	q[2].x = -1.0f - hx; q[2].y = -1.0f + hy; q[2].u = u0; q[2].v = v1;
	q[3].x =  1.0f - hx; q[3].y = -1.0f + hy; q[3].u = u1; q[3].v = v1;
	for (int i = 0; i < 4; ++i)
		q[i].z = 0.0f;

	DX8Wrapper::BindLayoutFVF(D3DFVF_XYZ | D3DFVF_TEX1, "W3DTaa::postRender");
	dev->SetVertexShader(s_quadVS);		// AFTER the layout bind — BindLayoutFVF clears the VS
	dev->SetPixelShader(s_resolvePS);

	const Bool haveHistory = ensureHistory((UnsignedInt)fbW, (UnsignedInt)fbH);

	// Ronin @feature 20/09/2026 DX9: current clip -> previous clip, in one matrix. A pixel's clip position goes back
	// through this frame's inverse view-projection to the world, then forward through last frame's, which is where it
	// sat on screen a frame ago. Transposed on upload, the convention every matrix in this project's shaders uses.
	// ALWAYS upload the viewport rect. The shader tests the history UV against it to reject pixels that were off
	// screen last frame, and that test runs whether or not reprojection is on — leaving c5 stale with reprojection
	// disabled rejected EVERY pixel, so no history was ever used and the jitter came back raw.
	// Ronin @bugfix 24/09/2026 DX9: the resolve rebuilds each pixel's NDC through c5, so it must be the viewport the SCENE
	// drew with. Measured: vp 1600x900 from preRender, the scene 1600x720 - the command bar is excluded.
	// Mapped over 900 rows, every pixel's NDC was wrong: harmless at rest (the same wrong mapping on both frames
	// cancels, which is why debug 1 read flat grey) but wrong while panning, and it was the offset under every mask,
	// rect and mesh silhouette since 22/09.
	const D3DVIEWPORT9 svp = s_meshVpValid ? s_meshVp : vp;
	const float c5[4] = { (float)svp.X / (float)fbW, (float)svp.Y / (float)fbH,
						  (float)svp.Width / (float)fbW, (float)svp.Height / (float)fbH };
	dev->SetPixelShaderConstantF(5, c5, 1);

	if (canReproject)
	{
		// Row-vector convention throughout: clip = pos * VP, so pos = clip * VP^-1 and prevClip = clip * VP^-1 * prevVP.
		// Then transpose and consume with mul(rowVec, M), the same pairing RigidInstance_ps uses for g_LightViewProj.
		// BOTH ends use the CLEAN matrix, and that is not a simplification — it is the point. With a still camera the
		// two matrices are then identical, the delta is exactly zero, and the history is sampled at this very pixel.
		// Using the jittered matrix for the inverse instead leaves a residual delta of -jitter that changes every
		// frame: the image wobbles while still, and keeps wobbling for about a second after the camera stops while the
		// EMA bleeds the error out. You must NOT cancel the jitter when sampling history — the history holds the
		// resolved colour at each pixel CENTRE, and the current frame's offset sample is the only new information the
		// frame carries. Averaging those offset samples IS the supersampling.
		D3DXMATRIX curInv, reproj, reprojT;
		if (D3DXMatrixInverse(&curInv, NULL, &s_curViewProjClean) != NULL)
		{
			D3DXMatrixMultiply(&reproj, &curInv, &s_prevViewProj);
			D3DXMatrixTranspose(&reprojT, &reproj);
			dev->SetPixelShaderConstantF(1, (const float *)&reprojT, 4);

			dev->SetTexture(2, depthTex);
			setResolveSampler(dev, 2);	// POINT: a depth texel must never be interpolated across an edge
		}
	}

	s_lastVelOK = FALSE;

	// ---- velocity drawn FROM THE MESHES, §14 ---------------------------------------------------------------------
	// Each mesh that moved this frame is re-drawn from its own vertex and triangle arrays, through its current and
	// previous world-view-projection. The pixel shader discards anything behind the scene depth, so only the visible
	// surface writes - the ownership the rectangles never had. Depth test off, blend off, cull none: all already set
	// for the resolve above.
	if (s_velocityOn && (s_meshDrawCount > 0 || cursorBibLive()) && s_velMeshVS != NULL && s_velMeshPS != NULL &&
		depthTex != NULL && s_curViewProjValid && s_prevViewProjValid && ensureVelocity((UnsignedInt)fbW, (UnsignedInt)fbH))
	{
		IDirect3DSurface9 *vSurf  = s_velTex->Get_D3D_Surface_Level();
		IDirect3DSurface9 *prevRT = NULL;
		if (vSurf != NULL && SUCCEEDED(dev->GetRenderTarget(0, &prevRT)) && prevRT != NULL)
		{
			if (SUCCEEDED(dev->SetRenderTarget(0, vSurf)))
			{
				dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);	// A 0 = not written
				// Ronin @bugfix 24/09/2026 DX9: rasterise with the viewport the SCENE drew with, so each
				// velocity texel lines up with the scene and depth texels it describes.
				const D3DVIEWPORT9 vvp = s_meshVpValid ? s_meshVp : vp;
				dev->SetViewport(&vvp);		// SetRenderTarget reset it to the whole target
				DX8Wrapper::BindLayoutFVF(D3DFVF_XYZ, "W3DTaa::velMesh");
				dev->SetVertexShader(s_velMeshVS);	// AFTER the layout bind - BindLayoutFVF clears the VS
				dev->SetPixelShader(s_velMeshPS);
				dev->SetTexture(2, depthTex);
				setResolveSampler(dev, 2);		// POINT
				const float pc0[4] = { (float)vvp.Width, (float)vvp.Height, s_zNear, s_zFar };
				const float pc1[4] = { (float)vvp.X / (float)fbW, (float)vvp.Y / (float)fbH,
									   (float)vvp.Width / (float)fbW, (float)vvp.Height / (float)fbH };
				dev->SetPixelShaderConstantF(10, pc0, 1);	// c10/c11 - c0..c9 belong to the resolve, c1..c4 is g_Reproject
				dev->SetPixelShaderConstantF(11, pc1, 1);
				s_lastVelOK = TRUE;

				// Ronin @feature 24/09/2026 DX9: translucent FIRST, so an opaque mover under a rotor overwrites it with its
				// velocity (which already forces the clamp). c12 = { alpha out, one-sided depth test, zero velocity }.
				// Ronin @bugfix 26/09/2026 DX9: the placement bib FIRST, so anything moving over it overwrites it. c12.y = 2: no
				// depth test - the corners are a flat quad at the preview's height and the terrain under it need not be flat.
				if (cursorBibLive())
				{
					static const float bibC12[4] = { 0.3f, 2.0f, 1.0f, 0.0f };	// GHOST, no depth test, zero velocity
					static const UnsignedShort bibIdx[6] = { 0, 1, 2, 0, 2, 3 };
					dev->SetPixelShaderConstantF(12, bibC12, 1);
					D3DXMATRIX bcT, bpT;
					D3DXMatrixTranspose(&bcT, &s_curViewProjClean);
					D3DXMatrixTranspose(&bpT, &s_prevViewProj);
					dev->SetVertexShaderConstantF(0, (const float *)&bcT, 4);
					dev->SetVertexShaderConstantF(4, (const float *)&bpT, 4);
					dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, bibIdx, D3DFMT_INDEX16,
												s_bibCorners, sizeof(Vector3));
				}

				Int boundKind = -1;
				for (Int k = 0; k < 2 * s_meshDrawCount; ++k)
				{
					TaaMeshDraw &md = s_meshDraws[k % s_meshDrawCount];
					const Bool firstGroup = (md.kind == TAA_KIND_TRANSLUCENT || md.kind == TAA_KIND_GHOST) ? TRUE : FALSE;
					if (firstGroup != (k < s_meshDrawCount))
						continue;
					if (md.skinCur != NULL)
						continue;		// skins: their own layout, below
					MeshModelClass *mdl = md.mesh->Peek_Model();
					if (mdl == NULL)
						continue;
					if (md.kind != boundKind)
					{
						static const float kindC12[6][4] = {
							{ 1.0f, 0.0f, 0.0f, 0.0f },		// velocity
							{ 0.7f, 0.0f, 0.0f, 0.0f },		// velocity + reactive
							{ 0.4f, 0.0f, 1.0f, 0.0f },		// reactive, opaque
							{ 0.4f, 1.0f, 1.0f, 0.0f },		// reactive, translucent: writes no depth, so one-sided
							{ 0.15f, 0.0f, 1.0f, 0.0f },	// still: stopped this frame, present but no mover
							{ 0.3f,  1.0f, 1.0f, 0.0f } };	// ghost: moving runtime-translucent, one-sided, reset by the resolve
						dev->SetPixelShaderConstantF(12, kindC12[md.kind], 1);
						boundKind = md.kind;
					}
					const Int vcount = mdl->Get_Vertex_Count();
					const Int pcount = mdl->Get_Polygon_Count();
					if (vcount <= 0 || pcount <= 0 || vcount > 65535)	// 16-bit indices
						continue;
					const Vector3  *verts = mdl->Get_Vertex_Array();
					const TriIndex *polys = mdl->Get_Polygon_Array();
					if (verts == NULL || polys == NULL)
						continue;

					// World in the D3D row-vector form Set_Transform(D3DTS_WORLD) uses, then x view-projection, then
					// transposed for mul(rowVec, M). The previous one pairs last frame's world with last frame's camera.
					D3DXMATRIX wCur(To_D3DMATRIX(md.cur)), wPrev(To_D3DMATRIX(md.prev));
					D3DXMATRIX cWVP, pWVP, cT, pT;
					D3DXMatrixMultiply(&cWVP, &wCur,  &s_curViewProjClean);
					D3DXMatrixMultiply(&pWVP, &wPrev, &s_prevViewProj);
					D3DXMatrixTranspose(&cT, &cWVP);
					D3DXMatrixTranspose(&pT, &pWVP);
					dev->SetVertexShaderConstantF(0, (const float *)&cT, 4);
					dev->SetVertexShaderConstantF(4, (const float *)&pT, 4);
					dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)vcount, (UINT)pcount,
												polys, D3DFMT_INDEX16, verts, sizeof(Vector3));
				}

				// Ronin @feature 26/09/2026 DX9: §14 stage 2 - SKINS. Already in world space, so only the view-projections;
				// each vertex carries this frame's and last frame's position. Same pixel shader as the rigid draws.
				if (s_skinDrawCount > 0 && s_velSkinVS != NULL)
				{
					DX8Wrapper::BindLayoutFVF(D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0), "W3DTaa::velSkin");
					dev->SetVertexShader(s_velSkinVS);	// AFTER the layout bind - it clears the VS
					setResolveSampler(dev, 2);			// POINT, re-set after the rebind
					D3DXMATRIX cT, pT;
					D3DXMatrixTranspose(&cT, &s_curViewProjClean);
					D3DXMatrixTranspose(&pT, &s_prevViewProj);
					dev->SetVertexShaderConstantF(0, (const float *)&cT, 4);
					dev->SetVertexShaderConstantF(4, (const float *)&pT, 4);
					for (Int k = 0; k < s_meshDrawCount; ++k)
					{
						TaaMeshDraw &md = s_meshDraws[k];
						if (md.skinCur == NULL)
							continue;
						MeshModelClass *mdl = md.mesh->Peek_Model();
						if (mdl == NULL)
							continue;
						const Int vcount = mdl->Get_Vertex_Count();
						const Int pcount = mdl->Get_Polygon_Count();
						const TriIndex *polys = mdl->Get_Polygon_Array();
						if (vcount <= 0 || pcount <= 0 || vcount > 65535 || polys == NULL)
							continue;
						if (md.kind != boundKind)
						{
							static const float skinC12[6][4] = {
								{ 1.0f, 0.0f, 0.0f, 0.0f }, { 0.7f, 0.0f, 0.0f, 0.0f }, { 0.4f, 0.0f, 1.0f, 0.0f },
								{ 0.4f, 1.0f, 1.0f, 0.0f }, { 0.15f, 0.0f, 1.0f, 0.0f }, { 0.3f, 1.0f, 1.0f, 0.0f } };	// as kindC12 above
							dev->SetPixelShaderConstantF(12, skinC12[md.kind], 1);
							boundKind = md.kind;
						}
						if (s_skinScratchCap < vcount)
						{
							delete [] s_skinScratch;
							s_skinScratch    = W3DNEWARRAY float[vcount * 6];
							s_skinScratchCap = vcount;
						}
						for (Int v = 0; v < vcount; ++v)
						{
							float *o = s_skinScratch + v * 6;
							o[0] = md.skinCur[v].X;  o[1] = md.skinCur[v].Y;  o[2] = md.skinCur[v].Z;
							o[3] = md.skinPrev[v].X; o[4] = md.skinPrev[v].Y; o[5] = md.skinPrev[v].Z;
						}
						dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)vcount, (UINT)pcount,
													polys, D3DFMT_INDEX16, s_skinScratch, sizeof(float) * 6);
					}
				}
			}
			dev->SetRenderTarget(0, prevRT);
			prevRT->Release();
		}
		if (vSurf != NULL)
			vSurf->Release();
		dev->SetViewport(&vp);

		// The resolve passes below assume their layout and shaders are still bound.
		DX8Wrapper::BindLayoutFVF(D3DFVF_XYZ | D3DFVF_TEX1, "W3DTaa::postRender");
		dev->SetVertexShader(s_quadVS);
		dev->SetPixelShader(s_resolvePS);
	}


	dev->SetTexture(0, sceneTex);
	setResolveSampler(dev, 0);

	// ---- pass 1: scene + previous history -> this frame's history ---------------------------------------------
	// Blending into the history target rather than straight to screen is what makes the accumulation compound: next
	// frame reads what we write here, not the one-frame-old back buffer.
	IDirect3DSurface9 *oldRT = NULL;
	Bool wroteHistory = FALSE;
	if (haveHistory && SUCCEEDED(dev->GetRenderTarget(0, &oldRT)) && oldRT != NULL)
	{
		TextureClass *cur  = s_history[s_historyIndex];
		TextureClass *prev = s_history[s_historyIndex ^ 1];
		IDirect3DSurface9 *curSurf = cur->Get_D3D_Surface_Level();
		if (curSurf != NULL)
		{
			if (SUCCEEDED(dev->SetRenderTarget(0, curSurf)))
			{
				// SetRenderTarget resets the viewport to the whole target; the 3D view is a sub-rect of it.
				dev->SetViewport(&vp);

				// Only trust the history once a frame has actually been written into it. c0.w turns reprojection on:
				// without depth or a previous matrix the shader keeps the history where it is, which is 2b-i's
				// behaviour — softer while panning, but never wrong.
				const float weight = s_historyValid ? s_weight : 0.0f;
				const float c0[4] = { weight, 1.0f / (float)fbW, 1.0f / (float)fbH,
									  canReproject ? 1.0f : 0.0f };
				dev->SetPixelShaderConstantF(0, c0, 1);

				// Debug views draw in this pass so they show what the RESOLVE sees, not a re-derived picture.
				// yz carry the clip planes: raw D3D depth sits above 0.99 for almost the whole scene, so the depth
				// debug view has to LINEARISE or it is a white screen that tells you nothing. (It told me nothing.)
				const float c6[4] = { (float)s_debugMode, s_zNear, s_zFar, 0.0f };
				const float c7[4] = { 0.0f, (s_lastVelOK && s_velTex != NULL) ? 1.0f : 0.0f, 0.0f, s_clamp };	// x = 0: no CAS into the history
				dev->SetPixelShaderConstantF(6, c6, 1);
				dev->SetPixelShaderConstantF(7, c7, 1);

				const Bool usePrevVel = (s_prevVelOK && s_velPrevTex != NULL);	// disocc AND last frame's reactive flags
				if (usePrevVel)
				{
					dev->SetTexture(6, s_velPrevTex->Peek_D3D_Texture());
					setResolveSampler(dev, 6);		// POINT - the alpha is a flag, not a value to blend
				}
				const float c10[4] = { usePrevVel ? 1.0f : 0.0f, (float)s_disocc, s_reactiveW, 0.0f };
				dev->SetPixelShaderConstantF(10, c10, 1);	// uploaded HERE, after the velocity pass used c10/c11
				const Bool useAuto = autoReactUsable(fbW, fbH);
				if (useAuto)
				{
					dev->SetTexture(7, s_opaqueTex->Peek_D3D_Texture());
					setResolveSampler(dev, 7);		// POINT - compared 1:1 with the scene
				}
				const float c11[4] = { 0.0f, 0.0f, s_disoccV, useAuto ? s_autoReact : 0.0f };
				dev->SetPixelShaderConstantF(11, c11, 1);	// likewise after the velocity pass

				if (s_lastVelOK && s_velTex != NULL)
				{
					dev->SetTexture(5, s_velTex->Peek_D3D_Texture());
					// POINT: packed velocity and kind flags must never be interpolated.
					setResolveSampler(dev, 5);
				}
				dev->SetTexture(1, prev->Peek_D3D_Texture());
				setResolveSampler(dev, 1, TRUE);	// LINEAR — the reprojected lookup lands between texels
				// POINT: a packed depth must never be interpolated - blending two encodings gives a nonsense value.
				if (s_depthHist[s_historyIndex ^ 1] != NULL)
				{
					dev->SetTexture(4, s_depthHist[s_historyIndex ^ 1]->Peek_D3D_Texture());
					setResolveSampler(dev, 4);
				}
				dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVertex));
				dev->SetTexture(1, NULL);
				dev->SetTexture(4, NULL);
				dev->SetTexture(5, NULL);
				dev->SetTexture(6, NULL);
				dev->SetTexture(7, NULL);
				wroteHistory = TRUE;
			}
			curSurf->Release();
		}
		// ---- depth store: this frame's linear depth -> depthHist[cur], for next frame's validation ------------
		IDirect3DSurface9 *dSurf = (wroteHistory && depthTex != NULL && s_depthStorePS != NULL && s_depthHist[s_historyIndex] != NULL)
								   ? s_depthHist[s_historyIndex]->Get_D3D_Surface_Level() : NULL;
		if (dSurf != NULL)
		{
			if (SUCCEEDED(dev->SetRenderTarget(0, dSurf)))
			{
				dev->SetViewport(&vp);
				dev->SetPixelShader(s_depthStorePS);
				// Ronin @feature 26/09/2026 DX9: c0.w = the auto-reactive threshold; the flag goes in alpha.
				const Bool useAutoD = autoReactUsable(fbW, fbH);
				const float dcfg[4] = { 0.0f, s_zNear, s_zFar, useAutoD ? s_autoReact : 0.0f };
				dev->SetPixelShaderConstantF(0, dcfg, 1);
				dev->SetTexture(2, depthTex);
				setResolveSampler(dev, 2);
				dev->SetTexture(0, sceneTex);
				setResolveSampler(dev, 0);
				if (useAutoD)
				{
					dev->SetTexture(7, s_opaqueTex->Peek_D3D_Texture());
					setResolveSampler(dev, 7);
				}
				dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVertex));
				dev->SetTexture(7, NULL);
				dev->SetPixelShader(s_resolvePS);
			}
			dSurf->Release();
		}

		dev->SetRenderTarget(0, oldRT);
		dev->SetViewport(&vp);
		oldRT->Release();
	}

	// ---- pass 2: the resolved image -> the frame buffer ---------------------------------------------------------
	// Weight 0 makes the shader a straight copy of stage 0. If pass 1 could not run we fall back to copying the raw
	// scene, so a missing history target costs quality, never a broken frame.
	{
		// RGB only for the screen copy. Alpha had to be writable for the history above, but the frame buffer's alpha
		// belongs to the soft water edge (Windowednew §31) and nothing here should be writing it.
		dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
													D3DCOLORWRITEENABLE_BLUE);
		const float c0[4] = { 0.0f, 1.0f / (float)fbW, 1.0f / (float)fbH, 0.0f };
		dev->SetPixelShaderConstantF(0, c0, 1);
		// Debug 3 is the ONE view that must run in this pass: it reads back the motion memory the resolve stored in
		// alpha. Running it in the resolve pass instead would write the debug colour into the history and corrupt the
		// very signal being measured. Every other view stays off here.
		const float dbg = (s_debugMode == 3) ? 3.0f : 0.0f;
		const float c6off[4] = { dbg, 0.0f, 0.0f, 0.0f };
		dev->SetPixelShaderConstantF(6, c6off, 1);
		const float c7cas[4] = { s_sharpen, 0.0f, 0.0f, 0.0f };	// Ronin @feature 26/09/2026 DX9: CAS here, never in pass 1
		dev->SetPixelShaderConstantF(7, c7cas, 1);
		dev->SetTexture(0, wroteHistory ? s_history[s_historyIndex]->Peek_D3D_Texture()
										: (IDirect3DBaseTexture9 *)sceneTex);
		setResolveSampler(dev, 0);
		dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVertex));
	}

	if (wroteHistory)
	{
		s_historyValid = TRUE;
		s_historyIndex ^= 1;		// what we just wrote becomes next frame's "previous"

		// Ronin @feature 24/09/2026 DX9: this frame's velocity becomes next frame's "where movers were".
		TextureClass *t = s_velPrevTex;
		s_velPrevTex = s_velTex;
		s_velTex     = t;
		s_prevVelOK  = s_lastVelOK;
	}

	dev->SetVertexShader(NULL);
	dev->SetPixelShader(NULL);
	dev->SetTexture(0, NULL);
	dev->SetTexture(2, NULL);
	if (depthSuspended)
		DX8Wrapper::Resume_Scene_Depth();
	dev->SetRenderState(D3DRS_ZENABLE,          oldZ);
	dev->SetRenderState(D3DRS_ZWRITEENABLE,     oldZW);
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAB);
	dev->SetRenderState(D3DRS_ALPHATESTENABLE,  oldAT);
	dev->SetRenderState(D3DRS_CULLMODE,         oldCull);
	// Back to whatever the frame had — the soft water edge depends on alpha writes being OFF (Windowednew §31).
	dev->SetRenderState(D3DRS_COLORWRITEENABLE, oldCW);

	// Our raw SetRenderState / SetTexture calls desynced the wrapper's caches (§12a) — force a resync, same as SSAO.
	ShaderClass::Invalidate();
	DX8Wrapper::Invalidate_Cached_Render_States();
	DX8Wrapper::Invalidate_Texture_State(0, 1);
}
