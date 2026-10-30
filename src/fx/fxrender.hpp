#pragma once

// The native effect renderer (research/ffxi-re/notes-effects-draw.md): draws a run of the recorded elements (frame.hpp)
// into the world's target with its depth, as 0467E0 and the class draws would have: WORLD = the element's draw matrix
// (view space), PROJECTION the camera's (or the element's own: the depth pull, screen-fixed), no culling, z test
// LESSEQUAL, z write by flags bit 12, the blend by its letter, fog unless flags bit 25 (H: the fog colour black), and
// the texture stages as the class's state block has them, the PS2 rule "0x80 = 1.0" (each MODULATE2X / 4X clamped).
//
// The vertices come from the frame (copied into an upload ring of ours: kFrames slots of kMaxVertices, slot = the
// flush's seq), read in the vertex shader by SV_VertexID (no input-assembler state of dgVoodoo's touched). Textures
// from src/fx/textures (its heap bound by the caller).
//
// Distortion (0426F0): a draw with a grab first copies the world as drawn so far (resolved when multisampled) and draws
// the grab's quad of it into a 256 x 256 target of ours (the game's VA 0490BB88), which the draw then samples
// (kDistTexture: textures' reserved descriptor 1; the world's copy is reserved descriptor 2).
//
// Light spots (040E70): an element into_spot draws into that same target (cleared, with a 256 x 256 depth of ours), the
// ground after it samples it (kSpotTexture, CLAMP).

#include <d3d12.h>
#include "fxframe.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace hifi::fx
{
	struct target
	{
		D3D12_CPU_DESCRIPTOR_HANDLE rtv {}, dsv {};
		DXGI_FORMAT rtv_format = DXGI_FORMAT_UNKNOWN, dsv_format = DXGI_FORMAT_UNKNOWN;
		UINT samples = 1;
		float width = 0.0f, height = 0.0f;
		ID3D12Resource *rt = nullptr; // (the world's target, in RENDER_TARGET during the run: what a grab copies)
	};

	struct run_stats
	{
		uint32_t elements = 0, draws = 0, triangles = 0, grabs = 0, spots = 0;
	};

	class fx_renderer
	{
	public:
		~fx_renderer() { release(); }
		bool init(ID3D12Device *device, std::string &why);
		void release();
		bool ready() const { return _root != nullptr; }
		// Run `run` of the frame (seq: the flush's, its upload slot) drawn on the list; the textures' heap bound by the caller
		run_stats record_run(ID3D12GraphicsCommandList *list, const target &t, const frame &f, uint32_t seq, uint32_t run);

	private:
		ID3D12PipelineState *pipeline(const target &t, uint32_t blend, bool z_write);
		// A distortion grab: the world copied, the grab's quad of it drawn into the distortion target (left readable by
		// a pixel shader); the world's target bound again by the caller. False: the draw is skipped.
		bool grab(ID3D12GraphicsCommandList *list, const target &t, const fx::grab &g);
		bool dist_resources(const target &t);
		bool spot_resources(const target &t);

		ID3D12Device *_device = nullptr;
		void *_vs = nullptr, *_ps = nullptr, *_vs_grab = nullptr, *_ps_grab = nullptr; // (ID3DBlob)
		ID3D12RootSignature *_root = nullptr;
		ID3D12Resource *_ring = nullptr;
		uint8_t *_mapped = nullptr;
		std::unordered_map<uint64_t, ID3D12PipelineState *> _pipelines;
		// Distortion: the 256 x 256 target (its RTV in a heap of its own), the world's copy, the grab's pipeline
		ID3D12Resource *_dist = nullptr, *_scene = nullptr;
		ID3D12DescriptorHeap *_dist_rtv = nullptr;
		ID3D12PipelineState *_grab_pso = nullptr;
		// Light spots: their picture's depth (the target is the distortion's)
		ID3D12Resource *_spot_depth = nullptr;
		ID3D12DescriptorHeap *_spot_dsv = nullptr;
		DXGI_FORMAT _dist_format = DXGI_FORMAT_UNKNOWN;
		UINT64 _scene_width = 0;
		UINT _scene_height = 0;
		D3D12_RESOURCE_STATES _scene_state = D3D12_RESOURCE_STATE_COMMON;
	};
}
