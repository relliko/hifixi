#pragma once
// How strong a lamp's glow is, following the game's own light for it (research/ffxi-re/notes-lamp-hours.md). The game
// works a lamp's brightness out each frame: its curve over the day at the hour (lamps whose brightness follows the
// hour: off by day, ramping at dusk and dawn), times its fade with the camera's distance; that's what its global light
// table holds. The glow at a lamp's full brightness grows with that brightness and levels off (the game's light is
// colour x brightness, mostly 2-20, but saturates within its range at any of those); below full, in a ramp or fading
// with distance, it's that times the share of full brightness the game gives it now, so it dims as the game's does.
// `now` the game's brightness, `lit` the lamp's fully lit (the bake's, LMP2; 0 when not known: then it's taken as
// fully lit now, as before the hour was known), `setting` the glow's strength setting.

#include <algorithm>
#include <cmath>

namespace grass
{
	inline float lamp_glow_strength(float setting, float now, float lit)
	{
		if (!(now > 0.0f) || !std::isfinite(now))
			return 0.0f;
		const float full = std::isfinite(lit) ? std::max(lit, now) : now;
		return setting * (1.0f - std::exp(-full / 4.0f)) * (now / full);
	}
}
