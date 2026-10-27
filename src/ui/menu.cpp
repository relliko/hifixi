#include <windows.h>
#include "menu.hpp"
#include "../backend/backend.hpp"
#include "../bake/runner.hpp"
#include "../game/drawcull.hpp"
#include "../game/feed.hpp"
#include "../game/hifps/exp.hpp"
#include "../game/hifps/step.hpp"
#include "../host/hooks.hpp"
#include "../render/worldfx_core.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// How it looks: one window, the presets across the top beside a search box, the tabs down the left, the page on the
// right. A page is sections, each a heading in the accent colour over its settings; every setting has a tooltip, and
// one that only matters with another on (the grass's distance with the grass) is greyed out while that's off. A
// change is saved at once and makes the preset Custom.

namespace hifi::ui
{
	namespace
	{
		settings::tab g_tab = settings::tab::overview;
		char g_search[64] = "";
		const gui *g_now = nullptr; // (the gui being drawn with: worldfx::draw_status's ui calls into it)

		const float kAccent[4] = { 0.89f, 0.71f, 0.38f, 1.0f };
		const float kGood[4] = { 0.55f, 0.85f, 0.45f, 1.0f };
		const float kWarn[4] = { 0.95f, 0.75f, 0.30f, 1.0f };

		// The frame rate, from the frames the menu is drawn on (one a present while it's open), and the hooks' calls a
		// frame over the same second
		struct meter
		{
			std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
			uint32_t frames = 0;
			float fps = 0.0f, ms = 0.0f;
			host::stats last = {};
			double lists = 0.0, calls = 0.0, draws = 0.0;
			bool known = false;
		};
		meter g_meter;

		void measure()
		{
			g_meter.frames++;
			const auto now = std::chrono::steady_clock::now();
			const double secs = std::chrono::duration<double>(now - g_meter.since).count();
			if (secs < 1.0)
				return;
			const host::stats s = host::get_stats();
			const double frames = double(g_meter.frames);
			g_meter.fps = float(frames / secs);
			g_meter.ms = float(secs * 1000.0 / frames);
			if (g_meter.known)
			{
				uint64_t calls = 0, draws = 0;
				for (uint32_t i = 0; i < host::slot_count; ++i)
					if (host::is_list_slot(i))
						calls += s.calls[i] - std::min(s.calls[i], g_meter.last.calls[i]);
				draws = (s.calls[host::list_draw] - std::min(s.calls[host::list_draw], g_meter.last.calls[host::list_draw])) +
					(s.calls[host::list_draw_indexed] - std::min(s.calls[host::list_draw_indexed], g_meter.last.calls[host::list_draw_indexed]));
				g_meter.calls = double(calls) / frames;
				g_meter.draws = double(draws) / frames;
				g_meter.lists = double(s.lists_executed - std::min(s.lists_executed, g_meter.last.lists_executed)) / frames;
			}
			g_meter.last = s;
			g_meter.known = true;
			g_meter.frames = 0;
			g_meter.since = now;
		}

		template <typename... A>
		void textf(const gui &g, const char *format, A... args)
		{
			char s[512];
			std::snprintf(s, sizeof(s), format, args...);
			g.text(s);
		}
		template <typename... A>
		void colored(const gui &g, const float *rgba, const char *format, A... args)
		{
			char s[512];
			std::snprintf(s, sizeof(s), format, args...);
			g.text_colored(rgba, s);
		}

		void heading(const gui &g, const char *text)
		{
			g.spacing();
			g.text_colored(kAccent, text);
			g.separator();
		}

		std::string narrow(const std::wstring &w)
		{
			std::string s;
			for (wchar_t c : w)
				s += c < 128 ? char(c) : '?';
			return s;
		}

		// One of HiFi XI's own options ("section.name": hifps, drawcull): its value as a number (a flag's 0 or
		// 1), its default when it isn't set
		float option_value(const settings::entry &e)
		{
			char section[32];
			const char *name = nullptr;
			std::string text;
			if (!settings::split_option(e.key, section, sizeof(section), &name) || !backend::option(section, name, text))
				return e.fallback;
			if (e.kind == settings::option_kind::flag)
				return text == "0" || _stricmp(text.c_str(), "false") == 0 ? 0.0f : 1.0f;
			char *end = nullptr;
			const float v = std::strtof(text.c_str(), &end);
			return end != text.c_str() && std::isfinite(v) ? v : e.fallback;
		}

