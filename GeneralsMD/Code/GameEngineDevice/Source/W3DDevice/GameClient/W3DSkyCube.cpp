/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 port: the sky cube - a cube map of a map's sky and ground, made when the map loads.
*/

// Ronin @feature 12/10/2026 DX9: LIGHTING PART 4, STEP 1 (docs/Lighting_Work.md 4.4b). See W3DSkyCube.h.
#include "WW3D2/dx8todx9.h"
#include "WWLib/always.h"
#include "Lib/BaseType.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/texture.h"
#include "WW3D2/assetmgr.h"
#include "Common/GlobalData.h"
#include "Common/GameType.h"
#include "GameClient/Water.h"
#include "GameClient/TerrainVisual.h"
#include "GameLogic/TerrainLogic.h"
#include "W3DDevice/GameClient/W3DSkyCube.h"
#include <d3dx9.h>
#include <cmath>
#include <cstdio>
#include <cstring>

static const Int	SKY_SIZE     = 256;		// a face of the cube, in texels
static const Int	SKY_SAMPLES  = 24;		// the ambient's six colours: this many squared a face
static const float	SKY_HORIZON  = 0.03f;	// the sky gives way to the ground over this much of the direction's z, either side of level

struct SkyFace
{
	Int    w, h;
	DWORD *px;		// A8R8G8B8, rows from the top
};

static IDirect3DCubeTexture9 *s_cube     = NULL;
static float                  s_six[6][3] = { { 0 } };
static float                  s_ground[3] = { 0.0f, 0.0f, 0.0f };	// 0..255, as the faces are
static char                   s_key[160]  = "";						// what the cube was built for: the map, its time of day, its sky
static char                   s_set[48]   = "";						// the north picture's name
static Bool                   s_byTime    = FALSE;					// the map names no sky: picked by its time of day
static Bool                   s_wanted    = FALSE;					// `sky rebuild`
// Ronin @feature 15/10/2026 DX9: global light 3 - the lit terrain colour map (buildGroundMap).
static const Int              GROUND_MAP  = 128;
static IDirect3DTexture9     *s_groundMap = NULL;
static float                  s_groundUV[4]  = { 0.0f, 0.0f, 0.0f, 0.0f };
static float                  s_groundAmb[3] = { 0.0f, 0.0f, 0.0f };

// Ronin @feature 12/10/2026 DX9: one sky picture as plain pixels. The archives hold them as DXT1; the device unpacks.
static Bool readFace(IDirect3DDevice9 *dev, const char *name, SkyFace &out)
{
	out.w = out.h = 0;
	out.px = NULL;
	TextureClass *tex = WW3DAssetManager::Get_Instance()->Get_Texture(name);
	if (tex == NULL)
		return FALSE;
	if (!tex->Is_Initialized())
		tex->Init();
	IDirect3DTexture9 *d3d = tex->Is_Missing_Texture() ? NULL : tex->Peek_D3D_Texture();
	IDirect3DSurface9 *src = NULL, *plain = NULL;
	D3DSURFACE_DESC desc;
	D3DLOCKED_RECT lock;
	if (d3d != NULL && SUCCEEDED(d3d->GetSurfaceLevel(0, &src)) && SUCCEEDED(src->GetDesc(&desc)) &&
		SUCCEEDED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &plain, NULL)) &&
		SUCCEEDED(D3DXLoadSurfaceFromSurface(plain, NULL, NULL, src, NULL, NULL, D3DX_FILTER_NONE, 0)) &&
		SUCCEEDED(plain->LockRect(&lock, NULL, D3DLOCK_READONLY)))
	{
		out.w  = (Int)desc.Width;
		out.h  = (Int)desc.Height;
		out.px = new DWORD[desc.Width * desc.Height];
		for (UINT y = 0; y < desc.Height; ++y)
			memcpy(out.px + y * desc.Width, (const char *)lock.pBits + y * lock.Pitch, desc.Width * sizeof(DWORD));
		plain->UnlockRect();
	}
	if (plain != NULL)
		plain->Release();
	if (src != NULL)
		src->Release();
	REF_PTR_RELEASE(tex);
	return (out.px != NULL) ? TRUE : FALSE;
}

