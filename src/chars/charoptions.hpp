#pragma once

// The native character renderer's options ([chars] in HiFi XI's settings) and their defaults, in one place: the
// plugin's follow functions and the settings schema (the menu, the presets) read them from here.
//
//   [chars] perpixel  HiFi XI's characters lit per pixel (/hifi nativechars perpixel)
//   [chars] detail    detail maps guessed from each texture (tex::make_detail): relief, metal's shine, cavities; per
//                     pixel only (/hifi nativechars detail)
//
// HiFi XI always draws the characters itself (Alex, 2026-10-26: the game's own drawing no longer an option); per-pixel
// light on by default (Alex, 2026-10-25: "all these things should be on by default"): the option's fallback and the
// Balanced, High and Ultra presets follow it. Characters are always skinned on the GPU (no switch: it's how HiFi XI
// draws them).

namespace hifi::chars::options
{
	constexpr bool kPerPixelByDefault = true;
	// (off by default: a guess from the colours alone, 2026-10-30; Alex: "as long as its a toggle its fine")
	constexpr bool kDetailByDefault = false;
}
