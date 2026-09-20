/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 screen-space ambient occlusion.
*/

// Ronin @feature 13/09/2026 DX9: SSAO. Options.ini key SSAOQuality (Off/Normal/High/Ultra, or 0..3). The main pass renders
// into a readable depth buffer; after the trees, renderPass computes and blurs AO at half resolution and multiplies it in.

#pragma once

#ifndef __W3DSSAO_H_
#define __W3DSSAO_H_

#include "Lib/BaseType.h"

class CameraClass;

class W3DSsao
{
public:
	// Corner view: 0 = none, 1 = scene depth, 2 = raw AO recomputed at end of frame, 3 = the blurred AO renderPass made.
	static const Int DEBUG_VIEW = 0;
	// Ronin @feature 13/09/2026 DX9: SSAO step 3b. 0 = compute AO but leave the frame untouched (A/B without a restart).
	static const Int APPLY_TO_FRAME = 1;


	static void beginFrame(void);							// after WW3D::Begin_Render, before the views draw
	static void renderPass(const CameraClass &camera);		// RTS3DScene::Flush after the trees, main pass only
	static void endFrame(void);								// gives the device its own depth buffer back
	static void drawDebugView(const CameraClass *camera);	// after endFrame — a bound depth buffer cannot be read
	static Int  getQuality(void);							// TheGlobalData->m_ssaoQuality, 0 when unavailable
	static Bool isSupported(void);
	static Bool isActive(void);
	// Ronin @diagnostic 20/09/2026 DX9: what the ACTIVE tier resolved to, so the HUD can prove a quality change
	// took effect instead of us eyeballing subtle differences. Zeroes until the targets exist.
	static void getTargetSize(Int *outW, Int *outH);
	static Int  getSampleCount(void);
	static void shutdown(void);
};

#endif
