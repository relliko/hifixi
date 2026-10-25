#pragma once

// hifps's experimental frame-time toggles (hifps.lua's /hifps exp: exp_set to exp_stat and what they use), in C++.
// None of them changes the game step; each puts back what it changed when turned off, when hifps's frame step goes
// off (hifps::on(), step.hpp: nothing of this while it's off, as the addon's "Enable hifps first") and on stop().
//   ashita     Ashita's per-draw plugin dispatch skipped (its four draw methods' je to jmp), refused while a loaded
//              addon or plugin uses the draw events unless ashita_force; turned off again by a newcomer that does
//   occlusion  the characters' behind-a-wall readback every 4m-th frame above 60fps
//   walls      that test answered from the zone's collision (walls.asm) instead of the GPU readback
//   sse        the client's SSE skinning on non-Intel CPUs
//   batch      consecutive text and UI quads merged into one draw (proxy.asm, the client device's vtable)
//   dedupe     device calls that set a value already in effect dropped (the same proxy)
// The options are [hifps] ashita_force (off by default) and exp_<name> (all on by default: the frame rate's options
// are on in every preset).

#include "../game.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::game::hifps::exp
{
	// IAshitaCore* from the plugin (the player's and the target's actors for walls; the plugin count and Ashita's
	// folder for the 'ashita' check); may be nullptr
	void set_ashita(void *ashita_core);
	// Every half second (after hifps::update()): follows hifps::on() and the options, turning toggles on and off (the
	// saved ones once the client's device exists, as hifps's restore_exp)
	void update();
	// Every present (now: seconds): walls' kept actors each frame; once a second the occlusion rate (from
	// hifps::fps()), the counters, the 'ashita' watch (exp_tick)
	void present(double now);
	// Everything put back (only over our own bytes), the memory freed when nothing points into it, all forgotten
	void stop();
	// For /hifi status and the menu: a line a toggle (as /hifps exp lists them), then why not and notes
	std::vector<std::string> status();

	// One toggle as the menu shows it: on, what it's doing (exp_stat, empty when nothing to say), why it isn't on
	// (exp.err, empty when none). name: ashita, occlusion, walls, sse, batch, dedupe
	struct toggle_state
	{
		bool on = false;
		std::string stat, err;
	};
	toggle_state toggle(const char *name);

	// The 'ashita' check's last answer: what would lose the draw events ('addon x (...)', 'plugin y (...)'), what
	// was checked ('n addons, m plugins'; empty before the first check), what couldn't be; forced on
	struct ashita_view
	{
		std::vector<std::string> users, notes;
		std::string checked;
		bool force = false;
	};
	ashita_view ashita_state();
	// /hifps exp ashita check: what uses the draw events now, changing nothing (lines to show)
	std::vector<std::string> ashita_check();

	// What the plugin reads through Ashita's interfaces, given instead (the tests); nullptr: Ashita's again
	struct host
	{
		// The loaded addons other than hifps (false: not known; a plugin can't list them)
		bool (*addons)(std::vector<std::string> &names) = nullptr;
		std::string install;       // Ashita's folder, with a slash at the end (the addons' files are in addons\<name>)
		int plugins = -1;          // plugins loaded (-1: not known)
		uintptr_t ashita_base = 0; // Ashita.dll's image
		size_t ashita_size = 0;
		// The player's actor, the target's (0: none), the player's status (false: not known: all 0, the stock test off)
		bool (*actors)(uint32_t &me, uint32_t &target, uint32_t &status) = nullptr;
	};
	void set_host(const host *h);

	// For the tests: our memory (0 while none) and the sites found
	struct sites
	{
		uintptr_t walls = 0, proxy = 0;            // the walls and proxy stubs' copies
		uintptr_t walls_site = 0, occlusion = 0;   // the actor draw's call, the occlusion test's 12 bytes
		uintptr_t sse = 0;                         // the CPU feature flag set (0: none, or already on)
		uintptr_t device = 0, vtable = 0;          // the client device while the proxy is on, its own vtable
		std::vector<uintptr_t> ashita;             // the jumps written in Ashita.dll
	};
	sites where();
}
