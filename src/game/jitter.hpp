#pragma once

// The world's sub-pixel jitter for FSR 2 (research/ffxi-re/notes-fsr.md, milestone 2). Every projection the world is
// drawn with comes from D3DXMatrixPerspectiveFovRH (FFXiMain 2D83C6, statically linked, __stdcall (out, fovy, aspect,
// near, far)), called by four builders: the camera's (015490, into gGame+0x154: the zone, characters, names, the sky),
// the effects' environment (0467E0), the shadow receivers' (177420) and the effects' depth push (046070). Their four
// calls are pointed at ours, which calls the builder and then moves the picture by this frame's offset: the matrix's
// _31 and _32 (row vectors, w = view depth, so NDC moves by minus them). The character shadow silhouettes' call (033800,
// a target of their own) is left alone.
//
// The offsets are FSR 2's own sequence (Halton 2, 3: ffxFsr2GetJitterOffset), in pixels of the world's target A (the
// background resolution, gGame+0x18 x +0x1A), one a frame: next_frame at each present, before the camera tick builds
// the next frame's projection. Off (no offset at all) unless set_on: jitter without FSR 2 to take it back out would
// shake the picture.

#include "game.hpp"
#include <string>

namespace hifi::game::jitter
{
	// The four calls through ours (they offset nothing until set_on), or out (on unload); false and why when they can't be
	bool start(std::string &why);
	bool stop(std::string &why);
	bool in();
	// Offsets on or off, and the sequence's length (FSR 2: 8 x (screen / background)^2 for its upscale ratio)
	void set_on(bool on);
	void set_phases(uint32_t phases);
	// At each present (the game's thread): the next frame's offset
	void next_frame();

	// The offset the world is drawn with now: in A's pixels (FSR 2's jitterOffset; +x right, +y down) and the NDC move
	// it is (2x / width, -2y / height); 0 while off. index counts frames from start.
	struct offset
	{
		float px[2] = {};
		float ndc[2] = {};
		uint32_t index = 0;
	};
	offset current();
	std::string status();

	// For the tests: the four calls (0 where not found) and the builder they call
	struct sites
	{
		uintptr_t calls[4] = {};
		uintptr_t builder = 0;
	};
	sites where();
	// A projection built as the game builds it, through ours (tests): out = D3DXMatrixPerspectiveFovRH(...) + offset
	void build(float out[16], float fovy, float aspect, float zn, float zf);
	// The sequence's offset for a frame index (pixels, each -0.5 .. 0.5)
	void sequence(uint32_t index, uint32_t phases, float out[2]);
}
