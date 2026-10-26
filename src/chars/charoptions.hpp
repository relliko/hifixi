#pragma once

// The native character renderer's options ([chars] in HiFi XI's settings) and their defaults, in one place: the
// plugin's follow functions and the settings schema (the menu, the presets) read them from here.
//
//   [chars] slot      the character passes' slot markers (/hifi charslot)
//   [chars] data      each drawn model recorded, its meshes and textures on the GPU (/hifi chardata)
//   [chars] native    HiFi XI draws the characters in the game's place (/hifi nativechars); brings slot and data
//   [chars] perpixel  ... lit per pixel (/hifi nativechars perpixel)
//
// On by default (Alex, 2026-10-25: "all these things should be on by default"): the option's fallback and the
// Balanced, High and Ultra presets follow these; the Off preset turns them off; /hifi nativechars off is the way back
// for one player. Characters are always skinned on the GPU (no switch: it's how HiFi XI draws them).

namespace hifi::chars::options
{
	constexpr bool kNativeByDefault = true;
	constexpr bool kPerPixelByDefault = true;
}
