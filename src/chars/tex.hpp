#pragma once

// A character texture chunk (0x20) decoded for the GPU (research/ffxi-re/notes-characters.md "Textures"; the reference
// this is tested against: tools/chartex.py Tex.rgba).
//
// Byte 0: bit 7 a DXT copy may follow the picture, bits 4-6 the layout (0, 1 plain; 2 DXT only; 3 a dword first whose
// low nibble is the compression class); the 16-byte name; a 40-byte bitmap header (width +4, height +8 as u16, bits a
// pixel +14, palette entry bits +36); from 57 the palette and the picture (rows bottom up), or the DXT copy ('DXTn'
// backwards, a dword, the row pitch, the blocks, rows top down). Only DXT1 and DXT3 copies are taken, as the game takes
// them.
//
// Out: RGBA8, rows top down (as the UVs expect), the game's own alpha (0x80 = opaque: the character pass doubles it
// and alpha tests at > 0x45), and the mips the game asks for: 6 levels (1 when a side is under 32), each the box
// filter of the level above (D3DX filter 5).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::chars::tex
{
	struct level
	{
		uint32_t w = 0, h = 0;
		std::vector<uint8_t> rgba; // w x h x 4
	};

	struct texture
	{
		uint8_t name[16] = {};
		uint32_t w = 0, h = 0;
		bool from_dxt = false;     // the DXT copy (as the game uploads it), else the picture expanded
		std::vector<level> levels; // [0] the picture, then its mips
	};

	// Level 0 only; false and why when the chunk has nothing the game would show
	bool decode(const uint8_t *data, size_t size, texture &out, std::string &why);
	// The mips the game asks for, made from level 0 (6, or 1 when a side is under 32)
	void make_mips(texture &t);
	uint32_t mip_count(uint32_t w, uint32_t h);

	// The detail map ([chars] detail; research/ffxi-re/tools/nmmockup.py, "multi-scale" and matmap) guessed from a
	// texture's level 0, with the same mips: r, g the surface's tilt (tangent space: +u, +v down the picture; 0.5 flat), the
	// height being the brightness with the painted broad shading taken out, at three scales; b metal (bright, finely
	// detailed texels: trim, studs, buckles); a cavity (darker than around it: seams, crevices). Nothing in the game has
	// these: a guess from the colours alone.
	void make_detail(const texture &colour, texture &out);
}
