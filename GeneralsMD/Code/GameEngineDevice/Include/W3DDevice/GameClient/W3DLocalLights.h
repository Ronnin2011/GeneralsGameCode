/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 local lights, drawn in screen space.
*/

// Ronin @feature 09/10/2026 DX9: local lights (explosions, weapon flashes) per pixel from the scene depth, over terrain and
// meshes alike. While it runs, the per-vertex terrain re-light and the per-object light stand down (ownsDynamicLights).

#pragma once

#ifndef __W3DLOCALLIGHTS_H_
#define __W3DLOCALLIGHTS_H_

#include "Lib/BaseType.h"

class CameraClass;
class RTS3DScene;
class MeshClass;

class W3DLocalLights
{
public:
	static void beginFrame(void);			// W3DDisplay::draw, after W3DTaa::beginFrame: can this frame run
	static void renderPass(const CameraClass &camera, RTS3DScene *scene);	// RTS3DScene::Flush, after the trees
	static void endFrame(void);				// after W3DTaa::endFrame: ends the scene depth if this pass began it
	static void shutdown(void);				// frees the shaders and the frame copy while the device exists

	// Ronin @feature 09/10/2026 DX9: TRUE while this pass draws the scene's dynamic lights - the old paths must not add them again.
	static Bool ownsDynamicLights(void);
	// Ronin @feature 09/10/2026 DX9: TRUE = W3DDisplay::createLightPulse keeps the pulses it drops as too small for the 10-unit
	// vertex grid (26 of the game's 65). Per pixel they show.
	static Bool wantsSmallLights(void);

	// Ronin @feature 09/10/2026 DX9: lighting part 2, the beams. MeshClass::Render asks this about every mesh once: 0 = no light fixture,
	// 1 = one, draw it, 2 = one whose beam a spot light replaces this frame: do not draw it. Lighting_Work.md 4.2.
	static int  noteFixtureMesh(MeshClass *mesh);
	// Ronin @feature 11/10/2026 DX9: lamp shadow maps. MeshClass::Render calls this for every mesh it draws; kept for the frame
	// (main pass, solid meshes) so each lamp's map can be drawn from what stands in its light. Lighting_Work.md 6b.
	static void noteCaster(MeshClass *mesh);
	// Ronin @feature 09/10/2026 DX9: TRUE = this mesh's beam polygons are clear this frame (their texture is swapped); TAA must not
	// treat the mesh as a moving-texture translucent one on their account.
	static Bool beamsHiddenIn(const MeshClass *mesh);

