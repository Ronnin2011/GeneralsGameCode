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

// Ronin @build 26/10/2025 DX9: Ensure d3d9.h and typedefs are available
#include <d3d9.h>

// Ronin @build 26/10/2025 DX9: Guard typedefs to prevent redefinition errors
#ifndef DX8_TO_DX9_TYPEDEFS_DEFINED
#define DX8_TO_DX9_TYPEDEFS_DEFINED

typedef IDirect3D9 IDirect3D8;
typedef IDirect3DDevice9 IDirect3DDevice8;
typedef IDirect3DVolume9 IDirect3DVolume8;
typedef IDirect3DSwapChain9 IDirect3DSwapChain8;
typedef D3DVIEWPORT9 D3DVIEWPORT8;
typedef IDirect3DBaseTexture9 IDirect3DBaseTexture8;
typedef IDirect3DTexture9 IDirect3DTexture8;
typedef IDirect3DCubeTexture9 IDirect3DCubeTexture8;
typedef IDirect3DVolumeTexture9 IDirect3DVolumeTexture8;
typedef IDirect3DSurface9 IDirect3DSurface8;

#endif // DX8_TO_DX9_TYPEDEFS_DEFINED

#include "always.h"

// Ronin @build 26/10/2025 DX9: Removed forward declarations - conflicts with typedefs defined above

class MissingTexture
{
public:
	static void _Init();
	static void _Deinit();

	static IDirect3DTexture8* _Get_Missing_Texture();		// Return a reference to missing texture
	static IDirect3DSurface8* _Create_Missing_Surface();	// Create new surface which contain missing texture image
};
