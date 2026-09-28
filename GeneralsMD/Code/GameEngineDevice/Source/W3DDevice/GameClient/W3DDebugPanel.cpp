/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 debug panel — hosts the HUD readouts in one translucent, draggable game window.
*/

// Ronin @feature 14/09/2026 DX9: debug panel. Design, steps and what the window system requires: docs/Debug_Panel_Design.md.

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "Common/GameEngine.h"
#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Recorder.h"
#include "Common/ThingFactory.h"
#include "Common/ThingSort.h"
#include "Common/ThingTemplate.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/TerrainLogic.h"
#include "GameClient/Color.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameFont.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/InGameUI.h"
#include "GameClient/Keyboard.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/Mouse.h"
#include "GameClient/SelectionXlat.h"
#include "GameClient/View.h"
#include "W3DDevice/GameClient/W3DDebugGrid.h"
#include "W3DDevice/GameClient/W3DDebugPanel.h"
#include "W3DDevice/GameClient/W3DWater.h"
#include "WW3D2/dx8wrapper.h"
#include "W3DDevice/GameClient/W3DTaa.h"
#include "W3DDevice/GameClient/W3DSsao.h"	// Ronin @feature 26/09/2026 DX9: `ssao` command
#include "W3DDevice/GameClient/W3DShadowMap.h"	// Ronin @feature 26/09/2026 DX9: `shadows` command
// Ronin @feature 28/09/2026 DX9: the readouts moved in from W3DDisplay.cpp.
#include "W3DDevice/GameClient/BaseHeightMap.h"
#include "W3DDevice/GameClient/WorldHeightMap.h"
#include "WW3D2/dx8instancing.h"
#include "WW3D2/statistics.h"

// Ronin @feature 28/09/2026 DX9: [SR] counters, in dx8instancing.cpp. Declared out here: inside the namespace below they
// would name internal functions that do not exist.
extern unsigned DX8_Get_Single_Rigid_Last_Frame_Draw_Count();
extern unsigned DX8_Get_Single_Rigid_Last_Frame_Flush_Count();

namespace
{
	// Ronin @feature 14/09/2026 DX9: ids no .wnd layout uses. Both windows are looked up every frame, because
	// GameWindowManager::reset() destroys every window and a cached pointer would dangle.
	const Int PANEL_WINDOW_ID = 0x7DEB0001;
	const Int ENTRY_WINDOW_ID = 0x7DEB0002;
	const Int LIST_WINDOW_ID  = 0x7DEB0003;	// Ronin @feature 16/09/2026 DX9: spawn picker list
	const Int PAD             = 4;
	const Int TITLE_HEIGHT    = 14;
	const Int ENTRY_HEIGHT    = 20;			// the entry draws its text 5 px down (W3DTextEntry.cpp:301)
	const Int ENTRY_MIN_WIDTH = 320;
	const Int OUTPUT_WRAP     = 110;		// Ronin @bugfix 27/09/2026 DX9: output characters per line before printAscii wraps
	const Int DEFAULT_X       = 3;
	const Int DEFAULT_Y       = 300;		// where the first readout line used to sit
	// Ronin @feature 16/09/2026 DX9: spawn picker size; the list's column width is set once at creation, so its width is fixed.
	const Int PICKER_WIDTH    = 600;
	const Int PICKER_HEIGHT   = 240;
	const Int LABEL_HEIGHT    = 14;
	// Ronin @feature 15/09/2026 DX9: OUTPUT_LINES 6 -> 10, `help` plus its echo no longer fitted.
	enum { OUTPUT_LINES = 10, HISTORY_SIZE = 16, PANEL_MAX_COMMANDS = 64, MAX_ARGS = 16, PICK_ROWS = 400, MAX_SIDES = 48,
		   // Ronin @feature 16/09/2026 DX9: copies per drop, and ghosts previewing them. Structures are big and rarely wanted
		   // in bulk; units are cheap to look at and useful in numbers.
		   PLACE_MAX_UNITS = 30, PLACE_MAX_STRUCTURES = 3, PLACE_MAX_COUNT = PLACE_MAX_UNITS };

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

	// Ronin @feature 28/09/2026 DX9: `rows` switches, defaults as W3DDisplay.cpp's old SHOW_* constants. [DRAW2] shares draw's
	// name, so one switch drives both.
	const char    *const ROW_NAMES[W3DDebugPanel::ROW_COUNT] =
		{ "sr", "inst", "draw", "draw", "shadow", "perf", "depth", "terrain", "ui", "rstate", "taa" };
	Bool           s_rowOn[W3DDebugPanel::ROW_COUNT] =
		{ TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE, TRUE, FALSE, TRUE };
	DisplayString *s_rowText[W3DDebugPanel::ROW_COUNT] = {};	// each row's string, made on first use
	DisplayString *s_noPanel      = nullptr;

	// Ronin @feature 28/09/2026 DX9: [DRAW] and [PERF] read monotonic counters, so they are sampled every frame - hidden,
	// switched off or with no panel - or a skipped frame folds into the next reading.
	unsigned       s_drawPrev[Debug_Statistics::DRAW_SUBSYS_COUNT] = {};
	unsigned       s_drawNow[Debug_Statistics::DRAW_SUBSYS_COUNT]  = {};	// this frame's draws per subsystem
	unsigned       s_drawTotal    = 0;
	Int64          s_perfFreq     = 0;
	Int64          s_perfNow      = 0;
	Int64          s_perfFirst    = 0;
	Int64          s_perfLast     = 0;
	UnsignedInt    s_perfFrames   = 0;
	double         s_perfSMSum    = 0.0;
	Real           s_perfWorstMs  = 0.0f;

	// Ronin @feature 16/09/2026 DX9: spawn picker. The categories it lists, in [Category] label order, with their row colour.
	struct PickCategory
	{
		EditorSortingType  sorting;
		const char        *name;
		UnsignedByte       r, g, b;
	};
	const PickCategory PICK_CATEGORIES[] =
	{
		{ ES_STRUCTURE,     "Structure", 255, 210, 120 },
		{ ES_INFANTRY,      "Infantry",  140, 230, 140 },
		{ ES_VEHICLE,       "Vehicle",   130, 200, 255 },
		{ ES_MISC_MAN_MADE, "ManMade",   210, 210, 210 },
		{ ES_MISC_NATURAL,  "Natural",   180, 200, 150 },
		{ ES_SHRUBBERY,     "Shrubbery", 150, 190, 130 },
	};
	const Int PICK_CATEGORY_COUNT = (Int)(sizeof(PICK_CATEGORIES) / sizeof(PICK_CATEGORIES[0]));

	Bool           s_pickerOpen      = FALSE;
	Bool           s_pickerDirty     = TRUE;	// refill next frame: a label changed or the list was recreated
	UnicodeString  s_pickerText;				// box text the list was last filled from
	Int            s_pickCount       = 1;		// a number typed after "spawn "
	Int            s_pickedID        = -1;		// template id clicked, spawned by update() next frame
	Bool           s_listPressed     = FALSE;	// the left press started on the list
	Int            s_sideIndex       = -1;		// -1 = All
	Int            s_categoryIndex   = -1;		// -1 = All
	AsciiString    s_sides[MAX_SIDES];
	Int            s_sideCount       = -1;		// -1 = not collected yet
	DisplayString *s_sideLabel       = nullptr;
	DisplayString *s_categoryLabel   = nullptr;
	Int            s_labelTop        = 0;		// labels' y inside the panel, set by update for the draw and the click test
	Int            s_sideLabelW      = 0;
	Int            s_categoryLabelX  = 0;
	Int            s_categoryLabelW  = 0;
	Int            s_pressX          = 0;		// last left press on the panel, to tell a label click from a drag
	Int            s_pressY          = 0;

	// Ronin @feature 16/09/2026 DX9: placement mode. The picked object rides the cursor until a click drops one.
	Int            s_placeID         = -1;		// template id on the cursor, -1 = not placing
	Int            s_placeCount      = 1;
	DrawableID     s_ghostIDs[PLACE_MAX_COUNT] = {};	// one ghost per copy, laid out like the drop will be
	Int            s_ghostCount      = 0;
	ICoord2D       s_placePress      = { 0, 0 };	// where the last press landed, to tell a click from a drag
	Bool           s_eatEscUp        = FALSE;	// the Esc that stopped placing must not reach the menu on its way up
	Bool           s_translatorReady = FALSE;
	// Ronin @feature 16/09/2026 DX9: facing. Holding the left button pins the spot and the drag turns the object, like the
	// dozer's placement arrow; the angle is kept for the next drop, and travels with the spawn message.
	Real           s_placeAngle      = 0.0f;
	Bool           s_placeAnchored   = FALSE;		// left button held down over the map
	Coord3D        s_placeAnchor     = { 0.0f, 0.0f, 0.0f };
	const Int      PLACE_DRAG_PIXELS = 5;			// same slack as PlaceEventTranslator's placement arrow
	// Ronin @feature 16/09/2026 DX9: snapping, while Ctrl is held.
	// Ronin @feature 27/09/2026 DX9: 20 = two height-map cells (MAP_XY_FACTOR is 10). The grid draws this same cell.
	const Real     SNAP_CELL         = 20.0f;
	const Real     SNAP_ANGLE        = 1.5707963f;	// a quarter turn; 0.7853982f would snap to eighths instead

	GameFont *panelFont()
	{
		return (TheFontLibrary != nullptr) ? TheFontLibrary->getFont("FixedSys", 8, FALSE) : nullptr;
	}

