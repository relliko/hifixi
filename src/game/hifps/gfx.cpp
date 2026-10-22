/*
 * hifps's graphics settings (gfx.hpp): hifps.lua's /hifps gfx (gfx_block to night_stat), in C++. Its comments carried
 * over where they still hold.
 *
 * Graphics (stock by default; applied while hifps's frame step is on (hifps::on()), put back when it goes off and on
 * stop()).
 *   lamps/reach  Lamp light at night. gfx.asm's stub (stubs.inc) sits on the three ways a lamp reaches the picture:
 *                the zone shader's lamp constants (17D3AF), the fixed-function lamp lights 1-4 (17CC26, 17CCC2,
 *                17CD5E, 17CDF9) and a character's lamp-lit light (0CB872). 'lamps' scales the colour, 'reach' the
 *                range (the shader lights fully up to the range and not past it, so the pools grow with the same
 *                edge; characters keep the stock reach).
 *   moon/fill    Night lighting: the moonlight's colour and the ambient light that fills shadows, scaled where the
 *                client builds them (188CA0, 188E00), so the zone, characters and effects all follow; full from 20:00
 *                to 4:00 Vana'diel time, stock by day.
 *
 * The settings are HiFi XI's options ([hifps] gfx_lamps, gfx_reach, gfx_moon, gfx_fill: 1.0 = stock), kept in hifps's
 * ranges and to hundredths as its slider and settings file kept them (so the factors are the addon's, to the bit);
 * applied when one changes. Unlike the addon, the stub's memory is freed on stop() when nothing points into it.
 */

