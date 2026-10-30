#pragma once

// The names over characters handed to HiFi XI (research/ffxi-re/notes-ui-draw.md "The names", "For a native renderer >
// Names"; client-notes.md "Names over characters", "Name icons", "Names behind walls"; PLAN-own-frame.md, the world part).
//
// The names pass (CXiActorNameDraw's update 0817F0, task priority 12, after the effect flush) walks the actors; each
// name is 0D0880 -> 086210(pos, text, scale, colour, linkshell colour), each star line 0D08B0 -> 086900(pos, ...). Both
// lay their string out, project it, and per glyph bind its page (120590: 'font font' for letters, 'menu ustatshd' for the
// icons, whose colour becomes the linkshell's) and hand the device one quad (00CD00, DrawPrimitiveUP: a 4-vertex strip of
// XYZRHW vertices in A's pixels, z the name point's NDC z). Through ours:
//   - the pass itself (its vtable slot) begins a frame of the names ring (world/frames.hpp), and after it publishes the
//     frame and sends the names' end marker (slotmark::names_end) in the field;
//   - the two calls of 086210 / 086900 record each name (its arguments) around the game's own layout;
//   - their calls of 120590 note each glyph's page and ALPHAREF (0x60 when the colour it gave back is the linkshell
//     colour, as 086210 0867E6 and 086900 086D77 decide);
//   - their calls of 00CD00 record the glyph's quad and, with native drawing on, don't reach the device.
// So everything the game computes for a name stays its own: the projection, the layout, the icons, the glyph scale
// (nameplate.dll's where it has replaced the vanilla code at 086378 / 086A5C, as in both dumps; the vanilla code's
// otherwise), the colours. The Direct3D 12 side draws the quads at the names' end marker into the world (the depth
// contract), or over the upscaled screen after FSR.
//
// The game's thread only.

#include "game.hpp"
#include <string>

namespace hifi::game::namedraw
{
	// The patches in or out; false and why (nothing changed then)
	bool start(std::string &why);
	bool stop(std::string &why);
	bool in();
	// Whether HiFi XI draws the names (its Direct3D 12 side up with the font, the names' markers coming through)
	void set_native(bool on);

	// Our side of each patched call, as the game's callee takes it
	uint8_t __fastcall on_pass(uintptr_t task, uintptr_t edx);
	void __cdecl on_name(const float *pos, const char *text, float scale, uint32_t colour, uint32_t ls_colour);
	void __cdecl on_stars(const float *pos, uint32_t a2, uint32_t a3);
	uint8_t __fastcall on_page(uintptr_t font, uintptr_t edx, uint32_t ch, uint32_t *colour, uint32_t *ls_colour);
	long __fastcall on_glyph(uintptr_t wrapper, uintptr_t edx, uint32_t type, uint32_t count, const void *vertices, uint32_t stride);

	struct counts
	{
		uint64_t passes = 0;  // names passes in the field
		uint64_t names = 0, stars = 0;
		uint64_t glyphs = 0;  // recorded for HiFi XI
		uint64_t game = 0;    // glyphs the game drew (native off, or past the frame's room)
		uint32_t seq = 0;     // (the names ring's)
		uint32_t last_glyphs = 0, most_glyphs = 0;
	};
	counts get_counts();
	std::string status();

	// For the tests: the patched places and what they went to
	struct sites
	{
		uintptr_t pass_slot = 0, pass = 0;               // the vtable slot, 0817F0
		uintptr_t name_call = 0, name_draw = 0;          // 0D08A3, 086210
		uintptr_t stars_call = 0, stars_draw = 0;        // 0D08C9, 086900
		uintptr_t name_page = 0, stars_page = 0, page = 0;   // 0867D2, 086D62, 120590
		uintptr_t name_glyph = 0, stars_glyph = 0, glyph = 0; // 08689D, 086E27, 00CD00
		uintptr_t phase = 0;                             // the global whose word is 0x60 in the field
	};
	sites where();
	// For the tests: what the patched calls go on to (start() sets the game's)
	void set_callees(uintptr_t pass, uintptr_t name_draw, uintptr_t stars_draw, uintptr_t page, uintptr_t glyph);
}
