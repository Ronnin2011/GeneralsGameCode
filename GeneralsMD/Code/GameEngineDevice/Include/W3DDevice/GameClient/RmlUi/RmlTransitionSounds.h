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

// Ronin @feature 30/10/2026 DX9: tintinhamans' TransitionSounds (GeneralsGameCode, arctic/go/feat/rmlui, his Core file), here
// header-only for the RmlUi menus: a WindowTransitions.ini group's sounds at the frames its windows would have played them.
#pragma once

#include "Common/AudioEventRTS.h"
#include "Common/GameAudio.h"
#include "Common/GlobalData.h"
#include "GameClient/GameWindowTransitions.h"

#include <chrono>
#include <vector>

namespace TransitionSounds
{
	typedef std::chrono::steady_clock Clock;

	struct PendingSound
	{
		AsciiString m_group;
		AsciiString m_sound;
		Clock::time_point m_due;
	};

	inline std::vector<PendingSound> &pending()
	{
		static std::vector<PendingSound> s_pending;
		return s_pending;
	}

	// Queues what TheTransitionHandler->setGroup(group) (or ->reverse(group)) would play; delayFrames starts it behind a group
	// played before. Returns the group's length in frames, 0 for no such group.
	inline Int play(const char *group, Bool reversed = FALSE, Int delayFrames = 0)
	{
		if (!TheTransitionHandler)
			return 0;
		TransitionSoundCues cues;
		const Int frames = TheTransitionHandler->getGroupSounds(AsciiString(group), reversed, cues);

		// the speed the transition groups run at (TransitionGroup::update)
		Real speed = 30.0f;
		if (TheGlobalData && TheGlobalData->m_gameWindowTransitionSpeedMultiplier > 0.0f)
			speed *= TheGlobalData->m_gameWindowTransitionSpeedMultiplier;

		const Clock::time_point now = Clock::now();
		for (size_t i = 0; i < cues.size(); ++i)
		{
			PendingSound p;
			p.m_group = group;
			p.m_sound = cues[i].m_sound;
			p.m_due = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<Real>((delayFrames + cues[i].m_frame) / speed));
			pending().push_back(p);
		}
		return frames;
	}

	// Once a frame: fires the sounds that are due.
	inline void update()
	{
		std::vector<PendingSound> &list = pending();
		if (list.empty())
			return;
		const Clock::time_point now = Clock::now();
		for (size_t i = 0; i < list.size();)
		{
			if (list[i].m_due > now)
			{
				++i;
				continue;
			}
			AudioEventRTS event(list[i].m_sound);
			list.erase(list.begin() + i);
			if (TheAudio)
				TheAudio->addAudioEvent(&event);
		}
	}
}
