#include <windows.h>
#include "feed.hpp"
#include "gamerain.hpp"
#include "gamewater.hpp"
#include "glows.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

// The worldfx Ashita addon's worldfx.lua, ported part by part (its comments carried over where they still hold).
// World FX itself is in this DLL: what the addon looked up with GetProcAddress is called directly; the zone and
// whether a character is in still go through this process's environment (FFXI_WORLDFX_ZONE, FFXI_WORLDFX_INGAME),
// where World FX reads them.

namespace hifi::game::feed
{
	namespace
	{
		// Characters and monsters near, for the grass to bend away from: the 32 nearest shown entities within 50 yalms
		// of you, picked a few times a second, their positions sent every frame. Ashita's axes (x east, y north, z
		// height) are the game's (x, z, y): y is height there, down positive, as Ashita's z. With each a word: 1 while
		// they're fighting (the game's engaged status, 1), which wears the grass under them faster.
		constexpr uint32_t kActorsMax = 32;
		constexpr float kActorsReach = 50.0f, kActorRadius = 0.9f; // (radius: at least, else their model size)
		constexpr double kActorsPick = 0.25;
		struct actors_state
		{
			std::vector<uint32_t> picked;
			double next_pick = 0.0;
			float buf[kActorsMax * 4] = {};
			uint32_t flags[kActorsMax] = {};
			uint32_t sent = 0, fighting = 0;
			// You: the party's target index for you; when it has none, the entity with your server id (looked for at
			// most every kActorsPick seconds, then kept while it still has it). World FX leaves the characters sent out
			// of its lamp shadows, and without yours it drew a stepped copy of your silhouette away from a Mog House
			// lamp (2026-10-21).
			uint32_t me = 0;
			bool me_known = false;
			double me_retry = 0.0;
			const char *me_from = "not looked for yet";
		};
		actors_state g_actors;

		// Weapon skills cut the grass. The action packet (0x028, read only: it goes on to the game as it came) of a
		// weapon skill used near (category 3) says who used it, which one and on whom: its fields packed bit by bit, low
		// bit first, from bit 40 (the server's GP_SERV_COMMAND_BATTLE2: user 32, targets 6, 4, category 4, the skill 32,
		// 32; then each target's id 32 and results 4, each result 85 bits, an added effect's flag 1 (+37) and a spikes'
		// flag 1 (+34)). Each target gets a swath from the user to kCutPast beyond it, kCutWidth yalms each side, when
		// it stands within kCutReach; one further off (a bow's, a gun's, a thrown skill) only a circle of kCutWidth
		// where it lands. An area skill also cuts a circle of its radius round the first target (the server's
		// weapon_skills table: aoe 2, its radius), first.
		constexpr float kCutWidth = 1.0f, kCutReach = 5.0f, kCutPast = 1.0f;
		constexpr uint32_t kCutsMax = 8;
		const std::unordered_map<uint32_t, float> kAreaSkills = { { 6, 4.0f }, { 20, 10.0f }, { 30, 10.0f }, { 38, 4.0f }, { 52, 10.0f },
			{ 91, 4.0f }, { 100, 4.0f }, { 123, 10.0f }, { 164, 6.0f }, { 178, 10.0f }, { 189, 10.0f }, { 238, 10.0f }, { 239, 10.0f },
			{ 240, 10.0f } };
		struct cuts_state
		{
			uint32_t sent = 0, last = 0;
			bool any = false;
		};
		cuts_state g_cuts;

		// Lamps: the game's global light table (reshade-worldfx's client notes, "Zone lamps"), found by the code of the
		// game's find-by-id for it (mov edx, [esp+4]; mov eax, <the table>; xor ecx, ecx; cmp [eax], edx; je): 256
		// entries of 0x70 bytes, +0 a light's 4-character id, +8 its D3DLIGHT8 (type +8, colour r g b +0xC, position
		// +0x3C, range +0x54, brightness +0x68, its Theta). A lamp the game has turned off has brightness 0. Every few
		// frames the lit ones (point lights with some brightness) go to World FX (worldfx_lamps), which matches them by
		// id to the zone's lamps it has baked. Read only.
		constexpr const char *kLampsPattern = "8B542404B8????????33C93910740E";
		constexpr uint32_t kLampsCount = 256, kLampWords = 10, kLampsEvery = 3;
		struct lamps_state
		{
			uintptr_t table = 0;
			const char *why = "not looked for yet";
			uint32_t buf[kLampsCount * kLampWords] = {};
			uint32_t frames = 0, lit = 0, listed = 0, sent = 0;
		};
		lamps_state g_lamps;

