/*
 * Offline test of the zone's objects handed to the native zone renderer (src/game/zonedraw.cpp), on both FFXiMain dumps
 * (game_dump.hpp) with zone 100's data on the GPU (WARP):
 *   - the site found at 17CBA8 and its exit 17DDFF (the je before it); a jmp to our stub there, nothing else changed;
 *   - the stub run as 17C9C0 would reach it (a stack laid out as its frame: the light table, the object and the
 *     scenery flag at +1D8, +1DC, +1E0; ebx the model, ecx the object, ebp the renderer), its two ways back pointed at
 *     stand-ins: a pass HiFi XI draws and a model it has: recorded, the game's exit, the object counted as drawn; a
 *     model it hasn't, a scenery object, a pass the gate kept (wanted off, data of another zone): the game draws it,
 *     with the instruction the jmp displaced done (esi = the count);
 *   - what's recorded: the matrix as columns, mirroring, alpha test, lamps (the same lamp once, an unlit one left out);
 *     the pass's light, camera and fog from the renderer at its end, published under its seq;
 *   - area environments through stand-ins for the game's calls (worked out once a pass, each its own index), a swaying
 *     plant's keyframe from the game's sway clock (run once for it), an object of the second map from that map's data;
 *   - the doors' site at 183E47 and its exit 1844A9: its stub run as 183DA0 would reach it (esi the renderer, ebp the
 *     part, the model at esp+10h, the light table at esp+2Ch): a door part recorded (the door flag, bit-28 pieces lit by
 *     renderer +0x50's environment with the zone's fog start and end), or back to the game with eax = part +0xE4;
 *   - stop(): the dump again, byte for byte.
 *
 *   build\zonedraw_test.exe
 */

