#pragma once

// The world part's passes drawn with Direct3D 12 (PLAN-own-frame.md): the sky dome, the names (their glyphs into the
// world with its depth, or over the upscaled screen after FSR), the Mog House layout grid, each from its ring
// (frames.hpp) with the game's look:
//   - the dome: 03D640's strips (dome.hpp), each a triangle strip of its vertex colours exactly (the state block's
//     SELECTARG1 DIFFUSE for colour and alpha), no blending, no fog, no depth writes, no culling that matters (the
//     camera is inside it);
//   - the names: each glyph quad as 086210 handed it to the device (XYZRHW in A's pixels), the page's texel bilinear
//     (MIN/MAG LINEAR, WRAP), colour = texture x vertex x 2 and alpha = texture x vertex x 4 (MODULATE2X / MODULATE4X,
//     saturated), alpha test GREATER the glyph's ALPHAREF (0 letters, 0x60 icons), blended SRCALPHA / INVSRCALPHA,
//     depth tested LESSEQUAL against the world's depth with the name point's z, which it writes where the test passes
//     (notes-ui-draw.md "The names", the depth contract); over the screen (after the upscale) the same at the screen's
//     resolution, the jitter taken out, the depth test against A's depth read at the matching pixel, no depth written;
//   - the grid: each outline as a 4-segment line loop, its colour as is, depth off unless the game kept it.
//
// No descriptor heaps are set for the draws into the world (vertices, glyphs and the font pages are root SRVs, the pages
// RGBA8 in a buffer, filtered in the shader): dgVoodoo's own heaps stay bound. Drawing over the screen reads the depth
// through a table in a heap of ours; the caller puts dgVoodoo's heaps back after (the upscale does). Each record leaves
// the list's root signature, pipeline, topology, viewport, scissor and targets changed: the caller binds dgVoodoo's again.
//
// Recording: dgVoodoo's thread. init/release/set_pages: off it (the game's thread).

#include <d3d12.h>
#include "frames.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hifi::world
{
	// Where a pass draws: the target's view (and the depth's when there is one), formats, samples and size
	struct target
	{
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = {}, dsv = {};
		DXGI_FORMAT rtv_format = DXGI_FORMAT_UNKNOWN, dsv_format = DXGI_FORMAT_UNKNOWN; // (dsv_format unknown: no depth)
		UINT samples = 1;
		UINT width = 0, height = 0;
	};

	// The two name pages, RGBA8 (bytes R, G, B, A; rows top down; the game's alpha, 0x80 = 1 under MODULATE4X... as decoded)
	struct font_pages
	{
		uint32_t w[2] = {}, h[2] = {};
		std::vector<uint32_t> texels[2]; // (R | G << 8 | B << 16 | A << 24)
	};

	// Drawing over the screen: A's depth (its view for a table, made by the caller in heap()), A's size, the jitter
	struct over_screen
	{
		D3D12_GPU_DESCRIPTOR_HANDLE depth = {}; // (an SRV of A's depth plane 0, in heap())
		UINT a_width = 0, a_height = 0;          // A as dgVoodoo has it
	};

	class renderer
	{
	public:
		renderer() = default;
		renderer(const renderer &) = delete;
		renderer &operator=(const renderer &) = delete;
		~renderer();

		// The shaders (d3dcompiler_47), root signatures, upload rings, the heap for the depth's views; false and why
		bool init(ID3D12Device *device, std::string &why);
		bool ready() const { return _root != nullptr; }
		void release();
		// The name pages into a buffer of ours (made again for new ones); false and why
		bool set_pages(const font_pages &pages, std::string &why);
		bool has_pages() const { return _pages != nullptr; }

		// Each pass onto the list; `frame` picks its upload ring's slot (each rewritten every kSlots frames: the GPU must be
		// done with the frame kSlots before). False and why when nothing was drawn.
		bool sky(ID3D12GraphicsCommandList *list, const target &t, const sky_frame &f, uint64_t frame, std::string &why);
		bool names(ID3D12GraphicsCommandList *list, const target &t, const names_frame &f, uint64_t frame, std::string &why);
		bool names_over(ID3D12GraphicsCommandList *list, const target &t, const over_screen &o, const names_frame &f, uint64_t frame, std::string &why);
		bool grid(ID3D12GraphicsCommandList *list, const target &t, const grid_frame &f, uint64_t frame, std::string &why);

		// The shader-visible heap of ours drawing over the screen reads A's depth from, and its slot for a frame (CPU and GPU)
		ID3D12DescriptorHeap *heap() const { return _heap; }
		D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu(uint64_t frame) const;
		D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu(uint64_t frame) const;

	private:
		enum kind : uint32_t { sky_kind, names_kind, over_kind, grid_flat, grid_depth };
		ID3D12PipelineState *pipeline(kind k, const target &t);
		bool ring(uint32_t which, size_t bytes, std::string &why);

		ID3D12Device *_device = nullptr;
		ID3D12RootSignature *_root = nullptr, *_root_over = nullptr;
		void *_code[6] = {}; // (ID3DBlob: sky vs/ps, names vs/ps, names over ps, grid vs/ps share sky's ps)
		ID3D12Resource *_rings[3] = {}; // upload: sky, names, grid
		uint8_t *_mapped[3] = {};
		ID3D12Resource *_pages = nullptr;
		uint32_t _page_w[2] = {}, _page_h[2] = {}, _page1 = 0;
		ID3D12DescriptorHeap *_heap = nullptr;
		UINT _heap_step = 0;
		std::unordered_map<uint64_t, ID3D12PipelineState *> _pipelines;
	};

	// Byte sizes of one ring slot (each ring holds kSlots of them)
	constexpr size_t kSkySlotBytes = 256 * 1024, kNamesSlotBytes = kMaxGlyphs * 128, kGridSlotBytes = kMaxGridLines * 64;
}
