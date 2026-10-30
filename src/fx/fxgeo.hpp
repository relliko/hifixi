#pragma once

// The effects' geometry chunks read from their data (the chunk after its 16-byte header, as the game keeps it at
// resource + 0x30), as the game lays them out (research/ffxi-re/notes-effects-draw.md "Chunk formats"; the reference
// these are tested against: research/ffxi-re/tools/effects_geo.py parse_d3m / parse_d3a / parse_morph). The game side
// reads the same layouts in the game's memory (src/game/fxdraw); the tests build frames from DATs with these.

#include "fxframe.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::fx::geo
{
	// 0x1F D3M (03FEE0, 03FCF0, 03FD70, 03FCE0; drawn by 040220): a triangle list, FVF 0x152
	struct d3m_run
	{
		uint32_t first = 0, count = 0; // vertices
		int32_t texture = -1;          // its name's index, -1: untextured (no textures at all, or the tail)
	};
	struct d3m
	{
		uint32_t kind = 0, textures = 0, tail = 0, triangles = 0;
		std::vector<std::string> names; // (16 bytes each)
		std::vector<vertex> vertices;   // triangles x 3
		std::vector<d3m_run> runs;      // as 040220 draws them
	};
	// double_colours: as the load (03FEE0) does with a zone up (every byte x 2, clamped)
	bool parse_d3m(const uint8_t *d, size_t size, d3m &out, std::string &why, bool double_colours = true);
	// The layouts 040220 reads (offsets from the data): the names (03FCF0) and the vertices (03FD70)
	size_t d3m_names_at(const uint8_t *d);
	size_t d3m_vertices_at(const uint8_t *d);

	// 0x21 D3A (03E6B0; drawn by 03E910): frames of quads (two triangles each, a triangle list), FVF 0x142 (24 bytes),
	// held here as FVF 0x152 with a zero normal; colours as they are (not doubled)
	struct d3a_frame
	{
		uint8_t head[4] = {};
		float t = 0.0f;                // (a flare piece: its place along the line through the screen's centre)
		std::vector<vertex> vertices;  // quads x 6
	};
	struct d3a
	{
		int16_t units = 0, frames = 0;
		uint8_t flare = 0, pieces = 0, quads = 0, uvs_unit = 0; // (+4, +5, +6, +7)
		std::string name;
		std::vector<d3a_frame> frame_list;
	};
	bool parse_d3a(const uint8_t *d, size_t size, d3a &out, std::string &why);

	// 0x25 morph (03F420, 03F8A0, 03F440): K targets blended, the triangles' colours (doubled as 03F440 copies them)
	struct morph
	{
		uint32_t targets = 0, positions = 0, normals = 0, triangles = 0;
		bool alpha_test = false;
		std::string name;
		std::vector<float> pos;             // targets x positions x 3
		std::vector<float> nrm;             // targets x normals x 3
		std::vector<uint32_t> colours;      // triangles x 3 (doubled)
		std::vector<float> uv;              // triangles x 3 x 2
		std::vector<uint16_t> pi, ni;       // triangles x 3 each
		// The triangles with the targets blended by weights (normalized; all 0: target 0)
		void blend(const float *weights, std::vector<vertex> &out) const;
	};
	bool parse_morph(const uint8_t *d, size_t size, morph &out, std::string &why, bool double_colours = true);

	// Every byte of a D3DCOLOR doubled, clamped (03FEE0, 03F440)
	uint32_t doubled(uint32_t c);
}
