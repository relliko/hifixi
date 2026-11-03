#pragma once

// The model actors' data on the GPU (frame.hpp actor_frame; src/game/modelactor): each DAT a model actor's mesh comes
// from (a ship's, a piece of furniture's, a plant's) read on a thread of its own (zonemesh::load_file, one at a time) the
// first time one of its models is drawn, then put on the GPU on a copy queue of HiFi XI's own (gpu_zone, as the zone's).
// Until then the game draws that actor itself. A DAT no model actor has drawn from for kUnusedPresents is let go
// (once the GPU is past every list that may draw it: host/retire.hpp).
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
	// Each present: loads finished uploaded, the next started, DATs unused lately let go; all let go while not wanted.
	// zone: the zone now (feed::zone()): at a change, DATs read in the texture pack's other mode are let go too
	void frame(ID3D12Device *device, bool wanted, uint32_t zone = 0);
	// A DAT's data once it's on the GPU (the file id: its root resource's +0x34), else nullptr: it's asked for then (read
	// at a later present), and stays nullptr for one that couldn't be read
	gpu_zone *find(uint32_t file);
	// The DATs on the GPU now (the texture pack's glue: their textures asked for), appended
	void ready_files(std::vector<gpu_zone *> &out);
	// The texture pack's mode for loads starting from now (docs/texture-pack/zones.md decision 5): hifi-textures passed
	// over, the textures' pack keys made (zonemesh::load_file's pack). Off by default; the glue sets it while in use
	void set_pack_mode(bool on);
	std::vector<std::string> status();
	// Everything let go (waits for a load in flight)
	void stop();
}
