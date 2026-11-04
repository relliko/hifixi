/*
 * BC4_UNORM blocks (bc4.hpp): the height records' format. The encoder takes each block's own range for the 8-value
 * mode, then moves each end a level at a time while that lowers the block's squared error (a block whose texels sit
 * in two clusters fits better with the ends pulled in), and also tries the 6-value mode with explicit 0 and 255 (a
 * block that touches both extremes). Every texel is coded to its nearest palette value.
 */

#include "bc4.hpp"
#include <algorithm>
#include <climits>
#include <cmath>

namespace hifi::pack::bc4
{
	namespace
	{
		// The 8 palette values of (r0, r1), rounded as decode rounds them
		void palette(int r0, int r1, int out[8])
		{
			out[0] = r0, out[1] = r1;
			if (r0 > r1)
				for (int c = 2; c < 8; ++c)
					out[c] = ((8 - c) * r0 + (c - 1) * r1 + 3) / 7;
			else
			{
				for (int c = 2; c < 6; ++c)
					out[c] = ((6 - c) * r0 + (c - 1) * r1 + 2) / 5;
				out[6] = 0, out[7] = 255;
			}
		}

		// The block's squared error with (r0, r1), each texel at its nearest value; codes filled, the largest error in worst
		int64_t fit(const int t[16], int r0, int r1, uint8_t codes[16], int &worst)
		{
			worst = 0;
			int pal[8];
			palette(r0, r1, pal);
			int64_t err = 0;
			for (int k = 0; k < 16; ++k)
			{
				int best = 0, best_d = INT_MAX;
				for (int c = 0; c < 8; ++c)
				{
					const int d = (t[k] - pal[c]) * (t[k] - pal[c]);
					if (d < best_d)
						best = c, best_d = d;
				}
				codes[k] = uint8_t(best);
				err += best_d;
				worst = std::max(worst, int(std::lround(std::sqrt(double(best_d)))));
			}
			return err;
		}

		void write(uint8_t *b, int r0, int r1, const uint8_t codes[16])
		{
			b[0] = uint8_t(r0), b[1] = uint8_t(r1);
			uint64_t bits = 0;
			for (int k = 0; k < 16; ++k)
				bits |= uint64_t(codes[k] & 7) << (3 * k);
			for (int i = 0; i < 6; ++i)
				b[2 + i] = uint8_t(bits >> (8 * i));
		}
	}

	std::vector<uint8_t> encode(const uint8_t *texels, uint32_t w, uint32_t h)
	{
		std::vector<uint8_t> out;
		if (w == 0 || h == 0 || w % 4 != 0 || h % 4 != 0 || texels == nullptr)
			return out;
		out.assign(size_t(w / 4) * (h / 4) * 8, 0);
		for (uint32_t by = 0; by < h / 4; ++by)
			for (uint32_t bx = 0; bx < w / 4; ++bx)
			{
				int t[16], lo = 255, hi = 0;
				for (int k = 0; k < 16; ++k)
				{
					t[k] = texels[(size_t(by) * 4 + k / 4) * w + bx * 4 + k % 4];
					lo = std::min(lo, t[k]), hi = std::max(hi, t[k]);
				}
				uint8_t *b = out.data() + (size_t(by) * (w / 4) + bx) * 8;
				uint8_t codes[16], try_codes[16];
				if (lo == hi)
				{
					// (one value: r0 > r1 needs two; r0 == r1 is the 6-value mode with every code 0)
					std::fill(codes, codes + 16, uint8_t(0));
					write(b, lo, lo, codes);
					continue;
				}
				// The 8-value mode on the block's range, each end then nudged a level at a time while the squared error drops
				// and no texel goes past the range's own worst (max_error's bound holds)
				int r0 = hi, r1 = lo, worst = 0, w2 = 0;
				int64_t best = fit(t, r0, r1, codes, worst);
				const int cap = worst;
				for (bool moved = true; moved;)
				{
					moved = false;
					for (const auto &[d0, d1] : { std::pair{ -1, 0 }, std::pair{ 0, 1 }, std::pair{ 1, 0 }, std::pair{ 0, -1 } })
					{
						const int n0 = r0 + d0, n1 = r1 + d1;
						if (n0 <= n1 || n0 > 255 || n1 < 0)
							continue;
						const int64_t e = fit(t, n0, n1, try_codes, w2);
						if (e < best && w2 <= cap)
						{
							best = e, r0 = n0, r1 = n1, moved = true;
							std::copy(try_codes, try_codes + 16, codes);
						}
					}
				}
				// The 6-value mode: the range of the texels that aren't 0 or 255 (those take the explicit codes)
				int ilo = 255, ihi = 0;
				for (int k = 0; k < 16; ++k)
					if (t[k] != 0 && t[k] != 255)
						ilo = std::min(ilo, t[k]), ihi = std::max(ihi, t[k]);
				if (ilo > ihi)
					ilo = ihi = lo == 0 ? hi : lo;
				const int64_t e6 = fit(t, ilo, ihi, try_codes, w2);
				if (e6 < best && w2 <= cap)
				{
					write(b, ilo, ihi, try_codes);
					continue;
				}
				write(b, r0, r1, codes);
			}
		return out;
	}

	std::vector<uint8_t> decode(const uint8_t *blocks, uint32_t w, uint32_t h)
	{
		std::vector<uint8_t> out(size_t(w) * h);
		for (uint32_t by = 0; by < h / 4; ++by)
			for (uint32_t bx = 0; bx < w / 4; ++bx)
			{
				const uint8_t *b = blocks + (size_t(by) * (w / 4) + bx) * 8;
				int pal[8];
				palette(b[0], b[1], pal);
				uint64_t bits = 0;
				for (int i = 0; i < 6; ++i)
					bits |= uint64_t(b[2 + i]) << (8 * i);
				for (int k = 0; k < 16; ++k)
					out[(size_t(by) * 4 + k / 4) * w + bx * 4 + k % 4] = uint8_t(pal[(bits >> (3 * k)) & 7]);
			}
		return out;
	}
}
