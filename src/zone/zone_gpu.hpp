#pragma once

// One map's zone data on the GPU, for the native zone renderer (src/bake/zonemesh: what's in it). Uploaded on a copy
// queue of its own, never on dgVoodoo's lists: the resources are made in COMMON, written by the copy queue, decay back
// to COMMON when it's done, and are promoted on first use by whichever queue draws with them (a buffer to vertex or
// index reads, a texture to a pixel shader resource), so nothing but the fence is shared with the game's queue.
//
//   vertices      one buffer, 48 bytes a vertex (zonemesh::vertex)
//   indices       one buffer, 32-bit, absolute
//   textures      DXT3 as BC2_UNORM, palettized as B8G8R8A8_UNORM, one mip (the game draws the zone without mips:
//                 MIPFILTER NONE), rows as stored
//   normal maps   B8G8R8A8_UNORM, as the game makes them (zonemesh::sobel)
//   descriptors   a shader-visible heap of its own: the textures, then the normal maps, then two stand-ins: white (a
//                 piece without a texture) and flat (one without a normal map)
//
// The CPU side keeps what drawing needs (models, pieces, placements); the vertex and texture data are dropped once
// they're on their way.

#include <d3d12.h>
#include "../bake/zonemesh.hpp"
#include <cstdint>
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
		// A piece's texture and normal map descriptors (the stand-ins for none)
		D3D12_GPU_DESCRIPTOR_HANDLE texture_srv(uint32_t texture) const;
		D3D12_GPU_DESCRIPTOR_HANDLE normal_srv(uint32_t normal_map) const;
		// Bytes on the GPU, and how long the copies took (ms from upload to ready; 0 until then)
		uint64_t bytes() const { return _bytes; }
		double upload_ms() const { return _upload_ms; }

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
		uint32_t _vertex_count = 0, _index_count = 0;
	};
}
