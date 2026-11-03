/*
 * The game's key tables found in the running game's image (dat.hpp key_tables). Everything else of dat.hpp is in
 * dat_core.cpp, which needs no game:: code (tools\texpack\keys.cpp is the 64-bit texpack's key_tables).
 */

#include "dat.hpp"
#include "../game/game.hpp"

namespace hifi::bake
{
	bool key_tables(const uint8_t *&first, const uint8_t *&second)
	{
		// (the table's first 16 bytes: it's there twice, whole both times; the second table 0x328 on from the first)
		const uintptr_t at = hifi::game::find("E2E506A9ED26F44215F4817FDE9ADED0");
		if (at == 0)
			return false;
		first = reinterpret_cast<const uint8_t *>(at);
		second = reinterpret_cast<const uint8_t *>(at + 0x328);
		return true;
	}
}
