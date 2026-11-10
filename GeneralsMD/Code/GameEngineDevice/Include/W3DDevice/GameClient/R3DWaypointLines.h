/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 port: waypoint ribbons - the waypoint, rally point and enemy-path lines as flat bands on the ground.
*/

// Ronin @feature 09/11/2026 DX9: waypoint ribbons (docs/Waypoints_Work.md). W3DWaypointBuffer::drawWaypoints hands its paths
// here instead of drawing 2003's laser lines and pucks; R3DTaa::noteOpaqueDone draws them after the opaque scene.
#pragma once

#include "Lib/BaseType.h"

class Vector3;

class R3DWaypointLines
{
public:
	enum Style { STYLE_WAYPOINTS, STYLE_RALLY, STYLE_ENEMY };	// rings at every node / at the end only / every node, orange

	static void beginGather(void);			// drops last frame's paths; drawWaypoints calls it before its own
	static void addPath(const Vector3 *points, Int count, Int style);	// main pass only; other passes are ignored
	static void drawGathered(void);			// after the opaque scene, once a frame; uses the paths up
	static void shutdown(void);				// frees the shaders while the device exists
	static Bool getEnabled(void);			// FALSE once its shaders failed to load: 2003's lines draw instead
};
