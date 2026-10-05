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

// FILE: W3DWater.h ///////////////////////////////////////////////////

#pragma once

// Ronin @build 27/10/2025 DX9: Include DX9 headers and define DX8 compatibility types
#include <d3d9.h>

// Ronin @build 27/10/2025 DX9: Guard typedefs to prevent redefinition errors
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
typedef IDirect3DVertexBuffer9 IDirect3DVertexBuffer8;
typedef IDirect3DIndexBuffer9 IDirect3DIndexBuffer8;

// Ronin @build 27/10/2025 DX9: Define LPDIRECT3D* pointer typedefs
typedef IDirect3DDevice9* LPDIRECT3DDEVICE8;
typedef IDirect3DVertexBuffer9* LPDIRECT3DVERTEXBUFFER8;
typedef IDirect3DIndexBuffer9* LPDIRECT3DINDEXBUFFER8;
typedef IDirect3DTexture9* LPDIRECT3DTEXTURE8;

#endif // DX8_TO_DX9_TYPEDEFS_DEFINED

#include "WWLib/always.h"
#include "WW3D2/rendobj.h"
#include "WW3D2/w3d_file.h"
#include "WW3D2/dx8vertexbuffer.h"
#include "WW3D2/dx8indexbuffer.h"
#include "WW3D2/shader.h"
#include "WW3D2/vertmaterial.h"
#include "WW3D2/light.h"
#include "Common/GameType.h"
#include "Common/Snapshot.h"

#include <cstddef>

#define INVALID_WATER_HEIGHT 0.0f	///water height guaranteed to be below all terrain.

#define NUM_BUMP_FRAMES 32	///number of animation frames in bump map
//Offsets in constant register file to Vertex shader constants
#define CV_ZERO 0
#define CV_ONE 1
#define CV_WORLDVIEWPROJ_0 2
#define CV_TEXPROJ_0 6
#define CV_PATCH_SCALE_OFFSET 10

