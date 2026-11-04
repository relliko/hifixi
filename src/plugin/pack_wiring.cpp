/*
 * The texture pack's wiring (pack_wiring.hpp): the module's consumers in the order main.cpp has always added them (the
 * zones', then the characters'), their sources, and the FSR bias into both injects. The game's thread only.
 */

#include <windows.h>
#include "pack_wiring.hpp"
#include "../chars/charinject.hpp"
#include "../chars/charpack.hpp"
#include "../game/charmodel.hpp"
#include "../pack/pack_module.hpp"
#include "../upscale/upscale.hpp"
#include "../zone/draw.hpp"
#include "../zone/inject.hpp"
#include "../zone/light_split.hpp"
#include "../zone/zonepack.hpp"
#include <algorithm>
#include <atomic>

namespace hifi::pack_wiring
{
	namespace
	{
		std::atomic<float> g_bias { 0.0f };
		std::atomic<uint64_t> g_presents { 0 };
		// (game_drawn_models: the counts and presents at its last call)
		uint64_t g_last_drawn = 0, g_last_presents = 0;
		bool g_counted = false;
	}

	void wire(camera_fn camera, log_fn log, count_fn game_drawn)
	{
		// (the zones' and model actors' glue first: it orders a zone's textures from the last zone pass's camera and logs
		// each zone's coverage; its requests outrank the characters' too)
		zone::zonepack::set_camera_source(camera);
		zone::zonepack::set_log(log);
		pack::add_consumer(zone::zonepack::consumer());
		// (then the characters': they stream only onto the module's device, and say when the hifi-textures overlay keeps
		// their keys out of the pack)
		chars::charpack::sources s;
		s.device = pack::device_in_use;
		s.overlay_listed = pack::overlay_listed;
		s.game_drawn = game_drawn;
		s.log = log;
		chars::charpack::set_sources(s);
		pack::add_consumer(chars::charpack::consumer());
	}

	void follow_texture_bias()
	{
		g_presents.fetch_add(1, std::memory_order_relaxed);
		// (docs/texture-pack/zones.md "The FSR bias", characters.md "The FSR bias in chardraw": the pack's textures only)
		float ratio = 1.0f;
		const float bias = pack::fsr_bias_on() && upscale::world_scale(ratio) ? pack::fsr_bias(ratio) : 0.0f;
		g_bias.store(bias, std::memory_order_relaxed);
		zone::inject::set_texture_bias(bias);
		chars::inject::set_texture_bias(bias);
		// (M9: the light split's marks while the pack's heights are drawn: light_split.hpp "Marks")
		light::set_marks(pack::heights_in_use());
		// (and parallax occlusion on that relief, while [textures] parallax is on: zones.md "Parallax")
		zone::set_parallax(pack::parallax_depth());
	}

	float texture_bias()
	{
		return g_bias.load(std::memory_order_relaxed);
	}

	uint32_t game_drawn_models()
	{
		const game::charmodel::counts c = game::charmodel::get_counts();
		const uint64_t drawn = c.not_ready + c.cloth_refused + c.passes_refused, presents = g_presents.load(std::memory_order_relaxed);
		const bool first = !g_counted;
		const uint64_t models = drawn - std::min(drawn, g_last_drawn), frames = presents - std::min(presents, g_last_presents);
		g_counted = true;
		g_last_drawn = drawn;
		g_last_presents = presents;
		if (first || frames == 0)
			return 0;
		// (rounded up: one model the game drew in half a second still shows)
		return uint32_t(std::min<uint64_t>((models + frames - 1) / frames, UINT32_MAX));
	}
}
