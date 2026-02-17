/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#pragma once
#include <d3d9.h>

// Ronin @feature 27/11/2025: Vertex declaration Testing for water tracks
// Replaces FVF 0x00000142 (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)
// Layout: Position(float3) + Color(D3DCOLOR) + UV(float2) = 24 bytes

struct WaterTrackVertex {
    float x, y, z;    // POSITION (offset 0, 12 bytes)
    DWORD diffuse;    // COLOR0 (offset 12, 4 bytes) - ARGB format
    float u, v;       // TEXCOORD0 (offset 16, 8 bytes)
    
    static const UINT Stride = 24;
    static const UINT FVF = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;
};

// Declaration elements for D3D9 vertex declaration
inline const D3DVERTEXELEMENT9* GetWaterTracksDeclElements() {
    static D3DVERTEXELEMENT9 decl[] = {
        { 0,  0,  D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_D3DCOLOR,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
        { 0, 16, D3DDECLTYPE_FLOAT2,    D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    return decl;
}

// Get stride for this vertex format
inline UINT GetWaterTracksStride() {
    return WaterTrackVertex::Stride;
}

// Factory to create vertex declaration
inline HRESULT CreateWaterTracksDecl(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9** outDecl) {
    if (!dev || !outDecl) return E_INVALIDARG;
    *outDecl = nullptr;
    return dev->CreateVertexDeclaration(GetWaterTracksDeclElements(), outDecl);
}
