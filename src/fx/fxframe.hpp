#pragma once

// The effect elements of one priority-12 flush as the game would have drawn them, handed from the game's thread
// (src/game/fxdraw: recorded at each element's setup, 046070's call of 0467E0) to the thread that records dgVoodoo's
// Direct3D 12 list (src/fx/inject: drawn at the fx_run markers). research/ffxi-re/notes-effects-draw.md has where each
// value comes from.
//
// A ring of kFrames slots keyed by the flush's sequence number (the markers carry its low 4 bits). A flush's elements
// come in runs: the elements recorded between two of the flush's other draws (a see-through character, a weapon trail,
// an element left to the game). Each run ends with an fx_run marker; the n-th fx_run marker of a flush draws its run n,
// so the far-to-near order of the flush stays as the game has it. The game's thread fills a run, then publishes it
// (runs stored last) and sends its marker; dgVoodoo's thread, a frame or so behind, reads the runs the markers name.

#include <atomic>
#include <cstdint>

namespace hifi::fx
{
	constexpr uint32_t kFrames = 16;          // (the markers' seq has 4 bits)
	constexpr uint32_t kMaxElements = 2048;   // a flush's elements HiFi XI draws (a busy fight: a few hundred)
	constexpr uint32_t kMaxDraws = 4096;      // their draws (one per texture run of a D3M, one per D3A frame...)
	constexpr uint32_t kMaxVertices = 24576;  // their vertices (36 bytes each, as FVF 0x152)
	constexpr uint32_t kMaxRuns = 256;        // runs a flush (see-through characters and the game's own draws between)
	constexpr uint32_t kNone = 0xFFFFFFFFu;

	// A vertex as the game's FVF 0x152 has it: position, normal, D3DCOLOR (0xAARRGGBB), one UV
	struct vertex
	{
		float pos[3];
		float normal[3];
		uint32_t colour;
		float uv[2];
	};
	static_assert(sizeof(vertex) == 36, "FVF 0x152");

	// The blend 0467E0 sets by the element's letter (+0x16C low byte; the jump table at 04725C/047234), and the one
	// the D3M class sets for I (040220: DESTCOLOR / SRCALPHA)
	enum blend : uint8_t
	{
		blend_one_zero = 0,        // 0x00
		blend_add = 1,             // H, h, I: SRCALPHA / ONE
		blend_alpha = 2,           // D, 3, d: SRCALPHA / INVSRCALPHA
		blend_darken = 3,          // F, G: ZERO / INVSRCALPHA
		blend_subtract = 4,        // A, B, C, b: SRCALPHA / ONE, REVSUBTRACT (046550 with the device's blend ops)
		blend_multiply = 5,        // the D3M class's I: DESTCOLOR / SRCALPHA
		blends = 6,
	};

	// The texture stages a draw uses (the state blocks the class draws apply; "0x80 = 1.0": every MODULATE2X/4X is
	// clamped to 0..1 after its stage, as the fixed function does)
	enum stage_bits : uint32_t
	{
		stage_texture = 1u << 0,    // stage 0 samples the texture: colour CURRENT x TEXTURE x 2, alpha x TEXTURE x 2;
		                            // without it the one-stage untextured table (352348, 352648, 353810): colour
		                            // CURRENT x TFACTOR x 2, alpha CURRENT x TFACTOR x 4
		stage_colour_diffuse = 1u << 1, // stage 0 colour SELECTARG1 CURRENT (the D3M's I, 352590)
		stage_alpha_diffuse = 1u << 2,  // stage 0 alpha SELECTARG1 (flags bit 28: 3523D0, 351EC8)
		stage_colour_tfactor = 1u << 3, // stage 1 colour SELECTARG2 TFACTOR (352590)
		stage_alpha_tfactor = 1u << 4,  // stage 1 alpha SELECTARG2 TFACTOR (the D3A's @, 352084)
		stage_alpha_2x = 1u << 5,       // untextured: alpha x TFACTOR x 2, not x 4 (the D3M's D, 3523B4)
		stage_alpha_test = 1u << 6,     // ALPHATEST GREATER 0x60 (the morph's header bit 7)
		stage_diffuse = 1u << 7,        // colour and alpha SELECTARG1 DIFFUSE, no TEXTUREFACTOR (the weapon trails, 01C940)
	};

