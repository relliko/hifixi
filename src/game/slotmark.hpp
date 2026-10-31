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
		// (the own-frame passes, PLAN-own-frame.md: each handled by its module through src/host/passes.hpp)
		first_pass = 5,
		fx_run = 5,      // the effect draws recorded since the last fx_run or see-through marker (the priority-12 flush)
		sky_end = 6,     // just after the sky dome (frame loop step 5, before the zone)
		names_end = 7,   // just after the names pass (0817F0)
		screen = 8,      // the loop tail on A, before the resolve (crossfade, wash, afterimage)
		ui_begin = 9,    // just before the interface
		ui_end = 10,     // just after it (before the fade after the menus, the movie and the text queue)
		frame_end = 11,  // the game's last draw of the frame
		grid_end = 12,   // just after the Mog House layout grid (177B40)
		world_end = 13,  // our d3d8.dll: the first draw or copy that reads the world's target after its zone began (the
		                 // resolve), before it binds anything; never a stencil pixel (src/host/frameapi.hpp)
		spare_14 = 14, spare_15 = 15,
		kinds = 16,
	};
	// The kinds sent on the back buffer's own depth (the screen's 2D: src/game/uidraw), not the world's: the world's depth
	// tracking leaves them alone
	constexpr bool on_back_buffer(uint32_t k)
	{
		return k == ui_begin || k == ui_end || k == frame_end;
	}
	inline const char *kind_name(uint32_t k)
	{
		static const char *const names[kinds] = { "zone begin", "zone end", "characters begin", "characters end", "see-through character",
			"effects run", "sky end", "names end", "screen", "interface begin", "interface end", "frame end", "grid end", "world end", "spare 14", "spare 15" };
		return k < kinds ? names[k] : "?";
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
