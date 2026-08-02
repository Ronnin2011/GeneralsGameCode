/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

// Ronin @build 27/01/2025 DX9: DX8-to-DX9 compatibility layer
// This header provides type aliases for legacy DX8 code to work with DX9

#ifndef DX8TODX9_H
#define DX8TODX9_H

#include <d3d9.h>

// DX8 interface typedefs -> DX9 equivalents
typedef IDirect3D9 IDirect3D8;
typedef IDirect3DDevice9 IDirect3DDevice8;
typedef IDirect3DVolume9 IDirect3DVolume8;
typedef IDirect3DSwapChain9 IDirect3DSwapChain8;
typedef IDirect3DBaseTexture9 IDirect3DBaseTexture8;
typedef IDirect3DTexture9 IDirect3DTexture8;
typedef IDirect3DCubeTexture9 IDirect3DCubeTexture8;
typedef IDirect3DVolumeTexture9 IDirect3DVolumeTexture8;
typedef IDirect3DSurface9 IDirect3DSurface8;
typedef IDirect3DVertexBuffer9 IDirect3DVertexBuffer8;
typedef IDirect3DIndexBuffer9 IDirect3DIndexBuffer8;

// DX8 pointer typedefs
typedef IDirect3D9* LPDIRECT3D8;
typedef IDirect3DDevice9* LPDIRECT3DDEVICE8;
typedef IDirect3DTexture9* LPDIRECT3DTEXTURE8;
typedef IDirect3DVolumeTexture9* LPDIRECT3DVOLUMETEXTURE8;
typedef IDirect3DCubeTexture9* LPDIRECT3DCUBETEXTURE8;
typedef IDirect3DVertexBuffer9* LPDIRECT3DVERTEXBUFFER8;
typedef IDirect3DIndexBuffer9* LPDIRECT3DINDEXBUFFER8;
typedef IDirect3DSurface9* LPDIRECT3DSURFACE8;
typedef IDirect3DVolume9* LPDIRECT3DVOLUME8;
typedef IDirect3DSwapChain9* LPDIRECT3DSWAPCHAIN8;

// DX8 structure typedefs -> DX9
typedef D3DVIEWPORT9 D3DVIEWPORT8;
typedef D3DMATERIAL9 D3DMATERIAL8;
typedef D3DLIGHT9 D3DLIGHT8;

// DX8 render states removed in DX9
#define D3DRS_SOFTWAREVERTEXPROCESSING D3DRS_FORCE_DWORD
#define D3DRS_PATCHSEGMENTS D3DRS_FORCE_DWORD

// DX8 raster caps removed in DX9
#define D3DPRASTERCAPS_ZBIAS 0x00000000

// DX8 texture stage states moved to sampler states in DX9
#define D3DTSS_ADDRESSU D3DSAMP_ADDRESSU
#define D3DTSS_ADDRESSV D3DSAMP_ADDRESSV  
#define D3DTSS_ADDRESSW D3DSAMP_ADDRESSW
#define D3DTSS_MAGFILTER D3DSAMP_MAGFILTER
#define D3DTSS_MINFILTER D3DSAMP_MINFILTER
#define D3DTSS_MIPFILTER D3DSAMP_MIPFILTER

#endif // DX8TODX9_H
