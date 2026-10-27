/*
 * A stand-in FFXiMain.dll for tests/e2e.cpp: just the game's view matrix getter (the bytes the add-on
 * looks for: mov eax, [gGame]; add eax, 0x94; ret) and a game object with the camera where the game
 * keeps it (+8 in the field, +9 not a zoning flag to trust, +0x94 view, +0x154 projection); the
 * environment's light; the global light table behind the code the Ashita addon finds it by; and the
 * weather: the environment's weather object and record, the zone renderer's fog and the weather's id,
 * each behind the code the Ashita addon finds it by.
 */

#include <windows.h>
#include <cstdint>
#include <cstring>

static uint8_t game_object[0x200];
extern "C" uint8_t *g_game = game_object;

extern "C" __declspec(dllexport) __declspec(naked) void *view_getter()
{
	__asm
	{
		mov eax, g_game
		add eax, 0x94
		ret
	}
}

// The environment object (its global found by the bytes after 'mov eax, [env]' at the start of a function in
// the game) with the light's direction, toward the ground, at +0x14C
static uint8_t env_object[0x200];
extern "C" uint8_t *g_env = env_object;

extern "C" __declspec(dllexport) __declspec(naked) void env_user()
{
	__asm
	{
		mov eax, g_env
		sub esp, 24h
		push esi
		mov esi, [eax + 18h]
		test esi, esi
		pop esi
		add esp, 24h
		ret
	}
}

extern "C" __declspec(dllexport) void set_light(const float *direction)
{
	std::memcpy(env_object + 0x14C, direction, 12);
}

// The weather (client-notes "Weather and sky"): the environment's +0x10 weather object (+8 the one it's changing from,
// +0x10 its name, +0x1C the new one's share) and +0x18 the record the zone is lit by (0xB0 bytes); the zone renderer
// behind its global, with the fog the game draws the world with at +0x39254 (colour 0x00RRGGBB), +0x3925C (start) and
// +0x39258 (end); the weather's id. Each behind the code the worldfx Ashita addon finds it by: the camera's wall test
// (mov ecx, [renderer] ...), the fog render states set from the renderer (mov edx, [ebp+off]; push edx; push 22h ...),
// the radar's weather icon (mov ax, [id]; cmp ax, 4 ...). Never called.
static uint8_t weather_object[0x40], weather_before[0x40], weather_record[0xB0];
static uint8_t renderer_object[0x39300];
extern "C" uint8_t *g_renderer = renderer_object;
extern "C" uint16_t g_weather_id = 0;

extern "C" __declspec(dllexport) __declspec(naked) void camera_wall_test()
{
	__asm
	{
		mov ecx, g_renderer
		_emit 0x8D
		_emit 0x94
		_emit 0x24
		_emit 0x8C
		_emit 0x00
		_emit 0x00
		_emit 0x00
		_emit 0x8D
		_emit 0x6F
		_emit 0x44
		_emit 0x52
		_emit 0x8D
		_emit 0x44
		_emit 0x24
		_emit 0x60
		_emit 0x55
		_emit 0x50
		_emit 0xE8
		_emit 0x00
		_emit 0x00
		_emit 0x00
		_emit 0x00
		_emit 0x84
		_emit 0xC0
		_emit 0x75
		_emit 0x17
		ret
	}
}

extern "C" __declspec(dllexport) __declspec(naked) void set_fog_states()
{
	__asm
	{
		// mov edx, [ebp+39254h]; push edx; push 22h; mov eax, [eax+0Ch]; push eax; mov ecx, [eax]; call [ecx+0C8h]; mov eax, [0]
		_emit 0x8B
		_emit 0x95
		_emit 0x54
		_emit 0x92
		_emit 0x03
		_emit 0x00
		_emit 0x52
		_emit 0x6A
		_emit 0x22
		_emit 0x8B
		_emit 0x40
		_emit 0x0C
		_emit 0x50
		_emit 0x8B
		_emit 0x08
		_emit 0xFF
		_emit 0x91
		_emit 0xC8
		_emit 0x00
		_emit 0x00
		_emit 0x00
		_emit 0xA1
		_emit 0x00
		_emit 0x00
		_emit 0x00
		_emit 0x00
		// mov edx, [ebp+3925Ch]; push edx; push 24h; ...
		_emit 0x8B
		_emit 0x95
		_emit 0x5C
		_emit 0x92
		_emit 0x03
		_emit 0x00
		_emit 0x52
		_emit 0x6A
		_emit 0x24
		_emit 0x8B
		_emit 0x40
		_emit 0x0C
		_emit 0x50
		_emit 0x8B
		_emit 0x08
		_emit 0xFF
		_emit 0x91
		_emit 0xC8
		_emit 0x00
		_emit 0x00
		_emit 0x00
		_emit 0xA1
		_emit 0x00
		_emit 0x00
		_emit 0x00
		_emit 0x00
		// mov edx, [ebp+39258h]; push edx; push 25h
		_emit 0x8B
		_emit 0x95
		_emit 0x58
		_emit 0x92
		_emit 0x03
		_emit 0x00
		_emit 0x52
		_emit 0x6A
		_emit 0x25
		ret
	}
}

extern "C" __declspec(dllexport) __declspec(naked) void weather_icon()
{
	__asm
	{
		mov ax, g_weather_id
		_emit 0x66
		_emit 0x3D
		_emit 0x04
		_emit 0x00
		_emit 0x72
		_emit 0x7E
		_emit 0x66
		_emit 0x3D
		_emit 0x14
		_emit 0x00
		ret
	}
}

