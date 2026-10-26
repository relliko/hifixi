#pragma once

// The characters' meshes and textures on the GPU, for the native character renderer: each Os2 mesh and texture chunk
// the game has loaded, taken once from the game's own memory the first time a drawn model names it (src/game/charmodel:
// the bytes copied on the game's thread), decoded on a thread of ours (os2, tex) and uploaded on a copy queue of HiFi
// XI's own, never on dgVoodoo's lists (as the zone's data: resources made in COMMON, decaying back to COMMON once the
// copy is done, promoted on first use by whichever queue reads them).
//
//   a mesh       its vertices (os2::vertex, 68 bytes) and its corners (os2::corner, 12 bytes) as two buffers with a
//                structured-buffer view each; its groups kept on the CPU with the cache's textures they use
//   a texture    R8G8B8A8_UNORM with the game's mips (tex::make_mips), its alpha as the game keeps it (0x80 = opaque)
//   descriptors  one shader-visible heap of ours (kDescriptors): a mesh's two views side by side, a texture's one
//   palettes     an upload buffer of kFrames slots, each frame's bones (charframe.hpp: 48 bytes a bone) copied into the
//                frame's slot as models are published
//
// Entries are keyed by the game's resource and checked against its handle, size and data pointer each frame (a freed
// resource's memory can be reused for another); an entry not named for kForgetSeconds is let go.

#include <d3d12.h>
#include "os2.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::chars
{
	struct frame;
}

namespace hifi::chars::cache
{
	constexpr uint32_t kNone = 0xFFFFFFFFu;
	constexpr uint32_t kDescriptors = 16384;
	constexpr double kForgetSeconds = 20.0;

	// What tells a game resource from another one at the same address
	struct key
	{
		uintptr_t resource = 0;
		uint32_t handle = 0, size = 0, data = 0;
		bool operator==(const key &o) const { return resource == o.resource && handle == o.handle && size == o.size && data == o.data; }
	};

	// The game's thread. A mesh or texture already known (seen now, so it's kept), else kNone.
	uint32_t mesh(const key &k);
	uint32_t texture(const key &k);
	// A new one, with its bytes (the chunk's data) to decode: its id. A mesh's texture ids, one per texture command in
	// command order (kNone where none was found); live_base: the data's address in the game (its section pointers).
	uint32_t add_mesh(const key &k, std::vector<uint8_t> &&data, uint32_t live_base, std::vector<uint32_t> &&textures);
	uint32_t add_texture(const key &k, std::vector<uint8_t> &&data);
	// A mesh's header byte 3 (the hide groups it switches on), known when it's added (0 for an unknown id)
	uint8_t mesh_hides(uint32_t id);

	// The game's thread, each present: the device to upload with (nullptr: decode only, nothing on the GPU), the time;
	// entries not named for a while let go
	void frame_tick(ID3D12Device *device, double now);
	// A frame's palette rows [from, to) into its upload slot (the game's thread, as models are published)
	void publish_palettes(const frame &f, uint32_t from, uint32_t to);
	// Where a frame slot's palette rows are on the GPU (0 before the upload buffer is made)
	D3D12_GPU_VIRTUAL_ADDRESS palettes(uint32_t seq);

	// For the renderer (any thread): a mesh or texture once it's on the GPU
	struct mesh_view
	{
		bool ready = false;
		uint8_t hides = 0;
		uint32_t vertices = 0, corners = 0;
		std::vector<os2::group> groups;
		std::vector<uint32_t> textures; // the cache's texture per the mesh's texture index
		D3D12_GPU_DESCRIPTOR_HANDLE views {}; // vertices' view, then the corners'
	};
	bool mesh_view_of(uint32_t id, mesh_view &out);
	// Whether a mesh and the textures it found are all on the GPU (the game's thread asks before leaving a model out)
	bool mesh_ready(uint32_t id);
	bool texture_view_of(uint32_t id, D3D12_GPU_DESCRIPTOR_HANDLE &out);
	ID3D12DescriptorHeap *heap();
	// The renderer's own descriptors in that heap (k < kReserved: the see-through target's view); false before the heap
	constexpr uint32_t kReserved = 2;
	bool reserved(uint32_t k, D3D12_CPU_DESCRIPTOR_HANDLE &cpu, D3D12_GPU_DESCRIPTOR_HANDLE &gpu);

	struct counts
	{
		uint32_t meshes = 0, meshes_ready = 0, meshes_failed = 0;
		uint32_t textures = 0, textures_ready = 0, textures_failed = 0;
		uint64_t bytes_in = 0, gpu_bytes = 0;
		double decode_ms = 0.0, upload_ms = 0.0; // (totals)
		uint32_t forgotten = 0;
		std::string last_failure;
	};
	counts get_counts();
	std::vector<std::string> status();
	// For the tests: a mesh's two buffers and a texture's resource once on the GPU (nullptr otherwise)
	void resources_of(uint32_t mesh, uint32_t texture, ID3D12Resource *&vertices, ID3D12Resource *&corners, ID3D12Resource *&tex);

	// Waits until nothing is queued or being uploaded (the tests); false on timeout
	bool settle(uint32_t ms);
	// Everything let go (the thread stopped, the GPU's copies waited for)
	void stop();
}
