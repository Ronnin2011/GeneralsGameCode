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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////


// WorldHeightMap.h
// Class to encapsulate height map.
// Author: John Ahlquist, April 2001

#pragma once

#include "Lib/BaseType.h"
#include "WWLib/refcount.h"
#include "WWMath/vector3.h"
#include "W3DDevice/GameClient/TileData.h"
#include "Common/MapObject.h"

#include "Common/STLTypedefs.h"
typedef std::vector<ICoord2D> VecICoord2D;


/** MapObject class
Not ref counted.  Do not store pointers to this class.  */

#define VERTEX_BUFFER_TILE_LENGTH 32 //tiles of side length 32 (grid of 33x33 vertices).
#define VERTS_IN_BLOCK_ROW (VERTEX_BUFFER_TILE_LENGTH + 1)
#define HEIGHTMAP_VERTEX_NUM (VERTEX_BUFFER_TILE_LENGTH * 2 * VERTEX_BUFFER_TILE_LENGTH * 2)
#define HEIGHTMAP_POLYGON_NUM (VERTEX_BUFFER_TILE_LENGTH * VERTEX_BUFFER_TILE_LENGTH * 2)

#define K_MIN_HEIGHT  0
#define K_MAX_HEIGHT  255

#define NUM_SOURCE_TILES 1024
#define NUM_BLEND_TILES 16192
#define NUM_CLIFF_INFO 32384
#define FLAG_VAL 0x7ADA0000

// For backwards compatiblity.
#define TEX_PATH_LEN 256


/// Struct in memory.
typedef struct {
	Int globalTextureClass;
	Int firstTile;
	Int numTiles;
	Int width;
	// @feature Ronin 05/04/2026 Actual source pixels per logical terrain tile.
	Int tilePixelExtent;
	// @feature Ronin 07/04/2026 Atlas page index for future multi-page terrain atlases.
	Int texturePage;
	Int isBlendEdgeTile; ///< True if the texture contains blend edges.
	AsciiString name;
	ICoord2D positionInTexture;
} TXTextureClass;

// @feature Ronin 07/04/2026 Persist page-level terrain atlas metadata alongside legacy single-page fields.
#define MAX_TEXTURE_ATLAS_PAGES NUM_TEXTURE_CLASSES

/// Struct in memory.
typedef struct {
	Int textureHeight;
} TTextureAtlasPageInfo;

typedef enum { POS_X, POS_Y, NEG_X, NEG_Y } TVDirection;
/// Struct in memory.
typedef struct {
	Real	u0, v0;	 // Upper left uv
	Real	u1, v1;	 // Lower left uv
	Real	u2, v2;	 // Lower right uv
	Real	u3, v3;	 // Upper right uv
	Bool  flip;
	Bool	mutant;  // Mutant mapping needed to get this to fit.
	Short tileIndex; // Tile texture.
} TCliffInfo;

#define NUM_TEXTURE_CLASSES 256


class TextureClass;
class ChunkInputStream;
class InputStream;
class OutputStream;
class DataChunkInput;
struct DataChunkInfo;
class TerrainTextureClass;
class AlphaTerrainTextureClass;
class AlphaEdgeTextureClass;

#define NUM_ALPHA_TILES 12

