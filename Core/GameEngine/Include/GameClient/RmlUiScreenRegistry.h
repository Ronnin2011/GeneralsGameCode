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

// Ronin @feature 29/10/2026 DX9: the shell screens RmlUi draws in place of their .wnd (Options.ini ModernUI). Core asks here and
// never depends on RmlUi; the device's RmlUiManager registers them. After tintinhamans' registry (GeneralsGameCode, arctic/go/feat/rmlui).
#pragma once

#include "Lib/BaseType.h"

class AsciiString;
class WindowLayout;

typedef void (*RmlUiScreenFunc)();
typedef bool (*RmlUiScreenQueryFunc)();

struct RmlUiScreenHooks
{
	RmlUiScreenFunc open = nullptr;
	RmlUiScreenFunc close = nullptr;
	RmlUiScreenQueryFunc isVisible = nullptr;
	RmlUiScreenFunc back = nullptr;			// Escape, while it is the newest open screen
	RmlUiScreenFunc created = nullptr;		// its placeholder window made / gone: the .wnd root's GWM_CREATE / GWM_DESTROY
	RmlUiScreenFunc destroyed = nullptr;
	RmlUiScreenFunc update = nullptr;		// the shell's update of its layout, for as long as the layout is on the stack
};

class RmlUiScreenRegistry
{
public:
	static void registerScreen(const char *wndPath, const RmlUiScreenHooks &hooks);	// wndPath: a literal, it is kept
	static void unregisterAll();

	static bool isRegistered(const AsciiString &wndPath);
	static bool open(const AsciiString &wndPath);		// false: not registered
	static bool close(const AsciiString &wndPath);
	static bool isOpen(const AsciiString &wndPath);
	static bool back(const AsciiString &wndPath);

	static bool anyOpen();		// a screen is up: it owns the mouse
	static bool escape();		// the newest open screen's Escape; false when none is open

	// For winCreateLayout(): a layout holding one hidden placeholder window. runInit / hide(FALSE) / bringForward open the
	// screen and runShutdown / hide(TRUE) close it, as the .wnd's own callbacks would.
	static WindowLayout *createLayout(const AsciiString &wndPath);
};
