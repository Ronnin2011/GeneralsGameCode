/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 screen-space ambient occlusion.
*/

// Ronin @feature 13/09/2026 DX9: SSAO. The depth swap itself lives in DX8Wrapper (Begin_Scene_Depth), which owns every
// device reset; this file gates it on SSAOQuality, runs the AO pass mid-frame and draws the debug views.

#include "WW3D2/dx8todx9.h"
#include "WWLib/always.h"
#include "Lib/BaseType.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/camera.h"
#include "WW3D2/texture.h"
#include "Common/GlobalData.h"
#include "W3DDevice/GameClient/W3DSsao.h"

// Ronin @feature 13/09/2026 DX9: SSAO step 2. The two tuning knobs, in world units and as a multiplier.
static const float AO_RADIUS    = 20.0f;

// Ronin @feature 20/09/2026 DX9: SSAO quality tiers, indexed by TheGlobalData->m_ssaoQuality (0 Off .. 3 Ultra).
// Measured 20/09/2026 on a 230 fps scene: the High path costs ~0.12 ms/frame (230 -> 224 fps), so every tier is
// affordable and the split is about scaling down for weaker cards, not about clawing back time here.
//   divisor - render-target divisor off the depth buffer's size: 2 = half resolution, 1 = full.
//   samples - kernel taps per pixel.
//   stride  - step through KERNEL[]. NOT a prefix: the kernel's lengths grow with index, so the first N entries are
//             all close-contact and a prefix would change the CHARACTER of the shading, not just its noise.
struct SsaoTier
{
	Int divisor;
	Int samples;
	Int stride;
};
static const SsaoTier AO_TIERS[4] =
{
	{ 2,  0, 1 },   // 0 Off    - never used; renderPass returns before this is read
	{ 2,  6, 2 },   // 1 Normal - half res, every other kernel entry
	{ 2, 12, 1 },   // 2 High   - half res, the whole kernel (the path tuned and verified 2026-09-14)
	{ 1, 12, 1 },   // 3 Ultra  - full res, the whole kernel
};

static const SsaoTier &activeTier(void)
{
	Int q = (TheGlobalData != NULL) ? TheGlobalData->m_ssaoQuality : 0;
	if (q < 0) q = 0;
	if (q > 3) q = 3;
	return AO_TIERS[q];
}
static const float AO_INTENSITY = 1.0f;
// Ronin @feature 13/09/2026 DX9: SSAO step 3a. The blur ignores a tap whose depth differs by more than this fraction of
// the centre's distance — that is what keeps occlusion from bleeding across object edges.
static const float BLUR_DEPTH_TOLERANCE = 0.02f;

static IDirect3DVertexShader9	*s_quadVS           = NULL;
static IDirect3DPixelShader9	*s_depthDebugPS     = NULL;
static IDirect3DPixelShader9	*s_aoRawPS          = NULL;
static IDirect3DPixelShader9	*s_blurPS           = NULL;
static IDirect3DPixelShader9	*s_showPS           = NULL;
static Bool						s_shadersTried      = FALSE;
static Bool						s_activeThisFrame   = FALSE;
static Bool						s_aoReadyThisFrame  = FALSE;

// Ronin @feature 13/09/2026 DX9: SSAO step 3a. Half-resolution ping-pong pair. TextureClass render targets, so
// DX8TextureManagerClass recreates them across a device reset — the shadow map's colour targets rely on the same.
static TextureClass				*s_aoTarget[2]      = { NULL, NULL };
static UnsignedInt				s_aoWidth           = 0;
static UnsignedInt				s_aoHeight          = 0;

// Same reader as W3DShadowMap.cpp's loadShaderBlob, which is file-static there. Caller frees with HeapFree.
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

static IDirect3DPixelShader9 *loadPixelShader(IDirect3DDevice9 *dev, const char *path)
{
	IDirect3DPixelShader9 *ps = NULL;
	DWORD *blob = readShaderBlob(path);
	if (blob != NULL)
	{
		if (FAILED(dev->CreatePixelShader(blob, &ps)))
			ps = NULL;
		HeapFree(GetProcessHeap(), 0, blob);
	}
	return ps;
}

static IDirect3DVertexShader9 *loadVertexShader(IDirect3DDevice9 *dev, const char *path)
{
	IDirect3DVertexShader9 *vs = NULL;
	DWORD *blob = readShaderBlob(path);
	if (blob != NULL)
	{
		if (FAILED(dev->CreateVertexShader(blob, &vs)))
			vs = NULL;
		HeapFree(GetProcessHeap(), 0, blob);
	}
	return vs;
}

