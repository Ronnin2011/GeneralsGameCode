/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 debug panel — hosts the HUD readouts in one translucent, draggable game window.
*/

// Ronin @feature 14/09/2026 DX9: debug panel. A real GameWindow, so it drags and blocks clicks, drawn with a translucent
// background. docs/Debug_Panel_Design.md.
// Ronin @feature 28/09/2026 DX9: every readout is built in W3DDebugPanel.cpp now, each behind a `rows` switch.
#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"
#include "GameClient/Color.h"

class DisplayString;

class W3DDebugPanel
{
public:

	// Ronin @feature 14/09/2026 DX9: one row per readout, drawn top to bottom in this order.
	enum Row
	{
		ROW_SR = 0,
		ROW_INST,
		ROW_DRAW,
		ROW_DRAW2,
		ROW_SHADOW,
		ROW_PERF,
		ROW_DEPTH,
		ROW_TERRAIN,
		ROW_UI,			// Ronin @diagnostic 15/09/2026 DX9: input ownership
		ROW_RSTATE,		// Ronin @diagnostic 20/09/2026 DX9: device-call counters, off by default
		ROW_TAA,		// Ronin @diagnostic 20/09/2026 DX9: TAA state; shown whenever TAA is on
		ROW_WATER,		// Ronin @diagnostic 28/09/2026 DX9: water shader load/use and draws, docs/Water_Work.md §5
		ROW_COUNT
	};

	// Ronin @feature 14/09/2026 DX9: a console command. argv[0] is the command name, so argc >= 1. Render/client-side
	// state only — anything that changes game logic desyncs multiplayer and replays.
	typedef void (*CommandFunc)(Int argc, const AsciiString *argv);

	// Ronin @feature 14/09/2026 DX9: once per frame, BEFORE the UI draws. Finds or recreates the window and its command
	// box (a window-manager reset destroys every window), builds the rows and sizes the window to them.
	static void update(void);

	// Ronin @feature 14/09/2026 DX9: Ctrl+Shift+Z, through Display::toggleDebugPanel. Session only, never saved.
	static void toggleVisible(void);

	// Ronin @feature 14/09/2026 DX9: a readout hands over its string for this frame. FALSE = no panel this frame.
	static Bool setRow(Row row, DisplayString *text, Color color);

	// Ronin @feature 14/09/2026 DX9: add a command; the same name again replaces it. FALSE when the table is full.
	static Bool registerCommand(const char *name, const char *help, CommandFunc func);

	// Ronin @feature 14/09/2026 DX9: one line into the output area under the command box; the oldest scrolls off.
	static void print(const UnicodeString &text, Bool isError = FALSE);
};
