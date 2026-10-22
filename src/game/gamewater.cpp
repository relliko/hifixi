/*
 * The game's own water (gamewater.hpp): the worldfx Ashita addon's /worldfx gamewater (worldfx.lua, gw_memory to
 * gamewater_seen, and its unload), in C++.
 *
 * The game's own water: sheets of scrolling texture that each zone's effect generators lay over its rivers and lakes,
 * drawn by the effect-mesh routine (FFXiMain 183760, called only from 040DC4 in the generator mesh's draw 040C20).
 * Clouds, the sun, waterfalls and lamp glows are drawn the same way, so only the zone's water meshes are skipped, told
 * apart by the name the game keeps in each mesh (mesh +0x18: the 4-character name of the DAT's MMB it was built from).
 * gamewater_meshes.inc lists them for each zone (tools/gamewater_table.py, from the addon's gamewater.lua, which
 * reshade-worldfx's tools/game_water.py makes: the meshes lying on the water World FX draws).
 *
 * The call at 040DC4 goes to a small routine of ours instead: when the mesh's name is on its list it returns as 183760
 * does (ret 10h) without drawing, else it jumps on to 183760. The list is filled only in a zone World FX draws water in
 * (a worldfx-water\<zone>.bin in its folder and its Water setting on), else it's empty and everything is drawn. The
 * routine also keeps the last 64 names it saw (seen()).
 *
 * The game's ripples at a wading character's feet go too, in those zones, as World FX draws its own rings there.
 * They're effects the character's footstep code (0D4BC0) starts from the zone's footstep folder (fses, or fser in rain)
 * by name, the terrain's digit second: each step in water starts 'w800' or '0800' (deep water '9'; they start splashes
 * a80x/b80x of their own) through 0D63F0, and standing in deep water keeps 'i900' going (0D64D0, stopped by 0D6550 on
 * leaving). The step's call (0D5365) goes to a second routine that skips it when the name's terrain is 8 or 9, and
 * 0D64D0's jump past its spawn (0D64FA, taken when the folder isn't there) is always taken. Footsteps on other ground
 * are untouched.
 *
 * Hidden while HiFi XI leaves the game's water out (its HideGameWater setting); all put back on unload.
 */

#include "gamewater.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <set>
#include <vector>

namespace hifi::game::gamewater
{
	namespace
	{
		// Our memory, for both routines (the addon's GW)
		namespace gw
		{
			constexpr uint32_t count = 0x00;   // u32: names on the list (0: everything is drawn)
			constexpr uint32_t skipped = 0x04; // u32: draws skipped
			constexpr uint32_t seen_at = 0x08; // u32: where the next name seen goes (mod 64)
			constexpr uint32_t steps = 0x0C;   // u32: steps in water whose effect was skipped
			constexpr uint32_t names = 0x10;   // u32[64]: the names whose draws are skipped
			constexpr uint32_t seen = 0x110;   // u32[64]: the names of the last meshes that came by
			constexpr uint32_t code = 0x210;   // the routine in place of 183760 (67 bytes)
			constexpr uint32_t step = 0x260;   // the routine in place of the step effect's call (31 bytes)
			constexpr uint32_t size = 0x280;
			constexpr uint32_t max = 64;
			constexpr uint32_t code_size = 67;
			constexpr uint32_t step_size = 31;
		}

		constexpr const char *kSite = "8B87040100008D8FA000000050518B0D????????E8????????8A877B010000"; // 040DB0, the call at +0x14
		constexpr intptr_t kSiteAt = 0x14;
		constexpr uint8_t kDraw[] = { 0x81, 0xEC, 0x8C, 0x00, 0x00, 0x00, 0x53, 0x8B, 0x9C, 0x24, 0x94, 0x00, 0x00, 0x00 }; // 183760's first bytes
		constexpr const char *kStepSite = "3BC3740C39187408508BCEE8????????391D????????"; // 0D535D, the step's spawn at +8
		constexpr intptr_t kStepAt = 8;
		constexpr uint8_t kStepCall[] = { 0x50, 0x8B, 0xCE, 0xE8 }; // push eax; mov ecx, esi; call
		constexpr uint8_t kStepSpawn[] = { 0x57, 0x8B, 0xF9, 0x8B, 0x07, 0xFF, 0x90, 0xE4, 0x01, 0x00, 0x00 }; // 0D63F0's first bytes
		constexpr const char *kRingSite = "8B8600080000C686E80900000185C074506869393030"; // 0D64EB, the jump at +0xF
		constexpr intptr_t kRingAt = 0xF;
		constexpr uint8_t kJe = 0x74, kJmp = 0xEB;

