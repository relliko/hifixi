#pragma once

// The hifps Ashita addon's frame step (hifps.lua 0.9.9), in HiFi XI: the game runs above 60 fps with its world kept at
// its own speed.
//
// The client advances timers and animation by a per-frame "step" in 1/60 s ticks, read through a getter that returns
// max(step, 1.0). The step normally comes from the fps divisor, so anything above 60 fps is clamped to 1 tick a frame
// and the game runs fast. Both copies of that getter are replaced: the first returns the real frame time in ticks
// (fractional), the second whole ticks accumulated from real time (0, 1, 2...). Every call site of the getters is
// pointed at one of them: sites that truncate the step to an integer get whole ticks, the rest the fractional step. The
// divisor is set to 0 (uncapped) and the limiter caps the frame rate. Fixed-step smoothing loops (the camera's, other
// characters' movement and turning) run once a frame with their factors rescaled to the frame's step; constants meant
// per 1/60 s (the walk animation's, the moving flag's) are scaled to it; the target's name pulse counts whole ticks.
// The sites are found again after a client update (sites.hpp). Everything is put back over our own bytes only.
//
// Everything here runs on the game's thread (Ashita's present callback), as the addon did.

#include "../game.hpp"
#include "sites.hpp"
#include <string>
#include <vector>

namespace hifi::game::hifps
{
	// Every half second: follows the options ([hifps] enabled, limit): turned on (the sites resolved once) or off
	void update();
	// Every present (now: seconds, steady): the limiter's wait, the step fed, the frame rate measured
	void present(double now);
	// Everything put back (only over our own bytes) and forgotten
	void stop();
	// What it's doing, for /hifi status and the menu
	std::vector<std::string> status();
	// Frames a second, measured over each second (on or off), for the counter
	double fps();
	// The frame step is unlocked now
	bool on();

	// For the tests: what's patched now (all 0 and empty while off)
	struct patched
	{
		uintptr_t memory = 0;                // ours: +0 the fractional step, +4 whole ticks, +16 the 1.0 stub...
		uintptr_t getters[2] = {};           // the fractional and the whole-tick getter
		uintptr_t divisor = 0;               // the fps divisor's address, and what it was
		uint32_t divisor_was = 0;
		uintptr_t pulse = 0;                 // the name pulse's call (0: left as it is)
		struct site
		{
			uintptr_t at;
			char kind; // 'f' fractional, 'i' whole ticks, 's' the 1.0 stub
			uintptr_t target;
		};
		std::vector<site> sites;
		std::vector<uintptr_t> immediates;   // the smoothing loops' immediate floats
		struct operand
		{
			uintptr_t at, constant, slot;
			char mode; // 'e' ease, 'l' lin, 'p' pow
		};
		std::vector<operand> operands;
		double step = 1.0;                   // the last frame's
		uint32_t moved = 0, missing = 0;     // the resolver's: offsets that moved, sites not found
		double timer = 0.0, spin = 0.0;      // the last limited frame's wait: on the timer, spinning (seconds)
		bool high_resolution = false;        // the limiter's timer is a high-resolution one
	};
	patched where();
	// The resolver's result (nullptr before it ran)
	const sites::found *resolved();
}
