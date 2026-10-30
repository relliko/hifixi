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

namespace hifi::zone
{
	class gpu_zone;
}

namespace hifi::fx
{
	constexpr uint32_t kFrames = 16;          // (the markers' seq has 4 bits)
	constexpr uint32_t kMaxElements = 2048;   // a flush's elements HiFi XI draws (a busy fight: a few hundred)
	constexpr uint32_t kMaxDraws = 4096;      // their draws (one per texture run of a D3M, one per D3A frame...)
	constexpr uint32_t kMaxVertices = 24576;  // their vertices (36 bytes each, as FVF 0x152)
	constexpr uint32_t kMaxRuns = 256;        // runs a flush (see-through characters and the game's own draws between)
	constexpr uint32_t kMaxLit = 1024;        // lit elements a flush (flags bit 16: their lights)
	constexpr uint32_t kMaxGrabs = 256;       // distortion grabs a flush (the world copied into the 256 x 256 target)
	constexpr uint32_t kNone = 0xFFFFFFFFu;
	constexpr uint32_t kDistTexture = 0xFFFFFFFEu; // a draw's texture: the distortion target its grab filled

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
		// The MMB effect meshes (183760): the draw's vertices are the element's zone mesh's (first and count index the
		// zone's indices); without either bit below, the untextured parts' table (CURRENT x TFACTOR x 2, alpha x 4)
		stage_zone_mesh = 1u << 8,
		stage_mmb_textured = 1u << 9,   // a textured part: colour TEXTURE x TFACTOR, then x DIFFUSE x 4; alpha TEXTURE x
		                                // TFACTOR x 2, then x DIFFUSE x 4
		stage_mmb_textured_d = 1u << 10, // ... with the textured mode (letter d, or flags bit 28): colour TEXTURE x
		                                 // DIFFUSE, then x TFACTOR x 4; alpha DIFFUSE x TFACTOR x 4
		stage_dist = 1u << 11,          // the distortion fan (353108): colour TEXTURE x DIFFUSE x 2, alpha DIFFUSE x TFACTOR x
		                                // 4, stage 1 off; the texture the distortion target, sampled MIRROR (353028's)
		stage_flare = 1u << 12,         // a lens flare's pieces (3520A8): colour DIFFUSE x TEXTURE x 2 (stage 1 CURRENT),
		                                // alpha DIFFUSE x TEXTURE x 2, then x TFACTOR x 2
		stage_dist_model = 1u << 13,    // a distortion D3M (040670's 352740, the target at stage 1): colour CURRENT x TFACTOR x
		                                // 2 then TEXTURE alone; alpha CURRENT x TFACTOR x 2, then x TEXTURE x 4 (stage_alpha_2x:
		                                // x 2, the D's 352848); the texture the distortion target, sampled WRAP
		stage_dist_ring = 1u << 14,     // a distortion ring (353378): colour CURRENT x TEXTURE x 2, alpha DIFFUSE x TEXTURE x 4,
		                                // stage 1 off; the texture the distortion target, sampled MIRROR
		// The specular D3M (0412B0): an environment stage after the base ones, its coordinates the camera-space normal
		// (normalized) through the element's texture matrix (element::spec), colour CURRENT + CURRENT's alpha x the
		// environment's (MODULATEALPHA_ADDCOLOR), alpha CURRENT. The base: untextured (3528F0) CURRENT x TFACTOR x 2,
		// alpha x 4; textured (352B40) TEXTURE x CURRENT x 2, alpha x 4, the stage after x TFACTOR x 4 unless kept;
		// three stages (3529C8) TEXTURE x CURRENT x 2, alpha x 2, then x TFACTOR x 2 (alpha x 4 unless kept)
		stage_spec = 1u << 15,
		stage_spec_three = 1u << 16,    // (3529C8)
		stage_spec_keep = 1u << 17,     // the alpha after the texture stage kept (SELECTARG1: letter d, flags bit 28)
		stage_spec_clamp = 1u << 18,    // the environment sampled CLAMP (+0x198 bit 3), else WRAP
		stage_spec_tint = 1u << 19,     // the environment tinted (0759C0's copy: x the tint x 2)
	};

	// How the element's PROJECTION is set (046070)
	enum projection : uint8_t
	{
		proj_camera = 0,   // the camera's, as the zone pass has it
		proj_near_far = 1, // the depth pull: near and far both moved (by (1 - z/32)^3 x 0.03 x +0x128), as 2D83C6 rebuilds it
		proj_screen = 2,   // screen-fixed elements: the fixed field of view 2 atan2(192, 350)
		proj_identity = 3, // pre-transformed vertices: WORLD takes the game's pixels to clip space, PROJECTION identity
	};

	// A lit element's light (flags bit 16), as 0467E0 sets D3D8's fixed-function lighting: lights 0 and 1 (directional,
	// 18B9A0 / 18B960 / 188B70, their directions turned by its frame), AMBIENT (18B9E0), the material (VA 0490BC28),
	// with the device's COLORVERTEX 1 and AMBIENT/DIFFUSEMATERIALSOURCE COLOR1 (009A1C, 009A42): the vertex colour is
	// both materials, so colour = vertex x (ambient + sum of diffuse x max(0, N.L)) + emissive, alpha the vertex's.
	// Normals by the inverse transpose of WORLD, not normalized (NORMALIZENORMALS 0); D3DCOLOR parts as 0..1.
	struct lighting
	{
		float normal[3][4];  // the rows normals are multiplied by: (WORLD's 3x3)^-1, transposed
		float toward[2][4];  // toward each light (its direction negated, normalized, as D3D takes it)
		float diffuse[2][4]; // each light's diffuse (0: the light is off)
		float ambient[4];    // AMBIENT plus each light's ambient
		float emissive[4];   // the material's
	};
	static_assert(sizeof(lighting) == 144, "the shader's");

	// One draw: a triangle list of the frame's vertices with one texture (kNone: untextured) and stage program
	struct draw
	{
		uint32_t first = 0, count = 0; // vertices
		uint32_t texture = kNone;      // (src/fx/textures' id; kDistTexture: the distortion target)
		uint32_t stages = 0;           // stage_bits
		uint32_t grab = kNone;         // a distortion grab drawn into the distortion target first (frame::grabs)
	};

	// A distortion grab (0426F0, 042D00): the world as drawn so far, copied into the game's 256 x 256 target (VA
	// 0490BB88) through a quad of pre-transformed vertices (a strip 0 1 2 3: D3D9 pixels of that target, z 0, rhw 1),
	// each with its UV into the world (3530D0 / 353028: colour the texture, alpha TFACTOR 0x80; no blend, no z; LINEAR,
	// ADDRESS MIRROR)
	struct grab
	{
		float pos[4][2];
		float uv[4][2];
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
		// An MMB effect mesh's: the zone's data on the GPU its model is in (the native zone renderer's: the effect's
		// model is the zone's own, 183420), its draws stage_zone_mesh
		const zone::gpu_zone *zone = nullptr;
		uint32_t light = kNone;   // a lit element's lighting (the frame's lights), kNone: unlit (LIGHTING 0)
		// A specular D3M's environment (stage_spec): its texture, and its record in the frame's lights (a lighting laid
		// out as: normal the rows camera-space normals are made with, toward[0] and [1] the texture matrix's columns 0
		// and 1 (its rows 0-3), diffuse[0] the tint)
		uint32_t env = kNone;
		uint32_t spec = kNone;
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
		class_dist = 7,    // a distortion quad (CMoDist 0426F0, CMoVuDist 043110)
		class_flare = 8,   // a lens flare's pieces (CMoLfdElem 0548D0 -> 03EBF0)
		class_dist_model = 9,  // a D3M as distortion (CMoDistModelElem 043320 -> 042D00, 040670)
		class_dist_morph = 10, // a morph as distortion (CMoDistMorphElem 043590 -> 042D00, 03F440)
		class_dist_ring = 11,  // rings as distortion (CMoDistRingElem 043720 -> 042D00)
		class_spec = 12,       // the specular D3M (CMoD3mSpecularElem 0412B0)
		class_number = 13,     // a battle number (kind 0x5C, 041AF0 -> 041C20): its glyphs' quads as the game places them
		classes = 14,
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
		uint32_t element_count = 0, draw_count = 0, vertex_count = 0, light_count = 0, grab_count = 0;
		run run_list[kMaxRuns];
		lighting lights[kMaxLit];
		grab grabs[kMaxGrabs];
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