		void set_option_value(const settings::entry &e, float v)
		{
			char section[32];
			const char *name = nullptr;
			if (!settings::split_option(e.key, section, sizeof(section), &name))
				return;
			char text[32];
			if (e.kind == settings::option_kind::flag)
				std::snprintf(text, sizeof(text), "%d", v >= 0.5f ? 1 : 0);
			else if (e.kind == settings::option_kind::whole || (v == std::floor(v) && std::fabs(v) < 1e7f))
				std::snprintf(text, sizeof(text), "%ld", std::lround(v));
			else
				for (int digits = 1; digits <= 9; ++digits)
				{
					std::snprintf(text, sizeof(text), "%.*g", digits, double(v));
					if (std::strtof(text, nullptr) == v)
						break;
				}
			backend::set_option(section, name, text);
		}

		bool on(const char *key)
		{
			if (std::strchr(key, '.') != nullptr)
			{
				const settings::entry *e = settings::find(key);
				return e != nullptr && option_value(*e) >= 0.5f;
			}
			float v[3] = {};
			return worldfx::get_setting(key, v) && v[0] >= 0.5f;
		}

		// One of HiFi XI's own options' widget, saved when it's changed
		void draw_option(const gui &g, const settings::entry &e)
		{
			float v = option_value(e);
			const bool greyed = e.needs != nullptr && !on(e.needs);
			g.push_id(e.key);
			if (greyed)
				g.begin_disabled(true);
			bool changed = false;
			const float width = g.available_width() * 0.45f;
			switch (e.kind)
			{
			case settings::option_kind::flag:
			{
				bool b = v >= 0.5f;
				if (g.checkbox(e.label, &b))
				{
					v = b ? 1.0f : 0.0f;
					changed = true;
				}
				break;
			}
			case settings::option_kind::number:
				g.set_next_item_width(width);
				changed = g.slider_float(e.label, &v, e.low, e.high, e.format != nullptr ? e.format : "%.2f");
				break;
			case settings::option_kind::whole:
			{
				int i = int(std::lround(v));
				g.set_next_item_width(width);
				if (g.slider_int(e.label, &i, int(e.low), int(e.high)))
				{
					v = float(i);
					changed = true;
				}
				break;
			}
			}
			{
				std::string help = e.help;
				if (greyed)
					help += std::string(" (Only with \"") + (settings::find(e.needs) != nullptr ? settings::find(e.needs)->label : e.needs) + "\" on.)";
				g.tooltip(help.c_str());
			}
			if (greyed)
				g.end_disabled();
			g.pop_id();
			if (changed)
			{
				set_option_value(e, v);
				backend::set_preset(settings::preset_key(settings::preset::custom));
			}
		}

		// One setting's widget, saved when it's changed
		void draw_entry(const gui &g, const settings::entry &e)
		{
			if (settings::is_option(e))
			{
				draw_option(g, e);
				return;
			}
			const worldfx::setting *s = worldfx::find_setting(e.key);
			if (s == nullptr)
				return;
			float v[3] = {};
			worldfx::get_setting(e.key, v);
			const bool greyed = e.needs != nullptr && !on(e.needs);
			g.push_id(e.key);
			if (greyed)
				g.begin_disabled(true);
			bool changed = false;
			const float width = g.available_width() * 0.45f;
			switch (s->kind)
			{
			case worldfx::setting::flag:
			{
				bool b = v[0] >= 0.5f;
				if (g.checkbox(e.label, &b))
				{
					v[0] = b ? 1.0f : 0.0f;
					changed = true;
				}
				break;
			}
			case worldfx::setting::number:
				g.set_next_item_width(width);
				changed = g.slider_float(e.label, &v[0], s->low, s->high, e.format != nullptr ? e.format : "%.2f");
				break;
			case worldfx::setting::whole:
			{
				int i = int(std::lround(v[0]));
				g.set_next_item_width(width);
				if (g.slider_int(e.label, &i, int(s->low), int(s->high)))
				{
					v[0] = float(i);
					changed = true;
				}
				break;
			}
			case worldfx::setting::colour:
				changed = g.color_edit3(e.label, v);
				break;
			}
			{
				std::string help = e.help;
				if (e.next_start)
					help += " Takes effect the next time the game starts.";
				if (greyed)
					help += std::string(" (Only with \"") + (settings::find(e.needs) != nullptr ? settings::find(e.needs)->label : e.needs) + "\" on.)";
				g.tooltip(help.c_str());
			}
			if (greyed)
				g.end_disabled();
			g.pop_id();
			if (changed)
			{
				worldfx::set_setting(e.key, v);
				backend::set_preset(settings::preset_key(settings::preset::custom));
			}
		}

