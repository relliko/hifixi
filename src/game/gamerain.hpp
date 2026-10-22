#pragma once

// The game's rain and snow hidden while World FX draws its own (the worldfx addon's /worldfx gamerain, in C++): the
// effect element draw's "mov al, [ebp+17Ah]" (FFXiMain 04609D) becomes a call to a routine of ours that also skips a
// weather's elements on its list (weather folder name, model name), keeping their sky layers. The list is filled
// while World FX draws its own rain or snow in the zone (a worldfx-rain\<zone>.bin, its Rain / Snow settings on with
// their amounts over 0). The routine's bytes are the addon's, byte for byte.

#include "game.hpp"
#include <string>

namespace hifi::game::gamerain
{
	// The draw's read to the routine (hide) or back (the HideGameRain setting); false and why when it can't be
	bool route(bool hide, std::string &why);
	// Every half second: the routine's list for the zone, or empty
	void update(uint32_t zone);
	void count_frame();
	// Put back, its memory freed when nothing points into it, everything forgotten (a route after starts afresh); false
	// and why when something else changed the code since
	bool stop(std::string &why);
	std::string status();
	std::string seen();

	// For the tests: the routine's memory (0 before the first route) and the site
	struct sites
	{
		uintptr_t memory = 0, site = 0;
	};
	sites where();
}
