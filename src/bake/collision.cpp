/*
 * A zone's collision (collision.hpp): reshade-worldfx's tools/zone_collision.py, in C++, plus zone_weather.py's
 * weather folders and the Python's float sums.
 *
 * The MZB's layout and terrain bits: the kuluu-ffxi project's ffxi-dat/src/mzb.rs (GPL-3.0) describes them; the
 * Python was written from that description, and this from the Python.
 */

#include "collision.hpp"
#include "dat.hpp"
#include <algorithm>
#include <cmath>

namespace hifi::bake::collision
{
	using namespace detail;

	namespace
	{
		// zone_collision.decrypt_mzb: runs of 16-23 bytes flipped as the key table says (from byte 8, in MZBs of version
		// 27 on), then the object list's names (each 0x64-byte record's first 16 bytes) xor 0x55
		void decrypt_mzb(std::vector<uint8_t> &m, const uint8_t *table)
		{
			const uint32_t h0 = u32(m.data(), 0), h1 = u32(m.data(), 4);
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
						const size_t end = std::min<size_t>(8 + size_t(pos) + piece, m.size());
						for (size_t i = 8 + size_t(pos); i < end; ++i)
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
		}

		// A DAT's chunks walked with their folder path (zone_weather.walk: type 1 opens a folder, type 0 closes one)
		template <typename F>
		void walk(const std::vector<uint8_t> &dat, F &&f)
		{
			std::vector<std::string> path;
			size_t off = 0;
			while (off + 16 <= dat.size())
			{
				const uint32_t w = u32(dat.data(), off + 4);
				const size_t size = size_t((w >> 7) & 0x7FFFF) * 16, kind = w & 0x7F;
				const std::string name(reinterpret_cast<const char *>(dat.data() + off), 4);
				if (kind == 1)
					path.push_back(name);
				else if (kind == 0)
				{
					if (!path.empty())
						path.pop_back();
				}
				else
					f(path, uint32_t(kind), name);
				if (size < 16)
					break;
				off += size;
			}
		}
	}

	bool read_zone(uint32_t zone, std::vector<uint8_t> &dat, result &r)
	{
		std::string why;
		const std::wstring path = zone_dat_path(zone, why);
		if (path.empty())
		{
			r.none = true;
			r.why = why;
			return false;
		}
		if (!read_file(path, dat))
		{
			r.why = "the zone's DAT can't be read";
			return false;
		}
		return true;
	}

	bool open(const std::vector<uint8_t> &dat, reader &rd, result &r)
	{
		rd = reader();
		if (!find_chunk(dat, 0x1C, rd.m))
		{
			r.none = true;
			r.why = "no MZB in the zone DAT";
			return false;
		}
		const uint8_t *first = nullptr, *second = nullptr;
		if (!key_tables(first, second))
		{
			r.why = "the game's key tables weren't found";
			return false;
		}
		std::vector<uint8_t> &m = rd.m;
		const uint64_t size = m.size();
		if (size < 16)
		{
			r.why = "the MZB is cut short";
			return false;
		}
		decrypt_mzb(m, first);
		const uint64_t coll = u32(m.data(), 8);
		if (coll == 0)
		{
			r.none = true;
			r.why = "the zone has no collision";
			return false;
		}
		if (coll + 0x14 > size)
		{
			r.why = "the MZB's collision is cut short";
			return false;
		}
		const uint64_t grid = u32(m.data(), size_t(coll + 0x10));
		const uint64_t cells = uint64_t(m[0xC] * (m[0xE] / 4)) * uint64_t(m[0xD] * (m[0xF] / 4));
		rd.pairs.reserve(65536);
		for (uint64_t c = 0; c < cells; ++c)
		{
			if (grid + c * 4 + 4 > size)
			{
				r.why = "the MZB's collision grid is cut short";
				return false;
			}
			uint64_t k = u32(m.data(), size_t(grid + c * 4));
			if (k == 0)
				continue;
			if (k + 4 > size)
			{
				rd.skipped++; // (not an offset: some grids hold a few of these)
				continue;
			}
			k += 4; // (the list's first entry isn't a pair)
			while (k + 8 <= size)
			{
				const uint32_t a = u32(m.data(), size_t(k));
				if (a == 0)
					break;
				rd.pairs.push_back(uint64_t(a) << 32 | u32(m.data(), size_t(k + 4)));
				k += 8;
			}
		}
		std::sort(rd.pairs.begin(), rd.pairs.end());
		rd.pairs.erase(std::unique(rd.pairs.begin(), rd.pairs.end()), rd.pairs.end());
		rd.pairs.shrink_to_fit();
		return true;
	}

	bool triangles(const std::vector<uint8_t> &dat, std::vector<triangle> &out, result &r)
	{
		out.clear();
		reader rd;
		if (!open(dat, rd, r))
			return false;
		each(rd, [&](const triangle &t) { out.push_back(t); });
		return true;
	}

	std::set<std::string> weathers(const std::vector<uint8_t> &dat)
	{
		std::set<std::string> out;
		walk(dat, [&](const std::vector<std::string> &path, uint32_t kind, const std::string &) {
			if (kind == 0x2F && path.size() >= 2 && path[path.size() - 2] == "weat")
				out.insert(path.back());
		});
		return out;
	}

	bool rainy(const std::vector<uint8_t> &dat, bool snow_too)
	{
		static const char *const kRainy[] = { "rain", "squl", "thdr", "bolt", "rai1", "squ1", "thd1", "bol1" };
		static const char *const kSnowy[] = { "snow", "bliz", "sno1", "bli1" };
		const std::set<std::string> names = weathers(dat);
		for (const char *n : kRainy)
			if (names.count(n) != 0)
				return true;
		if (snow_too)
			for (const char *n : kSnowy)
				if (names.count(n) != 0)
					return true;
		return false;
	}

	double python_sum(const double *v, size_t n)
	{
		if (n == 0)
			return 0.0;
		// (0 + the first: an int and a float make a float; then the rest with the compensation)
		double f = 0.0 + v[0], c = 0.0;
		for (size_t i = 1; i < n; ++i)
		{
			const double x = v[i], t = f + x;
			if (std::fabs(f) >= std::fabs(x))
				c += (f - t) + x;
			else
				c += (x - t) + f;
			f = t;
		}
		// (the sign of a negative result kept, and an infinite or overflowed sum not made a NaN)
		if (c != 0.0 && std::isfinite(c))
			f += c;
		return f;
	}

	double numpy_sum(const double *v, size_t n)
	{
		return numpy_sum(n, [v](size_t i) { return v[i]; });
	}
}
