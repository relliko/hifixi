#pragma once

// The native zone renderer's options ([zone] in HiFi XI's settings) and their defaults, in one place: the plugin's
// follow functions and the settings schema (the menu, the presets) read them from here.
//
//   [zone] slot      the zone pass's slot markers (/hifi zoneslot; on)
//   [zone] data      each zone's meshes, textures and normal maps on the GPU (/hifi zonedata); native brings it
//   [zone] native    HiFi XI draws the zone in the game's place (/hifi nativezone)
//   [zone] perpixel  ... lit per pixel, with the normal maps (/hifi nativezone perpixel)
//
// On by default (Alex, 2026-10-25: "all these things should be on by default"): the options' fallbacks, and the
// Balanced, High and Ultra presets with them (Off leaves the game drawing itself). /hifi nativezone off is the way
// back for one player; turning them off for everyone is these two lines.

namespace hifi::zone::options
{
	constexpr bool kNativeByDefault = true;
	constexpr bool kPerPixelByDefault = true;
}
