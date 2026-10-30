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
// FILE: SaveLoadData.cpp ////////////////////////////////////////////////////
// See SaveLoadData.h.
///////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include "GameClient/GUI/GUICallbacks/Menus/SaveLoadData.h"

#include "GameClient/CampaignManager.h"
#include "GameClient/Color.h"
#include "GameClient/GameText.h"

// Ronin @feature 30/10/2026 DX9: his GameState::describeAvailableGame(), here: it reads only public state, so GameState
// keeps upstream's shape. Label, time and date of a save and its list colour, as populateSaveGameListbox() shows them.
static Int describeAvailableGame( const AvailableGameInfo *info, UnsignedInt count,
	UnicodeString &label, UnicodeString &time, UnicodeString &date )
{
	const SaveGameInfo *saveGameInfo = &info->saveGameInfo;

	SYSTEMTIME systemTime;
	systemTime.wYear = saveGameInfo->date.year;
	systemTime.wMonth = saveGameInfo->date.month;
	systemTime.wDayOfWeek = saveGameInfo->date.dayOfWeek;
	systemTime.wDay = saveGameInfo->date.day;
	systemTime.wHour = saveGameInfo->date.hour;
	systemTime.wMinute = saveGameInfo->date.minute;
	systemTime.wSecond = saveGameInfo->date.second;
	systemTime.wMilliseconds = saveGameInfo->date.milliseconds;
	date = getUnicodeDateBuffer(systemTime);
	time = getUnicodeTimeBuffer(systemTime);

	label = saveGameInfo->description;
	if( label.isEmpty() == TRUE )
	{
		Bool exists = FALSE;
		label = TheGameText->fetch( saveGameInfo->mapLabel, &exists );
		if( exists == FALSE )
			label.format( L"%S", saveGameInfo->mapLabel.str() );
	}

	if( saveGameInfo->saveFileType == SAVE_FILE_TYPE_MISSION )
		return GameMakeColor( 200, 255, 200, 255 );
	else if( count & 0x1 )
		return GameMakeColor( 255, 255, 255, 255 );
	return GameMakeColor( 170, 170, 235, 255 );
}

// The save's map and campaign as the details of the list show them.
static void describeSave( const SaveGameInfo &info, SaveLoadRow &row )
{
	Bool exists = FALSE;
	row.m_mapLabel = TheGameText->fetch( info.mapLabel, &exists );
	if( exists == FALSE )
		row.m_mapLabel.translate( info.mapLabel );

	if( info.campaignSide.isNotEmpty() )
	{
		AsciiString label;
		label.format( "CAMPAIGN:%s", info.campaignSide.str() );
		row.m_campaign = TheGameText->fetch( label, &exists );
		if( exists == FALSE )
			row.m_campaign.translate( info.campaignSide );
		if( info.missionNumber != CampaignManager::INVALID_MISSION_NUMBER )
			row.m_missionNumber = info.missionNumber + 1;
	}

	row.m_isMissionSave = info.saveFileType == SAVE_FILE_TYPE_MISSION;
}

void SaveLoadData::open( SaveLoadLayoutType layoutType, Bool isPopup )
{
	m_layoutType = layoutType;
	m_isPopup = isPopup;
	m_dialog = DIALOG_NONE;
	m_description.clear();
	refresh();
}

void SaveLoadData::refresh()
{
	m_rows.clear();

	// saving is allowed: the first entry is a new save game
	if( m_layoutType != SLLT_LOAD_ONLY )
	{
		SaveLoadRow row;
		row.m_name = TheGameText->fetch( "GUI:NewSaveGame" );
		row.m_color = 0xC8C8FF;
		m_rows.push_back( row );
	}

	UnsignedInt count = 0;
	for( AvailableGameInfo *info = TheGameState->refreshAvailableGames(); info; info = info->next, count++ )
	{
		SaveLoadRow row;
		Int color = describeAvailableGame( info, count, row.m_name, row.m_time, row.m_date );
		row.m_color = color & 0xFFFFFF;
		row.m_info = info;
		describeSave( info->saveGameInfo, row );
		m_rows.push_back( row );
	}

	++m_rowsVersion;
	select( m_rows.empty() ? -1 : 0 );
}

void SaveLoadData::select( Int row )
{
	m_selected = ( row >= 0 && row < (Int)m_rows.size() ) ? row : -1;
	touch();
}

AvailableGameInfo *SaveLoadData::selectedInfo() const
{
	return ( m_selected >= 0 && m_selected < (Int)m_rows.size() ) ? m_rows[m_selected].m_info : nullptr;
}
