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

// Ronin @feature 18/02/2026 DX9: Hardware instancing for rigid mesh batching

#include <d3d9.h>
#include <d3dx9math.h>
#include <algorithm>   // Ronin @perf §16 DX9: std::stable_sort over the single-rigid draw-order index array

#include "dx8instancing.h"
#include "dx8wrapper.h"
#include "dx8polygonrenderer.h"
#include "dx8fvf.h"
#include "dx8caps.h"

#include "Common/GlobalData.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "assetmgr.h"
#include "vertmaterial.h"
#include "ww3d.h"   // Ronin @diagnostic 21/06/2026: WW3D::Get_Frame_Count for the per-frame SR counter
#include "statistics.h" // Ronin @diagnostic 02/08/2026: Record_Instanced_Draw (this path bypasses DX8Wrapper)


// Global instance
DX8InstanceManagerClass TheDX8InstanceManager;

// Ronin @feature 23/05/2026 DX9 R2/R3: resolver lives in dx8renderer.cpp.
extern TextureClass* Get_Normal_Map_For_Diffuse_Texture(TextureClass* diffuseTexture);

// Ronin @feature 07/06/2026 DX9: Per-instance lighting payload. Mirrors the lightenv
// branch of Build_Rigid_Shader_Lighting_Constants so a collected instance carries the
// same ambient + up to 4 directional lights the single-rigid path would have uploaded.
void DX8InstanceManagerClass::Extract_Instance_Lighting(LightEnvironmentClass* lightEnv, InstanceData& inst)
{
	memset(inst.ambient, 0, sizeof(inst.ambient));
	memset(inst.lightDir0, 0, sizeof(inst.lightDir0));
	memset(inst.lightDiffuse0, 0, sizeof(inst.lightDiffuse0));
	memset(inst.lightDir1, 0, sizeof(inst.lightDir1));
	memset(inst.lightDiffuse1, 0, sizeof(inst.lightDiffuse1));
	memset(inst.lightDir2, 0, sizeof(inst.lightDir2));
	memset(inst.lightDiffuse2, 0, sizeof(inst.lightDiffuse2));
	memset(inst.lightDir3, 0, sizeof(inst.lightDir3));
	memset(inst.lightDiffuse3, 0, sizeof(inst.lightDiffuse3));

	if (lightEnv == nullptr) {
		return;
	}

	const Vector3& amb = lightEnv->Get_Equivalent_Ambient();
	inst.ambient[0] = amb.X;
	inst.ambient[1] = amb.Y;
	inst.ambient[2] = amb.Z;

	float* dirs[4] = { inst.lightDir0, inst.lightDir1, inst.lightDir2, inst.lightDir3 };
	float* difs[4] = { inst.lightDiffuse0, inst.lightDiffuse1, inst.lightDiffuse2, inst.lightDiffuse3 };

	const int count = lightEnv->Get_Light_Count();
	int n = 0;
	for (int i = 0; i < count && n < 4; ++i, ++n) {
		const Vector3& d = lightEnv->Get_Light_Direction(i);
		const Vector3& c = lightEnv->Get_Light_Diffuse(i);
		dirs[n][0] = d.X; dirs[n][1] = d.Y; dirs[n][2] = d.Z; dirs[n][3] = 0.0f;
		difs[n][0] = c.X; difs[n][1] = c.Y; difs[n][2] = c.Z; difs[n][3] = 0.0f;
	}

	inst.ambient[3] = (float)n; // numLights
}


namespace
{
 static TextureClass* s_rigidCloudTexture = nullptr;

	// Ronin @bugfix 08/06/2026 DX9: Release the cached rigid cloud texture ref when
	// instancing resources are torn down. The cache intentionally keeps the asset alive
	// across draws, but it must not pin the texture for the rest of the process.
	static void Release_Instance_Texture_Caches()
	{
		if (s_rigidCloudTexture != nullptr) {
			s_rigidCloudTexture->Release_Ref();
			s_rigidCloudTexture = nullptr;
		}
	}

	struct RigidShaderLightingConstants
	{
		float c4[4];
		float c5[4];
		float c6[4];
		float c7[4];
		float c8[4];
		float c9[4];
		float c10[4];
		float c11[4];
		float c12[4];
		float c13[4];
		float c15[4];
		float c16[4];
		float c17[4];
		float c18[4];
		float numLights;
	};

	static void Get_Rigid_Shader_Light_Constant_Slots(
		RigidShaderLightingConstants* constants,
		int lightIndex,
		float** dirOut,
		float** diffOut)
	{
		*dirOut = nullptr;
		*diffOut = nullptr;

		switch (lightIndex) {
		default:
			break;
		case 0:
			*dirOut = constants->c5;
			*diffOut = constants->c6;
			break;
		case 1:
			*dirOut = constants->c11;
			*diffOut = constants->c12;
			break;
		case 2:
			*dirOut = constants->c15;
			*diffOut = constants->c16;
			break;
		case 3:
			*dirOut = constants->c17;
			*diffOut = constants->c18;
			break;
		}
	}

	static void Upload_Rigid_View_Projection(IDirect3DDevice9* dev, D3DXMATRIX* dxViewOut)
	{
		D3DMATRIX viewMat, projMat;
		dev->GetTransform(D3DTS_VIEW, &viewMat);
		dev->GetTransform(D3DTS_PROJECTION, &projMat);

		D3DXMATRIX dxView(viewMat), dxProj(projMat), dxViewProj;
		D3DXMatrixMultiply(&dxViewProj, &dxView, &dxProj);

		D3DXMATRIX dxViewProjT;
		D3DXMatrixTranspose(&dxViewProjT, &dxViewProj);
		dev->SetVertexShaderConstantF(0, (const float*)&dxViewProjT, 4);

		if (dxViewOut != nullptr) {
			*dxViewOut = dxView;
		}
	}

	static void Build_Rigid_Shader_Lighting_Constants(
		IDirect3DDevice9* dev,
		DWORD geometryFVF,
		LightEnvironmentClass* lightEnv,
		VertexMaterialClass* material,
		const D3DXMATRIX& dxView,
		RigidShaderLightingConstants* outConstants)
	{
		if (outConstants == nullptr) {
			return;
		}

		memset(outConstants, 0, sizeof(*outConstants));

		float ambientR = 0.0f, ambientG = 0.0f, ambientB = 0.0f;

		if (lightEnv != nullptr) {
			const Vector3& eqAmb = lightEnv->Get_Equivalent_Ambient();
			ambientR = eqAmb.X;
			ambientG = eqAmb.Y;
			ambientB = eqAmb.Z;
		}
		else {
			DWORD ambientDW = 0;
			dev->GetRenderState(D3DRS_AMBIENT, &ambientDW);
			ambientR = ((ambientDW >> 16) & 0xFF) / 255.0f;
			ambientG = ((ambientDW >> 8) & 0xFF) / 255.0f;
			ambientB = ((ambientDW >> 0) & 0xFF) / 255.0f;
		}

		outConstants->c4[0] = ambientR;
		outConstants->c4[1] = ambientG;
		outConstants->c4[2] = ambientB;
		outConstants->c4[3] = 0.0f;

		int numLights = 0;

		if (lightEnv != nullptr) {
			const int envLightCount = lightEnv->Get_Light_Count();
			for (int li = 0; li < envLightCount && numLights < 4; ++li) {
				const Vector3& worldDir = lightEnv->Get_Light_Direction(li);
				const Vector3& diffuse = lightEnv->Get_Light_Diffuse(li);

				float* dirOut = nullptr;
				float* diffOut = nullptr;
				Get_Rigid_Shader_Light_Constant_Slots(outConstants, numLights, &dirOut, &diffOut);
				if (dirOut == nullptr || diffOut == nullptr) {
					break;
				}

				dirOut[0] = worldDir.X;
				dirOut[1] = worldDir.Y;
				dirOut[2] = worldDir.Z;
				dirOut[3] = 0.0f;

				diffOut[0] = diffuse.X;
				diffOut[1] = diffuse.Y;
				diffOut[2] = diffuse.Z;
				diffOut[3] = 0.0f;

				++numLights;
			}
		}
		else {
			D3DXMATRIX dxViewInv;
			D3DXMatrixInverse(&dxViewInv, nullptr, &dxView);

			for (int li = 0; li < 4; ++li) {
				BOOL lightEnabled = FALSE;
				dev->GetLightEnable(li, &lightEnabled);
				if (!lightEnabled) {
					continue;
				}

				D3DLIGHT9 light;
				memset(&light, 0, sizeof(light));
				dev->GetLight(li, &light);

				D3DXVECTOR3 camDir(light.Direction.x, light.Direction.y, light.Direction.z);
				D3DXVECTOR3 worldDir;
				D3DXVec3TransformNormal(&worldDir, &camDir, &dxViewInv);
				D3DXVec3Normalize(&worldDir, &worldDir);

				float* dirOut = nullptr;
				float* diffOut = nullptr;
				Get_Rigid_Shader_Light_Constant_Slots(outConstants, numLights, &dirOut, &diffOut);
				if (dirOut == nullptr || diffOut == nullptr) {
					break;
				}

				dirOut[0] = -worldDir.x;
				dirOut[1] = -worldDir.y;
				dirOut[2] = -worldDir.z;
				dirOut[3] = 0.0f;

				diffOut[0] = light.Diffuse.r;
				diffOut[1] = light.Diffuse.g;
				diffOut[2] = light.Diffuse.b;
				diffOut[3] = 0.0f;

				++numLights;
			}
		}

		Vector3 matDiffuse(1.0f, 1.0f, 1.0f);
		Vector3 matEmissive(0.0f, 0.0f, 0.0f);
		Vector3 matAmbient(1.0f, 1.0f, 1.0f);
		float matOpacity = 1.0f;

		if (material != nullptr) {
			material->Get_Diffuse(&matDiffuse);
			material->Get_Emissive(&matEmissive);
			material->Get_Ambient(&matAmbient);
			matOpacity = material->Get_Opacity();
		}

		outConstants->c7[0] = matDiffuse.X;
		outConstants->c7[1] = matDiffuse.Y;
		outConstants->c7[2] = matDiffuse.Z;
		outConstants->c7[3] = matOpacity;

		outConstants->c8[0] = matEmissive.X;
		outConstants->c8[1] = matEmissive.Y;
		outConstants->c8[2] = matEmissive.Z;
		outConstants->c8[3] = 0.0f;

		outConstants->c10[0] = matAmbient.X;
		outConstants->c10[1] = matAmbient.Y;
		outConstants->c10[2] = matAmbient.Z;
		outConstants->c10[3] = 0.0f;

		DWORD lightingRS = FALSE;
		dev->GetRenderState(D3DRS_LIGHTING, &lightingRS);
		float hasVertexColorFlag = (geometryFVF & D3DFVF_DIFFUSE) ? 1.0f : 0.0f;
		outConstants->c9[0] = lightingRS ? 1.0f : 0.0f;
		outConstants->c9[1] = hasVertexColorFlag;
		outConstants->c9[2] = (float)numLights;
		outConstants->c9[3] = 0.0f;

		DWORD diffuseSrc = D3DMCS_MATERIAL, ambientSrc = D3DMCS_MATERIAL, emissiveSrc = D3DMCS_MATERIAL;
		dev->GetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, &diffuseSrc);
		dev->GetRenderState(D3DRS_AMBIENTMATERIALSOURCE, &ambientSrc);
		dev->GetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, &emissiveSrc);

		outConstants->c13[0] = (diffuseSrc == D3DMCS_COLOR1 || diffuseSrc == D3DMCS_COLOR2) ? 1.0f : 0.0f;
		outConstants->c13[1] = (ambientSrc == D3DMCS_COLOR1 || ambientSrc == D3DMCS_COLOR2) ? 1.0f : 0.0f;
		outConstants->c13[2] = (emissiveSrc == D3DMCS_COLOR1 || emissiveSrc == D3DMCS_COLOR2) ? 1.0f : 0.0f;
		outConstants->c13[3] = 0.0f;

		outConstants->numLights = (float)numLights;
	}

	static void Upload_Rigid_Shader_VS_Lighting_Constants(
		IDirect3DDevice9* dev,
		const RigidShaderLightingConstants& constants)
	{
		dev->SetVertexShaderConstantF(4, constants.c4, 1);
		dev->SetVertexShaderConstantF(5, constants.c5, 1);
		dev->SetVertexShaderConstantF(6, constants.c6, 1);
		dev->SetVertexShaderConstantF(7, constants.c7, 1);
		dev->SetVertexShaderConstantF(8, constants.c8, 1);
		dev->SetVertexShaderConstantF(9, constants.c9, 1);
		dev->SetVertexShaderConstantF(10, constants.c10, 1);
		dev->SetVertexShaderConstantF(11, constants.c11, 1);
		dev->SetVertexShaderConstantF(12, constants.c12, 1);
		dev->SetVertexShaderConstantF(13, constants.c13, 1);
		dev->SetVertexShaderConstantF(15, constants.c15, 1);
		dev->SetVertexShaderConstantF(16, constants.c16, 1);
		dev->SetVertexShaderConstantF(17, constants.c17, 1);
		dev->SetVertexShaderConstantF(18, constants.c18, 1);
	}

	static void Upload_Rigid_Shader_PS_Lighting_Constants(
		IDirect3DDevice9* dev,
		const RigidShaderLightingConstants& constants)
	{
		dev->SetPixelShaderConstantF(3, constants.c5, 1);
		dev->SetPixelShaderConstantF(4, constants.c6, 1);
		dev->SetPixelShaderConstantF(5, constants.c11, 1);
		dev->SetPixelShaderConstantF(6, constants.c12, 1);
		dev->SetPixelShaderConstantF(7, constants.c15, 1);
		dev->SetPixelShaderConstantF(8, constants.c16, 1);
		dev->SetPixelShaderConstantF(9, constants.c17, 1);
		dev->SetPixelShaderConstantF(10, constants.c18, 1);

		const float psC11[4] = { constants.numLights, 0.0f, 0.0f, 0.0f };
		dev->SetPixelShaderConstantF(11, psC11, 1);
	}

	static TextureClass* Get_Valid_Rigid_Cloud_Texture()
	{
		if (s_rigidCloudTexture == nullptr) {
			WW3DAssetManager* am = WW3DAssetManager::Get_Instance();
			if (am != nullptr) {
				TextureClass* candidate = am->Get_Texture("TSCloudMed.tga", MIP_LEVELS_ALL);
				if (candidate != nullptr) {
					candidate->Init();
					if (!candidate->Is_Missing_Texture() && candidate->Peek_D3D_Texture() != nullptr) {
						s_rigidCloudTexture = candidate;
					}
					else {
						candidate->Release_Ref();
					}
				}
			}
		}

		return s_rigidCloudTexture;
	}
}