		// The zones' water meshes: each zone's names, 4 bytes each (spaces kept: 'ka2 ')
		struct water_meshes
		{
			uint16_t zone;
			uint16_t count;
			const char *names;
		};
		constexpr water_meshes kWaterMeshes[] = {
#include "gamewater_meshes.inc"
		};

		constexpr const char *kSettingOff = "not left out: the \"The game's own\" tab of HiFi XI's menu";

		struct state
		{
			uintptr_t site = 0, draw = 0, mem = 0; // the call, 183760, our memory
			uint8_t saved[5] = {};                 // the call's bytes
			bool off = false, routed = false;
			std::string route_why;                 // (empty: routed as asked)
			std::string list;                      // the names on the routine's list now (joined)
			std::string why = "not hidden yet";    // why it's empty (empty: hidden)
			uint32_t zone = 0;                     // the zone (and folder) has_water was looked at for
			std::string folder;
			bool has_water = false;
			uint64_t frames = 0, last_frames = 0;
			uint32_t last_skipped = 0;
			double rate = 0.0;
		};
		state g;

		// The footstep sites: the step effect's spawn (its 8 bytes, 0D63F0) and the deep-water ring's jump
		struct feet_state
		{
			uintptr_t step = 0, spawn = 0, ring = 0;
			uint8_t step_saved[8] = {};
			bool hidden = false;
			std::string why; // (empty: as asked)
		};
		feet_state g_feet;

		// A routine's bytes as they're laid down, its addresses as little-endian words
		struct code_bytes
		{
			uint8_t bytes[80] = {};
			size_t n = 0;
			bool over = false;

			code_bytes &op(std::initializer_list<uint8_t> b)
			{
				for (const uint8_t x : b)
				{
					if (n >= sizeof(bytes))
					{
						over = true;
						return *this;
					}
					bytes[n++] = x;
				}
				return *this;
			}
			code_bytes &at(uint32_t address)
			{
				if (n + 4 > sizeof(bytes))
				{
					over = true;
					return *this;
				}
				put32(bytes + n, address);
				n += 4;
				return *this;
			}
		};

		// Our memory, for both routines (zeroed; executable)
		bool gw_memory(std::string &why)
		{
			if (g.mem == 0)
			{
				const uintptr_t mem = alloc_code(gw::size);
				if (mem == 0)
				{
					why = "couldn't get memory for it";
					return false;
				}
				g.mem = mem;
			}
			return true;
		}

		// Our routine (thiscall, as 183760: ecx and the 4 arguments are left as they are; eax and edx are free); false
		// if it isn't 67 bytes
		bool gw_code(uint32_t mem, uint32_t draw, code_bytes &c)
		{
			const uint32_t code = mem + gw::code;
			c.op({ 0x8B, 0x44, 0x24, 0x08 });               // 00        mov  eax, [esp+8]            the mesh
			c.op({ 0x85, 0xC0 });                           // 04        test eax, eax
			c.op({ 0x74, 0x36 });                           // 06        jz   draw
			c.op({ 0x8B, 0x40, 0x18 });                     // 08        mov  eax, [eax+18h]          its name
			c.op({ 0x8B, 0x15 }).at(mem + gw::seen_at);     // 0B        mov  edx, [seen_at]
			c.op({ 0x83, 0xE2, 0x3F });                     // 11        and  edx, 63
			c.op({ 0x89, 0x04, 0x95 }).at(mem + gw::seen);  // 14        mov  [seen+edx*4], eax
			c.op({ 0xFF, 0x05 }).at(mem + gw::seen_at);     // 1B        inc  dword [seen_at]
			c.op({ 0x8B, 0x15 }).at(mem + gw::count);       // 21        mov  edx, [count]
			c.op({ 0x85, 0xD2 });                           // 27 next:  test edx, edx
			c.op({ 0x74, 0x13 });                           // 29        jz   draw
			c.op({ 0x4A });                                 // 2B        dec  edx
			c.op({ 0x3B, 0x04, 0x95 }).at(mem + gw::names); // 2C        cmp  eax, [names+edx*4]
			c.op({ 0x75, 0xF2 });                           // 33        jne  next
			c.op({ 0xFF, 0x05 }).at(mem + gw::skipped);     // 35        inc  dword [skipped]
			c.op({ 0xC2, 0x10, 0x00 });                     // 3B        ret  10h                     not drawn
			c.op({ 0xE9 }).at(draw - (code + 67));          // 3E draw:  jmp  183760
			return !c.over && c.n == gw::code_size;
		}