class PolygonTrigger;
class WaterTracksRenderSystem;
class Xfer;
/// Custom render object that draws mirrors, water, and skies.
/**
This render object handles drawing reflected W3D scenes.  It will only work
with rectangular planar surfaces and was tuned with an emphasis on water.
Since skies are only visible in reflections, this code will also
render clouds and sky bodies.
*/
class WaterRenderObjClass : public Snapshot,
														public RenderObjClass
{

public:

	enum WaterType
	{
		WATER_TYPE_0_TRANSLUCENT = 0,	//translucent water, no reflection
		WATER_TYPE_1_FB_REFLECTION,		//legacy frame buffer reflection (non translucent)
		WATER_TYPE_2_PVSHADER,		//pixel/vertex shader, texture reflection
		WATER_TYPE_3_GRIDMESH,		//3D Mesh based water
	};

	WaterRenderObjClass();
	virtual ~WaterRenderObjClass() override;

	/////////////////////////////////////////////////////////////////////////////
	// Render Object Interface (W3D methods)
	/////////////////////////////////////////////////////////////////////////////
	virtual RenderObjClass *	Clone() const override;
	virtual int						Class_ID() const override;
	virtual void					Render(RenderInfoClass & rinfo) override;
/// @todo: Add methods for collision detection with water surface
//	virtual Bool					Cast_Ray(RayCollisionTestClass & raytest);
//	virtual Bool					Cast_AABox(AABoxCollisionTestClass & boxtest);
//	virtual Bool					Cast_OBBox(OBBoxCollisionTestClass & boxtest);
//	virtual Bool					Intersect_AABox(AABoxIntersectionTestClass & boxtest);
//	virtual Bool					Intersect_OBBox(OBBoxIntersectionTestClass & boxtest);

	virtual void					Get_Obj_Space_Bounding_Sphere(SphereClass & sphere) const override;
    virtual void					Get_Obj_Space_Bounding_Box(AABoxClass & aabox) const override;
	// Get and set static sort level
	virtual int		Get_Sort_Level() const override { return m_sortLevel; }
  	virtual void	Set_Sort_Level(int level) override { m_sortLevel = level;}

	///allocate W3D resources needed to render water
	void renderWater();				///<draw the water surface (flat)
	Int init(Real waterLevel, Real dx, Real dy, SceneClass *parentScene, WaterType type);
	void reset();  ///< reset any resources we need to
	void load();	///< load/setup any map dependent features
	void update(); ///< update phase of the water
	void enableWaterGrid(Bool state);	///< used to active custom water for special maps. (i.e DAM).
	void updateMapOverrides();	///< used to update any map specific map overrides for water appearance.
	void setTimeOfDay(TimeOfDay tod); ///<change sky/water for time of day
	void toggleCloudLayer(Bool state)	{	m_useCloudLayer=state;}	///<enables/disables the cloud layer
	void updateRenderTargetTextures(CameraClass *cam);	///< renders into any required textures.
	void ReleaseResources();	///< Release all dx8 resources so the device can be reset.
	void ReAcquireResources();  ///< Reacquire all resources after device reset.
	Real getWaterHeight(Real x, Real y);	///<return water height at given point - for use by WB.
	// Ronin @feature 03/10/2026 DX9: phase 5 - the swell on the CPU, as the vertex shader draws it, for units that float.
	Bool standingWater(Real x, Real y, Int *kind, Real *level) const;	///< a sea or lake polygon over this point: its kind and flat level
	Real swellHeight(Real x, Real y, Int kind, Real level) const;		///< how far the surface stands above that level there
	void rideSwell(Matrix3D &mtx, Real halfLength, Real halfWidth, Int kind, Real level) const;	///< lift and tilt a floating unit's transform by it
	// Ronin @feature 03/10/2026 DX9: phase 5 - wakes: a unit in the water this frame - its trail grows (`wake`), then it
	// rides the swell.
	void floatUnit(UnsignedInt drawableID, Matrix3D &mtx, Real halfLength, Real halfWidth, Bool wake);
	Bool ownsWake(UnsignedInt drawableID) const;	///< the water draws this unit's wake: its vanilla wake sprites can go
	void setGridHeightClamps(Real minz, Real maxz);	///<set min/max height values alllowed in grid
	void addVelocity( Real worldX, Real worldY, Real zVelocity, Real preferredHeight );	///< add velocity value
	void changeGridHeight(Real x, Real y, Real delta);	///<change height of grid at world point including neighbors within falloff.
	void setGridChangeAttenuationFactors(Real a, Real b, Real c, Real range);	///<adjust falloff parameters for grid change method.
	void setGridTransform(Real angle, Real x, Real y, Real z);	///<positoin/orientation of grid in space
	void setGridTransform(const Matrix3D *transform);	///< set transform by matrix
	void getGridTransform(Matrix3D *transform);	///< get grid transform matrix
	void setGridResolution(Real gridCellsX, Real gridCellsY, Real cellSize);	///<grid resolution and spacing
	void getGridResolution(Real *gridCellsX, Real *gridCellsY, Real *cellSize);  ///<get grid resolution params
	inline void setGridVertexHeight(Int x, Int y, Real value);
	void getGridVertexHeight(Int x, Int y, Real *value)
	{	if (m_meshData)	*value=m_meshData[(y+1)*(m_gridCellsX+1+2)+x+1].height+ Get_Position().Z;}
	inline Bool worldToGridSpace(Real worldX, Real worldY, Real &gridX, Real &gridY);	///<convert from world coordinates to grid's local coordinate system.

	void replaceSkyboxTexture(const AsciiString& oldTexName, const AsciiString& newTextName);

	// Ronin @bugfix 26/12/2025: Cleanup function to prevent texture stage state pollution
	void cleanupWaterShaderState(void);

protected:
	DX8IndexBufferClass			*m_indexBuffer;	///<indices defining quad
	SceneClass							*m_parentScene;	///<scene to be reflected
	ShaderClass m_shaderClass; ///<shader or rendering state for heightmap
	VertexMaterialClass	  		*m_vertexMaterialClass;	///<vertex lighting material
	VertexMaterialClass			*m_meshVertexMaterialClass; ///<vertex lighting marial for 3D water.
	LightClass					*m_meshLight;				///<light used for 3D Mesh Water.
	TextureClass *m_alphaClippingTexture;	///<used for faked clipping using alpha
	Real	m_dx;	///<x extent of water surface (offset from local center)
	Real	m_dy;	///<y extent of water surface (offset from local center)
	Vector3 m_planeNormal;		///<water plane normal
	Real		m_planeDistance;	///<water plane distance
	Real		m_level;			///<level of water (hack for water)
	Real		m_uOffset;			///<current texture offset on u axis
	Real		m_vOffset;			///<current texture offset on v axis
	Real		m_uScrollPerMs;		///<texels per/ms scroll rate in u direction
	Real		m_vScrollPerMs;		///<texels per/ms scroll rate in v direction
	Int			m_LastUpdateTime;	///<time of last cloud update
	Bool		m_useCloudLayer;	///<flag if clouds are on/off
	WaterType	m_waterType;		///<type of water being used
	Int			m_sortLevel;		///<sort order after main scene is rendered

	//Data used in GeForce3 bump-mapped water (uses direct D3D resources for better
	//performance and compatibility (most of these featues are not supported by W3D).
	struct SEA_PATCH_VERTEX	//vertex structure passed to D3D
	{
		float x,y,z;
		unsigned int c;
		float tu, tv;
	};

	static_assert(sizeof(WaterRenderObjClass::SEA_PATCH_VERTEX) == 24, "SEA_PATCH_VERTEX must be 24 bytes");
	static_assert(offsetof(WaterRenderObjClass::SEA_PATCH_VERTEX, x) == 0, "SEA_PATCH_VERTEX.x offset must be 0");
	static_assert(offsetof(WaterRenderObjClass::SEA_PATCH_VERTEX, c) == 12, "SEA_PATCH_VERTEX.c offset must be 12");
	static_assert(offsetof(WaterRenderObjClass::SEA_PATCH_VERTEX, tu) == 16, "SEA_PATCH_VERTEX.tu offset must be 16");


	LPDIRECT3DDEVICE8 m_pDev;						///<pointer to D3D Device
	LPDIRECT3DVERTEXBUFFER8 m_vertexBufferD3D;		///<D3D vertex buffer
	LPDIRECT3DINDEXBUFFER8	m_indexBufferD3D;	///<D3D index buffer
	Int						m_vertexBufferD3DOffset;	///<location to start writing vertices
	// Ronin @build DX9: Changed from DWORD handles to native DX9 shader pointers
	IDirect3DPixelShader9*	m_dwWavePixelShader;	///<DX9 pixel shader pointer
	// Ronin @feature 02/10/2026 DX9: phase 4 - the WaterSea variants beside m_dwWavePixelShader (standing water): with the
	// debug views, rivers, rivers with the views. Any may be missing; pickWavePS falls back.
	IDirect3DPixelShader9*	m_wavePSViews;
	IDirect3DPixelShader9*	m_wavePSRiver;
	IDirect3DPixelShader9*	m_wavePSRiverViews;
	Int						m_bindKind;			///< the WaterKind bindWaterSea last bound, for bindReflectiveShaders
	IDirect3DPixelShader9*	pickWavePS(Int kind) const;
	IDirect3DVertexShader9*	m_dwWaveVertexShader;	///<DX9 vertex shader pointer
	Int	m_numVertices;				///<number of vertices in D3D vertex buffer
	Int m_numIndices;				///<number of indices in D3D index buffer
	LPDIRECT3DTEXTURE8 m_pBumpTexture[NUM_BUMP_FRAMES]; ///<animation frames
	LPDIRECT3DTEXTURE8 m_pBumpTexture2[NUM_BUMP_FRAMES]; ///<animation frames
	Real				m_fBumpFrame;	///<current animation frame
	Real				m_fBumpScale;	///<scales bump map uv perturbation
	TextureClass * m_pReflectionTexture;	///<render target for reflection
	RenderObjClass	*m_skyBox;		///<box around level
	WaterTracksRenderSystem *m_waterTrackSystem;	///<object responsible for rendering water wakes
	// Ronin @build DX9: Changed from DWORD handles to native DX9 shader pointers
	IDirect3DPixelShader9* m_waterPixelShader;		///<DX9 pixel shader pointer
	IDirect3DPixelShader9* m_riverWaterPixelShader;		///<DX9 pixel shader pointer
	IDirect3DPixelShader9* m_trapezoidWaterPixelShader;	///<DX9 pixel shader pointer
	// Ronin @bugfix 30/11/2025: Cached vertex declarations for water shaders
	IDirect3DVertexDeclaration9* m_riverWaterDecl;			///<DX9 vertex declaration for river water
	IDirect3DVertexDeclaration9* m_seaWaterDecl;			///<DX9 vertex declaration for sea/ocean water
	IDirect3DVertexDeclaration9* m_trapezoidWaterDecl;		///<DX9 vertex declaration for trapezoid water

	enum WaterMeshStatus
	{
		AT_REST = 0x00,
		IN_MOTION = 0x01
	};
	struct WaterMeshData
	{
		Real height;										///< height of the 3D mesh at this point
		Real velocity;									///< velocity in Z that this point is moving up and down
		UnsignedByte status;						///< status for this grid point
		UnsignedByte preferredHeight;		///< the hight we prefer to be
	};
	WaterMeshData *m_meshData;  ///< heightmap data for 3D Mesh based water.
	UnsignedInt m_meshDataSize;	///< size of m_meshData
	Bool m_meshInMotion;				///< TRUE once we've messed with velocities and are in motion
	Bool m_doWaterGrid;	///< allows/prevents water grid rendering.

	Vector2	m_gridDirectionX;			///<vector along water grid's x-axis (scaled to world-space)
	Vector2	m_gridDirectionY;			///<vector along water grid's y-axis (scaled to world-space)
	Vector2	m_gridOrigin;				///<unit vector along water grid's x-axis
	Real m_gridWidth;					///<object-space width of water grid
	Real m_gridHeight;					///<object-space width of water grid
	Real m_minGridHeight;				///<minimum height value allowed for water mesh
	Real m_maxGridHeight;				///<maximum height value allowed for water mesh
	Real m_gridChangeMaxRange;			///<maximum range of changeGridHeight() method
	Real m_gridChangeAtt0;
	Real m_gridChangeAtt1;
	Real m_gridChangeAtt2;
	Real m_gridCellSize;				///<world-space width/height of each cell.
	Int  m_gridCellsX;					///<number of cells along width
	Int  m_gridCellsY;					///<number of cells along height

	Real m_riverVOrigin;
	TextureClass *m_riverTexture;
	TextureClass *m_whiteTexture;		///< a texture containing only white used for null pixel shader stages.
	TextureClass *m_waterNoiseTexture;
	TextureClass *m_waterSparklesTexture;
	TextureClass *m_waterNormalTexture;	///< Ronin @feature 28/09/2026 DX9: WaterSea surface normals, WaterNormal.tga
	TextureClass *m_waterNormalLake;	///< Ronin @feature 02/10/2026 DX9: phase 4 - the lake's calm ripples, WaterNormalLake.tga
	TextureClass *m_waterNormalRiver;	///< Ronin @feature 02/10/2026 DX9: phase 4 - the river's ripples, WaterNormalRiver.tga
	// Ronin @feature 29/09/2026 DX9: phase 1 - the terrain heights as a texture (D3DPOOL_MANAGED), built per map in load().
	IDirect3DTexture9 *m_heightTexture;
	Int m_heightTexW, m_heightTexH, m_heightBorder;
	Bool m_heightVTF;			///< Ronin @feature 29/09/2026 DX9: phase 2 - R32F the vertex shader can read (else L8, no swell)
	void buildHeightTexture();	///< from TheTerrainRenderObject's height map; releases any previous one
	// Ronin @feature 29/09/2026 DX9: phase 3 - the frame just before the flat water draws, for its refraction.
	TextureClass *m_refractionTexture;	///< POOL_DEFAULT render target, the size of the current target
	UnsignedInt m_refractionW, m_refractionH;
	Bool m_refractionOK;		///< this frame's copy succeeded
	void copyRefraction();		///< copy render target 0 into it, when the WaterSea shader will read it
	void ensureReflectionTarget();	///< Ronin @diagnostic 29/09/2026 DX9: the mirror at `mirrorres` size
	// Ronin @bugfix 01/10/2026 DX9: sea (a standing polygon reaching the map's edge) or lake, for the WaterSea opacity.
	Bool m_standingIsSea;			///< the standing polygon renderWater is drawing
	Bool isSeaPolygon(PolygonTrigger *pTrig) const;
	Real m_swellCellNow;			///< Ronin @bugfix 03/10/2026 DX9: the swell grid's real cell under the draw being bound, world units
	Real m_swellCellSea;			///< Ronin @feature 03/10/2026 DX9: phase 5 - the last real cell drawn in view, per kind:
	Real m_swellCellLake;			///< swellHeight moves by the waves the grid carried
	// Ronin @feature 03/10/2026 DX9: phase 5 - wakes. Each unit moving in the water keeps a trail of where its centre has
	// been (128 points); once a frame the trails are drawn as ribbons into m_wakeTexture, which the WaterSea shaders read.
	enum { WAKE_TRAILS = 24, WAKE_POINTS = 128 };
	struct WakePoint
	{
		Real x, y;			///< where the unit's centre was
		Real time;			///< the water clock then, seconds
		Real path;			///< the trail's length to here
		Real strength;		///< from the unit's speed, size and the depth there
	};
	struct WakeTrail
	{
		UnsignedInt id;		///< the unit's DrawableID, 0 = free
		Real seen;			///< the water clock when it last drew
		Real halfLength, halfWidth;
		Real lastX, lastY;	///< its centre last frame
		Real speed;			///< smoothed, world units a second
		Real lead;			///< +1 the bow leads, -1 the stern (reversing)
		Real headX, headY;	///< its centre now
		Real dirX, dirY;	///< the way it travels now: its heading x lead
		Real markX, markY;	///< that, at the last point dropped
		Real headStrength;
		Real idle;			///< Ronin @feature 04/10/2026 DX9: 0 moving .. 1 at rest, eased: how strong its rings are
		Real idleScale;		///< their height by its beam and the depth
		Real peak;			///< how fast it has lately been, as a share of a boat's full speed: a surge when it stops
		Int  first, count;	///< a ring of points, oldest first
		WakePoint pt[WAKE_POINTS];
	};
	WakeTrail m_wake[WAKE_TRAILS];
	// Ronin @feature 04/10/2026 DX9: a ring leaving a hull: the surge ahead of a unit that stopped. Drawn into the wake
	// texture with the trails (a resting unit's rings are drawn from its trail's `idle`).
	enum { WAKE_RIPPLES = 48 };
	struct WakeRipple
	{
		Real x, y;			///< the hull's centre when it left
		Real dirX, dirY;	///< the hull's heading then
		Real halfSeg;		///< half the hull's straight part: the ring keeps the hull's outline
		Real start;			///< its distance from that at birth
		Real time, life;	///< born (water clock), seconds it lasts; life 0 = free
		Real speed, height;
		Real ahead;			///< 0 all round (a ring), 1 ahead only (a surge)
	};
	WakeRipple m_ripple[WAKE_RIPPLES];
	Int m_rippleNext;			///< the next to write: the oldest goes
	void addRipple(Real x, Real y, Real dirX, Real dirY, Real halfLength, Real halfWidth, Real now, Real life, Real speed, Real height, Real ahead);
	IDirect3DTexture9 *m_wakeTexture;		///< POOL_DEFAULT render target, A16B16G16R16F: xy slope, z foam, w height
	IDirect3DVertexShader9 *m_wakeVS;		///< WaterWake.vso / .pso: one trail's ribbon into it
	IDirect3DPixelShader9  *m_wakePS;
	Int  m_wakeFormat;			///< -1 not asked; 0 the card cannot; 1 the PS reads it; 2 the VS too (the mesh moves)
	Bool m_wakeLive;			///< this frame's texture holds a wake: bindWaterSea hands it to the standing water
	Real m_wakeScale, m_wakeOffsetU, m_wakeOffsetV;	///< world xy -> its uv
	void noteWake(UnsignedInt drawableID, const Matrix3D &mtx, Real halfLength, Real halfWidth, Real depth);
	void renderWakes();			///< once a frame, before the water draws
	void clearWakes();
	Real m_riverXOffset;
	Real m_riverYOffset;
	Bool m_drawingRiver;
	Bool m_disableRiver;
	TextureClass *m_riverAlphaEdge;

	TimeOfDay m_tod;	///<time of day setting for reflected cloud layer

	struct Setting
	{
		TextureClass	*skyTexture;
		TextureClass	*waterTexture;
		Int				waterRepeatCount;
		Real			skyTexelsPerUnit;	//texel density of sky plane (higher value repeats texture more).
		DWORD			vertex00Diffuse;
		DWORD			vertex10Diffuse;
		DWORD			vertex11Diffuse;
		DWORD			vertex01Diffuse;
		DWORD			waterDiffuse;
		DWORD			transparentWaterDiffuse;
		Real			uScrollPerMs;
		Real			vScrollPerMs;
	};

	Setting m_settings[ TIME_OF_DAY_COUNT ];	///< settings for each time of day
	void drawRiverWater(PolygonTrigger *pTrig);
	void drawTrapezoidWater(Vector3 points[4]);
	void loadSetting ( Setting *skySetting, TimeOfDay timeOfDay );	///<init sky/water settings from GDF
	void renderSky();	///<draw the sky layer (clouds, stars, etc.)
	void testCurvedWater();	///<draw the sky layer (clouds, stars, etc.)
	void renderSkyBody(Matrix3D *mat);	///<draw the sky body (sun, moon, etc.)
	void renderWaterMesh();			///<draw the water surface mesh (deformed 3d mesh).
	HRESULT initBumpMap(LPDIRECT3DTEXTURE8 *pTex, TextureClass *pBumpSource);	///<copies data into bump-map format.
	void renderMirror(CameraClass *cam);	///< Draw reflected scene into texture
	void drawSea(RenderInfoClass & rinfo);	///< Draw the surface of the water
	///bounding box of frustum clipped polygon plane
	Bool getClippedWaterPlane(CameraClass *cam, AABoxClass *box);

	void setupFlatWaterShader();
	void setupJbaWaterShader();
	void cleanupJbaWaterShader();
	// Ronin @feature 28/09/2026 DX9: reflective water (WaterType 2) on the map's own polygons, WaterSea_vs/_ps.
	Bool useReflection() const;					///< WaterType 2 with both sea shaders and the reflection target
	void setupReflectiveWater(Bool river);		///< state, textures, constants; the shaders go on in bindReflectiveShaders
	void bindWaterSea(IDirect3DBaseTexture9 *water, IDirect3DBaseTexture9 *edge, Real edgeScaleU, Bool riverShroud, Bool standing);	///< fill and bind the W3DWaterSea.h inputs this object owns
	void bindReflectiveShaders();				///< after the draw's BindLayoutFVF, which clears the vertex shader
	void unbindReflectiveShaders(DWORD fvf);	///< back to fixed function, texture references dropped

	//Methods used for GeForce3 specific water
	HRESULT generateIndexBuffer(int sizeX, int sizeY);	///<Generate static index buufer
	HRESULT generateVertexBuffer( Int sizeX, Int sizeY, Int vertexSize, Bool doFill);///<Generate static vertex buffer

	// snapshot methods for save/load
	virtual void crc( Xfer *xfer ) override;
	virtual void xfer( Xfer *xfer ) override;
	virtual void loadPostProcess() override;

};

