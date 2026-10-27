/*
 * A zone's meshes, textures and normal maps for the native zone renderer (zonemesh.hpp has what's kept and why;
 * research/ffxi-re/notes-zone.md the formats). The MMB walk is bake_shadow_geo.py's (shadow.cpp's), with the vertex
 * count as the game reads it (the count dword's bits 0-27; bits 28-31 the piece bits) and kind 2's indices as the
 * triangle lists they are.
 */

#include "zonemesh.hpp"
#include "dat.hpp"
#include "mmb.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace hifi::bake::zonemesh
{
	namespace
	{
		using mmb::u16;
		using mmb::u32;

		// One piece as the MMB has it
		struct raw_piece
		{
			const uint8_t *tex;
			uint32_t bits, count;
			const uint8_t *verts;
			const uint8_t *index;
			uint32_t indices;
			bool first; // (the first of its group)
		};

		// The MMB's pieces (every group), in file order; false where a read would pass its end
		bool mmb_pieces(const uint8_t *b, size_t size, std::vector<raw_piece> &out)
		{
			out.clear();
			const uint32_t stride = b[4] == 2 ? 48 : 36;
			uint32_t groups = 0, ptr = 0;
			if (!u32(b, size, 32, groups) || !u32(b, size, 60, ptr))
				return false;
			std::vector<uint32_t> offs;
			if (ptr == 0)
			{
				if (groups == 0)
					offs.push_back(64);
				else
				{
					if (64 + 32 > size)
						return false;
					for (uint32_t i = 0; i < 8; ++i)
					{
						uint32_t o = 0;
						u32(b, size, 64 + i * 4, o);
						if (o != 0 && offs.size() < groups)
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
					uint32_t word = 0, ic = 0;
					u32(b, size, m + 16, word);
					const uint32_t vc = word & 0x0FFFFFFF;
					const uint64_t vstart = m + 20, vend = vstart + uint64_t(vc) * stride;
					if (vend + 4 > size)
						break;
					u16(b, size, vend, ic);
					const uint64_t istart = vend + 4;
					if (istart + uint64_t(ic) * 2 > size)
						break;
					out.push_back({ b + m, word >> 28, vc, b + vstart, b + istart, ic, k == 0 });
					m = istart + uint64_t(ic) * 2 + ((ic & 1) != 0 ? 2 : 0);
				}
			}
			return true;
		}

		// A strip's triangles without the degenerate ones (the winding kept: odd ones swapped), or a list's; a corner
		// past the vertices drops its triangle. Appended, plus base.
		void triangles(const raw_piece &p, bool strip, uint32_t base, std::vector<uint32_t> &out)
		{
			auto at = [&](uint32_t i) { return uint32_t(p.index[i * 2] | (p.index[i * 2 + 1] << 8)); };
			auto add = [&](uint32_t a, uint32_t b, uint32_t c) {
				if (a != b && b != c && a != c && a < p.count && b < p.count && c < p.count)
				{
					out.push_back(base + a);
					out.push_back(base + b);
					out.push_back(base + c);
				}
			};
			if (p.indices < 3)
				return;
			if (strip)
				for (uint32_t k = 0; k + 2 < p.indices; ++k)
				{
					const uint32_t a = at(k), b = at(k + 1), c = at(k + 2);
					if ((k & 1) != 0)
						add(b, a, c);
					else
						add(a, b, c);
				}
			else
				for (uint32_t k = 0; k + 2 < p.indices; k += 3)
					add(at(k), at(k + 1), at(k + 2));
		}

		vertex vertex_of(const uint8_t *v, bool swaying)
		{
			vertex out = {};
			if (swaying)
			{
				// (position, sway vector, normal, colour, UV)
				std::memcpy(out.pos, v, 12);
				std::memcpy(out.sway, v + 12, 12);
				std::memcpy(out.normal, v + 24, 12);
				std::memcpy(&out.colour, v + 36, 4);
				std::memcpy(out.uv, v + 40, 8);
			}
			else
			{
				// (position, normal, colour, UV)
				std::memcpy(out.pos, v, 12);
				std::memcpy(out.normal, v + 12, 12);
				std::memcpy(&out.colour, v + 24, 4);
				std::memcpy(out.uv, v + 28, 8);
			}
			return out;
		}

		// An image chunk (type 0x20) the zone's pieces can name
		struct image
		{
			const uint8_t *body;
			size_t size;
			uint32_t w, h, format;
		};

		// Scale x Rx x Ry x Rz, row vectors (bake_shadow_geo.rotation)
		void rotation(const float r[3], const float s[3], float out[9])
		{
			const double cx = std::cos(double(r[0])), sx = std::sin(double(r[0]));
			const double cy = std::cos(double(r[1])), sy = std::sin(double(r[1]));
			const double cz = std::cos(double(r[2])), sz = std::sin(double(r[2]));
			const double X[9] = { 1, 0, 0, 0, cx, sx, 0, -sx, cx };
			const double Y[9] = { cy, 0, -sy, 0, 1, 0, sy, 0, cy };
			const double Z[9] = { cz, sz, 0, -sz, cz, 0, 0, 0, 1 };
			auto mul = [](const double *a, const double *b, double *o) {
				for (int i = 0; i < 3; ++i)
					for (int j = 0; j < 3; ++j)
						o[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
			};
			double xy[9], xyz[9];
			mul(X, Y, xy);
			mul(xy, Z, xyz);
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					out[i * 3 + j] = float(double(s[i]) * xyz[i * 3 + j]);
		}

		bool fail(result &r, const char *why)
		{
			r.why = why;
			return false;
		}
	}

	uint32_t map_dat(uint32_t map)
	{
		return map < 600 ? map + 100 : map + 0x144F7;
	}

	uint32_t bump_dat(uint32_t map)
	{
		return map < 600 ? map + 0x9B97 : map + 0x14817;
	}

	uint32_t zone::find(const char name[16]) const
	{
		for (uint32_t i = 0; i < models.size(); ++i)
			if (std::memcmp(models[i].name, name, 16) == 0)
				return i;
		return kNone;
	}

	size_t zone::bytes() const
	{
		size_t n = vertices.size() * sizeof(vertex) + indices.size() * 4;
		for (const texture &t : textures)
			n += t.data.size();
		for (const normal_map &m : normal_maps)
			n += m.texels.size() * 4;
		return n;
	}

	void sobel(const uint8_t *h, uint32_t w, uint32_t ht, uint32_t *out)
	{
		for (uint32_t y = 0; y < ht; ++y)
		{
			const uint8_t *up = h + size_t((y + ht - 1) % ht) * w, *mid = h + size_t(y) * w, *dn = h + size_t((y + 1) % ht) * w;
			for (uint32_t x = 0; x < w; ++x)
			{
				const uint32_t l = (x + w - 1) % w, r = (x + 1) % w;
				const int gy = (up[l] + 2 * up[x] + up[r]) - (dn[l] + 2 * dn[x] + dn[r]);
				const int gx = (up[l] + 2 * mid[l] + dn[l]) - (up[r] + 2 * mid[r] + dn[r]);
				// (as the game: x87, 127 / sqrt, products truncated toward zero)
				const double k = 127.0 / std::sqrt(double(gx * gx + gy * gy + 0xFE01));
				const uint32_t R = uint32_t(int(gy * k) + 0x80), G = uint32_t(int(gx * k) + 0x80), B = uint32_t(0x80 - int(-255.0 * k));
				out[size_t(y) * w + x] = (R & 0xFF) << 16 | (G & 0xFF) << 8 | (B & 0xFF);
			}
		}
	}

	bool load(uint32_t map, zone &z, result &r)
	{
		r = result();
		z = zone();
		z.map = map;
		std::string why;
		const std::wstring path = dat_path(map_dat(map), why);
		if (path.empty())
		{
			r.none = true;
			r.why = why;
			return false;
		}
		std::vector<uint8_t> dat;
		if (!read_file(path, dat))
			return fail(r, "the map's DAT can't be read");
		const uint8_t *first = nullptr, *second = nullptr;
		if (!key_tables(first, second))
			return fail(r, "the game's key tables weren't found");
		const std::vector<chunk> all = chunks(dat);

		// The images by name, the first of each that's whole
		std::unordered_map<std::string, image> images;
		std::vector<std::string> images_in_order; // (their names, in the DAT's order)
		for (const chunk &c : all)
		{
			const uint8_t *b = c.body;
			if (c.type != 0x20 || c.size < 69 || (b[0] != 0xA1 && b[0] != 0xB1))
				continue;
			std::string name(reinterpret_cast<const char *>(b + 1), 16);
			if (images.count(name) != 0)
				continue;
			int32_t w = 0, h = 0;
			std::memcpy(&w, b + 21, 4);
			std::memcpy(&h, b + 25, 4);
			if (w <= 0 || h <= 0)
				continue;
			if (b[0] == 0xA1)
			{
				if (std::memcmp(b + 57, "3TXD", 4) != 0 || w % 4 != 0 || h % 4 != 0 || 69 + size_t(w / 4) * size_t(h / 4) * 16 > c.size)
					continue;
				images_in_order.push_back(name);
				images.emplace(std::move(name), image{ b, c.size, uint32_t(w), uint32_t(h), kBC2 });
			}
			else
			{
				// (a u32 at +57, the 256-entry B8G8R8A8 palette at +61, then a byte an index)
				if (61 + 1024 + size_t(w) * size_t(h) > c.size)
					continue;
				images_in_order.push_back(name);
				images.emplace(std::move(name), image{ b, c.size, uint32_t(w), uint32_t(h), kBGRA8 });
			}
		}

		// The bump DAT's height maps, in its order (by their names' last 8 characters)
		struct height_map
		{
			const uint8_t *body;
			uint32_t w, h;
		};
		std::vector<uint8_t> bump;
		std::vector<height_map> heights;
		{
			const std::wstring bump_path = dat_path(bump_dat(map), why);
			if (!bump_path.empty() && read_file(bump_path, bump))
				for (const chunk &c : chunks(bump))
				{
					if (c.type != 0x5D || c.size < 0x20 || c.body[0] != 'b' || c.body[1] != 'p')
						continue;
					const uint32_t w = uint32_t(c.body[4] | (c.body[5] << 8)), h = uint32_t(c.body[6] | (c.body[7] << 8));
					if (w != 0 && h != 0 && 0x20 + size_t(w) * h <= c.size)
						heights.push_back({ c.body, w, h });
				}
		}

		// The models, each piece's texture and normal map given an index at its first use
		std::unordered_map<std::string, uint32_t> texture_of;
		std::unordered_map<std::string, uint32_t> normal_of; // (by the 8 characters)
		std::vector<raw_piece> ps;
		std::vector<uint32_t> scratch;
		for (const chunk &c : all)
		{
			if (c.type != 0x2E || c.size <= 64)
				continue;
			uint8_t *b = dat.data() + (c.body - dat.data());
			mmb::decrypt_mmb(b, c.size, first, second);
			model mo;
			std::memcpy(mo.name, b + 16, 16);
			if (z.find(mo.name) != kNone || !mmb_pieces(b, c.size, ps))
				continue;
			mo.kind = b[4];
			// (16FCD0: kind 1 drawn by the bump shader when each group's first piece's texture has a height map; the
			// game's loop checks only the first piece of each, not moving on)
			{
				bool any = false, every = mo.kind == 1;
				for (const raw_piece &p : ps)
				{
					if (!p.first)
						continue;
					any = true;
					bool found = false;
					for (const height_map &hm : heights)
						if (std::memcmp(hm.body + 0x18, p.tex + 8, 8) == 0)
						{
							found = true;
							break;
						}
					every = every && found;
				}
				mo.bump = any && every;
			}
			const bool swaying = mo.kind == 2;
			const uint32_t stride = swaying ? 48 : 36;
			mo.first_piece = uint32_t(z.pieces.size());
			mo.first_vertex = uint32_t(z.vertices.size());
			for (const raw_piece &p : ps)
			{
				scratch.clear();
				const uint32_t base = uint32_t(z.vertices.size());
				triangles(p, !swaying, base, scratch);
				if (scratch.empty())
					continue;
				piece pc;
				pc.first_index = uint32_t(z.indices.size());
				pc.triangles = uint32_t(scratch.size() / 3);
				pc.bits = p.bits;
				z.indices.insert(z.indices.end(), scratch.begin(), scratch.end());
				for (uint32_t i = 0; i < p.count; ++i)
					z.vertices.push_back(vertex_of(p.verts + size_t(i) * stride, swaying));
				std::string tex(reinterpret_cast<const char *>(p.tex), 16);
				auto img = images.find(tex);
				// (none of that name: the first whose last 8 characters are the same, if they aren't blank. Chateau
				// d'Oraguille's fireplace names 'fireefc e_ekken', which no DAT has; its 'model   e_ekken' is the hall's
				// stone: drawn white without it, 2026-10-27)
				if (img == images.end() && tex.find_first_not_of(' ', 8) != std::string::npos)
					for (auto i = images_in_order.begin(); i != images_in_order.end(); ++i)
						if (i->compare(8, 8, tex, 8, 8) == 0)
						{
							tex = *i;
							img = images.find(tex);
							break;
						}
				if (img == images.end())
					z.missing_textures++;
				else
				{
					auto known = texture_of.find(tex);
					if (known == texture_of.end())
					{
						const image &im = img->second;
						texture t;
						std::memcpy(t.name, tex.data(), 16);
						t.w = im.w;
						t.h = im.h;
						t.format = im.format;
						if (im.format == kBC2)
							t.data.assign(im.body + 69, im.body + 69 + size_t(im.w / 4) * (im.h / 4) * 16);
						else
						{
							const uint8_t *pal = im.body + 61, *px = im.body + 61 + 1024;
							t.data.resize(size_t(im.w) * im.h * 4);
							for (size_t i = 0; i < size_t(im.w) * im.h; ++i)
								std::memcpy(t.data.data() + i * 4, pal + size_t(px[i]) * 4, 4);
						}
						known = texture_of.emplace(tex, uint32_t(z.textures.size())).first;
						z.textures.push_back(std::move(t));
					}
					pc.texture = known->second;
					// (its normal map: the first height map whose name ends in the texture's last 8 characters)
					const std::string key = tex.substr(8);
					auto nm = normal_of.find(key);
					if (nm == normal_of.end())
					{
						uint32_t index = kNone;
						for (const height_map &hm : heights)
							if (std::memcmp(hm.body + 0x18, key.data(), 8) == 0)
							{
								normal_map n;
								std::memcpy(n.name, hm.body + 0x10, 16);
								n.w = hm.w;
								n.h = hm.h;
								n.texels.resize(size_t(hm.w) * hm.h);
								sobel(hm.body + 0x20, hm.w, hm.h, n.texels.data());
								index = uint32_t(z.normal_maps.size());
								z.normal_maps.push_back(std::move(n));
								break;
							}
						nm = normal_of.emplace(key, index).first;
					}
					pc.normal_map = nm->second;
				}
				z.pieces.push_back(pc);
			}
			mo.pieces = uint32_t(z.pieces.size()) - mo.first_piece;
			mo.vertices = uint32_t(z.vertices.size()) - mo.first_vertex;
			if (mo.pieces == 0)
				continue;
			for (int k = 0; k < 3; ++k)
			{
				mo.lo[k] = 1e30f;
				mo.hi[k] = -1e30f;
			}
			for (uint32_t i = mo.first_vertex; i < mo.first_vertex + mo.vertices; ++i)
				for (int k = 0; k < 3; ++k)
				{
					const float p = z.vertices[i].pos[k], q = p + z.vertices[i].sway[k];
					mo.lo[k] = std::min(mo.lo[k], std::min(p, q));
					mo.hi[k] = std::max(mo.hi[k], std::max(p, q));
				}
			z.models.push_back(mo);
		}
		if (z.models.empty())
		{
			r.none = true;
			r.why = "no meshes in the map's DAT";
			return false;
		}

		// The MZB's placements of the models
		std::vector<uint8_t> m;
		if (find_chunk(dat, 0x1C, m) && mmb::decrypt_mzb(m, first))
		{
			uint32_t n = 0;
			u32(m.data(), m.size(), 4, n);
			n &= 0xFFFFFF;
			for (uint32_t i = 0; i < n; ++i)
			{
				const size_t a = 0x20 + size_t(i) * 0x64;
				if (a + 0x64 > m.size())
					break;
				const char *name = reinterpret_cast<const char *>(m.data() + a);
				float t[9];
				std::memcpy(t, m.data() + a + 16, sizeof(t));
				bool finite = true;
				for (const float v : t)
					finite = finite && std::isfinite(v);
				const uint32_t model = z.find(name);
				if (model == kNone || !finite)
					continue;
				placement pl;
				pl.model = model;
				pl.flags = (name[0] == '_' ? kAlphaTested : 0) | (t[6] * t[8] < 0.0f ? kMirrored : 0) | (name[0] == '#' ? kClamped : 0);
				rotation(t + 3, t + 6, pl.m);
				std::memcpy(pl.t, t, 12);
				z.placements.push_back(pl);
			}
		}
		return true;
	}
}
