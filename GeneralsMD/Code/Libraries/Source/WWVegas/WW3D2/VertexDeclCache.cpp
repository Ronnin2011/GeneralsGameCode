/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#include "VertexDeclCache.h"
#include "dx8wrapper.h"          // for DX8Wrapper::BindLayoutDecl / BindLayoutFVF
#include "WaterTracksDecl.h"     // if you provide GetWaterTracksDeclElements(), GetWaterTracksStride()

VertexDeclCache::VertexDeclCache(IDirect3DDevice9* device)
  : m_device(device) {
}

VertexDeclCache::~VertexDeclCache() {
  for (auto& kv : m_cache) {
    if (kv.second.decl) {
      kv.second.decl->Release();
      kv.second.decl = nullptr;
    }
  }
  m_cache.clear();
}

// Minimal recognizer; extend as needed for your formats
bool VertexDeclCache::IsKnownFvf(UINT fvf) {
  switch (fvf) {
  case (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1): // Water tracks
  case (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX2): // Water: VertexFormatXYZNDUV2 / dynamic_fvf_type
  case (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2): // Water mesh: VertexFormatXYZDUV2
    return true;
  default:
    return false;
  }
}

// Optional generic builder if not using WaterTracksDecl.h for this FVF
static bool BuildDecl_XYZ_Diffuse_Tex1(D3DVERTEXELEMENT9* out, UINT& stride) {
  const D3DVERTEXELEMENT9 decl[] = {
      {0, 0,  D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0},
      {0, 16, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()
  };
  memcpy(out, decl, sizeof(decl));
  stride = 24;
  return true;
}

// @bugfix Ronin 13/01/2026 DX9: Add water declarations for FVF->decl binding (prevents vertex misinterpretation/jigsaw polys)
static bool BuildDecl_XYZ_Normal_Diffuse_Tex2(D3DVERTEXELEMENT9* out, UINT& stride) {
  const D3DVERTEXELEMENT9 decl[] = {
      {0, 0,  D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, // x,y,z
      {0, 12, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0}, // nx,ny,nz
      {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0}, // diffuse
      {0, 28, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, // u1,v1
      {0, 36, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1}, // u2,v2
      D3DDECL_END()
  };
  memcpy(out, decl, sizeof(decl));
  stride = 44; // sizeof(VertexFormatXYZNDUV2)
  return true;
}

static bool BuildDecl_XYZ_Diffuse_Tex2(D3DVERTEXELEMENT9* out, UINT& stride) {
  const D3DVERTEXELEMENT9 decl[] = {
      {0, 0,  D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, // x,y,z
      {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0}, // diffuse
      {0, 16, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, // u1,v1
      {0, 24, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1}, // u2,v2
      D3DDECL_END()
  };
  memcpy(out, decl, sizeof(decl));
  stride = 32; // sizeof(VertexFormatXYZDUV2)
  return true;
}

bool VertexDeclCache::BuildDeclForFvf(UINT fvf, D3DVERTEXELEMENT9* outElements, UINT& outStride) {
  switch (fvf) {
  case (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1):
    return BuildDecl_XYZ_Diffuse_Tex1(outElements, outStride);
  case (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX2):
    return BuildDecl_XYZ_Normal_Diffuse_Tex2(outElements, outStride);
  case (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2):
    return BuildDecl_XYZ_Diffuse_Tex2(outElements, outStride);
  default:
    return false;
  }
}

const DeclEntry* VertexDeclCache::GetOrCreateDecl(UINT fvf) {
  auto it = m_cache.find(fvf);
  if (it != m_cache.end()) return &it->second;

  if (!IsKnownFvf(fvf)) {
#ifdef _DEBUG
    WWDEBUG_SAY(("VertexDeclCache: No declaration mapping for FVF=0x%08X", fvf));
#endif
    return nullptr;
  }

  DeclEntry entry{};
  HRESULT hr = E_FAIL;

  // Option A: use specific WaterTracksDecl helpers (if you have them)
  if (fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1) && GetWaterTracksDeclElements && GetWaterTracksStride) {
    hr = m_device->CreateVertexDeclaration(GetWaterTracksDeclElements(), &entry.decl);
    entry.stride = GetWaterTracksStride(); // should be 24
  }
  else {
    // Option B: generic builder
    D3DVERTEXELEMENT9 elements[8] = {};
    UINT stride = 0;
    if (!BuildDeclForFvf(fvf, elements, stride)) {
#ifdef _DEBUG
      WWDEBUG_SAY(("VertexDeclCache: BuildDeclForFvf failed for FVF=0x%08X", fvf));
#endif
      return nullptr;
    }
    hr = m_device->CreateVertexDeclaration(elements, &entry.decl);
    entry.stride = stride;
  }

  if (FAILED(hr) || !entry.decl) {
#ifdef _DEBUG
    WWDEBUG_SAY(("VertexDeclCache: CreateVertexDeclaration failed for FVF=0x%08X (hr=0x%08X)", fvf, hr));
#endif
    return nullptr;
  }

  auto [insIt, ok] = m_cache.emplace(fvf, entry);
  if (!ok) {
    entry.decl->Release();
    return nullptr;
  }
  return &insIt->second;
}

bool VertexDeclCache::BindDecl(UINT fvf, const char* ownerTag) {
  const DeclEntry* e = GetOrCreateDecl(fvf);
  if (!e || !e->decl) return false;
  DX8Wrapper::BindLayoutDecl(e->decl, ownerTag);
  return true;
}

bool VertexDeclCache::BindFVF(UINT fvf, const char* ownerTag) {
  DX8Wrapper::BindLayoutFVF(fvf, ownerTag);
  return true;
}

bool VertexDeclCache::BindStream0(IDirect3DVertexBuffer9* vb, UINT fvf) {
  const DeclEntry* e = GetOrCreateDecl(fvf);
  if (!e || !e->decl) return false;
  // If you always use declaration path for this FVF, use its stride
  return SUCCEEDED(m_device->SetStreamSource(0, vb, 0, e->stride));
}

bool VertexDeclCache::OwnsDecl(IDirect3DVertexDeclaration9* decl) const
{
  if (!decl) return false;

  for (const auto& kv : m_cache) {
    if (kv.second.decl == decl) {
      return true;
    }
  }

  return false;
}
