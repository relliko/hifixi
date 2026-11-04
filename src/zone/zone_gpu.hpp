#pragma once

// One map's zone data on the GPU, for the native zone renderer (src/bake/zonemesh: what's in it). Uploaded on a copy
// queue of its own, never on dgVoodoo's lists: the resources are made in COMMON, written by the copy queue, decay back
// to COMMON when it's done, and are promoted on first use by whichever queue draws with them (a buffer to vertex or
// index reads, a texture to a pixel shader resource), so nothing but the fence is shared with the game's queue.
//
//   vertices      one buffer, 48 bytes a vertex (zonemesh::vertex)
//   indices       one buffer, 32-bit, absolute
//   textures      DXT3 as BC2_UNORM, palettized as B8G8R8A8_UNORM, one mip (the game draws the zone without mips:
//                 MIPFILTER NONE), rows as stored; a bigger texture from an XIPivot overlay also gets its smaller
//                 levels down to the game's size (zonemesh::texture::mips, made on the loader's thread)
//   normal maps   B8G8R8A8_UNORM, as the game makes them (zonemesh::sobel)
//   descriptors   a shader-visible heap of its own: the textures, then the normal maps, then two stand-ins: white (a
//                 piece without a texture) and flat (one without a normal map); for a zone read in the texture pack's
//                 mode with keys (zonemesh::zone::pack, not overlaid), then a pack region of one slot a texture
//
// The CPU side keeps what drawing needs (models, pieces, placements); the vertex and texture data are dropped once
// they're on their way.
//
// The texture pack's region (docs/texture-pack/zones.md "The gpu_zone's pack region and the write-once slot rule"):
// each slot starts as the game texture's own view, and is written once at most in this gpu_zone's life, by its owner's
// glue (src/zone/zonepack: stream::write_view of a ready ticket), before its word says drawn; nothing written after.
// So no list in flight ever reads a slot that is written: until drawn texture_srv names the game's slot, after it no
// one writes. A word: none (no key, or not asked yet), asked, drawn (texture_srv names the pack slot), spent (missed,
// gone, cleared, the pack off in this visit: never asked again). The glue's stream calls live in zonepack; this file
// keeps only the data (the owner, the tickets: plain numbers) and calls the glue's drop hook when it's let go.
//
// The pack's heights (M9, zones.md "Pack normals"): a second block of the pack region, one slot a texture after the
// colour's, by the same write-once rule and with words of their own. Each starts as the flat stand-in's view and takes a
// ready height record's (BC4) view; with it its relief scale (4 x span x the map's width over the game texture's: the
// shader's Sobel in the game's own units). height_srv names it while its word says drawn, heights are on and compare is
// off; the zone shader then binds it at t5 in place of the normal map (draw.cpp kPackRelief). An entry's colour and its
// height are asked and settled apart: an entry with a height and no colour (a materials-only pack) draws the game's
// texture with the pack's relief.

