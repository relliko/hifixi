/*
 * HiFi XI's menu (src/ui/menu) drawn with a fake GUI that records what's drawn and clicks one widget when asked, on
 * its own settings in a folder of its own (made, as in the game, from a ReShade.ini there):
 *   - the settings file made from ReShade.ini's [FFXI_WORLDFX] ([worldfx], preset custom), ReShade.ini untouched; a
 *     file HiFi XI 0.10 left (its [mine] copy, the preset Mine) tidied: the copy out, the preset custom;
 *   - every tab drawn, every ImGui begin and push matched by its end and pop (ImGui stops the game otherwise);
 *   - each tab's own settings on it; a setting that needs another greyed out while that's off;
 *   - a preset picked: its values saved and it shown picked; the frame rate's options on in every preset;
 *   - a slider moved: saved, and the preset Custom; the search finding settings by name and description.
 *
 *   build\menu_test.exe     (from this folder: it works in build\test\menu)
 */

#include "../src/ui/menu.hpp"
#include "../src/backend/backend.hpp"
#include "../src/game/game.hpp"
#include "../src/render/worldfx_core.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" int __cdecl worldfx_setting(const char *key, char *out, uint32_t size);
extern "C" void __cdecl worldfx_weather(const uint32_t *words, uint32_t count);

namespace
{
	int failures = 0;
	void check(bool ok, const char *what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
		if (!ok)
			failures++;
	}

	// The fake GUI: what's drawn, in order (texts, and widgets as "[label]", a picked one "[label]*", a greyed one
	// "~label"), one widget to click or slider to move, and how deep each begin/push is
	namespace fake
	{
		std::vector<std::string> drawn;
		std::string click, slide;
		float slide_to = 0.0f;
		int windows = 0, children = 0, ids = 0, disabled = 0, greyed_depth = 0;
		bool unbalanced = false;
		std::vector<std::string> commands;

		void widget(const char *label, bool selected = false)
		{
			drawn.push_back(std::string(greyed_depth > 0 ? "~" : "[") + label + (greyed_depth > 0 ? "" : "]") + (selected ? "*" : ""));
		}
		bool clicked(const char *label)
		{
			if (!click.empty() && click == label)
			{
				click.clear();
				return true;
			}
			return false;
		}
		void reset()
		{
			drawn.clear();
			commands.clear();
		}
		bool has(const char *text)
		{
			for (const std::string &s : drawn)
				if (s.find(text) != std::string::npos)
					return true;
			return false;
		}
	}

	hifi::ui::gui make_gui()
	{
		hifi::ui::gui g;
		g.begin = [](const char *title, bool *, float, float) { fake::windows++; fake::drawn.push_back(std::string("window ") + title); return true; };
		g.end = [] { fake::unbalanced |= --fake::windows < 0; };
		g.begin_child = [](const char *, float, float, bool) { fake::children++; return true; };
		g.end_child = [] { fake::unbalanced |= --fake::children < 0; };
		g.same_line = [] {};
		g.separator = [] {};
		g.spacing = [] {};
		g.set_next_item_width = [](float) {};
		g.available_width = [] { return 500.0f; };
		g.push_id = [](const char *) { fake::ids++; };
		g.pop_id = [] { fake::unbalanced |= --fake::ids < 0; };
		g.begin_disabled = [](bool d) { fake::disabled++; if (d) fake::greyed_depth++; };
		g.end_disabled = [] { fake::unbalanced |= --fake::disabled < 0; if (fake::greyed_depth > fake::disabled) fake::greyed_depth = fake::disabled; };
		g.text = [](const char *s) { fake::drawn.push_back(s); };
		g.text_colored = [](const float *, const char *s) { fake::drawn.push_back(s); };
		g.text_disabled = [](const char *s) { fake::drawn.push_back(s); };
		g.text_wrapped = [](const char *s) { fake::drawn.push_back(s); };
		g.selectable = [](const char *label, bool selected) { fake::widget(label, selected); return fake::clicked(label); };
		g.button = [](const char *label) { fake::widget(label); return fake::clicked(label); };
		g.toggle_button = [](const char *label, bool selected) { fake::widget(label, selected); return fake::clicked(label); };
		g.checkbox = [](const char *label, bool *v) {
			fake::widget(label, *v);
			if (!fake::clicked(label))
				return false;
			*v = !*v;
			return true;
		};
		g.slider_float = [](const char *label, float *v, float, float, const char *) {
			fake::widget(label);
			if (fake::slide.empty() || fake::slide != label)
				return false;
			fake::slide.clear();
			*v = fake::slide_to;
			return true;
		};
		g.slider_int = [](const char *label, int *, int, int) { fake::widget(label); return false; };
		g.color_edit3 = [](const char *label, float *) { fake::widget(label); return false; };
		g.input_text = [](const char *, const char *hint, char *, size_t) { fake::drawn.push_back(std::string("input ") + hint); return false; };
		g.tooltip = [](const char *) {};
		g.command = [](const char *text) { fake::commands.push_back(text); };
		return g;
	}

