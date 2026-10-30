/*
 * The native character renderer on dgVoodoo's lists (charinject.hpp). Drawn at the marker's ClearDepthStencilView (the
 * hook calls in before the clear: dgVoodoo's barrier putting the stencil plane in DEPTH_WRITE for it is recorded
 * already; its clear runs after ours, with the planes back as it left them). Our own views of the world's target and
 * depth and of the see-through target T (made once per set), in heaps of our own; T's shader view in the cache's heap.
 */

#include <windows.h>
#include "charinject.hpp"
#include "cache.hpp"
#include "chardraw.hpp"
#include "charframe.hpp"
#include "../zone/light_split.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>



namespace hifi::chars::inject
{
	namespace
	{
		constexpr D3D12_RESOURCE_STATES kUnknown = D3D12_RESOURCE_STATES(0xFFFFFFFF);
		constexpr ULONGLONG kFailMs = 2000;

		struct heaps
		{
			UINT count = 0;
			ID3D12DescriptorHeap *h[2] = {};
		};

		struct state
		{
			std::mutex mutex;
			ID3D12Device *device = nullptr;
			std::unique_ptr<char_renderer> renderer; // (made and let go outside the mutex: see prepare)
			std::unique_ptr<aura_ring> aura;         // (a see-through model's aura ring: made with it)
			std::atomic<bool> per_pixel { false };
			// Toward the zone's sun (set_sun: the plugin's, from the zone pass), for the light split's share
			std::mutex sun_mutex;
			bool sun_known = false;
			float sun[3] = {};
			uint32_t lamp_count = 0; // (and its lamps: set_lamps)
			char_lamp lamps[kFrameLamps] = {};
			std::atomic<double> clock { -1.0 }; // (the en-spells' time: below 0, the real one)
			std::unordered_map<ID3D12GraphicsCommandList *, heaps> lists;
			ID3D12Resource *depth = nullptr;
			D3D12_RESOURCE_STATES plane[2] = { kUnknown, kUnknown };
			// Our views: the world's target (0) and T (1); the world's depth
			ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
			UINT rtv_step = 0;
			ID3D12Resource *view_rt = nullptr, *view_ds = nullptr;
			DXGI_FORMAT view_rt_format = DXGI_FORMAT_UNKNOWN, view_ds_format = DXGI_FORMAT_UNKNOWN;
			// T: as big as the world's target, its format and samples
			ID3D12Resource *t = nullptr;
			D3D12_RESOURCE_STATES t_state = D3D12_RESOURCE_STATE_COMMON;
			// The scene's copy for a see-through model's cloak (025870's T2): the world's target's size and format, one
			// sample; its view the cache's reserved descriptor 1
			ID3D12Resource *scene = nullptr;
			D3D12_RESOURCE_STATES scene_state = D3D12_RESOURCE_STATE_COMMON;
			// The frame being drawn: its seq, the see-through markers seen of it
			uint32_t seq = 0, see_through = 0;
			counts n;
			std::atomic<ULONGLONG> failed_at { 0 };
			std::string why;
			char_stats last;
			// (the offline probe's picture of the world's target after the characters: wanted, taken)
			std::atomic<bool> snapshot_wanted { false };
			ID3D12Resource *snapshot = nullptr;
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT snapshot_spot = {};
			DXGI_FORMAT snapshot_format = DXGI_FORMAT_UNKNOWN;
			bool snapshot_taken = false;
			// The opaque characters' motion (set_motion_wanted, motion): the world's size, its own RTV heap
			std::atomic<bool> motion_wanted { false };
			ID3D12Resource *motion = nullptr;
			ID3D12DescriptorHeap *motion_heap = nullptr;
			D3D12_RESOURCE_STATES motion_state = D3D12_RESOURCE_STATE_COMMON;
			uint64_t motion_frames = 0;
			std::vector<std::pair<ID3D12Resource *, uint64_t>> motion_old; // (let go 16 frames on: past the GPU)
		};
		state g;
		constexpr D3D12_RESOURCE_STATES kMotionRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

