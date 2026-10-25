/*
 * The game's sea sheets (bake.hpp: sea): reshade-worldfx's tools/bake_sea.py in C++, the same bytes. The game's own
 * sea sheets that carry on past the water World FX draws, each placed in the world and moved to World FX's water level,
 * for World FX to draw its water over them too (only where its baked water isn't): in Bibiki Bay (zone 4) World FX's
 * water stops at a straight edge where the collision does, while the game's sea goes on.
 *
 * The sheets: every effect generator (DAT chunk type 0x05) not under the weather or footstep folders (weat, fses,
 * fser), nor under a folder whose name starts with 'kumo' (cloud: Beaucedine Glacier's kum0-kum2 lie flat beside its
 * water) or 'kiri' (fog), whose model command names a model. The name is an MMB's (type 0x2E) when the DAT has one of that name, else a
 * D3M particle model's (type 0x1F); of several same-named chunks of that kind, the one in the generator's own folder,
 * else in the nearest folder over it holding one, else the first in the DAT. An MMB's triangles are its strips' (as
 * shadow.cpp reads them); a D3M's (offsets from the chunk's start, its 16-byte header included):
 *     +0x10 u16 6 (version), u16 0, u8 groups g, u8 flags, u16 triangles n, u16 count[2], u16 0x65 or 0
 *     +0x1E g texture names (16 bytes each) when flags is 0; none when flags is 1 (g 0: one run of n triangles)
 *     then 3 n vertices of 36 bytes (float3 position, float3 normal, u32 colour, float2 uv): a triangle list
 *     then zeros to the chunk's size: exactly (start + 108 n) rounded up to 16
 * Placed as the generator says: row vectors, v' = (v x scale) Rx Ry Rz + position. (sheets::placed, sheets.hpp: water.cpp
 * reads the game's sea sheets with it too.)
 *
 * Kept (yalms; y down positive): flat (its corners' heights spread less than 0.5); at least 3 of its corners on World
 * FX's water (one of the WTR2 file's triangles, steep ones left out, within 1 yalm across, its surface within 5 of the
 * corner's height: the nearest such, a tie the smaller offset), the median of those corners' offsets (World FX's
 * surface y - the corner's) at most 1.5 either way; and going on past the zone: at least 3 and at least 10% of its
 * corners neither on World FX's water nor over or under any collision triangle (steep ones too: the collision as
 * collision.hpp reads it, read only when a sheet gets that far). That keeps the open sea beyond the collision and drops
 * the rivers, ponds, foam and effect sheets inside the zone, which World FX's water covers already. A kept sheet is
 * moved by that median.
 *
 * The arithmetic is the Python's, step for step, in doubles: each sum and product rounded on its own (no fused
 * multiply-adds: the compiler makes none at /fp:precise, and the 32-bit build has no FMA instructions), in the order
 * the Python writes them; the median of the offsets sorted stably, in the corners' order, as Python's sorted(). The one
 * thing not the same: cos and sin are the 32-bit CRT's here, the 64-bit UCRT's there (an ulp apart for a few angles;
 * most sheets aren't rotated at all, where both give 1 and 0 exactly).
 *
 * Output ('SEA1', little-endian), the WTR2 corner layout:
 *     char magic[4] 'SEA1', u32 zone, u32 sheet count, u32 corner count (three a triangle)
 *     corners: f32 x, bed y (surface y + 30), z, kind (200: a game sea sheet)
 *              f32 surface y, edge distance (8), flow x (0), flow z (0)
 *     (the kept sheets in DAT order, each one's triangles in its own order)
 * A zone with no sheet kept (or no water file) gets no file: `none`.
 */

