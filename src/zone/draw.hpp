#pragma once

// The native zone renderer's drawing: one zone pass (frame.hpp) recorded onto a command list, into the world's target
// and depth, as the game draws its zone (research/ffxi-re/notes-zone.md "Drawing", "Closing the open items"):
//
//   fixed-function pieces (no normal map)   per vertex: lit = saturate(vcol x (sun x max(0, N.L) + ambient) + lamps)
//   bump pieces (a normal map)              per pixel: saturate(vcol x (c6 x saturate(n.L_tangent) + c7) + lamps),
//                                           the tangent frame as the game builds it (170B30): per triangle,
//                                           B = the direction u is constant along, T = B x N
//   both                                    rgb = 2 x lit x texture, alpha = saturate(4 x texture alpha x vertex alpha);
//                                           the alpha test (GREATER 0x60) on '_' objects; linear fog by view depth
//   pieces                                  blended (bit 31): SRCALPHA / INVSRCALPHA, no depth writes; two-sided (bit
//                                           29): no cull; else CULL CCW, CW for a mirrored object; in the game's order
//
// Options: per-pixel lighting on every piece (the sun and lamps per pixel, the normal maps' own frame from the UVs).
//
// Vertices and indices are read in the vertex shader from the zone's buffers (root SRVs), so nothing of the input
// assembler's is touched. The caller binds nothing: record() sets the targets, viewport, heaps, root signature and
// pipelines, and leaves them so (putting the list's own back is the caller's).

#include <d3d12.h>
#include "frame.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace hifi::zone
{
	class gpu_zone;

	struct draw_target
	{
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = {}, dsv = {};
		DXGI_FORMAT rtv_format = DXGI_FORMAT_UNKNOWN, dsv_format = DXGI_FORMAT_UNKNOWN;
		UINT samples = 1;
		float width = 0.0f, height = 0.0f;
		float offset = 0.5f; // (the viewport's corner: dgVoodoo's half-pixel for Direct3D 8's pixel centres)
	};

	struct draw_options
	{
		bool per_pixel = false; // sun and lamps per pixel on every piece
	};

	struct draw_stats
	{
		uint32_t objects = 0, draws = 0, triangles = 0, skipped = 0;
	};

	class zone_renderer
	{
	public:
		zone_renderer() = default;
		zone_renderer(const zone_renderer &) = delete;
		zone_renderer &operator=(const zone_renderer &) = delete;
		~zone_renderer();

		// The root signature, the shaders (compiled with d3dcompiler_47), the upload ring; false and why
		bool init(ID3D12Device *device, std::string &why);
		bool ready() const { return _root != nullptr; }
		void release();

		// The frame's objects onto the list. `frame_number` picks the upload ring's slot: each slot is rewritten every
		// kFrames frames, so the GPU must have finished the frame kFrames before (it never runs that far behind).
		draw_stats record(ID3D12GraphicsCommandList *list, const draw_target &t, const frame &f, const draw_options &o, uint64_t frame_number);

	private:
		ID3D12PipelineState *pipeline(const draw_target &t, bool blended, int cull, bool door);

		ID3D12Device *_device = nullptr;
		ID3D12RootSignature *_root = nullptr;
		void *_vs = nullptr, *_ps = nullptr; // (ID3DBlob)
		ID3D12Resource *_ring = nullptr;     // upload: kFrames slots of constants, objects, lamps
		uint8_t *_mapped = nullptr;
		std::unordered_map<uint64_t, ID3D12PipelineState *> _pipelines;
	};
}
