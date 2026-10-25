/*
 * HiFi XI's game-side feed (src/game/feed: the worldfx Ashita addon's work in C++) against both FFXiMain dumps, each
 * mapped at its own base, with fake entities and World FX's functions stubbed to record what they're sent:
 *   - the characters near: the nearest shown ones within 50 yalms of you, the game's x, height, z and a radius, the
 *     fighting ones marked; you found by the party's target index, else by your server id;
 *   - a weapon skill's action packet (built bit by bit): an area skill's circle round the first target, a swath from
 *     the user past each target near, a circle where a far one stands;
 *   - the zone told through the environment, a Mog House as 1000 + its room's model (from the zone-in packet);
 *   - lamps written into the game's light table: the lit point lights sent every 3rd frame, as they are;
 *   - the weather: an environment, its weather and record and the renderer's fog faked behind the game's globals;
 *   - the moon: the game's Vana'diel clock (its clock object's seconds + the server's offset) read through the globals
 *     its clock function uses, sent each frame once the clock object is set, nothing before;
 *   - the lens flare's je made a jmp and put back, the visibility tests' calls pointed at World FX's entries and back,
 *     each only over the game's own bytes;
 *   - the game's water and rain routed to their routines and back as their settings say;
 *   - after stop, the image is the dump again, byte for byte.
 *
 *   build\feed_test.exe     (from this folder)
 */

#include "../src/game/feed.hpp"
#include "../src/game/gamerain.hpp"
#include "../src/game/gamewater.hpp"
#include "game_dump.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
	int failures = 0;
	void check(bool ok, const char *what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
		if (!ok)
			failures++;
	}

	// What World FX was sent, and its settings
	struct sent
	{
		std::vector<float> actors;
		std::vector<uint32_t> fighting;
		uint32_t actor_calls = 0;
		std::vector<float> cuts;
		std::vector<uint32_t> lamps;
		uint32_t lamp_calls = 0;
		std::vector<uint32_t> weather;
		uint32_t weather_calls = 0;
		uint32_t moon = 0, moon_calls = 0;
		uint32_t glow_calls = 0;
		uintptr_t vis_original = 0;
		uint32_t vis_on = 0;
	};
	sent g_sent;
	std::map<std::string, std::string> g_settings;
	std::string g_folder;
	uint32_t g_casting = 0; // (World FX casting characters' shadows, as worldfx_casts_characters says)
	uint32_t g_drawing = 1; // (World FX drawing into the game's world, as worldfx_drawing says)
}

extern "C"
{
	void __cdecl worldfx_grass_actors2(const float *xyzr, const uint32_t *flags, uint32_t count)
	{
		g_sent.actors.assign(xyzr, xyzr + count * 4);
		g_sent.fighting.assign(flags, flags + count);
		g_sent.actor_calls++;
	}
	void __cdecl worldfx_grass_cut(const float *swaths, uint32_t count) { g_sent.cuts.assign(swaths, swaths + count * 7); }
	void __cdecl worldfx_lamps(const uint32_t *words, uint32_t count)
	{
		g_sent.lamps.assign(words, words + count * 10);
		g_sent.lamp_calls++;
	}
	void __cdecl worldfx_glows(const uint32_t *, uint32_t) { g_sent.glow_calls++; }
	void __cdecl worldfx_weather(const uint32_t *words, uint32_t count)
	{
		g_sent.weather.assign(words, words + count);
		g_sent.weather_calls++;
	}
	void __cdecl worldfx_moon(uint32_t clock)
	{
		g_sent.moon = clock;
		g_sent.moon_calls++;
	}
	// (each kind's entry: a stand-in address)
	uintptr_t __cdecl worldfx_vis_entry(uint32_t kind) { return 0x31000000u + kind * 0x10; }
	void __cdecl worldfx_vis_setup(uintptr_t original, uint32_t on)
	{
		if (original != 0)
			g_sent.vis_original = original;
		g_sent.vis_on = on;
	}
	void __cdecl worldfx_vis_stats(uint32_t out[12]) { std::memset(out, 0, 12 * 4); }
	int __cdecl worldfx_setting(const char *key, char *out, uint32_t size)
	{
		const auto it = g_settings.find(key);
		if (it == g_settings.end())
			return 0;
		std::snprintf(out, size, "%s", it->second.c_str());
		return 1;
	}
	const char *__cdecl worldfx_folder() { return g_folder.c_str(); }
	uint32_t __cdecl worldfx_casts_characters() { return g_casting; }
	uint32_t __cdecl worldfx_drawing() { return g_drawing; }
	// (all of the weather World FX's own: the routing is what's checked here, gamerain_test checks what follows from it)
	uint32_t __cdecl worldfx_weather_drawn() { return hifi::game::kWeatherFxAll; }
	int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
	void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
}