// ----------------------------------------------------------------------------

DX8InstanceManagerClass::DX8InstanceManagerClass()
	: m_available(false)
	, m_enabled(true)
	, m_singleRigidVB(nullptr)
	, m_singleRigidCursor(0)
	, m_instanceVS(nullptr)
	, m_instanceVSNoColor(nullptr)
	, m_rigidVS(nullptr)
	, m_rigidVSNoColor(nullptr)
	, m_instancePS(nullptr)
	, m_reflectivePS(nullptr)
	, m_declCacheCount(0)
	, m_geometryDeclCacheCount(0)
	, m_pendingSingleRigidCount(0)
	, m_srFVF(0)
	, m_instancedRecords(0)
	, m_instancedDrawCalls(0)
	, m_instancedMeshes(0)
	, m_instancedIndividualDraws(0)
	, m_instancedNormalMapped(0)
	, m_instancedNormalMappedMerged(0)
	, m_instancedLightBreaks(0)
	, m_instancedFlushes(0)
	, m_reflectiveDraws(0)
	, m_lastFrameInstancedRecords(0)
	, m_lastFrameInstancedDrawCalls(0)
	, m_lastFrameInstancedMeshes(0)
	, m_lastFrameInstancedIndividualDraws(0)
	, m_lastFrameInstancedNormalMapped(0)
	, m_lastFrameInstancedNormalMappedMerged(0)
	, m_lastFrameInstancedLightBreaks(0)
	, m_lastFrameInstancedFlushes(0)
	, m_lastFrameReflectiveDraws(0)
	, m_instancedStatsFrame(0)
{
	memset(m_declCache, 0, sizeof(m_declCache));
	memset(m_geometryDeclCache, 0, sizeof(m_geometryDeclCache));
}

// ----------------------------------------------------------------------------

void DX8InstanceManagerClass::Release_Resources()
{
  Release_Instance_Texture_Caches();

	if (m_singleRigidVB) { m_singleRigidVB->Release(); m_singleRigidVB = nullptr; }
	m_singleRigidCursor = 0;
	if (m_instanceVS) { m_instanceVS->Release(); m_instanceVS = nullptr; }
	if (m_instanceVSNoColor) { m_instanceVSNoColor->Release(); m_instanceVSNoColor = nullptr; }
	if (m_rigidVS) { m_rigidVS->Release(); m_rigidVS = nullptr; }
	if (m_rigidVSNoColor) { m_rigidVSNoColor->Release(); m_rigidVSNoColor = nullptr; }
	if (m_instancePS) { m_instancePS->Release(); m_instancePS = nullptr; }
	if (m_reflectivePS) { m_reflectivePS->Release(); m_reflectivePS = nullptr; }

	for (unsigned i = 0; i < m_declCacheCount; ++i) {
		if (m_declCache[i].decl) {
			m_declCache[i].decl->Release();
			m_declCache[i].decl = nullptr;
		}
	}
	m_declCacheCount = 0;

	for (unsigned i = 0; i < m_geometryDeclCacheCount; ++i) {
		if (m_geometryDeclCache[i].decl) {
			m_geometryDeclCache[i].decl->Release();
			m_geometryDeclCache[i].decl = nullptr;
		}
	}
	m_geometryDeclCacheCount = 0;
}

// ----------------------------------------------------------------------------

