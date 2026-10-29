/*
 * The world's sub-pixel jitter (jitter.hpp). The four calls, found by what's before them (the same bytes in both
 * clients known; only absolute addresses differ, wildcarded):
 *   015490 the camera:            fstp [esp+4]; fld [..]; fld [esi+2F4]; fpatan; fadd st0, st0; fstp [esp]; push edi; call
 *   0467E0 the effects' setup:    the same with [eax+2F4]; push edx
 *   177420 the shadow receivers:  fstp [esp+4]; fld [..]; fld [esp+20]; fpatan; fadd st0, st0; fstp [esp]; push eax; call
 *   046070 the effects' push:     mov eax, [esp+24]; fdivr [..]; mov edi, [esi]; fstp [esp]; push eax; push ebx; call
 * Each must call the same function (the builder) for any to be changed. Its rel32 then points at fov_rh, __stdcall
 * with the builder's arguments: no stub, nothing moved.
 */

#include "jitter.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace hifi::game::jitter
{
	namespace
	{
		struct call_site
		{
			const char *pattern;
			intptr_t call_at;
			const char *what;
		};
		const call_site kSites[4] = {
			{ "D95C2404DD05????????D986F4020000D9F3DCC0D91C2457E8", 0x18, "the camera's projection (015490)" },
			{ "D95C2404DD05????????D980F4020000D9F3DCC0D91C2452E8", 0x18, "the effects' projection (0467E0)" },
			{ "D95C2404DD05????????D9442420D9F3DCC0D91C2450E8", 0x16, "the shadow receivers' projection (177420)" },
			{ "8B442424D83D????????8B3ED91C245053E8", 0x11, "the effects' depth push (046070)" },
		};
		// gGame (the camera's object: [0x4568FC]): its pointer's place, from the camera reader's pattern
		constexpr const char *kGamePat = "A1????????0594000000C3";

		using builder_fn = float *(__stdcall *)(float *out, float fovy, float aspect, float zn, float zf);

		struct state
		{
			uintptr_t calls[4] = {};
			uint8_t saved[4][5] = {}, ours[4][5] = {};
			uintptr_t builder = 0;
			uintptr_t game_ptr = 0;
			bool in = false;
			std::atomic<bool> on{ false };
			std::atomic<uint32_t> phases{ 8 };
			uint32_t index = 0;
			// (this frame's, read by the builders and the zone pass on the game's thread)
			offset now;
		};
		state g;

		float halton(uint32_t index, uint32_t base)
		{
			float f = 1.0f, r = 0.0f;
			while (index > 0)
			{
				f /= float(base);
				r += f * float(index % base);
				index /= base;
			}
			return r;
		}

		float *__stdcall fov_rh(float *out, float fovy, float aspect, float zn, float zf)
		{
			float *const r = reinterpret_cast<builder_fn>(g.builder)(out, fovy, aspect, zn, zf);
			// (NDC moves by minus _31, _32: w is the view depth, and clip x gains view z x _31 = -w x _31)
			out[8] -= g.now.ndc[0];
			out[9] -= g.now.ndc[1];
			return r;
		}

		void put_call(uint8_t *at, uintptr_t from, uintptr_t to)
		{
			at[0] = 0xE8;
			put32(at + 1, uint32_t(to - (from + 5)));
		}

		// A's size (the background resolution): gGame+0x18, +0x1A
		bool world_size(uint32_t &w, uint32_t &h)
		{
			if (g.game_ptr == 0)
			{
				const uintptr_t p = find(kGamePat);
				if (p == 0)
					return false;
				g.game_ptr = word(p + 1);
			}
			const uint32_t game = word(g.game_ptr);
			uint16_t size[2] = {};
			if (!pointer(game) || !read(game + 0x18, size, 4) || size[0] < 16 || size[1] < 16)
				return false;
			w = size[0];
			h = size[1];
			return true;
		}
	}

	bool start(std::string &why)
	{
		if (g.in)
			return true;
		uintptr_t calls[4] = {}, builder = 0;
		for (int i = 0; i < 4; ++i)
		{
			const uintptr_t m = find(kSites[i].pattern);
			uint8_t now = 0;
			if (m == 0 || !read(m + kSites[i].call_at, &now, 1) || now != 0xE8)
			{
				why = std::string(kSites[i].what) + " wasn't found";
				return false;
			}
			calls[i] = m + kSites[i].call_at;
			const uintptr_t target = rel_target(calls[i]);
			if (i == 0)
				builder = target;
			else if (target != builder)
			{
				why = std::string(kSites[i].what) + " calls another function than the camera's";
				return false;
			}
		}
		g.builder = builder;
		g.game_ptr = 0; // (looked for again: another image may have it elsewhere)
		for (int i = 0; i < 4; ++i)
		{
			read(calls[i], g.saved[i], 5);
			put_call(g.ours[i], calls[i], reinterpret_cast<uintptr_t>(&fov_rh));
		}
		for (int i = 0; i < 4; ++i)
			if (!write_code(calls[i], g.ours[i], 5))
			{
				// (the ones written put back)
				for (int k = 0; k < i; ++k)
					write_code(calls[k], g.saved[k], 5);
				why = "couldn't write the game's code";
				return false;
			}
		std::memcpy(g.calls, calls, sizeof(calls));
		g.index = 0;
		g.now = offset();
		g.in = true;
		return true;
	}

	bool stop(std::string &why)
	{
		if (!g.in)
			return true;
		g.on = false;
		g.now = offset();
		bool all = true;
		for (int i = 0; i < 4; ++i)
		{
			if (!code_is(g.calls[i], g.ours[i], 5))
			{
				all = false;
				continue;
			}
			if (!write_code(g.calls[i], g.saved[i], 5))
				all = false;
		}
		if (!all)
		{
			why = "something else has changed a projection call since: left as it is";
			return false;
		}
		g.in = false;
		std::memset(g.calls, 0, sizeof(g.calls));
		return true;
	}

	bool in()
	{
		return g.in;
	}

	void set_on(bool on)
	{
		g.on = on;
	}

	void set_phases(uint32_t phases)
	{
		g.phases = phases < 2 ? 2 : phases > 128 ? 128 : phases;
	}

	void sequence(uint32_t index, uint32_t phases, float out[2])
	{
		const uint32_t k = (index % (phases < 1 ? 1 : phases)) + 1;
		out[0] = halton(k, 2) - 0.5f;
		out[1] = halton(k, 3) - 0.5f;
	}

	void next_frame()
	{
		offset o;
		uint32_t w = 0, h = 0;
		if (g.in && g.on.load() && world_size(w, h))
		{
			sequence(g.index, g.phases.load(), o.px);
			o.ndc[0] = 2.0f * o.px[0] / float(w);
			o.ndc[1] = -2.0f * o.px[1] / float(h);
		}
		o.index = g.index++;
		g.now = o;
	}

	offset current()
	{
		return g.now;
	}

	std::string status()
	{
		if (!g.in)
			return "jitter: not in";
		char line[200];
		std::snprintf(line, sizeof(line), "jitter: the four projection calls through ours; %s, %u phases, frame %u (now %.3f, %.3f px)",
			g.on.load() ? "on" : "off (no offset)", g.phases.load(), g.now.index, g.now.px[0], g.now.px[1]);
		return line;
	}

	sites where()
	{
		sites s;
		std::memcpy(s.calls, g.calls, sizeof(g.calls));
		s.builder = g.builder;
		return s;
	}

	void build(float out[16], float fovy, float aspect, float zn, float zf)
	{
		fov_rh(out, fovy, aspect, zn, zf);
	}
}