	// How the element's PROJECTION is set (046070)
	enum projection : uint8_t
	{
		proj_camera = 0,   // the camera's, as the zone pass has it
		proj_near_far = 1, // the depth pull: near and far both moved (by (1 - z/32)^3 x 0.03 x +0x128), as 2D83C6 rebuilds it
		proj_screen = 2,   // screen-fixed elements: the fixed field of view 2 atan2(192, 350)
	};

	// One draw: a triangle list of the frame's vertices with one texture (kNone: untextured) and stage program
	struct draw
	{
		uint32_t first = 0, count = 0; // vertices
		uint32_t texture = kNone;      // (src/fx/textures' id)
		uint32_t stages = 0;           // stage_bits
	};

	// One element: everything 0467E0 and its class draw set for it
	struct element
	{
		float world[16];          // WORLD: its draw matrix +0xA0 (045D40, view space, row vectors; VIEW is identity)
		uint32_t tfactor = 0;     // TEXTUREFACTOR (0xAARRGGBB) as the class draw sets it
		uint8_t blend = blend_add;
		uint8_t z_write = 0;      // flags bit 12 (the D3A draws force it off)
		uint8_t fog = 0;          // 0 none (flags bit 25), 1 by the view depth, 2 by the distance (RANGEFOG, vertex fog)
		uint8_t projection = proj_camera;
		float near_z = 0.0f, far_z = 0.0f; // (proj_near_far, proj_screen)
		float scale_x = 0.0f, scale_y = 0.0f; // (proj_screen: its _11 and _22)
		uint32_t fog_colour = 0;  // FOGCOLOR (H: 0, additive glows fade to nothing)
		float fog_start = 0.0f, fog_end = 0.0f;
		float uv[2] = {};         // TEXTURE0's offset (+0x120, +0x124 when +0x17B bits 0-1)
		uint32_t first_draw = 0, draws = 0;
		uint32_t cls = 0;         // what drew it (element_class, for the counts)
	};

	enum element_class : uint32_t
	{
		class_d3m = 0,
		class_mmb = 1,
		class_d3a = 2,
		class_vuanm = 3,
		class_morph = 4,
		class_ring = 5,
		class_trail = 6,   // a weapon trail (CYyAfterImage, 01C940): not an element, a flush callback of its own
		classes = 7,
	};

	struct run
	{
		uint32_t first = 0, count = 0; // elements
	};

	struct frame
	{
		std::atomic<uint32_t> seq { 0 };  // the flush this holds (0: none)
		std::atomic<uint32_t> runs { 0 }; // runs published (each one's marker sent after)
		// The camera's PROJECTION as the zone pass left it (renderer +0x38DDC: jitter in it); D3D's RH, row vectors
		float proj[16];
		// The camera's VIEW (gfx +0xB50): what the weapon trails' world-space points are drawn through (WORLD identity)
		float view[16];
		uint32_t element_count = 0, draw_count = 0, vertex_count = 0;
		run run_list[kMaxRuns];
		element elements[kMaxElements];
		draw draws[kMaxDraws];
		vertex vertices[kMaxVertices];
	};

	// The ring (made at the first use, on the heap): the slot for a seq, to fill (the game's thread) or to read
	// (dgVoodoo's: nullptr unless that seq's flush is in it)
	frame &frame_slot(uint32_t seq);
	const frame *published(uint32_t seq_low4);
	// Every slot emptied (unload)
	void clear_frames();

	// 0467E0's blend by the letter (+0x16C; the jump table at 04725C/047234) and its changes to the colour's alpha (d, h:
	// min(255, the letter's high byte x 2); b: the high byte); false for a letter it has no case for (the blend left as
	// the last element had it). A, B, C, b: subtractive as 046550 sets it with the device's blend ops.
	bool letter_blend(uint16_t letter, uint32_t &colour, uint8_t &blend);

	// The projection an element draws with: the frame's, its near and far moved, or the screen-fixed one (_11, _22 its
	// own); RH as D3DXMatrixPerspectiveFovRH: _33 = f / (n - f), _43 = n f / (n - f). The jitter (_31, _32) kept.
	void element_projection(const frame &f, const element &e, float out[16]);
}
