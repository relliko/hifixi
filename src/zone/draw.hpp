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
// Options: per-pixel lighting on every piece (the sun and lamps per pixel, the normal maps' own frame from the UVs;
// the lamps then every one of the frame's whose range reaches the object's box, not only the 4 the game names).
//
// Vertices and indices are read in the vertex shader from the zone's buffers (root SRVs), so nothing of the input
// assembler's is touched. The caller binds nothing: record() sets the targets, viewport, heaps, root signature and
// pipelines, and leaves them so (putting the list's own back is the caller's).

#include <d3d12.h>
#include "frame.hpp"
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
		D3D12_CPU_DESCRIPTOR_HANDLE light_rtv = {}; // the light split's (light_split.hpp), as the second target; 0: none
	};

	struct draw_options
	{
		bool per_pixel = false; // sun and lamps per pixel on every piece
		// The colour maps' extra mip bias (SampleBias, on top of the sampler's -0.5): the texture pack's under FSR
		// (docs/texture-pack/zones.md "The FSR bias"); 0: Sample, today's instruction
		float texture_bias = 0.0f;
	};

	// What a list of objects is drawn with besides them: the camera, the lighting environments (0 the zone's own), the
	// lamps, and the data each object's `zone` names (a frame: its zone and second map; the model actors: one DAT's each)
	struct draw_pass
	{
		const float *view_proj = nullptr; // (16: VIEW x PROJECTION, row vectors)
		const float *camera = nullptr;    // (3)
		const frame_env *envs = nullptr;
		uint32_t env_count = 0;
		const frame_lamp *lamps = nullptr;
		uint32_t lamp_count = 0;
		bool no_bump = false;
		gpu_zone *const *zones = nullptr;
		uint32_t zone_count = 0;
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
		// Objects of a pass of their own (up to kMaxObjects), likewise
		draw_stats record(ID3D12GraphicsCommandList *list, const draw_target &t, const draw_pass &p, const frame_object *objects, uint32_t count,
			const draw_options &o, uint64_t frame_number);

		// The GPU's breadcrumbs, for a GPU crash report: each piece's draw has a write before it (when the GPU has begun
		// every draw before it) and one after (when it has finished it) into memory the CPU still reads once the device
		// is lost; the last few frames' pieces (object, model, piece, lamps) and lamps kept to name them. Lines for the
		// report; empty without breadcrumbs (Direct3D's WriteBufferImmediate not had).
		std::string breadcrumbs() const;

		struct piece_note
		{
			uint32_t object = 0, model = 0, piece = 0, first_index = 0, triangles = 0, flags = 0, object_flags = 0;
			uint32_t texture = 0, normal_map = 0, lamps = 0, zone = 0;
			bool pack = false; // (the texture bound was the texture pack's: gpu_zone::texture_srv)
			bool relief = false; // (M9: the texture pack's height bound at t5: gpu_zone::height_srv)
			char name[16] = {};
			float at[3] = {};
		};
		struct frame_note
		{
			uint32_t tag = 0xFFFFFFFFu; // (the frame number's low 16 bits, as the breadcrumbs carry it)
			bool per_pixel = false;
			std::vector<piece_note> pieces;
			std::vector<frame_lamp> lamps;
		};

	private:
		ID3D12PipelineState *pipeline(const draw_target &t, bool blended, int cull, bool door);
		void make_breadcrumbs(ID3D12Device *device);

		static constexpr int kNotedFrames = 4;

		ID3D12Device *_device = nullptr;
		ID3D12Resource *_crumbs = nullptr;           // (a custom heap in system memory, written back: read after a loss)
		const volatile uint32_t *_crumb_words = nullptr; // [0] begun before draw n, [1] draw n finished: frame << 16 | n; [2] the setup's step
		// (the setup's steps before the first draw, word 2's low bits; written as the command processor passes them)
		static constexpr uint32_t kStepBegun = 1, kStepTargets = 2, kStepHeap = 3, kStepPipeline = 4;
		std::vector<piece_note> _building;
		frame_note _notes[kNotedFrames];
		mutable std::mutex _notes_mutex;
		ID3D12RootSignature *_root = nullptr;
		void *_vs = nullptr, *_ps = nullptr, *_ps_light = nullptr; // (ID3DBlob)
		ID3D12Resource *_ring = nullptr;     // upload: kFrames slots of constants, objects, lamps
		uint8_t *_mapped = nullptr;
		std::unordered_map<uint64_t, ID3D12PipelineState *> _pipelines;
	};

	// Every renderer's breadcrumbs, as the GPU crash report's section has them (tests)
	std::string breadcrumb_report();
}