static void texel(const SkyFace &f, float u, float v, float out[3])
{
	float x = u * (float)f.w - 0.5f, y = v * (float)f.h - 0.5f;
	x = (x < 0.0f) ? 0.0f : ((x > (float)(f.w - 1)) ? (float)(f.w - 1) : x);
	y = (y < 0.0f) ? 0.0f : ((y > (float)(f.h - 1)) ? (float)(f.h - 1) : y);
	const Int x0 = (Int)x, y0 = (Int)y;
	const Int x1 = (x0 + 1 < f.w) ? x0 + 1 : x0, y1 = (y0 + 1 < f.h) ? y0 + 1 : y0;
	const float fx = x - (float)x0, fy = y - (float)y0;
	const DWORD a = f.px[y0 * f.w + x0], b = f.px[y0 * f.w + x1], c = f.px[y1 * f.w + x0], d = f.px[y1 * f.w + x1];
	for (Int k = 0; k < 3; ++k)
	{
		const Int shift = 16 - 8 * k;		// red, green, blue
		const float top = (float)((a >> shift) & 255) * (1.0f - fx) + (float)((b >> shift) & 255) * fx;
		const float bot = (float)((c >> shift) & 255) * (1.0f - fx) + (float)((d >> shift) & 255) * fx;
		out[k] = top * (1.0f - fy) + bot * fy;
	}
}

// Ronin @feature 12/10/2026 DX9: a world direction looked up as the model new_skybox.w3d lays the five pictures out (read from its
// corners, scratchpad skybox_survey.py): N on x = -750, S on x = +750, E on y = -750, W on y = +750, T on z = +750; none below.
static void lookSky(const SkyFace faces[5], const float d[3], float out[3])
{
	const float ax = fabsf(d[0]), ay = fabsf(d[1]), az = fabsf(d[2]);
	const float m = (ax > ay) ? ((ax > az) ? ax : az) : ((ay > az) ? ay : az);
	const float x = d[0] / m, y = d[1] / m, z = d[2] / m;		// on the box, -1..1
	const float v = (1.0f - z) * 0.5f;
	if (az >= ax && az >= ay)
		texel(faces[4], (1.0f - x) * 0.5f, (y + 1.0f) * 0.5f, out);		// the top (asked for only above the horizon)
	else if (ax >= ay)
	{
		if (d[0] < 0.0f)
			texel(faces[0], (1.0f - y) * 0.5f, v, out);		// N
		else
			texel(faces[2], (y + 1.0f) * 0.5f, v, out);		// S
	}
	else if (d[1] < 0.0f)
		texel(faces[1], (x + 1.0f) * 0.5f, v, out);			// E
	else
		texel(faces[3], (1.0f - x) * 0.5f, v, out);			// W
}

// Ronin @feature 12/10/2026 DX9: sky above the horizon, the map's lit ground below it. The pictures hold no ground: below the
// horizon they MIRROR the sky, and an ambient taken from that is the same colour up and down (measured: 1.00 to 1).
static void lookWorld(const SkyFace faces[5], const float d[3], float out[3])
{
	const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	const float n[3] = { d[0] / len, d[1] / len, d[2] / len };
	float sky = (n[2] + SKY_HORIZON) / (2.0f * SKY_HORIZON);
	sky = (sky < 0.0f) ? 0.0f : ((sky > 1.0f) ? 1.0f : sky);
	out[0] = s_ground[0]; out[1] = s_ground[1]; out[2] = s_ground[2];
	if (sky > 0.0f)
	{
		const float up[3] = { n[0], n[1], (n[2] > 0.0f) ? n[2] : 0.0f };		// at the horizon the picture's own horizon row
		float c[3];
		lookSky(faces, up, c);
		for (Int k = 0; k < 3; ++k)
			out[k] = s_ground[k] + (c[k] - s_ground[k]) * sky;
	}
}

