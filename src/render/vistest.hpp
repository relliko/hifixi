#pragma once

// The game's visibility tests (FFXiMain 06C280: a screen box against the scene's depth, read back from the GPU,
// which waits for the GPU to finish the frame so far), answered from the add-on's copy of the world's depth
// instead, a few frames late and without waiting. The game's thread asks (one of the add-on's entries stands in
// for the test at the call sites worldfx.lua points at it) and gets that test object's newest answer at once; the
// boxes asked for go to the GPU at the world's end, are tested there the way the game tests them (an 8 x 8 grid of
// points over the box, each counted when its depth test at the box's depth passes), and their counts come back
// through the renderer's readback slots. Thread-safe: the game's thread, the one recording the world and the one
// presenting all come in here.

#include <cstdint>

namespace vistest
{
	constexpr uint32_t kMaxBoxes = 256; // tested a frame (more asked for wait for the next)
	constexpr uint32_t kPoints = 64;    // a box's points: the game's 8 x 8

	// Who asked (the call sites: what the answer does there)
	enum kind : uint32_t
	{
		glow = 0,      // 04A960: whether a glow is drawn
		glow_fade = 1, // 04E6FF: an effect element's fade (a lamp's light: its brightness in the game's light table)
		flare = 2,     // 054AE2: the sun's lens flare
		door = 3,      // 0AC894: a door's 'hidden' bit (targeting reads it)
		kinds = 4,
	};

	// A test as the game asks it: its screen box (viewport pixels, any corner order), the depth tested at (NDC z), and
	// the viewport's size. 32 bytes: the GPU reads it as two float4.
	struct box
	{
		float x0, y0, x1, y1;
		float depth;
		float width, height;
		uint32_t kind;
	};

	// The game's thread: a test asked for. Returns the newest answer for this test object (0..256, as the game's test
	// returns), or -1 when there's none yet. now_ms: GetTickCount64.
	int32_t ask(uint32_t object, const box &b, uint64_t now_ms);
	// Whether answers have come in lately (within kLiveMs): the game's own test runs when they haven't
	constexpr uint64_t kLiveMs = 250;
	bool live(uint64_t now_ms);

	// The thread recording the world, at its end: the boxes asked for since the last take (the newest for each test
	// object, in the order first asked; at most kMaxBoxes, the rest dropped: the game asks again next frame).
	// Returns how many.
	uint32_t take(uint32_t objects[kMaxBoxes], box boxes[kMaxBoxes]);
	// Counts read back for boxes taken earlier: points[i] of kPoints passed for objects[i]. An answer is 4 a point
	// (the game's: 128 a point, the sum shifted down 5; each test on its own, no history).
	void answer(const uint32_t *objects, const uint32_t *points, uint32_t n, uint64_t now_ms);

	// Whether the game's test would draw nothing and answer 0 for this box: no part of it on the screen (its corners
	// truncated as the game does, then kept within the viewport's size)
	bool off_screen(const box &b);

	// Forgets everything (the device went, the feature was turned off)
	void reset();

	struct stats
	{
		uint64_t asked = 0;    // tests asked for
		uint64_t unknown = 0;  // ... with no answer yet
		uint64_t taken = 0;    // boxes sent to the GPU
		uint64_t dropped = 0;  // asked for over kMaxBoxes in a take
		uint64_t answered = 0; // answers read back
		uint64_t by_kind[kinds] = {};
		uint32_t objects = 0;  // test objects with an answer kept
		uint64_t last_answer_ms = 0;
	};
	stats get_stats();
}
