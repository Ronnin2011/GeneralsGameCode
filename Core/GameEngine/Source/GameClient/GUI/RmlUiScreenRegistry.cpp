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

// Ronin @feature 29/10/2026 DX9: the shell screens RmlUi draws in place of their .wnd. After tintinhamans' registry
// (GeneralsGameCode, arctic/go/feat/rmlui), cut to what the main menu, the game menu and Options need.
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/RmlUiScreenRegistry.h"

#include "Common/AsciiString.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Shell.h"
#include "GameClient/WindowLayout.h"

#include <vector>

namespace
{
	struct Entry
	{
		const char *wndPath;
		RmlUiScreenHooks hooks;
		unsigned order;		// when it last opened: the newest open screen takes Escape
	};

	unsigned s_openCount = 0;

	std::vector<Entry> &entries()
	{
		static std::vector<Entry> s_entries;
		return s_entries;
	}

	Entry *find(const AsciiString &wndPath)
	{
		std::vector<Entry> &e = entries();
		for (size_t i = 0; i < e.size(); ++i)
			if (wndPath.compareNoCase(e[i].wndPath) == 0)
				return &e[i];
		return nullptr;
	}

	bool visible(const Entry &e)
	{
		return e.hooks.isVisible && e.hooks.isVisible();
	}

	// GWM_DESTROY reaches the placeholder from the destroy list a frame after winDestroy(); winGetUserData() names the
	// screen and must be cleared by then (processDestroyList asserts it).
	WindowMsgHandledType placeholderSystem(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
	{
		if (msg == GWM_DESTROY)
		{
			const char *wndPath = (const char *)window->winGetUserData();
			window->winSetUserData(nullptr);
			Entry *e = wndPath ? find(AsciiString(wndPath)) : nullptr;
			if (e && e->hooks.destroyed)
				e->hooks.destroyed();
			return MSG_HANDLED;
		}
		return GameWinDefaultSystem(window, msg, mData1, mData2);
	}

	void layoutInit(WindowLayout *layout, void *userData)
	{
		layout->hide(FALSE);	// routed: opens the screen
	}

	void layoutUpdate(WindowLayout *layout, void *userData)
	{
		Entry *e = find(layout->getFilename());
		if (e && e->hooks.update)
			e->hooks.update();
	}

	// No closing animation to wait for: the shell's push or pop completes now.
	void layoutShutdown(WindowLayout *layout, void *userData)
	{
		layout->hide(TRUE);
		if (TheShell && TheShell->top() == layout)
			TheShell->shutdownComplete(layout);
	}
}

//-------------------------------------------------------------------------------------------------
void RmlUiScreenRegistry::registerScreen(const char *wndPath, const RmlUiScreenHooks &hooks)
{
	Entry entry = { wndPath, hooks, 0 };
	Entry *existing = find(AsciiString(wndPath));
	if (existing)
		*existing = entry;
	else
		entries().push_back(entry);
}

void RmlUiScreenRegistry::unregisterAll()
{
	entries().clear();
}

bool RmlUiScreenRegistry::isRegistered(const AsciiString &wndPath)
{
	return find(wndPath) != nullptr;
}

// A screen already up keeps its place: bringForward() and hide(FALSE) reopen it on their way.
bool RmlUiScreenRegistry::open(const AsciiString &wndPath)
{
	Entry *e = find(wndPath);
	if (!e || !e->hooks.open)
		return false;
	if (!visible(*e))
		e->order = ++s_openCount;
	e->hooks.open();
	return true;
}

bool RmlUiScreenRegistry::close(const AsciiString &wndPath)
{
	Entry *e = find(wndPath);
	if (!e || !e->hooks.close)
		return false;
	e->hooks.close();
	return true;
}

bool RmlUiScreenRegistry::isOpen(const AsciiString &wndPath)
{
	const Entry *e = find(wndPath);
	return e && visible(*e);
}

bool RmlUiScreenRegistry::back(const AsciiString &wndPath)
{
	Entry *e = find(wndPath);
	if (!e || !e->hooks.back)
		return false;
	e->hooks.back();
	return true;
}

bool RmlUiScreenRegistry::anyOpen()
{
	const std::vector<Entry> &e = entries();
	for (size_t i = 0; i < e.size(); ++i)
		if (visible(e[i]))
			return true;
	return false;
}

bool RmlUiScreenRegistry::escape()
{
	const Entry *top = nullptr;
	const std::vector<Entry> &e = entries();
	for (size_t i = 0; i < e.size(); ++i)
		if (visible(e[i]) && (!top || e[i].order > top->order))
			top = &e[i];
	if (!top)
		return false;
	if (top->hooks.back)
		top->hooks.back();
	return true;
}

//-------------------------------------------------------------------------------------------------
WindowLayout *RmlUiScreenRegistry::createLayout(const AsciiString &wndPath)
{
	Entry *e = find(wndPath);
	WindowLayout *layout = newInstance(WindowLayout);
	layout->loadEmpty(wndPath);
	GameWindow *placeholder = TheWindowManager->winCreate(nullptr, WIN_STATUS_HIDDEN, 0, 0, 0, 0, placeholderSystem, nullptr);
	if (placeholder)
	{
		placeholder->winSetUserData((void *)(e ? e->wndPath : nullptr));
		layout->addWindow(placeholder);
	}
	layout->setInit(layoutInit);
	layout->setUpdate(layoutUpdate);
	layout->setShutdown(layoutShutdown);
	layout->routeToRmlUi(TRUE);
	if (e && e->hooks.created)
		e->hooks.created();
	return layout;
}