IDirect3DVertexDeclaration9* DX8InstanceManagerClass::Get_Or_Create_Geometry_Decl(DWORD geometryFVF)
{
	for (unsigned i = 0; i < m_geometryDeclCacheCount; ++i) {
		if (m_geometryDeclCache[i].fvf == geometryFVF) {
			return m_geometryDeclCache[i].decl;
		}
	}

	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();
	if (!dev) return nullptr;

	D3DVERTEXELEMENT9 elements[20];
	int idx = 0;
	WORD offset = 0;

	if (geometryFVF & D3DFVF_XYZ) {
		elements[idx++] = { 0, offset, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 };
		offset += 12;
	}
	else if (geometryFVF & D3DFVF_XYZRHW) {
		elements[idx++] = { 0, offset, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0 };
		offset += 16;
	}

	if (geometryFVF & D3DFVF_NORMAL) {
		elements[idx++] = { 0, offset, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 };
		offset += 12;
	}

	if (geometryFVF & D3DFVF_DIFFUSE) {
		elements[idx++] = { 0, offset, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 };
		offset += 4;
	}

	if (geometryFVF & D3DFVF_SPECULAR) {
		elements[idx++] = { 0, offset, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 1 };
		offset += 4;
	}

	const int texCount = (geometryFVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	if (texCount > 0) {
		elements[idx++] = { 0, offset, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 };
		offset += 8;
	}

	elements[idx] = D3DDECL_END();

	IDirect3DVertexDeclaration9* newDecl = nullptr;
	HRESULT hr = dev->CreateVertexDeclaration(elements, &newDecl);
	if (FAILED(hr)) {
		WWDEBUG_SAY(("DX8InstanceManager: CreateVertexDeclaration (geometry) for FVF 0x%08X failed: 0x%08X", geometryFVF, hr));
		return nullptr;
	}

	if (m_geometryDeclCacheCount < MAX_CACHED_DECLS) {
		m_geometryDeclCache[m_geometryDeclCacheCount].fvf = geometryFVF;
		m_geometryDeclCache[m_geometryDeclCacheCount].decl = newDecl;
		m_geometryDeclCacheCount++;
	}
	else {
		if (m_geometryDeclCache[0].decl) {
			m_geometryDeclCache[0].decl->Release();
		}
		memmove(&m_geometryDeclCache[0], &m_geometryDeclCache[1], sizeof(CachedDecl) * (MAX_CACHED_DECLS - 1));
		m_geometryDeclCache[MAX_CACHED_DECLS - 1].fvf = geometryFVF;
		m_geometryDeclCache[MAX_CACHED_DECLS - 1].decl = newDecl;
	}

	return newDecl;
}

// ----------------------------------------------------------------------------

bool DX8InstanceManagerClass::Load_Instance_Shader()
{
	// Ronin @bugfix 07/03/2026 DX9: Load two instancing VS variants:
	//  - RigidInstance.vso         for FVFs with D3DFVF_DIFFUSE / COLOR0
	//  - RigidInstance_NoColor.vso for FVFs without vertex diffuse color
	if (!Load_Vertex_Shader_From_File("shaders\\RigidInstance.vso", &m_instanceVS)) {
		WWDEBUG_SAY(("DX8InstanceManager: Failed loading shaders\\RigidInstance.vso"));
		return false;
	}

	if (!Load_Vertex_Shader_From_File("shaders\\RigidInstance_NoColor.vso", &m_instanceVSNoColor)) {
		WWDEBUG_SAY(("DX8InstanceManager: Failed loading shaders\\RigidInstance_NoColor.vso"));
		return false;
	}

	// Ronin @bugfix 23/05/2026 DX9 R3: Disable the non-instanced programmable
	// rigid fallback for now. Persistent artifacts remain even after reusing the
	// instanced shader/declaration path, so the safe behavior is to fall back to
	// the legacy rigid renderer until the root cause is properly identified.
	m_rigidVS = nullptr;
	m_rigidVSNoColor = nullptr;

	// Ronin @feature 08/03/2026 DX9: Use a minimal programmable pixel shader for instanced
	// draws so AMD does not have to interpret the fixed-function pixel combiner path here.
	if (!Load_Pixel_Shader_From_File("shaders\\RigidInstance.pso", &m_instancePS)) {
		WWDEBUG_SAY(("DX8InstanceManager: Failed loading shaders\\RigidInstance.pso"));
		return false;
	}

	// Ronin @feature DX9: per-pixel environment reflection PS for the reflective pass-0 of
	// reflective rigid meshes (SKYLIGHTS lakedusk). NON-FATAL: if absent we keep those meshes
	// on the legacy env path, so a missing .pso never breaks rigid rendering.
	if (!Load_Pixel_Shader_From_File("shaders\\ReflectiveRigid.pso", &m_reflectivePS)) {
		WWDEBUG_SAY(("DX8InstanceManager: ReflectiveRigid.pso not loaded; reflective rigid path disabled"));
		m_reflectivePS = nullptr;
	}

	return true;
}

// ----------------------------------------------------------------------------

// Ronin @diagnostic 21/06/2026 DX9: [SR] counters, drawn on screen because we A/B in RELEASE.
// Ronin @cleanup 01/08/2026 §16: g_SR_PerfMode removed with the legacy inline path. g_SR_DrawCount is
// bumped only by Collect_Single_Rigid, so [SR] draws/frame == [INST] recs; reflective counts separately.
static unsigned g_SR_DrawCount      = 0;          // accumulating during the current frame
static unsigned g_SR_LastFrameCount = 0;          // total from the last COMPLETED frame (for the HUD)
static unsigned g_SR_LastFrame      = 0xFFFFFFFFu;

// Ronin @feature 12/08/2026 DX9: §29 phase 2 — shadow map pushed down from W3DShadowMap.
static IDirect3DBaseTexture9* s_shadowMapTex   = nullptr;
static float                  s_shadowLightVP[16] = { 0 };
static float                  s_shadowTexelOfs  = 0.0f;
static float                  s_shadowDepthBias = 0.0f;
// Ronin @feature 17/08/2026 DX9: §29h-6. The receiver needs the sun direction (N.L fade) and the world
// size of one shadow texel (normal-offset bias). Both are fit-dependent, so they ride down the same
// channel as the matrix rather than being re-derived on this side.
static float                  s_shadowLightDir[3] = { 0.0f, 0.0f, -1.0f };
static float                  s_shadowTexelWorld  = 0.0f;

void DX8InstanceManagerClass::Set_Shadow_Map(IDirect3DBaseTexture9* tex, const float* lightViewProjT,
                                             float texelOffset, float depthBias,
                                             const float* lightTravelDir, float texelWorldSize)
{
	s_shadowMapTex     = tex;
	s_shadowTexelOfs   = texelOffset;
	s_shadowDepthBias  = depthBias;
	s_shadowTexelWorld = texelWorldSize;
	if (lightViewProjT != nullptr)
		memcpy(s_shadowLightVP, lightViewProjT, sizeof(float) * 16);
	if (lightTravelDir != nullptr)
		memcpy(s_shadowLightDir, lightTravelDir, sizeof(float) * 3);
}


// Ronin @diagnostic 26/06/2026 DX9 P2: how many category flushes per frame (=> avg batch size).
// Many tiny flushes => per-category pipeline re-setup is the cost; few big ones => it's per-mesh.
static unsigned g_SR_FlushCount          = 0;
static unsigned g_SR_LastFrameFlushCount = 0;

// Accessors for the on-screen readout in W3DDisplay.cpp (kept OUT of the debug-stats system).
unsigned DX8_Get_Single_Rigid_Last_Frame_Draw_Count()  { return g_SR_LastFrameCount; }
unsigned DX8_Get_Single_Rigid_Last_Frame_Flush_Count() { return g_SR_LastFrameFlushCount; }


// Ronin @bugfix 23/05/2026 DX9 R3: one world transform through stream 1, visually identical to a batch.
// Ronin @cleanup 01/08/2026 §16: was Draw_Single_Rigid(..., reflective). Specialized to the one draw that
// can't be batched -- needs m_reflectivePS + per-draw camera/view constants, and must land before its
// batched pass-1 overlay. Detail: InstancingMergePlan.md §11e.
bool DX8InstanceManagerClass::Draw_Reflective_Rigid(
	DX8PolygonRendererClass* renderer,
	DWORD geometryFVF,
	LightEnvironmentClass* lightEnv,
	VertexMaterialClass* material,
	TextureClass* diffuseTexture,
	const Matrix3D& worldTransform,
	const RigidTexGen& texGen)
{

	// Ronin @bugfix 31/07/2026 §16 DX9: strips can NEVER take the programmable path. The only draw this
	// function issues is Render_Instanced -- a bare D3DPT_TRIANGLELIST DrawIndexedPrimitive with
	// index_count/3 primitives -- while a strip needs Draw_Strip. Returning false makes the caller fall
	// through to legacy FFP, whose Render() branches on Is_Strip() correctly.
	if (renderer == nullptr || renderer->Is_Strip()) {
		return false;
	}

	// Ronin @diagnostic 01/08/2026 §16 DX9: reflective meshes are their own line item now. They used to
	// bump g_SR_DrawCount, which silently inflated [SR] draws/frame above [INST] recs (that was the
	// 556-vs-555 gap) because the [SR] counter is fed by Collect_Single_Rigid and reflective draws never
	// go through it. Rolled here so the frame boundary is picked up during the per-mesh walk (reflective
	// draws happen before any container flush); the count itself is bumped at the draw, so the device-lost
	// / lock-failure early-outs below can't report draws that never happened.
	Roll_Instancing_Stats_Frame();

	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev == nullptr) {
		return false;
	}

	IDirect3DVertexDeclaration9* instanceDecl = Get_Or_Create_Instance_Decl(geometryFVF);
	if (instanceDecl == nullptr) {
		return false;
	}

	const bool hasVertexColor = (geometryFVF & D3DFVF_DIFFUSE) != 0;
	IDirect3DVertexShader9* selectedVS = hasVertexColor ? m_instanceVS : m_instanceVSNoColor;
	// Ronin @feature DX9: the per-pixel env-reflection PS -- the whole reason this draw stays inline.
	// Null means ReflectiveRigid.pso failed to load; returning false drops the mesh to the legacy env
	// path (the caller also pre-checks Has_Reflective_PS(), this is the backstop).
	IDirect3DPixelShader9* selectedPS = m_reflectivePS;
	if (selectedVS == nullptr || selectedPS == nullptr || m_singleRigidVB == nullptr) {
		return false;
	}

	// Ronin @bugfix 17/06/2026 DX9 Rigid parity: the normal map is OPTIONAL here, mirroring
	// Draw_Instanced. Meshes without a <basename>_NRM map (e.g. scrolling treads) must still
	// take the single-rigid path instead of silently falling back to legacy.
	TextureClass* normalMapTex = Get_Normal_Map_For_Diffuse_Texture(diffuseTexture);
	const bool normalMapActive = (normalMapTex != nullptr);


	// Make sure the category's stage-0 texture/material/shader state (incl. the geometry VB on
	// stream 0) is on the device before we switch to the programmable rigid path.
	DX8Wrapper::Apply_Render_State_Changes();

	// Ronin @perf 20/06/2026 DX9 P0: removed the per-mesh device-state save via Get* queries.
	// Each Get* (Stream/Indices/FVF/Decl/VS/PS/SoftwareVP) stalls the D3D9 runtime; across
	// hundreds of single-rigid meshes that was the dominant cost. We don't need them:
	//   * Render_Instanced is a bare DrawIndexedPrimitive — it never changes stream 0 or the
	//     index buffer, so there's nothing to save there.
	//   * VS/PS are fixed-function (null) on entry, so we restore to null at the end, not to a
	//     queried value. The caller re-binds the FFP layout/shader and we Invalidate() below.
	dev->SetVertexDeclaration(instanceDecl);
	dev->SetVertexShader(selectedVS);
	dev->SetPixelShader(selectedPS);
	dev->SetSoftwareVertexProcessing(FALSE); // VS 3.0 instancing needs HW VP; no-op on pure-HW devices

	dev->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 1);
	dev->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1);

	// Stream 0 already holds the geometry VB (from Apply_Render_State_Changes above);
	// only the per-instance stream needs binding.
	// Ronin @perf 24/06/2026 DX9 P0.5: bind this mesh's slot in the single-rigid ring buffer. Advance
	// the cursor here (all modes) so the slot is stable for both the bind and the upload below; the
	// upload picks DISCARD vs NOOVERWRITE from ringSlot. The GPU reads instance 0 at this byte offset.
	const unsigned ringSlot = m_singleRigidCursor;
	m_singleRigidCursor++;
	if (m_singleRigidCursor >= SINGLE_RIGID_RING_INSTANCES) {
		m_singleRigidCursor = 0;
	}
	const unsigned ringByteOffset = ringSlot * sizeof(InstanceData);
	dev->SetStreamSource(1, m_singleRigidVB, ringByteOffset, sizeof(InstanceData));

	D3DMATRIX identityMat;
	memset(&identityMat, 0, sizeof(identityMat));
	identityMat._11 = identityMat._22 = identityMat._33 = identityMat._44 = 1.0f;
	dev->SetTransform(D3DTS_WORLD, &identityMat);

	D3DXMATRIX dxView;
	Upload_Rigid_View_Projection(dev, &dxView);
	
	// Ronin @feature DX9: world-space camera position for the reflective PS (register c0). The camera
	// origin in world space is the translation row of the inverse view matrix.
	float reflectiveCameraPosPS[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	{
		D3DXMATRIX invView;
		D3DXMatrixInverse(&invView, nullptr, &dxView);
		reflectiveCameraPosPS[0] = invView._41;
		reflectiveCameraPosPS[1] = invView._42;
		reflectiveCameraPosPS[2] = invView._43;
	}

	RigidShaderLightingConstants lightingConstants;
	Build_Rigid_Shader_Lighting_Constants(dev, geometryFVF, lightEnv, material, dxView, &lightingConstants);
	Upload_Rigid_Shader_VS_Lighting_Constants(dev, lightingConstants);

	
	// Ronin @feature 16/06/2026 DX9 Rigid parity: programmable texcoord-gen (Phase 1: AFFINE_UV).
	// Always uploaded so a disabled draw cannot inherit a previous draw's enable flag.
	{
		const float texGenParams[4] = { texGen.enabled ? 1.0f : 0.0f, (float)texGen.sourceMode, 0.0f, 0.0f };
		dev->SetVertexShaderConstantF(19, texGenParams, 1);
		dev->SetVertexShaderConstantF(20, texGen.row0, 1);
		dev->SetVertexShaderConstantF(21, texGen.row1, 1);
	}


	// Ronin @feature 07/06/2026 DX9: write this mesh's transform + lighting into the
	// instance VB. Lighting comes from the just-built constants so both the lightenv
	// and FFP-readback branches reproduce the previous shared-constant VS behavior.
	{
		InstanceData inst;
		memcpy(inst.row0, (const float*)&worldTransform[0], sizeof(inst.row0));
		memcpy(inst.row1, (const float*)&worldTransform[1], sizeof(inst.row1));
		memcpy(inst.row2, (const float*)&worldTransform[2], sizeof(inst.row2));

		inst.ambient[0] = lightingConstants.c4[0];
		inst.ambient[1] = lightingConstants.c4[1];
		inst.ambient[2] = lightingConstants.c4[2];
		inst.ambient[3] = lightingConstants.numLights;
		memcpy(inst.lightDir0, lightingConstants.c5, sizeof(inst.lightDir0));
		memcpy(inst.lightDiffuse0, lightingConstants.c6, sizeof(inst.lightDiffuse0));
		memcpy(inst.lightDir1, lightingConstants.c11, sizeof(inst.lightDir1));
		memcpy(inst.lightDiffuse1, lightingConstants.c12, sizeof(inst.lightDiffuse1));
		memcpy(inst.lightDir2, lightingConstants.c15, sizeof(inst.lightDir2));
		memcpy(inst.lightDiffuse2, lightingConstants.c16, sizeof(inst.lightDiffuse2));
		memcpy(inst.lightDir3, lightingConstants.c17, sizeof(inst.lightDir3));
		memcpy(inst.lightDiffuse3, lightingConstants.c18, sizeof(inst.lightDiffuse3));

		void* pData = nullptr;
		// Ronin @perf 24/06/2026 DX9 P0.5: ring + NOOVERWRITE instead of a per-mesh DISCARD on a
		// 1-instance VB. DISCARD only on the first slot of the frame (ringSlot 0); every other mesh
		// appends with NOOVERWRITE, which the driver guarantees won't stall (no rename, no wait).
		const DWORD lockFlag = (ringSlot == 0) ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE;
		HRESULT hrFill = m_singleRigidVB->Lock(ringByteOffset, sizeof(InstanceData), &pData, lockFlag);
		if (FAILED(hrFill)) {
			WWDEBUG_SAY(("DX8InstanceManager: Single rigid ring VB lock failed: 0x%08X", hrFill));
			if (normalMapTex != nullptr) {
				normalMapTex->Release_Ref();
			}
			return false;

		}
		memcpy(pData, &inst, sizeof(inst));
		m_singleRigidVB->Unlock();
	}

	{
		const bool cloudEnabled =
			(TheGlobalData != nullptr) &&
			TheGlobalData->m_useCloudMap &&
			(TheGlobalData->m_timeOfDay != TIME_OF_DAY_NIGHT) &&
			(geometryFVF & D3DFVF_DIFFUSE) == 0;

		TextureClass* rigidCloudTex = cloudEnabled ? Get_Valid_Rigid_Cloud_Texture() : nullptr;
		const bool cloudActive = cloudEnabled && (rigidCloudTex != nullptr);

		float cloudScale = 0.0f, cloudOffsetX = 0.0f, cloudOffsetY = 0.0f;
		if (cloudActive) {
			W3DShaderManager::getCloudMapState(&cloudScale, &cloudOffsetX, &cloudOffsetY);
		}

		const float c14[4] = { cloudActive ? 1.0f : 0.0f, cloudScale, cloudOffsetX, cloudOffsetY };
		dev->SetVertexShaderConstantF(14, c14, 1);

		const float psC0[4] = { cloudActive ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
		dev->SetPixelShaderConstantF(0, psC0, 1);

		const float psC1[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		dev->SetPixelShaderConstantF(1, psC1, 1);

		if (cloudActive) {
			IDirect3DBaseTexture9* d3dCloud = rigidCloudTex->Peek_D3D_Texture();
			dev->SetTexture(1, d3dCloud);
			dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
			dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
			dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
			dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
			dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
		}
		else {
			dev->SetTexture(1, nullptr);
		}

		const float psC2[4] = { normalMapActive ? 1.0f : 0.0f, 1.0f, 0.0f, 0.0f };
		dev->SetPixelShaderConstantF(2, psC2, 1);

		Upload_Rigid_Shader_PS_Lighting_Constants(dev, lightingConstants);

		if (normalMapActive) {
			IDirect3DBaseTexture9* d3dNormal = normalMapTex->Peek_D3D_Texture();
			dev->SetTexture(2, d3dNormal);
			dev->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
			dev->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
			dev->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
			dev->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
			dev->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
		}
		else {
			dev->SetTexture(2, nullptr);
		}

	}

	// Ronin @feature DX9: the reflective PS reads c0 as world-space camera position; override the
	// cloud flag the block above wrote to c0. Must be after that block and before the draw.
	{
		dev->SetPixelShaderConstantF(0, reflectiveCameraPosPS, 1);
		// Ronin @feature DX9: view matrix (world->camera) so the reflective PS can sphere-map the
		// reflection in CAMERA space (view-relative chrome look) instead of world space, which drops
		// the reflection on camera-facing panels as you orbit. Transposed to match the VS g_ViewProj
		// convention. Overwrites the cloud psC1/psC2 above — harmless, the reflective PS ignores cloud.
		D3DXMATRIX reflectiveViewT;
		D3DXMatrixTranspose(&reflectiveViewT, &dxView);
		dev->SetPixelShaderConstantF(1, (const float*)&reflectiveViewT, 4);
	}

	// Ronin @feature DX9: reflective pass-0 depth bias. HISTORICAL: pass-0 (programmable) wrote depth
	// ~1 LSB off the FFP pass-1 overlay, so the two z-fought -> shimmer, and we pushed pass-0 away so the
	// overlay would land. As of the BLENDED_NRM change, pass-1 is ALSO programmable and shares pass-0's
	// EXACT clip-Z (both = mul(worldPos, g_ViewProj)) -> the mismatch is gone and the bias is dead weight.
	// Behind an A/B toggle: flip true to restore the old hack ONLY if a reflective mesh whose overlay
	// still falls to FFP (no <diffuse>_NRM -> not caught by BLENDED_NRM) shimmers. Cleared after regardless.
	static const bool ENABLE_REFLECTIVE_DEPTH_BIAS = false; // false = drop the hack (pass-1 now programmable)
	if (ENABLE_REFLECTIVE_DEPTH_BIAS) {
		const float reflectiveSlopeBias = 1.0f;     // + = push pass-0 away from camera
		const float reflectiveConstBias = 1.0e-6f;  // floor for flat, camera-facing panels
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SLOPESCALEDEPTHBIAS, *(DWORD *)(&reflectiveSlopeBias));
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DEPTHBIAS, *(DWORD *)(&reflectiveConstBias));
	}

	renderer->Render_Instanced(0);
	++m_reflectiveDraws; // Ronin @diagnostic §16: [INST] refl= — draws actually issued, not attempts
	Debug_Statistics::Record_Instanced_Draw(                 // see the note in Flush_Single_Rigid
		(int)(renderer->Get_Index_Count() / 3),
		(int)renderer->Get_Vertex_Index_Range());

	// Ronin @feature DX9: clear the reflective pass-0 depth bias so it can't leak into later passes.
	{
		const float reflectiveZeroBias = 0.0f;
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SLOPESCALEDEPTHBIAS, *(DWORD *)(&reflectiveZeroBias));
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DEPTHBIAS, *(DWORD *)(&reflectiveZeroBias));
	}
	
	// Ronin @perf 20/06/2026 DX9 P0: restore to known fixed-function defaults, not queried values.
	// CRITICAL: reset the stream-frequency divider (otherwise the next non-instanced draw misreads
	// the streams) and clear the programmable VS/PS (the caller's BindLayoutFVF clears VS but not
	// PS). Stream 0 / index buffer were never changed; the caller re-binds the FFP layout and the
	// Invalidate() below makes DX8Wrapper re-apply its tracked VB/shader state on the next draw.
	dev->SetStreamSourceFreq(0, 1);
	dev->SetStreamSourceFreq(1, 1);
	dev->SetStreamSource(1, nullptr, 0, 0);

	dev->SetVertexDeclaration(nullptr);
	dev->SetVertexShader(nullptr);
	dev->SetPixelShader(nullptr);


	dev->SetTexture(1, nullptr);
	dev->SetTexture(2, nullptr);

	if (normalMapTex != nullptr) {
		normalMapTex->Release_Ref();
	}

	ShaderClass::Invalidate();
	DX8Wrapper::Invalidate_Vertex_Buffer_State();

	return true;
}

