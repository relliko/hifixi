#pragma once
// AMD FSR 3 frame generation (research/ffxi-re/notes-framegen.md; the FidelityFX SDK v1.1.4 in src/framegen/ffx, built
// with HiFi XI). Spike, step 1: dgVoodoo's swap chain for the game's window made as AMD's frame interpolation swap chain
// (its own present queue and pacing threads around a real FLIP_DISCARD swap chain), frame generation off: the game drawn
// and presented through it as before. DXGI's factory's CreateSwapChain and CreateSwapChainForHwnd go through ours from
// HiFi XI's early start (before dgVoodoo makes its device and swap chain), when the setting (upscale.framegen) is on
// then: turning it on takes effect the next time the game starts.
//
// Step 2, generation on (the setting on and FSR 2 drawing the world; turned off at once): each frame, from FSR 2's pass on dgVoodoo's
// list, prepare() hands AMD's frame interpolation FSR 2's depth and motion (made into its dilated depth and motion
// there) and keeps FSR 2's picture as the frame without the UI. At dgVoodoo's Present AMD's swap chain asks for the
// frame between the last one and this one: optical flow over the pictures without the UI, then frame interpolation,
// the UI taken from the back buffer where it differs from FSR 2's picture. A frame that had no prepare() (FSR 2 not
// drawn: loading, a menu over a black world) is shown twice instead.
#include <d3d12.h>
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::framegen
{
	// FSR 2's inputs and output this frame, in the states its pass leaves them
	struct frame
	{
		ID3D12Resource *depth = nullptr;   // A's size, FSR 2's depth (not inverted, far plane at infinity), NON_PIXEL_SHADER_RESOURCE
		ID3D12Resource *motion = nullptr;  // A's size, UV units, current to last, without the jitter, NON_PIXEL_SHADER_RESOURCE
		ID3D12Resource *picture = nullptr; // the screen's size, FSR 2's output (the world without the UI), PIXEL_SHADER_RESOURCE
		float jitter[2] = {};              // A's pixels
		float fov = 1.0f;                  // vertical, radians
		float dt_ms = 16.7f;
		bool reset = false;
	};
	// On dgVoodoo's list after FSR 2's dispatch (its own heaps, root signatures and pipelines set: the caller puts
	// dgVoodoo's back). False with why when there's no frame generation (not wanted, no AMD swap chain, its contexts not
	// made).
	bool prepare(ID3D12GraphicsCommandList *list, const frame &f, std::string &why);
	// Each present, on the game thread (Ashita's Direct3DPresent), no lock of HiFi XI's held: generation turned on in
	// AMD's swap chain once FSR 2 hands frames over, off a second after the last (turning it either way waits for the
	// presents in flight)
	void present();
	// (tests) The texture the last frame made between was written to, UNORDERED_ACCESS on the game's queue; nullptr before one
	ID3D12Resource *last_made();

	// At HiFi XI's early start, with DXGI's own CreateDXGIFactory1 (the starter's): the factory's swap chain calls through
	// ours, when wanted. False with why when they couldn't be; true (nothing done) when not wanted.
	// log: HiFi XI's log (level 0 info, 1 warning, 2 error), told of each swap chain made through ours.
	bool install(void *create_factory1, std::string &why, void (*log)(int level, const char *text) = nullptr);
	// The setting at the start (before install(): AMD's swap chain put in only then), and as it is now (generation
	// only while on: off at once, on again at once when the swap chain is AMD's)
	void set_wanted(bool on);
	bool wanted();
	void set_on(bool on);
	std::vector<std::string> status();
	// Frames made between since the start (the menu's frames shown)
	uint64_t made_between();
}
