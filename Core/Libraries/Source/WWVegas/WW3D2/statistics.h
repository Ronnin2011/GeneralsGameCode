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

#pragma once

#include "WWLib/always.h"
//#include "wwstring.h"

class TextureClass;
class StringClass;
class ShaderClass;

namespace Debug_Statistics
{
	enum RecordTextureMode
	{
		RECORD_TEXTURE_NONE,
		RECORD_TEXTURE_SIMPLE,
		RECORD_TEXTURE_DETAILS
	};

	// Texture memory tracking system
	void Record_Texture_Mode(RecordTextureMode m);
	RecordTextureMode Get_Record_Texture_Mode();

	void Record_Texture(TextureClass* t);
	int Get_Record_Texture_Size();	// Return total size of textures used during latest frame, in bytes
	int Get_Record_Lightmap_Texture_Size();	// Return total size of lightmap textures used during latest frame, in bytes
	int Get_Record_Procedural_Texture_Size();	// Return total size of procedural textures used during latest frame, in bytes
	int Get_Record_Texture_Count();	// Return total number of textures used during latest frame
	int Get_Record_Texture_Change_Count();
	int Get_Record_Lightmap_Texture_Count();	// Return total number of lightmap textures used during latest frame
	int Get_Record_Procedural_Texture_Count();	// Return total number of procedutal textures used during latest frame
	const StringClass& Get_Record_Texture_String();

	void Record_DX8_Skin_Polys_And_Vertices(int pcount,int vcount);
	void Record_DX8_Polys_And_Vertices(int pcount,int vcount,const ShaderClass& shader);
	void Record_Sorting_Polys_And_Vertices(int pcount,int vcount);
	int Get_DX8_Polygons();
	int Get_DX8_Vertices();
	int Get_DX8_Skin_Renders();
	int Get_DX8_Skin_Polygons();
	int Get_DX8_Skin_Vertices();
	int Get_Sorting_Polygons();
	int Get_Sorting_Vertices();
	int Get_Draw_Calls();

	// Ronin @diagnostic 02/08/2026 DX9: per-subsystem draw attribution ([DRAW] HUD). Renderers tag
	// themselves with a DrawSubsystemScope; draws recorded under a live tag are attributed to it.
	// Rationale + measured results: Windowednew.md §18f-0.
	enum DrawSubsystem
	{
		DRAW_SUBSYS_OTHER = 0,   // untagged: UI, debug geometry, anything not claimed below
		DRAW_SUBSYS_TERRAIN,
		DRAW_SUBSYS_SHADOW,
		DRAW_SUBSYS_WATER,
		DRAW_SUBSYS_SHROUD,
		DRAW_SUBSYS_SORTED,       // sorted, blend mode not classified below
		DRAW_SUBSYS_SORTED_ADD,   // additive == order-INdependent -> batchable outside the depth sort
		DRAW_SUBSYS_SORTED_ALPHA, // alpha == order-dependent -> must stay depth-sorted
		DRAW_SUBSYS_SKIN,
		DRAW_SUBSYS_RIGID_PROG,   // programmable rigid path (bypasses DX8Wrapper, self-reported)
		DRAW_SUBSYS_COUNT
	};

	void Set_Draw_Subsystem(DrawSubsystem s);
	DrawSubsystem Get_Draw_Subsystem();

	// Monotonic. NOT reset by Begin/End_Statistics -- those are bracketed twice per frame and would
	// truncate. Take a delta between frames for per-frame numbers.
	unsigned Get_Total_Draw_Calls_By_Subsystem(int s);
	const char* Get_Draw_Subsystem_Name(int s);

	// Ronin @diagnostic 02/08/2026 DX9: Render_Instanced calls the device directly, so the rigid path
	// never reached DX8_RECORD_RENDER and `draws:` undercounted it. Report it here.
	void Record_Instanced_Draw(int pcount, int vcount);

	// RAII tag; restores the previous subsystem so a nested renderer can't leak its tag.
	class DrawSubsystemScope
	{
	public:
		explicit DrawSubsystemScope(DrawSubsystem s) : m_prev(Get_Draw_Subsystem()) { Set_Draw_Subsystem(s); }
		~DrawSubsystemScope() { Set_Draw_Subsystem(m_prev); }
	private:
		DrawSubsystem m_prev;
	};

	void Begin_Statistics();
	void End_Statistics();
	void Shutdown_Statistics();
};

#define ADD_STATISTICS_VERTEX_PROCESSOR_PUSH Debug_Statistics::Add_Vertex_Processor()
#define DX8_RECORD_TEXTURE(t) Debug_Statistics::Record_Texture(t)
#define DX8_RECORD_TEXTURE_ARRAY(t,c) Debug_Statistics::Record_Texture_Array(t,c)
#define DX8_RECORD_RENDER(polys,verts,shader) Debug_Statistics::Record_DX8_Polys_And_Vertices(polys,verts,shader)
#define DX8_RECORD_SORTING_RENDER(polys,verts) Debug_Statistics::Record_Sorting_Polys_And_Vertices(polys,verts)
#define DX8_RECORD_SKIN_RENDER(polys,verts) Debug_Statistics::Record_DX8_Skin_Polys_And_Vertices(polys,verts)
