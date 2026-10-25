#pragma once

// The game's own zone and character drawing switched off, for the native renderer to come (a debug toggle, /hifi
// gamedraw; off at every start, never saved). research/ffxi-re/notes-offswitch.md has the sites and what depends on
// them:
//   - zone: the per-object zone draw (FFXiMain 17C9C0) and the lamp-lit models (17ADE0) return at once; the zone's
//     frame setup (camera, lights, fog, culling) still runs;
//   - chars: the model draw (02B6E0) leaves straight after posing through its own early return (02B7E3 jumps to
//     02B7B0), so palettes, the characters' lights, the visibility tests and cutscene cameras go on.
// Nothing draws into the world's depth in their place: walls stop hiding names and targets while one is off.

#include "game.hpp"
#include <string>

namespace hifi::game::gamedraw
{
	enum class part
	{
		zone,
		chars,
	};

	// One part's patches in (off: the game doesn't draw it) or out; false and why when they can't be
	bool set(part p, bool off, std::string &why);
	bool is_off(part p);
	// Both put back (on unload); false and why when something else changed the code since (then left as it is)
	bool stop(std::string &why);
	std::string status();

	// For the tests: where each site is (0 before it's been looked for or when it wasn't found)
	struct sites
	{
		uintptr_t zone_draw = 0, zone_lit = 0, chars_jump = 0, chars_return = 0;
	};
	sites where();
}
