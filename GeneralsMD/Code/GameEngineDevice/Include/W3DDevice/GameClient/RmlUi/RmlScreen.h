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

// Ronin @feature 29/10/2026 DX9: a shell screen RmlUi draws in place of its .wnd (RmlUiScreenRegistry): one document and one
// data model, loaded on first use. After tintinhamans' RmlScreen (GeneralsGameCode, arctic/go/feat/rmlui).
#pragma once

namespace Rml { class ElementDocument; }

class RmlScreen
{
public:
	virtual ~RmlScreen() {}

	virtual void open() = 0;			// loads on first use, reads the game's state, shows; again while shown: to the front only
	virtual void close() = 0;
	virtual bool isVisible() const = 0;
	virtual void back() = 0;			// Escape
	virtual void update() {}			// every frame while shown
	virtual void reload() = 0;			// `ui reload`: the document again from disk, the model kept
	virtual void release() = 0;			// before Rml::Shutdown(), which frees the document and the model
	virtual Rml::ElementDocument *document() const = 0;
};
