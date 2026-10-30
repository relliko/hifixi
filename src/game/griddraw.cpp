/*
 * The Mog House layout grid handed to HiFi XI (griddraw.hpp; research/ffxi-re/notes-world-passes.md 6).
 *
 *   1D5DD0: push esi; mov esi, ecx; call 1779C0; mov eax, [esp+8]; mov ecx, [esi+14h]; push eax; call 1CF110 (thiscall,
 *           ret 4); pop esi; ret 4
 *   1CF4BD, 1CF607: call 177B40 (cdecl; the corners float[4][4 floats], then R, G, B, A, keep-depth as bytes)
 *   177B40: ... 177C0F mov ecx, [wrapper]; lea edx, [esp+4]; push 18h; push edx; push 4; push 3; call 00CD00 (the line strip)
 * 177B40's colour: (A << 24) | (R << 16) | (G << 8) | B, A its 5th argument and R, G, B its 2nd to 4th.
 */

#include "griddraw.hpp"
#include "worldmark.hpp"
#include "../world/frames.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hifi::game::griddraw
{
	namespace
	{
		constexpr const char *kGridCall = "568BF1E8????????8B4424088B4E1450E8????????5EC20400"; // 1D5DD0, call at +0x10
		constexpr intptr_t kGridAt = 0x10;
		constexpr const char *kOutline1 = "D844241C50D9542440D9C9D95C2448D95C2450E8????????83C468C21400"; // call at +0x13
		constexpr intptr_t kOutline1At = 0x13;
		constexpr const char *kOutline2 = "C1EA10C1E90852518D54241C5052E8????????83C418"; // call at +0xE
		constexpr intptr_t kOutline2At = 0xE;
		constexpr const char *kOutline = "83EC788B44247C538A9C2494000000"; // 177B40
		constexpr const char *kWrapper = "8B0D????????8D5424046A18526A046A03E8"; // 177C0F, the wrapper's global at +2

		using grid_fn = uint32_t(__fastcall *)(uintptr_t, uintptr_t, uint32_t);
		using outline_fn = void(__cdecl *)(const float *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

		struct call_patch
		{
			uintptr_t at = 0;
			uint8_t saved[5] = {}, ours[5] = {};
		};

		struct state
		{
			bool in = false, native = false;
			sites s;
			call_patch calls[3]; // the grid's, the two outlines'
			uintptr_t grid = 0, outline = 0;
			world::grid_frame *f = nullptr;
			bool transforms = false; // (the frame's taken)
			counts n;
		};
		state g;

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

		// VIEW x PROJECTION from the wrapper's cache (WORLD is identity: 1779C0)
		bool view_proj(float out[16])
		{
			const uint32_t w = g.s.wrapper != 0 ? word(g.s.wrapper) : 0;
			float view[16], proj[16];
			if (!pointer(w) || !read(w + 0x97C, view, 64) || !read(w + 0x9BC, proj, 64))
				return false;
			multiply(view, proj, out);
			for (int i = 0; i < 16; ++i)
				if (!std::isfinite(out[i]))
					return false;
			return true;
		}

		bool retarget(call_patch &p, uintptr_t at, uintptr_t to, uintptr_t expect)
		{
			uint8_t now[5] = {};
			if (!read(at, now, 5) || now[0] != 0xE8 || (expect != 0 && rel_target(at) != expect))
				return false;
			p.at = at;
			std::memcpy(p.saved, now, 5);
			p.ours[0] = 0xE8;
			put32(p.ours + 1, uint32_t(to - (at + 5)));
			return true;
		}
	}

	uint32_t __fastcall on_grid(uintptr_t grid, uintptr_t, uint32_t arg)
	{
		const uint32_t seq = ++g.n.seq == 0 ? ++g.n.seq : g.n.seq;
		world::grid_frame &f = world::grid_slot(seq);
		f.native = g.native;
		f.count = f.dropped = 0;
		g.f = &f;
		g.transforms = false;
		g.n.frames++;
		const uint32_t r = reinterpret_cast<grid_fn>(g.grid)(grid, 0, arg);
		g.f = nullptr;
		// (outlines the transforms couldn't be read for went to the game: none drawn here then)
		if (f.native && !g.transforms)
			f.count = 0;
		g.n.last = f.count;
		world::grid_publish(f, seq);
		worldmark::send(worldmark::kGrid, seq);
		return r;
	}

	void __cdecl on_outline(const float *corners, uint32_t r, uint32_t gr, uint32_t b, uint32_t a, uint32_t keep_z)
	{
		world::grid_frame *const f = g.f;
		if (f != nullptr && f->native && corners != nullptr)
		{
			if (!g.transforms)
				g.transforms = view_proj(f->view_proj);
			if (g.transforms && f->count < world::kMaxGridLines)
			{
				world::grid_line &l = f->lines[f->count];
				float p[16];
				if (read(uintptr_t(corners), p, sizeof(p)))
				{
					for (int c = 0; c < 4; ++c)
						for (int k = 0; k < 3; ++k)
							l.p[c][k] = p[c * 4 + k];
					l.colour = (a & 0xFF) << 24 | (r & 0xFF) << 16 | (gr & 0xFF) << 8 | (b & 0xFF);
					l.keep_z = keep_z & 0xFF;
					f->count++;
					g.n.outlines++;
					return;
				}
			}
			else if (g.transforms)
				f->dropped++;
		}
		g.n.game++;
		reinterpret_cast<outline_fn>(g.outline)(corners, r, gr, b, a, keep_z);
	}

	bool start(std::string &why)
	{
		if (g.in)
			return true;
		const uintptr_t grid_call = find(kGridCall), o1 = find(kOutline1), o2 = find(kOutline2), outline = find(kOutline), wrapper = find(kWrapper);
		if (grid_call == 0 || o1 == 0 || o2 == 0 || outline == 0 || wrapper == 0)
		{
			why = "the layout grid's draw wasn't found";
			return false;
		}
		state s;
		s.s.grid_call = grid_call + kGridAt;
		s.s.grid = rel_target(s.s.grid_call);
		s.s.outline_calls[0] = o1 + kOutline1At;
		s.s.outline_calls[1] = o2 + kOutline2At;
		s.s.outline = outline;
		s.s.wrapper = word(wrapper + 2);
		if (!retarget(s.calls[0], s.s.grid_call, reinterpret_cast<uintptr_t>(&on_grid), 0) ||
			!retarget(s.calls[1], s.s.outline_calls[0], reinterpret_cast<uintptr_t>(&on_outline), outline) ||
			!retarget(s.calls[2], s.s.outline_calls[1], reinterpret_cast<uintptr_t>(&on_outline), outline))
		{
			why = "the layout grid's calls aren't as expected";
			return false;
		}
		bool ok = true;
		for (const call_patch &p : s.calls)
			ok = ok && write_code(p.at, p.ours, 5);
		if (!ok)
		{
			for (const call_patch &p : s.calls)
				write_code(p.at, p.saved, 5);
			why = "couldn't write the game's code";
			return false;
		}
		s.grid = s.s.grid;
		s.outline = outline;
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
				why = "something else has changed the layout grid's calls since: left as they are";
				return false;
			}
		bool ok = true;
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
			return "Mog House layout grid: drawn by the game";
		char line[300];
		std::snprintf(line, sizeof(line), "Mog House layout grid: %s; %llu frames in layout mode, %llu outlines drawn by HiFi XI, %llu by the game (the last frame %u)",
			g.native ? "HiFi XI draws it" : "the game draws it (HiFi XI's side not ready, or its markers not coming through)", static_cast<unsigned long long>(g.n.frames),
			static_cast<unsigned long long>(g.n.outlines), static_cast<unsigned long long>(g.n.game), g.n.last);
		return line;
	}

	sites where()
	{
		return g.s;
	}

	void set_callees(uintptr_t grid, uintptr_t outline)
	{
		g.grid = grid;
		g.outline = outline;
	}
}