		// The weather (reshade-worldfx's client notes, "Weather and sky"), for World FX's fog and shadows, each thing
		// found by the game's own code that uses it: the environment object's global (03D9A0: its +0x10 the weather
		// object, +0x18 the record the zone is lit by now), the zone renderer's global and where in it the game keeps
		// the fog it draws the world with (17BEAD), the weather's id (2202B8). Every other frame they go to World FX
		// (worldfx_weather) as 52 words as the game keeps them; the environment, its weather and the renderer are heap
		// objects, read through ReadProcessMemory (one freed under us, while zoning, is a failed read). The record is
		// sent only when it's the weather's own (+0x18 = the weather's +0x20): entering a zone, the game lights it with a
		// stand-in for a frame or so. Read only.
		constexpr const char *kWeatherEnv = "A1????????83EC24568B701885";                                         // +1
		constexpr const char *kWeatherRenderer = "8B0D????????8D94248C0000008D6F44528D4424605550E8????????84C07517"; // +2
		constexpr const char *kWeatherFog = "8B95????????526A228B400C508B08FF91C8000000A1????????8B95????????526A248B400C508B08FF91C8000000A1????????8B95????????526A25"; // +2, +0x1C, +0x36
		constexpr const char *kWeatherId = "66A1????????663D0400727E663D1400";                                   // +2
		constexpr uint32_t kWeatherWords = 52, kWeatherEvery = 2;
		const char *const kWeatherNames[] = { "fine", "suny", "clod", "mist", "dryw", "heat", "rain", "squl", "dust", "sand", "wind",
			"stom", "snow", "bliz", "thdr", "bolt", "aura", "ligt", "fogd", "dark" };
		struct weather_state
		{
			uintptr_t env = 0, renderer = 0, id = 0;
			bool fog_known = false;
			uint32_t fog_colour = 0, fog_start = 0, fog_finish = 0;
			const char *why = "not looked for yet";
			uint32_t buf[kWeatherWords] = {};
			uint32_t frames = 0, sent = 0;
		};
		weather_state g_weather;

		// The lens flare: hidden unless HiFi XI's "Hide the lens flare" setting is off. Its draw (in its object's update,
		// FFXiMain 0548D0) is skipped when its visibility test says hidden; this makes that jump (0x54AF1, je -> jmp)
		// always taken, so the flare is never drawn. The test itself still runs. Put back on unload.
		constexpr const char *kFlarePattern = "8BF88B860401000085C00F84????????85FF0F8E"; // (the je at +10)
		struct flare_state
		{
			uintptr_t at = 0;
			uint8_t saved[6] = {}, ours[6] = {};
			bool off = false;
			std::string why;
		};
		flare_state g_flare;

		// The game's own character shadows (client notes, "Frame order": 7. shadow receivers), left out while World FX
		// casts its own (HideGameShadows on, worldfx_casts_characters): drawn both, the game's at its own fixed angle
		// (elevation held near 41 degrees) and World FX's at the sun's, they lay two silhouettes of different lengths
		// on the ground, like copies of one (in game, 2026-10-22). The game's shadow dispatcher (033D..) calls one of
		// three receivers for a character: your silhouette projected (185CA0), a blob (184FE0), another (184C90); each
		// is reached only from there, starts sub esp, n (6 bytes) and cleans its own arguments. Each start made a
		// ret of the same size: the dispatcher goes on as after the draw. (Their silhouettes are still rendered into
		// the game's shadow target, 033800: its callers go by its answer.) Put back when not wanted, and on unload.
		struct shadow_site_info
		{
			const char *name, *pattern;
			uint32_t call_at;
			uint16_t ret; // (the stack it cleans)
		};
		const shadow_site_info kShadowSites[] = {
			// push eax; push ecx; mov ecx, [renderer]; push edx; call <your silhouette>; mov byte [esi+4], 0; mov al, 1; pop esi; ret 4
			{ "your silhouette", "50518B0D????????52E8????????C6460400B0015EC20400", 9, 0x10 },
			// mov eax, [esp+8]; push ecx; mov ecx, [renderer]; fstp [esp]; push eax; call <blob>; mov al, 1; pop esi; ret 4
			{ "blobs", "8B442408518B0D????????D91C2450E8????????B0015EC20400", 15, 8 },
			// push ecx; mov ecx, [renderer]; call <another>; mov al, 1; pop esi; ret 4
			{ "the other kind", "518B0D????????E8????????B0015EC20400", 7, 0xC },
		};
		struct shadow_site
		{
			uintptr_t at = 0;
			uint8_t saved[6] = {}, ours[6] = {};
		};
		struct shadows_state
		{
			std::vector<shadow_site> sites; // (found: all three, or none)
			bool looked = false, off = false;
			std::string why;
		};
		shadows_state g_shadows;

