/*
 * The game's rain and snow (gamerain.hpp): the worldfx Ashita addon's /worldfx gamerain (worldfx.lua, gr_code and
 * route_rain to gamerain_seen), in C++.
 *
 * The game's rain and snow are effects its weather starts (client-notes "Hiding one weather's visuals"). Every effect
 * element is drawn through 046070, which skips one whose +0x17A is set; the 6 bytes that read it (04609D, mov al,
 * [ebp+17Ah]) become a call to a routine of ours that also answers "skip" for an element whose generator ([[+0xFC]], a
 * type-5 resource) is a weather's (+0xDE bit 7, as the game marks them at load), isn't a sky layer (element flags
 * 0x400000: clouds stay) and whose weather (the name of the folder it's in, up its +0x28 parents, through clones) and
 * model (its model command's name, [+0xE4] + 0xC) are on the routine's list (model 0: any). Skipped elements run on
 * (their timers, fades and sound), only their draw is left out, as the game's own +0x17A does. The weather's records
 * still light, fog and draw the zone. The routine also keeps the last 32 weather elements it saw (seen()).
 *
 * The list is filled only while World FX draws its own rain or snow in the zone (a worldfx-rain\<zone>.bin in its
 * folder, the map both end on): the rain's entries with its Rain setting on, the snow's with its Snow setting on (and
 * their amounts over 0). Hidden while HiFi XI leaves the game's rain and snow out (its HideGameRain setting); put back on unload.
 */

#include "gamerain.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <set>
#include <vector>

namespace hifi::game::gamerain
{
	namespace
	{
		// The routine's memory (the addon's GR)
		namespace gr
		{
			constexpr uint32_t hidden = 0x00;  // u32: element draws skipped
			constexpr uint32_t seen_at = 0x04; // u32: where the next element seen goes (mod 32)
			constexpr uint32_t result = 0x08;  // u8: the routine's answer on its way out
			constexpr uint32_t list = 0x10;    // 16 pairs of u32: weather name, model name (0: any but the sky layers); a 0 name ends it
			constexpr uint32_t seen = 0x90;    // 32 pairs of u32: the weather and model of the last weather elements that came by
			constexpr uint32_t code = 0x190;   // the routine called in place of "mov al, [ebp+17Ah]" (207 bytes)
			constexpr uint32_t size = 0x260;
			constexpr uint32_t max = 16;
			constexpr uint32_t seen_max = 32;
			constexpr uint32_t code_size = 0xCF;
		}

		constexpr const char *kSite = "8D8DF8000000D91C2450E8????????8B00A3????????8A857A01000084C00F85"; // 046087, the mov at +0x16
		constexpr intptr_t kSiteAt = 0x16;
		constexpr uint8_t kSaved[6] = { 0x8A, 0x85, 0x7A, 0x01, 0x00, 0x00 };

		// What's hidden while World FX draws its rain: the weather (its folder's name; the game's second set as the
		// first) and the model (nullptr: everything but the sky). Thunderstorms (bolt) keep their lightning: only their
		// rain models go. Thunder (thdr) has no rain of its own in the game. Snow and blizzards (snow: while World FX
		// snows) lose everything but their sky layers (their hit3 flakes, the blizzard's smok). At most gr::max entries.
		struct hidden_entry
		{
			const char *weather, *model;
			bool snow;
		};
		constexpr hidden_entry kRainHidden[] = {
			{ "rain", nullptr, false }, { "squl", nullptr, false }, { "rai1", nullptr, false }, { "squ1", nullptr, false },
			{ "bolt", "rain", false }, { "bolt", "smok", false }, { "bolt", "asi1", false }, { "bolt", "hamo", false },
			{ "bol1", "rain", false }, { "bol1", "smok", false }, { "bol1", "asi1", false }, { "bol1", "hamo", false },
			{ "snow", nullptr, true }, { "bliz", nullptr, true }, { "sno1", nullptr, true }, { "bli1", nullptr, true },
		};