		// The step's routine, called in place of "push eax; mov ecx, esi; call 0D63F0": eax = the effect's resource,
		// esi = the character, edi = the effect's name ('w800': its terrain digit second); false if it isn't 31 bytes
		bool step_code(uint32_t mem, uint32_t spawn, code_bytes &c)
		{
			const uint32_t code = mem + gw::step;
			c.op({ 0x8B, 0xD7 });                           // 00        mov  edx, edi                the name
			c.op({ 0xC1, 0xEA, 0x08 });                     // 02        shr  edx, 8
			c.op({ 0x80, 0xFA, 0x38 });                     // 05        cmp  dl, '8'                 shallow water
			c.op({ 0x74, 0x0E });                           // 08        je   skip
			c.op({ 0x80, 0xFA, 0x39 });                     // 0A        cmp  dl, '9'                 deep water
			c.op({ 0x74, 0x09 });                           // 0D        je   skip
			c.op({ 0x50 });                                 // 0F        push eax
			c.op({ 0x8B, 0xCE });                           // 10        mov  ecx, esi
			c.op({ 0xE8 }).at(spawn - (code + 23));         // 12        call 0D63F0
			c.op({ 0xC3 });                                 // 17        ret
			c.op({ 0xFF, 0x05 }).at(mem + gw::steps);       // 18 skip:  inc  dword [steps]
			c.op({ 0xC3 });                                 // 1E        ret
			return !c.over && c.n == gw::step_size;
		}

		bool gw_find(std::string &why)
		{
			if (g.site != 0)
				return true;
			const uintptr_t m = find(kSite);
			if (m == 0)
			{
				why = "the game's water draw wasn't found";
				return false;
			}
			const uintptr_t site = m + kSiteAt;
			const uintptr_t draw = rel_target(site);
			uint8_t op = 0;
			if (!read(site, op) || op != 0xE8 || !code_is(draw, kDraw, sizeof(kDraw)) || !read(site, g.saved, sizeof(g.saved)))
			{
				why = "the game's water draw isn't as expected (changed by something else?)";
				return false;
			}
			g.site = site;
			g.draw = draw;
			return true;
		}

		// The call to our routine, in place of the call to 183760
		void gw_call(uint8_t out[5])
		{
			out[0] = 0xE8;
			put32(out + 1, uint32_t(g.mem + gw::code - (g.site + 5)));
		}

		// The water sheets' routine in (while the game's water is hidden) or out
		bool route_sheets(bool on, std::string &why)
		{
			if (on == g.routed)
				return true;
			if (!gw_find(why))
				return false;
			uint8_t call[5];
			if (on)
			{
				if (!gw_memory(why))
					return false;
				code_bytes c;
				if (!gw_code(uint32_t(g.mem), uint32_t(g.draw), c))
				{
					why = "its routine came out the wrong size";
					return false;
				}
				std::memcpy(reinterpret_cast<void *>(g.mem + gw::code), c.bytes, gw::code_size);
				gw_call(call);
				if (!write_code(g.site, call, sizeof(call)))
				{
					why = "couldn't write the game's code";
					return false;
				}
			}
			else
			{
				gw_call(call);
				if (!code_is(g.site, call, sizeof(call)))
				{
					why = "something else has changed the game's call since: left as it is";
					return false;
				}
				if (!write_code(g.site, g.saved, sizeof(g.saved)))
				{
					why = "couldn't write the game's code";
					return false;
				}
			}
			g.routed = on;
			return true;
		}

