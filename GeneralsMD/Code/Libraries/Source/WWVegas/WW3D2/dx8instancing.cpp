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

// Global instance
DX8InstanceManagerClass TheDX8InstanceManager;

// ----------------------------------------------------------------------------

DX8InstanceManagerClass::DX8InstanceManagerClass()
	: m_available(false)
	, m_enabled(true)
	, m_instanceVB(nullptr)
	, m_instanceVS(nullptr)
	, m_declCacheCount(0)
	, m_collectedCount(0)
	, m_instancedDrawCalls(0)
	, m_instancedMeshes(0)
	, m_lastFrameInstancedDrawCalls(0)
	, m_lastFrameInstancedMeshes(0)
{
	memset(m_declCache, 0, sizeof(m_declCache));
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
		WWDEBUG_SAY(("DX8InstanceManager: Failed to load instancing vertex shader"));
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

void DX8InstanceManagerClass::Release_Resources()
{
	if (m_instanceVB) { m_instanceVB->Release(); m_instanceVB = nullptr; }
	if (m_instanceVS) { m_instanceVS->Release(); m_instanceVS = nullptr; }

	// Ronin @bugfix 18/02/2026 DX9: Release all cached vertex declarations
	for (unsigned i = 0; i < m_declCacheCount; ++i) {
		if (m_declCache[i].decl) {
			m_declCache[i].decl->Release();
			m_declCache[i].decl = nullptr;
		}
	}
	m_declCacheCount = 0;
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

	// Texture coordinates
	int texCount = (geometryFVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	for (int t = 0; t < texCount; ++t) {
		elements[idx++] = { 0, offset, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, (BYTE)t };
		offset += 8;
	}

	// Stream 1: Per-instance world transform (3 rows of float4) at TEXCOORD4..6
	elements[idx++] = { 1,  0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 4 };
	elements[idx++] = { 1, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 5 };
	elements[idx++] = { 1, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 6 };

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
		WWDEBUG_SAY(("DX8InstanceManager: Decl cache full! FVF 0x%08X not cached.", geometryFVF));
	}

	WWDEBUG_SAY(("DX8InstanceManager: Created instancing decl for FVF 0x%08X (stream0 stride=%u, %d tex coords)",
		geometryFVF, (unsigned)offset, texCount));

	return newDecl;
}

// ----------------------------------------------------------------------------

bool DX8InstanceManagerClass::Load_Instance_Shader()
{
	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();

	const char* shaderPath = "shaders\\RigidInstance.vso";

	HANDLE hFile = CreateFileA(shaderPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) {
		WWDEBUG_SAY(("DX8InstanceManager: Could not open %s (error %d)", shaderPath, GetLastError()));
		return false;
	}

	DWORD fileSize = GetFileSize(hFile, nullptr);
	if (fileSize == 0 || fileSize == INVALID_FILE_SIZE) {
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

	HRESULT hr = dev->CreateVertexShader(shaderBytecode, &m_instanceVS);
	HeapFree(GetProcessHeap(), 0, shaderBytecode);

	if (FAILED(hr)) {
		WWDEBUG_SAY(("DX8InstanceManager: CreateVertexShader failed: 0x%08X", hr));
		return false;
	}

	WWDEBUG_SAY(("DX8InstanceManager: Loaded %s (%lu bytes), VS=%p", shaderPath, fileSize, m_instanceVS));
	return true;
}

// ----------------------------------------------------------------------------

unsigned DX8InstanceManagerClass::Collect_Instances(
	PolyRenderTaskClass* render_task_head,
	DX8PolygonRendererClass* first_renderer)
{
	m_collectedCount = 0;

	if (!Is_Enabled()) return 0;
	if (!render_task_head || !first_renderer) return 0;

	return m_collectedCount;
}

// ----------------------------------------------------------------------------

void DX8InstanceManagerClass::Draw_Instanced(DX8PolygonRendererClass* renderer, DWORD geometryFVF)
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

	// 1. Lock and fill the instance VB with collected transforms
	void* pData = nullptr;
	HRESULT hr = m_instanceVB->Lock(0, m_collectedCount * sizeof(InstanceData), &pData, D3DLOCK_DISCARD);
	if (FAILED(hr)) {
		WWDEBUG_SAY(("Instance VB Lock failed: 0x%08X", hr));
		return;
	}
	memcpy(pData, m_instanceBuffer, m_collectedCount * sizeof(InstanceData));
	m_instanceVB->Unlock();

	// 2. Ronin @bugfix 19/02/2026 DX9: Snapshot the current stream 0 binding BEFORE we
	// modify anything, so we can restore it precisely after the instanced draw.
	IDirect3DVertexBuffer9* savedVB0 = nullptr;
	UINT savedOffset0 = 0, savedStride0 = 0;
	dev->GetStreamSource(0, &savedVB0, &savedOffset0, &savedStride0);

	IDirect3DIndexBuffer9* savedIB = nullptr;
	dev->GetIndices(&savedIB);

	DWORD savedFVF = 0;
	dev->GetFVF(&savedFVF);

	// 3. Set the instancing vertex declaration and shader
	dev->SetVertexDeclaration(instanceDecl);
	dev->SetVertexShader(m_instanceVS);

	// 4. Bind stream 1 with the instance data
	dev->SetStreamSource(1, m_instanceVB, 0, sizeof(InstanceData));

	// 5. Set stream frequency: stream 0 = indexed geometry, stream 1 = per-instance
	dev->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | m_collectedCount);
	dev->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1);

	// 6. Set world transform to identity (transforms are per-instance in the shader)
	D3DMATRIX identityMat;
	memset(&identityMat, 0, sizeof(identityMat));
	identityMat._11 = identityMat._22 = identityMat._33 = identityMat._44 = 1.0f;
	dev->SetTransform(D3DTS_WORLD, &identityMat);

	// 7. Upload ViewProjection matrix into c0..c3
	D3DMATRIX viewMat, projMat;
	dev->GetTransform(D3DTS_VIEW, &viewMat);
	dev->GetTransform(D3DTS_PROJECTION, &projMat);

	D3DXMATRIX dxView(viewMat), dxProj(projMat), dxViewProj;
	D3DXMatrixMultiply(&dxViewProj, &dxView, &dxProj);

	// Ronin @bugfix 20/02/2026 DX9: Transpose ViewProj for HLSL mul(vector, matrix) convention.
	D3DXMATRIX dxViewProjT;
	D3DXMatrixTranspose(&dxViewProjT, &dxViewProj);
	dev->SetVertexShaderConstantF(0, (const float*)&dxViewProjT, 4);

	// 8. Upload lighting state into VS constants c4..c13
	// Ronin @bugfix 22/02/2026 DX9: Use LightEnvironmentClass for full-precision ambient and
	// world-space light directions. InputLights[].Direction points TOWARD the light source;
	// the shader computes dot(N, lightDir) without negation.
	{
		float ambientR = 0.0f, ambientG = 0.0f, ambientB = 0.0f;

		LightEnvironmentClass* lightEnv = DX8Wrapper::Get_Light_Environment();
		if (lightEnv) {
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
		float c4[4] = { ambientR, ambientG, ambientB, 0.0f };
		dev->SetVertexShaderConstantF(4, c4, 1);

		float c5[4] = { 0, 0, 0, 0 };
		float c6[4] = { 0, 0, 0, 0 };
		float c11[4] = { 0, 0, 0, 0 };
		float c12[4] = { 0, 0, 0, 0 };
		float numLights = 0.0f;

		if (lightEnv) {
			int envLightCount = lightEnv->Get_Light_Count();
			for (int li = 0; li < envLightCount && numLights < 2.0f; ++li) {
				const Vector3& worldDir = lightEnv->Get_Light_Direction(li);
				const Vector3& diffuse = lightEnv->Get_Light_Diffuse(li);

				float* dirOut = (numLights < 1.0f) ? c5 : c11;
				float* diffOut = (numLights < 1.0f) ? c6 : c12;

				dirOut[0] = worldDir.X;
				dirOut[1] = worldDir.Y;
				dirOut[2] = worldDir.Z;

				diffOut[0] = diffuse.X;
				diffOut[1] = diffuse.Y;
				diffOut[2] = diffuse.Z;

				numLights += 1.0f;
			}
		}
		else {
			// Fallback: read from D3D device and inverse-transform from camera to world space
			D3DXMATRIX dxViewInv;
			D3DXMatrixInverse(&dxViewInv, nullptr, &dxView);

			for (int li = 0; li < 2; ++li) {
				BOOL lightEnabled = FALSE;
				dev->GetLightEnable(li, &lightEnabled);
				if (!lightEnabled) continue;

				D3DLIGHT9 light;
				memset(&light, 0, sizeof(light));
				dev->GetLight(li, &light);

				D3DXVECTOR3 camDir(light.Direction.x, light.Direction.y, light.Direction.z);
				D3DXVECTOR3 worldDir;
				D3DXVec3TransformNormal(&worldDir, &camDir, &dxViewInv);
				D3DXVec3Normalize(&worldDir, &worldDir);

				float* dirOut = (numLights < 1.0f) ? c5 : c11;
				float* diffOut = (numLights < 1.0f) ? c6 : c12;

				// Negate: D3D device stores toward-surface, shader wants toward-light
				dirOut[0] = -worldDir.x;
				dirOut[1] = -worldDir.y;
				dirOut[2] = -worldDir.z;

				diffOut[0] = light.Diffuse.r;
				diffOut[1] = light.Diffuse.g;
				diffOut[2] = light.Diffuse.b;

				numLights += 1.0f;
			}
		}

		dev->SetVertexShaderConstantF(5, c5, 1);
		dev->SetVertexShaderConstantF(6, c6, 1);
		dev->SetVertexShaderConstantF(11, c11, 1);
		dev->SetVertexShaderConstantF(12, c12, 1);

		D3DMATERIAL9 mat;
		dev->GetMaterial(&mat);
		float c7[4] = { mat.Diffuse.r, mat.Diffuse.g, mat.Diffuse.b, mat.Diffuse.a };
		float c8[4] = { mat.Emissive.r, mat.Emissive.g, mat.Emissive.b, 0.0f };
		float c10[4] = { mat.Ambient.r, mat.Ambient.g, mat.Ambient.b, 0.0f };
		dev->SetVertexShaderConstantF(7, c7, 1);
		dev->SetVertexShaderConstantF(8, c8, 1);
		dev->SetVertexShaderConstantF(10, c10, 1);

		DWORD lightingRS = FALSE;
		dev->GetRenderState(D3DRS_LIGHTING, &lightingRS);
		float hasVertexColor = (geometryFVF & D3DFVF_DIFFUSE) ? 1.0f : 0.0f;
		float c9[4] = { lightingRS ? 1.0f : 0.0f, hasVertexColor, numLights, 0.0f };
		dev->SetVertexShaderConstantF(9, c9, 1);

		// Ronin @bugfix 21/02/2026 DX9: Upload material source flags to match FFP behavior.
		DWORD diffuseSrc = D3DMCS_MATERIAL, ambientSrc = D3DMCS_MATERIAL, emissiveSrc = D3DMCS_MATERIAL;
		dev->GetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, &diffuseSrc);
		dev->GetRenderState(D3DRS_AMBIENTMATERIALSOURCE, &ambientSrc);
		dev->GetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, &emissiveSrc);
		float c13[4] = {
			(diffuseSrc == D3DMCS_COLOR1 || diffuseSrc == D3DMCS_COLOR2) ? 1.0f : 0.0f,
			(ambientSrc == D3DMCS_COLOR1 || ambientSrc == D3DMCS_COLOR2) ? 1.0f : 0.0f,
			(emissiveSrc == D3DMCS_COLOR1 || emissiveSrc == D3DMCS_COLOR2) ? 1.0f : 0.0f,
			0.0f
		};
		dev->SetVertexShaderConstantF(13, c13, 1);
	}

	// Ronin @bugfix 22/02/2026 DX9: Disable stage 1 to prevent stale multi-texture ops
	// from leaking into the instanced draw (instancing only uses stage 0).
	dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

	// 9. Issue the instanced draw call
	renderer->Render_Instanced(0);

	// 10. Restore stream frequency to non-instanced defaults
	dev->SetStreamSourceFreq(0, 1);
	dev->SetStreamSourceFreq(1, 1);

	// 11. Unbind stream 1
	dev->SetStreamSource(1, nullptr, 0, 0);

	// 12. Restore fixed-function pipeline
	dev->SetVertexShader(nullptr);
	dev->SetVertexDeclaration(nullptr);

	// 13. Ronin @bugfix 19/02/2026 DX9: Restore the EXACT D3D device state that was
	// active before instancing, using the saved raw D3D pointers.
	if (savedVB0) {
		dev->SetStreamSource(0, savedVB0, savedOffset0, savedStride0);
	}
	if (savedIB) {
		dev->SetIndices(savedIB);
	}
	if (savedFVF != 0) {
		dev->SetFVF(savedFVF);
	}
	else if (DX8Wrapper::Get_Current_FVF() != 0) {
		dev->SetFVF(DX8Wrapper::Get_Current_FVF());
	}

	// Release the GetStreamSource/GetIndices refs
	if (savedVB0) savedVB0->Release();
	if (savedIB) savedIB->Release();

	// 14. Tell ShaderClass to re-apply its cached state on the next draw.
	ShaderClass::Invalidate();

	// 15. Ronin @bugfix 19/02/2026 DX9: Dirty the change flags so Apply_Render_State_Changes
	// re-validates. The container's VB is still valid and still bound on the device.
	DX8Wrapper::Invalidate_Vertex_Buffer_State();

	// Statistics
	m_instancedDrawCalls++;
	m_instancedMeshes += m_collectedCount;
}

// ----------------------------------------------------------------------------

void DX8InstanceManagerClass::Begin_Frame_Statistics()
{
	m_instancedDrawCalls = 0;
	m_instancedMeshes = 0;
}

void DX8InstanceManagerClass::End_Frame_Statistics()
{
	m_lastFrameInstancedDrawCalls = m_instancedDrawCalls;
	m_lastFrameInstancedMeshes = m_instancedMeshes;
}