		constexpr const char *kSettingOff = "not left out: the \"The game's own\" tab of HiFi XI's menu";

		struct state
		{
			uintptr_t site = 0, mem = 0;
			bool off = false, routed = false;
			std::string route_why;               // (empty: routed as asked)
			std::string list;                    // the pairs on the routine's list now, 8 bytes each (weather, model)
			std::string why = "not hidden yet";  // (empty: hidden)
			std::string hiding;
			uint32_t zone = 0;                   // the zone (and folder) has_rain was looked at for
			std::string folder;
			bool has_rain = false;
			uint64_t frames = 0, last_frames = 0;
			uint32_t last_hidden = 0;
			double rate = 0.0;
		};
		state g;

		// The routine's bytes as they're laid down, its addresses as little-endian words
		struct code_bytes
		{
			uint8_t bytes[gr::code_size] = {};
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

		// The routine (called with ebp = the element; everything but al is kept); false if it isn't 207 bytes
		bool gr_code(uint32_t mem, code_bytes &c)
		{
			c.op({ 0x8A, 0x85, 0x7A, 0x01, 0x00, 0x00 });                       // 00        mov  al, [ebp+17Ah]           hidden already
			c.op({ 0x84, 0xC0 });                                               // 06        test al, al
			c.op({ 0x74, 0x01 });                                               // 08        jz   go
			c.op({ 0xC3 });                                                     // 0A        ret
			c.op({ 0x60 });                                                     // 0B go:    pushad
			c.op({ 0x31, 0xDB });                                               // 0C        xor  ebx, ebx                 the answer: draw
			c.op({ 0x8B, 0xB5, 0xFC, 0x00, 0x00, 0x00 });                       // 0E        mov  esi, [ebp+0FCh]          its generator's handle
			c.op({ 0x85, 0xF6 });                                               // 14        test esi, esi
			c.op({ 0x74, 0x5A });                                               // 16        jz   out
			c.op({ 0x8B, 0x36 });                                               // 18        mov  esi, [esi]               the generator
			c.op({ 0x85, 0xF6 });                                               // 1A        test esi, esi
			c.op({ 0x74, 0x54 });                                               // 1C        jz   out
			c.op({ 0x8A, 0x46, 0x24 });                                         // 1E        mov  al, [esi+24h]            a generator (type 5)
			c.op({ 0x24, 0x7F });                                               // 21        and  al, 7Fh
			c.op({ 0x3C, 0x05 });                                               // 23        cmp  al, 5
			c.op({ 0x75, 0x4B });                                               // 25        jne  out
			c.op({ 0xF6, 0x86, 0xDE, 0x00, 0x00, 0x00, 0x80 });                 // 27        test byte [esi+0DEh], 80h     a weather's
			c.op({ 0x74, 0x42 });                                               // 2E        jz   out
			c.op({ 0xF7, 0x85, 0x0C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00 }); // 30      test dword [ebp+10Ch], 400000h  a sky layer: drawn
			c.op({ 0x75, 0x36 });                                               // 3A        jnz  out
			c.op({ 0x31, 0xFF });                                               // 3C        xor  edi, edi                 its model's name
			c.op({ 0x8B, 0x86, 0xE4, 0x00, 0x00, 0x00 });                       // 3E        mov  eax, [esi+0E4h]          (its model command)
			c.op({ 0x85, 0xC0 });                                               // 44        test eax, eax
			c.op({ 0x74, 0x08 });                                               // 46        jz   nomodel
			c.op({ 0x80, 0x38, 0x01 });                                         // 48        cmp  byte [eax], 1
			c.op({ 0x75, 0x03 });                                               // 4B        jne  nomodel
			c.op({ 0x8B, 0x78, 0x0C });                                         // 4D        mov  edi, [eax+0Ch]
			c.op({ 0xB9, 0x04, 0x00, 0x00, 0x00 });                             // 50 nomodel: mov ecx, 4                 up to 4 parents
			c.op({ 0x8B, 0x46, 0x28 });                                         // 55 up:    mov  eax, [esi+28h]           the parent's handle
			c.op({ 0x85, 0xC0 });                                               // 58        test eax, eax
			c.op({ 0x74, 0x16 });                                               // 5A        jz   out
			c.op({ 0x8B, 0x30 });                                               // 5C        mov  esi, [eax]
			c.op({ 0x85, 0xF6 });                                               // 5E        test esi, esi
			c.op({ 0x74, 0x10 });                                               // 60        jz   out
			c.op({ 0x8A, 0x46, 0x24 });                                         // 62        mov  al, [esi+24h]
			c.op({ 0x24, 0x7F });                                               // 65        and  al, 7Fh
			c.op({ 0x3C, 0x01 });                                               // 67        cmp  al, 1                    a folder: the weather's
			c.op({ 0x74, 0x09 });                                               // 69        je   found
			c.op({ 0x3C, 0x05 });                                               // 6B        cmp  al, 5                    a clone's source
			c.op({ 0x75, 0x03 });                                               // 6D        jne  out
			c.op({ 0x49 });                                                     // 6F        dec  ecx
			c.op({ 0x75, 0xE3 });                                               // 70        jnz  up
			c.op({ 0xEB, 0x4E });                                               // 72 out:   jmp  out2
			c.op({ 0x8B, 0x56, 0x20 });                                         // 74 found: mov  edx, [esi+20h]           the weather's name
			c.op({ 0x8B, 0x0D }).at(mem + gr::seen_at);                         // 77        mov  ecx, [seen_at]
			c.op({ 0x83, 0xE1, 0x1F });                                         // 7D        and  ecx, 31
			c.op({ 0x89, 0x14, 0xCD }).at(mem + gr::seen);                      // 80        mov  [seen+ecx*8], edx
			c.op({ 0x89, 0x3C, 0xCD }).at(mem + gr::seen + 4);                  // 87        mov  [seen+4+ecx*8], edi
			c.op({ 0xFF, 0x05 }).at(mem + gr::seen_at);                         // 8E        inc  dword [seen_at]
			c.op({ 0xBE }).at(mem + gr::list);                                  // 94        mov  esi, list
			c.op({ 0x8B, 0x06 });                                               // 99 scan:  mov  eax, [esi]
			c.op({ 0x85, 0xC0 });                                               // 9B        test eax, eax
			c.op({ 0x74, 0x23 });                                               // 9D        jz   out2
			c.op({ 0x39, 0xD0 });                                               // 9F        cmp  eax, edx
			c.op({ 0x75, 0x0B });                                               // A1        jne  next
			c.op({ 0x8B, 0x46, 0x04 });                                         // A3        mov  eax, [esi+4]
			c.op({ 0x85, 0xC0 });                                               // A6        test eax, eax
			c.op({ 0x74, 0x11 });                                               // A8        jz   hit                      (any model)
			c.op({ 0x39, 0xF8 });                                               // AA        cmp  eax, edi
			c.op({ 0x74, 0x0D });                                               // AC        je   hit
			c.op({ 0x83, 0xC6, 0x08 });                                         // AE next:  add  esi, 8
			c.op({ 0x81, 0xFE }).at(mem + gr::list + gr::max * 8);              // B1        cmp  esi, list_end
			c.op({ 0x72, 0xE0 });                                               // B7        jb   scan
			c.op({ 0xEB, 0x07 });                                               // B9        jmp  out2
			c.op({ 0xFF, 0x05 }).at(mem + gr::hidden);                          // BB hit:   inc  dword [hidden]
			c.op({ 0x43 });                                                     // C1        inc  ebx                      skip it
			c.op({ 0x88, 0x1D }).at(mem + gr::result);                          // C2 out2:  mov  [result], bl
			c.op({ 0x61 });                                                     // C8        popad
			c.op({ 0xA0 }).at(mem + gr::result);                                // C9        mov  al, [result]
			c.op({ 0xC3 });                                                     // CE        ret
			return !c.over && c.n == gr::code_size;
		}

