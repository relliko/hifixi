#pragma once

// The native zone renderer on dgVoodoo's Direct3D 12 lists (research/ffxi-re/notes-replace-slots.md "D3D12 side"):
// at the zone's end marker (src/game/slotmark.hpp), on the list dgVoodoo is recording, the pass the game side
// published under that seq (frame.hpp) is drawn into the world's target and depth, and dgVoodoo's own state is bound
// again: its targets, viewports, scissors, pipeline, topology and root arguments (the list's grass::list_state) and its
// descriptor heaps (followed here from SetDescriptorHeaps). The input assembler's buffers aren't touched (the renderer
// reads its vertices from root SRVs).
//
// The world's depth is followed plane by plane: dgVoodoo moves its depth plane between DEPTH_WRITE and DEPTH_READ as
// draws write it or not, and its stencil plane to DEPTH_WRITE around a stencil clear (the markers), back at its next
// draw. Both go to DEPTH_WRITE for the zone and back to what they were. dgVoodoo records on one thread: the planes'
// states are followed across its lists as recorded.
//
// Everything but prepare/release/set_options/ready/status runs on dgVoodoo's recording thread, inside the hooks.

#include <d3d12.h>
#include "../render/grass_render.hpp" // (grass::list_state)
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::zone::inject
{
	// The renderer made for the device (the game's thread, when native drawing is wanted); false and why
	bool prepare(ID3D12Device *device, std::string &why);
	void release();
	void set_options(bool per_pixel);
	// Whether passes can be handed over: the renderer is up and the last ones handed over were drawn (none failed in
	// the last two seconds)
	bool ready();
	std::vector<std::string> status();

	// dgVoodoo's lists, as the hooks see them
	void on_reset(ID3D12GraphicsCommandList *list);
	void on_heaps(ID3D12GraphicsCommandList *list, UINT count, ID3D12DescriptorHeap *const *heaps);
	void on_barrier(UINT count, const D3D12_RESOURCE_BARRIER *barriers);

	// The world as the markers name it: its target (with the format of dgVoodoo's view of it, and the state it's in on
	// this list) and depth (likewise)
	struct world
	{
		ID3D12Resource *rt = nullptr, *ds = nullptr;
		DXGI_FORMAT rt_format = DXGI_FORMAT_UNKNOWN, ds_format = DXGI_FORMAT_UNKNOWN;
		D3D12_RESOURCE_STATES rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
	};
	// Any marker: the world's depth (its planes followed from now on)
	void on_marker(ID3D12Resource *depth);
	// The world's depth planes' states as dgVoodoo's barriers left them (depth, stencil), for others drawing into it at
	// the markers (World FX's blades at the zone's end); false when that isn't this depth or a plane isn't known yet
	bool depth_planes(ID3D12Resource *depth, D3D12_RESOURCE_STATES out[2]);
	// The world's depth as the markers found it, and its planes' states (false while either isn't known)
	bool world_depth(ID3D12Resource *&depth, D3D12_RESOURCE_STATES planes[2]);
	// The camera of the last zone pass whose end marker dgVoodoo's thread has recorded (its frame record's VIEW,
	// PROJECTION with the jitter in it, the jitter, its seq; when, GetTickCount64): the frame being finished now, as
	// dgVoodoo records one frame after another on its one thread. False before the first.
	struct frame_camera
	{
		float view[16], proj[16];
		float jitter[2];
		float sun_dir[3];  // toward the zone's sun (world)
		uint32_t seq = 0;
		uint64_t at = 0;
	};
	bool last_camera(frame_camera &out);
	// The zone's end marker: the pass published under that seq drawn there when it's HiFi XI's
	void on_zone_end(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const world &w, uint32_t seq_low4);

	// World FX's sun shadow cascades from the zone this draws (zoneshadow.hpp; grass::renderer::set_zone_casters): the
	// zone drawn here in the last kZoneFreshMs (only then: a zone left is let go 240 presents on), the box around what it
	// places and a number that changes with its data; and its placements, depth only, into a cascade (the game's
	// descriptor heaps put back after; nothing drawn when they aren't known). Triangles drawn. The second map (an
	// interior) the pass drew is cast as well. While the moving layer is drawn, the near cascade leaves out its movers.
	bool zone_bounds(float lo[3], float hi[3], uint64_t &generation);
	uint32_t record_zone_casters(ID3D12GraphicsCommandList *list, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size);
	// The moving layer (zoneshadow.hpp): the swaying plants and the last pass's doors and elevators, both maps', depth
	// only into World FX's casters' map (grass::renderer::set_casters: drawn each frame around the camera, beside the
	// characters); the game's heaps put back after. Whether anything was drawn.
	bool record_zone_movers(ID3D12GraphicsCommandList *list, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size);
	// All as World FX takes them (the movers alone; the plugin hands World FX one caster function for them and the
	// characters together)
	grass::renderer::zone_bounds_fn zone_bounds_hook();
	grass::renderer::zone_draw_fn zone_draw_hook();
	grass::renderer::caster_fn movers_hook();
}
