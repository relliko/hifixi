#pragma once

// A Direct3D 8 texture's levels as the game's device holds them (LockRect's bytes: rows of pixels, or rows of 4x4
// blocks for DXT), turned into what a Direct3D 12 texture takes: the block formats and 32-bit BGRA kept as they are
// (BC1-3, B8G8R8A8, B8G8R8X8: the same bytes), everything else expanded to B8G8R8A8 as Direct3D 8 samples it (no alpha
// channel: 1; L8: grey). DXT with a side that isn't a multiple of 4 is decoded too (Direct3D 12 wants whole blocks).

#include <dxgiformat.h>
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::ui2d::texfmt
{
	// Direct3D 8's D3DFORMAT values used here
	enum : uint32_t
	{
		kA8R8G8B8 = 21, kX8R8G8B8 = 22, kR5G6B5 = 23, kX1R5G5B5 = 24, kA1R5G5B5 = 25, kA4R4G4B4 = 26, kA8 = 28, kX4R4G4B4 = 30,
		kL8 = 50, kA8L8 = 51,
		kDXT1 = 0x31545844, kDXT2 = 0x32545844, kDXT3 = 0x33545844, kDXT4 = 0x34545844, kDXT5 = 0x35545844,
	};

	// Whether the store takes a texture of this format (the rest: the game draws with it)
	bool supported(uint32_t d3d8_format);
	bool is_block(uint32_t d3d8_format);
	// Bytes of one row (of blocks, for DXT) of a level w wide
	uint32_t row_bytes(uint32_t d3d8_format, uint32_t w);
	uint32_t rows(uint32_t d3d8_format, uint32_t h);

	struct level
	{
		uint32_t w = 0, h = 0;
		uint32_t pitch = 0;          // bytes a row (of blocks)
		std::vector<uint8_t> bytes;  // rows(h) x pitch
	};
	struct texture
	{
		uint32_t format = 0;         // D3DFORMAT
		std::vector<level> levels;
	};

	// What it uploads as, and its levels in that format (pitch = row_bytes, rows packed). False and why for a format
	// it doesn't take.
	bool convert(const texture &in, DXGI_FORMAT &format, std::vector<level> &out, std::string &why);

	// BC1/BC2/BC3 (DXT1, DXT2/3, DXT4/5) blocks to B8G8R8A8, as the reference decoders have them (BC1's 3-colour mode
	// with transparent black; the colours' 5:6:5 widened by bit replication, the interpolants rounded down from
	// (2a + b) / 3 and (a + b) / 2)
	void decode_blocks(uint32_t d3d8_format, const uint8_t *blocks, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *bgra, uint32_t out_pitch);
}
