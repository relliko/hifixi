#pragma once

// The native character renderer on dgVoodoo's Direct3D 12 lists (as the zone's, src/zone/inject.hpp): at the
// characters' end marker the frame's opaque models HiFi XI draws (charframe.hpp, kNative) go into the world's target
// and depth; at each see-through marker that one model goes through the see-through path (a private target T with the
// world's depth, then laid over the world). Then dgVoodoo's own state is bound again: its descriptor heaps (followed
// here from SetDescriptorHeaps), then its targets, viewports, scissors, pipeline, topology and root arguments
// (grass::list_state). The world's depth planes are followed from dgVoodoo's barriers and put back as they were.
//
// Everything but prepare/release/set_options/ready/status runs on dgVoodoo's recording thread, inside the hooks.

#include <d3d12.h>
#include "../render/grass_render.hpp" // (grass::list_state)
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::chars::inject
{
	bool prepare(ID3D12Device *device, std::string &why);
	void release();
	void set_options(bool per_pixel);
	// Whether models can be handed over: the renderer is up and nothing failed in the last two seconds
	bool ready();
	std::vector<std::string> status();

	void on_reset(ID3D12GraphicsCommandList *list);
	void on_heaps(ID3D12GraphicsCommandList *list, UINT count, ID3D12DescriptorHeap *const *heaps);
	void on_barrier(UINT count, const D3D12_RESOURCE_BARRIER *barriers);

	struct world
	{
		ID3D12Resource *rt = nullptr, *ds = nullptr;
		DXGI_FORMAT rt_format = DXGI_FORMAT_UNKNOWN, ds_format = DXGI_FORMAT_UNKNOWN;
		D3D12_RESOURCE_STATES rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
	};
	void on_marker(ID3D12Resource *depth);
	// The characters' end marker (kind 3) and each see-through marker (kind 4), with the seq they carry
	void on_chars_end(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const world &w, uint32_t seq_low4);
	void on_see_through(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const world &w, uint32_t seq_low4);
	// World FX's casters (grass::renderer::set_casters): the last frame drawn's models, depth only, into a sun shadow map
	// seen through world_to_clip; the game's descriptor heaps put back after. Whether any was drawn.
	bool record_casters(ID3D12GraphicsCommandList *list, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size);
	// record_casters as World FX takes it (worldfx::set_casters(caster_hook(), nullptr))
	grass::renderer::caster_fn caster_hook();

	struct counts
	{
		uint64_t frames = 0, models = 0, see_through = 0, failed = 0;
		uint64_t caster_frames = 0, caster_models = 0; // (drawn into the sun's casters' map)
	};
	counts get_counts();
}
