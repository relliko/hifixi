#pragma once

// The model actors' data on the GPU (frame.hpp actor_frame; src/game/modelactor): each DAT a model actor's mesh comes
// from (a ship's, a piece of furniture's, a plant's) read on a thread of its own (zonemesh::load_file, one at a time) the
// first time one of its models is drawn, then put on the GPU on a copy queue of HiFi XI's own (gpu_zone, as the zone's).
// Until then the game draws that actor itself. A DAT no model actor has drawn from for kUnusedPresents is let go
// (kRetirePresents later: the GPU may still read it).
//
// Everything here is called on the game's thread (Ashita's present, the actor walk).

#include <cstdint>
#include <string>
#include <vector>

struct ID3D12Device;

namespace hifi::zone
{
	class gpu_zone;
}

namespace hifi::zone::actordata
{
	// Each present: loads finished uploaded, the next started, DATs unused lately let go; all let go while not wanted
	void frame(ID3D12Device *device, bool wanted);
	// A DAT's data once it's on the GPU (the file id: its root resource's +0x34), else nullptr: it's asked for then (read
	// at a later present), and stays nullptr for one that couldn't be read
	gpu_zone *find(uint32_t file);
	std::vector<std::string> status();
	// Everything let go (waits for a load in flight)
	void stop();
}
