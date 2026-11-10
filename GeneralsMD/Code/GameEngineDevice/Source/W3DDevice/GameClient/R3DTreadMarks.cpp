/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 port: tread relief - the tread and tyre marks shaded as grooves pressed into the ground.
*/

// Ronin @feature 09/11/2026 DX9: tread marks with depth (docs/TreadMarks_Work.md). See R3DTreadMarks.h.
#include "WW3D2/dx8todx9.h"
#include "WWLib/always.h"
#include "Lib/BaseType.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/dx8fvf.h"
#include "WW3D2/dx8vertexbuffer.h"
#include "WW3D2/dx8indexbuffer.h"
#include "WW3D2/texture.h"
#include "WW3D2/assetmgr.h"
#include "Common/GlobalData.h"
#include "W3DDevice/GameClient/R3DShadowMapState.h"	// Ronin @feature 10/11/2026 DX9: the sun's shadow map
#include "W3DDevice/GameClient/W3DShaderManager.h"	// Ronin @feature 10/11/2026 DX9: the ground's cloud drift
#include "W3DDevice/GameClient/R3DTreadMarks.h"
#include <d3dx9.h>
#include <cstring>

static IDirect3DVertexShader9	*s_vs = NULL;
static IDirect3DPixelShader9	*s_ps = NULL;
static Bool						s_triedShaders = FALSE;
static Bool						s_open     = FALSE;
static const float				RELIEF_STRENGTH = 1.0f;		// how strongly the sun shades the grooves (0..3)
static const float				RELIEF_GROOVE   = 0.25f;	// how much darker the rut's floor is (0..1)

// The relief map of each track texture, by name (four in the game); NULL = it has none.
enum { RELIEF_CACHE = 8 };
struct ReliefEntry { char name[64]; TextureClass *relief; };
static ReliefEntry	s_reliefs[RELIEF_CACHE];
static Int			s_reliefCount = 0;

// The device states this pass changes, and what they were.
static const D3DRENDERSTATETYPE RELIEF_STATES[] = {
	D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE,
	D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_CULLMODE, D3DRS_ALPHATESTENABLE, D3DRS_FOGENABLE,
	D3DRS_COLORWRITEENABLE };
enum { RELIEF_STATE_COUNT = sizeof(RELIEF_STATES) / sizeof(RELIEF_STATES[0]) };
static DWORD s_savedStates[RELIEF_STATE_COUNT];
static const D3DSAMPLERSTATETYPE RELIEF_SAMPLER_STATES[] = {
	D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER };
enum { RELIEF_SAMPLER_COUNT = sizeof(RELIEF_SAMPLER_STATES) / sizeof(RELIEF_SAMPLER_STATES[0]) };
static DWORD s_savedSamplers[4][RELIEF_SAMPLER_COUNT];	// s0 the relief map; s1, s2 the sun's map; s3 the clouds (Ronin @feature 10/11/2026 DX9)
static TextureClass *s_cloudTex = NULL;					// Ronin @feature 10/11/2026 DX9: the ground's cloud shadow texture (TerrainTex.cpp)

static DWORD *readShaderBlob(const char *path)
{
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	const DWORD size = GetFileSize(file, NULL);
	DWORD *blob = (size != 0 && size != INVALID_FILE_SIZE && size >= sizeof(DWORD))
				  ? (DWORD *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size) : NULL;
	DWORD read = 0;
	if (blob != NULL && (!ReadFile(file, blob, size, &read, NULL) || read != size))
	{
		HeapFree(GetProcessHeap(), 0, blob);
		blob = NULL;
	}
	CloseHandle(file);
	return blob;
}

static void ensureShaders(IDirect3DDevice9 *dev)
{
	if (s_triedShaders)
		return;
	s_triedShaders = TRUE;
	DWORD *blob = readShaderBlob("shaders\\TreadRelief.vso");
	if (blob != NULL)
	{
		if (FAILED(dev->CreateVertexShader(blob, &s_vs)))
			s_vs = NULL;
		HeapFree(GetProcessHeap(), 0, blob);
	}
	blob = readShaderBlob("shaders\\TreadRelief.pso");
	if (blob != NULL)
	{
		if (FAILED(dev->CreatePixelShader(blob, &s_ps)))
			s_ps = NULL;
		HeapFree(GetProcessHeap(), 0, blob);
	}
}

