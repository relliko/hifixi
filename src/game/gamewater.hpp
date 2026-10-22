#pragma once

// The game's own water hidden where World FX draws its own (the worldfx addon's /worldfx gamewater, in C++): the
// effect-mesh draw's call (FFXiMain 040DC4) goes to a routine of ours that skips the zone's water meshes (by name,
// from tools/game_water.py's list), and the wading ripples at a character's feet go too (the step's effect call
// 0D5365 to a second routine, 0D64FA's je to jmp). The routines' bytes are the addon's, byte for byte.

#include "game.hpp"
#include <string>

namespace hifi::game::gamewater
{
	// The draw's call to the routine (hide) or back to the game's (the HideGameWater setting); false and why when it
	// can't be (found, as expected, written)
	bool route(bool hide, std::string &why);
	// Every half second: the routine's list for the zone (filled where World FX draws water there and its Water setting
	// is on, else empty: everything drawn), the wading ripples hidden there and shown elsewhere
	void update(uint32_t zone);
	// Each present (for the layers skipped a frame)
	void count_frame();
	// Everything put back (the call, the footstep code), its memory freed when nothing points into it any more, and
	// everything forgotten (a route after starts afresh); false and why when something else changed the game's code
	// since (left as it is then, its memory kept)
	bool stop(std::string &why);
	std::string status();
	std::string seen();

	// For the tests: the routines' memory (0 before the first route), the call's site and the game's draw it went to,
	// the step's site and its spawn, the ring jump's site
	struct sites
	{
		uintptr_t memory = 0, call = 0, draw = 0, step = 0, spawn = 0, ring = 0;
	};
	sites where();
}