		// The en-spells' time in seconds: set_clock's, else the performance counter's
		double effect_time()
		{
			const double held = g.clock.load(std::memory_order_relaxed);
			if (held >= 0.0)
				return held;
			LARGE_INTEGER f, t;
			QueryPerformanceFrequency(&f);
			QueryPerformanceCounter(&t);
			return double(t.QuadPart) / double(f.QuadPart);
		}

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

		D3D12_CPU_DESCRIPTOR_HANDLE rtv_at(UINT i)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += SIZE_T(i) * g.rtv_step;
			return h;
		}

		bool views(const world &w, D3D12_CPU_DESCRIPTOR_HANDLE &rtv, D3D12_CPU_DESCRIPTOR_HANDLE &dsv)
		{
			if (g.rtv_heap == nullptr)
			{
				D3D12_DESCRIPTOR_HEAP_DESC d = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
				if (FAILED(g.device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g.rtv_heap))))
					return false;
				d = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
				if (FAILED(g.device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g.dsv_heap))))
					return false;
				g.rtv_step = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			}
			rtv = rtv_at(0);
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
			drop(g.t); // (T follows the world's target, and so does the scene's copy)
			drop(g.scene);
			return true;
		}

		// T as the world's target is (size, format, samples), its views; false when it can't be made
		bool see_through_target(const world &w, D3D12_CPU_DESCRIPTOR_HANDLE &rtv, D3D12_GPU_DESCRIPTOR_HANDLE &srv)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu;
			if (!cache::reserved(0, srv_cpu, srv))
				return false;
			rtv = rtv_at(1);
			if (g.t != nullptr)
				return true;
			D3D12_RESOURCE_DESC d = w.rt->GetDesc();
			d.Format = w.rt_format;
			d.MipLevels = 1;
			d.DepthOrArraySize = 1;
			d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			d.Alignment = 0;
			D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
			D3D12_CLEAR_VALUE cv = {};
			cv.Format = w.rt_format;
			if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&g.t))))
				return false;
			g.t_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
			D3D12_RENDER_TARGET_VIEW_DESC rv = {};
			rv.Format = w.rt_format;
			rv.ViewDimension = d.SampleDesc.Count > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
			g.device->CreateRenderTargetView(g.t, &rv, rtv);
			D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
			sv.Format = w.rt_format;
			sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			if (d.SampleDesc.Count > 1)
				sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
			else
			{
				sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				sv.Texture2D.MipLevels = 1;
			}
			g.device->CreateShaderResourceView(g.t, &sv, srv_cpu);
			return true;
		}

		// (under the lock, the world's target in w.rt_state) The scene as drawn so far copied (resolved when multisampled)
		// into the scene's copy, left readable by a pixel shader (025870: a full-viewport copy of A before a cloaked
		// model); its view. False when it can't be made.
		bool copy_scene(ID3D12GraphicsCommandList *list, const world &w, D3D12_GPU_DESCRIPTOR_HANDLE &srv)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu;
			if (!cache::reserved(1, srv_cpu, srv))
				return false;
			const D3D12_RESOURCE_DESC rd = w.rt->GetDesc();
			if (g.scene == nullptr)
			{
				D3D12_RESOURCE_DESC d = rd;
				d.Format = w.rt_format;
				d.MipLevels = 1;
				d.DepthOrArraySize = 1;
				d.SampleDesc.Count = 1;
				d.SampleDesc.Quality = 0;
				d.Flags = D3D12_RESOURCE_FLAG_NONE;
				d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
				d.Alignment = 0;
				D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
				if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g.scene))))
				{
					g.scene = nullptr;
					return false;
				}
				g.scene->SetName(L"hifixi: the scene's copy (the cloak)");
				g.scene_state = D3D12_RESOURCE_STATE_COMMON;
				D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
				sv.Format = w.rt_format;
				sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				sv.Texture2D.MipLevels = 1;
				g.device->CreateShaderResourceView(g.scene, &sv, srv_cpu);
			}
			const bool resolve = rd.SampleDesc.Count > 1;
			const D3D12_RESOURCE_STATES from = resolve ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE;
			const D3D12_RESOURCE_STATES to = resolve ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST;
			barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, w.rt_state, from);
			barrier(list, g.scene, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, g.scene_state, to);
			if (resolve)
				list->ResolveSubresource(g.scene, 0, w.rt, 0, w.rt_format);
			else
				list->CopyResource(g.scene, w.rt);
			barrier(list, g.scene, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, to, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			g.scene_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, w.rt_state);
			return true;
		}

		// (under the lock) What every draw checks; false (and failed) when one isn't so
		bool can_draw(const world &w, D3D12_CPU_DESCRIPTOR_HANDLE &rtv, D3D12_CPU_DESCRIPTOR_HANDLE &dsv)
		{
			if (!g.renderer || !g.renderer->ready() || g.device == nullptr)
			{
				fail("the renderer isn't made");
				return false;
			}
			if (w.rt == nullptr || w.ds == nullptr || w.ds != g.depth || w.rt_format == DXGI_FORMAT_UNKNOWN || w.ds_format == DXGI_FORMAT_UNKNOWN)
			{
				fail("the world's target isn't known here");
				return false;
			}
			if (g.plane[0] == kUnknown || g.plane[1] == kUnknown)
			{
				fail("the world's depth planes' states aren't known yet");
				return false;
			}
			if (!views(w, rtv, dsv))
			{
				fail("views of the world couldn't be made");
				return false;
			}
			return true;
		}

		// (under the lock) The motion texture the world's target's size, made (again) when it isn't
		bool motion_target(const world &w)
		{
			const D3D12_RESOURCE_DESC rd = w.rt->GetDesc();
			if (g.motion != nullptr)
			{
				const D3D12_RESOURCE_DESC md = g.motion->GetDesc();
				if (md.Width == rd.Width && md.Height == rd.Height)
					return true;
				g.motion_old.push_back({ g.motion, g.motion_frames });
				g.motion = nullptr;
			}
			if (g.device == nullptr)
				return false;
			if (g.motion_heap == nullptr)
			{
				const D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
				if (FAILED(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.motion_heap))))
					return false;
			}
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			d.Width = rd.Width;
			d.Height = rd.Height;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = kMotionFormat;
			d.SampleDesc.Count = 1;
			d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			const D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
			D3D12_CLEAR_VALUE cv = {};
			cv.Format = kMotionFormat;
			if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, &cv, IID_PPV_ARGS(&g.motion))))
			{
				g.motion = nullptr;
				return false;
			}
			g.motion->SetName(L"hifixi: the characters' motion");
			g.motion_state = D3D12_RESOURCE_STATE_COMMON;
			g.device->CreateRenderTargetView(g.motion, nullptr, g.motion_heap->GetCPUDescriptorHandleForHeapStart());
			return true;
		}

		char_target target_of(const world &w, D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv)
		{
			const D3D12_RESOURCE_DESC rd = w.rt->GetDesc();
			char_target t;
			t.rtv = rtv;
			t.dsv = dsv;
			t.rtv_format = w.rt_format;
			t.dsv_format = w.ds_format;
			t.samples = rd.SampleDesc.Count;
			t.width = float(rd.Width);
			t.height = float(rd.Height);
			return t;
		}

		// (under the lock, the target a render target) The world's target copied out for the probe, when it's wanted
		void take_snapshot(ID3D12GraphicsCommandList *list, const world &w)
		{
			if (!g.snapshot_wanted.exchange(false))
				return;
			const D3D12_RESOURCE_DESC d = w.rt->GetDesc();
			if (d.SampleDesc.Count != 1)
				return;
			UINT rows = 0;
			UINT64 row_bytes = 0, total = 0;
			g.device->GetCopyableFootprints(&d, 0, 1, 0, &g.snapshot_spot, &rows, &row_bytes, &total);
			drop(g.snapshot);
			D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK };
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = total;
			bd.Height = 1;
			bd.DepthOrArraySize = 1;
			bd.MipLevels = 1;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.snapshot))))
				return;
			barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
			dst.pResource = g.snapshot;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			dst.PlacedFootprint = g.snapshot_spot;
			src.pResource = w.rt;
			src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.snapshot_format = w.rt_format;
			g.snapshot_taken = true;
		}

		void put_back(ID3D12GraphicsCommandList *list, const grass::list_state &bound)
		{
			// (its heaps before its root tables: a table must be in the heaps bound when it's set)
			const auto known = g.lists.find(list);
			if (known != g.lists.end() && known->second.count != 0)
				list->SetDescriptorHeaps(known->second.count, known->second.h);
			bound.restore(list);
		}
	}

	// (No device call while the mutex is held: dgVoodoo's recording thread takes it inside our hooks, which with the
	// debug layer run inside its device lock; the renderer is made, and the old one let go, outside it)
	bool prepare(ID3D12Device *device, std::string &why)
	{
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			if (g.device == device && g.renderer && g.renderer->ready())
				return true;
		}
		if (device == nullptr)
			return false;
		auto made = std::make_unique<char_renderer>();
		if (!made->init(device, why))
			return false;
		// (the aura ring's: without it, no aura; the models still drawn)
		auto made_aura = std::make_unique<aura_ring>();
		std::string aura_why;
		if (!made_aura->init(device, aura_why))
			made_aura.reset();
		std::unique_ptr<char_renderer> old;
		std::unique_ptr<aura_ring> old_aura;
		ID3D12DescriptorHeap *rtv = nullptr, *dsv = nullptr;
		ID3D12Resource *t = nullptr, *scene = nullptr;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			old = std::move(g.renderer);
			g.renderer = std::move(made);
			old_aura = std::move(g.aura);
			g.aura = std::move(made_aura);
			rtv = g.rtv_heap;
			dsv = g.dsv_heap;
			t = g.t;
			scene = g.scene;
			g.rtv_heap = g.dsv_heap = nullptr;
			g.t = g.scene = nullptr;
			g.view_rt = g.view_ds = nullptr;
			g.device = device;
		}
		drop(rtv);
		drop(dsv);
		drop(t);
		drop(scene);
		return true;
	}

	void release()
	{
		light::release(); // (both passes' target: let go with either)
		std::unique_ptr<char_renderer> old;
		std::unique_ptr<aura_ring> old_aura;
		ID3D12DescriptorHeap *rtv = nullptr, *dsv = nullptr;
		ID3D12Resource *t = nullptr, *scene = nullptr;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			drop(g.motion);
			drop(g.motion_heap);
			for (auto &o : g.motion_old)
				o.first->Release();
			g.motion_old.clear();
			g.motion_state = D3D12_RESOURCE_STATE_COMMON;
			old = std::move(g.renderer);
			old_aura = std::move(g.aura);
			rtv = g.rtv_heap;
			dsv = g.dsv_heap;
			t = g.t;
			scene = g.scene;
			g.rtv_heap = g.dsv_heap = nullptr;
			g.t = g.scene = nullptr;
			g.view_rt = g.view_ds = nullptr;
			g.device = nullptr;
			g.lists.clear();
			g.depth = nullptr;
			g.plane[0] = g.plane[1] = kUnknown;
		}
		drop(rtv);
		drop(dsv);
		drop(t);
		drop(scene);
	}

	void set_options(bool per_pixel)
	{
		g.per_pixel.store(per_pixel, std::memory_order_relaxed);
	}

	void set_clock(double seconds)
	{
		g.clock.store(seconds, std::memory_order_relaxed);
	}

	bool ready()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		const ULONGLONG at = g.failed_at.load(std::memory_order_acquire);
		return g.renderer && g.renderer->ready() && (at == 0 || GetTickCount64() - at > kFailMs);
	}

	std::vector<std::string> status()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		char line[400];
		std::snprintf(line, sizeof(line), "native character renderer: %s; %llu frames drawn, %llu models, %llu see-through, %llu couldn't be%s%s; the last: %u models, %u draws, "
			"%u triangles (%u meshes not ready, %u groups hidden, %u untextured)",
			g.renderer && g.renderer->ready() ? "up" : "not made", static_cast<unsigned long long>(g.n.frames), static_cast<unsigned long long>(g.n.models),
			static_cast<unsigned long long>(g.n.see_through), static_cast<unsigned long long>(g.n.failed), g.why.empty() ? "" : " (the last: ",
			g.why.empty() ? "" : (g.why + ")").c_str(), g.last.models, g.last.draws, g.last.triangles, g.last.not_ready, g.last.hidden, g.last.untextured);
		std::vector<std::string> lines = { line };
		std::snprintf(line, sizeof(line), "native character renderer: sun shadows from their meshes: %llu frames, %llu models%s",
			static_cast<unsigned long long>(g.n.caster_frames), static_cast<unsigned long long>(g.n.caster_models),
			g.n.caster_frames == 0 ? " (none yet: needs World FX's sun shadow map and Characters cast sun shadows)" : "");
		lines.push_back(line);
		std::snprintf(line, sizeof(line), "native character renderer: extra passes: %llu overlays, %llu cloaks drawn (%llu scene copies couldn't be made); %llu aura rings drawn (%llu couldn't be)",
			static_cast<unsigned long long>(g.n.overlays), static_cast<unsigned long long>(g.n.cloaks), static_cast<unsigned long long>(g.n.cloak_failed),
			static_cast<unsigned long long>(g.n.auras), static_cast<unsigned long long>(g.n.aura_failed));
		lines.push_back(line);
		return lines;
	}

	counts get_counts()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		return g.n;
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

	void on_chars_end(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const world &w, uint32_t seq_low4)
	{
		const frame *f = published(seq_low4);
		if (f == nullptr)
			return;
		std::lock_guard<std::mutex> lock(g.mutex);
		const uint32_t seq = f->seq.load(std::memory_order_acquire);
		if (seq == g.seq)
			return;
		g.seq = seq;
		g.see_through = 0;
		const uint32_t opaque = std::min(f->opaque.load(std::memory_order_acquire), kMaxModels);
		bool any = false;
		for (uint32_t i = 0; i < opaque && !any; ++i)
			any = (f->model[i].flags & kNative) != 0;
		if (!any)
			return;
		D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;
		if (!can_draw(w, rtv, dsv))
			return;
		char_options o;
		o.per_pixel = g.per_pixel.load(std::memory_order_relaxed);
		{
			std::lock_guard<std::mutex> sun_lock(g.sun_mutex);
			o.sun_known = g.sun_known;
			std::memcpy(o.sun, g.sun, sizeof(o.sun));
			o.lamp_count = g.lamp_count;
			std::memcpy(o.lamps, g.lamps, sizeof(char_lamp) * g.lamp_count);
		}
		const D3D12_RESOURCE_STATES p0 = g.plane[0], p1 = g.plane[1];
		barrier(list, w.ds, 0, p0, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		barrier(list, w.ds, 1, p1, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, w.rt_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		char_target target = target_of(w, rtv, dsv);
		// (their motion for FSR, while it draws: the texture the world's size, cleared, as the second target)
		const bool moving = g.motion_wanted.load(std::memory_order_relaxed) && target.samples == 1 && motion_target(w);
		if (moving)
		{
			barrier(list, g.motion, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, g.motion_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
			const D3D12_CPU_DESCRIPTOR_HANDLE mrtv = g.motion_heap->GetCPUDescriptorHandleForHeapStart();
			const float none[4] = {};
			list->ClearRenderTargetView(mrtv, none, 0, nullptr);
			target.motion_rtv = mrtv;
			target.motion_format = kMotionFormat;
		}
		// (the light split as well, while World FX reads it: light_split.hpp)
		const bool lit = light::begin(list, g.device, w.rt, target.light_rtv);
		g.last = g.renderer->record_opaque(list, target, *f, o);
		if (lit)
			light::end(list);
		if (moving)
		{
			barrier(list, g.motion, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, kMotionRead);
			g.motion_state = kMotionRead;
			g.motion_frames++;
			for (auto it = g.motion_old.begin(); it != g.motion_old.end();)
				if (g.motion_frames - it->second > 16)
				{
					it->first->Release();
					it = g.motion_old.erase(it);
				}
				else
					++it;
		}
		{
			const char_stats en = g.renderer->record_enspells(list, target_of(w, rtv, dsv), *f, effect_time());
			g.n.enspell_models += en.models;
		}
		if (f->see_through.load(std::memory_order_acquire) == 0)
			take_snapshot(list, w);
		barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, w.rt_state);
		barrier(list, w.ds, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, p1);
		barrier(list, w.ds, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, p0);
		put_back(list, bound);
		g.n.frames++;
		g.n.models += g.last.models;
		g.n.overlays += g.last.overlays;
	}

	void set_sun(const float *toward)
	{
		std::lock_guard<std::mutex> lock(g.sun_mutex);
		g.sun_known = toward != nullptr;
		for (int i = 0; i < 3; ++i)
			g.sun[i] = toward != nullptr ? toward[i] : 0.0f;
	}

	void set_lamps(const float *lamps, uint32_t count)
	{
		std::lock_guard<std::mutex> lock(g.sun_mutex);
		g.lamp_count = lamps != nullptr ? std::min(count, kFrameLamps) : 0;
		if (g.lamp_count != 0)
			std::memcpy(g.lamps, lamps, sizeof(char_lamp) * g.lamp_count);
	}

	void set_motion_wanted(bool wanted)
	{
		g.motion_wanted = wanted;
	}

	bool motion(ID3D12Resource *&texture, uint64_t &frames)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		texture = g.motion_state == kMotionRead ? g.motion : nullptr;
		frames = g.motion_frames;
		return texture != nullptr;
	}

	bool record_casters(ID3D12GraphicsCommandList *list, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (!g.renderer || !g.renderer->ready() || g.seq == 0)
			return false;
		const frame *f = published(g.seq & 15u);
		if (f == nullptr || f->seq.load(std::memory_order_acquire) != g.seq)
			return false;
		// (the game's heaps go back after: World FX sets its own state, but binds no heaps. Not drawn at all when they
		// aren't known: the game's next tables would point into ours.)
		const auto known = g.lists.find(list);
		if (known == g.lists.end() || known->second.count == 0)
			return false;
		const char_stats st = g.renderer->record_casters(list, *f, world_to_clip, dsv, size);
		if (st.models != 0)
			list->SetDescriptorHeaps(known->second.count, known->second.h);
		if (st.draws == 0)
			return false;
		g.n.caster_frames++;
		g.n.caster_models += st.models;
		return true;
	}

	grass::renderer::caster_fn caster_hook()
	{
		return [](void *, ID3D12GraphicsCommandList *list, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size) {
			return record_casters(list, world_to_clip, dsv, size);
		};
	}

	void on_see_through(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const world &w, uint32_t seq_low4)
	{
		const frame *f = published(seq_low4);
		if (f == nullptr)
			return;
		std::lock_guard<std::mutex> lock(g.mutex);
		const uint32_t seq = f->seq.load(std::memory_order_acquire);
		if (seq != g.seq)
		{
			// (no end marker drawn for this frame here: its see-through markers counted from now)
			g.seq = seq;
			g.see_through = 0;
		}
		const uint32_t n = g.see_through++;
		if (n >= f->see_through.load(std::memory_order_acquire))
			return;
		const uint32_t mi = f->opaque.load(std::memory_order_relaxed) + n;
		if (mi >= std::min(f->models, kMaxModels))
			return;
		const model_entry &m = f->model[mi];
		if ((m.flags & kNative) == 0 || (m.colour >> 24) == 0)
			return;
		D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv, t_rtv;
		D3D12_GPU_DESCRIPTOR_HANDLE t_srv;
		if (!can_draw(w, rtv, dsv))
			return;
		if (!see_through_target(w, t_rtv, t_srv))
			return fail("the see-through target couldn't be made");
		char_options o;
		o.per_pixel = g.per_pixel.load(std::memory_order_relaxed);
		{
			std::lock_guard<std::mutex> sun_lock(g.sun_mutex);
			o.sun_known = g.sun_known;
			std::memcpy(o.sun, g.sun, sizeof(o.sun));
			o.lamp_count = g.lamp_count;
			std::memcpy(o.lamps, g.lamps, sizeof(char_lamp) * g.lamp_count);
		}
		// (a cloaked model: the scene as it is now copied first, for its cloak pass to sample)
		D3D12_GPU_DESCRIPTOR_HANDLE scene = {};
		if ((m.passes & kPassCloak) != 0 && !copy_scene(list, w, scene))
		{
			scene = {};
			g.n.cloak_failed++;
		}
		const D3D12_RESOURCE_STATES p0 = g.plane[0], p1 = g.plane[1];
		barrier(list, w.ds, 0, p0, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		barrier(list, w.ds, 1, p1, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		// The model into T (cleared), with the world's depth: test and write
		barrier(list, g.t, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, g.t_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		const float zero[4] = { 0, 0, 0, 0 };
		list->ClearRenderTargetView(t_rtv, zero, 0, nullptr);
		char_target tt = target_of(w, t_rtv, dsv);
		g.last = g.renderer->record_model(list, tt, *f, mi, o, scene);
		g.n.overlays += g.last.overlays;
		g.n.cloaks += g.last.cloaks;
		// T over the world
		barrier(list, g.t, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		g.t_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, w.rt_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		// (its aura ring first, as 038A80 / 038E10 before 01D3B0: model_entry::aura, chardraw.hpp aura_ring)
		if (m.aura != 0)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE a_cpu;
			D3D12_GPU_DESCRIPTOR_HANDLE a_gpu;
			if (g.aura && cache::reserved(2, a_cpu, a_gpu) && g.aura->record(list, target_of(w, rtv, dsv), cache::heap(), t_srv, a_cpu, a_gpu, m))
				g.n.auras++;
			else
				g.n.aura_failed++;
		}
		g.renderer->record_composite(list, target_of(w, rtv, dsv), t_srv, m.colour);
		take_snapshot(list, w);
		barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, w.rt_state);
		barrier(list, w.ds, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, p1);
		barrier(list, w.ds, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, p0);
		put_back(list, bound);
		g.n.see_through++;
	}
}

// For the offline probe: a picture of the world's target after the characters, asked for (taken at the next frame's
// last character draw) and written as a BMP once dgVoodoo's list has run (1 when written)
extern "C" __declspec(dllexport) void __cdecl hifixi_chars_snapshot()
{
	hifi::chars::inject::g.snapshot_wanted.store(true);
}

extern "C" __declspec(dllexport) int __cdecl hifixi_chars_snapshot_save(const char *path)
{
	using namespace hifi::chars::inject;
	std::lock_guard<std::mutex> lock(g.mutex);
	if (!g.snapshot_taken || g.snapshot == nullptr || path == nullptr)
		return 0;
	const UINT w = g.snapshot_spot.Footprint.Width, h = g.snapshot_spot.Footprint.Height, pitch = g.snapshot_spot.Footprint.RowPitch;
	uint8_t *p = nullptr;
	if (FAILED(g.snapshot->Map(0, nullptr, reinterpret_cast<void **>(&p))))
		return 0;
	FILE *f = nullptr;
	if (fopen_s(&f, path, "wb") != 0 || f == nullptr)
	{
		g.snapshot->Unmap(0, nullptr);
		return 0;
	}
	uint8_t head[54] = { 'B', 'M' };
	auto put = [&](int at, uint32_t v) { std::memcpy(head + at, &v, 4); };
	put(2, 54 + w * h * 4);
	put(10, 54);
	put(14, 40);
	put(18, w);
	put(22, uint32_t(-int32_t(h)));
	head[26] = 1;
	head[28] = 32;
	fwrite(head, 1, 54, f);
	const bool bgra = g.snapshot_format == DXGI_FORMAT_B8G8R8A8_UNORM || g.snapshot_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || g.snapshot_format == DXGI_FORMAT_B8G8R8X8_UNORM;
	for (UINT y = 0; y < h; ++y)
		for (UINT x = 0; x < w; ++x)
		{
			const uint8_t *q = p + g.snapshot_spot.Offset + size_t(y) * pitch + size_t(x) * 4;
			const uint8_t out[4] = { bgra ? q[0] : q[2], q[1], bgra ? q[2] : q[0], 255 };
			fwrite(out, 1, 4, f);
		}
	fclose(f);
	g.snapshot->Unmap(0, nullptr);
	return 1;
}
