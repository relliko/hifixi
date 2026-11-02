#pragma once
// HiFi XI's own stretch of the world over the screen (research/ffxi-re/notes-fsr.md, milestone 1). The game draws the
// world into its target A at the background resolution, then stretches A over the screen's back buffer with one
// bilinear quad (the resolve, 00C0B0), then draws the UI at the screen's size. When A is smaller than the screen and
// FSR is on, HiFi XI draws A over the screen again right after that quad, at our d3d8.dll's resolve mark: FSR 3.1 (or
// FSR 2) with the world's depth and motion, else AMD FSR 1's EASU up to the screen's size into a texture of its own,
// then RCAS sharpening into the back buffer. The UI is drawn on top as before, never upscaled.
//
// The resolve: the draw of at most 6 vertices that read A into a target bigger than it, as d3d8.dll marks it. Device
// objects are made off the recording (prepare, from the plugin's frame): a new screen size or format is asked for there
// and used from the next frame; until then the game's stretch stays.
#include <d3d12.h>
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
	// Drawn over the upscaled world right after it lands on the screen's target, before the UI (the names over the screen,
	// src/world): the list, the target (in RENDER_TARGET: its view, format, size) and A. The upscale binds dgVoodoo's heaps
	// and state again after.
	struct overlay_target
	{
		ID3D12Resource *target = nullptr;
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		UINT width = 0, height = 0;
		ID3D12Resource *world = nullptr;
	};
	using overlay_fn = void (*)(ID3D12GraphicsCommandList *list, const overlay_target &t);
	void set_overlay(overlay_fn fn);
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

	// Our d3d8.dll's resolve mark (src/host/frameapi.hpp, channel 4): right after the draw that read the world into the
	// target bound now, on the game's thread. FSR goes in here, with nothing to put back: d3d8.dll binds all of its own
	// again after a mark.
	struct resolve_mark
	{
		ID3D12GraphicsCommandList *list = nullptr;
		ID3D12Resource *world = nullptr;  // (its target, read by the resolve)
		D3D12_RESOURCE_STATES world_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		ID3D12Resource *target = nullptr; // (what the resolve drew into, bound now)
		uint64_t rtv = 0;
		DXGI_FORMAT target_format = DXGI_FORMAT_UNKNOWN;
		D3D12_RESOURCE_STATES target_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		uint32_t vertices = 0;
	};
	void on_resolve(const resolve_mark &r);
}