		bool gr_memory(std::string &why)
		{
			if (g.mem == 0)
			{
				const uintptr_t mem = alloc_code(gr::size); // (zeroed, executable)
				if (mem == 0)
				{
					why = "couldn't get memory for it";
					return false;
				}
				g.mem = mem;
			}
			return true;
		}

		bool gr_find(std::string &why)
		{
			if (g.site != 0)
				return true;
			const uintptr_t m = find(kSite);
			if (m == 0)
			{
				why = "the game's effect draw wasn't found";
				return false;
			}
			const uintptr_t site = m + kSiteAt;
			if (!code_is(site, kSaved, sizeof(kSaved)))
			{
				why = "the game's effect draw isn't as expected (changed by something else?)";
				return false;
			}
			g.site = site;
			return true;
		}

		// The 6 bytes at the site: a call to the routine and a nop
		void gr_call(uint8_t out[6])
		{
			out[0] = 0xE8;
			put32(out + 1, uint32_t(g.mem + gr::code - (g.site + 5)));
			out[5] = 0x90;
		}

		// The routine in (while the game's rain is hidden) or out
		bool route_rain(bool on, std::string &why)
		{
			if (on == g.routed)
				return true;
			if (!gr_find(why))
				return false;
			uint8_t call[6];
			if (on)
			{
				if (!gr_memory(why))
					return false;
				code_bytes c;
				if (!gr_code(uint32_t(g.mem), c))
				{
					why = "its routine came out the wrong size";
					return false;
				}
				std::memcpy(reinterpret_cast<void *>(g.mem + gr::code), c.bytes, gr::code_size);
				gr_call(call);
				if (!write_code(g.site, call, sizeof(call)))
				{
					why = "couldn't write the game's code";
					return false;
				}
			}
			else
			{
				gr_call(call);
				if (!code_is(g.site, call, sizeof(call)))
				{
					why = "something else has changed the game's code since: left as it is";
					return false;
				}
				if (!write_code(g.site, kSaved, sizeof(kSaved)))
				{
					why = "couldn't write the game's code";
					return false;
				}
			}
			g.routed = on;
			return true;
		}