// Ronin @perf 24/06/2026 DX9 P1: defer one eligible single-rigid mesh into the per-category batch.
// Mirrors Add_Instance, but also remembers the geometry + per-mesh texgen and the category-constant
// material/diffuse/FVF so Flush_Single_Rigid can set the programmable pipeline exactly once.
bool DX8InstanceManagerClass::Collect_Single_Rigid(
	DX8PolygonRendererClass* renderer,
	DWORD geometryFVF,
	LightEnvironmentClass* lightEnv,
	VertexMaterialClass* material,
	TextureClass* diffuseTexture,
	const Matrix3D& worldTransform,
	const RigidTexGen& texGen,
	const ShaderClass& shader)
{

	// Ronin @bugfix 31/07/2026 §16 DX9: strips can NEVER be collected -- Flush_Single_Rigid draws every
	// record with Render_Instanced (D3DPT_TRIANGLELIST, index_count/3), which is wrong for strip indices.
	// CHOKEPOINT guard: the BLENDED overlay branch reaches here without consulting
	// allowProgrammableRigidFallback (it can't -- overlays are multi-pass and that gate requires
	// Get_Pass_Count()==1), so the gate's !Is_Strip() term cannot cover this path. Returning false makes
	// the caller fall through to legacy FFP, which handles strips correctly.
	if (renderer == nullptr || renderer->Is_Strip()) {
		return false;
	}

	// If the per-category batch is full, flush what we have to make room (same category state).
	if (m_pendingSingleRigidCount >= MAX_PENDING_SINGLE_RIGID) {
		Flush_Single_Rigid();
	}

	PendingSingleRigid& rec = m_pendingSingleRigid[m_pendingSingleRigidCount];
	memcpy(rec.inst.row0, (const float*)&worldTransform[0], sizeof(rec.inst.row0));
	memcpy(rec.inst.row1, (const float*)&worldTransform[1], sizeof(rec.inst.row1));
	memcpy(rec.inst.row2, (const float*)&worldTransform[2], sizeof(rec.inst.row2));
	rec.lightEnv = lightEnv;
	rec.renderer = renderer;
	rec.texGen   = texGen;
	rec.diffuse  = diffuseTexture; // per-record: a container flush spans many texture-categories
	rec.material = material;
	rec.shader   = shader;         // per-record render state (blend/z/alpha-test/cull)
	m_pendingSingleRigidCount++;

	// Container-constant: every mesh in a DX8RigidFVFCategoryContainer shares one FVF (one decl/VB).
	m_srFVF = geometryFVF;

	// Keep the on-screen [SR] readout accurate now that meshes route through Collect, not Draw.
	{
		const unsigned f = WW3D::Get_Frame_Count();
		if (f != g_SR_LastFrame) {
			g_SR_LastFrameCount = g_SR_DrawCount;
			g_SR_LastFrameFlushCount = g_SR_FlushCount;
			g_SR_LastFrame = f;
			g_SR_DrawCount = 0;
			g_SR_FlushCount = 0;
		}
		++g_SR_DrawCount;
	}

	return true;
}

