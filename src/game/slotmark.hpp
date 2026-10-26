#pragma once

// The slot markers: how the game side tells HiFi XI's Direct3D 12 side where in dgVoodoo's stream one of the game's
// passes begins or ends (research/ffxi-re/notes-replace-slots.md, "Recommended: the one-pixel stencil clear").
//
// At each slot the game side clears one pixel of the world's own depth buffer's stencil to 0 through the game's
// Direct3D 8 device: Clear(1 rect, D3DCLEAR_STENCIL, stencil 0). dgVoodoo hands that on in place, as
// ClearDepthStencilView(the world's DSV, D3D12_CLEAR_FLAG_STENCIL, stencil 0, the same rect), on the list it's recording
// right then. The world never uses stencil, so the pixel changes nothing the game draws. The rect's x carries what it
// marks: 16 x kind + (seq & 15), in row 0. Should dgVoodoo ever scale the world, the pixel becomes an s x s square at
// s times that x: the D3D12 side divides by the square's side.
//
// Nothing else takes this shape: the game's own clears are full-size (the frame's target + depth, the second targets'
// depth only), and they always clear depth.

#include <cstdint>

namespace hifi::slotmark
{
	enum kind : uint32_t
	{
		zone_begin = 0, // just before the zone pass (FFXiMain 178CCE's call of 17B510)
		zone_end = 1,   // just after it
		// (the native characters' markers, src/game/charslot.cpp)
		chars_begin = 2, // just before the opaque actors' walk (0817E0's call of 082D70)
		chars_end = 3,   // just after it
		see_through = 4, // each see-through actor's callback from the priority-12 flush (0CBD90)
		kinds = 5,
	};
	inline const char *kind_name(uint32_t k)
	{
		return k == zone_begin ? "zone begin" : k == zone_end ? "zone end" : k == chars_begin ? "characters begin" : k == chars_end ? "characters end" :
			k == see_through ? "see-through character" : "?";
	}

	// The x the marker's pixel goes at (in the world's own pixels, row 0)
	constexpr uint32_t x_of(uint32_t k, uint32_t seq)
	{
		return 16 * k + (seq & 15);
	}

	// A depth clear read as a marker: only the stencil cleared, one rect, a square in row 0 at a multiple of its side,
	// below 16 x kinds of them. False for anything else.
	struct mark
	{
		uint32_t kind = 0, seq = 0; // (seq: its low 4 bits)
	};
	constexpr bool read(bool depth_flag, bool stencil_flag, uint32_t rect_count, long left, long top, long right, long bottom, mark &out)
	{
		if (depth_flag || !stencil_flag || rect_count != 1 || top != 0)
			return false;
		const long side = right - left;
		if (side < 1 || bottom - top != side || left < 0 || left % side != 0)
			return false;
		const long x = left / side;
		if (x >= long(16 * kinds))
			return false;
		out.kind = uint32_t(x) / 16;
		out.seq = uint32_t(x) % 16;
		return true;
	}
}
