/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 debug grid — the snap cells the debug panel places on, drawn over the terrain.
*/

// Ronin @feature 16/09/2026 DX9: grid for the debug panel's spawn placement. No shader of its own: a render object in the 3D
// scene, rebuilt from terrain heights each frame and drawn with the engine's alpha path. docs/Debug_Panel_Design.md step 7c4.
#pragma once

#include "Lib/BaseType.h"

// Ronin @feature 16/09/2026 DX9: how the lines look, live, so the panel's `grid` command can settle them without a rebuild.
// Width is in PIXELS: the lines are built to a constant width on screen, or the far ones go sub-pixel and shimmer.
struct DebugGridTuning
{
	Real lift;				// world units above the ground, so slopes do not swallow the lines
	Real halfWidthPixels;	// half the line width, on screen
	Real alpha;				// at the centre of the patch; it still fades towards the rim
	Int  radius;			// cells each way; 0 = size it from what is being placed
};
extern DebugGridTuning TheDebugGrid;

class W3DDebugGrid
{
public:

	// Ronin @feature 16/09/2026 DX9: draw the cells around a point; call once per frame while they are wanted. cell is the
	// cell size in world units, radius how far the patch should reach — TheDebugGrid.radius overrides it when set.
	// spots are where the copies will land and spotHalf how far each one reaches: the cells they cover are drawn as tiles,
	// so the green marks the objects themselves rather than the whole block they sit in.
	static void show(const Coord3D &centre, Real cell, Int radius,
					 const Coord3D *spots, Int spotCount, Real spotHalf);

	// Ronin @feature 16/09/2026 DX9: stop drawing it. Safe when it was never shown, and when the scene is already gone.
	static void hide(void);
};