		// The game's visibility tests (FFXiMain 06C280; client-notes "The visibility test, and a replacement that
		// doesn't wait"): a screen box drawn against the scene's depth and read back from the GPU, which waits for the
		// GPU to finish the frame so far. Glows, every effect element's fade, the sun's lens flare and doors test every
		// frame. With World FX's VisTest setting on, these four calls go to its entries (worldfx_vis_entry), which
		// answer from its copy of the world's depth a few frames late, without waiting; the game's own test still runs
		// for a test object with no answer yet and while none come in. (Characters' calls are left to hifps's walls.)
		// Put back when the setting goes off, and on unload.
		struct vis_site_info
		{
			const char *name, *pattern;
			uint32_t at, kind;
		};
		const vis_site_info kVisSites[] = {
			// push ecx; push edx; mov ecx, esi; call test; test eax, eax; je; mov al, 1 (a glow: drawn or not)
			{ "glow", "51528BCEE8????????85C07407B0015E83C410C3", 4, 0 },
			// push ecx; push edx; mov ecx, esi; call test; mov [esp+18h], eax; fild [esp+18h]; pop esi (an element's fade)
			{ "glow fade", "51528BCEE8????????89442418DB4424185E", 4, 1 },
			// push eax; push ecx; lea ecx, [esi+17Fh]; call test; mov edi, eax (the sun's lens flare)
			{ "lens flare", "50518D8E7F010000E8????????8BF88B8604010000", 8, 2 },
			// push ecx; push edx; lea ecx, [esi+88Ch]; call test; test eax, eax; je (a door; its 'hidden' bit)
			{ "door", "51528D8E8C080000E8????????85C00F84", 8, 3 },
		};
		// The test's first bytes: mov eax, [renderer]; push ebx; push esi; mov esi, ecx; mov bl, [eax+828h] (0: any byte)
		const uint8_t kVisTest[] = { 0xA1, 0, 0, 0, 0, 0x53, 0x56, 0x8B, 0xF1, 0x8A, 0x98, 0x28, 0x08, 0x00, 0x00 };
		struct vis_site
		{
			const char *name;
			uintptr_t site;
			uint32_t kind;
			uint8_t saved[4];
			uintptr_t entry;
		};
		struct vis_state
		{
			std::vector<vis_site> sites; // (empty: the game's own)
			bool tried = false;
			std::string why;
		};
		vis_state g_vis;

		// The Mog House: the party data keeps the town's zone there, so World FX drew the town's grass, water and
		// shadows in the room (in game, 2026-10-21). The zone-in packet (0x00A, read only) says where you are: its login
		// state (+0x80) is 1 in a Mog House, the room's model at +0xAA. There World FX is told kMogHouseZone + the
		// model, past every zone id, a zone it has nothing baked for; lamps still glow, and the game keeps its own water
		// and rain.
		constexpr uint32_t kMogHouseZone = 1000;
		struct zone_state
		{
			uint32_t mog_house = 0; // (the room's model + 1, while in one; 0 not)
			uint32_t last = 0;      // (told to World FX; 0 none yet)
			int in_game = -1;       // (told: 1, 0; -1 not yet)
			double next_check = 0.0;
		};
		zone_state g_zone;

		// The game's patches the settings ask for, each tried again only when its setting changes
		struct wanted
		{
			int flare = -1, water = -1, rain = -1;
		};
		wanted g_wanted;

		// n bits (at most 32) from bit pos of the packet, low bit first
		uint32_t bits(const uint8_t *data, uint32_t size, uint32_t pos, uint32_t n)
		{
			uint32_t v = 0;
			for (int k = int(n) - 1; k >= 0; --k)
			{
				const uint32_t p = pos + uint32_t(k);
				const uint32_t byte = p / 8 < size ? data[p / 8] : 0;
				v = v * 2 + ((byte >> (p % 8)) & 1u);
			}
			return v;
		}

		void publish_zone(uint32_t zone)
		{
			char text[16];
			std::snprintf(text, sizeof(text), "%u", zone);
			SetEnvironmentVariableA("FFXI_WORLDFX_ZONE", zone != 0 ? text : nullptr);
		}

		void publish_in_game(bool in)
		{
			const int now = in ? 1 : 0;
			if (now == g_zone.in_game)
				return;
			g_zone.in_game = now;
			SetEnvironmentVariableA("FFXI_WORLDFX_INGAME", in ? "1" : "0");
		}

		// Actors

		bool own_index(const entities &e, const party &p, double now, uint32_t &me)
		{
			if (p.target_index != 0)
			{
				g_actors.me_from = "the party";
				me = p.target_index;
				return true;
			}
			if (p.server_id == 0)
			{
				g_actors.me_from = "nowhere: the party has neither your target index nor your server id";
				return false;
			}
			if (g_actors.me_known && e.server_id(g_actors.me) == p.server_id)
			{
				me = g_actors.me;
				return true;
			}
			g_actors.me_known = false;
			if (now < g_actors.me_retry)
				return false;
			g_actors.me_retry = now + kActorsPick;
			for (uint32_t i = 0; i < entities::kCount; ++i)
				if (e.server_id(i) == p.server_id)
				{
					g_actors.me = i;
					g_actors.me_known = true;
					g_actors.me_from = "your server id (the party has no target index for you)";
					me = i;
					return true;
				}
			g_actors.me_from = "nowhere: no entity has your server id";
			return false;
		}

		// The nearest shown entities to (x, y) (Ashita's), every few frames
		void pick(const entities &e, float x, float y)
		{
			struct near_one { uint32_t i; float d; };
			std::vector<near_one> list;
			for (uint32_t i = 0; i < entities::kCount; ++i)
			{
				float ex, ey, ez;
				if (!e.shown(i) || !e.position(i, ex, ey, ez) || (ex == 0.0f && ey == 0.0f))
					continue;
				const float d = (ex - x) * (ex - x) + (ey - y) * (ey - y);
				if (d <= kActorsReach * kActorsReach)
					list.push_back({ i, d });
			}
			std::stable_sort(list.begin(), list.end(), [](const near_one &a, const near_one &b) { return a.d < b.d; });
			g_actors.picked.clear();
			for (size_t k = 0; k < list.size() && k < kActorsMax; ++k)
				g_actors.picked.push_back(list[k].i);
		}

