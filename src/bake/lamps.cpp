/*
 * A zone's lamps (bake.hpp: lamps): reshade-worldfx's tools/bake_lamps.py, with research/ffxi-re/tools/zone_lamps.py's
 * reading of the DAT, in C++, the same bytes. Every point light the zone's meshes can be lit by: where it is, its
 * colour, how far it reaches and how bright it is.
 *
 * The zone's MZB (chunk 0x1C) names its lamps by 4-character id in its light table (at [MZB+0x18]: 256 records of 0x4C
 * bytes, +0 the id); each is the effect generator (chunk 0x05) of the same name in the same DAT, whose light element
 * puts a point light at the generator's position: its command list at [+0x84] starts with 01 (0x20 bytes, world
 * position at +0x14), then a command whose byte 1 is 0x47 (the light element), 16 (colour, bytes R G B A, 0x80 = 1.0),
 * 58 (four words: f1, f2, w3, w4 -> range f1*(1+w3), brightness f2*(1+w4)); 2E in the next list is a distance window.
 * Ids with no generator of that name in the zone DAT (West Ronfaure's pgh1/pgh2, ...) can't be baked: they're left out
 * (the add-on can still see them live). How bright a lamp is now isn't in the DAT: the add-on reads that from the
 * game's global light table, by id. What the DAT has is its brightness fully lit: 58's, or for a lamp whose brightness
 * follows the hour (research/ffxi-re/notes-lamp-hours.md: its 6C names a curve over the day, chunk 0x19 in the
 * generator's own folder, which its 49 sets the brightness to each frame) max(0, the curve's peak) x (1 + w4)
 * (zone_lamps.py's zone_hours).
 *
 * The file ('LMP2'), little-endian, the grass file's layout with its own tag and three float4s a lamp:
 *     char  magic[4] 'LMP2'
 *     u32   zone
 *     f32   cell size (yalms)
 *     u32   cell count
 *     u32   lamp count
 *     cells: i32 cx, i32 cz, u32 first lamp, u32 lamp count   (sorted by cz, then cx)
 *     lamps: f32 x, y, z, range           (the game's world: y is height, down positive; range in yalms)
 *            f32 r, g, b, brightness      (colour 1.0 = 0x80; the game's light is colour x brightness; fully lit)
 *            u32 id                       (the 4 characters, as the game keeps them: first character lowest)
 *            u32 slot                     (its 1-based slot in the zone's light table)
 *            u32 flags                    (1 the id starts with 'c': lights the game uses on characters only;
 *                                          2 no colour in the DAT (white); 4 no range or brightness in it (5, 4);
 *                                          8 its brightness follows the hour: the one here is its curve's peak)
 *            f32 the far end of its distance window (yalms; 0 when it has none)
 *     (grouped by cell, in the light table's order within one)
 * LMP1 (before 2026-10-23) was the same but for the hour: brightness always 58's, no flag 8.
 *
 * The Python's arithmetic is in doubles (range and brightness, colour / 128), rounded to float32 when written; a read
 * past a chunk's end fails the zone, as the Python's struct.error does.
 */

