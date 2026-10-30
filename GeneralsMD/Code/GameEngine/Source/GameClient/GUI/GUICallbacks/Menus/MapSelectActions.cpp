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

// Ronin @feature 30/10/2026 DX9: tintinhamans' (GeneralsGameCode, arctic/go/feat/rmlui), ported as he wrote it.
// FILE: MapSelectActions.cpp //////////////////////////////////////////////////

#include "PreRTS.h"

#include "GameClient/GUI/GUICallbacks/Menus/MapSelectActions.h"

#include "Common/UserPreferences.h"
#include "GameClient/MapUtil.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "Common/GlobalData.h"
#include "Common/STLTypedefs.h"

#include <map>
#include <set>

// See header. Mirrors SkirmishMapSelectMenuInit()'s usesSystemMapDir logic exactly.
Bool MapSelectActions::initialUsesSystemMaps( const AsciiString &currentMap )
{
	LANPreferences pref;
	Bool usesSystemMapDir = pref.usesSystemMapDir();

	const MapMetaData *mmd = TheMapCache ? TheMapCache->findMap( currentMap ) : nullptr;
	if( mmd )
		usesSystemMapDir = mmd->m_isOfficial;

	return usesSystemMapDir;
}

// See header. Mirrors SkirmishMapSelectMenu.cpp's ButtonOK handler exactly.
void MapSelectActions::applySelectedMap( GameInfo *game, const AsciiString &mapName )
{
	if( !game )
		return;

	game->setMap( mapName );

	const MapMetaData *md = TheMapCache ? TheMapCache->findMap( mapName ) : nullptr;
	if( !md )
	{
		game->setMapCRC( 0 );
		game->setMapSize( 0 );
	}
	else
	{
		game->setMapCRC( md->m_CRC );
		game->setMapSize( md->m_filesize );
	}

	for( Int i = 0; i < MAX_SLOTS; ++i )
		game->getSlot( i )->setStartPos( -1 );
}

// Ronin @feature 30/10/2026 DX9: his MapUtil.cpp map-list builder (the same filter as populateMapListbox), kept with
// MapSelectActions so MapUtil stays as upstream has it.
typedef std::set<UnicodeString, rts::less_than_nocase<UnicodeString>/**/> MapNameList;
typedef std::map<UnicodeString, AsciiString> MapDisplayToFileNameList;

static void buildMapListForNumPlayers(MapNameList &outMapNames, MapDisplayToFileNameList &outFileNames, Int numPlayers)
{
	MapCache::iterator it = TheMapCache->begin();

	for (; it != TheMapCache->end(); ++it)
	{
		const MapMetaData &mapData = it->second;
		if (mapData.m_numPlayers == numPlayers)
		{
			outMapNames.insert(it->second.m_displayName);
			outFileNames[it->second.m_displayName] = it->first;
		}
	}
}


//-------------------------------------------------------------------------------------------------
static AsciiString computeMapListDir(Bool useSystemMaps)
{
	AsciiString mapDir;
	if (useSystemMaps)
	{
		mapDir = TheMapCache->getMapDir();
	}
	else
	{
		mapDir = TheGlobalData->getPath_UserData();
		mapDir.concat(TheMapCache->getMapDir());
	}
	mapDir.toLower();
	return mapDir;
}

//-------------------------------------------------------------------------------------------------
static void collectMapEntriesForCollection(
	MapEntryList& outEntries,
	const AsciiString& mapDir,
	Bool isMultiplayer,
	const AsciiString& mapToSelect,
	const MapNameList& mapNames,
	const MapDisplayToFileNameList& fileNames)
{
	MapNameList::const_iterator mapNameIt = mapNames.begin();

	for (; mapNameIt != mapNames.end(); ++mapNameIt)
	{
		MapDisplayToFileNameList::const_iterator fileNameIt = fileNames.find(*mapNameIt);
		DEBUG_ASSERTCRASH(fileNameIt != fileNames.end(), ("Map '%s' not found in file names map", mapNameIt->str()));

		const AsciiString& asciiMapName = fileNameIt->second;

#if RTS_ZEROHOUR
		//Patch 1.03 -- Purposely filter out these broken maps that exist in Generals.
		if( !asciiMapName.compare( "maps\\armored fury\\armored fury.map" ) ||
			!asciiMapName.compare( "maps\\scorched earth\\scorched earth.map" ) )
		{
			continue;
		}
#endif

		MapCache::iterator mapCacheIt = TheMapCache->find(asciiMapName);
		DEBUG_ASSERTCRASH(mapCacheIt != TheMapCache->end(), ("Map '%s' not found in map cache.", mapNameIt->str()));
		/*
		if (it != TheMapCache->end())
		{
			DEBUG_LOG(("populateMapListbox(): looking at %s (displayName = %ls), mp = %d (== %d?) mapDir=%s (ok=%d)",
				it->first.str(), it->second.m_displayName.str(), it->second.m_isMultiplayer, isMultiplayer,
				mapDir.str(), it->first.startsWith(mapDir.str())));
		}
		*/

		const AsciiString& mapName = mapCacheIt->first;
		const MapMetaData& mapMetaData = mapCacheIt->second;

		const Bool mapOk = mapName.startsWithNoCase(mapDir.str()) && isMultiplayer == mapMetaData.m_isMultiplayer && !mapMetaData.m_displayName.isEmpty();

		if (!mapOk)
			continue;

		MapListEntry entry;
		entry.mapName = mapName; // shares the ref-counted buffer with the map cache key
		entry.displayName = mapMetaData.m_displayName;
		entry.isOfficial = mapMetaData.m_isOfficial;
		entry.numPlayers = mapMetaData.m_numPlayers;
		entry.filesize = mapMetaData.m_filesize;
		entry.isSelected = (mapName == mapToSelect);
		outEntries.push_back(entry);
	}
}

//-------------------------------------------------------------------------------------------------
/** Build the widget-agnostic, filtered list of maps to display. Shared by gadget listboxes
 * and alternative (e.g. RmlUi) front ends. */
//-------------------------------------------------------------------------------------------------
MapEntryList buildFilteredMapList( Bool useSystemMaps, Bool isMultiplayer, AsciiString mapToSelect )
{
	MapEntryList entries;

	if (!TheMapCache)
		return entries;

	const AsciiString mapDir = computeMapListDir(useSystemMaps);

	MapNameList mapNames;
	MapDisplayToFileNameList fileNames;

	for (Int curNumPlayersInMap = 1; curNumPlayersInMap <= MAX_SLOTS; ++curNumPlayersInMap)
	{
		buildMapListForNumPlayers(mapNames, fileNames, curNumPlayersInMap);

		collectMapEntriesForCollection(entries, mapDir, isMultiplayer, mapToSelect, mapNames, fileNames);

		mapNames.clear();
		fileNames.clear();
	}

	return entries;
}