	// Ronin @feature 28/09/2026 DX9: every row is set inside update(), before the panel draws, so only this frame's rows are
	// live - a row switched off is gone the same frame. (The one-frame grace was for readouts that ran after the draw.)
	Bool rowIsLive(Int r)
	{
		return s_rows[r].text != nullptr && s_rows[r].stamp == s_frame;
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

	// Ronin @bugfix 27/09/2026 DX9: the panel sizes to its widest line, so one long line pushed it past the screen edge and
	// hid the command box. Wrapped at a space every OUTPUT_WRAP characters; continuation lines are indented.
	void printAscii(const AsciiString &text, Bool isError)
	{
		const char *s   = text.str();
		Int         len = (Int)strlen(s);
		Bool        first = TRUE;
		do
		{
			const Int room = first ? OUTPUT_WRAP : OUTPUT_WRAP - 2;
			Int take = len;
			if (len > room)
			{
				take = room;
				while (take > room / 2 && s[take] != ' ')
					--take;
				if (s[take] != ' ')
					take = room;		// no space in the second half: cut hard
			}
			char buf[OUTPUT_WRAP + 1];
			Int  n = 0;
			if (!first)
			{
				buf[n++] = ' ';
				buf[n++] = ' ';
			}
			memcpy(buf + n, s, take);
			buf[n + take] = 0;
			UnicodeString u;
			u.translate(AsciiString(buf));
			printLine(u, isError);
			s   += take;
			len -= take;
			while (len > 0 && *s == ' ')
			{
				++s;
				--len;
			}
			first = FALSE;
		} while (len > 0);
	}

	void cmdHelp(Int argc, const AsciiString *argv)
	{
		for (Int i = 0; i < s_commandCount; ++i)
		{
			// Ronin @feature 15/09/2026 DX9: `help <command>` shows just that one.
			if (argc >= 2 && stricmp(s_commands[i].name, argv[1].str()) != 0)
				continue;
			AsciiString line;
			line.format("%s - %s", s_commands[i].name, s_commands[i].help);
			printAscii(line, FALSE);
		}
	}

	void cmdClear(Int argc, const AsciiString *argv)
	{
		s_outCount = 0;
	}

	// Ronin @feature 14/09/2026 DX9: same as Ctrl+Shift+Z. update() hides the panel next frame and hands the keys back.
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

	// Ronin @feature 15/09/2026 DX9: water on/off. `water` prints the switches; `water <flat|mesh> <0|1>` sets one.
	void cmdWater(Int argc, const AsciiString *argv)
	{
		Bool ok = (argc == 1);
		if (argc == 3)
		{
			const Bool on = (atoi(argv[2].str()) != 0);
			if      (stricmp(argv[1].str(), "flat") == 0) { TheWaterDebug.skipFlat = !on; ok = TRUE; }
			else if (stricmp(argv[1].str(), "mesh") == 0) { TheWaterDebug.skipMesh = !on; ok = TRUE; }
		}
		if (!ok)
		{
			printAscii(AsciiString("usage: water [flat|mesh 0|1]"), TRUE);
			return;
		}
		AsciiString state;
		state.format("water: flat=%d mesh=%d", TheWaterDebug.skipFlat ? 0 : 1, TheWaterDebug.skipMesh ? 0 : 1);
		printAscii(state, FALSE);
	}

	// Ronin @bugfix 27/09/2026 DX9: a lone word after a command must be a number. atoi read `taa margin` as 0 and switched TAA
	// off; now it prints the usage instead.
	Bool isWholeNumber(const char *s)
	{
		if (s == nullptr || *s == 0)
			return FALSE;
		if (*s == '-')
			++s;
		if (*s == 0)
			return FALSE;
		for (; *s != 0; ++s)
			if (*s < '0' || *s > '9')
				return FALSE;
		return TRUE;
	}

	// Ronin @feature 28/09/2026 DX9: `rows` lists every row's switch; `rows <name>|all <0|1>` sets one or all. A row that is
	// on can still be empty: shadow needs shadow maps, taa needs `taa 1`.
	void cmdRows(Int argc, const AsciiString *argv)
	{
		Bool ok = (argc == 1);
		if (argc == 3 && isWholeNumber(argv[2].str()))
		{
			const Bool all = (stricmp(argv[1].str(), "all") == 0);
			for (Int r = 0; r < W3DDebugPanel::ROW_COUNT; ++r)
			{
				if (all || stricmp(argv[1].str(), ROW_NAMES[r]) == 0)
				{
					s_rowOn[r] = (atoi(argv[2].str()) != 0);
					ok = TRUE;
				}
			}
		}
		if (!ok)
		{
			printAscii(AsciiString("usage: rows [<name>|all 0|1] - names: sr inst draw shadow perf depth terrain ui rstate taa"), TRUE);
			return;
		}
		AsciiString state("rows:");
		for (Int r = 0; r < W3DDebugPanel::ROW_COUNT; ++r)
		{
			if (r == W3DDebugPanel::ROW_DRAW2)
				continue;		// same switch as draw
			AsciiString one;
			one.format("  %s=%d", ROW_NAMES[r], s_rowOn[r] ? 1 : 0);
			state.concat(one);
		}
		printAscii(state, FALSE);
	}

	// Ronin @feature 20/09/2026 DX9: TAA step 1 is the JITTER ONLY — no history, no resolve. With it on the image should
	// shimmer very slightly: that shimmer is the subpixel sampling a later step accumulates, and seeing it is how the
	// jitter is verified. It must NOT move placement or picking — those read the unjittered matrix. Hold Ctrl and check
	// the placement grid does not crawl. docs/AntiAliasing_Work.md.
	void cmdTaa(Int argc, const AsciiString *argv)
	{
		Bool ok = (argc == 1);
		if (argc == 2 && isWholeNumber(argv[1].str()))
		{
			W3DTaa::setEnabled(atoi(argv[1].str()) != 0);
			ok = TRUE;
		}
		else if (argc == 3)
		{
			if      (stricmp(argv[1].str(), "weight")   == 0) { W3DTaa::setWeight((float)atof(argv[2].str())); ok = TRUE; }
			else if (stricmp(argv[1].str(), "debug")    == 0) { W3DTaa::setDebug(atoi(argv[2].str())); ok = TRUE; }
			else if (stricmp(argv[1].str(), "sharpen")  == 0) { W3DTaa::setSharpen((float)atof(argv[2].str())); ok = TRUE; }
			else if (stricmp(argv[1].str(), "mipbias")  == 0) { W3DTaa::setMipBias((float)atof(argv[2].str())); ok = TRUE; }
			else if (stricmp(argv[1].str(), "clamp")    == 0) { W3DTaa::setClamp((float)atof(argv[2].str())); ok = TRUE; }
			else if (stricmp(argv[1].str(), "shadowmask")== 0) { W3DTaa::setShadowMask(atoi(argv[2].str()) != 0); ok = TRUE; }
			else if (stricmp(argv[1].str(), "maskcap")  == 0) { W3DTaa::setMaskCap(atoi(argv[2].str()) != 0); ok = TRUE; }
			else if (stricmp(argv[1].str(), "vel")      == 0) { W3DTaa::setVelocity(atoi(argv[2].str()) != 0); ok = TRUE; }
			else if (stricmp(argv[1].str(), "disocc")   == 0) { W3DTaa::setDisocc(atoi(argv[2].str())); ok = TRUE; }
			else if (stricmp(argv[1].str(), "disoccv")  == 0) { W3DTaa::setDisoccV((float)atof(argv[2].str())); ok = TRUE; }
			else if (stricmp(argv[1].str(), "reactive") == 0) { W3DTaa::setReactive((float)atof(argv[2].str())); ok = TRUE; }
			else if (stricmp(argv[1].str(), "autoreact")== 0) { W3DTaa::setAutoReact((float)atof(argv[2].str())); ok = TRUE; }
		}
		if (!ok)
		{
			printAscii(AsciiString("usage: taa [0|1] | weight <0..0.99> | debug <0..15> | sharpen <0..1> | mipbias <-2..0> | clamp <0..1> | shadowmask <0|1> | maskcap <0|1> | vel <0|1> | disocc <0..2> | disoccv <px> | reactive <0..1> | autoreact <0..1>"), TRUE);
			printAscii(AsciiString("  debug 1=reproj 2=depth 3=samples 4=history 13=velocity/reactive mask (orange = auto-reactive) 14=moving shadows (blue = in one now, red = clamped, yellow = also capped) 15=mesh motion px/frame (navy<.05 blue<.1 cyan<.2 green<.35 yellow<.5 orange<1 red)"), FALSE);
			return;
		}
		// Ronin @diagnostic 26/09/2026 DX9: labels are the knob names, so what is read here is what gets typed.
		// Ronin @bugfix 27/09/2026 DX9: three short lines, grouped; one line had grown wider than the screen.
		AsciiString state;
		state.format("taa: enabled=%d  running=%d  weight=%.2f  sharpen=%.2f  mipbias=%.2f  debug=%d",
			W3DTaa::isEnabled() ? 1 : 0, W3DTaa::isActive() ? 1 : 0, W3DTaa::getWeight(), W3DTaa::getSharpen(), W3DTaa::getMipBias(), W3DTaa::getDebug());
		printAscii(state, FALSE);
		state.format("  clamp=%.2f  shadowmask=%d  maskcap=%d  reactive=%.2f  autoreact=%.2f",
			W3DTaa::getClamp(), W3DTaa::getShadowMask() ? 1 : 0, W3DTaa::getMaskCap() ? 1 : 0,
			W3DTaa::getReactive(), W3DTaa::getAutoReact());
		printAscii(state, FALSE);
		state.format("  vel=%d  disocc=%d  disoccv=%.2f",
			W3DTaa::getVelocity() ? 1 : 0, W3DTaa::getDisocc(), W3DTaa::getDisoccV());
		printAscii(state, FALSE);
	}

	// Ronin @feature 26/09/2026 DX9: SSAO quality at runtime, 0 Off .. 3 Ultra - the Options.ini SSAOQuality value, not
	// saved back. W3DSsao reads it every frame and resizes its targets itself, so no restart.
	void cmdSsao(Int argc, const AsciiString *argv)
	{
		if (argc == 2 && TheWritableGlobalData != nullptr && isWholeNumber(argv[1].str()))
		{
			Int q = atoi(argv[1].str());
			if (q < 0) q = 0;
			if (q > 3) q = 3;
			TheWritableGlobalData->m_ssaoQuality = q;
		}
		// Ronin @feature 26/09/2026 DX9: `ssao trees auto|0|1` - AO on trees while TAA runs (auto), never, or always.
		else if (argc == 3 && stricmp(argv[1].str(), "trees") == 0)
			W3DSsao::setTrees(stricmp(argv[2].str(), "auto") == 0 ? W3DSsao::TREES_AUTO : ((atoi(argv[2].str()) != 0) ? W3DSsao::TREES_ON : W3DSsao::TREES_OFF));
		else if (argc != 1)
		{
			printAscii(AsciiString("usage: ssao [0..3]  (0 off, 1 normal, 2 high, 3 ultra) | ssao trees <auto|0|1>"), TRUE);
			return;
		}
		static const char *names[] = { "off", "normal", "high", "ultra" };
		const Int q = W3DSsao::getQuality();
		AsciiString state;
		// Ronin @bugfix 27/09/2026 DX9: no `active` here - it is decided at the next frame start, so it read stale right after
		// a change. The [DEPTH] row shows whether it actually ran.
		const Int tm = W3DSsao::getTrees();
		state.format("ssao: quality=%d (%s)  trees=%s%s", q, names[(q < 0) ? 0 : ((q > 3) ? 3 : q)],
			(tm == W3DSsao::TREES_AUTO) ? "auto" : ((tm == W3DSsao::TREES_ON) ? "1" : "0"),
			(tm == W3DSsao::TREES_AUTO) ? (W3DSsao::treesNow() ? " (on: TAA running)" : " (off: TAA not running)") : "");
		printAscii(state, FALSE);
	}

	// Ronin @feature 26/09/2026 DX9: shadow-map quality at runtime, 0 Off .. 3 Ultra - the Options.ini ShadowQuality value,
	// not saved back. applyQuality tears the receivers down and rebuilds the maps; at 0 the stencil volumes and decals,
	// which every object keeps, draw again (W3DShadow.cpp shadowMapsDriving).
	void cmdShadows(Int argc, const AsciiString *argv)
	{
		if (argc == 2 && TheWritableGlobalData != nullptr && isWholeNumber(argv[1].str()))
		{
			Int q = atoi(argv[1].str());
			if (q < 0) q = 0;
			if (q > 3) q = 3;
			TheWritableGlobalData->m_shadowMapQuality = q;
			W3DShadowMap::applyQuality();
		}
		else if (argc != 1)
		{
			printAscii(AsciiString("usage: shadows [0..3]  (0 off = stencil shadows, 1 normal, 2 high, 3 ultra)"), TRUE);
			return;
		}
		static const char *names[] = { "off", "normal", "high", "ultra" };
		const Int q = W3DShadowMap::getQuality();
		AsciiString state;
		state.format("shadows: quality=%d (%s)  maps=%d", q, names[(q < 0) ? 0 : ((q > 3) ? 3 : q)],
			TheUseShadowMaps ? 1 : 0);
		printAscii(state, FALSE);
	}

	// Ronin @diagnostic 16/09/2026 DX9: tune the placement grid while it is on screen (hold Ctrl). `grid` prints the knobs,
	// `grid <knob> <value>` sets one.
	void cmdGrid(Int argc, const AsciiString *argv)
	{
		Bool ok = (argc == 1);
		if (argc == 3)
		{
			const Real value = (Real)atof(argv[2].str());
			if      (stricmp(argv[1].str(), "lift")   == 0) { TheDebugGrid.lift            = value;      ok = TRUE; }
			else if (stricmp(argv[1].str(), "width")  == 0) { TheDebugGrid.halfWidthPixels = value;      ok = TRUE; }
			else if (stricmp(argv[1].str(), "alpha")  == 0) { TheDebugGrid.alpha           = value;      ok = TRUE; }
			else if (stricmp(argv[1].str(), "radius") == 0) { TheDebugGrid.radius          = (Int)value; ok = TRUE; }
		}
		if (!ok)
		{
			printAscii(AsciiString("usage: grid [lift|width|alpha|radius <value>] - radius 0 sizes it from the drop"), TRUE);
			return;
		}
		AsciiString state;
		state.format("grid: lift=%.2f width=%.2fpx alpha=%.2f radius=%d%s",
			TheDebugGrid.lift, TheDebugGrid.halfWidthPixels, TheDebugGrid.alpha, TheDebugGrid.radius,
			(TheDebugGrid.radius > 0) ? "" : " (from the drop)");
		printAscii(state, FALSE);

	}

	// Ronin @feature 16/09/2026 DX9: edit distance (insert/delete/replace) between two names, each capped at 64 characters.
	Int nameDistance(const char *a, const char *b)
	{
		enum { MAXLEN = 64 };
		Int la = (Int)strlen(a), lb = (Int)strlen(b);
		if (la > MAXLEN) la = MAXLEN;
		if (lb > MAXLEN) lb = MAXLEN;
		Int prev[MAXLEN + 1], cur[MAXLEN + 1];
		for (Int j = 0; j <= lb; ++j) prev[j] = j;
		for (Int i = 1; i <= la; ++i)
		{
			cur[0] = i;
			for (Int j = 1; j <= lb; ++j)
			{
				Int best = prev[j - 1] + ((a[i - 1] == b[j - 1]) ? 0 : 1);
				if (prev[j] + 1 < best)   best = prev[j] + 1;
				if (cur[j - 1] + 1 < best) best = cur[j - 1] + 1;
				cur[j] = best;
			}
			for (Int j = 0; j <= lb; ++j) prev[j] = cur[j];
		}
		return prev[lb];
	}


	// Ronin @feature 15/09/2026 DX9: game-changing commands are single player only (campaign, skirmish). isInMultiplayerGame()
	// is exactly LAN|INTERNET; replays must play back only what was recorded.
	// Ronin @feature 16/09/2026 DX9: the refusal text on its own, so the picker can show it as its only row.
	const char *logicRefusal()
	{
		if (TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isInShellGame())
			return "refused: only in a match";
		if (TheGameLogic->isInMultiplayerGame())
			return "refused: not in LAN/online matches";
		if (TheGameLogic->isInReplayGame())
			return "refused: not while watching a replay";
		return nullptr;
	}

	Bool logicCommandAllowed()
	{
		const char *refusal = logicRefusal();
		if (refusal != nullptr)
			printAscii(AsciiString(refusal), TRUE);
		return refusal == nullptr;
	}

	// Ronin @feature 28/09/2026 DX9: `credits` prints your money, `credits <amount>` adds it. A logic message like spawn, so
	// GameLogic applies it next logic frame and a skirmish replay reproduces it.
	void cmdCredits(Int argc, const AsciiString *argv)
	{
		Player *local = (ThePlayerList != nullptr) ? ThePlayerList->getLocalPlayer() : nullptr;
		const Bool give = (argc == 2 && isWholeNumber(argv[1].str()) && atoi(argv[1].str()) > 0);
		if (!give && argc != 1)
		{
			printAscii(AsciiString("usage: credits [amount] - a whole number above 0"), TRUE);
			return;
		}
		if (local == nullptr)
		{
			printAscii(AsciiString("credits: no local player"), TRUE);
			return;
		}
		AsciiString line;
		if (give)
		{
			if (!logicCommandAllowed())
				return;
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DEV_ADD_CASH);
			msg->appendIntegerArgument(atoi(argv[1].str()));
			line.format("credits: +%d (had %u)", atoi(argv[1].str()), local->getMoney()->countMoney());
		}
		else
			line.format("credits: %u", local->getMoney()->countMoney());
		printAscii(line, FALSE);
	}

