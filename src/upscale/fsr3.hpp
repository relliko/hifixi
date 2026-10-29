#pragma once
// AMD FSR 3.1's upscaler (the FidelityFX SDK v1.1.4 in src/framegen/ffx, its shaders compiled with DXC: tools/ffx_sc),
// for temporal.cpp in FSR 2's place: the same inputs (the world A, its depth and motion at A's size, the jitter, the
// camera) to the same output (the screen's size), better kept over the frames (less ghosting and shimmer, thin things
// kept). Built into the frame generation library (fg_names.h: the SDK's names apart from FSR 2's copy), so its header
// has nothing of the SDK's.
#include <d3d12.h>
#include <cstdint>
#include <string>

namespace hifi::upscale::fsr3
{
	struct upscaler; // (the SDK's context, its backend and shared resources: fsr3.cpp)

	// Made for A's size and the screen's (not on dgVoodoo's thread: its pipelines are made here); nullptr with why
	upscaler *make(ID3D12Device *device, uint32_t render_w, uint32_t render_h, uint32_t display_w, uint32_t display_h, std::string &why);
	// Once the GPU is done with it
	void destroy(upscaler *u);

	struct frame
	{
		ID3D12GraphicsCommandList *list = nullptr;
		ID3D12Resource *color = nullptr;        // A, NON_PIXEL_SHADER_RESOURCE
		ID3D12Resource *depth = nullptr;        // A's size, R32_FLOAT, not inverted, far plane at infinity: NON_PIXEL_SHADER_RESOURCE
		ID3D12Resource *motion = nullptr;       // A's size, R16G16_FLOAT, UV units, current to last, without the jitter: same
		ID3D12Resource *reactive = nullptr;     // A's size, R8_UNORM (how much to trust this frame over the history), or nullptr: same
		ID3D12Resource *composition = nullptr;  // A's size, R8_UNORM (transparent and composited things), or nullptr: same
		ID3D12Resource *output = nullptr;       // the screen's size, UNORDERED_ACCESS
		float jitter[2] = {};                   // A's pixels
		float sharpen = 0.0f;                   // 0..1 (RCAS)
		float dt_ms = 16.7f;
		float fov = 1.0f;                       // vertical, radians
		bool reset = false;
	};
	// Its passes on the list (its own heaps, root signatures and pipelines set: the caller puts its own back); every
	// resource left in the state it came in. False with why.
	bool dispatch(upscaler *u, const frame &f, std::string &why);
	int32_t jitter_phases(uint32_t render_w, uint32_t display_w);
}
