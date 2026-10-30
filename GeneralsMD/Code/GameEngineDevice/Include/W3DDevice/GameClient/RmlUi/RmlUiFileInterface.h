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

// Ronin @feature 28/10/2026 DX9: RmlUi's files through TheFileSystem, so loose files and the archives both resolve. Ported
// from tintinhamans' GeneralsGameCode fork (arctic/go/feat/rmlui).
#pragma once

#include <RmlUi/Core/FileInterface.h>
#include <set>
#include <string>

// Where the UI's documents, style sheets and fonts live, in the game folder.
static const char *const RmlUiRoot = "Data/UI/";

// The shared style sheet. Read through here it gains every INI MappedImage as a sprite, so a document names a cameo the
// way the INI does: decorator: image(SACDozer).
static const char *const RmlCommonRcssPath = "Data/UI/common.rcss";

class RmlUiFileInterface : public Rml::FileInterface
{
public:
	virtual Rml::FileHandle Open(const Rml::String &path) override;
	virtual void Close(Rml::FileHandle file) override;
	virtual size_t Read(void *buffer, size_t size, Rml::FileHandle file) override;
	virtual bool Seek(Rml::FileHandle file, long offset, int origin) override;
	virtual size_t Tell(Rml::FileHandle file) override;
	virtual size_t Length(Rml::FileHandle file) override;

private:
	struct MemFile
	{
		std::string data;
		size_t pos = 0;
	};

	std::string m_mappedImagesRcss;		// built on first use, once the image collection is loaded
	std::set<MemFile *> m_memFiles;		// open in-memory handles; any other handle is a File *
};
