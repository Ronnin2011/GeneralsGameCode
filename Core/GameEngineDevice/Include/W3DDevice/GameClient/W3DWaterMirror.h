/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 port: the water mirror's plane, for culls that must keep what it reflects.
*/

// Ronin @bugfix 01/10/2026 DX9: published by updateRenderTargetTextures each frame. W3DTreeBuffer::cull keeps a tree whose
// reflection is on screen though the tree is not: the mirror draws the main view's tree set. docs/Water_Work.md.
#pragma once

#include "Lib/BaseType.h"

struct WaterMirrorState
{
	Bool active;	///< the mirror renders this frame (WaterType 2, `water mirror 1`)
	Real level;		///< its plane, world z
};

inline WaterMirrorState TheWaterMirror = { FALSE, 0.0f };
