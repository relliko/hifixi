/*
 * The zone's objects handed to the native zone renderer (zonedraw.hpp; research/ffxi-re/notes-zone.md "The live zone",
 * "Closing the open items"; notes-replace-slots.md).
 *
 * 17C9C0 (thiscall on the zone renderer: the light table, the object, the moving-scenery flag) has tested the object
 * and chosen its model by 17CBA8 (ebx the model, ecx the object, ebp the renderer; its arguments at esp+1D8, 1DC,
 * 1E0); the je at 17CBA2 goes to its exit 17DDFF (pop edi/esi/ebp/ebx, add esp 1C4, ret 0Ch) at the same depth. The
 * six bytes there (mov esi, [ebp+39448h]) become a jmp to our stub:
 *   pushad; push [esp+1F8h] (light table); push [esp+204h] (scenery flag); push ebx; push ecx; push ebp
 *   call on_object; add esp, 14h; test eax, eax; popad
 *   jnz exit
 *   mov esi, [ebp+39448h]; jmp 17CBAE                 (the game draws it)
 *   exit: jmp 17DDFF                                 (HiFi XI draws it)
 *
 * The doors and elevators (183DA0, thiscall on the renderer: the light table, the part) have passed their distance and
 * frustum tests by 183E47 (esi the renderer, ebp the part, its near model at esp+10h, the light table at esp+2Ch); the
 * jne at 183E41 goes to its exit 1844A9. The six bytes there (mov eax, [ebp+0E4h]) become a jmp to a second stub:
 *   pushad; push [esp+4Ch] (light table); push [esp+34h] (model); push ebp; push esi
 *   call on_door; add esp, 10h; test eax, eax; popad
 *   jnz exit
 *   mov eax, [ebp+0E4h]; jmp 183E4D                   (the game draws it)
 *   exit: jmp 1844A9                                 (HiFi XI draws it)
 *
 * What's recorded (the object's own fields): the world matrix +0x40 (row vectors), the model's MMB name +8, +0xD4
 * mirrored, +0xD6 the name's first character's kind (1 '_' alpha test, 2 '#' clamp), +0xE4..+0xF0 the lamps (light
 * table entries, the D3DLIGHT8 at [table - 0x48 + 0x4C i] + 8, used unless null, unlit or named 'c...', as 17CBC2 has
 * it), +0xE0 its environment. Kind 2 (swaying plants): the game's sway clock 17A3F0 run for it (as 17DA7E would have)
 * when the model is animated (+0x68), and the keyframe it would draw, trunc((N - 1) x weight +0x15C) / (N - 1), N - 1
 * the model's +0x6C. The pass's light, from the renderer as 17B510 left it: VIEW x PROJECTION +0x38F5C, the camera
 * +0x390F0, the sun's D3DLIGHT8 +0x39180 (diffuse +4, direction +0x40), AMBIENT +0x39250, the bump shader's c6
 * +0x394D0 and c7 +0x394E0, FOGCOLOR +0x39254, FOGEND +0x39258, FOGSTART +0x3925C, the fog scale +0x3954C; the zone id
 * +0x3C.
 *
 * An area environment (object +0xE0 not 0) is worked out as 17D41F-17D4F6 and 17D5C1-17D5F7 do, once a pass for each:
 * 189120 its fog (colour, end, start), 188CA0 its sun (a D3DLIGHT8: fixed-function pieces' diffuse as it is, the bump
 * shader's c6 after the brightness option 00BE50 on the device wrapper), 188FA0 c7 and 188E00 AMBIENT, each thiscall on
 * the environment. A door's bit-28 pieces (18409C-1841BA): renderer +0x50's environment's sun, ambient and fog colour,
 * with the zone's own fog start and end. Objects the frame has no room for, or whose game calls weren't found, are
 * left to the game.
 *
 * The renderer's second map slot (+0x60 + 0x78: type +0xD8, map +0xDC, objects +0x100, their count +0xFC, moving
 * records +0x114 and their count +0x118): its objects and doors are drawn from the second map's data.
 */

