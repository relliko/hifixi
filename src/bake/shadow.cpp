/*
 * The sun shadow map's zone file (bake.hpp: shadow): reshade-worldfx's tools/bake_shadow_geo.py as it bakes by default
 * (bake_compact + write_compact: the compact 'SHD2' file), in C++, the same bytes. The zone's visual meshes, for the
 * add-on's sun shadow map: every model the zone's MZB places (its object list), from the zone DAT's own MMB meshes.
 *
 * What's in it (the Python's notes):
 * - Placement. The MZB's object list (0x64-byte records after its header: a 16-byte model name, translation, rotation
 *   in radians, scale, then four distances and eight words not used here). World = scale x Rx x Ry x Rz + translation,
 *   row vectors, angles as stored. Records naming a model that isn't in this DAT (invisible collision walls,
 *   "hitwall_*") are skipped.
 * - Meshes. MMB chunks (type 0x2E), unscrambled in two layers (both key tables are the game's own, found in
 *   FFXiMain.dll: key_tables), are triangle strips: each model's indices make a strip whose degenerate triangles (a
 *   repeated corner) join its runs. Vertices are 36 bytes (48 when the header's byte 4 is 2): position, normal,
 *   colour, UV.
 * - What casts. Blended models (their blending word's bit 0x8000: water sheets, the ground's decals, glows) don't.
 *   The rest do. Alpha testing is per placed object, by its MZB record name (one starting with '_'); every other
 *   object draws its textures solid. The zone pass tests ALPHAREF 0x60 GREATER against texture alpha x vertex alpha x
 *   4, the vertex alpha being 255: a texel passes when 4 x its alpha > 0x60. '_' objects cast with their leaves'
 *   shape: each of their triangles carries its texture coordinates and which cut-out texture it uses, and the file
 *   holds those textures' alpha as bits. One whose texture has no texel failing the test is written solid.
 *
 * The file ('SHD2'), little-endian: each MMB's casting triangles stored once in its own space and the MZB's placements
 * beside them, packed with Windows' XPRESS (dat.hpp's xpress: the same ntdll call, so the same bytes); the add-on
 * expands it into the triangle soup (SHD1) when the zone loads.
 *     header      'SHD2', u32 zone, f32 cell size, u32 models, u32 model triangles, u32 placements, u32 masks,
 *                 u32 mask words, u32 packing (4: XPRESS with Huffman; 0: stored), u32 packed bytes, u32 raw bytes,
 *                 u32 triangles once expanded, 4 x u32 0 (64 bytes)
 *     then, packed:
 *     models      u32 first triangle, u32 count: each placed MMB's casting triangles (blended models left out)
 *     triangles   48 bytes each: three corners, each f32 x, f32 y, f32 z, u32 w, in the model's own space; w is the
 *                 cut-out word whenever the texture has a cut-out mask (one placed with alpha testing used it), else
 *                 0: u | v << 12 | mask << 24, u and v that corner's texture coordinates as 12-bit fractions, mask the
 *                 cut-out texture's index + 1
 *     placements  56 bytes each, in the MZB's order: u32 model, u32 flags (bit 0: alpha-tested, its name starts with
 *                 '_'), f32 m[3][3] (scale x Rx x Ry x Rz, row vectors), f32 t[3]: world = corner m + t
 *     masks       4 words each (width, height, first word, words a row), then the words: each cut-out texture's
 *                 alpha, a bit a texel (1: drawn), rows top first, bit x & 31 of word x >> 5
 *
 * Where the Python's numbers come from numpy, the same arithmetic: the rotation is numpy's 3x3 products (OpenBLAS's
 * dgemm: each entry a chain of fused multiply-adds, k from 0), then rounded to float32; texture coordinates are
 * clipped, x 4095 and rounded half to even in doubles. The one thing not the same: cos and sin here are the 32-bit
 * CRT's, the Python's the 64-bit UCRT's, which differ by an ulp for about 3% of angles (and differ from themselves
 * with FMA3 on or off). Rounded to float32, that could change 39 of the 151,387 distinct rotation records in the
 * sunlit zones' MZBs, and changes none: every zone's file is the Python's, FMA3 on or off (2026-10-22,
 * bake2_test --all).
 */

