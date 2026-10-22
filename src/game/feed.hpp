#pragma once

// What the worldfx Ashita addon did, in HiFi XI (its Lua, ported): World FX told the zone (a Mog House as 1000 + its
// room's model, from the zone-in packet) and whether a character is in, sent the characters and monsters near, the
// weapon skills that cut the grass (from the action packet), the lamps lit, the glows (glows.hpp), the weather; and
// the game's lens flare, its water (gamewater.hpp), its rain and snow (gamerain.hpp) hidden, and its glow, lamp, flare
// and door visibility tests answered by World FX, as HiFi XI's settings say. Packets are only read: they go on to the
// game as they came.

#include "game.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::game::feed
{
	// You, as the party data has you
	struct party
	{
		uint32_t target_index = 0; // (0: not known)
		uint32_t server_id = 0;
		uint16_t zone = 0;         // (0 while zoning)
		bool in_game = false;      // logged in and not zoning
	};

	// Each present: World FX told and sent everything; every half second, the game's patches follow the settings
	void frame(const entities &e, const party &p, double now);
	// Each incoming packet (read only)
	void packet_in(uint16_t id, const uint8_t *data, uint32_t size, const entities &e);
	// Everything put back (the plugin unloaded) and forgotten
	void stop();
	// What it's doing, for /hifi status and the menu
	std::vector<std::string> status();
	// The zone told to World FX (1000 + the room's model in a Mog House), 0 before the first
	uint32_t zone();
}
