/*
 * The sky dome handed to HiFi XI (skydraw.hpp; research/ffxi-re/client-notes.md "Sky").
 *
 * 03DD00: call 03D640; mov eax, [esp+2Ch]; mov ecx, [esp+2Dh]; mov edx, [esp+2Eh]; push eax; push ecx; push edx;
 * call 014D90 (the clear colour again); mov eax, [wrapper]; add esp, 20h; mov dword [eax+178h], 128.0. The call's
 * target becomes on_dome (cdecl, the same five arguments: the caller takes them off).
 *
 * 03D8D1 (in 03D640): cmp dword [04AECB08], 14h; jle (count the strip, skip its draw): the session's first 21 strips.
 *
 * The device's transforms when the dome is drawn: the wrapper's cache (00BD50: WORLD +0x93C, VIEW +0x97C, PROJECTION
 * +0x9BC, 16 floats each), which 03DB60 has set by then (WORLD from the effect system's stack 06A480, VIEW its copy of
 * the frame's with the translation zeroed 06A520).
 */

#include "skydraw.hpp"
#include "slotmark.hpp"
#include "worldmark.hpp"
#include "../world/dome.hpp"
#include "../world/frames.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hifi::game::skydraw
{
	namespace
	{
		constexpr const char *kSite = "E8????????8B44242C8B4C242D8B54242E505152E8????????A1????????83C420C7807801000000000043"; // 03DD00
		constexpr intptr_t kWrapperAt = 0x1A;
		constexpr const char *kDome = "8B44240883EC3C48558B6C244C85C00F8E"; // 03D640
		constexpr const char *kCounter = "833D????????147E1A8B0D????????6AFF6A008D442D006A0550"; // 03D8D1
		constexpr intptr_t kCounterAt = 2;
		constexpr uint32_t kSkipped = 0x14; // (a strip is drawn once the counter is past this)

		using dome_fn = void(__cdecl *)(float radius, uint32_t stops, uint32_t segments, const uint32_t *colours, const float *heights);

		struct state
		{
			uintptr_t call = 0, original = 0, wrapper = 0, counter = 0;
			uint8_t saved[5] = {}, ours[5] = {};
			bool in = false, native = false;
			counts n;
		};
		state g;

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

		// The device's WORLD x VIEW x PROJECTION as the wrapper caches them; false when they can't be read
		bool transforms(float out[16])
		{
			const uint32_t w = g.wrapper != 0 ? word(g.wrapper) : 0;
			float world[16], view[16], proj[16], wv[16];
			if (!pointer(w) || !read(w + 0x93C, world, 64) || !read(w + 0x97C, view, 64) || !read(w + 0x9BC, proj, 64))
				return false;
			multiply(world, view, wv);
			multiply(wv, proj, out);
			for (int i = 0; i < 16; ++i)
				if (!std::isfinite(out[i]))
					return false;
			return true;
		}

		// The strips 03D640 would leave out now, the counter moved on as it would move it
		uint32_t skipped(const float *heights)
		{
			if (g.counter == 0)
				return 0;
			uint32_t count = 0;
			if (!read(g.counter, count) || int32_t(count) > int32_t(kSkipped))
				return 0;
			uint32_t strips = 0;
			for (int b = 0; b + 1 < 8; ++b)
				strips += world::dome::band_strips(heights[b], heights[b + 1]);
			uint32_t skip = 0;
			while (skip < strips && int32_t(count) <= int32_t(kSkipped))
			{
				count++;
				skip++;
			}
			write_code(g.counter, &count, 4); // (a data word: written as the game writes it)
			return skip;
		}
	}

	void __cdecl on_dome(float radius, uint32_t stops, uint32_t segments, const uint32_t *colours, const float *heights)
	{
		g.n.calls++;
		const uint32_t seq = ++g.n.seq == 0 ? ++g.n.seq : g.n.seq;
		world::sky_frame &f = world::sky_slot(seq);
		f.native = false;
		bool native = g.native && stops == 8 && segments >= 3 && segments <= 1024 && std::isfinite(radius) && colours != nullptr && heights != nullptr;
		if (native)
		{
			native = transforms(f.world_view_proj) && read(uintptr_t(colours), f.colours, sizeof(f.colours)) && read(uintptr_t(heights), f.heights, sizeof(f.heights));
			for (int i = 0; native && i < 8; ++i)
				native = std::isfinite(f.heights[i]);
		}
		if (native)
		{
			f.radius = radius;
			f.segments = segments;
			f.skip = skipped(f.heights);
			f.native = true;
			g.n.native++;
		}
		else
		{
			g.n.game++;
			reinterpret_cast<dome_fn>(g.original)(radius, stops, segments, colours, heights);
		}
		world::sky_publish(f, seq);
		worldmark::send(slotmark::sky_end, seq);
	}

	bool start(std::string &why)
	{
		if (g.in)
			return true;
		const uintptr_t site = find(kSite), dome = find(kDome);
		if (site == 0 || dome == 0)
		{
			why = "the sky dome's call wasn't found";
			return false;
		}
		uint8_t saved[5];
		if (!read(site, saved, 5) || saved[0] != 0xE8 || rel_target(site) != dome)
		{
			why = "the sky dome's call isn't 03D640's";
			return false;
		}
		// (the strip counter: optional, without it the session's first strips aren't left out)
		uintptr_t counter = 0;
		if (const uintptr_t c = find(kCounter); c != 0 && c > dome && c < dome + 0x400)
			counter = word(c + kCounterAt);
		uint8_t ours[5] = { 0xE8 };
		put32(ours + 1, uint32_t(reinterpret_cast<uintptr_t>(&on_dome) - (site + 5)));
		if (!write_code(site, ours, 5))
		{
			why = "couldn't write the game's code";
			return false;
		}
		g.call = site;
		g.original = dome;
		g.wrapper = word(site + kWrapperAt);
		g.counter = counter;
		std::memcpy(g.saved, saved, 5);
		std::memcpy(g.ours, ours, 5);
		g.in = true;
		return true;
	}

	bool stop(std::string &why)
	{
		if (!g.in)
			return true;
		if (!code_is(g.call, g.ours, 5))
		{
			why = "something else has changed the sky dome's call since: left as it is";
			return false;
		}
		if (!write_code(g.call, g.saved, 5))
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
			return "sky dome: drawn by the game";
		char line[300];
		std::snprintf(line, sizeof(line), "sky dome: %s; %llu domes, %llu drawn by HiFi XI, %llu by the game%s",
			g.native ? "HiFi XI draws it" : "the game draws it (HiFi XI's side not ready, or its markers not coming through)",
			static_cast<unsigned long long>(g.n.calls), static_cast<unsigned long long>(g.n.native), static_cast<unsigned long long>(g.n.game),
			g.counter == 0 ? "; the session's strip counter not found" : "");
		return line;
	}

	sites where()
	{
		return { g.call, g.original, g.wrapper, g.counter };
	}

	void set_draw(uintptr_t draw)
	{
		g.original = draw;
	}
}
