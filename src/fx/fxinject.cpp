/*
 * The native effect renderer on dgVoodoo's lists (inject.hpp). Drawn at the marker's ClearDepthStencilView (the hook
 * calls in before the clear, as the characters' markers do). Our own views of the world's target and depth, in heaps
 * of our own; the textures' views in theirs (src/fx/textures).
 *
 * The n-th fx_run marker of a flush draws its run n: the flush is told by the frame's seq (frame.hpp), and every
 * marker the game side sends has a run, possibly empty (a run sent only for new textures to be copied in, or as the
 * proof that markers come through before anything is left to HiFi XI).
 */

#include <windows.h>
#include "fxinject.hpp"
#include "fxframe.hpp"
#include "fxrender.hpp"
#include "fxtextures.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>

namespace hifi::fx::inject
{
	namespace
	{
		constexpr ULONGLONG kFailMs = 2000;
		constexpr uint64_t kUploadBudget = 16ull << 20; // (texture bytes copied in at one marker)

		struct state
		{
			std::mutex mutex;
			ID3D12Device *device = nullptr;
			std::unique_ptr<fx_renderer> renderer;
			ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
			ID3D12Resource *view_rt = nullptr, *view_ds = nullptr;
			DXGI_FORMAT view_rt_format = DXGI_FORMAT_UNKNOWN, view_ds_format = DXGI_FORMAT_UNKNOWN;
			uint32_t seq = 0, next = 0; // the flush being drawn, its next run
			counts n;
			std::atomic<uint64_t> markers { 0 };
			std::atomic<ULONGLONG> failed_at { 0 };
			std::string why;
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
			g.n.failed++;
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