#include <windows.h>
#include "bake.hpp"
#include "collision.hpp"
#include "dat.hpp"
#include "sheets.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace hifi::bake
{
	namespace
	{
		constexpr double kFlat = 0.5;      // a sheet's corners' heights spread less than this
		constexpr double kReach = 1.0;     // on World FX's water: one of its triangles within this across...
		constexpr double kWindow = 5.0;    // ...its surface within this of the corner's height
		constexpr double kMaxOffset = 1.5; // the median offset at most this either way
		constexpr size_t kMinOn = 3;       // at least this many corners on World FX's water
		constexpr size_t kOutShare = 10;   // at least 1 in this many (10%) of the corners out past the zone...
		constexpr size_t kMinOut = 3;      // ...and at least this many
		constexpr double kGroundCell = 4.0; // the collision triangles' lookup grid (the Python's: each in the cells its
		constexpr double kGroundPad = 1.0;  // box + kGroundPad touches, kept only for the cells asked about)
		constexpr double kCell = 4.0;      // the water triangles' lookup grid (the Python's: each triangle in the cells
		constexpr double kPad = 2.0;       // its box + kPad touches, in the file's order)
		constexpr double kKind = 200.0;
		constexpr double kBed = 30.0;
		constexpr double kEdge = 8.0;
		constexpr double kSteep = 100.0;
		constexpr double kFar = 1e6;       // a water corner further out than this: the file isn't read
		constexpr double kNoCell = 4e6;    // a sheet corner further out than this has no water cell

		uint32_t rd16(const uint8_t *b, size_t at) { return uint32_t(b[at] | (b[at + 1] << 8)); }
		uint32_t rd32(const uint8_t *b, size_t at)
		{
			uint32_t v = 0;
			std::memcpy(&v, b + at, 4);
			return v;
		}
		double rdf(const uint8_t *b, size_t at)
		{
			float v = 0.0f;
			std::memcpy(&v, b + at, 4);
			return double(v);
		}

		bool fail(result &r, const char *why)
		{
			r.why = why;
			return false;
		}

		// bake_shadow_geo.decrypt_mmb (as shadow.cpp's): the MMB's two layers, unscrambled in place
		void decrypt_mmb(uint8_t *b, size_t size, const uint8_t *first, const uint8_t *second)
		{
			if (size < 8)
				return;
			if (b[3] >= 5)
			{
				uint32_t key = first[b[5] ^ 0xF0], kc = 0;
				for (size_t i = 8; i < size; ++i)
				{
					const uint32_t x = ((key & 0xFF) << 8) | (key & 0xFF);
					kc += 1;
					key += kc;
					b[i] ^= uint8_t((x >> (key & 7)) & 0xFF);
					kc += 1;
					key += kc;
				}
			}
			if (b[6] == 0xFF && b[7] == 0xFF)
			{
				const size_t own = size_t(b[0] | (b[1] << 8) | (b[2] << 16));
				const size_t length = own <= size && size - own < 16 ? own : size;
				uint32_t k1 = b[5] ^ 0xF0u;
				uint32_t k2 = second[k1];
				const size_t n = ((length - 8) & ~size_t(0xF)) >> 1;
				size_t o1 = 8, o2 = 8 + n;
				for (size_t done = 0; done < n; done += 8)
				{
					if (k2 & 1)
					{
						uint8_t t[8];
						std::memcpy(t, b + o1, 8);
						std::memcpy(b + o1, b + o2, 8);
						std::memcpy(b + o2, t, 8);
					}
					k1 += 9;
					k2 += k1;
					o1 += 8;
					o2 += 8;
				}
			}
		}

		// bake_shadow_geo.mmb_models (as shadow.cpp's), only the corners kept: every model's strip cut into triangles
		// without the degenerate ones, their corners' positions in order (x, y, z each); false where the Python's
		// struct.unpack_from raises (the MMB then has none)
		bool mmb_corners(const uint8_t *b, size_t size, std::vector<double> &out)
		{
			out.clear();
			if (size < 64)
				return false;
			const uint32_t stride = b[4] == 2 ? 48 : 36;
			const uint32_t pieces = rd32(b, 32), ptr = rd32(b, 60);
			std::vector<uint32_t> offs;
			if (ptr == 0)
			{
				if (pieces == 0)
					offs.push_back(64);
				else
				{
					if (64 + 32 > size)
						return false;
					for (uint32_t i = 0; i < 8; ++i)
					{
						const uint32_t o = rd32(b, 64 + i * 4);
						if (o != 0 && offs.size() < pieces)
							offs.push_back(o);
					}
				}
			}
			else
			{
				offs.push_back(ptr);
				for (uint64_t o = 64; o < ptr; o += 4)
				{
					if (o + 4 > size)
						return false;
					const uint32_t v = rd32(b, size_t(o));
					if (v != 0 && v != ptr)
						offs.push_back(v);
				}
			}
			for (const uint32_t po : offs)
			{
				if (uint64_t(po) + 32 > size)
					continue;
				const uint32_t n = rd32(b, po);
				if (n > 100)
					continue;
				uint64_t m = uint64_t(po) + 32;
				for (uint32_t k = 0; k < n; ++k)
				{
					if (m + 20 > size)
						break;
					const uint32_t vc = rd16(b, size_t(m + 16));
					const uint64_t vstart = m + 20, vend = vstart + uint64_t(vc) * stride;
					if (vend + 4 > size)
						break;
					const uint32_t ic = rd16(b, size_t(vend));
					const uint64_t istart = vend + 4;
					if (istart + uint64_t(ic) * 2 > size)
						break;
					for (uint32_t i = 0; ic >= 3 && i + 2 < ic; ++i)
					{
						const uint32_t a = rd16(b, size_t(istart + i * 2)), bb = rd16(b, size_t(istart + i * 2 + 2)), c = rd16(b, size_t(istart + i * 2 + 4));
						const bool odd = (i & 1) == 1;
						const uint32_t t[3] = { odd ? bb : a, odd ? a : bb, c };
						if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2] || t[0] >= vc || t[1] >= vc || t[2] >= vc)
							continue;
						for (const uint32_t v : t)
							for (int e = 0; e < 3; ++e)
								out.push_back(rdf(b, size_t(vstart + uint64_t(v) * stride + e * 4)));
					}
					m = istart + uint64_t(ic) * 2 + ((ic & 1) != 0 ? 2 : 0);
				}
			}
			return true;
		}

		// read_d3m: a D3M chunk's (`ch`, `size` bytes, its header included) triangles' corners; false when it isn't one
		// this reads
		bool d3m_corners(const uint8_t *ch, size_t size, std::vector<double> &out)
		{
			out.clear();
			if (size < 0x1E)
				return false;
			const uint32_t ver = rd16(ch, 0x10), zero = rd16(ch, 0x12), groups = ch[0x14], flags = ch[0x15];
			const uint32_t n = rd16(ch, 0x16), c0 = rd16(ch, 0x18), c1 = rd16(ch, 0x1A);
			if (ver != 6 || zero != 0)
				return false;
			size_t start = 0;
			if (flags == 0 && groups == 1 && c0 == n && c1 == 0)
				start = 0x1E + 16;
			else if (flags == 0 && groups == 2 && c0 + c1 == n)
				start = 0x1E + 32;
			else if (flags == 1 && groups == 0 && c0 == n)
				start = 0x1E;
			else
				return false;
			if (((start + 108 * size_t(n) + 15) & ~size_t(15)) != size)
				return false;
			out.reserve(size_t(n) * 9);
			for (size_t i = 0; i < size_t(n) * 3; ++i)
				for (int e = 0; e < 3; ++e)
					out.push_back(rdf(ch, start + i * 36 + e * 4));
			return true;
		}

		// game_water.generator_model: the model's name, position, rotation and scale; false when there's none
		struct model_command
		{
			uint32_t name;
			double pos[3], rot[3] = { 0.0, 0.0, 0.0 }, scale[3] = { 1.0, 1.0, 1.0 };
		};
		bool generator_model(const uint8_t *ch, size_t size, model_command &g)
		{
			if (size < 0x8C)
				return false;
			const uint64_t o = rd32(ch, 0x84), end = rd32(ch, 0x88);
			if (o + 32 > size || ch[o] != 1)
				return false;
			for (int e = 0; e < 3; ++e)
				g.pos[e] = rdf(ch, size_t(o + 0x14 + e * 4));
			const uint64_t stop = std::min<uint64_t>(end, size);
			for (uint64_t p = o + 0x20; p + 16 <= stop;)
			{
				const uint32_t op = ch[p], b1 = ch[p + 1];
				if (op == 0 && b1 == 1)
					break;
				const uint32_t step = (b1 == 0x0B || b1 == 0x0C) ? 0x10 : (b1 & 0x1F) * 4;
				if (step == 0)
					break;
				if (op == 0x09)
					for (int e = 0; e < 3; ++e)
						g.rot[e] = rdf(ch, size_t(p + 4 + e * 4));
				else if (op == 0x0F)
					for (int e = 0; e < 3; ++e)
						g.scale[e] = rdf(ch, size_t(p + 4 + e * 4));
				p += step;
			}
			g.name = rd32(ch, size_t(o + 12));
			return true;
		}

		// Rx Ry Rz (row vectors), multiplied (Rx Ry) Rz, each entry (a0 b0 + a1 b1) + a2 b2
		void matrix(const double rot[3], double out[3][3])
		{
			const double cx = std::cos(rot[0]), sx = std::sin(rot[0]);
			const double cy = std::cos(rot[1]), sy = std::sin(rot[1]);
			const double cz = std::cos(rot[2]), sz = std::sin(rot[2]);
			const double rx[3][3] = { { 1.0, 0.0, 0.0 }, { 0.0, cx, sx }, { 0.0, -sx, cx } };
			const double ry[3][3] = { { cy, 0.0, -sy }, { 0.0, 1.0, 0.0 }, { sy, 0.0, cy } };
			const double rz[3][3] = { { cz, sz, 0.0 }, { -sz, cz, 0.0 }, { 0.0, 0.0, 1.0 } };
			auto mul = [](const double a[3][3], const double b[3][3], double o[3][3]) {
				for (int i = 0; i < 3; ++i)
					for (int j = 0; j < 3; ++j)
					{
						const double p0 = a[i][0] * b[0][j], p1 = a[i][1] * b[1][j], p2 = a[i][2] * b[2][j];
						const double s = p0 + p1;
						o[i][j] = s + p2;
					}
			};
			double xy[3][3];
			mul(rx, ry, xy);
			mul(xy, rz, out);
		}

		// World FX's water: its triangles but the steep ones (ax, az, ay, bx, bz, by, cx, cz, cy), and the grid
		struct water
		{
			std::vector<double> tris;
			std::unordered_map<uint64_t, std::vector<uint32_t>> cells;

			static uint64_t key(int64_t cx, int64_t cz) { return (uint64_t(uint32_t(int32_t(cx))) << 32) | uint32_t(int32_t(cz)); }
		};

		// read_water; false with why when the file isn't one
		bool read_water(const std::wstring &path, water &w, std::string &why)
		{
			std::vector<uint8_t> b;
			if (!read_file(path, b))
			{
				why = "the water file can't be read";
				return false;
			}
			if (b.size() < 20 || std::memcmp(b.data(), "WTR2", 4) != 0)
			{
				why = "the water file isn't a WTR2 file";
				return false;
			}
			const uint64_t cells = rd32(b.data(), 12), corners = rd32(b.data(), 16);
			const uint64_t at = 20 + cells * 16;
			if (corners % 3 != 0 || at + corners * 32 > b.size())
			{
				why = "the water file is cut short";
				return false;
			}
			for (uint64_t t = 0; t < corners / 3; ++t)
			{
				const size_t rec = size_t(at + t * 96);
				if (!(rdf(b.data(), rec + 12) < kSteep))
					continue;
				for (int e = 0; e < 3; ++e)
					for (const size_t f : { size_t(0), size_t(8), size_t(16) })
					{
						const double v = rdf(b.data(), rec + 32 * e + f);
						if (!(std::fabs(v) <= kFar))
						{
							why = "a water corner is out of range";
							return false;
						}
						w.tris.push_back(v);
					}
			}
			for (uint32_t i = 0; i < w.tris.size() / 9; ++i)
			{
				const double *t = &w.tris[size_t(i) * 9];
				const double x0 = std::min({ t[0], t[3], t[6] }) - kPad, x1 = std::max({ t[0], t[3], t[6] }) + kPad;
				const double z0 = std::min({ t[1], t[4], t[7] }) - kPad, z1 = std::max({ t[1], t[4], t[7] }) + kPad;
				const int64_t cx0 = int64_t(std::floor(x0 / kCell)), cx1 = int64_t(std::floor(x1 / kCell));
				const int64_t cz0 = int64_t(std::floor(z0 / kCell)), cz1 = int64_t(std::floor(z1 / kCell));
				for (int64_t cx = cx0; cx <= cx1; ++cx)
					for (int64_t cz = cz0; cz <= cz1; ++cz)
						w.cells[water::key(cx, cz)].push_back(i);
			}
			return true;
		}

		// edge2: the squared distance from (x, z) to the segment p-q
		double edge2(double x, double z, double px, double pz, double qx, double qz)
		{
			const double dx = qx - px, dz = qz - pz;
			const double len = dx * dx + dz * dz;
			double t = ((x - px) * dx + (z - pz) * dz) / (1e-12 > len ? 1e-12 : len);
			t = 0.0 > t ? 0.0 : t;   // (Python's max(t, 0.0), then min(t, 1.0))
			t = 1.0 < t ? 1.0 : t;
			const double ex = (x - px) - t * dx, ez = (z - pz) - t * dz;
			return ex * ex + ez * ez;
		}

		// surface: a water triangle's surface height at (x, z); false when it's further than kReach across
		bool surface(const double *t, double x, double z, double &s)
		{
			const double ax = t[0], az = t[1], ay = t[2], bx = t[3], bz = t[4], by = t[5], cx = t[6], cz = t[7], cy = t[8];
			const double d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
			if (std::fabs(d) < 1e-9)
				return false;
			double u = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / d;
			double v = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / d;
			double w = (1.0 - u) - v;
			if (u < 0.0 || v < 0.0 || w < 0.0)
			{
				double gap = edge2(x, z, ax, az, bx, bz);
				const double g2 = edge2(x, z, bx, bz, cx, cz), g3 = edge2(x, z, cx, cz, ax, az);
				gap = g2 < gap ? g2 : gap;
				gap = g3 < gap ? g3 : gap;
				if (gap > kReach * kReach)
					return false;
				u = 0.0 > u ? 0.0 : u;
				v = 0.0 > v ? 0.0 : v;
				w = 0.0 > w ? 0.0 : w;
				const double sum = (u + v) + w;
				u = u / sum;
				v = v / sum;
				w = w / sum;
			}
			s = (u * ay + v * by) + w * cy;
			return true;
		}

		// WaterGrid.offset: World FX's surface at (x, z) less y, the nearest within kWindow (a tie: the smaller)
		bool offset(const water &w, double x, double y, double z, double &best)
		{
			if (!(std::fabs(x) <= kNoCell) || !(std::fabs(z) <= kNoCell))
				return false;
			const auto c = w.cells.find(water::key(int64_t(std::floor(x / kCell)), int64_t(std::floor(z / kCell))));
			if (c == w.cells.end())
				return false;
			bool found = false;
			for (const uint32_t i : c->second)
			{
				double s = 0.0;
				if (!surface(&w.tris[size_t(i) * 9], x, z, s))
					continue;
				const double d = s - y;
				if (std::fabs(d) <= kWindow && (!found || std::fabs(d) < std::fabs(best) || (std::fabs(d) == std::fabs(best) && d < best)))
				{
					best = d;
					found = true;
				}
			}
			return found;
		}

		// GroundGrid: the zone's collision triangles (every one, steep or not; x and z only) by the cells their box +
		// kGroundPad touches, only for the cells of the points asked about; one with a corner's x or z not finite or
		// further out than kFar left out
		struct ground
		{
			std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
			std::vector<double> tris; // (ax, az, bx, bz, cx, cz)

			static int64_t cell(double v) { return int64_t(std::floor(v / kGroundCell)); }

			// covered: whether a collision triangle lies over or under (x, z) (game_water.ground_heights not empty)
			bool covered(double x, double z) const
			{
				if (!(std::fabs(x) <= kNoCell) || !(std::fabs(z) <= kNoCell))
					return false;
				const auto c = cells.find(water::key(cell(x), cell(z)));
				if (c == cells.end())
					return false;
				for (const uint32_t i : c->second)
				{
					const double *t = &tris[size_t(i) * 6];
					const double ax = t[0], az = t[1], bx = t[2], bz = t[3], cx = t[4], cz = t[5];
					const double d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
					if (std::fabs(d) < 1e-9)
						continue;
					const double u = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / d;
					const double v = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / d;
					if (u < -1e-4 || v < -1e-4 || u + v > 1.0 + 1e-4)
						continue;
					return true;
				}
				return false;
			}
		};

		// The grid for the points (x, z pairs) asked about; false with why when the collision can't be read (read only
		// when a point is in a cell)
		bool make_ground(const std::vector<uint8_t> &dat, const std::vector<double> &points, ground &g, std::string &why)
		{
			int64_t qx0 = 0, qx1 = 0, qz0 = 0, qz1 = 0;
			for (size_t i = 0; i + 1 < points.size(); i += 2)
			{
				const double x = points[i], z = points[i + 1];
				if (!(std::fabs(x) <= kNoCell) || !(std::fabs(z) <= kNoCell))
					continue;
				const int64_t cx = ground::cell(x), cz = ground::cell(z);
				if (g.cells.empty())
				{
					qx0 = qx1 = cx;
					qz0 = qz1 = cz;
				}
				qx0 = std::min(qx0, cx);
				qx1 = std::max(qx1, cx);
				qz0 = std::min(qz0, cz);
				qz1 = std::max(qz1, cz);
				g.cells[water::key(cx, cz)];
			}
			if (g.cells.empty())
				return true;
			collision::reader rd;
			result cr;
			if (!collision::open(dat, rd, cr))
			{
				why = cr.why.empty() ? "the zone's collision can't be read" : cr.why;
				return false;
			}
			collision::each(rd, [&](const collision::triangle &t) {
				const double v[6] = { t.p[0][0], t.p[0][2], t.p[1][0], t.p[1][2], t.p[2][0], t.p[2][2] };
				for (const double e : v)
					if (!(std::fabs(e) <= kFar))
						return;
				const double x0 = std::min({ v[0], v[2], v[4] }) - kGroundPad, x1 = std::max({ v[0], v[2], v[4] }) + kGroundPad;
				const double z0 = std::min({ v[1], v[3], v[5] }) - kGroundPad, z1 = std::max({ v[1], v[3], v[5] }) + kGroundPad;
				const int64_t cx0 = std::max(ground::cell(x0), qx0), cx1 = std::min(ground::cell(x1), qx1);
				const int64_t cz0 = std::max(ground::cell(z0), qz0), cz1 = std::min(ground::cell(z1), qz1);
				bool added = false;
				for (int64_t cx = cx0; cx <= cx1; ++cx)
					for (int64_t cz = cz0; cz <= cz1; ++cz)
					{
						const auto c = g.cells.find(water::key(cx, cz));
						if (c == g.cells.end())
							continue;
						if (!added)
						{
							g.tris.insert(g.tris.end(), v, v + 6);
							added = true;
						}
						c->second.push_back(uint32_t(g.tris.size() / 6 - 1));
					}
			});
			return true;
		}

		// A chunk with its folder ('/'-joined, as game_water.chunks joins them)
		struct entry
		{
			std::string folder;
			const uint8_t *chunk; // (its 16-byte header)
			size_t size;          // (with the header)
		};

		// resolve: of the chunks named so, the one in `folder` or the nearest folder over it, else the first
		const entry *resolve(const std::vector<entry> &list, const std::string &folder)
		{
			const entry *best = nullptr;
			size_t depth = 0;
			for (const entry &e : list)
			{
				const std::string &f = e.folder;
				const bool over = f.empty() || f == folder || (folder.size() > f.size() && folder.compare(0, f.size(), f) == 0 && folder[f.size()] == '/');
				if (over && (best == nullptr || f.size() + 1 > depth))
				{
					best = &e;
					depth = f.size() + 1;
				}
			}
			return best != nullptr ? best : &list.front();
		}

		bool skipped(const std::string &folder)
		{
			for (size_t at = 0;;)
			{
				const size_t slash = folder.find('/', at);
				const std::string part = folder.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
				if (part == "weat" || part == "fses" || part == "fser" || part.rfind("kumo", 0) == 0 || part.rfind("kiri", 0) == 0)
					return true;
				if (slash == std::string::npos)
					return false;
				at = slash + 1;
			}
		}

		bool bake_sea(uint32_t zone, const std::wstring &out, const std::wstring &water_file, result &r)
		{
			r = result();
			if (water_file.empty() || GetFileAttributesW(water_file.c_str()) == INVALID_FILE_ATTRIBUTES)
			{
				r.none = true;
				r.why = "no water file";
				return false;
			}
			water w;
			std::string why;
			if (!read_water(water_file, w, why))
				return fail(r, why.c_str());
			if (w.tris.empty())
			{
				r.none = true;
				r.why = "no sheet kept (no water but falls)";
				return false;
			}
			const std::wstring path = zone_dat_path(zone, why);
			if (path.empty())
				return fail(r, why.c_str());
			std::vector<uint8_t> dat;
			if (!read_file(path, dat))
				return fail(r, "the zone's DAT can't be read");
			// Each generator's model placed (sheets::placed) and checked: flat, on World FX's water, at its level; its corners
			// not on it
			struct sheet
			{
				std::vector<double> world;
				double median;
				std::vector<double> rest; // (x, z of each corner not on World FX's water)
			};
			std::vector<sheet> found;
			std::vector<double> on, sorted, rest, queries;
			const bool placed = sheets::placed(dat, [&](const std::vector<double> &world) {
				const size_t n = world.size() / 3;
				double lo = world[1], hi = world[1];
				for (size_t i = 1; i < n; ++i)
				{
					lo = std::min(lo, world[i * 3 + 1]);
					hi = std::max(hi, world[i * 3 + 1]);
				}
				if (!(hi - lo < kFlat))
					return;
				on.clear();
				rest.clear();
				for (size_t i = 0; i < n; ++i)
				{
					double d = 0.0;
					if (offset(w, world[i * 3], world[i * 3 + 1], world[i * 3 + 2], d))
						on.push_back(d);
					else
					{
						rest.push_back(world[i * 3]);
						rest.push_back(world[i * 3 + 2]);
					}
				}
				if (on.size() < kMinOn)
					return;
				sorted = on;
				std::stable_sort(sorted.begin(), sorted.end());
				const size_t mid = sorted.size() / 2;
				const double median = (sorted.size() & 1) != 0 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2.0;
				if (!(std::fabs(median) <= kMaxOffset))
					return;
				queries.insert(queries.end(), rest.begin(), rest.end());
				found.push_back(sheet{ world, median, rest });
			}, why);
			if (!placed)
				return fail(r, why.c_str());

			// Of those, the ones going on past the zone: at least kMinOut and 1 in kOutShare of their corners neither on
			// World FX's water nor over or under any collision
			ground gr;
			if (!queries.empty() && !make_ground(dat, queries, gr, why))
				return fail(r, why.c_str());
			std::vector<float> body;
			uint32_t sheets = 0;
			for (const sheet &sh : found)
			{
				const size_t n = sh.world.size() / 3;
				size_t out_past = 0;
				for (size_t i = 0; i + 1 < sh.rest.size(); i += 2)
					if (!gr.covered(sh.rest[i], sh.rest[i + 1]))
						out_past++;
				if (out_past < kMinOut || out_past * kOutShare < n)
					continue;
				sheets++;
				for (size_t i = 0; i < n; ++i)
				{
					const double surf = sh.world[i * 3 + 1] + sh.median;
					const double bed = surf + kBed;
					const float rec[8] = { float(sh.world[i * 3]), float(bed), float(sh.world[i * 3 + 2]), float(kKind), float(surf), float(kEdge), 0.0f, 0.0f };
					body.insert(body.end(), rec, rec + 8);
				}
			}
			if (sheets == 0)
			{
				r.none = true;
				r.why = "no sheet kept";
				return false;
			}
			std::vector<uint8_t> file(16 + body.size() * 4);
			const uint32_t head[3] = { zone, sheets, uint32_t(body.size() / 8) };
			std::memcpy(file.data(), "SEA1", 4);
			std::memcpy(file.data() + 4, head, 12);
			std::memcpy(file.data() + 16, body.data(), body.size() * 4);
			if (!write_file(out, file.data(), file.size()))
				return fail(r, "the file couldn't be written");
			return true;
		}
	}

	bool sheets::placed(const std::vector<uint8_t> &dat, const std::function<void(const std::vector<double> &world)> &each, std::string &why)
	{
		const uint8_t *first = nullptr, *second = nullptr;
		if (!key_tables(first, second))
		{
			why = "the game's key tables weren't found";
			return false;
		}

		// The MMBs, D3Ms (by name, in the DAT's order) and generators, each with its folder
		std::unordered_map<uint32_t, std::vector<entry>> mmbs, d3ms;
		struct gen
		{
			std::string folder;
			model_command m;
		};
		std::vector<gen> gens;
		{
			std::vector<std::string> stack;
			std::string folder;
			for (const chunk &c : chunks(dat))
			{
				const uint8_t *head = c.body - 16;
				// (the chunk as the Python slices it: its header and body; a header-only or empty one is too short
				// for anything read here)
				const size_t size = 16 + c.size;
				if (c.type == 1 || c.type == 0)
				{
					// (type 1 opens a folder, type 0 closes one)
					if (c.type == 1)
						stack.emplace_back(c.name, 4);
					else if (!stack.empty())
						stack.pop_back();
					folder.clear();
					for (size_t i = 0; i < stack.size(); ++i)
						folder += (i != 0 ? "/" : "") + stack[i];
				}
				else if (c.type == 0x2E || c.type == 0x1F)
				{
					uint32_t name = 0;
					std::memcpy(&name, c.name, 4);
					(c.type == 0x2E ? mmbs : d3ms)[name].push_back(entry{ folder, head, size });
				}
				else if (c.type == 0x05 && !skipped(folder))
				{
					gen g;
					if (generator_model(head, size, g.m))
					{
						g.folder = folder;
						gens.push_back(std::move(g));
					}
				}
			}
		}

		// Each generator's model, read once a chunk
		std::unordered_map<const uint8_t *, std::pair<bool, std::vector<double>>> read;
		std::vector<double> world;
		for (const gen &g : gens)
		{
			const entry *e = nullptr;
			bool is_mmb = false;
			if (const auto m = mmbs.find(g.m.name); m != mmbs.end())
			{
				e = resolve(m->second, g.folder);
				is_mmb = true;
			}
			else if (const auto d = d3ms.find(g.m.name); d != d3ms.end())
				e = resolve(d->second, g.folder);
			else
				continue;
			auto known = read.find(e->chunk);
			if (known == read.end())
			{
				std::pair<bool, std::vector<double>> got;
				if (is_mmb)
				{
					if (e->size - 16 > 64)
					{
						std::vector<uint8_t> b(e->chunk + 16, e->chunk + e->size);
						decrypt_mmb(b.data(), b.size(), first, second);
						if (!mmb_corners(b.data(), b.size(), got.second))
							got.second.clear();
					}
					got.first = true; // (an MMB that doesn't read has no triangles: not an error)
				}
				else
					got.first = d3m_corners(e->chunk, e->size, got.second);
				known = read.emplace(e->chunk, std::move(got)).first;
			}
			const std::vector<double> &corners = known->second.second;
			if (!known->second.first || corners.empty())
				continue;

			// Placed: u = x sx, v = y sy, w = z sz; ((u R0j + v R1j) + w R2j) + position j
			double R[3][3];
			matrix(g.m.rot, R);
			const size_t n = corners.size() / 3;
			world.resize(corners.size());
			bool finite = true;
			for (size_t i = 0; i < n; ++i)
			{
				const double u = corners[i * 3] * g.m.scale[0], v = corners[i * 3 + 1] * g.m.scale[1], ww = corners[i * 3 + 2] * g.m.scale[2];
				for (int j = 0; j < 3; ++j)
				{
					const double a = u * R[0][j], b = v * R[1][j], c = ww * R[2][j];
					const double s = a + b;
					const double t = s + c;
					world[i * 3 + j] = t + g.m.pos[j];
					finite = finite && std::isfinite(world[i * 3 + j]);
				}
			}
			if (!finite)
				continue;
			each(world);
		}
		return true;
	}

	bool sea(uint32_t zone, const std::wstring &out, const std::wstring &water_file, result &r)
	{
		try
		{
			return bake_sea(zone, out, water_file, r);
		}
		catch (const std::bad_alloc &)
		{
			r = result();
			r.why = "out of memory";
			return false;
		}
	}
}
