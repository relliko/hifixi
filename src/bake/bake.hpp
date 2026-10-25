#pragma once

// HiFi XI's zone bakers: reshade-worldfx's tools/bake_*.py in C++, run on the player's own PC from their own game
// files (dat.hpp), so nothing derived from Square Enix's data ships with HiFi XI. Each writes one zone's file for World
// FX, the same bytes the Python baker writes for that zone (the Python stays as the tests' oracle), whole (write_file:
// a reader never sees half of one). World FX looks for them in its folder: worldfx-<kind>\<zone>.bin.
//
// Each returns true when the file was written; false with `none` set when the zone simply has none of it (no grass,
// no water, no collision...: nothing written, not an error), or false with why when it failed. They run on a
// background thread (several at once for "Build all zones"): no shared state but what they're given.

#include <cstdint>
#include <string>

namespace hifi::bake
{
	struct result
	{
		bool none = false;   // (the zone has none of it: nothing written)
		std::string why;     // (when it failed, or why there's none)
	};

	// tools/bake_grass.py: the blades scattered over the zone's grass collision (Python's MT19937, seeded as it seeds)
	bool grass(uint32_t zone, const std::wstring &out, result &r);
	// tools/bake_water.py: the water surface (WTR2)
	bool water(uint32_t zone, const std::wstring &out, result &r);
	// tools/bake_wet.py: where the ground looks wet in rain (WET1)
	bool wet(uint32_t zone, const std::wstring &out, result &r);
	// tools/bake_rain.py: where rain lands (RHM1); water_file: the zone's worldfx-water file, when it has one
	bool rain(uint32_t zone, const std::wstring &out, const std::wstring &water_file, result &r);
	// tools/bake_shadow_geo.py: the zone's meshes for the sun's shadow map (compact SHD2)
	bool shadow(uint32_t zone, const std::wstring &out, result &r);
	// tools/bake_lamps.py: the zone's lamps (LMP2)
	bool lamps(uint32_t zone, const std::wstring &out, result &r);
	// tools/bake_sea.py: the game's own sea sheets that carry on past World FX's water, at its level (SEA1);
	// water_file: the zone's worldfx-water file (none without one)
	bool sea(uint32_t zone, const std::wstring &out, const std::wstring &water_file, result &r);
}
