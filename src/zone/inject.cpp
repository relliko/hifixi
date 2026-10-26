/*
 * The native zone renderer on dgVoodoo's lists (inject.hpp). The zone is drawn at the end marker's ClearDepthStencilView
 * (the hook calls in before the clear: dgVoodoo's barrier that put the stencil plane in DEPTH_WRITE for it is recorded
 * already; the clear runs after ours, with the planes back as dgVoodoo left them). Our own views of the world's target
 * and depth (made once per pair, in heaps of our own) with dgVoodoo's views' formats.
 */

#include <windows.h>
#include "inject.hpp"
#include "draw.hpp"
#include "frame.hpp"
#include "zone_gpu.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace hifi::zone::inject
{
	namespace
	{
		constexpr D3D12_RESOURCE_STATES kUnknown = D3D12_RESOURCE_STATES(0xFFFFFFFF);
		constexpr ULONGLONG kFailMs = 2000; // (not ready for this long after a pass couldn't be drawn)

		struct heaps
		{
			UINT count = 0;
			ID3D12DescriptorHeap *h[2] = {};
		};

		struct state
		{
			std::mutex mutex;
			ID3D12Device *device = nullptr;
			std::unique_ptr<zone_renderer> renderer; // (made and let go outside the mutex: see prepare)
			std::atomic<bool> per_pixel { false };
			std::unordered_map<ID3D12GraphicsCommandList *, heaps> lists;
			// The world's depth and its planes' states as recorded (dgVoodoo's one thread)
			ID3D12Resource *depth = nullptr;
			D3D12_RESOURCE_STATES plane[2] = { kUnknown, kUnknown };
			// Our views of the world
			ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
			ID3D12Resource *view_rt = nullptr, *view_ds = nullptr;
			DXGI_FORMAT view_rt_format = DXGI_FORMAT_UNKNOWN, view_ds_format = DXGI_FORMAT_UNKNOWN;
			// What's been drawn
			uint32_t last_seq = 0;
			uint64_t frame_number = 0, drawn = 0, failed = 0;
			std::atomic<ULONGLONG> failed_at { 0 };
			std::string why;
			draw_stats last;
		};
		state g;

		template <typename T>
		void drop(T *&p)
		{
			if (p != nullptr)
				p->Release();
			p = nullptr;
		}

		void fail(const char *why)
		{
			g.failed++;
			g.why = why;
			g.failed_at.store(GetTickCount64(), std::memory_order_release);
		}

		void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
		{
			if (from == to)
				return;
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = r;
			b.Transition.Subresource = sub;
			b.Transition.StateBefore = from;
			b.Transition.StateAfter = to;
			list->ResourceBarrier(1, &b);
		}

		// Our views of the world's target and depth (made again when they change)
		bool views(const world &w, D3D12_CPU_DESCRIPTOR_HANDLE &rtv, D3D12_CPU_DESCRIPTOR_HANDLE &dsv)
		{
			if (g.rtv_heap == nullptr)
			{
				D3D12_DESCRIPTOR_HEAP_DESC d = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
				if (FAILED(g.device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g.rtv_heap))))
					return false;
				d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
				if (FAILED(g.device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g.dsv_heap))))
					return false;
			}
			rtv = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
			dsv = g.dsv_heap->GetCPUDescriptorHandleForHeapStart();
			if (w.rt == g.view_rt && w.ds == g.view_ds && w.rt_format == g.view_rt_format && w.ds_format == g.view_ds_format)
				return true;
			const D3D12_RESOURCE_DESC rd = w.rt->GetDesc(), dd = w.ds->GetDesc();
			D3D12_RENDER_TARGET_VIEW_DESC rv = {};
			rv.Format = w.rt_format;
			rv.ViewDimension = rd.SampleDesc.Count > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
			D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
			dv.Format = w.ds_format;
			dv.ViewDimension = dd.SampleDesc.Count > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
			g.device->CreateRenderTargetView(w.rt, &rv, rtv);
			g.device->CreateDepthStencilView(w.ds, &dv, dsv);
			g.view_rt = w.rt;
			g.view_ds = w.ds;
			g.view_rt_format = w.rt_format;
			g.view_ds_format = w.ds_format;
			return true;
		}
	}

	// The renderer is made and let go outside the mutex, only swapped in and out under it: with the debug layer,
	// dgVoodoo's thread runs our hooks (on_barrier, on_heaps, on_marker: the mutex) inside its device lock, and making
	// device objects under the mutex took the two in the other order (a hang; the native character session's catch)
	bool prepare(ID3D12Device *device, std::string &why)
	{
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			if (g.device == device && g.renderer != nullptr && g.renderer->ready())
				return true;
		}
		std::unique_ptr<zone_renderer> made;
		if (device != nullptr)
		{
			made = std::make_unique<zone_renderer>();
			if (!made->init(device, why))
			{
				made->release();
				made.reset();
			}
		}
		std::unique_ptr<zone_renderer> old;
		ID3D12DescriptorHeap *old_rtv = nullptr, *old_dsv = nullptr;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			old = std::move(g.renderer);
			g.renderer = std::move(made);
			old_rtv = g.rtv_heap;
			old_dsv = g.dsv_heap;
			g.rtv_heap = g.dsv_heap = nullptr;
			g.view_rt = g.view_ds = nullptr;
			g.device = device;
		}
		if (old != nullptr)
			old->release();
		drop(old_rtv);
		drop(old_dsv);
		std::lock_guard<std::mutex> lock(g.mutex);
		return g.renderer != nullptr && g.renderer->ready();
	}

	void release()
	{
		std::unique_ptr<zone_renderer> old;
		ID3D12DescriptorHeap *old_rtv = nullptr, *old_dsv = nullptr;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			old = std::move(g.renderer);
			old_rtv = g.rtv_heap;
			old_dsv = g.dsv_heap;
			g.rtv_heap = g.dsv_heap = nullptr;
			g.view_rt = g.view_ds = nullptr;
			g.device = nullptr;
			g.lists.clear();
			g.depth = nullptr;
			g.plane[0] = g.plane[1] = kUnknown;
		}
		if (old != nullptr)
			old->release();
		drop(old_rtv);
		drop(old_dsv);
	}

	void set_options(bool per_pixel)
	{
		g.per_pixel.store(per_pixel, std::memory_order_relaxed);
	}

	bool ready()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		const ULONGLONG at = g.failed_at.load(std::memory_order_acquire);
		return g.renderer != nullptr && g.renderer->ready() && (at == 0 || GetTickCount64() - at > kFailMs);
	}

	std::vector<std::string> status()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		std::vector<std::string> lines;
		char line[320];
		std::snprintf(line, sizeof(line), "native zone renderer: %s; %llu passes drawn, %llu couldn't be%s%s; the last: %u objects, %u draws, %u triangles",
			g.renderer != nullptr && g.renderer->ready() ? "up" : "not made", static_cast<unsigned long long>(g.drawn), static_cast<unsigned long long>(g.failed),
			g.why.empty() ? "" : " (the last: ", g.why.empty() ? "" : (g.why + ")").c_str(), g.last.objects, g.last.draws, g.last.triangles);
		lines.push_back(line);
		return lines;
	}

	void on_reset(ID3D12GraphicsCommandList *list)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.lists[list] = heaps();
	}

	void on_heaps(ID3D12GraphicsCommandList *list, UINT count, ID3D12DescriptorHeap *const *h)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		heaps &x = g.lists[list];
		x.count = h != nullptr ? std::min<UINT>(count, 2) : 0;
		for (UINT i = 0; i < x.count; ++i)
			x.h[i] = h[i];
	}

	void on_barrier(UINT count, const D3D12_RESOURCE_BARRIER *b)
	{
		if (b == nullptr)
			return;
		std::lock_guard<std::mutex> lock(g.mutex);
		if (g.depth == nullptr)
			return;
		for (UINT i = 0; i < count; ++i)
		{
			if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || b[i].Transition.pResource != g.depth)
				continue;
			const UINT sub = b[i].Transition.Subresource;
			if (sub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
				g.plane[0] = g.plane[1] = b[i].Transition.StateAfter;
			else if (sub < 2)
				g.plane[sub] = b[i].Transition.StateAfter;
		}
	}

	void on_marker(ID3D12Resource *depth)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (depth != g.depth)
		{
			g.depth = depth;
			g.plane[0] = g.plane[1] = kUnknown;
		}
	}

	bool depth_planes(ID3D12Resource *depth, D3D12_RESOURCE_STATES out[2])
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (depth == nullptr || depth != g.depth || g.plane[0] == kUnknown || g.plane[1] == kUnknown)
			return false;
		out[0] = g.plane[0];
		out[1] = g.plane[1];
		return true;
	}

	void on_zone_end(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const world &w, uint32_t seq_low4)
	{
		const frame *f = published(seq_low4);
		if (f == nullptr || !f->native)
			return;
		std::lock_guard<std::mutex> lock(g.mutex);
		const uint32_t seq = f->seq.load(std::memory_order_acquire);
		if (seq == g.last_seq)
			return;
		g.last_seq = seq;
		if (g.renderer == nullptr || !g.renderer->ready() || g.device == nullptr)
			return fail("the renderer isn't made");
		if (f->zone == nullptr || !f->zone->ready())
			return fail("the zone's data isn't on the GPU");
		if (w.rt == nullptr || w.ds == nullptr || w.ds != g.depth || w.rt_format == DXGI_FORMAT_UNKNOWN || w.ds_format == DXGI_FORMAT_UNKNOWN)
			return fail("the world's target isn't known here");
		if (g.plane[0] == kUnknown || g.plane[1] == kUnknown)
			return fail("the world's depth planes' states aren't known yet");
		D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;
		if (!views(w, rtv, dsv))
			return fail("views of the world couldn't be made");

		const D3D12_RESOURCE_DESC rd = w.rt->GetDesc();
		draw_target t;
		t.rtv = rtv;
		t.dsv = dsv;
		t.rtv_format = w.rt_format;
		t.dsv_format = w.ds_format;
		t.samples = rd.SampleDesc.Count;
		t.width = float(rd.Width);
		t.height = float(rd.Height);
		draw_options o;
		o.per_pixel = g.per_pixel.load(std::memory_order_relaxed);

		// Depth and stencil planes writable, the target a target; the zone; all as it was
		const D3D12_RESOURCE_STATES p0 = g.plane[0], p1 = g.plane[1];
		barrier(list, w.ds, 0, p0, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		barrier(list, w.ds, 1, p1, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, w.rt_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		g.last = g.renderer->record(list, t, *f, o, ++g.frame_number);
		barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, w.rt_state);
		barrier(list, w.ds, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, p1);
		barrier(list, w.ds, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, p0);
		// (its heaps before its root tables: a table must be in the heaps bound when it's set)
		const auto known = g.lists.find(list);
		if (known != g.lists.end() && known->second.count != 0)
			list->SetDescriptorHeaps(known->second.count, known->second.h);
		bound.restore(list);
		g.drawn++;
	}
}