namespace
{
	// Entities: positions Ashita's (x east, y north, z height down)
	struct fake_entity
	{
		std::string name;
		float x = 0, y = 0, z = 0, size = 1.0f;
		bool shown = false;
		uint32_t id = 0, status = 0;
	};
	struct fakes final : hifi::game::entities
	{
		std::vector<fake_entity> list = std::vector<fake_entity>(kCount);
		const char *name(uint32_t i) const override { return list[i].name.c_str(); }
		bool position(uint32_t i, float &x, float &y, float &z) const override
		{
			x = list[i].x, y = list[i].y, z = list[i].z;
			return true;
		}
		bool shown(uint32_t i) const override { return list[i].shown; }
		uint32_t server_id(uint32_t i) const override { return list[i].id; }
		uint32_t status(uint32_t i) const override { return list[i].status; }
		float model_size(uint32_t i) const override { return list[i].size; }
	};

	// Bits into a packet, low bit first
	void put_bits(std::vector<uint8_t> &p, uint32_t pos, uint32_t n, uint32_t v)
	{
		for (uint32_t k = 0; k < n; ++k)
		{
			const uint32_t at = pos + k;
			if (at / 8 >= p.size())
				p.resize(at / 8 + 1);
			if ((v >> k) & 1u)
				p[at / 8] = uint8_t(p[at / 8] | (1u << (at % 8)));
		}
	}

	std::string env(const char *name)
	{
		char text[64] = "";
		const DWORD n = GetEnvironmentVariableA(name, text, sizeof(text));
		return n > 0 && n < sizeof(text) ? std::string(text) : std::string("(none)");
	}

	uint32_t bits_of(float f)
	{
		uint32_t u;
		std::memcpy(&u, &f, 4);
		return u;
	}

	void check(bool ok, const std::string &what)
	{
		check(ok, what.c_str());
	}

	// Where a dump has the worldfx addon's patches in: the bytes dumped and the game's own. The flare's je made a jmp
	// (054AF1: E9 rel+1, nop), the water draw's call (040DC4: to 183760) and a step's effect call (0D5365: push eax;
	// mov ecx, esi; call 0D63F0) sent to its routines, the deep-water ring's je (0D64FA) made a jmp.
	struct addon_site
	{
		uintptr_t at;
		std::string dumped, own;
	};
	std::vector<addon_site> addon_sites(uintptr_t base)
	{
		auto bytes = [](uintptr_t at, size_t n) { return std::string(reinterpret_cast<const char *>(at), n); };
		auto word = [](uint32_t v) { return std::string(reinterpret_cast<const char *>(&v), 4); };
		std::vector<addon_site> out;
		auto add = [&](uintptr_t at, const std::string &own) {
			const std::string dumped = bytes(at, own.size());
			if (dumped != own)
				out.push_back({ at, dumped, own });
		};
		const uintptr_t flare = base + 0x054AF1;
		const std::string f = bytes(flare, 6);
		if (uint8_t(f[0]) == 0xE9 && uint8_t(f[5]) == 0x90)
		{
			uint32_t rel = 0;
			std::memcpy(&rel, f.data() + 1, 4);
			add(flare, std::string("\x0F\x84", 2) + word(rel - 1));
		}
		add(base + 0x040DC4, "\xE8" + word(uint32_t(base + 0x183760 - (base + 0x040DC4 + 5))));
		add(base + 0x0D5365, "\x50\x8B\xCE\xE8" + word(uint32_t(base + 0x0D63F0 - (base + 0x0D5365 + 8))));
		add(base + 0x0D64FA, "\x74");
		return out;
	}

