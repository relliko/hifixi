#include "texfmt.hpp"
#include <algorithm>
#include <cstring>

namespace hifi::ui2d::texfmt
{
	namespace
	{
		uint32_t bpp(uint32_t f)
		{
			switch (f)
			{
			case kA8R8G8B8:
			case kX8R8G8B8:
				return 4;
			case kR5G6B5:
			case kX1R5G5B5:
			case kA1R5G5B5:
			case kA4R4G4B4:
			case kX4R4G4B4:
			case kA8L8:
				return 2;
			case kA8:
			case kL8:
				return 1;
			default:
				return 0;
			}
		}

		uint32_t block_bytes(uint32_t f)
		{
			return f == kDXT1 ? 8 : 16;
		}

		uint8_t widen(uint32_t v, uint32_t bits)
		{
			// (bit replication: 5 bits 31 -> 255, as Direct3D's conversions and the BC decoders)
			v &= (1u << bits) - 1;
			uint32_t out = v << (8 - bits);
			for (uint32_t have = bits; have < 8; have += bits)
				out |= v << (8 - bits) >> have;
			return uint8_t(out);
		}

		// One pixel of a 16- or 8-bit format as B, G, R, A
		void expand(uint32_t f, const uint8_t *p, uint8_t *o)
		{
			uint32_t v = 0;
			std::memcpy(&v, p, bpp(f));
			switch (f)
			{
			case kR5G6B5:
				o[0] = widen(v, 5), o[1] = widen(v >> 5, 6), o[2] = widen(v >> 11, 5), o[3] = 255;
				break;
			case kX1R5G5B5:
			case kA1R5G5B5:
				o[0] = widen(v, 5), o[1] = widen(v >> 5, 5), o[2] = widen(v >> 10, 5), o[3] = f == kX1R5G5B5 || (v & 0x8000) ? 255 : 0;
				break;
			case kA4R4G4B4:
			case kX4R4G4B4:
				o[0] = widen(v, 4), o[1] = widen(v >> 4, 4), o[2] = widen(v >> 8, 4), o[3] = f == kX4R4G4B4 ? 255 : widen(v >> 12, 4);
				break;
			case kA8:
				o[0] = o[1] = o[2] = 0, o[3] = uint8_t(v);
				break;
			case kL8:
				o[0] = o[1] = o[2] = uint8_t(v), o[3] = 255;
				break;
			case kA8L8:
				o[0] = o[1] = o[2] = uint8_t(v), o[3] = uint8_t(v >> 8);
				break;
			default:
				o[0] = o[1] = o[2] = 0, o[3] = 255;
				break;
			}
		}

