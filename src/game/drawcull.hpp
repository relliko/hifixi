#pragma once

// drawcull (the Ashita addon drawcull 1.5, in C++): the view distance raised without zone objects popping in or dropping
// detail early. Its options are HiFi XI's [drawcull] section: world, entity (the draw distance multipliers, 1 stock),
// lod (how much further objects keep their detailed mesh; 0: as world), vis (draw what the zone's visibility lists
// would hide), square (square pixels) and frustum (cull zone objects with the camera's own height of view).
//   - the world and entity multipliers written (the floats the drawdistance addon writes);
//   - the object culling scale's call through a stub that multiplies it by the world multiplier once more, and the
//     four mesh detail compares through stubs that scale the distance by 1 / lod^2 (in at the first update, out at
//     stop);
//   - the zone visibility list's read made 0 (vis), the projection's aspect constant 0.75 (square), the zone's copy of
//     the culling frustum narrowed to the camera's height, 384 (frustum: effects keep the game's), as the options say.
// Each patch goes in only over the game's own bytes and comes out only over ours; the stubs are the addon's, byte for
// byte (the frustum's routine is HiFi XI's own: the addon narrowed the effects' frustum too).

#include "game.hpp"
#include <string>
#include <vector>

namespace hifi::game::drawcull
{
	// Every half second: the options followed (the game's code looked for and patched at the first)
	void update();
	// Each present: nothing (drawcull needs nothing a frame)
	void frame(double now);
	// Everything put back (only where it's still ours) and forgotten: a later update starts afresh
	void stop();
	// For /hifi status and the menu
	std::vector<std::string> status();

	// For the tests: what was found (0: not found) and our memory (0: none)
	struct sites
	{
		uintptr_t world = 0, entity = 0, memory = 0, culling = 0, scale = 0, vis = 0, frustum = 0, game = 0;
		uintptr_t lod[4] = {};
	};
	sites where();
}