		// A tab's settings, section by section in the order they're listed
		void draw_tab_settings(const gui &g, settings::tab t)
		{
			size_t n = 0;
			const settings::entry *all = settings::entries(n);
			const char *section = nullptr;
			for (size_t i = 0; i < n; ++i)
			{
				if (all[i].where != t)
					continue;
				if (section == nullptr || std::strcmp(section, all[i].section) != 0)
				{
					section = all[i].section;
					heading(g, section);
				}
				draw_entry(g, all[i]);
			}
		}

		void draw_entry_by_key(const gui &g, const char *key)
		{
			if (const settings::entry *e = settings::find(key))
				draw_entry(g, *e);
		}

		bool contains(const char *haystack, const char *needle)
		{
			for (const char *h = haystack; *h != 0; ++h)
			{
				size_t k = 0;
				while (needle[k] != 0 && h[k] != 0 && std::tolower(static_cast<unsigned char>(h[k])) == std::tolower(static_cast<unsigned char>(needle[k])))
					++k;
				if (needle[k] == 0)
					return true;
			}
			return false;
		}

		// Everything whose name, tooltip or section has the search in it, under where it lives
		void draw_search(const gui &g)
		{
			size_t n = 0;
			const settings::entry *all = settings::entries(n);
			std::string place;
			uint32_t found = 0;
			for (size_t i = 0; i < n; ++i)
			{
				const settings::entry &e = all[i];
				if (!contains(e.label, g_search) && !contains(e.help, g_search) && !contains(e.section, g_search) && !contains(e.key, g_search))
					continue;
				const std::string here = std::string(settings::tab_name(e.where)) + " / " + e.section;
				if (here != place)
				{
					place = here;
					heading(g, here.c_str());
				}
				draw_entry(g, e);
				found++;
			}
			if (found == 0)
				g.text_disabled("No setting has that in its name or its description.");
		}

		void draw_overview(const gui &g)
		{
			heading(g, "HiFi XI");
			if (!backend::started())
				g.text_colored(kWarn, "Not started: its Direct3D 12 hooks aren't in (see /hifi status).");
			else if (backend::early())
				g.text_colored(kGood, "Started before the game's Direct3D 12 device (its starter, bootloader\\dxgi.dll).");
			else
				g.text_colored(kWarn, "Started late, without its starter (bootloader\\dxgi.dll): the world's views weren't seen, so nothing is drawn.");
			if (worldfx::attached() != nullptr)
				g.text_colored(kGood, "Drawing into the game's Direct3D 12 output (dgVoodoo).");
			else
				g.text_colored(kWarn, "Waiting for the game's Direct3D 12 output (dgVoodoo's OutputAPI must be d3d12_fl11_0).");
			if (GetModuleHandleW(L"ffxi_worldfx.addon32") != nullptr)
				g.text_colored(kWarn, "The World FX ReShade add-on is loaded too: ReShade belongs out of the bootloader folder.");

			heading(g, "At a glance");
			for (const char *key : { "hifps.enabled", "hifps.limit", "drawcull.world", "Grass", "SunShadows", "SunCharacters", "Water",
				"Rain", "Snow", "Sky", "LampGlow" })
				draw_entry_by_key(g, key);

			heading(g, "Its old addons");
			g.text_wrapped("HiFi XI does what hifps, drawcull and the worldfx addon did: leave them out of your boot script. One "
				"still loaded keeps its own changes to the game, and HiFi XI says which of its own it left out for that (Frame "
				"rate, Distance and display, The game's own).");
		}

		// Status lines under a heading
		void draw_lines(const gui &g, const char *title, const std::vector<std::string> &lines)
		{
			heading(g, title);
			for (const std::string &line : lines)
				g.text_wrapped(line.c_str());
		}

		void draw_frame_rate(const gui &g)
		{
			draw_tab_settings(g, settings::tab::frame_rate);
			static std::vector<std::string> checked;
			if (g.button("Check what uses Ashita's per-draw events"))
				checked = game::hifps::exp::ashita_check();
			g.tooltip("The plugins loaded now that would stop getting Ashita's per-draw events with \"Skip Ashita's per-draw "
				"dispatch\" on (Lua addons can't be listed from a plugin).");
			for (const std::string &line : checked)
				g.text_wrapped(line.c_str());
			std::vector<std::string> lines = game::hifps::status();
			for (const std::string &line : game::hifps::exp::status())
				lines.push_back(line);
			draw_lines(g, "What it's doing", lines);
		}