#include "bake.hpp"
#include "dat.hpp"
#include "mmb.hpp"
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace hifi::bake
{
	namespace
	{
		constexpr float kCell = 32.0f;
		constexpr uint32_t kBlended = 0x8000;
		constexpr uint32_t kAlphaRef = 0x60; // (the zone pass's ALPHAREF, GREATER, against texture alpha x vertex alpha x 4)
		constexpr uint32_t kPacking = 4;     // (XPRESS with Huffman)
		constexpr size_t kMasksMax = 255;

		using mmb::f32;
		using mmb::u16;
		using mmb::u32;
		using mmb::decrypt_mmb;

		// One model of an MMB: its texture's name (16 bytes), blending word, vertices (`stride` bytes each: position
		// first, UV last) and strip indices
		struct mesh
		{
			const uint8_t *tex;
			uint32_t blend;
			const uint8_t *verts;
			uint32_t count, stride;
			const uint8_t *index;
			uint32_t indices;
		};

		// The MMB's models (mmb_models), in its order; false where the Python's struct.unpack_from raises (the MMB then
		// gives nothing)
		bool mmb_models(const uint8_t *b, size_t size, std::vector<mesh> &out)
		{
			out.clear();
			const uint32_t stride = b[4] == 2 ? 48 : 36;
			uint32_t pieces = 0, ptr = 0;
			if (!u32(b, size, 32, pieces) || !u32(b, size, 60, ptr))
				return false;
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
						uint32_t o = 0;
						u32(b, size, 64 + i * 4, o);
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
					uint32_t v = 0;
					if (!u32(b, size, o, v))
						return false;
					if (v != 0 && v != ptr)
						offs.push_back(v);
				}
			}
			for (const uint32_t po : offs)
			{
				if (uint64_t(po) + 32 > size)
					continue;
				uint32_t n = 0;
				u32(b, size, po, n);
				if (n > 100)
					continue;
				uint64_t m = uint64_t(po) + 32;
				for (uint32_t k = 0; k < n; ++k)
				{
					if (m + 20 > size)
						break;
					uint32_t vc = 0, blend = 0, ic = 0;
					u16(b, size, m + 16, vc);
					u16(b, size, m + 18, blend);
					const uint64_t vstart = m + 20, vend = vstart + uint64_t(vc) * stride;
					if (vend + 4 > size)
						break;
					u16(b, size, vend, ic);
					const uint64_t istart = vend + 4;
					if (istart + uint64_t(ic) * 2 > size)
						break;
					out.push_back({ b + m, blend, b + vstart, vc, stride, b + istart, ic });
					m = istart + uint64_t(ic) * 2 + ((ic & 1) != 0 ? 2 : 0);
				}
			}
			return true;
		}

		// The strip cut into triangles without the degenerate ones (a repeated corner, or one past the vertices), the
		// winding kept: corner indices, three a triangle
		void strip_triangles(const mesh &s, std::vector<uint32_t> &tri)
		{
			tri.clear();
			if (s.indices < 3)
				return;
			auto at = [&](uint32_t i) { return uint32_t(s.index[i * 2] | (s.index[i * 2 + 1] << 8)); };
			for (uint32_t k = 0; k + 2 < s.indices; ++k)
			{
				const uint32_t a = at(k), bb = at(k + 1), c = at(k + 2);
				const bool odd = (k & 1) == 1;
				const uint32_t t0 = odd ? bb : a, t1 = odd ? a : bb, t2 = c;
				if (t0 != t1 && t1 != t2 && t0 != t2 && t0 < s.count && t1 < s.count && t2 < s.count)
				{
					tri.push_back(t0);
					tri.push_back(t1);
					tri.push_back(t2);
				}
			}
		}

		// numpy's 3x3 product of doubles (OpenBLAS's dgemm): each entry fused multiply-adds over k from 0, from +0
		void matmul(const double a[9], const double b[9], double out[9])
		{
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
				{
					double acc = 0.0;
					for (int k = 0; k < 3; ++k)
						acc = std::fma(a[i * 3 + k], b[k * 3 + j], acc);
					out[i * 3 + j] = acc;
				}
		}

		// Scale x Rx x Ry x Rz (row vectors: v' = v M + t), multiplied left to right as the Python does, as float32
		void rotation(double rx, double ry, double rz, double sx, double sy, double sz, float out[9])
		{
			const double cx = std::cos(rx), snx = std::sin(rx);
			const double cy = std::cos(ry), sny = std::sin(ry);
			const double cz = std::cos(rz), snz = std::sin(rz);
			const double s[9] = { sx, 0.0, 0.0, 0.0, sy, 0.0, 0.0, 0.0, sz };
			const double x[9] = { 1.0, 0.0, 0.0, 0.0, cx, snx, 0.0, -snx, cx };
			const double y[9] = { cy, 0.0, -sny, 0.0, 1.0, 0.0, sny, 0.0, cy };
			const double z[9] = { cz, snz, 0.0, -snz, cz, 0.0, 0.0, 0.0, 1.0 };
			double sx_[9], sxy[9], m[9];
			matmul(s, x, sx_);
			matmul(sx_, y, sxy);
			matmul(sxy, z, m);
			for (int i = 0; i < 9; ++i)
				out[i] = float(m[i]);
		}

		// A DXT3 texture of the DAT's (IMG chunks, type 0x20, flag 0xA1: the zone's meshes use only these): its body
		// and size
		struct texture
		{
			const uint8_t *body;
			uint32_t w, h;
		};

		// A cut-out texture's alpha as bits
		struct mask
		{
			uint32_t w, h, row_words;
			std::vector<uint32_t> words;
		};

		// A placed model's part that casts: its texture's name and the mesh's triangles
		struct part
		{
			std::string tex;
			const mesh *m;
			std::vector<uint32_t> tri;
		};

		struct placement
		{
			uint32_t model;
			bool tested;
			float m[9];
			uint8_t t[12]; // (the record's translation, its float32 bits as they are)
		};

		// A texture coordinate as a 12-bit fraction: clip(round(clip(uv, 0, 1) x 4095), 0, 4095), as uint32 (NaN: 0, as
		// numpy casts it)
		uint32_t fraction(float uv)
		{
			double v = double(uv);
			if (std::isnan(v))
				return 0;
			v = v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
			double r = std::nearbyint(v * 4095.0);
			r = r < 0.0 ? 0.0 : r > 4095.0 ? 4095.0 : r;
			return uint32_t(r);
		}

		void put32(std::vector<uint8_t> &to, size_t &at, uint32_t v)
		{
			std::memcpy(to.data() + at, &v, 4);
			at += 4;
		}

		bool fail(result &r, const char *why)
		{
			r.why = why;
			return false;
		}
	}

	bool shadow(uint32_t zone, const std::wstring &out, result &r)
	{
		r = result();
		std::string why;
		const std::wstring path = zone_dat_path(zone, why);
		if (path.empty())
			return fail(r, why.c_str());
		std::vector<uint8_t> dat;
		if (!read_file(path, dat))
			return fail(r, "the zone's DAT can't be read");
		const uint8_t *first = nullptr, *second = nullptr;
		if (!key_tables(first, second))
			return fail(r, "the game's key tables weren't found");
		const std::vector<chunk> all = chunks(dat);

		// The MMBs, unscrambled in place (each chunk's body is its own: they don't overlap) and read; the first with a
		// name wins, and only one that has models
		std::unordered_map<std::string, std::vector<mesh>> lib;
		{
			std::vector<mesh> ms;
			for (const chunk &c : all)
			{
				if (c.type != 0x2E || c.size <= 64)
					continue;
				uint8_t *b = dat.data() + (c.body - dat.data());
				decrypt_mmb(b, c.size, first, second);
				if (!mmb_models(b, c.size, ms) || ms.empty())
					continue;
				lib.emplace(std::string(reinterpret_cast<const char *>(b + 16), 16), ms);
			}
		}

		std::vector<uint8_t> m;
		if (!find_chunk(dat, 0x1C, m))
		{
			r.none = true;
			r.why = "no MZB in the zone DAT";
			return false;
		}
		if (!mmb::decrypt_mzb(m, first))
			return fail(r, "the MZB is too short");

		// The DAT's DXT3 textures by name (the first of a name that's whole); their alpha is decoded only for the masks
		// (the Python decodes them all: one whose size isn't a multiple of 4 fails the zone there, so here too)
		std::unordered_map<std::string, texture> tex;
		for (const chunk &c : all)
		{
			const uint8_t *b = c.body;
			if (c.type != 0x20 || c.size < 69 || b[0] != 0xA1 || std::memcmp(b + 57, "3TXD", 4) != 0)
				continue;
			std::string name(reinterpret_cast<const char *>(b + 1), 16);
			if (tex.count(name) != 0)
				continue;
			int32_t w = 0, h = 0;
			std::memcpy(&w, b + 21, 4);
			std::memcpy(&h, b + 25, 4);
			if (w <= 0 || h <= 0)
				continue;
			const int64_t n = int64_t(w / 4) * int64_t(h / 4);
			if (69 + n * 16 > int64_t(c.size))
				continue;
			if (w % 4 != 0 || h % 4 != 0)
				return fail(r, "a texture's size isn't a multiple of 4");
			tex.emplace(std::move(name), texture{ b, uint32_t(w), uint32_t(h) });
		}

		// Masks, assigned in the order the alpha-tested placements meet their textures (as SHD1's bake does): a
		// texture's index + 1, 0 when alpha testing cuts nothing out of it (or it isn't here)
		std::unordered_map<std::string, uint32_t> mask_ids;
		std::vector<mask> masks;
		bool too_many = false;
		auto mask_id = [&](const std::string &name) -> uint32_t {
			const auto known = mask_ids.find(name);
			if (known != mask_ids.end())
				return known->second;
			uint32_t id = 0;
			const auto t = tex.find(name);
			if (t != tex.end())
			{
				const texture &T = t->second;
				mask k;
				k.w = T.w;
				k.h = T.h;
				k.row_words = (T.w + 31) / 32;
				k.words.assign(size_t(k.row_words) * T.h, 0);
				bool all_drawn = true;
				const uint32_t bw = T.w / 4, bh = T.h / 4;
				for (uint32_t by = 0; by < bh; ++by)
					for (uint32_t bx = 0; bx < bw; ++bx)
					{
						// (the block's alpha: 16 nibbles, x 17, row by row)
						uint64_t a = 0;
						std::memcpy(&a, T.body + 69 + (size_t(by) * bw + bx) * 16, 8);
						for (uint32_t j = 0; j < 16; ++j)
						{
							const uint32_t alpha = uint32_t((a >> (4 * j)) & 15) * 17;
							const uint32_t x = bx * 4 + (j & 3), y = by * 4 + (j >> 2);
							if (alpha * 4 > kAlphaRef)
								k.words[size_t(y) * k.row_words + (x >> 5)] |= 1u << (x & 31);
							else
								all_drawn = false;
						}
					}
				if (!all_drawn)
				{
					masks.push_back(std::move(k));
					id = uint32_t(masks.size());
					if (masks.size() > kMasksMax)
						too_many = true;
				}
			}
			mask_ids.emplace(name, id);
			return id;
		};

		// The object list: each record naming a model here places it; each model's casting parts made at its first
		// placement
		uint32_t n = 0;
		u32(m.data(), m.size(), 4, n);
		n &= 0xFFFFFF;
		std::unordered_map<std::string, uint32_t> model_of;
		std::vector<std::vector<part>> models;
		std::vector<placement> placements;
		for (uint32_t i = 0; i < n; ++i)
		{
			const size_t a = 0x20 + size_t(i) * 0x64;
			if (a + 0x64 > m.size())
				break;
			const std::string name(reinterpret_cast<const char *>(m.data() + a), 16);
			float t[9];
			std::memcpy(t, m.data() + a + 16, sizeof(t));
			const auto found = lib.find(name);
			if (found == lib.end())
				continue;
			bool finite = true;
			for (const float v : t)
				finite = finite && std::isfinite(v);
			if (!finite)
				continue;
			auto known = model_of.find(name);
			if (known == model_of.end())
			{
				std::vector<part> parts;
				for (const mesh &s : found->second)
				{
					part p;
					strip_triangles(s, p.tri);
					if (p.tri.empty() || (s.blend & kBlended) != 0)
						continue;
					p.tex.assign(reinterpret_cast<const char *>(s.tex), 16);
					p.m = &s;
					parts.push_back(std::move(p));
				}
				known = model_of.emplace(name, uint32_t(models.size())).first;
				models.push_back(std::move(parts));
			}
			const bool tested = name[0] == '_';
			if (tested)
				for (const part &p : models[known->second])
				{
					mask_id(p.tex);
					if (too_many)
						return fail(r, "more than 255 cut-out textures");
				}
			placement pl;
			pl.model = known->second;
			pl.tested = tested;
			rotation(double(t[3]), double(t[4]), double(t[5]), double(t[6]), double(t[7]), double(t[8]), pl.m);
			std::memcpy(pl.t, m.data() + a + 16, 12);
			placements.push_back(pl);
		}
		if (placements.empty())
		{
			r.none = true;
			r.why = "nothing placed casts";
			return false;
		}

		// The body, as write_compact lays it out: model ranges, the models' triangles, the placements, the masks
		size_t triangles = 0, words = 0;
		for (const std::vector<part> &parts : models)
			for (const part &p : parts)
				triangles += p.tri.size() / 3;
		for (const mask &k : masks)
			words += k.words.size();
		uint64_t expanded = 0;
		std::vector<uint8_t> raw(models.size() * 8 + triangles * 48 + placements.size() * 56 + masks.size() * 16 + words * 4);
		size_t at = 0;
		{
			uint32_t first_tri = 0;
			for (const std::vector<part> &parts : models)
			{
				uint32_t count = 0;
				for (const part &p : parts)
					count += uint32_t(p.tri.size() / 3);
				put32(raw, at, first_tri);
				put32(raw, at, count);
				first_tri += count;
			}
		}
		for (const std::vector<part> &parts : models)
			for (const part &p : parts)
			{
				const auto k = mask_ids.find(p.tex);
				const uint32_t id = k != mask_ids.end() ? k->second : 0;
				const mesh &s = *p.m;
				for (const uint32_t v : p.tri)
				{
					const uint8_t *vert = s.verts + size_t(v) * s.stride;
					std::memcpy(raw.data() + at, vert, 12);
					at += 12;
					uint32_t w = 0;
					if (id != 0)
					{
						float u = 0.0f, vv = 0.0f;
						std::memcpy(&u, vert + s.stride - 8, 4);
						std::memcpy(&vv, vert + s.stride - 4, 4);
						w = fraction(u) | (fraction(vv) << 12) | (id << 24);
					}
					put32(raw, at, w);
				}
			}
		{
			std::vector<uint32_t> counts;
			for (const std::vector<part> &parts : models)
			{
				uint32_t count = 0;
				for (const part &p : parts)
					count += uint32_t(p.tri.size() / 3);
				counts.push_back(count);
			}
			for (const placement &pl : placements)
			{
				put32(raw, at, pl.model);
				put32(raw, at, pl.tested ? 1 : 0);
				std::memcpy(raw.data() + at, pl.m, 36);
				at += 36;
				std::memcpy(raw.data() + at, pl.t, 12);
				at += 12;
				expanded += counts[pl.model];
			}
		}
		{
			uint32_t first_word = 0;
			for (const mask &k : masks)
			{
				put32(raw, at, k.w);
				put32(raw, at, k.h);
				put32(raw, at, first_word);
				put32(raw, at, k.row_words);
				first_word += uint32_t(k.words.size());
			}
			for (const mask &k : masks)
			{
				std::memcpy(raw.data() + at, k.words.data(), k.words.size() * 4);
				at += k.words.size() * 4;
			}
		}

		// Packed, unless packing doesn't make it smaller
		std::vector<uint8_t> packed = xpress(raw.data(), raw.size());
		if (packed.empty())
			return fail(r, "XPRESS packing failed");
		uint32_t packing = kPacking;
		const std::vector<uint8_t> *body = &packed;
		if (packed.size() >= raw.size())
		{
			packing = 0;
			body = &raw;
		}

		std::vector<uint8_t> file(64 + body->size());
		at = 0;
		std::memcpy(file.data(), "SHD2", 4);
		at = 4;
		uint32_t cell = 0;
		std::memcpy(&cell, &kCell, 4);
		const uint32_t header[15] = { zone, cell, uint32_t(models.size()), uint32_t(triangles), uint32_t(placements.size()), uint32_t(masks.size()),
			uint32_t(words), packing, uint32_t(body->size()), uint32_t(raw.size()), uint32_t(expanded), 0, 0, 0, 0 };
		for (const uint32_t v : header)
			put32(file, at, v);
		std::memcpy(file.data() + 64, body->data(), body->size());
		if (!write_file(out, file.data(), file.size()))
			return fail(r, "the file couldn't be written");
		return true;
	}
}
