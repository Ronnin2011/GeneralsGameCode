/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 port: the water ini - the advanced look of the reflective water (WaterType 2), kept by the player.
*/

// Ronin @feature 04/10/2026 DX9: every knob of W3DWaterSeaTuning.h, per water kind, in a file beside Options.ini. Read
// when the water is created and by `water load`, written by `water save`; a missing file or key keeps the built-in value.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Common/GlobalData.h"
#include "W3DDevice/GameClient/W3DWaterSeaTuning.h"

// The file: DX9Water.ini in the user data folder, where Options.ini is.
inline AsciiString WaterSea_IniPath()
{
	AsciiString path;
	if (TheGlobalData != nullptr)
		path = TheGlobalData->getPath_UserData();
	path.concat("DX9Water.ini");
	return path;
}

// [All] holds the switches for all water, then one section per kind, in WaterKind order.
static const char *const WATERSEA_INI_SECTIONS[1 + WATER_KIND_COUNT] = { "All", "Sea", "Lake", "River" };

// Reads the file over the current values. Returns how many were set, or -1 when there is no file. `skipped` counts the
// lines it could not use: an unknown section or name, or a line with no `=`.
inline Int WaterSea_LoadIni(const char *path, Int *skipped)
{
	if (skipped != nullptr)
		*skipped = 0;
	FILE *file = fopen(path, "rt");
	if (file == nullptr)
		return -1;
	Int section = -1, set = 0, bad = 0;
	char line[256];
	while (fgets(line, sizeof(line), file) != nullptr)
	{
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == ';' || *p == '#' || *p == '\r' || *p == '\n' || *p == 0)
			continue;
		if (*p == '[')
		{
			section = -1;
			char *close = strchr(p, ']');
			if (close != nullptr)
			{
				*close = 0;
				for (Int s = 0; s < 1 + WATER_KIND_COUNT; s++)
					if (_stricmp(p + 1, WATERSEA_INI_SECTIONS[s]) == 0)
						section = s;
			}
			if (section < 0)
				bad++;
			continue;
		}
		char *equals = strchr(p, '=');
		if (equals == nullptr || section < 0)
		{
			bad++;
			continue;
		}
		char *nameEnd = equals;
		while (nameEnd > p && (nameEnd[-1] == ' ' || nameEnd[-1] == '\t'))
			nameEnd--;
		*nameEnd = 0;
		const char *value = equals + 1;
		while (*value == ' ' || *value == '\t')
			value++;
		const WaterKnobInfo *knob;
		void *base;
		if (section == 0)
		{
			knob = WaterSea_FindKnob(TheWaterGlobalKnobs, TheWaterGlobalKnobCount, p);
			base = &TheWaterSeaGlobal;
		}
		else
		{
			knob = WaterSea_FindKnob(TheWaterProfileKnobs, TheWaterProfileKnobCount, p);
			base = &TheWaterSeaProfiles[section - 1];
		}
		if (knob == nullptr || !knob->saved)
		{
			bad++;
			continue;
		}
		// a switch also takes yes / no, as Options.ini writes them
		Real number = (Real)atof(value);
		if (*value == 'y' || *value == 'Y')
			number = 1.0f;
		WaterSea_SetKnob(base, *knob, number);
		set++;
	}
	fclose(file);
	if (skipped != nullptr)
		*skipped = bad;
	return set;
}

inline void WaterSea_WriteKnobs(FILE *file, const void *base, const WaterKnobInfo *table, Int count)
{
	for (Int k = 0; k < count; k++)
	{
		if (!table[k].saved)
			continue;
		const Real value = WaterSea_GetKnob(base, table[k]);
		if (table[k].kind == WATER_KNOB_REAL)
			fprintf(file, "%s = %g\n", table[k].name, value);
		else
			fprintf(file, "%s = %d\n", table[k].name, (int)value);
	}
}

// Writes every saved knob as it stands. FALSE when the file cannot be written.
inline Bool WaterSea_SaveIni(const char *path)
{
	FILE *file = fopen(path, "wt");
	if (file == nullptr)
		return FALSE;
	fprintf(file, "; DX9 water: the advanced look of the reflective water (WaterType 2).\n");
	fprintf(file, "; Written by `water save` in the debug panel; read when the game starts and by `water load`.\n");
	fprintf(file, "; The names are the panel's `water` knobs. A missing line keeps the built-in value;\n");
	fprintf(file, "; delete the file to return to the built-in look.\n");
	fprintf(file, "; Reflections on / off is not here: Options.ini, DX9WaterReflections = yes | no.\n");
	fprintf(file, "\n[%s]\n", WATERSEA_INI_SECTIONS[0]);
	WaterSea_WriteKnobs(file, &TheWaterSeaGlobal, TheWaterGlobalKnobs, TheWaterGlobalKnobCount);
	for (Int kind = 0; kind < WATER_KIND_COUNT; kind++)
	{
		fprintf(file, "\n[%s]\n", WATERSEA_INI_SECTIONS[1 + kind]);
		WaterSea_WriteKnobs(file, &TheWaterSeaProfiles[kind], TheWaterProfileKnobs, TheWaterProfileKnobCount);
	}
	const Bool ok = (ferror(file) == 0) ? TRUE : FALSE;
	fclose(file);
	return ok;
}