// Ronin @feature 12/10/2026 DX9: the direction D3D looks a cube texel up by: face in D3DCUBEMAP_FACES order, s and t -1..1.
static void texelDirection(Int face, float s, float t, float d[3])
{
	switch (face)
	{
		case 0:  d[0] =  1.0f; d[1] = -t;    d[2] = -s;    break;
		case 1:  d[0] = -1.0f; d[1] = -t;    d[2] =  s;    break;
		case 2:  d[0] =  s;    d[1] =  1.0f; d[2] =  t;    break;
		case 3:  d[0] =  s;    d[1] = -1.0f; d[2] = -t;    break;
		case 4:  d[0] =  s;    d[1] = -t;    d[2] =  1.0f; break;
		default: d[0] = -s;    d[1] = -t;    d[2] = -1.0f; break;
	}
}

// Ronin @feature 12/10/2026 DX9: the ground's colour as the cube holds it: the terrain's own colour over a 16 x 16 grid of the map,
// lit as level ground is - the terrain's ambient plus what each of its lights gives a face that looks up.
static Bool groundColour(void)
{
	if (TheTerrainVisual == NULL || TheTerrainLogic == NULL || TheGlobalData == NULL)
		return FALSE;
	Region3D extent;
	TheTerrainLogic->getExtent(&extent);
	if (extent.hi.x - extent.lo.x < 1.0f || extent.hi.y - extent.lo.y < 1.0f)
		return FALSE;
	float albedo[3] = { 0.0f, 0.0f, 0.0f };
	const Int grid = 16;
	for (Int gy = 0; gy < grid; ++gy)
	{
		for (Int gx = 0; gx < grid; ++gx)
		{
			RGBColor c;
			c.red = c.green = c.blue = 0.0f;
			TheTerrainVisual->getTerrainColorAt(extent.lo.x + (extent.hi.x - extent.lo.x) * ((float)gx + 0.5f) / (float)grid,
												extent.lo.y + (extent.hi.y - extent.lo.y) * ((float)gy + 0.5f) / (float)grid, &c);
			albedo[0] += c.red; albedo[1] += c.green; albedo[2] += c.blue;
		}
	}
	float light[3] = { TheGlobalData->m_terrainAmbient[0].red, TheGlobalData->m_terrainAmbient[0].green, TheGlobalData->m_terrainAmbient[0].blue };
	for (Int i = 0; i < TheGlobalData->m_numGlobalLights; ++i)
	{
		const float down = -TheGlobalData->m_terrainLightPos[i].z;
		if (down > 0.0f)
		{
			light[0] += down * TheGlobalData->m_terrainDiffuse[i].red;
			light[1] += down * TheGlobalData->m_terrainDiffuse[i].green;
			light[2] += down * TheGlobalData->m_terrainDiffuse[i].blue;
		}
	}
	for (Int k = 0; k < 3; ++k)
	{
		const float v = albedo[k] / (float)(grid * grid) * light[k] * 255.0f;
		s_ground[k] = (v < 0.0f) ? 0.0f : ((v > 255.0f) ? 255.0f : v);
	}
	return TRUE;
}

// Ronin @feature 12/10/2026 DX9: which sky. The map's own if its map.ini names one (6 of the 170 shipped maps do); the other 164
// keep the default morning sky whatever their light, so those take one by their time of day (agreed 12/10).
static void pickSky(const WaterTransparencySetting *water, char names[5][48])
{
	static const char *const side = "NESWT";
	const AsciiString own[5] = { water->m_skyboxTextureN, water->m_skyboxTextureE, water->m_skyboxTextureS, water->m_skyboxTextureW,
								 water->m_skyboxTextureT };
	const char *set = NULL;
	s_byTime = (_stricmp(own[0].str(), "TSMorningN.tga") == 0) ? TRUE : FALSE;
	if (s_byTime)
	{
		switch (TheGlobalData->m_timeOfDay)
		{
			case TIME_OF_DAY_AFTERNOON: set = "TSMidday"; break;
			case TIME_OF_DAY_EVENING:   set = "TSDusky"; break;
			case TIME_OF_DAY_NIGHT:     set = "TSNightSky"; break;
			default: break;		// morning: the default itself
		}
	}
	for (Int f = 0; f < 5; ++f)
	{
		if (set != NULL)
			_snprintf(names[f], 48, "%s_%c.tga", set, side[f]);
		else
			strncpy(names[f], own[f].str(), 48);
		names[f][47] = 0;
	}
}