		// Whether World FX has rain for the zone (its worldfx-rain\<zone>.bin), looked at once a zone (and again if its
		// folder changes)
		bool has_rain(const std::string &folder, uint32_t zone)
		{
			if (g.zone != zone || g.folder != folder)
			{
				g.zone = zone;
				g.folder = folder;
				g.has_rain = has_zone_file("worldfx-rain", zone);
			}
			return g.has_rain;
		}

		// World FX's rain: its Rain setting on and its RainAmount (when set) over 0
		bool rain_on()
		{
			return setting_on("Rain", true) && setting_number("RainAmount", 1.0f) > 0.0f;
		}

		// World FX's snow: its Snow setting on and its SnowAmount (when set) over 0
		bool snow_on()
		{
			return setting_on("Snow", true) && setting_number("SnowAmount", 1.0f) > 0.0f;
		}

		// Elements skipped a frame, measured since the last update (kept as it was when no frame went by)
		void measure()
		{
			if (g.mem == 0)
				return;
			const uint32_t hidden = *reinterpret_cast<const volatile uint32_t *>(g.mem + gr::hidden);
			const uint64_t frames = g.frames - g.last_frames;
			if (frames == 0)
				return;
			g.rate = double(uint32_t(hidden - g.last_hidden)) / double(frames);
			g.last_hidden = hidden;
			g.last_frames = g.frames;
		}
	}

	// The setting: the game's rain hidden (while World FX draws its own) or drawn
	bool route(bool hide, std::string &why)
	{
		g.off = hide;
		std::string reason;
		const bool ok = route_rain(hide, reason);
		g.route_why = ok ? std::string() : reason;
		why = reason;
		return ok;
	}

