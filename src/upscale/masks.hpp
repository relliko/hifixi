#pragma once
// The world copied at four points of a frame for FSR's masks (temporal.cpp works them out): what changes between two
// copies wasn't there when the opaque world was drawn, so FSR leans less on its history there.
//   opaque:  at the characters' end marker, the native characters drawn: the zone, the blades and the characters;
//   pre_fx:  at the world's end before World FX's passes: the game's effects, particles and see-through things over it;
//   shaded:  after World FX's shading pass and snow layer;
//   pre_air: before World FX's light in the air and look: its water, rain, snow and fireflies over the shaded world.
// The reactive mask is opaque against pre_fx (the game's effects); the transparency and composition mask, shaded (or
// pre_fx when World FX didn't shade) against pre_air (World FX's own). Copies only while FSR 2 or 3.1 draws and the
// setting is on; each frame's taken once (take()), at FSR's pass.
#include <d3d12.h>
#include <cstdint>

namespace hifi::upscale::masks
{
	enum when : int { opaque = 0, pre_fx = 1, shaded = 2, pre_air = 3, kinds = 4 };

	void set_on(bool on);
	bool on();
	// The world (A) copied, from the state it's in and back to it (dgVoodoo's thread)
	void snapshot(ID3D12GraphicsCommandList *list, ID3D12Resource *world, D3D12_RESOURCE_STATES state, when w);
	// World FX's renderer's snapshots (grass::set_snapshot_hook): its 0, 1, 2 as pre_fx, shaded, pre_air
	void from_world_fx(ID3D12GraphicsCommandList *list, ID3D12Resource *world, D3D12_RESOURCE_STATES state, int when);

	struct taken
	{
		ID3D12Resource *copy[kinds] = {};  // NON_PIXEL_SHADER_RESOURCE, A's size and format
		bool fresh[kinds] = {};            // (copied this frame)
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // (A's; its copies' views take its typed format)
	};
	// This frame's copies, then none fresh till the next ones (dgVoodoo's thread)
	taken take();
	uint64_t frames_with(when w); // (how many frames had it: the status)
}