		void send_actors(const entities &e, const party &p, double now)
		{
			uint32_t me = 0;
			if (!own_index(e, p, now, me))
			{
				g_actors.sent = g_actors.fighting = 0;
				worldfx_grass_actors2(g_actors.buf, g_actors.flags, 0);
				return;
			}
			if (now >= g_actors.next_pick)
			{
				g_actors.next_pick = now + kActorsPick;
				float x, y, z;
				if (e.position(me, x, y, z))
					pick(e, x, y);
			}
			uint32_t n = 0, fights = 0;
			for (uint32_t i : g_actors.picked)
			{
				float x, y, z;
				if (!e.position(i, x, y, z) || !e.shown(i))
					continue;
				const float size = e.model_size(i);
				const float r = std::isfinite(size) ? std::min(std::max(size, kActorRadius), 5.0f) : kActorRadius;
				float *b = &g_actors.buf[n * 4];
				b[0] = x, b[1] = z, b[2] = y, b[3] = r;
				g_actors.flags[n] = e.status(i) == 1 ? 1u : 0u;
				fights += g_actors.flags[n];
				n++;
			}
			g_actors.sent = n;
			g_actors.fighting = fights;
			worldfx_grass_actors2(g_actors.buf, g_actors.flags, n);
		}

		// Weapon skills: the user, the skill and the targets (server ids); false for any other action
		bool weapon_skill(const uint8_t *data, uint32_t size, uint32_t &user, uint32_t &skill, std::vector<uint32_t> &targets)
		{
			if (size < 19 || bits(data, size, 82, 4) != 3)
				return false;
			user = bits(data, size, 40, 32);
			const uint32_t count = bits(data, size, 72, 6);
			skill = bits(data, size, 86, 32);
			uint32_t pos = 150;
			const uint32_t total = size * 8;
			for (uint32_t t = 0; t < std::min<uint32_t>(count, 15); ++t)
			{
				if (pos + 36 > total)
					break;
				targets.push_back(bits(data, size, pos, 32));
				const uint32_t results = bits(data, size, pos + 32, 4);
				pos += 36;
				for (uint32_t r = 0; r < results; ++r)
				{
					pos += 85;
					pos += 1 + (bits(data, size, pos, 1) == 1 ? 37 : 0);
					pos += 1 + (bits(data, size, pos, 1) == 1 ? 34 : 0);
				}
			}
			return true;
		}

		void cut_for(const uint8_t *data, uint32_t size, const entities &e)
		{
			uint32_t user = 0, skill = 0;
			std::vector<uint32_t> targets;
			if (!weapon_skill(data, size, user, skill, targets) || targets.empty())
				return;
			// (the entities of the user and the targets, in one look through the list)
			std::unordered_map<uint32_t, uint32_t> found;
			auto wanted = [&](uint32_t id) { return id == user || std::find(targets.begin(), targets.end(), id) != targets.end(); };
			for (uint32_t i = 0; i < entities::kCount; ++i)
			{
				const uint32_t id = e.server_id(i);
				if (id != 0 && wanted(id) && found.find(id) == found.end())
					found.emplace(id, i);
			}
			if (found.find(user) == found.end())
				return;
			// (the game's x, height, z: Ashita's x, z, y)
			auto where = [&](uint32_t i, float &x, float &y, float &z) {
				float ax = 0.0f, ay = 0.0f, az = 0.0f;
				e.position(i, ax, ay, az);
				x = ax, y = az, z = ay;
			};
			float ux, uy, uz;
			where(found[user], ux, uy, uz);
			float buf[kCutsMax * 7];
			uint32_t n = 0;
			auto add = [&](float ax, float ay, float az, float w, float bx, float by, float bz) {
				if (n >= kCutsMax)
					return;
				float *b = &buf[n * 7];
				b[0] = ax, b[1] = ay, b[2] = az, b[3] = w, b[4] = bx, b[5] = by, b[6] = bz;
				n++;
			};
			const auto first = found.find(targets[0]);
			const auto area = kAreaSkills.find(skill);
			if (first != found.end() && area != kAreaSkills.end())
			{
				float tx, ty, tz;
				where(first->second, tx, ty, tz);
				add(tx, ty, tz, area->second, tx, ty, tz);
			}
			for (uint32_t id : targets)
			{
				const auto t = found.find(id);
				if (t == found.end())
					continue;
				float tx, ty, tz;
				where(t->second, tx, ty, tz);
				const float dx = tx - ux, dz = tz - uz;
				const float d = std::sqrt(dx * dx + dz * dz);
				if (d > 0.01f && d <= kCutReach)
					add(ux, uy, uz, kCutWidth, tx + dx / d * kCutPast, ty, tz + dz / d * kCutPast);
				else
					add(tx, ty, tz, kCutWidth, tx, ty, tz);
			}
			if (n > 0)
			{
				worldfx_grass_cut(buf, n);
				g_cuts.sent++;
				g_cuts.last = skill;
				g_cuts.any = true;
			}
		}

		// Lamps

