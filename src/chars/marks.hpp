#pragma once

// The character markers (src/game/charslot.cpp, slotmark.hpp kinds 2-4) as HiFi XI's Direct3D 12 side reads them out of
// dgVoodoo's stream (backend.cpp ev_clear_depth, on dgVoodoo's recording thread): counted, and their order checked.
// A frame's character markers come as: begin (seq s), end (s), then a see-through marker (s) for each see-through
// actor the flush draws, then the next frame's begin (s + 1). The native character renderer will draw the opaque
// models at the end and each see-through one at its marker.

#include <cstdint>
#include <string>

namespace hifi::chars::marks
{
	// One marker seen (kind 2, 3 or 4; the seq's low 4 bits): whether it came in order
	bool seen(uint32_t kind, uint32_t seq);

	struct counts
	{
		uint64_t begin = 0, end = 0, see_through = 0, out_of_order = 0;
		uint32_t most_see_through = 0; // in one frame
	};
	counts get_counts();
	std::string status();
	void reset();
}
