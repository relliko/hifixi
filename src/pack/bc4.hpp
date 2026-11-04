#pragma once

// BC4_UNORM (one channel, 8 bytes a 4x4 block) for the texture pack's height records (docs/texture-pack/format.md
// "Height records", plan M9). The encoder is what builds packs (texpack make and heights, the tests); the decoder is
// the tests' and the height maker's check. Pure CPU, no device.
//
// A block (D3D's BC4_UNORM): r0, r1 (bytes 0, 1), then 16 3-bit codes, texel k's at bit 3k of the 48-bit little-endian
// value in bytes 2..7. r0 > r1: code 0 is r0, 1 is r1, 2..7 are ((8-c)*r0 + (c-1)*r1)/7. r0 <= r1: 2..5 are
// ((6-c)*r0 + (c-1)*r1)/5, 6 is 0 and 7 is 255.

#include <cstdint>
#include <vector>

namespace hifi::pack::bc4
{
	// w x h single-channel texels (rows top first; w and h multiples of 4) to tight BC4 blocks (rows of blocks back to
	// back): each block the better of the 8-value mode on its own minimum and maximum (each end then nudged while that
	// lowers the squared error) and the 6-value mode with explicit 0 and 255. Empty when a side isn't a multiple of 4
	std::vector<uint8_t> encode(const uint8_t *texels, uint32_t w, uint32_t h);
	// Tight BC4 blocks back to w x h texels, each palette value rounded to the nearest 8-bit level (the GPU's are
	// exact; the rounding is at most half a level)
	std::vector<uint8_t> decode(const uint8_t *blocks, uint32_t w, uint32_t h);
	// The bound encode keeps for every texel (pack_test and texpack_test hold it): at most this far from its source,
	// in 8-bit levels, for a block whose texels span lo..hi: half a palette step plus the decoder's rounding
	inline int max_error(int lo, int hi) { return (hi - lo + 13) / 14 + 1; }
}
