/*
 * The game's weather (gamerain.hpp): began as the worldfx Ashita addon's /worldfx gamerain (worldfx.lua, gr_code and
 * route_rain to gamerain_seen), in C++; now it also leaves out all of the game's weather while World FX draws all of
 * its own.
 *
 * The game's weather visuals are effects its weather starts (client-notes "Hiding one weather's visuals"). Every effect
 * element is drawn through 046070, which skips one whose +0x17A is set; the 6 bytes that read it (04609D, mov al,
 * [ebp+17Ah]) become a call to a routine of ours that also answers "skip" for an element whose generator ([[+0xFC]], a
 * type-5 resource) is a weather's (+0xDE bit 7, as the game marks them at load) and either:
 *   - all mode (the routine's all word not 0): any of them, sky layers too, whatever its weather or model;
 *   - the list mode: it isn't a sky layer (element flags 0x400000: clouds stay) and its weather (the name of the folder
 *     it's in, up its +0x28 parents, through clones) and model (its model command's name, [+0xE4] + 0xC) are on the
 *     routine's list (model 0: any).
 * Skipped elements run on (their timers, fades and sound), only their draw is left out, as the game's own +0x17A does.
 * The weather's records still light, fog and draw the zone. The routine also keeps the last 32 weather elements it saw
 * whose folder it found (seen()), in either mode.
 *
 * World FX says what it draws of its own (worldfx_weather_drawn, read at every update): all of the weather
 * (kWeatherFxAll: all mode, since its own sky and clouds would otherwise sit under the game's and the game's weather
 * effects spoil its shadow maps), else its rain (the list's rain entries) and its snow (the snow's). Hidden while
 * HiFi XI leaves the game's weather out (its HideGameRain setting, with World FX drawing); put back on unload.
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
		// The routine's memory (the addon's GR, with the all word in what was spare)
		namespace gr
		{
			constexpr uint32_t hidden = 0x00;  // u32: element draws skipped
			constexpr uint32_t seen_at = 0x04; // u32: where the next element seen goes (mod 32)
			constexpr uint32_t result = 0x08;  // u8: the routine's answer on its way out
			constexpr uint32_t all = 0x0C;     // u32: not 0: every weather element skipped (all mode), the list not looked at for it
			constexpr uint32_t list = 0x10;    // 16 pairs of u32: weather name, model name (0: any but the sky layers); a 0 name ends it
			constexpr uint32_t seen = 0x90;    // 32 pairs of u32: the weather and model of the last weather elements that came by
			constexpr uint32_t code = 0x190;   // the routine called in place of "mov al, [ebp+17Ah]" (224 bytes)
			constexpr uint32_t size = 0x270;
			constexpr uint32_t max = 16;
			constexpr uint32_t seen_max = 32;
			constexpr uint32_t code_size = 0xE0;
		}

		constexpr const char *kSite = "8D8DF8000000D91C2450E8????????8B00A3????????8A857A01000084C00F85"; // 046087, the mov at +0x16
		constexpr intptr_t kSiteAt = 0x16;
		constexpr uint8_t kSaved[6] = { 0x8A, 0x85, 0x7A, 0x01, 0x00, 0x00 };

		// The list mode's entries (while World FX draws its own rain or snow but not all of the weather): the weather
		// (its folder's name; the game's second set as the first) and the model (nullptr: everything but the sky).
		// Thunderstorms (bolt) keep their lightning: only their rain models go. Thunder (thdr) has no rain of its own in
		// the game. Snow and blizzards (snow: while World FX snows) lose everything but their sky layers (their hit3
		// flakes, the blizzard's smok). At most gr::max entries.
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
			bool all = false;                    // the routine's all word set
			std::string why = "not hidden yet";  // (empty: hidden)
			std::string hiding;
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

		// The routine (called with ebp = the element; everything but al is kept); false if it isn't 224 bytes. bl is the
		// answer all the way down: all mode sets it as soon as the element is known to be a weather's, so every way out
		// from there (no folder found, a model on no list) skips it; the one count of skipped draws is at the end, so an
		// element both in all mode and on the list is counted once.
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
			c.op({ 0x74, 0x66 });                                               // 16        jz   out
			c.op({ 0x8B, 0x36 });                                               // 18        mov  esi, [esi]               the generator
			c.op({ 0x85, 0xF6 });                                               // 1A        test esi, esi
			c.op({ 0x74, 0x60 });                                               // 1C        jz   out
			c.op({ 0x8A, 0x46, 0x24 });                                         // 1E        mov  al, [esi+24h]            a generator (type 5)
			c.op({ 0x24, 0x7F });                                               // 21        and  al, 7Fh
			c.op({ 0x3C, 0x05 });                                               // 23        cmp  al, 5
			c.op({ 0x75, 0x57 });                                               // 25        jne  out
			c.op({ 0xF6, 0x86, 0xDE, 0x00, 0x00, 0x00, 0x80 });                 // 27        test byte [esi+0DEh], 80h     a weather's
			c.op({ 0x74, 0x4E });                                               // 2E        jz   out
			c.op({ 0x83, 0x3D }).at(mem + gr::all).op({ 0x00 });                // 30        cmp  dword [all], 0           all mode:
			c.op({ 0x0F, 0x95, 0xC3 });                                         // 37        setne bl                      skip it, whatever it is
			c.op({ 0x75, 0x0C });                                               // 3A        jne  model                    (sky layers too)
			c.op({ 0xF7, 0x85, 0x0C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00 }); // 3C      test dword [ebp+10Ch], 400000h  a sky layer: drawn
			c.op({ 0x75, 0x36 });                                               // 46        jnz  out
			c.op({ 0x31, 0xFF });                                               // 48 model: xor  edi, edi                 its model's name
			c.op({ 0x8B, 0x86, 0xE4, 0x00, 0x00, 0x00 });                       // 4A        mov  eax, [esi+0E4h]          (its model command)
			c.op({ 0x85, 0xC0 });                                               // 50        test eax, eax
			c.op({ 0x74, 0x08 });                                               // 52        jz   nomodel
			c.op({ 0x80, 0x38, 0x01 });                                         // 54        cmp  byte [eax], 1
			c.op({ 0x75, 0x03 });                                               // 57        jne  nomodel
			c.op({ 0x8B, 0x78, 0x0C });                                         // 59        mov  edi, [eax+0Ch]
			c.op({ 0xB9, 0x04, 0x00, 0x00, 0x00 });                             // 5C nomodel: mov ecx, 4                 up to 4 parents
			c.op({ 0x8B, 0x46, 0x28 });                                         // 61 up:    mov  eax, [esi+28h]           the parent's handle
			c.op({ 0x85, 0xC0 });                                               // 64        test eax, eax
			c.op({ 0x74, 0x16 });                                               // 66        jz   out
			c.op({ 0x8B, 0x30 });                                               // 68        mov  esi, [eax]
			c.op({ 0x85, 0xF6 });                                               // 6A        test esi, esi
			c.op({ 0x74, 0x10 });                                               // 6C        jz   out
			c.op({ 0x8A, 0x46, 0x24 });                                         // 6E        mov  al, [esi+24h]
			c.op({ 0x24, 0x7F });                                               // 71        and  al, 7Fh
			c.op({ 0x3C, 0x01 });                                               // 73        cmp  al, 1                    a folder: the weather's
			c.op({ 0x74, 0x09 });                                               // 75        je   found
			c.op({ 0x3C, 0x05 });                                               // 77        cmp  al, 5                    a clone's source
			c.op({ 0x75, 0x03 });                                               // 79        jne  out
			c.op({ 0x49 });                                                     // 7B        dec  ecx
			c.op({ 0x75, 0xE3 });                                               // 7C        jnz  up
			c.op({ 0xEB, 0x49 });                                               // 7E out:   jmp  out2
			c.op({ 0x8B, 0x56, 0x20 });                                         // 80 found: mov  edx, [esi+20h]           the weather's name
			c.op({ 0x8B, 0x0D }).at(mem + gr::seen_at);                         // 83        mov  ecx, [seen_at]
			c.op({ 0x83, 0xE1, 0x1F });                                         // 89        and  ecx, 31
			c.op({ 0x89, 0x14, 0xCD }).at(mem + gr::seen);                      // 8C        mov  [seen+ecx*8], edx
			c.op({ 0x89, 0x3C, 0xCD }).at(mem + gr::seen + 4);                  // 93        mov  [seen+4+ecx*8], edi
			c.op({ 0xFF, 0x05 }).at(mem + gr::seen_at);                         // 9A        inc  dword [seen_at]
			c.op({ 0xBE }).at(mem + gr::list);                                  // A0        mov  esi, list                (empty in all mode)
			c.op({ 0x8B, 0x06 });                                               // A5 scan:  mov  eax, [esi]
			c.op({ 0x85, 0xC0 });                                               // A7        test eax, eax
			c.op({ 0x74, 0x1E });                                               // A9        jz   out2
			c.op({ 0x39, 0xD0 });                                               // AB        cmp  eax, edx
			c.op({ 0x75, 0x0B });                                               // AD        jne  next
			c.op({ 0x8B, 0x46, 0x04 });                                         // AF        mov  eax, [esi+4]
			c.op({ 0x85, 0xC0 });                                               // B2        test eax, eax
			c.op({ 0x74, 0x11 });                                               // B4        jz   hit                      (any model)
			c.op({ 0x39, 0xF8 });                                               // B6        cmp  eax, edi
			c.op({ 0x74, 0x0D });                                               // B8        je   hit
			c.op({ 0x83, 0xC6, 0x08 });                                         // BA next:  add  esi, 8
			c.op({ 0x81, 0xFE }).at(mem + gr::list + gr::max * 8);              // BD        cmp  esi, list_end
			c.op({ 0x72, 0xE0 });                                               // C3        jb   scan
			c.op({ 0xEB, 0x02 });                                               // C5        jmp  out2
			c.op({ 0xB3, 0x01 });                                               // C7 hit:   mov  bl, 1                    skip it
			c.op({ 0x84, 0xDB });                                               // C9 out2:  test bl, bl
			c.op({ 0x74, 0x06 });                                               // CB        jz   done
			c.op({ 0xFF, 0x05 }).at(mem + gr::hidden);                          // CD        inc  dword [hidden]
			c.op({ 0x88, 0x1D }).at(mem + gr::result);                          // D3 done:  mov  [result], bl
			c.op({ 0x61 });                                                     // D9        popad
			c.op({ 0xA0 }).at(mem + gr::result);                                // DA        mov  al, [result]
			c.op({ 0xC3 });                                                     // DF        ret
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

		// The routine in (while the game's weather is left out) or out
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

	// The setting: the game's weather left out (while World FX draws its own) or drawn
	bool route(bool hide, std::string &why)
	{
		g.off = hide;
		std::string reason;
		const bool ok = route_rain(hide, reason);
		g.route_why = ok ? std::string() : reason;
		why = reason;
		return ok;
	}

	// Sets the routine's all word (while World FX draws all of the weather here) or fills its list (while it draws its
	// rain or snow), as World FX says now; or clears both and says why
	void update(uint32_t zone)
	{
		// (asked every time: World FX knows its zone, its weather and its settings, and they change under us)
		const uint32_t drawn = worldfx_weather_drawn();
		std::string why;
		bool all = false, rain = false, snow = false;
		if (!g.off)
			why = std::string("drawn (") + kSettingOff + ")";
		else if (zone == 0)
			why = "drawn: no zone yet";
		else
		{
			all = (drawn & kWeatherFxAll) != 0;
			rain = !all && (drawn & kWeatherFxRain) != 0;
			snow = !all && (drawn & kWeatherFxSnow) != 0;
			if (!all && !rain && !snow)
				why = "drawn: World FX draws none of the weather here";
			else if (!g.routed)
				why = "drawn: " + (!g.route_why.empty() ? g.route_why : std::string("its draw isn't routed"));
		}
		if (!why.empty())
			all = rain = snow = false;
		g.why = why;
		g.hiding = rain && snow ? "rain and snow" : rain ? "rain (World FX's Snow setting is off: snow drawn)"
			: "snow (World FX's Rain setting is off: rain drawn)";
		std::vector<std::string> pairs; // (8 bytes each: the weather's name, the model's or 0; none in all mode)
		for (const hidden_entry &e : kRainHidden)
		{
			if (((e.snow && snow) || (!e.snow && rain)) && pairs.size() < gr::max)
				pairs.push_back(std::string(e.weather, 4) + (e.model != nullptr ? std::string(e.model, 4) : std::string(4, '\0')));
		}
		std::string list;
		for (const std::string &p : pairs)
			list += p;
		if (g.mem != 0)
		{
			// (all mode on before the list is emptied and off after it's filled: no draw between the two sees neither)
			volatile uint32_t *word = reinterpret_cast<volatile uint32_t *>(g.mem + gr::all);
			if (all)
				*word = 1;
			if (list != g.list)
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
			if (!all)
				*word = 0;
		}
		g.all = all;
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
		if (!g.why.empty() || (!g.all && g.list.empty()))
			return !g.why.empty() ? g.why : std::string("drawn");
		char rate[32];
		std::snprintf(rate, sizeof(rate), "%.1f", g.rate);
		const std::string skipped = std::string(" (") + rate + " of its effects skipped a frame)";
		if (g.all)
			return "all of the game's weather left out while World FX draws its own here" + skipped;
		return "hidden while World FX draws its own here: " + g.hiding + skipped;
	}

	// The weather effects that came by the routine lately (weather/model), hidden ones marked (all of them in all mode)
	std::string seen()
	{
		if (!g.routed)
			return std::string("nothing seen: the game's weather isn't being sorted (") + kSettingOff + ")";
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
			// (hidden: all mode, or its weather on the list for any model, or with this one)
			const bool skip = g.all || hidden.count(weather + std::string(4, '\0')) != 0 || hidden.count(weather + model) != 0;
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
