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
#include "light_split.hpp"
#include "zone_gpu.hpp"
#include "zoneshadow.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
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
			// The zone last drawn, and when (GetTickCount64): what the shadow cascades are drawn from while fresh; the
			// second map that pass drew (nullptr: none)
			gpu_zone *last_zone = nullptr, *last_second = nullptr;
			ULONGLONG last_zone_at = 0;
			// The movers of the pass last drawn (its door parts and swaying plants, both maps'), for the moving layer
			std::vector<frame_object> movers;
			// The shadow pass under a mutex of its own, never taken in the hooks: it makes resources (a zone's placements)
			// on dgVoodoo's thread, and with the debug layer a device call under the hooks' mutex deadlocks
			std::mutex shadow_mutex;
			std::unique_ptr<zone_shadow> shadow; // (made and let go outside both)
			std::unique_ptr<zone_shadow> second_shadow; // (the second map's: its own placements, its own movers)
			std::atomic<uint64_t> shadow_frames { 0 }, shadow_triangles { 0 };
			// The moving layer: frames it was drawn in, its triangles and instances in all, when it was last asked for
			std::atomic<uint64_t> mover_frames { 0 }, mover_triangles { 0 }, mover_instances { 0 };
			std::atomic<ULONGLONG> movers_at { 0 };
			std::atomic<bool> shadow_up { false };
			std::atomic<bool> per_pixel { false };
			std::unordered_map<ID3D12GraphicsCommandList *, heaps> lists;
			// The world's depth and its planes' states as recorded (dgVoodoo's one thread)
			ID3D12Resource *depth = nullptr;
			D3D12_RESOURCE_STATES plane[2] = { kUnknown, kUnknown };
			// Our views of the world
			ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
			ID3D12Resource *view_rt = nullptr, *view_ds = nullptr;
			DXGI_FORMAT view_rt_format = DXGI_FORMAT_UNKNOWN, view_ds_format = DXGI_FORMAT_UNKNOWN;
			// The camera of the last zone pass whose end was recorded (last_camera)
			frame_camera camera;
			bool have_camera = false;
			frame_lamp lamps[kMaxLamps]; // (and its lamps, when it was drawn here: last_lamps)
			uint32_t lamp_count = 0;
			// What's been drawn
			uint32_t last_seq = 0;
			uint64_t frame_number = 0, drawn = 0, failed = 0;
			std::atomic<ULONGLONG> failed_at { 0 };
			std::string why;
			draw_stats last;
			// The model actors' (on_actors_end): their own failures, so the zone's gate isn't shut by theirs
			uint32_t last_actor_seq = 0;
			uint64_t actors_drawn = 0, actors_failed = 0;
			std::atomic<ULONGLONG> actors_failed_at { 0 };
			std::string actors_why;
			draw_stats actors_last;
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

		void actors_fail(const char *why)
		{
			g.actors_failed++;
			g.actors_why = why;
			g.actors_failed_at.store(GetTickCount64(), std::memory_order_release);
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

		// A native pass into the world (g.mutex held): the depth's planes writable and the target a target (with the light
		// split as well, while World FX reads it), draw(t, o) recorded, then all as dgVoodoo left it, its heaps and state
		// bound again. Null, or why the world isn't ready for one.
		template <typename F>
		const char *draw_world(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const world &w, F draw)
		{
			if (g.renderer == nullptr || !g.renderer->ready() || g.device == nullptr)
				return "the renderer isn't made";
			if (w.rt == nullptr || w.ds == nullptr || w.ds != g.depth || w.rt_format == DXGI_FORMAT_UNKNOWN || w.ds_format == DXGI_FORMAT_UNKNOWN)
				return "the world's target isn't known here";
			if (g.plane[0] == kUnknown || g.plane[1] == kUnknown)
				return "the world's depth planes' states aren't known yet";
			D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;
			if (!views(w, rtv, dsv))
				return "views of the world couldn't be made";

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

			// Depth and stencil planes writable, the target a target; the pass; all as it was
			const D3D12_RESOURCE_STATES p0 = g.plane[0], p1 = g.plane[1];
			barrier(list, w.ds, 0, p0, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			barrier(list, w.ds, 1, p1, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, w.rt_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
			// (the light split as well, while World FX reads it: light_split.hpp)
			const bool lit = light::begin(list, g.device, w.rt, t.light_rtv);
			draw(t, o);
			if (lit)
				light::end(list);
			barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, w.rt_state);
			barrier(list, w.ds, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, p1);
			barrier(list, w.ds, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, p0);
			// (its heaps before its root tables: a table must be in the heaps bound when it's set)
			const auto known = g.lists.find(list);
			if (known != g.lists.end() && known->second.count != 0)
				list->SetDescriptorHeaps(known->second.count, known->second.h);
			bound.restore(list);
			return nullptr;
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
		std::unique_ptr<zone_shadow> made_shadow, made_second;
		if (device != nullptr)
		{
			made = std::make_unique<zone_renderer>();
			if (!made->init(device, why))
			{
				made->release();
				made.reset();
			}
			std::string shadow_why;
			made_shadow = std::make_unique<zone_shadow>();
			if (!made_shadow->init(device, shadow_why))
				made_shadow.reset(); // (the zone still draws: its shadows come from the bake then)
			made_second = std::make_unique<zone_shadow>();
			if (made_shadow == nullptr || !made_second->init(device, shadow_why))
				made_second.reset(); // (interiors then cast nothing)
		}
		std::unique_ptr<zone_renderer> old;
		std::unique_ptr<zone_shadow> old_shadow, old_second;
		ID3D12DescriptorHeap *old_rtv = nullptr, *old_dsv = nullptr;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			old = std::move(g.renderer);
			g.renderer = std::move(made);
			g.last_zone = g.last_second = nullptr;
			g.movers.clear();
			old_rtv = g.rtv_heap;
			old_dsv = g.dsv_heap;
			g.rtv_heap = g.dsv_heap = nullptr;
			g.view_rt = g.view_ds = nullptr;
			g.device = device;
		}
		{
			std::lock_guard<std::mutex> lock(g.shadow_mutex);
			old_shadow = std::move(g.shadow);
			old_second = std::move(g.second_shadow);
			g.shadow = std::move(made_shadow);
			g.second_shadow = std::move(made_second);
			g.shadow_up = g.shadow != nullptr;
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
		light::release(); // (both passes' target: let go with either)
		std::unique_ptr<zone_renderer> old;
		std::unique_ptr<zone_shadow> old_shadow, old_second;
		ID3D12DescriptorHeap *old_rtv = nullptr, *old_dsv = nullptr;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			old = std::move(g.renderer);
			g.last_zone = g.last_second = nullptr;
			g.movers.clear();
			old_rtv = g.rtv_heap;
			old_dsv = g.dsv_heap;
			g.rtv_heap = g.dsv_heap = nullptr;
			g.view_rt = g.view_ds = nullptr;
			g.device = nullptr;
			g.lists.clear();
			g.depth = nullptr;
			g.plane[0] = g.plane[1] = kUnknown;
		}
		{
			std::lock_guard<std::mutex> lock(g.shadow_mutex);
			old_shadow = std::move(g.shadow);
			old_second = std::move(g.second_shadow);
			g.shadow_up = false;
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
		std::snprintf(line, sizeof(line), "native zone renderer: sun shadow cascades from its data: %s; %llu drawn, %llu triangles in all",
			g.shadow_up ? "up" : "not made", static_cast<unsigned long long>(g.shadow_frames.load()), static_cast<unsigned long long>(g.shadow_triangles.load()));
		lines.push_back(line);
		const uint64_t mf = g.mover_frames.load();
		std::snprintf(line, sizeof(line), "native zone renderer: moving layer (swaying plants, doors) in the casters' map: %llu frames, %.0f instances and %.0f triangles a frame",
			static_cast<unsigned long long>(mf), mf != 0 ? double(g.mover_instances.load()) / double(mf) : 0.0, mf != 0 ? double(g.mover_triangles.load()) / double(mf) : 0.0);
		lines.push_back(line);
		std::snprintf(line, sizeof(line), "native zone renderer: model actors (ships, furniture, plants): %llu passes drawn, %llu couldn't be%s%s; the last: %u objects, %u draws, %u triangles",
			static_cast<unsigned long long>(g.actors_drawn), static_cast<unsigned long long>(g.actors_failed), g.actors_why.empty() ? "" : " (the last: ",
			g.actors_why.empty() ? "" : (g.actors_why + ")").c_str(), g.actors_last.objects, g.actors_last.draws, g.actors_last.triangles);
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

	bool world_depth(ID3D12Resource *&depth, D3D12_RESOURCE_STATES planes[2])
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (g.depth == nullptr || g.plane[0] == kUnknown || g.plane[1] == kUnknown)
			return false;
		depth = g.depth;
		planes[0] = g.plane[0];
		planes[1] = g.plane[1];
		return true;
	}

	bool last_camera(frame_camera &out)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (!g.have_camera)
			return false;
		out = g.camera;
		return true;
	}

	uint32_t last_lamps(frame_lamp out[kMaxLamps])
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (!g.have_camera)
			return 0;
		std::memcpy(out, g.lamps, size_t(g.lamp_count) * sizeof(frame_lamp));
		return g.lamp_count;
	}

	void on_zone_end(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const world &w, uint32_t seq_low4)
	{
		const frame *f = published(seq_low4);
		if (f == nullptr)
			return;
		{
			// (the pass's camera, drawn natively or not: World FX and FSR 2 take this frame's from here)
			std::lock_guard<std::mutex> lock(g.mutex);
			std::memcpy(g.camera.view, f->view, sizeof(g.camera.view));
			std::memcpy(g.camera.proj, f->proj, sizeof(g.camera.proj));
			g.camera.jitter[0] = f->jitter[0];
			g.camera.jitter[1] = f->jitter[1];
			std::memcpy(g.camera.sun_dir, f->sun_dir, sizeof(g.camera.sun_dir));
			g.camera.seq = f->seq.load(std::memory_order_acquire);
			g.camera.at = GetTickCount64();
			g.have_camera = true;
			g.lamp_count = f->native ? std::min(f->lamp_count, kMaxLamps) : 0;
			std::memcpy(g.lamps, f->lamps, size_t(g.lamp_count) * sizeof(frame_lamp));
		}
		if (!f->native)
			return;
		std::lock_guard<std::mutex> lock(g.mutex);
		const uint32_t seq = f->seq.load(std::memory_order_acquire);
		if (seq == g.last_seq)
			return;
		g.last_seq = seq;
		if (f->zone == nullptr || !f->zone->ready())
			return fail("the zone's data isn't on the GPU");
		if (const char *why = draw_world(list, bound, w, [&](const draw_target &t, const draw_options &o) { g.last = g.renderer->record(list, t, *f, o, ++g.frame_number); }))
			return fail(why);
		g.drawn++;
		g.last_zone = f->zone;
		g.last_second = f->second;
		g.last_zone_at = GetTickCount64();
		// (the movers it drew, for the moving layer: World FX asks for that later in this frame, in its shading pass)
		g.movers.clear();
		const gpu_zone *const zones[2] = { f->zone, f->second };
		for (uint32_t i = 0; i < std::min(f->count, kMaxObjects); ++i)
		{
			const frame_object &ob = f->objects[i];
			const gpu_zone *const z = ob.zone < 2 ? zones[ob.zone] : nullptr;
			if (z == nullptr || ob.model >= z->data().models.size())
				continue;
			if ((ob.flags & kDoor) != 0 || z->data().models[ob.model].kind == 2)
				g.movers.push_back(ob);
		}
	}

	bool actors_ready()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		const ULONGLONG at = g.actors_failed_at.load(std::memory_order_acquire);
		return g.renderer != nullptr && g.renderer->ready() && (at == 0 || GetTickCount64() - at > kFailMs);
	}

	void on_actors_end(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const world &w, uint32_t seq_low4)
	{
		const actor_frame *f = actor_published(seq_low4);
		if (f == nullptr || f->count == 0)
			return;
		std::lock_guard<std::mutex> lock(g.mutex);
		const uint32_t seq = f->seq.load(std::memory_order_acquire);
		if (seq == g.last_actor_seq)
			return;
		g.last_actor_seq = seq;
		draw_pass p;
		p.view_proj = f->view_proj;
		p.camera = f->camera;
		p.envs = &f->env;
		p.env_count = 1;
		p.lamps = f->lamps;
		p.lamp_count = f->lamp_count;
		p.zones = f->data;
		p.zone_count = std::min(f->data_count, kMaxActors);
		const uint32_t count = std::min(f->count, kMaxActors);
		if (const char *why = draw_world(list, bound, w,
				[&](const draw_target &t, const draw_options &o) { g.actors_last = g.renderer->record(list, t, p, f->objects, count, o, ++g.frame_number); }))
			return actors_fail(why);
		g.actors_drawn++;
	}

	namespace
	{
		constexpr ULONGLONG kZoneFreshMs = 250;
		// (the moving layer counts as drawn this long after it was last asked for: the near cascade's hole comes and goes
		// with it, not with a frame it missed)
		constexpr ULONGLONG kMoversFreshMs = 2000;

		// The zone drawn lately, else nullptr; the second map that pass drew (nullptr: none); and the list's heaps
		// (count 0: not known)
		gpu_zone *fresh_zone(ID3D12GraphicsCommandList *list, heaps &h, gpu_zone **second = nullptr)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			if (list != nullptr)
				if (const auto known = g.lists.find(list); known != g.lists.end())
					h = known->second;
			if (g.last_zone == nullptr || GetTickCount64() - g.last_zone_at > kZoneFreshMs)
				return nullptr;
			if (second != nullptr)
				*second = g.last_second;
			return g.last_zone;
		}

		bool movers_drawn()
		{
			const ULONGLONG at = g.movers_at.load(std::memory_order_relaxed);
			return at != 0 && GetTickCount64() - at <= kMoversFreshMs;
		}
	}

	// (the second map's placements as well, when the pass drew one: an interior's walls and roof shade its floor where
	// the sun reaches in; the box around both)
	bool zone_bounds(float lo[3], float hi[3], uint64_t &generation)
	{
		heaps h;
		gpu_zone *second = nullptr;
		gpu_zone *const z = fresh_zone(nullptr, h, &second);
		std::lock_guard<std::mutex> lock(g.shadow_mutex);
		if (z == nullptr || g.shadow == nullptr || !g.shadow->bounds(*z, lo, hi))
			return false;
		generation = (uint64_t(reinterpret_cast<uintptr_t>(z)) << 16) ^ z->data().map;
		float slo[3], shi[3];
		if (second != nullptr && g.second_shadow != nullptr && g.second_shadow->bounds(*second, slo, shi))
		{
			for (int a = 0; a < 3; ++a)
			{
				lo[a] = std::min(lo[a], slo[a]);
				hi[a] = std::max(hi[a], shi[a]);
			}
			generation ^= (uint64_t(second->data().map) << 40) ^ (uint64_t(g.second_shadow->mover_generation()) << 52);
		}
		// (the cascades drawn again when the moving layer comes or goes, or what they leave out changes)
		generation ^= (movers_drawn() ? uint64_t(1) << 62 : 0) ^ (uint64_t(g.shadow->mover_generation()) << 32);
		return true;
	}

	uint32_t record_zone_casters(ID3D12GraphicsCommandList *list, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size)
	{
		heaps h;
		gpu_zone *second = nullptr;
		gpu_zone *const z = fresh_zone(list, h, &second);
		if (z == nullptr || h.count == 0)
			return 0;
		std::lock_guard<std::mutex> lock(g.shadow_mutex);
		if (g.shadow == nullptr)
			return 0;
		const bool hole = movers_drawn();
		uint32_t triangles = g.shadow->record(list, *z, world_to_clip, dsv, size, hole);
		if (second != nullptr && g.second_shadow != nullptr)
			triangles += g.second_shadow->record(list, *second, world_to_clip, dsv, size, hole);
		// (the game's heaps back: World FX binds none of its own)
		list->SetDescriptorHeaps(h.count, h.h);
		g.shadow_frames++;
		g.shadow_triangles += triangles;
		return triangles;
	}

	bool record_zone_movers(ID3D12GraphicsCommandList *list, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size)
	{
		heaps h;
		gpu_zone *second = nullptr;
		gpu_zone *const z = fresh_zone(list, h, &second);
		if (z == nullptr || h.count == 0)
			return false;
		std::vector<frame_object> movers;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			movers = g.movers;
		}
		std::lock_guard<std::mutex> lock(g.shadow_mutex);
		if (g.shadow == nullptr)
			return false;
		g.movers_at.store(GetTickCount64(), std::memory_order_relaxed);
		g.shadow->note_pass(*z, movers.data(), uint32_t(movers.size()), 0);
		uint32_t triangles = g.shadow->record_movers(list, *z, world_to_clip, dsv, size);
		uint32_t instances = g.shadow->last_movers();
		if (second != nullptr && g.second_shadow != nullptr)
		{
			g.second_shadow->note_pass(*second, movers.data(), uint32_t(movers.size()), 1);
			triangles += g.second_shadow->record_movers(list, *second, world_to_clip, dsv, size);
			instances += g.second_shadow->last_movers();
		}
		list->SetDescriptorHeaps(h.count, h.h);
		g.mover_frames++;
		g.mover_triangles += triangles;
		g.mover_instances += instances;
		return triangles != 0;
	}

	grass::renderer::zone_bounds_fn zone_bounds_hook()
	{
		return [](void *, float lo[3], float hi[3], uint64_t *generation) { return zone_bounds(lo, hi, *generation); };
	}

	grass::renderer::zone_draw_fn zone_draw_hook()
	{
		return [](void *, ID3D12GraphicsCommandList *list, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size) {
			return record_zone_casters(list, world_to_clip, dsv, size);
		};
	}

	grass::renderer::caster_fn movers_hook()
	{
		return [](void *, ID3D12GraphicsCommandList *list, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size) {
			return record_zone_movers(list, world_to_clip, dsv, size);
		};
	}
}
