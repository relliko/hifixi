#pragma once

// The text font at 2048 x 4096 for the players on a smaller one ([screen2d] font_high, off by default; docs/own-frame/2d.md).
//
// The game's text glyphs (11BBD0) take u = (cell & 63) / 64, v = (cell >> 6) / 128 whatever the font's size
// (notes-ui-draw "Text"), so any of its three fonts can stand in for another: 'font    moji' (1024 x 2048 DXT3 in the menu
// DAT, file 0x9A76), the PNG 'font high' 1024 x 2048 (file 0x9A77) or 2048 x 4096 (file 0x9A78), config 0x24 picking
// one. With the option on, a texture the recorder takes that is exactly one of the two smaller ones (its pixels as the
// game's files have them) is handed to the store as the 2048 x 4096 PNG instead. The PNGs are decoded with Windows'
// own WIC; the files are read only.

#include "texfmt.hpp"
#include <string>

namespace hifi::ui2d::fonthigh
{
	// The three fonts read from the game's files (once; the game's thread): false and why when they can't be
	bool load(std::string &why);
	bool loaded();
	// Whether t is the 1024 x 2048 moji (DXT3 blocks) or the 1024 x 2048 PNG (A8R8G8B8 as D3DX made it from the file)
	bool is_smaller_font(const texfmt::texture &t);
	// The 2048 x 4096 PNG as a Direct3D 8 A8R8G8B8 texture would hold it (one level)
	const texfmt::texture &high();
	void unload();

	// For the tests: a PNG file's pixels as B, G, R, A rows top down; false and why
	bool decode_png(const std::vector<uint8_t> &file, uint32_t &w, uint32_t &h, std::vector<uint8_t> &bgra, std::string &why);
	// The moji's DXT3 blocks from the menu DAT; false and why
	bool moji_blocks(std::vector<uint8_t> &blocks, uint32_t &w, uint32_t &h, std::string &why);
}