// Ronin @perf 30/07/2026 §16 DX9: opaque == the single-rigid gate's blend contract. Anything else in the
// pending array is a blended diffuse overlay (ENABLE_BLENDED_RIGID collects those here too) and acts as a
// SORT BARRIER below, so alpha compositing order stays byte-for-byte what it is today.
static inline bool Single_Rigid_Shader_Is_Opaque(const ShaderClass& shader)
{
	return shader.Get_Src_Blend_Func() == ShaderClass::SRCBLEND_ONE &&
		shader.Get_Dst_Blend_Func() == ShaderClass::DSTBLEND_ZERO;
}

// Ronin @perf §16 DX9: total order on the merge key, so records that can share ONE instanced draw end up
// adjacent. Decreasing significance: render state -> material -> diffuse -> geometry -> texGen.
// Deliberately does NOT consider lighting: per-instance lights ride stream 1 (VS side), and the per-DRAW
// PS light set only ever BREAKS a run (Single_Rigid_Records_Merge), it never reorders records.
bool DX8InstanceManagerClass::Single_Rigid_Order_Less(const PendingSingleRigid& a, const PendingSingleRigid& b)
{
	// Render state (blend/z/alpha-test/cull) — the most expensive swap in the flush, so it sorts first.
	const unsigned aBits = a.shader.Get_Bits();
	const unsigned bBits = b.shader.Get_Bits();
	if (aBits != bBits) return aBits < bBits;

	// VS material constants (c7..c13).
	if (a.material != b.material) return a.material < b.material;

	// s0 diffuse — and through it the optional <diffuse>_NRM bound on s2.
	if (a.diffuse != b.diffuse) return a.diffuse < b.diffuse;

	// Geometry: the same 6 fields the old Polygon_Renderers_Are_Equivalent compared, expressed as an
	// ordering. Two DIFFERENT renderers with identical fields ARE the same geometry (meshes registered
	// into one container VB/IB) and must compare equal here, or they could never merge.
	DX8PolygonRendererClass* ra = a.renderer;
	DX8PolygonRendererClass* rb = b.renderer;
	if (ra != rb) {
		if (ra->Get_Vertex_Offset()      != rb->Get_Vertex_Offset())      return ra->Get_Vertex_Offset()      < rb->Get_Vertex_Offset();
		if (ra->Get_Index_Offset()       != rb->Get_Index_Offset())       return ra->Get_Index_Offset()       < rb->Get_Index_Offset();
		if (ra->Get_Index_Count()        != rb->Get_Index_Count())        return ra->Get_Index_Count()        < rb->Get_Index_Count();
		if (ra->Get_Min_Vertex_Index()   != rb->Get_Min_Vertex_Index())   return ra->Get_Min_Vertex_Index()   < rb->Get_Min_Vertex_Index();
		if (ra->Get_Vertex_Index_Range() != rb->Get_Vertex_Index_Range()) return ra->Get_Vertex_Index_Range() < rb->Get_Vertex_Index_Range();
		if (ra->Is_Strip()               != rb->Is_Strip())               return (int)ra->Is_Strip() < (int)rb->Is_Strip();
	}

	// texGen is per-MESH, not per-category: live scrolling treads carry their own customUVOffset, so they
	// land in separate runs and draw individually (correct, and better than the old instancing, which
	// excluded material-override meshes wholesale). Rows compare as raw bytes — exact-equality consistent
	// and immune to a NaN breaking the strict weak ordering std::sort requires.
	if (a.texGen.enabled != b.texGen.enabled)       return (int)a.texGen.enabled < (int)b.texGen.enabled;
	if (a.texGen.sourceMode != b.texGen.sourceMode) return a.texGen.sourceMode < b.texGen.sourceMode;
	int cmp = memcmp(a.texGen.row0, b.texGen.row0, sizeof(a.texGen.row0));
	if (cmp != 0) return cmp < 0;
	cmp = memcmp(a.texGen.row1, b.texGen.row1, sizeof(a.texGen.row1));
	if (cmp != 0) return cmp < 0;

	return false; // equal on the whole merge key -> these two can share one instanced draw
}

// Ronin @perf 30/07/2026 §16 DX9: can these two records ride ONE instanced draw? Equality is DERIVED from
// the sort comparator so the two can't drift apart. requireLightMatch applies only to normal-mapped runs:
// the PS reads its lights from per-DRAW constants c3..c11, so they may merge only on identical payloads.
// Rationale: InstancingMergePlan.md §11a / §11c.1.
bool DX8InstanceManagerClass::Single_Rigid_Records_Merge(
	const PendingSingleRigid& a, const PendingSingleRigid& b, bool requireLightMatch)
{
	if (Single_Rigid_Order_Less(a, b) || Single_Rigid_Order_Less(b, a)) {
		return false;
	}

	if (requireLightMatch) {
		// ambient (rgb + numLights in .w) + 4x (direction, diffuse): one contiguous block in InstanceData.
		const size_t lightingPayloadSize =
			sizeof(a.inst.ambient) + sizeof(a.inst.lightDir0) + sizeof(a.inst.lightDiffuse0) +
			sizeof(a.inst.lightDir1) + sizeof(a.inst.lightDiffuse1) + sizeof(a.inst.lightDir2) +
			sizeof(a.inst.lightDiffuse2) + sizeof(a.inst.lightDir3) + sizeof(a.inst.lightDiffuse3);
		if (memcmp(&a.inst.ambient, &b.inst.ambient, lightingPayloadSize) != 0) {
			return false;
		}
	}

	return true;
}

// Ronin @perf §16 DX9: build the draw order for one flush. Opaque records are sorted into merge runs; a
// BLENDED record keeps its exact collection position and nothing sorts across it, so alpha compositing
// order is unchanged from today. O(N log N) over a compact array (~hundreds of entries per container) —
// NOT a pointer-chasing list walk. That distinction is the whole point of the merge: the old bucket pass
// paid a second cache-missing traversal of render_task_head for the same information.
//
// Ronin @bugfix 02/08/2026 DX9: stable_sort — deterministic order for records tying on the full merge
// key. Did NOT fix the §19e.2 shimmer (that was volumetric shadows, §20); kept as correct on its own.
void DX8InstanceManagerClass::Build_Single_Rigid_Order(unsigned count)
{
	for (unsigned k = 0; k < count; ++k) {
		m_srOrder[k] = k;
	}

	const PendingSingleRigid* recs = m_pendingSingleRigid;

	unsigned i = 0;
	while (i < count) {
		if (!Single_Rigid_Shader_Is_Opaque(recs[i].shader)) {
			++i;      // barrier: this record draws exactly where it was collected
			continue;
		}
		unsigned j = i + 1;
		while (j < count && Single_Rigid_Shader_Is_Opaque(recs[j].shader)) {
			++j;
		}
		if ((j - i) > 1) {
			std::stable_sort(m_srOrder + i, m_srOrder + j,
				[recs](unsigned lhs, unsigned rhs) { return Single_Rigid_Order_Less(recs[lhs], recs[rhs]); });
		}
		i = j;
	}
}

