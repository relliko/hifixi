#pragma once

// The zone's objects handed to HiFi XI's native zone renderer instead of drawn by the game (src/zone: frame.hpp, draw).
//
// The game's per-object zone draw (FFXiMain 17C9C0) tests each object's distance and frustum and picks its level of
// detail; right after (17CBA8), where it counts the object as drawn, a stub of ours asks on_object: for a pass HiFi XI
// draws (the gate, decided at the zone's begin marker) and a model it has, the object is recorded into the pass's frame
// (its matrix, model, alpha test, mirroring, lamps) and the game leaves it (its own exit, 17DDFF), counted as drawn
// all the same. The doors and elevators (183DA0, a second stub at 183E47) the same way, and the objects of the
// renderer's second map (a building's interior) from that map's data. Each object is lit by its own environment (an
// area's worked out by the game's own calls once a pass), kind 2's plants sway by the game's own clock. Anything else
// the game draws as before: models HiFi XI doesn't have (a second map until its data is loaded, the boats' scenery),
// objects whose game calls weren't found, and every object of a pass the gate kept.
//
// At the zone's end marker (zoneslot's listener) the pass's camera, sun, ambient and fog are copied from the zone
// renderer and the frame is published for the Direct3D 12 side, which draws it at that marker.
//
// The gate opens a pass only when: native drawing is wanted, the zone's data is on the GPU for this zone, and the
// Direct3D 12 side has drawn the passes it was given lately (set_d3d12_ready). Otherwise the game draws all of it.

#include "game.hpp"
#include <string>

namespace hifi::zone
{
	class gpu_zone;
}

namespace hifi::game::zonedraw
{
	// The stub in (17CBA8 through it) or out; false and why
	bool start(std::string &why);
	bool stop(std::string &why);
	bool in();

	// Each present (the game's thread): whether native drawing is wanted, the zone's and the second map's data on the
	// GPU (nullptr: none ready) and whether the Direct3D 12 side can draw
	void set_state(bool wanted, zone::gpu_zone *zone, zone::gpu_zone *second, bool d3d12_ready);
	// The renderer's second map (its slot 1: a building's interior), 0 when there's none (renderer 0: the last pass's)
	uint32_t second_map(uintptr_t renderer = 0);

	// zoneslot's listener: the zone pass's begin (kind 0, the renderer) and end (kind 1)
	void on_slot(uint32_t kind, uint32_t seq, uintptr_t renderer);

	struct counts
	{
		uint64_t passes = 0, native_passes = 0; // zone passes seen, of them drawn by HiFi XI
		uint64_t recorded = 0, left = 0;        // objects recorded (the game left them), left to the game (unknown model)
		uint64_t doors = 0, recorded_second = 0; // of them doors and elevators' parts, the second map's
		uint64_t envs = 0;                      // area environments worked out
		uint32_t last_recorded = 0, last_left = 0, last_envs = 0; // the last native pass's
		uint32_t last_bands[3] = {}; // ... its objects by their distance from the camera: under 100 yalms, 100-300, past 300
		std::string gate;                       // why the last pass wasn't HiFi XI's ("" when it was)
	};
	counts get_counts();
	std::string status();

	// For the tests: the stubs, their sites, the game's exits (the doors' 0 when not found); on_object and on_door as the
	// stubs call them
	struct sites
	{
		uintptr_t site = 0, stub = 0, exit = 0;
		uintptr_t door_site = 0, door_stub = 0, door_exit = 0;
	};
	sites where();
	int __cdecl on_object(uintptr_t renderer, uintptr_t object, uintptr_t model, uint32_t ship, uintptr_t light_table);
	int __cdecl on_door(uintptr_t renderer, uintptr_t part, uintptr_t model, uintptr_t light_table);

	// The game's functions called for area environments and plants (each thiscall; 0 not found), found by start();
	// the tests set stand-ins (kept until set again)
	struct game_calls
	{
		uintptr_t fog = 0;        // 189120(colour*, end*, start*, kind) on an environment
		uintptr_t light = 0;      // 188CA0(D3DLIGHT8*, kind) on an environment
		uintptr_t c7 = 0;         // 188FA0(float[4]*) on an environment
		uintptr_t ambient = 0;    // 188E00(D3DCOLOR*, kind) on an environment
		uintptr_t bright = 0;     // 00BE50(D3DLIGHT8*) on the device wrapper
		uintptr_t wrapper_at = 0; // the device wrapper's global (0x45666C)
		uintptr_t sway = 0;       // 17A3F0(object) on the renderer
	};
	game_calls calls();
	void set_calls(const game_calls &c);
}
