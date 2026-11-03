#pragma once

// The texture pack's glue for the characters (docs/texture-pack/characters.md): the second consumer the "textures"
// module calls (src/pack/pack_module.hpp, pack::consumer; main.cpp adds it after the zones'). Everything here runs on
// the game's thread (the module's update and present), with no lock of ours held across a streamer or device call: the
// cache (src/chars/cache.hpp) keeps the keys, tickets, pairs and states under its own mutex and makes no streamer call.
//
//   - set_mode: chars on, the characters ask from the next frame on; off, every pack part let go at once (the game's
//     textures handed out again from that call on). Characters switch at once both ways, unlike the zones.
//   - Each present while the pack is in use (frame), and only while the cache's device is the module's (the cache
//     never follows a device change: decision 14): one owner, "characters"; the keyed textures on the GPU asked for, at
//     most kRequestsPerPresent a present, none while the streamer's pressure() says the card is nearly full or while
//     kPackSlotFloor pairs or fewer are free, each down to the game's own smallest level, at kPriority. Then the owner's
//     tickets that changed (take_done): a ready one's pair taken (cache::pack_begin), its two views written with the
//     cache's mutex left (stream::write_view, cache::write_pack_detail), then marked ready (cache::pack_end); a missed
//     or gone one let go, the texture spent (counted by why). Read before backend::present's retire::on_present: a
//     ticket keeps its texture until it's dropped (zones.md decision 1), and the drop and the pair go behind the same
//     fence (characters.md "When tickets are read"). While the streamer's pressure() holds (over 95% of the video
//     memory), each half second the textures a model drew longest ago go back to the game's (cache::pack_evict, through
//     the retire list like any let go), held so till it's been gone the hold (kEvictHoldS, doubled while it keeps coming
//     back just after), then asked again a round each half second (characters.md "Eviction under pressure").
//   - clear_all (the module, before every close, open, reload, stop, device change and loss): every pack part let go
//     (cache::forget_pack_textures: tickets to the drop hook, pairs behind the GPU), the owner dropped. The game's
//     textures stay on the GPU: the characters keep being drawn by HiFi XI with them.

#include "../pack/pack_module.hpp"
#include <d3d12.h>
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::chars::charpack
{
	constexpr uint32_t kRequestsPerPresent = 64;
	// (after the zone's own textures, 2,000,000 and up, and before its second map's, 1,000,000, and the model actors', 0;
	// among the characters' in the order asked)
	constexpr int32_t kPriority = 1500000;
	// Under the streamer's pressure (plan section 5): a round each kEvictEveryS, the least recently drawn ready textures
	// back to the game's up to kEvictBytes of view bytes; asked again once the pressure has been gone the hold
	// (kEvictHoldS), kEvictBytes a round, the ones drawn last first. A pressure back within kBackoffWindowS of a round
	// asked again doubles the hold, up to kEvictHoldMaxS; it's kEvictHoldS again once all are back and it stayed away so long
	constexpr double kEvictEveryS = 0.5, kEvictHoldS = 10.0, kEvictHoldMaxS = 160.0, kBackoffWindowS = 30.0;
	constexpr uint64_t kEvictBytes = 64ull << 20;

	// The module's consumer (pack::add_consumer, main.cpp: after the zones')
	pack::consumer consumer();
	void clear_all();
	void set_mode(const pack::modes &m);
	void frame(double now, uint32_t epoch);
	void set_compare(bool on);
	// (update, every half second) The characters' line for the menu and the status
	void lines(std::vector<std::string> &out);

	// What the glue reads from outside (main.cpp: the module's device_in_use and overlay_listed, backend::log; the
	// tests their own). No device source: nothing is ever asked
	struct sources
	{
		ID3D12Device *(*device)() = nullptr;          // (the device the pack is in use on)
		bool (*overlay_listed)() = nullptr;           // (pivot.ini lists hifi-textures: every key misses)
		uint32_t (*game_drawn)() = nullptr;           // (models the game drew itself lately, a frame's worth; none: not said)
		void (*log)(int level, const char *line) = nullptr;
	};
	void set_sources(const sources &s);

	struct counts
	{
		uint64_t asked = 0, views = 0;                     // (requests made; pack views written)
		uint64_t not_in_pack = 0, failed = 0, gone = 0;    // (tickets missed as not in it or no_pack, missed otherwise, gone)
		uint64_t refused = 0;                              // (requests that gave no ticket)
		uint64_t evicted = 0, evict_rounds = 0;            // (textures back to the game's under the pressure; rounds that did)
		double hold_s = kEvictHoldS;                       // (the hold now: how long the pressure must be gone before they're asked again)
		uint32_t last_requests = 0, last_views = 0;        // (the last frame's)
		bool other_device = false;                         // (the cache's device isn't the module's: nothing asked)
		bool owner = false;                                // (an owner held now)
	};
	counts get_counts();

	namespace test
	{
		// (tests) The view bytes an eviction round lets go in place of kEvictBytes (0: it), so a test's small textures
		// are evicted a few at a time
		void set_evict_bytes(uint64_t bytes);
	}
}