static void ensureShaders(IDirect3DDevice9 *dev)
{
	if (s_shadersTried)
		return;
	s_shadersTried = TRUE;
	s_quadVS       = loadVertexShader(dev, "shaders\\ScreenQuad.vso");
	s_depthDebugPS = loadPixelShader(dev, "shaders\\SceneDepthDebug.pso");
	s_aoRawPS      = loadPixelShader(dev, "shaders\\SsaoRaw.pso");
	s_blurPS       = loadPixelShader(dev, "shaders\\SsaoBlur.pso");
	s_showPS       = loadPixelShader(dev, "shaders\\SsaoShow.pso");
}

// Ronin @feature 13/09/2026 DX9: SSAO step 3a. (Re)create the half-resolution pair when the size changes.
static Bool ensureTargets(UnsignedInt width, UnsignedInt height)
{
	if (s_aoTarget[0] != NULL && s_aoTarget[1] != NULL && s_aoWidth == width && s_aoHeight == height)
		return TRUE;

	REF_PTR_RELEASE(s_aoTarget[0]);
	REF_PTR_RELEASE(s_aoTarget[1]);
	s_aoWidth  = 0;
	s_aoHeight = 0;

	for (Int i = 0; i < 2; ++i)
	{
		s_aoTarget[i] = NEW_REF(TextureClass, (width, height, WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_1,
											   TextureClass::POOL_DEFAULT, true));
		if (s_aoTarget[i]->Peek_D3D_Base_Texture() == NULL)
		{
			REF_PTR_RELEASE(s_aoTarget[0]);
			REF_PTR_RELEASE(s_aoTarget[1]);
			return FALSE;
		}
	}
	s_aoWidth  = width;
	s_aoHeight = height;
	return TRUE;
}

static void setSampler(IDirect3DDevice9 *dev, DWORD stage, Bool linear)
{
	const DWORD filter = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
	dev->SetSamplerState(stage, D3DSAMP_MINFILTER, filter);
	dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, filter);
	dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}

// Ronin @feature 13/09/2026 DX9: SSAO step 3a. Every render state the passes touch, read raw and written back raw; the
// wrapper cache is invalidated afterwards so it re-asserts its own view. Same rule as the terrain shadow pass (§12a).
struct SsaoSavedStates
{
	DWORD zEnable, zWrite, alphaBlend, alphaTest, stencil, cull, colorWrite, srcBlend, destBlend;

	void save(IDirect3DDevice9 *dev)
	{
		dev->GetRenderState(D3DRS_ZENABLE,          &zEnable);
		dev->GetRenderState(D3DRS_ZWRITEENABLE,     &zWrite);
		dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
		dev->GetRenderState(D3DRS_ALPHATESTENABLE,  &alphaTest);
		dev->GetRenderState(D3DRS_STENCILENABLE,    &stencil);
		dev->GetRenderState(D3DRS_CULLMODE,         &cull);
		dev->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
		dev->GetRenderState(D3DRS_SRCBLEND,         &srcBlend);		// Ronin @feature 13/09/2026 DX9: SSAO 3b — composite blend
		dev->GetRenderState(D3DRS_DESTBLEND,        &destBlend);
	}

	void restore(IDirect3DDevice9 *dev)
	{
		dev->SetRenderState(D3DRS_ZENABLE,          zEnable);
		dev->SetRenderState(D3DRS_ZWRITEENABLE,     zWrite);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, alphaBlend);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE,  alphaTest);
		dev->SetRenderState(D3DRS_STENCILENABLE,    stencil);
		dev->SetRenderState(D3DRS_CULLMODE,         cull);
		dev->SetRenderState(D3DRS_COLORWRITEENABLE, colorWrite);
		dev->SetRenderState(D3DRS_SRCBLEND,         srcBlend);
		dev->SetRenderState(D3DRS_DESTBLEND,        destBlend);
	}
};

static void setPassStates(IDirect3DDevice9 *dev)
{
	dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
						D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
}