#include <d3d12.h>
#include "../bake/zonemesh.hpp"
#include "../pack/stream.hpp" // (the owner and ticket types only: nothing of the streamer's is called here)
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hifi::zone
{
	class gpu_zone
	{
	public:
		gpu_zone() = default;
		gpu_zone(const gpu_zone &) = delete;
		gpu_zone &operator=(const gpu_zone &) = delete;
		~gpu_zone();

		// The data made into resources and their copies sent on `queue` (a copy or direct queue of the device's),
		// signalling a fence of its own; z's big arrays are freed. False and why when something couldn't be made
		// (nothing is left half made).
		bool upload(ID3D12Device *device, ID3D12CommandQueue *queue, bake::zonemesh::zone &&z, std::string &why);
		// Whether the copies have finished (the upload buffer is let go then)
		bool ready();
		// Waits for them (the tests)
		bool wait(uint32_t ms);
		void release();

		const bake::zonemesh::zone &data() const { return _z; }
		ID3D12Resource *vertices() const { return _vb; }
		ID3D12Resource *indices() const { return _ib; }
		D3D12_VERTEX_BUFFER_VIEW vertex_view() const;
		D3D12_INDEX_BUFFER_VIEW index_view() const;
		ID3D12DescriptorHeap *heap() const { return _heap; }
		// A piece's texture and normal map descriptors (the stand-ins for none). texture_srv: the texture pack's slot when
		// its word says drawn and compare is off (pack: set whether it was), else the game's; game_srv: the game's always
		// (the effects' zone meshes and the zone shadows' cut-outs: zones.md "Selection")
		D3D12_GPU_DESCRIPTOR_HANDLE texture_srv(uint32_t texture, bool *pack = nullptr) const;
		D3D12_GPU_DESCRIPTOR_HANDLE game_srv(uint32_t texture) const;
		D3D12_GPU_DESCRIPTOR_HANDLE normal_srv(uint32_t normal_map) const;
		// Bytes on the GPU (the texture pack's textures its tickets hold counted in: one shared by two owners in each), and
		// how long the copies took (ms from upload to ready; 0 until then)
		uint64_t bytes() const { return _bytes + _pack.bytes; }
		double upload_ms() const { return _upload_ms; }

		// ---- The texture pack's region (zones.md; the game's thread)
		enum pack_word : uint8_t
		{
			kPackNone = 0,  // (no key, or not asked yet)
			kPackAsked = 1, // (a ticket out)
			kPackDrawn = 2, // (its slot holds the pack's view: texture_srv names it)
			kPackSpent = 3, // (missed, gone, cleared, or the pack off in this visit: never asked again)
		};
		// What the glue keeps here as plain data (src/zone/zonepack): the owner (0: none), the epoch it asked in, a
		// ticket a texture, the bytes of the ready ones, and whether this visit is done (its owner dropped: never asked
		// again)
		struct pack_data
		{
			hifi::pack::stream::owner owner = 0;
			uint32_t epoch = 0;
			std::vector<hifi::pack::stream::ticket> tickets;
			std::vector<hifi::pack::stream::ticket> height_tickets; // (M9: a texture's height record's)
			uint64_t bytes = 0;
			bool done = false;
		};
		// Whether it has a pack region (read in pack mode from the game's own file, with textures)
		bool has_pack() const { return _pack_words != nullptr; }
		pack_data &pack() { return _pack; }
		const pack_data &pack() const { return _pack; }
		uint8_t pack_word(uint32_t texture) const;
		// (release: after the view is written, drawn is stored last)
		void set_pack_word(uint32_t texture, uint8_t word);
		// The texture's pack slot for stream::write_view (CPU side of the shader-visible heap: written, never read)
		D3D12_CPU_DESCRIPTOR_HANDLE pack_cpu(uint32_t texture) const;
		// The glue says it wrote a slot (the test log's 'w')
		void pack_written(uint32_t texture) const;
		// ---- The pack's heights (M9): the same words, a block of their own (the test log's events for them carry
		// kHeightLog in the texture's number)
		static constexpr uint32_t kHeightLog = 0x80000000u;
		uint8_t height_word(uint32_t texture) const;
		void set_height_word(uint32_t texture, uint8_t word);
		D3D12_CPU_DESCRIPTOR_HANDLE height_cpu(uint32_t texture) const;
		// (before its word says drawn) The relief scale the shader multiplies its Sobel by, and the slot's write logged
		void height_written(uint32_t texture, float relief);
		// The texture's height view when its word says drawn, heights are on and compare is off (relief: its scale), else
		// a null handle (ptr 0) and relief 0
		D3D12_GPU_DESCRIPTOR_HANDLE height_srv(uint32_t texture, float *relief) const;
		// Pack normals drawn at all ([textures] height, every gpu_zone; on by default: no zone has a height until the glue
		// asks one)
		static void set_heights(bool on);
		static bool heights();
		// Its pack textures let go now (the drop hook, when it has an owner): before a hand-over to retire::later, so the
		// textures and the zone are freed at the same fence. release() and the destructor call it too
		void pack_let_go();
		// Counts of the words (status lines)
		struct pack_counts
		{
			uint32_t textures = 0, keyed = 0, asked = 0, drawn = 0, spent = 0;
			uint32_t heights_asked = 0, heights_drawn = 0, heights_spent = 0; // (M9: the height words')
		};
		pack_counts pack_summary() const;
		// /hifi textures compare: the game's own textures drawn where the pack's are ready (every gpu_zone; nothing freed)
		static void set_compare(bool on);
		static bool comparing();
		// The glue's drop hook (zonepack: stream::drop_owner, every word spent, the owner 0, out of its list), run by
		// pack_let_go and release() when the zone has an owner; nullptr: none (nothing gets one then)
		static void set_pack_drop(void (*fn)(gpu_zone &));
		// (tests) Each pack slot write ('w') and word change ('a' asked, 'd' drawn, 's' spent); nullptr in the plugin
		static void set_test_log(void (*fn)(const gpu_zone &, uint32_t texture, char what));

	private:
		bake::zonemesh::zone _z;
		ID3D12Device *_device = nullptr;
		ID3D12Resource *_vb = nullptr, *_ib = nullptr, *_upload = nullptr;
		std::vector<ID3D12Resource *> _textures; // (textures, normal maps, white, flat)
		ID3D12DescriptorHeap *_heap = nullptr;
		UINT _srv_step = 0;
		ID3D12Fence *_fence = nullptr;
		ID3D12CommandAllocator *_allocator = nullptr;
		ID3D12GraphicsCommandList *_list = nullptr;
		uint64_t _bytes = 0;
		int64_t _started = 0;
		double _upload_ms = 0.0;
		bool _ready = false;
		// The copy on the copy queue: 0 none running, 1 sent without its fence signalled (its end never known), 2 sent
		// with it (release waits for it)
		uint32_t _copying = 0;
		uint32_t _vertex_count = 0, _index_count = 0;
		// The texture pack's region: its first slot (the images' count), a word a texture (nullptr: no region)
		uint32_t _pack_base = 0;
		std::unique_ptr<std::atomic<uint8_t>[]> _pack_words;
		// (M9: the height block's words and relief scales; its slots follow the colour's: _pack_base + textures + i)
		std::unique_ptr<std::atomic<uint8_t>[]> _height_words;
		std::unique_ptr<float[]> _relief;
		pack_data _pack;
	};
}
