/*
 * The model actors handed to the zone renderer (modelactor.hpp; research/ffxi-re/notes-world-passes.md 1).
 *
 * 0C4CBA: mov eax, [esi+770h]; mov ecx, [zone renderer]; push eax; push ebp; call 1844C0 (at +0xE). The call's target
 * becomes on_draw (a __fastcall with the same stack: 1844C0 is a thiscall taking two arguments off, ret 8), which calls
 * 1844C0 itself for whatever it leaves to the game.
 *
 * 1844C0's start (1844DE): mov eax, [Mog House object]; ...; push 'l_01'; call 178760 (at +0x16), whose own start
 * (mov edx, [esp+4]; mov eax, imm32) names the light table: 256 entries of 0x70 bytes, the name at +0, a D3DLIGHT8 at +8.
 */

#include "modelactor.hpp"
#include "../zone/frame.hpp"
#include "../zone/zone_gpu.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hifi::game::modelactor
{
	namespace
	{
		namespace zm = bake::zonemesh;

		constexpr const char *kSite = "8B86700700008B0D????????5055E8"; // 0C4CBA, the call at +0xE
		constexpr intptr_t kCallAt = 0xE;
		constexpr const char *kMogHouse = "A1????????C74424200000000085C074??686C5F3031E8"; // 1844DE: the object at +1, call 178760 at +0x16
		constexpr intptr_t kMogHouseAt = 1, kFindLightAt = 0x16;
		constexpr uint32_t kLampName = 0x31305F6C; // 'l_01'
		constexpr uint32_t kLightEntries = 256, kLightEntryBytes = 0x70;
		constexpr uint32_t kMaxFile = 0x40000;

		using draw_fn = void(__fastcall *)(uintptr_t renderer, uintptr_t, uintptr_t block, uint32_t alpha);

		struct state
		{
			uintptr_t call = 0, original = 0;
			uint8_t saved[5] = {}, ours[5] = {};
			uintptr_t mog_house = 0, lights = 0;
			bool in = false;
			bool native = false;
			zone::gpu_zone *(*find)(uint32_t) = nullptr;
			// The walk under way, and its frame (nullptr: none)
			bool in_walk = false;
			zone::actor_frame *f = nullptr;
			bool lit = false; // (its camera and light taken)
			counts n;
		};
		state g;

		template <typename T>
		T at(uintptr_t a)
		{
			T v;
			std::memcpy(&v, reinterpret_cast<const void *>(a), sizeof(T));
			return v;
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

		// The camera and the default environment's light, as 17B510 left them in the zone renderer (zonedraw's snapshot)
		void take_light(zone::actor_frame &f, uintptr_t r)
		{
			std::memcpy(f.view_proj, reinterpret_cast<const void *>(r + 0x38F5C), 64);
			std::memcpy(f.camera, reinterpret_cast<const void *>(r + 0x390F0), 12);
			zone::frame_env &e = f.env;
			e = zone::frame_env();
			float dir[3];
			std::memcpy(dir, reinterpret_cast<const void *>(r + 0x391C0), 12);
			const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
			for (int k = 0; k < 3; ++k)
				e.sun_dir[k] = l > 0.0f ? -dir[k] / l : 0.0f;
			std::memcpy(e.sun, reinterpret_cast<const void *>(r + 0x39184), 12);
			std::memcpy(e.sun_bump, e.sun, 12);
			put_colour(e.ambient, at<uint32_t>(r + 0x39250));
			std::memcpy(e.ambient_bump, e.ambient, 12);
			put_colour(e.fog, at<uint32_t>(r + 0x39254));
			e.fog_end = at<float>(r + 0x39258);
			e.fog_start = at<float>(r + 0x3925C);
			const float s = at<float>(r + 0x3954C);
			e.fog_scale = std::isfinite(s) && s > 0.0f ? s : 1.0f;
			// (a Mog House's room lamp, l_01 of the light table: 1844C0 lights every model actor by it there)
			f.lamp_count = 0;
			if (g.mog_house == 0 || g.lights == 0 || at<uint32_t>(g.mog_house) == 0)
				return;
			for (uint32_t i = 0; i < kLightEntries; ++i)
			{
				const uintptr_t entry = g.lights + uintptr_t(i) * kLightEntryBytes;
				if (at<uint32_t>(entry) != kLampName)
					continue;
				// (the D3DLIGHT8 at +8: diffuse +4, position +0x34, range +0x4C, attenuation 2 +0x5C)
				const uintptr_t light = entry + 8;
				zone::frame_lamp &lamp = f.lamps[0];
				std::memcpy(lamp.colour, reinterpret_cast<const void *>(light + 4), 12);
				std::memcpy(lamp.pos, reinterpret_cast<const void *>(light + 0x34), 12);
				lamp.range = at<float>(light + 0x4C);
				lamp.attenuation2 = at<float>(light + 0x5C);
				if (!(lamp.attenuation2 > 0.0f))
					lamp.attenuation2 = 0.5f;
				f.lamp_count = 1;
				break;
			}
		}

		// The DAT a resource was loaded from: its root's file id (up the parents as 072240 goes; 0 when it can't be read)
		uint32_t file_of(uintptr_t resource)
		{
			uintptr_t r = resource;
			for (int depth = 0; r != 0 && depth < 64; ++depth)
			{
				uint32_t handle = 0, parent = 0;
				if (!read(r + 0x28, handle))
					return 0;
				if (handle == 0 || !read(handle, parent) || parent == 0 || parent == r)
					break;
				r = parent;
			}
			uint32_t id = 0;
			return r != 0 && read(r + 0x34, id) ? id : 0;
		}

		// The data's index in the frame (added at its first use); kNone when there's no room
		uint32_t data_index(zone::actor_frame &f, zone::gpu_zone *z)
		{
			for (uint32_t i = 0; i < f.data_count; ++i)
				if (f.data[i] == z)
					return i;
			if (f.data_count >= zone::kMaxActors)
				return zm::kNone;
			f.data[f.data_count] = z;
			return f.data_count++;
		}

		// The model actor into the walk's frame: true when it's recorded (the game leaves it)
		bool record(uintptr_t renderer, uintptr_t block, uint32_t alpha)
		{
			if (!g.native || !g.in_walk || g.f == nullptr || g.find == nullptr)
			{
				g.n.game++;
				return false;
			}
			const uintptr_t mesh = at<uint32_t>(block + 0x18), resource = at<uint32_t>(block - 4);
			if (mesh == 0 || resource == 0)
			{
				g.n.game++;
				return false;
			}
			// (a DAT's file id: under ROM9's last ones; anything else is a resource read wrong, not asked for)
			const uint32_t file = file_of(resource);
			zone::gpu_zone *const z = file != 0 && file < kMaxFile ? g.find(file) : nullptr;
			if (z == nullptr)
			{
				g.n.not_ready++;
				return false;
			}
			char name[16];
			if (!read(mesh + 8, name, 16))
			{
				g.n.unknown++;
				return false;
			}
			const zm::zone &d = z->data();
			const uint32_t model = d.find(name);
			// (a texture of its named but not in its DAT: the game may have it from another; left to the game)
			if (model == zm::kNone || d.models[model].missing != 0)
			{
				g.n.unknown++;
				return false;
			}
			zone::actor_frame &f = *g.f;
			const uint32_t index = data_index(f, z);
			if (index == zm::kNone || f.count >= zone::kMaxActors)
			{
				g.n.dropped++;
				return false;
			}
			if (!g.lit)
			{
				take_light(f, renderer);
				g.lit = true;
			}
			zone::frame_object &o = f.objects[f.count++];
			o = zone::frame_object();
			float m[16];
			std::memcpy(m, reinterpret_cast<const void *>(block + 0x40), 64);
			for (int k = 0; k < 3; ++k)
			{
				o.world[k * 4 + 0] = m[0 * 4 + k];
				o.world[k * 4 + 1] = m[1 * 4 + k];
				o.world[k * 4 + 2] = m[2 * 4 + k];
				o.world[k * 4 + 3] = m[3 * 4 + k];
			}
			o.model = model;
			// (fixed-function, blended parts keeping their Z writes: as the doors pass draws; 1844C0 sets no ZWRITE)
			o.flags = (alpha != 0 ? zm::kAlphaTested : 0) | zone::kDoor;
			if (f.lamp_count != 0)
				o.lamps[0] = 0;
			o.zone = index;
			g.n.drawn++;
			return true;
		}
	}

	void __fastcall on_draw(uintptr_t renderer, uintptr_t, uintptr_t block, uint32_t alpha)
	{
		if (record(renderer, block, alpha))
			return;
		reinterpret_cast<draw_fn>(g.original)(renderer, 0, block, alpha);
	}

	void on_slot(uint32_t kind, uint32_t seq, uintptr_t)
	{
		if (!g.in)
			return;
		if (kind == 2)
		{
			g.f = &zone::actor_slot(seq);
			g.f->count = 0;
			g.f->data_count = 0;
			g.f->lamp_count = 0;
			g.lit = false;
			g.in_walk = true;
		}
		else if (kind == 3)
		{
			g.in_walk = false;
			if (g.f != nullptr)
			{
				g.n.last = g.f->count;
				zone::actor_publish(*g.f, seq);
			}
			g.f = nullptr;
		}
	}

	bool start(std::string &why)
	{
		if (g.in)
			return true;
		const uintptr_t at_ = find(kSite);
		if (at_ == 0)
		{
			why = "the model actors' draw call wasn't found";
			return false;
		}
		const uintptr_t call = at_ + kCallAt;
		uint8_t saved[5];
		if (!read(call, saved, 5) || saved[0] != 0xE8)
		{
			why = "the model actors' draw call isn't laid out as expected";
			return false;
		}
		const uintptr_t original = rel_target(call);
		// The Mog House's object and the light table (optional: without them no room lamp)
		uintptr_t mog_house = 0, lights = 0;
		if (const uintptr_t m = find(kMogHouse); m != 0)
		{
			uint8_t op = 0, mov = 0;
			if (read(m + kFindLightAt, op) && op == 0xE8)
			{
				const uintptr_t finder = rel_target(m + kFindLightAt);
				if (read(finder + 4, mov) && mov == 0xB8)
				{
					mog_house = word(m + kMogHouseAt);
					lights = word(finder + 5);
				}
			}
		}
		uint8_t ours[5] = { 0xE8 };
		put32(ours + 1, uint32_t(reinterpret_cast<uintptr_t>(&on_draw) - (call + 5)));
		if (!write_code(call, ours, 5))
		{
			why = "couldn't write the game's code";
			return false;
		}
		g.call = call;
		g.original = original;
		std::memcpy(g.saved, saved, 5);
		std::memcpy(g.ours, ours, 5);
		g.mog_house = mog_house;
		g.lights = lights;
		g.in = true;
		return true;
	}

	bool stop(std::string &why)
	{
		if (!g.in)
			return true;
		if (!code_is(g.call, g.ours, 5))
		{
			why = "something else has changed the model actors' draw call since: left as it is";
			return false;
		}
		if (!write_code(g.call, g.saved, 5))
		{
			why = "couldn't write the game's code";
			return false;
		}
		const auto kept = g.find;
		g = state();
		g.find = kept;
		zone::clear_actor_frames();
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

	void set_data(zone::gpu_zone *(*find)(uint32_t file))
	{
		g.find = find;
	}

	counts get_counts()
	{
		return g.n;
	}

	std::string status()
	{
		if (!g.in)
			return "model actors (ships, furniture, plants): drawn by the game";
		char line[400];
		std::snprintf(line, sizeof(line),
			"model actors (ships, furniture, plants): %s; %llu drawn by HiFi XI (the last frame %u), left to the game: %llu (off or outside the walk), %llu (data not up yet), "
			"%llu (mesh not found, or textures from another DAT), %llu (no room)%s",
			g.native ? "HiFi XI draws them" : "the game draws them (the zone renderer or the characters' markers not ready)",
			static_cast<unsigned long long>(g.n.drawn), g.n.last, static_cast<unsigned long long>(g.n.game), static_cast<unsigned long long>(g.n.not_ready),
			static_cast<unsigned long long>(g.n.unknown), static_cast<unsigned long long>(g.n.dropped), g.mog_house == 0 ? "; the Mog House's lamp not found" : "");
		return line;
	}

	sites where()
	{
		return { g.call, g.original, g.mog_house, g.lights };
	}

	void set_draw(uintptr_t draw)
	{
		g.original = draw;
	}
}