// Ronin @perf 30/07/2026 §16 DX9 AUTO-INSTANCING: draw this container's whole single-rigid batch with ONE
// ring lock and ONE programmable-state setup/teardown, collapsing each maximal run of records that share
// full pipeline state + geometry into ONE instanced DrawIndexedPrimitive. This REPLACES the old instancing
// bucket pass, which walked the per-category render-task linked list a SECOND time (pointer chasing +
// cache misses, ~60 fps fixed) to rediscover groups this array already contains. Same technique as
// Unreal's FMeshDrawCommand::MatchesForDynamicInstancing: sort the collected draws, merge identical runs.
void DX8InstanceManagerClass::Flush_Single_Rigid()
{
	// Ronin @perf §16 DX9: minimum identical records before we collapse them into ONE instanced draw. 2
	// matches the old bucket pass's minInstancedBatchSize so the numbers stay comparable. SWEEP this
	// (2/4/8) on a DENSE map — on a normal map almost nothing reaches it, which is exactly the intent:
	// instancing is free when it can't help. The threshold does NOT affect the ordering cost.
	static const unsigned MIN_INSTANCED_RUN = 2u;

	const unsigned count = m_pendingSingleRigidCount;
	if (count == 0) {
		return;
	}
	++g_SR_FlushCount; // P2 diagnostic: count non-empty container flushes this frame
	m_pendingSingleRigidCount = 0; // consume now so a re-entrant Collect (full) can't recurse

	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev == nullptr || m_singleRigidVB == nullptr) {
		return;
	}

	IDirect3DVertexDeclaration9* instanceDecl = Get_Or_Create_Instance_Decl(m_srFVF);
	if (instanceDecl == nullptr) {
		return;
	}

	const bool hasVertexColor = (m_srFVF & D3DFVF_DIFFUSE) != 0;
	IDirect3DVertexShader9* selectedVS = hasVertexColor ? m_instanceVS : m_instanceVSNoColor;
	IDirect3DPixelShader9* selectedPS = m_instancePS;
	if (selectedVS == nullptr || selectedPS == nullptr) {
		return;
	}
	// Ensure stream 0 (category geometry VB) + index buffer + category texture/material/shader are on
	// the device before we switch to the programmable pipeline.
	DX8Wrapper::Apply_Render_State_Changes();

	// --- Order the records so mergeable ones are adjacent (and a run's ring slots contiguous) ---------
	Build_Single_Rigid_Order(count);

	// --- Fill per-instance lighting and upload the whole batch to the ring (ONE lock) ----------
	// Ronin @bugfix DX9: the batch MUST share the rolling cursor with the inline Draw_Reflective_Rigid,
	// NOT a fixed offset 0. Both draw from m_singleRigidVB; when an inline draw (reflective overlays —
	// the only remaining inline path; blended is now batched) writes a slot with NOOVERWRITE that lands
	// inside a fixed 0..count flush region whose GPU draw is still in flight, it stomps that batched
	// instance's transform/lighting -> meshes render at the wrong place / wrong team color. Allocating
	// the batch at the cursor (and advancing past it below) keeps the two paths in disjoint slots.
	// count (<=4096) < ring (8192), so a wrapped block always fits.
	if (m_singleRigidCursor + count > SINGLE_RIGID_RING_INSTANCES) {
		m_singleRigidCursor = 0; // wrap; the DISCARD below hands back a fresh buffer
	}
	const unsigned baseSlot = m_singleRigidCursor;
	const DWORD batchLockFlag = (baseSlot == 0) ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE;

	void* pData = nullptr;
	HRESULT hr = m_singleRigidVB->Lock(baseSlot * sizeof(InstanceData), count * sizeof(InstanceData), &pData, batchLockFlag);
	if (FAILED(hr)) {
		WWDEBUG_SAY(("DX8InstanceManager: Single-rigid batch lock failed: 0x%08X", hr));
		return;
	}
	InstanceData* dst = (InstanceData*)pData;
	// Upload in DRAW order: ring slot k holds the record drawn at position k, so a merged run [i, i+L)
	// occupies contiguous slots baseSlot+i .. baseSlot+i+L-1 — what an instanced draw requires.
	for (unsigned k = 0; k < count; ++k) {
		PendingSingleRigid& rec = m_pendingSingleRigid[m_srOrder[k]];
		Extract_Instance_Lighting(rec.lightEnv, rec.inst); // no device queries
		dst[k] = rec.inst;
	}
	m_singleRigidVB->Unlock();

	// Ronin @diagnostic §16 DX9: [INST] reports the MERGED flush now (self-contained per-frame roll —
	// do NOT go back to Begin/End_Frame_Statistics, they are double-bracketed and reset mid-frame).
	Roll_Instancing_Stats_Frame();
	m_instancedRecords += count;
	++m_instancedFlushes;

	// --- Programmable pipeline: set ONCE for the whole batch -----------------------------------
	dev->SetVertexDeclaration(instanceDecl);
	dev->SetVertexShader(selectedVS);
	dev->SetPixelShader(selectedPS);
	dev->SetSoftwareVertexProcessing(FALSE);

	dev->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 1);
	dev->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1);

	D3DMATRIX identityMat;
	memset(&identityMat, 0, sizeof(identityMat));
	identityMat._11 = identityMat._22 = identityMat._33 = identityMat._44 = 1.0f;
	dev->SetTransform(D3DTS_WORLD, &identityMat);

	D3DXMATRIX dxView;
	Upload_Rigid_View_Projection(dev, &dxView); // frame-constant: once per batch now, not per mesh

	// Cloud (container-constant) ----------------------------------------------------------------
	// Ronin @bugfix 26/08/2026 DX9: match the terrain's night rule (BaseHeightMapRenderObjClass::
	// useCloud). Without it rigid meshes kept sampling the cloud after dark while terrain and trees
	// had dropped it.
	const bool cloudEnabled =
		(TheGlobalData != nullptr) &&
		TheGlobalData->m_useCloudMap &&
		(TheGlobalData->m_timeOfDay != TIME_OF_DAY_NIGHT) &&
		(m_srFVF & D3DFVF_DIFFUSE) == 0;
		(m_srFVF & D3DFVF_DIFFUSE) == 0;
	TextureClass* rigidCloudTex = cloudEnabled ? Get_Valid_Rigid_Cloud_Texture() : nullptr;
	const bool cloudActive = cloudEnabled && (rigidCloudTex != nullptr);

	float cloudScale = 0.0f, cloudOffsetX = 0.0f, cloudOffsetY = 0.0f;
	if (cloudActive) {
		W3DShaderManager::getCloudMapState(&cloudScale, &cloudOffsetX, &cloudOffsetY);
	}
	const float c14[4] = { cloudActive ? 1.0f : 0.0f, cloudScale, cloudOffsetX, cloudOffsetY };
	dev->SetVertexShaderConstantF(14, c14, 1);

	const float psC0[4] = { cloudActive ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
	dev->SetPixelShaderConstantF(0, psC0, 1);
	const float psC1[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	dev->SetPixelShaderConstantF(1, psC1, 1);

	if (cloudActive) {
		IDirect3DBaseTexture9* d3dCloud = rigidCloudTex->Peek_D3D_Texture();
		dev->SetTexture(1, d3dCloud);
		dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
	} else {
		dev->SetTexture(1, nullptr);
	}

		// Ronin @feature 12/08/2026 DX9: §29 phase 2 — shadow map on s3, once per flush (batch-invariant).
	// LINEAR filtering is what triggers hardware PCF on an NVIDIA depth texture.
	if (s_shadowMapTex != nullptr) {
		dev->SetPixelShaderConstantF(12, s_shadowLightVP, 4);
		const float psC16[4] = { 1.0f, s_shadowDepthBias, s_shadowTexelOfs, s_shadowTexelOfs };
		dev->SetPixelShaderConstantF(16, psC16, 1);
		// Ronin @feature 17/08/2026 DX9: §29h-6. c17 = sun travel direction + texel world size, for the
		// receiver's N.L fade and normal-offset bias.
		const float psC17[4] = { s_shadowLightDir[0], s_shadowLightDir[1], s_shadowLightDir[2],
								 s_shadowTexelWorld };
		dev->SetPixelShaderConstantF(17, psC17, 1);
		dev->SetTexture(3, s_shadowMapTex);
		dev->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		dev->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		dev->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	} else {
		const float psC16[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		dev->SetPixelShaderConstantF(16, psC16, 1);
		dev->SetTexture(3, nullptr);
	}

	// Stage-0 (diffuse) sampler defaults for the programmable path. FFP categories normally set these
	// per texture; a container flush spans many, so use the common linear/wrap defaults once.
	dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

	// --- Draw loop. State that varies between texture-categories (shader, material, diffuse, normal map,
	// texGen) is swapped only when it changes; the sort put like with like, so these swaps happen about
	// once per distinct state, and the run-length scan then collapses whatever is left. -----------------
	VertexMaterialClass* lastMaterial = nullptr;
	TextureClass*        lastDiffuse  = nullptr;
	bool                 haveMaterial = false;
	bool                 haveDiffuse  = false;
	TextureClass*        curNormalMap = nullptr;     // ref owned here; released on change/teardown
	bool                 normalMapActive = false;
	RigidTexGen          lastTexGen;
	bool                 haveLastTexGen = false;
	unsigned             lastShaderBits = 0;
	bool                 haveShader = false;
	unsigned             lastFreq0 = D3DSTREAMSOURCE_INDEXEDDATA | 1; // matches the setup above

	unsigned i = 0;
	while (i < count) {
		PendingSingleRigid& rec = m_pendingSingleRigid[m_srOrder[i]];

		// Per-shader: re-apply this group's render state (blend/z/alpha-test/cull/fog). The gate
		// guarantees opaque BLEND for merged records, but z/alpha-test/cull still vary and a container
		// flush spans many categories — so the batch must NOT inherit one category's state (that was the
		// translucent / popping bug). Set_Shader+Apply runs the engine's own render-state path; only
		// SHADER_CHANGED is dirty here, so it sets render states, not the decl. We re-assert the
		// programmable pipeline after, and force material/diffuse re-bind in case Apply touched tracked
		// texture/material state.
		if (!haveShader || rec.shader.Get_Bits() != lastShaderBits) {
			DX8Wrapper::Set_Shader(rec.shader);
			DX8Wrapper::Apply_Render_State_Changes();
			dev->SetVertexDeclaration(instanceDecl);
			dev->SetVertexShader(selectedVS);
			dev->SetPixelShader(selectedPS);
			dev->SetSoftwareVertexProcessing(FALSE);
			dev->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 1);
			dev->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1);
			lastFreq0 = D3DSTREAMSOURCE_INDEXEDDATA | 1;
			haveMaterial = false;
			haveDiffuse  = false;
			lastShaderBits = rec.shader.Get_Bits();
			haveShader = true;
		}

		// Per-material: upload VS material constants (c7..c13). Lights ride the stream, so the lightEnv
		// here only feeds material constants; build once per distinct material.
		if (!haveMaterial || rec.material != lastMaterial) {
			RigidShaderLightingConstants lightingConstants;
			Build_Rigid_Shader_Lighting_Constants(dev, m_srFVF, rec.lightEnv, rec.material, dxView, &lightingConstants);
			Upload_Rigid_Shader_VS_Lighting_Constants(dev, lightingConstants);
			lastMaterial = rec.material;
			haveMaterial = true;
		}

		// Per-diffuse: bind the diffuse on sampler 0 and resolve/bind its optional normal map (s2).
		// normalMapActive also decides whether this run needs identical per-instance light payloads.
		if (!haveDiffuse || rec.diffuse != lastDiffuse) {
			dev->SetTexture(0, (rec.diffuse != nullptr) ? rec.diffuse->Peek_D3D_Texture() : nullptr);

			if (curNormalMap != nullptr) {
				curNormalMap->Release_Ref();
				curNormalMap = nullptr;
			}
			curNormalMap = Get_Normal_Map_For_Diffuse_Texture(rec.diffuse);
			normalMapActive = (curNormalMap != nullptr);

			const float psC2[4] = { normalMapActive ? 1.0f : 0.0f, 1.0f, 0.0f, 0.0f };
			dev->SetPixelShaderConstantF(2, psC2, 1);

			if (normalMapActive) {
				dev->SetTexture(2, curNormalMap->Peek_D3D_Texture());
				dev->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
				dev->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
				dev->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
				dev->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
				dev->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
			} else {
				dev->SetTexture(2, nullptr);
			}
			lastDiffuse = rec.diffuse;
			haveDiffuse = true;
		}

		// Per-mesh texgen (treads carry a per-unit UV offset). Skip the upload when unchanged.
		if (!haveLastTexGen || memcmp(&lastTexGen, &rec.texGen, sizeof(RigidTexGen)) != 0) {
			const float texGenParams[4] = { rec.texGen.enabled ? 1.0f : 0.0f, (float)rec.texGen.sourceMode, 0.0f, 0.0f };
			dev->SetVertexShaderConstantF(19, texGenParams, 1);
			dev->SetVertexShaderConstantF(20, rec.texGen.row0, 1);
			dev->SetVertexShaderConstantF(21, rec.texGen.row1, 1);
			lastTexGen = rec.texGen;
			haveLastTexGen = true;
		}

		// How many of the FOLLOWING records can ride this same draw? State + geometry adjacency comes
		// from the sort; the light-payload check only applies when this diffuse resolved a normal map.
		// Strips are never merged (Render_Instanced issues a TRIANGLELIST draw).
		unsigned runLen = 1;
		if (!rec.renderer->Is_Strip()) {
			while ((i + runLen) < count &&
				Single_Rigid_Records_Merge(rec, m_pendingSingleRigid[m_srOrder[i + runLen]], normalMapActive)) {
				++runLen;
			}
		}

		// Ronin @diagnostic §16 DX9: did a normal-mapped run stop ONLY because the next record's light
		// set differs? That count is the evidence for/against keeping NRM merging (flip the third
		// argument above to a hard false to disable it entirely).
		if (normalMapActive && (i + runLen) < count &&
			Single_Rigid_Records_Merge(rec, m_pendingSingleRigid[m_srOrder[i + runLen]], false)) {
			++m_instancedLightBreaks;
		}

		if (runLen < MIN_INSTANCED_RUN) {
			runLen = 1; // below the merge threshold -> draw this record on its own, exactly as before
		}

		const unsigned freq0 = D3DSTREAMSOURCE_INDEXEDDATA | runLen;
		if (freq0 != lastFreq0) {
			dev->SetStreamSourceFreq(0, freq0);
			lastFreq0 = freq0;
		}

		// Bind this run's FIRST instance slot on stream 1; the GPU walks runLen consecutive slots.
		dev->SetStreamSource(1, m_singleRigidVB, (baseSlot + i) * sizeof(InstanceData), sizeof(InstanceData));

		// PS per-pixel normal-map lighting needs this draw's lights as constants (only when a normal map
		// is present). For a merged run every instance's payload is identical by construction, so the
		// first record's values ARE the whole run's values.
		if (normalMapActive) {
			dev->SetPixelShaderConstantF(3, rec.inst.lightDir0, 1);
			dev->SetPixelShaderConstantF(4, rec.inst.lightDiffuse0, 1);
			dev->SetPixelShaderConstantF(5, rec.inst.lightDir1, 1);
			dev->SetPixelShaderConstantF(6, rec.inst.lightDiffuse1, 1);
			dev->SetPixelShaderConstantF(7, rec.inst.lightDir2, 1);
			dev->SetPixelShaderConstantF(8, rec.inst.lightDiffuse2, 1);
			dev->SetPixelShaderConstantF(9, rec.inst.lightDir3, 1);
			dev->SetPixelShaderConstantF(10, rec.inst.lightDiffuse3, 1);
			const float psNum[4] = { rec.inst.ambient[3], 0.0f, 0.0f, 0.0f };
			dev->SetPixelShaderConstantF(11, psNum, 1);
		}

		rec.renderer->Render_Instanced(0);

		// Ronin @diagnostic 02/08/2026 DX9: Render_Instanced bypasses DX8_RECORD_RENDER, so report the
		// draw here. One instanced draw for runLen meshes = 1 call, runLen x the polygons.
		Debug_Statistics::Record_Instanced_Draw(
			(int)((rec.renderer->Get_Index_Count() / 3) * runLen),
			(int)(rec.renderer->Get_Vertex_Index_Range() * runLen));

		if (runLen > 1) {
			++m_instancedDrawCalls;        // ONE instanced draw standing in for runLen individual draws
			m_instancedMeshes += runLen;
		} else {
			++m_instancedIndividualDraws;
		}

		// Ronin @diagnostic §16 DX9: makes lbrk readable. nrm=0 -> no normal-mapped records at all;
		// nrm>0 with nrmMerged=0 -> NRM records never had a state-matching neighbour (merging them is
		// moot); nrm>0, nrmMerged>0, lbrk=0 -> NRM merging works and the light payloads match.
		if (normalMapActive) {
			m_instancedNormalMapped += runLen;
			if (runLen > 1) {
				m_instancedNormalMappedMerged += runLen;
			}
		}

		i += runLen;
	}

	// Ronin @bugfix DX9: advance the shared ring cursor past this batch so the next inline
	// Draw_Reflective_Rigid (or the next flush) can't reuse these slots while the GPU is still drawing them.
	m_singleRigidCursor = baseSlot + count;
	if (m_singleRigidCursor >= SINGLE_RIGID_RING_INSTANCES) {
		m_singleRigidCursor = 0;
	}

	// --- Teardown ONCE -------------------------------------------------------------------------
	dev->SetStreamSourceFreq(0, 1);
	dev->SetStreamSourceFreq(1, 1);
	dev->SetStreamSource(1, nullptr, 0, 0);
	dev->SetVertexDeclaration(nullptr);
	dev->SetVertexShader(nullptr);
	dev->SetPixelShader(nullptr);
	dev->SetTexture(0, nullptr);
	dev->SetTexture(1, nullptr);
	dev->SetTexture(2, nullptr);

	if (curNormalMap != nullptr) {
		curNormalMap->Release_Ref();
	}

	ShaderClass::Invalidate();
	DX8Wrapper::Invalidate_Vertex_Buffer_State();
}

