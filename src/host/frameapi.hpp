#pragma once

// The direct channel between HiFi XI's own d3d8.dll and HiFi XI (docs/own-frame/no-dgvoodoo.md, step 2): the game side
// says where one of the game's passes begins or ends with a call, not a stencil pixel for the Direct3D 12 side to spot.
//
// HifiFrameMark(kind, seq), on the game's thread (the one the game's Direct3D 8 calls come on): d3d8.dll binds its
// current targets, puts its pending barriers on the list, and calls the handler HiFi XI set (HifiFrameSetHandler) right
// there, with its list, its current target and depth, their views, and the states they're in (both of the depth's
// planes). The handler may record anything on the list; it leaves the target and depth in the states it found them in.
// After it, d3d8.dll forgets everything it had bound (the next draw binds all of it again) and sets its descriptor heaps
// and root signature itself: nothing has to be put back. kind and seq as src/game/slotmark.hpp has them.
//
// Returns 0 when there's nothing to call (no handler, no device yet): the caller does what it did before.

#include <d3d12.h>
#include <cstdint>

struct hifi_frame_mark
{
	ID3D12GraphicsCommandList *list;
	ID3D12Resource *rt, *ds;            // (nullptr: none bound)
	D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;
	DXGI_FORMAT rt_format, ds_format;   // (the views')
	D3D12_RESOURCE_STATES rt_state;
	D3D12_RESOURCE_STATES ds_planes[2]; // (plane 1: the stencil's, when the depth has one)
	uint32_t ds_plane_count;
	uint32_t width, height;             // (the target's)
	// (HifiFrameVersion() >= 2 only; an older d3d8.dll's struct ends above) The world: the target and depth bound at the
	// last zone's begin mark, bound now or not, and the states they're in at this mark (nullptr: no zone begun yet, or
	// the game has let it go). Every mark has them, the world's end and the back buffer's too: nothing has to follow
	// barriers to know them.
	ID3D12Resource *world_rt, *world_ds;
	D3D12_RESOURCE_STATES world_rt_state;
	D3D12_RESOURCE_STATES world_ds_planes[2];
	uint32_t world_ds_plane_count;
	// (HifiFrameVersion() >= 3) The colour the world's target was last cleared to, the whole of it (the game clears it
	// to the zone's fog at each frame's start); world_clear_known 0 while there's been none since the world was named
	float world_clear[4];
	uint32_t world_clear_known;
};

using hifi_frame_handler = void(__cdecl *)(void *context, uint32_t kind, uint32_t seq, const hifi_frame_mark *m);
using hifi_frame_set_handler_fn = void(__cdecl *)(hifi_frame_handler handler, void *context);
using hifi_frame_mark_fn = int(__cdecl *)(uint32_t kind, uint32_t seq);
// HifiFrameSetWorldFormat(view): the GPU format the world's target is to be in (DXGI_FORMAT_R16G16B16A16_FLOAT: the float
// world, light past white; DXGI_FORMAT_UNKNOWN or R8G8B8A8_UNORM: the game's 8 bits). d3d8.dll makes the target bound at
// the next zone's begin mark again in it when it differs, before that mark's handler sees it (its pixels black that
// frame), its views kept. Returns 0 for a format it doesn't make.
using hifi_frame_set_world_format_fn = int(__cdecl *)(DXGI_FORMAT view);

// HifiFrameSetSpeedups(flags): the frame-rate speed-ups done in d3d8.dll (docs/frame-rate.md), on the game's thread:
// kHifiDedupe, a Set* call that sets what's already in effect returns at once; kHifiBatch, consecutive DrawPrimitiveUP
// quads (the interface's and the text's, a two-triangle strip each) are drawn as one triangle list. Returns what was on;
// kHifiSpeedupsAsk only asks. HifiFrameSpeedupStats(out): running counts of Set* calls dropped, quads batched, the draws
// they became, and every draw the device made.
constexpr uint32_t kHifiDedupe = 1, kHifiBatch = 2, kHifiSpeedupsAsk = 0xFFFFFFFFu;
using hifi_frame_set_speedups_fn = uint32_t(__cdecl *)(uint32_t flags);
using hifi_frame_speedup_stats_fn = void(__cdecl *)(uint32_t out[4]);

// HifiFrameVersion(): what this d3d8.dll's channel has (1 when the export is missing: the struct ends at width/height;
// 2: the world's fields too; 3: and the world's clear colour). HifiFrameDevice(device, queue): its device and the direct
// queue the game's lists go to, not AddRef'd (valid while it draws: from the device's making till the game lets it go);
// 0 when there's none yet.
constexpr uint32_t kHifiFrameChannel = 3;
using hifi_frame_version_fn = uint32_t(__cdecl *)();
using hifi_frame_device_fn = int(__cdecl *)(ID3D12Device **device, ID3D12CommandQueue **queue);

// (the exports' names in d3d8.dll)
constexpr const char *kHifiFrameVersion = "HifiFrameVersion";
constexpr const char *kHifiFrameDevice = "HifiFrameDevice";
constexpr const char *kHifiFrameSetHandler = "HifiFrameSetHandler";
constexpr const char *kHifiFrameMark = "HifiFrameMark";
constexpr const char *kHifiFrameSetWorldFormat = "HifiFrameSetWorldFormat";
constexpr const char *kHifiFrameSetSpeedups = "HifiFrameSetSpeedups";
constexpr const char *kHifiFrameSpeedupStats = "HifiFrameSpeedupStats";
