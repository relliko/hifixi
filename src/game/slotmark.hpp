#pragma once

// The slot markers: how the game side tells HiFi XI's Direct3D 12 side where in the frame one of the game's passes
// begins or ends. The game side calls HiFi XI's own d3d8.dll (src/game/directmark.hpp), which hands the mark on in place
// among the game's draws, on its list, with what's bound (src/host/frameapi.hpp). Each carries a kind and the low 4 bits
// of its pass's seq.

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
		                 // resolve), before it binds anything (src/host/frameapi.hpp)
		resolve = 14,    // our d3d8.dll: right after the draw that ended the world (the resolve: the world read into the
		                 // target bound now), the draw's vertices with it (src/host/frameapi.hpp)
		spare_15 = 15,
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
			"effects run", "sky end", "names end", "screen", "interface begin", "interface end", "frame end", "grid end", "world end", "resolve", "spare 15" };
		return k < kinds ? names[k] : "?";
	}

	// A mark as handed on: its kind and its pass's seq (the low 4 bits)
	struct mark
	{
		uint32_t kind = 0, seq = 0;
	};
}
