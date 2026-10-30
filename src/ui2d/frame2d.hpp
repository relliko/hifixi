#pragma once

// The game's 2D draws as Direct3D 8 had them, handed from the game's thread (src/game/uidraw: recorded at the device
// while the loop tail draws the screen's 2D) to the thread that records dgVoodoo's Direct3D 12 list (src/ui2d: drawn at
// the slot markers). docs/own-frame/2d.md has the whole of it.
//
// A ring of kFrames slots keyed by the window's sequence number (the markers carry its low 4 bits), as the zone's: the
// game thread fills slot seq % kFrames while the window is open. A window is cut into segments: each one is published
// (its draws and vertices written first, the segment count stored last) just before the game side sends the marker
// that draws it, so dgVoodoo's thread, a frame or so behind, finds the n-th segment there at the n-th marker.
//
// A draw keeps the vertices as Direct3D 8 had them (pre-transformed: x, y, z, rhw, D3DCOLOR, u, v; a triangle list,
// strips and fans unrolled with each triangle's first vertex first: the flat-shaded colour) and the state it was drawn
// with, in Direct3D 8's own values.

#include <atomic>
#include <cstdint>

namespace hifi::ui2d
{
	constexpr uint32_t kFrames = 16;          // (the markers' seq has 4 bits)
	constexpr uint32_t kMaxDraws = 4096;      // a window's draws (the 2026-10-25 stream log: 2,156 UI draws in a frame)
	constexpr uint32_t kMaxVertices = 6 * kMaxDraws;
	constexpr uint32_t kMaxSegments = 64;
	constexpr uint32_t kNoTexture = 0xFFFFFFFFu;

	struct vertex
	{
		float x, y, z, rhw;
		uint32_t colour; // (D3DCOLOR: 0xAARRGGBB)
		float u, v;
	};
	static_assert(sizeof(vertex) == 28, "a vertex as FVF 0x144 has it");

	// Stage 0's colour or alpha operation: D3DTOP and its two D3DTA arguments (with their modifier bits)
	struct stage_op
	{
		uint8_t op = 1, arg1 = 2, arg2 = 1, unused = 0;
		uint32_t packed() const { return uint32_t(op) | uint32_t(arg1) << 8 | uint32_t(arg2) << 16; }
	};

	// The output merger's part (one pipeline each), in Direct3D 8's values
	struct blend_key
	{
		uint8_t enable = 0;
		uint8_t src = 2, dst = 1, op = 1; // (D3DBLEND, D3DBLENDOP)
		uint8_t write_mask = 0xF;         // (D3DRS_COLORWRITEENABLE)
		uint8_t cull = 1;                 // (D3DCULL: 1 none, 2 clockwise, 3 counter-clockwise)
		uint8_t unused[2] = {};
		uint64_t value() const
		{
			return uint64_t(enable) | uint64_t(src) << 8 | uint64_t(dst) << 16 | uint64_t(op) << 24 | uint64_t(write_mask) << 32 | uint64_t(cull) << 40;
		}
		bool operator==(const blend_key &o) const { return value() == o.value(); }
	};

	struct draw
	{
		uint32_t first = 0, count = 0;     // the frame's vertices (a triangle list)
		uint32_t texture = kNoTexture;     // the texture store's slot (render2d), stage 0
		blend_key blend;
		stage_op colour, alpha;            // stage 0 (COLOROP DISABLE: the diffuse colour as it is)
		uint8_t alpha_test = 0;            // ALPHATESTENABLE
		uint8_t alpha_func = 8, alpha_ref = 0; // (D3DCMP, 0-255)
		uint8_t flat = 0;                  // SHADEMODE FLAT
		uint8_t min_filter = 1, mag_filter = 1, mip_filter = 0; // (D3DTEXF)
		uint8_t address_u = 1, address_v = 1; // (D3DTADDRESS)
		uint8_t unused[3] = {};
		uint32_t tfactor = 0xFFFFFFFFu;    // TEXTUREFACTOR
		int32_t clip[4] = {};              // the viewport: left, top, right, bottom (the target's pixels)
		uint32_t target_w = 0, target_h = 0; // the Direct3D 8 target the vertices are in

		// Whether a following draw goes in the same Direct3D 12 draw
		bool same_state(const draw &o) const
		{
			return texture == o.texture && blend == o.blend && colour.packed() == o.colour.packed() && alpha.packed() == o.alpha.packed() &&
				alpha_test == o.alpha_test && alpha_func == o.alpha_func && alpha_ref == o.alpha_ref && flat == o.flat && min_filter == o.min_filter &&
				mag_filter == o.mag_filter && mip_filter == o.mip_filter && address_u == o.address_u && address_v == o.address_v && tfactor == o.tfactor &&
				clip[0] == o.clip[0] && clip[1] == o.clip[1] && clip[2] == o.clip[2] && clip[3] == o.clip[3] && target_w == o.target_w && target_h == o.target_h;
		}
	};

	struct segment
	{
		uint32_t first = 0, count = 0; // (draws)
		uint32_t kind = 0;             // the marker's (slotmark: 8 screen, 10 interface end or a cut, 11 frame end)
		bool native = false;           // the game left these draws out: HiFi XI draws them (else the game drew them too)
	};

	struct frame
	{
		std::atomic<uint32_t> seq { 0 };      // the window this holds (0: none, or being written)
		std::atomic<uint32_t> segments { 0 }; // published
		segment seg[kMaxSegments];
		uint32_t draw_count = 0, vertex_count = 0;
		draw draws[kMaxDraws];
		vertex vertices[kMaxVertices];
	};

	// The ring (static): the slot for a seq, to fill (the game's thread: begin() empties it), or to read (dgVoodoo's:
	// nullptr unless that seq's window is there)
	frame &begin(uint32_t seq);
	const frame *published(uint32_t seq_low4);
	// A segment published (the game's thread): false when there's no room for another
	bool publish(frame &f, const segment &s);
	// Every slot emptied (unload)
	void clear();
}
