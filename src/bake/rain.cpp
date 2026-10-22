/*
 * Rain (bake.hpp): reshade-worldfx's tools/bake_rain.py, in C++. Where rain stops in a zone, for World FX's rain: seen
 * from above, the first surface a drop falling straight down meets, on a grid of 0.5-yalm squares, and what it is
 * there.
 *
 * - The zone's own meshes (bake_shadow_geo.py's bake: every model its MZB places, from the zone DAT's MMBs, without the
 *   blended ones: water sheets, decals, glows), so drops stop at roofs, bridges, awnings and tree crowns. The cut-out
 *   ones (leaves and branches on cards: objects the game alpha-tests) only where their texture is drawn, as the shadow
 *   map has them.
 * - The collision (collision.hpp) where no mesh covers a square (models the DAT doesn't hold).
 * - The water World FX draws (bake_water.py's WTR2 file: the surface rebuilt over the water collision) wherever it's
 *   above what's under it: drops end on the water, where World FX draws rings.
 *
 * Each square keeps the topmost of those over its middle and what it is: 2 ground facing up (within 40 degrees of
 * flat: drops splash there), 1 anything else that stops rain (steep, or leaves: no splash), 3 water, 0 nothing (no
 * surface under the square at all). Heights are kept to 1/16 yalm (half a yalm, rounded up, where nothing splashes:
 * leaves and steep faces). The grid covers the collision's extent and 16 yalms around it. Only zones with a rainy or
 * snowy weather of their own get a file (rain, squalls, thunder, thunderstorms, snow or blizzards: a folder of that name
 * in the zone DAT's weather; World FX's snow ends on the same map).
 *
 * The Python lays every mesh triangle of the zone at once (millions in a wood: Zi'Tah's are 4.3 million), the whole grid
 * at once, three times over. Here the grid is laid a band of rows at a time, each placed model turned into the world
 * only for the bands it reaches, and the collision walked again for each band, so a 32-bit process holds a band's
 * heights rather than the zone's. A square takes what the Python's does: the topmost over its middle, and of equals the
 * first in the Python's order (the shadow soup's: by 32-yalm cell of each triangle's middle, then as placed), which a
 * key per square keeps here, the meshes being laid as placed. The heights' range (for the 1/16 steps) is known only once
 * every band is laid: a small zone's heights are kept in memory till then, a big zone's in a temporary file (its bands
 * laid again when there can't be one).
 *
 * Output (little-endian):
 *     char  magic[4] 'RHM1'
 *     u32   zone
 *     f32   square size (yalms)
 *     u32   width (squares across x)
 *     u32   squares with a surface
 *     u32   height (squares across z)
 *     f32   x0, z0 (the world's x and z at the grid's corner)
 *     f32   y0, y step (a square's height: y0 + q * step; y is the game's, down positive)
 *     u32   packing (4: Windows' XPRESS with Huffman, RtlDecompressBufferEx; 0: stored)
 *     u32   packed bytes
 *     the packed bytes: two planes, each width x height, rows along x from the corner, z first:
 *       u8  what each square is (0..3)
 *       u16 q of each square (0 where there's nothing), as the difference from the square before it in its row (the
 *           row's first from 0), mod 65536
 */

