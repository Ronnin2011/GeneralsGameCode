/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Ronin @feature 29/10/2026 DX9: the Options logic with no widgets - read a setting, apply it to the game, write it to
// Options.ini - for the RmlUi Options screen. From tintinhamans' fork (arctic/go/feat/rmlui); the .wnd menu keeps its own.
#pragma once

#include "Common/AsciiString.h"
#include "Lib/BaseType.h"

class OptionPreferences;

namespace OptionsValues
{
	// -- Detail: the preset and its Custom values, as the .wnd's showDetailPreset() / saveOptions() read and write them ----
	struct DetailPresetValues
	{
		Int textureResolutionSliderPos = 2;		// 0..2, 2 = full resolution
		Int particleCap = 0;
		Bool shadow3D = FALSE;
		Bool shadow2D = FALSE;
		Bool cloudShadows = FALSE;
		Bool groundLighting = FALSE;
		Bool smoothWater = FALSE;
		Bool extraAnimations = FALSE;
		Bool noDynamicLod = FALSE;
		Bool heatEffects = FALSE;
		Bool buildingOcclusion = FALSE;			// no preset sets it, only Custom
		Bool props = FALSE;
	};

	Int GetDetailCustomLevel();
	Int GetCurrentDetailLevel();						// a level never set becomes the recommended one, as the .wnd's init does
	DetailPresetValues GetDetailPresetPreview(Int level);	// what a preset would set; applies nothing
	DetailPresetValues GetCurrentGraphicsValues();
	void ApplyDetailLevel(OptionPreferences &pref, Int level);
	void ApplyCustomGraphicsSettings(OptionPreferences &pref, const DetailPresetValues &values);

	// -- Resolution ---------------------------------------------------------------------------------------------------
	Int GetDisplayModeCount();
	void GetDisplayMode(Int index, Int &width, Int &height);
	Int GetCurrentDisplayModeIndex();					// -1: the running size is not in the list
	// Changes the mode if it differs; TRUE if it did. Call it last: it rebuilds the shell, and the caller then shows
	// DoResolutionDialog() (MainMenu.cpp), as the .wnd's Accept does.
	Bool ApplyDisplayMode(OptionPreferences &pref, Int index);

	// -- Network ------------------------------------------------------------------------------------------------------
	Int GetIPChoiceCount();
	AsciiString GetIPChoiceLabel(Int index);
	UnsignedInt GetIPChoiceIP(Int index);
	Int GetCurrentLANIPIndex(OptionPreferences &pref);
	Int GetCurrentOnlineIPIndex(OptionPreferences &pref);
	void ApplyLANIPChoice(OptionPreferences &pref, UnsignedInt ip);
	void ApplyOnlineIPChoice(OptionPreferences &pref, UnsignedInt ip);
	Int GetCurrentFirewallPortOverride();				// 0 when unset
	void ApplyFirewallPortOverride(OptionPreferences &pref, Int port);
	void RefreshFirewallBehavior(OptionPreferences &pref);

	// In a game (not the shell) or online: the .wnd greys out resolution, detail and the network rows then.
	Bool IsOptionsRestrictedContext();

	// -- Audio, 0..100 ------------------------------------------------------------------------------------------------
	Int GetCurrentMusicVolumePercent(OptionPreferences &pref);
	Int GetCurrentSFXVolumePercent(OptionPreferences &pref);
	Int GetCurrentVoiceVolumePercent(OptionPreferences &pref);
	Int GetCurrentMoneyVolumePercent(OptionPreferences &pref);
	void ApplyMusicVolumePercent(OptionPreferences &pref, Int percent);
	void ApplySFXVolumePercent(OptionPreferences &pref, Int percent);
	void ApplyVoiceVolumePercent(OptionPreferences &pref, Int percent);
	void ApplyMoneyVolumePercent(OptionPreferences &pref, Int percent);
	Int GetDefaultMusicVolumePercent();
	Int GetDefaultSFXVolumePercent();
	Int GetDefaultVoiceVolumePercent();

	// -- Brightness: the .wnd's gamma slider, 0..100, 50 = as made -------------------------------------------------------
	Int GetCurrentBrightnessPercent(OptionPreferences &pref);
	void ApplyBrightnessPercent(OptionPreferences &pref, Int percent);

	// -- Controls -----------------------------------------------------------------------------------------------------
	struct ControlValues
	{
		Int scrollSpeed = 50;					// percent, ScrollFactor
		Bool alternateMouse = FALSE;
		Bool rightMouseScrollWithAlternate = FALSE;
		Bool retaliation = TRUE;
		Bool doubleClickAttackMove = FALSE;
		Bool drawScrollAnchor = FALSE;
		Bool moveScrollAnchor = FALSE;
		Bool edgeScrollWindowed = FALSE;
		Bool edgeScrollFullscreen = TRUE;
		Bool captureWindowedGame = FALSE;
		Bool captureWindowedMenu = FALSE;
		Bool captureFullscreenGame = TRUE;
		Bool captureFullscreenMenu = TRUE;
	};
	ControlValues GetCurrentControls(OptionPreferences &pref);
	void SetDefaultControls(ControlValues &values);	// only what the .wnd's Defaults resets: scroll speed and the mouse
	// scrollSpeedLoaded: the value shown on opening; an unmoved slider keeps a setting beyond its range, as the .wnd does
	void ApplyControls(OptionPreferences &pref, const ControlValues &values, Int scrollSpeedLoaded);

	// -- Interface and replays ------------------------------------------------------------------------------------------
	struct InterfaceValues
	{
		Bool languageFilter = TRUE;
		Bool showMoneyPerMinute = FALSE;
		Bool playerObserver = TRUE;			// PlayerObserverEnabled
		Bool archiveReplays = FALSE;
		Bool saveCameraInReplays = FALSE;
		Bool useCameraInReplays = FALSE;
		// The info overlays: a font size, 0 hides one
		Int renderFpsFontSize = 8;
		Int systemTimeFontSize = 8;
		Int gameTimeFontSize = 8;
		Int networkLatencyFontSize = 8;
		Int playerInfoListFontSize = 8;
	};
	InterfaceValues GetCurrentInterface(OptionPreferences &pref);
	void ApplyInterface(OptionPreferences &pref, const InterfaceValues &values);
}
