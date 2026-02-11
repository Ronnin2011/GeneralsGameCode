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

#ifndef SCOPED_2D_STATE_GUARD_H
#define SCOPED_2D_STATE_GUARD_H

#include <d3d9.h>
#include "matrix4.h"
#include "dx8wrapper.h"

// Ronin @bugfix 02/12/2025: RAII guard for 2D rendering state isolation
// Automatically saves and restores GPU pipeline state to prevent 2D rendering
// from polluting 3D rendering pipeline (especially vertex format leakage in DX9)
class Scoped2DStateGuard {
private:
	IDirect3DDevice9* m_dev;

	// INPUT ASSEMBLY STATE
	DWORD m_savedFVF;
	IDirect3DVertexDeclaration9* m_savedDecl;
	IDirect3DVertexBuffer9* m_savedVB0;
	UINT m_savedOffset0;
	UINT m_savedStride0;
	IDirect3DIndexBuffer9* m_savedIB;

	// TRANSFORM STATE
	Matrix4x4 m_savedWorld;
	Matrix4x4 m_savedView;
	Matrix4x4 m_savedProjection;

	// VIEWPORT STATE
	D3DVIEWPORT9 m_savedViewport;

	// 2D-SPECIFIC RENDER STATES
	DWORD m_savedZEnable;
	DWORD m_savedZWriteEnable;
	DWORD m_savedLighting;
	DWORD m_savedCullMode;

	const char* m_captureLocation;

#ifdef _DEBUG
	// Pipeline snapshot (device-side verification)
	PipelineStateSnapshot* m_snapshot = nullptr;

	// Throttle logging to once per 60 frames (~1 second at 60 FPS)
	static unsigned long s_frameCounter;
	static const unsigned long LOG_INTERVAL = 5;

	static bool ShouldLog() {
		s_frameCounter++;
		if (s_frameCounter >= LOG_INTERVAL) {
			s_frameCounter = 0;
			return true;
		}
		return false;
	}
#endif

public:
	Scoped2DStateGuard(IDirect3DDevice9* dev, const char* location)
		: m_dev(dev)
		, m_savedFVF(0)
		, m_savedDecl(nullptr)
		, m_savedVB0(nullptr)
		, m_savedOffset0(0)
		, m_savedStride0(0)
		, m_savedIB(nullptr)
		, m_savedZEnable(0)
		, m_savedZWriteEnable(0)
		, m_savedLighting(0)
		, m_savedCullMode(0)
		, m_captureLocation(location)
	{
		if (!m_dev) return;

#ifdef _DEBUG
		m_snapshot = DX8Wrapper::Capture_Pipeline_State(location);
#endif

		// SAVE INPUT ASSEMBLY STATE
		m_dev->GetFVF(&m_savedFVF);
		m_dev->GetVertexDeclaration(&m_savedDecl);
		m_dev->GetStreamSource(0, &m_savedVB0, &m_savedOffset0, &m_savedStride0);
		m_dev->GetIndices(&m_savedIB);

		// SAVE TRANSFORMS
		DX8Wrapper::Get_Transform(D3DTS_WORLD, m_savedWorld);
		DX8Wrapper::Get_Transform(D3DTS_VIEW, m_savedView);
		DX8Wrapper::Get_Transform(D3DTS_PROJECTION, m_savedProjection);

		// SAVE VIEWPORT
		m_dev->GetViewport(&m_savedViewport);

		// SAVE 2D-SPECIFIC RENDER STATES
		m_savedZEnable = DX8Wrapper::Get_DX8_Render_State(D3DRS_ZENABLE);
		m_savedZWriteEnable = DX8Wrapper::Get_DX8_Render_State(D3DRS_ZWRITEENABLE);
		m_savedLighting = DX8Wrapper::Get_DX8_Render_State(D3DRS_LIGHTING);
		m_savedCullMode = DX8Wrapper::Get_DX8_Render_State(D3DRS_CULLMODE);


		
		// Ronin @bugfix 16/12/2025: CLEAR TEXTURES ON ENTRY (don't save them!)
		// This ensures 2D rendering starts with a clean slate
		// Any 3D pollution (decal atlas, shadow map) is wiped before we render
		for (unsigned int stage = 0; stage < 8; stage++) {
			// Do NOT clear SetTexture(stage, nullptr) here.
			// Do disable fixed-function stage ops to prevent stale multi-texture blending.
			//m_dev->SetTexture(stage, nullptr);
			DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
			DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		}

		/*
		// Re-enable stage 0 for 2D rendering (will be configured by Render2D)
		//m_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		//m_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);

		// Ronin @bugfix 26/12/2025: Reset texture coordinate generation state
		// Critical: Water shader leaves D3DTSS_TCI_CAMERASPACEPOSITION on stage 2
		// which pollutes infantry rendering and causes black triangles
		for (unsigned int stage = 0; stage < 8; stage++) {
			m_dev->SetTextureStageState(stage, D3DTSS_TEXCOORDINDEX, stage); // Passthrough from vertex UV
			m_dev->SetTextureStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		}*/


#ifdef _DEBUG
		if (ShouldLog()) {
			WWDEBUG_SAY(("[2D GUARD] Cleared textures on entry at %s", location));
			WWDEBUG_SAY(("[2D GUARD] Saved state at %s:", location));
			WWDEBUG_SAY(("   FVF=0x%08X, Decl=%p, VB=%p, Stride=%u",
				m_savedFVF, m_savedDecl, m_savedVB0, m_savedStride0));
			WWDEBUG_SAY(("   ZEnable=%d, ZWrite=%d, Light=%d, Cull=%d",
				m_savedZEnable, m_savedZWriteEnable, m_savedLighting, m_savedCullMode));

		}
#endif
	}

