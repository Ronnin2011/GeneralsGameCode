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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// TileData.cpp
// Class to handle tile data.
// Author: John Ahlquist, April 2001

#include "W3DDevice/GameClient/TileData.h"
#include "W3DDevice/GameClient/WorldHeightMap.h"

//
// TileData - create a new texture tile.
// 
TileData::TileData(Int pixelExtent) :
	m_pixelExtent(pixelExtent),
	m_tileData(pixelExtent* pixelExtent* TILE_BYTES_PER_PIXEL),
	m_texturePage(0)
{
}

Bool TileData::hasRGBDataForWidth(Int width)
{
	if (width <= 0) {
		return false;
	}
	if (width > m_pixelExtent) {
		return false;
	}
	if ((m_pixelExtent % width) != 0) {
		return false;
	}
	if ((width & (width - 1)) != 0) {
		return false;
	}
	return true;
}

UnsignedByte* TileData::getRGBDataForWidth(Int width)
{
	if (width == m_pixelExtent) {
		return m_tileData.data();
	}

	Int curWidth = m_pixelExtent / 2;
	for (std::vector<UnsignedByte>& mip : m_mipData) {
		if (width == curWidth) {
			return mip.data();
		}
		curWidth /= 2;
	}

	return m_tileData.data();
}

void TileData::updateMips()
{
	m_mipData.clear();

	UnsignedByte* pHiRes = m_tileData.data();
	Int hiRow = m_pixelExtent;
	while (hiRow > 1) {
		const Int loRow = hiRow / 2;
		m_mipData.emplace_back(loRow * loRow * TILE_BYTES_PER_PIXEL);
		doMip(pHiRes, hiRow, m_mipData.back().data());
		pHiRes = m_mipData.back().data();
		hiRow = loRow;
	}
}

void TileData::doMip(UnsignedByte* pHiRes, Int hiRow, UnsignedByte* pLoRes)
{
	Int i, j;
	for (i = 0; i < hiRow; i += 2) {
		for (j = 0; j < hiRow; j += 2) {
			Int pxl;
			Int ndx = (j * hiRow + i) * TILE_BYTES_PER_PIXEL;
			Int loNdx = (j / 2) * (hiRow / 2) + (i / 2);
			loNdx *= TILE_BYTES_PER_PIXEL;
			Int p;
			for (p = 0; p < TILE_BYTES_PER_PIXEL; p++, ndx++, loNdx++) {
				pxl = pHiRes[ndx]
					+ pHiRes[ndx + TILE_BYTES_PER_PIXEL]
					+ pHiRes[ndx + TILE_BYTES_PER_PIXEL * hiRow]
					+ pHiRes[ndx + TILE_BYTES_PER_PIXEL * hiRow + TILE_BYTES_PER_PIXEL]
					+ 2;
				pxl /= 4;
				pLoRes[loNdx] = pxl;
			}
		}
	}
}
