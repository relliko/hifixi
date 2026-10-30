#pragma once

// The effects' textures on the GPU, for the native effect renderer: each texture chunk (0x20) an element's geometry
// names, taken once from the game's own memory the first time a recorded element uses it (src/game/fxdraw: the chunk's
// bytes copied on the game's thread), decoded on a thread of ours (chars::tex, level 0 only: the effects set MIN/MAG
// LINEAR and no MIPFILTER, so the game samples the picture itself) and copied to the GPU on dgVoodoo's own list at the
// next fx_run marker (an upload buffer of ours, let go two seconds later), then a view of it in our heap.
//
// The game's thread draws an element natively only once every texture it names is on the GPU (ready): until then the
// element is the game's. Entries are keyed by the game's resource and checked against its handle, header and data word
// (a freed resource's memory can be reused for another); one not named for kForgetSeconds is let go (its texture two
// seconds after: past the GPU).
//
//   descriptors  one shader-visible heap of ours: kReserved of the renderer's own first (0: a white 1x1, bound for
//                untextured draws), then one per id

#include <d3d12.h>
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::fx::textures
{
	constexpr uint32_t kNone = 0xFFFFFFFFu;
	constexpr uint32_t kMax = 4096;
	constexpr uint32_t kReserved = 8;
	constexpr double kForgetSeconds = 20.0;

	struct key
	{
		uintptr_t resource = 0;
		uint32_t handle = 0, header = 0, data = 0;
		bool operator==(const key &o) const { return resource == o.resource && handle == o.handle && header == o.header && data == o.data; }
	};

	// The game's thread. A texture known (named now, so it's kept), else kNone; a new one with its chunk's bytes (kNone
	// when there's no room); whether one is on the GPU; the forgotten let go.
	uint32_t find(const key &k, double now);
	uint32_t add(const key &k, std::vector<uint8_t> &&chunk, double now);
	bool ready(uint32_t id);
	bool failed(uint32_t id);
	void tick(double now);
	// Decoded and waiting for their copy (the game side sends a marker for them even with nothing to draw)
	uint32_t waiting();

	// dgVoodoo's thread (at a marker). The heap made for this device; false and why when it can't be.
	bool prepare(ID3D12Device *device, std::string &why);
	void release();
	// The decoded ones copied in on this list (at most budget bytes of them), the forgotten ones let go; how many went
	uint32_t upload(ID3D12GraphicsCommandList *list, uint64_t budget);
	ID3D12DescriptorHeap *heap();
	D3D12_GPU_DESCRIPTOR_HANDLE view(uint32_t id); // (kNone: the white 1x1)
	bool reserved(uint32_t k, D3D12_CPU_DESCRIPTOR_HANDLE &cpu, D3D12_GPU_DESCRIPTOR_HANDLE &gpu);

	struct counts
	{
		uint32_t entries = 0, on_gpu = 0, failed = 0, forgotten = 0;
		uint64_t gpu_bytes = 0;
	};
	counts get_counts();
	std::string status();
	// The tests: waits until nothing is queued for decoding; false on timeout
	bool settle(uint32_t ms);
	// Everything let go (the thread stopped)
	void stop();
}
