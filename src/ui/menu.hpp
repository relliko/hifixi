#pragma once

// HiFi XI's menu (/hifi): the presets in a dropdown (the built-in ones and your own, saved by name; Custom says which
// one it was changed from and goes back to it), a search box, what's waiting for the next start, the tabs down the
// left (settings/schema), each setting with its tooltip and a right-click reset, saved as it's changed. Drawn each
// frame it's open, through a gui (gui.hpp).

#include "gui.hpp"
#include "../settings/schema.hpp"
#include <string>
#include <vector>

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
	// The preset Custom was changed from, kept as the settings file keeps a preset ("ultra", "saved:<name>"; nullptr:
	// none): set as one is picked or saved
	void set_base_preset(const char *key);

	// Your own presets: every setting as it is now saved under a name (presets\<name>.ini beside the settings file; the
	// same name again: updated), listed by name, put back (kept as the one picked: "saved:<name>"), deleted. A name is
	// 1-40 letters, digits, spaces and - _ ( ) ' . and not one of the built-in presets'. False with why.
	std::vector<std::string> saved_presets();
	bool save_preset(const char *name, std::string &why);
	bool load_preset(const char *name, std::string &why);
	bool delete_preset(const char *name, std::string &why);
}
