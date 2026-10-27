#pragma once

// Each character model the game is about to draw, taken at its draw (research/ffxi-re/notes-characters.md "For a
// native character renderer", parts 1-4): the model draw 02B6E0 has posed the model by 02B7E3, the actor's lights are
// set (0CB4E0 ran just before), and every actor class comes through here. The 5 bytes there (mov ecx, imm32) become a
// jump to a stub of ours that records the model into the characters' frame (src/chars/charframe.hpp) and then lets the
// game draw it, or leaves through the draw's own early return (02B7B0) when the game's character drawing is off: the
// same exit /hifi gamedraw chars uses, which this replaces while it's in.
//
// Recorded per model: the actor, the model, 02B6E0's argument 7 (the colour: every fade), from_flush; the lights
// table and AMBIENT 0CB4E0 set; each skeleton instance's palette (copied: it's only current while the actor is drawn)
// and its meshes, each named by the characters' cache (src/chars/cache): a mesh or texture seen the first time is
// copied out of the game's memory there and then (its texture found as the game finds it: the command's cached object,
// else 03AF10's search of the mesh's folder by name).
//
// The game's thread only.

#include "game.hpp"
#include <string>

namespace hifi::game::charmodel
{
	// The stub in or out; false and why when it can't be (nothing changed then). start() refuses while /hifi gamedraw
	// chars has the site patched: put it back first.
	bool start(std::string &why);
	bool stop(std::string &why);
	bool in();
	// Whether the game draws the models itself (false: each recorded model leaves through the early return, as
	// /hifi gamedraw chars off)
	void set_game_draws(bool on);
	bool game_draws();
	// Whether models are recorded (the stub only passes through when not)
	void set_recording(bool on);
	// Whether HiFi XI draws the models it can (the native draw wanted and its Direct3D 12 side ready): a model whose
	// meshes and textures are all on the GPU is then marked kNative and left out of the game's draw; any other the game
	// draws as before
	void set_native(bool on);
	// The actor (02B6E0's argument 1, Ashita's actor pointer) whose models carry an en-spell effect, and which one
	// (charframe.hpp kEn*; kEnNone: none). One actor: the player's.
	void set_enspell(uintptr_t actor, uint8_t element);

	// charslot's listener: the walk's begin (2: a frame begun), end (3: its opaque models published)
	void on_slot(uint32_t kind, uint32_t seq, uintptr_t actor);

	struct counts
	{
		uint64_t models = 0, see_through = 0, skipped = 0; // recorded, of them from the flush; left to the early return
		uint64_t faults = 0, dropped = 0;  // models whose memory couldn't be read; models that didn't fit a frame
		uint64_t outside = 0;             // drawn outside the walk and the flush (not recorded)
		uint64_t new_meshes = 0, new_textures = 0, textures_missing = 0;
		uint64_t native = 0, not_ready = 0; // models left to HiFi XI's draw; models the game drew as their data wasn't up yet
		uint32_t last_models = 0, last_bones = 0; // the last published frame's
	};
	counts get_counts();
	std::string status();

	// For the tests: the sites (0 before found), and the stub's two exits pointed somewhere else
	struct sites
	{
		uintptr_t site = 0, early_return = 0, stub = 0, lights = 0, ambient = 0, tfactor = 0, renderer = 0, device = 0;
	};
	sites where();
	void set_exits(uintptr_t go_on, uintptr_t leave);
}
