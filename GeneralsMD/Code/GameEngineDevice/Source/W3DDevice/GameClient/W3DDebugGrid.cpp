/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 debug grid — the snap cells the debug panel places on, drawn over the terrain.
*/

// Ronin @feature 16/09/2026 DX9: the lines follow the ground, so they are terrain geometry, not a flat quad: strips laid on
// the height map. Same fixed-function alpha path the move hints use — no new shader.
//
// The plain cells show only a tick at each corner, which reads as a grid without drawing a cage over the battlefield. The
// cells the drop will land on are outlined and filled instead, so the colour says where the objects are going.

#include "WW3D2/dx8todx9.h"

#include <math.h>

#include "Common/GameMemory.h"
#include "GameClient/Display.h"
#include "W3DDevice/GameClient/BaseHeightMap.h"
#include "W3DDevice/GameClient/W3DDebugGrid.h"
#include "W3DDevice/GameClient/W3DDisplay.h"
#include "W3DDevice/GameClient/W3DScene.h"
#include "WW3D2/camera.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/rinfo.h"
#include "WW3D2/shader.h"
#include "WW3D2/vertmaterial.h"

// Ronin @feature 16/09/2026 DX9: the look, live, set by the panel's `grid` command. These are the defaults.
DebugGridTuning TheDebugGrid = { 0.5f, 1.25f, 0.6f, 0 };

namespace
{
	// 20 is 41x41 cells. The cap is not taste: every strip costs 6 vertices and 12 indices, and the index buffer counts in
	// unsigned shorts, so a much wider patch would overrun it.
	// 11 is 23x23 cells. The cap is not taste: each corner costs eight strips and each strip 12 indices, and the index buffer
	// counts in unsigned shorts, so a wider patch would overrun it.
	const Int  GRID_MAX_RADIUS  = 11;
	const Int  GRID_MAX_SPOTS   = 64;		// the panel drops at most 30 at a time; the rest is headroom
	const Int  GRID_ROW         = 2 * GRID_MAX_RADIUS + 1;
	const Real GRID_SOLID_TO    = 0.6f;		// full strength out to this share of the radius, then it fades to nothing
	const Real GRID_MIN_HALF    = 0.05f;	// world units, so a line never collapses to nothing up close
	const Real GRID_MAX_HALF    = 2.0f;		// and never turns into a road when the camera pulls back
	// Ronin @feature 28/09/2026 DX9: arms a third shorter (0.26 -> 0.18), measured from the hole's edge.
	const Real GRID_TICK        = 0.18f;	// how far the mark's arms reach into the cell, as a share of the cell
	// Ronin @feature 28/09/2026 DX9: gap between tiles halved (inset 0.08 -> 0.04); the hole follows, so the arms stay 0.01 of a
	// cell outside the tile edges they frame.
	const Real GRID_TICK_GAP    = 0.03f;	// half the square hole in its middle, which is also how far apart the arms run
	const Real GRID_TILE_INSET  = 0.04f;	// a marked cell is drawn as its own square, this far inside the cell
	const Int  GRID_MARGIN      = 3;		// cells of plain grid kept around whatever is being placed
	const Real GRID_FILL_ALPHA  = 0.22f;	// the drop's tiles, relative to the line alpha
	const Real GRID_FILL_LIFT   = 0.5f;		// the fill sits under the lines by this share of the lift
	// Ronin @feature 27/09/2026 DX9: lines and tiles are split at the height-map spacing (MAP_XY_FACTOR), so a cell wider
	// than one height-map cell still follows the ground instead of cutting through it between its corners.
	const Real GRID_DRAPE_STEP  = 10.0f;

	inline UnsignedInt lerpColor(UnsignedInt a, UnsignedInt b, Real t)
	{
		if (a == b)
			return a;
		UnsignedInt out = 0;
		for (Int shift = 0; shift < 32; shift += 8)
		{
			const Real ca = (Real)((a >> shift) & 0xFF);
			const Real cb = (Real)((b >> shift) & 0xFF);
			out |= ((UnsignedInt)(ca + (cb - ca) * t + 0.5f) & 0xFF) << shift;
		}
		return out;
	}

	inline Int drapePieces(Real length)
	{
		return (length > GRID_DRAPE_STEP) ? (Int)ceilf(length / GRID_DRAPE_STEP) : 1;
	}

