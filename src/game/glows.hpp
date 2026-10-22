#pragma once

// Glows (the worldfx addon's send_glows, in C++): lights for glowing things the game draws but lights nothing with,
// sent to World FX (worldfx_glows) every other frame, 10 words each: id, kind, colour r g b, where (the game's world,
// y down), reach, brightness.
//   - fixtures (kind 2): NPCs the game draws glowing (Home Points, Survival Guides, waypoints...), found by name
//     (a few entities a frame), each pulsing slowly at its glow's height;
//   - effects (kind 3): the game's glowing effect elements alive now, from its task list, grouped by what made them,
//     merged when close, the nearest kept; where, from their own view-space position through the effects' view matrix.
// Read only: nothing is written to the game.

#include "game.hpp"
#include <string>
#include <vector>

namespace hifi::game::glows
{
	// The game's effect code looked for (once; status says why not when it isn't found)
	void find_code();
	// A new zone: its fixtures looked for again
	void zone_changed();
	// Everything forgotten (the plugin unloaded: started again, the code is looked for afresh)
	void stop();
	// Each present: the fixtures scanned on, and every other frame the glows sent (now: seconds, for the pulses)
	void frame(const entities &e, double now);
	// For /hifi status and the menu
	std::string status();
	std::vector<std::string> list(const entities &e);
}