//Public inline function declarations
inline Bool WaterRenderObjClass::worldToGridSpace(Real worldX, Real worldY, Real &gridX, Real &gridY)
{
	Real dx,dy;
	Real ooGridCellSize = 1.0f/m_gridCellSize;

	dx=worldX - m_gridOrigin.X;
	dy=worldY - m_gridOrigin.Y;
	gridX = ooGridCellSize * (dx * m_gridDirectionX.X + dy * m_gridDirectionX.Y);
	gridY = ooGridCellSize * (dx * m_gridDirectionY.X + dy * m_gridDirectionY.Y);

	if (gridX < 0)
		return FALSE;
	if (gridX > m_gridCellsX-1 )
		return FALSE;
	if (gridY < 0)
		return FALSE;
	if (gridY > m_gridCellsY-1 )
		return FALSE;

	return TRUE;
}

extern WaterRenderObjClass *TheWaterRenderObj; ///<global water rendering object

// Ronin @feature 15/09/2026 DX9: water switches for the debug panel `water` command (dev tool). The defaults draw normally.
struct WaterDebugFlags
{
	Bool skipFlat;		///< skip renderWater: flat water areas and rivers
	Bool skipMesh;		///< skip renderWaterMesh: the deforming water grid
	// Ronin @feature 28/09/2026 DX9: shore waves, Water_Work.md F5.
	Bool skipWaves;		///< skip WaterTracksRenderSystem::flush: the shore waves
	// Ronin @feature 28/09/2026 DX9: WaterType 2. The look knobs live in W3DWaterSeaTuning.h (29/09).
	Bool seaPlane;		///< draw the original infinite sea plane (drawSea) instead of the map's water polygons
};
extern WaterDebugFlags TheWaterDebug;

