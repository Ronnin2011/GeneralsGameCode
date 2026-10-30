/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 port: the modern control bar's panels the 2003 bar never had.
*/

// Ronin @feature 28/10/2026 DX9: the control bar's selected units (18 boxes, then by type), control groups, health and factory
// tabs. Client state only: a click sends what a click in the world or a number key would.
#pragma once

#include <RmlUi/Core/DataModelHandle.h>

namespace Rml { class Element; }

class RmlHudPanels
{
public:
	static void bind(Rml::DataModelConstructor &constructor);	// with the bar's own variables, once
	static void refresh(Rml::DataModelHandle &model);			// every tick, before the context updates
	static void hover(Rml::Element *element);					// what the pointer is on, for the selection's label
	static bool click(Rml::Element *element, int button);		// a release on one of ours: true when it was
	static void reset();										// the document went away: no group stays open
};