#include "zonedraw.hpp"
#include "../zone/frame.hpp"
#include "../zone/zone_gpu.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace hifi::game::zonedraw
{
	namespace
	{
		namespace zm = bake::zonemesh;

		constexpr const char *kSite = "8BB5489403008B95009103008BBC24D801000046"; // 17CBA8: mov esi, [ebp+39448h]; mov edx, [ebp+39100h]; mov edi, [esp+1D8h]; inc esi
		constexpr size_t kStubSize = 0x40;
		// 183E2A: lea eax, [ebp+0F4h]; lea ecx, [esi+38FDCh]; push eax; push ecx; mov ecx, esi; call 181BB0; test eax, eax;
		// jne 1844A9; mov eax, [ebp+0E4h] (the site, +0x1D)
		constexpr const char *kDoorSite = "8D85F40000008D8EDC8F030050518BCEE8????????85C00F85????????8B85E4000000";
		constexpr intptr_t kDoorSiteAt = 0x1D, kDoorJneAt = 0x17;
		constexpr size_t kDoorStubSize = 0x30;
		// The game's calls, each found at the call in 17C9C0 that makes it
		constexpr const char *kFogCall = "8D853C9303008D9D389303006A008DB534930300505356E8";         // 17D41F, call 189120 at +0x17
		constexpr const char *kLightCall = "8DB56092030085FF0F84????????6A0056E8";                    // 17D458, call 188CA0 at +0x11
		constexpr const char *kC7Call = "8B8D38910300508995D8940300E8";                              // 17D4E9, call 188FA0 at +0xD
		constexpr const char *kAmbientCall = "8B8D389103008DB5309303006A0056E8";                     // 17D5E8, call 188E00 at +0xF
		constexpr const char *kBrightCall = "B91A0000008D7C242CF3A58B0D????????8D44242C50E8";         // 17D46E: the wrapper's global at +0xD, call 00BE50 at +0x16
		constexpr const char *kSwayCall = "8B436885C07408568BCDE8";                                  // 17DA74, call 17A3F0 at +0xA

		using fog_fn = void(__fastcall *)(uintptr_t env, uintptr_t, uint32_t *colour, float *end, float *start, int kind);
		using light_fn = void(__fastcall *)(uintptr_t env, uintptr_t, void *light, int kind);
		using c7_fn = void(__fastcall *)(uintptr_t env, uintptr_t, float *out);
		using ambient_fn = void(__fastcall *)(uintptr_t env, uintptr_t, uint32_t *out, int kind);
		using bright_fn = uint32_t(__fastcall *)(uintptr_t wrapper, uintptr_t, void *light);
		using sway_fn = void(__fastcall *)(uintptr_t renderer, uintptr_t, uintptr_t object);

		struct state
		{
			uintptr_t site = 0, stub = 0, exit = 0;
			uint8_t saved[6] = {}, ours[6] = {};
			uintptr_t door_site = 0, door_stub = 0, door_exit = 0;
			uint8_t door_saved[6] = {}, door_ours[6] = {};
			bool in = false;
			game_calls calls;
			bool calls_set = false; // (the tests' own)
			// Set each present
			bool wanted = false, d3d12_ready = false;
			zone::gpu_zone *zones[2] = {};
			// The pass under way
			bool native = false;
			uint32_t seq = 0;
			uintptr_t renderer = 0;
			zone::frame *f = nullptr;
			std::unordered_map<uintptr_t, uint32_t> lamp_of; // (light entry -> the frame's lamp)
			std::unordered_map<uint64_t, uint32_t> env_of;   // (environment | door << 32 -> the frame's environment)
			// Model pointer -> index in a map's data (kNone: not there), for the data it was made for
			zone::gpu_zone *models_for[2] = {};
			std::unordered_map<uintptr_t, uint32_t> model_of[2];
			counts n;
		};
		state g;
		// The game's own count of the zone objects drawn in its last pass (renderer +0x39448, after 17B510: ours counted
		// in it too), native drawing on or off: the same at a spot either way when the same objects are picked
		uint32_t g_game_drawn = 0;
		uint64_t g_game_passes = 0;

		template <typename T>
		T at(uintptr_t a)
		{
			T v;
			std::memcpy(&v, reinterpret_cast<const void *>(a), sizeof(T));
			return v;
		}

		void put_rel(uint8_t *b, size_t at_, uintptr_t from_next, uintptr_t to)
		{
			put32(b + at_, uint32_t(to - from_next));
		}

		// The target of the call at a pattern's offset (0 when the pattern isn't there or that isn't a call)
		uintptr_t call_at(const char *pattern, intptr_t offset)
		{
			const uintptr_t a = find(pattern, offset);
			uint8_t op = 0;
			return a != 0 && read(a, op) && op == 0xE8 ? rel_target(a) : 0;
		}

		game_calls find_calls()
		{
			game_calls c;
			c.fog = call_at(kFogCall, 0x17);
			c.light = call_at(kLightCall, 0x11);
			c.c7 = call_at(kC7Call, 0xD);
			c.ambient = call_at(kAmbientCall, 0xF);
			c.bright = call_at(kBrightCall, 0x16);
			if (const uintptr_t b = find(kBrightCall); b != 0)
				c.wrapper_at = word(b + 0xD);
			c.sway = call_at(kSwayCall, 0xA);
			return c;
		}

		bool envs_known()
		{
			return g.calls.fog != 0 && g.calls.light != 0 && g.calls.c7 != 0 && g.calls.ambient != 0 && g.calls.bright != 0 && g.calls.wrapper_at != 0;
		}

		// Which of the renderer's map slots an object is in: 1 when it's one of the second slot's objects, else 0
		uint32_t slot_of_object(uintptr_t renderer, uintptr_t object)
		{
			const uintptr_t s = renderer + 0x60 + 0x78;
			if (at<uint32_t>(s) == 0)
				return 0;
			const uintptr_t objects = at<uint32_t>(s + 0x28);
			const uint32_t count = at<uint32_t>(s + 0x24);
			return objects != 0 && object >= objects && object < objects + uintptr_t(count) * 0x16C ? 1 : 0;
		}

		// Which slot a door part is in: 1 when one of the second slot's moving records holds it, else 0
		uint32_t slot_of_part(uintptr_t renderer, uintptr_t part)
		{
			const uintptr_t s = renderer + 0x60 + 0x78;
			if (at<uint32_t>(s) == 0)
				return 0;
			const uintptr_t records = at<uint32_t>(s + 0x3C);
			const int32_t count = at<int32_t>(s + 0x40);
			for (int32_t k = 0; records != 0 && k < count && k < 4096; ++k)
				for (int p = 0; p < 4; ++p)
					if (at<uint32_t>(records + uintptr_t(k) * 0x1AC + 0x198 + 4 * p) == part)
						return 1;
			return 0;
		}

		uint32_t model_index(uint32_t slot, uintptr_t model)
		{
			zone::gpu_zone *const z = g.zones[slot];
			if (g.models_for[slot] != z)
			{
				g.model_of[slot].clear();
				g.models_for[slot] = z;
			}
			const auto known = g.model_of[slot].find(model);
			if (known != g.model_of[slot].end())
				return known->second;
			char name[16];
			std::memcpy(name, reinterpret_cast<const void *>(model + 8), 16);
			const uint32_t i = z->data().find(name);
			g.model_of[slot].emplace(model, i);
			return i;
		}

		float colour(uint32_t c, int shift)
		{
			return float((c >> shift) & 0xFF) / 255.0f;
		}

		void put_colour(float out[3], uint32_t c)
		{
			out[0] = colour(c, 16);
			out[1] = colour(c, 8);
			out[2] = colour(c, 0);
		}

		// Toward the light, from a D3DLIGHT8's direction
		void toward(float out[3], const float dir[3])
		{
			const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
			for (int k = 0; k < 3; ++k)
				out[k] = l > 0.0f ? -dir[k] / l : 0.0f;
		}

		float fog_scale(uintptr_t r)
		{
			const float s = at<float>(r + 0x3954C);
			return std::isfinite(s) && s > 0.0f ? s : 1.0f;
		}

		// The pass's light and camera, as 17B510 left them in the renderer
		void snapshot(zone::frame &f, uintptr_t r)
		{
			std::memcpy(f.view_proj, reinterpret_cast<const void *>(r + 0x38F5C), 64);
			std::memcpy(f.camera, reinterpret_cast<const void *>(r + 0x390F0), 12);
			float dir[3];
			std::memcpy(dir, reinterpret_cast<const void *>(r + 0x391C0), 12);
			toward(f.sun_dir, dir);
			std::memcpy(f.sun, reinterpret_cast<const void *>(r + 0x39184), 12);
			std::memcpy(f.sun_bump, reinterpret_cast<const void *>(r + 0x394D0), 12);
			std::memcpy(f.ambient_bump, reinterpret_cast<const void *>(r + 0x394E0), 12);
			put_colour(f.ambient, at<uint32_t>(r + 0x39250));
			put_colour(f.fog, at<uint32_t>(r + 0x39254));
			f.fog_end = at<float>(r + 0x39258);
			f.fog_start = at<float>(r + 0x3925C);
			f.fog_scale = fog_scale(r);
			// (no shader path: the bump constants are the fixed path's)
			if (f.sun_bump[0] == 0.0f && f.sun_bump[1] == 0.0f && f.sun_bump[2] == 0.0f)
			{
				std::memcpy(f.sun_bump, f.sun, 12);
				std::memcpy(f.ambient_bump, f.ambient, 12);
			}
		}

		// An environment's index in the pass's frame (0 the zone's own), worked out at its first use; kNone when there's
		// no room for it or its calls weren't found. door: a door's bit-28 pieces' (the zone's own fog start and end).
		uint32_t environment(uintptr_t env, bool door)
		{
			if (env == 0)
				return 0;
			const uint64_t key = uint64_t(env) | (door ? 1ull << 32 : 0);
			const auto known = g.env_of.find(key);
			if (known != g.env_of.end())
				return known->second;
			zone::frame &f = *g.f;
			if (!envs_known() || f.env_count >= zone::kMaxEnvs)
				return zm::kNone;
			zone::frame_env &e = f.envs[f.env_count];
			e = zone::frame_env();
			// (a D3DLIGHT8 and room past it)
			alignas(16) uint8_t light[0x80] = {}, bright[0x80] = {};
			reinterpret_cast<light_fn>(g.calls.light)(env, 0, light, 0);
			float dir[3];
			std::memcpy(dir, light + 0x40, 12);
			toward(e.sun_dir, dir);
			std::memcpy(e.sun, light + 4, 12);
			std::memcpy(bright, light, 0x68);
			reinterpret_cast<bright_fn>(g.calls.bright)(at<uint32_t>(g.calls.wrapper_at), 0, bright);
			std::memcpy(e.sun_bump, bright + 4, 12);
			float c7[4] = {};
			reinterpret_cast<c7_fn>(g.calls.c7)(env, 0, c7);
			std::memcpy(e.ambient_bump, c7, 12);
			uint32_t ambient = 0;
			reinterpret_cast<ambient_fn>(g.calls.ambient)(env, 0, &ambient, 0);
			put_colour(e.ambient, ambient);
			uint32_t fog = 0;
			float end = 0.0f, start = 0.0f;
			reinterpret_cast<fog_fn>(g.calls.fog)(env, 0, &fog, &end, &start, 0);
			put_colour(e.fog, fog);
			e.fog_start = door ? at<float>(g.renderer + 0x3925C) : start;
			e.fog_end = door ? at<float>(g.renderer + 0x39258) : end;
			e.fog_scale = fog_scale(g.renderer);
			g.env_of.emplace(key, f.env_count);
			g.n.envs++;
			return f.env_count++;
		}

		// A lamp of the object's (a light table index; 0 none): the frame's lamp, kNone when it isn't lit
		uint32_t lamp(uintptr_t table, uint32_t index)
		{
			if (index == 0 || table == 0)
				return zm::kNone;
			const uintptr_t entry = at<uint32_t>(table - 0x48 + uintptr_t(index) * 0x4C);
			if (entry == 0 || at<uint32_t>(entry + 8) == 0 || at<uint8_t>(entry) == 'c')
				return zm::kNone;
			const auto known = g.lamp_of.find(entry);
			if (known != g.lamp_of.end())
				return known->second;
			zone::frame &f = *g.f;
			if (f.lamp_count >= zone::kMaxLamps)
				return zm::kNone;
			// (the D3DLIGHT8 at +8: diffuse +4, position +0x34, range +0x4C, attenuation 2 +0x5C)
			const uintptr_t light = entry + 8;
			zone::frame_lamp &l = f.lamps[f.lamp_count];
			std::memcpy(l.colour, reinterpret_cast<const void *>(light + 4), 12);
			std::memcpy(l.pos, reinterpret_cast<const void *>(light + 0x34), 12);
			l.range = at<float>(light + 0x4C);
			l.attenuation2 = at<float>(light + 0x5C);
			if (!(l.attenuation2 > 0.0f))
				l.attenuation2 = 0.5f;
			g.lamp_of.emplace(entry, f.lamp_count);
			return f.lamp_count++;
		}

		// An object or door part into the pass's frame: 1 when it's recorded (the game leaves it), 0 when the game
		// draws it
		int record(uintptr_t renderer, uintptr_t object, uintptr_t model, uint32_t slot, uintptr_t light_table, bool door)
		{
			zone::gpu_zone *const z = g.zones[slot];
			if (model == 0 || z == nullptr)
			{
				g.n.left++;
				return 0;
			}
			const uint32_t index = model_index(slot, model);
			zone::frame &f = *g.f;
			if (index == zm::kNone || f.count >= zone::kMaxObjects)
			{
				g.n.left++;
				return 0;
			}
			// Its environment (and a door's bit-28 pieces': renderer +0x50's, when there is one)
			const uint32_t env = environment(at<uint32_t>(object + 0xE0), false);
			const uintptr_t ev10 = door ? at<uint32_t>(renderer + 0x50) : 0;
			const uint32_t door_env = ev10 != 0 ? environment(ev10, true) : env;
			// Kind 2's sway clock (the game's own, run as it would have been)
			const bool swaying = at<uint32_t>(model + 0x1C) == 2 && at<uint32_t>(model + 0x68) != 0;
			if (env == zm::kNone || door_env == zm::kNone || (swaying && g.calls.sway == 0))
			{
				g.n.left++;
				return 0;
			}
			float sway = 0.0f;
			if (swaying)
			{
				reinterpret_cast<sway_fn>(g.calls.sway)(renderer, 0, object);
				const float frames = at<float>(model + 0x6C), weight = at<float>(object + 0x15C);
				if (std::isfinite(frames) && frames > 0.0f && std::isfinite(weight))
					sway = std::trunc(frames * weight) / frames;
			}
			zone::frame_object &o = f.objects[f.count++];
			float m[16];
			std::memcpy(m, reinterpret_cast<const void *>(object + 0x40), 64);
			for (int k = 0; k < 3; ++k)
			{
				o.world[k * 4 + 0] = m[0 * 4 + k];
				o.world[k * 4 + 1] = m[1 * 4 + k];
				o.world[k * 4 + 2] = m[2 * 4 + k];
				o.world[k * 4 + 3] = m[3 * 4 + k];
			}
			o.model = index;
			const uint8_t name_kind = at<uint8_t>(object + 0xD6);
			o.flags = (name_kind == 1 ? zm::kAlphaTested : 0) | (name_kind == 2 ? zm::kClamped : 0) | (at<uint8_t>(object + 0xD4) != 0 ? zm::kMirrored : 0) |
				(door ? zone::kDoor : 0);
			for (int k = 0; k < 4; ++k)
				o.lamps[k] = lamp(light_table, at<uint32_t>(object + 0xE4 + 4 * k));
			o.sway = sway;
			o.env = env;
			o.door_env = door_env;
			o.zone = slot;
			if (slot == 1)
				g.n.recorded_second++;
			return 1;
		}
	}

	int __cdecl on_object(uintptr_t renderer, uintptr_t object, uintptr_t model, uint32_t ship, uintptr_t light_table)
	{
		if (!g.native || g.f == nullptr || ship != 0 || renderer != g.renderer)
			return 0;
		if (record(renderer, object, model, slot_of_object(renderer, object), light_table, false) == 0)
			return 0;
		// (counted as drawn, as the game would have)
		*reinterpret_cast<uint32_t *>(renderer + 0x39448) += 1;
		g.n.recorded++;
		return 1;
	}

	int __cdecl on_door(uintptr_t renderer, uintptr_t part, uintptr_t model, uintptr_t light_table)
	{
		if (!g.native || g.f == nullptr || renderer != g.renderer)
			return 0;
		if (record(renderer, part, model, slot_of_part(renderer, part), light_table, true) == 0)
			return 0;
		g.n.doors++;
		return 1;
	}

	void on_slot(uint32_t kind, uint32_t seq, uintptr_t renderer)
	{
		if (kind == 1 && renderer != 0)
		{
			g_game_drawn = at<uint32_t>(renderer + 0x39448);
			g_game_passes++;
		}
		if (kind == 0)
		{
			g.n.passes++;
			g.seq = seq;
			g.renderer = renderer;
			g.native = false;
			g.f = nullptr;
			const char *why = nullptr;
			zone::gpu_zone *const z = g.zones[0];
			if (!g.in)
				why = "not hooked";
			else if (!g.wanted)
				why = "off";
			else if (z == nullptr || !z->ready())
				why = "the zone's data isn't on the GPU yet";
			else if (z->data().map != at<uint32_t>(renderer + 0x3C))
				why = "the zone's data is another zone's";
			else if (!g.d3d12_ready)
				why = "the Direct3D 12 side isn't ready (markers, renderer or targets)";
			g.n.gate = why != nullptr ? why : "";
			if (why != nullptr)
				return;
			g.f = &zone::frame_slot(seq);
			g.f->native = true;
			g.f->no_bump = false;
			g.f->zone = z;
			g.f->map = z->data().map;
			// (the second map's data, when it's that slot's map and on the GPU: else its objects stay the game's)
			zone::gpu_zone *const second = g.zones[1];
			const bool second_ok = at<uint32_t>(renderer + 0xD8) != 0 && second != nullptr && second->ready() && second->data().map == at<uint32_t>(renderer + 0xDC);
			g.f->second = second_ok ? second : nullptr;
			g.zones[1] = g.f->second;
			g.f->count = 0;
			g.f->lamp_count = 0;
			g.f->env_count = 1;
			g.lamp_of.clear();
			g.env_of.clear();
			g.native = true;
			g.n.native_passes++;
			g.n.last_left = uint32_t(g.n.left);
		}
		else if (kind == 1 && g.native && g.f != nullptr && seq == g.seq)
		{
			snapshot(*g.f, g.renderer);
			g.n.last_recorded = g.f->count;
			g.n.last_left = uint32_t(g.n.left) - g.n.last_left;
			g.n.last_envs = g.f->env_count - 1;
			// (by distance from the camera: what's drawn far off, for comparing with the game's own)
			for (uint32_t &b : g.n.last_bands)
				b = 0;
			for (uint32_t i = 0; i < g.f->count; ++i)
			{
				const zone::frame_object &o = g.f->objects[i];
				const float dx = o.world[3] - g.f->camera[0], dy = o.world[7] - g.f->camera[1], dz = o.world[11] - g.f->camera[2];
				const float d2 = dx * dx + dy * dy + dz * dz;
				g.n.last_bands[d2 < 100.0f * 100.0f ? 0 : d2 < 300.0f * 300.0f ? 1 : 2]++;
			}
			zone::publish(*g.f, seq);
			g.native = false;
			g.f = nullptr;
		}
	}

	bool start(std::string &why)
	{
		if (g.in)
			return true;
		const uintptr_t site = find(kSite);
		if (site == 0)
		{
			why = "the game's zone object draw wasn't found";
			return false;
		}
		// (the je just before: 0F 84 rel32 to the function's exit)
		uint8_t je[6] = {};
		if (!read(site - 6, je, 6) || je[0] != 0x0F || je[1] != 0x84)
		{
			why = "the zone object draw isn't laid out as expected";
			return false;
		}
		int32_t rel = 0;
		std::memcpy(&rel, je + 2, 4);
		const uintptr_t exit = site + intptr_t(rel);
		uint8_t saved[6];
		if (!read(site, saved, 6))
		{
			why = "the game's code couldn't be read";
			return false;
		}
		// The doors' site (optional: without it the game draws the doors), its jne's exit
		uintptr_t door_site = 0, door_exit = 0;
		uint8_t door_saved[6] = {};
		if (const uintptr_t d = find(kDoorSite); d != 0)
		{
			int32_t drel = 0;
			if (read(d + kDoorJneAt + 2, drel) && read(d + kDoorSiteAt, door_saved, 6))
			{
				door_site = d + kDoorSiteAt;
				door_exit = door_site + intptr_t(drel);
			}
		}
		if (!g.calls_set)
			g.calls = find_calls();
		if (g.stub == 0)
		{
			g.stub = alloc_code(kStubSize);
			if (g.stub == 0)
			{
				why = "no memory for the stub";
				return false;
			}
		}
		const uintptr_t s = g.stub;
		uint8_t b[kStubSize] = {
			0x60,                                     // 00 pushad
			0xFF, 0xB4, 0x24, 0xF8, 0x01, 0x00, 0x00, // 01 push [esp+1F8h]   the light table
			0xFF, 0xB4, 0x24, 0x04, 0x02, 0x00, 0x00, // 08 push [esp+204h]   the scenery flag
			0x53, 0x51, 0x55,                         // 0F push ebx; push ecx; push ebp
			0xE8, 0, 0, 0, 0,                         // 12 call on_object
			0x83, 0xC4, 0x14,                         // 17 add esp, 14h
			0x85, 0xC0,                               // 1A test eax, eax
			0x61,                                     // 1C popad
			0x0F, 0x85, 0, 0, 0, 0,                   // 1D jnz +2E
			0x8B, 0xB5, 0x48, 0x94, 0x03, 0x00,       // 23 mov esi, [ebp+39448h]
			0xE9, 0, 0, 0, 0,                         // 29 jmp site + 6
			0xE9, 0, 0, 0, 0,                         // 2E jmp exit
			0xCC };
		put_rel(b, 0x13, s + 0x17, reinterpret_cast<uintptr_t>(&on_object));
		put_rel(b, 0x1F, s + 0x23, s + 0x2E);
		put_rel(b, 0x2A, s + 0x2E, site + 6);
		put_rel(b, 0x2F, s + 0x33, exit);
		if (!write_code(s, b, kStubSize))
		{
			why = "couldn't write the stub";
			return false;
		}
		if (door_site != 0 && g.door_stub == 0)
			g.door_stub = alloc_code(kDoorStubSize);
		if (door_site != 0 && g.door_stub != 0)
		{
			const uintptr_t ds = g.door_stub;
			uint8_t d[kDoorStubSize] = {
				0x60,                               // 00 pushad
				0xFF, 0x74, 0x24, 0x4C,             // 01 push [esp+4Ch]   the light table
				0xFF, 0x74, 0x24, 0x34,             // 05 push [esp+34h]   the model
				0x55, 0x56,                         // 09 push ebp; push esi
				0xE8, 0, 0, 0, 0,                   // 0B call on_door
				0x83, 0xC4, 0x10,                   // 10 add esp, 10h
				0x85, 0xC0,                         // 13 test eax, eax
				0x61,                               // 15 popad
				0x0F, 0x85, 0, 0, 0, 0,             // 16 jnz +27
				0x8B, 0x85, 0xE4, 0x00, 0x00, 0x00, // 1C mov eax, [ebp+0E4h]
				0xE9, 0, 0, 0, 0,                   // 22 jmp door site + 6
				0xE9, 0, 0, 0, 0,                   // 27 jmp exit
				0xCC };
			put_rel(d, 0x0C, ds + 0x10, reinterpret_cast<uintptr_t>(&on_door));
			put_rel(d, 0x18, ds + 0x1C, ds + 0x27);
			put_rel(d, 0x23, ds + 0x27, door_site + 6);
			put_rel(d, 0x28, ds + 0x2C, door_exit);
			if (!write_code(ds, d, kDoorStubSize))
				door_site = 0;
		}
		else
			door_site = 0;
		uint8_t ours[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
		put_rel(ours, 1, site + 5, s);
		if (!write_code(site, ours, 6))
		{
			why = "couldn't write the game's code";
			return false;
		}
		g.site = site;
		g.exit = exit;
		std::memcpy(g.saved, saved, 6);
		std::memcpy(g.ours, ours, 6);
		if (door_site != 0)
		{
			uint8_t door_ours[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
			put_rel(door_ours, 1, door_site + 5, g.door_stub);
			if (write_code(door_site, door_ours, 6))
			{
				g.door_site = door_site;
				g.door_exit = door_exit;
				std::memcpy(g.door_saved, door_saved, 6);
				std::memcpy(g.door_ours, door_ours, 6);
			}
		}
		g.in = true;
		return true;
	}

	bool stop(std::string &why)
	{
		if (!g.in)
			return true;
		if (!code_is(g.site, g.ours, 6) || (g.door_site != 0 && !code_is(g.door_site, g.door_ours, 6)))
		{
			why = "something else has changed the zone object or door draw since: left as it is";
			return false;
		}
		if (!write_code(g.site, g.saved, 6) || (g.door_site != 0 && !write_code(g.door_site, g.door_saved, 6)))
		{
			why = "couldn't write the game's code";
			return false;
		}
		free_code(g.stub);
		if (g.door_stub != 0)
			free_code(g.door_stub);
		const game_calls kept = g.calls;
		const bool kept_set = g.calls_set;
		g = state();
		if (kept_set)
		{
			g.calls = kept;
			g.calls_set = true;
		}
		zone::clear_frames();
		return true;
	}

	bool in()
	{
		return g.in;
	}

	void set_state(bool wanted, zone::gpu_zone *zone, zone::gpu_zone *second, bool d3d12_ready)
	{
		g.wanted = wanted;
		g.zones[0] = zone;
		// (the second map's: taken at the next pass's begin, while a pass is under way the one it began with stays)
		if (!g.native)
			g.zones[1] = second;
		g.d3d12_ready = d3d12_ready;
	}

	uint32_t second_map(uintptr_t renderer)
	{
		if (renderer == 0)
			renderer = g.renderer;
		return renderer != 0 && at<uint32_t>(renderer + 0xD8) != 0 ? at<uint32_t>(renderer + 0xDC) : 0;
	}

	counts get_counts()
	{
		return g.n;
	}

	std::string status()
	{
		char game[96];
		std::snprintf(game, sizeof(game), "; the game's count for its last zone pass: %u objects drawn", g_game_drawn);
		if (!g.in)
			return std::string("native zone drawing: off (/hifi nativezone on)") + (g_game_passes != 0 ? game : "");
		char line[480];
		if (!g.n.gate.empty())
			std::snprintf(line, sizeof(line), "native zone drawing: the game draws the zone (%s); %llu of %llu passes HiFi XI's", g.n.gate.c_str(),
				static_cast<unsigned long long>(g.n.native_passes), static_cast<unsigned long long>(g.n.passes));
		else
			std::snprintf(line, sizeof(line), "native zone drawing: HiFi XI draws the zone: last pass %u objects (%u left to the game: models it hasn't), %u area lights; %llu of %llu passes; %llu doors%s%s",
				g.n.last_recorded, g.n.last_left, g.n.last_envs, static_cast<unsigned long long>(g.n.native_passes), static_cast<unsigned long long>(g.n.passes),
				static_cast<unsigned long long>(g.n.doors), g.door_site == 0 ? " (the doors' site not found: the game's)" : "",
				envs_known() ? "" : " (area lights' calls not found: those objects the game's)");
		std::string out = line;
		if (g.n.gate.empty())
		{
			char bands[128];
			std::snprintf(bands, sizeof(bands), "; by distance: %u under 100 yalms, %u to 300, %u past", g.n.last_bands[0], g.n.last_bands[1], g.n.last_bands[2]);
			out += bands;
		}
		return out + (g_game_passes != 0 ? game : "");
	}

	sites where()
	{
		return { g.site, g.stub, g.exit, g.door_site, g.door_stub, g.door_exit };
	}

	game_calls calls()
	{
		return g.calls;
	}

	void set_calls(const game_calls &c)
	{
		g.calls = c;
		g.calls_set = true;
	}
}
