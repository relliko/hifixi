#pragma once

// hifps's experimental frame-time toggles (hifps.lua's /hifps exp: exp_set to exp_stat and what they use), in C++.
// None of them changes the game step; each puts back what it changed when turned off, when hifps's frame step goes
// off (hifps::on(), step.hpp: nothing of this while it's off, as the addon's "Enable hifps first") and on stop().
//   ashita     Ashita's per-draw plugin dispatch skipped (its four draw methods' je to jmp), refused while a loaded
//              addon or plugin uses the draw events unless ashita_force; turned off again by a newcomer that does
//   occlusion  the characters' behind-a-wall readback every 4m-th frame above 60fps; every frame while World FX's
//              VisTest answers it (set_actors_elsewhere)
//   batch      consecutive text and UI quads drawn as one draw, in HiFi XI's d3d8.dll (HifiFrameSetSpeedups)
//   dedupe     device calls that set a value already in effect dropped, in the same d3d8.dll
// The options are [hifps] ashita_force and exp_<name>, all off by default and out of the menu since 0.66 (with the
// zone and characters drawn by HiFi XI the game sends ~115 draws a frame: nothing for them to save; a settings file
// can still turn one on). docs/frame-rate.md has what each saves, measured, and why hifps's
// walls, sse and its asm device proxy aren't here any more.

#include "../game.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::game::hifps::exp
{
	// IAshitaCore* from the plugin (the plugin count and Ashita's folder for the 'ashita' check); may be nullptr
	void set_ashita(void *ashita_core);
	// Every half second (after hifps::update()): follows hifps::on() and the options, turning toggles on and off (the
	// saved ones once the client's device exists, as hifps's restore_exp)
	void update();
	// Every present (now: seconds): once a second the occlusion rate (from hifps::fps()), the counters, the 'ashita'
	// watch (exp_tick)
	void present(double now);
	// The characters' visibility test answered elsewhere (World FX's VisTest, from its depth copy: src/game/feed.cpp):
	// occlusion has every character tested every frame then (an answer costs nothing there) instead of every 4m-th
	void set_actors_elsewhere(bool on);
	// Everything put back (only over our own bytes), the memory freed when nothing points into it, all forgotten
	void stop();
	// A tap on the game's draws (the stream log's, till it went with the Direct3D 12 hooks; unused now): the client device's DrawPrimitive,
	// DrawIndexedPrimitive, DrawPrimitiveUP and DrawIndexedPrimitiveUP (slots 70-73) pointed at taps[0..3], each
	// calling on to originals[i] (filled here), in a copy of the device's vtable put in front of it. On the game's
	// thread only (a present); false and why when it can't. draw_untap puts the device's own vtable back.
	bool draw_tap(const void *const taps[4], void *originals[4], std::string &why);
	void draw_untap();
	// For /hifi status and the menu: a line a toggle (as /hifps exp lists them), then why not and notes
	std::vector<std::string> status();
	// One line for the log and the crash and hang reports: which are on and off, and why one isn't
	std::string state_line();

	// One toggle as the menu shows it: on, what it's doing (exp_stat, empty when nothing to say), why it isn't on
	// (exp.err, empty when none). name: ashita, occlusion, batch, dedupe
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

	// What the plugin reads through Ashita's interfaces and finds in the game's d3d8.dll, given instead (the tests);
	// nullptr: Ashita's and the d3d8.dll's again
	struct host
	{
		// The loaded addons other than hifps (false: not known; a plugin can't list them)
		bool (*addons)(std::vector<std::string> &names) = nullptr;
		std::string install;       // Ashita's folder, with a slash at the end (the addons' files are in addons\<name>)
		int plugins = -1;          // plugins loaded (-1: not known)
		uintptr_t ashita_base = 0; // Ashita.dll's image
		size_t ashita_size = 0;
		// HiFi XI's d3d8.dll's HifiFrameSetSpeedups and HifiFrameSpeedupStats (src/host/frameapi.hpp; nullptr: a d3d8.dll
		// that isn't ours)
		uint32_t(__cdecl *set_speedups)(uint32_t flags) = nullptr;
		void(__cdecl *speedup_stats)(uint32_t out[4]) = nullptr;
	};
	void set_host(const host *h);

	// For the tests: the sites patched and what the d3d8.dll was told
	struct sites
	{
		uintptr_t occlusion = 0;                   // the occlusion test's 12 bytes
		std::vector<uintptr_t> ashita;             // the jumps written in Ashita.dll
		uint32_t speedups = 0;                     // the d3d8.dll's speed-ups as last set (kHifiDedupe, kHifiBatch)
	};
	sites where();
}
