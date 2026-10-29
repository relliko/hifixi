#pragma once
// AMD FSR 2 over the world (research/ffxi-re/notes-fsr.md, milestone 4), for upscale.cpp: the world A (the background
// resolution, jittered: game/jitter) up to the screen's size, anti-aliased over the frames, into a texture of its own
// that upscale.cpp then draws over the back buffer.
//
// Each frame, on the list where the game's resolve was drawn:
//   - A's depth (its plane 0, D24: dgVoodoo's depth may not be readable through a view) copied into a buffer of ours;
//   - one compute pass from it: the depth as floats (FSR 2's depth) and the camera's motion (FSR 2's motion vectors,
//     in UV units, current to last frame, without the jitter), the point under each pixel moved through this frame's
//     view (unjittered) and the last one's view x projection; where a blade of World FX's grass is still what's seen
//     (its view depth the world's), the blade's own motion instead, its sway in the wind in it (grass::renderer::
//     blade_motion). Characters and effects move only as the camera does here;
//   - FSR 2's dispatch: auto exposure, depth not inverted with its far plane at infinity (near 0.1: the game's), its
//     sharpening (RCAS) at the setting's sharpness;
//   - A, the depth and motion textures and the output put back in the states they're expected in (FSR 2 leaves them
//     where its passes wanted them).
// The context (FSR 2's own resources and pipelines: its shaders compiled then, fsr2_shaders.cpp) is made off
// dgVoodoo's thread by make(), for one size of A and of the screen.
#include <d3d12.h>
#include "fsr3.hpp"
#include "masks.hpp"
#include <cstdint>
#include <string>

namespace hifi::upscale::temporal
{
	// This frame's camera and jitter (the zone pass's, from its frame record: zone::inject::last_camera)
	struct camera
	{
		float view[16];   // VIEW, row vectors
		float proj[16];   // PROJECTION, jitter in its _31, _32
		float jitter[2];  // A's pixels (FSR 2's jitterOffset)
		uint32_t seq = 0; // the zone pass's
	};

	class context
	{
	public:
		context() = default;
		context(const context &) = delete;
		context &operator=(const context &) = delete;
		~context();
		// Made for A's size and the screen's (not on dgVoodoo's thread), with FSR 3.1's upscaler (v3: fsr3.hpp) or FSR 2's;
		// false with why
		bool make(ID3D12Device *device, uint32_t render_w, uint32_t render_h, uint32_t display_w, uint32_t display_h, bool v3, std::string &why);
		void release();
		bool ready() const { return _made; }
		bool fits(uint32_t render_w, uint32_t render_h, uint32_t display_w, uint32_t display_h, bool v3) const
		{
			return _made && render_w == _rw && render_h == _rh && display_w == _dw && display_h == _dh && v3 == (_fsr3 != nullptr);
		}
		bool v3() const { return _fsr3 != nullptr; }
		const char *name() const { return _fsr3 != nullptr ? "FSR 3.1" : "FSR 2"; }
		// The frame (dgVoodoo's thread): A (color) in color_state, its depth (plane 0 in depth_state); its output left in
		// PIXEL_SHADER_RESOURCE (output()). Sets its own heaps, root signatures and pipelines: the caller puts dgVoodoo's
		// back. False with why when nothing was drawn.
		// blades: World FX's blades' motion this frame (A's size, shader-readable), or nullptr.
		bool record(ID3D12GraphicsCommandList *list, ID3D12Resource *color, D3D12_RESOURCE_STATES color_state, ID3D12Resource *depth,
			D3D12_RESOURCE_STATES depth_state, ID3D12Resource *blades, const camera &cam, float sharpen, float dt_ms, bool reset, std::string &why,
			const masks::taken *m = nullptr, ID3D12Resource *chars = nullptr);
		ID3D12Resource *output() const { return _output; }
		ID3D12Resource *motion() const { return _mv; } // (the motion FSR 2 was given: NON_PIXEL_SHADER_RESOURCE after record)
		ID3D12Resource *depth() const { return _depth; } // (FSR 2's depth: NON_PIXEL_SHADER_RESOURCE after record)
		DXGI_FORMAT output_format() const { return DXGI_FORMAT_R8G8B8A8_UNORM; }
		uint32_t frames() const { return _frames; }
		uint32_t blade_frames() const { return _blade_frames; } // (of them, with the blades' own motion)
		uint32_t mask_frames() const { return _mask_frames; }   // (of them, with the reactive or transparency mask)
		uint32_t char_frames() const { return _char_frames; }   // (of them, with the characters' own motion)
		ID3D12Resource *reactive() const { return _reactive; } // (the last masks: NON_PIXEL_SHADER_RESOURCE after record)
		ID3D12Resource *composition() const { return _tc; }

	private:
		bool _made = false;
		uint32_t _rw = 0, _rh = 0, _dw = 0, _dh = 0;
		ID3D12Device *_device = nullptr;
		void *_scratch = nullptr;
		void *_fsr = nullptr; // (FfxFsr2Context, allocated: its header stays in temporal.cpp)
		fsr3::upscaler *_fsr3 = nullptr; // (or FSR 3.1's)
		ID3D12RootSignature *_root = nullptr;
		ID3D12PipelineState *_motion = nullptr;
		ID3D12DescriptorHeap *_heap = nullptr;
		UINT _step = 0;
		ID3D12Resource *_depth_copy = nullptr; // (a buffer: plane 0 as the copy lays it out)
		uint32_t _pitch_words = 0;
		ID3D12Resource *_depth = nullptr, *_mv = nullptr, *_output = nullptr;
		D3D12_RESOURCE_STATES _depth_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, _mv_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		D3D12_RESOURCE_STATES _output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		D3D12_RESOURCE_STATES _copy_state = D3D12_RESOURCE_STATE_COMMON;
		uint32_t _slot = 0;
		bool _have_last = false;
		float _last_view_proj[16] = {}; // (the last frame's VIEW x PROJECTION without the jitter)
		uint32_t _frames = 0, _blade_frames = 0, _mask_frames = 0, _char_frames = 0;
		// The masks' pass (masks.hpp): its root signature, pipeline, descriptors (six a frame) and the two masks (R8_UNORM)
		ID3D12RootSignature *_mask_root = nullptr;
		ID3D12PipelineState *_mask = nullptr;
		ID3D12DescriptorHeap *_mask_heap = nullptr;
		ID3D12Resource *_reactive = nullptr, *_tc = nullptr;
		D3D12_RESOURCE_STATES _mask_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	};
}
