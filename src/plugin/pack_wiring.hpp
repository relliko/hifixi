#pragma once

// The texture pack's wiring, as the plugin makes it (docs/texture-pack/characters.md decision 12): the "textures" module's
// consumers and their sources, and the FSR bias each present. In a file of its own so packjoin_test makes the same
// calls main.cpp does (M4's open item: main.cpp's wiring had no offline test).
//
//   - wire: the zones' and model actors' glue first (src/zone/zonepack: the camera to order a zone's textures by, the
//     log for each zone's coverage), then the characters' (src/chars/charpack: the module's device in use, whether
//     pivot.ini lists the hifi-textures overlay, the models the game drew itself, the log). Once, from the plugin's
//     Initialize (the module keeps a consumer added twice once).
//   - follow_texture_bias: each present on the game's thread, after the loaders' frames: the pack's textures sampled with
//     more detail under FSR while the pack is in use (log2 of the world's width over the screen's, at least -1; 0 else),
//     into both injects (the zone's and the characters'); each present is also counted for game_drawn_models.

#include <cstdint>

namespace hifi::pack_wiring
{
	using camera_fn = bool (*)(float out[3], uint32_t &map);
	using log_fn = void (*)(int level, const char *line);
	using count_fn = uint32_t (*)();

	void wire(camera_fn camera, log_fn log, count_fn game_drawn);
	void follow_texture_bias();
	// (the bias as last set: the tests)
	float texture_bias();

	// The characters' models the game drew itself, a present's worth over the time since the last call (the characters'
	// line asks every half second): charmodel's models drawn by the game as their data wasn't up yet, for their cloth and
	// for their extra passes. main.cpp hands it to wire
	uint32_t game_drawn_models();
}