		void colour_block(const uint8_t *b, bool four_always, uint8_t out[16][4])
		{
			const uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
			uint8_t pal[4][4];
			const auto put = [&](uint8_t *o, uint32_t c) {
				o[0] = widen(c, 5), o[1] = widen(c >> 5, 6), o[2] = widen(c >> 11, 5), o[3] = 255;
			};
			put(pal[0], c0);
			put(pal[1], c1);
			if (c0 > c1 || four_always)
			{
				for (int k = 0; k < 3; ++k)
				{
					pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
					pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
				}
				pal[2][3] = pal[3][3] = 255;
			}
			else
			{
				for (int k = 0; k < 3; ++k)
					pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2), pal[3][k] = 0;
				pal[2][3] = 255;
				pal[3][3] = 0;
			}
			const uint32_t idx = b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24;
			for (int i = 0; i < 16; ++i)
				std::memcpy(out[i], pal[(idx >> (2 * i)) & 3], 4);
		}
	}

	bool supported(uint32_t f)
	{
		return is_block(f) || bpp(f) != 0;
	}

	bool is_block(uint32_t f)
	{
		return f == kDXT1 || f == kDXT2 || f == kDXT3 || f == kDXT4 || f == kDXT5;
	}

	uint32_t row_bytes(uint32_t f, uint32_t w)
	{
		return is_block(f) ? std::max(1u, (w + 3) / 4) * block_bytes(f) : w * bpp(f);
	}

	uint32_t rows(uint32_t f, uint32_t h)
	{
		return is_block(f) ? std::max(1u, (h + 3) / 4) : h;
	}

	void decode_blocks(uint32_t f, const uint8_t *blocks, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *bgra, uint32_t out_pitch)
	{
		const uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4), size = block_bytes(f);
		for (uint32_t by = 0; by < bh; ++by)
			for (uint32_t bx = 0; bx < bw; ++bx)
			{
				const uint8_t *b = blocks + by * pitch + bx * size;
				uint8_t px[16][4];
				if (f == kDXT1)
					colour_block(b, false, px);
				else
				{
					colour_block(b + 8, true, px);
					if (f == kDXT2 || f == kDXT3)
						for (int i = 0; i < 16; ++i)
							px[i][3] = widen((b[i / 2] >> (4 * (i & 1))) & 15, 4);
					else
					{
						uint8_t a[8];
						a[0] = b[0], a[1] = b[1];
						if (a[0] > a[1])
							for (int k = 1; k < 7; ++k)
								a[k + 1] = uint8_t(((7 - k) * a[0] + k * a[1]) / 7);
						else
						{
							for (int k = 1; k < 5; ++k)
								a[k + 1] = uint8_t(((5 - k) * a[0] + k * a[1]) / 5);
							a[6] = 0, a[7] = 255;
						}
						uint64_t bits = 0;
						for (int k = 0; k < 6; ++k)
							bits |= uint64_t(b[2 + k]) << (8 * k);
						for (int i = 0; i < 16; ++i)
							px[i][3] = a[(bits >> (3 * i)) & 7];
					}
				}
				for (uint32_t i = 0; i < 16; ++i)
				{
					const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
					if (x < w && y < h)
						std::memcpy(bgra + y * out_pitch + x * 4, px[i], 4);
				}
			}
	}

	bool convert(const texture &in, DXGI_FORMAT &format, std::vector<level> &out, std::string &why)
	{
		out.clear();
		if (in.levels.empty())
		{
			why = "no levels";
			return false;
		}
		const uint32_t f = in.format;
		if (!supported(f))
		{
			why = "format " + std::to_string(f) + " isn't taken";
			return false;
		}
		const level &top = in.levels[0];
		const bool whole_blocks = top.w % 4 == 0 && top.h % 4 == 0;
		if (is_block(f) && whole_blocks)
		{
			format = f == kDXT1 ? DXGI_FORMAT_BC1_UNORM : f == kDXT2 || f == kDXT3 ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
			for (const level &l : in.levels)
			{
				level o;
				o.w = l.w, o.h = l.h, o.pitch = row_bytes(f, l.w);
				const uint32_t n = rows(f, l.h);
				if (l.pitch < o.pitch || l.bytes.size() < size_t(l.pitch) * n)
				{
					why = "a level's bytes are short";
					return false;
				}
				o.bytes.resize(size_t(o.pitch) * n);
				for (uint32_t r = 0; r < n; ++r)
					std::memcpy(o.bytes.data() + size_t(r) * o.pitch, l.bytes.data() + size_t(r) * l.pitch, o.pitch);
				out.push_back(std::move(o));
			}
			return true;
		}
		format = f == kX8R8G8B8 ? DXGI_FORMAT_B8G8R8X8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
		for (const level &l : in.levels)
		{
			level o;
			o.w = l.w, o.h = l.h, o.pitch = l.w * 4;
			o.bytes.resize(size_t(o.pitch) * l.h);
			const uint32_t n = rows(f, l.h), need = row_bytes(f, l.w);
			if (l.pitch < need || l.bytes.size() < size_t(l.pitch) * n)
			{
				why = "a level's bytes are short";
				return false;
			}
			if (is_block(f))
				decode_blocks(f, l.bytes.data(), l.pitch, l.w, l.h, o.bytes.data(), o.pitch);
			else if (f == kA8R8G8B8 || f == kX8R8G8B8)
				for (uint32_t y = 0; y < l.h; ++y)
					std::memcpy(o.bytes.data() + size_t(y) * o.pitch, l.bytes.data() + size_t(y) * l.pitch, o.pitch);
			else
				for (uint32_t y = 0; y < l.h; ++y)
					for (uint32_t x = 0; x < l.w; ++x)
						expand(f, l.bytes.data() + size_t(y) * l.pitch + x * bpp(f), o.bytes.data() + size_t(y) * o.pitch + x * 4);
			out.push_back(std::move(o));
		}
		return true;
	}
}
