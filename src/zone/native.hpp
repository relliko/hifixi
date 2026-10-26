#pragma once

// The native zone renderer's per-zone data in the game: the zone's meshes, textures and normal maps read from the
// player's own DATs on a thread of its own when a zone (or its second map) is entered (src/bake/zonemesh), then put on the GPU on a copy
// queue of HiFi XI's own (zone_gpu). Nothing draws with them yet: that's the renderer to come. Off unless
// [zone] data is on (/hifi zonedata on|off).
//
// Everything here is called on the game's thread (Ashita's present and commands).

// (no d3d12.h: the plugin includes Ashita's Direct3D 8 headers beside this)
#include <cstdint>
#include <string>
#include <vector>

struct ID3D12Device;

namespace hifi::zone
{
	class gpu_zone;
}

namespace hifi::zone::native
{
	// Each present: the zone's data loaded and uploaded while wanted, for `zone` (0: none known, nothing loaded), and
	// the second map's (the renderer's slot 1: a building's interior; 0 none); a map's last data let go a while after
	// the next is in (the GPU may still read it)
	void frame(ID3D12Device *device, uint32_t zone, uint32_t second, bool wanted);
	// Slot i's map on the GPU once its copies are done (0 the zone, 1 the second map), when it's the one the last
	// present asked for (nullptr until then, while the next is read, and while not wanted)
	gpu_zone *ready_zone(uint32_t i = 0);
	std::vector<std::string> status();
	// Everything let go (waits for a load in flight)
	void stop();
}