		void find_lamps()
		{
			if (g_lamps.table != 0 || std::strcmp(g_lamps.why, "not looked for yet") != 0)
				return;
			const uintptr_t m = find(kLampsPattern);
			if (m == 0)
			{
				g_lamps.why = "the game's light table wasn't found";
				return;
			}
			const uint32_t at = word(m + 5);
			if (at == 0 || at % 4 != 0)
			{
				g_lamps.why = "the game's light table isn't where its code says";
				return;
			}
			g_lamps.table = at;
			g_lamps.why = nullptr;
		}

		// The lit lights into the buffer, 10 words each: id, type, r, g, b, x, y, z, range, brightness; how many
		uint32_t read_lamps()
		{
			static uint32_t table[kLampsCount * 28];
			uint32_t n = 0, listed = 0;
			if (read(g_lamps.table, table, sizeof(table)))
				for (uint32_t i = 0; i < kLampsCount; ++i)
				{
					const uint32_t *p = &table[i * 28]; // (0x70 bytes)
					if (p[0] == 0)
						continue;
					listed++;
					const uint32_t bright = p[26];
					// (a point light whose brightness is a positive, finite float: its bits between 0 and 0x7F800000)
					if (p[2] != 1 || bright == 0 || bright >= 0x7F800000u)
						continue;
					uint32_t *b = &g_lamps.buf[n * kLampWords];
					b[0] = p[0], b[1] = 1;
					b[2] = p[3], b[3] = p[4], b[4] = p[5];
					b[5] = p[15], b[6] = p[16], b[7] = p[17];
					b[8] = p[21], b[9] = bright;
					n++;
				}
			g_lamps.lit = n;
			g_lamps.listed = listed;
			return n;
		}

		void send_lamps()
		{
			g_lamps.frames++;
			if (g_lamps.table == 0 || g_lamps.frames % kLampsEvery != 0)
				return;
			worldfx_lamps(g_lamps.buf, read_lamps());
			g_lamps.sent++;
		}

		// Weather

		void find_weather()
		{
			if (std::strcmp(g_weather.why != nullptr ? g_weather.why : "", "not looked for yet") != 0)
				return;
			auto at = [](const char *pattern, intptr_t offset) {
				const uintptr_t m = find(pattern);
				return m != 0 ? uintptr_t(word(m + offset)) : uintptr_t(0);
			};
			g_weather.env = at(kWeatherEnv, 1);
			g_weather.renderer = at(kWeatherRenderer, 2);
			g_weather.id = at(kWeatherId, 2);
			if (const uintptr_t m = find(kWeatherFog))
			{
				const uint32_t colour = word(m + 2), start = word(m + 0x1C), finish = word(m + 0x36);
				if (colour % 4 == 0 && start % 4 == 0 && finish % 4 == 0 && std::max({ colour, start, finish }) < 0x100000)
				{
					g_weather.fog_known = true;
					g_weather.fog_colour = colour;
					g_weather.fog_start = start;
					g_weather.fog_finish = finish;
				}
			}
			g_weather.why = g_weather.env == 0 ? "the game's environment object wasn't found" : nullptr;
		}

		// The words for World FX (grass_render.hpp, grass::kWeatherWords): [0] the id, [1] the weather shown, [2] the
		// one it's changing from, [3] the change's share (bits), [4..6] the fog's colour, start, end (bits), [7] what
		// was read (1 the record, 2 the fog, 4 the weather object, 8 the id), [8..51] the record. Returns [7].
		uint32_t read_weather()
		{
			uint32_t *w = g_weather.buf;
			std::memset(w, 0, sizeof(g_weather.buf));
			uint32_t got = 0;
			w[0] = 0xFFFFFFFFu;
			if (g_weather.id != 0)
			{
				uint16_t id = 0;
				if (read(g_weather.id, id))
				{
					w[0] = id;
					got += 8;
				}
			}
			const uint32_t env = g_weather.env != 0 ? word(g_weather.env) : 0;
			uint32_t e[7];
			if (env != 0 && read(env, e, sizeof(e)))
			{
				const uint32_t object = e[4], record = e[6]; // (+0x10, +0x18)
				uint32_t own = 0;                            // (the weather's own record, +0x20)
				uint32_t o[9];
				if (object != 0 && read(object, o, sizeof(o)))
				{
					const uint32_t before = o[2], name = o[4], share = o[7];
					own = o[8];
					w[1] = name;
					w[3] = share;
					uint32_t b = 0;
					if (before != 0 && read(before + 0x10, b))
						w[2] = b;
					got += 4;
				}
				if (record != 0 && record == own && read(record, &w[8], 44 * 4))
					got += 1;
			}
			const uint32_t renderer = g_weather.renderer != 0 ? word(g_weather.renderer) : 0;
			if (renderer != 0 && g_weather.fog_known)
			{
				uint32_t c = 0, s = 0, f = 0;
				if (read(renderer + g_weather.fog_colour, c))
				{
					w[4] = c;
					if (read(renderer + g_weather.fog_start, s))
					{
						w[5] = s;
						if (read(renderer + g_weather.fog_finish, f))
						{
							w[6] = f;
							got += 2;
						}
					}
				}
			}
			w[7] = got;
			return got;
		}

