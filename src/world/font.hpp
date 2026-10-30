#pragma once

// The names' font from the game's DATs (research/ffxi-re/client-notes.md "Fonts", "Name icons"; notes-ui-draw.md "The
// names"): the menu DAT (file 0x9A76, ROM/119/51.DAT) holds the sprite set 'font    fontshp ' (type 0x31: one record
// per character 0x20..0xB1, each one part on a page) and its two pages, 'font    font    ' (the letters, 256x256 DXT3)
// and 'menu    ustatshd' (the icons 0x8E..0xB1, 256x256 DXT3), type-0x20 images found by name as the game finds them
// (120660 -> 03AC80). The pages are decoded as the characters' textures are (chars/tex: DXT3 to RGBA8, rows top down,
// the game's own alpha) for render.hpp's font_pages.
//
// The PNG font 'font high' (files 0x9A77 / 0x9A78, picked by config 0x24) is the text object's ('font    moji''s stand-in:
// chat, menus, 11BAE0); the names' font object [VA 04971BF8] binds only these two pages (120590, its page table +0x1C), so
// the names never use it.

#include "render.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::world::font
{
	constexpr uint32_t kFile = 0x9A76;
	extern const char kPageNames[2][17]; // ("font    font    ", "menu    ustatshd")
	extern const char kSetName[17];      // ("font    fontshp ")

	// A sprite set's part as 11E4E0 reads it from the DAT (61 bytes)
	struct part
	{
		int16_t quad[4][2] = {}; // x, y of TL, TR, BL, BR
		int16_t w = 0, h = 0, u = 0, v = 0;
		uint8_t flip = 0;
		uint8_t colours[4][4] = {}; // R G B A per corner
		uint8_t blend[4] = {};      // enable, A, B, D
		char texture[16] = {};
	};
	struct sprite_set
	{
		char name[16] = {};
		std::vector<std::string> textures;          // (16 bytes each)
		std::vector<std::vector<part>> records;
		size_t used = 0;                            // bytes it took of its chunk's body
	};

	// The named type-0x20 image of a DAT decoded (RGBA8 packed R | G << 8 | B << 16 | A << 24); false when it isn't there
	bool image(const std::vector<uint8_t> &dat, const char name[16], uint32_t &w, uint32_t &h, std::vector<uint32_t> &texels, std::string &why);
	// The named type-0x31 set of a DAT; false when it isn't there or doesn't parse
	bool find_set(const std::vector<uint8_t> &dat, const char name[16], sprite_set &out);
	// The two name pages from the menu DAT's bytes, or read from the game's folder (bake::dat_path(kFile)); false and why
	bool pages(const std::vector<uint8_t> &dat, font_pages &out, std::string &why);
	bool load(font_pages &out, std::string &why);
}
