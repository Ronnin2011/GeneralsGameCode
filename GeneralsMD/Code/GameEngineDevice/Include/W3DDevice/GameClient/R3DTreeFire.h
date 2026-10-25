/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 port: the map's trees catch fire, burn and stay burnt.
*/

// Ronin @feature 25/10/2026 DX9: Zero Hour's map trees have no logic object (OPTIMIZED_TREE), so flame damage never reached
// them. The logic reports fire through GameClient::notifyTerrainFire; this burns the trees in W3DTreeBuffer. Visual only.

#pragma once

#ifndef __R3DTREEFIRE_H_
#define __R3DTREEFIRE_H_

#include "Lib/BaseType.h"

class W3DTreeBuffer;

class R3DTreeFire
{
public:
	// The tree buffer's hooks.
	static void attach(W3DTreeBuffer *buffer);				// its constructor
	static void detach(W3DTreeBuffer *buffer);				// its destructor
	static void reset(W3DTreeBuffer *buffer, Bool types);	// clearAllTrees (types too); a save's trees loading (fires only)
	static void typeAdded(W3DTreeBuffer *buffer, Int type);	// addTree, after a new type: can it burn, and where its fire sits
	static void update(W3DTreeBuffer *buffer, Bool mainPass);	// drawTrees, before the atlas: burnt types once, then the fires
	static Int  typeToSave(W3DTreeBuffer *buffer, Int tree);	// xfer: a burnt tree saves as its green type, so saves stay retail
	// The packed vertex slot's x: the sway index, and the tree's burn byte in its fraction (Trees_vs.hlsl reads it back).
	static Real swaySlot(Int swayType, UnsignedByte burnChar) { return (Real)swayType + (Real)burnChar * (0.25f / 255.0f); }

	// The logic's reports (W3DGameClient::notifyTerrainFire): flame damage of this radius, or the nuke's scorch (no flames).
	static void onFire(const Coord3D *pos, Real radius, Bool scorch);

	// The panel's `treefire`. Session only.
	static void setEnabled(Bool on);
	static Bool isEnabled(void);
	static void setCap(Int fires);
	static Int  getCap(void);
	static void setSpread(Bool on);
	static Bool getSpread(void);
	static void setThin(Real share);		// the burnt canopy's share gone, 0..1, against the green tree
	static Real getThin(void);
	static Bool igniteNearest(const Coord3D *pos, Real range);	// FALSE: no green tree that can burn in range
	static void stats(Int *fires, Int *burnt, Int *burntTypes, Int *types);

private:
	static void startFire(W3DTreeBuffer *buffer, Int tree);
	static void charTree(W3DTreeBuffer *buffer, Int tree);
	static void firePosition(W3DTreeBuffer *buffer, Int tree, Coord3D *pos);
	static Bool canBurn(W3DTreeBuffer *buffer, Int tree);
	static Int  closestToBurn(W3DTreeBuffer *buffer, const Coord3D *pos, Real range, Int skip);
	static void addBurntType(W3DTreeBuffer *buffer, Int type);
};

#endif
