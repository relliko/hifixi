#pragma once

// The native effect renderer on dgVoodoo's Direct3D 12 lists (as the characters', src/chars/charinject.hpp): at each
// fx_run marker (slotmark kind 5, src/host/passes.hpp hands it here) the run of the flush it names is drawn into the
// world's target and depth, the effects' new textures copied in first. Then dgVoodoo's own state is bound again: its
// descriptor heaps, then its targets, viewports, scissors, pipeline, topology and root arguments (grass::list_state).
// The world's depth planes are put back as the zone renderer found them.
//
// Everything but prepare/release/ready/status runs on dgVoodoo's recording thread, inside the hooks.

#include <d3d12.h>
#include "../host/passes.hpp"
#include "../render/grass_render.hpp" // (grass::list_state)
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::fx::inject
{
	// The game's thread: the renderer made for a device (nullptr: let go); false and why when it can't be
	bool prepare(ID3D12Device *device, std::string &why);
	void release();
	// Made, and nothing failed in the last two seconds
	bool ready();
	// fx_run markers handled since the start (the game side's proof that its markers come through)
	uint64_t markers_seen();
	void on_run(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const passes::world &w, uint32_t seq_low4);

	struct counts
	{
		uint64_t markers = 0, runs = 0, elements = 0, draws = 0, triangles = 0, failed = 0, textures = 0;
		uint64_t no_frame = 0; // a marker whose flush wasn't in the ring (any more)
	};
	counts get_counts();
	std::vector<std::string> status();
}
