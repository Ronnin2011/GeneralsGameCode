/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 port: units that float ride the WaterSea swell.
*/

// Ronin @feature 03/10/2026 DX9: phase 5 - a floating unit's model is lifted and tilted by the swell under it. Draw only:
// the Object never moves. Called from W3DModelDraw::doDrawModule; which units float is decided in W3DWater.cpp.
#pragma once

class Drawable;
class Matrix3D;

// Ronin @feature 03/10/2026 DX9: phase 5 - the same call grows the unit's wake trail (the water draws it).
void W3DWater_RideSwell(const Drawable *draw, Matrix3D &mtx);

// Ronin @feature 03/10/2026 DX9: phase 5 - TRUE = the water draws this drawable's wake, so its vanilla wake sprites
// (ground-aligned particle systems attached to it) are not wanted. `water oldwakes 1` = always FALSE.
bool W3DWater_OwnsWake(unsigned int drawableID);