// "EXTnkTrack.tga" -> its relief map "EXTnkTrack_nrm.tga" (Art/Textures), loaded once; NULL when there is none.
// Ronin @feature 10/11/2026 DX9: the 2003 cloud shadows' texture, as the sun shafts take it (R3DLocalLights); NULL when missing.
static TextureClass *cloudTexture(void)
{
	if (s_cloudTex == NULL)
		s_cloudTex = WW3DAssetManager::Get_Instance()->Get_Texture("TSCloudMed.tga");
	if (s_cloudTex != NULL && !s_cloudTex->Is_Initialized())
		s_cloudTex->Init();
	return (s_cloudTex != NULL && !s_cloudTex->Is_Missing_Texture() && s_cloudTex->Peek_D3D_Texture() != NULL) ? s_cloudTex : NULL;
}

static TextureClass *reliefFor(TextureClass *track)
{
	if (track == NULL)
		return NULL;
	const char *name = track->Get_Texture_Name().str();
	for (Int i = 0; i < s_reliefCount; ++i)
		if (_stricmp(s_reliefs[i].name, name) == 0)
			return s_reliefs[i].relief;
	if (s_reliefCount >= RELIEF_CACHE || strlen(name) >= sizeof(s_reliefs[0].name) - 8)
		return NULL;
	ReliefEntry &e = s_reliefs[s_reliefCount++];
	strcpy(e.name, name);
	e.relief = NULL;
	char file[80];
	strcpy(file, name);
	char *dot = strrchr(file, '.');
	if (dot != NULL)
		*dot = 0;
	strcat(file, "_nrm.tga");
	WW3DAssetManager *assets = WW3DAssetManager::Get_Instance();
	TextureClass *tex = (assets != NULL) ? assets->Get_Texture(file) : NULL;
	if (tex != NULL)
	{
		tex->Init();
		if (tex->Is_Missing_Texture())
			REF_PTR_RELEASE(tex);
	}
	e.relief = tex;
	return tex;
}