	inline UnsignedInt gridColor(Int r, Int g, Int b, Real alpha)
	{
		Int a = (Int)(alpha * 255.0f);
		if (a < 0)   a = 0;
		if (a > 255) a = 255;
		return ((UnsignedInt)a << 24) | ((UnsignedInt)r << 16) | ((UnsignedInt)g << 8) | (UnsignedInt)b;
	}

	Real groundAt(Real x, Real y)
	{
		return (TheTerrainRenderObject != nullptr) ? TheTerrainRenderObject->getHeightMapHeight(x, y, nullptr) : 0.0f;
	}

	// Cells outside the patch count as unmarked, so the edge of the patch needs no special case.
	inline Bool isMarked(const Bool *marked, Int ix, Int iy, Int radius)
	{
		if (ix < -radius || ix >= radius || iy < -radius || iy >= radius)
			return FALSE;
		return marked[(ix + radius) * GRID_ROW + (iy + radius)];
	}

	// 1 out to GRID_SOLID_TO of the radius, then down to 0 at the rim.
	Real rimFade(Real x, Real y, const Coord3D &centre, Real reach)
	{
		const Real distSq = (x - centre.x) * (x - centre.x) + (y - centre.y) * (y - centre.y);
		const Real solid  = reach * GRID_SOLID_TO;
		if (distSq <= solid * solid)
			return 1.0f;
		const Real dist = sqrtf(distSq);
		if (dist >= reach)
			return 0.0f;
		return (reach - dist) / (reach - solid);
	}

	//-------------------------------------------------------------------------------------------------
	// Ronin @feature 16/09/2026 DX9: writes the geometry as it walks the patch. Counting and filling run the same loops, so
	// the count pass leaves vb/ib null and only tallies — the two can never disagree about how much room is needed.
	struct GridBuilder
	{
		VertexFormatXYZDUV1 *vb;
		UnsignedShort       *ib;
		Int                  vertices;
		Int                  indices;

		Vector3              camPos;
		Real                 worldPerPx;
		Real                 seenX, seenY;		// how much of each axis's width the camera can actually see
		Real                 lift;

		void reset(VertexFormatXYZDUV1 *v, UnsignedShort *i)
		{
			vb = v; ib = i; vertices = 0; indices = 0;
		}

		Real halfWidthAt(Real mx, Real my, Real mz, Bool alongX) const
		{
			const Real dist = sqrtf((mx - camPos.X) * (mx - camPos.X) +
									(my - camPos.Y) * (my - camPos.Y) +
									(mz - camPos.Z) * (mz - camPos.Z));
			Real half = TheDebugGrid.halfWidthPixels * worldPerPx * dist / (alongX ? seenX : seenY);
			if (half < GRID_MIN_HALF) half = GRID_MIN_HALF;
			if (half > GRID_MAX_HALF) half = GRID_MAX_HALF;
			return half;
		}