static void build(const WaterTransparencySetting *water)
{
	IDirect3DDevice9 *dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev == NULL)
		return;
	char names[5][48];
	pickSky(water, names);
	strncpy(s_set, names[0], sizeof(s_set));
	s_set[sizeof(s_set) - 1] = 0;
	SkyFace faces[5];
	Bool ok = TRUE;
	for (Int f = 0; f < 5; ++f)
		ok = (readFace(dev, names[f], faces[f]) && ok) ? TRUE : FALSE;
	if (ok && s_cube == NULL && FAILED(dev->CreateCubeTexture(SKY_SIZE, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_cube, NULL)))
		s_cube = NULL;
	if (ok && s_cube != NULL)
	{
		for (Int face = 0; face < 6; ++face)
		{
			D3DLOCKED_RECT lock;
			if (FAILED(s_cube->LockRect((D3DCUBEMAP_FACES)face, 0, &lock, NULL, 0)))
				continue;
			for (Int j = 0; j < SKY_SIZE; ++j)
			{
				DWORD *row = (DWORD *)((char *)lock.pBits + j * lock.Pitch);
				for (Int i = 0; i < SKY_SIZE; ++i)
				{
					float d[3], c[3];
					texelDirection(face, ((float)i + 0.5f) / (float)SKY_SIZE * 2.0f - 1.0f, ((float)j + 0.5f) / (float)SKY_SIZE * 2.0f - 1.0f, d);
					lookWorld(faces, d, c);
					row[i] = 0xFF000000u | ((DWORD)(c[0] + 0.5f) << 16) | ((DWORD)(c[1] + 0.5f) << 8) | (DWORD)(c[2] + 0.5f);
				}
			}
			s_cube->UnlockRect((D3DCUBEMAP_FACES)face, 0);
		}
		D3DXFilterTexture(s_cube, NULL, 0, D3DX_FILTER_BOX);		// its smaller levels, for the reflections to come
		// the ambient's six: over every direction, weighted by how squarely the axis faces it and by the texel's share of the sphere
		for (Int axis = 0; axis < 6; ++axis)
		{
			float a[3] = { 0.0f, 0.0f, 0.0f };
			a[axis / 2] = (axis & 1) ? -1.0f : 1.0f;
			float sum[3] = { 0.0f, 0.0f, 0.0f }, weight = 0.0f;
			for (Int face = 0; face < 6; ++face)
			{
				for (Int j = 0; j < SKY_SAMPLES; ++j)
				{
					for (Int i = 0; i < SKY_SAMPLES; ++i)
					{
						const float s = ((float)i + 0.5f) / (float)SKY_SAMPLES * 2.0f - 1.0f, t = ((float)j + 0.5f) / (float)SKY_SAMPLES * 2.0f - 1.0f;
						float d[3], c[3];
						texelDirection(face, s, t, d);
						const float len2 = 1.0f + s * s + t * t, len = sqrtf(len2);
						const float facing = (d[0] * a[0] + d[1] * a[1] + d[2] * a[2]) / len;
						if (facing <= 0.0f)
							continue;
						const float w = facing / (len2 * len);
						lookWorld(faces, d, c);
						sum[0] += c[0] * w; sum[1] += c[1] * w; sum[2] += c[2] * w;
						weight += w;
					}
				}
			}
			for (Int k = 0; k < 3; ++k)
				s_six[axis][k] = (weight > 0.0f) ? sum[k] / weight / 255.0f : 0.0f;
		}
	}
	else if (s_cube != NULL)
	{
		s_cube->Release();		// a picture is missing: no cube, and step 2 keeps the flat ambient
		s_cube = NULL;
	}
	for (Int f = 0; f < 5; ++f)
		delete [] faces[f].px;
}

