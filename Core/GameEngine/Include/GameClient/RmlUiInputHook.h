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

// Ronin @feature 28/10/2026 DX9: RmlUi's input hook, so WindowXlat can hand it the mouse and keys without Core depending
// on RmlUi. Ported from tintinhamans' GeneralsGameCode fork (arctic/go/feat/rmlui); TheRmlUiInputHook lives in WindowXlat.cpp.
#pragma once

class RmlUiInputHook
{
public:
	virtual ~RmlUiInputHook() {}

	// The pointer is over an element of a visible document that takes pointer events.
	virtual bool wantsMouseInput(int mouseX, int mouseY) const = 0;
	// A layer that takes the whole mouse is up. A HUD over the game is not one: it leaves the world its drags.
	virtual bool capturesInput() const = 0;
	// The element under the pointer leaves the middle button and the wheel to the world (the radar).
	virtual bool leavesMiddleToWorld(int mouseX, int mouseY) const = 0;
	// A text field has focus, so keys go to RmlUi before the game.
	virtual bool wantsKeyboardInput() const = 0;

	virtual void processMouseMove(int x, int y) = 0;
	virtual void processMouseButton(int button, bool down) = 0;	// 0 left, 1 right, 2 middle
	virtual void processMouseWheel(float delta) = 0;
	// false: not taken, the game handles the key. The state is 16 bits so KEY_STATE_AUTOREPEAT survives.
	virtual bool processKey(unsigned char engineKey, unsigned short engineKeyState) = 0;

	// Ronin @feature 29/10/2026 DX9: one UTF-16 unit from WM_CHAR, for a focused text field (the beacon's): DirectInput, the
	// game's key path, gives scan codes, not text. WinMain's window procedure sends it while wantsKeyboardInput().
	virtual void processTextInput(unsigned short utf16Char) = 0;

	// Ronin @feature 28/10/2026 DX9: the modern control bar is on (Options.ini ModernUI): ControlBar::init() runs its windows
	// headless and RmlUi draws the bar over them.
	virtual bool drawsControlBar() const = 0;
};

extern RmlUiInputHook *TheRmlUiInputHook;	// null until RmlUi is up