Bool R3DTreadMarks::beginRelief(DX8VertexBufferClass *vb, DX8IndexBufferClass *ib, const Matrix3D &world)
{
	IDirect3DDevice9 *dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev == NULL || vb == NULL || ib == NULL || TheGlobalData == NULL)
		return FALSE;
	ensureShaders(dev);
	if (s_vs == NULL || s_ps == NULL || vb->Get_DX8_Vertex_Buffer() == NULL || ib->Get_DX8_Index_Buffer() == NULL)
		return FALSE;

	// The camera the terrain was just drawn with (TAA's jitter in it) is on the device once the wrapper's pending changes are.
	DX8Wrapper::Apply_Render_State_Changes();
	D3DMATRIX view, proj;
	dev->GetTransform(D3DTS_VIEW, &view);
	dev->GetTransform(D3DTS_PROJECTION, &proj);
	const D3DMATRIX w = To_D3DMATRIX(Matrix4x4(world));
	D3DXMATRIX wvp, wvpT;
	D3DXMatrixMultiply(&wvp, (const D3DXMATRIX *)&w, (const D3DXMATRIX *)&view);
	D3DXMatrixMultiply(&wvp, &wvp, (const D3DXMATRIX *)&proj);
	D3DXMatrixTranspose(&wvpT, &wvp);

	for (Int i = 0; i < RELIEF_STATE_COUNT; ++i)
		dev->GetRenderState(RELIEF_STATES[i], &s_savedStates[i]);
	for (DWORD st = 0; st < 4; ++st)
		for (Int i = 0; i < RELIEF_SAMPLER_COUNT; ++i)
			dev->GetSamplerState(st, RELIEF_SAMPLER_STATES[i], &s_savedSamplers[st][i]);

	DX8Wrapper::BindLayoutFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1, "R3DTreadMarks::relief");
	dev->SetVertexShader(s_vs);		// AFTER the layout bind - BindLayoutFVF clears the vertex shader
	dev->SetPixelShader(s_ps);
	dev->SetStreamSource(0, vb->Get_DX8_Vertex_Buffer(), 0, sizeof(VertexFormatXYZDUV1));
	dev->SetIndices(ib->Get_DX8_Index_Buffer());
	dev->SetVertexShaderConstantF(240, (const float *)&wvpT, 4);

	// The sun as the terrain shader takes it (HeightMap.cpp: lightPos is the direction the light TRAVELS).
	const Coord3D &lp = TheGlobalData->m_terrainLightPos[0];
	float sun[4] = { -lp.x, -lp.y, -lp.z, 0.0f };
	const float len = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
	if (len > 1.0e-6f)
		for (Int k = 0; k < 3; ++k)
			sun[k] /= len;
	const RGBColor &dif = TheGlobalData->m_terrainDiffuse[0];
	const RGBColor &amb = TheGlobalData->m_terrainAmbient[0];
	const float sunCol[4] = { dif.red, dif.green, dif.blue, RELIEF_STRENGTH };
	const float ambCol[4] = { amb.red, amb.green, amb.blue, RELIEF_GROOVE };
	dev->SetPixelShaderConstantF(200, sun, 1);
	dev->SetPixelShaderConstantF(201, sunCol, 1);
	dev->SetPixelShaderConstantF(202, ambCol, 1);

	// Ground x 2 x output: 0.5 leaves it as it was. Depth tested as the decal, never written; colour only, not alpha.
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
	dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
	dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
	dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
	dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_FOGENABLE, FALSE);		// fog would tint the 0.5 that means "unchanged"
	dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

	// Ronin @feature 10/11/2026 DX9: the sun's shadow map (s1 near, s2 far) as the terrain's receiver gets it: in a shadow the
	// grooves show no sunlit side (TreadRelief_ps sunVisible). No map this frame (shadow maps off) = lit, as before.
	const TerrainShadowPassState &sp = TheTerrainShadowPass;
	const Bool sunMap  = (sp.active && sp.shadowTex != NULL) ? TRUE : FALSE;
	const Bool twoMaps = (sunMap && sp.cascadeCount > 1.5f && sp.shadowTexFar != NULL) ? TRUE : FALSE;
	if (sunMap)
	{
		dev->SetPixelShaderConstantF(204, sp.lightViewProjT, 4);
		dev->SetPixelShaderConstantF(208, twoMaps ? sp.lightViewProjFarT : sp.lightViewProjT, 4);
	}
	const float mapPrm[4]  = { sunMap ? 1.0f : 0.0f, sp.depthBias, sp.texelOffset, twoMaps ? 2.0f : 1.0f };
	const float mapPrm2[4] = { twoMaps ? sp.depthBiasFar : sp.depthBias, sp.texelWorldSize,
							   twoMaps ? sp.texelWorldSizeFar : sp.texelWorldSize, 0.0f };
	dev->SetPixelShaderConstantF(212, mapPrm, 1);
	dev->SetPixelShaderConstantF(213, mapPrm2, 1);
	dev->SetTexture(1, sunMap ? sp.shadowTex : NULL);
	dev->SetTexture(2, sunMap ? (twoMaps ? sp.shadowTexFar : sp.shadowTex) : NULL);
	for (DWORD st = 1; st <= 2; ++st)
	{
		dev->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);		// a depth texture: the hardware's compare
		dev->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		dev->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		dev->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	}
	// Ronin @feature 10/11/2026 DX9: the 2003 cloud shadows (s3), mapped as the ground's (W3DShaderManager::getCloudMapState).
	TextureClass *clouds = cloudTexture();
	const Bool cloudsOn = (clouds != NULL && TheGlobalData->m_useCloudMap) ? TRUE : FALSE;
	float cloudScale = 0.0f, cloudX = 0.0f, cloudY = 0.0f;
	W3DShaderManager::getCloudMapState(&cloudScale, &cloudX, &cloudY);
	const float cloudPrm[4] = { cloudsOn ? 1.0f : 0.0f, cloudScale, cloudX, cloudY };
	dev->SetPixelShaderConstantF(214, cloudPrm, 1);
	dev->SetTexture(3, cloudsOn ? clouds->Peek_D3D_Texture() : NULL);
	dev->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
	dev->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
	dev->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
	s_open = TRUE;
	return TRUE;
}

void R3DTreadMarks::drawRelief(TextureClass *trackTexture, Int baseVertex, Int edgeCount)
{
	if (!s_open || edgeCount < 2)
		return;
	TextureClass *relief = reliefFor(trackTexture);
	if (relief == NULL)
		return;
	if (!relief->Is_Initialized())
		relief->Init();		// a device reset invalidates the asset manager's textures; Init loads it again
	IDirect3DTexture9 *tex = relief->Peek_D3D_Texture();
	if (tex == NULL)
		return;
	IDirect3DDevice9 *dev = DX8Wrapper::_Get_D3D_Device8();
	dev->SetTexture(0, tex);
	dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, baseVertex, 0, edgeCount * 2, 0, (edgeCount - 1) * 2);
}

