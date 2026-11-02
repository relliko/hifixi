#pragma once

// BC2 (DXT3) blocks to 8-bit RGBA texels and back, and the smaller mip levels a bigger texture from an XIPivot
// overlay gets (zonemesh::texture::levels, made by the zone loader's thread, never the game's)

#include <algorithm>
#include <climits>
#include <cstdint>
#include <vector>

namespace hifi::bake::bc2
{
	// BC2 (DXT3) blocks to 8-bit RGBA texels and back, for the smaller levels below
	inline void colour565(uint32_t c, int out[3])
	{
		out[0] = int((c >> 11) & 31) * 255 / 31;
		out[1] = int((c >> 5) & 63) * 255 / 63;
		out[2] = int(c & 31) * 255 / 31;
	}
	inline std::vector<uint8_t> decode_bc2(const uint8_t *blocks, uint32_t w, uint32_t h)
	{
		std::vector<uint8_t> px(size_t(w) * h * 4);
		for (uint32_t by = 0; by < h / 4; ++by)
			for (uint32_t bx = 0; bx < w / 4; ++bx)
			{
				const uint8_t *b = blocks + (size_t(by) * (w / 4) + bx) * 16;
				int pal[4][3];
				colour565(b[8] | (b[9] << 8), pal[0]);
				colour565(b[10] | (b[11] << 8), pal[1]);
				for (int c = 0; c < 3; ++c)
				{
					pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
					pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
				}
				const uint32_t bits = b[12] | (b[13] << 8) | (b[14] << 16) | (uint32_t(b[15]) << 24);
				for (int k = 0; k < 16; ++k)
				{
					uint8_t *p = px.data() + ((size_t(by) * 4 + k / 4) * w + bx * 4 + k % 4) * 4;
					const int *c = pal[(bits >> (2 * k)) & 3];
					p[0] = uint8_t(c[0]), p[1] = uint8_t(c[1]), p[2] = uint8_t(c[2]);
					p[3] = uint8_t(((b[k / 2] >> (4 * (k % 2))) & 15) * 17);
				}
			}
		return px;
	}
	inline std::vector<uint8_t> encode_bc2(const uint8_t *px, uint32_t w, uint32_t h)
	{
		std::vector<uint8_t> out(size_t(w / 4) * (h / 4) * 16);
		for (uint32_t by = 0; by < h / 4; ++by)
			for (uint32_t bx = 0; bx < w / 4; ++bx)
			{
				uint8_t *b = out.data() + (size_t(by) * (w / 4) + bx) * 16;
				const uint8_t *t[16];
				int lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 }, mean[3] = {};
				for (int k = 0; k < 16; ++k)
				{
					t[k] = px + ((size_t(by) * 4 + k / 4) * w + bx * 4 + k % 4) * 4;
					for (int c = 0; c < 3; ++c)
						lo[c] = std::min(lo[c], int(t[k][c])), hi[c] = std::max(hi[c], int(t[k][c])), mean[c] += t[k][c];
					b[k / 2] |= uint8_t(((t[k][3] * 15 + 127) / 255) << (4 * (k % 2)));
				}
				// (the box's diagonal as the colour line, red and blue flipped when they run against green)
				int cov_rg = 0, cov_bg = 0;
				for (int k = 0; k < 16; ++k)
				{
					cov_rg += (t[k][0] * 16 - mean[0]) * (t[k][1] * 16 - mean[1]) / 256;
					cov_bg += (t[k][2] * 16 - mean[2]) * (t[k][1] * 16 - mean[1]) / 256;
				}
				if (cov_rg < 0)
					std::swap(lo[0], hi[0]);
				if (cov_bg < 0)
					std::swap(lo[2], hi[2]);
				auto to565 = [](const int c[3]) { return uint32_t(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 | ((c[2] * 31 + 127) / 255)); };
				const uint32_t c0 = to565(hi), c1 = to565(lo);
				int pal[4][3];
				colour565(c0, pal[0]);
				colour565(c1, pal[1]);
				for (int c = 0; c < 3; ++c)
				{
					pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
					pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
				}
				uint32_t bits = 0;
				for (int k = 0; k < 16; ++k)
				{
					int best = 0, best_d = INT_MAX;
					for (int i = 0; i < 4; ++i)
					{
						const int dr = t[k][0] - pal[i][0], dg = t[k][1] - pal[i][1], db = t[k][2] - pal[i][2];
						const int d = dr * dr + dg * dg + db * db;
						if (d < best_d)
							best = i, best_d = d;
					}
					bits |= uint32_t(best) << (2 * k);
				}
				b[8] = uint8_t(c0), b[9] = uint8_t(c0 >> 8), b[10] = uint8_t(c1), b[11] = uint8_t(c1 >> 8);
				b[12] = uint8_t(bits), b[13] = uint8_t(bits >> 8), b[14] = uint8_t(bits >> 16), b[15] = uint8_t(bits >> 24);
			}
		return out;
	}

	// A texture's levels after the first, each the last averaged 2x2 (levels counts the first too): BC2 decoded and
	// encoded again, B8G8R8A8 as it is
	inline std::vector<std::vector<uint8_t>> smaller_levels(const std::vector<uint8_t> &data, uint32_t w, uint32_t h, bool bc2, uint32_t levels)
	{
		std::vector<std::vector<uint8_t>> out;
		if (levels < 2)
			return out;
		std::vector<uint8_t> px = bc2 ? decode_bc2(data.data(), w, h) : data;
		for (uint32_t level = 1; level < levels; ++level)
		{
			const uint32_t hw = w / 2, hh = h / 2;
			std::vector<uint8_t> half(size_t(hw) * hh * 4);
			for (uint32_t y = 0; y < hh; ++y)
				for (uint32_t x = 0; x < hw; ++x)
					for (int c = 0; c < 4; ++c)
					{
						const uint8_t *s = px.data() + ((size_t(y) * 2) * w + x * 2) * 4 + c;
						half[(size_t(y) * hw + x) * 4 + c] = uint8_t((s[0] + s[4] + s[w * 4] + s[w * 4 + 4] + 2) / 4);
					}
			px.swap(half);
			w = hw, h = hh;
			out.push_back(bc2 ? encode_bc2(px.data(), w, h) : px);
		}
		return out;
	}
}
