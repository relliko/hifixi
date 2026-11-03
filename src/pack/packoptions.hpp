#pragma once

// The texture pack's options ([textures] in HiFi XI's settings) and their defaults, in one place: the module
// (src/pack/pack_module.cpp) and the settings schema (the menu, the presets) read them from here.
//
//   [textures] pack            the texture pack used (the menu's "Use the texture pack"; /hifi textures on|off). Off by
//                              default: Alex turns it on in game. The Off preset sets it to 0, the others leave it
//
// Hidden (no menu entry; read by the module every half second unless said otherwise):
//   [textures] zone            the zone and its second map ask for pack textures (with pack on)
//   [textures] actors          the model actors (ships, furniture, plants) do
//   [textures] chars           the characters (gear, NPCs, monsters) do: at once, both ways
//   [textures] fsr_bias        under FSR the pack's textures sampled with log2(render / display) more detail
//   [textures] budget_mb       a video memory budget lower than Windows' (0: Windows')
//   [textures] ring_mb         the streamer's upload ring, 8-64 MB; read once, when the streamer starts (its address space
//                              isn't given back: docs/texture-pack/streamer.md "As built")
//   [textures] per_present_mb  record bytes the streamer starts in one present at most (0: no cap)
//   [textures] zone_delay_s    seconds after a zone-in before the pack streams (pack work stays off zoning's path)
// (height, material and surface: M9-M11's, not read yet)

namespace hifi::pack::options
{
	constexpr bool kPackByDefault = false;
	constexpr bool kZoneByDefault = true, kActorsByDefault = true, kCharsByDefault = true, kFsrBiasByDefault = true;
	constexpr float kBudgetMb = 0.0f, kRingMb = 32.0f, kPerPresentMb = 64.0f, kZoneDelayS = 2.0f;
}
