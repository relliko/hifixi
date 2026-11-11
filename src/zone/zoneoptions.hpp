#pragma once

// The native zone renderer's options ([zone] in HiFi XI's settings) and their defaults, in one place: the plugin's
// follow functions and the settings schema (the menu, the presets) read them from here.
//
//   [zone] perpixel  HiFi XI's zone lit per pixel, with the normal maps (/hifi nativezone perpixel)
//
// HiFi XI always draws the zone itself (Alex, 2026-10-26: the game's own drawing no longer an option); per-pixel light
// on by default (Alex, 2026-10-25: "all these things should be on by default"): the option's fallback, and the Balanced,
// High and Ultra presets with it.

namespace hifi::zone::options
{
	constexpr bool kPerPixelByDefault = true;
}
