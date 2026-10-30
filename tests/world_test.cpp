/*
 * Offline test of the world part of owning the frame (src/game/skydraw, namedraw, griddraw, worldmark; src/world):
 *   - on both FFXiMain dumps (game_dump.hpp): each pass's places found where research/ffxi-re has them (03DD00's call of
 *     03D640, the strip counter; the names pass's vtable slot, 0D08A3 / 0D08C9 and the page and glyph calls inside
 *     086210 / 086900; 1D5DE0, 1CF4BD, 1CF607 and 177B40; the wrapper's global), our calls in, and stop() leaving the
 *     dump byte for byte;
 *   - the recording on the first dump with stand-ins for the game's callees (the game's code as each patched call
 *     reaches it): the sky's arguments, the transforms from a made-up wrapper's cache, the session's skipped strips and
 *     its counter, the game's 03D640 called only with native off; the names' frame (each name, each glyph's quad, page
 *     and ALPHAREF, the game's draw not reached with native on, reached with it off; A's size from gGame), outside the
 *     field no frame and no marker; the grid's outlines (the colour packed as 177B40 packs it, the depth flag, the
 *     transforms); each pass's end marker with its seq;
 *   - the dome builder against tests/world_oracle.py (03D640's arithmetic read from the disassembly, over every weather
 *     keyframe with a dome in ten zones): every strip's band, elevations, colours to the bit, and its vertices;
 *   - the names' font from the menu DAT against the oracle (chartex.py, ui_sprites.py): both pages' pixels, the
 *     'font fontshp' set's every record and part;
 *   - the renderer on WARP with the debug layer: the dome against World FX's own model of the game's dome
 *     (grass::dome_colour, which its sky repaint keys on), the names into the world (MODULATE2X / 4X, the alpha test,
 *     LESSEQUAL against the depth, the depth written where the glyph passes), the names over a screen twice A's size
 *     (the jitter taken out, the depth read from A at the matching pixel), the grid's outlines; nothing said by the
 *     debug layer.
 *
 *     build\world_test.exe build\test\world_oracle.txt
 */

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include "../src/game/griddraw.hpp"
#include "../src/game/namedraw.hpp"
#include "../src/game/skydraw.hpp"
#include "../src/game/slotmark.hpp"
#include "../src/game/worldmark.hpp"
#include "../src/render/grass_render.hpp"
#include "../src/world/dome.hpp"
#include "../src/world/font.hpp"
#include "../src/world/frames.hpp"
#include "../src/world/render.hpp"
#include "../src/bake/dat.hpp"
#include "game_dump.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	namespace sd = hifi::game::skydraw;
	namespace nd = hifi::game::namedraw;
	namespace gd = hifi::game::griddraw;
	namespace w = hifi::world;

	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	size_t differing(const char *path, uintptr_t base, size_t size)
	{
		std::vector<uint8_t> file(size);
		FILE *f = nullptr;
		if (fopen_s(&f, path, "rb") != 0 || f == nullptr)
			return size_t(-1);
		const size_t got = fread(file.data(), 1, size, f);
		fclose(f);
		if (got != size)
			return size_t(-1);
		size_t n = 0;
		const uint8_t *image = reinterpret_cast<const uint8_t *>(base);
		for (size_t i = 0; i < size; ++i)
			n += image[i] != file[i];
		return n;
	}

	uint32_t bits_of(float f)
	{
		uint32_t b;
		std::memcpy(&b, &f, 4);
		return b;
	}
	float float_of(uint32_t b)
	{
		float f;
		std::memcpy(&f, &b, 4);
		return f;
	}
	uint32_t word_at(uintptr_t a)
	{
		uint32_t v;
		std::memcpy(&v, reinterpret_cast<const void *>(a), 4);
		return v;
	}
	void put_word(uintptr_t a, uint32_t v)
	{
		std::memcpy(reinterpret_cast<void *>(a), &v, 4);
	}

	// The markers sent (to the sink)
	struct mark
	{
		uint32_t kind, seq;
	};
	std::vector<mark> g_marks;
	void sink(uint32_t kind, uint32_t seq)
	{
		g_marks.push_back({ kind, seq });
	}

	// ---- stand-ins for the game's callees ----
	int g_dome_calls = 0;
	void __cdecl game_dome(float, uint32_t, uint32_t, const uint32_t *, const float *)
	{
		g_dome_calls++;
	}

	int g_glyph_calls = 0, g_page_calls = 0;
	uint32_t g_font_first[2] = { 0x20, 0x8E }; // (the font object's first codes at +0x24, +0x28)
	std::vector<uint8_t> g_font(0x40);
	uint8_t __fastcall game_page(uintptr_t, uintptr_t, uint32_t ch, uint32_t *colour, uint32_t *ls)
	{
		g_page_calls++;
		if (ch >= 0x8E)
			*colour = *ls; // (120590: an icon's colour becomes the linkshell's)
		return 1;
	}
	long __fastcall game_glyph(uintptr_t, uintptr_t, uint32_t, uint32_t, const void *, uint32_t)
	{
		g_glyph_calls++;
		return 0;
	}
	// 086210 as far as the patched calls go: a glyph per character, its page asked for, its quad handed over
	void glyphs_of(const char *text, uint32_t colour, uint32_t ls, float z)
	{
		float x = 100.0f;
		for (const char *c = text; *c != 0; ++c)
		{
			uint32_t col = colour, l = ls;
			nd::on_page(uintptr_t(g_font.data()), 0, uint8_t(*c), &col, &l);
			hifi::world::name_vertex v[4];
			for (int k = 0; k < 4; ++k)
			{
				v[k].x = x - 0.5f + (k & 1 ? 8.0f : 0.0f);
				v[k].y = 50.0f - 0.5f + (k & 2 ? 16.0f : 0.0f);
				v[k].z = z;
				v[k].rhw = 1.0f;
				v[k].colour = col;
				v[k].u = float(k & 1) * 0.5f;
				v[k].v = float(k >> 1) * 0.5f;
			}
			nd::on_glyph(0x1234, 0, 5, 2, v, 28);
			x += 8.0f;
		}
	}
	void __cdecl game_name(const float *, const char *text, float, uint32_t colour, uint32_t ls)
	{
		glyphs_of(text, colour, ls, 0.98f);
	}
	void __cdecl game_stars(const float *, uint32_t, uint32_t)
	{
		const char stars[] = "\xAD\xAD\xAD";
		glyphs_of(stars, 0x80808080, 0x80808080, 0.97f);
	}
	uint8_t __fastcall game_pass(uintptr_t, uintptr_t)
	{
		const float pos[4] = { 1.0f, -2.0f, 3.0f, 1.0f };
		nd::on_name(pos, "Relli", 0.75f, 0xC0808080, 0x00FF8000);
		const char icon[] = "\x92 LS";
		nd::on_name(pos, icon, 1.0f, 0xC0618080, 0x00FF8000);
		nd::on_stars(pos, 7, 8);
		return 0;
	}

	int g_grid_calls = 0, g_outline_calls = 0;
	void __cdecl game_outline(const float *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
	{
		g_outline_calls++;
	}
	uint32_t __fastcall game_grid(uintptr_t, uintptr_t, uint32_t)
	{
		g_grid_calls++;
		float c[16] = {};
		for (int k = 0; k < 4; ++k)
		{
			c[k * 4 + 0] = float(k & 1);
			c[k * 4 + 1] = -0.04f;
			c[k * 4 + 2] = float(k >> 1);
		}
		gd::on_outline(c, 0xFF, 0x60, 0x60, 0x80, 0);
		gd::on_outline(c, 0x80, 0x80, 0xFF, 0x80, 1);
		return 7;
	}

	// a x b, row vectors
	void multiply(const float a[16], const float b[16], float out[16])
	{
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 4; ++c)
			{
				float s = 0.0f;
				for (int k = 0; k < 4; ++k)
					s += a[r * 4 + k] * b[k * 4 + c];
				out[r * 4 + c] = s;
			}
	}
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

