/*
 * CRC-32 for the texture pack (hash.hpp), eight bytes a step (slicing-by-8: a pack's 7 GB checked in seconds rather
 * than half a minute).
 */

#include "hash.hpp"
#include <cstring>

namespace hifi::pack
{
	namespace
	{
		struct tables
		{
			uint32_t t[8][256];
			tables()
			{
				for (uint32_t i = 0; i < 256; ++i)
				{
					uint32_t c = i;
					for (int k = 0; k < 8; ++k)
						c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
					t[0][i] = c;
				}
				for (uint32_t i = 0; i < 256; ++i)
					for (int s = 1; s < 8; ++s)
						t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
			}
		};
		// (built once, on first use: a function-local static is thread-safe)
		const tables &table()
		{
			static const tables t;
			return t;
		}
	}

	uint32_t crc32(uint32_t crc, const void *data, size_t n)
	{
		const auto &t = table().t;
		const uint8_t *p = static_cast<const uint8_t *>(data);
		crc = ~crc;
		while (n >= 8)
		{
			uint32_t a, b;
			std::memcpy(&a, p, 4);
			std::memcpy(&b, p + 4, 4);
			a ^= crc;
			crc = t[7][a & 0xFF] ^ t[6][(a >> 8) & 0xFF] ^ t[5][(a >> 16) & 0xFF] ^ t[4][a >> 24] ^
				t[3][b & 0xFF] ^ t[2][(b >> 8) & 0xFF] ^ t[1][(b >> 16) & 0xFF] ^ t[0][b >> 24];
			p += 8;
			n -= 8;
		}
		while (n-- > 0)
			crc = (crc >> 8) ^ t[0][(crc ^ *p++) & 0xFF];
		return ~crc;
	}
}
