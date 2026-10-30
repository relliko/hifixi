#pragma once

// The world part's slot markers (slotmark.hpp; PLAN-own-frame.md): the sky's end (kind 6), the names' end (7) and the
// Mog House layout grid's end (12, a spare kind taken for it), each a one-pixel stencil clear of the bound depth through
// the game's own device, as zoneslot's and charslot's: only while the Direct3D 12 side can see them (set_wanted) and the
// bound depth has a stencil. Each kind keeps its own seq (the marker carries its low 4 bits; the ring its pass records
// into is keyed by it).
//
// The game's thread only.

#include "game.hpp"
#include <string>

namespace hifi::game::worldmark
{
	constexpr uint32_t kGrid = 12; // (slotmark::spare_12: the layout grid's end)

	// Whether markers are sent
	void set_wanted(bool wanted);
	bool wanted();
	// A marker of a kind with its seq now: true when it went out (sent, or to the tests' sink)
	bool send(uint32_t kind, uint32_t seq);

	struct counts
	{
		uint64_t sent[16] = {};
		uint64_t not_sent = 0; // wanted, but no device or no stencil on the bound depth
	};
	counts get_counts();

	// For the tests: markers to a function instead of the device, or to a stand-in device
	void set_sink(void (*sink)(uint32_t kind, uint32_t seq));
	void set_device(uintptr_t device);
}