namespace
{
	// ---------------------------------------------------------------- the dumps
	void dumps()
	{
		std::string why;
		for (const char *path : dump::kDumps)
		{
			uintptr_t base = 0;
			const size_t size = dump::map(path, base);
			if (size == 0)
			{
				check(false, std::string("mapped ") + path);
				continue;
			}
			const std::string name = std::string(path).substr(std::string(path).rfind('\\') + 1);
			why.clear();
			check(sd::start(why) && sd::in(), name + ": sky start()" + (why.empty() ? "" : ": " + why));
			const sd::sites s = sd::where();
			check(s.call == base + 0x03DD00 && s.original == base + 0x03D640, name + ": the sky's call at 03DD00, to 03D640");
			check(s.wrapper == base + 0x45666C && s.counter == base + 0x65CB08, name + ": the wrapper's global (VA +45666C) and the strip counter (+65CB08)");
			check(hifi::game::rel_target(s.call) == reinterpret_cast<uintptr_t>(&sd::on_dome), name + ": 03DD00 now calls on_dome");
			why.clear();
			check(nd::start(why) && nd::in(), name + ": names start()" + (why.empty() ? "" : ": " + why));
			const nd::sites n = nd::where();
			check(n.pass_slot == base + 0x329C38 && n.pass == base + 0x0817F0 && n.phase == base + 0x456A28, name + ": the names pass 0817F0, its vtable slot (+329C38), the phase global");
			check(word_at(n.pass_slot) == uint32_t(reinterpret_cast<uintptr_t>(&nd::on_pass)), name + ": the slot now holds on_pass");
			check(n.name_call == base + 0x0D08A3 && n.name_draw == base + 0x086210 && n.stars_call == base + 0x0D08C9 && n.stars_draw == base + 0x086900,
				name + ": 0D08A3 -> 086210, 0D08C9 -> 086900");
			check(n.name_page == base + 0x0867D2 && n.stars_page == base + 0x086D62 && n.page == base + 0x120590, name + ": the page calls 0867D2, 086D62 of 120590");
			check(n.name_glyph == base + 0x08689D && n.stars_glyph == base + 0x086E27 && n.glyph == base + 0x00CD00, name + ": the glyph draws 08689D, 086E27 of 00CD00");
			check(hifi::game::rel_target(n.name_call) == reinterpret_cast<uintptr_t>(&nd::on_name) && hifi::game::rel_target(n.stars_call) == reinterpret_cast<uintptr_t>(&nd::on_stars) &&
					hifi::game::rel_target(n.name_page) == reinterpret_cast<uintptr_t>(&nd::on_page) && hifi::game::rel_target(n.stars_page) == reinterpret_cast<uintptr_t>(&nd::on_page) &&
					hifi::game::rel_target(n.name_glyph) == reinterpret_cast<uintptr_t>(&nd::on_glyph) && hifi::game::rel_target(n.stars_glyph) == reinterpret_cast<uintptr_t>(&nd::on_glyph),
				name + ": the six calls now ours");
			// (nameplate.dll's glyph scale call in both dumps: what the names keep, as the game computes it)
			const uint8_t nameplate[] = { 0x8D, 0x4C, 0x24, 0x4C, 0x8D, 0x94, 0x24, 0xE0, 0x06, 0x00, 0x00, 0xB8 };
			check(std::memcmp(reinterpret_cast<const void *>(base + 0x086378), nameplate, sizeof(nameplate)) == 0, name + ": 086378 holds nameplate.dll's call (the glyph scale), left alone");
			why.clear();
			check(gd::start(why) && gd::in(), name + ": grid start()" + (why.empty() ? "" : ": " + why));
			const gd::sites gs = gd::where();
			check(gs.grid_call == base + 0x1D5DE0 && gs.grid == base + 0x1CF110 && gs.outline_calls[0] == base + 0x1CF4BD && gs.outline_calls[1] == base + 0x1CF607 &&
					gs.outline == base + 0x177B40 && gs.wrapper == base + 0x45666C,
				name + ": 1D5DE0 -> 1CF110, 1CF4BD and 1CF607 -> 177B40, the wrapper's global");
			why.clear();
			const bool stopped = sd::stop(why) && nd::stop(why) && gd::stop(why);
			check(stopped && !sd::in() && !nd::in() && !gd::in() && differing(path, base, size) == 0, name + ": stop(): the dump again, byte for byte" + why);
			dump::unmap(base);
		}
	}

