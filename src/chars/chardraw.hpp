#pragma once

// The native character renderer's drawing: a frame's character models (charframe.hpp) recorded onto a command list,
// into the world's target and depth, as the game draws them (research/ffxi-re/notes-characters.md "Materials",
// "Lighting", "For a native character renderer" part 2):
//
//   skinning      in the vertex shader from the frame's palette rows (02F1D0: one-bone (x y z 1) M[b], two-bone
//                 (x1 y1 z1 w1) M[a] + (x2 y2 z2 w2) M[b]; the mirrored half with the mirrored bones, flipped by axis)
//   lighting      Direct3D 8's fixed function per vertex, as the game sets it: up to 4 directional lights (the
//                 actor's, 0CB4E0) and AMBIENT, both times the material's light scale (dword 4); the material diffuse
//                 0.5 and ambient the actor colour / 128 (its bytes in the game's order: byte 0 to red); clamped
//   texturing     stage 0 MODULATE2X(texture, lit), alpha MODULATE2X(texture, TFACTOR); stage 1 MODULATE2X(current,
//                 TFACTOR), alpha MODULATE4X(current, TFACTOR) (TFACTOR = the global colour factor, alpha 0x80);
//                 each stage clamped; alpha test GREATER 0x45 (not for a shine material: its alpha always passes);
//                 linear fog by view depth
//   states        no blending, Z LESSEQUAL with writes, no culling; hide groups the model's meshes switch on skipped
//   see-through   the model drawn alone into a private target T (cleared to 0) with the world's depth (test and
//                 write), then T laid over the world: rgb MODULATE2X(T, actor colour), alpha MODULATE2X(T, actor
//                 alpha), SRCALPHA / INVSRCALPHA, no depth
//
// Option: per-pixel lighting (the same lights at each pixel, the normal interpolated).
//
// Vertices, corners and palette rows are read in the vertex shader (descriptor table, root SRV), so nothing of the
// input assembler's is touched. record_*() set the targets, viewport, heaps, root signature and pipelines and leave
// them so (putting the list's own back is the caller's).

#include <d3d12.h>
#include "charframe.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace hifi::chars
{
	struct char_target
	{
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = {}, dsv = {};
		DXGI_FORMAT rtv_format = DXGI_FORMAT_UNKNOWN, dsv_format = DXGI_FORMAT_UNKNOWN;
		UINT samples = 1;
		float width = 0.0f, height = 0.0f;
		float offset = 0.5f; // (dgVoodoo's half-pixel for Direct3D 8's pixel centres)
	};

	struct char_options
	{
		bool per_pixel = false;
	};

	struct char_stats
	{
		uint32_t models = 0, draws = 0, triangles = 0;
		uint32_t not_ready = 0, hidden = 0, untextured = 0; // groups (or models, not ready) left out
	};

	class char_renderer
	{
	public:
		char_renderer() = default;
		char_renderer(const char_renderer &) = delete;
		char_renderer &operator=(const char_renderer &) = delete;
		~char_renderer();

		bool init(ID3D12Device *device, std::string &why);
		bool ready() const { return _root != nullptr; }
		void release();

		// The frame's opaque models HiFi XI draws (kNative), each with its meshes ready, into the target. The
		// constants go into the upload ring's slot for the frame's seq.
		char_stats record_opaque(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, const char_options &o);
		// One model into `t` as it is (the see-through path's first half: the caller has bound T's view as t.rtv and
		// cleared it)
		char_stats record_model(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, uint32_t model, const char_options &o);
		// T (an SRV of it in `srv`, a descriptor of the cache's heap) laid over the world's target as the game does
		void record_composite(ID3D12GraphicsCommandList *list, const char_target &world, D3D12_GPU_DESCRIPTOR_HANDLE srv, uint32_t colour);

	private:
		bool begin(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, const char_options &o);
		void draw_model(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, uint32_t model, char_stats &st);
		ID3D12PipelineState *pipeline(const char_target &t, int kind);

		ID3D12Device *_device = nullptr;
		ID3D12RootSignature *_root = nullptr;
		void *_vs = nullptr, *_ps = nullptr, *_cvs = nullptr, *_cps = nullptr, *_cps_ms = nullptr; // (ID3DBlob)
		ID3D12Resource *_ring = nullptr; // upload: kFrames slots of the frame's constants and its models'
		uint8_t *_mapped = nullptr;
		std::unordered_map<uint64_t, ID3D12PipelineState *> _pipelines;
	};
}