// Ronin @feature 13/09/2026 DX9: SSAO step 2. SsaoRaw's c0..c3 from the camera that rendered the depth buffer.
static void uploadAOConstants(IDirect3DDevice9 *dev, const CameraClass &camera, UnsignedInt depthW, UnsignedInt depthH)
{
	float zNear = 1.0f;
	float zFar  = 1000.0f;
	camera.Get_Clip_Planes(zNear, zFar);
	Vector2 planeMin, planeMax, vpMin, vpMax;
	camera.Get_View_Plane(planeMin, planeMax);
	camera.Get_Viewport(vpMin, vpMax);
	const float c[16] =
	{
		zNear, zFar, AO_RADIUS, AO_INTENSITY,
		planeMin.X, planeMax.Y, planeMax.X - planeMin.X, planeMax.Y - planeMin.Y,
		vpMin.X, vpMin.Y, vpMax.X - vpMin.X, vpMax.Y - vpMin.Y,
		1.0f / (float)depthW, 1.0f / (float)depthH, (float)depthW, (float)depthH
	};
	dev->SetPixelShaderConstantF(0, c, 4);

	// Ronin @feature 20/09/2026 DX9: c4 - the tier's sample count, kernel stride, and 1/count for the normalise.
	const SsaoTier &tier = activeTier();
	const float q[4] = { (float)tier.samples, (float)tier.stride,
						 (tier.samples > 0) ? (1.0f / (float)tier.samples) : 0.0f, 0.0f };
	dev->SetPixelShaderConstantF(4, q, 1);
}

// Ronin @feature 13/09/2026 DX9: SSAO. A target-pixel rectangle with UV 0..1 across it, through ScreenQuad.vso.
// D3D9 puts texel centres half a pixel off pixel centres, so the geometry moves by half a pixel, not the UVs.
// The layout goes through the wrapper — a raw SetFVF leaves its cached layout stale, the §29 tree-flicker bug.
static void drawScreenQuad(IDirect3DDevice9 *dev, IDirect3DPixelShader9 *ps, float targetW, float targetH,
						   float x0, float y0, float w, float h)
{
	const float l = ((x0 - 0.5f) / targetW) * 2.0f - 1.0f;
	const float r = ((x0 + w - 0.5f) / targetW) * 2.0f - 1.0f;
	const float t = 1.0f - ((y0 - 0.5f) / targetH) * 2.0f;
	const float b = 1.0f - ((y0 + h - 0.5f) / targetH) * 2.0f;

	struct QuadVertex { float x, y, z; float u, v; } q[4];
	q[0].x = l; q[0].y = t; q[0].u = 0.0f; q[0].v = 0.0f;
	q[1].x = r; q[1].y = t; q[1].u = 1.0f; q[1].v = 0.0f;
	q[2].x = l; q[2].y = b; q[2].u = 0.0f; q[2].v = 1.0f;
	q[3].x = r; q[3].y = b; q[3].u = 1.0f; q[3].v = 1.0f;
	for (int i = 0; i < 4; ++i)
		q[i].z = 0.0f;

	DX8Wrapper::BindLayoutFVF(D3DFVF_XYZ | D3DFVF_TEX1, "W3DSsao::drawScreenQuad");
	dev->SetVertexShader(s_quadVS);		// AFTER the layout bind — BindLayoutFVF clears the VS
	dev->SetPixelShader(ps);
	dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVertex));
}

void W3DSsao::getTargetSize(Int *outW, Int *outH)
{
	if (outW != NULL) *outW = (Int)s_aoWidth;
	if (outH != NULL) *outH = (Int)s_aoHeight;
}

Int W3DSsao::getSampleCount(void)
{
	return activeTier().samples;
}

Int W3DSsao::getQuality(void)
{
	return (TheGlobalData != NULL) ? TheGlobalData->m_ssaoQuality : 0;
}

void W3DSsao::beginFrame(void)
{
	s_aoReadyThisFrame = FALSE;
	s_activeThisFrame  = (getQuality() > 0 && DX8Wrapper::Begin_Scene_Depth()) ? TRUE : FALSE;
}

