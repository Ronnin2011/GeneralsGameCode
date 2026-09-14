/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 debug panel — hosts the HUD readouts in one translucent, draggable game window.
*/

// Ronin @feature 14/09/2026 DX9: debug panel. Design, steps and what the window system requires: docs/Debug_Panel_Design.md.

#include <string.h>

#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/Recorder.h"
#include "GameLogic/GameLogic.h"
#include "GameClient/Color.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameFont.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/InGameUI.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/Mouse.h"
#include "GameClient/SelectionXlat.h"
#include "GameClient/View.h"
#include "W3DDevice/GameClient/W3DDebugPanel.h"

namespace
{
	// Ronin @feature 14/09/2026 DX9: ids no .wnd layout uses. Both windows are looked up every frame, because
	// GameWindowManager::reset() destroys every window and a cached pointer would dangle.
	const Int PANEL_WINDOW_ID = 0x7DEB0001;
	const Int ENTRY_WINDOW_ID = 0x7DEB0002;
	const Int PAD             = 4;
	const Int TITLE_HEIGHT    = 14;
	const Int ENTRY_HEIGHT    = 20;			// the entry draws its text 5 px down (W3DTextEntry.cpp:301)
	const Int ENTRY_MIN_WIDTH = 320;
	const Int DEFAULT_X       = 3;
	const Int DEFAULT_Y       = 300;		// where the first readout line used to sit
	enum { OUTPUT_LINES = 6, HISTORY_SIZE = 16, PANEL_MAX_COMMANDS = 64, MAX_ARGS = 16 };

	struct PanelRow
	{
		DisplayString *text;
		Color          color;
		UnsignedInt    stamp;					// panel frame the row was last set in
	};

	struct PanelCommand
	{
		const char                 *name;
		const char                 *help;
		W3DDebugPanel::CommandFunc  func;
	};

	PanelRow       s_rows[W3DDebugPanel::ROW_COUNT] = {};
	UnsignedInt    s_frame        = 1;
	Bool           s_available    = FALSE;
	Bool           s_visible      = false;
	Int            s_posX         = DEFAULT_X;
	Int            s_posY         = DEFAULT_Y;
	Int            s_outputTop    = 0;		// output area's y inside the panel, set by update for the draw
	DisplayString *s_title        = nullptr;
	Bool           s_mouseWasDown = FALSE;

	DisplayString *s_out[OUTPUT_LINES]      = {};
	Color          s_outColor[OUTPUT_LINES] = {};
	Int            s_outCount               = 0;

	AsciiString    s_history[HISTORY_SIZE];
	Int            s_historyCount = 0;
	Int            s_historyPos   = 0;		// == s_historyCount means "past the newest", an empty box

	PanelCommand   s_commands[PANEL_MAX_COMMANDS] = {};
	Int            s_commandCount = 0;
	Bool           s_builtinsDone = FALSE;

	GameFont *panelFont()
	{
		return (TheFontLibrary != nullptr) ? TheFontLibrary->getFont("FixedSys", 8, FALSE) : nullptr;
	}

	// Ronin @feature 14/09/2026 DX9: the panel draws inside winRepaint, BEFORE this frame's readouts run, so a row set last
	// frame is still live. A readout that stops calling setRow drops out one frame later.
	Bool rowIsLive(Int r)
	{
		return s_rows[r].text != nullptr && s_rows[r].stamp + 1 >= s_frame;
	}

	void printLine(const UnicodeString &text, Bool isError)
	{
		if (TheDisplayStringManager == nullptr)
			return;
		if (s_outCount == OUTPUT_LINES)
		{
			// Scroll: the oldest string object is reused for the new line.
			DisplayString *oldest = s_out[0];
			for (Int i = 0; i < OUTPUT_LINES - 1; ++i)
			{
				s_out[i]      = s_out[i + 1];
				s_outColor[i] = s_outColor[i + 1];
			}
			s_out[OUTPUT_LINES - 1] = oldest;
			--s_outCount;
		}
		if (s_out[s_outCount] == nullptr)
		{
			s_out[s_outCount] = TheDisplayStringManager->newDisplayString();
			if (s_out[s_outCount] == nullptr)
				return;
			s_out[s_outCount]->setFont(panelFont());
		}
		s_out[s_outCount]->setText(text);
		s_outColor[s_outCount] = isError ? GameMakeColor(255, 120, 80, 255) : GameMakeColor(220, 220, 220, 255);
		++s_outCount;
	}

