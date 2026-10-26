/*
 * One map's zone data on the GPU (zone_gpu.hpp). Everything goes through one upload buffer: the vertices, the
 * indices, then each texture's rows at its copyable footprint (512-byte aligned); one command list copies them all
 * into default-heap resources made in COMMON, and a fence says when it's done.
 */

#include <windows.h>
#include "zone_gpu.hpp"
#include <algorithm>
#include <cstring>

namespace hifi::zone
{
	namespace
	{
		namespace zm = bake::zonemesh;

		template <typename T>
		void drop(T *&p)
		{
			if (p != nullptr)
				p->Release();
			p = nullptr;
		}

		int64_t ticks()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double ms_since(int64_t from)
		{
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			return double(ticks() - from) * 1000.0 / double(f.QuadPart);
		}

		ID3D12Resource *make_buffer(ID3D12Device *device, D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_STATES state)
		{
			D3D12_HEAP_PROPERTIES hp = {};
			hp.Type = heap;
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			d.Width = std::max<uint64_t>(size, 256);
			d.Height = 1;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.SampleDesc.Count = 1;
			d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ID3D12Resource *r = nullptr;
			if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
				return nullptr;
			return r;
		}

		D3D12_RESOURCE_DESC texture_desc(uint32_t w, uint32_t h, DXGI_FORMAT format)
		{
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			d.Width = w;
			d.Height = h;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = format;
			d.SampleDesc.Count = 1;
			return d;
		}

		// A texture's rows as the upload wants them: (data, bytes a row as stored, rows of blocks or texels)
		struct image
		{
			const uint8_t *data;
			uint32_t w, h;
			DXGI_FORMAT format;
			uint32_t row_bytes, rows;
		};
		image image_of(const zm::texture &t)
		{
			if (t.format == zm::kBC2)
				return { t.data.data(), t.w, t.h, DXGI_FORMAT_BC2_UNORM, (t.w / 4) * 16, t.h / 4 };
			return { t.data.data(), t.w, t.h, DXGI_FORMAT_B8G8R8A8_UNORM, t.w * 4, t.h };
		}
	}

	gpu_zone::~gpu_zone()
	{
		release();
	}

	void gpu_zone::release()
	{
		for (ID3D12Resource *&t : _textures)
			drop(t);
		_textures.clear();
		drop(_vb);
		drop(_ib);
		drop(_upload);
		drop(_heap);
		drop(_list);
		drop(_allocator);
		drop(_fence);
		_device = nullptr;
		_ready = false;
		_bytes = 0;
		_upload_ms = 0.0;
		_z = zm::zone();
	}