class WorldHeightMap : public RefCountClass,
                       public WorldHeightMapInterfaceClass
{
	friend class TerrainTextureClass;
	friend class AlphaTerrainTextureClass;
	friend class W3DCustomEdging;
	friend class AlphaEdgeTextureClass;

#define NO_EVAL_TILING_MODES

public:

	struct DrawArea
	{
		Int originX;
		Int originY;
		Int sizeX;
		Int sizeY;
	};

#ifdef EVAL_TILING_MODES
	enum {TILE_4x4, TILE_6x6, TILE_8x8} m_tileMode;
#endif
	enum {
		NORMAL_DRAW_WIDTH = 1 + 4*VERTEX_BUFFER_TILE_LENGTH,
		NORMAL_DRAW_HEIGHT = 1 + 4*VERTEX_BUFFER_TILE_LENGTH,
		STRETCH_DRAW_WIDTH = 1 + 2*VERTEX_BUFFER_TILE_LENGTH,
		STRETCH_DRAW_HEIGHT = 1 + 2*VERTEX_BUFFER_TILE_LENGTH,
		LOW_ANGLE_DRAW_WIDTH = 1 + (NORMAL_DRAW_WIDTH-1) * 2,
		LOW_ANGLE_DRAW_HEIGHT = 1 + (NORMAL_DRAW_HEIGHT-1) * 2,
	};

protected:
	Int m_width;				///< Height map width.
	Int m_height;				///< Height map height (y size of array).
	Int m_borderSize;		///< Non-playable border area.
	VecICoord2D m_boundaries;	///< the in-game boundaries
	Int m_dataSize;			///< size of m_data.
	UnsignedByte* m_data;	///< array of z(height) values in the height map.

	UnsignedByte* m_seismicUpdateFlag;  ///< array of bits to prevent ovelapping physics-update regions from doubling effects on shared cells
	UnsignedInt   m_seismicUpdateWidth; ///< width of the array holding SeismicUpdateFlags
	Real* m_seismicZVelocities; ///< how fast is the dirt rising/falling at this location

	UnsignedByte* m_cellFlipState;	///< array of bits to indicate the flip state of each cell.
	Int m_flipStateWidth;			///< with of the array holding cellFlipState
	UnsignedByte* m_cellCliffState;	///< array of bits to indicate the cliff state of each cell.

	/// Texture indices.
	Short* m_tileNdxes;  ///< matches m_Data, indexes into m_SourceTiles.
	Short* m_blendTileNdxes;  ///< matches m_Data, indexes into m_blendedTiles.  0 means no blend info.
	Short* m_cliffInfoNdxes;  ///< matches m_Data, indexes into m_cliffInfo.	 0 means no cliff info.
	Short* m_extraBlendTileNdxes;  ///< matches m_Data, indexes into m_extraBlendedTiles.  0 means no blend info.

	Int m_numBitmapTiles;	// Number of tiles initialized from bitmaps in m_SourceTiles.
	Int m_numEdgeTiles;	// Number of tiles initialized from bitmaps in m_SourceTiles.
	Int m_numBlendedTiles;	// Number of blended tiles created from bitmap tiles.

	TileData* m_sourceTiles[NUM_SOURCE_TILES];	///< Tiles for m_textureClasses
	TileData* m_edgeTiles[NUM_SOURCE_TILES];	///< Tiles for m_textureClasses

	TBlendTileInfo	m_blendedTiles[NUM_BLEND_TILES];
	TBlendTileInfo	m_extraBlendedTiles[NUM_BLEND_TILES];

	TCliffInfo	m_cliffInfo[NUM_CLIFF_INFO];
	Int m_numCliffInfo; ///< Number of cliffInfo's used in m_cliffInfo.

	// Texture classes.  There is one texture class for each bitmap read in.
	// A class may have more than one tile.  For example, if the grass bitmap is
	// 128x128, it creates 4 64x64 tiles, so the grass texture class will have 4 tiles.
	int m_numTextureClasses;
	TXTextureClass m_textureClasses[NUM_TEXTURE_CLASSES];

	// Edge Texture classes.  There is one texture class for each bitmap read in.
	// An edge class will normally have 4 tiles.
	int m_numEdgeTextureClasses;
	TXTextureClass m_edgeTextureClasses[NUM_TEXTURE_CLASSES];

	/** The actual texture used to render the 3d mesh.  Note that it is
	 basically m_SourceTiles laid out in rows, so by itself it is not useful.
	 Use GetUVData to get the mapping info for height cells to map into the
	 texture. */
	TerrainTextureClass* m_terrainTex;
	Int	m_terrainTexHeight; /// Height of m_terrainTex allocated.
	// @feature Ronin 08/04/2026 Store per-page diffuse terrain textures while preserving legacy page-0 aliases.
	TerrainTextureClass* m_terrainTextures[MAX_TEXTURE_ATLAS_PAGES];
	// @feature Ronin 07/04/2026 Future page-aware terrain atlas metadata.
	Int m_numTerrainTexturePages;
	TTextureAtlasPageInfo m_terrainTexturePages[MAX_TEXTURE_ATLAS_PAGES];
	/** The texture that contains the alpha edge tiles that get blended on
			top of the base texture. getAlphaUVData does the mapping. */
	AlphaTerrainTextureClass* m_alphaTerrainTex;
	Int	m_alphaTexHeight; /// Height of m_alphaTerrainTex allocated.
	// @feature Ronin 08/04/2026 Store per-page terrain alpha textures while preserving legacy page-0 aliases.
	AlphaTerrainTextureClass* m_alphaTerrainTextures[MAX_TEXTURE_ATLAS_PAGES];


	/** The texture that contains custom blend edge tiles. */
	AlphaEdgeTextureClass* m_alphaEdgeTex;
	Int	m_alphaEdgeHeight; /// Height of m_alphaEdgeTex allocated.
	// @feature Ronin 08/04/2026 Store per-page edge terrain textures while preserving legacy page-0 aliases.
	AlphaEdgeTextureClass* m_alphaEdgeTextures[MAX_TEXTURE_ATLAS_PAGES];
	// @feature Ronin 07/04/2026 Future page-aware edge atlas metadata.
	Int m_numEdgeTexturePages;
	TTextureAtlasPageInfo m_edgeTexturePages[MAX_TEXTURE_ATLAS_PAGES];

	// @feature Ronin 06/05/2026 Splat S20-A3: cell->texel ratio + dirty flag for the per-material splat bake.
	// (Legacy m_primaryBlendControlTexture and its width/height fields were removed; the per-material
	// weight atlas in m_perMaterialWeightAtlas is now the only sampled splat resource.)
	Int  m_primaryBlendTexelsPerCell;
	Bool m_primaryBlendControlTextureDirty;

	/// Drawing info - re the part of the map that is being drawn.
	Int m_drawOriginX;
	Int m_drawOriginY;
	Int m_drawWidthX;
	Int m_drawHeightY;

	/// Tiles that hold the alpha channel info.
	static TileData* m_alphaTiles[NUM_ALPHA_TILES];

	// @bugfix Ronin 26/04/2026 Reference count of live WorldHeightMap instances;
	// the last destructor releases the shared static m_alphaTiles.
	static Int s_instanceCount;

protected:
	TileData *getSourceTile(UnsignedInt ndx) { if (ndx<NUM_SOURCE_TILES) return(m_sourceTiles[ndx]); return(nullptr); };
	TileData *getEdgeTile(UnsignedInt ndx) { if (ndx<NUM_SOURCE_TILES) return(m_edgeTiles[ndx]); return(nullptr); };
	/// UV mapping data for a cell to map into the terrain texture.
	void getUVForNdx(Int ndx, float *minU, float *minV, float *maxU, float*maxV);
	Bool getUVForTileIndex(Int ndx, Short tileNdx, float U[4], float V[4]);
	Int getTextureClassFromNdx(Int tileNdx);
	void readTexClass(TXTextureClass *texClass, TileData **tileData);
	Int updateTileTexturePositions(Int *edgeHeight); ///< Places each tile in the texture.
	void initCliffFlagsFromHeights();
	void setCellCliffFlagFromHeights(Int xIndex, Int yIndex);

protected:	 // file reader callbacks.
	static Bool ParseHeightMapDataChunk(DataChunkInput &file, DataChunkInfo *info, void *userData);
	Bool ParseHeightMapData(DataChunkInput &file, DataChunkInfo *info, void *userData);
	static Bool ParseSizeOnlyInChunk(DataChunkInput &file, DataChunkInfo *info, void *userData);
	Bool ParseSizeOnly(DataChunkInput &file, DataChunkInfo *info, void *userData);
	static Bool ParseBlendTileDataChunk(DataChunkInput &file, DataChunkInfo *info, void *userData);
	Bool ParseBlendTileData(DataChunkInput &file, DataChunkInfo *info, void *userData);
	static Bool ParseWorldDictDataChunk(DataChunkInput &file, DataChunkInfo *info, void *userData);
	static Bool ParseObjectsDataChunk(DataChunkInput &file, DataChunkInfo *info, void *userData);
	static Bool ParseObjectDataChunk(DataChunkInput &file, DataChunkInfo *info, void *userData);
	Bool ParseObjectData(DataChunkInput &file, DataChunkInfo *info, void *userData, Bool readDict);
	static Bool ParseLightingDataChunk(DataChunkInput &file, DataChunkInfo *info, void *userData);

protected:
	WorldHeightMap();			///< Simple constructor for WorldHeightMapEdit class.

public: // constructors/destructors
	WorldHeightMap(ChunkInputStream* pFile, Bool bHMapOnly = false);	// read from file.
	virtual ~WorldHeightMap() override;			// destroy.

public:  // Boundary info
	const VecICoord2D& getAllBoundaries() const { return m_boundaries; }

public:  // height map info.
	static Int getMinHeightValue() { return K_MIN_HEIGHT; }
	static Int getMaxHeightValue() { return K_MAX_HEIGHT; }

	UnsignedByte* getDataPtr() { return m_data; }

	Int getXExtent() {return m_width;}	///<number of vertices in x
	Int getYExtent() {return m_height;}	///<number of vertices in y

	Int getDrawOrgX() {return m_drawOriginX;}
	Int getDrawOrgY() {return m_drawOriginY;}

	Int getDrawWidth() {return m_drawWidthX;}
	Int getDrawHeight() {return m_drawHeightY;}
	void setDrawWidth(Int width) {DEBUG_ASSERTCRASH(width <= m_width, ("Draw width must not exceed map width")); m_drawWidthX = width;}
	void setDrawHeight(Int height) {DEBUG_ASSERTCRASH(height <= m_height, ("Draw height must not exceed map height")); m_drawHeightY = height;}
	virtual Int getBorderSize() override {return m_borderSize;}
  Int getBorderSizeInline() const { return m_borderSize; }
	/// Get height with the offset that HeightMapRenderObjClass uses built in.
	UnsignedByte getDisplayHeight(Int x, Int y) { return m_data[x + m_drawOriginX + m_width * (y + m_drawOriginY)]; }

	/// Get height in normal coordinates.
	UnsignedByte getHeight(Int xIndex, Int yIndex)
	{
		Int ndx = (yIndex * m_width) + xIndex;
		if ((ndx >= 0) && (ndx < m_dataSize) && m_data)
			return(m_data[ndx]);
		else
			return(0);
	};

	void getUVForBlend(Int edgeClass, Region2D *range);

	DrawArea createDrawArea(Int xOrg, Int yOrg);
	Bool setDrawArea(const DrawArea& drawArea);
	Bool setDrawOrg(Int xOrg, Int yOrg);

	static void freeListOfMapObjects();

	Int getTextureClassNoBlend(Int xIndex, Int yIndex, Bool baseClass = false);
	Int getTextureClass(Int xIndex, Int yIndex, Bool baseClass = false);
	TXTextureClass getTextureFromIndex(Int textureIndex);

public:  // tile and texture info.

	// @feature Ronin 07/04/2026 Added compatibility accessors for legacy single-page terrain atlas metadata.
	Int getTerrainTexturePageCount() const { return m_numTerrainTexturePages; }
	Int getEdgeTexturePageCount() const { return m_numEdgeTexturePages; }

	Int getTerrainTextureHeightForPage(Int page) const
	{
		if (page < 0 || page >= m_numTerrainTexturePages) {
			return 0;
		}
		return m_terrainTexturePages[page].textureHeight;
	}

	Int getEdgeTextureHeightForPage(Int page) const
	{
		if (page < 0 || page >= m_numEdgeTexturePages) {
			return 0;
		}
		return m_edgeTexturePages[page].textureHeight;
	}

	// @feature Ronin 08/04/2026 Draw-local page ownership queries used by page-grouped terrain submission.
	Int getTerrainTexturePageForCell(Int xIndex, Int yIndex) const;
	Int getAlphaTexturePageForCell(Int xIndex, Int yIndex) const;
	Int getExtraAlphaTexturePageForCell(Int xIndex, Int yIndex) const;


	// @feature Ronin 08/04/2026 Page-aware terrain texture accessors for future multi-page terrain rendering.
	TextureClass* getTerrainTexture(Int page);
	TextureClass* getAlphaTerrainTexture(Int page);
	TextureClass* getEdgeTerrainTexture(Int page);

	// @feature Ronin 26/04/2026 Splat S20-A1/A2: per-material weight texture bake.
	// One weight channel per active material on the map. Active set computed at bake time
	// (`m_activeMaterialIndices`). Sum of all channels per texel == 1.0 by construction.
	// The baked data now feeds the live per-material terrain path;
	enum { SPLAT_MAX_ACTIVE_MATERIALS = 32 };
	// @feature Ronin 27/04/2026 Splat S20-A2a: 4 weight channels per BGRA8 page,
	// SPLAT_MAX_ACTIVE_MATERIALS / 4 pages == 8 covers every legal active set.
	enum { MAX_S20_WEIGHT_ATLAS_PAGES = SPLAT_MAX_ACTIVE_MATERIALS / 4 };


	Bool buildPerMaterialWeightTextures(
		Int texelsPerCell,
		Int blurRadiusCells);

	// @feature Ronin 27/04/2026 Splat S20-A2a: allocate / refresh the GPU weight-atlas
	// pages from m_perMaterialWeightBytes. Must be called AFTER
	// buildPerMaterialWeightTextures(). The resulting pages are consumed by the live
	// per-material terrain shader path in HeightMap.cpp::renderPrimaryBlendControlPass().
	Bool ensurePerMaterialWeightAtlasTextures();

	// @feature Ronin 10/05/2026 Normal-map N1: allocate one A8R8G8B8 normal atlas page
	// per diffuse atlas page, populate per source-tile rectangles by attempting to load
	// <basename>_NRM.dds then <basename>_NRM.tga via WW3DAssetManager, falling back to
	// flat-normal default (128,128,255,255) when absent or resolution-mismatched. Calls
	// D3DXFilterTexture(...,D3DX_FILTER_BOX) for mips. Idempotent; safe to call from
	// ensureSplatTextures() on the same dirty-flag the weight atlas uses. Logs an
	// [NRM] line summarizing allocated pages, loaded tiles, and flat-default fallbacks.
	Bool ensurePerMaterialNormalAtlasTextures();


	Int  getActiveMaterialCount() const { return m_numActiveMaterials; }
	Int  getActiveMaterialClass(Int activeIdx) const {
		return (activeIdx >= 0 && activeIdx < m_numActiveMaterials)
			? m_activeMaterialIndices[activeIdx] : -1;
	}

	// @feature Ronin 29/04/2026 Splat S20-A2d2: public read-only accessors for the
	// weight-atlas pages allocated by ensurePerMaterialWeightAtlasTextures().
	// HeightMap.cpp::renderPrimaryBlendControlPass binds these to PS samplers s1..s8
	// for the live per-material terrain path. Page count is the count actually
	// allocated this bake (== ceil(numActive/4)), not the compile-time
	// MAX_S20_WEIGHT_ATLAS_PAGES ceiling.
	Int getPerMaterialWeightAtlasPageCount() const { return m_numPerMaterialWeightAtlasPages; }
	TextureClass* getPerMaterialWeightAtlasPage(Int page) const {
		return (page >= 0 && page < m_numPerMaterialWeightAtlasPages)
			? m_perMaterialWeightAtlas[page] : nullptr;
	}

private:
	// Bake state. Cleared by freeMapResources(); allocated by buildPerMaterialWeightTextures().
	Int            m_numActiveMaterials = 0;
	Int            m_activeMaterialIndices[SPLAT_MAX_ACTIVE_MATERIALS] = {};
	UnsignedByte* m_perMaterialWeightBytes = nullptr;   // [activeIdx][texelY*pitch + texelX], one byte per texel
	Int            m_perMaterialWeightWidth = 0;
	Int            m_perMaterialWeightHeight = 0;
	Int            m_perMaterialWeightPitch = 0;         // bytes per row (== width, alignment-padded if needed)

	// @feature Ronin 27/04/2026 Splat S20-A2a: GPU weight atlas pages. Each page is a
	// BGRA8 2D texture packing up to 4 active material weight channels (slot s lives
	// in page s/4, BGRA channel s%4 -> B,G,R,A). Allocated by
	// ensurePerMaterialWeightAtlasTextures(), released in ~WorldHeightMap.
	TextureClass* m_perMaterialWeightAtlas[MAX_S20_WEIGHT_ATLAS_PAGES] = {};
	Int            m_numPerMaterialWeightAtlasPages = 0;
	Int            m_perMaterialWeightAtlasWidth = 0;
	Int            m_perMaterialWeightAtlasHeight = 0;


	// @feature Ronin 10/05/2026 Normal-map N1: GPU normal atlas pages. One page per
	// diffuse atlas page, mirrors m_terrainTextures[] layout exactly so the existing
	// getSplatAtlasRegionsForActiveSetPage() region tables address the normal atlas
	// without modification. Each page is an A8R8G8B8 2D texture; per source tile the
	// rectangle at TXTextureClass::positionInTexture / tilePixelExtent holds the
	// tangent-space normal sampled from <basename>_NRM.dds / .tga, or the flat-normal
	// default (128,128,255,255) when the source asset is absent or mismatched.
	// Allocated by ensurePerMaterialNormalAtlasTextures(), released in ~WorldHeightMap
	// and on map reload.
	TextureClass* m_perMaterialNormalAtlas[MAX_TEXTURE_ATLAS_PAGES] = {};
	Int            m_numPerMaterialNormalAtlasPages = 0;

public:
	// @feature Ronin 27/04/2026 Splat S20-A2b: per-ACTIVE-slot atlas region table for the
	// new per-material splat PS. Same regionA / regionB layout as getSplatAtlasRegions(),
	// but indexed by active slot (0..m_numActiveMaterials-1) so it lines up with the
	// weight-atlas pages produced by ensurePerMaterialWeightAtlasTextures():
	//   active slot s -> weight atlas page (s/4), BGRA channel (s%4)
	//                 -> regionA[s] / regionB[s] for the atlas UV math.
	// Caller passes buffers sized SPLAT_MAX_ACTIVE_MATERIALS * 4 floats each.
	// Slots past m_numActiveMaterials are zero-filled (extent=0 -> any sample reads (0,0)).
	// Returns the number of valid slots written (== m_numActiveMaterials).
	// No D3D state touched; pure CPU-side table emission.
	// See docs/Terrain_Splat_Map_Design.md S20 A2-b.
	Int getSplatAtlasRegionsForActiveSet(float* outRegionA, float* outRegionB);

	// @feature Ronin 10/05/2026 Normal-map N1: accessors for the per-material normal
	// atlas pages. Layout mirrors m_terrainTextures[] so getSplatAtlasRegionsForActive
	// SetPage() addresses both atlases. Returns nullptr / 0 when the normal atlas has
	// not been built yet (e.g. before ensureSplatTextures() has run, or on hardware
	// where allocation failed). See docs/Terrain_Normal_Map_Design.md N1.
	TextureClass* getPerMaterialNormalAtlasPage(Int page) const;
	Int           getPerMaterialNormalAtlasPageCount() const { return m_numPerMaterialNormalAtlasPages; }

	// @feature Ronin 03/05/2026 Splat S20 multi-atlas: page-aware variant of the
	// active-slot atlas region table.
	// Emits regionA / regionB for all active slots exactly like
	// getSplatAtlasRegionsForActiveSet(), but only slots whose source material lives on
	// `terrainPage` are enabled. `outSlotEnableMask[s]` is 1.0f for slots on this page
	// and 0.0f otherwise. Caller passes buffers sized:
	//   - outRegionA:       SPLAT_MAX_ACTIVE_MATERIALS * 4 floats
	//   - outRegionB:       SPLAT_MAX_ACTIVE_MATERIALS * 4 floats
	//   - outSlotEnableMask SPLAT_MAX_ACTIVE_MATERIALS floats
	// Returns the total active-slot count (`m_numActiveMaterials` clamped to the API cap),
	// not the enabled-slot count for this page.
	Int getSplatAtlasRegionsForActiveSetPage(
		Int terrainPage,
		float* outRegionA,
		float* outRegionB,
		float* outSlotEnableMask);

	// @feature Ronin 29/04/2026 Splat S20-A3: mark the per-material weight atlas as needing a rebuild.
	// (Was invalidatePrimaryBlendControlTexture(); same dirty flag, honest name.)
	void invalidateSplatTextures();

	// @feature Ronin 29/04/2026 Splat S20-A3: per-material weight atlas geometry, used by HeightMap.cpp
	// to compute the world->control-UV scale for the per-material PS.
	Int getPerMaterialWeightAtlasWidth()  const { return m_perMaterialWeightAtlasWidth; }
	Int getPerMaterialWeightAtlasHeight() const { return m_perMaterialWeightAtlasHeight; }
	Int getPrimaryBlendControlTexelsPerCell() const { return m_primaryBlendTexelsPerCell; }

	// @feature Ronin 29/04/2026 Splat S20-A3: lazily (re)build the per-material weight atlas.
	// Replaces ensurePrimaryBlendControlTexture(); cheap when m_primaryBlendControlTextureDirty == FALSE.
	void ensureSplatTextures();

	// @feature Ronin 08/04/2026 Page ownership queries used by multi-page terrain atlas rendering.
	Int getTerrainTexturePageForTileNdx(Short tileNdx) const;
	Int getEdgeTexturePageForBlendClass(Int edgeClass) const;

	void setTextureLOD(Int lod);	///< set maximum lod level sent to the hardware.
	TextureClass* getTerrainTexture();  //< generates if needed and returns the terrain texture
	TextureClass* getAlphaTerrainTexture(); //< generates if needed and returns alpha terrain texture
	TextureClass* getEdgeTerrainTexture(); //< generates if needed and returns blend edge texture
	/// UV mapping data for a cell to map into the terrain texture.  Returns true if the textures had to be stretched for cliffs.
	Bool getUVData(Int xIndex, Int yIndex, float U[4], float V[4]);
	Bool getFlipState(Int xIndex, Int yIndex) const;
	///Faster version of above function without all the safety checks - For people that do checks externally.
	Bool getQuickFlipState(Int xIndex, Int yIndex) const
	{
		return m_cellFlipState[yIndex * m_flipStateWidth + (xIndex >> 3)] & (1 << (xIndex & 0x7));
	}

	void setFlipState(Int xIndex, Int yIndex, Bool value);
	void clearFlipStates();
	Bool getCliffState(Int xIndex, Int yIndex) const;
	Bool getExtraAlphaUVData(Int xIndex, Int yIndex, float U[4], float V[4], UnsignedByte alpha[4], Bool* flip, Bool* cliff);
	/// UV mapping data for a cell to map into the alpha terrain texture.
	void getAlphaUVData(Int xIndex, Int yIndex, float U[4], float V[4], UnsignedByte alpha[4], Bool* flip);
	void getTerrainColorAt(Real x, Real y, RGBColor* pColor);
	AsciiString getTerrainNameAt(Real x, Real y);
	Bool isCliffMappedTexture(Int xIndex, Int yIndex);

	Bool getSeismicUpdateFlag(Int xIndex, Int yIndex) const;
	void setSeismicUpdateFlag(Int xIndex, Int yIndex, Bool value);
	void clearSeismicUpdateFlags();
	virtual Real getSeismicZVelocity(Int xIndex, Int yIndex) const override;
	virtual void setSeismicZVelocity(Int xIndex, Int yIndex, Real value) override;
	void fillSeismicZVelocities(Real value);
	virtual Real getBilinearSampleSeismicZVelocity(Int x, Int y) override;

public:  // Flat tile texture info.
	TerrainTextureClass* getFlatTexture(Int xCell, Int yCell, Int cellWidth, Int pixelsPerCell);  //< generates and returns the terrain texture

	static void setupAlphaTiles();
	UnsignedByte* getPointerToTileData(Int xIndex, Int yIndex, Int width);
	Bool getRawTileData(Short tileNdx, Int width, UnsignedByte* buffer, Int bufLen);
  UnsignedByte* getRGBAlphaDataForWidth(Int width, const TBlendTileInfo* pBlend);

public:  // modify height value
	void setRawHeight(Int xIndex, Int yIndex, UnsignedByte height) {
		Int ndx = (yIndex * m_width) + xIndex;
		if ((ndx >= 0) && (ndx < m_dataSize) && m_data) m_data[ndx] = height;
	};

public: // Read tile utilities. jba [7/9/2003]
	static Int getMaxTextureSheetWidthInTiles();
	static Bool readTiles(InputStream* pStrm, TileData** tiles, Int numRows, Int tilePixelExtent = TILE_PIXEL_EXTENT);
	static Int countTiles(InputStream* pStrm, Bool* halfTile = nullptr);

protected:
	void setCliffState(Int xIndex, Int yIndex, Bool state);
};
