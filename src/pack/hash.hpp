#pragma once

// The texture pack's check sums (docs/texture-pack/format.md): CRC-32 as zlib's crc32 computes it (polynomial
// 0xEDB88320 reflected, start and end inverted), so tests\pack_oracle.py checks a pack with zlib.crc32 under python -I.
// The key's FNV-1a 64 is in pack.hpp (key_of).

#include <cstddef>
#include <cstdint>

namespace hifi::pack
{
	// crc32(0, p, n) is zlib.crc32(bytes); feeding the last result back continues it (crc32(crc32(0, a), b) is the CRC
	// of a then b)
	uint32_t crc32(uint32_t crc, const void *p, size_t n);
}
