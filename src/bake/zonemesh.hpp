#pragma once

// A zone's meshes, textures and normal maps for HiFi XI's native zone renderer, read from the player's own game files
// at zone load (nothing of Square Enix's ships, and nothing is written: unlike the other bakers this is held in memory
// and uploaded). research/ffxi-re/notes-zone.md has the formats; research/ffxi-re/tools/zone_mesh_oracle.py reads the
// same independently (the test's oracle).
//
// A map number is a zone or a second map (a building interior: map 412 is Port Windurst's Fishermen's Guild): its
// DAT is file id map + 100 (+ 0x144F7 from 600), its bump DAT map + 0x9B97 (+ 0x14817 from 600).
//
// What's kept, in this order:
//   - models: each MMB chunk (type 0x2E, unscrambled), the first of its name (header bytes 16-31: the name the live
//     model has at +8), with at least one piece that has a triangle; kind = header byte 4 (1: 36-byte vertices, drawn
//     by the game fixed-function or, when its textures have normal maps, by its bump shader (kind 3); 2: 48-byte,
//     swaying plants);
//   - pieces, in file order: kind 1 strips cut into triangles without the degenerate ones (the winding kept), kind 2
//     triangle lists; a triangle with a corner past the vertices is dropped; each kept piece's vertices appended
//     whole, its indices made absolute;
//   - textures the kept pieces name, in first-use order, the DAT's first image of that name (03AD40: all 16
//     characters): 0xA1 DXT3, kept as its BC2 blocks; 0xB1 palettized, expanded to B8G8R8A8 through its palette;
//   - normal maps (03ADC0): the bump DAT's first height map (chunk 0x5D) whose name's last 8 characters are the
//     texture's last 8, made into B8G8R8A8 texels as the game makes them (01E0A0, sobel below), in first-use order;
//   - placements: the MZB's object records naming a kept model (the offline pictures; the live draw takes the game's
//     own objects).

#include "bake.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::bake::zonemesh
{
	constexpr uint32_t kNone = 0xFFFFFFFFu;

	// One vertex, whatever the MMB's kind: position, normal, colour (D3DCOLOR: alpha 0x80 = 1), UV, and the sway
	// vector (kind 2: the frame's position is position + weight x sway; 0 for the others)
	struct vertex
	{
		float pos[3];
		float normal[3];
		uint32_t colour;
		float uv[2];
		float sway[3];
	};
	static_assert(sizeof(vertex) == 48);

	// A piece's bits: its count dword's bits 28-31, shifted down (notes-zone "The piece bits")
	enum piece_bit : uint32_t
	{
		kDoorsEnv = 1,  // bit 28: lit by the zone's ev10 environment in the doors pass
		kTwoSided = 2,  // bit 29: CULLMODE NONE
		kBit30 = 4,     // bit 30: never read by the game
		kBlended = 8,   // bit 31: SRCALPHA / INVSRCALPHA, Z writes off, ZBIAS 8
	};

	struct piece
	{
		uint32_t first_index = 0, triangles = 0; // (indices into the zone's, absolute)
		uint32_t texture = kNone, normal_map = kNone;
		uint32_t bits = 0;                       // (bits 28-31 of the count dword, >> 28)
	};

	struct model
	{
		char name[16] = {};
		uint32_t kind = 1;
		bool bump = false; // drawn by the bump shader (16FCD0's kind 3): kind 1 with every group's first piece's
		                   // texture height-mapped; its pieces with a normal map then use it (the others: fixed-function)
		uint32_t first_piece = 0, pieces = 0;
		uint32_t first_vertex = 0, vertices = 0;
		float lo[3] = {}, hi[3] = {}; // (its positions' box, sways included)
	};

	enum texture_format : uint32_t
	{
		kBC2 = 0,   // DXT3 blocks, 16 bytes per 4x4
		kBGRA8 = 1, // a palettized image expanded
	};
	struct texture
	{
		char name[16] = {};
		uint32_t w = 0, h = 0, format = kBC2;
		std::vector<uint8_t> data; // (rows top first, as stored)
	};

	struct normal_map
	{
		char name[16] = {};   // (the height map's: 'bump    ' + the 8 characters it's matched by)
		uint32_t w = 0, h = 0;
		std::vector<uint32_t> texels; // (B8G8R8A8: R the up-down slope, G the left-right one, B up; A 0)
	};

	enum placement_flag : uint32_t
	{
		kAlphaTested = 1, // the record's name starts with '_'
		kMirrored = 2,    // scale x * scale z < 0: CULL CW
		kClamped = 4,     // '#': no alpha test, CLAMP
	};
	struct placement
	{
		uint32_t model = 0, flags = 0;
		float m[9] = {}; // scale x Rx x Ry x Rz, row vectors: world = position m + t
		float t[3] = {};
	};

	struct zone
	{
		uint32_t map = 0;
		std::vector<vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<piece> pieces;
		std::vector<model> models;
		std::vector<texture> textures;
		std::vector<normal_map> normal_maps;
		std::vector<placement> placements;
		uint32_t missing_textures = 0; // (pieces whose texture isn't in the DAT: drawn untextured)

		// A model by its 16-character name (the live model's +8), kNone when there's none
		uint32_t find(const char name[16]) const;
		// Bytes it takes (what the GPU will hold)
		size_t bytes() const;
	};

	uint32_t map_dat(uint32_t map);
	uint32_t bump_dat(uint32_t map);

	// The zone's data; false with r.why (or r.none: no such DAT) when it can't be read
	bool load(uint32_t map, zone &out, result &r);

	// 01E0A0: a height map (w x h bytes, rows top first) made into normals, wrapping at the edges. With gy = (the row
	// above's 1 2 1) - (the row below's) and gx = (the left column's 1 2 1) - (the right's), and k = 127 /
	// sqrt(gx^2 + gy^2 + 255^2), each texel (as a word, B8G8R8A8) is R = 128 + trunc(gy k), G = 128 + trunc(gx k),
	// B = 128 + trunc(255 k), A = 0.
	void sobel(const uint8_t *heights, uint32_t w, uint32_t h, uint32_t *out);
}