#include <windows.h>
#include "gfx.hpp"
#include "step.hpp" // (hifps::on(), hifps::fps())
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace hifi::game::hifps::gfx
{
	namespace
	{
#include "stubs.inc"

		namespace gs = stubs::gfx;

		constexpr const char *kSection = "hifps";
		// Graphics settings' ranges (1.0 = stock).
		constexpr double kLampsMin = 0.5, kLampsMax = 3.0, kReachMin = 1.0, kReachMax = 2.0;
		constexpr double kNightMin = 0.5, kNightMax = 2.5;

		struct lamp_call
		{
			const char *name, *pat;
			intptr_t at;
			uint32_t entry;
			int target; // -1: 'call [ecx+13Ch]', else kTargets' function its call must reach
		};
		const lamp_call kLampCalls[] = {
			// push 4; push <lamp constants>; push 15; ...; call [ecx+13Ch] (SetVertexShaderConstant)
			{ "lamp constants", "6A0468????????6A0F8B400C508B08FF913C010000", 15, gs::VSC_LAMP, -1 },
			// mov edx, [ebp+lamp n]; mov ecx, [renderer]; push esi; push edx; call SetLight
			{ "lamp light 1", "8B95B09403008B0D????????5652E8", 14, gs::LIGHT_LAMP1, 0 },
			{ "lamp light 2", "8B95B49403008B0D????????5652E8", 14, gs::LIGHT_LAMP2, 0 },
			{ "lamp light 3", "8B95B89403008B0D????????5652E8", 14, gs::LIGHT_LAMP3, 0 },
			{ "lamp light 4", "8B85BC9403008B0D????????5650E8", 14, gs::LIGHT_LAMP4, 0 },
			// mov eax, [esp+ebx+4Ch]; add esi, -3Ch; push eax; push esi; call vec3 *= float (a lamp on a character)
			{ "character lamp", "8B441C4C83C6C45056E8", 9, gs::ACTOR_LAMP, 1 },
		};
		// The functions those calls reach: sub esp, 68h; push ebx; mov ebx, [esp+74h] (SetLight) and mov eax, [esp+4];
		// fld [esp+8]; fmul [eax]; fstp [eax] (vec3 *= float).
		constexpr const char *kTargets[2] = { "83EC68538B5C2474", "8B442404D9442408D808D918" };

		// Night lighting: every sun/moon light and ambient colour the client uses comes from two builders on an
		// environment object (thiscall (out, kind), ret 8; kind 0 the zone, 1 characters): 188CA0 fills a directional
		// D3DLIGHT8 and 188E00 a D3DCOLOR. Their entries jump to gfx.asm's stub, which runs the builder from a copy of
		// its first instructions and scales the result by MOONF / AMBF. Those follow the Vana'diel hour: the full
		// setting from 20:00 to 4:00, stock from 7:00 to 17:00, eased between.
		struct night_hook
		{
			const char *name, *pat;
			uint32_t len, entry, cont;
		};
		const night_hook kNightHooks[] = {
			// sub esp, 30h; push ebx; push esi; mov esi, [esp+3Ch]; push edi; mov ebx, ecx; xor edi, edi; mov [esi+34h], edi
			{ "light builder", "83EC3053568B74243C578BD933FF897E34", 5, gs::LIGHT_HOOK, gs::LIGHT_CONT },
			// mov eax, [ecx-1Ch]; sub esp, 20h; cmp eax, 1; push esi; je
			{ "ambient builder", "8B41E483EC2083F8015674", 6, gs::AMB_HOOK, gs::AMB_CONT },
		};
		// The Vana'diel clock in Earth seconds: call <clock>; add eax, [offset]; ret (the radar's clock).
		constexpr const char *kClockPat = "E8????????0305????????C3";
		// What it calls: call <tick>; jmp <seconds>. The tick starts push esi; push edi; call <now>;
		// mov edi, [clock object]; mov esi, eax; push esi; mov eax, [edi]. The client sets that pointer while
		// it starts up; until then (the title screen, logging in) the clock reads through a null pointer
		// (FFXiMain+0DBF30): hifps 0.9.10's launch crash fix, which this follows.
		constexpr const char *kClockCall = "E8????????E9";
		constexpr const char *kClockTick = "5657E8????????8B3D????????8BF0568B07";

		// A call or entry changed: where, the game's bytes, ours
		struct patch
		{
			uintptr_t addr = 0;
			size_t len = 0;
			uint8_t backup[6] = {}, bytes[6] = {};
		};

		struct module_state
		{
			uintptr_t base = 0;       // gfx.asm's copy, kept once made (freed by stop() when nothing calls into it)
			bool lamps = false;       // while lamps are changed: the calls
			std::vector<patch> calls;
			bool night = false;       // while night lighting is changed: the builders' entries
			std::vector<patch> hooks;
			bool held = false;        // something else changed one of them since: the copy stays
			std::string err, nerr;    // why the lamps, night lighting could not be changed
			int clock_looked = 0;     // the Vana'diel clock: 0 not looked for yet, 1 found (clock), -1 not found
			uintptr_t clock = 0;
			uintptr_t clock_obj = 0;  // where the client keeps its clock object's address (0 until the clock runs)
			bool clock_faulted = false; // (the clock faulted when called: never called again)
			bool have_hour = false;   // the Vana'diel hour at the last update (fractional), how much it was night then
			double hour = 0.0, dark = 0.0;
			bool have_prev = false, have_rate = false;
			uint32_t prev[3] = {}, rate[3] = {}; // lamp counters: constants, lights, characters (a second)
			// The settings applied
			bool applied = false;
			double lamps_v = 1.0, reach_v = 1.0, moon_v = 1.0, fill_v = 1.0;
			// Once a second (gfx_tick), from present()
			bool timer = false;
			double tick_at = 0.0;
		};
		module_state g;
		uint32_t(__cdecl *g_clock_hook)() = nullptr;

		std::string fmt(const char *f, ...)
		{
			char buf[512];
			va_list a;
			va_start(a, f);
			std::vsnprintf(buf, sizeof(buf), f, a);
			va_end(a);
			return buf;
		}

		double round2(double v)
		{
			return std::floor(v * 100 + 0.5) / 100;
		}

		double clamp(double v, double lo, double hi)
		{
			return v < lo ? lo : v > hi ? hi : v;
		}

		uintptr_t gsym(uint32_t offset)
		{
			return g.base + offset;
		}

		void set32(uintptr_t at, uint32_t v)
		{
			std::memcpy(reinterpret_cast<void *>(at), &v, 4);
		}

		uint32_t get32(uintptr_t at)
		{
			uint32_t v = 0;
			std::memcpy(&v, reinterpret_cast<const void *>(at), 4);
			return v;
		}

		// (computed as the addon's Lua does, in double, stored as a float)
		void set_float(uintptr_t at, double v)
		{
			const float f = float(v);
			std::memcpy(reinterpret_cast<void *>(at), &f, 4);
		}

		// True if the bytes at addr match a hex pattern with ?? wildcards.
		bool bytes_match(uintptr_t addr, const char *pat)
		{
			const size_t n = std::strlen(pat) / 2;
			std::vector<uint8_t> t(n);
			if (!read(addr, t.data(), n))
				return false;
			for (size_t i = 0; i < n; ++i)
			{
				if (pat[2 * i] == '?')
					continue;
				const char two[3] = { pat[2 * i], pat[2 * i + 1], 0 };
				if (t[i] != uint8_t(std::strtoul(two, nullptr, 16)))
					return false;
			}
			return true;
		}

		// gfx.asm's copy, made once; 0 if it could not be.
		uintptr_t gfx_block()
		{
			if (g.base != 0)
				return g.base;
			const uintptr_t base = alloc_code(gs::size);
			if (base == 0)
				return 0;
			std::memcpy(reinterpret_cast<void *>(base), gs::bytes, gs::size);
			for (const uint32_t r : gs::relocs)
				set32(base + r, get32(base + r) + uint32_t(base));
			g.base = base;
			return base;
		}

		// Puts patched code back, unless something else has changed it since.
		void unpatch(std::vector<patch> &list)
		{
			for (const patch &c : list)
			{
				if (code_is(c.addr, c.bytes, c.len))
					write_code(c.addr, c.backup, c.len);
				else
					g.held = true;
			}
			list.clear();
		}

		void lamps_values()
		{
			const double b = g.lamps_v, r = g.reach_v;
			set_float(gsym(gs::BRIGHT), b);
			set_float(gsym(gs::REACH), r);
			set_float(gsym(gs::REACH2), r * r);
			set_float(gsym(gs::INV_REACH), 1 / r);
			set_float(gsym(gs::INV_REACH2), 1 / (r * r));
		}

		void lamps_off()
		{
			unpatch(g.calls);
			g.lamps = false;
		}

		// Empty, or why not.
		std::string lamps_on()
		{
			if (g.lamps)
			{
				lamps_values();
				return std::string();
			}
			struct site
			{
				const lamp_call *s;
				uintptr_t addr;
				size_t len;
			};
			std::vector<site> sites;
			uint32_t targets[2] = {};
			for (const lamp_call &s : kLampCalls)
			{
				const uintptr_t p = find(s.pat);
				if (p == 0)
					return fmt("the %s call was not found (changed client, or another addon patched it)", s.name);
				const uintptr_t at = p + s.at;
				if (s.target < 0)
				{
					if (!bytes_match(at, "FF913C010000"))
						return fmt("the %s call is not the stock code", s.name);
					sites.push_back({ &s, at, 6 });
				}
				else
				{
					const uint32_t t = rel_target(at);
					if (!bytes_match(t, kTargets[s.target]) || (targets[s.target] != 0 && targets[s.target] != t))
						return fmt("the %s call goes somewhere else (another addon hooks it)", s.name);
					targets[s.target] = t;
					sites.push_back({ &s, at, 5 });
				}
			}
			if (gfx_block() == 0)
				return "could not allocate memory";
			set32(gsym(gs::ORIG_SETLIGHT), targets[0]);
			set32(gsym(gs::ORIG_SCALE), targets[1]);
			lamps_values();
			g.lamps = true;
			g.calls.clear();
			g.have_prev = g.have_rate = false;
			for (const site &c : sites)
			{
				patch p;
				p.addr = c.addr;
				p.len = c.len;
				p.bytes[0] = 0xE8;
				put32(p.bytes + 1, uint32_t(gsym(c.s->entry) - (c.addr + 5)));
				if (c.len == 6)
					p.bytes[5] = 0x90;
				if (!read(c.addr, p.backup, c.len) || !write_code(c.addr, p.bytes, c.len))
					return "could not write the client's code";
				g.calls.push_back(p);
			}
			return std::string();
		}

		// Looks for the Vana'diel clock once (and its object's pointer, through the clock's own calls); whether it's there
		bool find_clock()
		{
			if (g.clock_looked != 0)
				return g.clock_looked > 0;
			g.clock_looked = -1;
			const uintptr_t p = find(kClockPat);
			if (p == 0)
				return false;
			const uintptr_t call = rel_target(p);
			if (!bytes_match(call, kClockCall))
				return false;
			const uintptr_t tick = rel_target(call);
			if (!bytes_match(tick, kClockTick))
				return false;
			g.clock_obj = word(tick + 9);
			g.clock = p;
			g.clock_looked = 1;
			return true;
		}

		// The game's clock called, under a guard: a fault in it (anything the check below doesn't foresee) leaves the
		// clock alone from then on instead of taking the game down. (No objects here: the guard can't unwind them.)
		bool call_clock(uintptr_t fn, uint32_t &t)
		{
			__try
			{
				t = reinterpret_cast<uint32_t(__cdecl *)()>(fn)();
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// The Vana'diel hour (0 to 24, fractional) from the client's clock; false if it was not found or the client hasn't
		// started it yet (calling it then would crash the game)
		bool vana_hour(double &h)
		{
			if (!find_clock() || g.clock_faulted)
				return false;
			uint32_t t = 0;
			if (g_clock_hook != nullptr)
				t = g_clock_hook();
			else
			{
				uint32_t object = 0;
				if (!read(g.clock_obj, object) || object == 0)
					return false;
				if (!call_clock(g.clock, t))
				{
					g.clock_faulted = true;
					return false;
				}
			}
			h = double(t % 3456) / 144;
			return true;
		}

		// How much it is night at hour h: 1 from 20:00 to 4:00, 0 from 7:00 to 17:00, linear between.
		double night_amount(bool known, double h)
		{
			if (!known)
				return 0;
			if (h >= 20 || h < 4)
				return 1;
			if (h < 7)
				return (7 - h) / 3;
			if (h < 17)
				return 0;
			return (h - 17) / 3;
		}

		// The factors for the current hour (once a second, and on changes).
		void night_values()
		{
			if (!g.night)
				return;
			g.have_hour = vana_hour(g.hour);
			g.dark = night_amount(g.have_hour, g.hour);
			set_float(gsym(gs::MOONF), 1 + (g.moon_v - 1) * g.dark);
			set_float(gsym(gs::AMBF), 1 + (g.fill_v - 1) * g.dark);
		}

		void night_off()
		{
			unpatch(g.hooks);
			g.night = false;
		}

		// Empty, or why not.
		std::string night_on()
		{
			if (g.night)
			{
				night_values();
				return std::string();
			}
			uintptr_t at[std::size(kNightHooks)] = {};
			for (size_t i = 0; i < std::size(kNightHooks); ++i)
			{
				at[i] = find(kNightHooks[i].pat);
				if (at[i] == 0)
					return fmt("the %s was not found (changed client, or another addon hooks it)", kNightHooks[i].name);
			}
			// (before the client starts its clock the factors stay stock; the once-a-second tick looks again)
			if (!find_clock())
				return "the Vana'diel clock was not found";
			if (gfx_block() == 0)
				return "could not allocate memory";
			for (size_t i = 0; i < std::size(kNightHooks); ++i)
				set32(gsym(kNightHooks[i].cont), uint32_t(at[i] + kNightHooks[i].len));
			g.night = true;
			g.hooks.clear();
			night_values();
			for (size_t i = 0; i < std::size(kNightHooks); ++i)
			{
				patch p;
				p.addr = at[i];
				p.len = kNightHooks[i].len;
				p.bytes[0] = 0xE9;
				put32(p.bytes + 1, uint32_t(gsym(kNightHooks[i].entry) - (at[i] + 5)));
				for (size_t k = 5; k < p.len; ++k)
					p.bytes[k] = 0x90;
				if (!read(p.addr, p.backup, p.len) || !write_code(p.addr, p.bytes, p.len))
					return "could not write the client's code";
				g.hooks.push_back(p);
			}
			return std::string();
		}

		// Applies the settings; what failed is in g.err / g.nerr.
		void gfx_apply()
		{
			if (g.lamps_v != 1 || g.reach_v != 1)
			{
				g.err = lamps_on();
				if (!g.err.empty())
					lamps_off();
			}
			else
			{
				lamps_off();
				g.err.clear();
			}
			if (g.moon_v != 1 || g.fill_v != 1)
			{
				g.nerr = night_on();
				if (!g.nerr.empty())
					night_off();
			}
			else
			{
				night_off();
				g.nerr.clear();
			}
		}

		void gfx_off()
		{
			lamps_off();
			night_off();
		}

		// Once a second: the night factors, and how many lamps went through the stub.
		void gfx_tick()
		{
			night_values();
			if (!g.lamps)
				return;
			const uint32_t now_c[3] = { get32(gsym(gs::S_VSC)), get32(gsym(gs::S_LIGHT)), get32(gsym(gs::S_ACTOR)) };
			if (g.have_prev)
			{
				for (size_t i = 0; i < 3; ++i)
					g.rate[i] = now_c[i] - g.prev[i];
				g.have_rate = true;
			}
			std::memcpy(g.prev, now_c, sizeof(now_c));
			g.have_prev = true;
		}

		std::string gfx_stat()
		{
			const double f = hifps::fps();
			if (!g.lamps || !g.have_rate || f <= 0)
				return std::string();
			const uint32_t *r = g.rate;
			if (uint64_t(r[0]) + r[1] + r[2] == 0)
				return "no lamps lit here right now";
			return fmt("a frame: %.0f lamp-lit meshes, %.0f lamp lights, %.0f lamps on characters", r[0] / f, r[1] / f, r[2] / f);
		}

		std::string night_stat()
		{
			if (!g.night)
				return std::string();
			if (!g.have_hour)
				return g.clock_faulted ? "The game's clock faulted when asked the hour: night lighting stays stock"
					: "Night lighting starts once the game's clock runs (after logging in)";
			const double h = std::floor(g.hour);
			return fmt("Vana'diel %02d:%02d: night lighting at %d%%", int(h), int(std::floor((g.hour - h) * 60)), int(std::floor(g.dark * 100 + 0.5)));
		}
	}

	void update()
	{
		if (!hifps::on())
		{
			// (stock while the frame step is off, as the addon's)
			if (g.applied)
				gfx_off();
			g.applied = false;
			return;
		}
		const double lamps = round2(clamp(option_number(kSection, "gfx_lamps", 1.0f), kLampsMin, kLampsMax));
		const double reach = round2(clamp(option_number(kSection, "gfx_reach", 1.0f), kReachMin, kReachMax));
		const double moon = round2(clamp(option_number(kSection, "gfx_moon", 1.0f), kNightMin, kNightMax));
		const double fill = round2(clamp(option_number(kSection, "gfx_fill", 1.0f), kNightMin, kNightMax));
		if (g.applied && lamps == g.lamps_v && reach == g.reach_v && moon == g.moon_v && fill == g.fill_v)
			return;
		if (!g.applied)
			g.timer = false;
		g.lamps_v = lamps;
		g.reach_v = reach;
		g.moon_v = moon;
		g.fill_v = fill;
		g.applied = true;
		gfx_apply();
	}

	void present(double now)
	{
		if (!g.applied)
			return;
		if (!g.timer)
		{
			g.timer = true;
			g.tick_at = now;
		}
		else if (now - g.tick_at >= 1.0)
		{
			g.tick_at = now;
			gfx_tick();
		}
	}

	void stop()
	{
		gfx_off();
		if (g.base != 0 && !g.held)
			free_code(g.base);
		g = module_state();
	}

	std::vector<std::string> status()
	{
		std::vector<std::string> lines;
		if (!g.applied)
		{
			lines.push_back("off: hifps's frame step is off, so all stock");
			return lines;
		}
		if (g.lamps_v == 1 && g.reach_v == 1)
			lines.push_back("lamps: stock");
		else
		{
			const std::string s = gfx_stat();
			lines.push_back(fmt("lamps: brightness %.2f, reach %.2f", g.lamps_v, g.reach_v) + (!g.err.empty() ? ": not changed: " + g.err :
				!s.empty() ? " (" + s + ")" : std::string()));
		}
		if (g.moon_v == 1 && g.fill_v == 1)
			lines.push_back("night lighting: stock");
		else
		{
			const std::string s = night_stat();
			lines.push_back(fmt("night lighting: moonlight %.2f, shadow fill %.2f", g.moon_v, g.fill_v) + (!g.nerr.empty() ? ": not changed: " + g.nerr :
				!s.empty() ? " (" + s + ")" : std::string()));
		}
		return lines;
	}

	view state()
	{
		view v;
		v.lamps = g.lamps;
		v.night = g.night;
		v.lamps_err = g.err;
		v.night_err = g.nerr;
		v.lamps_stat = gfx_stat();
		v.night_stat = night_stat();
		return v;
	}

	void set_clock(uint32_t(__cdecl *clock)())
	{
		g_clock_hook = clock;
	}

	sites where()
	{
		sites s;
		s.memory = g.base;
		for (const patch &p : g.calls)
			s.lamps.push_back(p.addr);
		for (const patch &p : g.hooks)
			s.night.push_back(p.addr);
		return s;
	}
}