	// ---------------------------------------------------------------- the recording
	void recording()
	{
		uintptr_t base = 0;
		const size_t size = dump::map(dump::kDumps[0], base);
		if (size == 0)
		{
			check(false, "the FFXiMain dump mapped");
			return;
		}
		std::string why;
		hifi::game::worldmark::set_sink(sink);
		check(sd::start(why) && nd::start(why) && gd::start(why), "all three in for the recording" + why);
		sd::set_draw(reinterpret_cast<uintptr_t>(&game_dome));
		nd::set_callees(reinterpret_cast<uintptr_t>(&game_pass), reinterpret_cast<uintptr_t>(&game_name), reinterpret_cast<uintptr_t>(&game_stars),
			reinterpret_cast<uintptr_t>(&game_page), reinterpret_cast<uintptr_t>(&game_glyph));
		gd::set_callees(reinterpret_cast<uintptr_t>(&game_grid), reinterpret_cast<uintptr_t>(&game_outline));

		// A wrapper whose transform cache holds WORLD, VIEW, PROJECTION
		std::vector<uint8_t> wrapper(0xA00);
		float world[16] = {}, view[16] = {}, proj[16] = {};
		for (int i = 0; i < 16; ++i)
		{
			world[i] = i % 5 == 0 ? 1.0f : 0.0f;
			view[i] = float((i * 7) % 5) * 0.25f;
			proj[i] = float((i * 3) % 4) * 0.5f;
		}
		world[12] = 2.0f;
		std::memcpy(wrapper.data() + 0x93C, world, 64);
		std::memcpy(wrapper.data() + 0x97C, view, 64);
		std::memcpy(wrapper.data() + 0x9BC, proj, 64);
		const uint32_t wrapper_was = word_at(sd::where().wrapper);
		put_word(sd::where().wrapper, uint32_t(uintptr_t(wrapper.data())));
		float wv[16], wvp[16], vp[16];
		multiply(world, view, wv);
		multiply(wv, proj, wvp);
		multiply(view, proj, vp);

		// ---- the sky
		const uint32_t colours[8] = { 0x80414454, 0x80373A48, 0x80323441, 0x80323441, 0x80292C36, 0x80252630, 0x8015161E, 0x800E0E14 };
		const float heights[8] = { 0.0f, 0.165f, 0.273f, 0.381f, 0.49f, 0.66f, 0.828f, 1.0f };
		uint32_t strips = 0;
		for (int b = 0; b < 7; ++b)
			strips += w::dome::band_strips(heights[b], heights[b + 1]);
		const uintptr_t counter = sd::where().counter;
		const uint32_t counter_was = word_at(counter);
		g_marks.clear();
		sd::set_native(false);
		g_dome_calls = 0;
		sd::on_dome(52427.92f, 8, 10, colours, heights);
		uint32_t seq = sd::get_counts().seq;
		const w::sky_frame *sf = w::sky_published(seq & 15);
		check(g_dome_calls == 1 && sf != nullptr && !sf->native && g_marks.size() == 1 && g_marks[0].kind == hifi::slotmark::sky_end && g_marks[0].seq == seq,
			"sky, native off: the game's 03D640 called, its frame published as the game's, the sky's end marker with its seq");
		sd::set_native(true);
		put_word(counter, 0);
		g_marks.clear();
		sd::on_dome(52427.92f, 8, 10, colours, heights);
		seq = sd::get_counts().seq;
		sf = w::sky_published(seq & 15);
		bool same = sf != nullptr;
		for (int i = 0; same && i < 16; ++i)
			same = std::fabs(sf->world_view_proj[i] - wvp[i]) <= 1e-5f * std::max(1.0f, std::fabs(wvp[i]));
		check(g_dome_calls == 1 && sf != nullptr && sf->native && sf->radius == 52427.92f && sf->segments == 10 && std::memcmp(sf->colours, colours, 32) == 0 &&
				std::memcmp(sf->heights, heights, 32) == 0 && g_marks.size() == 1 && g_marks[0].seq == seq,
			"sky, native on: recorded (03D640's arguments), the game's 03D640 not called, the marker");
		check(same, "sky: the transforms as WORLD x VIEW x PROJECTION from the wrapper's cache");
		check(sf != nullptr && sf->skip == std::min(strips, 21u) && word_at(counter) == std::min(strips, 21u), "sky: a session's first strips left out as the game leaves them (" +
			std::to_string(sf != nullptr ? sf->skip : 0) + " of " + std::to_string(strips) + "), the game's counter moved on as it would be");
		sd::on_dome(52427.92f, 8, 10, colours, heights); // (10 more: the counter at 20)
		sd::on_dome(52427.92f, 8, 10, colours, heights); // (its last strip to leave out: 21)
		sf = w::sky_published(sd::get_counts().seq & 15);
		const bool last_one = sf != nullptr && sf->skip == 1 && word_at(counter) == 21;
		sd::on_dome(52427.92f, 8, 10, colours, heights);
		sf = w::sky_published(sd::get_counts().seq & 15);
		check(last_one && sf != nullptr && sf->skip == 0 && word_at(counter) == 21, "sky: the 21st strip the last left out, then every strip drawn and the counter left alone");
		g_dome_calls = 0;
		sd::on_dome(52427.92f, 8, 2, colours, heights);
		check(g_dome_calls == 1, "sky: a dome of 2 segments (none drawn) left to the game");
		put_word(counter, counter_was);

		// ---- the names
		const nd::sites ns = nd::where();
		const uint32_t phase_was = word_at(ns.phase);
		uint32_t phase_value = 0x70;
		put_word(ns.phase, uint32_t(uintptr_t(&phase_value)));
		std::memcpy(g_font.data() + 0x24, g_font_first, 8);
		const uint32_t game_was = hifi::game::game_object();
		g_marks.clear();
		nd::set_native(true);
		g_glyph_calls = 0;
		const uint64_t passes_before = nd::get_counts().passes;
		nd::on_pass(0, 0);
		check(nd::get_counts().passes == passes_before && g_marks.empty() && g_glyph_calls == 12, "names outside the field (phase not 0x60): the game's pass, no frame, no marker");
		phase_value = 0x60;
		g_marks.clear();
		g_glyph_calls = 0;
		nd::on_pass(0, 0);
		seq = nd::get_counts().seq;
		const w::names_frame *nf = w::names_published(seq & 15);
		check(g_glyph_calls == 0 && nf != nullptr && nf->native && g_marks.size() == 1 && g_marks[0].kind == hifi::slotmark::names_end && g_marks[0].seq == seq,
			"names, native on: published, the names' end marker, no glyph reached the device");
		if (nf != nullptr)
		{
			check(nf->name_count == 3 && nf->glyph_count == 5 + 4 + 3, "names: 3 names (one a star line), 12 glyphs");
			const w::name_entry &a = nf->names[0], &b = nf->names[1], &c = nf->names[2];
			check(std::strcmp(a.text, "Relli") == 0 && a.scale == 0.75f && a.colour == 0xC0808080 && a.ls_colour == 0x00FF8000 && a.pos[1] == -2.0f && !a.stars &&
					a.first == 0 && a.count == 5, "names: the first's text, scale, colours, point, its glyphs");
			check(b.first == 5 && b.count == 4 && c.stars && c.first == 9 && c.count == 3, "names: the second's glyphs, the star line's");
			const w::name_glyph &l = nf->glyphs[0], &icon = nf->glyphs[5], &letter = nf->glyphs[6], &star = nf->glyphs[9];
			check(l.page == 0 && l.alpha_ref == 0 && l.v[0].colour == 0xC0808080 && l.v[3].x == 100.0f - 0.5f + 8.0f && l.v[3].z == 0.98f && l.name == 0,
				"names: a letter on page 0, ALPHAREF 0, its quad as handed over");
			check(icon.page == 1 && icon.alpha_ref == 0x60 && icon.v[0].colour == 0x00FF8000 && letter.page == 0 && letter.alpha_ref == 0,
				"names: the pearl on page 1 with ALPHAREF 0x60 (its colour the linkshell's), the next letter page 0");
			check(star.page == 1 && star.alpha_ref == 0x60 && star.name == 2, "names: the star line's stars on page 1, ALPHAREF 0x60");
		}
		nd::set_native(false);
		g_glyph_calls = 0;
		nd::on_pass(0, 0);
		nf = w::names_published(nd::get_counts().seq & 15);
		check(g_glyph_calls == 12 && nf != nullptr && !nf->native && nf->glyph_count == 0 && nf->name_count == 3, "names, native off: every glyph to the game's draw, the names noted");
		(void)game_was;
		put_word(ns.phase, phase_was);

		// ---- the grid
		g_marks.clear();
		gd::set_native(true);
		g_outline_calls = 0;
		const uint32_t r = gd::on_grid(0x5678, 0, 3);
		seq = gd::get_counts().seq;
		const w::grid_frame *gf = w::grid_published(seq & 15);
		check(r == 7 && g_grid_calls == 1 && g_outline_calls == 0 && gf != nullptr && gf->native && gf->count == 2 && g_marks.size() == 1 && g_marks[0].kind == 12 &&
				g_marks[0].seq == seq,
			"grid, native on: 1CF110 run, both outlines recorded (none to the game), the grid's end marker (kind 12)");
		if (gf != nullptr && gf->count == 2)
		{
			check(gf->lines[0].colour == 0x80FF6060 && gf->lines[0].keep_z == 0 && gf->lines[1].colour == 0x808080FF && gf->lines[1].keep_z == 1,
				"grid: colours packed as 177B40 packs them (0x80FF6060 red, 0x808080FF blue), the depth flag");
			check(gf->lines[0].p[3][0] == 1.0f && gf->lines[0].p[3][2] == 1.0f && gf->lines[0].p[1][1] == -0.04f, "grid: the corners (stride 16)");
			bool vp_ok = true;
			for (int i = 0; i < 16; ++i)
				vp_ok = vp_ok && std::fabs(gf->view_proj[i] - vp[i]) <= 1e-5f * std::max(1.0f, std::fabs(vp[i]));
			check(vp_ok, "grid: VIEW x PROJECTION from the wrapper's cache");
		}
		gd::set_native(false);
		gd::on_grid(0x5678, 0, 3);
		check(g_outline_calls == 2, "grid, native off: both outlines to the game");
		put_word(sd::where().wrapper, wrapper_was);
		hifi::game::worldmark::set_sink(nullptr);
		check(sd::stop(why) && nd::stop(why) && gd::stop(why) && differing(dump::kDumps[0], base, size) <= 3 * 4 + 8,
			"stop() after the recording (only the data words the test wrote differ)" + why);
		dump::unmap(base);
	}

