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

// Ronin @feature 28/10/2026 DX9: RmlUi's clock and log. Ported from tintinhamans' GeneralsGameCode fork (arctic/go/feat/rmlui).
#include "W3DDevice/GameClient/RmlUi/RmlUiSystemInterface.h"

#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"
#include "W3DDevice/GameClient/R3DDebugPanel.h"

#include <stdio.h>
#include <string.h>

void RmlUiPrint(const char *text, bool isError)
{
	const int wrap = 110;
	const char *s = text;
	bool first = true;
	while (*s)
	{
		int len = (int)strlen(s);
		if (len > wrap)
		{
			len = wrap;
			while (len > wrap / 2 && s[len] != ' ')
				--len;
		}
		char part[160];
		_snprintf_s(part, sizeof(part), _TRUNCATE, "%s%.*s", first ? "" : "    ", len, s);
		UnicodeString line;
		line.translate(AsciiString(part));
		R3DDebugPanel::print(line, isError ? TRUE : FALSE);
		s += len;
		while (*s == ' ')
			++s;
		first = false;
	}
}

RmlUiSystemInterface::RmlUiSystemInterface()
{
	LARGE_INTEGER frequency, now;
	::QueryPerformanceFrequency(&frequency);
	::QueryPerformanceCounter(&now);
	m_start = now.QuadPart;
	m_period = frequency.QuadPart > 0 ? 1.0 / (double)frequency.QuadPart : 0.0;
}

double RmlUiSystemInterface::GetElapsedTime()
{
	LARGE_INTEGER now;
	::QueryPerformanceCounter(&now);
	return (double)(now.QuadPart - m_start) * m_period;
}

bool RmlUiSystemInterface::LogMessage(Rml::Log::Type type, const Rml::String &message)
{
	const char *prefix = "info";
	switch (type)
	{
		case Rml::Log::LT_ERROR: prefix = "error"; break;
		case Rml::Log::LT_ASSERT: prefix = "assert"; break;
		case Rml::Log::LT_WARNING: prefix = "warning"; break;
		case Rml::Log::LT_DEBUG: prefix = "debug"; break;
		default: break;
	}
	char line[1024];
	_snprintf_s(line, sizeof(line), _TRUNCATE, "RmlUi %s: %s", prefix, message.c_str());
	::OutputDebugStringA(line);
	::OutputDebugStringA("\n");

	if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT || type == Rml::Log::LT_WARNING)
		RmlUiPrint(line, type != Rml::Log::LT_WARNING);
	return true;	// go on, never break into the debugger
}