// Ronin @feature 15/10/2026 DX9: GLOBAL LIGHT 3 - THE LIT TERRAIN COLOUR MAP. What the ground sends up, per texel of a 128 x 128 grid
// over the map: its colour x the light on level ground (as groundColour), blurred twice over 5 texels. Halved into 8 bits.
static void buildGroundMap(void)
{
	IDirect3DDevice9 *dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev == NULL || TheTerrainVisual == NULL || TheTerrainLogic == NULL || TheGlobalData == NULL)
		return;
	Region3D extent;
	TheTerrainLogic->getExtent(&extent);
	const float w = extent.hi.x - extent.lo.x, h = extent.hi.y - extent.lo.y;
	if (w < 1.0f || h < 1.0f)
		return;
	float light[3] = { TheGlobalData->m_terrainAmbient[0].red, TheGlobalData->m_terrainAmbient[0].green, TheGlobalData->m_terrainAmbient[0].blue };
	for (Int i = 0; i < TheGlobalData->m_numGlobalLights; ++i)
	{
		const float down = -TheGlobalData->m_terrainLightPos[i].z;
		if (down > 0.0f)
		{
			light[0] += down * TheGlobalData->m_terrainDiffuse[i].red;
			light[1] += down * TheGlobalData->m_terrainDiffuse[i].green;
			light[2] += down * TheGlobalData->m_terrainDiffuse[i].blue;
		}
	}
	static float a[GROUND_MAP * GROUND_MAP * 3];
	static float b[GROUND_MAP * GROUND_MAP * 3];
	for (Int y = 0; y < GROUND_MAP; ++y)
	{
		for (Int x = 0; x < GROUND_MAP; ++x)
		{
			RGBColor c;
			c.red = c.green = c.blue = 0.0f;
			TheTerrainVisual->getTerrainColorAt(extent.lo.x + w * ((float)x + 0.5f) / (float)GROUND_MAP,
												extent.lo.y + h * ((float)y + 0.5f) / (float)GROUND_MAP, &c);
			float *o = &a[(y * GROUND_MAP + x) * 3];
			o[0] = c.red * light[0];
			o[1] = c.green * light[1];
			o[2] = c.blue * light[2];
		}
	}
	// Twice across and down, 5 texels a time: what a wall sees is the ground round it, not the texel under it.
	for (Int pass = 0; pass < 4; ++pass)
	{
		const Bool across = ((pass & 1) == 0) ? TRUE : FALSE;
		float *src = (pass & 1) ? b : a;
		float *dst = (pass & 1) ? a : b;
		for (Int y = 0; y < GROUND_MAP; ++y)
		{
			for (Int x = 0; x < GROUND_MAP; ++x)
			{
				float s[3] = { 0.0f, 0.0f, 0.0f };
				for (Int t = -2; t <= 2; ++t)
				{
					Int sx = across ? (x + t) : x, sy = across ? y : (y + t);
					sx = (sx < 0) ? 0 : ((sx >= GROUND_MAP) ? GROUND_MAP - 1 : sx);
					sy = (sy < 0) ? 0 : ((sy >= GROUND_MAP) ? GROUND_MAP - 1 : sy);
					const float *p = &src[(sy * GROUND_MAP + sx) * 3];
					s[0] += p[0]; s[1] += p[1]; s[2] += p[2];
				}
				float *o = &dst[(y * GROUND_MAP + x) * 3];
				o[0] = s[0] * 0.2f; o[1] = s[1] * 0.2f; o[2] = s[2] * 0.2f;
			}
		}
	}
	if (s_groundMap == NULL && FAILED(dev->CreateTexture(GROUND_MAP, GROUND_MAP, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_groundMap, NULL)))
	{
		s_groundMap = NULL;
		return;
	}
	D3DLOCKED_RECT lock;
	if (FAILED(s_groundMap->LockRect(0, &lock, NULL, 0)))
		return;
	for (Int y = 0; y < GROUND_MAP; ++y)
	{
		DWORD *row = (DWORD *)((BYTE *)lock.pBits + y * lock.Pitch);
		for (Int x = 0; x < GROUND_MAP; ++x)
		{
			const float *p = &a[(y * GROUND_MAP + x) * 3];
			Int v[3];
			for (Int k = 0; k < 3; ++k)
			{
				const float f = p[k] * 0.5f * 255.0f + 0.5f;
				v[k] = (f < 0.0f) ? 0 : ((f > 255.0f) ? 255 : (Int)f);
			}
			row[x] = 0xFF000000u | ((DWORD)v[0] << 16) | ((DWORD)v[1] << 8) | (DWORD)v[2];
		}
	}
	s_groundMap->UnlockRect(0);
	s_groundUV[0] = extent.lo.x;
	s_groundUV[1] = extent.lo.y;
	s_groundUV[2] = 1.0f / w;
	s_groundUV[3] = 1.0f / h;
	s_groundAmb[0] = TheGlobalData->m_terrainAmbient[0].red;
	s_groundAmb[1] = TheGlobalData->m_terrainAmbient[0].green;
	s_groundAmb[2] = TheGlobalData->m_terrainAmbient[0].blue;
}

