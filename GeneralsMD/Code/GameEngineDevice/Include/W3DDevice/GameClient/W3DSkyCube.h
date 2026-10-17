/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 port: the sky cube - a cube map of a map's sky and ground, made when the map loads.
*/

// Ronin @feature 12/10/2026 DX9: LIGHTING PART 4, STEP 1 (docs/Lighting_Work.md 4.4b). The sky the game ships as five pictures
// on a box, put into one cube map with the map's lit ground below the horizon: the reflective meshes mirror it, `lights view 5` shows it.
#pragma once

#include "Lib/BaseType.h"

struct IDirect3DCubeTexture9;

class W3DSkyCube
{
public:
	static void update(void);			// every frame: builds the cube again when the map, its time of day or its sky changed
	static void rebuild(void);			// ... or when asked (`sky rebuild`)
	static void shutdown(void);			// frees the cube while the device exists

	// Ronin @feature 12/10/2026 DX9: looked up by a WORLD direction as it stands (z up): texCUBE(sky, dir). NULL until one is built.
	static IDirect3DCubeTexture9 *peek(void);
	static void describe(char *out, Int size);	// one line for the panel
};
