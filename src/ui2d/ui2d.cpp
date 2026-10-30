/*
 * The screen's 2D as an own-frame pass (PLAN-own-frame.md, docs/own-frame/2d.md): src/game/uidraw records it on the
 * game's thread, src/ui2d/render2d draws it on dgVoodoo's list at the markers, and this ties them together:
 *   - the markers (kinds 9, 10, 11 on the back buffer's depth, 8 on A's): 9 starts a window's segments, each 10, 11 or
 *     8 draws (or, while the game still draws them, checks) the window's next one into the target bound with that depth
 *     (A's window has no 9: its seq, new, starts its count);
 *   - the gate, each present: off unless [screen2d] native is on; shadow (the game draws, HiFi XI checks every
 *     segment) until the markers come through and 30 segments in a row could have been drawn with the target known;
 *     native (HiFi XI draws them) until a segment can't be, or the markers stop for a quarter second;
 *   - /hifi ui [on | off | shadow], the status lines, the device.
 */

#include <windows.h>
#include "fonthigh.hpp"
#include "frame2d.hpp"
#include "gate.hpp"
#include "render2d.hpp"
#include "../game/game.hpp"
#include "../game/slotmark.hpp"
#include "../game/uidraw.hpp"
#include "../host/passes.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace hifi::ui2d
{
	namespace
	{
		constexpr const char *kSection = "screen2d";

		// dgVoodoo's side (its thread)
		struct side_state
		{
			uint32_t seq = 0xFFFFFFFFu;  // the window whose segments come next (low 4 bits), its next segment
			uint32_t next = 0;
			// The target each depth was last bound with (dgVoodoo binds lazily: the list may not have it bound yet)
			ID3D12Resource *depth[4] = {}, *target[4] = {};
			DXGI_FORMAT format[4] = {};
			uint32_t learned = 0;
			std::mutex why_mutex;
			std::string why;
		};
		side_state side;
		std::atomic<ULONGLONG> ok_at { 0 }, fail_at { 0 };
		std::atomic<uint64_t> drawn { 0 }, checked { 0 }, failed { 0 }, in_a_row { 0 };

		struct game_state
		{
			bool wanted = false, shadow_only = false, font_high = false;
			std::string why;
		};
		game_state gs;

		void fail(const char *why)
		{
			failed.fetch_add(1, std::memory_order_relaxed);
			in_a_row.store(0, std::memory_order_relaxed);
			fail_at.store(GetTickCount64(), std::memory_order_release);
			std::lock_guard<std::mutex> lock(side.why_mutex);
			side.why = why;
		}

		bool target_for(const passes::world &w, ID3D12Resource *&target, DXGI_FORMAT &format)
		{
			if (w.bound_rt != nullptr && w.bound_ds == w.ds && w.bound_rt_format != DXGI_FORMAT_UNKNOWN)
			{
				target = w.bound_rt;
				format = w.bound_rt_format;
				for (uint32_t i = 0; i < 4; ++i)
					if (side.depth[i] == w.ds)
					{
						side.target[i] = target;
						side.format[i] = format;
						return true;
					}
				const uint32_t at = side.learned++ % 4;
				side.depth[at] = w.ds;
				side.target[at] = target;
				side.format[at] = format;
				return true;
			}
			for (uint32_t i = 0; i < 4; ++i)
				if (side.depth[i] == w.ds && side.target[i] != nullptr)
				{
					target = side.target[i];
					format = side.format[i];
					return true;
				}
			return false;
		}

		void on_begin(ID3D12GraphicsCommandList *, const grass::list_state &, const passes::world &w, uint32_t seq_low4)
		{
			side.seq = seq_low4 & 15u;
			side.next = 0;
			ID3D12Resource *target = nullptr;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			target_for(w, target, format);
			ok_at.store(GetTickCount64(), std::memory_order_release);
		}

		void on_segment(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const passes::world &w, uint32_t seq_low4)
		{
			const uint32_t seq = seq_low4 & 15u;
			if (seq != side.seq)
			{
				// (its begin wasn't seen: counted from here)
				side.seq = seq;
				side.next = 0;
			}
			const uint32_t k = side.next++;
			const frame *f = published(seq);
			if (f == nullptr)
			{
				fail("a window's frame isn't there");
				return;
			}
			if (k >= f->segments.load(std::memory_order_acquire))
			{
				fail("a segment isn't published");
				return;
			}
			const segment s = f->seg[k];
			ID3D12Resource *target = nullptr;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			if (!target_for(w, target, format))
			{
				fail("the target bound with the marker's depth isn't known yet");
				return;
			}
			// (the world's target as its barriers left it on this list; the back buffer is a target throughout)
			const D3D12_RESOURCE_STATES state = target == w.rt ? w.rt_state : D3D12_RESOURCE_STATE_RENDER_TARGET;
			// (the world, for the draws that copy it: A itself in A's window, else as the world's markers found it)
			render::world_source world;
			if (s.kind == slotmark::screen)
				world = { target, format, state };
			else
				world = { w.rt, w.rt_format, w.rt_state };
			if (const char *why = render::record(list, *f, s, target, format, bound, w.heap_count, w.heaps, s.native, state, world))
			{
				fail(why);
				return;
			}
			(s.native ? drawn : checked).fetch_add(1, std::memory_order_relaxed);
			in_a_row.fetch_add(1, std::memory_order_relaxed);
			ok_at.store(GetTickCount64(), std::memory_order_release);
		}

		// ---------------------------------------------------------------- the game's side

		void follow_options()
		{
			gs.wanted = game::option_on(kSection, "native", false);
			gs.shadow_only = game::option_on(kSection, "shadow", false);
			gs.font_high = game::option_on(kSection, "font_high", false);
		}

		void update(double)
		{
			follow_options();
			std::string why;
			if (gs.wanted && !game::uidraw::in())
			{
				if (!game::uidraw::start(why))
					gs.why = why;
			}
			else if (!gs.wanted && game::uidraw::in())
			{
				if (!game::uidraw::stop(why))
					gs.why = why;
			}
			// (the 2048 x 4096 text font: read from the game's files the first time it's asked for)
			if (gs.font_high && !fonthigh::loaded() && !fonthigh::load(why))
			{
				gs.why = "the 2048 x 4096 font: " + why;
				gs.font_high = false;
			}
			game::uidraw::set_font_high(gs.font_high && fonthigh::loaded());
		}

		void present(double now, bool)
		{
			render::collect(now);
			gate_in in;
			in.wanted = gs.wanted;
			in.shadow_only = gs.shadow_only;
			in.patched = game::uidraw::in();
			in.renderer = render::ready();
			DXGI_FORMAT format;
			uint32_t samples;
			in.target = render::target_known(format, samples);
			in.now_ms = GetTickCount64();
			in.ok_at = ok_at.load(std::memory_order_acquire);
			in.fail_at = fail_at.load(std::memory_order_acquire);
			in.in_a_row = in_a_row.load(std::memory_order_relaxed);
			in.was = gate_mode(game::uidraw::current());
			game::uidraw::set_mode(game::uidraw::mode(next_mode(in)));
		}

		std::vector<std::string> status()
		{
			std::vector<std::string> lines = game::uidraw::status();
			const render::stats s = render::get_stats();
			char line[400];
			std::snprintf(line, sizeof(line), "  Direct3D 12: %s; %llu segments drawn (%llu draws in %llu batches), %llu checked while the game drew, %llu failed; "
				"%u textures (%.1f MB), %u pipelines",
				render::ready() ? "ready" : "not made", static_cast<unsigned long long>(drawn.load()), static_cast<unsigned long long>(s.draws),
				static_cast<unsigned long long>(s.batches), static_cast<unsigned long long>(checked.load()), static_cast<unsigned long long>(failed.load()),
				s.textures, double(s.texture_bytes) / (1024.0 * 1024.0), s.pipelines);
			lines.push_back(line);
			{
				std::lock_guard<std::mutex> lock(side.why_mutex);
				if (!side.why.empty())
					lines.push_back("  last failure: " + side.why);
			}
			if (!gs.why.empty())
				lines.push_back("  " + gs.why);
			if (!gs.wanted)
				lines.push_back("  (off: /hifi ui on)");
			return lines;
		}

		bool stop(std::string &why)
		{
			game::uidraw::set_mode(game::uidraw::off);
			if (!game::uidraw::stop(why))
				return false;
			render::release();
			return true;
		}

		bool command(const std::vector<std::string> &args, std::vector<std::string> &out)
		{
			if (args.size() < 2 || args[1] == "status")
			{
				out = status();
				return true;
			}
			if (args[1] == "on" || args[1] == "off" || args[1] == "shadow")
			{
				game::set_option_on(kSection, "native", args[1] != "off");
				game::set_option_on(kSection, "shadow", args[1] == "shadow");
				follow_options();
				update(0.0);
				out.push_back(args[1] == "off" ? "Screen 2D: the game draws it." : args[1] == "shadow" ? "Screen 2D: recorded and checked, the game draws it."
					: "Screen 2D: HiFi XI draws it once its side is ready (the game until then).");
				return true;
			}
			if (args[1] == "font")
			{
				const bool on = args.size() > 2 ? args[2] == "on" : !gs.font_high;
				game::set_option_on(kSection, "font_high", on);
				follow_options();
				update(0.0);
				out.push_back(gs.font_high ? "Screen 2D: text drawn with the 2048 x 4096 font." : "Screen 2D: text drawn with the font the game set." +
					(gs.why.empty() ? std::string() : " (" + gs.why + ")"));
				return true;
			}
			out.push_back("/hifi ui [on | off | shadow | font [on | off] | status]");
			return true;
		}

		void device(ID3D12Device *dev)
		{
			// (the textures held belong to the store being let go)
			game::uidraw::set_mode(game::uidraw::off);
			game::uidraw::drop_textures();
			std::string why;
			if (dev == nullptr)
				render::release();
			else
			{
				if (!render::make(dev, why))
					gs.why = "the 2D renderer: " + why;
			}
		}

		passes::module make_module()
		{
			passes::module m;
			m.name = "ui";
			m.present = present;
			m.update = update;
			m.status = status;
			m.stop = stop;
			m.command = command;
			m.device = device;
			return m;
		}

		const passes::registrar r_module { make_module() };
		const passes::registrar r_begin { slotmark::ui_begin, on_begin };
		const passes::registrar r_end { slotmark::ui_end, on_segment };
		const passes::registrar r_frame { slotmark::frame_end, on_segment };
		const passes::registrar r_screen { slotmark::screen, on_segment };
	}
}
