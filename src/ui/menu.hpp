#pragma once

// HiFi XI's menu (/hifi): presets across the top, a search box, the tabs down the left (settings/schema), each
// setting with its tooltip, saved as it's changed. Drawn each frame it's open, through a gui (gui.hpp).

#include "gui.hpp"
#include "../settings/schema.hpp"

namespace hifi::ui
{
	// open: false once the window's close box is clicked
	void draw_menu(const gui &g, bool *open);
	// The tab shown, and the search (for the tests, and /hifi <tab>)
	void show_tab(settings::tab t);
	settings::tab shown_tab();
	void set_search(const char *text);
	// A preset's values applied (mine: the old setup's), and the preset kept as the one picked
	void apply_preset(settings::preset p);
}