#include <windows.h>
#include "bake.hpp"
#include "collision.hpp"
#include "dat.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace hifi::bake
{
	namespace
	{
		constexpr double kSquare = 0.5;
		constexpr double kMargin = 16.0;
		constexpr double kUp = 0.766044443118978; // cos(radians(40))
		enum : uint8_t { kNothing = 0, kStops = 1, kGround = 2, kWater = 3 };
		constexpr double kStepQ = 1.0 / 16.0;    // height step (yalms), unless the zone is taller than 16383 of them
		constexpr double kCoarse = 0.5;          // ...and where nothing splashes (rounded up to it)
		constexpr uint32_t kXpressHuff = 4;
		constexpr size_t kBandSquares = 1 << 20; // (squares laid at once: 26 MB of band)
		constexpr size_t kKeepSquares = 1 << 22; // (a grid this small keeps its heights in memory for the second pass: 32 MB)
		constexpr double kInf = std::numeric_limits<double>::infinity();

		// bake_shadow_geo.py's
		constexpr double kShadowCell = 32.0;
		constexpr uint32_t kBlended = 0x8000;
		constexpr uint32_t kAlphaRef = 0x60;

		uint32_t u32(const uint8_t *b, size_t at)
		{
			uint32_t v;
			std::memcpy(&v, b + at, 4);
			return v;
		}
		uint32_t u16(const uint8_t *b, size_t at)
		{
			return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8;
		}
		float f32(const uint8_t *b, size_t at)
		{
			float v;
			std::memcpy(&v, b + at, 4);
			return v;
		}

		// ---- The zone's meshes (bake_shadow_geo.py: decrypt_mmb, mmb_models, rotation, textures, zone_meshes, bake)

		struct model
		{
			uint8_t tex[16];
			uint32_t blend;
			std::vector<float> pos, uv;  // (x y z, u v a corner)
			std::vector<uint16_t> tri;   // (three corners a triangle: the strip cut, its degenerate ones left out)
			double lo[3], hi[3];         // (its corners' extent, in its own space)
		};

		struct placement
		{
			uint32_t lib;     // (its MMB's models)
			bool tested;      // (alpha-tested: its MZB record's name starts with '_')
			double R[3][3], T[3];
			uint64_t first;   // (its first triangle's place among all the zone's, as the soup has them before its sort)
			double zlo, zhi;  // (where in z it can reach; NaN when it can't be said: every band)
		};

		struct mask
		{
			uint32_t w, h;
			std::vector<uint8_t> bits; // (a texel drawn: 4 x its alpha over kAlphaRef)
		};

		struct meshes
		{
			std::vector<std::vector<model>> lib;
			std::vector<placement> placements;
			std::vector<mask> masks;
			std::map<std::string, uint32_t> mask_of; // (texture name: its mask + 1, 0 when it cuts nothing out)
			uint64_t triangles = 0;
		};

		// decrypt_mmb: two layers, both key tables the game's own
		void decrypt_mmb(std::vector<uint8_t> &b, const uint8_t *kt1, const uint8_t *kt2)
		{
			if (b.size() < 8)
				return;
			if (b[3] >= 5)
			{
				uint32_t key = kt1[b[5] ^ 0xF0], kc = 0;
				for (size_t i = 8; i < b.size(); ++i)
				{
					const uint32_t x = ((key & 0xFF) << 8) | (key & 0xFF);
					kc += 1;
					key = key + kc;
					b[i] ^= uint8_t((x >> (key & 7)) & 0xFF);
					kc += 1;
					key = key + kc;
				}
			}
			if (b[6] == 0xFF && b[7] == 0xFF)
			{
				// The swaps pair the two halves of the MMB's own length (its header's first three bytes), not the
				// chunk's, which is padded to 16. (A header whose size isn't just short of the chunk's isn't one: the
				// chunk's length.)
				const size_t own = size_t(b[0]) | size_t(b[1]) << 8 | size_t(b[2]) << 16;
				const size_t length = (own <= b.size() && b.size() - own < 16) ? own : b.size();
				uint32_t k1 = b[5] ^ 0xF0u;
				uint32_t k2 = kt2[k1];
				const size_t n = ((length - 8) & ~size_t(0xF)) >> 1;
				size_t o1 = 8, o2 = 8 + n;
				for (size_t done = 0; done < n; done += 8)
				{
					if (k2 & 1)
						for (int k = 0; k < 8; ++k)
							std::swap(b[o1 + k], b[o2 + k]);
					k1 += 9;
					k2 += k1;
					o1 += 8;
					o2 += 8;
				}
			}
		}

		// mmb_models: the MMB's models, its strips cut into triangles without the degenerate ones; false when the Python
		// raises (struct.error: the MMB is dropped)
		bool mmb_models(const std::vector<uint8_t> &b, std::vector<model> &out)
		{
			out.clear();
			const size_t size = b.size();
			const size_t stride = b[4] == 2 ? 48 : 36, floats = stride / 4;
			const uint32_t pieces = u32(b.data(), 32), ptr = u32(b.data(), 60);
			std::vector<uint64_t> offs;
			if (ptr == 0)
			{
				if (pieces != 0)
				{
					if (64 + 32 > size)
						return false;
					for (int k = 0; k < 8; ++k)
						if (const uint32_t o = u32(b.data(), 64 + 4 * k); o != 0 && offs.size() < pieces)
							offs.push_back(o);
				}
				else
					offs.push_back(64);
			}
			else
			{
				offs.push_back(ptr);
				for (uint64_t o = 64; o < ptr; o += 4)
				{
					if (o + 4 > size)
						return false;
					const uint32_t v = u32(b.data(), size_t(o));
					if (v != 0 && v != ptr)
						offs.push_back(v);
				}
			}
			for (const uint64_t po : offs)
			{
				if (po + 32 > size)
					continue;
				const uint32_t n = u32(b.data(), size_t(po));
				if (n > 100)
					continue;
				uint64_t m = po + 32;
				for (uint32_t piece = 0; piece < n; ++piece)
				{
					if (m + 20 > size)
						break;
					model md;
					std::memcpy(md.tex, b.data() + m, 16);
					const uint32_t vc = u16(b.data(), size_t(m + 16));
					md.blend = u16(b.data(), size_t(m + 18));
					const uint64_t vstart = m + 20, vend = vstart + uint64_t(vc) * stride;
					if (vend + 4 > size)
						break;
					const uint32_t ic = u16(b.data(), size_t(vend));
					const uint64_t istart = vend + 4;
					if (istart + uint64_t(ic) * 2 > size)
						break;
					md.pos.resize(size_t(vc) * 3);
					md.uv.resize(size_t(vc) * 2);
					for (int k = 0; k < 3; ++k)
					{
						md.lo[k] = kInf;
						md.hi[k] = -kInf;
					}
					for (uint32_t v = 0; v < vc; ++v)
					{
						const size_t at = size_t(vstart + uint64_t(v) * stride);
						for (int k = 0; k < 3; ++k)
						{
							const float f = f32(b.data(), at + 4 * k);
							md.pos[v * 3 + k] = f;
							// (NaN poisons the extent: such a model is laid in every band)
							if (std::isnan(f))
								md.lo[k] = md.hi[k] = std::nan("");
							else if (!std::isnan(md.lo[k]))
							{
								md.lo[k] = std::min(md.lo[k], double(f));
								md.hi[k] = std::max(md.hi[k], double(f));
							}
						}
						md.uv[v * 2] = f32(b.data(), at + 4 * (floats - 2));
						md.uv[v * 2 + 1] = f32(b.data(), at + 4 * (floats - 1));
					}
					if (ic >= 3)
					{
						for (uint32_t t = 0; t + 2 < ic; ++t)
						{
							const uint32_t i0 = u16(b.data(), size_t(istart + 2 * t)), i1 = u16(b.data(), size_t(istart + 2 * t + 2)),
								i2 = u16(b.data(), size_t(istart + 2 * t + 4));
							// (winding kept: every other triangle of a strip turned)
							const uint32_t a = (t & 1) ? i1 : i0, bb = (t & 1) ? i0 : i1, c = i2;
							if (a == bb || bb == c || a == c || std::max({ a, bb, c }) >= vc)
								continue;
							md.tri.push_back(uint16_t(a));
							md.tri.push_back(uint16_t(bb));
							md.tri.push_back(uint16_t(c));
						}
					}
					md.tri.shrink_to_fit();
					out.push_back(std::move(md));
					m = istart + uint64_t(ic) * 2 + ((ic & 1) ? 2 : 0);
				}
			}
			return true;
		}

		// Two 3x3 matrices multiplied as numpy's matmul does it (OpenBLAS's dgemm: each term fused into the sum, in
		// order, from 0)
		void matmul3(const double A[3][3], const double B[3][3], double C[3][3])
		{
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
				{
					double acc = std::fma(A[i][0], B[0][j], 0.0);
					acc = std::fma(A[i][1], B[1][j], acc);
					acc = std::fma(A[i][2], B[2][j], acc);
					C[i][j] = acc + 0.0;
				}
		}

		// rotation: scale x Rx x Ry x Rz (row vectors): v' = v M + t
		void rotation(double rx, double ry, double rz, double sx, double sy, double sz, double M[3][3])
		{
			const double D[3][3] = { { sx, 0, 0 }, { 0, sy, 0 }, { 0, 0, sz } };
			double c = std::cos(rx), s = std::sin(rx);
			const double X[3][3] = { { 1, 0, 0 }, { 0, c, s }, { 0, -s, c } };
			c = std::cos(ry);
			s = std::sin(ry);
			const double Y[3][3] = { { c, 0, -s }, { 0, 1, 0 }, { s, 0, c } };
			c = std::cos(rz);
			s = std::sin(rz);
			const double Z[3][3] = { { c, s, 0 }, { -s, c, 0 }, { 0, 0, 1 } };
			double DX[3][3], DXY[3][3];
			matmul3(D, X, DX);
			matmul3(DX, Y, DXY);
			matmul3(DXY, Z, M);
		}

		// textures: the DAT's DXT3 images (IMG chunks, type 0x20, flag 0xA1), by name, where their body starts; false
		// when one can't be laid out as the Python lays it (its sides not multiples of 4: numpy's reshape raises, and
		// the Python's meshes fail whole)
		struct texture
		{
			const uint8_t *blocks;
			int32_t w, h;
		};
		bool textures(const std::vector<uint8_t> &dat, std::map<std::string, texture> &out)
		{
			for (const chunk &c : chunks(dat))
			{
				const uint8_t *b = c.body;
				if (c.type != 0x20 || c.size < 69 || b[0] != 0xA1 || std::memcmp(b + 57, "3TXD", 4) != 0)
					continue;
				const std::string name(reinterpret_cast<const char *>(b + 1), 16);
				if (out.count(name) != 0)
					continue;
				int32_t w, h;
				std::memcpy(&w, b + 21, 4);
				std::memcpy(&h, b + 25, 4);
				if (w <= 0 || h <= 0)
					continue;
				const uint64_t n = uint64_t(w / 4) * uint64_t(h / 4);
				if (69 + n * 16 > c.size)
					continue;
				if (w % 4 != 0 || h % 4 != 0)
					return false;
				out[name] = texture{ b + 69, w, h };
			}
			return true;
		}

		// A texture's alpha test (4 x its alpha over kAlphaRef: DXT3's 4-bit alpha x 17, so 2 and up), texel by texel
		void alpha_bits(const texture &t, std::vector<uint8_t> &bits)
		{
			bits.assign(size_t(t.w) * size_t(t.h), 0);
			const int32_t bw = t.w / 4, bh = t.h / 4;
			for (int32_t by = 0; by < bh; ++by)
				for (int32_t bx = 0; bx < bw; ++bx)
				{
					uint64_t a;
					std::memcpy(&a, t.blocks + 16 * (size_t(by) * bw + bx), 8);
					for (int p = 0; p < 16; ++p)
					{
						const uint32_t alpha = uint32_t((a >> (4 * p)) & 15) * 17;
						bits[size_t(by * 4 + p / 4) * t.w + size_t(bx * 4 + p % 4)] = alpha * 4 > kAlphaRef ? 1 : 0;
					}
				}
		}

		// The zone's meshes as the soup has them, ready to be laid band by band; false when the Python's bake fails
		// (its ValueError: the collision alone then), with why
		bool load_meshes(const std::vector<uint8_t> &dat, const std::vector<uint8_t> &mzb, meshes &ms, std::string &why)
		{
			const uint8_t *kt1 = nullptr, *kt2 = nullptr;
			if (!key_tables(kt1, kt2))
			{
				why = "the game's key tables weren't found";
				return false;
			}
			// The MMBs by name (the first of a name)
			std::map<std::string, uint32_t> lib_of;
			for (const chunk &c : chunks(dat))
			{
				if (c.type != 0x2E || c.size <= 64)
					continue;
				std::vector<uint8_t> b(c.body, c.body + c.size);
				decrypt_mmb(b, kt1, kt2);
				std::vector<model> models;
				if (!mmb_models(b, models) || models.empty())
					continue;
				const std::string name(reinterpret_cast<const char *>(b.data() + 16), 16);
				if (lib_of.count(name) == 0)
				{
					lib_of[name] = uint32_t(ms.lib.size());
					ms.lib.push_back(std::move(models));
				}
			}
			std::map<std::string, texture> tex;
			if (!textures(dat, tex))
			{
				why = "a texture's sides aren't multiples of 4";
				return false;
			}
			// The MZB's object list: each record a 16-byte model name, translation, rotation in radians, scale
			const size_t n = u32(mzb.data(), 4) & 0xFFFFFF;
			for (size_t i = 0; i < n; ++i)
			{
				const size_t a = 0x20 + i * 0x64;
				if (a + 0x64 > mzb.size())
					break;
				const std::string name(reinterpret_cast<const char *>(mzb.data() + a), 16);
				const auto it = lib_of.find(name);
				if (it == lib_of.end())
					continue;
				double t[9];
				bool finite = true;
				for (int k = 0; k < 9; ++k)
				{
					t[k] = f32(mzb.data(), a + 16 + 4 * k);
					finite = finite && std::isfinite(t[k]);
				}
				if (!finite)
					continue;
				placement pl;
				pl.lib = it->second;
				pl.tested = mzb[a] == '_';
				rotation(t[3], t[4], t[5], t[6], t[7], t[8], pl.R);
				pl.T[0] = t[0];
				pl.T[1] = t[1];
				pl.T[2] = t[2];
				pl.first = ms.triangles;
				// (where its models can reach in z: their extents' corners turned, a yalm to spare)
				pl.zlo = kInf;
				pl.zhi = -kInf;
				bool unknown = false;
				for (const model &md : ms.lib[pl.lib])
				{
					if (md.tri.empty() || (md.blend & kBlended) != 0)
						continue;
					ms.triangles += md.tri.size() / 3;
					if (pl.tested)
					{
						// (mask_id: a cut-out texture's mask, made when first met)
						const std::string tname(reinterpret_cast<const char *>(md.tex), 16);
						if (ms.mask_of.count(tname) == 0)
						{
							const auto ti = tex.find(tname);
							mask mk;
							bool cuts = false;
							if (ti != tex.end())
							{
								mk.w = uint32_t(ti->second.w);
								mk.h = uint32_t(ti->second.h);
								alpha_bits(ti->second, mk.bits);
								cuts = std::find(mk.bits.begin(), mk.bits.end(), uint8_t(0)) != mk.bits.end();
							}
							if (!cuts)
								ms.mask_of[tname] = 0;
							else
							{
								ms.masks.push_back(std::move(mk));
								ms.mask_of[tname] = uint32_t(ms.masks.size());
								if (ms.masks.size() > 255)
								{
									why = "more than 255 cut-out textures";
									return false;
								}
							}
						}
					}
					for (int cx = 0; cx < 2; ++cx)
						for (int cy = 0; cy < 2; ++cy)
							for (int cz = 0; cz < 2; ++cz)
							{
								const double x = cx ? md.hi[0] : md.lo[0], y = cy ? md.hi[1] : md.lo[1], z = cz ? md.hi[2] : md.lo[2];
								const double wz = x * pl.R[0][2] + y * pl.R[1][2] + z * pl.R[2][2] + pl.T[2];
								if (!std::isfinite(wz))
									unknown = true;
								else
								{
									pl.zlo = std::min(pl.zlo, wz - 1.0);
									pl.zhi = std::max(pl.zhi, wz + 1.0);
								}
							}
				}
				if (unknown)
					pl.zlo = pl.zhi = std::nan("");
				ms.placements.push_back(pl);
			}
			return true;
		}

		// ---- The grid, a band of rows at a time

		struct grid
		{
			double x0, z0;
			int64_t width, height;
		};

		struct band
		{
			int64_t r0, r1; // (its rows)
			std::vector<double> top;
			std::vector<uint8_t> kind;
			std::vector<uint64_t> key; // (the meshes': which of equals came first in the Python's order)
			void reset(int64_t first, int64_t end, int64_t width, bool keyed)
			{
				r0 = first;
				r1 = end;
				const size_t n = size_t((end - first) * width);
				top.assign(n, kInf);
				kind.assign(n, kNothing);
				if (keyed)
					key.assign(n, ~uint64_t(0));
				else
					key.clear();
			}
		};

		// A cut-out triangle's texture: its mask and its corners' coordinates
		struct cut
		{
			const mask *m;
			double u[3], v[3];
		};

		// Grid.lay for one triangle (corners P, x y z): each square of the band whose middle it covers (where its
		// texture is drawn, for a cut-out one) takes it when it's the topmost there (y least), or as high and first
		// (key, for the meshes; else the order they're laid in)
		void lay(const grid &g, band &bd, const double P[3][3], uint8_t what, const cut *ct, uint64_t key, const std::vector<double> *covered = nullptr)
		{
			const double S = kSquare;
			const double ax = P[0][0], az = P[0][2], bx = P[1][0], bz = P[1][2], cx = P[2][0], cz = P[2][2];
			const double minx = std::min({ ax, bx, cx }), maxx = std::max({ ax, bx, cx });
			const double minz = std::min({ az, bz, cz }), maxz = std::max({ az, bz, cz });
			const double i0d = std::max(std::ceil((minx - g.x0) / S - 0.5), 0.0);
			const double i1d = std::min(std::floor((maxx - g.x0) / S - 0.5), double(g.width - 1));
			const double j0d = std::max(std::ceil((minz - g.z0) / S - 0.5), 0.0);
			const double j1d = std::min(std::floor((maxz - g.z0) / S - 0.5), double(g.height - 1));
			// (the barycentric weights' denominator; a triangle seen edge-on from above covers nothing)
			const double d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
			if (!(i1d >= i0d) || !(j1d >= j0d) || !(std::fabs(d) > 1e-12))
				return;
			const int64_t i0 = int64_t(i0d), i1 = int64_t(i1d);
			const int64_t j0 = std::max(int64_t(j0d), bd.r0), j1 = std::min(int64_t(j1d), bd.r1 - 1);
			const double e1 = bz - cz, e2 = cx - bx, e3 = cz - az, e4 = ax - cx;
			const bool keyed = !bd.key.empty();
			for (int64_t j = j0; j <= j1; ++j)
			{
				const double pz = g.z0 + (double(j) + 0.5) * S;
				const size_t row = size_t((j - bd.r0) * g.width);
				for (int64_t i = i0; i <= i1; ++i)
				{
					const size_t at = row + size_t(i);
					if (covered != nullptr && (*covered)[at] != kInf)
						continue;
					const double px = g.x0 + (double(i) + 0.5) * S;
					const double l1 = (e1 * (px - cx) + e2 * (pz - cz)) / d;
					const double l2 = (e3 * (px - cx) + e4 * (pz - cz)) / d;
					const double l3 = 1.0 - l1 - l2;
					if (!(l1 >= -1e-9 && l2 >= -1e-9 && l3 >= -1e-9))
						continue;
					if (ct != nullptr)
					{
						const double u = l1 * ct->u[0] + l2 * ct->u[1] + l3 * ct->u[2];
						const double v = l1 * ct->v[0] + l2 * ct->v[1] + l3 * ct->v[2];
						const double uc = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u), vc = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
						const int64_t tx = std::min(int64_t(uc * double(ct->m->w)), int64_t(ct->m->w) - 1);
						const int64_t ty = std::min(int64_t(vc * double(ct->m->h)), int64_t(ct->m->h) - 1);
						if (!ct->m->bits[size_t(ty) * ct->m->w + size_t(tx)])
							continue;
					}
					const double y = l1 * P[0][1] + l2 * P[1][1] + l3 * P[2][1];
					if (y < bd.top[at] || (keyed && y == bd.top[at] && key < bd.key[at]))
					{
						bd.top[at] = y;
						bd.kind[at] = what;
						if (keyed)
							bd.key[at] = key;
					}
				}
			}
		}

		// facing_up: |normal.y| at least kUp of its length (numpy's cross and norm, term by term)
		bool facing_up(const double P[3][3])
		{
			const double u0 = P[1][0] - P[0][0], u1 = P[1][1] - P[0][1], u2 = P[1][2] - P[0][2];
			const double v0 = P[2][0] - P[0][0], v1 = P[2][1] - P[0][1], v2 = P[2][2] - P[0][2];
			const double n0 = u1 * v2 - u2 * v1, n1 = u2 * v0 - u0 * v2, n2 = u0 * v1 - u1 * v0;
			const double length = std::sqrt(0.0 + ((n0 * n0 + n1 * n1) + n2 * n2));
			return std::fabs(n1) >= kUp * (length >= 1e-12 ? length : 1e-12);
		}

		// The rows a placement reaches
		bool reaches(const grid &g, const placement &pl, const band &bd)
		{
			if (std::isnan(pl.zlo))
				return true;
			const double jlo = std::floor((pl.zlo - g.z0) / kSquare - 0.5), jhi = std::ceil((pl.zhi - g.z0) / kSquare - 0.5);
			return jhi >= double(bd.r0) && jlo <= double(bd.r1 - 1);
		}

		// The meshes laid over a band: each placement it reaches, its models turned into the world as the Python turns
		// them (pos @ R + T, then the soup's float32), each triangle with its key in the soup's order
		void lay_meshes(const grid &g, const meshes &ms, band &bd, std::vector<float> &world, std::vector<double> &uvq)
		{
			for (const placement &pl : ms.placements)
			{
				if (!reaches(g, pl, bd))
					continue;
				uint64_t index = pl.first;
				for (const model &md : ms.lib[pl.lib])
				{
					if (md.tri.empty() || (md.blend & kBlended) != 0)
						continue;
					const size_t vc = md.pos.size() / 3, count = md.tri.size() / 3;
					uint32_t k = 0;
					if (pl.tested)
					{
						const auto it = ms.mask_of.find(std::string(reinterpret_cast<const char *>(md.tex), 16));
						k = it != ms.mask_of.end() ? it->second : 0;
					}
					world.resize(vc * 3);
					for (size_t v = 0; v < vc; ++v)
					{
						const double x = md.pos[v * 3], y = md.pos[v * 3 + 1], z = md.pos[v * 3 + 2];
						for (int j = 0; j < 3; ++j)
						{
							double acc = std::fma(x, pl.R[0][j], 0.0);
							acc = std::fma(y, pl.R[1][j], acc);
							acc = std::fma(z, pl.R[2][j], acc);
							world[v * 3 + j] = float((acc + 0.0) + pl.T[j]);
						}
					}
					if (k != 0)
					{
						// (the cut-out word's u and v: 12-bit fractions, as the soup keeps them)
						uvq.resize(vc * 2);
						for (size_t e = 0; e < vc * 2; ++e)
						{
							const double c = double(md.uv[e]);
							const double cl = std::isnan(c) ? c : (c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c));
							const double q = std::nearbyint(cl * 4095.0);
							const uint32_t qi = std::isnan(q) ? 0u : uint32_t(q < 0.0 ? 0.0 : (q > 4095.0 ? 4095.0 : q));
							uvq[e] = double(qi) / 4095.0;
						}
					}
					for (size_t t = 0; t < count; ++t, ++index)
					{
						const float *c[3] = { &world[size_t(md.tri[t * 3]) * 3], &world[size_t(md.tri[t * 3 + 1]) * 3], &world[size_t(md.tri[t * 3 + 2]) * 3] };
						bool finite = true;
						for (int e = 0; e < 3; ++e)
							finite = finite && std::isfinite(c[e][0]) && std::isfinite(c[e][1]) && std::isfinite(c[e][2]);
						if (!finite)
							continue;
						// (the soup's order: by the 32-yalm cell of its middle, in float32 as numpy has it, z then x)
						const float mx = (c[0][0] + c[1][0]) + c[2][0], mz = (c[0][2] + c[1][2]) + c[2][2];
						const float fx = mx / 3.0f, fz = mz / 3.0f;
						const float qx = std::floor(fx / float(kShadowCell)), qz = std::floor(fz / float(kShadowCell));
						const int64_t kx = std::clamp<int64_t>(int64_t(std::clamp(qx, -40000.0f, 40000.0f)), -32768, 32767);
						const int64_t kz = std::clamp<int64_t>(int64_t(std::clamp(qz, -40000.0f, 40000.0f)), -32768, 32767);
						const uint64_t key = uint64_t(kz + 32768) << 48 | uint64_t(kx + 32768) << 32 | uint64_t(index & 0xFFFFFFFFu);
						double P[3][3];
						for (int e = 0; e < 3; ++e)
							for (int a = 0; a < 3; ++a)
								P[e][a] = double(c[e][a]);
						if (k != 0)
						{
							cut ct;
							ct.m = &ms.masks[k - 1];
							for (int e = 0; e < 3; ++e)
							{
								ct.u[e] = uvq[size_t(md.tri[t * 3 + e]) * 2];
								ct.v[e] = uvq[size_t(md.tri[t * 3 + e]) * 2 + 1];
							}
							lay(g, bd, P, kStops, &ct, key);
						}
						else
							lay(g, bd, P, facing_up(P) ? kGround : kStops, nullptr, key);
					}
				}
			}
		}

		// The water World FX draws (a WTR2 file): triangles at the surface's height, steep ones (falls, channel sides)
		// left out
		bool read_water(const std::wstring &path, std::vector<double> &tris, std::string &why)
		{
			std::vector<uint8_t> data;
			if (!read_file(path, data))
			{
				why = "the water file can't be read";
				return false;
			}
			if (data.size() < 20 || std::memcmp(data.data(), "WTR2", 4) != 0)
			{
				why = "the water file isn't a WTR2 file";
				return false;
			}
			const uint64_t cells = u32(data.data(), 12), corners = u32(data.data(), 16);
			const uint64_t at = 20 + cells * 16;
			if (corners % 3 != 0 || at + corners * 32 > data.size())
			{
				why = "the water file is cut short";
				return false;
			}
			for (uint64_t t = 0; t < corners / 3; ++t)
			{
				const size_t rec = size_t(at + t * 96);
				if (!(f32(data.data(), rec + 12) < 100.0f))
					continue;
				for (int e = 0; e < 3; ++e)
				{
					tris.push_back(double(f32(data.data(), rec + 32 * e)));
					tris.push_back(double(f32(data.data(), rec + 32 * e + 16)));
					tris.push_back(double(f32(data.data(), rec + 32 * e + 8)));
				}
			}
			return true;
		}

		// One band laid whole: the meshes; the collision where they cover nothing; the water where it's above what's
		// there
		struct sources
		{
			const grid *g;
			const meshes *ms;
			const collision::reader *rd;
			const std::vector<double> *water;
		};

		void lay_band(const sources &s, band &seen, band &other, std::vector<float> &world, std::vector<double> &uvq)
		{
			const grid &g = *s.g;
			lay_meshes(g, *s.ms, seen, world, uvq);
			// (the collision only where no mesh is: the Python lays it all and takes it there)
			other.reset(seen.r0, seen.r1, g.width, false);
			collision::each(*s.rd, [&](const collision::triangle &t) {
				for (const auto &p : t.p)
					if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
						return;
				const uint8_t what = (t.terrain == collision::kShallow || t.terrain == collision::kDeep) ? kWater : (facing_up(t.p) ? kGround : kStops);
				lay(g, other, t.p, what, nullptr, 0, &seen.top);
			});
			for (size_t i = 0; i < seen.top.size(); ++i)
				if (!std::isfinite(seen.top[i]) && std::isfinite(other.top[i]))
				{
					seen.top[i] = other.top[i];
					seen.kind[i] = other.kind[i];
				}
			// The water, where it's above what's there
			if (!s.water->empty())
			{
				other.reset(seen.r0, seen.r1, g.width, false);
				const std::vector<double> &w = *s.water;
				for (size_t t = 0; t + 9 <= w.size(); t += 9)
				{
					const double P[3][3] = { { w[t], w[t + 1], w[t + 2] }, { w[t + 3], w[t + 4], w[t + 5] }, { w[t + 6], w[t + 7], w[t + 8] } };
					lay(g, other, P, kWater, nullptr, 0);
				}
				for (size_t i = 0; i < seen.top.size(); ++i)
					if (other.top[i] < seen.top[i] + 1e-6)
					{
						seen.top[i] = other.top[i];
						seen.kind[i] = kWater;
					}
			}
			for (size_t i = 0; i < seen.top.size(); ++i)
				if (!std::isfinite(seen.top[i]))
					seen.kind[i] = kNothing;
		}
	}

	namespace
	{
		// The first pass's heights, kept for the second: in memory for a small grid, else in a temporary file (gone
		// when closed); ok() false when neither (the bands are laid again then)
		class kept_heights
		{
		public:
			explicit kept_heights(size_t squares)
			{
				if (squares <= kKeepSquares)
				{
					mem_.resize(squares);
					ok_ = true;
					return;
				}
				wchar_t folder[MAX_PATH], path[MAX_PATH];
				if (GetTempPathW(MAX_PATH, folder) == 0 || GetTempFileNameW(folder, L"hfx", 0, path) == 0)
					return;
				file_ = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
					FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
				ok_ = file_ != INVALID_HANDLE_VALUE;
				if (!ok_)
					DeleteFileW(path);
			}
			~kept_heights()
			{
				if (file_ != INVALID_HANDLE_VALUE)
					CloseHandle(file_);
			}
			kept_heights(const kept_heights &) = delete;
			kept_heights &operator=(const kept_heights &) = delete;

			bool ok() const { return ok_; }

			// The heights of squares [at, at + n): kept (in order, band after band), then read back
			bool put(size_t at, const double *top, size_t n)
			{
				if (!mem_.empty())
				{
					std::copy(top, top + n, mem_.begin() + ptrdiff_t(at));
					return true;
				}
				return ok_ = ok_ && io(at, const_cast<double *>(top), n, true);
			}
			bool get(size_t at, double *top, size_t n)
			{
				if (!mem_.empty())
				{
					std::copy(mem_.begin() + ptrdiff_t(at), mem_.begin() + ptrdiff_t(at + n), top);
					return true;
				}
				return ok_ = ok_ && io(at, top, n, false);
			}

		private:
			std::vector<double> mem_;
			HANDLE file_ = INVALID_HANDLE_VALUE;
			bool ok_ = false;

			bool io(size_t at, double *top, size_t n, bool write)
			{
				LARGE_INTEGER to;
				to.QuadPart = LONGLONG(at) * 8;
				if (!SetFilePointerEx(file_, to, nullptr, FILE_BEGIN))
					return false;
				const DWORD bytes = DWORD(n * 8);
				DWORD done = 0;
				const BOOL r = write ? WriteFile(file_, top, bytes, &done, nullptr) : ReadFile(file_, top, bytes, &done, nullptr);
				return r && done == bytes;
			}
		};
	}

	namespace
	{
		bool bake_rain(uint32_t zone, const std::wstring &out, const std::wstring &water_file, result &r)
		{
			std::vector<uint8_t> dat;
			if (!collision::read_zone(zone, dat, r))
				return false;
			if (!collision::rainy(dat, true))
			{
				r.none = true;
				r.why = "no rainy or snowy weather";
				return false;
			}
			collision::reader rd;
			if (!collision::open(dat, rd, r))
				return false;

			// The grid: the collision's extent (its finite triangles) and kMargin yalms round it
			size_t all = 0, finite = 0;
			double lo[3] = { kInf, kInf, kInf }, hi[3] = { -kInf, -kInf, -kInf };
			collision::each(rd, [&](const collision::triangle &t) {
				all++;
				for (const auto &p : t.p)
					if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
						return;
				finite++;
				for (const auto &p : t.p)
					for (int k = 0; k < 3; ++k)
					{
						lo[k] = std::min(lo[k], p[k]);
						hi[k] = std::max(hi[k], p[k]);
					}
			});
			if (all == 0 || finite == 0)
			{
				r.none = true;
				r.why = "the zone has no collision";
				return false;
			}
			grid g;
			g.x0 = std::floor((lo[0] - kMargin) / kSquare) * kSquare;
			g.z0 = std::floor((lo[2] - kMargin) / kSquare) * kSquare;
			g.width = int64_t(std::ceil((hi[0] + kMargin - g.x0) / kSquare));
			g.height = int64_t(std::ceil((hi[2] + kMargin - g.z0) / kSquare));
			const size_t squares = size_t(g.width * g.height);

			// The meshes (none when the Python's bake fails: the collision alone, as there)
			meshes ms;
			{
				std::string why;
				if (!load_meshes(dat, rd.m, ms, why))
					ms = meshes();
			}
			dat = std::vector<uint8_t>();
			std::vector<double> water;
			if (!water_file.empty() && GetFileAttributesW(water_file.c_str()) != INVALID_FILE_ATTRIBUTES)
			{
				std::string why;
				if (!read_water(water_file, water, why))
				{
					r.why = why;
					return false;
				}
			}

			// First pass: every band laid, what each square is kept, and the heights' range
			std::vector<uint8_t> planes(squares * 3);
			const int64_t rows = std::max<int64_t>(1, int64_t(kBandSquares) / g.width);
			kept_heights kept(squares);
			const sources src{ &g, &ms, &rd, &water };
			band seen, other;
			std::vector<float> world;
			std::vector<double> uvq;
			double ymin = kInf, ymax = -kInf;
			uint32_t count = 0;
			for (int64_t r0 = 0; r0 < g.height; r0 += rows)
			{
				seen.reset(r0, std::min(g.height, r0 + rows), g.width, true);
				lay_band(src, seen, other, world, uvq);
				const size_t at = size_t(r0 * g.width);
				for (size_t i = 0; i < seen.top.size(); ++i)
				{
					planes[at + i] = seen.kind[i];
					if (std::isfinite(seen.top[i]))
					{
						count++;
						if (seen.top[i] < ymin)
							ymin = seen.top[i];
						if (seen.top[i] > ymax)
							ymax = seen.top[i];
					}
				}
				kept.put(at, seen.top.data(), seen.top.size());
			}
			if (count == 0)
			{
				r.none = true;
				r.why = "nothing rain stops on";
				return false;
			}

			// The heights in steps: y0 + q * step, q 0..16383 (half a yalm, rounded up, where nothing splashes)
			const double y0 = std::floor(ymin * 16.0) / 16.0;
			const double span = (ymax - y0) / 16383.0;
			const double step = span > kStepQ ? span : kStepQ;
			const int64_t coarse = std::max<int64_t>(1, int64_t(std::nearbyint(kCoarse / step)));
			uint8_t *deltas = planes.data() + squares;
			for (int64_t r0 = 0; r0 < g.height; r0 += rows)
			{
				const int64_t r1 = std::min(g.height, r0 + rows);
				seen.reset(r0, r1, g.width, true);
				if (!kept.get(size_t(r0 * g.width), seen.top.data(), seen.top.size()))
				{
					seen.reset(r0, r1, g.width, true);
					lay_band(src, seen, other, world, uvq);
				}
				const double *top = seen.top.data();
				for (int64_t j = r0; j < r1; ++j)
				{
					int32_t before = 0;
					for (int64_t i = 0; i < g.width; ++i)
					{
						const size_t sq = size_t(j * g.width + i);
						const double y = top[sq - size_t(r0 * g.width)];
						int32_t q = 0;
						if (std::isfinite(y))
						{
							const double e = std::nearbyint((y - y0) / step);
							q = int32_t(e < 0.0 ? 0.0 : (e > 16383.0 ? 16383.0 : e));
							if (planes[sq] == kStops)
								q = int32_t(q / coarse * coarse);
						}
						const uint16_t dq = uint16_t(q - before);
						deltas[2 * sq] = uint8_t(dq & 0xFF);
						deltas[2 * sq + 1] = uint8_t(dq >> 8);
						before = q;
					}
				}
			}
			seen = band();
			other = band();
			ms = meshes();
			rd = collision::reader();

			const std::vector<uint8_t> packed = xpress(planes.data(), planes.size());
			planes = std::vector<uint8_t>();
			if (packed.empty())
			{
				r.why = "XPRESS packing failed";
				return false;
			}
			std::vector<uint8_t> file(48 + packed.size());
			std::memcpy(file.data(), "RHM1", 4);
			const float fsq = float(kSquare), fx0 = float(g.x0), fz0 = float(g.z0), fy0 = float(y0), fstep = float(step);
			const uint32_t w = uint32_t(g.width), h = uint32_t(g.height), packing = kXpressHuff, size = uint32_t(packed.size());
			std::memcpy(&file[4], &zone, 4);
			std::memcpy(&file[8], &fsq, 4);
			std::memcpy(&file[12], &w, 4);
			std::memcpy(&file[16], &count, 4);
			std::memcpy(&file[20], &h, 4);
			std::memcpy(&file[24], &fx0, 4);
			std::memcpy(&file[28], &fz0, 4);
			std::memcpy(&file[32], &fy0, 4);
			std::memcpy(&file[36], &fstep, 4);
			std::memcpy(&file[40], &packing, 4);
			std::memcpy(&file[44], &size, 4);
			std::memcpy(&file[48], packed.data(), packed.size());
			if (!write_file(out, file.data(), file.size()))
			{
				r.why = "the file couldn't be written";
				return false;
			}
			return true;
		}
	}

	bool rain(uint32_t zone, const std::wstring &out, const std::wstring &water_file, result &r)
	{
		try
		{
			return bake_rain(zone, out, water_file, r);
		}
		catch (const std::bad_alloc &)
		{
			r = result();
			r.why = "out of memory";
			return false;
		}
	}
}
