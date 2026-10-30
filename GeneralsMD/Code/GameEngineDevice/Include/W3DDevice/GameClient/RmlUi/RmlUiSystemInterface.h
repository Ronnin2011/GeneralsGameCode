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

// Ronin @feature 28/10/2026 DX9: RmlUi's clock and log. Ported from tintinhamans' GeneralsGameCode fork
// (arctic/go/feat/rmlui); its warnings and errors also print in the debug panel.
#pragma once

#include <RmlUi/Core/SystemInterface.h>

// A line into the debug panel, wrapped at a space every 110 characters as the panel's own output is.
void RmlUiPrint(const char *text, bool isError);

class RmlUiSystemInterface : public Rml::SystemInterface
{
public:
	RmlUiSystemInterface();

	virtual double GetElapsedTime() override;	// the wall clock, so transitions keep their pace at any frame rate
	virtual bool LogMessage(Rml::Log::Type type, const Rml::String &message) override;

private:
	long long m_start;
	double m_period;	// seconds per counter tick
};