// Ronin @feature 13/09/2026 DX9: SSAO step 3a. Everything solid is in the depth buffer and nothing see-through has drawn.
// Raw AO into target 0, then the depth-aware blur across into target 1 and down back into target 0.
void W3DSsao::renderPass(const CameraClass &camera)
{
	if (!s_activeThisFrame)
		return;

	IDirect3DDevice9 *dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev == NULL)
		return;
	ensureShaders(dev);
	if (s_quadVS == NULL || s_aoRawPS == NULL || s_blurPS == NULL)
		return;

	// A second render target still bound would force every target to one size. The trees release theirs before here.
	IDirect3DSurface9 *rt1 = NULL;
	if (SUCCEEDED(dev->GetRenderTarget(1, &rt1)) && rt1 != NULL)
	{
		rt1->Release();
		return;
	}

	if (!DX8Wrapper::Suspend_Scene_Depth())
		return;

	IDirect3DTexture9 *depthTex = DX8Wrapper::Peek_Scene_Depth_Texture();
	D3DSURFACE_DESC dd;
	IDirect3DSurface9 *oldRT = NULL;
	if (depthTex == NULL || FAILED(depthTex->GetLevelDesc(0, &dd)) ||
		!ensureTargets((dd.Width + activeTier().divisor - 1) / activeTier().divisor,
					   (dd.Height + activeTier().divisor - 1) / activeTier().divisor) ||
		FAILED(dev->GetRenderTarget(0, &oldRT)) || oldRT == NULL)
	{
		DX8Wrapper::Resume_Scene_Depth();
		return;
	}

	D3DVIEWPORT9 oldVP;
	const Bool haveVP = SUCCEEDED(dev->GetViewport(&oldVP));
	SsaoSavedStates saved;
	saved.save(dev);
	setPassStates(dev);

	const float aoW = (float)s_aoWidth;
	const float aoH = (float)s_aoHeight;
	IDirect3DSurface9 *surf0 = s_aoTarget[0]->Get_D3D_Surface_Level();
	IDirect3DSurface9 *surf1 = s_aoTarget[1]->Get_D3D_Surface_Level();

	if (surf0 != NULL && surf1 != NULL)
	{
		// Raw AO. SetRenderTarget resets the viewport to the whole half-size target, which is what the quad expects.
		dev->SetRenderTarget(0, surf0);
		uploadAOConstants(dev, camera, dd.Width, dd.Height);
		dev->SetTexture(0, depthTex);
		setSampler(dev, 0, FALSE);
		drawScreenQuad(dev, s_aoRawPS, aoW, aoH, 0.0f, 0.0f, aoW, aoH);

		float zNear = 1.0f;
		float zFar  = 1000.0f;
		camera.Get_Clip_Planes(zNear, zFar);
		const float blurParams[4] = { zNear, zFar, BLUR_DEPTH_TOLERANCE, 0.0f };
		const float stepAcross[4] = { 1.0f / aoW, 0.0f, 0.0f, 0.0f };
		const float stepDown[4]   = { 0.0f, 1.0f / aoH, 0.0f, 0.0f };
		dev->SetTexture(1, depthTex);
		setSampler(dev, 1, FALSE);

		// Blur across: target 0 -> target 1.
		dev->SetRenderTarget(0, surf1);
		dev->SetPixelShaderConstantF(0, blurParams, 1);
		dev->SetPixelShaderConstantF(1, stepAcross, 1);
		const float depthTexel[4] = { 1.0f / (float)dd.Width, 1.0f / (float)dd.Height, (float)dd.Width, (float)dd.Height };
		dev->SetPixelShaderConstantF(2, depthTexel, 1);
		dev->SetTexture(0, s_aoTarget[0]->Peek_D3D_Texture());
		drawScreenQuad(dev, s_blurPS, aoW, aoH, 0.0f, 0.0f, aoW, aoH);

		// Blur down: target 1 -> target 0.
		dev->SetRenderTarget(0, surf0);
		dev->SetPixelShaderConstantF(1, stepDown, 1);
		dev->SetTexture(0, s_aoTarget[1]->Peek_D3D_Texture());
		drawScreenQuad(dev, s_blurPS, aoW, aoH, 0.0f, 0.0f, aoW, aoH);

		s_aoReadyThisFrame = TRUE;
	}
	if (surf1 != NULL)
		surf1->Release();
	if (surf0 != NULL)
		surf0->Release();

	// Back to the frame. SetRenderTarget resets the viewport to the whole target — what the full-target composite needs.
	dev->SetTexture(1, NULL);
	dev->SetRenderTarget(0, oldRT);

	// Ronin @feature 13/09/2026 DX9: SSAO step 3b. MULTIPLY the blurred AO into the frame (dest = dest * ao), upsampled
	// bilinearly from half resolution. RGB only — destination alpha belongs to the frame.
	D3DSURFACE_DESC rd;
	if (APPLY_TO_FRAME && s_aoReadyThisFrame && s_showPS != NULL && SUCCEEDED(oldRT->GetDesc(&rd)))
	{
		dev->SetTexture(0, s_aoTarget[0]->Peek_D3D_Texture());
		setSampler(dev, 0, TRUE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_ZERO);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
		dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
							D3DCOLORWRITEENABLE_BLUE);
		drawScreenQuad(dev, s_showPS, (float)rd.Width, (float)rd.Height, 0.0f, 0.0f, (float)rd.Width, (float)rd.Height);
	}

	// Then the 3D viewport, our depth buffer, the states, and let the wrapper re-assert itself.
	dev->SetTexture(0, NULL);
	oldRT->Release();
	if (haveVP)
		dev->SetViewport(&oldVP);
	DX8Wrapper::Resume_Scene_Depth();
	saved.restore(dev);

	dev->SetVertexShader(NULL);
	dev->SetPixelShader(NULL);
	DX8Wrapper::Invalidate_Cached_Render_States();
}

