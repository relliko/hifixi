#pragma once

// The texture pack's glue for the zone, its second map and the model actors (docs/texture-pack/zones.md): the consumer
// the "textures" module calls (src/pack/pack_module.hpp, pack::consumer; main.cpp adds it first). Everything here runs
// on the game's thread: the module's update and present, the gpu_zones' destruction (the drop hook).
//
//   - The loaders' pack mode (set_mode): native::set_pack_mode, actordata::set_pack_mode. A gpu_zone read in pack mode
//     from the game's own file has a pack region (gpu_zone::has_pack).
//   - Each present while the pack is in use (frame): every such gpu_zone that is ready gets an owner, then its keyed
//     textures are asked for, the most used first, then the nearest the last zone pass's camera (priorities: the zone's
//     over its second map's over the model actors'), at most kRequestsPerPresent a present in all, each down to the
//     game's own size (stream::want::down_to). Then each owner's tickets that changed (take_done): a ready one's view
//     written into the zone's pack slot (stream::write_view), then its word set drawn; a missed or gone one dropped
//     and its word spent. Read before backend::present's retire::on_present: a ticket keeps its texture until it's
//     dropped, gone or not (zones.md decision 1), so the view stays valid while the word says drawn.
//   - (M9) While heights are on (set_mode's heights, [textures] height), after the colours: each keyed texture whose entry
//     has a height record in the pack (stream::current) asked for that record, its whole chain, under the same cap; a
//     ready one's view written into the zone's height block with its relief scale (gpu_zone::height_written), then its
//     height word set drawn. Asked and settled apart from the colour, so an entry with no colour still gets its relief.
//   - A gpu_zone let go (gpu_zone::pack_let_go before retire::later, or its destructor) runs the drop hook: its owner
//     dropped (stream::drop_owner: no wait, it may go right after), every word spent, out of the list; its visit done.
//   - clear_all (the module, before every close, open, stop, a device change or loss): every owner dropped, every word
//     spent, the zones' visits done: the pack's textures come again only for the next zone read in pack mode.
//
// No lock: nothing here is called off the game's thread. The streamer's calls take its own leaf mutex only.

#include "../pack/pack_module.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::zone
{
	class gpu_zone;
}

namespace hifi::zone::zonepack
{
	constexpr uint32_t kRequestsPerPresent = 128;
	constexpr int32_t kZonePriority = 2000000, kSecondPriority = 1000000, kActorPriority = 0;
	// (a zone's requests wait at most this long for a zone pass of its own to order them by distance; then by use only)
	constexpr double kCameraWaitS = 2.0;

	// The module's consumer (pack::add_consumer, main.cpp: the first; its set_mode takes the zones' and actors' modes)
	pack::consumer consumer();
	void clear_all();
	// (heights: M9's [textures] height: the pack's height records asked and drawn as relief, gpu_zone::set_heights)
	void set_mode(bool zones, bool actors, bool heights = true);
	void frame(double now, uint32_t epoch);
	void set_compare(bool on);
	void lines(std::vector<std::string> &out);

	// The last zone pass's camera and its zone's map (main.cpp: game::zonedraw::camera; the tests their own; none: by
	// use only)
	using camera_fn = bool (*)(float out[3], uint32_t &map);
	void set_camera_source(camera_fn fn);
	// Where its log lines go (main.cpp: backend::log; none: nowhere): each zone's coverage once its requests settle
	using log_fn = void (*)(int level, const char *line);
	void set_log(log_fn fn);

	struct counts
	{
		uint32_t zones = 0;                  // (gpu_zones with an owner now)
		uint64_t asked = 0, drawn = 0;       // (requests made; views written and drawn)
		uint64_t not_in_pack = 0, failed = 0, gone = 0; // (spent: not in it or no_pack, a miss of another kind, gone)
		uint64_t dropped = 0;                // (owners dropped: the hook and clear_all)
		uint32_t most_in_a_present = 0;      // (requests in one present, at most)
		uint32_t last_requests = 0, last_views = 0; // (the last present's)
		uint64_t heights_asked = 0, heights_drawn = 0, heights_failed = 0; // (M9: of those, the height records')
	};
	counts get_counts();
	// (tests) The order a zone's textures are asked in (empty until it has been ordered)
	std::vector<uint32_t> order_of(const gpu_zone *z);
	// (tests) The requests a present in place of kRequestsPerPresent (0: it): a test's zones have fewer keyed textures
	// than that, so it binds only below it
	void set_request_cap(uint32_t n);
}