		void send_weather()
		{
			g_weather.frames++;
			if (g_weather.why != nullptr || g_weather.frames % kWeatherEvery != 0)
				return;
			read_weather();
			worldfx_weather(g_weather.buf, kWeatherWords);
			g_weather.sent++;
		}

		float floated(uint32_t bits)
		{
			float f = 0.0f;
			return float_of(bits, f) ? f : 0.0f;
		}

		std::string weather_status()
		{
			if (g_weather.why != nullptr)
				return std::string("not read: ") + g_weather.why;
			const uint32_t got = read_weather();
			const uint32_t *w = g_weather.buf;
			std::vector<std::string> parts;
			char line[256];
			if ((got & 8) != 0)
			{
				std::snprintf(line, sizeof(line), "the server's weather %u (%s)", w[0], w[0] < 20 ? kWeatherNames[w[0]] : "?");
				parts.push_back(line);
			}
			if ((got & 4) != 0)
			{
				std::string s = "shown " + (w[1] != 0 ? name_text(w[1]) : std::string("none"));
				if (w[2] != 0 && floated(w[3]) < 1.0f)
				{
					std::snprintf(line, sizeof(line), ", changing from %s, %.0f%% there", name_text(w[2]).c_str(), floated(w[3]) * 100.0f);
					s += line;
				}
				parts.push_back(s);
			}
			if ((got & 2) != 0)
			{
				std::snprintf(line, sizeof(line), "fog %u %u %u from %.0f to %.0f yalms", (w[4] >> 16) & 0xFF, (w[4] >> 8) & 0xFF, w[4] & 0xFF, floated(w[5]), floated(w[6]));
				parts.push_back(line);
			}
			if ((got & 1) != 0)
			{
				auto r = [&](uint32_t off) { return floated(w[8 + off / 4]); };
				// (the sun's light, as World FX weighs it against the zone's clear sky: its colour's luminance times the intensity)
				const uint32_t c = w[8 + 0x2C / 4];
				const float sun = (0.3f * float(c & 0xFF) + 0.59f * float((c >> 8) & 0xFF) + 0.11f * float((c >> 16) & 0xFF)) / 255.0f * r(0x44);
				std::snprintf(line, sizeof(line), "the zone's light %.2f (the sun's %.2f), its record's fog from %.0f to %.0f, drawn to %.0f yalms", r(0x44), sun, r(0x40), r(0x3C), r(0x58));
				parts.push_back(line);
			}
			else if ((got & 4) != 0)
				parts.push_back("its record not read yet (the game lights the zone with a stand-in for now)");
			if (parts.empty())
				parts.push_back("nothing read (not in a zone?)");
			std::string s;
			for (const std::string &p : parts)
				s += (s.empty() ? "" : "; ") + p;
			return s;
		}

		// The lens flare

		bool set_flare(bool off, std::string &why)
		{
			if (g_flare.at == 0)
			{
				const uintptr_t m = find(kFlarePattern);
				if (m == 0)
				{
					why = "the lens flare's code wasn't found";
					return false;
				}
				if (!read(m + 10, g_flare.saved, 6) || g_flare.saved[0] != 0x0F || g_flare.saved[1] != 0x84)
				{
					why = "the lens flare's code isn't as expected";
					return false;
				}
				g_flare.at = m + 10;
				// (je rel32 -> jmp rel32, a byte shorter: rel + 1, then a nop)
				uint32_t rel = 0;
				std::memcpy(&rel, g_flare.saved + 2, 4);
				g_flare.ours[0] = 0xE9;
				put32(g_flare.ours + 1, rel + 1);
				g_flare.ours[5] = 0x90;
			}
			if (off == g_flare.off)
				return true;
			const uint8_t *now = off ? g_flare.saved : g_flare.ours, *to = off ? g_flare.ours : g_flare.saved;
			if (!code_is(g_flare.at, now, 6))
			{
				why = "something else has changed the lens flare's code since (the worldfx addon?): left as it is";
				return false;
			}
			if (!write_code(g_flare.at, to, 6))
			{
				why = "couldn't write the game's code";
				return false;
			}
			g_flare.off = off;
			return true;
		}

		// The game's character shadows: found once (all three receivers, or why not)
		bool find_shadows(std::string &why)
		{
			if (g_shadows.looked)
			{
				why = g_shadows.why;
				return !g_shadows.sites.empty();
			}
			g_shadows.looked = true;
			std::vector<shadow_site> found;
			for (const shadow_site_info &s : kShadowSites)
			{
				const uintptr_t m = find(s.pattern);
				const uintptr_t entry = m != 0 ? rel_target(m + s.call_at) : 0;
				shadow_site f;
				// (its start: sub esp, imm32)
				if (entry == 0 || !read(entry, f.saved, 6) || f.saved[0] != 0x81 || f.saved[1] != 0xEC)
				{
					g_shadows.why = std::string("the game's shadow code (") + s.name + ") wasn't found";
					why = g_shadows.why;
					return false;
				}
				f.at = entry;
				f.ours[0] = 0xC2; // ret n; nop x 3
				f.ours[1] = uint8_t(s.ret & 0xFF);
				f.ours[2] = uint8_t(s.ret >> 8);
				f.ours[3] = f.ours[4] = f.ours[5] = 0x90;
				found.push_back(f);
			}
			g_shadows.sites = found;
			return true;
		}