DX8InstanceManagerClass::~DX8InstanceManagerClass()
{
	Shutdown();
}

// ----------------------------------------------------------------------------

bool DX8InstanceManagerClass::Init()
{
	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();
	if (!dev) return false;

	// Check for SM3.0 stream frequency support
	D3DCAPS9 caps;
	dev->GetDeviceCaps(&caps);

	if (caps.VertexShaderVersion < D3DVS_VERSION(3, 0)) {
		WWDEBUG_SAY(("DX8InstanceManager: VS 3.0 not available, instancing disabled"));
		m_available = false;
		return false;
	}

	// Ronin @feature 08/03/2026 DX9: The instancing path uses a small programmable
	// pixel shader instead of the fixed-function pixel combiner path.
	if (caps.PixelShaderVersion < D3DPS_VERSION(2, 0)) {
		WWDEBUG_SAY(("DX8InstanceManager: PS 2.0 not available, instancing disabled"));
		m_available = false;
		return false;
	}

	// D3DDEVCAPS2_STREAMOFFSET is required for stream frequency divider
	if (!(caps.DevCaps2 & D3DDEVCAPS2_STREAMOFFSET)) {
		WWDEBUG_SAY(("DX8InstanceManager: Stream offset not supported, instancing disabled"));
		m_available = false;
		return false;
	}

	if (!Create_Instance_VB()) {
		WWDEBUG_SAY(("DX8InstanceManager: Failed to create instance VB"));
		Release_Resources();
		return false;
	}

	if (!Load_Instance_Shader()) {
		WWDEBUG_SAY(("DX8InstanceManager: Failed to load instancing shaders"));
		Release_Resources();
		return false;
	}

	m_available = true;
	WWDEBUG_SAY(("DX8InstanceManager: Programmable rigid path initialized (instance ring = %d)", SINGLE_RIGID_RING_INSTANCES));
	return true;
}

// ----------------------------------------------------------------------------

void DX8InstanceManagerClass::Shutdown()
{
	Release_Resources();
	m_available = false;
}

// ----------------------------------------------------------------------------


bool DX8InstanceManagerClass::Create_Instance_VB()
{
	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();

	// Ronin @perf §14e.3 DX9: the container-level instanced batch draws from the single-rigid ring
	// (m_singleRigidVB); the old per-draw m_instanceVB was removed. Create only the ring.

	HRESULT hrRing = dev->CreateVertexBuffer(
		SINGLE_RIGID_RING_INSTANCES * sizeof(InstanceData),
		D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
		0,
		D3DPOOL_DEFAULT,
		&m_singleRigidVB,
		nullptr);

	if (FAILED(hrRing)) {
		WWDEBUG_SAY(("CreateVertexBuffer for single-rigid ring failed: 0x%08X", hrRing));
		return false;
	}

	return true;
}

// ----------------------------------------------------------------------------
// Ronin @bugfix 19/02/2026 DX9: Build a combined vertex declaration from the
// mesh's FVF (stream 0) + instance transform data (stream 1).
// Cached per unique FVF to avoid recreating declarations every frame.
// Ronin @bugfix 28/02/2026 DX9: Use contiguous TEXCOORD indices to fix AMD driver
// compatibility. AMD rejects declarations with gaps in TEXCOORD usage indices.
// Ronin @bugfix 01/03/2026 DX9: Always use TEXCOORD1..3 for stream 1 instance data
// to match the compiled HLSL shader's hardcoded input semantics. The previous approach
// of using contiguous indices after the geometry's last TEXCOORD caused a declaration/
// shader semantic mismatch whenever texCount != 1, which AMD strictly rejects.