	// ---------------------------------------------------------------- the oracle
	void oracle(const char *path)
	{
		std::ifstream in(path);
		if (!in)
		{
			check(false, std::string("the oracle's file ") + path);
			return;
		}
		const float radius = float(0.800000011920929 * (65535.0 - 0.10000000149011612));
		uint32_t domes = 0, strips = 0, bad_strips = 0, bad_counts = 0, bad_vertex = 0;
		std::vector<w::dome::strip> got;
		std::vector<w::dome::vertex> verts;
		uint32_t segments = 0;
		size_t at = 0;
		std::string line;
		std::vector<std::string> font_lines;
		std::vector<std::vector<std::string>> expected_strips;
		auto finish_dome = [&]() {
			if (domes == 0)
				return;
			if (expected_strips.size() != got.size())
			{
				bad_counts++;
				return;
			}
			for (size_t i = 0; i < got.size(); ++i)
			{
				const std::vector<std::string> &e = expected_strips[i];
				const w::dome::strip &s = got[i];
				char buf[64];
				std::snprintf(buf, sizeof(buf), "%08X", s.c_lo);
				bool ok = std::stoul(e[1]) == s.band && std::stoul(e[2], nullptr, 16) == bits_of(s.e_lo) && std::stoul(e[3], nullptr, 16) == bits_of(s.e_hi) &&
					std::stoul(e[4], nullptr, 16) == s.c_lo && std::stoul(e[5], nullptr, 16) == s.c_hi;
				if (!ok)
				{
					if (bad_strips < 4)
						std::printf("     strip %zu: expected band %s e %s %s c %s %s; got %u e %08X %08X c %08X %08X\n", i, e[1].c_str(), e[2].c_str(), e[3].c_str(), e[4].c_str(),
							e[5].c_str(), s.band, bits_of(s.e_lo), bits_of(s.e_hi), s.c_lo, s.c_hi);
					bad_strips++;
				}
				const w::dome::vertex &v = verts[s.first + 2];
				const float ex[3] = { float_of(uint32_t(std::stoul(e[6], nullptr, 16))), float_of(uint32_t(std::stoul(e[7], nullptr, 16))), float_of(uint32_t(std::stoul(e[8], nullptr, 16))) };
				const float gv[3] = { v.x, v.y, v.z };
				for (int k = 0; k < 3; ++k)
					if (std::fabs(gv[k] - ex[k]) > 1e-5f * std::max(1.0f, std::fabs(ex[k])))
					{
						bad_vertex++;
						break;
					}
				strips++;
			}
			(void)segments;
		};
		while (std::getline(in, line))
		{
			std::istringstream ss(line);
			std::vector<std::string> t;
			for (std::string word; ss >> word;)
				t.push_back(word);
			if (t.empty())
				continue;
			if (t[0] == "dome" && t.size() == 21)
			{
				finish_dome();
				expected_strips.clear();
				domes++;
				segments = uint32_t(std::stoul(t[4]));
				uint32_t cols[8];
				float hs[8];
				for (int i = 0; i < 8; ++i)
				{
					cols[i] = uint32_t(std::stoul(t[5 + i], nullptr, 16));
					hs[i] = float_of(uint32_t(std::stoul(t[13 + i], nullptr, 16)));
				}
				w::dome::build(radius, segments, cols, hs, got, verts);
				const bool sized = verts.size() == got.size() * 2 * (segments + 1);
				if (!sized)
					bad_counts++;
			}
			else if (t[0] == "strip" && t.size() == 9)
				expected_strips.push_back(t);
			else if (t[0] == "page" || t[0] == "set" || t[0] == "rec" || t[0] == "part")
				font_lines.push_back(line);
		}
		finish_dome();
		at = 0;
		check(domes > 300 && strips > 3000 && bad_strips == 0 && bad_counts == 0, "the dome against the oracle: " + std::to_string(domes) + " domes, " + std::to_string(strips) +
			" strips, each band, elevation and colour to the bit (" + std::to_string(bad_strips) + " differ, " + std::to_string(bad_counts) + " domes of another strip count)");
		check(bad_vertex == 0, "the dome's vertices against the oracle (" + std::to_string(bad_vertex) + " differ)");

		// The font: the menu DAT read as HiFi XI reads it
		std::string why;
		const std::wstring dat_path = hifi::bake::dat_path(w::font::kFile, why);
		std::vector<uint8_t> dat;
		check(!dat_path.empty() && hifi::bake::read_file(dat_path, dat), "the menu DAT (file 0x9A76) found and read" + why);
		w::font_pages pages;
		check(w::font::pages(dat, pages, why), "the two name pages decoded" + why);
		w::font::sprite_set set;
		check(w::font::find_set(dat, w::font::kSetName, set), "the set 'font fontshp' found");
		uint32_t page_ok = 0, rec_bad = 0, part_bad = 0;
		size_t rec = 0, part = 0;
		for (const std::string &fl : font_lines)
		{
			std::istringstream ss(fl);
			std::vector<std::string> t;
			for (std::string word; ss >> word;)
				t.push_back(word);
			if (t[0] == "page" && t.size() == 5)
			{
				const int i = t[1] == "666f6e7420202020666f6e7420202020" ? 0 : 1;
				uint64_t h = 0xCBF29CE484222325ull;
				for (uint32_t texel : pages.texels[i])
					for (int k = 0; k < 4; ++k)
					{
						h ^= (texel >> (8 * k)) & 0xFF;
						h *= 0x100000001B3ull;
					}
				char hex[20];
				std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(h));
				if (std::stoul(t[2]) == pages.w[i] && std::stoul(t[3]) == pages.h[i] && t[4] == hex)
					page_ok++;
				else
					std::printf("     page %d: %ux%u %s, the oracle %sx%s %s\n", i, pages.w[i], pages.h[i], hex, t[2].c_str(), t[3].c_str(), t[4].c_str());
			}
			else if (t[0] == "set" && t.size() == 5)
				check(std::stoul(t[2]) == set.textures.size() && std::stoul(t[3]) == set.records.size() && std::stoul(t[4]) == set.used,
					"the set: " + std::to_string(set.textures.size()) + " textures, " + std::to_string(set.records.size()) + " records, " + std::to_string(set.used) + " bytes, as the oracle");
			else if (t[0] == "rec" && t.size() == 3)
			{
				rec = std::stoul(t[1]);
				part = 0;
				if (rec >= set.records.size() || set.records[rec].size() != std::stoul(t[2]))
					rec_bad++;
			}
			else if (t[0] == "part" && t.size() == 1 + 8 + 4 + 1 + 3)
			{
				if (rec >= set.records.size() || part >= set.records[rec].size())
				{
					part_bad++;
					continue;
				}
				const w::font::part &p = set.records[rec][part++];
				bool ok = true;
				for (int k = 0; k < 4; ++k)
					ok = ok && std::stoi(t[1 + 2 * k]) == p.quad[k][0] && std::stoi(t[2 + 2 * k]) == p.quad[k][1];
				ok = ok && std::stoi(t[9]) == p.w && std::stoi(t[10]) == p.h && std::stoi(t[11]) == p.u && std::stoi(t[12]) == p.v && std::stoul(t[13]) == p.flip;
				char cols[40], blend[10], tex[40];
				for (int k = 0; k < 16; ++k)
					std::snprintf(cols + 2 * k, 3, "%02x", p.colours[k / 4][k % 4]);
				for (int k = 0; k < 4; ++k)
					std::snprintf(blend + 2 * k, 3, "%02x", p.blend[k]);
				for (int k = 0; k < 16; ++k)
					std::snprintf(tex + 2 * k, 3, "%02x", uint8_t(std::tolower(uint8_t(p.texture[k]))));
				ok = ok && t[14] == cols && t[15] == blend && t[16] == tex;
				part_bad += ok ? 0 : 1;
			}
		}
		check(page_ok == 2, "both pages' pixels as chartex.py decodes them (256x256 each)");
		check(rec_bad == 0 && part_bad == 0 && set.records.size() == 146, "every record and part of 'font fontshp' as ui_sprites.py reads them (0x20..0xB1)");
		(void)at;
	}

	// ---------------------------------------------------------------- WARP
	struct gpu
	{
		ID3D12Device *device = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		ID3D12InfoQueue *info = nullptr;
		ID3D12Fence *fence = nullptr;
		uint64_t fence_value = 0;
		HANDLE ev = nullptr;

		bool make()
		{
			ID3D12Debug *debug = nullptr;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
				debug->EnableDebugLayer();
			IDXGIFactory4 *factory = nullptr;
			IDXGIAdapter *warp = nullptr;
			CreateDXGIFactory1(IID_PPV_ARGS(&factory));
			if (factory != nullptr)
				factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
			D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
			if (device == nullptr)
				return false;
			device->QueryInterface(IID_PPV_ARGS(&info));
			D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
			device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
			device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
			ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			return queue != nullptr && fence != nullptr;
		}
		void run(ID3D12GraphicsCommandList *list)
		{
			list->Close();
			ID3D12CommandList *lists[] = { list };
			queue->ExecuteCommandLists(1, lists);
			queue->Signal(fence, ++fence_value);
			fence->SetEventOnCompletion(fence_value, ev);
			WaitForSingleObject(ev, 60000);
		}
		uint32_t complaints()
		{
			const UINT64 messages = info != nullptr ? info->GetNumStoredMessages() : 0;
			uint32_t bad = 0;
			for (UINT64 i = 0; i < messages; ++i)
			{
				SIZE_T n = 0;
				info->GetMessage(i, nullptr, &n);
				std::vector<uint8_t> m(n);
				D3D12_MESSAGE *msg = reinterpret_cast<D3D12_MESSAGE *>(m.data());
				info->GetMessage(i, msg, &n);
				// (the test's own clears to other values than the resources were made with: slower, not wrong)
				if (msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING && msg->ID != D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE &&
					msg->ID != D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE)
				{
					bad++;
					std::printf("     debug layer: %s\n", msg->pDescription);
				}
			}
			if (info != nullptr)
				info->ClearStoredMessages();
			return bad;
		}
	};

	// A target (R8G8B8A8) and a depth (D24S8, typeless so it can be read), their views, a readback of the target
	struct surfaces
	{
		ID3D12Resource *rt = nullptr, *ds = nullptr, *readback = nullptr;
		ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
		UINT w = 0, h = 0;
		std::vector<uint8_t> px;

		void make(ID3D12Device *device, UINT width, UINT height, bool depth)
		{
			w = width;
			h = height;
			D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, rb = { D3D12_HEAP_TYPE_READBACK };
			D3D12_RESOURCE_DESC td = {};
			td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			td.Width = w;
			td.Height = h;
			td.DepthOrArraySize = 1;
			td.MipLevels = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			D3D12_CLEAR_VALUE cv = {};
			cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt));
			D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
			device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
			device->CreateRenderTargetView(rt, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
			if (depth)
			{
				td.Format = DXGI_FORMAT_R24G8_TYPELESS;
				td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
				D3D12_CLEAR_VALUE dv = {};
				dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
				dv.DepthStencil.Depth = 1.0f;
				device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dv, IID_PPV_ARGS(&ds));
				hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
				device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap));
				D3D12_DEPTH_STENCIL_VIEW_DESC dsd = {};
				dsd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
				dsd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
				device->CreateDepthStencilView(ds, &dsd, dsv_heap->GetCPUDescriptorHandleForHeapStart());
			}
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = UINT64(w) * h * 4;
			bd.Height = 1;
			bd.DepthOrArraySize = 1;
			bd.MipLevels = 1;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
		}
		D3D12_CPU_DESCRIPTOR_HANDLE rtv() const { return rtv_heap->GetCPUDescriptorHandleForHeapStart(); }
		D3D12_CPU_DESCRIPTOR_HANDLE dsv() const { return dsv_heap->GetCPUDescriptorHandleForHeapStart(); }
		w::target target(bool with_depth) const
		{
			w::target t;
			t.rtv = rtv();
			t.rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
			if (with_depth)
			{
				t.dsv = dsv();
				t.dsv_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			}
			t.width = w;
			t.height = h;
			return t;
		}
		void copy_out(ID3D12GraphicsCommandList *list)
		{
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = rt;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			list->ResourceBarrier(1, &b);
			D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
			src.pResource = rt;
			src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst.pResource = readback;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, w * 4 };
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
			list->ResourceBarrier(1, &b);
		}
		void read()
		{
			px.assign(size_t(w) * h * 4, 0);
			uint8_t *p = nullptr;
			const D3D12_RANGE all = { 0, px.size() };
			if (SUCCEEDED(readback->Map(0, &all, reinterpret_cast<void **>(&p))))
			{
				std::memcpy(px.data(), p, px.size());
				readback->Unmap(0, nullptr);
			}
		}
		const uint8_t *at(UINT x, UINT y) const { return px.data() + (size_t(y) * w + x) * 4; }
		void release()
		{
			for (IUnknown *u : { static_cast<IUnknown *>(rt), static_cast<IUnknown *>(ds), static_cast<IUnknown *>(readback), static_cast<IUnknown *>(rtv_heap),
					 static_cast<IUnknown *>(dsv_heap) })
				if (u != nullptr)
					u->Release();
		}
	};

	// The game's camera looking along a yaw and pitch (radians; y down, row vectors), its projection (fov y, aspect)
	void camera(float yaw, float pitch, float fovy, float aspect, float view[16], float proj[16])
	{
		const float f[3] = { std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
		// (right = f x up(0, -1, 0), up = right x f; view columns right, up, -f)
		float r[3] = { f[1] * 0.0f - f[2] * -1.0f, f[2] * 0.0f - f[0] * 0.0f, f[0] * -1.0f - f[1] * 0.0f };
		const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
		for (float &v : r)
			v /= rl;
		const float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
		std::memset(view, 0, 64);
		for (int i = 0; i < 3; ++i)
		{
			view[i * 4 + 0] = r[i];
			view[i * 4 + 1] = u[i];
			view[i * 4 + 2] = -f[i];
		}
		view[15] = 1.0f;
		std::memset(proj, 0, 64);
		const float ys = 1.0f / std::tan(fovy * 0.5f), zn = 0.1f, zf = 65535.0f;
		proj[0] = ys / aspect;
		proj[5] = ys;
		proj[10] = zf / (zn - zf);
		proj[11] = -1.0f;
		proj[14] = zn * zf / (zn - zf);
	}

	void warp(const char *oracle_path)
	{
		(void)oracle_path;
		gpu g;
		if (!g.make())
		{
			check(false, "a WARP device");
			return;
		}
		std::string why;
		w::renderer r;
		check(r.init(g.device, why), "the world's renderer made on WARP" + why);
		w::font_pages pages;
		check(w::font::load(pages, why) && r.set_pages(pages, why), "the name pages read and put up" + why);
		ID3D12CommandAllocator *alloc = nullptr;
		ID3D12GraphicsCommandList *list = nullptr;
		g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
		g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));

		// ---- the dome: West Ronfaure's cloudy midnight record (the oracle's first), looking north a little up
		{
			surfaces s;
			s.make(g.device, 320, 180, false);
			w::sky_frame &f = w::sky_slot(1);
			f.native = true;
			f.radius = float(0.800000011920929 * (65535.0 - 0.10000000149011612));
			f.segments = 10;
			const uint32_t cols[8] = { 0x80414454, 0x80373A48, 0x80323441, 0x80323441, 0x80292C36, 0x80252630, 0x8015161E, 0x800E0E14 };
			const uint32_t hb[8] = { 0x00000000, 0x3E28F5C3, 0x3E8BC6A8, 0x3EC3126F, 0x3EFAE148, 0x3F28F5C3, 0x3F53F7CF, 0x3F800000 };
			std::memcpy(f.colours, cols, 32);
			for (int i = 0; i < 8; ++i)
				f.heights[i] = float_of(hb[i]);
			f.skip = 0;
			float view[16], proj[16];
			const float yaw = 0.3f, pitch = 0.35f, fovy = 1.2f, aspect = 320.0f / 180.0f;
			camera(yaw, pitch, fovy, aspect, view, proj);
			multiply(view, proj, f.world_view_proj);
			w::sky_publish(f, 1);
			const float clear[4] = { float(0x54) / 255.0f, float(0x44) / 255.0f, float(0x41) / 255.0f, float(0x80) / 255.0f };
			list->ClearRenderTargetView(s.rtv(), clear, 0, nullptr);
			check(r.sky(list, s.target(false), f, 1, why), "the dome recorded" + why);
			s.copy_out(list);
			g.run(list);
			s.read();
			grass::look::sky_dome d;
			d.segments = 10;
			for (int i = 0; i < 8; ++i)
			{
				d.stops[i][0] = float(cols[i] >> 16 & 0xFF);
				d.stops[i][1] = float(cols[i] >> 8 & 0xFF);
				d.stops[i][2] = float(cols[i] & 0xFF);
				d.stops[i][3] = f.heights[i];
			}
			d.clear[0] = float(0x54);
			d.clear[1] = float(0x44);
			d.clear[2] = float(0x41);
			float worst = 0.0f, sum = 0.0f;
			uint32_t n = 0, over_half = 0, sky_pixels = 0;
			for (UINT y = 0; y < s.h; y += 3)
				for (UINT x = 0; x < s.w; x += 3)
				{
					// (the pixel's ray: the viewport's corner at 0.5, Direct3D 8's pixel centres)
					const float nx = float(x) / float(s.w) * 2.0f - 1.0f, ny = 1.0f - float(y) / float(s.h) * 2.0f;
					const float v[3] = { nx / proj[0], ny / proj[5], -1.0f };
					float dir[3];
					for (int i = 0; i < 3; ++i)
						dir[i] = view[i * 4 + 0] * v[0] + view[i * 4 + 1] * v[1] + view[i * 4 + 2] * v[2];
					const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
					for (float &c : dir)
						c /= l;
					float want[3];
					grass::dome_colour(d, dir, want);
					const uint8_t *p = s.at(x, y);
					for (int c = 0; c < 3; ++c)
					{
						const float e = std::fabs(float(p[c]) - want[c]);
						worst = std::max(worst, e);
						sum += e;
						over_half += e > 0.5f ? 1 : 0;
						n++;
					}
					sky_pixels += -dir[1] > 0.0f ? 1 : 0;
				}
			check(sky_pixels > 1000 && worst <= 1.5f, "the dome on WARP against World FX's model of the game's dome (grass::dome_colour): worst " + std::to_string(worst) +
				" bytes, mean " + std::to_string(sum / float(std::max(1u, n))) + ", " + std::to_string(over_half) + " of " + std::to_string(n) + " channels over half a byte (" +
				std::to_string(sky_pixels) + " sky pixels)");
			const uint8_t *top = s.at(160, 0);
			check(top[3] == 0x80, "the dome writes its vertex alpha (0x80), as the game's SELECTARG1 DIFFUSE does");
			check(g.complaints() == 0, "the dome: the debug layer said nothing");
			alloc->Reset();
			list->Reset(alloc, nullptr);
			s.release();
		}

		// ---- the names into A: the letter 'A' of 'font fontshp' (u 211, v 131, 19 x 23 texels) at 1:1, a wall in the middle
		const uint32_t rec_u = 211, rec_v = 131, rec_w = 19, rec_h = 23;
		auto glyph_at = [&](w::name_glyph &q, float x0, float y0, float z, uint32_t colour, uint32_t page, uint32_t ref, float scale, uint32_t u0, uint32_t v0, uint32_t gw, uint32_t gh) {
			for (int k = 0; k < 4; ++k)
			{
				w::name_vertex &v = q.v[k];
				v.x = x0 - 0.5f + (k & 1 ? float(gw) * scale : 0.0f);
				v.y = y0 - 0.5f + (k & 2 ? float(gh) * scale : 0.0f);
				v.z = z;
				v.rhw = 1.0f;
				v.colour = colour;
				v.u = float(u0 + (k & 1 ? gw : 0)) / 256.0f;
				v.v = float(v0 + (k & 2 ? gh : 0)) / 256.0f;
			}
			q.page = page;
			q.alpha_ref = ref;
			q.name = 0;
		};
		auto texel = [&](int page, uint32_t x, uint32_t y) { return pages.texels[page][size_t(y) * pages.w[page] + x]; };
		// What the game's combiner gives a texel under a vertex colour (bytes), and whether the alpha test passes
		auto shade = [&](uint32_t t, uint32_t colour, uint32_t ref, float out[4]) {
			const float tr = float(t & 0xFF) / 255.0f, tg = float(t >> 8 & 0xFF) / 255.0f, tb = float(t >> 16 & 0xFF) / 255.0f, ta = float(t >> 24) / 255.0f;
			const float cr = float(colour >> 16 & 0xFF) / 255.0f, cg = float(colour >> 8 & 0xFF) / 255.0f, cb = float(colour & 0xFF) / 255.0f, ca = float(colour >> 24) / 255.0f;
			out[0] = std::min(1.0f, tr * cr * 2.0f);
			out[1] = std::min(1.0f, tg * cg * 2.0f);
			out[2] = std::min(1.0f, tb * cb * 2.0f);
			out[3] = std::min(1.0f, ta * ca * 4.0f);
			return std::round(out[3] * 255.0f) > float(ref);
		};
		{
			surfaces s;
			s.make(g.device, 256, 128, true);
			w::names_frame &f = w::names_slot(2);
			f.native = true;
			f.width = 256;
			f.height = 128;
			f.jitter[0] = f.jitter[1] = 0.0f;
			f.name_count = 1;
			f.glyph_count = 4;
			const uint32_t pc = 0xC0808080, party = 0xC0618080;
			glyph_at(f.glyphs[0], 20, 20, 0.40f, pc, 0, 0, 1.0f, rec_u, rec_v, rec_w, rec_h);   // in front of the wall
			glyph_at(f.glyphs[1], 60, 20, 0.60f, pc, 0, 0, 1.0f, rec_u, rec_v, rec_w, rec_h);   // behind it: hidden
			glyph_at(f.glyphs[2], 140, 20, 0.30f, party, 0, 0, 1.0f, rec_u, rec_v, rec_w, rec_h); // in the open, the party colour
			glyph_at(f.glyphs[3], 140, 20, 0.35f, pc, 0, 0, 1.0f, rec_u, rec_v, rec_w, rec_h);  // after it, farther: kept off by its depth
			w::names_publish(f, 2);
			const float black[4] = { 0, 0, 0, 0 };
			list->ClearRenderTargetView(s.rtv(), black, 0, nullptr);
			list->ClearDepthStencilView(s.dsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			const D3D12_RECT wall = { 0, 0, 120, 128 };
			list->ClearDepthStencilView(s.dsv(), D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 1, &wall);
			check(r.names(list, s.target(true), f, 2, why), "the names into A recorded" + why);
			s.copy_out(list);
			g.run(list);
			s.read();
			uint32_t front_bad = 0, front_on = 0, hidden_on = 0, open_bad = 0, open_on = 0;
			for (uint32_t j = 0; j < rec_h; ++j)
				for (uint32_t i = 0; i < rec_w; ++i)
				{
					const uint32_t t = texel(0, rec_u + i, rec_v + j);
					float want[4];
					const bool pass = shade(t, pc, 0, want);
					const uint8_t *p = s.at(20 + i, 20 + j);
					// (blended SRCALPHA / INVSRCALPHA over black: the colour times its alpha)
					if (pass)
					{
						front_on++;
						for (int c = 0; c < 3; ++c)
							front_bad += std::fabs(float(p[c]) - want[c] * want[3] * 255.0f) > 1.5f ? 1 : 0;
					}
					else
						front_bad += (p[0] | p[1] | p[2]) != 0 ? 1 : 0;
					const uint8_t *q = s.at(60 + i, 20 + j);
					hidden_on += (q[0] | q[1] | q[2] | q[3]) != 0 ? 1 : 0;
					float want2[4];
					const bool pass2 = shade(t, party, 0, want2);
					const uint8_t *o = s.at(140 + i, 20 + j);
					if (pass2)
					{
						open_on++;
						for (int c = 0; c < 3; ++c)
							open_bad += std::fabs(float(o[c]) - want2[c] * want2[3] * 255.0f) > 1.5f ? 1 : 0;
					}
				}
			check(front_on > 40 && front_bad == 0, "names: a glyph in front of the wall: each pixel the texel x colour x 2, alpha x 4, blended, the alpha test (" +
				std::to_string(front_on) + " pixels pass, " + std::to_string(front_bad) + " differ)");
			check(hidden_on == 0, "names: a glyph behind the wall hidden pixel by pixel (LESSEQUAL against the world's depth)");
			check(open_on > 40 && open_bad == 0, "names: a nearer glyph in the open kept, a farther one drawn after it kept off where it wrote its depth (" +
				std::to_string(open_bad) + " differ)");
			check(g.complaints() == 0, "names into A: the debug layer said nothing");
			alloc->Reset();
			list->Reset(alloc, nullptr);

			// ---- the icons' alpha test (0x60): the pearl of 'menu ustatshd' (u 0, v 64, 32 x 32 texels), alone
			{
				w::names_frame &f2 = w::names_slot(3);
				f2.native = true;
				f2.width = 256;
				f2.height = 128;
				f2.glyph_count = 1;
				glyph_at(f2.glyphs[0], 180, 60, 0.2f, 0x80808080, 1, 0x60, 1.0f, 0, 64, 32, 32);
				w::names_publish(f2, 3);
				list->ClearRenderTargetView(s.rtv(), black, 0, nullptr);
				list->ClearDepthStencilView(s.dsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
				check(r.names(list, s.target(true), f2, 3, why), "the pearl recorded" + why);
				s.copy_out(list);
				g.run(list);
				s.read();
				uint32_t bad = 0, on = 0, cut = 0;
				for (uint32_t j = 0; j < 32; ++j)
					for (uint32_t i = 0; i < 32; ++i)
					{
						float want[4];
						const bool pass = shade(texel(1, i, 64 + j), 0x80808080, 0x60, want);
						const uint8_t *p = s.at(180 + i, 60 + j);
						const bool lit = p[3] != 0;
						on += pass ? 1 : 0;
						const float a = want[3];
						cut += !pass && a > 0.0f ? 1 : 0;
						bad += pass != lit ? 1 : 0;
					}
				check(on > 50 && bad == 0, "names: an icon alpha tested at 0x60 (" + std::to_string(on) + " pixels pass, " + std::to_string(cut) + " faint ones cut, " + std::to_string(bad) +
					" differ)");
				check(g.complaints() == 0, "the icon: the debug layer said nothing");
				alloc->Reset();
				list->Reset(alloc, nullptr);
			}

			// ---- over a screen twice A's size: the jitter taken out, the depth read from A
			{
				surfaces screen;
				screen.make(g.device, 512, 256, false);
				w::names_frame &f3 = w::names_slot(4);
				f3.native = true;
				f3.width = 256;
				f3.height = 128;
				f3.jitter[0] = 0.25f;
				f3.jitter[1] = -0.25f;
				f3.glyph_count = 2;
				// (as the game would record them: at 20 + jitter, and one behind the wall)
				glyph_at(f3.glyphs[0], 20.0f + 0.25f, 70.0f - 0.25f, 0.40f, pc, 0, 0, 1.0f, rec_u, rec_v, rec_w, rec_h);
				glyph_at(f3.glyphs[1], 60.0f + 0.25f, 70.0f - 0.25f, 0.60f, pc, 0, 0, 1.0f, rec_u, rec_v, rec_w, rec_h);
				w::names_publish(f3, 4);
				list->ClearRenderTargetView(screen.rtv(), black, 0, nullptr);
				list->ClearDepthStencilView(s.dsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
				list->ClearDepthStencilView(s.dsv(), D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 1, &wall);
				D3D12_RESOURCE_BARRIER b = {};
				b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				b.Transition.pResource = s.ds;
				b.Transition.Subresource = 0;
				b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
				b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				list->ResourceBarrier(1, &b);
				D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
				sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
				sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				sd.Texture2D.MipLevels = 1;
				g.device->CreateShaderResourceView(s.ds, &sd, r.heap_cpu(4));
				w::over_screen o;
				o.depth = r.heap_gpu(4);
				o.a_width = 256;
				o.a_height = 128;
				check(r.names_over(list, screen.target(false), o, f3, 4, why), "the names over the screen recorded" + why);
				std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
				list->ResourceBarrier(1, &b);
				screen.copy_out(list);
				g.run(list);
				screen.read();
				// (the glyph at A's 20, 70 - 0.5 before the jitter: on the screen at 40, 140, each texel 2 x 2 pixels)
				uint32_t lo_x = 9999, lo_y = 9999, hi_x = 0, hi_y = 0, behind = 0;
				for (UINT y = 0; y < screen.h; ++y)
					for (UINT x = 0; x < screen.w; ++x)
					{
						const uint8_t *p = screen.at(x, y);
						if ((p[0] | p[1] | p[2]) == 0)
							continue;
						if (x >= 100 && x < 240)
						{
							behind++;
							continue;
						}
						lo_x = std::min<uint32_t>(lo_x, x);
						lo_y = std::min<uint32_t>(lo_y, y);
						hi_x = std::max<uint32_t>(hi_x, x);
						hi_y = std::max<uint32_t>(hi_y, y);
					}
				// (where the texels that pass the alpha test lie in the record: the screen's box is twice that, from 40, 140)
				uint32_t tx0 = 99, ty0 = 99, tx1 = 0, ty1 = 0;
				for (uint32_t j = 0; j < rec_h; ++j)
					for (uint32_t i = 0; i < rec_w; ++i)
					{
						float want[4];
						if (shade(texel(0, rec_u + i, rec_v + j), pc, 0, want))
						{
							tx0 = std::min(tx0, i);
							ty0 = std::min(ty0, j);
							tx1 = std::max(tx1, i);
							ty1 = std::max(ty1, j);
						}
					}
				const bool placed = lo_x + 2 >= 40 + 2 * tx0 && lo_x <= 40 + 2 * tx0 + 2 && lo_y + 2 >= 140 + 2 * ty0 && lo_y <= 140 + 2 * ty0 + 2 && hi_x + 2 >= 40 + 2 * tx1 + 1 &&
					hi_x <= 40 + 2 * tx1 + 3;
				check(placed && behind == 0, "names over the screen: at twice the size where A's glyph would be without the jitter (box " + std::to_string(lo_x) + "," +
					std::to_string(lo_y) + "-" + std::to_string(hi_x) + "," + std::to_string(hi_y) + "), the one behind the wall hidden by A's depth");
				check(g.complaints() == 0, "names over the screen: the debug layer said nothing");
				alloc->Reset();
				list->Reset(alloc, nullptr);
				screen.release();
			}
			s.release();
		}

		// ---- the grid: a cell's outline seen from above (an orthographic view: world x, z onto the screen)
		{
			surfaces s;
			s.make(g.device, 128, 128, true);
			w::grid_frame &f = w::grid_slot(5);
			f.native = true;
			f.count = 2;
			const float vp[16] = { 0.5f, 0, 0, 0, 0, 0, 0.1f, 0, 0, 0.5f, 0, 0, 0, 0, 0.5f, 1 };
			std::memcpy(f.view_proj, vp, 64);
			const float a[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
			for (int c = 0; c < 4; ++c)
			{
				f.lines[0].p[c][0] = a[c][0];
				f.lines[0].p[c][1] = 0.0f;
				f.lines[0].p[c][2] = a[c][1];
				f.lines[1].p[c][0] = a[c][0] * 0.5f;
				f.lines[1].p[c][1] = 0.0f;
				f.lines[1].p[c][2] = a[c][1] * 0.5f;
			}
			f.lines[0].colour = 0x80FF6060;
			f.lines[0].keep_z = 0;
			f.lines[1].colour = 0x808080FF;
			f.lines[1].keep_z = 1;
			w::grid_publish(f, 5);
			const float black[4] = { 0, 0, 0, 0 };
			list->ClearRenderTargetView(s.rtv(), black, 0, nullptr);
			list->ClearDepthStencilView(s.dsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			check(r.grid(list, s.target(true), f, 5, why), "the grid recorded" + why);
			s.copy_out(list);
			g.run(list);
			s.read();
			uint32_t red = 0, blue = 0, other = 0;
			for (UINT y = 0; y < s.h; ++y)
				for (UINT x = 0; x < s.w; ++x)
				{
					const uint8_t *p = s.at(x, y);
					if ((p[0] | p[1] | p[2]) == 0)
						continue;
					if (p[0] == 0xFF && p[1] == 0x60 && p[2] == 0x60 && p[3] == 0x80)
						red++;
					else if (p[0] == 0x80 && p[1] == 0x80 && p[2] == 0xFF && p[3] == 0x80)
						blue++;
					else
						other++;
				}
			check(red > 4 * 50 && blue > 4 * 25 && other == 0, "the grid: two square outlines, each in its colour exactly (" + std::to_string(red) + " red, " + std::to_string(blue) +
				" blue, " + std::to_string(other) + " other pixels)");
			const uint8_t *inside = s.at(64, 64);
			check((inside[0] | inside[1] | inside[2]) == 0, "the grid: outlines only (the middle left as it was)");
			check(g.complaints() == 0, "the grid: the debug layer said nothing");
			s.release();
		}
		list->Release();
		alloc->Release();
		r.release();
	}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	dumps();
	recording();
	// (the rest needs the dump's key tables for nothing, but the DAT reader takes the game's folder)
	oracle(argc > 1 ? argv[1] : "build\\test\\world_oracle.txt");
	warp(argc > 1 ? argv[1] : "build\\test\\world_oracle.txt");
	std::printf(failures == 0 ? "\nall passed (%d)\n" : "\n%d failed\n", failures == 0 ? passes : failures);
	return failures == 0 ? 0 : 1;
}