#include <d3d12.h>
#include <dxgi1_4.h>
#include "../src/game/zonedraw.hpp"
#include "../src/zone/frame.hpp"
#include "../src/zone/zone_gpu.hpp"
#include "game_dump.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	namespace zm = hifi::bake::zonemesh;
	namespace zd = hifi::game::zonedraw;

	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	// The stand-in frame and registers (statics: the run swaps esp and ebp)
	alignas(16) uint8_t fake_stack[0x3000];
	uint32_t g_esp_save, g_path, g_esi, g_eax, g_ebx_after, g_ecx_after;
	uint32_t g_stub, g_door_stub, g_model, g_object, g_renderer;

	__declspec(naked) void back_to_game()
	{
		__asm {
			mov g_path, 1
			mov g_esi, esi
			mov g_eax, eax
			mov g_ebx_after, ebx
			mov g_ecx_after, ecx
			mov esp, g_esp_save
			popad
			ret
		}
	}
	__declspec(naked) void to_exit()
	{
		__asm {
			mov g_path, 2
			mov g_esi, esi
			mov g_eax, eax
			mov g_ebx_after, ebx
			mov g_ecx_after, ecx
			mov esp, g_esp_save
			popad
			ret
		}
	}
	__declspec(naked) void run_stub()
	{
		__asm {
			pushad
			mov g_esp_save, esp
			lea esp, fake_stack[0x2000]
			mov ebx, g_model
			mov ecx, g_object
			mov ebp, g_renderer
			mov esi, 0x5151
			jmp dword ptr [g_stub]
		}
	}

	__declspec(naked) void run_door_stub()
	{
		__asm {
			pushad
			mov g_esp_save, esp
			lea esp, fake_stack[0x2000]
			mov esi, g_renderer
			mov ebp, g_object
			mov eax, 0x5151
			jmp dword ptr [g_door_stub]
		}
	}

	// 183DA0's frame at 183E47: the model and the light table where the door stub reads them
	uint32_t run_door(uint32_t model, uint32_t part, uint32_t renderer, uint32_t table)
	{
		uint8_t *esp = fake_stack + 0x2000;
		std::memcpy(esp + 0x10, &model, 4);
		std::memcpy(esp + 0x2C, &table, 4);
		g_object = part;
		g_renderer = renderer;
		g_path = 0;
		run_door_stub();
		return g_path;
	}

	// Stand-ins for the game's environment calls (thiscall: the environment in ecx), each environment's values from
	// its address: the light's diffuse (e / 1000, 0.25, 0.5), direction (0, 0, 2); c7 (0.125...); ambient and fog
	// colours; fog end and start 900 and 300; the brightness option adds 0.5 to red
	int g_env_calls = 0, g_sway_calls = 0;
	void __fastcall fake_light(uintptr_t env, uintptr_t, void *light, int)
	{
		g_env_calls++;
		float *l = static_cast<float *>(light);
		l[1] = float(env % 1000) / 1000.0f;
		l[2] = 0.25f;
		l[3] = 0.5f;
		l[0x10] = 0.0f;
		l[0x11] = 0.0f;
		l[0x12] = 2.0f;
	}
	uint32_t __fastcall fake_bright(uintptr_t, uintptr_t, void *light)
	{
		static_cast<float *>(light)[1] += 0.5f;
		return 0;
	}
	void __fastcall fake_c7(uintptr_t, uintptr_t, float *out)
	{
		out[0] = 0.125f;
		out[1] = 0.25f;
		out[2] = 0.375f;
		out[3] = 0.0f;
	}
	void __fastcall fake_ambient(uintptr_t, uintptr_t, uint32_t *out, int)
	{
		*out = 0x00102030;
	}
	void __fastcall fake_fog(uintptr_t, uintptr_t, uint32_t *colour, float *end, float *start, int)
	{
		*colour = 0x00FF0000;
		*end = 900.0f;
		*start = 300.0f;
	}
	// The sway clock: the object's weight +0x15C set to 0.5
	void __fastcall fake_sway(uintptr_t, uintptr_t, uintptr_t object)
	{
		g_sway_calls++;
		const float w = 0.5f;
		std::memcpy(reinterpret_cast<void *>(object + 0x15C), &w, 4);
	}
	uint32_t g_wrapper = 0x1234;

	// The 17C9C0 frame: its arguments where the stub reads them
	void set_frame(uint32_t table, uint32_t object, uint32_t ship)
	{
		uint8_t *esp = fake_stack + 0x2000;
		std::memcpy(esp + 0x1D8, &table, 4);
		std::memcpy(esp + 0x1DC, &object, 4);
		std::memcpy(esp + 0x1E0, &ship, 4);
	}

	uint32_t run(uint32_t model, uint32_t object, uint32_t renderer, uint32_t table, uint32_t ship)
	{
		g_model = model;
		g_object = object;
		g_renderer = renderer;
		g_path = 0;
		set_frame(table, object, ship);
		run_stub();
		return g_path;
	}

	template <typename T>
	void put(std::vector<uint8_t> &m, size_t at, T v)
	{
		std::memcpy(m.data() + at, &v, sizeof(T));
	}

	size_t differences(const std::string &path, uintptr_t base, size_t size)
	{
		FILE *f = nullptr;
		if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr)
			return size_t(-1);
		std::vector<uint8_t> file(size);
		const size_t got = fread(file.data(), 1, size, f);
		fclose(f);
		if (got != size)
			return size_t(-1);
		size_t n = 0;
		const uint8_t *image = reinterpret_cast<const uint8_t *>(base);
		for (size_t i = 0; i < size; ++i)
			n += image[i] != file[i];
		return n;
	}
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main()
{
	// Zone 100 on WARP (the key tables from the first dump). The rest of the second dump's place is kept before the
	// device is made: its memory mustn't land there.
	uintptr_t base = 0;
	size_t size = dump::map(dump::kDumps[0], base);
	if (size == 0)
	{
		check(false, "the FFXiMain dump mapped");
		return 1;
	}
	std::vector<void *> kept;
	{
		WIN32_FILE_ATTRIBUTE_DATA a = {};
		const uintptr_t b2 = dump::base_of(dump::kDumps[1]);
		if (GetFileAttributesExA(dump::kDumps[1], GetFileExInfoStandard, &a))
			for (uintptr_t at = b2; at < b2 + a.nFileSizeLow; at += 0x10000)
				if (void *p = VirtualAlloc(reinterpret_cast<void *>(at), 0x10000, MEM_RESERVE, PAGE_NOACCESS))
					kept.push_back(p);
	}
	zm::zone z;
	hifi::bake::result r;
	check(zm::load(100, z, r), "zone 100 read");
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
	ID3D12Device *device = nullptr;
	ID3D12CommandQueue *queue = nullptr;
	D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_COPY };
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
	hifi::zone::gpu_zone gz;
	std::string why;
	check(gz.upload(device, queue, std::move(z), why) && gz.wait(10000), "zone 100 on the GPU" + why);
	const zm::model &known = gz.data().models[5];

	// A renderer, an object, two models (one of the zone's, one not), a light table with two lamps (one unlit)
	std::vector<uint8_t> ren(0x40000), obj(0x200), mod_known(0x80), mod_other(0x80), table(0x200), lit(0x80), unlit(0x80);
	const uint32_t R = uint32_t(uintptr_t(ren.data())), O = uint32_t(uintptr_t(obj.data()));
	put<uint32_t>(ren, 0x3C, 100);
	put<uint32_t>(ren, 0x39448, 41); // (the objects drawn so far)
	for (int i = 0; i < 16; ++i)
		put<float>(ren, 0x38F5C + 4 * i, float(i + 1));
	put<float>(ren, 0x390F0, 10.0f);
	put<float>(ren, 0x39184, 1.25f);
	put<float>(ren, 0x39188, 1.0f);
	put<float>(ren, 0x3918C, 0.5f);
	put<float>(ren, 0x391C0, 0.0f);
	put<float>(ren, 0x391C4, 2.0f); // (the sun's direction: down, y; toward it: up, -y)
	put<float>(ren, 0x391C8, 0.0f);
	put<uint32_t>(ren, 0x39250, 0x00804020);
	put<uint32_t>(ren, 0x39254, 0x00FF8000);
	put<float>(ren, 0x39258, 400.0f);
	put<float>(ren, 0x3925C, 100.0f);
	put<float>(ren, 0x3954C, 1.0f);
	put<float>(ren, 0x394D0, 0.9f);
	for (int i = 0; i < 16; ++i)
		put<float>(obj, 0x40 + 4 * i, float(100 + i));
	put<uint8_t>(obj, 0xD4, 1);
	put<uint8_t>(obj, 0xD6, 1);
	put<uint32_t>(obj, 0xE4, 2);
	put<uint32_t>(obj, 0xE8, 2);
	put<uint32_t>(obj, 0xEC, 3);
	std::memcpy(mod_known.data() + 8, known.name, 16);
	put<uint32_t>(mod_known, 0x1C, 3);
	std::memcpy(mod_other.data() + 8, "not_in_this_zone", 16);
	lit[0] = 'l';
	put<uint32_t>(lit, 8, 1);            // (D3DLIGHT_POINT)
	put<float>(lit, 8 + 4, 0.7f);        // diffuse
	put<float>(lit, 8 + 0x34, 5.0f);     // position
	put<float>(lit, 8 + 0x4C, 6.0f);     // range
	put<float>(lit, 8 + 0x5C, 0.5f);     // attenuation 2
	unlit[0] = 'c';
	put<uint32_t>(unlit, 8, 1);
	const uint32_t T = uint32_t(uintptr_t(table.data())) + 0x100;
	put<uint32_t>(table, 0x100 - 0x48 + 2 * 0x4C, uint32_t(uintptr_t(lit.data())));
	put<uint32_t>(table, 0x100 - 0x48 + 3 * 0x4C, uint32_t(uintptr_t(unlit.data())));
	const uint32_t MK = uint32_t(uintptr_t(mod_known.data())), MO = uint32_t(uintptr_t(mod_other.data()));
	// A swaying plant's model (kind 2, animated, 16 frames: N - 1 = 15) and an object of an area environment; a second
	// map slot holding an array of objects; an ev10 for the doors
	std::vector<uint8_t> mod_sway(0x80), obj_area(0x200), slot1_objects(0x16C * 3), env_a(0x200), env_door(0x200);
	std::memcpy(mod_sway.data() + 8, known.name, 16);
	put<uint32_t>(mod_sway, 0x1C, 2);
	put<uint32_t>(mod_sway, 0x68, 1);
	put<float>(mod_sway, 0x6C, 15.0f);
	const uint32_t MS = uint32_t(uintptr_t(mod_sway.data()));
	const uint32_t EA = uint32_t(uintptr_t(env_a.data())), ED = uint32_t(uintptr_t(env_door.data()));
	obj_area = obj;
	put<uint32_t>(obj_area, 0xE0, EA);
	const uint32_t OA = uint32_t(uintptr_t(obj_area.data()));
	std::memcpy(slot1_objects.data() + 0x16C, obj.data(), 0x16C);
	const uint32_t O1 = uint32_t(uintptr_t(slot1_objects.data())) + 0x16C;
	{
		zd::game_calls c;
		c.fog = uintptr_t(&fake_fog);
		c.light = uintptr_t(&fake_light);
		c.c7 = uintptr_t(&fake_c7);
		c.ambient = uintptr_t(&fake_ambient);
		c.bright = uintptr_t(&fake_bright);
		c.wrapper_at = uintptr_t(&g_wrapper);
		c.sway = uintptr_t(&fake_sway);
		zd::set_calls(c);
	}

	for (const char *path : dump::kDumps)
	{
		std::printf("\n%s\n", path);
		if (base == 0)
		{
			for (void *p : kept)
				VirtualFree(p, 0, MEM_RELEASE);
			kept.clear();
			size = dump::map(path, base);
			if (size == 0)
			{
				check(false, "mapped at its base");
				continue;
			}
		}
		check(zd::start(why) && zd::in(), "in" + (why.empty() ? "" : ": " + why));
		const zd::sites s = zd::where();
		const uint8_t *site = reinterpret_cast<const uint8_t *>(s.site);
		check(s.site == base + 0x17CBA8 && s.exit == base + 0x17DDFF, "  the site at 17CBA8, its exit 17DDFF");
		check(site[0] == 0xE9 && s.site + 5 + *reinterpret_cast<const int32_t *>(site + 1) == s.stub && site[5] == 0x90, "  17CBA8: jmp the stub; nop");
		check(s.door_site == base + 0x183E47 && s.door_exit == base + 0x1844A9, "  the doors' site at 183E47, its exit 1844A9");
		const uint8_t *door_site = reinterpret_cast<const uint8_t *>(s.door_site);
		check(s.door_site != 0 && door_site[0] == 0xE9 && s.door_site + 5 + *reinterpret_cast<const int32_t *>(door_site + 1) == s.door_stub && door_site[5] == 0x90,
			"  183E47: jmp the door stub; nop");
		check(differences(path, base, size) <= 12, "  nothing else changed");

		// The stub's ways back into the game pointed at the stand-ins
		uint8_t *stub = reinterpret_cast<uint8_t *>(s.stub);
		*reinterpret_cast<int32_t *>(stub + 0x2A) = int32_t(uintptr_t(&back_to_game) - (s.stub + 0x2E));
		*reinterpret_cast<int32_t *>(stub + 0x2F) = int32_t(uintptr_t(&to_exit) - (s.stub + 0x33));
		g_stub = uint32_t(s.stub);
		uint8_t *door_stub = reinterpret_cast<uint8_t *>(s.door_stub);
		*reinterpret_cast<int32_t *>(door_stub + 0x23) = int32_t(uintptr_t(&back_to_game) - (s.door_stub + 0x27));
		*reinterpret_cast<int32_t *>(door_stub + 0x28) = int32_t(uintptr_t(&to_exit) - (s.door_stub + 0x2C));
		g_door_stub = uint32_t(s.door_stub);

		// A pass HiFi XI draws
		zd::set_state(true, &gz, nullptr, true);
		zd::on_slot(0, 21, R);
		check(zd::get_counts().gate.empty(), "a pass opened: " + (zd::get_counts().gate.empty() ? std::string("HiFi XI's") : zd::get_counts().gate));
		put<uint32_t>(ren, 0x39448, 41);
		check(run(MK, O, R, T, 0) == 2, "  a model it has: the game's exit");
		check(*reinterpret_cast<uint32_t *>(ren.data() + 0x39448) == 42, "  counted as drawn (42)");
		check(g_ebx_after == MK && g_ecx_after == O, "  ebx and ecx as they were");
		const bool back = run(MO, O, R, T, 0) == 1;
		check(back && g_esi == 42, "  a model it hasn't: back to the game, esi = the count (" + std::to_string(g_esi) + ")");
		check(run(MK, O, R, T, 1) == 1, "  a scenery object (moving map): back to the game");
		check(run(MK, O, R, T, 0) == 2, "  the known model again: recorded");
		zd::on_slot(1, 21, R);
		const hifi::zone::frame *f = hifi::zone::published(21 & 15);
		check(f != nullptr && f->seq == 21 && f->native && f->zone == &gz && f->count == 2, "the pass published under seq 21: 2 objects");
		if (f != nullptr && f->count >= 1)
		{
			const hifi::zone::frame_object &o = f->objects[0];
			check(o.model == 5 && o.world[0] == 100 && o.world[1] == 104 && o.world[2] == 108 && o.world[3] == 112 && o.world[4] == 101 && o.world[11] == 114,
				"  the model and the matrix as columns (x: 100 104 108 112; z's translation 114)");
			check(o.flags == (zm::kAlphaTested | zm::kMirrored), "  alpha-tested and mirrored");
			check(o.lamps[0] == 0 && o.lamps[1] == 0 && o.lamps[2] == zm::kNone && o.lamps[3] == zm::kNone && f->lamp_count == 1,
				"  lamps: the lit one once (twice named), the unlit one left out");
			check(f->lamps[0].pos[0] == 5.0f && f->lamps[0].range == 6.0f && f->lamps[0].colour[0] == 0.7f, "  the lamp's position, range, colour");
			check(f->view_proj[0] == 1.0f && f->view_proj[15] == 16.0f && f->camera[0] == 10.0f, "  view x projection, camera");
			check(f->sun_dir[1] == -1.0f && f->sun[0] == 1.25f && f->sun_bump[0] == 0.9f, "  the sun: toward it (-y), its colours");
			check(std::fabs(f->ambient[0] - 128.0f / 255) < 1e-6f && std::fabs(f->fog[0] - 1.0f) < 1e-6f && f->fog_start == 100.0f && f->fog_end == 400.0f,
				"  ambient, fog colour, start and end");
		}
		check(zd::status().find("2 objects") != std::string::npos, "  status: " + zd::status());

		// Area environments, a swaying plant, a door, the second map
		zd::set_state(true, &gz, &gz, true);
		put<uint32_t>(ren, 0x50, ED);
		put<uint32_t>(ren, 0xD8, 2);       // (slot 1: a map of type 2)
		put<uint32_t>(ren, 0xDC, 100);     // (its map: zone 100's data stands in for it)
		put<uint32_t>(ren, 0xD8 + 0x24, 3);
		put<uint32_t>(ren, 0xD8 + 0x28, uint32_t(uintptr_t(slot1_objects.data())));
		g_env_calls = 0;
		g_sway_calls = 0;
		zd::on_slot(0, 25, R);
		check(run(MK, OA, R, T, 0) == 2 && run(MK, OA, R, T, 0) == 2 && g_env_calls == 1, "an area environment's objects recorded, its calls made once");
		check(run(MS, O, R, T, 0) == 2 && g_sway_calls == 1, "a swaying plant recorded, the game's sway clock run for it");
		check(run(MK, O1, R, T, 0) == 2, "an object of the second map recorded");
		check(run_door(MK, OA, R, T) == 2 && g_env_calls == 2, "a door part recorded; ev10 worked out (the area's again from the pass's table)");
		check(run_door(MO, O, R, T) == 1 && g_eax == *reinterpret_cast<const uint32_t *>(obj.data() + 0xE4), "  a door of a model it hasn't: back to the game, eax = part +0xE4");
		zd::on_slot(1, 25, R);
		f = hifi::zone::published(25 & 15);
		check(f != nullptr && f->count == 5 && f->env_count == 3 && f->second == &gz, "published: 5 objects, 2 area environments, the second map's data");
		if (f != nullptr && f->count == 5 && f->env_count == 3)
		{
			const hifi::zone::frame_env &a = f->envs[1], &d = f->envs[2];
			const float red = float(EA % 1000) / 1000.0f;
			check(f->objects[0].env == 1 && f->objects[1].env == 1 && f->objects[2].env == 0 && f->objects[3].env == 0, "  each object's environment: the area's 1, the zone's own 0");
			check(a.sun[0] == red && a.sun_bump[0] == red + 0.5f && a.sun[1] == 0.25f && a.sun_dir[2] == -1.0f, "  the area's sun: its diffuse, after the brightness option for the bump shader, toward it");
			check(a.ambient_bump[2] == 0.375f && std::fabs(a.ambient[0] - 16.0f / 255) < 1e-6f && a.fog[0] == 1.0f && a.fog_start == 300.0f && a.fog_end == 900.0f,
				"  its c7, ambient, fog colour, start and end");
			check(std::fabs(f->objects[2].sway - 7.0f / 15.0f) < 1e-6f && f->objects[0].sway == 0.0f, "  the plant's keyframe: trunc(15 x 0.5) / 15");
			check(f->objects[3].zone == 1 && f->objects[0].zone == 0, "  the second map's object from its data (zone 1)");
			const hifi::zone::frame_object &door = f->objects[4];
			check((door.flags & hifi::zone::kDoor) != 0 && door.env == 1 && door.door_env == 2, "  the door: flagged, its own environment 1, its bit-28 pieces' 2");
			check(d.fog_start == 100.0f && d.fog_end == 400.0f && d.fog[0] == 1.0f, "  ev10 for the doors: its fog colour, the zone's own start and end");
		}
		check(zd::get_counts().doors == 1 && zd::get_counts().recorded_second == 1, "  counted: 1 door, 1 of the second map's");
		put<uint32_t>(ren, 0xD8, 0);
		put<uint32_t>(ren, 0x50, 0);
		zd::set_state(true, &gz, nullptr, true);
		zd::on_slot(0, 26, R);
		check(run(MK, O1, R, T, 0) == 2, "no second map: the same object from the zone's data");
		zd::on_slot(1, 26, R);
		f = hifi::zone::published(26 & 15);
		check(f != nullptr && f->count == 1 && f->objects[0].zone == 0 && f->second == nullptr, "  (zone 0, no second map's data)");

		// Passes the gate keeps
		zd::set_state(false, &gz, nullptr, true);
		zd::on_slot(0, 22, R);
		check(run(MK, O, R, T, 0) == 1 && zd::get_counts().gate == "off", "wanted off: the game draws all of it");
		zd::on_slot(1, 22, R);
		check(hifi::zone::published(22 & 15) == nullptr, "  nothing published");
		zd::set_state(true, &gz, nullptr, true);
		put<uint32_t>(ren, 0x3C, 101);
		zd::on_slot(0, 23, R);
		check(run(MK, O, R, T, 0) == 1, "data of another zone: the game draws it (" + zd::get_counts().gate + ")");
		zd::on_slot(1, 23, R);
		put<uint32_t>(ren, 0x3C, 100);
		zd::set_state(true, &gz, nullptr, false);
		zd::on_slot(0, 24, R);
		check(run(MK, O, R, T, 0) == 1, "the Direct3D 12 side not ready: the game draws it");
		zd::on_slot(1, 24, R);
		zd::set_state(false, nullptr, nullptr, false);

		// Back
		*reinterpret_cast<int32_t *>(stub + 0x2A) = int32_t((s.site + 6) - (s.stub + 0x2E));
		*reinterpret_cast<int32_t *>(stub + 0x2F) = int32_t(s.exit - (s.stub + 0x33));
		*reinterpret_cast<int32_t *>(door_stub + 0x23) = int32_t((s.door_site + 6) - (s.door_stub + 0x27));
		*reinterpret_cast<int32_t *>(door_stub + 0x28) = int32_t(s.door_exit - (s.door_stub + 0x2C));
		why.clear();
		check(zd::stop(why) && !zd::in() && dump::same_as_file(path, base, size), "stop(): the dump again, byte for byte" + why);
		dump::unmap(base);
		base = 0;
	}
	device->Release();
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
