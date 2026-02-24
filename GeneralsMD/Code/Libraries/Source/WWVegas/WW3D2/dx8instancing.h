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

#pragma once

#include <d3d9.h>
#include "always.h"
#include "wwdebug.h"

class MeshClass;
class DX8PolygonRendererClass;
class PolyRenderTaskClass;

/**
** DX8InstanceManagerClass
**
** Manages a stream1 instance buffer containing per-instance world transforms.
** Used by DX8TextureCategoryClass::Render() to batch identical rigid meshes
** via DrawIndexedPrimitive with stream-frequency instancing (DX9 SM3.0+).
**
** Requirements for instancing eligibility:
**  - Device supports stream-frequency instancing (SM3.0)
**  - Mesh is NOT a skin (skins use dynamic VBs each frame)
**  - Mesh is NOT sorted (sorting requires per-polygon z-ordering)
**  - Mesh is NOT billboard-aligned or camera-oriented
**  - All instances share the same polygon renderer (same index range)
**  - Mesh has no alpha override or material override
**  - Object scale is 1.0 (avoids normalize-normals state thrash)
**  - At least 2 instances (instancing overhead not worth it for 1)
*/
class DX8InstanceManagerClass
{
public:

	// Maximum instances per single instanced draw call.
	// Capped to keep the instance VB small and avoid huge batches that stall the GPU.
	enum { MAX_INSTANCES_PER_DRAW = 256 };

	// Ronin @bugfix 18/02/2026 DX9: Maximum cached vertex declarations for different FVFs.
	enum { MAX_CACHED_DECLS = 16 };

	DX8InstanceManagerClass();
	~DX8InstanceManagerClass();

	/**
	** One-time initialization. Call after device creation.
	** Creates the instance vertex buffer on stream 1 and loads the instancing vertex shader.
	** Returns true if hardware instancing is available and initialized successfully.
	*/
	bool Init();

	/**
	** Shutdown and release all D3D resources.
	*/
	void Shutdown();

	/**
	** Returns true if hardware instancing is supported and initialized.
	*/
	bool Is_Available() const { return m_available; }

	/**
	** Enable or disable instancing at runtime (e.g., from a settings menu).
	*/
	void Set_Enabled(bool enabled) { m_enabled = enabled; }
	bool Is_Enabled() const { return m_enabled && m_available; }

	/**
	** Collects eligible instances from a render task list.
	** Returns the number of instances collected. If >= 2, caller should
	** use Draw_Instanced() instead of the per-mesh loop.
	**
	** @param render_task_head  Head of the PolyRenderTaskClass linked list
	** @param first_renderer    The polygon renderer to match (all instances must share this)
	** @return Number of instances collected into the internal buffer
	*/
	unsigned Collect_Instances(
		PolyRenderTaskClass * render_task_head,
		DX8PolygonRendererClass * first_renderer);

	/**
	** Issue the instanced draw call for the previously collected instances.
	** Caller is responsible for having already set textures, shader, material,
	** and the geometry vertex buffer on stream 0.
	**
	** @param renderer      The polygon renderer that defines the index range
	** @param geometryFVF   The FVF of the stream 0 vertex buffer (used to build the combined declaration)
	*/
	void Draw_Instanced(DX8PolygonRendererClass * renderer, DWORD geometryFVF);

	/**
	** Reset the collection buffer for a new batch of instances.
	*/
	void Reset_Collection() { m_collectedCount = 0; }

	/**
	** Add a single instance world transform (Matrix3D rows) to the collection buffer.
	** Returns true if the instance was added, false if the buffer is full.
	**
	** Matrix3D stores rows as Vector4: Row[i] = (m[i][0], m[i][1], m[i][2], m[i][3])
	** which matches the InstanceData layout expected by the HLSL shader.
	*/
	bool Add_Instance_Transform(const float row0[4], const float row1[4], const float row2[4])
	{
		if (m_collectedCount >= MAX_INSTANCES_PER_DRAW) return false;
		InstanceData & inst = m_instanceBuffer[m_collectedCount];
		inst.row0[0] = row0[0]; inst.row0[1] = row0[1]; inst.row0[2] = row0[2]; inst.row0[3] = row0[3];
		inst.row1[0] = row1[0]; inst.row1[1] = row1[1]; inst.row1[2] = row1[2]; inst.row1[3] = row1[3];
		inst.row2[0] = row2[0]; inst.row2[1] = row2[1]; inst.row2[2] = row2[2]; inst.row2[3] = row2[3];
		m_collectedCount++;
		return true;
	}

	/**
	** Returns the number of instances currently collected.
	*/
	unsigned Get_Collected_Count() const { return m_collectedCount; }

	// Statistics for the draw call HUD
	unsigned Get_Last_Frame_Instanced_Draw_Calls() const { return m_lastFrameInstancedDrawCalls; }
	unsigned Get_Last_Frame_Instanced_Meshes() const { return m_lastFrameInstancedMeshes; }
	void Begin_Frame_Statistics();
	void End_Frame_Statistics();

private:

	/**
	** Per-instance data written to stream 1.
	** This is a 4x3 world transform matrix stored as 3 float4 rows.
	** Matches the HLSL shader's expected TEXCOORD1..TEXCOORD3 input.
	*/
	struct InstanceData
	{
		float row0[4]; // world matrix row 0 (m00, m01, m02, m03)
		float row1[4]; // world matrix row 1 (m10, m11, m12, m13)
		float row2[4]; // world matrix row 2 (m20, m21, m22, m23)
	};

	static_assert(sizeof(InstanceData) == 48, "InstanceData must be 48 bytes (3x float4)");

	// Ronin @bugfix 18/02/2026 DX9: Cached vertex declaration entry keyed by FVF.
	struct CachedDecl
	{
		DWORD fvf;
		IDirect3DVertexDeclaration9 * decl;
	};

	bool m_available;        // Hardware supports instancing
	bool m_enabled;          // User has instancing enabled

	IDirect3DVertexBuffer9*       m_instanceVB;       // Stream 1 instance buffer
	IDirect3DVertexShader9*       m_instanceVS;       // Instancing vertex shader

	// Ronin @bugfix 18/02/2026 DX9: Per-FVF declaration cache (replaces single m_instanceDecl)
	CachedDecl m_declCache[MAX_CACHED_DECLS];
	unsigned   m_declCacheCount;

	// Per-frame collection buffer (CPU side, written to m_instanceVB before draw)
	InstanceData m_instanceBuffer[MAX_INSTANCES_PER_DRAW];
	unsigned     m_collectedCount;

	// Statistics
	unsigned m_instancedDrawCalls;
	unsigned m_instancedMeshes;
	unsigned m_lastFrameInstancedDrawCalls;
	unsigned m_lastFrameInstancedMeshes;

	// Internal helpers
	bool Create_Instance_VB();
	IDirect3DVertexDeclaration9 * Get_Or_Create_Instance_Decl(DWORD geometryFVF);
	bool Load_Instance_Shader();
	void Release_Resources();
};

/**
** Global instance manager, created/destroyed alongside TheDX8MeshRenderer.
*/
extern DX8InstanceManagerClass TheDX8InstanceManager;
