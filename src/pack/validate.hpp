#pragma once

// The texture pack's level layout and the rules a map record must meet before it's uploaded (docs/texture-pack/
// format.md "Level layout", "Rules"). Pure CPU: the 64-bit texpack lays out and checks records with no device; the
// plugin's worker (M3) and pack_test hold the layout against ID3D12Device::GetCopyableFootprints as well.

#include "pack.hpp"
#include <cstdint>

struct ID3D12Device;

namespace hifi::pack
{
	// One level as a record stores it, as D3D12 copies it (CopyTextureRegion from a placed footprint): offset from the
	// record's start (a multiple of 512), rows of BC blocks each row_pitch apart (a multiple of 256), row_bytes of
	// blocks in each
	struct level_layout
	{
		uint32_t offset = 0, row_pitch = 0, rows = 0, row_bytes = 0, w = 0, h = 0;
	};
	struct layout
	{
		uint32_t levels = 0;
		level_layout level[kMaxLevels];
		// (all of it, to the last level's last block: GetCopyableFootprints' total bytes)
		uint64_t total = 0;
		// The bytes of level i as a record stores them: to the next level's offset (the zeros between levels with it),
		// the last to total
		uint64_t level_span(uint32_t i) const { return (i + 1 < levels ? level[i + 1].offset : total) - level[i].offset; }
	};

	// Bytes per 4x4 block: 16 for BC2 and BC7, 8 for BC4; 0 for any other format (not one a pack holds)
	uint32_t block_bytes(uint32_t format);
	// D3D12's whole chain for w x h, down to 1x1 (the most levels a record may claim)
	uint32_t full_levels(uint32_t w, uint32_t h);
	// The layout of levels levels of a w x h texture; false (out empty) when the format isn't BC2, BC7 or BC4, a side is
	// 0 or over kMaxSide, or levels is 0 or over full_levels
	bool footprint(uint32_t format, uint32_t w, uint32_t h, uint32_t levels, layout &out);
	// The rules (format.md "Rules"): a known kind, BC2/BC7/BC4 UNORM, w and h multiples of 4 and at most kMaxSide,
	// 1 to full_levels levels, the offset a multiple of 512 and the stored bytes footprint's total. With a device, also
	// GetCopyableFootprints' offsets, pitches, rows and total (free-threaded: any thread). nullptr when the record
	// meets them, else why.
	const char *check(const record &r, ID3D12Device *device = nullptr);
}
