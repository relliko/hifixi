/*
 * Grass (bake.hpp): reshade-worldfx's tools/bake_grass.py, in C++. Blade positions for one zone, from its own
 * collision (collision.hpp, the game's DAT): blades are scattered over the triangles whose terrain is grass and that
 * face up, at the ground's real height, and kept a yalm off path triangles (the collision's paths are narrower than
 * the dirt drawn on them). The randomness is Python's (random.Random(1): MT19937 seeded by init_by_array, random()'s
 * 53-bit float), drawn in the Python's order, so the blades are the same ones.
 *
 * Output (little-endian), read by World FX's grass:
 *     char  magic[4] 'GRS2'
 *     u32   zone
 *     f32   cell size (yalms)
 *     u32   cell count
 *     u32   blade count
 *     cells:  i32 cx, i32 cz, u32 first blade, u32 blade count   (sorted by cz, then cx)
 *     blades: f32 x, y, z, seed (0..1)                          (grouped by cell)
 */

#include "bake.hpp"
#include "collision.hpp"
#include "dat.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <unordered_map>
#include <vector>

namespace hifi::bake
{
	namespace
	{
		constexpr double kCell = 16.0;
		constexpr double kDensity = 3.0;   // (blades per square yalm)
		constexpr double kPathMargin = 1.0; // (yalms kept bare around paths)
		constexpr double kPathHeight = 2.0; // (...within this above or below them)

		// CPython's random.Random: MT19937 (genrand_uint32), seeded from an int by init_by_array over its 32-bit words
		class python_random
		{
		public:
			explicit python_random(uint32_t seed)
			{
				const uint32_t key[1] = { seed };
				init_by_array(key, 1);
			}

			// random(): 53 bits from two draws, (a * 2^26 + b) / 2^53
			double random()
			{
				const uint32_t a = next() >> 5, b = next() >> 6;
				return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
			}

		private:
			static constexpr int N = 624, M = 397;
			uint32_t mt[N];
			int mti = N + 1;

			void init_genrand(uint32_t s)
			{
				mt[0] = s;
				for (mti = 1; mti < N; mti++)
					mt[mti] = 1812433253u * (mt[mti - 1] ^ (mt[mti - 1] >> 30)) + uint32_t(mti);
			}

