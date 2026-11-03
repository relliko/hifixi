#pragma once

// The texture pack's module ("textures", src/pack/pack_module.cpp): the pack found, opened and closed on the
// streamer's worker as [textures] pack says, the streamer's limits, pause bits and pacing, its log lines, the status
// and the menu's lines. docs/texture-pack/zones.md "The module's lifecycle" is the design.
//
// What the menu and the settings read (the game's thread, while the menu is drawn): answers the module cached in its
// update. None of them reads a file, asks DXGI or calls the streamer.
//
// What the owners read (the zone's and the model actors' glue, src/zone/zonepack: the game's thread) and what they
// hand the module (a consumer): the module drops every owner before it closes, opens or stops (decision 2), and calls
// their frame each present while the pack is in use, after the streamer's on_present.

#include <cstdint>
#include <string>
#include <vector>

namespace hifi::pack
{
	// ---- The menu and the settings (cached answers: no file, DXGI or streamer call)

	// A pack folder on this PC (<bootloader>\hifixi-pack\pack.idx there): looked for at the module's first update and at
	// each /hifi textures on and reload. The setting and the menu's lines are shown only then
	bool folder_exists();
	// The menu's lines under "Use the texture pack" (what was found, this area, memory, pauses, how to remove it)
	std::vector<std::string> menu_lines();
	// The pack's folder for the menu's Files list ("" before the module's first update)
	std::string folder_text();

	// ---- The owners (the game's thread)

	// The pack open and in use: owners may ask for its textures, in epoch_in_use() (stream::want::epoch). false while
	// off, opening, failed, after a device loss, and before the first open
	bool in_use();
	uint32_t epoch_in_use(); // (0 while not in use)
	// /hifi textures compare: the game's own textures drawn where the pack's are ready (nothing freed; not saved)
	bool comparing();
	// The quiet window: from a zone change until the character has been in for [textures] zone_delay_s and the old
	// zone is let go (or there's video memory to spare). The streamer is paused meanwhile (kQuiet): requests wait
	bool quiet();
	// Under FSR, the pack's textures sampled with more detail ([textures] fsr_bias, while in use); the bias for a ratio
	// of the world's drawn width to the screen's: log2(ratio) clamped to [-1, 0] (with the zone sampler's own -0.5, at
	// least -1.5 in all; 0 when ratio >= 1 or unknown)
	bool fsr_bias_on();
	float fsr_bias(float ratio);

	// What the owners give the module (any of them nullptr: nothing to call)
	struct consumer
	{
		// Every owner's pack textures let go (drop_owner), every slot of this visit spent, flags cleared: before every
		// close, open (on, reload) and stop, in the same call (zones.md decision 2); also at a device change and loss
		void (*clear_all)() = nullptr;
		// The loaders' pack mode for loads starting from now: the zone and its second map ([textures] zone), the model
		// actors ([textures] actors); both false while the pack isn't in use
		void (*set_mode)(bool zones, bool actors) = nullptr;
		// Each present while the pack is in use, after stream::on_present: requests (in `epoch`, epoch_in_use()'s),
		// take_done, write_view
		void (*frame)(double now, uint32_t epoch) = nullptr;
		// /hifi textures compare turned on or off (also called with false when the pack is turned off)
		void (*set_compare)(bool on) = nullptr;
		// (update, every half second) Lines for the menu and the status: this area's textures from the pack
		void (*lines)(std::vector<std::string> &out) = nullptr;
	};
	void set_consumer(const consumer &c);

	// ---- The quiet window as a function of its inputs (the module feeds it each present; tests drive it)
	struct quiet_inputs
	{
		double now = 0.0;
		double in_since = -1.0;      // (when the character came in to this zone: feed::in(); -1 not in)
		double delay = 2.0;          // ([textures] zone_delay_s)
		uint32_t ready_presents = 0; // (presents native's slot 0 has shown its gpu_zone ready since the zone changed)
		bool zone_wanted = true;     // (a zone to draw: feed::zone() not 0)
		uint64_t vram_usage = 0, vram_budget = 0; // (0: unknown, headroom)
	};
	// Quiet until the character has been in for `delay`, and then until the old zone is retired (native hands it to the
	// retire list as the new one goes up, so 3 presents of the new one ready put it past the +2 rule), no zone is wanted,
	// or the video memory is under 85% of the budget
	constexpr uint32_t kQuietPresents = 3, kQuietPercent = 85;
	bool quiet_window(const quiet_inputs &q);

	namespace test
	{
		// (tests) Every file, DXGI and streamer call the module made so far: unchanged across a menu draw
		uint64_t io_calls();
		// (tests) How many times the module started the streamer (once a session; again after its stop)
		uint32_t stream_starts();
		// (tests) The quiet window forced: -1 as the inputs say, 0 never quiet, 1 always
		void force_quiet(int v);
	}
}
