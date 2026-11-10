/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 port: tread relief - the tread and tyre marks shaded as grooves pressed into the ground.
*/

// Ronin @feature 09/11/2026 DX9: tread marks with depth (docs/TreadMarks_Work.md). TerrainTracksRenderObjClassSystem::flush draws
// each track's strip here first (TreadRelief_vs/_ps: its groove lit by the sun, x2 onto the ground), then its decal as in 2003.
#pragma once

#include "Lib/BaseType.h"

class DX8VertexBufferClass;
class DX8IndexBufferClass;
class TextureClass;
class Matrix3D;
class Vector3;
struct VertexFormatXYZDUV1;

class R3DTreadMarks
{
public:
	// Ronin @feature 09/11/2026 DX9: rounded tread marks. Ronin @feature 10/11/2026 DX9: laid like a road - the centre path through the
	// stored edges, its corners circular arcs, each edge square to it. Returns the edges written (as stored when there is no room).
	enum { ROUND_EDGE_FACTOR = 4 };
	static Int writeRoundedStrip(const Vector3 *left, const Vector3 *right, const float *v, const float *fade, Int count,
		float spacing, UnsignedInt diffuseRGB, VertexFormatXYZDUV1 *out, Int maxEdges);

	// TRUE when the relief draws this frame: shaders loaded; binds the tracks' buffers and the pass's state.
	static Bool beginRelief(DX8VertexBufferClass *vb, DX8IndexBufferClass *ib, const Matrix3D &world);
	// One track: its texture names its relief map (<name>_nrm.tga); base vertex and edge count as the decal's draw.
	static void drawRelief(TextureClass *trackTexture, Int baseVertex, Int edgeCount);
	static void endRelief(void);		// the device's states back; the wrapper re-applies its own
	static void shutdown(void);			// frees the shaders and the relief maps while the device exists

	// Ronin @bugfix 10/11/2026 DX9: the pieces the tracks gained this frame, a triangle list for R3DTaa's velocity pass - new marks on still
	// ground carry no other evidence, so the history faded each one in behind the vehicle. take hands them over and clears them.
	static Bool hasTaaMarks(void);
	static Int takeTaaMarks(const Vector3 **verts);
};
