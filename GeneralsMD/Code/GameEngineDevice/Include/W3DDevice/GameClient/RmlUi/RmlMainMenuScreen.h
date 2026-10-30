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

// Ronin @feature 29/10/2026 DX9: the main menu over the shell map (Data/UI/MainMenu.rml) in place of Menus/MainMenu.wnd. Its
// buttons do what MainMenu.cpp's do; the sub-menus are panels beside the root. After tintinhamans' (arctic/go/feat/rmlui).
#pragma once

#include "W3DDevice/GameClient/RmlUi/RmlHqStatus.h"
#include "W3DDevice/GameClient/RmlUi/RmlScreen.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

class RmlMainMenuScreen : public RmlScreen
{
public:
	static RmlMainMenuScreen &instance();

	virtual void open() override;
	virtual void close() override;
	virtual bool isVisible() const override;
	virtual void back() override;				// a sub-panel back to the root; nothing at the root
	virtual void update() override;				// the shell map's script hooks, from the button under the pointer
	virtual void reload() override;
	virtual void release() override;
	virtual Rml::ElementDocument *document() const override { return m_document; }

	// The registry's: as the .wnd's root window gets GWM_CREATE / GWM_DESTROY, and its layout the shell's update.
	static void created();
	static void destroyed();
	static void shellUpdate();

private:
	RmlMainMenuScreen() {}

	void load();
	void setPanel(const Rml::String &panel);
	void setShellHook(const Rml::String &hook);
	void startCampaign(int difficulty);

	void act(const Rml::String &what, const Rml::String &arg);	// what his document's events ask for

	Rml::ElementDocument *m_document = nullptr;
	Rml::DataModelHandle m_model;
	bool m_opening = false;				// showShellMap() brings the shell's top layout forward, which opens this again
	bool m_challenge = false;			// the difficulty picked next opens the Generals Challenge
	bool m_raiseMessageBoxes = false;	// GameSpy's boxes come back to the front once after opening
	Rml::String m_hook;					// the shell hook now highlighted

	Rml::String m_panel = "main";		// main, single, difficulty, multi, loadreplay
	Rml::String m_faction;				// the campaign being started: USA, GLA, China, Training (the Challenge)
	Rml::String m_hover;				// the faction art shown beside the single player panel
	Rml::String m_version;
	bool m_noShellMap = false;			// -noshellmap: the screen draws its own background
	// Generals Online's news strip and player count: kept for that merge, empty until then.
	Rml::Vector<Rml::String> m_newsLines;
	bool m_hasNews = false;
	Rml::String m_playersOnline;
	RmlHqStatus m_hq;
};