		// A line from A to B, in pieces no longer than GRID_DRAPE_STEP so it follows the ground.
		void line(Real ax, Real ay, Real bx, Real by, Bool alongX, UnsignedInt solidA, UnsignedInt solidB)
		{
			const Int n = drapePieces(sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay)));
			if (vb == nullptr)
			{
				vertices += 6 * n;
				indices  += 12 * n;
				return;
			}
			for (Int k = 0; k < n; ++k)
			{
				const Real t0 = (Real)k / (Real)n;
				const Real t1 = (Real)(k + 1) / (Real)n;
				piece(ax + (bx - ax) * t0, ay + (by - ay) * t0, ax + (bx - ax) * t1, ay + (by - ay) * t1, alongX,
					  lerpColor(solidA, solidB, t0), lerpColor(solidA, solidB, t1));
			}
		}

		// One straight piece: three vertices across at each end — clear, solid, clear — so it anti-aliases itself.
		void piece(Real ax, Real ay, Real bx, Real by, Bool alongX, UnsignedInt solidA, UnsignedInt solidB)
		{

			const Real mx = (ax + bx) * 0.5f;
			const Real my = (ay + by) * 0.5f;
			const Real mz = groundAt(mx, my);
			const Real half = halfWidthAt(mx, my, mz, alongX);
			const Real wx = alongX ? half : 0.0f;		// the width runs across the line
			const Real wy = alongX ? 0.0f : half;

			const UnsignedInt clear = (solidA & 0x00FFFFFF);
			const Real za = groundAt(ax, ay) + lift;
			const Real zb = groundAt(bx, by) + lift;

			const Int v = vertices;
			vb[v + 0].x = ax - wx; vb[v + 0].y = ay - wy; vb[v + 0].z = za; vb[v + 0].diffuse = clear;  vb[v + 0].u1 = 0.0f; vb[v + 0].v1 = 0.0f;
			vb[v + 1].x = ax;      vb[v + 1].y = ay;      vb[v + 1].z = za; vb[v + 1].diffuse = solidA; vb[v + 1].u1 = 0.0f; vb[v + 1].v1 = 0.0f;
			vb[v + 2].x = ax + wx; vb[v + 2].y = ay + wy; vb[v + 2].z = za; vb[v + 2].diffuse = clear;  vb[v + 2].u1 = 0.0f; vb[v + 2].v1 = 0.0f;
			vb[v + 3].x = bx - wx; vb[v + 3].y = by - wy; vb[v + 3].z = zb; vb[v + 3].diffuse = clear;  vb[v + 3].u1 = 0.0f; vb[v + 3].v1 = 0.0f;
			vb[v + 4].x = bx;      vb[v + 4].y = by;      vb[v + 4].z = zb; vb[v + 4].diffuse = solidB; vb[v + 4].u1 = 0.0f; vb[v + 4].v1 = 0.0f;
			vb[v + 5].x = bx + wx; vb[v + 5].y = by + wy; vb[v + 5].z = zb; vb[v + 5].diffuse = clear;  vb[v + 5].u1 = 0.0f; vb[v + 5].v1 = 0.0f;

			const Int i = indices;
			const UnsignedShort base = (UnsignedShort)v;
			ib[i + 0] = base + 0; ib[i + 1]  = base + 1; ib[i + 2]  = base + 4;		// clear shoulder to core
			ib[i + 3] = base + 0; ib[i + 4]  = base + 4; ib[i + 5]  = base + 3;
			ib[i + 6] = base + 1; ib[i + 7]  = base + 2; ib[i + 8]  = base + 5;		// core to the far shoulder
			ib[i + 9] = base + 1; ib[i + 10] = base + 5; ib[i + 11] = base + 4;

			vertices += 6;
			indices  += 12;
		}

		// A translucent tile, split like the lines so it drapes over the ground rather than spanning it corner to corner.
		void tile(Real x0, Real y0, Real x1, Real y1, UnsignedInt color)
		{
			const Int nx = drapePieces(x1 - x0);
			const Int ny = drapePieces(y1 - y0);
			if (vb == nullptr)
			{
				vertices += 4 * nx * ny;
				indices  += 6 * nx * ny;
				return;
			}
			for (Int i = 0; i < nx; ++i)
				for (Int j = 0; j < ny; ++j)
					quad(x0 + (x1 - x0) * (Real)i / (Real)nx,       y0 + (y1 - y0) * (Real)j / (Real)ny,
						 x0 + (x1 - x0) * (Real)(i + 1) / (Real)nx, y0 + (y1 - y0) * (Real)(j + 1) / (Real)ny, color);
		}

		// One flat piece of a tile, draped by its four corner heights.
		void quad(Real x0, Real y0, Real x1, Real y1, UnsignedInt color)
		{

			const Real fillLift = lift * GRID_FILL_LIFT;
			const Int  v = vertices;
			vb[v + 0].x = x0; vb[v + 0].y = y0; vb[v + 0].z = groundAt(x0, y0) + fillLift; vb[v + 0].diffuse = color; vb[v + 0].u1 = 0.0f; vb[v + 0].v1 = 0.0f;
			vb[v + 1].x = x1; vb[v + 1].y = y0; vb[v + 1].z = groundAt(x1, y0) + fillLift; vb[v + 1].diffuse = color; vb[v + 1].u1 = 0.0f; vb[v + 1].v1 = 0.0f;
			vb[v + 2].x = x1; vb[v + 2].y = y1; vb[v + 2].z = groundAt(x1, y1) + fillLift; vb[v + 2].diffuse = color; vb[v + 2].u1 = 0.0f; vb[v + 2].v1 = 0.0f;
			vb[v + 3].x = x0; vb[v + 3].y = y1; vb[v + 3].z = groundAt(x0, y1) + fillLift; vb[v + 3].diffuse = color; vb[v + 3].u1 = 0.0f; vb[v + 3].v1 = 0.0f;

			const Int i = indices;
			const UnsignedShort base = (UnsignedShort)v;
			ib[i + 0] = base + 0; ib[i + 1] = base + 1; ib[i + 2] = base + 2;
			ib[i + 3] = base + 0; ib[i + 4] = base + 2; ib[i + 5] = base + 3;

			vertices += 4;
			indices  += 6;
		}
	};

	//-------------------------------------------------------------------------------------------------
	class DebugGridRenderObj : public RenderObjClass
	{
	public:

		DebugGridRenderObj();
		virtual ~DebugGridRenderObj() override;

		virtual RenderObjClass *Clone(void) const override             { return nullptr; }
		virtual int             Class_ID(void) const override          { return RenderObjClass::CLASSID_UNKNOWN; }
		virtual Bool            Cast_Ray(RayCollisionTestClass &) override { return false; }
		virtual void            Render(RenderInfoClass &rinfo) override;
		virtual void            Get_Obj_Space_Bounding_Sphere(SphereClass &sphere) const override;
		virtual void            Get_Obj_Space_Bounding_Box(AABoxClass &box) const override;

		void set(const Coord3D &centre, Real cell, Int radius, const Coord3D *spots, Int spotCount, Real spotHalf);

	private:

		void build(GridBuilder &out) const;		// walks the patch; counts when out.vb is null, fills when it is not
		void markCells(Bool *marked, Real x0, Real y0) const;

		Coord3D              m_centre;
		Real                 m_cell;
		Int                  m_radius;
		Coord3D              m_spots[GRID_MAX_SPOTS];
		Int                  m_spotCount;
		Real                 m_spotHalf;
		ShaderClass          m_shader;
		VertexMaterialClass *m_material;
	};

	// Alpha blended, depth tested, never depth written: objects stand in front of the lines and the ground cannot fight them.
	#define GRID_SHADER ( SHADE_CNST(ShaderClass::PASS_LEQUAL, ShaderClass::DEPTH_WRITE_DISABLE, ShaderClass::COLOR_WRITE_ENABLE, \
		ShaderClass::SRCBLEND_SRC_ALPHA, ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA, ShaderClass::FOG_DISABLE, \
		ShaderClass::GRADIENT_MODULATE, ShaderClass::SECONDARY_GRADIENT_DISABLE, ShaderClass::TEXTURING_DISABLE, \
		ShaderClass::ALPHATEST_DISABLE, ShaderClass::CULL_MODE_DISABLE, \
		ShaderClass::DETAILCOLOR_DISABLE, ShaderClass::DETAILALPHA_DISABLE) )

	DebugGridRenderObj::DebugGridRenderObj() :
		m_cell(20.0f),
		m_radius(0),
		m_spotCount(0),
		m_spotHalf(0.0f),
		m_material(nullptr)
	{
		m_centre.x = m_centre.y = m_centre.z = 0.0f;
		m_shader   = ShaderClass(GRID_SHADER);
		m_material = VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	}

	DebugGridRenderObj::~DebugGridRenderObj()
	{
		REF_PTR_RELEASE(m_material);
	}

	void DebugGridRenderObj::set(const Coord3D &centre, Real cell, Int radius,
								 const Coord3D *spots, Int spotCount, Real spotHalf)
	{
		m_centre   = centre;
		m_cell     = (cell > 1.0f) ? cell : 1.0f;
		m_spotHalf = (spotHalf > 0.0f) ? spotHalf : 0.0f;

		m_spotCount = 0;
		if (spots != nullptr)
		{
			const Int take = (spotCount > GRID_MAX_SPOTS) ? GRID_MAX_SPOTS : spotCount;
			for (Int i = 0; i < take; ++i)
				m_spots[m_spotCount++] = spots[i];
		}

		//
		// Centre the patch on what is being placed rather than on the cursor, and make it reach past the copies on every
		// side. A count that does not fill its last row sits off to one side of the cursor — five copies fill two rows of a
		// three by three block — and centring on the cursor left plain grid showing along one edge only.
		//
		Int needed = 1;
		if (m_spotCount > 0)
		{
			Real minX = m_spots[0].x, maxX = m_spots[0].x;
			Real minY = m_spots[0].y, maxY = m_spots[0].y;
			for (Int i = 1; i < m_spotCount; ++i)
			{
				if (m_spots[i].x < minX) minX = m_spots[i].x;
				if (m_spots[i].x > maxX) maxX = m_spots[i].x;
				if (m_spots[i].y < minY) minY = m_spots[i].y;
				if (m_spots[i].y > maxY) maxY = m_spots[i].y;
			}
			m_centre.x = (minX + maxX) * 0.5f;
			m_centre.y = (minY + maxY) * 0.5f;

			const Real halfX = (maxX - minX) * 0.5f + m_spotHalf;
			const Real halfY = (maxY - minY) * 0.5f + m_spotHalf;
			const Real half  = (halfX > halfY) ? halfX : halfY;
			needed = (Int)(half / m_cell) + 1 + GRID_MARGIN;
		}

		// The knob overrides the lot when someone sets it.
		Int want = (TheDebugGrid.radius > 0) ? TheDebugGrid.radius : ((needed > radius) ? needed : radius);
		if (want < 1)               want = 1;
		if (want > GRID_MAX_RADIUS) want = GRID_MAX_RADIUS;
		m_radius = want;
	}

	void DebugGridRenderObj::Get_Obj_Space_Bounding_Sphere(SphereClass &sphere) const
	{
		const Real reach = m_cell * (Real)(m_radius + 1);
		sphere.Init(Vector3(m_centre.x, m_centre.y, m_centre.z), reach * 1.5f);
	}

	void DebugGridRenderObj::Get_Obj_Space_Bounding_Box(AABoxClass &box) const
	{
		const Real reach = m_cell * (Real)(m_radius + 1);
		box.Init(Vector3(m_centre.x - reach, m_centre.y - reach, m_centre.z - reach),
				 Vector3(m_centre.x + reach, m_centre.y + reach, m_centre.z + reach));
	}

	//-------------------------------------------------------------------------------------------------
	// Ronin @feature 16/09/2026 DX9: one walk of the patch, used twice — once to count, once to write.
	void DebugGridRenderObj::build(GridBuilder &out) const
	{
		const Real reach = m_cell * (Real)m_radius;
		const Real tick  = m_cell * GRID_TICK;
		const Real gap   = m_cell * GRID_TICK_GAP;
		const Real inset = m_cell * GRID_TILE_INSET;

		// The patch sits on cell boundaries, so the lines stay put while the cursor moves inside a cell.
		const Real x0 = (Real)REAL_TO_INT_FLOOR(m_centre.x / m_cell) * m_cell;
		const Real y0 = (Real)REAL_TO_INT_FLOOR(m_centre.y / m_cell) * m_cell;

		// Which cells the copies actually stand on. Marking each object, not the box around the whole block, is what leaves
		// plain grid showing between them — the block's own box covers nearly the entire patch.
		Bool marked[GRID_ROW * GRID_ROW];
		markCells(marked, x0, y0);

		//
		// The plain grid: a cross at every cell corner. Corners that touch a marked cell are skipped — that area gets its own
		// outline, and drawing both would blend twice.
		//
		for (Int ix = -m_radius; ix <= m_radius; ++ix)
		{
			for (Int iy = -m_radius; iy <= m_radius; ++iy)
			{
				// Corners buried inside the tiles are dropped; the ones along their edge are kept, so every tile on the rim of
				// the drop is framed by marks — that is the part that reads as StarCraft.
				const Bool insideTiles = isMarked(marked, ix,     iy,     m_radius) &&
										 isMarked(marked, ix - 1, iy,     m_radius) &&
										 isMarked(marked, ix,     iy - 1, m_radius) &&
										 isMarked(marked, ix - 1, iy - 1, m_radius);
				if (insideTiles)
					continue;

				const Real cx = x0 + (Real)ix * m_cell;
				const Real cy = y0 + (Real)iy * m_cell;
				const Real fade = rimFade(cx, cy, m_centre, reach);
				if (fade <= 0.0f)
					continue;
				const UnsignedInt white = gridColor(255, 255, 255, TheDebugGrid.alpha * fade);

				// The mark is a plus sign drawn as an OUTLINE around a square hole: four L corners, each with an arm running
				// out along both axes. Every corner is the same L turned a quarter, so the whole mark is symmetric.
				out.line(cx - gap, cy + gap, cx - gap, cy + tick, TRUE,  white, white);		// top left corner
				out.line(cx - tick, cy + gap, cx - gap, cy + gap, FALSE, white, white);
				out.line(cx + gap, cy + gap, cx + gap, cy + tick, TRUE,  white, white);		// top right
				out.line(cx + gap, cy + gap, cx + tick, cy + gap, FALSE, white, white);
				out.line(cx - gap, cy - tick, cx - gap, cy - gap, TRUE,  white, white);		// bottom left
				out.line(cx - tick, cy - gap, cx - gap, cy - gap, FALSE, white, white);
				out.line(cx + gap, cy - tick, cx + gap, cy - gap, TRUE,  white, white);		// bottom right
				out.line(cx + gap, cy - gap, cx + tick, cy - gap, FALSE, white, white);
			}
		}

		//
		// The cells the copies stand on. Each is its own square, drawn a little inside its cell so neighbours never touch:
		// separate tiles read as "these spots are taken", where edge-to-edge boxes just read as more grid.
		//
		for (Int ix = -m_radius; ix < m_radius; ++ix)
		{
			for (Int iy = -m_radius; iy < m_radius; ++iy)
			{
				if (!isMarked(marked, ix, iy, m_radius))
					continue;

				const Real ax = x0 + (Real)ix * m_cell;
				const Real ay = y0 + (Real)iy * m_cell;
				const Real fade = rimFade(ax + m_cell * 0.5f, ay + m_cell * 0.5f, m_centre, reach);
				if (fade <= 0.0f)
					continue;

				const Real lx = ax + inset;
				const Real ly = ay + inset;
				const Real hx = ax + m_cell - inset;
				const Real hy = ay + m_cell - inset;

				out.tile(lx, ly, hx, hy, gridColor(120, 255, 140, TheDebugGrid.alpha * GRID_FILL_ALPHA * fade));

				const UnsignedInt edge = gridColor(140, 255, 160, TheDebugGrid.alpha * fade);
				out.line(lx, ly, lx, hy, TRUE,  edge, edge);
				out.line(hx, ly, hx, hy, TRUE,  edge, edge);
				out.line(lx, ly, hx, ly, FALSE, edge, edge);
				out.line(lx, hy, hx, hy, FALSE, edge, edge);
			}
		}
	}

	//-------------------------------------------------------------------------------------------------
	// Ronin @feature 16/09/2026 DX9: flags every cell any copy stands on. A copy covers the square its bounding circle fills,
	// which for most objects is a cell or four — that is what the green is meant to show.
	void DebugGridRenderObj::markCells(Bool *marked, Real x0, Real y0) const
	{
		for (Int i = 0; i < GRID_ROW * GRID_ROW; ++i)
			marked[i] = FALSE;

		if (m_spotHalf <= 0.0f)
			return;

		for (Int s = 0; s < m_spotCount; ++s)
		{
			Int lo_x = REAL_TO_INT_FLOOR((m_spots[s].x - m_spotHalf - x0) / m_cell);
			Int hi_x = REAL_TO_INT_FLOOR((m_spots[s].x + m_spotHalf - x0) / m_cell);
			Int lo_y = REAL_TO_INT_FLOOR((m_spots[s].y - m_spotHalf - y0) / m_cell);
			Int hi_y = REAL_TO_INT_FLOOR((m_spots[s].y + m_spotHalf - y0) / m_cell);
			if (lo_x < -m_radius) lo_x = -m_radius;
			if (lo_y < -m_radius) lo_y = -m_radius;
			if (hi_x > m_radius - 1) hi_x = m_radius - 1;
			if (hi_y > m_radius - 1) hi_y = m_radius - 1;

			for (Int ix = lo_x; ix <= hi_x; ++ix)
				for (Int iy = lo_y; iy <= hi_y; ++iy)
					marked[(ix + m_radius) * GRID_ROW + (iy + m_radius)] = TRUE;
		}
	}

	//-------------------------------------------------------------------------------------------------
	// Ronin @feature 16/09/2026 DX9: the whole patch is rebuilt here every frame — it follows the cursor and the ground under
	// it changes, so there is nothing worth keeping between frames.
	void DebugGridRenderObj::Render(RenderInfoClass &rinfo)
	{
		if (m_radius < 1)
			return;

		const Real reach = m_cell * (Real)m_radius;
		SphereClass bounds(Vector3(m_centre.x, m_centre.y, m_centre.z), reach * 1.5f);
		if (rinfo.Camera.Cull_Sphere(bounds))
			return;

		GridBuilder builder;
		builder.reset(nullptr, nullptr);
		builder.lift = TheDebugGrid.lift;

		//
		// Width is asked for in pixels, so work out what a pixel is worth in world units: at distance d the screen covers
		// 2*d*tan(fov/2), spread over the render target's height. Without this the far lines go sub-pixel and shimmer.
		//
		builder.camPos     = rinfo.Camera.Get_Position();
		const Real screenPx = (TheDisplay != nullptr && TheDisplay->getHeight() > 0) ? (Real)TheDisplay->getHeight() : 768.0f;
		builder.worldPerPx  = 2.0f * tanf(rinfo.Camera.Get_Vertical_FOV() * 0.5f) / screenPx;

		//
		// A line's width runs across it, along the ground. From a low camera the across-direction of one axis is nearly edge
		// on and would draw thin, so each axis is widened by how much of it the camera can actually see.
		//
		Vector3 toCentre(m_centre.x - builder.camPos.X, m_centre.y - builder.camPos.Y, m_centre.z - builder.camPos.Z);
		if (toCentre.Length2() > 0.0f)
			toCentre.Normalize();
		builder.seenX = sqrtf(1.0f - (toCentre.X * toCentre.X));
		builder.seenY = sqrtf(1.0f - (toCentre.Y * toCentre.Y));
		if (builder.seenX < 0.25f) builder.seenX = 0.25f;	// never widen by more than 4x, or a grazing line becomes a smear
		if (builder.seenY < 0.25f) builder.seenY = 0.25f;

		// Count first, so the buffers are asked for exactly what the same walk will write.
		build(builder);
		const Int vertices = builder.vertices;
		const Int indices  = builder.indices;
		if (vertices <= 0 || indices <= 0 || vertices > 65000 || indices > 65000)
			return;

		DynamicVBAccessClass vb_access(BUFFER_TYPE_DYNAMIC_DX8, DX8_FVF_XYZDUV1, (unsigned short)vertices);
		DynamicIBAccessClass ib_access(BUFFER_TYPE_DYNAMIC_DX8, (unsigned short)indices);
		{
			DynamicVBAccessClass::WriteLockClass lock(&vb_access);
			DynamicIBAccessClass::WriteLockClass lockib(&ib_access);
			builder.reset((VertexFormatXYZDUV1 *)lock.Get_Formatted_Vertex_Array(), lockib.Get_Index_Array());
			build(builder);
		}

		DX8Wrapper::Set_Material(m_material);
		DX8Wrapper::Set_Shader(m_shader);
		DX8Wrapper::Set_Texture(0, nullptr);
		DX8Wrapper::Set_Index_Buffer(ib_access, 0, "W3DDebugGrid::Render");
		DX8Wrapper::Set_Vertex_Buffer(vb_access);
		DX8Wrapper::Set_Transform(D3DTS_WORLD, Matrix3D(1));
		// Ronin @bugfix 16/09/2026 DX9: bind our own layout — Apply_Render_State_Changes re-binds the PREVIOUS draw's one.
		DX8Wrapper::BindLayoutFVF(vb_access.FVF_Info().Get_FVF(), "W3DDebugGrid::Render");
		DX8Wrapper::Draw_Triangles(0, indices / 3, 0, vertices);
	}

	DebugGridRenderObj *s_grid  = nullptr;
	RTS3DScene         *s_scene = nullptr;		// the scene it was added to, so a reset cannot make us remove it from another
}

//-------------------------------------------------------------------------------------------------
void W3DDebugGrid::show(const Coord3D &centre, Real cell, Int radius,
						const Coord3D *spots, Int spotCount, Real spotHalf)
{
	if (W3DDisplay::m_3DScene == nullptr)
		return;

	if (s_grid == nullptr)
	{
		s_grid = NEW DebugGridRenderObj;
		if (s_grid == nullptr)
			return;
	}
	s_grid->set(centre, cell, radius, spots, spotCount, spotHalf);

	if (s_scene != W3DDisplay::m_3DScene)
	{
		hide();
		s_scene = W3DDisplay::m_3DScene;
		s_scene->Add_Render_Object(s_grid);
	}
}

//-------------------------------------------------------------------------------------------------
void W3DDebugGrid::hide(void)
{
	if (s_scene != nullptr && s_grid != nullptr && s_scene == W3DDisplay::m_3DScene)
		s_scene->Remove_Render_Object(s_grid);
	s_scene = nullptr;
}
