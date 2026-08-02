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
#include <unordered_map>

struct DeclEntry {
  IDirect3DVertexDeclaration9* decl = nullptr;
  UINT stride = 0;
};

class VertexDeclCache {
public:

  //Pointer for comparison in our cache.
  bool OwnsDecl(IDirect3DVertexDeclaration9* decl) const;

  explicit VertexDeclCache(IDirect3DDevice9* device);
  ~VertexDeclCache();

  // Create or fetch a cached declaration for the given FVF.
  const DeclEntry* GetOrCreateDecl(UINT fvf);

  // Bind helpers (implemented in .cpp to avoid including dx8wrapper.h here)
  bool BindDecl(UINT fvf, const char* ownerTag);
  bool BindFVF(UINT fvf, const char* ownerTag);

  // Bind stream 0 with the stride for the given FVF declaration.
  bool BindStream0(IDirect3DVertexBuffer9* vb, UINT fvf);

private:
  // Optionally expand to support more known FVFs
  static bool IsKnownFvf(UINT fvf);
  static bool BuildDeclForFvf(UINT fvf, D3DVERTEXELEMENT9* outElements, UINT& outStride);

private:
  IDirect3DDevice9* m_device = nullptr;
  std::unordered_map<UINT, DeclEntry> m_cache;
};
