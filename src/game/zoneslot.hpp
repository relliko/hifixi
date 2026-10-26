#pragma once

// The zone pass's slot marked in the game's Direct3D 8 stream (slotmark.hpp; research/ffxi-re/notes-replace-slots.md):
// the zone task's one call of the zone pass (FFXiMain 178CCE: call 17B510, thiscall on the zone renderer) goes
// through a stub of ours that marks the zone's begin, calls the pass, and marks its end. HiFi XI's Direct3D 12 side
// keys on the markers: the world is the target the zone is drawn into (World FX's world found with the game's zone
// off too), and the native zone renderer to come draws there.
//
// The marker is a one-pixel stencil clear of the world's depth through the game's own device (its vtable as the game
// calls it: hifps's batching proxy flushes before a Clear, so the marker keeps its place among the draws). Only while
// the Direct3D 12 side can see it (set_wanted) and the bound depth surface has a stencil; otherwise the stub just
// calls the pass. The pass itself is always the game's (gamedraw switches its drawing off).

#include "game.hpp"
#include <string>
#include <vector>

namespace hifi::game::zoneslot
{
	// The stub in (the zone task's call through it) or out; false and why when it can't be
	bool start(std::string &why);
	// Out again (on unload); false and why when something else changed the call since (then left as it is)
	bool stop(std::string &why);
	bool in();
	// Whether markers are sent (the Direct3D 12 side is in to see them); the stub calls the pass either way
	void set_wanted(bool wanted);

	// Markers sent of each kind since start, and the frame number of the zone's last begin (its low 4 bits are the seq
	// the marker carries)
	struct counts
	{
		uint64_t sent[4] = {};
		uint64_t not_sent = 0;      // wanted, but no device or no stencil on the bound depth
		uint32_t seq = 0;
	};
	counts get_counts();
	std::string status();

	// For the tests: where the call is and our stub (0 when not in), the pass it calls; what a marker does instead of
	// the device's Clear (kind, seq), and a device of their own (an object whose first word is its vtable)
	struct sites
	{
		uintptr_t call = 0, stub = 0, pass = 0;
	};
	sites where();
	void set_sink(void (*sink)(uint32_t kind, uint32_t seq));
	void set_device(uintptr_t device);

	// Told of each pass's begin (kind 0) and end (1) on the game's thread, before its marker, whether markers are sent
	// or not: the native zone draw's (zonedraw::on_slot). The renderer is the zone renderer (the begin's ecx).
	void set_listener(void (*listener)(uint32_t kind, uint32_t seq, uintptr_t renderer));
}
