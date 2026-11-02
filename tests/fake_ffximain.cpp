/*
 * A stand-in FFXiMain.dll for tests/worldfx_core_test.cpp (from tests/e2e.cpp's, 307ff12^, trimmed): just the game's view
 * matrix getter (the bytes World FX's core looks for: mov eax, [gGame]; add eax, 0x94; ret) and a game object with the
 * camera where the game keeps it (+8 in the field, +9 not a zoning flag to trust, +0x94 view, +0x154 projection); the
 * environment's light, behind the code the core finds the environment by. x86 only (naked functions).
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

extern "C" __declspec(dllexport) void set_camera(const float *view, const float *proj, int in_field)
{
	game_object[8] = in_field ? 1 : 0;
	game_object[9] = 1; // set in the field in some zones (Jugner Forest): grass must draw anyway
	std::memcpy(game_object + 0x94, view, 64);
	std::memcpy(game_object + 0x154, proj, 64);
}