// Ronin @diagnostic 28/09/2026 DX9: [WATER] panel row. W3DWater.cpp writes it; the panel reads it, zeroes the draw counts
// every frame and sets `enabled`. docs/Water_Work.md §5 step 1.
struct WaterDebugStats
{
	enum { PS_NOT_TRIED = 0, PS_ASM_FAILED, PS_CREATE_FAILED, PS_OK };
	Bool        enabled;				///< count draws and check the bound shader only while TRUE
	Int         flatPSStage;			///< m_trapezoidWaterPixelShader's last creation attempt, a PS_ value
	Int         riverPSStage;			///< m_riverWaterPixelShader's last creation attempt
	HRESULT     flatPSHr;				///< result of the call that failed
	HRESULT     riverPSHr;
	UnsignedInt flatDraws,  flatPSDraws;	///< this frame's draws / of them, with the water shader bound
	UnsignedInt riverDraws, riverPSDraws;
	UnsignedInt meshDraws,  meshPSDraws;
	// Ronin @feature 28/09/2026 DX9: WaterType 2 sea (WaterSea_vs/_ps).
	Int         seaVSStage, seaPSStage;	///< PS_OK, PS_CREATE_FAILED (load or create) or PS_NOT_TRIED
	HRESULT     seaVSHr, seaPSHr;
	Int         bumpFrames;				///< caust frames initBumpMap built, of NUM_BUMP_FRAMES
	UnsignedInt seaDraws, seaPSDraws;
	Real        seaLevel;				///< the sea plane's height, set per map in load()
	Bool        seaLevelFromMap;		///< TRUE = a water area's height, FALSE = GameData WaterPositionZ
	Int         normalMap;				///< WaterNormal.tga: 2 loaded, 1 missing (WW3D's placeholder), 0 not loaded
	Int         normalMapLake;			///< Ronin @feature 02/10/2026 DX9: WaterNormalLake.tga, the same codes
	Int         normalMapRiver;			///< Ronin @feature 02/10/2026 DX9: WaterNormalRiver.tga, the same codes
	Int         psVariants;				///< Ronin @feature 02/10/2026 DX9: phase 4 - WaterSea pixel shader builds loaded, of 4
	Int         heightW, heightH;		///< Ronin @feature 29/09/2026 DX9: the terrain-height texture, 0 x 0 = none
	Bool        heightVTF;				///< Ronin @feature 29/09/2026 DX9: phase 2 - it is R32F and vertex-readable (the swell)
	UnsignedInt flatVerts;				///< Ronin @feature 29/09/2026 DX9: this frame's flat-water grid vertices
	Real        flatCell;				///< Ronin @bugfix 03/10/2026 DX9: the swell grid's largest real cell in view this frame, 0 = no swell grid
	Int         refraction;				///< Ronin @feature 29/09/2026 DX9: phase 3 - 0 off, 1 copy failed, 2 ok
	Int         mirrorW, mirrorH;		///< Ronin @diagnostic 29/09/2026 DX9: the mirror target's size
	UnsignedInt seaPolys, lakePolys;	///< Ronin @diagnostic 01/10/2026 DX9: this frame's standing polygons by kind
	// Ronin @feature 03/10/2026 DX9: phase 5 - wakes
	Int         wakeState;				///< 0 off or no shaders, 1 the card cannot, 2 the PS reads the texture, 3 the mesh moves too
	UnsignedInt wakeTrails, wakeVerts;	///< this frame's trails drawn and their ribbon vertices
	UnsignedInt wakeRipples;			///< Ronin @feature 04/10/2026 DX9: this frame's rings and surges drawn
	Real        wakeTexel;				///< world units a wake texel this frame, 0 = none drawn
};
extern WaterDebugStats TheWaterStats;

