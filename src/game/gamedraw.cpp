/*
 * The game's own zone and character drawing switched off (gamedraw.hpp; research/ffxi-re/notes-offswitch.md).
 *
 * zone: 17B510 (the CTsZoneMap task's frame) sets up the camera, lights, fog and culling, then walks both map slots'
 * objects into 17C9C0 (the per-object draw, thiscall, 3 arguments) and the lamp-lit models into 17ADE0 (thiscall, 1
 * argument). Both made to return at once (ret 0Ch, ret 4): the walk and its frustum tests still run, the setup and the
 * slot tail's state resets too. (17DE20, the other per-object draw, is only used when renderer +0x3949C is 2, and the
 * game only ever sets 0 or 1: left alone.)
 *
 * chars: 02B6E0 (the model draw, from the actor draw 0CBDE0) poses the model (02C2D0) and then draws it: meshes,
 * silhouettes, overlays, ground shadows. The "mov ecx, imm32" right after the posing (02B7E3) becomes a short jump to
 * the function's own early return (02B7B0: model +0xB4 = triangles drawn, 0 here; ret 1Ch), at the same stack depth.
 * Each site is found by its pattern (unique in both dumps) and checked to be the game's bytes before it's written.
 */

#include "gamedraw.hpp"
#include <cstring>

namespace hifi::game::gamedraw
{
	namespace
	{
		constexpr const char *kZoneDraw = "8B44240C81ECC401000085C053";              // 17C9C0, the entry
		constexpr const char *kZoneLit = "515355568BF15733FFC78638910300FFFFFFFF";   // 17ADE0, the entry
		constexpr const char *kCharsJump = "84C00F84????????B9????????E8????????C705????????000080BF"; // 02B7DB; the mov at +8
		constexpr intptr_t kCharsJumpAt = 8;
		constexpr const char *kCharsReturn = "A1????????5F8985B40000005E5D5B83C47CC21C00"; // 02B7B0, its early return

		constexpr uint8_t kRet0C[3] = { 0xC2, 0x0C, 0x00 };
		constexpr uint8_t kRet04[3] = { 0xC2, 0x04, 0x00 };

		struct patch
		{
			uintptr_t at = 0;
			uint8_t saved[3] = {}, now[3] = {};
			size_t size = 0;
		};

		struct state
		{
			bool found = false;
			std::string find_why;
			patch zone[2], chars[1];
			bool zone_off = false, chars_off = false;
			sites s;
		};
		state g;

		bool fill(patch &p, uintptr_t at, const uint8_t *now, size_t size)
		{
			p.at = at;
			p.size = size;
			std::memcpy(p.now, now, size);
			return read(at, p.saved, size);
		}

		// Every site, once; false and why when one isn't where it should be
		bool find_all(std::string &why)
		{
			if (g.found)
				return true;
			if (!g.find_why.empty())
			{
				why = g.find_why;
				return false;
			}
			const uintptr_t draw = find(kZoneDraw), lit = find(kZoneLit), jump_m = find(kCharsJump), ret = find(kCharsReturn);
			g.s = { draw, lit, jump_m != 0 ? jump_m + kCharsJumpAt : 0, ret };
			const uintptr_t jump = g.s.chars_jump;
			const intptr_t rel = intptr_t(ret) - intptr_t(jump + 2);
			if (draw == 0 || lit == 0)
				why = "the game's zone draw wasn't found";
			else if (jump == 0 || ret == 0)
				why = "the game's model draw wasn't found";
			else if (rel < -128 || rel >= 0)
				why = "the game's model draw isn't laid out as expected (its early return isn't just before the draws)";
			else
			{
				const uint8_t jmp[2] = { 0xEB, uint8_t(int8_t(rel)) };
				if (!fill(g.zone[0], draw, kRet0C, 3) || !fill(g.zone[1], lit, kRet04, 3) || !fill(g.chars[0], jump, jmp, 2))
					why = "the game's code couldn't be read";
				else if (g.chars[0].saved[0] != 0xB9)
					why = "the game's model draw isn't as expected (changed by something else?)";
			}
			if (!why.empty())
			{
				g.find_why = why;
				return false;
			}
			g.found = true;
			return true;
		}

		// A part's patches written in (off) or the game's bytes back; all or none
		bool apply(patch *ps, size_t n, bool off, std::string &why)
		{
			for (size_t i = 0; i < n; ++i)
				if (!code_is(ps[i].at, off ? ps[i].saved : ps[i].now, ps[i].size))
				{
					why = "something else has changed the game's code there: left as it is";
					return false;
				}
			for (size_t i = 0; i < n; ++i)
				if (!write_code(ps[i].at, off ? ps[i].now : ps[i].saved, ps[i].size))
				{
					for (size_t j = 0; j < i; ++j) // (the ones already written put back)
						write_code(ps[j].at, off ? ps[j].saved : ps[j].now, ps[j].size);
					why = "couldn't write the game's code";
					return false;
				}
			return true;
		}
	}

	bool set(part p, bool off, std::string &why)
	{
		bool &now = p == part::zone ? g.zone_off : g.chars_off;
		if (off == now)
			return true;
		if (!find_all(why))
			return false;
		const bool ok = p == part::zone ? apply(g.zone, 2, off, why) : apply(g.chars, 1, off, why);
		if (ok)
			now = off;
		return ok;
	}

	bool is_off(part p)
	{
		return p == part::zone ? g.zone_off : g.chars_off;
	}

	bool stop(std::string &why)
	{
		std::string a, b;
		const bool zone = set(part::zone, false, a), chars = set(part::chars, false, b);
		why = !a.empty() ? a : b;
		if (zone && chars)
			g = state();
		return zone && chars;
	}

	std::string status()
	{
		if (!g.zone_off && !g.chars_off)
			return "game drawing: all of it (/hifi gamedraw zone|chars switches the game's own off)";
		std::string s = "game drawing: ";
		s += g.zone_off ? "zone OFF" : "zone on";
		s += g.chars_off ? ", characters OFF" : ", characters on";
		return s + " (nothing in their depth: walls don't hide names or targets)";
	}

	sites where()
	{
		return g.s;
	}
}
