/*
 * The effects as an own-frame pass (src/host/passes.hpp): the game side (src/game/fxdraw) followed every half second,
 * the native renderer (inject) at the fx_run markers, /hifi fx.
 *
 * The gate: the effects' sites are patched in while [fx] native is on (default on). Markers are sent while the plugin
 * has a Direct3D 12 device for us; elements are handed over only once markers are seen coming through (empty runs sent
 * as a probe until then), the renderer is up and nothing failed on its side for two seconds, and in a zone. Should the
 * markers stop coming through (sent, not seen, over a window of updates), the game draws again and the probe resumes.
 */

#include "fxinject.hpp"
#include "fxtextures.hpp"
#include "../game/fxdraw.hpp"
#include "../game/slotmark.hpp"
#include "../host/passes.hpp"
#include <algorithm>
#include <cstdio>

namespace hifi::fx
{
	namespace
	{
		namespace fd = hifi::game::fxdraw;

		struct flow
		{
			static constexpr uint32_t kWindow = 8; // (updates: four seconds)
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

		struct state
		{
			ID3D12Device *device = nullptr;
			bool refused = false, made_refused = false;
			std::string why;
			bool in_game = false, flowing = false;
			flow f;
			double last_tick = 0.0;
		};
		state g;

		bool wanted_on()
		{
			return game::option_on("fx", "native", true);
		}

		void present(double now, bool in_game)
		{
			g.in_game = in_game;
			fd::set_clock(now);
			if (now - g.last_tick > 1.0)
			{
				g.last_tick = now;
				textures::tick(now);
			}
		}

		void update(double)
		{
			const bool want = wanted_on();
			std::string why;
			if (want && !fd::in() && !g.refused)
			{
				if (!fd::start(why))
				{
					g.refused = true;
					g.why = why;
				}
			}
			else if (!want && fd::in())
				fd::stop(why);
			const bool sending = fd::in() && g.device != nullptr;
			fd::set_wanted(sending);
			g.flowing = sending && g.f.update(fd::get_counts().markers, inject::markers_seen());
			const bool native = sending && g.flowing && g.in_game && inject::ready();
			fd::set_native(native);
			fd::set_probe(sending && !native);
		}

		std::vector<std::string> status()
		{
			std::vector<std::string> lines = { fd::status() };
			if (g.refused)
				lines.push_back("effects: not patched in: " + g.why);
			for (const std::string &l : inject::status())
				lines.push_back(l);
			return lines;
		}

		bool stop(std::string &why)
		{
			fd::set_native(false);
			fd::set_wanted(false);
			if (!fd::stop(why))
				return false;
			textures::stop();
			return true;
		}

		bool command(const std::vector<std::string> &args, std::vector<std::string> &out)
		{
			if (args.size() >= 2 && (args[1] == "on" || args[1] == "off"))
			{
				game::set_option_on("fx", "native", args[1] == "on");
				g.refused = false;
				update(0.0);
			}
			out = status();
			out.push_back(std::string("/hifi fx on|off: HiFi XI drawing the effects (now ") + (wanted_on() ? "on" : "off") + ")");
			return true;
		}

		void device(ID3D12Device *device)
		{
			g.device = device;
			if (device == nullptr)
			{
				fd::set_native(false);
				fd::set_wanted(false);
				inject::release();
				return;
			}
			std::string why;
			if (!inject::prepare(device, why))
			{
				g.made_refused = true;
				g.why = why;
			}
		}

		void on_marker(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const passes::world &w, uint32_t seq_low4)
		{
			inject::on_run(list, bound, w, seq_low4);
		}

		const passes::registrar module_entry(passes::module { "fx", &present, &update, &status, &stop, &command, &device });
		const passes::registrar marker_entry(hifi::slotmark::fx_run, &on_marker);
	}
}
