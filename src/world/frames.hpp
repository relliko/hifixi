#pragma once

// The world part's passes as the game would have drawn them (PLAN-own-frame.md: the sky dome, the names, the Mog
// House layout grid), handed from the game's thread (src/game/skydraw, namedraw, griddraw: recorded in the pass itself)
// to dgVoodoo's recording thread (src/world/render: drawn at the pass's end marker, src/game/worldmark).
//
// One ring per pass, kSlots slots keyed by the pass's own seq (its marker carries the low 4 bits), as zone/frame.hpp's:
// the game's thread fills slot seq % kSlots and publishes it (its seq stored last, release); dgVoodoo's thread, a frame
// or so behind, reads the slot its marker names once it's published with that seq (acquire). Seqs start at 1.

#include <atomic>
#include <cstdint>

namespace hifi::world
{
	constexpr uint32_t kSlots = 16; // (the markers' seq has 4 bits)

	// The sky dome (research/ffxi-re/client-notes.md "Sky"; 03DB60 -> 03D640 -> 043F40): 03D640's arguments as 03DB60
	// passes them, and the transforms the game's device has when it draws (the wrapper's cache, set by 03DB60: VIEW the
	// effect system's copy with its translation zeroed, so the dome turns with the camera and never moves)
	constexpr uint32_t kDomeStops = 8;
	struct sky_frame
	{
		std::atomic<uint32_t> seq { 0 };
		bool native = false;       // HiFi XI draws it (the game's 03D640 wasn't called)
		float world_view_proj[16]; // WORLD x VIEW x PROJECTION (row vectors), as the device would transform the dome
		float radius = 0.0f;       // 0.8 x (far - near): about 52,400 yalms
		uint32_t segments = 0;     // sky +0x12: around (the dome is drawn only past 2)
		uint32_t colours[kDomeStops] = {}; // sky +0x20..: the stops' colours as D3DCOLORs (0xAARRGGBB, 03E280)
		float heights[kDomeStops] = {};    // sky +0x40..: their heights (0 the horizon .. 1 overhead)
		uint32_t skip = 0;         // its first strips the game leaves out (the first 21 of a session: [VA 04AECB08])
	};

	// The names (research/ffxi-re/notes-ui-draw.md "The names"; 0817F0 -> 0D0880 -> 086210, the star line 0D08B0 ->
	// 086900): each name's call as the game makes it, and each glyph quad its draw hands the device (086210's own layout,
	// scale and projection, nameplate.dll's glyph scale where it's in: XYZRHW in A's pixels, z the name point's NDC z,
	// rhw 1; the colour after 086210's own conversions; the UVs of its page)
	constexpr uint32_t kMaxNames = 512, kMaxGlyphs = 4096;
	struct name_vertex
	{
		float x, y, z, rhw;
		uint32_t colour; // D3DCOLOR 0xAARRGGBB (0x80 = 1.0 under MODULATE2X / MODULATE4X)
		float u, v;
	};
	struct name_glyph
	{
		name_vertex v[4];   // as the strip draws them (TL, TR, BL, BR)
		uint32_t page = 0;  // 0 'font font' (letters), 1 'menu ustatshd' (icons): 120590's choice by the character
		uint32_t alpha_ref = 0; // ALPHAREF: 0 letters, 0x60 icons (086210 0867E6: the colour 120590 gave back is the linkshell colour)
		uint32_t name = 0;  // (index into the frame's names)
	};
	struct name_entry
	{
		float pos[4] = {};     // the name point (world, the game's y down)
		float scale = 1.0f;
		uint32_t colour = 0, ls_colour = 0; // (the arguments: the palette entry, the linkshell's 0x00BBGGRR)
		bool stars = false;    // the star line (086900)
		char text[40] = {};    // (the string as passed, up to 0x24 bytes)
		uint32_t first = 0, count = 0; // its glyphs
	};
	struct names_frame
	{
		std::atomic<uint32_t> seq { 0 };
		bool native = false;     // HiFi XI draws them (the game's glyph draws weren't sent)
		uint32_t width = 0, height = 0; // A's size the game laid them out in (gGame +0x18, +0x1A: the background resolution)
		float jitter[2] = {};    // the jitter A was drawn with this frame (game/jitter: A's pixels, + right and down)
		uint32_t name_count = 0, glyph_count = 0;
		uint32_t dropped = 0;    // glyphs past kMaxGlyphs (the game draws none of them then: the frame's native is off)
		name_entry names[kMaxNames];
		name_glyph glyphs[kMaxGlyphs];
	};

	// The Mog House layout grid (research/ffxi-re/notes-world-passes.md 6; 1D5DD0 -> 1CF110 -> 1CF410/1CF510 -> 177B40):
	// each outline 177B40 draws (a 5-point line strip p0 p1 p2 p3 p0, WORLD identity, colour as is), and the transforms
	constexpr uint32_t kMaxGridLines = 2048;
	struct grid_line
	{
		float p[4][3];      // the four corners (world)
		uint32_t colour;    // 0xAARRGGBB (177B40's: A its 5th argument, R G B its 2nd to 4th)
		uint32_t keep_z;    // its 6th argument: 0 ZENABLE off around it (the grid's cells), else the depth state left
	};
	struct grid_frame
	{
		std::atomic<uint32_t> seq { 0 };
		bool native = false;
		float view_proj[16]; // VIEW x PROJECTION from the device's cache (WORLD is identity: 1779C0)
		uint32_t count = 0, dropped = 0;
		grid_line lines[kMaxGridLines];
	};

	// The rings (static): the slot for a seq to fill (the game's thread; unpublished until publish), or to read (dgVoodoo's:
	// nullptr unless that seq is published there)
	sky_frame &sky_slot(uint32_t seq);
	const sky_frame *sky_published(uint32_t seq_low4);
	void sky_publish(sky_frame &f, uint32_t seq);
	names_frame &names_slot(uint32_t seq);
	const names_frame *names_published(uint32_t seq_low4);
	void names_publish(names_frame &f, uint32_t seq);
	grid_frame &grid_slot(uint32_t seq);
	const grid_frame *grid_published(uint32_t seq_low4);
	void grid_publish(grid_frame &f, uint32_t seq);
	// Every slot emptied (unload)
	void clear_frames();
}
