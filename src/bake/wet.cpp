/*
 * Wet ground (bake.hpp): reshade-worldfx's tools/bake_wet.py, in C++. Where a zone's ground is path or stone, for World
 * FX's wet look in rain: a top-down mask of the zone's collision, a bit a 0.5-yalm square, set where the topmost
 * surface facing up there is path (terrain 1) or stone (5).
 *
 * From the zone's own collision (collision.hpp, the game's DAT): its triangles facing up (within 60 degrees of flat)
 * are laid on the grid from above, each square taking the terrain of the highest one over its middle, so a bridge or a
 * ledge over a path keeps the path under it dry, and a rock on the grass is stone. Only zones with a rainy weather of
 * their own get a file (rain, squalls, thunder or thunderstorms: a folder of that name in the zone DAT's weather); the
 * game shows another weather in a zone without one, so elsewhere the ground is never wet.
 *
 * The Python lays the whole grid at once; here it's laid a band of rows at a time (the collision walked again for each),
 * so a big zone's heights never all sit in memory: each square still takes the first of the highest over it, as there.
 *
 * Output (little-endian):
 *     char  magic[4] 'WET1'
 *     u32   zone
 *     f32   square size (yalms)
 *     u32   width (squares across x)
 *     u32   squares set
 *     u32   height (squares across z)
 *     f32   x0, z0 (the world's x and z at the grid's corner)
 *     u32   words a row
 *     rows of u32 words, z first: bit i of word k in row j = the square (32 k + i, j)
 */

