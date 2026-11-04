// BC7 colour for hifixi-texpack make (bc7.hpp)

#include "bc7.hpp"
#include "../../src/pack/bc2.hpp"
#include "../../src/pack/pack.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hifi::texpack::bc7
{
	// (measured 2026-11-04 on 61 model textures of the trial and 10 of the tests, docs/texture-pack/upscaler.md "BC7":
	// texconv's GPU encoder with the alpha error weighted 4 times: 0.6 dB of colour below its default (still 6 dB over
	// BC2) for a worst alpha error of 64 instead of 110 and a fifth fewer alpha tests flipped; -bc x gives 0.1 dB for
	// a third more time, the CPU encoder 0.4 dB for 130 times the time)
	const wchar_t *kTexconvOptions = L"-aw 4";

	namespace
	{
		struct mode_info
		{
			uint8_t subsets, partition_bits, rotation_bits, select_bits, colour_bits, alpha_bits, endpoint_pbits, shared_pbits, index_bits, index2_bits;
		};
		constexpr mode_info kModes[8] = {
			{ 3, 4, 0, 0, 4, 0, 1, 0, 3, 0 },
			{ 2, 6, 0, 0, 6, 0, 0, 1, 3, 0 },
			{ 3, 6, 0, 0, 5, 0, 0, 0, 2, 0 },
			{ 2, 6, 0, 0, 7, 0, 1, 0, 2, 0 },
			{ 1, 0, 2, 1, 5, 6, 0, 0, 2, 3 },
			{ 1, 0, 2, 0, 7, 8, 0, 0, 2, 2 },
			{ 1, 0, 0, 0, 7, 7, 1, 0, 4, 0 },
			{ 2, 6, 0, 0, 5, 5, 1, 0, 2, 0 },
		};

		// (bit i: texel i's subset; read from texconv's decoder)
		constexpr uint16_t kPartition2[64] = { 0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8,
			0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0,
			0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966,
			0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22 };

		constexpr uint8_t kPartition3[64][16] = {
			{ 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2 }, { 0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1 },
			{ 0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1 }, { 0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1 },
			{ 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2 }, { 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2 },
			{ 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1 }, { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1 },
			{ 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2 }, { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2 },
			{ 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2 }, { 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2 },
			{ 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2 }, { 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2 },
			{ 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2 }, { 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0 },
			{ 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2 }, { 0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0 },
			{ 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2 }, { 0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1 },
			{ 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2 }, { 0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1 },
			{ 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2 }, { 0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0 },
			{ 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0 }, { 0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2 },
			{ 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0 }, { 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1 },
			{ 0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2 }, { 0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2 },
			{ 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1 }, { 0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1 },
			{ 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2 }, { 0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1 },
			{ 0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2 }, { 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0 },
			{ 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0 }, { 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0 },
			{ 0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0 }, { 0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1 },
			{ 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1 }, { 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2 },
			{ 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1 }, { 0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2 },
			{ 0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1 }, { 0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1 },
			{ 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1 }, { 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1 },
			{ 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2 }, { 0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1 },
			{ 0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2 }, { 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2 },
			{ 0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2 }, { 0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2 },
			{ 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2 }, { 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2 },
			{ 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2 }, { 0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2 },
			{ 0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2 }, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2 },
			{ 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1 }, { 0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2 },
			{ 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 }, { 0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0 },
		};

		// The anchor texels (their index one bit shorter): subset 1 of the 2-subset partitions, subsets 1 and 2 of the 3
		constexpr uint8_t kAnchor2[64] = { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
			15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15 };
		constexpr uint8_t kAnchor3a[64] = { 3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15, 8, 15, 3,
			5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3 };
		constexpr uint8_t kAnchor3b[64] = { 15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10,
			8, 15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8 };

		constexpr uint8_t kWeights2[4] = { 0, 21, 43, 64 };
		constexpr uint8_t kWeights3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
		constexpr uint8_t kWeights4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

		struct bits
		{
			const uint8_t *p;
			uint32_t at = 0;
			uint32_t get(uint32_t n)
			{
				uint32_t v = 0;
				for (uint32_t i = 0; i < n; ++i, ++at)
					v |= uint32_t((p[at >> 3] >> (at & 7)) & 1) << i;
				return v;
			}
		};

		inline int expand(uint32_t v, uint32_t n) { return n >= 8 ? int(v) : int((v << (8 - n)) | (v >> (2 * n - 8))); }
		inline const uint8_t *weights(uint32_t n) { return n == 2 ? kWeights2 : n == 3 ? kWeights3 : kWeights4; }
	}

	void decode_block(const uint8_t *block, uint8_t out[16][4])
	{
		uint32_t mode = 0;
		while (mode < 8 && ((block[0] >> mode) & 1) == 0)
			++mode;
		if (mode >= 8)
		{
			std::memset(out, 0, 64);
			return;
		}
		const mode_info &m = kModes[mode];
		bits b { block, mode + 1 };
		const uint32_t partition = b.get(m.partition_bits), rotation = b.get(m.rotation_bits), select = b.get(m.select_bits);
		uint32_t ep[3][2][4] = {};
		for (int c = 0; c < 3; ++c)
			for (uint32_t s = 0; s < m.subsets; ++s)
				for (int e = 0; e < 2; ++e)
					ep[s][e][c] = b.get(m.colour_bits);
		if (m.alpha_bits)
			for (uint32_t s = 0; s < m.subsets; ++s)
				for (int e = 0; e < 2; ++e)
					ep[s][e][3] = b.get(m.alpha_bits);
		uint32_t cbits = m.colour_bits, abits = m.alpha_bits;
		if (m.endpoint_pbits || m.shared_pbits)
		{
			uint32_t p[3][2] = {};
			for (uint32_t s = 0; s < m.subsets; ++s)
				if (m.endpoint_pbits)
					p[s][0] = b.get(1), p[s][1] = b.get(1);
				else
					p[s][0] = p[s][1] = b.get(1);
			for (uint32_t s = 0; s < m.subsets; ++s)
				for (int e = 0; e < 2; ++e)
					for (int c = 0; c < (m.alpha_bits ? 4 : 3); ++c)
						ep[s][e][c] = (ep[s][e][c] << 1) | p[s][e];
			cbits++;
			if (abits)
				abits++;
		}
		int col[3][2][4];
		for (uint32_t s = 0; s < m.subsets; ++s)
			for (int e = 0; e < 2; ++e)
			{
				for (int c = 0; c < 3; ++c)
					col[s][e][c] = expand(ep[s][e][c], cbits);
				col[s][e][3] = abits ? expand(ep[s][e][3], abits) : 255;
			}
		auto subset_of = [&](uint32_t i) -> uint32_t {
			if (m.subsets == 2)
				return (kPartition2[partition] >> i) & 1;
			if (m.subsets == 3)
				return kPartition3[partition][i];
			return 0;
		};
		auto anchor = [&](uint32_t i) {
			if (i == 0)
				return true;
			if (m.subsets == 2)
				return i == kAnchor2[partition];
			if (m.subsets == 3)
				return i == kAnchor3a[partition] || i == kAnchor3b[partition];
			return false;
		};
		uint32_t idx[16], idx2[16] = {};
		for (uint32_t i = 0; i < 16; ++i)
			idx[i] = b.get(anchor(i) ? m.index_bits - 1 : m.index_bits);
		if (m.index2_bits)
			for (uint32_t i = 0; i < 16; ++i)
				idx2[i] = b.get(i == 0 ? m.index2_bits - 1 : m.index2_bits);
		for (uint32_t i = 0; i < 16; ++i)
		{
			const uint32_t s = subset_of(i);
			uint32_t ci = idx[i], ai = idx[i], cn = m.index_bits, an = m.index_bits;
			if (m.index2_bits)
			{
				if (select)
					ci = idx2[i], cn = m.index2_bits, ai = idx[i], an = m.index_bits;
				else
					ai = idx2[i], an = m.index2_bits;
			}
			const int wc = weights(cn)[ci], wa = weights(an)[ai];
			int px[4];
			for (int c = 0; c < 3; ++c)
				px[c] = ((64 - wc) * col[s][0][c] + wc * col[s][1][c] + 32) >> 6;
			px[3] = ((64 - wa) * col[s][0][3] + wa * col[s][1][3] + 32) >> 6;
			if (rotation >= 1 && rotation <= 3)
				std::swap(px[3], px[rotation - 1]);
			for (int c = 0; c < 4; ++c)
				out[i][c] = uint8_t(px[c]);
		}
	}

	std::vector<uint8_t> decode(const uint8_t *blocks, uint32_t w, uint32_t h)
	{
		std::vector<uint8_t> px(size_t(w) * h * 4);
		uint8_t t[16][4];
		for (uint32_t by = 0; by < h / 4; ++by)
			for (uint32_t bx = 0; bx < w / 4; ++bx)
			{
				decode_block(blocks + (size_t(by) * (w / 4) + bx) * 16, t);
				for (int k = 0; k < 16; ++k)
					std::memcpy(px.data() + ((size_t(by) * 4 + k / 4) * w + bx * 4 + k % 4) * 4, t[k], 4);
			}
		return px;
	}

	std::vector<uint8_t> decode_level(uint32_t format, const uint8_t *blocks, uint32_t w, uint32_t h)
	{
		if (format == pack::kBC2)
			return pack::bc2::decode_bc2(blocks, w, h);
		if (format == pack::kBC7)
			return decode(blocks, w, h);
		return {};
	}

	std::vector<std::vector<uint8_t>> rgba_chain(std::vector<uint8_t> px, uint32_t w, uint32_t h, uint8_t alpha_kind, float cut_threshold, uint8_t alpha_lo,
		uint8_t alpha_hi)
	{
		std::vector<std::vector<uint8_t>> out;
		const uint32_t levels = pack::bc2::chain_levels(w, h);
		if (levels == 0 || px.size() != size_t(w) * h * 4)
			return out;
		if (alpha_hi < alpha_lo)
			alpha_hi = alpha_lo;
		for (size_t i = 3; i < px.size(); i += 4)
			px[i] = std::clamp(px[i], alpha_lo, alpha_hi);
		out.push_back(px);
		const bool cut = alpha_kind == pack::kAlphaCut;
		const double want = cut ? pack::bc2::coverage(px.data(), w, h, cut_threshold) : 0.0;
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
				out.push_back(px);
				continue;
			}
			// (bc2::full_chain's search, on 8-bit alpha capped at alpha_hi)
			uint64_t hist[256] = {};
			const size_t n = size_t(w) * h;
			for (size_t i = 0; i < n; ++i)
				hist[px[i * 4 + 3]]++;
			auto scaled = [&](double s) {
				uint64_t pass = 0;
				for (int a = 0; a < 256; ++a)
					if (hist[a] != 0 && float(std::min<int>(alpha_hi, int(a * s + 0.5))) > cut_threshold)
						pass += hist[a];
				return n ? double(pass) / double(n) : 0.0;
			};
			double lo = 0.0, hi = 256.0;
			for (int step = 0; step < 32; ++step)
			{
				const double mid = (lo + hi) / 2;
				(scaled(mid) >= want ? hi : lo) = mid;
			}
			double best = 1.0, best_d = std::fabs(scaled(1.0) - want);
			for (double s : { hi, lo })
			{
				const double d = std::fabs(scaled(s) - want);
				if (d < best_d)
					best = s, best_d = d;
			}
			std::vector<uint8_t> level_px = px;
			if (best != 1.0)
			{
				uint8_t lut[256];
				for (int a = 0; a < 256; ++a)
					lut[a] = uint8_t(std::min<int>(std::max<int>(alpha_hi, a), int(a * best + 0.5)));
				for (size_t i = 0; i < n; ++i)
					level_px[i * 4 + 3] = lut[level_px[i * 4 + 3]];
			}
			out.push_back(std::move(level_px));
		}
		return out;
	}

	namespace
	{
		constexpr uint32_t kDdsMagic = 0x20534444, kDx10 = 0x30315844;

		struct dds_header
		{
			uint32_t magic = kDdsMagic, size = 124, flags = 0, height = 0, width = 0, pitch = 0, depth = 0, mips = 0, reserved1[11] = {};
			uint32_t pf_size = 32, pf_flags = 4, pf_fourcc = kDx10, pf_bits = 0, pf_r = 0, pf_g = 0, pf_b = 0, pf_a = 0;
			uint32_t caps = 0, caps2 = 0, caps3 = 0, caps4 = 0, reserved2 = 0;
			uint32_t dxgi = 0, dimension = 3, misc = 0, array = 1, misc2 = 0;
		};
		static_assert(sizeof(dds_header) == 148);
	}

	bool write_rgba_dds(const std::wstring &path, const std::vector<std::vector<uint8_t>> &levels, uint32_t w, uint32_t h, std::string &why)
	{
		dds_header d;
		d.flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000; // caps, height, width, pitch, pixel format, mip count
		d.height = h, d.width = w, d.pitch = w * 4, d.mips = uint32_t(levels.size());
		d.caps = 0x1000 | (levels.size() > 1 ? 0x400000 | 0x8 : 0);
		d.dxgi = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
		HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f == INVALID_HANDLE_VALUE)
		{
			why = "can't be written (error " + std::to_string(GetLastError()) + ")";
			return false;
		}
		DWORD put = 0;
		bool ok = WriteFile(f, &d, sizeof(d), &put, nullptr) && put == sizeof(d);
		for (size_t l = 0; ok && l < levels.size(); ++l)
		{
			const size_t want = size_t(std::max(1u, w >> l)) * std::max(1u, h >> l) * 4;
			ok = levels[l].size() == want && WriteFile(f, levels[l].data(), DWORD(want), &put, nullptr) && put == want;
		}
		CloseHandle(f);
		if (!ok)
			why = "writing failed";
		return ok;
	}

	bool read_bc_dds(const std::wstring &path, uint32_t format, uint32_t w, uint32_t h, uint32_t levels, std::vector<std::vector<uint8_t>> &out,
		std::string &why)
	{
		out.clear();
		HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (f == INVALID_HANDLE_VALUE)
		{
			why = "no file from texconv";
			return false;
		}
		LARGE_INTEGER size = {};
		GetFileSizeEx(f, &size);
		std::vector<uint8_t> bytes(size_t(size.QuadPart));
		DWORD got = 0;
		const bool read = size.QuadPart > 0 && size.QuadPart < (1ll << 31) && ReadFile(f, bytes.data(), DWORD(bytes.size()), &got, nullptr) && got == bytes.size();
		CloseHandle(f);
		if (!read || bytes.size() < 128)
		{
			why = "texconv's file can't be read";
			return false;
		}
		dds_header d;
		std::memcpy(&d, bytes.data(), 128);
		size_t at = 128;
		uint32_t dxgi = 0;
		if (d.pf_fourcc == kDx10 && bytes.size() >= 148)
		{
			std::memcpy(&d, bytes.data(), 148);
			dxgi = d.dxgi;
			at = 148;
		}
		const uint32_t mips = (d.flags & 0x20000) && d.mips ? d.mips : 1;
		char text[200];
		if (d.magic != kDdsMagic || dxgi != format || d.width != w || d.height != h || mips != levels)
		{
			std::snprintf(text, sizeof(text), "texconv wrote %ux%u, format %u, %u levels (wanted %ux%u, format %u, %u levels)", d.width, d.height, dxgi, mips, w, h,
				format, levels);
			why = text;
			return false;
		}
		for (uint32_t l = 0; l < levels; ++l)
		{
			const size_t n = size_t((std::max(1u, w >> l) + 3) / 4) * ((std::max(1u, h >> l) + 3) / 4) * 16;
			if (at + n > bytes.size())
			{
				why = "texconv's file is short";
				out.clear();
				return false;
			}
			out.emplace_back(bytes.begin() + ptrdiff_t(at), bytes.begin() + ptrdiff_t(at + n));
			at += n;
		}
		return true;
	}

	upscale::end compress(const compress_options &o, std::string &why)
	{
		std::wstring cmd = L"\"" + o.texconv + L"\" -nologo -y -f BC7_UNORM -dx10 ";
		if (*kTexconvOptions)
			cmd += std::wstring(kTexconvOptions) + L" ";
		if (!o.extra.empty())
			cmd += o.extra + L" ";
		cmd += L"-o \"" + o.out + L"\" -flist \"" + o.list + L"\"";
		upscale::program_options po;
		po.log = o.log, po.poll_s = o.poll_s, po.stall_s = o.stall_s, po.should_end = o.should_end;
		po.watch = o.out, po.pattern = L"*.dds";
		std::wstring folder = o.texconv;
		folder.resize(folder.find_last_of(L"\\/") == std::wstring::npos ? 0 : folder.find_last_of(L"\\/"));
		return upscale::run_program(o.texconv, cmd, !o.cwd.empty() ? o.cwd : folder.empty() ? o.out : folder, po, why);
	}
}
