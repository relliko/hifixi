/*
 * The texture pack's mip chains (bc2.hpp full_chain; docs/texture-pack/format.md "Mips"). Each level is the last one
 * averaged 2x2 (smaller_levels' box exactly, on 8-bit texels kept from level to level, each level encoded from them).
 * A cut-out texture's alpha is then scaled in each level, by the factor that brings the fraction of texels passing the
 * game's alpha test closest to level 0's: averaged alpha falls under the threshold at the edges of thin shapes
 * (leaves, grass, fences, hair), so plain mips thin them out and they vanish at a distance. The fraction is counted on
 * the alpha as BC2 stores it (4 bits, decoded as times 17), from a histogram, so the search costs nothing per texel.
 */

#include "bc2.hpp"
#include "pack.hpp"
#include <cmath>

namespace hifi::pack::bc2
{
	namespace
	{
		// The 8-bit alpha a texel decodes to once encode_bc2 has stored it
		inline int stored(int a) { return (a * 15 + 127) / 255 * 17; }

		// The fraction of a histogram's texels that pass the test once their alpha is scaled by s (and stored)
		double scaled_coverage(const uint64_t hist[256], uint64_t total, double s, float threshold)
		{
			uint64_t pass = 0;
			for (int a = 0; a < 256; ++a)
				if (hist[a] != 0 && float(stored(std::min(255, int(a * s + 0.5)))) > threshold)
					pass += hist[a];
			return total ? double(pass) / double(total) : 0.0;
		}
	}

	uint32_t chain_levels(uint32_t w, uint32_t h)
	{
		if (w == 0 || h == 0 || w % 4 != 0 || h % 4 != 0)
			return 0;
		uint32_t n = 1;
		while ((w / 2) % 4 == 0 && (h / 2) % 4 == 0)
			w /= 2, h /= 2, ++n;
		return n;
	}

	float zone_cut_threshold(uint8_t vcol_a)
	{
		const int v = vcol_a != 0 ? vcol_a : 0x80;
		return 96.0f * 255.0f / (4.0f * float(v));
	}

	double coverage(const uint8_t *px, uint32_t w, uint32_t h, float threshold)
	{
		const size_t n = size_t(w) * h;
		size_t pass = 0;
		for (size_t i = 0; i < n; ++i)
			pass += float(px[i * 4 + 3]) > threshold;
		return n ? double(pass) / double(n) : 0.0;
	}

	std::vector<std::vector<uint8_t>> full_chain(const uint8_t *level0, uint32_t w, uint32_t h, uint8_t alpha_kind, float cut_threshold)
	{
		std::vector<std::vector<uint8_t>> out;
		const uint32_t levels = chain_levels(w, h);
		if (levels == 0 || level0 == nullptr)
			return out;
		out.emplace_back(level0, level0 + size_t(w / 4) * (h / 4) * 16);
		const bool cut = alpha_kind == kAlphaCut;
		std::vector<uint8_t> px = decode_bc2(level0, w, h);
		const double want = cut ? coverage(px.data(), w, h, cut_threshold) : 0.0;
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
			if (!cut)
			{
				out.push_back(encode_bc2(px.data(), w, h));
				continue;
			}
			// (the scale: the smallest whose coverage reaches level 0's, or the largest below it, whichever is nearer;
			// 1 when it's as near: no change for nothing)
			uint64_t hist[256] = {};
			const size_t n = size_t(w) * h;
			for (size_t i = 0; i < n; ++i)
				hist[px[i * 4 + 3]]++;
			double lo = 0.0, hi = 256.0;
			for (int step = 0; step < 32; ++step)
			{
				const double mid = (lo + hi) / 2;
				(scaled_coverage(hist, n, mid, cut_threshold) >= want ? hi : lo) = mid;
			}
			double best = 1.0, best_d = std::fabs(scaled_coverage(hist, n, 1.0, cut_threshold) - want);
			for (double s : { hi, lo })
			{
				const double d = std::fabs(scaled_coverage(hist, n, s, cut_threshold) - want);
				if (d < best_d)
					best = s, best_d = d;
			}
			std::vector<uint8_t> scaled = px;
			if (best != 1.0)
			{
				uint8_t lut[256];
				for (int a = 0; a < 256; ++a)
					lut[a] = uint8_t(std::min(255, int(a * best + 0.5)));
				for (size_t i = 0; i < n; ++i)
					scaled[i * 4 + 3] = lut[scaled[i * 4 + 3]];
			}
			out.push_back(encode_bc2(scaled.data(), w, h));
		}
		return out;
	}
}
