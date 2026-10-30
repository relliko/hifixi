/*
 * The names' font from the menu DAT (font.hpp). The set's layout (11E4E0; research/ffxi-re/tools/ui_sprites.py): name[16],
 * a texture count byte and 16-byte names, u16 record count, per record a part count byte and 61-byte parts (4 x (s16 x,
 * s16 y), s16 w, h, u, v, u8 flip, 4 x R G B A, 4 blend bytes, texture name[16]).
 */

#include "font.hpp"
#include "../bake/dat.hpp"
#include "../chars/tex.hpp"
#include <cctype>
#include <cstring>

namespace hifi::world::font
{
	const char kPageNames[2][17] = { "font    font    ", "menu    ustatshd" };
	const char kSetName[17] = "font    fontshp ";

	namespace
	{
		bool same_name(const uint8_t *a, const char *b)
		{
			for (int i = 0; i < 16; ++i)
				if (std::tolower(a[i]) != std::tolower(uint8_t(b[i])))
					return false;
			return true;
		}

		int16_t s16(const uint8_t *p)
		{
			int16_t v;
			std::memcpy(&v, p, 2);
			return v;
		}
	}

	bool image(const std::vector<uint8_t> &dat, const char name[16], uint32_t &w, uint32_t &h, std::vector<uint32_t> &texels, std::string &why)
	{
		for (const bake::chunk &c : bake::chunks(dat))
		{
			if (c.type != 0x20 || c.size < 17 + 40 || !same_name(c.body + 1, name))
				continue;
			chars::tex::texture t;
			if (!chars::tex::decode(c.body, c.size, t, why) || t.levels.empty())
				return false;
			const chars::tex::level &l = t.levels[0];
			w = l.w;
			h = l.h;
			texels.resize(size_t(w) * h);
			for (size_t i = 0; i < texels.size(); ++i)
				texels[i] = uint32_t(l.rgba[i * 4]) | uint32_t(l.rgba[i * 4 + 1]) << 8 | uint32_t(l.rgba[i * 4 + 2]) << 16 | uint32_t(l.rgba[i * 4 + 3]) << 24;
			return true;
		}
		why = std::string("no image '") + std::string(name, 16) + "' in it";
		return false;
	}

	bool find_set(const std::vector<uint8_t> &dat, const char name[16], sprite_set &out)
	{
		for (const bake::chunk &c : bake::chunks(dat))
		{
			if (c.type != 0x31 || c.size < 19 || !same_name(c.body, name))
				continue;
			const uint8_t *const b = c.body;
			const size_t size = c.size;
			out = sprite_set();
			std::memcpy(out.name, b, 16);
			size_t p = 16;
			const uint32_t textures = b[p];
			if (p + 1 + 16 * size_t(textures) + 2 > size)
				return false;
			for (uint32_t i = 0; i < textures; ++i)
				out.textures.emplace_back(reinterpret_cast<const char *>(b + p + 1 + 16 * i), 16);
			p += 1 + 16 * size_t(textures);
			uint16_t records;
			std::memcpy(&records, b + p, 2);
			p += 2;
			for (uint32_t r = 0; r < records; ++r)
			{
				if (p + 1 > size)
					return false;
				const uint32_t parts = b[p];
				if (p + 1 + 61 * size_t(parts) > size)
					return false;
				std::vector<part> rec(parts);
				for (uint32_t j = 0; j < parts; ++j)
				{
					const uint8_t *const q = b + p + 1 + 61 * j;
					part &pt = rec[j];
					for (int k = 0; k < 4; ++k)
					{
						pt.quad[k][0] = s16(q + 4 * k);
						pt.quad[k][1] = s16(q + 4 * k + 2);
					}
					pt.w = s16(q + 16);
					pt.h = s16(q + 18);
					pt.u = s16(q + 20);
					pt.v = s16(q + 22);
					pt.flip = q[24];
					std::memcpy(pt.colours, q + 25, 16);
					std::memcpy(pt.blend, q + 41, 4);
					std::memcpy(pt.texture, q + 45, 16);
				}
				out.records.push_back(std::move(rec));
				p += 1 + 61 * size_t(parts);
			}
			out.used = p;
			return true;
		}
		return false;
	}

	bool pages(const std::vector<uint8_t> &dat, font_pages &out, std::string &why)
	{
		for (int i = 0; i < 2; ++i)
			if (!image(dat, kPageNames[i], out.w[i], out.h[i], out.texels[i], why))
			{
				why = std::string("the name page '") + kPageNames[i] + "': " + why;
				return false;
			}
		return true;
	}

	bool load(font_pages &out, std::string &why)
	{
		const std::wstring path = bake::dat_path(kFile, why);
		std::vector<uint8_t> dat;
		if (path.empty())
			return false;
		if (!bake::read_file(path, dat))
		{
			why = "the menu DAT couldn't be read";
			return false;
		}
		return pages(dat, out, why);
	}
}