	bool gpu_zone::upload(ID3D12Device *device, ID3D12CommandQueue *queue, zm::zone &&z, std::string &why)
	{
		release();
		_started = ticks();
		_device = device;
		_z = std::move(z);
		const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
		auto failed = [&](const char *what) {
			why = what;
			release();
			return false;
		};

		// The images: the zone's textures, its normal maps, then white and flat (B8G8R8A8: FFFFFFFF; 128 128 255 0)
		static const uint32_t kWhite = 0xFFFFFFFFu, kFlat = 0x008080FFu;
		std::vector<image> images;
		for (const zm::texture &t : _z.textures)
			images.push_back(image_of(t));
		for (const zm::normal_map &n : _z.normal_maps)
			images.push_back({ reinterpret_cast<const uint8_t *>(n.texels.data()), n.w, n.h, DXGI_FORMAT_B8G8R8A8_UNORM, n.w * 4, n.h });
		images.push_back({ reinterpret_cast<const uint8_t *>(&kWhite), 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 4, 1 });
		images.push_back({ reinterpret_cast<const uint8_t *>(&kFlat), 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 4, 1 });

		// Where each goes in the upload buffer
		const uint64_t vbytes = uint64_t(_z.vertices.size()) * sizeof(zm::vertex), ibytes = uint64_t(_z.indices.size()) * 4;
		uint64_t at = (vbytes + ibytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
		std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> spots(images.size());
		for (size_t i = 0; i < images.size(); ++i)
		{
			const D3D12_RESOURCE_DESC d = texture_desc(images[i].w, images[i].h, images[i].format);
			UINT64 size = 0;
			device->GetCopyableFootprints(&d, 0, 1, at, &spots[i], nullptr, nullptr, &size);
			at = (spots[i].Offset + size + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
		}
		_upload = make_buffer(device, D3D12_HEAP_TYPE_UPLOAD, at, D3D12_RESOURCE_STATE_GENERIC_READ);
		_vb = make_buffer(device, D3D12_HEAP_TYPE_DEFAULT, vbytes, D3D12_RESOURCE_STATE_COMMON);
		_ib = make_buffer(device, D3D12_HEAP_TYPE_DEFAULT, ibytes, D3D12_RESOURCE_STATE_COMMON);
		if (_upload == nullptr || _vb == nullptr || _ib == nullptr)
			return failed("the zone's buffers couldn't be made (video memory?)");
		_bytes = vbytes + ibytes;
		for (size_t i = 0; i < images.size(); ++i)
		{
			D3D12_HEAP_PROPERTIES hp = {};
			hp.Type = D3D12_HEAP_TYPE_DEFAULT;
			const D3D12_RESOURCE_DESC d = texture_desc(images[i].w, images[i].h, images[i].format);
			ID3D12Resource *t = nullptr;
			if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t))))
				return failed("a zone texture couldn't be made (video memory?)");
			_textures.push_back(t);
			_bytes += device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
		}

		// The data into the upload buffer
		uint8_t *mapped = nullptr;
		const D3D12_RANGE none = { 0, 0 };
		if (FAILED(_upload->Map(0, &none, reinterpret_cast<void **>(&mapped))))
			return failed("the upload buffer couldn't be mapped");
		std::memcpy(mapped, _z.vertices.data(), size_t(vbytes));
		std::memcpy(mapped + vbytes, _z.indices.data(), size_t(ibytes));
		for (size_t i = 0; i < images.size(); ++i)
		{
			const image &im = images[i];
			for (uint32_t row = 0; row < im.rows; ++row)
				std::memcpy(mapped + spots[i].Offset + uint64_t(row) * spots[i].Footprint.RowPitch, im.data + size_t(row) * im.row_bytes, im.row_bytes);
		}
		_upload->Unmap(0, nullptr);

		// The copies, on the given queue
		if (FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&_allocator))) ||
			FAILED(device->CreateCommandList(0, type, _allocator, nullptr, IID_PPV_ARGS(&_list))) ||
			FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence))))
			return failed("the upload's command list couldn't be made");
		_list->CopyBufferRegion(_vb, 0, _upload, 0, vbytes);
		_list->CopyBufferRegion(_ib, 0, _upload, vbytes, ibytes);
		for (size_t i = 0; i < images.size(); ++i)
		{
			D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
			dst.pResource = _textures[i];
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.pResource = _upload;
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint = spots[i];
			_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		}
		if (FAILED(_list->Close()))
			return failed("the upload's command list couldn't be closed");
		ID3D12CommandList *lists[] = { _list };
		queue->ExecuteCommandLists(1, lists);
		if (FAILED(queue->Signal(_fence, 1)))
			return failed("the upload's fence couldn't be signalled");

		// The descriptors
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = UINT(images.size());
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_heap))))
			return failed("the zone's descriptor heap couldn't be made");
		_srv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		D3D12_CPU_DESCRIPTOR_HANDLE h = _heap->GetCPUDescriptorHandleForHeapStart();
		for (size_t i = 0; i < images.size(); ++i)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
			v.Format = images[i].format;
			v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			v.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(_textures[i], &v, h);
			h.ptr += _srv_step;
		}

		// What drawing doesn't need dropped (the sizes stay)
		_vertex_count = uint32_t(_z.vertices.size());
		_index_count = uint32_t(_z.indices.size());
		std::vector<zm::vertex>().swap(_z.vertices);
		std::vector<uint32_t>().swap(_z.indices);
		for (zm::texture &t : _z.textures)
			std::vector<uint8_t>().swap(t.data);
		for (zm::normal_map &n : _z.normal_maps)
			std::vector<uint32_t>().swap(n.texels);
		return true;
	}

	bool gpu_zone::ready()
	{
		if (_ready)
			return true;
		if (_fence == nullptr || _fence->GetCompletedValue() < 1)
			return false;
		_ready = true;
		_upload_ms = ms_since(_started);
		drop(_upload);
		drop(_list);
		drop(_allocator);
		return true;
	}

	bool gpu_zone::wait(uint32_t ms)
	{
		if (_fence == nullptr)
			return false;
		if (_fence->GetCompletedValue() < 1)
		{
			HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			_fence->SetEventOnCompletion(1, e);
			WaitForSingleObject(e, ms);
			CloseHandle(e);
		}
		return ready();
	}

	D3D12_VERTEX_BUFFER_VIEW gpu_zone::vertex_view() const
	{
		D3D12_VERTEX_BUFFER_VIEW v = {};
		if (_vb != nullptr)
		{
			v.BufferLocation = _vb->GetGPUVirtualAddress();
			v.SizeInBytes = _vertex_count * UINT(sizeof(zm::vertex));
			v.StrideInBytes = UINT(sizeof(zm::vertex));
		}
		return v;
	}

	D3D12_INDEX_BUFFER_VIEW gpu_zone::index_view() const
	{
		D3D12_INDEX_BUFFER_VIEW v = {};
		if (_ib != nullptr)
		{
			v.BufferLocation = _ib->GetGPUVirtualAddress();
			v.SizeInBytes = _index_count * 4;
			v.Format = DXGI_FORMAT_R32_UINT;
		}
		return v;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE gpu_zone::texture_srv(uint32_t texture) const
	{
		D3D12_GPU_DESCRIPTOR_HANDLE h = _heap->GetGPUDescriptorHandleForHeapStart();
		const size_t white = _z.textures.size() + _z.normal_maps.size();
		h.ptr += uint64_t(texture < _z.textures.size() ? texture : white) * _srv_step;
		return h;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE gpu_zone::normal_srv(uint32_t normal_map) const
	{
		D3D12_GPU_DESCRIPTOR_HANDLE h = _heap->GetGPUDescriptorHandleForHeapStart();
		const size_t flat = _z.textures.size() + _z.normal_maps.size() + 1;
		h.ptr += uint64_t(normal_map < _z.normal_maps.size() ? _z.textures.size() + normal_map : flat) * _srv_step;
		return h;
	}
}