void R3DTreadMarks::endRelief(void)
{
	if (!s_open)
		return;
	s_open = FALSE;
	IDirect3DDevice9 *dev = DX8Wrapper::_Get_D3D_Device8();
	dev->SetTexture(0, NULL);
	dev->SetTexture(1, NULL);
	dev->SetTexture(2, NULL);
	dev->SetTexture(3, NULL);
	dev->SetVertexShader(NULL);
	dev->SetPixelShader(NULL);
	for (Int i = 0; i < RELIEF_STATE_COUNT; ++i)
		dev->SetRenderState(RELIEF_STATES[i], s_savedStates[i]);
	for (DWORD st = 0; st < 4; ++st)
		for (Int i = 0; i < RELIEF_SAMPLER_COUNT; ++i)
			dev->SetSamplerState(st, RELIEF_SAMPLER_STATES[i], s_savedSamplers[st][i]);
	// The wrapper's caches no longer match the device; the decal's own Set_* calls follow. The camera stays: it was applied above.
	DX8Wrapper::Invalidate_Cached_Render_States();
}

void R3DTreadMarks::shutdown(void)
{
	for (Int i = 0; i < s_reliefCount; ++i)
		REF_PTR_RELEASE(s_reliefs[i].relief);
	s_reliefCount = 0;
	REF_PTR_RELEASE(s_cloudTex);	// Ronin @feature 10/11/2026 DX9
	if (s_vs != NULL)
	{
		s_vs->Release();
		s_vs = NULL;
	}
	if (s_ps != NULL)
	{
		s_ps->Release();
		s_ps = NULL;
	}
	s_triedShaders = FALSE;
}

// ---- Ronin @feature 09/11/2026 DX9: rounded tread marks (see the header) ----
static const float	ROUND_STEP_DEGREES = 6.0f;		// one inserted edge per this much of a segment's turn
static const float	ROUND_MAX_TURN     = 120.0f;	// sharper (a reversal) stays a corner: a curve there would loop
enum { ROUND_MAX_INSERTS = 8, ROUND_MAX_STORED = 256 };	// stored: MAX_TRACK_EDGE_COUNT (200) fits

// The turn at centre i in degrees; 0 at the ends and next to a gap (a break in the track: a jump, a cut).
static float roundTurn(const Vector3 *c, Int n, Int i, float maxGap)
{
	if (i <= 0 || i >= n - 1)
		return 0.0f;
	const Vector3 a = c[i] - c[i - 1];
	const Vector3 b = c[i + 1] - c[i];
	const float la = a.Length();
	const float lb = b.Length();
	if (la < 1.0e-3f || lb < 1.0e-3f || la > maxGap || lb > maxGap)
		return 0.0f;
	float cs = Vector3::Dot_Product(a, b) / (la * lb);
	if (cs >= 0.99999f)
		return 0.0f;
	if (cs < -1.0f)
		cs = -1.0f;
	return acosf(cs) * (180.0f / 3.14159265f);
}

// Centripetal Catmull-Rom (Barry-Goldman) between p1 and p2, u in 0..1 - as the projectile trails (streak.cpp).
static Vector3 roundCatmullRom(const Vector3 &p0, const Vector3 &p1, const Vector3 &p2, const Vector3 &p3, float u)
{
	const float d01 = sqrtf((p1 - p0).Length());
	const float d12 = sqrtf((p2 - p1).Length());
	const float d23 = sqrtf((p3 - p2).Length());
	const float t1 = (d01 > 1.0e-4f) ? d01 : 1.0e-4f;
	const float t2 = t1 + ((d12 > 1.0e-4f) ? d12 : 1.0e-4f);
	const float t3 = t2 + ((d23 > 1.0e-4f) ? d23 : 1.0e-4f);
	const float t = t1 + (t2 - t1) * u;
	const Vector3 a1 = p0 * ((t1 - t) / t1) + p1 * (t / t1);
	const Vector3 a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
	const Vector3 a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
	const Vector3 b1 = a1 * ((t2 - t) / t2) + a2 * (t / t2);
	const Vector3 b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
	return b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1));
}

// One edge as TerrainTracksRenderObjClassSystem::flush wrote it in 2003: left u 0, right u 1, the fade in the alpha.
static VertexFormatXYZDUV1 *roundPutEdge(VertexFormatXYZDUV1 *out, const Vector3 &l, const Vector3 &r, float v, float fade,
	UnsignedInt diffuseRGB)
{
	const UnsignedInt col = diffuseRGB | ((UnsignedInt)REAL_TO_INT(fade * 255.0f) << 24);
	out->x = l.X; out->y = l.Y; out->z = l.Z; out->u1 = 0.0f; out->v1 = v; out->diffuse = col; ++out;
	out->x = r.X; out->y = r.Y; out->z = r.Z; out->u1 = 1.0f; out->v1 = v; out->diffuse = col; ++out;
	return out;
}

