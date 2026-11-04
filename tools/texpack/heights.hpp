#pragma once

// The height maps' game side, shared by make --heights and the heights command (heights.cpp; the maker itself is
// src\pack\height.hpp): the zone's bump DAT height maps by name, and one entry's height record made from what's at
// hand.

#include "shared.hpp"
#include "../../src/pack/height.hpp"
#include "../../src/pack/pack_write.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace hifi::texpack::hgt
{
	// A bump DAT's height map: w x h bytes, rows top first
	struct game_map
	{
		std::vector<uint8_t> bytes;
		uint32_t w = 0, h = 0;
	};
	// A bump DAT's height maps (chunk 0x5D, 'bp' at +0, w and h u16 at +4 and +6, the bytes from +0x20) by their name's
	// last 8 characters (body+0x18), the first of each: a zone texture whose name's last 8 characters (name+8) are one
	// of them is drawn by the game with that map's normals (zonemesh.cpp, 03ADC0)
	std::unordered_map<std::string, game_map> game_maps(const std::vector<uint8_t> &bump);
	// The texture name's 8 characters a bump map is found by (name+8)
	inline std::string bump_name(const uint8_t *name16) { return std::string(reinterpret_cast<const char *>(name16) + 8, 8); }
	// A zone DAT's bump DAT (zonemesh::bump_dat of its map) by file id; 0 when the file id isn't a zone's
	uint32_t bump_id_of(uint32_t zone_file_id);
	bool is_zone_id(uint32_t file_id);

	// One entry's height: the colour it's drawn with (RGBA8 cw x ch, the game's raw alpha), the original's w x h, its
	// alpha kind and cut-out threshold, the game's map when it has one (nullptr: from the colour). ok false: why
	struct made
	{
		pk::map_in map;
		bool ok = false, from_game = false;
		std::string why;
	};
	made make(const uint8_t *rgba, uint32_t cw, uint32_t ch, uint32_t w, uint32_t h, uint8_t alpha_kind, float cut_threshold, const game_map *game,
		pk::height::scale s);
	// The colour an entry already in the pack is drawn with (RGBA8 cw x ch): its BC2 colour record read through rd, else
	// the original chunk (body, size) decoded, as the heights command takes it. false: neither (why)
	bool colour_of(pk::reader &rd, const pk::entry &e, const uint8_t *body, uint32_t size, std::vector<uint8_t> &rgba, uint32_t &cw, uint32_t &ch,
		std::string &why);
}