// The weather shown (4 characters), the one it's changing from (nullptr: none) and the new one's share; the record
// (0xB0 bytes; nullptr: none, the environment's +0x18 0)
extern "C" __declspec(dllexport) void set_weather(uint16_t id, const char *name, const char *before, float share, const uint8_t *record)
{
	g_weather_id = id;
	std::memset(weather_object, 0, sizeof(weather_object));
	std::memcpy(weather_object + 0x10, name, 4);
	std::memcpy(weather_object + 0x1C, &share, 4);
	if (before != nullptr)
	{
		std::memcpy(weather_before + 0x10, before, 4);
		const uint32_t at = uint32_t(uintptr_t(weather_before));
		std::memcpy(weather_object + 0x08, &at, 4);
	}
	const uint32_t object = uint32_t(uintptr_t(weather_object)), lit_by = record != nullptr ? uint32_t(uintptr_t(weather_record)) : 0;
	if (record != nullptr)
		std::memcpy(weather_record, record, sizeof(weather_record));
	std::memcpy(weather_object + 0x20, &lit_by, 4); // (the weather's own record)
	std::memcpy(env_object + 0x10, &object, 4);
	std::memcpy(env_object + 0x18, &lit_by, 4);
}

// The game's stand-in record (the static default 18B170 builds: colours 0x80, fog 100 to 80, light 1, drawn to 100),
// which the zone is lit with while its weather has no record of its own yet (entering a zone): in place with 1, the
// weather's own back with 0
static uint8_t stand_in_record[0xB0];

extern "C" __declspec(dllexport) void set_weather_stand_in(int on)
{
	std::memset(stand_in_record, 0x80, sizeof(stand_in_record));
	const float fog[2] = { 100.0f, 80.0f }, light = 1.0f, drawn_to = 100.0f;
	std::memcpy(stand_in_record + 0x3C, fog, 8);
	std::memcpy(stand_in_record + 0x44, &light, 4);
	std::memcpy(stand_in_record + 0x58, &drawn_to, 4);
	const uint32_t own = on ? 0u : uint32_t(uintptr_t(weather_record));
	const uint32_t lit_by = on ? uint32_t(uintptr_t(stand_in_record)) : own;
	std::memcpy(weather_object + 0x20, &own, 4);
	std::memcpy(env_object + 0x18, &lit_by, 4);
}

// Another area's environment of the zone's, chained after the default one (its +4) with 1, out with 0: id 'subl',
// raining, a dim record of its own (the zone frame isn't drawn with it: zone objects with that area are)
static uint8_t area_env[0x200], area_weather[0x40], area_record[0xB0];

extern "C" __declspec(dllexport) void set_area_environment(int on)
{
	std::memcpy(area_env + 0x08, "subl", 4);
	std::memcpy(area_weather + 0x10, "rain", 4);
	const float share = 1.0f, light = 0.3f;
	std::memcpy(area_weather + 0x1C, &share, 4);
	std::memset(area_record, 0x40, sizeof(area_record));
	const float fog[2] = { 60.0f, 0.0f }, drawn_to = 80.0f;
	std::memcpy(area_record + 0x3C, fog, 8);
	std::memcpy(area_record + 0x44, &light, 4);
	std::memcpy(area_record + 0x58, &drawn_to, 4);
	const uint32_t weather = uint32_t(uintptr_t(area_weather)), record = uint32_t(uintptr_t(area_record));
	std::memcpy(area_weather + 0x20, &record, 4);
	std::memcpy(area_env + 0x10, &weather, 4);
	std::memcpy(area_env + 0x18, &record, 4);
	const uint32_t next = on ? uint32_t(uintptr_t(area_env)) : 0u;
	std::memcpy(env_object + 0x04, &next, 4);
}

// The fog the renderer holds for the world this frame
extern "C" __declspec(dllexport) void set_fog(uint32_t colour, float start, float end)
{
	std::memcpy(renderer_object + 0x39254, &colour, 4);
	std::memcpy(renderer_object + 0x3925C, &start, 4);
	std::memcpy(renderer_object + 0x39258, &end, 4);
}

// The global light table (256 entries of 0x70 bytes: +0 the light's id, +8 its D3DLIGHT8: type +8, colour +0xC,
// position +0x3C, range +0x54, brightness +0x68), behind the code of the game's find-by-id for it, whose bytes the
// worldfx Ashita addon looks for: mov edx, [esp+4]; mov eax, <the table>; xor ecx, ecx; cmp [eax], edx; je +0Eh
static uint8_t light_table[256 * 0x70];

extern "C" __declspec(dllexport) __declspec(naked) void light_find()
{
	__asm
	{
		mov edx, [esp + 4]
		mov eax, offset light_table
		_emit 0x33
		_emit 0xC9
		_emit 0x39
		_emit 0x10
		_emit 0x74
		_emit 0x0E
		ret
	}
}

// An entry of the light table, as the game fills it (type 1 a point light; brightness 0 a lamp turned off)
extern "C" __declspec(dllexport) void set_lamp(int index, const char *id, uint32_t type, const float *colour, const float *position, float range, float brightness)
{
	uint8_t *e = light_table + index * 0x70;
	std::memset(e, 0, 0x70);
	std::memcpy(e, id, 4);
	std::memcpy(e + 8, &type, 4);
	std::memcpy(e + 0xC, colour, 12);
	std::memcpy(e + 0x3C, position, 12);
	std::memcpy(e + 0x54, &range, 4);
	std::memcpy(e + 0x68, &brightness, 4);
}

extern "C" __declspec(dllexport) void set_camera(const float *view, const float *proj, int in_field)
{
	game_object[8] = in_field ? 1 : 0;
	game_object[9] = 1; // set in the field in some zones (Jugner Forest): grass must draw anyway
	std::memcpy(game_object + 0x94, view, 64);
	std::memcpy(game_object + 0x154, proj, 64);
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
	return TRUE;
}
