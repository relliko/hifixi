#pragma once

// The game's screen 2D recorded at its Direct3D 8 device and handed to HiFi XI's own 2D renderer (src/ui2d;
// docs/own-frame/2d.md; research/ffxi-re/notes-ui-draw.md "For a native renderer", notes-screen-passes.md).
//
// The window: the loop tail's back-buffer part, from just after the resolve (FFXiMain 012779: the call of the empty
// 00BB20 after the resolve's Z states) to the frame's EndScene (012AF5: call 009740). Everything the game draws there is
// 2D on the back buffer: the radar, the menus and windows, text, icons, the mouse cursor, the viewer marker, the fade
// after the menus (00A820), the movie (21D4C0), the screen-text queue (038520); and, with a UI-size target, the world's
// copy into it and its stretch over the back buffer (00ABE0).
//
// A second window, the screen passes on A: from the frozen-frame crossfade (012465: call 012F40) to A's pop (0126A1:
// call 00A1D0): the wash and the screenshot's copyright text recorded, the crossfade's and afterimage's copies of A the
// game's; its cuts and end marked with kind 8 on A's depth, no begin marker. No window opens while the opening movie's
// player exists ([0x456D1C]): the game draws it all.
//
// While the window is open the client device (Ashita's object, [[0x45666C]+0xC]) wears a copy of its own vtable whose
// state and draw slots are ours: every state call is followed (then made), every DrawPrimitiveUP of pre-transformed
// vertices with a state the 2D renderer draws as dgVoodoo does is recorded into the frame (src/ui2d/frame2d.hpp), and,
// with native drawing on, left out of the game's stream. Anything else (another FVF, a pixel shader, Z, fog, a render
// target or dynamic texture, a DrawPrimitive, a Clear, a SetRenderTarget) cuts: what's recorded so far is published as a
// segment and a marker sent for it (kind 10, slotmark.hpp), then the game's call goes through. So the order on screen is
// the game's, whatever is left to it.
//
// Markers: one-pixel stencil clears of the bound depth (the back buffer's own D24S8), as zoneslot's: kind 9 at the
// window's start, 10 at each cut and just before the fade after the menus (0129EA: the interface's end), 11 at its
// end; each 10 or 11 names the next segment of the window. A window opens only while the bound depth has a stencil.
//
// Render targets: A (the world's, dgVoodoo's own resource) is read by the draws that copy it (the crossfade's, the
// afterimage's, the UI-size target's); the game's other render-target textures (the crossfade's frozen frame, the
// afterimage's history, the UI-size target) get targets of HiFi XI's own, drawn into and read by the recorded draws,
// so a switch to one of them doesn't cut: the whole sequence is HiFi XI's. The UI-size target's is the screen's size:
// the interface is drawn sharp there and stretched 1:1.
//
// Textures: taken from the game's own (LockRect, read only, every level) the first time a draw uses one, held
// (AddRef) while in use so it can't be freed and another made at its address, checked again for new pixels (small
// ones each frame they're used, large ones every few seconds), and let go after a few seconds unused. Render targets
// and dynamic textures are never taken (their draws go to the game).
//
// The gate (set_mode): off, the window isn't opened; shadow, recorded and marked but the game draws everything (the
// Direct3D 12 side only checks it could have); native, the recorded draws are left to HiFi XI.
//
// The game's thread only.

#include "game.hpp"
#include <string>
#include <vector>

namespace hifi::game::uidraw
{
	// The three calls through our stubs, or back; false and why (nothing changed then)
	bool start(std::string &why);
	bool stop(std::string &why);
	bool in();

	enum mode : uint32_t { off = 0, shadow = 1, native = 2 };
	// From the next window on
	void set_mode(mode m);
	mode current();

	// The window, as the stubs call it (wrapper: the game's device wrapper, [0x45666C]; its +0xC the client device):
	// on the back buffer, or (screen) the screen passes' on A. The tests call them with a device of their own.
	void window_begin(uintptr_t device, bool screen = false);
	// kind 10 (a cut, or the interface's end), 11 (the back buffer's window's end: closed after) or 8 (a cut on A)
	void window_flush(uint32_t kind);
	void window_end();

	struct counts
	{
		uint64_t windows = 0;          // opened
		uint64_t no_window = 0;        // ... not: the device's state couldn't be read, or no stencil to mark on
		uint64_t movie = 0;            // ... not: the opening movie plays (the game draws it all)
		uint64_t recorded = 0;         // draws recorded
		uint64_t native = 0;           // ... and left out of the game's stream
		uint64_t passed = 0;           // draws in a window left to the game
		uint64_t cuts = 0;             // segments cut for them
		uint64_t segments = 0;         // published
		uint64_t markers = 0, not_marked = 0;
		uint64_t textures = 0;         // taken from the game
		uint64_t texture_changes = 0;  // ... taken again: their pixels changed
		uint64_t targets = 0;          // the game's render-target textures given targets of ours
		uint64_t upload_failures = 0;
		uint32_t frame_draws = 0, frame_segments = 0; // the last window's
		uint32_t held = 0;             // textures held now
		uint32_t seq = 0;
		uint64_t why[16] = {};         // draws passed, by reason (reason_name)
	};
	counts get_counts();
	const char *reason_name(uint32_t r);
	std::vector<std::string> status();

	// Every texture held let go (the device going, the renderer made again): taken again at their next use
	void drop_textures();

	// For the tests: the five calls, their targets, the movie player's global, our stubs (begin +0, end +0x20, frame
	// +0x40, screen +0x60, pop +0x80)
	struct sites
	{
		uintptr_t begin_call = 0, begin_target = 0, end_call = 0, end_target = 0, frame_call = 0, frame_target = 0;
		uintptr_t screen_call = 0, screen_target = 0, pop_call = 0, pop_target = 0, movie = 0, code = 0;
	};
	sites where();
	// Which of the game's textures are A and the UI-size target (tests: the game's from the wrapper, +0x194 and +0x1E0)
	void set_render_targets(uintptr_t world, uintptr_t ui_target);
	// Markers handed to sink instead of the device's Clear (tests)
	void set_sink(void (*sink)(uint32_t kind, uint32_t seq));
	// The device the stubs use instead of the wrapper's (tests; 0: the wrapper's)
	void set_device(uintptr_t device);
}
