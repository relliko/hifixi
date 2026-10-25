#pragma once

// /hifi streamlog: one frame of the world as dgVoodoo records it in Direct3D 12, call by call as HiFi XI's hooks see it,
// and the game's own Direct3D 8 draws of the same frame with where in FFXiMain each comes from (Ashita hands the plugin
// every draw), written to a text file beside hifixi.log. It's for comparing the stream with the game's own zone or
// character drawing on and off (/hifi gamedraw), and for finding the slots a native renderer draws in
// (research/ffxi-re/notes-replace-slots.md).
//
// Nothing is traced until a log is asked for: the hooks hand their calls on (host::set_tracer) only from the present
// after the request until the file is written. A world frame starts at the depth clear of a target at least 640 wide
// over all of it (dgVoodoo clears the world's colour, then its depth, at the frame's start); the colour clear just
// before it is kept from the calls seen while waiting.

// (no d3d12.h here: the plugin includes Ashita's Direct3D 8 headers beside this)
#include <cstdint>
#include <string>

namespace hifi::backend::streamlog
{
	// Asked for (the command, the game thread): the next `frames` world frames (1-8) from the next present. label (letters,
	// digits, '-' and '_' kept) goes in the file's name; context lines (what /hifi gamedraw has off...) in its header.
	// False when one is under way already.
	bool request(uint32_t frames, const std::string &label, const std::string &context);
	bool busy();
	// At each present, on the game thread: starts what was asked for, ends the game's frames of Direct3D 8 draws, and
	// once the Direct3D 12 frames are in (or none came in time), writes the file. A line to show when it started or
	// wrote one, else empty.
	std::string present();

	// The game's Direct3D 8 draws come through a tap on the client device's four draw methods (the game side's:
	// hifps::exp::draw_tap, set here by the plugin), on from the present that starts a log until its game frames are
	// in; each tap calls d3d8_draw and then the method it stood in for. (Ashita's own draw events can't serve: HiFi XI's
	// 'ashita' speed-up skips them.) on fills originals[4] for taps[4], false and why when it can't.
	void set_d3d8_tap(bool (*on)(const void *const taps[4], void *originals[4], std::string &why), void (*off)());
	// The game's Direct3D 8 draws (from the tap, on the game thread): while d3d8_on(), each is kept with the
	// FFXiMain return addresses on the stack. call: "DP", "DIP", "DPUP", "DIPUP"; extra: the vertices (DIP, DIPUP) or the
	// stride (DPUP)
	bool d3d8_on();
	void d3d8_draw(const char *call, uint32_t type, uint32_t primitives, uint32_t extra);

	// The backend's: the resource behind a render target or depth view (null when not known), and the folder the file
	// goes in (with a slash at the end)
	void set_views(void *(*resource_of_view)(uint64_t view));
	void set_folder(const std::wstring &folder);

	// For the tests: how long to wait for a world frame before writing what there is (ms; 5000 by default)
	void set_wait_ms(uint32_t ms);
	// For the tests: the return addresses in a module image on a stack, innermost first, as image offsets: the stack
	// words that point into [code_from, code_to) of the image (offsets) just after a call instruction. image is the
	// module's memory at address base.
	uint32_t callers(const uint8_t *image, uintptr_t base, uint32_t code_from, uint32_t code_to, const uint32_t *stack, uint32_t words,
		uint32_t *out, uint32_t most);
}
