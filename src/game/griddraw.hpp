#pragma once

// The Mog House layout grid handed to HiFi XI (research/ffxi-re/notes-world-passes.md 6; PLAN-own-frame.md, the world
// part).
//
// In layout mode (CAtelIdleTask, priority 13 -> 1D4D90 -> 1D5DD0), 1D5DD0 sets the grid's states (1779C0: WORLD
// identity, no texture, no lighting, colour = the vertex's) and calls 1CF110 at 1D5DE0, which draws each cell and the
// item being placed through 1CF410 / 1CF510 -> 177B40 (cdecl: the four corners, R, G, B, A, keep-depth): one 5-point
// line strip, ZENABLE off around it unless the last argument is set. Through ours:
//   - 1D5DE0's call begins a frame of the grid ring (world/frames.hpp), and after 1CF110 publishes it and sends the
//     grid's end marker (worldmark::kGrid, the spare kind 12);
//   - the two calls of 177B40 (1CF4BD, 1CF607) record each outline and, with native drawing on, don't draw it.
// The transforms are the device's when the grid draws (the wrapper's cache: VIEW x PROJECTION, WORLD identity).
//
// The game's thread only.

#include "game.hpp"
#include <string>

namespace hifi::game::griddraw
{
	bool start(std::string &why);
	bool stop(std::string &why);
	bool in();
	void set_native(bool on);

	uint32_t __fastcall on_grid(uintptr_t grid, uintptr_t edx, uint32_t arg);
	void __cdecl on_outline(const float *corners, uint32_t r, uint32_t g, uint32_t b, uint32_t a, uint32_t keep_z);

	struct counts
	{
		uint64_t frames = 0;   // grid draws (layout mode's frames)
		uint64_t outlines = 0; // recorded for HiFi XI
		uint64_t game = 0;     // drawn by the game
		uint32_t seq = 0, last = 0;
	};
	counts get_counts();
	std::string status();

	struct sites
	{
		uintptr_t grid_call = 0, grid = 0;         // 1D5DE0, 1CF110
		uintptr_t outline_calls[2] = {}, outline = 0; // 1CF4BD, 1CF607; 177B40
		uintptr_t wrapper = 0;                     // the wrapper's global (its transform cache)
	};
	sites where();
	void set_callees(uintptr_t grid, uintptr_t outline);
}
