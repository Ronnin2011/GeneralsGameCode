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

#include "dx8instancing.h"
#include "dx8wrapper.h"
#include "dx8polygonrenderer.h"
#include "dx8fvf.h"
#include "dx8caps.h"

#include "Common/GlobalData.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "assetmgr.h"
#include "vertmaterial.h"

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
	, m_instanceVB(nullptr)
	, m_instanceVS(nullptr)
	, m_instanceVSNoColor(nullptr)
	, m_rigidVS(nullptr)
	, m_rigidVSNoColor(nullptr)
	, m_instancePS(nullptr)
	, m_declCacheCount(0)
	, m_geometryDeclCacheCount(0)
	, m_collectedCount(0)
	, m_instancedDrawCalls(0)
	, m_instancedMeshes(0)
	, m_instancedMixedLightDrawCalls(0)
	, m_instancedMixedLightMeshes(0)
	, m_lastFrameInstancedDrawCalls(0)
	, m_lastFrameInstancedMeshes(0)
	, m_lastFrameInstancedMixedLightDrawCalls(0)
	, m_lastFrameInstancedMixedLightMeshes(0)
{
	memset(m_declCache, 0, sizeof(m_declCache));
	memset(m_geometryDeclCache, 0, sizeof(m_geometryDeclCache));
}

// ----------------------------------------------------------------------------

void DX8InstanceManagerClass::Release_Resources()
{
  Release_Instance_Texture_Caches();

	if (m_instanceVB) { m_instanceVB->Release(); m_instanceVB = nullptr; }
	if (m_instanceVS) { m_instanceVS->Release(); m_instanceVS = nullptr; }
	if (m_instanceVSNoColor) { m_instanceVSNoColor->Release(); m_instanceVSNoColor = nullptr; }
	if (m_rigidVS) { m_rigidVS->Release(); m_rigidVS = nullptr; }
	if (m_rigidVSNoColor) { m_rigidVSNoColor->Release(); m_rigidVSNoColor = nullptr; }
	if (m_instancePS) { m_instancePS->Release(); m_instancePS = nullptr; }

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

	return true;
}

// ----------------------------------------------------------------------------