	~Scoped2DStateGuard()
	{
		if (!m_dev) return;

		// CRITICAL: Clear wrapper-tracked IA to ensure next pass rebinds intended layout
		DX8Wrapper::Clear_Current_Decl();
		DX8Wrapper::Clear_Current_FVF();

		// Don't try to restore layout - just clear tracking and let next pass set what it needs

		// Restore stream0 exactly as captured (even if null)
		m_dev->SetStreamSource(0, m_savedVB0, m_savedOffset0, m_savedStride0);
		if (m_savedVB0) {
			m_savedVB0->Release();
			m_savedVB0 = nullptr;
		}

		// Always release the saved declaration reference
		if (m_savedDecl) {
			m_savedDecl->Release();
			m_savedDecl = nullptr;
		}

		// Always restore IB exactly as captured (even if null)
		m_dev->SetIndices(m_savedIB);
		if (m_savedIB) {
			m_savedIB->Release();
			m_savedIB = nullptr;
		}

		// ========= RESTORE TRANSFORMS =========
		DX8Wrapper::Set_Transform(D3DTS_PROJECTION, m_savedProjection);
		DX8Wrapper::Set_Transform(D3DTS_VIEW, m_savedView);
		DX8Wrapper::Set_Transform(D3DTS_WORLD, m_savedWorld);

		// ========= RESTORE VIEWPORT =========
		m_dev->SetViewport(&m_savedViewport);

		// ========= RESTORE 2D-SPECIFIC RENDER STATES =========
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZENABLE, m_savedZEnable);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE, m_savedZWriteEnable);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_LIGHTING, m_savedLighting);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_CULLMODE, m_savedCullMode);

#ifdef _DEBUG
		if (m_snapshot) {
			DX8Wrapper::Validate_Pipeline_State_Restored(m_snapshot, m_captureLocation);
			delete m_snapshot;
			m_snapshot = nullptr;
		}

		if (ShouldLog()) {
			WWDEBUG_SAY(("[2D GUARD] Restored and cleared wrapper state at %s", m_captureLocation));
		}
#endif
	}

	// Prevent copying (RAII guard must have unique ownership)
	Scoped2DStateGuard(const Scoped2DStateGuard&) = delete;
	Scoped2DStateGuard& operator=(const Scoped2DStateGuard&) = delete;
};

// Static member initialization
#ifdef _DEBUG
unsigned long Scoped2DStateGuard::s_frameCounter = 0;
#endif

#endif // SCOPED_2D_STATE_GUARD_H
