#include <windows.h>
#include "menu.hpp"
#include "../backend/backend.hpp"
#include "../bake/runner.hpp"
#include "../framegen/framegen.hpp"
#include "../game/drawcull.hpp"
#include "../game/feed.hpp"
#include "../game/hifps/exp.hpp"
#include "../game/hifps/step.hpp"
#include "../host/hooks.hpp"
#include "../render/worldfx_core.hpp"
#include "../upscale/upscale.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// How it looks: one window, the presets' dropdown across the top beside a search box (under them what's waiting for
// the next start), the tabs down the left, the page on the right. A page is sections, each a heading in the accent
// colour over a line about it and its settings; every setting has a tooltip and a right-click "Reset to default", and
// one that only matters with another on (the grass's distance with the grass) is greyed out while that's off. A
// change is saved at once and makes the preset Custom, which remembers the preset it was changed from.

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
			uint64_t made = 0;      // (frame generation's frames made between, so far)
			float made_rate = 0.0f; // (and a second, over the same second)
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
			const uint64_t made = framegen::made_between();
			g_meter.made_rate = g_meter.known ? float(double(made - std::min(made, g_meter.made)) / secs) : 0.0f;
			g_meter.made = made;
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

		// A number as the settings file keeps it: a whole one without a point, else the fewest digits that read back the same
		std::string number_text(float v, bool whole)
		{
			char text[32];
			if (whole || (v == std::floor(v) && std::fabs(v) < 1e7f))
				std::snprintf(text, sizeof(text), "%ld", std::lround(v));
			else
				for (int digits = 1; digits <= 9; ++digits)
				{
					std::snprintf(text, sizeof(text), "%.*g", digits, double(v));
					if (std::strtof(text, nullptr) == v)
						break;
				}
			return text;
		}

		std::string option_text(const settings::entry &e, float v)
		{
			if (e.kind == settings::option_kind::flag)
				return v >= 0.5f ? "1" : "0";
			return number_text(v, e.kind == settings::option_kind::whole);
		}

		void set_option_value(const settings::entry &e, float v)
		{
			char section[32];
			const char *name = nullptr;
			if (!settings::split_option(e.key, section, sizeof(section), &name))
				return;
			backend::set_option(section, name, option_text(e, v).c_str());
		}

		// (below, with the presets of yours: a setting's value as a preset file keeps it; the preset Custom was changed
		// from, as kept, its name as shown, the settings now different from it; the settings waiting for the next start)
		std::string entry_text(const settings::entry &e);
		std::string base_preset();
		std::string base_label(const std::string &base);
		std::vector<std::string> changed_from(const std::string &base);
		std::vector<std::string> waiting_for_restart();

		// A setting back to its default (one of HiFi XI's own: the schema's; World FX's: the core's), the preset Custom
		void reset_to_default(const settings::entry &e)
		{
			if (settings::is_option(e))
				set_option_value(e, e.fallback);
			else if (const worldfx::setting *s = worldfx::find_setting(e.key))
			{
				float v[3] = { s->fallback[0], s->fallback[1], s->fallback[2] };
				worldfx::set_setting(e.key, v);
			}
			backend::set_preset(settings::preset_key(settings::preset::custom));
		}

		// The right-click menu on the setting just drawn
		void draw_reset(const gui &g, const settings::entry &e)
		{
			if (!g.begin_context("reset"))
				return;
			if (g.selectable("Reset to default", false))
				reset_to_default(e);
			g.end_popup();
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
				if (e.next_start)
					help += " Takes effect the next time the game starts.";
				if (greyed)
					help += std::string(" (Only with \"") + (settings::find(e.needs) != nullptr ? settings::find(e.needs)->label : e.needs) + "\" on.)";
				g.tooltip(help.c_str());
			}
			draw_reset(g, e);
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
			draw_reset(g, e);
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
					if (const char *about = settings::section_help(section); about[0] != 0)
						g.text_disabled(about);
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

		// FSR and frame generation's settings, then what they're doing
		void draw_upscaling(const gui &g)
		{
			draw_tab_settings(g, settings::tab::upscaling);
			std::vector<std::string> lines = upscale::status();
			for (const std::string &line : framegen::status())
				lines.push_back(line);
			draw_lines(g, "What it's doing", lines);
		}

		void draw_performance(const gui &g)
		{
			heading(g, "Frames");
			if (g_meter.fps > 0.0f && g_meter.made_rate >= 1.0f)
			{
				// (with frame generation the game's own frames and those on the screen differ: both, so neither looks lost)
				textf(g, "%.0f fps from the game (%.1f ms a frame), %.0f shown: frame generation makes %.0f a second between them",
					g_meter.fps, g_meter.ms, g_meter.fps + g_meter.made_rate, g_meter.made_rate);
				g.text_disabled("The game itself runs at up to half the screen's refresh rate while frame generation is on.");
			}
			else if (g_meter.fps > 0.0f)
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
			// The en-spell effect on your weapons ([chars] enspell, as /hifi enspell sets it; the plugin follows it every
			// half second): the en-spell on you, off, or one held to look at it
			heading(g, "En-spell effects");
			g.text_wrapped("While an en-spell is on you, your weapons glow with its element. Pick one to see it now.");
			std::string mode;
			if (!backend::option("chars", "enspell", mode) || mode.empty())
				mode = "auto";
			static const char *const kModes[][3] = {
				{ "auto", "While one is on you", "The en-spell on you, in its element (Enfire to Endark, the IIs too)." },
				{ "off", "Off", "Never." },
				{ "fire", "Enfire", "Always Enfire's, to see it." },
				{ "ice", "Enblizzard", "Always Enblizzard's, to see it." },
				{ "wind", "Enaero", "Always Enaero's, to see it." },
				{ "earth", "Enstone", "Always Enstone's, to see it." },
				{ "thunder", "Enthunder", "Always Enthunder's, to see it." },
				{ "water", "Enwater", "Always Enwater's, to see it." },
				{ "light", "Enlight", "Always Enlight's, to see it." },
				{ "dark", "Endark", "Always Endark's, to see it." },
			};
			for (size_t i = 0; i < std::size(kModes); ++i)
			{
				if (i != 0 && i != 2 && i != 6)
					g.same_line();
				g.push_id(kModes[i][0]);
				if (g.toggle_button(kModes[i][1], mode == kModes[i][0]))
					backend::set_option("chars", "enspell", kModes[i][0]);
				g.tooltip(kModes[i][2]);
				g.pop_id();
			}
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
			draw_weather_debug(g);
			heading(g, "Files");
			textf(g, "Settings: %s", narrow(backend::settings_file()).c_str());
			textf(g, "Log: %shifixi.log", narrow(backend::folder()).c_str());
			textf(g, "Zone files: %s (worldfx-grass and the rest)", narrow(backend::folder()).c_str());
		}

		// The preset row: a dropdown of the built-in presets and your own, a name and Save (every setting as it is now,
		// under that name), Delete (the one of yours picked), the search; under it what the last of those did
		char g_save_name[48] = "";
		std::string g_preset_note;
		bool g_preset_note_bad = false;

		void note(bool ok, const std::string &text)
		{
			g_preset_note = text;
			g_preset_note_bad = !ok;
		}

		void draw_presets(const gui &g)
		{
			const std::string key = backend::preset();
			const bool yours = key.rfind("saved:", 0) == 0;
			const std::string your_name = yours ? key.substr(6) : std::string();
			const settings::preset now = yours ? settings::preset::custom : settings::preset_from(key.c_str());
			// (Custom: the preset it was changed from, when known)
			const std::string base = !yours && now == settings::preset::custom ? base_preset() : std::string();
			const std::string from = base.empty() ? std::string() : base_label(base);
			const std::string preview = yours ? your_name : !from.empty() ? "Custom (from " + from + ")" : std::string(settings::preset_label(now));
			g.text("Preset");
			g.same_line();
			g.set_next_item_width(220.0f);
			if (g.begin_combo("##preset", preview.c_str()))
			{
				if (!from.empty())
				{
					if (g.selectable(("Back to " + from).c_str(), false))
					{
						std::string why;
						if (base.rfind("saved:", 0) == 0)
							note(load_preset(from.c_str(), why), why.empty() ? "Back to " + from + "." : why);
						else
						{
							apply_preset(settings::preset_from(base.c_str()));
							note(true, "Back to " + from + ".");
						}
					}
					g.tooltip(("Every setting as " + from + " has it again: your changes since undone.").c_str());
					g.separator();
				}
				for (int i = 0; i < settings::kFixedPresets; ++i)
				{
					const settings::preset p = settings::preset(i);
					if (g.selectable(settings::preset_label(p), !yours && p == now))
					{
						apply_preset(p);
						note(true, std::string(settings::preset_label(p)) + " picked.");
					}
					g.tooltip(settings::preset_help(p));
				}
				const std::vector<std::string> names = saved_presets();
				if (!names.empty())
				{
					g.separator();
					for (const std::string &name : names)
					{
						g.push_id(name.c_str());
						if (g.selectable(name.c_str(), yours && name == your_name))
						{
							std::string why;
							note(load_preset(name.c_str(), why), why.empty() ? name + " picked." : why);
						}
						g.tooltip("One of yours: every setting as it was when you saved it.");
						g.pop_id();
					}
				}
				g.end_combo();
			}
			if (!from.empty())
			{
				// (what's been changed since, at most a dozen named)
				const std::vector<std::string> changed = changed_from(base);
				std::string tip = changed.empty() ? "Nothing differs from " + from + " any more." : "Changed from " + from + ": ";
				for (size_t i = 0; i < changed.size() && i < 12; ++i)
					tip += (i == 0 ? "" : ", ") + changed[i];
				if (changed.size() > 12)
					tip += " and " + std::to_string(changed.size() - 12) + " more";
				if (!changed.empty())
					tip += ". \"Back to " + from + "\" in the dropdown undoes them.";
				g.tooltip(tip.c_str());
			}
			else
				g.tooltip(yours ? "One of yours: every setting as it was when you saved it." : settings::preset_help(now));
			g.same_line();
			g.set_next_item_width(150.0f);
			g.input_text("##save-name", "Name to save as", g_save_name, sizeof(g_save_name));
			g.tooltip("A name for your own preset: letters, digits, spaces and - _ ( ) ' . (up to 40).");
			g.same_line();
			if (g.button("Save"))
			{
				std::string why;
				const std::string name = g_save_name;
				note(save_preset(g_save_name, why), why.empty() ? name + " saved." : why);
			}
			g.tooltip("Every setting as it is now, saved as a preset of your own under the name beside it (the same name again: updated). "
				"It's in the dropdown from then on.");
			g.same_line();
			g.begin_disabled(!yours);
			if (g.button("Delete"))
			{
				std::string why;
				note(delete_preset(your_name.c_str(), why), why.empty() ? your_name + " deleted (your settings stay as they are)." : why);
			}
			g.tooltip(yours ? "The preset of yours picked, deleted. Your settings stay as they are." : "Pick one of your own presets to delete it.");
			g.end_disabled();
			g.same_line();
			g.set_next_item_width(std::max(140.0f, g.available_width()));
			g.input_text("##find", "Find a setting", g_search, sizeof(g_search));
			if (!g_preset_note.empty())
			{
				if (g_preset_note_bad)
					g.text_colored(kWarn, g_preset_note.c_str());
				else
					g.text_disabled(g_preset_note.c_str());
			}
			const std::vector<std::string> waiting = waiting_for_restart();
			if (!waiting.empty())
			{
				std::string line = "Waiting for the game's next start: ";
				for (size_t i = 0; i < waiting.size(); ++i)
					line += (i == 0 ? "" : ", ") + waiting[i];
				g.text_colored(kWarn, (line + ".").c_str());
			}
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
		set_base_preset(settings::preset_key(p));
	}

	namespace
	{
		// (the name as typed, its spaces at the ends left out; false with why when it can't be a preset's)
		bool preset_name(const char *typed, std::string &name, std::string &why)
		{
			name = typed != nullptr ? typed : "";
			while (!name.empty() && name.back() == ' ')
				name.pop_back();
			while (!name.empty() && name.front() == ' ')
				name.erase(name.begin());
			if (name.empty())
			{
				why = "Type a name to save it as first.";
				return false;
			}
			if (name.size() > 40)
			{
				why = "That name's too long (40 letters at most).";
				return false;
			}
			bool letters = false;
			for (char c : name)
			{
				const bool alnum = std::isalnum(static_cast<unsigned char>(c)) != 0 && static_cast<unsigned char>(c) < 128;
				letters |= alnum;
				if (!alnum && std::strchr(" -_()'.", c) == nullptr)
				{
					why = "A name can only have letters, digits, spaces and - _ ( ) ' .";
					return false;
				}
			}
			if (!letters)
			{
				why = "A name needs a letter or a digit in it.";
				return false;
			}
			for (int i = 0; i <= int(settings::preset::custom); ++i)
				if (_stricmp(name.c_str(), settings::preset_label(settings::preset(i))) == 0)
				{
					why = "That's a built-in preset's name: pick another.";
					return false;
				}
			return true;
		}

		std::wstring presets_folder()
		{
			const std::wstring &file = backend::settings_file();
			const size_t slash = file.find_last_of(L"\\/");
			return (slash == std::wstring::npos ? std::wstring() : file.substr(0, slash + 1)) + L"presets\\";
		}

		std::wstring wide(const std::string &s)
		{
			return std::wstring(s.begin(), s.end()); // (names and values are ASCII)
		}

		std::wstring preset_path(const std::string &name)
		{
			return presets_folder() + wide(name) + L".ini";
		}

		// A setting's value as a preset file keeps it (a colour's three numbers with commas); empty for none
		std::string entry_text(const settings::entry &e)
		{
			if (settings::is_option(e))
				return option_text(e, option_value(e));
			const worldfx::setting *s = worldfx::find_setting(e.key);
			float v[3] = {};
			if (s == nullptr || !worldfx::get_setting(e.key, v))
				return std::string();
			if (s->kind == worldfx::setting::colour)
				return number_text(v[0], false) + "," + number_text(v[1], false) + "," + number_text(v[2], false);
			return number_text(v[0], s->kind != worldfx::setting::number);
		}

		// That text's numbers (up to three: a colour's), how many; 0 when it isn't numbers
		int values_of(const std::string &text, float v[3])
		{
			int count = 0;
			const char *at = text.c_str();
			while (count < 3 && *at != 0)
			{
				char *end = nullptr;
				const float x = std::strtof(at, &end);
				if (end == at || !std::isfinite(x))
					return 0;
				v[count++] = x;
				at = *end == ',' ? end + 1 : end;
			}
			return count;
		}

		// And put back from that (false when the text isn't a number)
		bool set_entry_text(const settings::entry &e, const std::string &text)
		{
			float v[3] = {};
			const int count = values_of(text, v);
			if (count == 0)
				return false;
			if (settings::is_option(e))
			{
				set_option_value(e, v[0]);
				return true;
			}
			const worldfx::setting *s = worldfx::find_setting(e.key);
			if (s == nullptr)
				return false;
			if (s->kind != worldfx::setting::colour || count < 3)
			{
				v[1] = s->fallback[1];
				v[2] = s->fallback[2];
			}
			worldfx::set_setting(e.key, v);
			return true;
		}

		// A setting's value(s) now, as numbers: how many (a colour's three)
		int values_now(const settings::entry &e, float v[3])
		{
			if (settings::is_option(e))
			{
				v[0] = option_value(e);
				return 1;
			}
			const worldfx::setting *s = worldfx::find_setting(e.key);
			if (s == nullptr || !worldfx::get_setting(e.key, v))
				return 0;
			return s->kind == worldfx::setting::colour ? 3 : 1;
		}

		bool differs(float a, float b)
		{
			return std::fabs(a - b) > std::max(1e-4f, 1e-4f * std::fabs(b));
		}

		// The settings that differ from the preset Custom was changed from, by label (looked at again at most once a second)
		struct changed_cache
		{
			std::string base;
			std::chrono::steady_clock::time_point at;
			std::vector<std::string> labels;
			bool known = false;
		};
		changed_cache g_changed;
	}

	namespace
	{
		std::string base_preset()
		{
			std::string base;
			return backend::option("hifixi", "preset_base", base) ? base : std::string();
		}

		std::string base_label(const std::string &base)
		{
			if (base.rfind("saved:", 0) == 0)
				return base.substr(6);
			const settings::preset p = settings::preset_from(base.c_str());
			return p == settings::preset::custom ? std::string() : std::string(settings::preset_label(p));
		}

		std::vector<std::string> changed_from(const std::string &base)
		{
			const auto now = std::chrono::steady_clock::now();
			if (g_changed.known && g_changed.base == base && now - g_changed.at < std::chrono::seconds(1))
				return g_changed.labels;
			std::vector<std::string> labels;
			size_t n = 0;
			const settings::entry *all = settings::entries(n);
			const bool yours = base.rfind("saved:", 0) == 0;
			const std::wstring path = yours ? preset_path(base.substr(6)) : std::wstring();
			const settings::preset p = settings::preset_from(base.c_str());
			for (size_t i = 0; i < n; ++i)
			{
				const settings::entry &e = all[i];
				float was[3] = {}, is[3] = {};
				int count = 0;
				if (yours)
				{
					wchar_t text[128] = L"";
					GetPrivateProfileStringW(L"settings", wide(e.key).c_str(), L"", text, DWORD(std::size(text)), path.c_str());
					std::string value;
					for (const wchar_t *c = text; *c != 0; ++c)
						value += *c < 128 ? char(*c) : '?';
					count = values_of(value, was);
				}
				else if (int(p) < settings::kFixedPresets && !std::isnan(e.presets[int(p)]))
				{
					was[0] = e.presets[int(p)];
					count = 1;
				}
				if (count == 0)
					continue;
				count = std::min(count, values_now(e, is));
				for (int k = 0; k < count; ++k)
					if (differs(is[k], was[k]))
					{
						labels.push_back(e.label);
						break;
					}
			}
			g_changed = { base, now, labels, true };
			return labels;
		}

		// The settings that take effect at the next start, as they were the first time the menu was drawn; and frame
		// generation, on but without AMD's swap chain this time (it's made only as the game starts)
		std::vector<std::pair<const settings::entry *, std::string>> g_at_start;
		bool g_start_known = false;

		std::vector<std::string> waiting_for_restart()
		{
			if (!g_start_known)
			{
				size_t n = 0;
				const settings::entry *all = settings::entries(n);
				for (size_t i = 0; i < n; ++i)
					if (all[i].next_start)
						g_at_start.emplace_back(&all[i], entry_text(all[i]));
				g_start_known = true;
			}
			std::vector<std::string> labels;
			for (const auto &[e, text] : g_at_start)
				if (entry_text(*e) != text)
					labels.push_back(e->label);
			if (const settings::entry *fg = settings::find("upscale.framegen"); fg != nullptr && option_value(*fg) >= 0.5f && !framegen::wanted())
				labels.push_back(fg->label);
			return labels;
		}
	}

	void set_base_preset(const char *key)
	{
		backend::set_option("hifixi", "preset_base", key);
		g_changed.known = false;
	}

	std::vector<std::string> saved_presets()
	{
		std::vector<std::string> names;
		if (backend::settings_file().empty())
			return names;
		WIN32_FIND_DATAW f;
		const HANDLE h = FindFirstFileW((presets_folder() + L"*.ini").c_str(), &f);
		if (h == INVALID_HANDLE_VALUE)
			return names;
		do
		{
			std::wstring file = f.cFileName;
			if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || file.size() <= 4)
				continue;
			file.resize(file.size() - 4);
			std::string name, why, unused;
			bool ascii = true;
			for (wchar_t c : file)
			{
				ascii &= c < 128;
				name += char(c);
			}
			if (ascii && preset_name(name.c_str(), unused, why) && unused == name)
				names.push_back(name);
		} while (FindNextFileW(h, &f));
		FindClose(h);
		std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
		return names;
	}

	bool save_preset(const char *typed, std::string &why)
	{
		std::string name;
		if (!preset_name(typed, name, why))
			return false;
		if (backend::settings_file().empty())
		{
			why = "The settings file isn't known: nothing saved.";
			return false;
		}
		CreateDirectoryW(presets_folder().c_str(), nullptr);
		const std::wstring path = preset_path(name);
		// (written fresh: an update keeps nothing of the old)
		DeleteFileW(path.c_str());
		bool ok = WritePrivateProfileStringW(L"hifixi", L"name", wide(name).c_str(), path.c_str()) != FALSE;
		size_t n = 0;
		const settings::entry *all = settings::entries(n);
		for (size_t i = 0; i < n && ok; ++i)
		{
			const std::string text = entry_text(all[i]);
			if (!text.empty())
				ok = WritePrivateProfileStringW(L"settings", wide(all[i].key).c_str(), wide(text).c_str(), path.c_str()) != FALSE;
		}
		if (!ok)
		{
			why = "Couldn't write " + name + "'s file in the presets folder (error " + std::to_string(GetLastError()) + ").";
			return false;
		}
		backend::set_preset(("saved:" + name).c_str());
		set_base_preset(("saved:" + name).c_str());
		std::snprintf(g_save_name, sizeof(g_save_name), "%s", name.c_str());
		return true;
	}

	bool load_preset(const char *typed, std::string &why)
	{
		std::string name;
		if (!preset_name(typed, name, why))
			return false;
		const std::wstring path = preset_path(name);
		if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			why = name + " isn't there any more (its file in the presets folder is gone).";
			return false;
		}
		size_t n = 0;
		const settings::entry *all = settings::entries(n);
		for (size_t i = 0; i < n; ++i)
		{
			// (a setting the preset doesn't have, newer than it: left as it is)
			wchar_t text[128] = L"";
			GetPrivateProfileStringW(L"settings", wide(all[i].key).c_str(), L"", text, DWORD(std::size(text)), path.c_str());
			std::string value;
			for (const wchar_t *c = text; *c != 0; ++c)
				value += *c < 128 ? char(*c) : '?';
			if (!value.empty())
				set_entry_text(all[i], value);
		}
		backend::set_preset(("saved:" + name).c_str());
		set_base_preset(("saved:" + name).c_str());
		std::snprintf(g_save_name, sizeof(g_save_name), "%s", name.c_str());
		return true;
	}

	bool delete_preset(const char *typed, std::string &why)
	{
		std::string name;
		if (!preset_name(typed, name, why))
			return false;
		if (!DeleteFileW(preset_path(name).c_str()))
		{
			why = "Couldn't delete " + name + " (error " + std::to_string(GetLastError()) + ").";
			return false;
		}
		if (backend::preset() == "saved:" + name)
			backend::set_preset(settings::preset_key(settings::preset::custom));
		if (base_preset() == "saved:" + name)
			set_base_preset(nullptr);
		if (_stricmp(g_save_name, name.c_str()) == 0)
			g_save_name[0] = 0;
		return true;
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
				case settings::tab::upscaling: draw_upscaling(g); break;
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