		bool feet_find(std::string &why)
		{
			if (g_feet.step != 0)
				return true;
			uintptr_t s = find(kStepSite);
			uintptr_t r = find(kRingSite);
			if (s == 0 || r == 0)
			{
				why = "the game's footstep code wasn't found";
				return false;
			}
			s += kStepAt;
			r += kRingAt;
			const uintptr_t spawn = rel_target(s + 3);
			uint8_t ring = 0;
			if (!code_is(s, kStepCall, sizeof(kStepCall)) || !code_is(spawn, kStepSpawn, sizeof(kStepSpawn)) || !read(r, ring) ||
				ring != kJe || !read(s, g_feet.step_saved, sizeof(g_feet.step_saved)))
			{
				why = "the game's footstep code isn't as expected (changed by something else?)";
				return false;
			}
			g_feet.step = s;
			g_feet.spawn = spawn;
			g_feet.ring = r;
			return true;
		}

		// The call to the step's routine and 3 nops, in place of "push eax; mov ecx, esi; call 0D63F0"
		void step_call(uint8_t out[8])
		{
			out[0] = 0xE8;
			put32(out + 1, uint32_t(g.mem + gw::step - (g_feet.step + 5)));
			out[5] = out[6] = out[7] = 0x90;
		}

		// The wading ripples hidden (in a zone World FX draws water in) or shown
		bool hide_feet(bool on, std::string &why)
		{
			if (on == g_feet.hidden)
				return true;
			if (!feet_find(why))
				return false;
			uint8_t call[8];
			if (on)
			{
				if (!gw_memory(why))
					return false;
				code_bytes c;
				if (!step_code(uint32_t(g.mem), uint32_t(g_feet.spawn), c))
				{
					why = "its routine came out the wrong size";
					return false;
				}
				std::memcpy(reinterpret_cast<void *>(g.mem + gw::step), c.bytes, gw::step_size);
				step_call(call);
				if (!write_code(g_feet.step, call, sizeof(call)) || !write_code(g_feet.ring, &kJmp, 1))
				{
					why = "couldn't write the game's code";
					return false;
				}
			}
			else
			{
				step_call(call);
				if (!code_is(g_feet.step, call, sizeof(call)) || !code_is(g_feet.ring, &kJmp, 1))
				{
					why = "something else has changed the game's footstep code since: left as it is";
					return false;
				}
				if (!write_code(g_feet.step, g_feet.step_saved, sizeof(g_feet.step_saved)) || !write_code(g_feet.ring, &kJe, 1))
				{
					why = "couldn't write the game's code";
					return false;
				}
			}
			g_feet.hidden = on;
			return true;
		}

		// Whether World FX has water for the zone (its worldfx-water\<zone>.bin), looked at once a zone (and again if
		// its folder changes)
		bool has_water(const std::string &folder, uint32_t zone)
		{
			if (g.zone != zone || g.folder != folder)
			{
				g.zone = zone;
				g.folder = folder;
				g.has_water = has_zone_file("worldfx-water", zone);
			}
			return g.has_water;
		}

		// The zone's water meshes, nullptr when none are known
		const water_meshes *meshes_for(uint32_t zone)
		{
			for (const water_meshes &w : kWaterMeshes)
				if (w.zone == zone)
					return &w;
			return nullptr;
		}

		// Layers skipped a frame, measured over the last update
		void measure()
		{
			if (g.mem == 0)
				return;
			const uint32_t skipped = *reinterpret_cast<const volatile uint32_t *>(g.mem + gw::skipped);
			const uint64_t frames = g.frames - g.last_frames;
			g.rate = frames > 0 ? double(uint32_t(skipped - g.last_skipped)) / double(frames) : 0.0;
			g.last_skipped = skipped;
			g.last_frames = g.frames;
		}

		// A name off the list as the status shows it (its trailing spaces cut)
		std::string trimmed(std::string name)
		{
			while (!name.empty() && name.back() == ' ')
				name.pop_back();
			return name;
		}
	}

	// The setting: the game's water hidden (where World FX draws its own) or drawn
	bool route(bool hide, std::string &why)
	{
		g.off = hide;
		std::string reason;
		const bool ok = route_sheets(hide, reason);
		g.route_why = ok ? std::string() : reason;
		why = reason;
		return ok;
	}