// Ronin @bugfix 09/11/2026 DX9: on a turn tighter than half the track's width the inner side runs backwards, and its pieces
// folded over each other (a fan of thin lines). A side that would turn a triangle over holds still: the strip pivots on it.
static void roundHoldSide(const Vector3 &pl, const Vector3 &pr, Vector3 &l, Vector3 &r)
{
	// the strip's triangles (pl, pr, r) and (pl, r, l) wind anticlockwise seen from above while the track runs forward
	if ((pr.X - pl.X) * (r.Y - pl.Y) - (pr.Y - pl.Y) * (r.X - pl.X) < 0.0f)
		r = pr;
	if ((r.X - pl.X) * (l.Y - pl.Y) - (r.Y - pl.Y) * (l.X - pl.X) < 0.0f)
		l = pl;
}

Int R3DTreadMarks::writeRoundedStrip(const Vector3 *left, const Vector3 *right, const float *v, const float *fade, Int count,
	float spacing, UnsignedInt diffuseRGB, VertexFormatXYZDUV1 *out, Int maxEdges)
{
	const float maxGap = 2.5f * spacing;
	Int inserts[ROUND_MAX_STORED];
	Bool smooth[ROUND_MAX_STORED];	// segment i has no gap and no reversal: its sides may hold (roundHoldSide)
	Int total = count;
	if (count >= 3 && count <= ROUND_MAX_STORED)
	{
		Vector3 centre[ROUND_MAX_STORED];
		float turn[ROUND_MAX_STORED];
		for (Int i = 0; i < count; ++i)
			centre[i] = (left[i] + right[i]) * 0.5f;
		for (Int i = 0; i < count; ++i)
			turn[i] = roundTurn(centre, count, i, maxGap);
		for (Int i = 0; i + 1 < count; ++i)
		{
			Int k = 0;
			const float len = (centre[i + 1] - centre[i]).Length();
			smooth[i] = (len <= maxGap && turn[i] <= ROUND_MAX_TURN && turn[i + 1] <= ROUND_MAX_TURN) ? TRUE : FALSE;
			if (smooth[i] && fade[i] > 0.0f && fade[i + 1] > 0.0f)
				k = (Int)(0.5f * (turn[i] + turn[i + 1]) / ROUND_STEP_DEGREES);	// the segment turns half of each end's angle
			inserts[i] = (k > ROUND_MAX_INSERTS) ? ROUND_MAX_INSERTS : k;
			total += inserts[i];
		}
		if (total > maxEdges)
			total = count;		// no room this frame: as stored
	}
	const Bool rounded = (total > count) ? TRUE : FALSE;

	Vector3 pl, pr;		// the edge written last
	for (Int i = 0; i < count; ++i)
	{
		Vector3 l = left[i];
		Vector3 r = right[i];
		if (rounded && i > 0 && smooth[i - 1])
			roundHoldSide(pl, pr, l, r);
		out = roundPutEdge(out, l, r, v[i], fade[i], diffuseRGB);
		pl = l;
		pr = r;
		if (!rounded || i + 1 >= count || inserts[i] == 0)
			continue;
		// Neighbours past a gap or the ends are mirrored, so the curve never reaches across a break.
		const Bool prevOK = (i > 0) && (((left[i] + right[i]) - (left[i - 1] + right[i - 1])).Length() * 0.5f <= maxGap);
		const Bool nextOK = (i + 2 < count) && (((left[i + 2] + right[i + 2]) - (left[i + 1] + right[i + 1])).Length() * 0.5f <= maxGap);
		const Vector3 l0 = prevOK ? left[i - 1]  : (left[i] * 2.0f - left[i + 1]);
		const Vector3 r0 = prevOK ? right[i - 1] : (right[i] * 2.0f - right[i + 1]);
		const Vector3 l3 = nextOK ? left[i + 2]  : (left[i + 1] * 2.0f - left[i]);
		const Vector3 r3 = nextOK ? right[i + 2] : (right[i + 1] * 2.0f - right[i]);
		for (Int k = 1; k <= inserts[i]; ++k)
		{
			const float u = (float)k / (float)(inserts[i] + 1);
			Vector3 cl = roundCatmullRom(l0, left[i], left[i + 1], l3, u);
			Vector3 cr = roundCatmullRom(r0, right[i], right[i + 1], r3, u);
			roundHoldSide(pl, pr, cl, cr);
			out = roundPutEdge(out, cl, cr, v[i] + (v[i + 1] - v[i]) * u, fade[i] + (fade[i + 1] - fade[i]) * u, diffuseRGB);
			pl = cl;
			pr = cr;
		}
	}
	return total;
}
