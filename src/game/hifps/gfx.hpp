#pragma once

// hifps's graphics settings (hifps.lua's /hifps gfx: gfx_*, lamps_*, night_*), in C++: stock by default (1.0), applied
// while hifps's frame step is on (hifps::on(), step.hpp), put back when it goes off and on stop().
//   lamps/reach  Lamp light at night. gfx.asm's stub (stubs.inc) sits on the three ways a lamp reaches the picture:
//                the zone shader's lamp constants (17D3AF), the fixed-function lamp lights 1-4 (17CC26, 17CCC2,
//                17CD5E, 17CDF9) and a character's lamp-lit light (0CB872). gfx_lamps scales the colour, gfx_reach
//                the range (the shader lights fully up to the range and not past it, so the pools grow with the same
//                edge; characters keep the stock reach).
//   moon/fill    Night lighting: the moonlight's colour and the ambient light that fills shadows, scaled where the
//                client builds them (188CA0, 188E00), so the zone, characters and effects all follow; full from 20:00
//                to 4:00 Vana'diel time, stock by day.

#include "../game.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::game::hifps::gfx
{
	// Every half second (after hifps::update()): follows hifps::on() and the options ([hifps] gfx_lamps, gfx_reach,
	// gfx_moon, gfx_fill)
	void update();
	// Every present (now: seconds): once a second the night factors (the Vana'diel hour) and the lamp counters (their
	// rate a frame from hifps::fps())
	void present(double now);
	// Everything put back (only over our own bytes), the memory freed when nothing points into it, all forgotten
	void stop();
	// For /hifi status and the menu
	std::vector<std::string> status();

	// As the menu shows it: lamps and night lighting changed now, why not (empty when nothing's wrong), what they're
	// doing (gfx_stat, night_stat: empty when nothing to say)
	struct view
	{
		bool lamps = false, night = false;
		std::string lamps_err, night_err, lamps_stat, night_stat;
	};
	view state();

	// For the tests: the Vana'diel clock called instead of the client's (nullptr: the client's), our memory (0 while
	// none) and the sites changed
	void set_clock(uint32_t(__cdecl *clock)());
	struct sites
	{
		uintptr_t memory = 0;
		std::vector<uintptr_t> lamps, night; // the calls and the builders' entries while changed
	};
	sites where();
}
