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
	// The en-spell effects' time (chardraw.hpp record_enspells) held at `seconds` (the tests, the offline viewer); below 0,
	// the real clock again
	void set_clock(double seconds);
	// Whether models can be handed over: the renderer is up and nothing failed in the last two seconds
	bool ready();
	// The opaque characters' motion for FSR (chardraw.hpp "Motion"): wanted while FSR draws (the plugin follows it each
	// present); the texture (the world's size, R16G16B16A16_FLOAT: xyz the move since the last frame in the world, w
	// the view depth, 0 where no character is; NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE between frames) and how
	// many frames it's been drawn in. False when there's none.
	void set_motion_wanted(bool wanted);
	// Toward the zone's sun (world; nullptr: not known), for the light split's share of the sun (light_split.hpp)
	void set_sun(const float *toward);
	// The native zone's lamps of its last pass (8 floats each: chardraw.hpp's char_lamp; nullptr or 0: none known), the
	// models' lamp light (chardraw.hpp "Lamps")
	void set_lamps(const float *lamps, uint32_t count);
	bool motion(ID3D12Resource *&texture, uint64_t &frames);
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
		uint64_t enspell_models = 0; // (models drawn with an en-spell effect)
		uint64_t overlays = 0, cloaks = 0, cloak_failed = 0; // (models drawn with their extra passes; scene copies that couldn't be made)
		uint64_t auras = 0, aura_failed = 0; // (see-through models drawn with their aura ring; ones whose ring couldn't be: a multisampled world)
	};
	counts get_counts();
}