		void draw_distance(const gui &g)
		{
			draw_tab_settings(g, settings::tab::distance);
			draw_lines(g, "What it's doing", game::drawcull::status());
		}

		// Weather (debug): World FX's own weather forced to any weather (worldfx::force_weather, as /hifi weather), a
		// button each, the one forced picked; back to the real weather; a flash of lightning now. Not saved.
		void draw_weather_debug(const gui &g)
		{
			heading(g, "Weather (debug)");
			g.text_wrapped("Draws World FX's own weather as another weather's, whatever the zone's real one, fading over as a "
				"weather change does. Only World FX's drawing changes: the zone's light and fog still follow the real weather. "
				"Not saved: off at every start.");
			const std::string forced = worldfx::forced_weather();
			g.push_id("weather-debug"); // (its buttons apart from the settings named the same: Rain, Snow, Clouds)
			if (g.toggle_button("Off: the real weather", forced.empty()))
				worldfx::force_weather("off");
			g.tooltip("World FX's own weather follows the zone's real weather again (/hifi weather off).");
			size_t n = 0;
			const worldfx::weather_choice *c = worldfx::weather_choices(n);
			for (size_t i = 0; i < n; ++i)
			{
				if (i % 5 != 0)
					g.same_line();
				if (g.toggle_button(c[i].label, forced == c[i].name))
					worldfx::force_weather(c[i].name);
				char tip[96];
				std::snprintf(tip, sizeof(tip), "World FX's own weather drawn as %s (/hifi weather %s).", c[i].label, c[i].name);
				g.tooltip(tip);
			}
			if (g.button("Flash lightning now"))
				worldfx::flash_lightning();
			g.tooltip("A flash of lightning with its bolt now, whatever the weather (/hifi weather flash), while World FX draws the zone's weather.");
			g.pop_id();
			g.text_wrapped(worldfx::forced_weather_text().c_str());
		}

		// The world's settings, then the zone files HiFi XI makes from the game's own (once a zone, the first time you
		// enter it; or all at once)
		void draw_world(const gui &g)
		{
			draw_tab_settings(g, settings::tab::world);
			draw_weather_debug(g);
			heading(g, "Zone files");
			g.text_wrapped("Grass, water, rain, wet ground and the lamps come from each zone's own files in "
				"your game, made the first time you enter a zone (a few seconds, in the background). Or make them all now:");
			const bake::runner_status r = bake::runner::status();
			if (r.busy)
			{
				textf(g, "Making zone %u's files (%u of %u zones to go)...", r.zone, r.left, r.total);
				if (g.button("Stop"))
					bake::runner::cancel();
			}
			else if (g.button("Make every zone's files now"))
				bake::runner::build_all();
			g.tooltip("Every zone in your game, one after another in the background, those already made skipped (a few minutes).");
			for (const std::string &line : r.lines)
				g.text_wrapped(line.c_str());
		}

		void draw_performance(const gui &g)
		{
			heading(g, "Frames");
			if (g_meter.fps > 0.0f)
				textf(g, "%.0f fps (%.1f ms a frame)", g_meter.fps, g_meter.ms);
			else
				g.text_disabled("Measuring...");
			if (g_meter.known && host::installed())
			{
				textf(g, "dgVoodoo's Direct3D 12: %.1f command lists, %.0f draws and %.0f calls a frame through HiFi XI's hooks",
					g_meter.lists, g_meter.draws, g_meter.calls);
				const host::stats s = host::get_stats();
				if (s.world.width != 0)
					textf(g, "The world drawn at %llu x %u", static_cast<unsigned long long>(s.world.width), s.world.height);
			}
			heading(g, "World FX");
			static const worldfx::ui u = [] {
				worldfx::ui x;
				x.text = [](const char *s) { g_now->text(s); };
				x.text_colored = [](const float rgba[4], const char *s) { g_now->text_colored(rgba, s); };
				x.text_disabled = [](const char *s) { g_now->text_disabled(s); };
				x.text_wrapped = [](const char *s) { g_now->text_wrapped(s); };
				x.checkbox = [](const char *label, bool *v) { return g_now->checkbox(label, v); };
				x.slider_float = [](const char *label, float *v, float low, float high, const char *format) { return g_now->slider_float(label, v, low, high, format); };
				x.slider_int = [](const char *label, int *v, int low, int high) { return g_now->slider_int(label, v, low, high); };
				x.color_edit3 = [](const char *label, float rgb[3]) { return g_now->color_edit3(label, rgb); };
				x.button = [](const char *label) { return g_now->button(label); };
				x.header = [](const char *, bool) { return true; };
				return x;
			}();
			worldfx::draw_status(u);
		}