void W3DSsao::endFrame(void)
{
	DX8Wrapper::End_Scene_Depth();
}

Bool W3DSsao::isSupported(void)
{
	return DX8Wrapper::Is_Scene_Depth_Supported() ? TRUE : FALSE;
}

Bool W3DSsao::isActive(void)
{
	return s_activeThisFrame;
}

// Ronin @feature 13/09/2026 DX9: SSAO. Top-right corner at the screen's aspect. 1 = depth (grey = distance), 2 = raw AO
// recomputed here, 3 = the blurred AO renderPass produced. White = open, black = occluded; RED = outside the 3D viewport.
void W3DSsao::drawDebugView(const CameraClass *camera)
{
	if (DEBUG_VIEW == 0 || !s_activeThisFrame || camera == NULL)
		return;

	IDirect3DDevice9 *dev = DX8Wrapper::_Get_D3D_Device8();
	IDirect3DTexture9 *depthTex = DX8Wrapper::Peek_Scene_Depth_Texture();
	if (dev == NULL || depthTex == NULL)
		return;
	ensureShaders(dev);

	D3DSURFACE_DESC dd;
	if (FAILED(depthTex->GetLevelDesc(0, &dd)) || dd.Width == 0 || dd.Height == 0)
		return;

	IDirect3DPixelShader9 *ps  = NULL;
	IDirect3DTexture9     *tex = NULL;
	Bool                   linear = FALSE;
	if (DEBUG_VIEW == 1)
	{
		ps  = s_depthDebugPS;
		tex = depthTex;
	}
	else if (DEBUG_VIEW == 2)
	{
		ps  = s_aoRawPS;
		tex = depthTex;
	}
	else
	{
		ps     = s_showPS;
		tex    = (s_aoReadyThisFrame && s_aoTarget[0] != NULL) ? s_aoTarget[0]->Peek_D3D_Texture() : NULL;
		linear = TRUE;
	}
	if (s_quadVS == NULL || ps == NULL || tex == NULL)
		return;

	// First, so nothing the wrapper re-applies can land on top of the constants below.
	DX8Wrapper::Invalidate_Cached_Render_States();

	if (DEBUG_VIEW == 1)
	{
		float zNear = 1.0f;
		float zFar  = 1000.0f;
		camera->Get_Clip_Planes(zNear, zFar);
		const float c0[4] = { zNear, zFar, 1500.0f, 0.0f };
		dev->SetPixelShaderConstantF(0, c0, 1);
	}
	else if (DEBUG_VIEW == 2)
	{
		uploadAOConstants(dev, *camera, dd.Width, dd.Height);
	}

	dev->SetTexture(0, tex);
	setSampler(dev, 0, linear);
	setPassStates(dev);

	const float H = (float)dd.Height * 0.35f;
	const float W = H * (float)dd.Width / (float)dd.Height;
	drawScreenQuad(dev, ps, (float)dd.Width, (float)dd.Height, (float)dd.Width - W - 8.0f, 60.0f, W, H);

	// Unbind before anything else can touch these textures; End_Render re-invalidates the cache after this.
	dev->SetTexture(0, NULL);
	dev->SetVertexShader(NULL);
	dev->SetPixelShader(NULL);
	DX8Wrapper::Invalidate_Cached_Render_States();
}

void W3DSsao::shutdown(void)
{
	REF_PTR_RELEASE(s_aoTarget[0]);
	REF_PTR_RELEASE(s_aoTarget[1]);
	s_aoWidth  = 0;
	s_aoHeight = 0;

	IDirect3DPixelShader9 **pixelShaders[] = { &s_showPS, &s_blurPS, &s_aoRawPS, &s_depthDebugPS };
	for (Int i = 0; i < 4; ++i)
	{
		if (*pixelShaders[i] != NULL)
		{
			(*pixelShaders[i])->Release();
			*pixelShaders[i] = NULL;
		}
	}
	if (s_quadVS != NULL)
	{
		s_quadVS->Release();
		s_quadVS = NULL;
	}
	s_shadersTried = FALSE;
}