void W3DSkyCube::update(void)
{
	const WaterTransparencySetting *water = TheWaterTransparency;		// none before the INIs are read
	if (TheGlobalData == NULL || water == NULL)
		return;
	char key[160];
	_snprintf(key, sizeof(key), "%s|%d|%s", TheGlobalData->m_mapName.str(), (int)TheGlobalData->m_timeOfDay, water->m_skyboxTextureN.str());
	key[sizeof(key) - 1] = 0;
	if (!s_wanted && strcmp(key, s_key) == 0)
		return;
	if (!groundColour())
		return;			// no terrain yet: asked again next frame
	s_wanted = FALSE;
	strcpy(s_key, key);
	build(water);
	buildGroundMap();		// Ronin @feature 15/10/2026 DX9: global light 3, with the cube
}

void W3DSkyCube::rebuild(void)	{ s_wanted = TRUE; }

void W3DSkyCube::shutdown(void)
{
	if (s_cube != NULL)
		s_cube->Release();
	s_cube = NULL;
	if (s_groundMap != NULL)
		s_groundMap->Release();
	s_groundMap = NULL;
	s_key[0] = 0;
}

IDirect3DCubeTexture9 *W3DSkyCube::peek(void)	{ return s_cube; }

IDirect3DTexture9 *W3DSkyCube::groundMap(float uv[4], float ambient[3])
{
	for (Int k = 0; k < 4; ++k)
		uv[k] = s_groundUV[k];
	for (Int k = 0; k < 3; ++k)
		ambient[k] = s_groundAmb[k];
	return s_groundMap;
}

void W3DSkyCube::describe(char *out, Int size)
{
	if (s_cube == NULL)
		_snprintf(out, (size_t)size, "sky: no cube (%s)", (s_key[0] == 0) ? "not built yet" : "a sky picture is missing");
	else
		_snprintf(out, (size_t)size, "sky: %s (%s), %d a face; ground %.0f %.0f %.0f; ambient up %.2f %.2f %.2f  down %.2f %.2f %.2f  "
				  "+x %.2f %.2f %.2f  -x %.2f %.2f %.2f  +y %.2f %.2f %.2f  -y %.2f %.2f %.2f",
				  s_set, s_byTime ? "by the time of day" : "the map's own", SKY_SIZE, s_ground[0], s_ground[1], s_ground[2],
				  s_six[4][0], s_six[4][1], s_six[4][2], s_six[5][0], s_six[5][1], s_six[5][2], s_six[0][0], s_six[0][1], s_six[0][2],
				  s_six[1][0], s_six[1][1], s_six[1][2], s_six[2][0], s_six[2][1], s_six[2][2], s_six[3][0], s_six[3][1], s_six[3][2]);
	out[size - 1] = 0;
}