		// The game's character shadows left out (off) or drawn again; false and why when it can't be
		bool set_shadows(bool off, std::string &why)
		{
			if (off == g_shadows.off)
				return true;
			if (!find_shadows(why))
				return false;
			for (const shadow_site &s : g_shadows.sites)
				if (!code_is(s.at, off ? s.saved : s.ours, 6))
				{
					why = "something else has changed the game's shadow code since: left as it is";
					return false;
				}
			for (const shadow_site &s : g_shadows.sites)
				if (!write_code(s.at, off ? s.ours : s.saved, 6))
				{
					why = "couldn't write the game's code";
					return false;
				}
			g_shadows.off = off;
			return true;
		}

		// The visibility tests

		uint32_t call_target(uintptr_t site)
		{
			return rel_target(site);
		}

		void vis_off()
		{
			for (const vis_site &f : g_vis.sites)
				// (put back unless something else has pointed the call elsewhere since)
				if (f.entry != 0 && call_target(f.site) == f.entry)
					write_code(f.site + 1, f.saved, 4);
			if (!g_vis.sites.empty())
				worldfx_vis_setup(0, 0);
			g_vis.sites.clear();
		}

		bool vis_on(std::string &why)
		{
			if (!g_vis.sites.empty())
				return true;
			std::vector<vis_site> found;
			uint32_t test = 0;
			for (const vis_site_info &s : kVisSites)
			{
				const uintptr_t m = find(s.pattern);
				if (m == 0)
				{
					why = std::string("the ") + s.name + "'s call wasn't found";
					return false;
				}
				const uintptr_t site = m + s.at;
				const uint32_t t = call_target(site);
				if (test != 0 && t != test)
				{
					why = std::string("the ") + s.name + "'s call goes somewhere else (another addon hooks it)";
					return false;
				}
				test = t;
				vis_site f = { s.name, site, s.kind, {}, 0 };
				read(site + 1, f.saved, 4);
				found.push_back(f);
			}
			uint8_t head[sizeof(kVisTest)] = {};
			if (!read(test, head, sizeof(head)))
			{
				why = "the game's test couldn't be read";
				return false;
			}
			for (size_t i = 0; i < sizeof(kVisTest); ++i)
				if (kVisTest[i] != 0 && head[i] != kVisTest[i])
				{
					why = "the game's test isn't as expected (another addon hooks it?)";
					return false;
				}
			worldfx_vis_setup(test, 1);
			g_vis.sites = found;
			for (vis_site &f : g_vis.sites)
			{
				const uintptr_t entry = worldfx_vis_entry(f.kind);
				uint8_t rel[4];
				put32(rel, uint32_t(entry - (f.site + 5)));
				if (entry == 0 || !write_code(f.site + 1, rel, 4))
				{
					vis_off();
					why = std::string("the ") + f.name + "'s call couldn't be pointed at World FX";
					return false;
				}
				f.entry = entry;
			}
			return true;
		}

		// Every half second: the calls follow World FX's VisTest setting (tried once each time it's turned on)
		void update_vis()
		{
			const bool want = setting_on("VisTest", true);
			if (want && g_vis.sites.empty() && !g_vis.tried)
			{
				g_vis.tried = true;
				std::string why;
				g_vis.why = vis_on(why) ? std::string() : why;
			}
			else if (!want)
			{
				g_vis.tried = false;
				g_vis.why.clear();
				if (!g_vis.sites.empty())
					vis_off();
			}
		}

		std::string vis_status()
		{
			if (g_vis.sites.empty())
				return !g_vis.why.empty() ? "the game's own (not pointed at World FX: " + g_vis.why + ")"
					: std::string("the game's own (World FX's VisTest setting is off: the Frame rate tab)");
			uint32_t b[12] = {};
			worldfx_vis_stats(b);
			char line[512];
			char since[32];
			if (b[6] == 0xFFFFFFFFu)
				std::snprintf(since, sizeof(since), "none yet");
			else
				std::snprintf(since, sizeof(since), "%u ms ago", b[6]);
			std::snprintf(line, sizeof(line), "answered from World FX's depth copy %u times, by the game's own test %u; %u boxes tested on the GPU, %u answers back (last %s), %u objects known, %u dropped; asked by glows %u, fades %u, the flare %u, doors %u",
				b[0], b[1], b[2], b[3], since, b[4], b[5], b[7], b[8], b[9], b[10]);
			return line;
		}

		// The patches the settings ask for (each tried again only when its setting changes)
		void follow_settings()
		{
			const int flare = setting_on("HideFlare", true) ? 1 : 0;
			if (flare != g_wanted.flare)
			{
				g_wanted.flare = flare;
				g_flare.why.clear();
				std::string why;
				if (!set_flare(flare == 1, why))
					g_flare.why = why;
			}
			// The game's character shadows: out while World FX casts its own (checked every time: the sun sets, a zone
			// without open sky)
			{
				const bool want = setting_on("HideGameShadows", true) && worldfx_casts_characters() != 0;
				std::string shadows_why;
				if (want != g_shadows.off && !set_shadows(want, shadows_why))
					g_shadows.why = shadows_why;
				else if (want == g_shadows.off)
					g_shadows.why.clear();
			}
			// (why one couldn't be routed: in its own status, from its next update)
			std::string why;
			const int water = setting_on("HideGameWater", true) ? 1 : 0;
			if (water != g_wanted.water)
			{
				g_wanted.water = water;
				gamewater::route(water == 1, why);
			}
			const int rain = setting_on("HideGameRain", true) ? 1 : 0;
			if (rain != g_wanted.rain)
			{
				g_wanted.rain = rain;
				gamerain::route(rain == 1, why);
			}
		}

