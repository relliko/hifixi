#pragma once

// What shadow.cpp and lamps.cpp share (private to them): a zone DAT's bytes read as the Python reads them, a read past
// the end being a failure as struct.error is there, and the MZB unscrambled as zone_collision.py's decrypt_mzb does.
// (In a namespace of its own: the other bakers have their own MZB code.)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hifi::bake::mmb
{
	// Little-endian reads; false (and nothing read) when they'd pass the end, as struct.unpack_from raises
	inline bool u16(const uint8_t *b, size_t size, uint64_t at, uint32_t &out)
	{
		if (at + 2 > size)
			return false;
		out = uint32_t(b[at] | (b[at + 1] << 8));
		return true;
	}

	inline bool u32(const uint8_t *b, size_t size, uint64_t at, uint32_t &out)
	{
		if (at + 4 > size)
			return false;
		std::memcpy(&out, b + at, 4);
		return true;
	}

	inline bool f32(const uint8_t *b, size_t size, uint64_t at, float &out)
	{
		if (at + 4 > size)
			return false;
		std::memcpy(&out, b + at, 4);
		return true;
	}

	// zone_collision.decrypt_mzb: the MZB's first layer (runs of 16-23 bytes flipped as the key says, from byte 8, in
	// MZBs of version 27 on) and its object list's names (each record's first 16 bytes, xor 0x55). `table` is the game's
	// first key table (key_tables). False when the MZB is too short for its header.
	inline bool decrypt_mzb(std::vector<uint8_t> &m, const uint8_t *table)
	{
		uint32_t h0 = 0, h1 = 0;
		if (!u32(m.data(), m.size(), 0, h0) || !u32(m.data(), m.size(), 4, h1))
			return false;
		const uint32_t count = h0 & 0xFFFFFF, version = h0 >> 24;
		const uint32_t nodes = h1 & 0xFFFFFF, index = h1 >> 24;
		if (version >= 27)
		{
			uint32_t key = table[index ^ 0xFF], step = 0, pos = 0;
			while (pos < count)
			{
				const uint32_t piece = ((key >> 4) & 7) + 16;
				if ((key & 1) == 1 && pos + piece < count)
				{
					const size_t end = size_t(8 + pos + piece) < m.size() ? size_t(8 + pos + piece) : m.size();
					for (size_t i = 8 + pos; i < end; ++i)
						m[i] ^= 0xFF;
				}
				step += 1;
				key = (key + step) & 0xFF;
				pos += piece;
			}
		}
		for (uint32_t i = 0; i < nodes; ++i)
		{
			const size_t a = 0x20 + size_t(i) * 0x64;
			if (a + 16 > m.size())
				break;
			for (size_t k = 0; k < 16; ++k)
				m[a + k] ^= 0x55;
		}
		return true;
	}
}
