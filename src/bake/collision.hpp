#pragma once

// A zone's collision mesh from the game's own files (reshade-worldfx's tools/zone_collision.py, in C++), for the
// bakers that start from it (grass, water, wet, rain): each triangle's corners in the game's world coordinates (x east,
// y height with down positive, z north), as doubles computed as the Python computes them, and its terrain (the
// footstep surface: grass, path, stone...). Also the zone DAT's weather folders (research/ffxi-re's zone_weather.py),
// for the bakers made only where it rains, and CPython's and numpy's ways of adding floats, where a baker's sums must
// come out to the same bits as the Python's.
//
// The zone's DAT is file id 100 + zone (dat.hpp). Its MZB chunk (type 0x1C) holds the collision: a grid of 4-yalm
// cells listing (world matrix, mesh) pairs. The MZB is scrambled; the key table is the game's own (key_tables). A
// triangle is four u16 words (v0, v1, v2, normal): indices in the low 15/14/14/15 bits, and the terrain nibble in
// their top bits (v0 bit 0 .. normal bit 3).

#include "bake.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace hifi::bake::collision
{
	// The terrains (zone_collision.TERRAIN): 0 object, 1 path, 2 grass, 3 sand, 4 snow, 5 stone, 6 metal, 7 wood,
	// 8 shallow water, 9 deep water, 10 unknown
	constexpr uint32_t kPath = 1, kGrass = 2, kStone = 5, kShallow = 8, kDeep = 9;

	struct triangle
	{
		double p[3][3]; // (corners, each x, y, z)
		uint32_t terrain;
	};

	// A zone's DAT read whole (file id 100 + zone). False with why; none set as well when no ROM has it (an id that
	// isn't a zone)
	bool read_zone(uint32_t zone, std::vector<uint8_t> &dat, result &r);

	// The collision of a DAT, ready to be walked: its MZB unscrambled and its (matrix, mesh) pairs, sorted and each
	// once, as the Python's sorted(set)
	struct reader
	{
		std::vector<uint8_t> m;
		std::vector<uint64_t> pairs; // (matrix offset << 32 | mesh offset)
		uint32_t skipped = 0;        // (grid entries that aren't offsets: the Python skips them too)
	};
	// False with why when there's nothing to walk: none set too when the DAT has no MZB or the MZB no collision (the
	// Python's ValueError), not when it can't be read (its struct.error, or the key tables not found)
	bool open(const std::vector<uint8_t> &dat, reader &rd, result &r);

	// zone_collision.triangles: every collision triangle, in the Python's order (the pairs in turn, each mesh's
	// triangles in turn), handed to f (const triangle &) as it's made, so a baker keeps only what it needs; returns how
	// many the Python skips too (a pair or a corner past the MZB's end)
	template <typename F>
	uint32_t each(const reader &rd, F &&f);

	// All of them
	bool triangles(const std::vector<uint8_t> &dat, std::vector<triangle> &out, result &r);

	// zone_weather.weathers' names: the folders under an environment's weat\ that hold keyframes
	std::set<std::string> weathers(const std::vector<uint8_t> &dat);
	// Whether one of them is rain, squalls, thunder or a thunderstorm (bake_wet.py's RAINY); snow_too: or snow or a
	// blizzard (bake_rain.py's)
	bool rainy(const std::vector<uint8_t> &dat, bool snow_too);

	// CPython 3.12's sum() of floats: from the int 0, then Neumaier's compensated sum (for the Python's sum(list))
	double python_sum(const double *v, size_t n);
	// numpy's add.reduce of float64: the identity 0.0 plus its pairwise sum (8 accumulators, blocks of up to 128), for
	// np.sum and np.mean over many; of n terms term(i) (a product made as it's needed, say), or of an array's
	template <typename F>
	double numpy_sum(size_t n, const F &term);
	double numpy_sum(const double *v, size_t n);

	// ---

	namespace detail
	{
		template <typename F>
		double pairwise(size_t lo, size_t n, const F &term)
		{
			if (n < 8)
			{
				double res = -0.0;
				for (size_t i = 0; i < n; ++i)
					res += term(lo + i);
				return res;
			}
			if (n <= 128)
			{
				double r[8];
				for (size_t k = 0; k < 8; ++k)
					r[k] = term(lo + k);
				size_t i = 8;
				for (; i < n - (n % 8); i += 8)
					for (size_t k = 0; k < 8; ++k)
						r[k] += term(lo + i + k);
				double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
				for (; i < n; ++i)
					res += term(lo + i);
				return res;
			}
			size_t n2 = n / 2;
			n2 -= n2 % 8;
			return pairwise(lo, n2, term) + pairwise(lo + n2, n - n2, term);
		}
	}

	template <typename F>
	double numpy_sum(size_t n, const F &term)
	{
		return 0.0 + detail::pairwise(0, n, term);
	}

	namespace detail
	{
		inline uint32_t u32(const uint8_t *b, size_t at)
		{
			uint32_t v;
			std::memcpy(&v, b + at, 4);
			return v;
		}
		inline uint32_t u16(const uint8_t *b, size_t at)
		{
			return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8;
		}
		inline double f32(const uint8_t *b, size_t at)
		{
			float f;
			std::memcpy(&f, b + at, 4);
			return double(f);
		}
	}

	template <typename F>
	uint32_t each(const reader &rd, F &&f)
	{
		using namespace detail;
		const uint8_t *m = rd.m.data();
		const uint64_t size = rd.m.size();
		uint32_t skipped = 0;
		// (each mesh's corners in the world, made once: the Python's cache, a stamp per index instead of a dict)
		std::vector<double> cache(0x8000 * 3);
		std::vector<uint32_t> stamp(0x8000, 0);
		uint32_t pass = 0;
		for (const uint64_t pair : rd.pairs)
		{
			const uint64_t mat = pair >> 32, geo = pair & 0xFFFFFFFFu;
			if (mat + 64 > size || geo + 16 > size)
			{
				skipped++;
				continue;
			}
			double M[16];
			for (int i = 0; i < 16; ++i)
				M[i] = f32(m, size_t(mat + i * 4));
			const uint64_t vo = u32(m, size_t(geo)), io = u32(m, size_t(geo + 8));
			const uint32_t count = u16(m, size_t(geo + 12));
			if (vo >= size || io + uint64_t(count) * 8 > size)
			{
				skipped++;
				continue;
			}
			pass++;
			// (a corner past the MZB's end: the Python's IndexError, that triangle skipped)
			auto world = [&](uint32_t i, const double *&out) -> bool {
				if (stamp[i] != pass)
				{
					if (vo + uint64_t(i) * 12 + 12 > size)
						return false;
					const size_t at = size_t(vo + uint64_t(i) * 12);
					const double x = f32(m, at), y = f32(m, at + 4), z = f32(m, at + 8);
					double *p = &cache[i * 3];
					p[0] = x * M[0] + y * M[4] + z * M[8] + M[12];
					p[1] = x * M[1] + y * M[5] + z * M[9] + M[13];
					p[2] = x * M[2] + y * M[6] + z * M[10] + M[14];
					stamp[i] = pass;
				}
				out = &cache[i * 3];
				return true;
			};
			triangle t;
			for (uint32_t k = 0; k < count; ++k)
			{
				const size_t at = size_t(io + uint64_t(k) * 8);
				const uint32_t w0 = u16(m, at), w1 = u16(m, at + 2), w2 = u16(m, at + 4), w3 = u16(m, at + 6);
				t.terrain = (w0 >> 15) | ((w1 >> 15) << 1) | ((w2 >> 15) << 2) | ((w3 >> 15) << 3);
				const double *a, *b, *c;
				if (!world(w0 & 0x7FFF, a) || !world(w1 & 0x3FFF, b) || !world(w2 & 0x3FFF, c))
				{
					skipped++;
					continue;
				}
				std::memcpy(t.p[0], a, sizeof(t.p[0]));
				std::memcpy(t.p[1], b, sizeof(t.p[1]));
				std::memcpy(t.p[2], c, sizeof(t.p[2]));
				f(static_cast<const triangle &>(t));
			}
		}
		return skipped;
	}
}
