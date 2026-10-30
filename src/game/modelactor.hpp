#pragma once

// The model actors handed to HiFi XI's zone renderer instead of drawn by the game (research/ffxi-re/notes-world-passes.md
// 1: XiModelActor and XiFurnitureActor, an actor whose look is one MMB: ships, Mog House furniture, gardening plants,
// "model" NPCs).
//
// Their draw (actor vt+0x288, FFXiMain 0C4C20, in the priority-6 actor walk with the characters) builds the world matrix
// at actor +0x608 and calls 1844C0 (thiscall on the zone renderer: the block at actor +0x5C8, the alpha-test flag
// +0x770) at 0C4CC8. That call goes to on_draw instead: for an actor HiFi XI can draw (native wanted, inside the walk,
// its DAT's data on the GPU, its mesh found there with every texture) the actor is recorded into the walk's actor frame
// (zone/frame.hpp) and the game's draw is skipped; anything else goes on to 1844C0 as before. The frame is published at
// the walk's end (charslot's listener, before its marker) and drawn by the zone renderer at that marker.
//
// What's recorded: the matrix (block +0x40, row vectors), the mesh's MMB name (block +0x18 = resource +0x30, the name at
// +8), its DAT (the resource at block - 4 = actor +0x5C4, up its parents (+0x28, as 072240) to the DAT's root, whose
// +0x34 is the file id), the alpha test (+0x770: '_' models and ships: GREATER 0x60). The light: the default
// environment's (the zone renderer's own sun, ambient and fog, as 17B510 left them), and in a Mog House (the object at
// [VA 04AF31D4]) the room lamp l_01 of the light table (178760), as 1844C0 sets it.
//
// The game's thread only.

#include "game.hpp"
#include <string>

namespace hifi::zone
{
	class gpu_zone;
}

namespace hifi::game::modelactor
{
	// The call through on_draw, or back to 1844C0; false and why (nothing changed then)
	bool start(std::string &why);
	bool stop(std::string &why);
	bool in();
	// Whether HiFi XI draws the model actors it can (its zone renderer up and the characters' end markers coming through)
	void set_native(bool on);
	// Where a DAT's data is found (zone::actordata::find; the tests' own), nullptr: none
	void set_data(zone::gpu_zone *(*find)(uint32_t file));

	// charslot's listener: the walk's begin (2: a frame begun), end (3: its model actors published)
	void on_slot(uint32_t kind, uint32_t seq, uintptr_t actor);
	// The call at 0C4CC8, as 1844C0 takes it (thiscall: ecx the renderer; the block and the alpha flag on the stack, taken off)
	void __fastcall on_draw(uintptr_t renderer, uintptr_t edx, uintptr_t block, uint32_t alpha);

	struct counts
	{
		uint64_t drawn = 0;     // recorded: HiFi XI draws them
		uint64_t game = 0;      // left to the game: native off, or outside the walk
		uint64_t not_ready = 0; // ... their DAT's data not on the GPU (yet)
		uint64_t unknown = 0;   // ... their mesh not in it, or not as the game has it (another DAT's textures, its parts)
		uint64_t dropped = 0;   // ... no room in the frame
		uint32_t last = 0;      // the last published frame's
	};
	counts get_counts();
	std::string status();

	// For the tests: the call's site, the game's draw it went to, the Mog House object's and the light table's addresses
	struct sites
	{
		uintptr_t call = 0, original = 0, mog_house = 0, lights = 0;
	};
	sites where();
	// For the tests: the game's draw the call goes on to for what's left to it (start() sets 1844C0)
	void set_draw(uintptr_t draw);
}
