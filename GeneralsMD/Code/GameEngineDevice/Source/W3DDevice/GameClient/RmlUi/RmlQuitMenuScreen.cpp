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

// Ronin @feature 29/10/2026 DX9: the game menu. The buttons run QuitMenu.cpp's own handlers (the confirmation boxes and their
// callbacks), so restart, surrender and exit are the .wnd's; after tintinhamans' (arctic/go/feat/rmlui).
#include "W3DDevice/GameClient/RmlUi/RmlQuitMenuScreen.h"

#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/GameText.h"
#include "GameClient/InGameUI.h"
#include "GameClient/RmlUiScreenRegistry.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/VictoryConditions.h"
#include "W3DDevice/GameClient/RmlUi/RmlTransitionSounds.h"
#include "W3DDevice/GameClient/RmlUi/RmlUiManager.h"
#include "W3DDevice/GameClient/RmlUi/RmlUiText.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>

// QuitMenu.cpp: what its Save/Load, Restart and Exit buttons do.
void QuitMenuOpenSaveLoad();
void QuitMenuAskRestart();
void QuitMenuAskExit();

namespace
{
	const char *const kDocument = "Data/UI/QuitMenu.rml";
}

RmlQuitMenuScreen &RmlQuitMenuScreen::instance()
{
	static RmlQuitMenuScreen s_screen;
	return s_screen;
}

void RmlQuitMenuScreen::load()
{
	if (m_document || !TheRmlUiManager || !TheRmlUiManager->getContext())
		return;
	if (!m_model)
	{
		// His document's names (QuitMenu.rml).
		Rml::DataModelConstructor c = TheRmlUiManager->getContext()->CreateDataModel("quitmenu");
		if (!c)
			return;
		c.Bind("restart_label", &m_restartLabel);
		c.Bind("exit_label", &m_exitLabel);
		c.Bind("show_saveload", &m_showSaveLoad);
		c.Bind("restart_disabled", &m_restartDisabled);
		c.Bind("saveload_disabled", &m_cinematic);
		c.Bind("options_disabled", &m_cinematic);
		const char *const events[] = { "return_to_game", "options", "save_load", "restart", "exit" };
		for (const char *name : events)
		{
			const Rml::String what = name;
			c.BindEventCallback(name, [this, what](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { act(what); });
		}
		m_model = c.GetModelHandle();
	}
	m_document = TheRmlUiManager->loadScreen(kDocument);
}

// ToggleQuitMenu()'s button set-up: QuitNoSave.wnd in a multiplayer game or a replay, and the labels it sets.
void RmlQuitMenuScreen::refresh()
{
	const bool multiplayer = TheGameLogic->isInMultiplayerGame();
	const bool skirmish = TheGameLogic->isInSkirmishGame();
	const bool replay = TheGameLogic->isInReplayGame();
	m_showSaveLoad = !multiplayer && !replay;
	// The menu opens paused, and the pause turns the game's input off: what it was before is the pause's memory. Read
	// after the pause, the input greyed Options and Save/Load in every campaign and skirmish (29/10).
	const Bool inputOn = TheGameLogic->isGamePaused() ? TheGameLogic->getInputEnabledMemory() : TheInGameUI->getInputEnabled();
	m_cinematic = inputOn == FALSE;
	const char *restartKey = "GUI:RestartMission";
	m_restartDisabled = false;
	if (multiplayer && !skirmish)
	{
		restartKey = "GUI:Surrender";
		Player *local = ThePlayerList->getLocalPlayer();
		m_restartDisabled = !local->isPlayerActive() || TheVictoryConditions->isLocalAlliedVictory();	// beaten already
	}
	else if (skirmish || replay)
		restartKey = "GUI:RestartGame";
	m_restartLabel = unicodeToUtf8(TheGameText->fetch(restartKey));
	m_exitLabel = unicodeToUtf8(TheGameText->fetch((multiplayer || skirmish || replay) ? "GUI:Exit" : "GUI:ExitMission"));
	if (m_model)
		m_model.DirtyAllVariables();
}

void RmlQuitMenuScreen::open()
{
	load();
	if (!m_document)
		return;
	if (!isVisible())
	{
		refresh();
		TransitionSounds::play(m_showSaveLoad ? "QuitFull" : "QuitNoSave");	// the .wnd layouts' entrance groups
	}
	TheRmlUiManager->showScreen(m_document);
}

void RmlQuitMenuScreen::close()
{
	if (m_document)
		m_document->Hide();
}

bool RmlQuitMenuScreen::isVisible() const
{
	return m_document && m_document->IsVisible();
}

void RmlQuitMenuScreen::back()
{
	TransitionSounds::play(m_showSaveLoad ? "QuitFullBack" : "QuitNoSaveBack", TRUE);
	ToggleQuitMenu();	// closes it and unpauses, as the .wnd's Return button
}

void RmlQuitMenuScreen::reload()
{
	if (!m_document)
		return;
	const bool shown = isVisible();
	m_document->Close();
	m_document = nullptr;
	load();
	if (shown && m_document)
		TheRmlUiManager->showScreen(m_document);
}

void RmlQuitMenuScreen::release()
{
	m_document = nullptr;
	m_model = Rml::DataModelHandle();
}

void RmlQuitMenuScreen::act(const Rml::String &what)
{
	if (what == "return_to_game")
		back();
	else if (what == "options" && !m_cinematic)
		RmlUiScreenRegistry::open(AsciiString("Menus/OptionsMenu.wnd"));
	else if (what == "save_load" && m_showSaveLoad && !m_cinematic)
		QuitMenuOpenSaveLoad();
	else if (what == "restart" && !m_restartDisabled)
		QuitMenuAskRestart();
	else if (what == "exit")
		QuitMenuAskExit();
}