	// Fills the routine's list for the zone, or empties it (and says why); hides the wading ripples wherever World FX
	// draws water
	void update(uint32_t zone)
	{
		const std::string folder = data_folder();
		std::string why;
		if (!g.off)
			why = std::string("drawn (") + kSettingOff + ")";
		else if (zone == 0)
			why = "drawn: no zone yet";
		else if (folder.empty())
			why = "drawn: World FX's folder isn't known";
		else if (!has_water(folder, zone))
			why = "drawn: World FX has no water for zone " + std::to_string(zone);
		else if (!setting_on("Water", true))
			why = "drawn: World FX's Water setting is off";
		std::string err;
		g_feet.why = hide_feet(why.empty(), err) ? std::string() : err;
		std::string list; // (the names, 4 bytes each)
		if (why.empty())
		{
			const water_meshes *w = meshes_for(zone);
			if (w == nullptr)
				why = "drawn: no water meshes known for zone " + std::to_string(zone);
			else
				list.assign(w->names, size_t(std::min<uint32_t>(w->count, gw::max)) * 4);
		}
		if (why.empty() && !g.routed)
		{
			why = "drawn: " + (!g.route_why.empty() ? g.route_why : std::string("its draw isn't routed"));
			list.clear();
		}
		g.why = why;
		if (g.mem != 0 && list != g.list)
		{
			volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(g.mem);
			const uint32_t n = uint32_t(list.size() / 4);
			p[gw::count / 4] = 0; // (the routine never sees half a list)
			for (uint32_t i = 0; i < n; ++i)
				p[gw::names / 4 + i] = name_value(list.data() + i * 4);
			p[gw::count / 4] = n;
			g.list = list;
		}
		measure();
	}

	void count_frame()
	{
		g.frames++;
	}

	bool stop(std::string &why)
	{
		std::string sheets_why, steps_why;
		const bool sheets = route_sheets(false, sheets_why);
		const bool steps = hide_feet(false, steps_why);
		if (!sheets || !steps)
		{
			why = !sheets && !steps ? sheets_why + "; " + steps_why : !sheets ? sheets_why : steps_why;
			return false; // (something else may still call into it: its memory stays)
		}
		free_code(g.mem);
		g = state();
		g_feet = feet_state();
		return true;
	}

	std::string status()
	{
		std::string s;
		if (!g.why.empty() || g.list.empty())
			s = !g.why.empty() ? g.why : std::string("drawn");
		else
		{
			std::string names;
			const size_t n = g.list.size() / 4;
			for (size_t i = 0; i < n; ++i)
				names += (i > 0 ? " " : "") + trimmed(g.list.substr(i * 4, 4));
			char rate[32];
			std::snprintf(rate, sizeof(rate), "%.1f", g.rate);
			s = "hidden here (" + std::to_string(n) + " meshes: " + names + "; " + rate + " layers skipped a frame)";
		}
		if (!g_feet.why.empty())
			return s + "; wading ripples drawn: " + g_feet.why;
		if (g_feet.hidden)
		{
			const uint32_t steps = *reinterpret_cast<const volatile uint32_t *>(g.mem + gw::steps);
			return s + "; wading ripples hidden (" + std::to_string(steps) + " steps in water so far)";
		}
		return s;
	}

	// The meshes that came by the routine lately, hidden ones marked
	std::string seen()
	{
		if (!g.routed)
			return std::string("nothing seen: the game's water isn't being sorted (") + kSettingOff + ")";
		std::set<std::string> hidden;
		for (size_t i = 0; i + 4 <= g.list.size(); i += 4)
			hidden.insert(g.list.substr(i, 4));
		const volatile uint32_t *p = reinterpret_cast<const volatile uint32_t *>(g.mem + gw::seen);
		std::set<uint32_t> known;
		std::vector<std::string> out;
		for (uint32_t i = 0; i < gw::max; ++i)
		{
			const uint32_t v = p[i];
			if (v == 0 || !known.insert(v).second)
				continue;
			const std::string raw(reinterpret_cast<const char *>(&v), 4);
			out.push_back(name_text(v) + (hidden.count(raw) != 0 ? " (hidden)" : ""));
		}
		std::sort(out.begin(), out.end());
		std::string s;
		for (const std::string &n : out)
			s += (s.empty() ? "" : ", ") + n;
		return !s.empty() ? s : std::string("none yet");
	}

	sites where()
	{
		sites s;
		s.memory = g.mem;
		s.call = g.site;
		s.draw = g.draw;
		s.step = g_feet.step;
		s.spawn = g_feet.spawn;
		s.ring = g_feet.ring;
		return s;
	}
}
