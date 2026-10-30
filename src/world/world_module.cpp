/*
 * The world part of owning the frame (PLAN-own-frame.md): the sky dome, the names and the Mog House layout grid as
 * own-frame passes (src/host/passes.hpp). For each, a module (/hifi sky, /hifi names, /hifi grid) that puts its game
 * side in (src/game/skydraw, namedraw, griddraw) and opens its gate, and a handler for its marker (slotmark::sky_end,
 * names_end, worldmark::kGrid) that draws the published pass with src/world/render.
 *
 * The gate (each pass on its own): the game keeps drawing the pass until HiFi XI's side is up (the renderer made, the
 * font read for the names), its markers are coming through (over the last 32 half-seconds' counts, as the plugin checks
 * the zone's: at least one seen, no more than 8 fewer seen than sent), the world's target is known (the sky's marker
 * comes before the zone's, from which it's learnt), and none of its draws failed in the last two seconds. Its option
 * ([world] sky / names / grid, on by default) can keep it the game's.
 *
 * The names over the screen ([world] names_over, on by default): while the upscale draws (FSR: it called our overlay
 * in the last quarter second) the names' frame is kept at their marker and drawn by the overlay at the screen's
 * resolution (render.hpp names_over: the jitter taken out, the depth test against A's depth), after the upscaled world
 * and before the UI; otherwise into A at the marker. A frame kept and not drawn (the upscale didn't come) is counted.
 */

