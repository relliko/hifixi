#pragma once

// The character passes' slots marked in the game's Direct3D 8 stream (slotmark.hpp; research/ffxi-re/notes-characters.md
// "For a native character renderer", part 1; notes-replace-slots.md "Characters: 0817E0 call 082D70"), for HiFi XI's
// native character renderer:
//   - the opaque actors: CXiActorDraw's one call of the actor walk (FFXiMain 0817E0: call 082D70, priority 6) goes
//     through a stub of ours that marks the walk's begin (kind 2) and end (kind 3). The native renderer draws every
//     opaque model at the end;
//   - each see-through actor: the sorted list's callback for actors (0CBD90, its address the immediate the actor's
//     Update stores at 0C7E7C) is pointed at a wrapper of ours that marks it (kind 4) and says, while it runs, that the
//     model draw it leads to came from the priority-12 flush (from_flush). The n-th see-through marker of a frame names
//     the n-th callback of that frame.
//
// The markers are one-pixel stencil clears of the bound depth through the game's own device, as zoneslot's: only while
// the Direct3D 12 side can see them (set_wanted) and the bound depth has a stencil. The game's walk and callbacks always
// run as before (/hifi gamedraw chars switches the drawing off).

#include "game.hpp"
#include <string>

namespace hifi::game::charslot
{
	// The stub and the wrapper in, or out; false and why when they can't be (then nothing is changed)
	bool start(std::string &why);
	// Out again (on unload); false and why when something else changed the game's code since (then left as it is)
	bool stop(std::string &why);
	bool in();
	// Whether markers are sent; the walk and the callbacks run either way
	void set_wanted(bool wanted);
	// While the see-through callback runs (the game's thread): its model draw came from the flush
	bool from_flush();
	// The see-through markers sent by the model draw's stub (src/game/charmodel) instead of the wrapper: one per model
	// that reaches the draw from the flush, after it's recorded (so the data a marker names is there before it); the
	// wrapper still says from_flush
	void set_model_marks(bool on);
	void mark_see_through(uintptr_t actor);

	struct counts
	{
		uint64_t sent[5] = {};   // (by kind: 2 begin, 3 end, 4 see-through)
		uint64_t not_sent = 0;   // wanted, but no device or no stencil on the bound depth
		uint64_t see_through = 0; // the see-through callbacks run since start
		uint32_t seq = 0;        // the opaque walks since start (the markers carry its low 4 bits)
		uint32_t frame_see_through = 0, most_see_through = 0; // in the last walk's frame, the most in one frame
	};
	counts get_counts();
	std::string status();

	// For the tests: the call and its stub, the walk; the immediate and the wrapper, the callback
	struct sites
	{
		uintptr_t call = 0, stub = 0, walk = 0;
		uintptr_t immediate = 0, wrapper = 0, callback = 0;
	};
	sites where();
	void set_sink(void (*sink)(uint32_t kind, uint32_t seq));
	void set_device(uintptr_t device);

	// Told of each begin (kind 2), end (3) and see-through callback (4, with its actor) on the game's thread, before
	// its marker: the native character renderer's
	void set_listener(void (*listener)(uint32_t kind, uint32_t seq, uintptr_t actor));
}