	// Ronin @diagnostic 09/10/2026 DX9: the panel's `lights` knobs. Session only, not saved.
	static void  setEnabled(Bool on);
	static Bool  isEnabled(void);
	static void  setIntensity(float v);		// multiplies every light's colour
	static float getIntensity(void);
	static void  setOver(float v);			// how far above the surface's own colour a light may push a pixel; 1 = the old limit
	static float getOver(void);
	static void  setBoost(float v);			// frame-to-surface factor; 0 = worked out from the map's light values
	static float getBoost(void);
	static void  setRange(float v);			// multiplies every light's reach
	static float getRange(void);
	static void  setSmall(Bool on);			// see wantsSmallLights
	static Bool  getSmall(void);
	static void  setView(Int v);			// 0 off, 1 the light alone, 2 the facing used, 3 the rectangles drawn, 4 the lamps' shadow maps
	static Int   getView(void);
	static void  setBeams(float v);			// the beams' strength as lights; 0 = the art's own beams, no lights
	static float getBeams(void);
	static void  setShafts(float v);		// the beam in the air, drawn by the shader: its strength beside the light's; 0 = none
	static float getShafts(void);
	static void  setSpill(float v);			// the dim light around a beam's cone: its strength beside the cone's; 0 = none
	static float getSpill(void);
	static void  setSpillWide(float v);		// ... and how wide it reaches, in cone widths
	static float getSpillWide(void);
	static void  setGlows(float v);			// the lamp glows as point lights: their strength; 0 = none
	static float getGlows(void);
	static void  setGlowReach(float v);		// multiplies every glow light's reach
	static float getGlowReach(void);
	static void  setCoronas(float v);		// the lamp's own glow drawn by the shader, its strength; 0 = the art's glow textures
	static float getCoronas(void);
	static void  setCoronaSize(float v);	// its radius, in the art glow's own
	static float getCoronaSize(void);
	static void  setPools(Bool keep);		// TRUE = the art's painted pools of light (and welded glows) stay drawn
	static Bool  getPools(void);
	static void  setBeamReach(float v);		// a beam's light on surfaces ends this far along, in the art cone's own length
	static float getBeamReach(void);
	static void  setPoolFill(float v);		// strength of the light that stands in for a painted pool no lamp covers; 0 = none
	static float getPoolFill(void);
	static void  setHotspot(float v);		// the share of a beam's cone at full strength; 0 = it fades from the axis out
	static float getHotspot(void);
	// Ronin @feature 11/10/2026 DX9: lamp shadow maps: a depth map per beam, drawn from its lamp, and read by its light (stage B).
	static void  setLampMaps(Bool on);		// FALSE = no maps drawn, so no lamp shadows
	static Bool  getLampMaps(void);
	static void  setLampShadows(float v);	// how much a beam's shadow takes; 0 = the maps are drawn and not read
	static float getLampShadows(void);
	static void  setLampBias(float v);		// multiplies the receiver's bias: lower = tighter contact, more risk of stripes
	static float getLampBias(void);
	static void  setLampSelf(Bool on);		// TRUE = the lamp's own object casts in its light too
	static Bool  getLampSelf(void);
	static void  setLampSelfNear(float v);	// ... but only past this share of the beam's length: a housing round the bulb casts nothing
	static float getLampSelfNear(void);
	static void  setPointMaps(Bool on);		// TRUE = the engine's point lights (firing flashes, blasts) get a map too, straight down
	static Bool  getPointMaps(void);
	static void  setPointSides(Bool on);	// TRUE = a big point light low over the ground (a blast) gets four level maps as well
	static Bool  getPointSides(void);
	// Ronin @feature 10/10/2026 DX9: lighting part 3, step 1. Called round the mesh shader's draws (DX8InstanceManagerClass hooks): binds a
	// second target that takes the sun's shade (RigidInstance_ps COLOR1), in the main pass only.
	static void  beginSunShade(void);
	static void  endSunShade(void);
	static void  setSunFill(float v);		// how much of the sun's shade a lamp undoes on a mesh; 0 = none (as before)
	static float getSunFill(void);
	static void  setSunErase(float v);		// how fast a lamp restores what the sun's shadow took: gone at lamp strength 1 / this; 0 = never
	static float getSunErase(void);
	static void  setMapShadow(float v);		// 1 = a sun shadow leaves what this map's light is without its sun; 0 = 0.45 on every map
	static float getMapShadow(void);

	// Ronin @diagnostic 09/10/2026 DX9: last frame's numbers, for the panel's [LIGHT] row.
	struct Stats
	{
		Bool  active;			// the pass could run: shaders loaded and a readable scene depth
		Bool  ownsDepth;		// it began the scene depth itself - neither SSAO nor TAA had
		Int   live;				// enabled lights in the scene
		Int   drawn;			// of those, drawn
		float coverage;			// summed rectangle area, in screens
		Bool  masked;			// TAA took the change mask this frame
		float sceneLight[3];	// the frame-to-surface factor in use
		Int   beams;			// beams that became spot lights
		Int   glows;			// lamp glows that became point lights
		Int   pools;			// painted pools no lamp covers that got a light of their own
		Int   sunShade;			// the sun's shade the lights read this frame: 1 = the meshes', 2 = the terrain's, 3 = both
		Int   casters;			// lamp shadow maps: solid meshes drawn this frame, the casters to choose from
		Int   lampMaps;			// ... lights that got a map
		Int   lampPoints;		// ... of them the engine's point lights (the rest are beams)
		Int   lampSides;		// ... and of those, the ones with four level maps as well (blasts)
		Int   lampDraws;		// ... draws into the maps, summed
		Int   lampMost;			// ... the most any one map took
		Int   lampTris;			// ... and their triangles, summed
		Int   lampOwn;			// ... casters left out as the lamp's own object (`lights lampself 0`)
		Int   lampSkins;		// ... and skinned meshes there was no room to deform
		Int   beamMeshes;		// beam-only meshes left undrawn (every pass counts)
		Int   beamModels;		// beam models read so far
	};
	static void getStats(Stats *out);
};

#endif