#include <windows.h>
#include "font.hpp"
#include "frames.hpp"
#include "render.hpp"
#include "../game/griddraw.hpp"
#include "../game/namedraw.hpp"
#include "../game/skydraw.hpp"
#include "../game/slotmark.hpp"
#include "../game/worldmark.hpp"
#include "../host/passes.hpp"
#include "../render/grass_render.hpp"
#include "../upscale/upscale.hpp"
#include "../zone/inject.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hifi::world
{
	namespace
	{
		constexpr D3D12_RESOURCE_STATES kUnknown = D3D12_RESOURCE_STATES(0xFFFFFFFF);
		constexpr ULONGLONG kFailMs = 2000, kOverMs = 250;
		enum pass : uint32_t { sky_pass, names_pass, grid_pass, passes_count };
		constexpr const char *kNames[passes_count] = { "sky", "names", "grid" };
		constexpr uint32_t kKinds[passes_count] = { slotmark::sky_end, slotmark::names_end, game::worldmark::kGrid };

		// Whether the markers sent are coming through (as the plugin's marker_flow)
		struct flow
		{
			static constexpr uint32_t kWindow = 32;
			uint64_t sent[kWindow] = {}, seen[kWindow] = {};
			uint32_t at = 0, filled = 0;
			bool update(uint64_t s, uint64_t e)
			{
				const uint32_t oldest = filled < kWindow ? 0 : at;
				const uint64_t s0 = filled != 0 ? sent[oldest] : s, e0 = filled != 0 ? seen[oldest] : e;
				sent[at] = s;
				seen[at] = e;
				at = (at + 1) % kWindow;
				filled = std::min(filled + 1, kWindow);
				const uint64_t ds = s - std::min(s, s0), de = e - std::min(e, e0);
				return s != 0 && e != 0 && de + 8 >= ds;
			}
		};

		struct per_pass
		{
			std::atomic<uint64_t> seen { 0 }, drawn { 0 }, failed { 0 };
			std::atomic<ULONGLONG> failed_at { 0 };
			std::string why; // (under the mutex)
			// The game's thread
			bool refused = false, native = false, flowing = false;
			std::string refused_why;
			flow f;
		};

		struct state
		{
			std::mutex mutex;
			ID3D12Device *device = nullptr;
			std::unique_ptr<renderer> r; // (made and swapped in outside the hooks' use: see device())
			bool pages = false;
			std::string pages_why = "not read yet";
			per_pass p[passes_count];
			std::atomic<bool> target_known { false };
			// Our views of the world (dgVoodoo's thread, under the mutex)
			ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
			ID3D12Resource *view_rt = nullptr, *view_ds = nullptr;
			DXGI_FORMAT view_rt_format = DXGI_FORMAT_UNKNOWN, view_ds_format = DXGI_FORMAT_UNKNOWN;
			uint64_t frame = 0;
			// The names over the screen
			std::atomic<ULONGLONG> over_at { 0 };
			std::atomic<bool> over_wanted { true }; // ([world] names_over, followed by update(): read on dgVoodoo's thread)
			const names_frame *pending = nullptr;
			uint32_t pending_seq = 0;
			std::atomic<uint64_t> over_drawn { 0 }, over_missed { 0 }, over_failed { 0 };
			std::string over_why;
			bool overlay_set = false;
		};
		state g;

		bool option(const char *key)
		{
			return game::option_on("world", key, true);
		}

		void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
		{
			if (from == to)
				return;
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = res;
			b.Transition.Subresource = sub;
			b.Transition.StateBefore = from;
			b.Transition.StateAfter = to;
			list->ResourceBarrier(1, &b);
		}

		void fail(pass k, const char *why)
		{
			g.p[k].failed++;
			g.p[k].why = why;
			g.p[k].failed_at.store(GetTickCount64(), std::memory_order_release);
		}

		// Our views of the world's target and depth (mutex held; made again when they change)
		bool views(const passes::world &w, D3D12_CPU_DESCRIPTOR_HANDLE &rtv, D3D12_CPU_DESCRIPTOR_HANDLE &dsv)
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

		// The world as a draw target (mutex held): null, or why it isn't one
		const char *world_target(const passes::world &w, target &t)
		{
			if (g.r == nullptr || !g.r->ready() || g.device == nullptr)
				return "the renderer isn't made";
			if (w.rt == nullptr || w.ds == nullptr || w.rt_format == DXGI_FORMAT_UNKNOWN || w.ds_format == DXGI_FORMAT_UNKNOWN)
				return "the world's target isn't known here";
			const D3D12_RESOURCE_DESC rd = w.rt->GetDesc(), dd = w.ds->GetDesc();
			if (rd.Width != dd.Width || rd.Height != dd.Height)
				return "the world's target and depth differ in size (a target from before a change)";
			if (!views(w, t.rtv, t.dsv))
				return "views of the world couldn't be made";
			t.rtv_format = w.rt_format;
			t.dsv_format = w.ds_format;
			t.samples = rd.SampleDesc.Count;
			t.width = UINT(rd.Width);
			t.height = rd.Height;
			return nullptr;
		}

		// A pass into the world at its marker: the target a target (and the depth's planes writable when `depth`), draw(t),
		// then all as dgVoodoo left it
		template <typename F>
		void into_world(pass k, ID3D12GraphicsCommandList *list, const grass::list_state &bound, const passes::world &w, bool depth, F draw)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			target t;
			if (const char *why = world_target(w, t))
			{
				fail(k, why);
				return;
			}
			D3D12_RESOURCE_STATES planes[2] = { kUnknown, kUnknown };
			if (depth && !zone::inject::depth_planes(w.ds, planes))
			{
				fail(k, "the world's depth planes' states aren't known yet");
				return;
			}
			if (!depth)
				t.dsv_format = DXGI_FORMAT_UNKNOWN;
			if (depth)
			{
				barrier(list, w.ds, 0, planes[0], D3D12_RESOURCE_STATE_DEPTH_WRITE);
				barrier(list, w.ds, 1, planes[1], D3D12_RESOURCE_STATE_DEPTH_WRITE);
			}
			barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, w.rt_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
			std::string why;
			const bool ok = draw(t, g.frame++, why);
			barrier(list, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, w.rt_state);
			if (depth)
			{
				barrier(list, w.ds, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, planes[1]);
				barrier(list, w.ds, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, planes[0]);
			}
			bound.restore(list);
			if (ok)
				g.p[k].drawn++;
			else
				fail(k, why.empty() ? "not drawn" : why.c_str());
		}

		// The marker handlers (dgVoodoo's thread)
		void on_sky(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const passes::world &w, uint32_t seq)
		{
			g.p[sky_pass].seen++;
			g.target_known.store(w.rt != nullptr && w.rt_format != DXGI_FORMAT_UNKNOWN, std::memory_order_relaxed);
			const sky_frame *const f = sky_published(seq);
			if (f == nullptr || !f->native)
				return;
			into_world(sky_pass, list, bound, w, false, [&](const target &t, uint64_t frame, std::string &why) { return g.r->sky(list, t, *f, frame, why); });
		}

		// SRV formats of a depth resource's plane 0 (unknown: none can be made)
		DXGI_FORMAT depth_srv_format(DXGI_FORMAT f)
		{
			switch (f)
			{
			case DXGI_FORMAT_R24G8_TYPELESS:
				return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			case DXGI_FORMAT_R32G8X24_TYPELESS:
				return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			case DXGI_FORMAT_R32_TYPELESS:
				return DXGI_FORMAT_R32_FLOAT;
			case DXGI_FORMAT_R16_TYPELESS:
				return DXGI_FORMAT_R16_UNORM;
			default:
				return DXGI_FORMAT_UNKNOWN;
			}
		}

		void on_names(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const passes::world &w, uint32_t seq)
		{
			g.p[names_pass].seen++;
			const names_frame *const f = names_published(seq);
			if (f == nullptr || !f->native)
				return;
			// (over the screen while the upscale draws, A's depth readable there)
			if (g.over_wanted.load(std::memory_order_relaxed) && GetTickCount64() - g.over_at.load(std::memory_order_relaxed) < kOverMs && w.ds != nullptr)
			{
				const D3D12_RESOURCE_DESC dd = w.ds->GetDesc();
				if (dd.SampleDesc.Count == 1 && depth_srv_format(dd.Format) != DXGI_FORMAT_UNKNOWN)
				{
					std::lock_guard<std::mutex> lock(g.mutex);
					if (g.pending != nullptr)
						g.over_missed++;
					g.pending = f;
					g.pending_seq = f->seq.load(std::memory_order_acquire);
					return;
				}
			}
			into_world(names_pass, list, bound, w, true, [&](const target &t, uint64_t frame, std::string &why) { return g.r->names(list, t, *f, frame, why); });
		}

		void on_grid(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const passes::world &w, uint32_t seq)
		{
			g.p[grid_pass].seen++;
			const grid_frame *const f = grid_published(seq);
			if (f == nullptr || !f->native || f->count == 0)
				return;
			bool depth = false;
			for (uint32_t i = 0; i < f->count && !depth; ++i)
				depth = f->lines[i].keep_z != 0;
			into_world(grid_pass, list, bound, w, depth, [&](const target &t, uint64_t frame, std::string &why) { return g.r->grid(list, t, *f, frame, why); });
		}

		// The upscale's overlay: the names kept at their marker, over the upscaled screen
		void overlay(ID3D12GraphicsCommandList *list, const upscale::overlay_target &o)
		{
			g.over_at.store(GetTickCount64(), std::memory_order_relaxed);
			std::lock_guard<std::mutex> lock(g.mutex);
			const names_frame *const f = g.pending;
			const uint32_t seq = g.pending_seq;
			g.pending = nullptr;
			if (f == nullptr)
				return;
			// (still that frame: the game's thread fills a slot again 16 frames on)
			if (f->seq.load(std::memory_order_acquire) != seq || g.r == nullptr || !g.r->ready())
			{
				g.over_missed++;
				return;
			}
			ID3D12Resource *depth = nullptr;
			D3D12_RESOURCE_STATES planes[2] = {};
			const char *why = nullptr;
			DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
			D3D12_RESOURCE_DESC dd = {};
			if (!zone::inject::world_depth(depth, planes))
				why = "the world's depth isn't known";
			else
			{
				dd = depth->GetDesc();
				srv_format = depth_srv_format(dd.Format);
				if (srv_format == DXGI_FORMAT_UNKNOWN || dd.SampleDesc.Count != 1)
					why = "the world's depth can't be read in a shader";
			}
			if (why != nullptr)
			{
				g.over_failed++;
				g.over_why = why;
				return;
			}
			const uint64_t frame = g.frame++;
			D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = srv_format;
			sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			sd.Texture2D.MipLevels = 1;
			g.device->CreateShaderResourceView(depth, &sd, g.r->heap_cpu(frame));
			target t;
			t.rtv = o.rtv;
			t.rtv_format = o.format;
			t.width = o.width;
			t.height = o.height;
			over_screen os;
			os.depth = g.r->heap_gpu(frame);
			os.a_width = UINT(dd.Width);
			os.a_height = dd.Height;
			const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barrier(list, depth, 0, planes[0], readable);
			std::string fail_why;
			const bool ok = g.r->names_over(list, t, os, *f, frame, fail_why);
			barrier(list, depth, 0, readable, planes[0]);
			if (ok)
				g.over_drawn++;
			else
			{
				g.over_failed++;
				g.over_why = fail_why;
			}
		}

		// The game's side of each pass: in, out, its gate (the game's thread, every half second)
		bool start_pass(pass k, std::string &why)
		{
			return k == sky_pass ? game::skydraw::start(why) : k == names_pass ? game::namedraw::start(why) : game::griddraw::start(why);
		}
		bool stop_pass(pass k, std::string &why)
		{
			return k == sky_pass ? game::skydraw::stop(why) : k == names_pass ? game::namedraw::stop(why) : game::griddraw::stop(why);
		}
		bool in_pass(pass k)
		{
			return k == sky_pass ? game::skydraw::in() : k == names_pass ? game::namedraw::in() : game::griddraw::in();
		}
		void set_native(pass k, bool on)
		{
			if (k == sky_pass)
				game::skydraw::set_native(on);
			else if (k == names_pass)
				game::namedraw::set_native(on);
			else
				game::griddraw::set_native(on);
		}

		void update_pass(pass k)
		{
			per_pass &p = g.p[k];
			if (!in_pass(k) && !p.refused && game::image_known() && option(kNames[k]))
			{
				std::string why;
				if (!start_pass(k, why))
				{
					p.refused = true; // (not tried again: the game draws the pass itself)
					p.refused_why = why;
				}
			}
			p.flowing = p.f.update(game::worldmark::get_counts().sent[kKinds[k]], p.seen.load(std::memory_order_relaxed));
			bool ready = false;
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				ready = g.r != nullptr && g.r->ready() && (k != names_pass || g.pages);
			}
			const bool failing = GetTickCount64() - p.failed_at.load(std::memory_order_acquire) < kFailMs;
			p.native = in_pass(k) && option(kNames[k]) && ready && p.flowing && !failing && (k != sky_pass || g.target_known.load(std::memory_order_relaxed));
			set_native(k, p.native);
		}

		void update(double)
		{
			game::worldmark::set_wanted(g.device != nullptr);
			g.over_wanted.store(option("names_over"), std::memory_order_relaxed);
			for (uint32_t k = 0; k < passes_count; ++k)
				update_pass(pass(k));
		}

		void device(ID3D12Device *device)
		{
			if (device == g.device)
				return;
			for (uint32_t k = 0; k < passes_count; ++k)
				set_native(pass(k), false);
			game::worldmark::set_wanted(false);
			if (device == nullptr)
			{
				// (the renderer's objects are left: dgVoodoo's lists may still hold them)
				std::lock_guard<std::mutex> lock(g.mutex);
				g.device = nullptr;
				return;
			}
			auto r = std::make_unique<renderer>();
			std::string why;
			if (!r->init(device, why))
			{
				for (per_pass &p : g.p)
					p.why = "the renderer couldn't be made: " + why;
				r.reset();
			}
			font_pages pages;
			bool pages_ok = false;
			std::string pages_why;
			if (r != nullptr && font::load(pages, pages_why))
				pages_ok = r->set_pages(pages, pages_why);
			std::unique_ptr<renderer> old;
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				g.device = device;
				old = std::move(g.r);
				g.r = std::move(r);
				g.pages = pages_ok;
				g.pages_why = pages_ok ? "read" : pages_why;
				g.pending = nullptr;
				g.view_rt = g.view_ds = nullptr;
				if (g.rtv_heap != nullptr)
					g.rtv_heap->Release();
				if (g.dsv_heap != nullptr)
					g.dsv_heap->Release();
				g.rtv_heap = g.dsv_heap = nullptr;
			}
			(void)old.release(); // (a renderer from another device: left, as the zone's are)
			if (!g.overlay_set)
			{
				upscale::set_overlay(overlay);
				g.overlay_set = true;
			}
		}

		std::string pass_line(pass k)
		{
			const per_pass &p = g.p[k];
			std::string why;
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				why = p.why;
			}
			std::string s = std::string(kNames[k]) + " (Direct3D 12): ";
			if (!option(kNames[k]))
				return s + "off ([world] " + kNames[k] + " = 0): the game draws it";
			if (p.refused)
				return s + "not in (" + p.refused_why + "): the game draws it";
			s += p.native ? "HiFi XI draws it; " : !p.flowing ? "the game draws it (its markers aren't coming through yet); " :
				"the game draws it (HiFi XI's side isn't ready, or failed lately); ";
			char counts[160];
			std::snprintf(counts, sizeof(counts), "markers seen %llu, drawn %llu, failed %llu", static_cast<unsigned long long>(p.seen.load()),
				static_cast<unsigned long long>(p.drawn.load()), static_cast<unsigned long long>(p.failed.load()));
			s += counts;
			if (p.failed.load() != 0)
				s += " (last: " + why + ")";
			return s;
		}

		std::vector<std::string> sky_status()
		{
			return { game::skydraw::status(), pass_line(sky_pass) };
		}
		std::vector<std::string> names_status()
		{
			std::vector<std::string> out = { game::namedraw::status(), pass_line(names_pass) };
			char line[300];
			std::string pages_why, over_why;
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				pages_why = g.pages_why;
				over_why = g.over_why;
			}
			std::snprintf(line, sizeof(line), "names: font %s; over the upscaled screen: %s, %llu frames drawn there, %llu kept and not drawn, %llu failed%s%s",
				pages_why.c_str(), option("names_over") ? (GetTickCount64() - g.over_at.load() < kOverMs ? "now (FSR draws)" : "when FSR draws") : "off ([world] names_over = 0)",
				static_cast<unsigned long long>(g.over_drawn.load()), static_cast<unsigned long long>(g.over_missed.load()), static_cast<unsigned long long>(g.over_failed.load()),
				over_why.empty() ? "" : " (last: ", over_why.empty() ? "" : (over_why + ")").c_str());
			out.push_back(line);
			return out;
		}
		std::vector<std::string> grid_status()
		{
			return { game::griddraw::status(), pass_line(grid_pass) };
		}

		bool stop_sky(std::string &why) { return stop_pass(sky_pass, why); }
		bool stop_names(std::string &why) { return stop_pass(names_pass, why); }
		bool stop_grid(std::string &why) { return stop_pass(grid_pass, why); }

		// /hifi sky|names|grid [on|off]; /hifi names over on|off
		bool command(pass k, const std::vector<std::string> &args, std::vector<std::string> &out)
		{
			if (args.size() >= 2 && (args[1] == "on" || args[1] == "off"))
			{
				const bool on = args[1] == "on";
				game::set_option_on("world", kNames[k], on);
				if (!on)
				{
					set_native(k, false);
					std::string why;
					if (!stop_pass(k, why))
						out.push_back(std::string(kNames[k]) + ": " + why);
				}
				g.p[k].refused = false;
				out.push_back(std::string(kNames[k]) + (on ? ": HiFi XI draws it when its side is ready" : ": the game draws it"));
				return true;
			}
			if (k == names_pass && args.size() >= 3 && args[1] == "over" && (args[2] == "on" || args[2] == "off"))
			{
				game::set_option_on("world", "names_over", args[2] == "on");
				out.push_back(args[2] == "on" ? "names: over the upscaled screen while FSR draws" : "names: into the world, always");
				return true;
			}
			out = k == sky_pass ? sky_status() : k == names_pass ? names_status() : grid_status();
			out.push_back(std::string("/hifi ") + kNames[k] + " on|off" + (k == names_pass ? "; /hifi names over on|off" : ""));
			return true;
		}
		bool sky_command(const std::vector<std::string> &a, std::vector<std::string> &o) { return command(sky_pass, a, o); }
		bool names_command(const std::vector<std::string> &a, std::vector<std::string> &o) { return command(names_pass, a, o); }
		bool grid_command(const std::vector<std::string> &a, std::vector<std::string> &o) { return command(grid_pass, a, o); }

		// (one module's update and device serve all three)
		passes::module make(const char *name, std::vector<std::string> (*status)(), bool (*stop)(std::string &), bool (*cmd)(const std::vector<std::string> &, std::vector<std::string> &),
			bool first)
		{
			passes::module m;
			m.name = name;
			m.status = status;
			m.stop = stop;
			m.command = cmd;
			if (first)
			{
				m.update = update;
				m.device = device;
			}
			return m;
		}

		const passes::registrar r_sky(make("sky", sky_status, stop_sky, sky_command, true));
		const passes::registrar r_names(make("names", names_status, stop_names, names_command, false));
		const passes::registrar r_grid(make("grid", grid_status, stop_grid, grid_command, false));
		const passes::registrar m_sky(slotmark::sky_end, on_sky);
		const passes::registrar m_names(slotmark::names_end, on_names);
		const passes::registrar m_grid(game::worldmark::kGrid, on_grid);
	}
}