		std::string zone_name(uint32_t zone)
		{
			char text[64];
			if (zone == 0)
				return "not known yet";
			if (zone >= kMogHouseZone)
				std::snprintf(text, sizeof(text), "%u (a Mog House, room %u)", zone, zone - kMogHouseZone);
			else
				std::snprintf(text, sizeof(text), "%u", zone);
			return text;
		}
	}

	void frame(const entities &e, const party &p, double now)
	{
		publish_in_game(p.in_game);
		send_actors(e, p, now);
		send_lamps();
		glows::frame(e, now);
		send_weather();
		gamewater::count_frame();
		gamerain::count_frame();
		if (now < g_zone.next_check)
			return;
		g_zone.next_check = now + 0.5;
		find_lamps();
		glows::find_code();
		find_weather();
		// (0 while zoning: the last zone stays, so World FX doesn't load its files again for nothing)
		uint32_t zone = p.zone;
		if (zone != 0 && g_zone.mog_house != 0)
			zone = kMogHouseZone + (g_zone.mog_house - 1);
		if (zone != 0 && zone != g_zone.last)
		{
			g_zone.last = zone;
			publish_zone(zone);
			glows::zone_changed(); // (the last zone's entities: looked for again)
		}
		follow_settings();
		gamewater::update(g_zone.last);
		gamerain::update(g_zone.last);
		update_vis();
	}

	void packet_in(uint16_t id, const uint8_t *data, uint32_t size, const entities &e)
	{
		if (data == nullptr)
			return;
		if (id == 0x028)
		{
			cut_for(data, size, e);
			return;
		}
		if (id != 0x00A || size < 0xAC)
			return;
		g_zone.mog_house = data[0x80] == 1 ? uint32_t(data[0xAA] | (data[0xAB] << 8)) + 1 : 0;
	}

	void stop()
	{
		publish_zone(0);
		SetEnvironmentVariableA("FFXI_WORLDFX_INGAME", nullptr);
		g_zone.in_game = -1;
		std::string why;
		if (g_flare.off)
			set_flare(false, why);
		if (g_shadows.off)
			set_shadows(false, why);
		vis_off();
		gamewater::stop(why);
		gamerain::stop(why);
		glows::stop();
		// (all forgotten: started again, everything is looked for afresh)
		g_actors = actors_state();
		g_cuts = cuts_state();
		g_lamps = lamps_state();
		g_weather = weather_state();
		g_flare = flare_state();
		g_shadows = shadows_state();
		g_vis = vis_state();
		g_zone = zone_state();
		g_wanted = wanted();
	}

	uint32_t zone()
	{
		return g_zone.last;
	}

	std::vector<std::string> status()
	{
		std::vector<std::string> out;
		char line[512];
		out.push_back("zone " + zone_name(g_zone.last) + (g_zone.in_game == 1 ? ", a character in" : ", no character in"));
		{
			std::string s;
			if (!g_actors.me_known && g_actors.picked.empty() && g_actors.sent == 0)
				std::snprintf(line, sizeof(line), "characters near: %u sent (you weren't found: %s)", g_actors.sent, g_actors.me_from);
			else
				std::snprintf(line, sizeof(line), "characters near: %u sent (%u fighting); you found by %s", g_actors.sent, g_actors.fighting, g_actors.me_from);
			s = line;
			if (g_cuts.any)
			{
				std::snprintf(line, sizeof(line), "; weapon skills have cut the grass %u times (the last skill %u)", g_cuts.sent, g_cuts.last);
				s += line;
			}
			out.push_back(s);
		}
		if (g_lamps.table == 0)
			out.push_back(std::string("lamps: not read: ") + (g_lamps.why != nullptr ? g_lamps.why : "-"));
		else
		{
			std::snprintf(line, sizeof(line), "lamps: %u lights in the game's table, %u lit", g_lamps.listed, g_lamps.lit);
			out.push_back(line);
		}
		out.push_back("glows: " + glows::status());
		out.push_back("weather: " + weather_status());
		out.push_back(std::string("lens flare: ") + (!g_flare.why.empty() ? "shown (" + g_flare.why + ")" : g_flare.off ? "hidden" : "shown"));
		out.push_back(std::string("the game's character shadows: ") + (!g_shadows.why.empty() ? "drawn (" + g_shadows.why + ")"
			: g_shadows.off ? "left out (HiFi XI casts characters' shadows)" : "drawn (HiFi XI isn't casting characters' shadows here now)"));
		out.push_back("the game's water: " + gamewater::status());
		out.push_back("the game's rain and snow: " + gamerain::status());
		out.push_back("visibility tests (glows, lamps, the flare, doors): " + vis_status());
		return out;
	}
}