		// (under the lock) Our views of the world's target and depth
		bool views(const passes::world &w, D3D12_CPU_DESCRIPTOR_HANDLE &rtv, D3D12_CPU_DESCRIPTOR_HANDLE &dsv)
		{
			if (g.rtv_heap == nullptr)
			{
				D3D12_DESCRIPTOR_HEAP_DESC d = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
				if (FAILED(g.device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g.rtv_heap))))
					return false;
				d = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
				if (FAILED(g.device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g.dsv_heap))))
					return false;
			}
			rtv = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
			dsv = g.dsv_heap->GetCPUDescriptorHandleForHeapStart();
			// (made again at every marker: a target let go and made again at the same address would leave a view of
			// the old one; target views are read as the list is recorded, so writing them again is safe)
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

	// (No device call while the mutex is held on the game's thread: dgVoodoo's recording thread takes it inside our
	// hooks, which with the debug layer run inside its device lock; the renderer is made, and the old one let go,
	// outside it)
	bool prepare(ID3D12Device *device, std::string &why)
	{
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			if (g.device == device && g.renderer && g.renderer->ready())
				return true;
		}
		if (device == nullptr)
			return false;
		auto made = std::make_unique<fx_renderer>();
		if (!made->init(device, why))
			return false;
		std::unique_ptr<fx_renderer> old;
		ID3D12DescriptorHeap *rtv = nullptr, *dsv = nullptr;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			old = std::move(g.renderer);
			g.renderer = std::move(made);
			rtv = g.rtv_heap;
			dsv = g.dsv_heap;
			g.rtv_heap = g.dsv_heap = nullptr;
			g.view_rt = g.view_ds = nullptr;
			g.device = device;
			g.seq = g.next = 0;
		}
		drop(rtv);
		drop(dsv);
		return true;
	}

	void release()
	{
		std::unique_ptr<fx_renderer> old;
		ID3D12DescriptorHeap *rtv = nullptr, *dsv = nullptr;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			old = std::move(g.renderer);
			rtv = g.rtv_heap;
			dsv = g.dsv_heap;
			g.rtv_heap = g.dsv_heap = nullptr;
			g.view_rt = g.view_ds = nullptr;
			g.device = nullptr;
		}
		drop(rtv);
		drop(dsv);
		textures::release();
	}

	bool ready()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		const ULONGLONG at = g.failed_at.load(std::memory_order_acquire);
		return g.renderer && g.renderer->ready() && (at == 0 || GetTickCount64() - at > kFailMs);
	}

	uint64_t markers_seen()
	{
		return g.markers.load(std::memory_order_acquire);
	}

	void on_run(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const passes::world &w, uint32_t seq_low4)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.n.markers++;
		g.markers.fetch_add(1, std::memory_order_release);
		if (!g.renderer || !g.renderer->ready() || g.device == nullptr)
			return fail("the renderer isn't made");
		const frame *f = published(seq_low4);
		uint32_t run = kNone;
		if (f != nullptr)
		{
			const uint32_t seq = f->seq.load(std::memory_order_acquire);
			if (seq != g.seq)
			{
				g.seq = seq;
				g.next = 0;
			}
			run = g.next++;
		}
		else
			g.n.no_frame++;
		std::string why;
		if (!textures::prepare(g.device, why))
			return fail("the effects' descriptor heap couldn't be made");
		g.n.textures += textures::upload(list, kUploadBudget);
		if (f == nullptr || run >= std::min(f->runs.load(std::memory_order_acquire), kMaxRuns) || f->run_list[run].count == 0)
			return;
		g.n.runs++;
		if (w.rt == nullptr || w.ds == nullptr || w.rt_format == DXGI_FORMAT_UNKNOWN || w.ds_format == DXGI_FORMAT_UNKNOWN)
			return fail("the world's target isn't known here");
		if (!w.planes_known)
			return fail("the world's depth planes' states aren't known yet");
		if (w.heap_count == 0)
			return fail("the list's descriptor heaps aren't known");
		D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;
		if (!views(w, rtv, dsv))
			return fail("views of the world couldn't be made");
		const D3D12_RESOURCE_STATES p0 = w.ds_planes[0], p1 = w.ds_planes[1];
		barrier(list, w.ds, 0, p0, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		barrier(list, w.ds, 1, p1, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, w.rt_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		const D3D12_RESOURCE_DESC rd = w.rt->GetDesc();
		target t;
		t.rtv = rtv;
		t.dsv = dsv;
		t.rt = w.rt;
		t.rtv_format = w.rt_format;
		t.dsv_format = w.ds_format;
		t.samples = rd.SampleDesc.Count;
		t.width = float(rd.Width);
		t.height = float(rd.Height);
		ID3D12DescriptorHeap *const heap = textures::heap();
		list->SetDescriptorHeaps(1, &heap);
		const run_stats st = g.renderer->record_run(list, t, *f, g.seq, run);
		barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, w.rt_state);
		barrier(list, w.ds, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, p1);
		barrier(list, w.ds, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, p0);
		// (its heaps before its root tables: a table must be in the heaps bound when it's set)
		list->SetDescriptorHeaps(w.heap_count, w.heaps);
		bound.restore(list);
		g.n.elements += st.elements;
		g.n.draws += st.draws;
		g.n.triangles += st.triangles;
		g.n.grabs += st.grabs;
	}

	counts get_counts()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		return g.n;
	}

	std::vector<std::string> status()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		char line[400];
		std::snprintf(line, sizeof(line), "native effect renderer: %s; %llu markers, %llu runs drawn (%llu elements, %llu draws, %llu triangles, %llu distortion grabs), %llu textures copied in, "
			"%llu markers without their flush, %llu couldn't be drawn%s%s",
			g.renderer && g.renderer->ready() ? "up" : "not made", static_cast<unsigned long long>(g.n.markers), static_cast<unsigned long long>(g.n.runs),
			static_cast<unsigned long long>(g.n.elements), static_cast<unsigned long long>(g.n.draws), static_cast<unsigned long long>(g.n.triangles), static_cast<unsigned long long>(g.n.grabs),
			static_cast<unsigned long long>(g.n.textures), static_cast<unsigned long long>(g.n.no_frame), static_cast<unsigned long long>(g.n.failed),
			g.why.empty() ? "" : " (the last: ", g.why.empty() ? "" : (g.why + ")").c_str());
		return { line, textures::status() };
	}
}