	std::string read_all(const std::wstring &path)
	{
		std::string s;
		if (FILE *f = nullptr; _wfopen_s(&f, path.c_str(), L"rb") == 0 && f != nullptr)
		{
			char buffer[4096];
			size_t n;
			while ((n = fread(buffer, 1, sizeof(buffer), f)) != 0)
				s.append(buffer, n);
			fclose(f);
		}
		return s;
	}

	float value(const char *key)
	{
		float v[3] = {};
		worldfx::get_setting(key, v);
		return v[0];
	}

	std::string saved(const char *key)
	{
		char out[64] = "";
		return worldfx_setting(key, out, sizeof(out)) == 1 ? std::string(out) : std::string("(none)");
	}
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	// A folder of its own, with the old setup's ReShade.ini (some of Alex's settings, others left to their defaults)
	wchar_t here[MAX_PATH] = L"";
	GetFullPathNameW(L"build\\test\\menu", MAX_PATH, here, nullptr);
	CreateDirectoryW(L"build\\test", nullptr);
	CreateDirectoryW(here, nullptr);
	const std::wstring folder = std::wstring(here) + L"\\", settings_file = folder + L"config\\hifixi\\settings.ini", reshade_ini = folder + L"ReShade.ini";
	DeleteFileW(settings_file.c_str());
	const std::string old_ini = "[FFXI_WORLDFX]\r\nAOStrength=0\r\nGrass=1\r\nGrassDistance=80\r\nGrassHueR=0.3259837\r\nGrassHueG=0.42704624\r\n"
		"GrassHueB=0.23555931\r\nGrassTuft=12\r\nSunShadows=0.5\r\nSky=1\r\n\r\n[GENERAL]\r\nPerformanceMode=1\r\n";
	if (FILE *f = nullptr; _wfopen_s(&f, reshade_ini.c_str(), L"wb") == 0 && f != nullptr)
	{
		fwrite(old_ini.data(), 1, old_ini.size(), f);
		fclose(f);
	}
	// The addons HiFi XI took over, their settings as they keep them (Ashita's folder: this one's parent): hifps's,
	// drawcull's for two characters (the one saved last is taken)
	const std::wstring root = std::wstring(here).substr(0, std::wstring(here).find_last_of(L'\\') + 1);
	auto write_text = [](const std::wstring &path, const std::string &text) {
		for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1))
			CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
		if (FILE *f = nullptr; _wfopen_s(&f, path.c_str(), L"wb") == 0 && f != nullptr)
		{
			fwrite(text.data(), 1, text.size(), f);
			fclose(f);
		}
	};
	write_text(root + L"config\\addons\\hifps\\settings.lua", "-- hifps settings\nreturn {\n    enabled      = true,     -- run above 60fps\n"
		"    limit        = 0,      -- frame cap\n    counter      = false,\n    exp = {\n        ashita    = true,\n        walls     = true,\n"
		"        glows     = false,\n        sse       = false,\n    },\n    gfx = {\n        lamps = 1.25,\n        moon  = 1.00,\n    },\n};\n");
	write_text(root + L"config\\addons\\drawcull\\Other_1\\settings.lua", "require('common');\nlocal settings = T{ };\nsettings[\"world\"] = 1;\nreturn settings;\n");
	Sleep(50);
	write_text(root + L"config\\addons\\drawcull\\Relli_1542\\settings.lua", "require('common');\n\nlocal settings = T{ };\nsettings[\"lod\"] = 10;\n"
		"settings[\"frustum\"] = true;\nsettings[\"entity\"] = 10;\nsettings[\"world\"] = 5;\nsettings[\"square\"] = true;\nsettings[\"vis\"] = true;\n\nreturn settings;\n");
	// 0. A settings file as HiFi XI 0.10 left it (a copy of the old setup's, [mine], and the preset Mine): tidied
	{
		CreateDirectoryW((folder + L"config").c_str(), nullptr);
		CreateDirectoryW((folder + L"config\\hifixi").c_str(), nullptr);
		const std::string v010 = "; HiFi XI's settings.\n\n[hifixi]\npreset=mine\naddons=1\n\n[worldfx]\nAOStrength=0\nGrass=1\nGrassDistance=80\n"
			"SunShadows=0.5\nSky=1\n\n[mine]\nAOStrength=0\nGrass=1\nGrassDistance=80\nSunShadows=0.5\nSky=1\nhifps.limit=0\n";
		if (FILE *f = nullptr; _wfopen_s(&f, settings_file.c_str(), L"wb") == 0 && f != nullptr)
		{
			fwrite(v010.data(), 1, v010.size(), f);
			fclose(f);
		}
		check(hifi::backend::start_settings_only(settings_file.c_str(), folder.c_str()), "the settings alone started on a file HiFi XI 0.10 left");
		const std::string s = read_all(settings_file);
		check(hifi::backend::preset() == "custom" && s.find("[mine]") == std::string::npos && s.find("SunShadows=0.5") != std::string::npos,
			"tidied: its [mine] copy taken out, the preset custom, [worldfx] as it was");
		check(read_all(reshade_ini) == old_ini, "ReShade.ini as it was");
		DeleteFileW(settings_file.c_str());
	}

	check(hifi::backend::start_settings_only(settings_file.c_str(), folder.c_str()), "the settings alone started (no hooks)");

	// 1. Made from ReShade.ini
	{
		const std::string s = read_all(settings_file);
		check(s.find("[worldfx]") != std::string::npos && s.find("[mine]") == std::string::npos && s.find("preset=custom") != std::string::npos,
			"its settings file made from ReShade.ini: [worldfx], preset custom");
		check(value("GrassDistance") == 80.0f && value("SunShadows") == 0.5f && value("AOStrength") == 0.0f && value("Sky") == 1.0f,
			"the old setup's values in use");
		check(read_all(reshade_ini) == old_ini, "ReShade.ini as it was");
		check(saved("GrassTuft") == "12" && saved("LampGlow") == "(none)", "worldfx_setting: what's set, and nothing for what isn't");
	}
	// HiFi XI's own options (what hifps and drawcull did): taken from the addons' settings once, in their own sections
	{
		auto option = [](const char *section, const char *key) {
			std::string v;
			return hifi::backend::option(section, key, v) ? v : std::string("(none)");
		};
		check(option("hifps", "enabled") == "1" && option("hifps", "limit") == "0" && option("hifps", "counter") == "0" && option("hifps", "exp_walls") == "1" &&
			option("hifps", "exp_sse") == "0" && option("hifps", "gfx_lamps") == "(none)" && option("hifps", "exp_glows") == "(none)",
			"hifps's settings taken ([hifps]: its exp table as exp_; its gfx table, the game's own lights, and glows toggle left out)");
		check(option("drawcull", "world") == "5" && option("drawcull", "entity") == "10" && option("drawcull", "square") == "1",
			"drawcull's taken from the character whose settings were saved last");
		check(option("display", "aspect") == "(none)" && option("display", "mipmap") == "(none)", "nothing of aspect's or mipmap's (not HiFi XI's)");
		check(read_all(settings_file).find("[mine]") == std::string::npos, "and no copy of them kept elsewhere ([mine])");
	}

	const hifi::ui::gui g = make_gui();
	bool open = true;
	auto draw = [&] {
		fake::reset();
		hifi::ui::draw_menu(g, &open);
		return !fake::unbalanced && fake::windows == 0 && fake::children == 0 && fake::ids == 0 && fake::disabled == 0;
	};

	// 2. Every tab, balanced, with its own settings on it
	struct tab_check { hifi::settings::tab t; const char *expect; };
	const tab_check tabs[] = {
		{ hifi::settings::tab::overview, "At a glance" }, { hifi::settings::tab::frame_rate, "[Answer glow, lamp, lens flare and door tests without waiting]" },
		{ hifi::settings::tab::distance, "[Characters and monsters]" }, { hifi::settings::tab::lighting, "[Sun shadows]" },
		{ hifi::settings::tab::world, "[Rings from moving]" }, { hifi::settings::tab::performance, "World FX" },
		{ hifi::settings::tab::advanced, "[Direct3D 12 debug layer]" },
	};
	bool all_balanced = true, all_found = true;
	for (const tab_check &t : tabs)
	{
		hifi::ui::show_tab(t.t);
		all_balanced &= draw();
		all_found &= fake::has(t.expect);
		if (!fake::has(t.expect))
			std::printf("  (%s: no \"%s\")\n", hifi::settings::tab_name(t.t), t.expect);
	}
	check(all_balanced, "every tab drawn with its begins and pushes matched");
	check(all_found, "each tab with its own settings and text");
	hifi::ui::show_tab(hifi::settings::tab::overview);
	draw();
	check(fake::has("[Off]") && fake::has("[Balanced]") && fake::has("[High]") && fake::has("[Ultra]") && !fake::has("[Ultra]*") && !fake::has("[Mine]") &&
		fake::has("Custom"), "the presets across the top (Off, Balanced, High, Ultra), none picked: Custom");
	check(fake::has("Not started"), "the overview says HiFi XI's hooks aren't in (here, none)");

	// 3. A preset picked: its values in use and saved, it picked
	fake::click = "Ultra";
	draw();
	draw();
	check(value("GrassDistance") == 140.0f && value("AOStrength") == 0.8f && value("SunCharacters") == 1.0f, "Ultra: its values in use");
	check(saved("GrassDistance") == "140" && saved("AOStrength") == "0.8" && hifi::backend::preset() == "ultra" && fake::has("[Ultra]*"),
		"Ultra's values saved, and it shown picked");
	fake::click = "Off";
	draw();
	check(value("Grass") == 0.0f && value("Water") == 0.0f && value("SunShadows") == 0.0f && value("GrassDistance") == 140.0f,
		"Off: everything off, what it leaves (the grass's distance) left");
	check(value("VisTest") == 1.0f && hifi::game::option_on("hifps", "exp_walls", false) && hifi::game::option_on("hifps", "exp_dedupe", false) &&
		hifi::game::option_on("hifps", "enabled", false), "Off: the frame rate's options still on (on in every preset)");
	fake::click = "Balanced";
	draw();
	check(value("Grass") == 1.0f && value("Water") == 1.0f && value("SunShadows") == 0.5f && hifi::backend::preset() == "balanced",
		"Balanced: its values in use");

	// 4. A slider moved: saved, Custom
	hifi::ui::show_tab(hifi::settings::tab::world);
	fake::slide = "Distance";
	fake::slide_to = 120.0f;
	draw();
	draw();
	check(value("GrassDistance") == 120.0f && saved("GrassDistance") == "120" && hifi::backend::preset() == "custom" && fake::has("Custom"),
		"a slider moved: saved, and the preset Custom");

	// 5. Greyed out while what it needs is off
	draw();
	const bool distance_live = fake::has("[Distance]");
	fake::click = "Grass";
	draw();
	draw();
	check(distance_live && value("Grass") == 0.0f && fake::has("~Distance") && !fake::has("[Distance]"), "the grass's distance greyed out while the grass is off");
	fake::click = "Grass";
	draw();

	// 6. The search
	hifi::ui::set_search("rain");
	draw();
	check(fake::has("[Rain]") && fake::has("[How hard it rains]") && fake::has("[Wet paths and stone in rain]") && !fake::has("[Sun shadows]"),
		"the search: rain finds the rain's settings (and the wet ground's, by its name), not the sun's");
	hifi::ui::set_search("indirect light");
	draw();
	check(fake::has("[Ambient occlusion]"), "the search finds by description too");
	hifi::ui::set_search("zzzz");
	draw();
	check(fake::has("No setting has that"), "nothing found: said so");
	hifi::ui::set_search("");

	// 7. HiFi XI's own options (what hifps and drawcull did) in the menu
	hifi::ui::show_tab(hifi::settings::tab::frame_rate);
	draw();
	check(fake::has("[Run above 60 fps]") && fake::has("[Frame cap]") && fake::has("[Hide characters by walls without waiting]"),
		"the frame rate tab: hifps's settings");
	fake::click = "Run above 60 fps";
	draw();
	draw();
	check(fake::has("~Frame cap") && !fake::has("[Frame cap]") && hifi::game::option_on("hifps", "enabled", true) == false,
		"turned off: saved, and its cap greyed out");
	fake::click = "Run above 60 fps";
	draw();
	hifi::ui::show_tab(hifi::settings::tab::distance);
	fake::slide = "World";
	fake::slide_to = 7.0f;
	draw();
	draw();
	check(hifi::game::option_number("drawcull", "world", 0.0f) == 7.0f && hifi::backend::preset() == "custom", "a view distance slider moved: saved, Custom");
	fake::click = "Ultra";
	draw();
	check(hifi::game::option_number("drawcull", "world", 0.0f) == 7.0f, "Ultra leaves the view distance as it is");

	// 8. Weather (debug): the World tab's buttons force World FX's own weather; the look follows it, off restores the real one
	{
		uint32_t words[grass::kWeatherWords] = {};
		words[1] = grass::weather_name("fine");
		words[3] = 0x3F800000u; // (change 1.0)
		words[7] = 4;           // (the weather object read)
		worldfx_weather(words, grass::kWeatherWords);
		grass::look::weather_fx real, x;
		grass::weather w;
		worldfx::weather_look_now(real, &w);
		hifi::ui::show_tab(hifi::settings::tab::world);
		const bool balanced = draw();
		check(balanced && fake::has("Weather (debug)") && fake::has("[Off: the real weather]*") && fake::has("[Thunderstorm]") && fake::has("[Flash lightning now]") &&
			fake::has("light and fog still follow the real weather"), "the World tab: Weather (debug), Off picked, a button each, the lightning, light and fog said to stay real");
		fake::click = "Thunderstorm";
		draw();
		draw();
		worldfx::weather_look_now(x, &w);
		check(worldfx::forced_weather() == "bolt" && fake::has("[Thunderstorm]*") && !fake::has("[Off: the real weather]*") && fake::has("forced to bolt"),
			"Thunderstorm clicked: forced, shown picked and said");
		check(w.name == grass::weather_name("bolt") && w.before == grass::weather_name("fine") && w.change < 0.5f, "it fades over from the real weather (fine)");
		worldfx::force_weather("off", 0.0f);
		worldfx::force_weather("bolt", 0.0f);
		worldfx::weather_look_now(x, &w);
		check(w.name == grass::weather_name("bolt") && w.change == 1.0f && x.cloud_cover == 1.0f && real.cloud_cover < 0.5f && x.sky_dark > real.sky_dark,
			"forced: weather_look gives the thunderstorm's clouds, not the real weather's");
		fake::click = "Off: the real weather";
		draw();
		worldfx::weather_look_now(x, &w);
		check(worldfx::forced_weather().empty() && w.name == grass::weather_name("fine") && w.before == grass::weather_name("bolt") && w.change < 0.5f,
			"Off clicked: fading back to the real weather");
		worldfx::force_weather("bolt", 0.0f);
		worldfx::force_weather("off", 0.0f);
		worldfx::weather_look_now(x, &w);
		check(w.name == grass::weather_name("fine") && w.before == 0 && x.cloud_cover == real.cloud_cover && x.sky_dark == real.sky_dark &&
			x.wind_strength == real.wind_strength, "off: weather_look the real weather's again");
		check(!worldfx::force_weather("nope") && worldfx::forced_weather().empty() && worldfx::force_weather("Gales") && worldfx::forced_weather() == "stom",
			"a name it doesn't know refused; a label works as a name");
		worldfx::force_weather("off", 0.0f);
		fake::click = "Flash lightning now";
		check(draw() && fake::click.empty(), "Flash lightning now clicked");
	}

	// 9. En-spell effects: the Game tab's buttons set [chars] enspell, the one picked shown so
	{
		hifi::ui::show_tab(hifi::settings::tab::game);
		const bool balanced = draw();
		check(balanced && fake::has("En-spell effects") && fake::has("[While one is on you]*") && fake::has("[Enfire]") && fake::has("[Endark]"),
			"the Game tab: En-spell effects, following the en-spell on you, a button for each");
		fake::click = "Enthunder";
		draw();
		draw();
		check(hifi::game::option_text("chars", "enspell", "") == "thunder" && fake::has("[Enthunder]*") && !fake::has("[While one is on you]*"), "Enthunder clicked: saved, shown picked");
		fake::click = "While one is on you";
		draw();
		check(hifi::game::option_text("chars", "enspell", "") == "auto", "back to following the en-spell on you");
	}

	check(read_all(reshade_ini) == old_ini, "ReShade.ini as it was, at the end");
	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures == 0 ? 0 : 1;
}