	void run(const char *path)
	{
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		std::printf("\n== %s\n", path);
		check(size != 0, "the dump mapped at its base");
		if (size == 0)
			return;
		g_sent = sent();
		g_casting = 0;
		g_drawing = 1;
		g_settings = { { "HideFlare", "1" }, { "HideGameWater", "0" }, { "HideGameRain", "0" }, { "VisTest", "0" } };
		char temp[MAX_PATH] = "";
		GetTempPathA(MAX_PATH, temp);
		g_folder = std::string(temp) + "hifixi_feed_test\\";
		CreateDirectoryA(g_folder.c_str(), nullptr);
		// (World FX's water and rain for zones 100 and 101: only their being there is looked at)
		for (const char *kind : { "worldfx-water", "worldfx-rain" })
		{
			CreateDirectoryA((g_folder + kind).c_str(), nullptr);
			for (const char *zone : { "100", "101" })
				if (FILE *f = nullptr; fopen_s(&f, (g_folder + kind + "\\" + zone + ".bin").c_str(), "wb") == 0 && f != nullptr)
				{
					std::fputs("TEST", f);
					std::fclose(f);
				}
		}

		// You (entity 5) and others round you
		fakes e;
		auto put = [&](uint32_t i, float x, float y, float z, bool shown, uint32_t id, uint32_t status = 0, float size_ = 1.0f) {
			e.list[i] = { "", x, y, z, size_, shown, id, status };
		};
		put(5, 100.0f, 200.0f, -10.0f, true, 0x1000005);
		put(6, 103.0f, 200.0f, -10.0f, true, 0x1000006, 1, 2.0f);     // 3 yalms, fighting, a big one
		put(7, 110.0f, 200.0f, -10.0f, true, 0x1000007, 0, 0.5f);     // 10, small: the radius's floor
		put(8, 101.0f, 200.0f, -10.0f, false, 0x1000008);              // not shown
		put(9, 160.0f, 200.0f, -10.0f, true, 0x1000009);               // 60: out of reach
		put(10, 100.0f, 205.0f, -11.0f, true, 0x100000A, 0, 50.0f);    // 5, huge: the radius's ceiling
		hifi::game::feed::party p;
		p.target_index = 5;
		p.server_id = 0x1000005;
		p.zone = 100;
		p.in_game = true;

		// A dump taken with the worldfx addon's patches in (FFXiMain_041C0000.bin: its flare and water): left alone,
		// nothing written; then the game's own bytes put in the mapped copy for the rest, the dumped ones back at the end
		const std::vector<addon_site> addon = addon_sites(base);
		if (!addon.empty())
		{
			std::printf("(dumped with the worldfx addon's patches in, at");
			for (const addon_site &s : addon)
				std::printf(" %06X", unsigned(s.at - base));
			std::printf(")\n");
			// (the rain's site isn't the addon's in it: left out here)
			g_settings = { { "HideFlare", "1" }, { "HideGameWater", "1" }, { "HideGameRain", "0" }, { "VisTest", "0" } };
			hifi::game::feed::frame(e, p, 0.5);
			const std::vector<std::string> s = hifi::game::feed::status();
			auto line = [&](const char *start) {
				for (const std::string &l : s)
					if (l.rfind(start, 0) == 0)
						return l;
				return std::string();
			};
			check(dump::same_as_file(path, base, size), "the addon's patches left alone: nothing written");
			check(line("lens flare: ").find("shown (the lens flare's code wasn't found)") != std::string::npos, "  " + line("lens flare: "));
			check(line("the game's water: ").find("isn't as expected") != std::string::npos, "  " + line("the game's water: "));
			hifi::game::feed::stop();
			check(dump::same_as_file(path, base, size), "  stopped: still nothing written");
			for (const addon_site &a : addon)
				std::memcpy(reinterpret_cast<void *>(a.at), a.own.data(), a.own.size());
			g_settings = { { "HideFlare", "1" }, { "HideGameWater", "0" }, { "HideGameRain", "0" }, { "VisTest", "0" } };
		}
		// (the image with the game's own bytes; the test's own writes into it, the game's data faked, put back from this)
		const std::vector<uint8_t> clean(reinterpret_cast<const uint8_t *>(base), reinterpret_cast<const uint8_t *>(base) + size);
		auto inside = [&](uintptr_t at, size_t n) { return at >= base && at + n <= base + size; };
		auto put_back = [&](uintptr_t at, size_t n) {
			if (inside(at, n))
				std::memcpy(reinterpret_cast<void *>(at), &clean[at - base], n);
		};

		// (the lens flare's je, before the first frame hides it: its pattern holds the je)
		const uintptr_t flare = hifi::game::find("8BF88B860401000085C00F84????????85FF0F8E");

		double now = 1.0;
		hifi::game::feed::frame(e, p, now);

		// 1. The characters near
		{
			const std::vector<float> &a = g_sent.actors;
			const bool ok = a.size() == 4 * 4 && a[0] == 100.0f && a[1] == -10.0f && a[2] == 200.0f && a[3] == 1.0f &&
				a[4] == 103.0f && a[7] == 2.0f && a[8] == 100.0f && a[9] == -11.0f && a[10] == 205.0f && a[11] == 5.0f &&
				a[12] == 110.0f && a[15] == 0.9f;
			check(ok, "the characters near sent: you first, then by distance, the game's x, height, z, the radius kept to 0.9..5");
			check(g_sent.fighting.size() == 4 && g_sent.fighting[1] == 1 && g_sent.fighting[0] == 0, "the one fighting marked");
		}
		p.target_index = 0;
		now += 1.0;
		hifi::game::feed::frame(e, p, now);
		check(g_sent.actors.size() == 16 && g_sent.actors[0] == 100.0f, "you found by your server id when the party has no target index");
		p.server_id = 0;
		now += 1.0;
		hifi::game::feed::frame(e, p, now);
		check(g_sent.actors.empty(), "nobody sent when you can't be found");
		p.target_index = 5;
		p.server_id = 0x1000005;

		// 2. The zone and in game, through the environment
		check(env("FFXI_WORLDFX_ZONE") == "100" && env("FFXI_WORLDFX_INGAME") == "1", "the zone (100) and in game told to World FX");
		{
			std::vector<uint8_t> zone_in(0x100, 0);
			zone_in[0x80] = 1;
			zone_in[0xAA] = 0x05;
			zone_in[0xAB] = 0x01; // (room 0x0105)
			hifi::game::feed::packet_in(0x00A, zone_in.data(), uint32_t(zone_in.size()), e);
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			check(env("FFXI_WORLDFX_ZONE") == "1261" && hifi::game::feed::zone() == 1261, "in a Mog House: 1000 + its room's model (1261)");
			zone_in[0x80] = 0;
			hifi::game::feed::packet_in(0x00A, zone_in.data(), uint32_t(zone_in.size()), e);
			p.zone = 0;
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			check(env("FFXI_WORLDFX_ZONE") == "1261", "zoning (zone 0): the last zone stays");
			p.zone = 101;
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			check(env("FFXI_WORLDFX_ZONE") == "101", "out again: the party's zone (101)");
		}

		// 3. A weapon skill's action packet: user 6, skill 20 (an area skill, radius 10), targets 5 (3 yalms off) and 9 (57)
		{
			std::vector<uint8_t> pk(64, 0);
			put_bits(pk, 40, 32, 0x1000006);
			put_bits(pk, 72, 6, 2);
			put_bits(pk, 82, 4, 3);
			put_bits(pk, 86, 32, 20);
			uint32_t pos = 150;
			for (uint32_t id : { 0x1000005u, 0x1000009u })
			{
				put_bits(pk, pos, 32, id);
				put_bits(pk, pos + 32, 4, 1);
				pos += 36 + 85 + 1 + 1;
			}
			pk.resize((pos + 7) / 8 + 2);
			hifi::game::feed::packet_in(0x028, pk.data(), uint32_t(pk.size()), e);
			const std::vector<float> &c = g_sent.cuts;
			// (the game's x, height, z: Ashita's x, z, y)
			const bool area = c.size() == 3 * 7 && c[0] == 100.0f && c[1] == -10.0f && c[2] == 200.0f && c[3] == 10.0f;
			const bool swath = c.size() == 21 && c[7] == 103.0f && c[8] == -10.0f && c[10] == 1.0f && std::fabs(c[11] - 99.0f) < 1e-4f && c[13] == 200.0f;
			const bool far_one = c.size() == 21 && c[14] == 160.0f && c[17] == 1.0f && c[18] == 160.0f;
			check(area && swath && far_one, "a weapon skill: its area round the first target, a swath a yalm past the near one, a circle at the far one");
			std::vector<uint8_t> spell = pk;
			spell[10] = 0;
			put_bits(spell, 82, 4, 4);
			g_sent.cuts.clear();
			hifi::game::feed::packet_in(0x028, spell.data(), uint32_t(spell.size()), e);
			check(g_sent.cuts.empty(), "any other action cuts nothing");
		}

		// 4. Lamps: written into the game's light table (found by its code)
		{
			const uintptr_t code = hifi::game::find("8B542404B8????????33C93910740E");
			const uintptr_t table = code != 0 ? *reinterpret_cast<const uint32_t *>(code + 5) : 0;
			check(code != 0 && inside(table, 256 * 0x70), "the game's light table found in the image");
			if (inside(table, 256 * 0x70))
			{
				auto *t = reinterpret_cast<uint32_t *>(table);
				std::memset(t, 0, 256 * 0x70);
				auto light = [&](uint32_t i, const char *id, uint32_t type, float bright) {
					uint32_t *l = &t[i * 28];
					std::memcpy(&l[0], id, 4);
					l[2] = type;
					l[3] = bits_of(1.0f), l[4] = bits_of(0.8f), l[5] = bits_of(0.6f);
					l[15] = bits_of(10.0f), l[16] = bits_of(-5.0f), l[17] = bits_of(20.0f);
					l[21] = bits_of(8.0f);
					l[26] = bits_of(bright);
				};
				light(0, "lmp1", 1, 4.0f);
				light(1, "lmp2", 1, 0.0f);   // off
				light(2, "spt1", 2, 4.0f);   // a spot light
				light(7, "lmp3", 1, 12.0f);
				g_sent.lamp_calls = 0;
				for (int f = 0; f < 6; ++f)
				{
					now += 0.02;
					hifi::game::feed::frame(e, p, now);
				}
				const std::vector<uint32_t> &l = g_sent.lamps;
				check(g_sent.lamp_calls == 2, "lamps sent every 3rd frame");
				check(l.size() == 20 && std::memcmp(&l[0], "lmp1", 4) == 0 && l[1] == 1 && l[2] == bits_of(1.0f) && l[5] == bits_of(10.0f) &&
					l[7] == bits_of(20.0f) && l[8] == bits_of(8.0f) && l[9] == bits_of(4.0f) && std::memcmp(&l[10], "lmp3", 4) == 0 && l[19] == bits_of(12.0f),
					"the lit point lights sent as the game keeps them (the one off and the spot left out)");
				put_back(table, 256 * 0x70);
			}
		}

		// 5. The weather: an environment, its weather, its record and the renderer's fog behind the game's globals
		{
			const uintptr_t env_code = hifi::game::find("A1????????83EC24568B701885", 1);
			const uintptr_t renderer_code = hifi::game::find("8B0D????????8D94248C0000008D6F44528D4424605550E8????????84C07517", 2);
			const uintptr_t id_code = hifi::game::find("66A1????????663D0400727E663D1400", 2);
			const uintptr_t fog_code = hifi::game::find("8B95????????526A228B400C508B08FF91C8000000A1????????8B95????????526A248B400C508B08FF91C8000000A1????????8B95????????526A25");
			const uintptr_t env_at = env_code != 0 ? *reinterpret_cast<const uint32_t *>(env_code) : 0;
			const uintptr_t renderer_at = renderer_code != 0 ? *reinterpret_cast<const uint32_t *>(renderer_code) : 0;
			const uintptr_t id_at = id_code != 0 ? *reinterpret_cast<const uint32_t *>(id_code) : 0;
			check(inside(env_at, 4) && inside(renderer_at, 4) && inside(id_at, 2) && fog_code != 0, "the weather's globals (in the image) and the fog's code found");
			if (inside(env_at, 4) && inside(renderer_at, 4) && inside(id_at, 2) && fog_code != 0)
			{
				static uint32_t record[44], object[9], before[8], envo[8], renderer[0x100000 / 4];
				for (uint32_t i = 0; i < 44; ++i)
					record[i] = 0x3F000000u + i;
				std::memcpy(&before[4], "fine", 4);
				object[2] = uint32_t(uintptr_t(before));
				std::memcpy(&object[4], "rain", 4);
				object[7] = bits_of(0.25f);
				object[8] = uint32_t(uintptr_t(record));
				envo[4] = uint32_t(uintptr_t(object));
				envo[6] = uint32_t(uintptr_t(record));
				const uint32_t colour = *reinterpret_cast<const uint32_t *>(fog_code + 2), start = *reinterpret_cast<const uint32_t *>(fog_code + 0x1C),
					finish = *reinterpret_cast<const uint32_t *>(fog_code + 0x36);
				const bool fog_fits = std::max({ colour, start, finish }) < sizeof(renderer);
				check(fog_fits, "the fog's places in the renderer read from the game's code");
				if (fog_fits)
				{
					renderer[colour / 4] = 0x00CC33E6u;
					renderer[start / 4] = bits_of(15.0f);
					renderer[finish / 4] = bits_of(250.0f);
				}
				*reinterpret_cast<uint32_t *>(env_at) = uint32_t(uintptr_t(envo));
				*reinterpret_cast<uint32_t *>(renderer_at) = uint32_t(uintptr_t(renderer));
				*reinterpret_cast<uint16_t *>(id_at) = 6;
				g_sent.weather_calls = 0;
				for (int f = 0; f < 4; ++f)
				{
					now += 0.02;
					hifi::game::feed::frame(e, p, now);
				}
				const std::vector<uint32_t> &w = g_sent.weather;
				check(g_sent.weather_calls == 2, "the weather sent every other frame");
				check(w.size() == 52 && w[0] == 6 && std::memcmp(&w[1], "rain", 4) == 0 && std::memcmp(&w[2], "fine", 4) == 0 && w[3] == bits_of(0.25f) &&
					w[4] == 0x00CC33E6u && w[5] == bits_of(15.0f) && w[6] == bits_of(250.0f) && w[7] == 15 && w[8] == record[0] && w[51] == record[43],
					"the weather's words: its id, what's shown and what it's changing from, the fog the world is drawn with, its own record");
				envo[6] = uint32_t(uintptr_t(before)); // (a stand-in record: not the weather's own)
				now += 0.02;
				hifi::game::feed::frame(e, p, now);
				now += 0.02;
				hifi::game::feed::frame(e, p, now);
				check(g_sent.weather.size() == 52 && g_sent.weather[7] == 14 && g_sent.weather[8] == 0, "a stand-in record isn't sent (the zone's own isn't read yet)");
				*reinterpret_cast<uint32_t *>(env_at) = 0x7FFF0000u; // (freed: a failed read)
				now += 0.02;
				hifi::game::feed::frame(e, p, now);
				now += 0.02;
				hifi::game::feed::frame(e, p, now);
				check(g_sent.weather.size() == 52 && (g_sent.weather[7] & 5) == 0, "an environment that can't be read: nothing of it sent, no crash");
				put_back(env_at, 4);
				put_back(renderer_at, 4);
				put_back(id_at, 2);
			}
		}

		// 5b. The moon: the game's Vana'diel clock, its clock object's seconds (+0xC) plus the server's offset, read
		// through the globals the clock function's code names (FFXiMain 1039E0 -> 0DC130 -> 0DC080)
		{
			const uintptr_t clock_fn = hifi::game::find("E8????????0305????????C3");
			const uintptr_t seconds_fn = clock_fn != 0 ? hifi::game::rel_target(clock_fn) : 0;
			const uintptr_t reader = seconds_fn != 0 ? hifi::game::rel_target(seconds_fn + 5) : 0;
			const uintptr_t object_at = reader != 0 ? *reinterpret_cast<const uint32_t *>(reader + 1) : 0;
			const uintptr_t offset_at = clock_fn != 0 ? *reinterpret_cast<const uint32_t *>(clock_fn + 7) : 0;
			check(inside(object_at, 4) && inside(offset_at, 4), "the Vana'diel clock's object and offset globals found (in the image)");
			if (inside(object_at, 4) && inside(offset_at, 4))
			{
				static uint32_t object[8];
				*reinterpret_cast<uint32_t *>(object_at) = 0; // (the title screen: not running yet)
				*reinterpret_cast<uint32_t *>(offset_at) = 0x2A000000u;
				g_sent.moon_calls = 0;
				now += 0.02;
				hifi::game::feed::frame(e, p, now);
				check(g_sent.moon_calls == 0, "the clock not running yet: no moon sent");
				object[3] = 1234567u; // (+0xC: whole seconds)
				*reinterpret_cast<uint32_t *>(object_at) = uint32_t(uintptr_t(object));
				for (int f = 0; f < 3; ++f)
				{
					now += 0.02;
					hifi::game::feed::frame(e, p, now);
				}
				check(g_sent.moon_calls == 3 && g_sent.moon == 0x2A000000u + 1234567u, "the game's clock sent every frame: its seconds plus the offset");
				// (day 46 of the 84: the full moon, the game's phase 6; 103BA0 says 100)
				object[3] = 46u * 3456u + 1000u - 0x2A000000u % 290304u + 290304u;
				now += 0.02;
				hifi::game::feed::frame(e, p, now);
				const std::vector<std::string> lines = hifi::game::feed::status();
				const bool full = std::count_if(lines.begin(), lines.end(), [](const std::string &l) {
					return l.find("moon: the game's clock") == 0 && l.find("day 46 of the 84, phase 6 of 12, 100% lit") != std::string::npos; }) == 1;
				check(full, "the status: the game's day, phase and light from its clock (day 46: full)");
				put_back(object_at, 4);
				put_back(offset_at, 4);
				now += 0.02;
				hifi::game::feed::frame(e, p, now);
			}
		}

		// 6. The lens flare: hidden (HideFlare on), shown again, never over something else's bytes
		{
			check(flare != 0, "the lens flare's code found");
			if (flare != 0)
			{
				const uint8_t *at = reinterpret_cast<const uint8_t *>(flare + 10);
				check(at[0] == 0xE9 && at[5] == 0x90, "hidden: its je made a jmp (HideFlare on, the default)");
				g_settings["HideFlare"] = "0";
				now += 1.0;
				hifi::game::feed::frame(e, p, now);
				check(at[0] == 0x0F && at[1] == 0x84, "shown again with HideFlare off: the game's je back");
				g_settings["HideFlare"] = "1";
				now += 1.0;
				hifi::game::feed::frame(e, p, now);
				check(at[0] == 0xE9, "hidden again");
			}
		}

		// 7. The visibility tests: pointed at World FX's entries with VisTest on, put back with it off
		{
			const char *const patterns[] = { "51528BCEE8????????85C07407B0015E83C410C3", "51528BCEE8????????89442418DB4424185E",
				"50518D8E7F010000E8????????8BF88B8604010000", "51528D8E8C080000E8????????85C00F84" };
			const uint32_t offsets[] = { 4, 4, 8, 8 };
			uintptr_t sites[4] = {};
			uint32_t test = 0;
			bool found = true;
			for (int i = 0; i < 4; ++i)
			{
				const uintptr_t m = hifi::game::find(patterns[i]);
				found = found && m != 0;
				sites[i] = m != 0 ? m + offsets[i] : 0;
				if (m != 0)
					test = hifi::game::rel_target(sites[i]);
			}
			check(found, "the four visibility test calls found");
			g_settings["VisTest"] = "1";
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			bool pointed = found;
			for (int i = 0; i < 4 && found; ++i)
				pointed = pointed && hifi::game::rel_target(sites[i]) == 0x31000000u + uint32_t(i) * 0x10;
			check(pointed && g_sent.vis_original == test && g_sent.vis_on == 1, "VisTest on: each call pointed at World FX's entry for its kind, World FX told the game's test");
			g_settings["VisTest"] = "0";
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			bool back = found;
			for (int i = 0; i < 4 && found; ++i)
				back = back && hifi::game::rel_target(sites[i]) == test;
			check(back && g_sent.vis_on == 0, "VisTest off: the calls back to the game's test");
		}

		// 8. The game's water and rain: routed as their settings say (their own tests check the routines and lists)
		{
			g_settings["HideGameWater"] = "1";
			g_settings["HideGameRain"] = "1";
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			const hifi::game::gamewater::sites w = hifi::game::gamewater::where();
			const hifi::game::gamerain::sites r = hifi::game::gamerain::where();
			const bool water_in = w.memory != 0 && w.call != 0 && hifi::game::rel_target(w.call) >= w.memory && hifi::game::rel_target(w.call) < w.memory + 0x10000;
			const bool rain_in = r.memory != 0 && r.site != 0 && hifi::game::rel_target(r.site) >= r.memory && hifi::game::rel_target(r.site) < r.memory + 0x10000;
			check(water_in && rain_in, "HideGameWater and HideGameRain on: the water's draw and the rain's read go to their routines");
			g_settings["HideGameWater"] = "0";
			g_settings["HideGameRain"] = "0";
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			check(w.call != 0 && hifi::game::rel_target(w.call) == w.draw && r.site != 0 && *reinterpret_cast<const uint8_t *>(r.site) == 0x8A,
				"both off: the game's draw called again, its read back");
			g_settings["HideGameWater"] = "1";
			g_settings["HideGameRain"] = "1";
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			check(hifi::game::rel_target(w.call) >= w.memory && hifi::game::rel_target(r.site) >= r.memory, "on again (put back by stop, below)");
		}

		// 9. The game's character shadows: its three receivers made to return at once while World FX casts characters'
		// shadows (HideGameShadows on), back when it doesn't, back when the setting goes off
		{
			struct receiver { const char *pattern; uint32_t call_at; uint8_t ret; };
			const receiver receivers[] = { { "50518B0D????????52E8????????C6460400B0015EC20400", 9, 0x10 },
				{ "8B442408518B0D????????D91C2450E8????????B0015EC20400", 15, 8 }, { "518B0D????????E8????????B0015EC20400", 7, 0xC } };
			uintptr_t entry[3] = {};
			std::string game_bytes[3];
			bool found = true;
			for (int i = 0; i < 3; ++i)
			{
				const uintptr_t m = hifi::game::find(receivers[i].pattern);
				entry[i] = m != 0 ? hifi::game::rel_target(m + receivers[i].call_at) : 0;
				found = found && entry[i] != 0 && *reinterpret_cast<const uint8_t *>(entry[i]) == 0x81;
				if (entry[i] != 0)
					game_bytes[i] = std::string(reinterpret_cast<const char *>(entry[i]), 6);
			}
			check(found, "the game's three shadow receivers found (each starting sub esp, n)");
			auto returning = [&] {
				bool all = found;
				for (int i = 0; i < 3 && found; ++i)
				{
					const uint8_t *b = reinterpret_cast<const uint8_t *>(entry[i]);
					all = all && b[0] == 0xC2 && b[1] == receivers[i].ret && b[2] == 0 && b[3] == 0x90 && b[4] == 0x90 && b[5] == 0x90;
				}
				return all;
			};
			auto as_game = [&] {
				bool all = found;
				for (int i = 0; i < 3 && found; ++i)
					all = all && std::string(reinterpret_cast<const char *>(entry[i]), 6) == game_bytes[i];
				return all;
			};
			g_settings["HideGameShadows"] = "1";
			g_casting = 0;
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			check(as_game(), "World FX not casting characters' shadows: the game's drawn");
			g_casting = 1;
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			check(returning(), "World FX casting them: each receiver returns at once (ret 10h, 8, 0Ch: as much as it cleans)");
			g_casting = 0;
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			check(as_game(), "not casting again (the sun down, no open sky): the game's back");
			g_casting = 1;
			g_settings["HideGameShadows"] = "0";
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			check(as_game(), "HideGameShadows off: the game's drawn even while World FX casts");
			g_settings["HideGameShadows"] = "1";
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			const std::vector<std::string> lines = hifi::game::feed::status();
			check(returning() && std::count_if(lines.begin(), lines.end(),
				[](const std::string &l) { return l.find("character shadows: left out") != std::string::npos; }) == 1,
				"left out again (put back by stop, below), and the status says so");
		}

		// 10. World FX not drawing into the game's world (another output: DXVK, dgVoodoo's Direct3D 11): the game keeps its
		// own water, rain and snow, character shadows and visibility tests, whatever the settings say; back with it
		{
			const hifi::game::gamewater::sites w = hifi::game::gamewater::where();
			const hifi::game::gamerain::sites r = hifi::game::gamerain::where();
			g_settings["HideGameWater"] = "1";
			g_settings["HideGameRain"] = "1";
			g_settings["VisTest"] = "1";
			g_casting = 1;
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			const bool routed = w.call != 0 && hifi::game::rel_target(w.call) >= w.memory && r.site != 0 && *reinterpret_cast<const uint8_t *>(r.site) == 0xE8 &&
				g_sent.vis_on == 1;
			g_drawing = 0;
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			const std::vector<std::string> lines = hifi::game::feed::status();
			const bool own = w.call != 0 && hifi::game::rel_target(w.call) == w.draw && *reinterpret_cast<const uint8_t *>(r.site) == 0x8A && g_sent.vis_on == 0 &&
				std::count_if(lines.begin(), lines.end(), [](const std::string &l) { return l.find("its own (World FX isn't drawing") != std::string::npos; }) == 1;
			check(routed && own, "World FX not drawing: the game's water, rain and visibility tests its own again, the status says why");
			g_drawing = 1;
			now += 1.0;
			hifi::game::feed::frame(e, p, now);
			check(hifi::game::rel_target(w.call) >= w.memory && *reinterpret_cast<const uint8_t *>(r.site) == 0xE8 && g_sent.vis_on == 1,
				"drawing again: left out again (put back by stop, below)");
			g_settings["VisTest"] = "0";
		}

		// 11. Status, then everything put back
		{
			const std::vector<std::string> s = hifi::game::feed::status();
			check(s.size() >= 8 && s[0].find("zone 101") != std::string::npos, "the status lines (the zone first)");
		}
		hifi::game::feed::stop();
		check(env("FFXI_WORLDFX_ZONE") == "(none)" && env("FFXI_WORLDFX_INGAME") == "(none)", "stopped: the zone and in game taken out of the environment");
		// (the test's own writes, the light table and the weather's globals, were put back above)
		const bool own = std::memcmp(clean.data(), reinterpret_cast<const void *>(base), size) == 0;
		for (const addon_site &a : addon)
			std::memcpy(reinterpret_cast<void *>(a.at), a.dumped.data(), a.dumped.size());
		check(own && dump::same_as_file(path, base, size), "stopped: the image is the dump again, byte for byte (the flare, the visibility calls, the water and rain)");
		dump::unmap(base);
	}
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	for (const char *path : dump::kDumps)
		run(path);
	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures == 0 ? 0 : 1;
}
