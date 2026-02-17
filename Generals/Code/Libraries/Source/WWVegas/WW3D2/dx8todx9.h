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

// Ronin @bugfix 09/11/2025: DX8 texture stage states DON'T map to DX9 sampler states!
// In DX8, these were part of SetTextureStageState()
// In DX9, they're separate SetSamplerState() calls
// We need wrapper functions to handle this, not direct defines!

// DO NOT USE THESE - They will break rendering:
// #define D3DTSS_ADDRESSU D3DSAMP_ADDRESSU  // WRONG!
// #define D3DTSS_ADDRESSV D3DSAMP_ADDRESSV  // WRONG!

// Instead, we need to intercept SetTextureStageState calls and redirect
// texture sampling states to SetSamplerState. This must be done in dx8wrapper.cpp!

// Ronin @bugfix 09/11/2025: DX8 to DX9 texture stage state mapping
// These DX8 constants need special handling:
enum DX8_TSS_SAMPLER_STATES {
  DX8_TSS_ADDRESSU = 13,      // Maps to D3DSAMP_ADDRESSU
  DX8_TSS_ADDRESSV = 14,      // Maps to D3DSAMP_ADDRESSV
  DX8_TSS_ADDRESSW = 25,      // Maps to D3DSAMP_ADDRESSW
  DX8_TSS_MAGFILTER = 16,     // Maps to D3DSAMP_MAGFILTER
  DX8_TSS_MINFILTER = 17,     // Maps to D3DSAMP_MINFILTER
  DX8_TSS_MIPFILTER = 18,     // Maps to D3DSAMP_MIPFILTER
  DX8_TSS_MIPMAPLODBIAS = 19, // Maps to D3DSAMP_MIPMAPLODBIAS
  DX8_TSS_MAXMIPLEVEL = 20,   // Maps to D3DSAMP_MAXMIPLEVEL
  DX8_TSS_MAXANISOTROPY = 21, // Maps to D3DSAMP_MAXANISOTROPY
};

#endif // DX8TODX9_H