	void printAscii(const AsciiString &text, Bool isError)
	{
		UnicodeString u;
		u.translate(text);
		printLine(u, isError);
	}

	void cmdHelp(Int argc, const AsciiString *argv)
	{
		for (Int i = 0; i < s_commandCount; ++i)
		{
			AsciiString line;
			line.format("%s - %s", s_commands[i].name, s_commands[i].help);
			printAscii(line, FALSE);
		}
	}

	void cmdClear(Int argc, const AsciiString *argv)
	{
		s_outCount = 0;
	}

	// Ronin @feature 14/09/2026 DX9: same as Ctrl+Shift+D. update() hides the panel next frame and hands the keys back.
	void cmdClose(Int argc, const AsciiString *argv)
	{
		s_visible = FALSE;
	}

	// Ronin @feature 14/09/2026 DX9: the engine's own benchmark exit (GameEngine.cpp:968-976) — finish the replay, drop the
	// match, quit. Not the Alt+F4 path: in a match that opens the quit menu first. Online, the others see a disconnect.
	void cmdShutdown(Int argc, const AsciiString *argv)
	{
		if (TheGameLogic != nullptr && TheGameLogic->isInGame())
		{
			if (TheRecorder != nullptr && TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
				TheRecorder->stopRecording();
			TheGameLogic->clearGameData(FALSE);
		}
		if (TheGameEngine != nullptr)
			TheGameEngine->setQuitting(TRUE);
	}


	void runCommand(const UnicodeString &line)
	{
		AsciiString text;
		text.translate(line);
		text.trim();
		if (text.isEmpty())
			return;

		// History, oldest first; a repeat of the last command is not stored twice.
		if (s_historyCount == 0 || strcmp(s_history[s_historyCount - 1].str(), text.str()) != 0)
		{
			if (s_historyCount == HISTORY_SIZE)
			{
				for (Int i = 0; i < HISTORY_SIZE - 1; ++i)
					s_history[i] = s_history[i + 1];
				--s_historyCount;
			}
			s_history[s_historyCount++] = text;
		}
		s_historyPos = s_historyCount;

		UnicodeString echo;
		echo.format(L"> %ls", line.str());
		printLine(echo, FALSE);

		AsciiString argv[MAX_ARGS];
		Int argc = 0;
		AsciiString rest = text;
		AsciiString token;
		while (argc < MAX_ARGS && rest.nextToken(&token, " \t"))
			argv[argc++] = token;
		if (argc == 0)
			return;

		for (Int i = 0; i < s_commandCount; ++i)
		{
			if (stricmp(s_commands[i].name, argv[0].str()) == 0)
			{
				s_commands[i].func(argc, argv);
				return;
			}
		}
		AsciiString err;
		err.format("unknown command '%s' - type help", argv[0].str());
		printAscii(err, TRUE);
	}

	void recallHistory(GameWindow *entry, Int step)
	{
		if (s_historyCount == 0)
			return;
		s_historyPos += step;
		if (s_historyPos < 0)
			s_historyPos = 0;
		if (s_historyPos >= s_historyCount)
		{
			s_historyPos = s_historyCount;
			GadgetTextEntrySetText(entry, UnicodeString::TheEmptyString);
			return;
		}
		UnicodeString u;
		u.translate(s_history[s_historyPos]);
		GadgetTextEntrySetText(entry, u);
	}

	// Ronin @feature 14/09/2026 DX9: a wall for mouse input, so the panel drags and the battlefield under it is not clicked.
	// Same as GameWinBlockInput, which touches the selection translator, tactical view and InGameUI without null checks.
	WindowMsgHandledType panelInput(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
	{
		if (msg == GWM_CHAR || msg == GWM_MOUSE_POS)
			return MSG_IGNORED;

		if (msg == GWM_LEFT_UP)
		{
			// Stop a drag-select that was released over the panel.
			if (TheSelectionTranslator != nullptr)
			{
				TheSelectionTranslator->setLeftMouseButton(FALSE);
				TheSelectionTranslator->setDragSelecting(FALSE);
			}
			if (TheTacticalView != nullptr)
				TheTacticalView->setMouseLock(FALSE);
			if (TheInGameUI != nullptr)
			{
				TheInGameUI->setSelecting(FALSE);
				TheInGameUI->endAreaSelectHint(nullptr);
			}
		}
		return MSG_HANDLED;
	}

	// Ronin @feature 14/09/2026 DX9: Enter in the command box — the entry sends GEM_EDIT_DONE to its owner, this panel.
	WindowMsgHandledType panelSystem(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
	{
		if (msg != GEM_EDIT_DONE)
			return MSG_IGNORED;

		GameWindow *entry = (GameWindow *)mData1;
		if (entry != nullptr)
		{
			const UnicodeString line = GadgetTextEntryGetText(entry);
			GadgetTextEntrySetText(entry, UnicodeString::TheEmptyString);
			runCommand(line);
		}
		return MSG_HANDLED;
	}

	// Ronin @feature 14/09/2026 DX9: the stock entry input, minus keys that would leak: ESC reached the game menu, arrows and
	// Tab moved focus to menu buttons. Up/Down recall history; ESC clears on press and hands the keys back on release.
	WindowMsgHandledType entryInput(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
	{
		if (msg == GWM_CHAR)
		{
			const Bool down = BitIsSet(mData2, KEY_STATE_DOWN);
			switch (mData1)
			{
				case KEY_ESC:
					if (down)
					{
						GadgetTextEntrySetText(window, UnicodeString::TheEmptyString);
						s_historyPos = s_historyCount;
					}
					else
					{
						TheWindowManager->winSetFocus(nullptr);
					}
					return MSG_HANDLED;

				case KEY_UP:
				case KEY_DOWN:
					if (down)
						recallHistory(window, (mData1 == KEY_UP) ? -1 : 1);
					return MSG_HANDLED;

				case KEY_TAB:
				case KEY_LEFT:
				case KEY_RIGHT:
					return MSG_HANDLED;
			}
		}
		return GadgetTextEntryInput(window, msg, mData1, mData2);
	}

	GameWindow *createEntry(GameWindow *panel)
	{
		WinInstanceData inst;
		inst.init();
		inst.m_style = GWS_ENTRY_FIELD;

		EntryData data;
		memset(&data, 0, sizeof(data));
		data.maxTextLen = ENTRY_TEXT_LEN;
		data.aSCIIOnly  = TRUE;

		GameFont *font = panelFont();
		GameWindow *entry = TheWindowManager->gogoGadgetTextEntry(panel, WIN_STATUS_ENABLED,
			PAD, TITLE_HEIGHT + PAD, ENTRY_MIN_WIDTH, ENTRY_HEIGHT, &inst, &data, font, FALSE);
		if (entry == nullptr)
			return nullptr;

		entry->winSetWindowId(ENTRY_WINDOW_ID);
		entry->winSetInputFunc(entryInput);
		if (font != nullptr)
			GadgetTextEntrySetFont(entry, font);

		// Translucent like the panel; a brighter border while it has focus (focus sets the hilite state).
		const Color back   = GameMakeColor(0, 0, 0, 110);
		const Color border = GameMakeColor(160, 160, 160, 170);
		const Color drop   = GameMakeColor(0, 0, 0, 255);
		const Color white  = GameMakeColor(255, 255, 255, 255);
		GadgetTextEntrySetEnabledColor(entry, back);
		GadgetTextEntrySetEnabledBorderColor(entry, border);
		GadgetTextEntrySetHiliteColor(entry, back);
		GadgetTextEntrySetHiliteBorderColor(entry, GameMakeColor(230, 230, 230, 230));
		GadgetTextEntrySetDisabledColor(entry, back);
		GadgetTextEntrySetDisabledBorderColor(entry, border);
		entry->winSetEnabledTextColors(white, drop);
		entry->winSetHiliteTextColors(white, drop);
		entry->winSetDisabledTextColors(GameMakeColor(140, 140, 140, 255), drop);
		entry->winSetIMECompositeTextColors(white, drop);
		return entry;
	}

	// Ronin @feature 14/09/2026 DX9: translucent body, a darker title strip to grab, the rows in their own colours, then the
	// output lines under the command box (the box draws itself, as a child, after this).
	void panelDraw(GameWindow *window, WinInstanceData *instData)
	{
		Int x, y, w, h;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);

		TheWindowManager->winFillRect(GameMakeColor(0, 0, 0, 95), 1.0f, x, y, x + w, y + h);
		TheWindowManager->winFillRect(GameMakeColor(60, 60, 60, 140), 1.0f, x, y, x + w, y + TITLE_HEIGHT);
		TheWindowManager->winOpenRect(GameMakeColor(160, 160, 160, 170), 1.0f, x, y, x + w, y + h);

		const Color drop = GameMakeColor(0, 0, 0, 255);
		if (s_title != nullptr)
			s_title->draw(x + PAD, y + 1, GameMakeColor(220, 220, 220, 255), drop);

		Int rowY = y + TITLE_HEIGHT + PAD;
		for (Int r = 0; r < W3DDebugPanel::ROW_COUNT; ++r)
		{
			if (!rowIsLive(r))
				continue;
			Int tw = 0, th = 0;
			s_rows[r].text->getSize(&tw, &th);
			s_rows[r].text->draw(x + PAD, rowY, s_rows[r].color, drop);
			rowY += th;
		}

		Int outY = y + s_outputTop;
		for (Int i = 0; i < s_outCount; ++i)
		{
			Int tw = 0, th = 0;
			s_out[i]->getSize(&tw, &th);
			s_out[i]->draw(x + PAD, outY, s_outColor[i], drop);
			outY += th;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void W3DDebugPanel::update(void)
{
	++s_frame;
	s_available = FALSE;

	if (TheWindowManager == nullptr || TheGlobalData == nullptr || TheGlobalData->m_headless)
		return;

	if (!s_builtinsDone)
	{
		s_builtinsDone = TRUE;
		registerCommand("help", "list the commands", cmdHelp);
		registerCommand("clear", "clear this output", cmdClear);
		registerCommand("close", "hide this panel (Ctrl+Shift+D shows it again)", cmdClose);
		registerCommand("shutdown", "quit the game to the desktop now", cmdShutdown);
	}

	GameWindow *win = TheWindowManager->winGetWindowFromId(nullptr, PANEL_WINDOW_ID);
	if (win == nullptr)
	{
		// Created, or recreated after a reset, where the player last left it — kept on screen if the resolution shrank.
		if (TheDisplay != nullptr)
		{
			if (s_posX > (Int)TheDisplay->getWidth() - 40)  s_posX = DEFAULT_X;
			if (s_posY > (Int)TheDisplay->getHeight() - 40) s_posY = DEFAULT_Y;
		}
		win = TheWindowManager->winCreate(nullptr,
			WIN_STATUS_ENABLED | WIN_STATUS_DRAGGABLE | WIN_STATUS_ABOVE | WIN_STATUS_NO_FOCUS,
			s_posX, s_posY, 100, TITLE_HEIGHT + 2 * PAD, nullptr, nullptr);
		if (win == nullptr)
			return;
		win->winSetWindowId(PANEL_WINDOW_ID);
		win->winSetInputFunc(panelInput);
		win->winSetSystemFunc(panelSystem);
		win->winSetDrawFunc(panelDraw);
	}
	s_available = TRUE;

	// Remember where the player dragged it, for the next time a reset destroys it.
	win->winGetPosition(&s_posX, &s_posY);

	// Search the panel's children only — a nullptr start would search every window.
	GameWindow *entry = (win->winGetChild() != nullptr)
		? TheWindowManager->winGetWindowFromId(win->winGetChild(), ENTRY_WINDOW_ID) : nullptr;
	if (entry == nullptr)
		entry = createEntry(win);
	const Bool entryFocused = (entry != nullptr && TheWindowManager->winGetFocus() == entry);

	// Ronin @feature 14/09/2026 DX9: the window manager never takes focus back on a click elsewhere, so a box left focused
	// would keep swallowing hotkeys. A button press outside the panel hands the keys back to the game.
	if (TheMouse != nullptr)
	{
		const MouseIO *mouse = TheMouse->getMouseStatus();
		const Bool down = mouse->leftState == MBS_Down || mouse->leftState == MBS_DoubleClick ||
						  mouse->rightState == MBS_Down || mouse->rightState == MBS_DoubleClick;
		if (down && !s_mouseWasDown && entryFocused)
		{
			Int px = 0, py = 0, pw = 0, ph = 0;
			win->winGetScreenPosition(&px, &py);
			win->winGetSize(&pw, &ph);
			if (mouse->pos.x < px || mouse->pos.x >= px + pw || mouse->pos.y < py || mouse->pos.y >= py + ph)
				TheWindowManager->winSetFocus(nullptr);
		}
		s_mouseWasDown = down;
	}

	if (s_title == nullptr && TheDisplayStringManager != nullptr && TheFontLibrary != nullptr)
	{
		s_title = TheDisplayStringManager->newDisplayString();
		if (s_title != nullptr)
		{
			s_title->setFont(panelFont());
			s_title->setText(UnicodeString(L"DEBUG"));
		}
	}

	// Size to the widest of title, rows, command box and output; rows, box and output stack under the title.
	Int width = 0, rowsHeight = 0, outHeight = 0;
	if (s_title != nullptr)
	{
		Int tw = 0, th = 0;
		s_title->getSize(&tw, &th);
		width = tw;
	}
	for (Int r = 0; r < ROW_COUNT; ++r)
	{
		if (!rowIsLive(r))
			continue;
		Int tw = 0, th = 0;
		s_rows[r].text->getSize(&tw, &th);
		if (tw > width) width = tw;
		rowsHeight += th;
	}
	for (Int i = 0; i < s_outCount; ++i)
	{
		Int tw = 0, th = 0;
		s_out[i]->getSize(&tw, &th);
		if (tw > width) width = tw;
		outHeight += th;
	}
	if (width < ENTRY_MIN_WIDTH)
		width = ENTRY_MIN_WIDTH;

	const Bool show = s_visible && !TheGlobalData->m_loadScreenRender;
	if (!show)
	{
		// Hiding clears focus WITHOUT telling the box (GameWindowManager::windowHiding), which would leave the IME hooked to it.
		if (entryFocused)
			TheWindowManager->winSetFocus(nullptr);
		if (!win->winIsHidden())
			win->winHide(TRUE);
		return;
	}
	if (win->winIsHidden())
		win->winHide(FALSE);

	const Int entryY = TITLE_HEIGHT + PAD + rowsHeight + ((rowsHeight > 0) ? PAD : 0);
	s_outputTop = entryY + ENTRY_HEIGHT + PAD;
	const Int panelW = width + 2 * PAD;
	const Int panelH = s_outputTop + ((outHeight > 0) ? outHeight + PAD : 0);

	Int curW = 0, curH = 0;
	win->winGetSize(&curW, &curH);
	if (curW != panelW || curH != panelH)
		win->winSetSize(panelW, panelH);

	if (entry != nullptr)
	{
		Int ex = 0, ey = 0, ew = 0, eh = 0;
		entry->winGetPosition(&ex, &ey);
		entry->winGetSize(&ew, &eh);
		if (ex != PAD || ey != entryY)
			entry->winSetPosition(PAD, entryY);
		if (ew != width || eh != ENTRY_HEIGHT)
			entry->winSetSize(width, ENTRY_HEIGHT);
	}
}

//-------------------------------------------------------------------------------------------------
void W3DDebugPanel::toggleVisible(void)
{
	s_visible = !s_visible;
}

//-------------------------------------------------------------------------------------------------
Bool W3DDebugPanel::setRow(Row row, DisplayString *text, Color color)
{
	if (!s_available || row < 0 || row >= ROW_COUNT || text == nullptr)
		return FALSE;

	s_rows[row].text  = text;
	s_rows[row].color = color;
	s_rows[row].stamp = s_frame;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
Bool W3DDebugPanel::registerCommand(const char *name, const char *help, CommandFunc func)
{
	if (name == nullptr || func == nullptr)
		return FALSE;
	for (Int i = 0; i < s_commandCount; ++i)
	{
		if (stricmp(s_commands[i].name, name) == 0)
		{
			s_commands[i].help = (help != nullptr) ? help : "";
			s_commands[i].func = func;
			return TRUE;
		}
	}
	if (s_commandCount >= PANEL_MAX_COMMANDS)
		return FALSE;
	s_commands[s_commandCount].name = name;
	s_commands[s_commandCount].help = (help != nullptr) ? help : "";
	s_commands[s_commandCount].func = func;
	++s_commandCount;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void W3DDebugPanel::print(const UnicodeString &text, Bool isError)
{
	printLine(text, isError);
}
