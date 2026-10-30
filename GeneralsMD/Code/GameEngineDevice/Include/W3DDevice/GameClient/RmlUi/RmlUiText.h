/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 port: the game's UTF-16 text to RmlUi's UTF-8 and back.
*/

// Ronin @feature 30/10/2026 DX9: his Common/UnicodeUtf8 (the same WideCharToMultiByte conversion, U+FFFD for bad input);
// our inline copies went, as both defined unicodeToUtf8().
#pragma once

#include "Common/UnicodeUtf8.h"
