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

// Ronin @feature 29/10/2026 DX9: the game menu (Escape in a match, Data/UI/QuitMenu.rml) in place of QuitMenu.wnd and
// QuitNoSave.wnd. QuitMenu.cpp's ToggleQuitMenu() opens and closes it; after tintinhamans' (arctic/go/feat/rmlui).
#pragma once

#include "W3DDevice/GameClient/RmlUi/RmlScreen.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

namespace Rml { class Event; }

class RmlQuitMenuScreen : public RmlScreen
{
public:
	static RmlQuitMenuScreen &instance();

	virtual void open() override;
	virtual void close() override;
	virtual bool isVisible() const override;
	virtual void back() override;		// Return to game
	virtual void reload() override;
	virtual void release() override;
	virtual Rml::ElementDocument *document() const override { return m_document; }

private:
	RmlQuitMenuScreen() {}

	void load();
	void refresh();						// the labels and the greyed buttons, from the game now
	void act(const Rml::String &what);	// what his document's events ask for

	Rml::ElementDocument *m_document = nullptr;
	Rml::DataModelHandle m_model;

	Rml::String m_restartLabel;
	Rml::String m_exitLabel;
	bool m_showSaveLoad = true;			// not in a multiplayer game or a replay (QuitNoSave.wnd)
	bool m_restartDisabled = false;		// no surrender once beaten
	bool m_cinematic = false;			// a cinematic's letterbox: no Save/Load and no Options, as the .wnd
};