IDirect3DVertexDeclaration9* DX8InstanceManagerClass::Get_Or_Create_Instance_Decl(DWORD geometryFVF)
{
	// Search cache first
	for (unsigned i = 0; i < m_declCacheCount; ++i) {
		if (m_declCache[i].fvf == geometryFVF) {
			return m_declCache[i].decl;
		}
	}

	// Cache miss � build a new combined declaration
	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();
	if (!dev) return nullptr;

	D3DVERTEXELEMENT9 elements[20];
	int idx = 0;
	WORD offset = 0;

	// Position
	if (geometryFVF & D3DFVF_XYZ) {
		elements[idx++] = { 0, offset, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 };
		offset += 12;
	}
	else if (geometryFVF & D3DFVF_XYZRHW) {
		elements[idx++] = { 0, offset, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0 };
		offset += 16;
	}

	// Normal
	if (geometryFVF & D3DFVF_NORMAL) {
		elements[idx++] = { 0, offset, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 };
		offset += 12;
	}

	// Diffuse color
	if (geometryFVF & D3DFVF_DIFFUSE) {
		elements[idx++] = { 0, offset, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 };
		offset += 4;
	}

	// Specular color
	if (geometryFVF & D3DFVF_SPECULAR) {
		elements[idx++] = { 0, offset, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 1 };
		offset += 4;
	}

	// Texture coordinates � expose ONLY geometry TEXCOORD0 on stream 0.
	// The instancing shader reads geometry UVs from TEXCOORD0 and instance rows from
	// TEXCOORD1, TEXCOORD2, TEXCOORD3. Do NOT emit geometry TEXCOORD1+ here, or the
	// declaration will contain duplicate TEXCOORD usage indices across streams.
	// NVIDIA tends to tolerate that ambiguity; AMD does not.
	int texCount = (geometryFVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	if (texCount > 0) {
		elements[idx++] = { 0, offset, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 };
		offset += 8;
	}

	// Ronin @bugfix 01/03/2026 DX9: Stream 1 instance data MUST use TEXCOORD1, TEXCOORD2,
	// TEXCOORD3 � matching the compiled RigidInstance.vso's hardcoded input semantics.
	// The previous approach used nextTexIdx = texCount, which only matched the shader when
	// texCount == 1. For texCount == 0 or texCount >= 2, the declaration/shader semantic
	// indices diverged. NVIDIA tolerates this silently; AMD strictly validates and delivers
	// zeroes or skips the draw entirely when the VS input signature doesn't match the decl.
	//
	// Using fixed indices 1/2/3 means there IS a gap when texCount == 0 (TEXCOORD0 missing
	// from stream 0), but this is acceptable because:
	//   a) Meshes with 0 UV channels are extremely rare in instancing-eligible rigid meshes
	//   b) The gap is between streams (stream 0 has no TEXCOORD, stream 1 starts at TEXCOORD1)
	//      which AMD handles correctly � the gap rejection only applies within a single stream
	//   c) The shader's input signature is the authoritative contract; the decl must match it
	elements[idx++] = { 1,  0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 };
	elements[idx++] = { 1, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 };
	elements[idx++] = { 1, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3 };

	// Ronin @feature 07/06/2026 DX9: per-instance lighting payload, contiguous TEXCOORD4..12
	// on stream 1 (no gaps - AMD). Offsets follow the transform rows (0/16/32).
	elements[idx++] = { 1,  48, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 4 };  // ambient + numLights
	elements[idx++] = { 1,  64, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 5 };  // dir0
	elements[idx++] = { 1,  80, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 6 };  // diffuse0
	elements[idx++] = { 1,  96, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 7 };  // dir1
	elements[idx++] = { 1, 112, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 8 };  // diffuse1
	elements[idx++] = { 1, 128, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 9 };  // dir2
	elements[idx++] = { 1, 144, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 10 }; // diffuse2
	elements[idx++] = { 1, 160, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 11 }; // dir3
	elements[idx++] = { 1, 176, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 12 }; // diffuse3

	elements[idx] = D3DDECL_END();

	IDirect3DVertexDeclaration9* newDecl = nullptr;
	HRESULT hr = dev->CreateVertexDeclaration(elements, &newDecl);
	if (FAILED(hr)) {
		WWDEBUG_SAY(("DX8InstanceManager: CreateVertexDeclaration for FVF 0x%08X failed: 0x%08X", geometryFVF, hr));
		return nullptr;
	}

	if (m_declCacheCount < MAX_CACHED_DECLS) {
		m_declCache[m_declCacheCount].fvf = geometryFVF;
		m_declCache[m_declCacheCount].decl = newDecl;
		m_declCacheCount++;
	}
	else {
		// Ronin @bugfix 27/02/2026 DX9: Evict oldest cache entry to prevent COM leak
		WWDEBUG_SAY(("DX8InstanceManager: Decl cache full, evicting FVF 0x%08X for 0x%08X",
			m_declCache[0].fvf, geometryFVF));
		if (m_declCache[0].decl) {
			m_declCache[0].decl->Release();
		}
		memmove(&m_declCache[0], &m_declCache[1], sizeof(CachedDecl) * (MAX_CACHED_DECLS - 1));
		m_declCache[MAX_CACHED_DECLS - 1].fvf = geometryFVF;
		m_declCache[MAX_CACHED_DECLS - 1].decl = newDecl;
	}

	WWDEBUG_SAY(("DX8InstanceManager: Created instancing decl for FVF 0x%08X (stream0 stride=%u, %d tex coords, instance TEXCOORD1..3 fixed)",
		geometryFVF, (unsigned)offset, texCount));

	return newDecl;
}

// ----------------------------------------------------------------------------

bool DX8InstanceManagerClass::Load_Vertex_Shader_From_File(const char* shaderPath, IDirect3DVertexShader9** outShader)
{
	if (!shaderPath || !outShader) {
		return false;
	}

	*outShader = nullptr;

	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();
	if (!dev) {
		return false;
	}

	HANDLE hFile = CreateFileA(shaderPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) {
		WWDEBUG_SAY(("DX8InstanceManager: Could not open %s (error %d)", shaderPath, GetLastError()));
		return false;
	}

	DWORD fileSize = GetFileSize(hFile, nullptr);
	if (fileSize == 0 || fileSize == INVALID_FILE_SIZE || fileSize < sizeof(DWORD)) {
		CloseHandle(hFile);
		return false;
	}

	DWORD* shaderBytecode = (DWORD*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, fileSize);
	if (!shaderBytecode) {
		CloseHandle(hFile);
		return false;
	}

	DWORD bytesRead = 0;
	BOOL readOk = ReadFile(hFile, shaderBytecode, fileSize, &bytesRead, nullptr);
	CloseHandle(hFile);

	if (!readOk || bytesRead != fileSize) {
		HeapFree(GetProcessHeap(), 0, shaderBytecode);
		return false;
	}

	// Ronin @bugfix 27/02/2026 DX9: Validate VS 3.0 bytecode magic before CreateVertexShader
	if (shaderBytecode[0] != 0xFFFE0300) {
		WWDEBUG_SAY(("DX8InstanceManager: %s is not a VS 3.0 shader (magic=0x%08X)", shaderPath, shaderBytecode[0]));
		HeapFree(GetProcessHeap(), 0, shaderBytecode);
		return false;
	}

	HRESULT hr = dev->CreateVertexShader(shaderBytecode, outShader);
	HeapFree(GetProcessHeap(), 0, shaderBytecode);

	if (FAILED(hr)) {
		WWDEBUG_SAY(("DX8InstanceManager: CreateVertexShader failed for %s: 0x%08X", shaderPath, hr));
		return false;
	}

	WWDEBUG_SAY(("DX8InstanceManager: Loaded %s (%lu bytes), VS=%p", shaderPath, fileSize, *outShader));
	return true;
}

bool DX8InstanceManagerClass::Load_Pixel_Shader_From_File(const char* shaderPath, IDirect3DPixelShader9** outShader)
{
	if (!shaderPath || !outShader) {
		return false;
	}

	*outShader = nullptr;

	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();
	if (!dev) {
		return false;
	}

	HANDLE hFile = CreateFileA(shaderPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) {
		WWDEBUG_SAY(("DX8InstanceManager: Could not open %s (error %d)", shaderPath, GetLastError()));
		return false;
	}

	DWORD fileSize = GetFileSize(hFile, nullptr);
	if (fileSize == 0 || fileSize == INVALID_FILE_SIZE || fileSize < sizeof(DWORD)) {
		CloseHandle(hFile);
		return false;
	}

	DWORD* shaderBytecode = (DWORD*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, fileSize);
	if (!shaderBytecode) {
		CloseHandle(hFile);
		return false;
	}

	DWORD bytesRead = 0;
	BOOL readOk = ReadFile(hFile, shaderBytecode, fileSize, &bytesRead, nullptr);
	CloseHandle(hFile);

	if (!readOk || bytesRead != fileSize) {
		HeapFree(GetProcessHeap(), 0, shaderBytecode);
		return false;
	}

	// Ronin @feature 08/03/2026 DX9: Accept either ps_2_0 or ps_3_0 bytecode.
	const DWORD shaderMagic = shaderBytecode[0];
	if (shaderMagic != 0xFFFF0200 && shaderMagic != 0xFFFF0300) {
		WWDEBUG_SAY(("DX8InstanceManager: %s is not a supported pixel shader (magic=0x%08X)", shaderPath, shaderMagic));
		HeapFree(GetProcessHeap(), 0, shaderBytecode);
		return false;
	}

	HRESULT hr = dev->CreatePixelShader(shaderBytecode, outShader);
	HeapFree(GetProcessHeap(), 0, shaderBytecode);

	if (FAILED(hr)) {
		WWDEBUG_SAY(("DX8InstanceManager: CreatePixelShader failed for %s: 0x%08X", shaderPath, hr));
		return false;
	}

	WWDEBUG_SAY(("DX8InstanceManager: Loaded %s (%lu bytes), PS=%p", shaderPath, fileSize, *outShader));
	return true;
}

// ----------------------------------------------------------------------------

// Ronin @diagnostic §14e.3 DX9: roll the instancing counters on frame change, independent of
// Debug_Statistics::Begin/End_Statistics (bracketed from ww3d.cpp AND W3DDisplay.cpp -> it reset these
// mid-frame, so the HUD read 0). Mirrors the [SR] counters' WW3D::Get_Frame_Count() reset.
void DX8InstanceManagerClass::Roll_Instancing_Stats_Frame()
{
	const unsigned f = WW3D::Get_Frame_Count();
	if (f == m_instancedStatsFrame) return;
	m_lastFrameInstancedRecords         = m_instancedRecords;
	m_lastFrameInstancedDrawCalls       = m_instancedDrawCalls;
	m_lastFrameInstancedMeshes          = m_instancedMeshes;
	m_lastFrameInstancedIndividualDraws = m_instancedIndividualDraws;
	m_lastFrameInstancedLightBreaks     = m_instancedLightBreaks;
	m_lastFrameInstancedNormalMapped       = m_instancedNormalMapped;
	m_lastFrameInstancedNormalMappedMerged = m_instancedNormalMappedMerged;
	m_lastFrameInstancedFlushes         = m_instancedFlushes;
	m_lastFrameReflectiveDraws          = m_reflectiveDraws;
	m_instancedRecords = 0;
	m_instancedDrawCalls = 0;
	m_instancedMeshes = 0;
	m_instancedIndividualDraws = 0;
	m_instancedLightBreaks = 0;
	m_instancedNormalMapped = 0;
	m_instancedNormalMappedMerged = 0;
	m_instancedFlushes = 0;
	m_reflectiveDraws = 0;
	m_instancedStatsFrame = f;
}

// Ronin @diagnostic §14e.3 DX9: the instancing counters roll themselves now (Roll_Instancing_Stats_Frame,
// like the [SR] line). These stay no-ops so DX8Wrapper::Begin/End_Statistics still link — but they must NOT
// touch the counters (the mid-frame double-bracket was resetting them to 0).
void DX8InstanceManagerClass::Begin_Frame_Statistics() {}
void DX8InstanceManagerClass::End_Frame_Statistics()   {}

