#pragma once
// HiFi XI's own stretch of the world over the screen (research/ffxi-re/notes-fsr.md, milestone 1). The game draws the
// world into its target A at the background resolution, then stretches A over the screen's back buffer with one
// bilinear quad (the resolve, 00C0B0), then draws the UI at the screen's size. When A is smaller than the screen and
// FSR is on, HiFi XI draws A over the screen again right after that quad, before anything else on the list (the UI's
// first draw, a clear, a barrier or the list's close): AMD FSR 1's EASU up to the screen's size into a texture of its
// own, then RCAS sharpening into the back buffer. The UI is drawn on top as before, never upscaled.
//
// The resolve is told on the list where A turned readable (its end: World FX's passes went in there): the first draw
// after it of at most 6 vertices into a target bigger than A. Nothing is drawn on a list whose descriptor heaps aren't
// known (they're put back after, with dgVoodoo's targets, pipeline, root signature and arguments). Device objects are
// made off dgVoodoo's thread (prepare, from the plugin's frame): a new screen size or format is asked for there and
// used from the next frame; until then the game's stretch stays.
#include <d3d12.h>
#include "../render/grass_render.hpp" // (grass::list_state)
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::upscale
{
	// FSR on (else the game's stretch), FSR 2 first (temporal: with the world at most the screen's size; FSR 1 for a
	// smaller one when FSR 2 can't draw), and the sharpening (0 none .. 1 the most)
	void set_options(bool fsr, bool temporal, float sharpen, bool fsr3 = true);
	// Where the characters' own motion comes from (the plugin: chars::inject::motion; none in the tests): the texture
	// and how many frames it's been drawn in
	using chars_motion_fn = bool (*)(ID3D12Resource *&texture, uint64_t &frames);
	void set_chars_motion_source(chars_motion_fn fn);
	// The colour the game's resolve multiplies the world by (0xRRGGBB, 0x80 each the picture as it is: game::resolve_tint),
	// put on ours as well: every cutscene fade is it. Each present, from the game's thread.
	void set_tint(uint32_t rgb);
	// Whether FSR 2 drew in the last quarter second (the world's jitter goes with it: game/jitter), and the length of its
	// jitter sequence for A's size and the screen's
	bool temporal_running();
	uint32_t jitter_phases();
	// For the device dgVoodoo draws with: its root signature and pipelines, and what dgVoodoo's thread has asked for
	// since (the screen-size texture, a pipeline for the back buffer's format). Not on dgVoodoo's thread. False with why
	// when they can't be made (FSR then stays off).
	bool prepare(ID3D12Device *device, std::string &why);
	void release();
	std::vector<std::string> status();

	// dgVoodoo's lists (its thread)
	void on_reset(ID3D12GraphicsCommandList *list);
	void on_heaps(ID3D12GraphicsCommandList *list, UINT count, ID3D12DescriptorHeap *const *heaps); // after
	// The world turned readable on this list (its end), in that state
	void on_world_end(ID3D12GraphicsCommandList *list, ID3D12Resource *world, D3D12_RESOURCE_STATES state);
	// Before a draw of `vertices` into rt (through rtv: format_of gives a view's format), with the list's state as dgVoodoo
	// left it. Costs an atomic load while no list waits on a resolve.
	void before_draw(ID3D12GraphicsCommandList *list, uint64_t vertices, ID3D12Resource *rt, uint64_t rtv, DXGI_FORMAT (*format_of)(uint64_t),
		const grass::list_state &state);
	// Before a clear or the list's close
	void before_other(ID3D12GraphicsCommandList *list, const grass::list_state &state);
	// After barriers (the states they left)
	void after_barrier(ID3D12GraphicsCommandList *list, UINT count, const D3D12_RESOURCE_BARRIER *barriers, const grass::list_state &state);
}