#include "bake.hpp"
#include "collision.hpp"
#include "dat.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace hifi::bake
{
	namespace
	{
		constexpr double kSquare = 0.5;
		constexpr double kUp = 0.5000000000000001; // cos(radians(60)): facing up within this of flat
		constexpr double kMargin = 2.0;            // yalms around the path and stone kept in the grid
		constexpr size_t kBandSquares = 1 << 20;   // (squares laid at once: 8 MB of heights)

		bool is_wet(uint32_t terrain)
		{
			return terrain == collision::kPath || terrain == collision::kStone;
		}

		bool facing_up(const collision::triangle &t)
		{
			const double *a = t.p[0], *b = t.p[1], *c = t.p[2];
			const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
			const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
			const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
			const double n = std::sqrt(nx * nx + ny * ny + nz * nz);
			return n > 1e-9 && std::fabs(ny) / n >= kUp;
		}
	}

	namespace
	{
		bool bake_wet(uint32_t zone, const std::wstring &out, result &r, bool any_weather)
		{
			collision::reader rd;
			{
				std::vector<uint8_t> dat;
				if (!collision::read_zone(zone, dat, r))
					return false;
				if (!any_weather && !collision::rainy(dat, false))
				{
					r.none = true;
					r.why = "no rainy weather";
					return false;
				}
				if (!collision::open(dat, rd, r))
					return false;
			}

			// The grid: the path and stone facing up, and kMargin yalms round them
			bool any = false;
			double minx = 0, maxx = 0, minz = 0, maxz = 0;
			collision::each(rd, [&](const collision::triangle &t) {
				if (!is_wet(t.terrain) || !facing_up(t))
					return;
				for (const auto &p : t.p)
				{
					if (!any)
					{
						minx = maxx = p[0];
						minz = maxz = p[2];
						any = true;
						continue;
					}
					// (Python's min and max: the first kept when they're equal)
					if (p[0] < minx)
						minx = p[0];
					if (p[0] > maxx)
						maxx = p[0];
					if (p[2] < minz)
						minz = p[2];
					if (p[2] > maxz)
						maxz = p[2];
				}
			});
			if (!any)
			{
				r.none = true;
				r.why = "no path or stone facing up";
				return false;
			}
			const double x0 = std::floor((minx - kMargin) / kSquare) * kSquare;
			const double z0 = std::floor((minz - kMargin) / kSquare) * kSquare;
			const int64_t width = int64_t(std::ceil((maxx + kMargin - x0) / kSquare));
			const int64_t height = int64_t(std::ceil((maxz + kMargin - z0) / kSquare));
			const size_t words_a_row = size_t((width + 31) / 32);
			const size_t head = 36;
			std::vector<uint8_t> file(head + size_t(height) * words_a_row * 4, 0);

			// Each band of rows: every triangle facing up laid over it, each square taking the highest (y is down, so the
			// least; doubles, so surfaces a hair apart keep their order)
			const int64_t rows = std::max<int64_t>(1, int64_t(kBandSquares) / width);
			std::vector<double> top;
			std::vector<uint8_t> top_wet;
			uint32_t count = 0;
			for (int64_t r0 = 0; r0 < height; r0 += rows)
			{
				const int64_t r1 = std::min(height, r0 + rows);
				top.assign(size_t((r1 - r0) * width), std::numeric_limits<double>::infinity());
				top_wet.assign(top.size(), 0);
				collision::each(rd, [&](const collision::triangle &t) {
					if (!facing_up(t))
						return;
					const double *a = t.p[0], *b = t.p[1], *c = t.p[2];
					const double ax = a[0], az = a[2], bx = b[0], bz = b[2], cx = c[0], cz = c[2];
					const int64_t i0 = std::max<int64_t>(int64_t(std::floor((std::min({ ax, bx, cx }) - x0) / kSquare)), 0);
					const int64_t i1 = std::min<int64_t>(int64_t(std::ceil((std::max({ ax, bx, cx }) - x0) / kSquare)), width - 1);
					const int64_t j0 = std::max<int64_t>(int64_t(std::floor((std::min({ az, bz, cz }) - z0) / kSquare)), 0);
					const int64_t j1 = std::min<int64_t>(int64_t(std::ceil((std::max({ az, bz, cz }) - z0) / kSquare)), height - 1);
					if (i1 < i0 || j1 < j0)
						return;
					const double d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
					if (std::fabs(d) < 1e-12)
						return;
					const int64_t jj0 = std::max(j0, r0), jj1 = std::min(j1, r1 - 1);
					const double e1 = bz - cz, e2 = cx - bx, e3 = cz - az, e4 = ax - cx;
					const uint8_t wet_here = is_wet(t.terrain) ? 1 : 0;
					for (int64_t j = jj0; j <= jj1; ++j)
					{
						const double pz = z0 + (double(j) + 0.5) * kSquare;
						double *row = &top[size_t((j - r0) * width)];
						uint8_t *row_wet = &top_wet[size_t((j - r0) * width)];
						for (int64_t i = i0; i <= i1; ++i)
						{
							const double px = x0 + (double(i) + 0.5) * kSquare;
							const double l1 = (e1 * (px - cx) + e2 * (pz - cz)) / d;
							const double l2 = (e3 * (px - cx) + e4 * (pz - cz)) / d;
							const double l3 = 1.0 - l1 - l2;
							if (!(l1 >= -1e-6 && l2 >= -1e-6 && l3 >= -1e-6))
								continue;
							const double y = l1 * a[1] + l2 * b[1] + l3 * c[1];
							if (y < row[i])
							{
								row[i] = y;
								row_wet[i] = wet_here;
							}
						}
					}
				});
				// (bit i of a word: square 32 k + i)
				for (int64_t j = r0; j < r1; ++j)
				{
					const uint8_t *row_wet = &top_wet[size_t((j - r0) * width)];
					uint8_t *words = &file[head + size_t(j) * words_a_row * 4];
					for (int64_t i = 0; i < width; ++i)
						if (row_wet[i])
						{
							words[i >> 3] |= uint8_t(1u << (i & 7));
							count++;
						}
				}
			}

			std::memcpy(file.data(), "WET1", 4);
			const float square = float(kSquare), fx0 = float(x0), fz0 = float(z0);
			const uint32_t w = uint32_t(width), h = uint32_t(height), wr = uint32_t(words_a_row);
			std::memcpy(&file[4], &zone, 4);
			std::memcpy(&file[8], &square, 4);
			std::memcpy(&file[12], &w, 4);
			std::memcpy(&file[16], &count, 4);
			std::memcpy(&file[20], &h, 4);
			std::memcpy(&file[24], &fx0, 4);
			std::memcpy(&file[28], &fz0, 4);
			std::memcpy(&file[32], &wr, 4);
			if (!write_file(out, file.data(), file.size()))
			{
				r.why = "the file couldn't be written";
				return false;
			}
			return true;
		}
	}

	bool wet(uint32_t zone, const std::wstring &out, result &r, bool any_weather)
	{
		try
		{
			return bake_wet(zone, out, r, any_weather);
		}
		catch (const std::bad_alloc &)
		{
			r = result();
			r.why = "out of memory";
			return false;
		}
	}
}