// Ronin @bugfix 23/05/2026 DX9 R3: Reuse the exact instanced programmable rigid path
// for the single-mesh fallback too. The separate non-instanced VS path diverged from
// the working instanced shader contract and produced persistent normal-map artifacts on
// rigid meshes. Feeding one world transform through stream 1 keeps the fallback visually
// identical to the instanced path.
bool DX8InstanceManagerClass::Draw_Single_Rigid(
	DX8PolygonRendererClass* renderer,
	DWORD geometryFVF,
	LightEnvironmentClass* lightEnv,
	VertexMaterialClass* material,
	TextureClass* diffuseTexture,
	const Matrix3D& worldTransform,
	unsigned baseVertexOffset)
{
	if (renderer == nullptr) {
		return false;
	}

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
	IDirect3DPixelShader9* selectedPS = m_instancePS;
	if (selectedVS == nullptr || selectedPS == nullptr || m_instanceVB == nullptr) {
		return false;
	}

	TextureClass* normalMapTex = Get_Normal_Map_For_Diffuse_Texture(diffuseTexture);
	if (normalMapTex == nullptr) {
		return false;
	}

	// Make sure the category's stage-0 texture/material/shader state is already on the device
	// before we temporarily switch to the programmable rigid fallback.
	DX8Wrapper::Apply_Render_State_Changes();

	IDirect3DVertexBuffer9* savedVB0 = nullptr;
	UINT savedOffset0 = 0, savedStride0 = 0;
	dev->GetStreamSource(0, &savedVB0, &savedOffset0, &savedStride0);

	IDirect3DIndexBuffer9* savedIB = nullptr;
	dev->GetIndices(&savedIB);

	DWORD savedFVF = 0;
	dev->GetFVF(&savedFVF);

	IDirect3DVertexDeclaration9* savedDecl = nullptr;
	dev->GetVertexDeclaration(&savedDecl);

	IDirect3DVertexShader9* savedVS = nullptr;
	dev->GetVertexShader(&savedVS);

	IDirect3DPixelShader9* savedPS = nullptr;
	dev->GetPixelShader(&savedPS);

	BOOL savedSoftwareVP = dev->GetSoftwareVertexProcessing();
	if (savedSoftwareVP) {
		dev->SetSoftwareVertexProcessing(FALSE);
	}

	dev->SetVertexDeclaration(instanceDecl);
	dev->SetVertexShader(selectedVS);
	dev->SetPixelShader(selectedPS);

	dev->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 1);
	dev->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1);

	if (savedVB0) {
		dev->SetStreamSource(0, savedVB0, savedOffset0, savedStride0);
	}
	dev->SetStreamSource(1, m_instanceVB, 0, sizeof(InstanceData));

	D3DMATRIX identityMat;
	memset(&identityMat, 0, sizeof(identityMat));
	identityMat._11 = identityMat._22 = identityMat._33 = identityMat._44 = 1.0f;
	dev->SetTransform(D3DTS_WORLD, &identityMat);

	D3DXMATRIX dxView;
	Upload_Rigid_View_Projection(dev, &dxView);

	RigidShaderLightingConstants lightingConstants;
	Build_Rigid_Shader_Lighting_Constants(dev, geometryFVF, lightEnv, material, dxView, &lightingConstants);
	Upload_Rigid_Shader_VS_Lighting_Constants(dev, lightingConstants);

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
		HRESULT hrFill = m_instanceVB->Lock(0, sizeof(InstanceData), &pData, D3DLOCK_DISCARD);
		if (FAILED(hrFill)) {
			WWDEBUG_SAY(("DX8InstanceManager: Single rigid instance VB lock failed: 0x%08X", hrFill));
			normalMapTex->Release_Ref();
			return false;
		}
		memcpy(pData, &inst, sizeof(inst));
		m_instanceVB->Unlock();
	}

	{
		const bool cloudEnabled =
			(TheGlobalData != nullptr) &&
			TheGlobalData->m_useCloudMap &&
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

		const float psC2[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
		dev->SetPixelShaderConstantF(2, psC2, 1);

		Upload_Rigid_Shader_PS_Lighting_Constants(dev, lightingConstants);

		IDirect3DBaseTexture9* d3dNormal = normalMapTex->Peek_D3D_Texture();
		dev->SetTexture(2, d3dNormal);
		dev->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		dev->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
	}

	renderer->Render_Instanced(0);

	dev->SetStreamSourceFreq(0, 1);
	dev->SetStreamSourceFreq(1, 1);
	dev->SetStreamSource(1, nullptr, 0, 0);

	if (savedDecl) {
		dev->SetVertexDeclaration(savedDecl);
	}
	else if (savedFVF != 0) {
		dev->SetFVF(savedFVF);
	}
	else if (DX8Wrapper::Get_Current_FVF() != 0) {
		dev->SetFVF(DX8Wrapper::Get_Current_FVF());
	}

	dev->SetVertexShader(savedVS);
	dev->SetPixelShader(savedPS);

	if (savedSoftwareVP) {
		dev->SetSoftwareVertexProcessing(savedSoftwareVP);
	}

	if (savedVB0) {
		dev->SetStreamSource(0, savedVB0, savedOffset0, savedStride0);
	}
	if (savedIB) {
		dev->SetIndices(savedIB);
	}

	if (savedVB0) savedVB0->Release();
	if (savedIB) savedIB->Release();
	if (savedDecl) savedDecl->Release();
	if (savedVS) savedVS->Release();
	if (savedPS) savedPS->Release();

	dev->SetTexture(1, nullptr);
	dev->SetTexture(2, nullptr);

	normalMapTex->Release_Ref();

	ShaderClass::Invalidate();
	DX8Wrapper::Invalidate_Vertex_Buffer_State();

	return true;
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
	WWDEBUG_SAY(("DX8InstanceManager: Hardware instancing initialized (max %d instances)", MAX_INSTANCES_PER_DRAW));
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

	HRESULT hr = dev->CreateVertexBuffer(
		MAX_INSTANCES_PER_DRAW * sizeof(InstanceData),
		D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
		0,
		D3DPOOL_DEFAULT,
		&m_instanceVB,
		nullptr);

	if (FAILED(hr)) {
		WWDEBUG_SAY(("CreateVertexBuffer for instance data failed: 0x%08X", hr));
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

	// Cache miss — build a new combined declaration
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

	// Texture coordinates — expose ONLY geometry TEXCOORD0 on stream 0.
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
	// TEXCOORD3 — matching the compiled RigidInstance.vso's hardcoded input semantics.
	// The previous approach used nextTexIdx = texCount, which only matched the shader when
	// texCount == 1. For texCount == 0 or texCount >= 2, the declaration/shader semantic
	// indices diverged. NVIDIA tolerates this silently; AMD strictly validates and delivers
	// zeroes or skips the draw entirely when the VS input signature doesn't match the decl.
	//
	// Using fixed indices 1/2/3 means there IS a gap when texCount == 0 (TEXCOORD0 missing
	// from stream 0), but this is acceptable because:
	//   a) Meshes with 0 UV channels are extremely rare in instancing-eligible rigid meshes
	//   b) The gap is between streams (stream 0 has no TEXCOORD, stream 1 starts at TEXCOORD1)
	//      which AMD handles correctly — the gap rejection only applies within a single stream
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

void DX8InstanceManagerClass::Draw_Instanced(
	DX8PolygonRendererClass* renderer,
	DWORD geometryFVF,
	LightEnvironmentClass* lightEnv,
	VertexMaterialClass* material,
	TextureClass* diffuseTexture)
{
	if (m_collectedCount < 2 || !renderer) return;

	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();
	if (!dev) return;

	// Ronin @bugfix 18/02/2026 DX9: Get the correct vertex declaration for this mesh's FVF
	IDirect3DVertexDeclaration9* instanceDecl = Get_Or_Create_Instance_Decl(geometryFVF);
	if (!instanceDecl) {
		WWDEBUG_SAY(("DX8InstanceManager: No decl for FVF 0x%08X, falling back to non-instanced", geometryFVF));
		return;
	}

	// Ronin @bugfix 07/03/2026 DX9: Select the shader variant that matches the
	// geometry FVF. AMD strictly validates VS input semantics against the vertex
	// declaration; meshes without D3DFVF_DIFFUSE must not use the COLOR0 variant.
	const bool hasVertexColor = (geometryFVF & D3DFVF_DIFFUSE) != 0;
	IDirect3DVertexShader9* selectedVS = hasVertexColor ? m_instanceVS : m_instanceVSNoColor;
	if (!selectedVS) {
		WWDEBUG_SAY(("DX8InstanceManager: Missing instancing shader variant for FVF 0x%08X (hasColor=%d)",
			geometryFVF, hasVertexColor ? 1 : 0));
		return;
	}

	// Ronin @feature 08/03/2026 DX9: Bind a small programmable pixel shader for the
	// instanced path instead of relying on fixed-function pixel combiners.
	IDirect3DPixelShader9* selectedPS = m_instancePS;
	if (!selectedPS) {
		WWDEBUG_SAY(("DX8InstanceManager: Missing instancing pixel shader"));
		return;
	}

		// 1. Lock and fill the instance VB with collected transforms + per-instance lighting
	void* pData = nullptr;
	HRESULT hr = m_instanceVB->Lock(0, m_collectedCount * sizeof(InstanceData), &pData, D3DLOCK_DISCARD);
	if (FAILED(hr)) {
		WWDEBUG_SAY(("Instance VB Lock failed: 0x%08X", hr));
		return;
	}

	InstanceData* dst = (InstanceData*)pData;
	bool mixedLighting = false;
	const size_t lightingPayloadSize =
		sizeof(dst[0].ambient) +
		sizeof(dst[0].lightDir0) +
		sizeof(dst[0].lightDiffuse0) +
		sizeof(dst[0].lightDir1) +
		sizeof(dst[0].lightDiffuse1) +
		sizeof(dst[0].lightDir2) +
		sizeof(dst[0].lightDiffuse2) +
		sizeof(dst[0].lightDir3) +
		sizeof(dst[0].lightDiffuse3);

	for (unsigned i = 0; i < m_collectedCount; ++i) {
		dst[i] = m_instanceBuffer[i];
		Extract_Instance_Lighting(m_collectedLightEnv[i], dst[i]);

		if (i > 0 && !mixedLighting) {
			if (memcmp(dst[0].ambient, dst[i].ambient, lightingPayloadSize) != 0) {
				mixedLighting = true;
			}
		}
	}

	m_instanceVB->Unlock();

	// 2. Snapshot only the raw D3D state this function actually mutates.
	IDirect3DVertexBuffer9* savedVB0 = nullptr;
	UINT savedOffset0 = 0, savedStride0 = 0;
	dev->GetStreamSource(0, &savedVB0, &savedOffset0, &savedStride0);

	IDirect3DIndexBuffer9* savedIB = nullptr;
	dev->GetIndices(&savedIB);

	DWORD savedFVF = 0;
	dev->GetFVF(&savedFVF);

	IDirect3DVertexDeclaration9* savedDecl = nullptr;
	dev->GetVertexDeclaration(&savedDecl);

	IDirect3DVertexShader9* savedVS = nullptr;
	dev->GetVertexShader(&savedVS);

	IDirect3DPixelShader9* savedPS = nullptr;
	dev->GetPixelShader(&savedPS);

	// Ronin @bugfix 01/03/2026 DX9: On MIXED_VERTEXPROCESSING devices, AMD requires explicit
	// hardware vertex processing mode before using VS 3.0 with stream frequency instancing.
	BOOL savedSoftwareVP = dev->GetSoftwareVertexProcessing();
	if (savedSoftwareVP) {
		dev->SetSoftwareVertexProcessing(FALSE);
	}

	// Ronin @bugfix 28/02/2026 DX9: AMD driver requires specific call ordering:
	// 1) SetVertexDeclaration  2) SetVertexShader  3) SetStreamSourceFreq
	// 4) SetStreamSource  5) Draw
	// Setting declaration BEFORE stream frequency is critical on AMD.

	// 3. Set the instancing programmable pipeline.
	dev->SetVertexDeclaration(instanceDecl);
	dev->SetVertexShader(selectedVS);
	dev->SetPixelShader(selectedPS);

	// 4. Set stream frequency AFTER declaration (AMD requirement)
	dev->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | m_collectedCount);
	dev->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1);

	// 5. Ronin @bugfix 01/03/2026 DX9: Re-bind BOTH stream sources AFTER setting frequency.
	// AMD invalidates stream source bindings when SetStreamSourceFreq is called.
	// NVIDIA preserves them, which is why it works there without re-binding.
	if (savedVB0) {
		dev->SetStreamSource(0, savedVB0, savedOffset0, savedStride0);
	}
	dev->SetStreamSource(1, m_instanceVB, 0, sizeof(InstanceData));


	// 6. Set world transform to identity (transforms are per-instance in the shader)
	D3DMATRIX identityMat;
	memset(&identityMat, 0, sizeof(identityMat));
	identityMat._11 = identityMat._22 = identityMat._33 = identityMat._44 = 1.0f;
	dev->SetTransform(D3DTS_WORLD, &identityMat);

	// 7. Upload ViewProjection matrix into c0..c3
	D3DXMATRIX dxView;
	Upload_Rigid_View_Projection(dev, &dxView);

	// 8. Upload lighting state into VS constants c4..c13
	RigidShaderLightingConstants lightingConstants;
	Build_Rigid_Shader_Lighting_Constants(dev, geometryFVF, lightEnv, material, dxView, &lightingConstants);
	Upload_Rigid_Shader_VS_Lighting_Constants(dev, lightingConstants);

	TextureClass* normalMapTex = nullptr;	

	// Ronin @feature 17/05/2026 DX9: project the terrain cloud field onto rigid
	// instanced meshes. The instancing path owns sampler 1 explicitly; we do NOT
	// rely on the fixed-function rigid path having left anything on stage 1.
	// Ronin @feature 23/05/2026 DX9 R2: also resolve and bind an optional per-diffuse
	// normal map on sampler 2 and push the PS constants the rigid PS needs to evaluate
	// a screen-space TBN Lambert delta on top of the existing Gouraud lighting.
	{
		const bool cloudEnabled =
			(TheGlobalData != nullptr) &&
			TheGlobalData->m_useCloudMap &&
			(geometryFVF & D3DFVF_DIFFUSE) == 0;

		TextureClass* rigidCloudTex = cloudEnabled ? Get_Valid_Rigid_Cloud_Texture() : nullptr;

		const bool cloudActive = cloudEnabled && (rigidCloudTex != nullptr);

		float cloudScale = 0.0f, cloudOffsetX = 0.0f, cloudOffsetY = 0.0f;
		if (cloudActive) {
			W3DShaderManager::getCloudMapState(&cloudScale, &cloudOffsetX, &cloudOffsetY);
		}

		const float c14[4] = { cloudActive ? 1.0f : 0.0f, cloudScale, cloudOffsetX, cloudOffsetY };
		dev->SetVertexShaderConstantF(14, c14, 1);

		// PS c0: cloud enable
		const float psC0[4] = { cloudActive ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
		dev->SetPixelShaderConstantF(0, psC0, 1);

		// PS c1: debug mode (kept for parity with existing PS)
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

		// Ronin @feature 23/05/2026 DX9 R2: resolve <diffuse>_NRM and bind on sampler 2.
		normalMapTex = Get_Normal_Map_For_Diffuse_Texture(diffuseTexture);
		const bool normalMapActive = (normalMapTex != nullptr);

		// PS c2: normal-map params (enable, intensity, reserved, reserved).
		// Intensity 1.0f matches the sampled normal exactly; lower values soften the perturbation.
		const float psC2[4] = { normalMapActive ? 1.0f : 0.0f, 1.0f, 0.0f, 0.0f };
		dev->SetPixelShaderConstantF(2, psC2, 1);

		// PS c3..c6: forward the same two lights the VS used, so the PS can evaluate
		// a per-pixel Lambert delta against the perturbed normal without re-querying state.
		// Mirrors the values pushed into VS c5/c6/c11/c12 above.
		//const float* vsLightDir0 = nullptr;
		//const float* vsLightDiff0 = nullptr;
		//const float* vsLightDir1 = nullptr;
		//const float* vsLightDiff1 = nullptr;

		// We already wrote these into VS constants; re-emit them as PS constants.
		// Simpler than re-querying lightEnv with two code paths.
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

	// 9. Issue the instanced draw call
	renderer->Render_Instanced(0);

	// 10. Restore stream frequency to non-instanced defaults
	dev->SetStreamSourceFreq(0, 1);
	dev->SetStreamSourceFreq(1, 1);

	// 11. Unbind stream 1
	dev->SetStreamSource(1, nullptr, 0, 0);

	// 12. Restore raw D3D state touched by instancing
	if (savedDecl) {
		dev->SetVertexDeclaration(savedDecl);
	}
	else if (savedFVF != 0) {
		dev->SetFVF(savedFVF);
	}
	else if (DX8Wrapper::Get_Current_FVF() != 0) {
		dev->SetFVF(DX8Wrapper::Get_Current_FVF());
	}

	dev->SetVertexShader(savedVS);
	dev->SetPixelShader(savedPS);

	// Ronin @bugfix 01/03/2026 DX9: Restore software vertex processing mode if it was active.
	if (savedSoftwareVP) {
		dev->SetSoftwareVertexProcessing(savedSoftwareVP);
	}

	if (savedVB0) {
		dev->SetStreamSource(0, savedVB0, savedOffset0, savedStride0);
	}
	if (savedIB) {
		dev->SetIndices(savedIB);
	}

	// Release the Get* refs
	if (savedVB0) savedVB0->Release();
	if (savedIB) savedIB->Release();
	if (savedDecl) savedDecl->Release();
	if (savedVS) savedVS->Release();
	if (savedPS) savedPS->Release();

	// Ronin @feature 16/05/2026 DX9: release our cloud binding so subsequent
	// fixed-function draws don't accidentally sample it.
	dev->SetTexture(1, nullptr);
	// Ronin @feature 23/05/2026 DX9 R2: release rigid normal-map binding for the same reason.
	dev->SetTexture(2, nullptr);

	if (normalMapTex != nullptr) {
		normalMapTex->Release_Ref();
	}

	// 13. Tell ShaderClass to re-apply its cached state on the next draw.
	ShaderClass::Invalidate();

	// 14. Ronin @bugfix 19/02/2026 DX9: Dirty the change flags so Apply_Render_State_Changes
	// re-validates. The container's VB is still valid and still bound on the device.
	DX8Wrapper::Invalidate_Vertex_Buffer_State();

	// Statistics
	m_instancedDrawCalls++;
	m_instancedMeshes += m_collectedCount;

	if (mixedLighting) {
		m_instancedMixedLightDrawCalls++;
		m_instancedMixedLightMeshes += m_collectedCount;
	}
}
// ----------------------------------------------------------------------------

void DX8InstanceManagerClass::Begin_Frame_Statistics()
{
	m_instancedDrawCalls = 0;
	m_instancedMeshes = 0;
	m_instancedMixedLightDrawCalls = 0;
	m_instancedMixedLightMeshes = 0;
}

void DX8InstanceManagerClass::End_Frame_Statistics()
{
	m_lastFrameInstancedDrawCalls = m_instancedDrawCalls;
	m_lastFrameInstancedMeshes = m_instancedMeshes;
	m_lastFrameInstancedMixedLightDrawCalls = m_instancedMixedLightDrawCalls;
	m_lastFrameInstancedMixedLightMeshes = m_instancedMixedLightMeshes;
}