	// Ronin @feature 28/09/2026 DX9: `power` prints the state, `power 0|1` switches unlimited power for your player. plants and
	// used are the real numbers - the power bar keeps showing them while it is on.
	void cmdPower(Int argc, const AsciiString *argv)
	{
		Player *local = (ThePlayerList != nullptr) ? ThePlayerList->getLocalPlayer() : nullptr;
		const Bool set = (argc == 2 && isWholeNumber(argv[1].str()));
		if (!set && argc != 1)
		{
			printAscii(AsciiString("usage: power [0|1] - 1 = never short of power, 0 = your real output again"), TRUE);
			return;
		}
		if (local == nullptr)
		{
			printAscii(AsciiString("power: no local player"), TRUE);
			return;
		}
		const Energy *energy = local->getEnergy();
		Bool unlimited = energy->isUnlimited();
		if (set)
		{
			if (!logicCommandAllowed())
				return;
			unlimited = (atoi(argv[1].str()) != 0);
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DEV_SET_POWER);
			msg->appendIntegerArgument(unlimited ? 1 : 0);
		}
		AsciiString line;
		line.format("power: unlimited=%d  plants=%d  used=%d", unlimited ? 1 : 0, energy->getProduction(), energy->getConsumption());
		printAscii(line, FALSE);
	}

	// Ronin @feature 16/09/2026 DX9: the command box, for a command that leaves text in it (spawn reopens its picker).
	GameWindow *findEntry()
	{
		GameWindow *panel = TheWindowManager->winGetWindowFromId(nullptr, PANEL_WINDOW_ID);
		return (panel != nullptr && panel->winGetChild() != nullptr)
			? TheWindowManager->winGetWindowFromId(panel->winGetChild(), ENTRY_WINDOW_ID) : nullptr;
	}

	// Ronin @feature 16/09/2026 DX9: placement mode (step 7c3). The object rides the cursor, a left click drops one where it
	// points and the cursor keeps it, right click or Esc stops. Like the dozer's build cursor, except nothing is built: every
	// click sends MSG_DEV_SPAWN_OBJECT, so GameLogic creates it and a skirmish replay records it.

	// Ronin @feature 16/09/2026 DX9: hold Ctrl to line things up — the spot snaps to SNAP_CELL cells and the facing
	// to quarter turns. An odd footprint sits on a cell centre, an even one on the corner where four cells meet, so buildings
	// snapped this way stand edge to edge instead of half a cell apart.
	Bool snapHeld()
	{
		return TheKeyboard != nullptr && TheKeyboard->isCtrl();
	}

	void snapToCells(const ThingTemplate *thing, Coord3D *world)
	{
		Real offset = 0.0f;
		if (thing != nullptr)
		{
			const Int cells = (Int)(thing->getTemplateGeometryInfo().getMajorRadius() * 2.0f / SNAP_CELL + 0.5f);
			if ((cells % 2) != 0)
				offset = SNAP_CELL * 0.5f;
		}
		world->x = (Real)REAL_TO_INT_FLOOR((world->x - offset) / SNAP_CELL + 0.5f) * SNAP_CELL + offset;
		world->y = (Real)REAL_TO_INT_FLOOR((world->y - offset) / SNAP_CELL + 0.5f) * SNAP_CELL + offset;
	}

	Real snapToQuarterTurn(Real angle)
	{
		return (Real)REAL_TO_INT_FLOOR(angle / SNAP_ANGLE + 0.5f) * SNAP_ANGLE;
	}

	void endPlacement()
	{
		for (Int i = 0; i < s_ghostCount; ++i)
		{
			Drawable *ghost = (TheGameClient != nullptr) ? TheGameClient->findDrawableByID(s_ghostIDs[i]) : nullptr;
			if (ghost != nullptr)
				TheGameClient->destroyDrawable(ghost);
			s_ghostIDs[i] = INVALID_DRAWABLE_ID;
		}
		s_ghostCount    = 0;
		s_placeID       = -1;
		s_placeAnchored = FALSE;
		W3DDebugGrid::hide();
	}

	// Ronin @feature 16/09/2026 DX9: where copy `index` of `count` lands. MUST match the grid in GameLogic::onDevSpawnObject
	// (GameLogicDispatch.cpp) — the preview is a promise about where they will be.
	void placementSpot(const ThingTemplate *thing, Int index, Int count, const Coord3D &centre, Coord3D *out)
	{
		Int side = 1;
		while (side * side < count)
			++side;
		const Real spacing = thing->getTemplateGeometryInfo().getBoundingCircleRadius() * 2.0f + 5.0f;
		out->x = centre.x + ((Real)(index % side) - (Real)(side - 1) * 0.5f) * spacing;
		out->y = centre.y + ((Real)(index / side) - (Real)(side - 1) * 0.5f) * spacing;
		out->z = (TheTerrainLogic != nullptr) ? TheTerrainLogic->getGroundHeight(out->x, out->y) : centre.z;
	}

	// Ronin @feature 16/09/2026 DX9: how far the block of copies reaches from its centre, in world units. The grid highlights
	// the cells inside this, and sizes its patch to reach past it — a block of 20 covers far more ground than a block of 5.
	Real placementHalfExtent(const ThingTemplate *thing, Int count)
	{
		Int side = 1;
		while (side * side < count)
			++side;
		const Real spacing = thing->getTemplateGeometryInfo().getBoundingCircleRadius() * 2.0f + 5.0f;
		return ((Real)(side - 1) * 0.5f + 0.5f) * spacing;
	}

	Int gridRadiusFor(const ThingTemplate *thing, Int count)
	{
		const Int cells = (Int)(placementHalfExtent(thing, count) / SNAP_CELL) + 2;
		return (cells < 4) ? 4 : cells;
	}


	void startPlacement(const ThingTemplate *thing, Int count)
	{
		endPlacement();
		if (thing == nullptr)
			return;
		const Int limit = (thing->getEditorSorting() == ES_STRUCTURE) ? PLACE_MAX_STRUCTURES : PLACE_MAX_UNITS;
		if (count < 1)
			count = 1;
		if (count > limit)
		{
			AsciiString capped;
			capped.format("%d is more than this drops at once - using %d", count, limit);
			printAscii(capped, FALSE);
			count = limit;
		}
		s_placeID      = (Int)thing->getTemplateID();
		s_placeCount   = count;
		s_placeAngle   = thing->getPlacementViewAngle();
		s_placeAnchored = FALSE;

		AsciiString line;
		line.format("placing %d x %s - left click drops one (hold and drag to turn it, Ctrl to snap), right click or Esc stops",
			count, thing->getName().str());
		printAscii(line, FALSE);
	}

	// One drop at the pinned spot, facing s_placeAngle; the cursor keeps the object for the next click.
	Bool placeAtWorld(const Coord3D &world)
	{
		if (s_placeID < 0 || TheMessageStream == nullptr || ThePlayerList == nullptr)
			return FALSE;
		if (!logicCommandAllowed())
		{
			endPlacement();
			return FALSE;
		}
		Player *local = ThePlayerList->getLocalPlayer();
		if (local == nullptr)
			return FALSE;

		// Snapped here as well as in the preview, so a click and release inside one frame still lands on the cell.
		Coord3D at = world;
		if (snapHeld())
		{
			snapToCells((TheThingFactory != nullptr) ? TheThingFactory->findByTemplateID((UnsignedShort)s_placeID) : nullptr, &at);
			s_placeAngle = snapToQuarterTurn(s_placeAngle);
		}

		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DEV_SPAWN_OBJECT);
		msg->appendIntegerArgument(s_placeID);
		msg->appendLocationArgument(at);
		msg->appendIntegerArgument(s_placeCount);
		msg->appendIntegerArgument((Int)local->getPlayerIndex());
		msg->appendRealArgument(s_placeAngle);
		return TRUE;
	}

	// Ronin @feature 16/09/2026 DX9: the things on the cursor are drawables with no object, like the dozer's build icon, so the
	// selection code and the game logic never see them. Looked up by id every frame: the end of a match destroys every drawable.
	void updatePlacement()
	{
		if (s_placeID < 0)
			return;
		if (!s_visible || logicRefusal() != nullptr || TheThingFactory == nullptr || TheGameClient == nullptr)
		{
			endPlacement();
			return;
		}
		const ThingTemplate *thing = TheThingFactory->findByTemplateID((UnsignedShort)s_placeID);
		if (thing == nullptr)
		{
			endPlacement();
			return;
		}
		if (TheMouse == nullptr || TheTacticalView == nullptr)
			return;

		// Where the block of copies is centred: the pinned spot while the button is held, the cursor otherwise.
		ICoord2D pixel = TheMouse->getMouseStatus()->pos;
		Coord3D centre;
		if (s_placeAnchored)
		{
			// Held: the copies stay on the spot that was pressed and the drag away from it picks the facing.
			const Int dx = pixel.x - s_placePress.x;
			const Int dy = pixel.y - s_placePress.y;
			Coord3D world;
			if (dx * dx + dy * dy >= PLACE_DRAG_PIXELS * PLACE_DRAG_PIXELS &&
				TheTacticalView->screenToTerrain(&pixel, &world))
			{
				Coord2D facing;
				facing.x = world.x - s_placeAnchor.x;
				facing.y = world.y - s_placeAnchor.y;
				if (facing.x != 0.0f || facing.y != 0.0f)
					s_placeAngle = facing.toAngle();
			}
			// Snapping the pinned spot again every frame costs nothing and catches Ctrl pressed after the button went down.
			if (snapHeld())
				snapToCells(thing, &s_placeAnchor);
			centre = s_placeAnchor;
		}
		else
		{
			if (!TheTacticalView->screenToTerrain(&pixel, &centre))
				return;
			if (snapHeld())
				snapToCells(thing, &centre);
		}
		if (snapHeld())
			s_placeAngle = snapToQuarterTurn(s_placeAngle);

		// One ghost per copy, on the same grid the drop will use. Their spots are kept so the grid can mark the cells they
		// stand on — the same numbers, so the green cannot disagree with the ghosts.
		Coord3D spots[PLACE_MAX_COUNT];
		s_ghostCount = (s_placeCount < PLACE_MAX_COUNT) ? s_placeCount : PLACE_MAX_COUNT;
		for (Int i = 0; i < s_ghostCount; ++i)
		{
			Drawable *ghost = TheGameClient->findDrawableByID(s_ghostIDs[i]);
			if (ghost == nullptr)
			{
				// Ronin @bugfix 18/09/2026 DX9: CURSOR_PREVIEW, or the shadow code bakes these as static scenery — they have
				// no object — and their shadows trail behind the cursor.
				ghost = TheThingFactory->newDrawable(thing,
					(DrawableStatusBits)(DRAWABLE_STATUS_NO_STATE_PARTICLES | DRAWABLE_STATUS_CURSOR_PREVIEW));
				if (ghost == nullptr)
					break;
				ghost->setDrawableOpacity((TheGlobalData != nullptr) ? TheGlobalData->m_objectPlacementOpacity : 0.5f);
				s_ghostIDs[i] = ghost->getID();
			}
			placementSpot(thing, i, s_ghostCount, centre, &spots[i]);
			ghost->setPosition(&spots[i]);
			ghost->setOrientation(s_placeAngle);
		}

		// Ronin @feature 16/09/2026 DX9: the cells only mean anything while Ctrl snaps to them, so that is exactly when the
		// grid is drawn — lines you cannot land on would be lying.
		if (snapHeld())
		{
			W3DDebugGrid::show(centre, SNAP_CELL, gridRadiusFor(thing, s_ghostCount), spots, s_ghostCount,
				thing->getTemplateGeometryInfo().getBoundingCircleRadius());
		}
		else
		{
			W3DDebugGrid::hide();
		}

	}

	// Ronin @feature 16/09/2026 DX9: while placing, the mouse is ours — taken before the selection (50) and command (70)
	// translators, the way PlaceEventTranslator takes it for the dozer. Does nothing while nothing is on the cursor.
	class DevPlaceTranslator : public GameMessageTranslator
	{
	public:
		virtual GameMessageDisposition translateGameMessage(const GameMessage *msg) override
		{
			const GameMessage::Type type = msg->getType();

			if (type == GameMessage::MSG_RAW_KEY_UP && s_eatEscUp && msg->getArgument(0)->integer == KEY_ESC)
			{
				s_eatEscUp = FALSE;
				return DESTROY_MESSAGE;
			}
			if (s_placeID < 0)
				return KEEP_MESSAGE;

			switch (type)
			{
				case GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN:
				case GameMessage::MSG_RAW_MOUSE_LEFT_DOUBLE_CLICK:
				{
					// The press pins the spot; until the button comes up the drag only turns the object.
					ICoord2D pixel = msg->getArgument(0)->pixel;
					Coord3D world;
					if (TheTacticalView != nullptr && TheTacticalView->screenToTerrain(&pixel, &world))
					{
						s_placePress    = pixel;
						s_placeAnchor   = world;
						s_placeAnchored = TRUE;
					}
					return DESTROY_MESSAGE;
				}

				case GameMessage::MSG_RAW_MOUSE_LEFT_DRAG:
					return DESTROY_MESSAGE;

				case GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP:
					if (s_placeAnchored)
					{
						placeAtWorld(s_placeAnchor);
						s_placeAnchored = FALSE;
					}
					return DESTROY_MESSAGE;

				case GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN:
					s_placePress = msg->getArgument(0)->pixel;
					return KEEP_MESSAGE;				// a right drag still scrolls the camera

				case GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP:
				{
					const ICoord2D pixel = msg->getArgument(0)->pixel;
					if (abs(pixel.x - s_placePress.x) > 2 || abs(pixel.y - s_placePress.y) > 2)
						return KEEP_MESSAGE;			// that was a scroll, not a click
					endPlacement();
					printAscii(AsciiString("placement off"), FALSE);
					return DESTROY_MESSAGE;				// eaten, so nothing selected gets a move order
				}

				case GameMessage::MSG_RAW_KEY_DOWN:
					if (msg->getArgument(0)->integer == KEY_ESC)
					{
						endPlacement();
						printAscii(AsciiString("placement off"), FALSE);
						s_eatEscUp = TRUE;
						return DESTROY_MESSAGE;
					}
					break;

				default:
					break;
			}
			return KEEP_MESSAGE;
		}
	};

	// Ronin @feature 15/09/2026 DX9: `spawn <ThingTemplate> [count]` spawns directly.
	// Ronin @feature 16/09/2026 DX9: `spawn` alone, or a name that is no template, leaves the text in the box: the picker opens.
	void cmdSpawn(Int argc, const AsciiString *argv)
	{
		if (!logicCommandAllowed() || TheThingFactory == nullptr)
			return;

		GameWindow *entry = findEntry();
		if (argc < 2)
		{
			if (entry != nullptr)
				GadgetTextEntrySetText(entry, UnicodeString(L"spawn "));
			printAscii(AsciiString("type to filter, click a row to put it on the cursor; click [Side]/[Category] to step, right click back"), FALSE);
			return;
		}

		// Exact name, then the same name ignoring case.
		const ThingTemplate *thing = TheThingFactory->findTemplate(argv[1], FALSE);
		if (thing == nullptr)
		{
			for (const ThingTemplate *t = TheThingFactory->firstTemplate(); t != nullptr; t = t->friend_getNextTemplate())
			{
				if (stricmp(t->getName().str(), argv[1].str()) == 0)
				{
					thing = t;
					break;
				}
			}
		}
		if (thing == nullptr)
		{
			AsciiString err;
			err.format("no object named '%s'", argv[1].str());
			printAscii(err, TRUE);
			// Ronin @feature 16/09/2026 DX9: suggest the closest names — ones containing the text first, then by edit distance.
			AsciiString needle = argv[1];
			needle.toLower();
			enum { MAX_SUGGEST = 4 };
			const ThingTemplate *best[MAX_SUGGEST] = {};
			Int bestScore[MAX_SUGGEST] = { 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF };
			for (const ThingTemplate *t = TheThingFactory->firstTemplate(); t != nullptr; t = t->friend_getNextTemplate())
			{
				AsciiString lower = t->getName();
				lower.toLower();
				const Int score = (strstr(lower.str(), needle.str()) != nullptr) ? 0 : nameDistance(needle.str(), lower.str());
				for (Int s = 0; s < MAX_SUGGEST; ++s)
				{
					if (score < bestScore[s])
					{
						for (Int k = MAX_SUGGEST - 1; k > s; --k)
						{
							best[k] = best[k - 1];
							bestScore[k] = bestScore[k - 1];
						}
						best[s] = t;
						bestScore[s] = score;
						break;
					}
				}
			}
			const Int limit = needle.getLength() / 3 + 2;	// further than this the "closest" name is noise
			for (Int s = 0; s < MAX_SUGGEST; ++s)
			{
				if (best[s] != nullptr && bestScore[s] <= limit)
				{
					AsciiString line;
					line.format("  did you mean %s", best[s]->getName().str());
					printAscii(line, FALSE);
				}
			}

			// Ronin @feature 16/09/2026 DX9: back into the box as typed, so the picker shows what the words match.
			if (entry != nullptr)
			{
				AsciiString line = argv[0];
				for (Int i = 1; i < argc; ++i)
				{
					line.concat(' ');
					line.concat(argv[i]);
				}
				UnicodeString u;
				u.translate(line);
				GadgetTextEntrySetText(entry, u);
			}
			return;
		}

		startPlacement(thing, (argc >= 3) ? atoi(argv[2].str()) : 1);
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

	// Ronin @feature 16/09/2026 DX9: spawn picker (docs/Debug_Panel_Design.md step 7c2). Index into PICK_CATEGORIES, -1 for
	// objects it does not list (system, audio, test, debris, roads, waypoints).
	Int categoryOf(const ThingTemplate *t)
	{
		const EditorSortingType sorting = t->getEditorSorting();
		for (Int c = 0; c < PICK_CATEGORY_COUNT; ++c)
		{
			if (PICK_CATEGORIES[c].sorting == sorting)
				return c;
		}
		return -1;
	}

	// Ronin @feature 16/09/2026 DX9: the owning sides of the listed objects, sorted, collected once.
	void collectSides()
	{
		if (s_sideCount >= 0 || TheThingFactory == nullptr)
			return;
		s_sideCount = 0;
		for (const ThingTemplate *t = TheThingFactory->firstTemplate(); t != nullptr; t = t->friend_getNextTemplate())
		{
			const AsciiString &side = t->getDefaultOwningSide();
			if (categoryOf(t) < 0 || side.isEmpty())
				continue;
			Int at = 0;
			while (at < s_sideCount && stricmp(s_sides[at].str(), side.str()) < 0)
				++at;
			if (s_sideCount == MAX_SIDES || (at < s_sideCount && stricmp(s_sides[at].str(), side.str()) == 0))
				continue;
			for (Int k = s_sideCount; k > at; --k)
				s_sides[k] = s_sides[k - 1];
			s_sides[at] = side;
			++s_sideCount;
		}
	}

	void setLabel(DisplayString **label, const AsciiString &text)
	{
		if (*label == nullptr && TheDisplayStringManager != nullptr)
		{
			*label = TheDisplayStringManager->newDisplayString();
			if (*label != nullptr)
				(*label)->setFont(panelFont());
		}
		if (*label != nullptr)
		{
			UnicodeString u;
			u.translate(text);
			(*label)->setText(u);
		}
	}

	// Ronin @feature 16/09/2026 DX9: case-insensitive substring test; the needle is already lower case.
	Bool containsNoCase(const char *hay, const char *needle)
	{
		if (needle[0] == 0)
			return TRUE;
		for (; *hay != 0; ++hay)
		{
			Int i = 0;
			while (needle[i] != 0 && tolower((unsigned char)hay[i]) == needle[i])
				++i;
			if (needle[i] == 0)
				return TRUE;
		}
		return FALSE;
	}

	int compareTemplateNames(const void *a, const void *b)
	{
		const ThingTemplate *ta = *(const ThingTemplate *const *)a;
		const ThingTemplate *tb = *(const ThingTemplate *const *)b;
		return stricmp(ta->getName().str(), tb->getName().str());
	}

	// Ronin @feature 16/09/2026 DX9: box text "spawn <words>" -> TRUE and the words after "spawn ".
	Bool pickerFilter(const UnicodeString &text, AsciiString *filter)
	{
		AsciiString ascii;
		ascii.translate(text);
		const char *s = ascii.str();
		while (*s == ' ' || *s == '\t')
			++s;
		if (strnicmp(s, "spawn", 5) != 0 || (s[5] != ' ' && s[5] != '\t'))
			return FALSE;
		*filter = AsciiString(s + 6);
		return TRUE;
	}

	// Ronin @feature 16/09/2026 DX9: every word must appear in the name, side or category; a number is the count. Sorted by
	// name; past PICK_ROWS a last row says how many more. Each row keeps its template id + 1 as item data (0 = not a pick).
	void fillPicker(GameWindow *list, const AsciiString &filter)
	{
		GadgetListBoxReset(list);
		s_pickCount = 1;
		collectSides();

		AsciiString label;
		label.format("[Side: %s]", (s_sideIndex >= 0 && s_sideIndex < s_sideCount) ? s_sides[s_sideIndex].str() : "All");
		setLabel(&s_sideLabel, label);
		label.format("[Category: %s]", (s_categoryIndex >= 0) ? PICK_CATEGORIES[s_categoryIndex].name : "All");
		setLabel(&s_categoryLabel, label);

		const char *refusal = logicRefusal();
		if (refusal != nullptr || TheThingFactory == nullptr)
		{
			UnicodeString u;
			u.translate(AsciiString((refusal != nullptr) ? refusal : "no objects loaded"));
			GadgetListBoxAddEntryText(list, u, GameMakeColor(255, 120, 80, 255), -1, 0);
			return;
		}

		AsciiString words[MAX_ARGS];
		Int wordCount = 0;
		AsciiString rest = filter;
		AsciiString token;
		while (wordCount < MAX_ARGS && rest.nextToken(&token, " \t"))
		{
			if (token.str()[0] >= '0' && token.str()[0] <= '9')
			{
				s_pickCount = atoi(token.str());
				continue;
			}
			token.toLower();
			words[wordCount++] = token;
		}

		enum { MAX_MATCHES = 8192 };
		static const ThingTemplate *matches[MAX_MATCHES];
		Int matchCount = 0;
		for (const ThingTemplate *t = TheThingFactory->firstTemplate(); t != nullptr; t = t->friend_getNextTemplate())
		{
			const Int category = categoryOf(t);
			if (category < 0 || (s_categoryIndex >= 0 && category != s_categoryIndex))
				continue;
			const char *side = t->getDefaultOwningSide().str();
			if (s_sideIndex >= 0 && s_sideIndex < s_sideCount && stricmp(side, s_sides[s_sideIndex].str()) != 0)
				continue;
			Bool match = TRUE;
			for (Int w = 0; w < wordCount && match; ++w)
			{
				match = containsNoCase(t->getName().str(), words[w].str()) || containsNoCase(side, words[w].str()) ||
						containsNoCase(PICK_CATEGORIES[category].name, words[w].str());
			}
			if (match && matchCount < MAX_MATCHES)
				matches[matchCount++] = t;
		}
		qsort(matches, matchCount, sizeof(matches[0]), compareTemplateNames);

		const Int shown = (matchCount < PICK_ROWS) ? matchCount : PICK_ROWS;
		for (Int i = 0; i < shown; ++i)
		{
			const PickCategory &category = PICK_CATEGORIES[categoryOf(matches[i])];
			AsciiString line;
			line.format("%-38s %-10s %s", matches[i]->getName().str(), category.name, matches[i]->getDefaultOwningSide().str());
			UnicodeString u;
			u.translate(line);
			const Int row = GadgetListBoxAddEntryText(list, u, GameMakeColor(category.r, category.g, category.b, 255), -1, 0);
			if (row >= 0)
				GadgetListBoxSetItemData(list, (void *)(intptr_t)(matches[i]->getTemplateID() + 1), row, 0);
		}
		if (matchCount == 0 || matchCount > shown)
		{
			AsciiString line;
			if (matchCount == 0)
				line = "no match";
			else
				line.format("... %d more - keep typing to narrow it down", matchCount - shown);
			UnicodeString u;
			u.translate(line);
			GadgetListBoxAddEntryText(list, u, GameMakeColor(150, 150, 150, 255), -1, 0);
		}
	}

	// Ronin @feature 16/09/2026 DX9: the row under a screen point, measured where W3DGadgetListBoxDraw draws rows (4 px down).
	// The stock click test skips those 4 px, so the hover bar and the click both use this.
	Int listRowAt(GameWindow *list, Int screenX, Int screenY)
	{
		ListboxData *data = (ListboxData *)list->winGetUserData();
		if (data == nullptr || list->winIsHidden())
			return -1;
		Int x = 0, y = 0, w = 0, h = 0;
		list->winGetScreenPosition(&x, &y);
		list->winGetSize(&w, &h);
		if (screenX <= x || screenX >= x + w - 1 || screenY <= y || screenY >= y + h - 1)
			return -1;
		const Int local = screenY - (y + 4) + data->displayPos;
		if (local < 0)
			return -1;
		for (Int i = 0; i < data->endPos; ++i)
		{
			if (data->listData[i].listHeight > local)
				return i;
		}
		return -1;
	}

	// Ronin @feature 16/09/2026 DX9: the stock list input takes keyboard focus and picks with the offset test, so only the
	// wheel goes to it. A click that started on the list picks the row; update() spawns it next frame.
	WindowMsgHandledType listInput(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
	{
		switch (msg)
		{
			case GWM_WHEEL_UP:
			case GWM_WHEEL_DOWN:
				return GadgetListBoxInput(window, msg, mData1, mData2);

			case GWM_LEFT_DOWN:
				s_listPressed = TRUE;
				return MSG_HANDLED;

			case GWM_LEFT_UP:
			{
				if (s_listPressed)
				{
					const Int row = listRowAt(window, (Int)(mData1 & 0xFFFF), (Int)((mData1 >> 16) & 0xFFFF));
					const void *data = (row >= 0) ? GadgetListBoxGetItemData(window, row, 0) : nullptr;
					if (data != nullptr)
						s_pickedID = (Int)(intptr_t)data - 1;
				}
				s_listPressed = FALSE;
				// Ignored, so a drag-select from the battlefield released here still reaches panelInput, which ends it.
				return MSG_IGNORED;
			}

			case GWM_CHAR:
			case GWM_MOUSE_POS:
				return MSG_IGNORED;
		}
		return MSG_HANDLED;
	}

	// Ronin @feature 16/09/2026 DX9: the stock colour draw, plus a bar on the row under the mouse (its selection bar is image-only).
	void listDraw(GameWindow *window, WinInstanceData *instData)
	{
		GameWinDrawFunc draw = TheWindowManager->getListBoxDrawFunc();
		if (draw != nullptr)
			draw(window, instData);
		if (TheMouse == nullptr)
			return;
		const MouseIO *mouse = TheMouse->getMouseStatus();
		const Int row = listRowAt(window, mouse->pos.x, mouse->pos.y);
		if (row < 0 || GadgetListBoxGetItemData(window, row, 0) == nullptr)
			return;
		ListboxData *data = (ListboxData *)window->winGetUserData();
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		Int top    = y + 4 - data->displayPos + ((row > 0) ? data->listData[row - 1].listHeight : 0);
		Int bottom = top + data->listData[row].height + 1;
		if (top < y + 1)
			top = y + 1;
		if (bottom > y + h - 1)
			bottom = y + h - 1;
		TheWindowManager->winFillRect(GameMakeColor(255, 255, 255, 45), 1.0f, x + 1, top, x + w - 1, bottom);
	}

	GameWindow *createList(GameWindow *panel)
	{
		WinInstanceData inst;
		inst.init();
		inst.m_style = GWS_SCROLL_LISTBOX;

		ListboxData data;
		memset(&data, 0, sizeof(data));
		data.listLength = PICK_ROWS + 1;		// + the "N more" row
		data.columns    = 1;
		data.scrollBar  = FALSE;				// its arrows and thumb are image-only; the wheel scrolls

		GameWindow *list = TheWindowManager->gogoGadgetListBox(panel, WIN_STATUS_ENABLED | WIN_STATUS_ONE_LINE,
			PAD, 0, PICKER_WIDTH, PICKER_HEIGHT, &inst, &data, panelFont(), FALSE);
		if (list == nullptr)
			return nullptr;

		list->winSetWindowId(LIST_WINDOW_ID);
		list->winSetInputFunc(listInput);
		list->winSetDrawFunc(listDraw);
		list->winHide(TRUE);

		const Color back   = GameMakeColor(0, 0, 0, 110);
		const Color border = GameMakeColor(160, 160, 160, 170);
		const Color none   = WIN_COLOR_UNDEFINED;
		GadgetListBoxSetColors(list, back, border, none, none, back, border, none, none, back, border, none, none);
		return list;
	}

	// Ronin @feature 16/09/2026 DX9: -1 is All; stepping wraps through it.
	Int wrapIndex(Int index, Int step, Int count)
	{
		index += step;
		if (index >= count)
			index = -1;
		if (index < -1)
			index = count - 1;
		return index;
	}

	// Ronin @feature 16/09/2026 DX9: a click on the Side or Category label steps it (left next, right back); update() refills.
	void cycleLabelAt(GameWindow *panel, Int screenX, Int screenY, Int step)
	{
		Int px = 0, py = 0;
		panel->winGetScreenPosition(&px, &py);
		const Int lx = screenX - px;
		const Int ly = screenY - py;
		if (!s_pickerOpen || ly < s_labelTop || ly >= s_labelTop + LABEL_HEIGHT)
			return;
		if (lx >= PAD && lx < PAD + s_sideLabelW)
			s_sideIndex = wrapIndex(s_sideIndex, step, (s_sideCount > 0) ? s_sideCount : 0);
		else if (lx >= s_categoryLabelX && lx < s_categoryLabelX + s_categoryLabelW)
			s_categoryIndex = wrapIndex(s_categoryIndex, step, PICK_CATEGORY_COUNT);
		else
			return;
		s_pickerDirty = TRUE;
	}

	// Ronin @feature 14/09/2026 DX9: a wall for mouse input, so the panel drags and the battlefield under it is not clicked.
	// Same as GameWinBlockInput, which touches the selection translator, tactical view and InGameUI without null checks.
	WindowMsgHandledType panelInput(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
	{
		if (msg == GWM_CHAR || msg == GWM_MOUSE_POS)
			return MSG_IGNORED;

		// Ronin @feature 16/09/2026 DX9: picker labels. A press that moved is a drag of the panel, not a click.
		const Int mouseX = (Int)(mData1 & 0xFFFF);
		const Int mouseY = (Int)((mData1 >> 16) & 0xFFFF);
		if (msg == GWM_LEFT_DOWN)
		{
			s_pressX = mouseX;
			s_pressY = mouseY;
		}
		else if (msg == GWM_LEFT_UP && abs(mouseX - s_pressX) <= 2 && abs(mouseY - s_pressY) <= 2)
		{
			cycleLabelAt(window, mouseX, mouseY, 1);
		}
		else if (msg == GWM_RIGHT_UP)
		{
			cycleLabelAt(window, mouseX, mouseY, -1);
		}

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

	// Ronin @feature 28/09/2026 DX9: every frame, before update() can return early - see s_drawPrev.
	void sampleReadouts()
	{
		s_drawTotal = 0;
		for (Int i = 0; i < Debug_Statistics::DRAW_SUBSYS_COUNT; ++i)
		{
			const unsigned now = Debug_Statistics::Get_Total_Draw_Calls_By_Subsystem(i);
			s_drawNow[i]  = now - s_drawPrev[i];	// unsigned wrap is fine: monotonic counters
			s_drawPrev[i] = now;
			s_drawTotal  += s_drawNow[i];
		}

		// Ronin @diagnostic 09/09/2026 DX9: §29i.3 step 2. [PERF] run mean: single frames drift with scene activity all
		// session, so a cascade A/B needs means over a tour, not one screenshot.
		if (s_perfFreq == 0)
			QueryPerformanceFrequency((LARGE_INTEGER *)&s_perfFreq);
		QueryPerformanceCounter((LARGE_INTEGER *)&s_perfNow);
		if (TheGameLogic != nullptr && TheGameLogic->getFrame() > 60)
		{
			if (s_perfFirst == 0)
			{
				s_perfFirst = s_perfNow;		// the first tracked frame has nothing to time against
			}
			else
			{
				const Real ms = (Real)((double)(s_perfNow - s_perfLast) * 1000.0 / (double)s_perfFreq);
				if (ms > s_perfWorstMs) s_perfWorstMs = ms;
				s_perfSMSum += (double)s_drawNow[Debug_Statistics::DRAW_SUBSYS_SHADOWMAP];
				++s_perfFrames;
			}
			s_perfLast = s_perfNow;
		}
	}

	// Ronin @feature 28/09/2026 DX9: sets the row's text and hands it to the panel for this frame.
	void putRow(W3DDebugPanel::Row row, const UnicodeString &text, Color color)
	{
		if (s_rowText[row] == nullptr && TheDisplayStringManager != nullptr && TheFontLibrary != nullptr)
		{
			s_rowText[row] = TheDisplayStringManager->newDisplayString();
			if (s_rowText[row] != nullptr)
				s_rowText[row]->setFont(panelFont());
		}
		if (s_rowText[row] == nullptr)
			return;
		s_rowText[row]->setText(text);
		W3DDebugPanel::setRow(row, s_rowText[row], color);
	}

	// Ronin @feature 28/09/2026 DX9: every row, each behind its `rows` switch. The first seven lived in W3DDisplay.cpp.
	void renderReadouts()
	{
		UnicodeString text;

		// Ronin @diagnostic 21/06/2026 DX9: single-rigid records per frame; draws/frame equals [INST] recs.
		if (s_rowOn[W3DDebugPanel::ROW_SR])
		{
			const unsigned draws   = DX8_Get_Single_Rigid_Last_Frame_Draw_Count();
			const unsigned flushes = DX8_Get_Single_Rigid_Last_Frame_Flush_Count();
			text.format(L"[SR] draws/frame=%u  flushes=%u  avgBatch=%.1f",
				draws, flushes, (flushes > 0) ? ((float)draws / (float)flushes) : 0.0f);
			putRow(W3DDebugPanel::ROW_SR, text, GameMakeColor(255, 255, 0, 255));
		}

		if (s_rowOn[W3DDebugPanel::ROW_INST])
		{
			const unsigned runs       = TheDX8InstanceManager.Get_Last_Frame_Instanced_Draw_Calls();
			const unsigned instMeshes = TheDX8InstanceManager.Get_Last_Frame_Instanced_Meshes();
			text.format(L"[INST] recs=%u  runs=%u  inst=%u  indiv=%u  nrm=%u/%u  lbrk=%u  refl=%u  flushes=%u  avgRun=%.1f",
				TheDX8InstanceManager.Get_Last_Frame_Instanced_Records(), runs, instMeshes,
				TheDX8InstanceManager.Get_Last_Frame_Instanced_Individual_Draws(),
				TheDX8InstanceManager.Get_Last_Frame_Instanced_Normal_Mapped_Merged(),
				TheDX8InstanceManager.Get_Last_Frame_Instanced_Normal_Mapped(),
				TheDX8InstanceManager.Get_Last_Frame_Instanced_Light_Breaks(),
				TheDX8InstanceManager.Get_Last_Frame_Reflective_Draws(),
				TheDX8InstanceManager.Get_Last_Frame_Instanced_Flushes(),
				(runs > 0) ? ((float)instMeshes / (float)runs) : 0.0f);
			putRow(W3DDebugPanel::ROW_INST, text, GameMakeColor(0, 255, 255, 255));
		}

		// Ronin @diagnostic 02/08/2026 DX9: where the frame's draw calls come from; total includes the programmable rigid path.
		// sortAdd = draws batchable outside the depth sort (Windowednew.md §19b.1); [DRAW2] splits `other` (§19b.2).
		if (s_rowOn[W3DDebugPanel::ROW_DRAW])
		{
			text.format(L"[DRAW] total=%u  terr=%u  shroud=%u  sortAdd=%u  sortAlpha=%u  sortOth=%u  shadow=%u  rigid=%u  water=%u  skin=%u",
				s_drawTotal,
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_TERRAIN],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_SHROUD],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_SORTED_ADD],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_SORTED_ALPHA],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_SORTED],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_SHADOW],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_RIGID_PROG],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_WATER],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_SKIN]);
			putRow(W3DDebugPanel::ROW_DRAW, text, GameMakeColor(255, 160, 0, 255));
		}
		if (s_rowOn[W3DDebugPanel::ROW_DRAW2])
		{
			text.format(L"[DRAW2] rigidFFP=%u  matpass=%u  fxLine=%u  fxPoint=%u  ui2D=%u  other=%u  shadowMap=%u  shadowRecv=%u",
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_RIGID_FFP],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_MATPASS],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_FX_LINE],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_FX_POINT],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_UI2D],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_OTHER],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_SHADOWMAP],
				s_drawNow[Debug_Statistics::DRAW_SUBSYS_SHADOWRECV]);
			putRow(W3DDebugPanel::ROW_DRAW2, text, GameMakeColor(255, 160, 0, 255));
		}

		// Ronin @diagnostic 06/09/2026 DX9: §29j.13k. Shadow fit - texel moves with zoom, yaw and sun. No maps, no row.
		if (s_rowOn[W3DDebugPanel::ROW_SHADOW] && TheUseShadowMaps)
		{
			// Ronin @diagnostic 09/09/2026 DX9: §29i.3 step 2. PEAK = the worst fit seen while moving; the near split's win is
			// one quantiser step wide. Starts over on a tier change (`shadows <n>`) or a new match.
			static Real        s_reqPeak     = 0.0f;
			static Real        s_extPeak     = 0.0f;
			static Int         s_peakQuality = -1;
			static UnsignedInt s_peakFrame   = 0;
			const UnsignedInt  logicFrame    = (TheGameLogic != nullptr) ? TheGameLogic->getFrame() : 0;
			if (W3DShadowMap::getQuality() != s_peakQuality || logicFrame < s_peakFrame)
			{
				s_peakQuality = W3DShadowMap::getQuality();
				s_reqPeak     = 0.0f;
				s_extPeak     = 0.0f;
			}
			s_peakFrame = logicFrame;
			if (logicFrame > 60)
			{
				const Real reqNow = W3DShadowMap::getFitRequired();
				const Real exX    = W3DShadowMap::getFitExtentX();
				const Real exY    = W3DShadowMap::getFitExtentY();
				const Real extNow = (exX > exY) ? exX : exY;
				if (reqNow > s_reqPeak) s_reqPeak = reqNow;
				if (extNow > s_extPeak) s_extPeak = extNow;
			}
			// Ronin @diagnostic 13/09/2026 DX9: texel is NEAR/FAR, both cascades live at once; at one split far reads 0.00.
			text.format(L"[SHADOW] texel=%.2f/%.2f ext=%.0fx%.0f req=%.0f box=%.0fx%.0f bare=%.0fx%.0f hdrm=%.0f  PEAK req=%.0f ext=%.0f",
				W3DShadowMap::getTexelWorldSize(0),
				(W3DShadowMap::getSplitCount() > 1) ? W3DShadowMap::getTexelWorldSize(1) : 0.0f,
				W3DShadowMap::getFitExtentX(),
				W3DShadowMap::getFitExtentY(),
				W3DShadowMap::getFitRequired(),
				W3DShadowMap::getFitBoxX(),
				W3DShadowMap::getFitBoxY(),
				W3DShadowMap::getFitBareBoxX(),
				W3DShadowMap::getFitBareBoxY(),
				W3DShadowMap::getHeadroom(),
				s_reqPeak,
				s_extPeak);
			putRow(W3DDebugPanel::ROW_SHADOW, text, GameMakeColor(120, 200, 255, 255));
		}

		if (s_rowOn[W3DDebugPanel::ROW_PERF])
		{
			const double secs    = (s_perfFrames > 0) ? ((double)(s_perfNow - s_perfFirst) / (double)s_perfFreq) : 0.0;
			const Real   meanFps = (secs > 0.0) ? (Real)((double)s_perfFrames / secs) : 0.0f;
			const Real   meanMs  = (s_perfFrames > 0) ? (Real)(secs * 1000.0 / (double)s_perfFrames) : 0.0f;
			const Real   meanSM  = (s_perfFrames > 0) ? (Real)(s_perfSMSum / (double)s_perfFrames) : 0.0f;
			text.format(L"[PERF] frames=%u  avgFps=%.1f  avgMs=%.3f  worstMs=%.1f  avgShadowMap=%.0f",
				s_perfFrames, meanFps, meanMs, s_perfWorstMs, meanSM);
			putRow(W3DDebugPanel::ROW_PERF, text, GameMakeColor(255, 230, 120, 255));
		}

		// Ronin @diagnostic 13/09/2026 DX9: can the card read scene depth, and did this frame use it. aoRes/samples prove a
		// quality change took effect - the tiers differ subtly by eye.
		if (s_rowOn[W3DDebugPanel::ROW_DEPTH])
		{
			Int aoW = 0, aoH = 0;
			W3DSsao::getTargetSize(&aoW, &aoH);
			text.format(L"[DEPTH] ssao=%d  intz=%d  active=%d  aa=%d  aoRes=%dx%d  samples=%d",
				W3DSsao::getQuality(),
				W3DSsao::isSupported() ? 1 : 0,
				W3DSsao::isActive() ? 1 : 0,
				DX8Wrapper::Get_Anti_Aliasing_Level(),
				aoW, aoH,
				W3DSsao::getSampleCount());
			putRow(W3DDebugPanel::ROW_DEPTH, text, GameMakeColor(140, 255, 140, 255));
		}

		// Ronin @diagnostic 14/09/2026 DX9: §19e.3. Splat bake under channel reuse; droppedCells must be 0.
		if (s_rowOn[W3DDebugPanel::ROW_TERRAIN] && TheTerrainRenderObject != nullptr && TheTerrainRenderObject->getMap() != nullptr)
		{
			WorldHeightMap *map = TheTerrainRenderObject->getMap();
			text.format(L"[TERRAIN] materials=%d  channels=%d  droppedCells=%d  pages=%d  maxPerTile=%d",
				map->getSplatWeightableClasses(),
				map->getActiveMaterialCount(),
				map->getSplatDroppedCells(),
				map->getTerrainTexturePageCount(),
				map->getSplatMaxPerTile());
			putRow(W3DDebugPanel::ROW_TERRAIN, text, GameMakeColor(255, 255, 255, 255));
		}

		// Ronin @diagnostic 15/09/2026 DX9: [UI] - who owns input right now, for "map clicks stop until the options menu opens".
		// Panel window ids: 7DEB0001 panel, 7DEB0002 command box, 7DEB0003 spawn picker list.
		if (s_rowOn[W3DDebugPanel::ROW_UI])
		{
			GameWindow *focus   = TheWindowManager->winGetFocus();
			GameWindow *grab    = TheWindowManager->winGetGrabWindow();
			GameWindow *capture = TheWindowManager->winGetCapture();
			// Ronin @diagnostic 16/09/2026 DX9: the three things that can swallow a left click before the selection code acts
			// on it - `quit` destroys it outright (SelectionXlat.cpp onMouseLeftClick), `place` takes it at priority 30 and
			// `gui` at 40. A raw down/up trace cannot show this: raw clicks always run to the end of the stream.
			text.format(L"[UI] input=%d selecting=%d mouseLock=%d quit=%d place=%d gui=%d focus=%08X grab=%08X capture=%08X",
				(TheInGameUI != nullptr && TheInGameUI->getInputEnabled()) ? 1 : 0,
				(TheInGameUI != nullptr && TheInGameUI->isSelecting()) ? 1 : 0,
				(TheTacticalView != nullptr && TheTacticalView->isMouseLocked()) ? 1 : 0,
				(TheInGameUI != nullptr && TheInGameUI->isQuitMenuVisible()) ? 1 : 0,
				(TheInGameUI != nullptr && TheInGameUI->getPendingPlaceType() != nullptr) ? 1 : 0,
				(TheInGameUI != nullptr && TheInGameUI->getGUICommand() != nullptr) ? 1 : 0,
				focus   ? (UnsignedInt)focus->winGetWindowId()   : 0u,
				grab    ? (UnsignedInt)grab->winGetWindowId()    : 0u,
				capture ? (UnsignedInt)capture->winGetWindowId() : 0u);
			putRow(W3DDebugPanel::ROW_UI, text, GameMakeColor(200, 170, 255, 255));
		}

		// Ronin @diagnostic 20/09/2026 DX9: [RSTATE] - what the bypassed render-state cache costs, off by default. rs is every
		// Set_DX8_Render_State reaching the device (dx8wrapper.h:1334); rs/draw says whether fixing it is worth the risk.
		// docs/RenderStateCache.md.
		if (s_rowOn[W3DDebugPanel::ROW_RSTATE])
		{
			const DX8FrameStatistics &fs = DX8Wrapper::Get_Last_Frame_Statistics();
			const Real perDraw = (fs.draw_calls > 0) ? ((Real)fs.render_state_changes / (Real)fs.draw_calls) : 0.0f;
			text.format(L"[RSTATE] rs=%u  rs/draw=%.1f  tss=%u  tex=%u  mat=%u  vb=%u  draw=%u  dx8=%u",
				fs.render_state_changes, perDraw, fs.texture_stage_state_changes, fs.texture_changes,
				fs.material_changes, fs.vertex_buffer_changes, fs.draw_calls, fs.dx8_calls);
			putRow(W3DDebugPanel::ROW_RSTATE, text, GameMakeColor(120, 220, 255, 255));
		}

		// Ronin @diagnostic 20/09/2026 DX9: [TAA] - live state while TAA is on. docs/Debug_Panel_Design.md §7 explains each field.
		if (s_rowOn[W3DDebugPanel::ROW_TAA] && W3DTaa::isEnabled())
		{
			// moving = meshes given motion vectors / meshes drawn; untracked should stay 0.
			UnicodeString moving;
			if (W3DTaa::getVelocity())
				moving.format(L"%d/%d", W3DTaa::getMoverCount(), W3DTaa::getMeshSeen());
			else
				moving = L"off";
			// Ronin @diagnostic 27/09/2026 DX9: gpu = TAA's whole GPU time per frame, mask = the moving-shadow pass inside it, with
			// how many meshes it drew (off = `taa shadowmask 0`, - = nothing moved or no shadow maps).
			UnicodeString gpu;
			float gpuTotal = -1.0f, gpuMask = -1.0f;
			W3DTaa::getGpuMs(&gpuTotal, &gpuMask);
			const Int maskDraws = W3DTaa::getShadowMaskDraws();
			if (gpuTotal < 0.0f)
				gpu = L"n/a";
			else if (!W3DTaa::getShadowMask())
				gpu.format(L"%.2fms  mask=off", gpuTotal);
			else if (maskDraws < 0)
				gpu.format(L"%.2fms  mask=-", gpuTotal);
			else
				gpu.format(L"%.2fms  mask=%.2fms/%d", gpuTotal, gpuMask, maskDraws);
			text.format(L"[TAA] state=%s  weight=%.2f  moving=%s  reactive=%d  skinned=%d  fx-mask=%s  untracked=%d  view=%d  gpu=%s",
				W3DTaa::isActive() ? L"running" : L"off(MSAA)", W3DTaa::getWeight(),
				moving.str(),
				W3DTaa::getReactiveCount(), W3DTaa::getSkinDrawCount(), W3DTaa::getOpaqueOK() ? L"on" : L"off",
				W3DTaa::getTrackMisses(),
				W3DTaa::getDebug(), gpu.str());
			putRow(W3DDebugPanel::ROW_TAA, text, GameMakeColor(255, 200, 120, 255));
		}
	}

	// Ronin @feature 28/09/2026 DX9: no panel window, so no rows. One line where the first row used to sit says so, instead of
	// a second layout for every readout. Drawn after the 3D views and their TAA resolve, before the UI.
	void drawNoPanel()
	{
		if (s_noPanel == nullptr && TheDisplayStringManager != nullptr && TheFontLibrary != nullptr)
		{
			s_noPanel = TheDisplayStringManager->newDisplayString();
			if (s_noPanel != nullptr)
			{
				s_noPanel->setFont(panelFont());
				s_noPanel->setText(UnicodeString(L"[PANEL] the debug panel window could not be created - no readouts"));
			}
		}
		if (s_noPanel != nullptr)
			s_noPanel->draw(DEFAULT_X, DEFAULT_Y, GameMakeColor(255, 255, 0, 255), GameMakeColor(0, 0, 0, 255));
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

		// Ronin @feature 16/09/2026 DX9: the picker's Side/Category labels, above its list (the list draws itself, as a child).
		if (s_pickerOpen && s_sideLabel != nullptr && s_categoryLabel != nullptr)
		{
			const Color label = GameMakeColor(255, 220, 120, 255);
			s_sideLabel->draw(x + PAD, y + s_labelTop, label, drop);
			s_categoryLabel->draw(x + s_categoryLabelX, y + s_labelTop, label, drop);
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
	sampleReadouts();

	if (TheWindowManager == nullptr || TheGlobalData == nullptr || TheGlobalData->m_headless)
		return;

	if (!s_builtinsDone)
	{
		s_builtinsDone = TRUE;
		registerCommand("help", "list the commands", cmdHelp);
		registerCommand("clear", "clear this output", cmdClear);
		registerCommand("close", "hide this panel (Ctrl+Shift+Z shows it again)", cmdClose);
		registerCommand("shutdown", "quit the game to the desktop now", cmdShutdown);
		registerCommand("water", "water [flat|mesh 0|1] - draw the flat water / the water grid mesh", cmdWater);
		registerCommand("rows", "rows [<name>|all 0|1] - list the readout rows, or switch one or all", cmdRows);
		registerCommand("taa", "taa [0|1] | taa <knob> <v> - temporal AA; the [TAA] row shows live state", cmdTaa);
		registerCommand("ssao", "ssao [0..3] | ssao trees <auto|0|1> - ambient occlusion quality (not saved); trees: AO on trees", cmdSsao);
		registerCommand("shadows", "shadows [0..3] - shadow-map quality, 0 off (stencil shadows) .. 3 ultra (not saved)", cmdShadows);
		registerCommand("grid", "grid [lift|width|alpha|radius <value>] - the placement grid Ctrl draws", cmdGrid);
		registerCommand("spawn", "spawn [words] [count] - pick from a list, or spawn <ThingTemplate> [count]; single player only", cmdSpawn);
		registerCommand("credits", "credits [amount] - add money to your player; single player only", cmdCredits);
		registerCommand("power", "power [0|1] - unlimited power for your player, 0 = your real output; single player only", cmdPower);
	}

	// Ronin @feature 16/09/2026 DX9: placement takes the mouse after the window translator (10) and before meta events (20),
	// selection (50) and commands (70). Attached once; MessageStream owns it from here on and reset() keeps it.
	if (!s_translatorReady && TheMessageStream != nullptr)
	{
		s_translatorReady = TRUE;
		TheMessageStream->attachTranslator(NEW DevPlaceTranslator, 15);
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
		{
			drawNoPanel();
			return;
		}
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
	// Ronin @feature 16/09/2026 DX9: the spawn picker list, recreated and refilled after a reset like the box.
	GameWindow *list = (win->winGetChild() != nullptr)
		? TheWindowManager->winGetWindowFromId(win->winGetChild(), LIST_WINDOW_ID) : nullptr;
	if (list == nullptr)
	{
		list = createList(win);
		s_pickerDirty = TRUE;
	}

	// Ronin @feature 16/09/2026 DX9: a row clicked since last frame — spawn it and close the picker. Done here, not inside the
	// list's input call, because clearing the box empties that same list.
	if (s_pickedID >= 0)
	{
		const Int picked = s_pickedID;
		s_pickedID = -1;
		const ThingTemplate *thing = (TheThingFactory != nullptr) ? TheThingFactory->findByTemplateID((UnsignedShort)picked) : nullptr;
		if (thing != nullptr && logicCommandAllowed())
			startPlacement(thing, s_pickCount);
		if (entry != nullptr)
			GadgetTextEntrySetText(entry, UnicodeString::TheEmptyString);
		TheWindowManager->winSetFocus(nullptr);
	}

	// Ronin @feature 16/09/2026 DX9: the picker is open while the box reads "spawn "; refilled when the text or a label changes.
	{
		const UnicodeString text = (entry != nullptr) ? GadgetTextEntryGetText(entry) : UnicodeString::TheEmptyString;
		if (list == nullptr)
		{
			s_pickerOpen = FALSE;
		}
		else if (s_pickerDirty || text.compare(s_pickerText) != 0)
		{
			s_pickerDirty = FALSE;
			s_pickerText  = text;
			AsciiString filter;
			s_pickerOpen = pickerFilter(text, &filter);
			if (s_pickerOpen)
				fillPicker(list, filter);
			else
				GadgetListBoxReset(list);
		}
	}
	updatePlacement();
	const Bool entryFocused = (entry != nullptr && TheWindowManager->winGetFocus() == entry);

	renderReadouts();

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
	// Ronin @feature 16/09/2026 DX9: the picker list's column width is fixed at creation, so the panel fits the list.
	if (s_pickerOpen && width < PICKER_WIDTH)
		width = PICKER_WIDTH;

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
	// Ronin @feature 16/09/2026 DX9: with the picker open, its Side/Category labels and list sit between the box and the output.
	s_labelTop = entryY + ENTRY_HEIGHT + PAD;
	const Int listY = s_labelTop + LABEL_HEIGHT + PAD;
	s_outputTop = s_pickerOpen ? listY + PICKER_HEIGHT + PAD : entryY + ENTRY_HEIGHT + PAD;
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

	if (list != nullptr)
	{
		if (list->winIsHidden() == s_pickerOpen)
			list->winHide(!s_pickerOpen);
		Int lx = 0, ly = 0;
		list->winGetPosition(&lx, &ly);
		if (lx != PAD || ly != listY)
			list->winSetPosition(PAD, listY);
	}
	// Ronin @feature 16/09/2026 DX9: label widths, for panelInput's click test.
	if (s_pickerOpen && s_sideLabel != nullptr && s_categoryLabel != nullptr)
	{
		Int tw = 0, th = 0;
		s_sideLabel->getSize(&tw, &th);
		s_sideLabelW     = tw;
		s_categoryLabelX = PAD + tw + 16;
		s_categoryLabel->getSize(&tw, &th);
		s_categoryLabelW = tw;
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