#include "bake.hpp"
#include "dat.hpp"
#include "mmb.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hifi::bake
{
	namespace
	{
		constexpr double kCell = 64.0;
		constexpr uint32_t kCharacters = 1, kNoColour = 2, kNoRange = 4, kFollowsHour = 8;
		constexpr double kDefaultRange = 5.0, kDefaultBrightness = 4.0;

		using mmb::f32;
		using mmb::u32;

		// A generator's light (light_of): where, colour, range and brightness, the distance window's far end
		struct light
		{
			bool is_light = false;
			double pos[3] = {};
			bool has_colour = false;
			double colour[3] = {};
			bool has_range = false;
			double range = 0.0, brightness = 0.0;
			bool has_window = false;
			double far_end = 0.0;
		};

		// A command's size in the generator's lists (cmd_size); 0 ends a list
		uint32_t cmd_size(uint32_t op, uint32_t b1)
		{
			if (op == 0 && b1 == 1)
				return 0;
			if (b1 == 0x47 || op == 0x08)
				return 0x10;
			return b1 == 0x0B || b1 == 0x0C ? 0x10 : (b1 & 0x1F) * 4;
		}

		// light_of: the generator chunk `ch` (with its 16-byte header) read as a light; false where the Python raises
		bool light_of(const uint8_t *ch, size_t size, light &out)
		{
			out = light();
			uint32_t o = 0, e = 0;
			if (!u32(ch, size, 0x84, o) || !u32(ch, size, 0x88, e))
				return false;
			if (uint64_t(o) + 0x30 > size || ch[o] != 1 || ch[o + 0x21] != 0x47)
				return true; // (not a light)
			out.is_light = true;
			for (int i = 0; i < 3; ++i)
			{
				float v = 0.0f;
				f32(ch, size, uint64_t(o) + 0x14 + i * 4, v);
				out.pos[i] = double(v);
			}
			const uint64_t end = std::min<uint64_t>(e, size);
			for (uint64_t p = uint64_t(o) + 0x20; p + 4 <= end;)
			{
				const uint32_t op = ch[p], b1 = ch[p + 1];
				const uint32_t step = cmd_size(op, b1);
				if (step == 0)
					break;
				if (op == 0x16)
				{
					if (p + 7 > size)
						return false; // (an IndexError there)
					out.has_colour = true;
					for (int i = 0; i < 3; ++i)
						out.colour[i] = ch[p + 4 + i] / 128.0;
				}
				else if (op == 0x58)
				{
					float f1 = 0.0f, f2 = 0.0f, w3 = 0.0f, w4 = 0.0f;
					if (!f32(ch, size, p + 4, f1) || !f32(ch, size, p + 8, f2) || !f32(ch, size, p + 12, w3) || !f32(ch, size, p + 16, w4))
						return false;
					out.has_range = true;
					out.range = double(f1) * (1.0 + double(w3));
					out.brightness = double(f2) * (1.0 + double(w4));
				}
				p += step;
			}
			for (uint64_t p = e; p + 4 <= size;)
			{
				const uint32_t op = ch[p], b1 = ch[p + 1];
				const uint32_t step = cmd_size(op, b1);
				if (step == 0)
					break;
				if (op == 0x2E)
				{
					float near_end = 0.0f, far_end = 0.0f;
					if (!f32(ch, size, p + 4, near_end) || !f32(ch, size, p + 8, far_end))
						return false;
					out.has_window = true;
					out.far_end = double(far_end);
				}
				p += step;
			}
			return true;
		}

		// hour_of: how a light generator's brightness follows the hour (the curve's name and its 58's w4); false when
		// it doesn't (no 58, or no 6C naming a curve in a register its per-frame list's 49 reads)
		bool hour_of(const uint8_t *ch, size_t size, std::string &curve, double &w4)
		{
			uint32_t o = 0, e = 0;
			if (size < 0x8C || !u32(ch, size, 0x84, o) || !u32(ch, size, 0x88, e))
				return false;
			if (uint64_t(o) + 0x30 > size || ch[o] != 1 || ch[o + 0x21] != 0x47)
				return false;
			std::map<uint32_t, std::string> named;
			bool has_w4 = false;
			const uint64_t end = std::min<uint64_t>(e, size);
			for (uint64_t p = uint64_t(o) + 0x20; p + 4 <= end;)
			{
				const uint32_t op = ch[p], b1 = ch[p + 1];
				const uint32_t step = cmd_size(op, b1);
				if (step == 0)
					break;
				uint32_t word = 0;
				float w = 0.0f;
				if (op == 0x6C && p + 12 <= size && u32(ch, size, p, word))
					named.emplace((word >> 13) & 0x3F, std::string(reinterpret_cast<const char *>(ch + p + 8), 4));
				else if (op == 0x58 && p + 20 <= size && f32(ch, size, p + 16, w))
				{
					has_w4 = true;
					w4 = double(w);
				}
				p += step;
			}
			for (uint64_t p = e; p + 4 <= size && has_w4;)
			{
				const uint32_t op = ch[p], b1 = ch[p + 1];
				const uint32_t step = cmd_size(op, b1);
				if (step == 0)
					break;
				uint32_t word = 0;
				if (op == 0x49 && u32(ch, size, p, word))
				{
					const auto n = named.find((word >> 13) & 0x3F);
					if (n != named.end())
					{
						curve = n->second;
						return true;
					}
				}
				p += step;
			}
			return false;
		}

		// curve_peak: a curve's (chunk 0x19, its body) highest value, its keyframes (t, value) up to the first with t >= 1
		// (where the game's evaluators stop); false when it has none
		bool curve_peak(const uint8_t *body, size_t size, double &peak)
		{
			bool any = false;
			for (size_t p = 0; p + 8 <= size; p += 8)
			{
				float t = 0.0f, v = 0.0f;
				std::memcpy(&t, body + p, 4);
				std::memcpy(&v, body + p + 4, 4);
				if (!any || double(v) > peak)
					peak = double(v);
				any = true;
				if (t >= 1.0f)
					break;
			}
			return any;
		}

		// A lamp as written: x, y, z, range, r, g, b, brightness, id, slot, flags, far
		struct lamp
		{
			double f[8];
			uint32_t id, slot, flags;
			double far_end;
		};

		// A double as float32 (struct.pack's 'f': one too big for float32 can't be written)
		bool to_float(double v, float &out)
		{
			out = float(v);
			return !(std::isfinite(v) && !std::isfinite(out));
		}

		void put(std::vector<uint8_t> &to, const void *v, size_t size)
		{
			const auto b = static_cast<const uint8_t *>(v);
			to.insert(to.end(), b, b + size);
		}

		bool fail(result &r, const char *why)
		{
			r.why = why;
			return false;
		}
	}

	bool lamps(uint32_t zone, const std::wstring &out, result &r)
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

		std::vector<uint8_t> m;
		if (!find_chunk(dat, 0x1C, m))
		{
			r.none = true;
			r.why = "no MZB in the zone DAT";
			return false;
		}
		if (!mmb::decrypt_mzb(m, first))
			return fail(r, "the MZB is too short");
		const uint32_t version = m.size() > 3 ? m[3] : 0;
		uint32_t table = 0;
		if (!u32(m.data(), m.size(), 0x18, table))
			return fail(r, "the MZB is too short");

		// The light table: slot (1-based) -> id; an id cut short by the MZB's end matches no generator
		std::map<uint32_t, std::string> ids;
		if (table != 0 && version >= 0x12)
			for (uint32_t i = 0; i < 256; ++i)
			{
				const uint64_t at = uint64_t(table) + uint64_t(i) * 0x4C;
				std::string id;
				if (at < m.size())
					id.assign(reinterpret_cast<const char *>(m.data() + at), size_t(std::min<uint64_t>(4, m.size() - at)));
				if (id != std::string(4, '\0'))
					ids.emplace(i + 1, id);
			}

		// The generators of those names (the first of a name), each read as a light
		std::set<std::string> want;
		for (const auto &[slot, id] : ids)
			want.insert(id);
		// (and the curves over the day, the first of a name in each folder (its path, as the Python's chunks gives it);
		// with each generator's, the folder it's in and the curve it follows the hour by, when it does)
		std::map<std::string, light> gens;
		std::map<std::pair<std::string, std::string>, std::pair<bool, double>> curves;
		std::map<std::string, std::pair<std::string, std::pair<std::string, double>>> hours;
		std::vector<std::string> folders;
		for (const chunk &c : chunks(dat))
		{
			if (c.type == 0x01)
				folders.emplace_back(c.name, 4);
			else if (c.type == 0x00 && !folders.empty())
				folders.pop_back();
			if (c.type == 0x00 || c.type == 0x01)
				continue;
			std::string folder;
			for (size_t i = 0; i < folders.size(); ++i)
				folder += (i != 0 ? "/" : "") + folders[i];
			const std::string name(c.name, 4);
			if (c.type == 0x19)
			{
				double peak = 0.0;
				const bool any = curve_peak(c.body, c.size, peak);
				curves.emplace(std::make_pair(folder, name), std::make_pair(any, peak));
				continue;
			}
			if (c.type != 0x05 || want.count(name) == 0 || gens.count(name) != 0)
				continue;
			light l;
			if (!light_of(c.body - 16, c.size + 16, l))
				return fail(r, "a light generator is cut short");
			gens.emplace(name, l);
			std::string curve;
			double w4 = 0.0;
			if (hour_of(c.body - 16, c.size + 16, curve, w4))
				hours.emplace(name, std::make_pair(folder, std::make_pair(curve, w4)));
		}

		// The object list's light slots (zone_lamps reads them all: a list cut short by the MZB's end fails the zone)
		uint32_t records = 0;
		u32(m.data(), m.size(), 4, records);
		records &= 0xFFFFFF;
		if (records != 0 && 0x20 + uint64_t(records) * 0x64 > m.size())
			return fail(r, "the MZB's object list is cut short");

		// The lamps, in the light table's order
		std::vector<lamp> items;
		uint32_t missing = 0;
		for (const auto &[slot, id] : ids)
		{
			const auto g = gens.find(id);
			if (g == gens.end() || !g->second.is_light)
			{
				missing++;
				continue;
			}
			const light &l = g->second;
			bool finite = true;
			double far_away = 0.0;
			for (const double v : l.pos)
			{
				finite = finite && std::isfinite(v);
				far_away = std::max(far_away, std::fabs(v));
			}
			if (!finite || far_away > 10000.0)
			{
				missing++;
				continue;
			}
			lamp it;
			it.flags = id[0] == 'c' || id[0] == 'C' ? kCharacters : 0;
			double colour[3] = { 1.0, 1.0, 1.0 };
			if (l.has_colour)
				std::copy(l.colour, l.colour + 3, colour);
			else
				it.flags |= kNoColour;
			double range = l.range, brightness = l.brightness;
			if (!l.has_range || !std::isfinite(range) || !std::isfinite(brightness))
			{
				range = kDefaultRange;
				brightness = kDefaultBrightness;
				it.flags |= kNoRange;
			}
			else if (const auto h = hours.find(id); h != hours.end())
			{
				// (fully lit by its curve: max(0, the peak) x (1 + w4), NaN kept as the Python's max keeps it)
				const auto c = curves.find(std::make_pair(h->second.first, h->second.second.first));
				if (c != curves.end() && c->second.first)
				{
					const double peak = c->second.second;
					const double lit = (0.0 > peak ? 0.0 : peak) * (1.0 + h->second.second.second);
					if (std::isfinite(lit))
					{
						brightness = lit;
						it.flags |= kFollowsHour;
					}
				}
			}
			const double f[8] = { l.pos[0], l.pos[1], l.pos[2], range, colour[0], colour[1], colour[2], brightness };
			std::copy(f, f + 8, it.f);
			std::memcpy(&it.id, id.data(), 4);
			it.slot = slot;
			it.far_end = l.has_window && std::isfinite(l.far_end) ? l.far_end : 0.0;
			items.push_back(it);
		}
		if (items.empty())
		{
			r.none = true;
			r.why = "no lamps (" + std::to_string(missing) + " ids without a light generator)";
			return false;
		}

		// Grouped by the 64-yalm cell their x and z are in, cells sorted by z, then x
		std::map<std::pair<int64_t, int64_t>, std::vector<const lamp *>> cells;
		for (const lamp &it : items)
		{
			const int64_t cx = int64_t(std::floor(it.f[0] / kCell)), cz = int64_t(std::floor(it.f[2] / kCell));
			cells[{ cz, cx }].push_back(&it);
		}
		std::vector<uint8_t> file;
		put(file, "LMP2", 4);
		const float cell = float(kCell);
		const uint32_t head[2] = { uint32_t(cells.size()), uint32_t(items.size()) };
		put(file, &zone, 4);
		put(file, &cell, 4);
		put(file, head, 8);
		uint32_t first_lamp = 0;
		for (const auto &[key, list] : cells)
		{
			const int32_t c[2] = { int32_t(key.second), int32_t(key.first) };
			const uint32_t span[2] = { first_lamp, uint32_t(list.size()) };
			put(file, c, 8);
			put(file, span, 8);
			first_lamp += uint32_t(list.size());
		}
		for (const auto &[key, list] : cells)
			for (const lamp *it : list)
			{
				float f[9];
				for (int i = 0; i < 8; ++i)
					if (!to_float(it->f[i], f[i]))
						return fail(r, "a lamp's number is too big for float32");
				if (!to_float(it->far_end, f[8]))
					return fail(r, "a lamp's number is too big for float32");
				const uint32_t w[3] = { it->id, it->slot, it->flags };
				put(file, f, 32);
				put(file, w, 12);
				put(file, &f[8], 4);
			}
		if (!write_file(out, file.data(), file.size()))
			return fail(r, "the file couldn't be written");
		return true;
	}
}
