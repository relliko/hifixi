/*
 * The names handed to HiFi XI (namedraw.hpp; research/ffxi-re/notes-ui-draw.md "The names").
 *
 * The places (both dumps; each a rel32 call pointed at ours, the names pass a vtable slot):
 *   0817F0  mov eax, [phase]; cmp dword [eax], 60h; jne ...      the names pass (its address is in one place in the
 *                                                                image: CXiActorNameDraw's vtable, its update slot)
 *   0D08A3  call 086210 (cdecl: pos, text, scale, colour, linkshell colour)
 *   0D08C9  call 086900 (cdecl: pos and two more)
 *   0867D2, 086D62  call 120590 (thiscall on the name font [VA 04971BF8]: the character, &colour, &linkshell colour;
 *                   ret 0Ch; al: a page bound). The page: the last of the font's first codes (+0x24, +0x28) at or below
 *                   the character (120590's loop), 1 the icons; ALPHAREF 0x60 when the colour is the linkshell's after it
 *   08689D, 086E27  call 00CD00 (thiscall on the wrapper: DrawPrimitiveUP(5 strip, 2, vertices, 28); ret 10h)
 */

#include "namedraw.hpp"
#include "jitter.hpp"
#include "slotmark.hpp"
#include "worldmark.hpp"
#include "../world/frames.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hifi::game::namedraw
{
	namespace
	{
		constexpr const char *kPass = "A1????????8338600F85????????8B0D????????566A016A0E8B410C508B10FF92C8000000"; // 0817F0
		constexpr const char *kName = "6683B9B2000000007521" "8B4424148B4C24108B54240C508B44240C518B4C240C525051E8"; // 0D0880, call at +0x23
		constexpr intptr_t kNameAt = 0x23;
		constexpr const char *kStars = "6683B9B20000000075178B44240C8B4C24088B542404505152E8"; // 0D08B0, call at +0x19
		constexpr intptr_t kStarsAt = 0x19;
		constexpr const char *kNamePage = "8D5424245125FF0000005250B9????????E8????????84C00F84"; // 0867C1, call at +0x11
		constexpr intptr_t kNamePageAt = 0x11;
		constexpr const char *kStarsPage = "BE808080805250B9????????897424288974241CE8????????84C00F84"; // 086D4E, call at +0x14
		constexpr intptr_t kStarsPageAt = 0x14;
		constexpr const char *kNameGlyph = "8B0D????????8D9424E40100006A1C526A026A05E8"; // 086889, call at +0x14
		constexpr const char *kStarsGlyph = "8B0D????????8D9424480100006A1C526A026A05E8"; // 086E13, call at +0x14
		constexpr intptr_t kGlyphAt = 0x14;
		constexpr const char *kPage = "568BF18B461C85C07512E8"; // 120590

		using pass_fn = uint8_t(__fastcall *)(uintptr_t, uintptr_t);
		using name_fn = void(__cdecl *)(const float *, const char *, float, uint32_t, uint32_t);
		using stars_fn = void(__cdecl *)(const float *, uint32_t, uint32_t);
		using page_fn = uint8_t(__fastcall *)(uintptr_t, uintptr_t, uint32_t, uint32_t *, uint32_t *);
		using glyph_fn = long(__fastcall *)(uintptr_t, uintptr_t, uint32_t, uint32_t, const void *, uint32_t);

		// A rel32 call pointed elsewhere, its bytes kept
		struct call_patch
		{
			uintptr_t at = 0;
			uint8_t saved[5] = {}, ours[5] = {};
		};

		struct state
		{
			bool in = false, native = false;
			sites s;
			call_patch calls[6]; // name, stars, name page, stars page, name glyph, stars glyph
			uint32_t saved_slot = 0, ours_slot = 0;
			// The callees (the game's, or the tests')
			uintptr_t pass = 0, name_draw = 0, stars_draw = 0, page = 0, glyph = 0;
			// The pass under way
			world::names_frame *f = nullptr;
			int32_t name = -1;       // the name being drawn (index into f->names), -1 outside one
			uint32_t page_now = 0, ref_now = 0;
			counts n;
		};
		state g;

		bool retarget(call_patch &p, uintptr_t at, uintptr_t to, uintptr_t expect, std::string &why, const char *what)
		{
			uint8_t now[5] = {};
			if (!read(at, now, 5) || now[0] != 0xE8 || (expect != 0 && rel_target(at) != expect))
			{
				why = std::string(what) + " isn't the call it should be";
				return false;
			}
			p.at = at;
			std::memcpy(p.saved, now, 5);
			p.ours[0] = 0xE8;
			put32(p.ours + 1, uint32_t(to - (at + 5)));
			return true;
		}

		std::string hex_of(uint32_t v)
		{
			char s[9];
			std::snprintf(s, sizeof(s), "%02X%02X%02X%02X", v & 0xFF, v >> 8 & 0xFF, v >> 16 & 0xFF, v >> 24);
			return s;
		}

		bool in_field()
		{
			const uint32_t p = g.s.phase != 0 ? word(g.s.phase) : 0;
			return pointer(p) && word(p) == 0x60;
		}

		// A name begun in the frame: its index, -1 when there's no room (its glyphs then go to the game)
		int32_t begin_name(const float *pos, bool stars)
		{
			world::names_frame *const f = g.f;
			if (f == nullptr || f->name_count >= world::kMaxNames)
				return -1;
			world::name_entry &e = f->names[f->name_count];
			e = world::name_entry();
			if (pos != nullptr)
				read(uintptr_t(pos), e.pos, sizeof(e.pos));
			e.stars = stars;
			e.first = f->glyph_count;
			return int32_t(f->name_count++);
		}

		void end_name()
		{
			if (g.f != nullptr && g.name >= 0)
			{
				world::name_entry &e = g.f->names[g.name];
				e.count = g.f->glyph_count - e.first;
			}
			g.name = -1;
		}
	}

	uint8_t __fastcall on_pass(uintptr_t task, uintptr_t)
	{
		if (!in_field())
			return reinterpret_cast<pass_fn>(g.pass)(task, 0);
		const uint32_t seq = ++g.n.seq == 0 ? ++g.n.seq : g.n.seq;
		world::names_frame &f = world::names_slot(seq);
		f.native = g.native;
		f.name_count = f.glyph_count = f.dropped = 0;
		f.width = f.height = 0;
		if (const uint32_t game = game_object(); game != 0)
		{
			uint16_t w = 0, h = 0;
			if (read(game + 0x18, w) && read(game + 0x1A, h))
			{
				f.width = w;
				f.height = h;
			}
		}
		const jitter::offset j = jitter::current();
		f.jitter[0] = j.px[0];
		f.jitter[1] = j.px[1];
		g.f = &f;
		g.name = -1;
		g.n.passes++;
		const uint8_t r = reinterpret_cast<pass_fn>(g.pass)(task, 0);
		g.f = nullptr;
		g.name = -1;
		g.n.last_glyphs = f.glyph_count;
		g.n.most_glyphs = std::max(g.n.most_glyphs, f.glyph_count);
		world::names_publish(f, seq);
		worldmark::send(slotmark::names_end, seq);
		return r;
	}

	void __cdecl on_name(const float *pos, const char *text, float scale, uint32_t colour, uint32_t ls_colour)
	{
		g.n.names++;
		g.name = begin_name(pos, false);
		if (g.name >= 0)
		{
			world::name_entry &e = g.f->names[g.name];
			e.scale = scale;
			e.colour = colour;
			e.ls_colour = ls_colour;
			if (text != nullptr)
				for (size_t i = 0; i < 0x24; ++i)
				{
					char c = 0;
					if (!read(uintptr_t(text) + i, c) || c == 0)
						break;
					e.text[i] = c;
				}
		}
		reinterpret_cast<name_fn>(g.name_draw)(pos, text, scale, colour, ls_colour);
		end_name();
	}

	void __cdecl on_stars(const float *pos, uint32_t a2, uint32_t a3)
	{
		g.n.stars++;
		g.name = begin_name(pos, true);
		reinterpret_cast<stars_fn>(g.stars_draw)(pos, a2, a3);
		end_name();
	}

	uint8_t __fastcall on_page(uintptr_t font, uintptr_t, uint32_t ch, uint32_t *colour, uint32_t *ls_colour)
	{
		const uint8_t r = reinterpret_cast<page_fn>(g.page)(font, 0, ch, colour, ls_colour);
		if (g.name >= 0)
		{
			// (120590's choice: the page whose first code is the last at or below the character, of the two at +0x24)
			int k = 1;
			while (k >= 0 && word(font + 0x24 + 4 * uintptr_t(k)) > ch)
				k--;
			g.page_now = k == 1 ? 1u : 0u;
			uint32_t c = 0, l = 1;
			g.ref_now = colour != nullptr && ls_colour != nullptr && read(uintptr_t(colour), c) && read(uintptr_t(ls_colour), l) && c == l ? 0x60u : 0u;
		}
		return r;
	}

	long __fastcall on_glyph(uintptr_t wrapper, uintptr_t, uint32_t type, uint32_t count, const void *vertices, uint32_t stride)
	{
		world::names_frame *const f = g.f;
		if (f != nullptr && f->native && g.name >= 0 && type == 5 && count == 2 && stride == sizeof(world::name_vertex) && vertices != nullptr)
		{
			if (f->glyph_count < world::kMaxGlyphs)
			{
				world::name_glyph &q = f->glyphs[f->glyph_count];
				if (read(uintptr_t(vertices), q.v, sizeof(q.v)))
				{
					q.page = g.page_now;
					q.alpha_ref = g.ref_now;
					q.name = uint32_t(g.name);
					f->glyph_count++;
					g.n.glyphs++;
					return 0;
				}
			}
			else
				f->dropped++;
		}
		g.n.game++;
		return reinterpret_cast<glyph_fn>(g.glyph)(wrapper, 0, type, count, vertices, stride);
	}

	bool start(std::string &why)
	{
		if (g.in)
			return true;
		const uintptr_t pass = find(kPass), page = find(kPage);
		const uintptr_t name = find(kName), stars = find(kStars), name_page = find(kNamePage), stars_page = find(kStarsPage);
		const uintptr_t name_glyph = find(kNameGlyph), stars_glyph = find(kStarsGlyph);
		if (pass == 0 || page == 0 || name == 0 || stars == 0 || name_page == 0 || stars_page == 0 || name_glyph == 0 || stars_glyph == 0)
		{
			why = "the names' draw wasn't found";
			return false;
		}
		const uintptr_t slot = find(hex_of(uint32_t(pass)).c_str());
		if (slot == 0 || word(slot) != uint32_t(pass))
		{
			why = "the names pass's vtable slot wasn't found";
			return false;
		}
		state s;
		s.s.pass_slot = slot;
		s.s.pass = pass;
		s.s.phase = word(pass + 1);
		s.s.name_call = name + kNameAt;
		s.s.stars_call = stars + kStarsAt;
		s.s.name_draw = rel_target(s.s.name_call);
		s.s.stars_draw = rel_target(s.s.stars_call);
		s.s.name_page = name_page + kNamePageAt;
		s.s.stars_page = stars_page + kStarsPageAt;
		s.s.page = page;
		s.s.name_glyph = name_glyph + kGlyphAt;
		s.s.stars_glyph = stars_glyph + kGlyphAt;
		s.s.glyph = rel_target(s.s.name_glyph);
		// (each page and glyph call inside its own draw: the names' after 086210's start, the stars' after 086900's)
		if (!(s.s.name_page > s.s.name_draw && s.s.name_page < s.s.name_draw + 0x800 && s.s.name_glyph > s.s.name_draw && s.s.name_glyph < s.s.name_draw + 0x800 &&
				s.s.stars_page > s.s.stars_draw && s.s.stars_page < s.s.stars_draw + 0x800 && s.s.stars_glyph > s.s.stars_draw && s.s.stars_glyph < s.s.stars_draw + 0x800))
		{
			why = "the names' page and glyph calls aren't inside their draws";
			return false;
		}
		const uintptr_t to[6] = { reinterpret_cast<uintptr_t>(&on_name), reinterpret_cast<uintptr_t>(&on_stars), reinterpret_cast<uintptr_t>(&on_page),
			reinterpret_cast<uintptr_t>(&on_page), reinterpret_cast<uintptr_t>(&on_glyph), reinterpret_cast<uintptr_t>(&on_glyph) };
		const uintptr_t at[6] = { s.s.name_call, s.s.stars_call, s.s.name_page, s.s.stars_page, s.s.name_glyph, s.s.stars_glyph };
		const uintptr_t expect[6] = { 0, 0, page, page, 0, s.s.glyph };
		const char *const what[6] = { "the names' call of their draw", "the star line's call of its draw", "the names' page call", "the star line's page call",
			"the names' glyph draw", "the star line's glyph draw" };
		for (int i = 0; i < 6; ++i)
			if (!retarget(s.calls[i], at[i], to[i], expect[i], why, what[i]))
				return false;
		// All in, or none
		const uint32_t ours_slot = uint32_t(reinterpret_cast<uintptr_t>(&on_pass));
		int done = 0;
		bool ok = true;
		for (; done < 6 && ok; ++done)
			ok = write_code(s.calls[done].at, s.calls[done].ours, 5);
		ok = ok && write_code(slot, &ours_slot, 4);
		if (!ok)
		{
			for (int i = 0; i < 6; ++i)
				write_code(s.calls[i].at, s.calls[i].saved, 5);
			const uint32_t back = uint32_t(pass);
			write_code(slot, &back, 4);
			why = "couldn't write the game's code";
			return false;
		}
		s.saved_slot = uint32_t(pass);
		s.ours_slot = ours_slot;
		s.pass = pass;
		s.name_draw = s.s.name_draw;
		s.stars_draw = s.s.stars_draw;
		s.page = page;
		s.glyph = s.s.glyph;
		s.in = true;
		s.n = g.n;
		g = s;
		return true;
	}

	bool stop(std::string &why)
	{
		if (!g.in)
			return true;
		for (const call_patch &p : g.calls)
			if (!code_is(p.at, p.ours, 5))
			{
				why = "something else has changed the names' calls since: left as they are";
				return false;
			}
		if (!code_is(g.s.pass_slot, &g.ours_slot, 4))
		{
			why = "something else has changed the names pass's vtable slot since: left as it is";
			return false;
		}
		bool ok = write_code(g.s.pass_slot, &g.saved_slot, 4);
		for (const call_patch &p : g.calls)
			ok = write_code(p.at, p.saved, 5) && ok;
		if (!ok)
		{
			why = "couldn't write the game's code";
			return false;
		}
		const counts n = g.n;
		g = state();
		g.n = n;
		return true;
	}

	bool in()
	{
		return g.in;
	}

	void set_native(bool on)
	{
		g.native = on;
	}

	counts get_counts()
	{
		return g.n;
	}

	std::string status()
	{
		if (!g.in)
			return "names: drawn by the game";
		char line[360];
		std::snprintf(line, sizeof(line), "names: %s; %llu passes, %llu names, %llu star lines; %llu glyphs drawn by HiFi XI, %llu by the game; the last frame %u glyphs (most %u)",
			g.native ? "HiFi XI draws them" : "the game draws them (HiFi XI's side or its font not ready, or its markers not coming through)",
			static_cast<unsigned long long>(g.n.passes), static_cast<unsigned long long>(g.n.names), static_cast<unsigned long long>(g.n.stars),
			static_cast<unsigned long long>(g.n.glyphs), static_cast<unsigned long long>(g.n.game), g.n.last_glyphs, g.n.most_glyphs);
		return line;
	}

	sites where()
	{
		return g.s;
	}

	void set_callees(uintptr_t pass, uintptr_t name_draw, uintptr_t stars_draw, uintptr_t page, uintptr_t glyph)
	{
		g.pass = pass;
		g.name_draw = name_draw;
		g.stars_draw = stars_draw;
		g.page = page;
		g.glyph = glyph;
	}
}