			void init_by_array(const uint32_t *key, size_t length)
			{
				init_genrand(19650218u);
				size_t i = 1, j = 0;
				for (size_t k = N > length ? N : length; k; k--)
				{
					mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525u)) + key[j] + uint32_t(j); // (non linear)
					i++;
					j++;
					if (i >= N)
					{
						mt[0] = mt[N - 1];
						i = 1;
					}
					if (j >= length)
						j = 0;
				}
				for (size_t k = N - 1; k; k--)
				{
					mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941u)) - uint32_t(i); // (non linear)
					i++;
					if (i >= N)
					{
						mt[0] = mt[N - 1];
						i = 1;
					}
				}
				mt[0] = 0x80000000u; // (MSB is 1, assuring a non-zero initial array)
			}

			uint32_t next()
			{
				static const uint32_t mag01[2] = { 0u, 0x9908B0DFu };
				uint32_t y;
				if (mti >= N)
				{
					int kk;
					for (kk = 0; kk < N - M; kk++)
					{
						y = (mt[kk] & 0x80000000u) | (mt[kk + 1] & 0x7FFFFFFFu);
						mt[kk] = mt[kk + M] ^ (y >> 1) ^ mag01[y & 1u];
					}
					for (; kk < N - 1; kk++)
					{
						y = (mt[kk] & 0x80000000u) | (mt[kk + 1] & 0x7FFFFFFFu);
						mt[kk] = mt[kk + (M - N)] ^ (y >> 1) ^ mag01[y & 1u];
					}
					y = (mt[N - 1] & 0x80000000u) | (mt[0] & 0x7FFFFFFFu);
					mt[N - 1] = mt[M - 1] ^ (y >> 1) ^ mag01[y & 1u];
					mti = 0;
				}
				y = mt[mti++];
				y ^= (y >> 11);
				y ^= (y << 7) & 0x9D2C5680u;
				y ^= (y << 15) & 0xEFC60000u;
				y ^= (y >> 18);
				return y;
			}
		};

		struct corners
		{
			double p[3][3];
		};

		double area(const corners &t)
		{
			const double *a = t.p[0], *b = t.p[1], *c = t.p[2];
			const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
			const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
			const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
			return 0.5 * std::sqrt(nx * nx + ny * ny + nz * nz);
		}

		// How much a triangle faces up or down (|normal.y|, 1 flat, 0 a wall)
		double facing_up(const double *a, const double *b, const double *c)
		{
			const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
			const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
			const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
			const double n = std::sqrt(nx * nx + ny * ny + nz * nz);
			return n > 0 ? std::fabs(ny) / n : 0.0;
		}

		double segment_distance2(double px, double pz, double ax, double az, double bx, double bz)
		{
			const double dx = bx - ax, dz = bz - az;
			const double l2 = dx * dx + dz * dz;
			double t = 0.0;
			if (!(l2 == 0))
			{
				// (Python's max(0.0, min(1.0, v)): a NaN comes out 1.0)
				const double v = ((px - ax) * dx + (pz - az) * dz) / l2;
				const double lo = v < 1.0 ? v : 1.0;
				t = lo > 0.0 ? lo : 0.0;
			}
			const double ex = ax + t * dx - px, ez = az + t * dz - pz;
			return ex * ex + ez * ez;
		}

		int64_t floor_int(double v)
		{
			return int64_t(std::floor(v));
		}

		uint64_t key_of(int64_t a, int64_t b)
		{
			return uint64_t(uint32_t(int32_t(a))) << 32 | uint32_t(int32_t(b));
		}

		// Triangles by 4-yalm squares across the ground (x, z), for: is a point within r of one?
		class across_ground
		{
		public:
			across_ground(const std::vector<corners> &tris, double r) : tris_(tris), r_(r)
			{
				for (uint32_t k = 0; k < tris.size(); ++k)
				{
					const corners &t = tris[k];
					const double minx = std::min({ t.p[0][0], t.p[1][0], t.p[2][0] }), maxx = std::max({ t.p[0][0], t.p[1][0], t.p[2][0] });
					const double minz = std::min({ t.p[0][2], t.p[1][2], t.p[2][2] }), maxz = std::max({ t.p[0][2], t.p[1][2], t.p[2][2] });
					for (int64_t i = floor_int((minx - r) / 4), i1 = floor_int((maxx + r) / 4); i <= i1; ++i)
						for (int64_t j = floor_int((minz - r) / 4), j1 = floor_int((maxz + r) / 4); j <= j1; ++j)
							cells_[key_of(i, j)].push_back(k);
				}
			}

			// Within r across the ground of a triangle, and within height above or below it
			bool near(double x, double y, double z, double height) const
			{
				const double r2 = r_ * r_;
				const auto it = cells_.find(key_of(floor_int(x / 4), floor_int(z / 4)));
				if (it == cells_.end())
					return false;
				for (const uint32_t k : it->second)
				{
					const double *a = tris_[k].p[0], *b = tris_[k].p[1], *c = tris_[k].p[2];
					if (std::min({ a[1], b[1], c[1] }) - height > y || std::max({ a[1], b[1], c[1] }) + height < y)
						continue;
					// inside (same side of all three edges), or near an edge
					const double d1 = (x - b[0]) * (a[2] - b[2]) - (a[0] - b[0]) * (z - b[2]);
					const double d2 = (x - c[0]) * (b[2] - c[2]) - (b[0] - c[0]) * (z - c[2]);
					const double d3 = (x - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (z - a[2]);
					if (!((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0)))
						return true;
					if (segment_distance2(x, z, a[0], a[2], b[0], b[2]) <= r2 || segment_distance2(x, z, b[0], b[2], c[0], c[2]) <= r2 ||
						segment_distance2(x, z, c[0], c[2], a[0], a[2]) <= r2)
						return true;
				}
				return false;
			}

		private:
			const std::vector<corners> &tris_;
			double r_;
			std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
		};

		void put32(std::vector<uint8_t> &out, size_t at, uint32_t v)
		{
			std::memcpy(out.data() + at, &v, 4);
		}
		void putf(std::vector<uint8_t> &out, size_t at, double v)
		{
			const float f = float(v);
			std::memcpy(out.data() + at, &f, 4);
		}
	}

	namespace
	{
		bool bake_grass(uint32_t zone, const std::wstring &out, result &r)
		{
			std::vector<corners> grass, paths;
			{
				std::vector<uint8_t> dat;
				collision::reader rd;
				if (!collision::read_zone(zone, dat, r) || !collision::open(dat, rd, r))
					return false;
				dat = std::vector<uint8_t>();
				collision::each(rd, [&](const collision::triangle &t) {
					corners c;
					std::memcpy(c.p, t.p, sizeof(c.p));
					if (t.terrain == collision::kGrass && facing_up(t.p[0], t.p[1], t.p[2]) > 0.5)
						grass.push_back(c);
					if (t.terrain == collision::kPath)
						paths.push_back(c);
				});
			}
			// (a path corner that isn't finite: the Python's math.floor raises, and it bakes nothing)
			for (const corners &t : paths)
				for (const auto &p : t.p)
					if (!std::isfinite(p[0]) || !std::isfinite(p[2]))
					{
						r.why = "a path triangle's corner isn't finite";
						return false;
					}

			// The blades over the grass triangles, in the Python's order, each handed to emit(cz, cx, x, y, z, seed) with
			// its cell. (Scattered twice: to count each cell's, then into the file in place, so the blades are never held
			// beside it: a field's are 50 MB.)
			const across_ground *bare = nullptr;
			across_ground bare_paths(paths, kPathMargin);
			if (kPathMargin > 0 && !paths.empty())
				bare = &bare_paths;
			auto scatter = [&](auto &&emit) {
				python_random rng(1);
				for (const corners &t : grass)
				{
					const double *a = t.p[0], *b = t.p[1], *c = t.p[2];
					const double s = area(t);
					const double want = s * kDensity;
					const double whole = std::trunc(want);
					const int64_t n = int64_t(whole) + (rng.random() < want - whole ? 1 : 0);
					for (int64_t k = 0; k < n; ++k)
					{
						double u = rng.random(), v = rng.random();
						if (u + v > 1)
						{
							u = 1 - u;
							v = 1 - v;
						}
						const double x = a[0] + u * (b[0] - a[0]) + v * (c[0] - a[0]);
						const double y = a[1] + u * (b[1] - a[1]) + v * (c[1] - a[1]);
						const double z = a[2] + u * (b[2] - a[2]) + v * (c[2] - a[2]);
						const double seed = rng.random();
						if (bare != nullptr && bare->near(x, y, z, kPathHeight))
							continue;
						emit(floor_int(z / kCell), floor_int(x / kCell), x, y, z, seed);
					}
				}
			};

			// Each cell's blades counted
			std::unordered_map<uint64_t, uint32_t> cell_ids;
			std::vector<std::pair<int64_t, int64_t>> cells; // (cz, cx), by id
			std::vector<uint32_t> count;
			size_t blades = 0;
			scatter([&](int64_t cz, int64_t cx, double, double, double, double) {
				const auto [it, added] = cell_ids.try_emplace(key_of(cz, cx), uint32_t(cells.size()));
				if (added)
				{
					cells.emplace_back(cz, cx);
					count.push_back(0);
				}
				count[it->second]++;
				blades++;
			});
			if (blades == 0)
			{
				r.none = true;
				r.why = "no grass";
				return false;
			}

			// Cells sorted by cz, then cx; then the blades again, each into its cell's place, in the order they were made
			std::vector<uint32_t> order(cells.size());
			for (uint32_t i = 0; i < order.size(); ++i)
				order[i] = i;
			std::sort(order.begin(), order.end(), [&](uint32_t p, uint32_t q) { return cells[p] < cells[q]; });
			std::vector<uint32_t> next(cells.size(), 0);
			const size_t head = 20, table = head + 16 * cells.size();
			std::vector<uint8_t> file(table + 16 * blades);
			std::memcpy(file.data(), "GRS2", 4);
			put32(file, 4, zone);
			putf(file, 8, kCell);
			put32(file, 12, uint32_t(cells.size()));
			put32(file, 16, uint32_t(blades));
			uint32_t at = 0;
			for (size_t i = 0; i < order.size(); ++i)
			{
				const uint32_t c = order[i];
				next[c] = at;
				put32(file, head + 16 * i, uint32_t(int32_t(cells[c].second)));
				put32(file, head + 16 * i + 4, uint32_t(int32_t(cells[c].first)));
				put32(file, head + 16 * i + 8, at);
				put32(file, head + 16 * i + 12, count[c]);
				at += count[c];
			}
			scatter([&](int64_t cz, int64_t cx, double x, double y, double z, double seed) {
				const float v[4] = { float(x), float(y), float(z), float(seed) };
				std::memcpy(file.data() + table + 16 * size_t(next[cell_ids[key_of(cz, cx)]]++), v, 16);
			});
			grass = std::vector<corners>();
			if (!write_file(out, file.data(), file.size()))
			{
				r.why = "the file couldn't be written";
				return false;
			}
			return true;
		}
	}

	bool grass(uint32_t zone, const std::wstring &out, result &r)
	{
		try
		{
			return bake_grass(zone, out, r);
		}
		catch (const std::bad_alloc &)
		{
			r = result();
			r.why = "out of memory";
			return false;
		}
	}
}
