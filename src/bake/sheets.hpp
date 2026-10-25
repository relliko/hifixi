#pragma once

// The game's own sheets, as sea.cpp and water.cpp read them (private to the bakers): reshade-worldfx's
// tools/bake_sea.py sheets(), every effect generator's model placed in the world (sea.cpp says how).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hifi::bake::sheets
{
	// Each generator's model (an MMB or D3M of the zone's DAT, `dat`) placed in the world, in DAT order, handed to
	// `each` as its corners (x, y, z each, three a triangle; never empty, all finite); a model that doesn't read, has no
	// triangles or a corner not finite is left out, as the Python leaves it. False with why when the game's key tables
	// aren't known.
	bool placed(const std::vector<uint8_t> &dat, const std::function<void(const std::vector<double> &world)> &each, std::string &why);
}