	// Fills the routine's list (while World FX draws its rain or snow here), or empties it and says why
	void update(uint32_t zone)
	{
		const std::string folder = data_folder();
		std::string why;
		bool rain = false, snow = false;
		if (!g.off)
			why = std::string("drawn (") + kSettingOff + ")";
		else if (zone == 0)
			why = "drawn: no zone yet";
		else if (folder.empty())
			why = "drawn: World FX's folder isn't known";
		else if (!has_rain(folder, zone))
			why = "drawn: World FX has no rain or snow for zone " + std::to_string(zone);
		else
		{
			rain = rain_on();
			snow = snow_on();
			if (!rain && !snow)
				why = "drawn: World FX's Rain and Snow settings are off (or their amounts 0)";
			else if (!g.routed)
				why = "drawn: " + (!g.route_why.empty() ? g.route_why : std::string("its draw isn't routed"));
		}
		g.why = why;
		g.hiding = rain && snow ? "rain and snow" : rain ? "rain (World FX's Snow setting is off: snow drawn)"
			: "snow (World FX's Rain setting is off: rain drawn)";
		std::vector<std::string> pairs; // (8 bytes each: the weather's name, the model's or 0)
		if (why.empty())
		{
			for (const hidden_entry &e : kRainHidden)
			{
				if (((e.snow && snow) || (!e.snow && rain)) && pairs.size() < gr::max)
					pairs.push_back(std::string(e.weather, 4) + (e.model != nullptr ? std::string(e.model, 4) : std::string(4, '\0')));
			}
		}
		std::string list;
		for (const std::string &p : pairs)
			list += p;
		if (g.mem != 0 && list != g.list)
		{
			volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(g.mem + gr::list);
			for (uint32_t i = 0; i < gr::max * 2; ++i) // (ends the list first: the routine never sees half of one)
				p[i] = 0;
			for (size_t i = pairs.size(); i >= 1; --i)
			{
				p[(i - 1) * 2 + 1] = name_value(pairs[i - 1].data() + 4);
				p[(i - 1) * 2] = name_value(pairs[i - 1].data());
			}
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
		if (!route_rain(false, why))
			return false; // (something else may still call into it: its memory stays)
		free_code(g.mem);
		g = state();
		return true;
	}

	std::string status()
	{
		if (!g.why.empty() || g.list.empty())
			return !g.why.empty() ? g.why : std::string("drawn");
		char rate[32];
		std::snprintf(rate, sizeof(rate), "%.1f", g.rate);
		return "hidden while World FX draws its own here: " + g.hiding + " (" + rate + " of its effects skipped a frame)";
	}

	// The weather effects that came by the routine lately (weather/model), hidden ones marked
	std::string seen()
	{
		if (!g.routed)
			return std::string("nothing seen: the game's rain and snow aren't being sorted (") + kSettingOff + ")";
		std::set<std::string> hidden;
		for (size_t i = 0; i + 8 <= g.list.size(); i += 8)
			hidden.insert(g.list.substr(i, 8));
		const volatile uint32_t *p = reinterpret_cast<const volatile uint32_t *>(g.mem + gr::seen);
		std::set<std::string> known;
		std::vector<std::string> out;
		for (uint32_t i = 0; i < gr::seen_max; ++i)
		{
			const uint32_t w = p[i * 2], m = p[i * 2 + 1];
			if (w == 0)
				continue;
			const std::string weather(reinterpret_cast<const char *>(&w), 4), model(reinterpret_cast<const char *>(&m), 4);
			if (!known.insert(weather + model).second)
				continue;
			// (hidden: its weather on the list for any model, or with this one)
			const bool skip = hidden.count(weather + std::string(4, '\0')) != 0 || hidden.count(weather + model) != 0;
			out.push_back(name_text(w) + "/" + (m != 0 ? name_text(m) : std::string("-")) + (skip ? " (hidden)" : ""));
		}
		std::sort(out.begin(), out.end());
		std::string s;
		for (const std::string &line : out)
			s += (s.empty() ? "" : ", ") + line;
		return !s.empty() ? s : std::string("none yet");
	}

	sites where()
	{
		return { g.mem, g.site };
	}
}
