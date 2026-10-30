#pragma once

// The game's sky dome handed to HiFi XI (research/ffxi-re/client-notes.md "Sky"; notes-world-passes.md 2;
// PLAN-own-frame.md, the world part).
//
// The environment draw's sky (03DB60, frame loop step 5, before the zone) sets the frame's clear colour, the dome's
// state block and transforms, then calls 03D640 (radius, 8, segments, the 8 colours, the 8 heights) at 03DD00, which
// fills a vertex buffer strip by strip and draws each (043F40), skipping the first 21 strips of a session (its counter
// [VA 04AECB08]). That call goes to on_dome instead: with native drawing on, the dome is recorded into the sky ring
// (world/frames.hpp: 03D640's arguments, the device's WORLD x VIEW x PROJECTION from the wrapper's cache, the strips the
// game would have skipped, the counter moved on as the game would) and the game's 03D640 isn't called; else it is, as
// before. Either way the sky's end marker (slotmark::sky_end) follows, so the Direct3D 12 side draws the recorded dome
// right where the game's would have been. The clear colour and everything else of 03DB60 stay the game's.
//
// The game's thread only.

#include "game.hpp"
#include <string>

namespace hifi::game::skydraw
{
	// The call through on_dome, or back to 03D640; false and why (nothing changed then)
	bool start(std::string &why);
	bool stop(std::string &why);
	bool in();
	// Whether HiFi XI draws the dome (its Direct3D 12 side up, the sky's markers coming through)
	void set_native(bool on);

	// The call at 03DD00, as 03D640 takes it (cdecl)
	void __cdecl on_dome(float radius, uint32_t stops, uint32_t segments, const uint32_t *colours, const float *heights);

	struct counts
	{
		uint64_t calls = 0;   // domes the game asked for
		uint64_t native = 0;  // ... recorded for HiFi XI
		uint64_t game = 0;    // ... drawn by the game (native off, or arguments it doesn't know)
		uint32_t seq = 0;     // (the sky ring's: the marker carries its low 4 bits)
	};
	counts get_counts();
	std::string status();

	// For the tests: the call's site, the game's 03D640, the wrapper's global, the strip counter; the game's 03D640 the
	// call goes on to (start() sets it)
	struct sites
	{
		uintptr_t call = 0, original = 0, wrapper = 0, counter = 0;
	};
	sites where();
	void set_draw(uintptr_t draw);
}