		// The game's own effects HiFi XI leaves out, and what its game-side parts are doing (what the worldfx addon did)
		void draw_game(const gui &g)
		{
			draw_tab_settings(g, settings::tab::game);
			heading(g, "What HiFi XI does with the game");
			for (const std::string &line : game::feed::status())
				g.text_wrapped(line.c_str());
		}

		void draw_advanced(const gui &g)
		{
			draw_tab_settings(g, settings::tab::advanced);
			g.spacing();
			if (g.button("Forget the grass colours learned"))
				worldfx::forget_colours();
			g.tooltip("Grass learns its colour from the ground again, from what's in view now.");
			heading(g, "Files");
			textf(g, "Settings: %s", narrow(backend::settings_file()).c_str());
			textf(g, "Log: %shifixi.log", narrow(backend::folder()).c_str());
			textf(g, "Zone files: %s (worldfx-grass and the rest)", narrow(backend::folder()).c_str());
		}

		void draw_presets(const gui &g)
		{
			const settings::preset now = settings::preset_from(backend::preset().c_str());
			g.text("Preset");
			for (int i = 0; i < settings::kFixedPresets; ++i)
			{
				const settings::preset p = settings::preset(i);
				g.same_line();
				if (g.toggle_button(settings::preset_label(p), p == now))
					apply_preset(p);
				g.tooltip(settings::preset_help(p));
			}
			if (now == settings::preset::custom)
			{
				g.same_line();
				g.text_disabled("Custom");
				g.tooltip(settings::preset_help(settings::preset::custom));
			}
			g.same_line();
			g.set_next_item_width(std::max(140.0f, g.available_width()));
			g.input_text("##find", "Find a setting", g_search, sizeof(g_search));
		}
	}

	void show_tab(settings::tab t)
	{
		g_tab = t;
		g_search[0] = 0;
	}

	settings::tab shown_tab()
	{
		return g_tab;
	}

	void set_search(const char *text)
	{
		std::snprintf(g_search, sizeof(g_search), "%s", text != nullptr ? text : "");
	}

	void apply_preset(settings::preset p)
	{
		if (int(p) >= settings::kFixedPresets)
			return;
		size_t n = 0;
		const settings::entry *all = settings::entries(n);
		for (size_t i = 0; i < n; ++i)
		{
			const settings::entry &e = all[i];
			// (a NaN: the preset leaves it as it is)
			if (std::isnan(e.presets[int(p)]))
				continue;
			if (settings::is_option(e))
			{
				set_option_value(e, e.presets[int(p)]);
				continue;
			}
			const worldfx::setting *s = worldfx::find_setting(e.key);
			if (s == nullptr)
				continue;
			float v[3] = { e.presets[int(p)], s->fallback[1], s->fallback[2] };
			worldfx::set_setting(e.key, v);
		}
		backend::set_preset(settings::preset_key(p));
	}

	void draw_menu(const gui &g, bool *open)
	{
		g_now = &g;
		measure();
		if (!g.begin("HiFi XI", open, 780.0f, 620.0f))
		{
			g.end();
			return;
		}
		draw_presets(g);
		g.separator();
		const bool searching = g_search[0] != 0;
		if (g.begin_child("tabs", 190.0f, 0.0f, true))
			for (int i = 0; i < int(settings::tab::count); ++i)
			{
				const settings::tab t = settings::tab(i);
				if (g.selectable(settings::tab_name(t), !searching && t == g_tab))
					show_tab(t);
			}
		g.end_child();
		g.same_line();
		if (g.begin_child("page", 0.0f, 0.0f, false))
		{
			if (searching)
				draw_search(g);
			else
				switch (g_tab)
				{
				case settings::tab::overview: draw_overview(g); break;
				case settings::tab::frame_rate: draw_frame_rate(g); break;
				case settings::tab::distance: draw_distance(g); break;
				case settings::tab::world: draw_world(g); break;
				case settings::tab::performance: draw_performance(g); break;
				case settings::tab::game: draw_game(g); break;
				case settings::tab::advanced: draw_advanced(g); break;
				default: draw_tab_settings(g, g_tab); break;
				}
			g.spacing();
			g.separator();
			g.text_disabled("Saved as you change them, in HiFi XI's own settings (/hifi status says where).");
		}
		g.end_child();
		g.end();
	}
}
