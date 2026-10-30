#include "gpu.hpp"
#include <algorithm>

namespace hifi::d3d8
{
	namespace
	{
		constexpr UINT64 kPageSize = 8ull << 20;
		constexpr uint32_t kSrvHeap = 3 * 32768;   // shader visible, a third a frame set
		constexpr uint32_t kSamplerHeap = 2048;    // (Direct3D 12's limit for a shader-visible sampler heap)
		constexpr uint32_t kSamplerTables = kSamplerHeap / kMaxStages;

		ID3D12DescriptorHeap *make_heap(ID3D12Device *d, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT n, bool visible)
		{
			D3D12_DESCRIPTOR_HEAP_DESC hd = {};
			hd.Type = type;
			hd.NumDescriptors = n;
			hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			ID3D12DescriptorHeap *h = nullptr;
			return SUCCEEDED(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&h))) ? h : nullptr;
		}
	}

	D3D12_CPU_DESCRIPTOR_HANDLE gpu::pool::take()
	{
		uint32_t i;
		if (!free_list.empty())
		{
			i = free_list.back();
			free_list.pop_back();
		}
		else if (next < size)
			i = next++;
		else
		{
			log_once(2, "out of CPU descriptors (a heap of %u)", size);
			i = size - 1;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += SIZE_T(i) * step;
		return h;
	}

	void gpu::pool::give(D3D12_CPU_DESCRIPTOR_HANDLE h)
	{
		if (h.ptr == 0)
			return;
		free_list.push_back(uint32_t((h.ptr - heap->GetCPUDescriptorHandleForHeapStart().ptr) / step));
	}

	bool gpu::init(IDXGIFactory2 *f, IDXGIAdapter1 *adapter, bool debug, std::string &why)
	{
		factory = f;
		factory->AddRef();
		if (debug)
		{
			ID3D12Debug *d = nullptr;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d))))
			{
				d->EnableDebugLayer();
				d->Release();
			}
		}
		HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
		if (FAILED(hr))
		{
			why = "no Direct3D 12 device (feature level 11_0) on the adapter";
			return false;
		}
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
		{
			why = "no queue or fence";
			return false;
		}
		fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (debug && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&info))))
			info->SetMuteDebugOutput(FALSE);
		for (frame_set &s : frames)
			if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))))
			{
				why = "no command allocator";
				return false;
			}

		// Heaps: the CPU-only pools for views, the shader-visible ones the draws' tables are copied into
		rtv_pool.heap = make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtv_pool.size = 4096, false);
		dsv_pool.heap = make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsv_pool.size = 1024, false);
		srv_pool.heap = make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, srv_pool.size = 65536, false);
		srv_heap = make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeap, true);
		sampler_heap = make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeap, true);
		if (rtv_pool.heap == nullptr || dsv_pool.heap == nullptr || srv_pool.heap == nullptr || srv_heap == nullptr || sampler_heap == nullptr)
		{
			why = "no descriptor heaps";
			return false;
		}
		rtv_pool.step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		dsv_pool.step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
		srv_pool.step = srv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		sampler_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
		for (uint32_t i = 0; i < kFrames; ++i)
		{
			frames[i].srv_begin = i * (kSrvHeap / kFrames);
			frames[i].srv_end = (i + 1) * (kSrvHeap / kFrames);
		}
		srv_next = frames[0].srv_begin;
		sampler_keys.resize(kSamplerTables);

		// The views of empty stages: 2D, cube and volume, reading white (as dgVoodoo's: a stage with no texture, or
		// a pixel shader's tex of one, gives 1 1 1 1)
		const D3D12_SRV_DIMENSION dims[3] = { D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURECUBE, D3D12_SRV_DIMENSION_TEXTURE3D };
		for (int i = 0; i < 3; ++i)
		{
			null_views[i] = srv_pool.take();
			D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			sd.ViewDimension = dims[i];
			sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
				D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
			sd.Texture2D.MipLevels = 1;
			D3D12_RESOURCE_DESC rd = {};
			rd.Dimension = i == 2 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			rd.Width = rd.Height = 1;
			rd.DepthOrArraySize = UINT16(i == 1 ? 6 : 1);
			rd.MipLevels = 1;
			rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			rd.SampleDesc.Count = 1;
			D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
			// (its contents never read: every component is forced to 1 by the view)
			if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&white[i]))))
			{
				why = "no texture for empty stages";
				return false;
			}
			device->CreateShaderResourceView(white[i], &sd, null_views[i]);
		}

		// The root signature
		D3D12_DESCRIPTOR_RANGE ranges[2] = {};
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = kMaxStages;
		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
		ranges[1].NumDescriptors = kMaxStages;
		D3D12_ROOT_PARAMETER params[4] = {};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[0].Descriptor.ShaderRegister = 0;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[1].Descriptor.ShaderRegister = 1;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[2].DescriptorTable.NumDescriptorRanges = 1;
		params[2].DescriptorTable.pDescriptorRanges = &ranges[0];
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[3].DescriptorTable.NumDescriptorRanges = 1;
		params[3].DescriptorTable.pDescriptorRanges = &ranges[1];
		params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_ROOT_SIGNATURE_DESC rd = {};
		rd.NumParameters = 4;
		rd.pParameters = params;
		rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
		ID3DBlob *blob = nullptr, *error = nullptr;
		hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
		if (SUCCEEDED(hr))
			hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
		release(blob);
		release(error);
		if (FAILED(hr))
		{
			why = "no root signature";
			return false;
		}
		open_list();
		return true;
	}

	void gpu::shutdown()
	{
		if (device == nullptr)
			return;
		if (cmd != nullptr)
		{
			cmd->Close();
			wait_idle();
		}
		run_deferred();
		for (frame_set &s : frames)
		{
			for (page &p : s.pages)
				release(p.buffer);
			s.pages.clear();
			release(s.allocator);
		}
		for (page &p : spare_pages)
			release(p.buffer);
		spare_pages.clear();
		for (readback &r : spare_readbacks)
			release(r.buffer);
		spare_readbacks.clear();
		release(cmd);
		for (ID3D12Resource *&w : white)
			release(w);
		release(root);
		release(srv_heap);
		release(sampler_heap);
		release(rtv_pool.heap);
		release(dsv_pool.heap);
		release(srv_pool.heap);
		release(fence);
		release(info);
		release(queue);
		release(device);
		release(factory);
		if (fence_event != nullptr)
			CloseHandle(fence_event);
		fence_event = nullptr;
	}

	void gpu::open_list()
	{
		frame_set &s = frames[current];
		if (cmd == nullptr)
		{
			device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator, nullptr, IID_PPV_ARGS(&cmd));
		}
		else
			cmd->Reset(s.allocator, nullptr);
		epoch++;
		draws_in_list = 0;
		// (heaps first: HiFi XI's hooks follow them from here, and put them back after drawing into the frame)
		ID3D12DescriptorHeap *heaps[2] = { srv_heap, sampler_heap };
		cmd->SetDescriptorHeaps(2, heaps);
		cmd->SetGraphicsRootSignature(root);
		if (on_reset)
			on_reset();
	}

	void gpu::submit(bool wait)
	{
		flush_barriers();
		cmd->Close();
		ID3D12CommandList *lists[1] = { cmd };
		queue->ExecuteCommandLists(1, lists);
		queue->Signal(fence, ++fence_value);
		if (wait)
			wait_for(fence_value);
		open_list();
	}

	bool gpu::done(UINT64 value) const
	{
		return fence->GetCompletedValue() >= value;
	}

	void gpu::wait_for(UINT64 value)
	{
		if (fence->GetCompletedValue() >= value)
			return;
		fence->SetEventOnCompletion(value, fence_event);
		WaitForSingleObject(fence_event, INFINITE);
	}

	void gpu::wait_idle()
	{
		queue->Signal(fence, ++fence_value);
		wait_for(fence_value);
	}

	void gpu::run_deferred()
	{
		const UINT64 completed = fence->GetCompletedValue();
		size_t kept = 0;
		for (size_t i = 0; i < deferred_list.size(); ++i)
		{
			deferred &d = deferred_list[i];
			if (d.fence <= completed)
			{
				if (d.object != nullptr)
					d.object->Release();
				if (d.fn)
					d.fn();
			}
			else
				deferred_list[kept++] = std::move(d);
		}
		deferred_list.resize(kept);
	}

	void gpu::drain_messages()
	{
		if (info == nullptr)
			return;
		const UINT64 n = info->GetNumStoredMessages();
		for (UINT64 i = 0; i < n; ++i)
		{
			SIZE_T size = 0;
			info->GetMessage(i, nullptr, &size);
			std::vector<uint8_t> buffer(size);
			D3D12_MESSAGE *m = reinterpret_cast<D3D12_MESSAGE *>(buffer.data());
			if (FAILED(info->GetMessage(i, m, &size)) || m->Severity > D3D12_MESSAGE_SEVERITY_WARNING)
				continue;
			// (clears to colours other than one given at creation: only slower, and the game's clear colour is the fog's)
			if (std::strstr(m->pDescription, "clear value") != nullptr)
				continue;
			debug_messages++;
			log_once(m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR ? 2 : 1, "Direct3D 12 debug layer: %s", m->pDescription);
		}
		info->ClearStoredMessages();
	}

	void gpu::end_frame()
	{
		drain_messages();
		// (the caller has submitted: the list is open and empty, on the frame's allocator)
		frame_set &s = frames[current];
		s.fence = fence_value;
		frame_number++;
		current = (current + 1) % kFrames;
		frame_set &n = frames[current];
		wait_for(n.fence);
		run_deferred();
		for (page &p : n.pages)
		{
			p.used = 0;
			if (p.size == kPageSize)
				spare_pages.push_back(p);
			else
				release(p.buffer);
		}
		n.pages.clear();
		srv_next = n.srv_begin;
		// The list was reset on the old set's allocator: closed empty and reset again on this one's
		cmd->Close();
		n.allocator->Reset();
		open_list();
	}

	gpu::page gpu::new_page(UINT64 size)
	{
		if (size <= kPageSize && !spare_pages.empty())
		{
			page p = spare_pages.back();
			spare_pages.pop_back();
			return p;
		}
		page p;
		p.size = std::max(size, kPageSize);
		D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = p.size;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&p.buffer))))
		{
			log(2, "upload memory of %llu bytes couldn't be made", static_cast<unsigned long long>(p.size));
			p.size = 0;
			return p;
		}
		D3D12_RANGE none = { 0, 0 };
		p.buffer->Map(0, &none, reinterpret_cast<void **>(&p.cpu));
		return p;
	}

	upload_alloc gpu::upload(UINT64 size, UINT64 align)
	{
		frame_set &s = frames[current];
		page *p = s.pages.empty() ? nullptr : &s.pages.back();
		UINT64 at = p != nullptr ? align_up64(p->used, align) : 0;
		if (p == nullptr || at + size > p->size)
		{
			s.pages.push_back(new_page(size));
			p = &s.pages.back();
			at = 0;
		}
		upload_alloc a;
		if (p->buffer == nullptr)
			return a;
		p->used = at + size;
		a.cpu = p->cpu + at;
		a.gpu = p->buffer->GetGPUVirtualAddress() + at;
		a.resource = p->buffer;
		a.offset = at;
		return a;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE gpu::new_rtv() { return rtv_pool.take(); }
	D3D12_CPU_DESCRIPTOR_HANDLE gpu::new_dsv() { return dsv_pool.take(); }
	D3D12_CPU_DESCRIPTOR_HANDLE gpu::new_srv() { return srv_pool.take(); }
	void gpu::free_rtv(D3D12_CPU_DESCRIPTOR_HANDLE h) { rtv_pool.give(h); }
	void gpu::free_dsv(D3D12_CPU_DESCRIPTOR_HANDLE h) { dsv_pool.give(h); }
	void gpu::free_srv(D3D12_CPU_DESCRIPTOR_HANDLE h) { srv_pool.give(h); }

	D3D12_CPU_DESCRIPTOR_HANDLE gpu::null_srv(D3D12_SRV_DIMENSION dim) const
	{
		return dim == D3D12_SRV_DIMENSION_TEXTURECUBE ? null_views[1] : dim == D3D12_SRV_DIMENSION_TEXTURE3D ? null_views[2] : null_views[0];
	}

	D3D12_GPU_DESCRIPTOR_HANDLE gpu::srv_table(const D3D12_CPU_DESCRIPTOR_HANDLE *views, uint32_t n)
	{
		frame_set &s = frames[current];
		if (srv_next + n > s.srv_end)
		{
			// (the frame's share is used up: everything waited on, the share starts again)
			log_once(1, "the shader-visible descriptors of a frame ran out: waited for the GPU");
			submit(true);
			srv_next = s.srv_begin;
		}
		const uint32_t at = srv_next;
		srv_next += n;
		D3D12_CPU_DESCRIPTOR_HANDLE dst = srv_heap->GetCPUDescriptorHandleForHeapStart();
		dst.ptr += SIZE_T(at) * srv_step;
		for (uint32_t i = 0; i < n; ++i)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE d = dst;
			d.ptr += SIZE_T(i) * srv_step;
			device->CopyDescriptorsSimple(1, d, views[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		}
		D3D12_GPU_DESCRIPTOR_HANDLE g = srv_heap->GetGPUDescriptorHandleForHeapStart();
		g.ptr += UINT64(at) * srv_step;
		return g;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE gpu::sampler_table(const D3D12_SAMPLER_DESC *descs)
	{
		const size_t bytes = sizeof(D3D12_SAMPLER_DESC) * kMaxStages;
		const uint64_t h = hash_bytes(descs, bytes);
		const auto it = sampler_tables.find(h);
		if (it != sampler_tables.end())
		{
			const uint32_t i = uint32_t((it->second.ptr - sampler_heap->GetGPUDescriptorHandleForHeapStart().ptr) / (UINT64(sampler_step) * kMaxStages));
			if (std::memcmp(sampler_keys[i].data(), descs, bytes) == 0)
				return it->second;
		}
		if (sampler_next == kSamplerTables)
		{
			// (full: once everything in flight is done the tables are made afresh)
			log_once(1, "more than %u sampler combinations: the sampler heap was started again", kSamplerTables);
			submit(true);
			sampler_tables.clear();
			sampler_next = 0;
		}
		const uint32_t i = sampler_next++;
		sampler_keys[i].assign(reinterpret_cast<const uint8_t *>(descs), reinterpret_cast<const uint8_t *>(descs) + bytes);
		D3D12_CPU_DESCRIPTOR_HANDLE c = sampler_heap->GetCPUDescriptorHandleForHeapStart();
		c.ptr += SIZE_T(i) * kMaxStages * sampler_step;
		for (uint32_t k = 0; k < kMaxStages; ++k)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE d = c;
			d.ptr += SIZE_T(k) * sampler_step;
			device->CreateSampler(&descs[k], d);
		}
		D3D12_GPU_DESCRIPTOR_HANDLE g = sampler_heap->GetGPUDescriptorHandleForHeapStart();
		g.ptr += UINT64(i) * kMaxStages * sampler_step;
		sampler_tables[h] = g;
		return g;
	}

	namespace
	{
		void queue_barrier(std::vector<D3D12_RESOURCE_BARRIER> &pending, ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
		{
			// (one already queued for it: the two made one, or none when it ends where it began)
			for (size_t i = 0; i < pending.size(); ++i)
				if (pending[i].Transition.pResource == r && pending[i].Transition.Subresource == sub)
				{
					if (pending[i].Transition.StateBefore == to)
						pending.erase(pending.begin() + ptrdiff_t(i));
					else
						pending[i].Transition.StateAfter = to;
					return;
				}
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = r;
			b.Transition.Subresource = sub;
			b.Transition.StateBefore = from;
			b.Transition.StateAfter = to;
			pending.push_back(b);
		}
	}

	void gpu::transition(gpu_resource &r, D3D12_RESOURCE_STATES to)
	{
		if (r.res == nullptr)
			return;
		if (r.depth)
		{
			for (uint32_t p = 0; p < r.planes; ++p)
				transition_plane(r, p, to);
			return;
		}
		if (r.state == to)
			return;
		queue_barrier(pending, r.res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, r.state, to);
		r.state = to;
	}

	void gpu::transition_plane(gpu_resource &r, uint32_t plane, D3D12_RESOURCE_STATES to)
	{
		if (r.res == nullptr || plane >= r.planes || r.plane[plane] == to)
			return;
		queue_barrier(pending, r.res, plane, r.plane[plane], to);
		r.plane[plane] = to;
	}

	void gpu::flush_barriers()
	{
		if (pending.empty())
			return;
		cmd->ResourceBarrier(UINT(pending.size()), pending.data());
		pending.clear();
	}

	void gpu::defer_release(IUnknown *object)
	{
		if (object == nullptr)
			return;
		deferred d;
		d.fence = fence_value + 1;
		d.object = object;
		deferred_list.push_back(std::move(d));
	}

	void gpu::defer(std::function<void()> fn)
	{
		deferred d;
		d.fence = fence_value + 1;
		d.object = nullptr;
		d.fn = std::move(fn);
		deferred_list.push_back(std::move(d));
	}

	readback gpu::new_readback(UINT64 size)
	{
		for (size_t i = 0; i < spare_readbacks.size(); ++i)
			if (spare_readbacks[i].size >= size)
			{
				readback r = spare_readbacks[i];
				spare_readbacks.erase(spare_readbacks.begin() + ptrdiff_t(i));
				return r;
			}
		readback r;
		r.size = align_up64(std::max<UINT64>(size, 65536), 65536);
		D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = r.size;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r.buffer))))
			r.size = 0;
		return r;
	}

	void gpu::free_readback(readback &r)
	{
		if (r.buffer == nullptr)
			return;
		if (spare_readbacks.size() < 16)
			spare_readbacks.push_back(r);
		else
			defer_release(r.buffer);
		r = readback();
	}
}
