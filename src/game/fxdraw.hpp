#pragma once

// The effect elements of the priority-12 flush handed to HiFi XI's native effect renderer instead of drawn by the game
// (research/ffxi-re/notes-effects-draw.md; PLAN-own-frame.md, part fx).
//
// Three sites, all on the game's thread:
//   - the flush (055620: call 055520, the sorted list's walk, far to near) goes through a stub of ours that tells us its
//     begin and end;
//   - the walk's call of each queued item's callback (055539: push eax; call [esi+8]; add esp, 4) goes through a stub
//     that tells us what it's about to call: anything that isn't an element drawn through 046070 (a see-through
//     character, a weapon trail, a lens flare) first ends the run of elements recorded so far (an fx_run marker);
//   - each element's setup (046070's call of 0467E0 at 04612E) goes to us: an element HiFi XI can draw as the game
//     would is recorded into the flush's frame (src/fx/fxframe.hpp) and its setup and class draw are skipped (0467E0
//     answering "skip"); anything else ends the run first and goes on to 0467E0 and its class draw as before.
// The element draws before the setup (its colour into VA 0490BC0C, the hidden check that src/game/gamerain answers)
// run as before, so what World FX's patches leave out stays out.
//
// The gate: elements are taken only while set_native says so (the plugin's module: the Direct3D 12 side up and its
// markers seen), the bound depth has a stencil for the markers, and per element only what's reproduced faithfully:
// the classes handled, every texture on the GPU, a lit element's lights worked out as 0467E0 does, a blend letter
// 0467E0 knows. Everything else is the game's, element by element: never a hole.

#include "game.hpp"
#include <string>

namespace hifi::zone
{
	class gpu_zone;
}

namespace hifi::game::fxdraw
{
	// The stubs and the call in, or out; false and why (nothing changed then)
	bool start(std::string &why);
	bool stop(std::string &why);
	bool in();
	// Whether elements are taken (the native side ready and drawing)
	void set_native(bool on);
	// Whether markers are sent at all (a Direct3D 12 side to see them)
	void set_wanted(bool wanted);
	// While native drawing waits for proof that markers come through: an empty run sent at each flush's end
	void set_probe(bool on);
	// The time new textures are stamped with (the plugin's clock; the textures' forgetting)
	void set_clock(double now);
	// Each present: the native zone renderer's data of the renderer's map slots (0 the zone, 1 a second map; nullptr
	// while not on the GPU): an MMB effect mesh is the zone's own model, drawn from it
	void set_zones(zone::gpu_zone *zone, zone::gpu_zone *second);

	struct counts
	{
		uint64_t flushes = 0;
		uint64_t taken[16] = {};  // (by fx::element_class)
		uint64_t empty = 0;       // taken, nothing to draw (as the game's own early returns)
		uint64_t game = 0;        // left to the game: not taking (native off, no stencil)
		uint64_t unsupported = 0; // ... a class not handled yet
		uint64_t lit = 0;         // ... lit (flags bit 16), its lights not to be had
		uint64_t lit_taken = 0;   // (of those taken, lit)
		uint64_t letter = 0;      // ... a blend letter 0467E0 has no case for (the blend inherited)
		uint64_t texture = 0;     // ... a texture not on the GPU yet (or not found)
		uint64_t full = 0;        // ... no room in the frame
		uint64_t faulted = 0;     // ... its memory couldn't be read
		uint64_t overwritten = 0; // ... an untextured D3M whose first vertex the load wrote over (below)
		uint64_t mesh = 0;        // ... an MMB mesh not in the zone's data on the GPU (or not as the game has it)
		uint64_t water = 0;       // taken, nothing drawn: an MMB mesh World FX's gamewater hides (its list)
		uint64_t spot_triangles = 0; // the ground's triangles the light spots taken were laid on (182700's)
		uint64_t runs = 0, markers = 0, not_sent = 0;
		uint32_t last_elements = 0, last_runs = 0;
	};
	counts get_counts();
	std::string status();

	// For the tests: the sites and our stubs; a sink for the markers (instead of the device); the device
	struct sites
	{
		uintptr_t setup_call = 0, setup = 0;        // 04612E, 0467E0
		uintptr_t flush_call = 0, walk = 0;         // 05562B, 055520
		uintptr_t entry_site = 0, element_cb = 0;   // 055539, 0555A0
		uintptr_t element_draw = 0, d3m_draw = 0;   // 046070, 040BA0
		uintptr_t draw_matrix = 0, fog_env = 0;     // 045D40, 18BA20
		uintptr_t d3a_draw = 0, trail_cb = 0, ring_draw = 0; // 03EB30, 01C930, 0565A0
		uintptr_t mmb_draw = 0, morph_draw = 0;     // 040C20, 03FBB0
		uintptr_t vuanm_draw = 0, vu_walk = 0, vu_cursor = 0; // 03F130, 063C80, VA 0490B9D4
		uintptr_t dist_draw = 0, vudist_draw = 0, dist_app = 0; // 0426F0, 043110, VA 048E666C
		uintptr_t distmodel_draw = 0, distmorph_draw = 0, distring_draw = 0; // 043320, 043590, 043720
		uintptr_t spec_draw = 0, spec_cubemap = 0; // 0412B0, VA 0490BB90
		uintptr_t spot_draw = 0, spot_target = 0;  // 040E70, VA 0490BB88
		uintptr_t number_draw = 0, glyph_bind_call = 0, glyph_bind = 0, glyph_draw_call = 0, glyph_draw = 0; // 041AF0; 041EBB -> 1207B0, 041F06 -> 00CD00
		uintptr_t flare_call = 0, flare_pieces = 0, flare_textures = 0; // 054B81, 03EBF0, VA 0490B970
		uintptr_t frame_of = 0, light1 = 0, lights2 = 0, ambient = 0, material = 0; // 045C50, 18B9A0, 18B960, 18B9E0, VA 0490BC28
		uintptr_t colour = 0, gfx = 0, renderer = 0, camera = 0, env = 0, math = 0; // (the globals' addresses)
		uintptr_t code = 0;                         // our stubs
	};
	sites where();
	void set_sink(void (*sink)(uint32_t kind, uint32_t seq));
	void set_device(uintptr_t device);
	// The setup the call goes on to for what's left to the game (start() sets 0467E0)
	void set_setup(uintptr_t setup);
	// ... and the battle numbers' glyph bind (1207B0) and draw (00CD00) theirs go on to (start() sets the game's)
	void set_glyph_calls(uintptr_t bind, uintptr_t draw);

	// The entries (cdecl / fastcall as the stubs and the patched call reach them)
	void __cdecl on_flush(uint32_t end);
	void __cdecl on_entry(uintptr_t callback, uintptr_t arg);
	uint32_t __fastcall on_setup(uintptr_t element, uintptr_t edx, uint32_t arg);
	// A lens flare's pieces (0548D0's call of 03EBF0, thiscall on the flare's D3A resource, ret 0x18)
	void __fastcall on_flare(uintptr_t resource, uintptr_t edx, int32_t visibility, float x, float y, float far_x, float far_y, uint32_t zero);
	// A battle number's glyph: 041C20's call of 1207B0 (thiscall on the font object, ret 0xC) and of 00CD00 (thiscall on the
	// device wrapper: DrawPrimitiveUP, ret 0x10)
	uint32_t __fastcall on_glyph(uintptr_t font, uintptr_t edx, int32_t c, uint32_t *colour, uint32_t *other);
	void __fastcall on_glyph_draw(uintptr_t wrapper, uintptr_t edx, uint32_t type, uint32_t count, const void *data, uint32_t stride);
}
