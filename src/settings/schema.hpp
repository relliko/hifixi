#pragma once

// HiFi XI's menu, as data: every setting it shows (the core's by name, worldfx_core.hpp), the tab and section it's
// in, its label and tooltip, the setting it needs on to matter, and what each preset sets it to. The menu
// (src/ui/menu) draws from this; the presets apply from it. World FX's settings are the core's; HiFi XI's own options
// (hifps's and drawcull's) are keyed "section.name", their kind, range and default here.

#include <cstddef>
#include <cstdint>

namespace hifi::settings
{
	enum class tab : uint8_t { overview, frame_rate, distance, lighting, world, game, upscaling, performance, advanced, count };
	const char *tab_name(tab t);
	// A section's one line under its heading ("Sun and moon": what its settings are about); "" for none
	const char *section_help(const char *section);

	// The presets, in the order the menu shows them; custom: changed by hand since one was picked
	enum class preset : uint8_t { off, balanced, high, ultra, custom };
	constexpr int kFixedPresets = 4; // (off..ultra: their values are here)
	const char *preset_key(preset p);   // as kept in the settings file
	const char *preset_label(preset p); // as the menu shows it
	const char *preset_help(preset p);
	preset preset_from(const char *key);

	// What one of HiFi XI's own options is (World FX's settings say it themselves, in the core's table)
	enum class option_kind : uint8_t { flag, number, whole };

	struct entry
	{
		const char *key;     // the core's setting; or one of HiFi XI's own options, "section.name" ([hifps] limit...)
		tab where;
		const char *section;
		const char *label;
		const char *help;    // the tooltip
		const char *format;  // a slider's ("%.2f", "%.0f yalms"); nullptr for a checkbox or a colour
		const char *needs;   // a flag this one only matters with (greyed out while it's off), or nullptr
		bool next_start;     // takes effect the next time the game starts
		float presets[kFixedPresets]; // off, balanced, high, ultra (a NaN: the preset leaves it as it is)
		// (HiFi XI's own options only: what it is, its range and its default)
		option_kind kind = option_kind::flag;
		float low = 0.0f, high = 1.0f, fallback = 0.0f;
	};
	const entry *entries(size_t &count);
	const entry *find(const char *key);

	// An option's section and name ("hifps.limit": "hifps", "limit"); false for one of World FX's settings
	bool is_option(const entry &e);
	bool split_option(const char *key, char *section, size_t section_size, const char **name);
}
