/*
 * End-to-end test of World FX on HiFi XI, without the game (from reshade-worldfx, where it tested the ReShade add-on,
 * until 2026-10-27).
 *
 *   build.cmd e2e        (fills build\e2e-native: this program, HiFi XI's starter as dxgi.dll, hifixi.dll beside it,
 *                         a stand-in FFXiMain.dll, worldfx-grass\100.bin from build\testdata)
 *   cd build\e2e-native & e2e.exe     (WFX_E2E_HOUR=22 or 3 for night)
 *
 * Frames are drawn the way dgVoodoo draws them on Direct3D 12 (on WARP, debug layer on):
 *   world: clear the world target and its depth; a terrain of 12,000 vertices with a root signature
 *   of constants, a root CBV (a tint) and a descriptor table (the world, for the resolve); then, as
 *   dgVoodoo does, its depth plane alone to DEPTH_READ (the stencil plane stays in DEPTH_WRITE: the add-on
 *   mustn't touch it, the debug layer says if it does) and the target to pixel-shader-resource;
 *   resolve: the resolve pipeline is bound *before* moving to the back buffer, so the add-on's grass
 *   goes in at the move and the resolve's draw relies on its state being put back.
 * The camera is set in the stand-in FFXiMain.dll where the game keeps it; the zone comes in through
 * FFXI_WORLDFX_ZONE as from the worldfx Ashita addon.
 * Checks on the last frame: terrain, sky and blade pixels, all through the resolve's tint (0.5); no
 * debug-layer errors; the add-on's "first grass draw" line in ReShade.log.
 * Then far from any grass, with the shading pass on (ambient occlusion): nothing but the shading pass is
 * drawn, and the resolve after it still draws the world through its tint (the game's state was put back:
 * left bound, the game's next draws hung the GPU). Last, the device is removed (ID3D12Device5::RemoveDevice)
 * and the add-on's GPU crash report has to appear (worldfx-gpu-crash.txt).
 * Zone 100's meshes for the sun's shadow map (worldfx-shadow\100.bin, written here: one small card 100 yalms
 * off, its shadow off the terrain) are loaded too: the map is drawn and read from the first frames on the grass,
 * drawn again far from it, and the pictures and the debug layer stay as they are.
 * Zone 100's lamps (worldfx-lamps\100.bin, written here: a street lamp and a whole area's light where the camera
 * looks far from the grass): the stand-in FFXiMain.dll has the game's light table behind the code the Ashita addon
 * finds it by; read as the addon reads it, lit, the street lamp glows on the ground there, turned off, nothing.
 * The weather: the stand-in FFXiMain.dll has the environment's weather object and record, the zone renderer's fog and
 * the weather's id behind the code the Ashita addon finds them by; read as the addon reads them, the blades take the
 * fog's colour past where it ends, rain takes the sun's shadows away (half of them halfway through a change) and wets
 * the ground in zone 100's wet mask (worldfx-wet\100.bin, written here: the far half of the terrain on screen; its
 * ReShade.ini turns WetGround on), the add-on rains in rain and snows in snow and blizzards (on zone 100's rain map,
 * worldfx-rain\100.bin, written here), and the weather not sent lately leaves everything as before.
 * The weather of its own (with the sky of its own, under open sky): all of zone 100's weather World FX's own
 * (worldfx_weather_drawn's bits for each weather, the parts it has: worldfx_weather_fx_stats), clouds over the sky (more
 * in clouds than in fine weather), a lightning flash brightening the frame and gone after, dust tinting the terrain, and
 * the bits with a part turned off, WeatherOwn or the sky off, and in a zone without open sky (worldfx-sunlit.txt). The
 * settings are changed as a menu changes them (worldfx_set_setting).
 */

#include "grass_render.hpp" // (HiFi XI's render engine: src\render)

// HiFi XI's starter as dxgi.dll and hifixi.dll beside it, World FX's core on HiFi XI's own hooks, its log hifixi.log,
// and hifixi_present called before each Present (where Ashita has the plugin do its present)
#define WFX_MODULE L"hifixi.dll"
#define WFX_LOG "hifixi.log"
#include <algorithm>
#include <chrono>
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <share.h>
#include <cstring>
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

	template <typename T>
	void release(T *&p)
	{
		if (p != nullptr)
			p->Release();
		p = nullptr;
	}

	// A stand-in for the game's visibility test (FFXiMain 06C280: thiscall, 9 arguments, ret 24h), for the add-on's
	// VisTest entries to fall back on: counts its calls, answers 777 (no real answer is)
	int game_tests = 0;
	int32_t __fastcall game_test(void *, void *, const float *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
	{
		game_tests++;
		return 777;
	}

	const char kShader[] = R"(
cbuffer rc : register(b0) { row_major float4x4 vp; float4 ground; };
cbuffer tint_cb : register(b1) { float4 tint; };
Texture2D<float4> world : register(t0);

float4 terrain_vs(uint id : SV_VertexID) : SV_Position
{
	uint quad = id / 6, corner = id % 6;
	uint cx = (corner == 1 || corner == 4 || corner == 5) ? 1 : 0;
	uint cz = (corner == 2 || corner == 3 || corner == 5) ? 1 : 0;
	float x = ground.x + (quad % 50 + cx) * ground.z;
	float z = ground.y + (quad / 50 + cz) * ground.z;
	return mul(float4(x, ground.w, z, 1.0), vp);
}
float4 terrain_ps() : SV_Target { return float4(0.3, 0.35, 0.2, 1.0); } // grassy: the road filter keeps blades on it

float4 resolve_vs(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 resolve_ps(float4 pos : SV_Position) : SV_Target { return world.Load(int3(pos.xy, 0)) * tint; }
)";

	pD3DCompile compile_fn = nullptr;

	std::vector<uint8_t> compile(const char *entry, const char *target)
	{
		ID3DBlob *code = nullptr, *messages = nullptr;
		std::vector<uint8_t> out;
		if (SUCCEEDED(compile_fn(kShader, sizeof(kShader) - 1, "e2e", nullptr, nullptr, entry, target, 0, 0, &code, &messages)))
			out.assign(static_cast<const uint8_t *>(code->GetBufferPointer()), static_cast<const uint8_t *>(code->GetBufferPointer()) + code->GetBufferSize());
		else
			std::printf("compile %s: %s\n", entry, messages != nullptr ? static_cast<const char *>(messages->GetBufferPointer()) : "?");
		release(code);
		release(messages);
		return out;
	}

	void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to,
		UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = res;
		b.Transition.Subresource = subresource;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		list->ResourceBarrier(1, &b);
	}

	struct vec3
	{
		float x, y, z;
	};
	vec3 sub(vec3 a, vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	vec3 cross(vec3 a, vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	vec3 normalize(vec3 a)
	{
		const float l = std::sqrt(dot(a, a));
		return { a.x / l, a.y / l, a.z / l };
	}
}

int main()
{
	// worldfx.hlsl next to the plugin: the shaders from it (src\\render\\grass.hlsl, run from build\\e2e-native, with a comment
	// added so it's its own file, this run's own: not in the shader cache), checked on the first present and
	// compiled in the background while the frames go on
	{
		std::string source;
		if (FILE *f = nullptr; fopen_s(&f, "..\\..\\src\\render\\grass.hlsl", "rb") == 0 && f != nullptr)
		{
			char buffer[4096];
			size_t n;
			while ((n = fread(buffer, 1, sizeof(buffer), f)) != 0)
				source.append(buffer, n);
			fclose(f);
		}
		source += "\n// (e2e's copy, run " + std::to_string(GetTickCount64()) + ")\n";
		if (FILE *f = nullptr; fopen_s(&f, "worldfx.hlsl", "wb") == 0 && f != nullptr)
		{
			fwrite(source.data(), 1, source.size(), f);
			fclose(f);
		}
	}

	// The add-on's debug-layer mode (D3D12DebugLayer=1): its messages to worldfx-d3d12-debug.txt
	DeleteFileW(L"worldfx-d3d12-debug.txt");
	if (FILE *f = nullptr; fopen_s(&f, "ReShade.ini", "wb") == 0 && f != nullptr)
	{
		// (the Vana'diel hour: 10, the sun up; WFX_E2E_HOUR=22 runs it by night, the stand-in game's light then
		// standing for the moon: the sun's half-size pass must run then too. Characters cast, as Alex plays: the
		// actors sent below go through the sun pass's character shadows. The moon full, day 46 of its 84, whatever the
		// date: its disc whole where the light points)
		char hour[16] = "10";
		if (GetEnvironmentVariableA("WFX_E2E_HOUR", hour, sizeof(hour)) == 0)
			strcpy_s(hour, "10");
		// (the look over the world off: the colour checks below look for the world's colours exactly as drawn; the look's
		// own checks turn it on, as a menu does)
		std::fprintf(f, "[FFXI_WORLDFX]\nD3D12DebugLayer=1\nSunShadows=0.6\nSunCharacters=1\nTestVanadielHour=%s\nTestMoon=0.5476\nTestShadeMaxMs=500\nWetGround=1\nSky=1\n"
			"Tonemap=0\nBloom=0\nColourGrade=0\n", hour);
		fclose(f);
	}
	// HiFi XI keeps its own settings: none yet, so it makes them from ReShade.ini above (once), and leaves that as it is
	std::string reshade_ini_before;
	if (FILE *f = nullptr; fopen_s(&f, "ReShade.ini", "rb") == 0 && f != nullptr)
	{
		char buffer[1024];
		size_t n;
		while ((n = fread(buffer, 1, sizeof(buffer), f)) != 0)
			reshade_ini_before.append(buffer, n);
		fclose(f);
	}
	wchar_t settings_path[MAX_PATH] = L"";
	GetFullPathNameW(L"hifixi-settings.ini", MAX_PATH, settings_path, nullptr);
	DeleteFileW(settings_path);
	SetEnvironmentVariableW(L"HIFIXI_SETTINGS", settings_path);

	// ReShade first (dxgi.dll next to this program), so it can hook d3d12.dll as it loads
	HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
	HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
	HMODULE game = LoadLibraryW(L"FFXiMain.dll");
	compile_fn = reinterpret_cast<pD3DCompile>(GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile"));
	if (dxgi == nullptr || d3d12 == nullptr || game == nullptr || compile_fn == nullptr)
	{
		std::printf("FAIL  run from build\\e2e (dxgi.dll, FFXiMain.dll missing?)\n");
		return 1;
	}
	char dxgi_path[MAX_PATH] = "";
	GetModuleFileNameA(dxgi, dxgi_path, MAX_PATH);
	std::printf("  dxgi.dll: %s\n", dxgi_path);
	DeleteFileW(L"worldfx-gpu-crash.txt");
	SetEnvironmentVariableA("FFXI_WORLDFX_ZONE", "100");
	SetEnvironmentVariableA("FFXI_WORLDFX_INGAME", "1"); // (as the Ashita addon says while a character is in the game)

	// Zone 100's water, for the rings: a 20-yalm square of it half a yalm deep, far from where the camera
	// looks (30,000 yalms off: never drawn, only where the add-on knows there's water)
	CreateDirectoryW(L"worldfx-water", nullptr);
	if (FILE *f = nullptr; _wfopen_s(&f, L"worldfx-water\\100.bin", L"wb") == 0 && f != nullptr)
	{
		const uint32_t header[5] = { 0x32525457 /* 'WTR2' */, 100, 0x42000000 /* 32.0f */, 1, 6 };
		const int32_t cell[4] = { -938, -938, 0, 6 };
		const float x0 = -30000.0f, x1 = -29980.0f, z0 = -30000.0f, z1 = -29980.0f;
		const float corners[6][2] = { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z0 }, { x1, z1 }, { x0, z1 } };
		fwrite(header, sizeof(header), 1, f);
		fwrite(cell, sizeof(cell), 1, f);
		for (const auto &c : corners)
		{
			const float record[8] = { c[0], 10.0f, c[1], 8.0f, 9.5f, 5.0f, 0.0f, 0.0f };
			fwrite(record, sizeof(record), 1, f);
		}
		fclose(f);
	}

	// The blade the camera looks at, as in grass_test
	grass::blade_file file;
	std::string error;
	check(grass::read_blade_file(L"worldfx-grass\\100.bin", file, error), "blade file");
	if (file.blade_count() == 0)
		return 1;
	const grass::blade_file::cell *busiest = &file.cells[0];
	for (const grass::blade_file::cell &c : file.cells)
		if (c.count > busiest->count)
			busiest = &c;
	const float *b = &file.blades[size_t(busiest->first) * 4];
	const vec3 at = { b[0], b[1], b[2] };

	// Zone 100's sea (as HiFi XI's bake 'sea' writes it: SEA1, the zone, its sheets, its corners in the water's layout, kind
	// 200, its bed 30 under it, its edge 8 yalms off): one sheet 3,000 yalms square, half a yalm over the ground, 20,000
	// yalms from the grass the other way from the lamps (sea_at: out of reach of every other view here)
	const vec3 sea_at = { at.x + 20000.0f, at.y, at.z - 20000.0f };
	const float sea_level = sea_at.y - 0.5f; // (y is down)
	CreateDirectoryW(L"worldfx-sea", nullptr);
	if (FILE *f = nullptr; _wfopen_s(&f, L"worldfx-sea\\100.bin", L"wb") == 0 && f != nullptr)
	{
		const uint32_t header[4] = { 0x31414553 /* 'SEA1' */, 100, 1, 6 };
		const float x0 = sea_at.x - 1500.0f, x1 = sea_at.x + 1500.0f, z0 = sea_at.z - 1500.0f, z1 = sea_at.z + 1500.0f;
		const float corners[6][2] = { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z0 }, { x1, z1 }, { x0, z1 } };
		fwrite(header, sizeof(header), 1, f);
		for (const auto &c : corners)
		{
			const float record[8] = { c[0], sea_level + 30.0f, c[1], 200.0f, sea_level, 8.0f, 0.0f, 0.0f };
			fwrite(record, sizeof(record), 1, f);
		}
		fclose(f);
	}

	// Zone 100's meshes for the sun's shadow map: one small upright card 100 yalms off (in the far cascade, past
	// the terrain's end: its shadow falls nowhere in the picture)
	CreateDirectoryW(L"worldfx-shadow", nullptr);
	if (FILE *f = nullptr; _wfopen_s(&f, L"worldfx-shadow\\100.bin", L"wb") == 0 && f != nullptr)
	{
		const uint32_t header[8] = { 0x31444853 /* 'SHD1' */, 100, 0x42000000 /* 32.0f */, 1, 2, 0, 0, 0 };
		const int32_t cell[4] = { 0, 0, 0, 2 };
		const float x = at.x + 100.0f, y = at.y, z = at.z;
		const float triangles[2][12] = {
			{ x, y, z, 0.0f, x + 2.0f, y, z, 0.0f, x + 2.0f, y - 3.0f, z, 0.0f },
			{ x, y, z, 0.0f, x + 2.0f, y - 3.0f, z, 0.0f, x, y - 3.0f, z, 0.0f } };
		fwrite(header, sizeof(header), 1, f);
		fwrite(cell, sizeof(cell), 1, f);
		fwrite(triangles, sizeof(triangles), 1, f);
		fclose(f);
	}
	// Zone 100's lamps: a street lamp 3 yalms over the ground where the camera looks far from the grass (20,000
	// yalms off: see there), and a whole area's light (range 30) at the same place, as tools/bake_lamps.py writes them
	const vec3 far_at = { at.x + 20000.0f, at.y, at.z + 20000.0f };
	const float lamp_at[3] = { far_at.x, far_at.y - 3.0f, far_at.z };
	const float lamp_colour[3] = { 1.46875f, 1.0f, 0.21875f };
	CreateDirectoryW(L"worldfx-lamps", nullptr);
	if (FILE *f = nullptr; _wfopen_s(&f, L"worldfx-lamps\\100.bin", L"wb") == 0 && f != nullptr)
	{
		const uint32_t header[5] = { 0x31504D4C /* 'LMP1' */, 100, 0x42800000 /* 64.0f */, 1, 2 };
		const int32_t cell[4] = { int32_t(std::floor(lamp_at[0] / 64.0f)), int32_t(std::floor(lamp_at[2] / 64.0f)), 0, 2 };
		fwrite(header, sizeof(header), 1, f);
		fwrite(cell, sizeof(cell), 1, f);
		const char *ids[2] = { "tl01", "ar01" };
		const float ranges[2] = { 4.0f, 30.0f }, brightness[2] = { 6.0f, 10.0f };
		for (int i = 0; i < 2; ++i)
		{
			const float record[8] = { lamp_at[0], lamp_at[1], lamp_at[2], ranges[i], lamp_colour[0], lamp_colour[1], lamp_colour[2], brightness[i] };
			uint32_t tail[4] = { 0, uint32_t(i + 1), 0, 0 };
			std::memcpy(&tail[0], ids[i], 4);
			fwrite(record, sizeof(record), 1, f);
			fwrite(tail, sizeof(tail), 1, f);
		}
		fclose(f);
	}

	// Zone 100's wet mask (as tools/bake_wet.py writes it): path or stone over the grass scene's terrain where x is less
	// than a yalm past the looked-at blade (the far half of the ground on screen; the near half, by the camera, isn't)
	CreateDirectoryW(L"worldfx-wet", nullptr);
	if (FILE *f = nullptr; _wfopen_s(&f, L"worldfx-wet\\100.bin", L"wb") == 0 && f != nullptr)
	{
		const uint32_t width = 200, height = 160, words_a_row = (width + 31) / 32, wet_squares = 102;
		const float square = 0.5f, x0 = at.x - 50.0f, z0 = at.z - 40.0f;
		uint32_t header[9] = { 0x31544557 /* 'WET1' */, 100, 0, width, wet_squares * height, height, 0, 0, words_a_row };
		std::memcpy(&header[2], &square, 4);
		std::memcpy(&header[6], &x0, 4);
		std::memcpy(&header[7], &z0, 4);
		fwrite(header, sizeof(header), 1, f);
		std::vector<uint32_t> row(words_a_row, 0u);
		for (uint32_t i = 0; i < wet_squares; ++i)
			row[i >> 5] |= 1u << (i & 31);
		for (uint32_t j = 0; j < height; ++j)
			fwrite(row.data(), 4, row.size(), f);
		fclose(f);
	}

	// Zone 100's rain map (as tools/bake_rain.py writes it, stored rather than packed): flat ground at the looked-at
	// blade's height over the grass scene's terrain
	CreateDirectoryW(L"worldfx-rain", nullptr);
	if (FILE *f = nullptr; _wfopen_s(&f, L"worldfx-rain\\100.bin", L"wb") == 0 && f != nullptr)
	{
		const uint32_t width = 200, height = 160;
		const float square = 0.5f, x0 = at.x - 50.0f, z0 = at.z - 40.0f, y0 = at.y, step = 1.0f / 16.0f;
		uint32_t header[12] = { 0x314D4852 /* 'RHM1' */, 100, 0, width, width * height, height, 0, 0, 0, 0, 0 /* stored */, width * height * 3 };
		std::memcpy(&header[2], &square, 4);
		std::memcpy(&header[6], &x0, 4);
		std::memcpy(&header[7], &z0, 4);
		std::memcpy(&header[8], &y0, 4);
		std::memcpy(&header[9], &step, 4);
		fwrite(header, sizeof(header), 1, f);
		const std::vector<uint8_t> kinds(size_t(width) * height, 2), deltas(size_t(width) * height * 2, 0);
		fwrite(kinds.data(), 1, kinds.size(), f);
		fwrite(deltas.data(), 1, deltas.size(), f);
		fclose(f);
	}

	// Each zone's clear sky (as tools/bake_clear_sky.py writes it): zone 100's sun light all day as the weather records'
	// below have it in sunshine (colour 0x80 0x80 0x80, light intensity 1.5)
	if (FILE *f = nullptr; _wfopen_s(&f, L"worldfx-clearsky.txt", L"wb") == 0 && f != nullptr)
	{
		std::fprintf(f, "# e2e\n100");
		for (int h = 0; h < 24; ++h)
			std::fprintf(f, " %.6f", 128.0 / 255.0 * 1.5);
		std::fprintf(f, "\n");
		fclose(f);
	}

	const vec3 eye = { at.x + 4.0f, at.y - 2.0f, at.z + 3.0f };
	const vec3 z = normalize(sub(eye, at)), x = normalize(cross({ 0.0f, -1.0f, 0.0f }, z)), y = cross(z, x);
	const float view[16] = { x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1 };
	const UINT W = 640, H = 360;
	const float proj[16] = { 1.26f * H / W, 0, 0, 0, 0, 1.26f, 0, 0, 0, 0, -1, -1, 0, 0, -0.1f, 0 };
	using set_camera_fn = void (*)(const float *, const float *, int);
	reinterpret_cast<set_camera_fn>(GetProcAddress(game, "set_camera"))(view, proj, 1);
	// The game's light, toward the ground (the add-on's sun is toward it: turned round)
	using set_light_fn = void (*)(const float *);
	const float light[3] = { 0.3f, 0.8f, 0.2f };
	reinterpret_cast<set_light_fn>(GetProcAddress(game, "set_light"))(light);

	// Device on WARP with the debug layer, through ReShade
	ID3D12Debug *debug = nullptr;
	if (SUCCEEDED(reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(d3d12, "D3D12GetDebugInterface"))(IID_PPV_ARGS(&debug))))
		debug->EnableDebugLayer();
	using create_factory_fn = HRESULT(WINAPI *)(REFIID, void **);
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	ID3D12Device *device = nullptr;
	reinterpret_cast<create_factory_fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"))(IID_PPV_ARGS(&factory));
	factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
	if (FAILED(reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"))(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
	{
		std::printf("FAIL  no device\n");
		return 1;
	}
	ID3D12InfoQueue *info = nullptr;
	device->QueryInterface(IID_PPV_ARGS(&info));
	check(info != nullptr, "debug layer on");

	ID3D12CommandQueue *queue = nullptr;
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
	ID3D12CommandAllocator *allocator = nullptr;
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
	ID3D12GraphicsCommandList *list = nullptr;
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
	list->Close();
	ID3D12Fence *fence = nullptr;
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	UINT64 fence_value = 0;

	// A window and a swap chain (ReShade makes its runtime, and the add-on finds the device, from it)
	WNDCLASSW wc = {};
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"worldfx_e2e";
	RegisterClassW(&wc);
	HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"worldfx e2e", WS_OVERLAPPEDWINDOW, 0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);
	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = W;
	sd.Height = H;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	IDXGISwapChain1 *swapchain1 = nullptr;
	IDXGISwapChain3 *swapchain = nullptr;
	factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &swapchain1);
	swapchain1->QueryInterface(IID_PPV_ARGS(&swapchain));
	check(swapchain != nullptr, "swap chain");
	ID3D12Resource *back[2] = {};
	for (UINT i = 0; i < 2; ++i)
		swapchain->GetBuffer(i, IID_PPV_ARGS(&back[i]));

	// The world: target, depth, and an SRV of the target for the resolve
	D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, upload = { D3D12_HEAP_TYPE_UPLOAD }, readback = { D3D12_HEAP_TYPE_READBACK };
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = W;
	td.Height = H;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.SampleDesc.Count = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	const float sky[4] = { 0.55f, 0.6f, 0.7f, 1.0f }; // bright and greyish, like the game's fog (brighter than its ground)
	D3D12_CLEAR_VALUE rt_clear = { DXGI_FORMAT_R8G8B8A8_UNORM, { sky[0], sky[1], sky[2], sky[3] } };
	ID3D12Resource *world = nullptr, *depth = nullptr;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &rt_clear, IID_PPV_ARGS(&world));
	td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	D3D12_CLEAR_VALUE ds_clear = { DXGI_FORMAT_D24_UNORM_S8_UINT };
	ds_clear.DepthStencil.Depth = 1.0f;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &ds_clear, IID_PPV_ARGS(&depth));

	ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr, *srv_heap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap));
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap));
	const UINT rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE rtv[3];
	for (UINT i = 0; i < 3; ++i)
		rtv[i] = { rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * rtv_step };
	device->CreateRenderTargetView(back[0], nullptr, rtv[0]);
	device->CreateRenderTargetView(back[1], nullptr, rtv[1]);
	device->CreateRenderTargetView(world, nullptr, rtv[2]);
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
	device->CreateDepthStencilView(depth, nullptr, dsv);
	device->CreateShaderResourceView(world, nullptr, srv_heap->GetCPUDescriptorHandleForHeapStart());

	// The tint (root CBV) and a readback buffer
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = 256;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *tint = nullptr, *rb = nullptr;
	device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tint));
	const float tint_value[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
	void *mapped = nullptr;
	tint->Map(0, nullptr, &mapped);
	std::memcpy(mapped, tint_value, 16);
	tint->Unmap(0, nullptr);
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
	UINT64 rb_size = 0;
	const D3D12_RESOURCE_DESC back_desc = back[0]->GetDesc();
	device->GetCopyableFootprints(&back_desc, 0, 1, 0, &fp, nullptr, nullptr, &rb_size);
	bd.Width = rb_size;
	device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));

	// The "game's" root signature (constants, root CBV, a table) and its two pipelines
	D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
	D3D12_ROOT_PARAMETER params[3] = {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants = { 0, 0, 20 };
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].Descriptor = { 1, 0 };
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable = { 1, &range };
	D3D12_ROOT_SIGNATURE_DESC rsd = { 3, params };
	ID3DBlob *blob = nullptr, *blob_error = nullptr;
	reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &blob_error);
	ID3D12RootSignature *root = nullptr;
	device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
	release(blob);
	release(blob_error);

	const std::vector<uint8_t> tvs = compile("terrain_vs", "vs_5_0"), tps = compile("terrain_ps", "ps_5_0");
	const std::vector<uint8_t> rvs = compile("resolve_vs", "vs_5_0"), rps = compile("resolve_ps", "ps_5_0");
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
	pd.pRootSignature = root;
	pd.VS = { tvs.data(), tvs.size() };
	pd.PS = { tps.data(), tps.size() };
	pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pd.SampleMask = UINT_MAX;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.RasterizerState.DepthClipEnable = TRUE;
	pd.DepthStencilState.DepthEnable = TRUE;
	pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 1;
	pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	pd.SampleDesc.Count = 1;
	ID3D12PipelineState *terrain_pso = nullptr, *resolve_pso = nullptr;
	device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&terrain_pso));
	pd.VS = { rvs.data(), rvs.size() };
	pd.PS = { rps.data(), rps.size() };
	pd.DepthStencilState.DepthEnable = FALSE;
	pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
	device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&resolve_pso));
	check(terrain_pso != nullptr && resolve_pso != nullptr, "the game's pipelines");

	float constants[20];
	grass::multiply(view, proj, constants);
	constants[16] = at.x - 50.0f; // terrain: 50 x 40 quads of 2 yalms around the blade, at its height
	constants[17] = at.z - 40.0f;
	constants[18] = 2.0f;
	constants[19] = at.y;

	UINT frame = 0;
	double longest_frame_s = 0.0; // (the longest a frame took, after the first: while worldfx.hlsl compiles, too)
	auto render = [&](bool capture) {
		const auto frame_start = std::chrono::steady_clock::now();
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			DispatchMessageW(&msg);

		const UINT bi = swapchain->GetCurrentBackBufferIndex();
		allocator->Reset();
		list->Reset(allocator, nullptr);
		barrier(list, back[bi], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		if (frame != 0)
		{
			barrier(list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			barrier(list, depth, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE, 0);
		}

		// The world
		list->OMSetRenderTargets(1, &rtv[2], FALSE, &dsv);
		list->ClearRenderTargetView(rtv[2], sky, 0, nullptr);
		list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		ID3D12DescriptorHeap *heaps[] = { srv_heap };
		list->SetDescriptorHeaps(1, heaps);
		list->SetGraphicsRootSignature(root);
		list->SetGraphicsRoot32BitConstants(0, 20, constants, 0);
		list->SetGraphicsRootConstantBufferView(1, tint->GetGPUVirtualAddress());
		list->SetGraphicsRootDescriptorTable(2, srv_heap->GetGPUDescriptorHandleForHeapStart());
		list->SetPipelineState(terrain_pso);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		const D3D12_VIEWPORT vp = { 0, 0, float(W), float(H), 0, 1 };
		const D3D12_RECT sc = { 0, 0, LONG(W), LONG(H) };
		list->RSSetViewports(1, &vp);
		list->RSSetScissorRects(1, &sc);
		list->DrawInstanced(12000, 1, 0, 0);
		barrier(list, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ, 0);
		barrier(list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		// The resolve: its pipeline first, then the move to the back buffer (grass goes in here), then its draw
		list->SetPipelineState(resolve_pso);
		list->OMSetRenderTargets(1, &rtv[bi], FALSE, nullptr);
		list->DrawInstanced(3, 1, 0, 0);

		if (capture)
		{
			barrier(list, back[bi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
			dst.PlacedFootprint = fp;
			D3D12_TEXTURE_COPY_LOCATION src = { back[bi], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			barrier(list, back[bi], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
		}
		else
			barrier(list, back[bi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		list->Close();
		ID3D12CommandList *lists[] = { list };
		queue->ExecuteCommandLists(1, lists);
		{
			// (what Ashita has the plugin do at each present)
			using present_fn = void(__cdecl *)();
			static const auto present = reinterpret_cast<present_fn>(GetProcAddress(GetModuleHandleW(WFX_MODULE), "hifixi_present"));
			if (present != nullptr)
				present();
		}
		swapchain->Present(0, 0);
		queue->Signal(fence, ++fence_value);
		fence->SetEventOnCompletion(fence_value, fence_event);
		WaitForSingleObject(fence_event, INFINITE);
		if (frame != 0)
			longest_frame_s = std::max(longest_frame_s, std::chrono::duration<double>(std::chrono::steady_clock::now() - frame_start).count());
		frame++;
	};

	// The last frame's picture: sky, terrain and blades, each times the tint. By night (the sun below the horizon at
	// ReShade.ini's Vana'diel hour, as the add-on works it out) also fireflies: drawn over whatever is behind them, the
	// sky too (they fly in front of it), each adding its yellow-green light: green up, red up about as much, blue up
	// far less (the core 1 0.95 0.6, the glow 0.55 0.85 0.2), where a star's light is whitish
	char e2e_hour_text[16] = "10";
	GetEnvironmentVariableA("WFX_E2E_HOUR", e2e_hour_text, sizeof(e2e_hour_text));
	const double e2e_up = std::sin(3.14159265358979 * (std::strtod(e2e_hour_text, nullptr) - 6.0) / 12.0);
	const bool e2e_night = e2e_up < 0.0;
	auto firefly_light = [&](const uint8_t *px, double r, double g, double b) {
		const double dr = px[0] - r, dg = px[1] - g, db = px[2] - b;
		return e2e_night && dg >= 1.0 && dr >= -1.0 && db >= -1.0 && db <= 0.75 * dg + 1.0 && dr <= 1.25 * dg + 2.0;
	};
	std::vector<uint8_t> pixels(size_t(W) * H * 4);
	uint32_t sky_px = 0, terrain_px = 0, blade_px = 0, firefly_px = 0, other = 0;
	auto read_picture = [&]() {
		uint8_t *p = nullptr;
		rb->Map(0, nullptr, reinterpret_cast<void **>(&p));
		for (UINT row = 0; row < H; ++row)
			std::memcpy(&pixels[size_t(row) * W * 4], p + size_t(row) * fp.Footprint.RowPitch, size_t(W) * 4);
		rb->Unmap(0, nullptr);
		auto near_colour = [](const uint8_t *px, float r, float g, float b) {
			return std::abs(px[0] - r * 255.0f) <= 2.0f && std::abs(px[1] - g * 255.0f) <= 2.0f && std::abs(px[2] - b * 255.0f) <= 2.0f;
		};
		sky_px = terrain_px = blade_px = firefly_px = other = 0;
		for (size_t i = 0; i < size_t(W) * H; ++i)
		{
			const uint8_t *px = &pixels[i * 4];
			if (near_colour(px, 0.275f, 0.3f, 0.35f))
				sky_px++;
			else if (near_colour(px, 0.15f, 0.175f, 0.1f))
				terrain_px++;
			else if (px[0] > 0 && px[2] <= px[1]) // the ground's colour turned green (darker and browner at the root), through the tint
				blade_px++;
			else if (firefly_light(px, 0.275 * 255.0, 0.3 * 255.0, 0.35 * 255.0) || firefly_light(px, 0.15 * 255.0, 0.175 * 255.0, 0.1 * 255.0))
				firefly_px++;
			else
			{
				if (other < 6)
					std::printf("  other pixel %zu,%zu: %u %u %u\n", i % W, i / W, px[0], px[1], px[2]);
				other++;
			}
		}
		std::printf("  last frame: %u sky, %u terrain, %u blade, %u firefly, %u other pixels\n", sky_px, terrain_px, blade_px, firefly_px, other);
	};
	using last_draw_fn = void(__cdecl *)(uint32_t *);
	const auto last_draw = reinterpret_cast<last_draw_fn>(GetProcAddress(GetModuleHandleW(WFX_MODULE), "worldfx_last_draw"));
	check(last_draw != nullptr, "the add-on loaded (worldfx_last_draw)");
	uint32_t stats[5] = {}; // (the sun's shadows on: ReShade.ini fixes the Vana'diel hour at 10)
	using sun_map_fn = void(__cdecl *)(uint32_t *);
	const auto sun_map = reinterpret_cast<sun_map_fn>(GetProcAddress(GetModuleHandleW(WFX_MODULE), "worldfx_sun_map"));
	check(sun_map != nullptr, "the add-on's shadow map export (worldfx_sun_map)");
	uint32_t map[2] = {};

	// On the grass: grass and the shading pass from the first frames the game says a character is in
	// (FFXI_WORLDFX_INGAME), no waiting (the zone's blades loaded at the first frame with the zone set)
	while (frame < 200)
	{
		render(false);
		if (last_draw != nullptr)
			last_draw(stats);
		if (stats[1] != 0 && stats[2] != 0)
			break;
	}
	const UINT first_drawn = frame;
	render(true);
	read_picture();
	if (last_draw != nullptr)
		last_draw(stats);
	std::printf("  on the grass: %u blades in %u draws, shading pass %s, sun shadows %s\n", stats[0], stats[1],
		stats[2] ? "ran" : "didn't run", stats[4] ? "worked out" : "not");
	check(stats[1] != 0 && stats[2] != 0, "grass and the shading pass both drawn");

	// The game's visibility tests through the add-on's entries (VisTest: worldfx.lua 0.15 points the game's calls at
	// worldfx_vis_entry's), each asked every frame as the game asks them: a box of sky at the sky's depth (the lens
	// flare's: its 8th argument 0), one of terrain at that depth, the same in front of everything, and one off the
	// screen. At first the game's own test answers (no answers yet); a few frames on, the depth copy's: the sky's all
	// there (256), the terrain hides the sky's depth (0) and not the nearest (256); off the screen 0 at once, never the
	// game's. Once answers are coming in, an object new in view is answered hidden (0) at once and its own a few frames
	// on, never by the game's test. Turned off, the game's test again.
	{
		const HMODULE addon = GetModuleHandleW(WFX_MODULE);
		using vis_entry_fn = uintptr_t(__cdecl *)(uint32_t);
		using vis_setup_fn = void(__cdecl *)(uintptr_t, uint32_t);
		using vis_stats_fn = void(__cdecl *)(uint32_t *);
		using test_fn = int32_t(__fastcall *)(void *, void *, const float *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
		const auto vis_entry = reinterpret_cast<vis_entry_fn>(GetProcAddress(addon, "worldfx_vis_entry"));
		const auto vis_setup = reinterpret_cast<vis_setup_fn>(GetProcAddress(addon, "worldfx_vis_setup"));
		const auto vis_stats = reinterpret_cast<vis_stats_fn>(GetProcAddress(addon, "worldfx_vis_stats"));
		check(vis_entry != nullptr && vis_setup != nullptr && vis_stats != nullptr, "the add-on's visibility test exports");
		// Blocks of the last picture all sky, and with no sky in them (the terrain, its grass on it)
		const int kBlock = 24;
		int sky_x = -1, sky_y = -1, ground_x = -1, ground_y = -1;
		auto is_sky = [&](int x, int y) {
			const uint8_t *px = &pixels[(size_t(y) * W + x) * 4];
			return std::abs(px[0] - 0.275f * 255.0f) <= 2.0f && std::abs(px[1] - 0.3f * 255.0f) <= 2.0f && std::abs(px[2] - 0.35f * 255.0f) <= 2.0f;
		};
		for (int by = 0; by + kBlock <= int(H) && (sky_x < 0 || ground_x < 0); by += kBlock)
			for (int bx = 0; bx + kBlock <= int(W); bx += kBlock)
			{
				int sky_n = 0;
				for (int py = by; py < by + kBlock; ++py)
					for (int px = bx; px < bx + kBlock; ++px)
						sky_n += is_sky(px, py) ? 1 : 0;
				if (sky_n == kBlock * kBlock && sky_x < 0)
					sky_x = bx, sky_y = by;
				if (sky_n == 0 && ground_x < 0)
					ground_x = bx, ground_y = by;
			}
		check(sky_x >= 0 && ground_x >= 0, "a block of sky and one of terrain in the picture");
		if (vis_entry != nullptr && vis_setup != nullptr && vis_stats != nullptr && sky_x >= 0 && ground_x >= 0)
		{
			const auto glow = reinterpret_cast<test_fn>(vis_entry(0));
			const auto fade = reinterpret_cast<test_fn>(vis_entry(1));
			const auto flare = reinterpret_cast<test_fn>(vis_entry(2));
			const auto door = reinterpret_cast<test_fn>(vis_entry(3));
			const auto actor = reinterpret_cast<test_fn>(vis_entry(4));
			check(glow != nullptr && fade != nullptr && flare != nullptr && door != nullptr && actor != nullptr && vis_entry(5) == 0,
				"an entry for each of the five kinds (characters' too), none for a sixth");
			const float fw = float(W), fh = float(H);
			auto ask = [&](test_fn entry, uintptr_t object, float x, float y, float d, uint32_t use_depth) {
				const float rect[4] = { x, y, x + kBlock, y + kBlock };
				uint32_t depth_bits, w_bits, h_bits;
				std::memcpy(&depth_bits, &d, 4);
				std::memcpy(&w_bits, &fw, 4);
				std::memcpy(&h_bits, &fh, 4);
				return entry(reinterpret_cast<void *>(object), nullptr, rect, depth_bits, w_bits, h_bits, 0, 0, 1, use_depth, 0);
			};
			vis_setup(reinterpret_cast<uintptr_t>(&game_test), 1);
			int32_t first[5] = {}, last[5] = {};
			int answered_at = -1;
			const int tests_before = game_tests;
			for (int f = 0; f < 60; ++f)
			{
				last[0] = ask(flare, 0x5000, float(sky_x), float(sky_y), 0.5f, 0);   // (the depth ignored: the sky's 0.99999988)
				last[1] = ask(glow, 0x5010, float(ground_x), float(ground_y), 0.5f, 0);
				last[2] = ask(fade, 0x5020, float(ground_x), float(ground_y), 0.0f, 1);
				last[3] = ask(door, 0x5030, fw + 100.0f, float(ground_y), 0.0f, 1); // off the screen
				last[4] = ask(actor, 0x5050, float(ground_x), float(ground_y), 0.0f, 1); // (a character in front of the terrain)
				if (f == 0)
					std::memcpy(first, last, sizeof(first));
				render(false);
				if (answered_at < 0 && last[0] != 777 && last[1] != 777 && last[2] != 777 && last[4] != 777)
					answered_at = f;
				if (answered_at >= 0 && f >= answered_at + 3)
					break;
			}
			uint32_t vs[12] = {};
			vis_stats(vs);
			std::printf("  visibility tests: the game's own answered the first frame (%d %d %d), the depth copy's from frame %d: sky %d, terrain %d, in front %d, off the screen %d; %u from the copy, %u by the game, %u boxes, %u answers back\n",
				first[0], first[1], first[2], answered_at, last[0], last[1], last[2], last[3], vs[0], vs[1], vs[2], vs[3]);
			check(first[0] == 777 && first[1] == 777 && first[2] == 777 && first[3] == 0, "no answers yet: the game's own test runs (off the screen: 0 at once)");
			check(answered_at > 0 && answered_at < 20, "answers from the depth copy within a few frames");
			check(last[0] == 256 && last[1] == 0 && last[2] == 256 && last[3] == 0, "sky visible, terrain hiding the sky's depth, nothing hiding the nearest, off the screen 0");
			check(first[4] == 777 && last[4] == 256, "a character's test: the game's own first, then the depth copy's (in front of the terrain: 256)");
			check(vs[11] == 1 && vs[2] != 0 && vs[3] != 0 && vs[7] != 0 && vs[8] != 0 && vs[9] != 0, "the counts: on, boxes tested, answers back, asked by glows, fades and the flare");
			const int by_game = game_tests - tests_before;
			check(by_game >= 3 && by_game <= 3 * (answered_at + 1), "the game's test ran only until the answers came (its calls: as many as the frames before them)");
			// An object new in view once answers are coming in: hidden at once (vistest::kUnknown), never the game's
			// waiting test; its own answer a few frames on
			const int tests_now = game_tests;
			const int32_t new_first = ask(glow, 0x5040, float(sky_x), float(sky_y), 0.5f, 0);
			int new_at = -1;
			for (int f = 0; f < 20 && new_at < 0; ++f)
			{
				render(false);
				if (ask(glow, 0x5040, float(sky_x), float(sky_y), 0.5f, 0) == 256)
					new_at = f;
			}
			std::printf("  a new object once answers come in: %d at once, its own answer after %d frames\n", new_first, new_at + 1);
			check(new_first == 0 && game_tests == tests_now, "an object new once answers come in: hidden at once (0), the game's test not run for it");
			check(new_at >= 0 && game_tests == tests_now, "  its own answer from the depth copy a few frames on (sky: 256), still never the game's test");
			vis_setup(0, 0);
			check(ask(glow, 0x5010, float(ground_x), float(ground_y), 0.5f, 0) == 777, "turned off: the game's test again");
		}
	}

	// The worldfx addon 0.14's exports: who's fighting, and weapon skills' cuts. Someone fighting on the looked-at blade
	// and a cut through it, over a few frames: the grass still drawn (its wear on, as by default)
	{
		const HMODULE addon = GetModuleHandleW(WFX_MODULE);
		using actors2_fn = void(__cdecl *)(const float *, const uint32_t *, uint32_t);
		using cut_fn = void(__cdecl *)(const float *, uint32_t);
		const auto send2 = reinterpret_cast<actors2_fn>(GetProcAddress(addon, "worldfx_grass_actors2"));
		const auto send_cut = reinterpret_cast<cut_fn>(GetProcAddress(addon, "worldfx_grass_cut"));
		check(send2 != nullptr && send_cut != nullptr, "the add-on's exports for who's fighting and weapon skills' cuts");
		if (send2 != nullptr && send_cut != nullptr && last_draw != nullptr)
		{
			const float fighter[4] = { at.x, at.y, at.z, 1.0f };
			const uint32_t engaged[1] = { 1 };
			const float swath[7] = { at.x - 2.0f, at.y, at.z, 1.0f, at.x + 2.0f, at.y, at.z };
			for (int i = 0; i < 6; ++i)
			{
				send2(fighter, engaged, 1);
				if (i == 1)
					send_cut(swath, 1);
				render(false);
			}
			uint32_t after[5] = {};
			last_draw(after);
			check(after[0] != 0 && after[1] != 0, "grass still drawn with someone fighting in it and a cut through it");
			send2(nullptr, nullptr, 0);
		}
	}
	std::printf("  first drawn at frame %u\n", first_drawn);
	check(first_drawn <= 5, "drawn from the first frames in the game (no waiting)");
	check(stats[4] != 0, "the sun's shadows worked out (their half-size pass)");
	check(terrain_px > 1000 && sky_px > 1000, "the resolve drew the world with its tint (its state was put back)");
	check(blade_px > 500, "blades in the world, through the resolve");
	check(other == 0, "no pixel of any other colour");
	if (sun_map != nullptr)
		sun_map(map);
	std::printf("  the sun's shadow map: %s last frame, drawn %u times\n", map[0] ? "read" : "not read", map[1]);
	check(map[0] != 0 && map[1] >= 1, "the sun's shadow map drawn from the zone's meshes and read");
	if (FILE *f = nullptr; fopen_s(&f, "e2e.ppm", "wb") == 0 && f != nullptr)
	{
		std::fprintf(f, "P6 %u %u 255\n", W, H);
		for (size_t i = 0; i < size_t(W) * H; ++i)
			std::fwrite(&pixels[i * 4], 1, 3, f);
		fclose(f);
	}

	// Far from any grass (20,000 yalms off): the shading pass alone, and the resolve after it
	{
		const vec3 far_eye = { far_at.x + 4.0f, far_at.y - 2.0f, far_at.z + 3.0f };
		const vec3 fz = normalize(sub(far_eye, far_at)), fx = normalize(cross({ 0.0f, -1.0f, 0.0f }, fz)), fy = cross(fz, fx);
		const float far_view[16] = { fx.x, fy.x, fz.x, 0, fx.y, fy.y, fz.y, 0, fx.z, fy.z, fz.z, 0, -dot(fx, far_eye), -dot(fy, far_eye), -dot(fz, far_eye), 1 };
		reinterpret_cast<set_camera_fn>(GetProcAddress(game, "set_camera"))(far_view, proj, 1);
		grass::multiply(far_view, proj, constants);
		constants[16] = far_at.x - 50.0f;
		constants[17] = far_at.z - 40.0f;
		constants[19] = far_at.y;
	}
	for (int i = 0; i < 5; ++i)
		render(false);
	render(true);
	read_picture();
	if (last_draw != nullptr)
		last_draw(stats);
	std::printf("  far from grass: %u blades in %u draws, shading pass %s, game's state %s\n", stats[0], stats[1],
		stats[2] ? "ran" : "didn't run", stats[3] ? "put back" : "not touched");
	check(stats[1] == 0 && stats[2] != 0 && stats[4] != 0, "far from grass: the shading pass alone (with the sun's shadows)");
	check(stats[3] != 0, "far from grass: the game's state put back after the shading pass");
	check(terrain_px > 1000 && sky_px > 1000, "far from grass: the resolve drew the world with its tint (its state was put back)");
	check(blade_px == 0, "far from grass: no blades");
	check(other == 0, "far from grass: no pixel of any other colour");
	const uint32_t drawn_before = map[1];
	if (sun_map != nullptr)
		sun_map(map);
	check(map[0] != 0 && map[1] > drawn_before, "far from grass: the shadow map drawn again around the camera, and read");

	// The look over the world (tone mapping, bloom, the colour grade: off above), turned on as a menu does: the world's
	// colours change through the resolve (the exposure set at once in the zone: its sky and terrain each still one
	// colour), the resolve still draws (the game's state put back), and turned off again the world is as drawn
	{
		using set_setting_fn = uint32_t(__cdecl *)(const char *, float);
		const auto set_look = reinterpret_cast<set_setting_fn>(GetProcAddress(GetModuleHandleW(WFX_MODULE), "worldfx_set_setting"));
		check(set_look != nullptr && set_look("Tonemap", 1.0f) == 1 && set_look("Bloom", 1.0f) == 1 && set_look("ColourGrade", 1.0f) == 1,
			"the look turned on (Tonemap, Bloom, ColourGrade)");
		for (int i = 0; i < 4; ++i)
			render(false);
		render(true);
		read_picture();
		if (last_draw != nullptr)
			last_draw(stats);
		const uint8_t *mid = &pixels[(size_t(H / 2) * W + W / 2) * 4];
		std::printf("  the look on: %u sky, %u terrain, %u other pixels; the middle %u %u %u\n", sky_px, terrain_px, other, mid[0], mid[1], mid[2]);
		check(sky_px + terrain_px < size_t(W) * H / 10, "the look on: the world's colours changed through the resolve");
		check(stats[3] != 0, "the look on: the game's state put back");
		bool dark = true;
		for (size_t i = 0; i < size_t(W) * H; ++i)
			dark = dark && pixels[i * 4] < 140 && pixels[i * 4 + 1] < 140 && pixels[i * 4 + 2] < 140;
		check(dark, "the look on: nothing blown out (the world through the tint stays under 140)");
		if (set_look != nullptr)
		{
			set_look("Tonemap", 0.0f);
			set_look("Bloom", 0.0f);
			set_look("ColourGrade", 0.0f);
		}
		for (int i = 0; i < 2; ++i)
			render(false);
		render(true);
		read_picture();
		check(other == 0 && terrain_px > 1000 && sky_px > 1000, "the look off again: the world as drawn");
	}

	// Characters' own shadows (SunCharacters=1 above), the game's light low from the left (20 degrees up): someone
	// standing in the middle of the picture but not drawn there (nothing of theirs in the depth) casts none (their shape
	// is looked for along each ray and isn't there); someone ten yalms to the left, off the screen's edge, casts their
	// slim capsule's across the terrain; sent no more, none
	{
		const HMODULE addon = GetModuleHandleW(WFX_MODULE);
		using actors_fn = void(__cdecl *)(const float *, uint32_t);
		const auto send = reinterpret_cast<actors_fn>(GetProcAddress(addon, "worldfx_grass_actors"));
		const auto set_light = reinterpret_cast<set_light_fn>(GetProcAddress(game, "set_light"));
		check(send != nullptr, "the add-on's actor export");
		if (send != nullptr)
		{
			const vec3 far_eye = { far_at.x + 4.0f, far_at.y - 2.0f, far_at.z + 3.0f };
			const vec3 right = normalize(cross({ 0.0f, -1.0f, 0.0f }, normalize(sub(far_eye, far_at))));
			const float c = std::cos(0.349f), s = std::sin(0.349f);
			const float low[3] = { right.x * c, s, right.z * c }; // (toward the ground, as the game keeps it: the sun to the left)
			set_light(low);
			const float middle[4] = { far_at.x, far_at.y, far_at.z, 2.0f };
			const float aside[4] = { far_at.x - right.x * 10.0f, far_at.y, far_at.z - right.z * 10.0f, 2.0f };
			auto frames_with = [&](const float *who, uint32_t count) {
				for (int i = 0; i < 4; ++i)
				{
					send(who, count);
					render(false);
				}
				send(who, count);
				render(true);
				read_picture();
				if (last_draw != nullptr)
					last_draw(stats);
				// (the terrain's pixels darker than it's drawn: a tenth darker in green or more; read_picture takes
				// those for blades)
				uint32_t shaded = 0;
				for (size_t i = 0; i < size_t(W) * H; ++i)
					shaded += pixels[i * 4 + 1] < 0.9f * 0.175f * 255.0f && pixels[i * 4 + 2] < pixels[i * 4 + 1] ? 1 : 0;
				return shaded;
			};
			const uint32_t middle_px = frames_with(middle, 1);
			const bool sun_ran = stats[4] != 0;
			const uint32_t aside_px = frames_with(aside, 1);
			if (FILE *f = nullptr; fopen_s(&f, "e2e_characters.ppm", "wb") == 0 && f != nullptr) // (a picture of it for a look)
			{
				std::fprintf(f, "P6 %u %u 255\n", W, H);
				for (size_t i = 0; i < size_t(W) * H; ++i)
					std::fwrite(&pixels[i * 4], 1, 3, f);
				fclose(f);
			}
			const uint32_t none_px = frames_with(nullptr, 0);
			set_light(light);
			std::printf("  characters casting, the light low from the left: %u terrain pixels shaded with someone undrawn in the middle, %u with someone off screen to the left, %u with no one\n",
				middle_px, aside_px, none_px);
			check(sun_ran, "the sun's half-size pass ran with characters near");
			check(middle_px == 0, "someone not drawn in the picture casts no shadow there");
			check(aside_px > 200, "someone off screen casts their capsule's shadow into the picture");
			check(none_px == 0, "sent no more: no shadow");
		}
	}

	// Lamps: the stand-in game's light table found by the code the Ashita addon looks for, and read as it does (the
	// lit point lights, 10 words each; here before every frame) for the add-on's worldfx_lamps. With the baked
	// street lamp lit there, the ground around its foot glows (warm: red up more than blue) and the whole area's
	// light lit with it is left out; with both turned off (brightness 0), no glow at all.
	{
		const HMODULE addon = GetModuleHandleW(WFX_MODULE);
		using lamps_fn = void(__cdecl *)(const uint32_t *, uint32_t);
		using lamp_stats_fn = void(__cdecl *)(uint32_t *);
		using set_lamp_fn = void (*)(int, const char *, uint32_t, const float *, const float *, float, float);
		const auto send_lamps = reinterpret_cast<lamps_fn>(GetProcAddress(addon, "worldfx_lamps"));
		const auto lamp_stats = reinterpret_cast<lamp_stats_fn>(GetProcAddress(addon, "worldfx_lamp_stats"));
		const auto set_lamp = reinterpret_cast<set_lamp_fn>(GetProcAddress(game, "set_lamp"));
		check(send_lamps != nullptr && lamp_stats != nullptr && set_lamp != nullptr, "the add-on's lamp exports, the stand-in game's light table");
		// (8B542404 B8???????? 33C9 3910 740E in the game's code: the table is the dword at +5)
		const uint8_t *table = nullptr;
		{
			const auto base = reinterpret_cast<const uint8_t *>(game);
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_lfanew);
			const uint8_t want[15] = { 0x8B, 0x54, 0x24, 0x04, 0xB8, 0, 0, 0, 0, 0x33, 0xC9, 0x39, 0x10, 0x74, 0x0E };
			const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
			for (WORD i = 0; i < nt->FileHeader.NumberOfSections && table == nullptr; ++i, ++s)
			{
				if ((s->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
					continue;
				const uint8_t *p = base + s->VirtualAddress;
				for (size_t k = 0; k + sizeof(want) <= s->Misc.VirtualSize && table == nullptr; ++k)
				{
					bool same = true;
					for (size_t j = 0; j < sizeof(want) && same; ++j)
						same = (j >= 5 && j < 9) || p[k + j] == want[j];
					if (same)
					{
						uint32_t address = 0;
						std::memcpy(&address, p + k + 5, 4);
						table = reinterpret_cast<const uint8_t *>(uintptr_t(address));
					}
				}
			}
		}
		check(table != nullptr, "the light table found by the game's code (the Ashita addon's pattern)");
		if (send_lamps != nullptr && lamp_stats != nullptr && set_lamp != nullptr && table != nullptr)
		{
			const float no_colour[3] = {}, nowhere[3] = {};
			set_lamp(0, "tl01", 1, lamp_colour, lamp_at, 4.0f, 6.0f);
			set_lamp(1, "ar01", 1, lamp_colour, lamp_at, 30.0f, 10.0f);
			set_lamp(2, "iw01", 3, no_colour, nowhere, 0.0f, 0.0f); // (registered, never lit: as the game leaves them by day)
			// (as worldfx.lua's read_lamps: point lights with a positive, finite brightness)
			auto send = [&]() {
				static uint32_t words[256 * 10];
				uint32_t n = 0;
				for (int i = 0; i < 256; ++i)
				{
					uint32_t e[28];
					std::memcpy(e, table + i * 0x70, sizeof(e));
					if (e[0] != 0 && e[2] == 1 && e[26] > 0 && e[26] < 0x7F800000)
					{
						const uint32_t w[10] = { e[0], 1, e[3], e[4], e[5], e[15], e[16], e[17], e[21], e[26] };
						std::memcpy(&words[n++ * 10], w, sizeof(w));
					}
				}
				send_lamps(words, n);
				return n;
			};
			// (pixels of the ground lit warmer than the terrain through the tint; how much red and blue rose on them on
			// average, and the most red rose)
			auto glowing = [&](double *red, double *blue, double *most) {
				uint32_t n = 0;
				double sum_r = 0.0, sum_b = 0.0;
				*most = 0.0;
				for (size_t i = 0; i < size_t(W) * H; ++i)
				{
					const uint8_t *px = &pixels[i * 4];
					const double dr = px[0] - 0.15 * 255.0, db = px[2] - 0.1 * 255.0;
					const bool sky_here = std::abs(px[0] - 0.275 * 255.0) <= 2.0 && std::abs(px[1] - 0.3 * 255.0) <= 2.0 && std::abs(px[2] - 0.35 * 255.0) <= 2.0;
					if (dr > 3.0 && !sky_here)
					{
						n++;
						sum_r += dr;
						sum_b += db;
						*most = std::max(*most, dr);
					}
				}
				*red = n != 0 ? sum_r / n : 0.0;
				*blue = n != 0 ? sum_b / n : 0.0;
				return n;
			};
			uint32_t sent = 0, ls[5] = {};
			for (int i = 0; i < 4; ++i)
			{
				sent = send();
				render(false);
			}
			send();
			render(true);
			read_picture();
			lamp_stats(ls);
			double red = 0.0, blue = 0.0, most = 0.0;
			const uint32_t lit_px = glowing(&red, &blue, &most);
			std::printf("  lamps: %u lit sent; %u baked, %u of them lit, %u other lights used, %u whole-area lights left out, %u glowing; %u pixels lit warmer (red +%.1f, blue +%.1f on average, red +%.0f at most)\n",
				sent, ls[0], ls[1], ls[2], ls[3], ls[4], lit_px, red, blue, most);
			check(sent == 2 && ls[0] == 2 && ls[1] == 1 && ls[3] == 1 && ls[4] == 1, "the lit street lamp glows; the whole area's light is left out");
			check(lit_px > 2000 && red > 2.0 * blue && most > 15.0, "its warm glow on the ground around its foot");
			check(sky_px > 1000, "the sky left as it was");
			set_lamp(0, "tl01", 1, lamp_colour, lamp_at, 4.0f, 0.0f);
			set_lamp(1, "ar01", 1, lamp_colour, lamp_at, 30.0f, 0.0f);
			for (int i = 0; i < 3; ++i)
			{
				sent = send();
				render(false);
			}
			send();
			render(true);
			read_picture();
			lamp_stats(ls);
			const uint32_t off_px = glowing(&red, &blue, &most);
			std::printf("  lamps turned off: %u lit sent, %u glowing; %u pixels lit warmer, %u terrain, %u other\n", sent, ls[4], off_px, terrain_px, other);
			check(sent == 0 && ls[1] == 0 && ls[4] == 0 && off_px == 0 && other == 0, "turned off in the game (brightness 0): no glow");
			check(terrain_px > 1000 && sky_px > 1000, "the world through the resolve as before (the game's state put back)");

			// Glows (worldfx_glows, as the Ashita addon 0.10 sends them, every other frame): a Home Point's (kind 2,
			// blue-cyan, 1.6 yalms up where the lamp's foot is) lights the ground there blue; an effect's (kind 3, a fire's
			// orange) by the street lamp lit again is left to the lamp; with the lamp off, the effect's glows; sent none,
			// or not sent for over a second, nothing.
			using glows_fn = void(__cdecl *)(const uint32_t *, uint32_t);
			using glow_stats_fn = void(__cdecl *)(uint32_t *);
			const auto send_glows = reinterpret_cast<glows_fn>(GetProcAddress(addon, "worldfx_glows"));
			const auto glow_stats = reinterpret_cast<glow_stats_fn>(GetProcAddress(addon, "worldfx_glow_stats"));
			check(send_glows != nullptr && glow_stats != nullptr, "the add-on's glow exports");
			if (send_glows != nullptr && glow_stats != nullptr)
			{
				auto glow = [&](uint32_t kind, const float colour[3], float range, float brightness, uint32_t *out) {
					const float f[8] = { colour[0], colour[1], colour[2], lamp_at[0], lamp_at[1] + 3.0f - 1.6f, lamp_at[2], range, brightness };
					out[0] = 0x46000005;
					out[1] = kind;
					std::memcpy(out + 2, f, sizeof(f));
				};
				// (pixels of the ground lit bluer than the terrain through the tint, and how much blue and red rose on them)
				auto bluer = [&](double *red, double *blue) {
					uint32_t n = 0;
					double sum_r = 0.0, sum_b = 0.0;
					for (size_t i = 0; i < size_t(W) * H; ++i)
					{
						const uint8_t *px = &pixels[i * 4];
						const double dr = px[0] - 0.15 * 255.0, db = px[2] - 0.1 * 255.0;
						const bool sky_here = std::abs(px[0] - 0.275 * 255.0) <= 2.0 && std::abs(px[1] - 0.3 * 255.0) <= 2.0 && std::abs(px[2] - 0.35 * 255.0) <= 2.0;
						if (db > 3.0 && !sky_here)
						{
							n++;
							sum_r += dr;
							sum_b += db;
						}
					}
					*red = n != 0 ? sum_r / n : 0.0;
					*blue = n != 0 ? sum_b / n : 0.0;
					return n;
				};
				const float home_point[3] = { 0.35f, 0.75f, 1.0f }, fire[3] = { 1.0f, 0.5f, 0.1f };
				uint32_t words[2 * 10] = {}, gs[5] = {};
				glow(2, home_point, 7.0f, 6.0f, words);
				for (int i = 0; i < 4; ++i)
				{
					send();
					if (i % 2 == 0)
						send_glows(words, 1);
					render(false);
				}
				send();
				render(true);
				read_picture();
				glow_stats(gs);
				double r_rise = 0.0, b_rise = 0.0;
				const uint32_t blue_px = bluer(&r_rise, &b_rise);
				std::printf("  a Home Point's glow: %u held (%u fixtures), %u used, %u lights drawn; %u pixels lit bluer (blue +%.1f, red +%.1f on average)\n",
					gs[0], gs[1], gs[2], gs[4], blue_px, b_rise, r_rise);
				check(gs[0] == 1 && gs[1] == 1 && gs[2] == 1 && gs[4] == 1, "a Home Point's glow goes to the renderer (the lamps off)");
				check(blue_px > 2000 && b_rise > 1.5 * r_rise, "and lights the ground round it blue (on its brownish ground: the street lamp's raises red 9 times blue)");

				glow(3, fire, 5.0f, 6.0f, words);
				set_lamp(0, "tl01", 1, lamp_colour, lamp_at, 4.0f, 6.0f);
				for (int i = 0; i < 4; ++i)
				{
					send();
					if (i % 2 == 0)
						send_glows(words, 1);
					render(false);
				}
				glow_stats(gs);
				std::printf("  an effect's glow by the street lamp lit again: %u used, %u left to the lamp, %u lights drawn\n", gs[2], gs[3], gs[4]);
				check(gs[2] == 0 && gs[3] == 1 && gs[4] == 1, "an effect's glow by a lit lamp is left to the lamp (its own flames)");
				set_lamp(0, "tl01", 1, lamp_colour, lamp_at, 4.0f, 0.0f);
				for (int i = 0; i < 4; ++i)
				{
					send();
					if (i % 2 == 0)
						send_glows(words, 1);
					render(false);
				}
				send();
				render(true);
				read_picture();
				glow_stats(gs);
				const uint32_t fire_px = glowing(&red, &blue, &most);
				std::printf("  the effect's glow with the lamp off: %u used, %u lights drawn; %u pixels lit warmer\n", gs[2], gs[4], fire_px);
				check(gs[2] == 1 && gs[3] == 0 && fire_px > 2000, "with the lamp off, the effect's glow lights the ground");

				for (int i = 0; i < 4; ++i)
				{
					send();
					if (i % 2 == 0)
						send_glows(words, 0);
					render(false);
				}
				send();
				render(true);
				read_picture();
				glow_stats(gs);
				const uint32_t none_px = glowing(&red, &blue, &most) + bluer(&r_rise, &b_rise);
				std::printf("  none sent: %u held, %u lights drawn, %u pixels lit\n", gs[0], gs[4], none_px);
				check(gs[0] == 0 && gs[4] == 0 && none_px == 0 && other == 0, "its effect gone (none sent): no glow");

				glow(2, home_point, 7.0f, 6.0f, words);
				send_glows(words, 1);
				const ULONGLONG t0 = GetTickCount64();
				while (GetTickCount64() - t0 < 1200)
				{
					send();
					render(false);
					Sleep(16);
				}
				send();
				render(true);
				read_picture();
				glow_stats(gs);
				const uint32_t stale_px = bluer(&r_rise, &b_rise);
				std::printf("  sent once, then not for 1.2 s: %u lights drawn, %u pixels lit bluer\n", gs[4], stale_px);
				check(gs[4] == 0 && stale_px == 0, "glows not sent for over a second (the addon gone): none drawn");
			}
		}
	}

	// Rings from moving through the water: someone walking a second through the zone's water (sent as the
	// Ashita addon does, every frame) makes several; the same walk on land, none
	{
		using actors_fn = void(__cdecl *)(const float *, uint32_t);
		using ripple_stats_fn = void(__cdecl *)(uint32_t *);
		const HMODULE addon = GetModuleHandleW(WFX_MODULE);
		const auto send = reinterpret_cast<actors_fn>(GetProcAddress(addon, "worldfx_grass_actors"));
		const auto ripple_stats = reinterpret_cast<ripple_stats_fn>(GetProcAddress(addon, "worldfx_ripple_stats"));
		check(send != nullptr && ripple_stats != nullptr, "the add-on's actor and ring exports");
		if (send != nullptr && ripple_stats != nullptr)
		{
			auto walk = [&](float x0, float z0) {
				const ULONGLONG t0 = GetTickCount64();
				while (GetTickCount64() - t0 < 1000)
				{
					const float a[4] = { x0 + 5.0f * float(GetTickCount64() - t0) / 1000.0f, 10.0f, z0, 0.9f };
					send(a, 1);
					Sleep(16);
				}
			};
			uint32_t before[2] = {}, wet[2] = {}, dry[2] = {};
			ripple_stats(before);
			walk(-29995.0f, -29990.0f);
			ripple_stats(wet);
			walk(-29000.0f, -29990.0f);
			ripple_stats(dry);
			send(nullptr, 0);
			std::printf("  rings: %u made walking through the water (%u spreading), %u more walking on land\n", wet[0] - before[0], wet[1], dry[0] - wet[0]);
			check(wet[0] - before[0] >= 3, "walking through the water makes rings");
			check(dry[0] == wet[0], "walking on land makes none");
		}
	}

	// The sea (zone 100's sea file above, WaterSea on by default): out on it, the terrain the game draws only 50 yalms
	// around (its bed, half a yalm under it), the sky past that. From 3 yalms over it, looking out along it: the open water
	// from 10 to 250 yalms off drawn (the picture changed there against WaterSea off); from 60 yalms up, from 320 to 800
	// yalms off too (past where the zone's water would have faded out). Its sheets loaded and drawn, its reach at least the
	// water's; with WaterSea off none drawn
	{
		const HMODULE addon = GetModuleHandleW(WFX_MODULE);
		using sea_stats_fn = void(__cdecl *)(uint32_t *);
		using set_setting_fn = uint32_t(__cdecl *)(const char *, float);
		const auto sea_stats = reinterpret_cast<sea_stats_fn>(GetProcAddress(addon, "worldfx_sea_stats"));
		const auto set_setting = reinterpret_cast<set_setting_fn>(GetProcAddress(addon, "worldfx_set_setting"));
		check(sea_stats != nullptr && set_setting != nullptr, "the add-on's sea export (worldfx_sea_stats)");
		if (sea_stats != nullptr && set_setting != nullptr)
		{
			uint32_t ss[4] = {};
			auto sea_frames = [&](const vec3 &from, const vec3 &to, float *out_view) {
				const vec3 sz = normalize(sub(from, to)), sx = normalize(cross({ 0.0f, -1.0f, 0.0f }, sz)), sy = cross(sz, sx);
				const float v[16] = { sx.x, sy.x, sz.x, 0, sx.y, sy.y, sz.y, 0, sx.z, sy.z, sz.z, 0, -dot(sx, from), -dot(sy, from), -dot(sz, from), 1 };
				std::memcpy(out_view, v, sizeof(v));
				reinterpret_cast<set_camera_fn>(GetProcAddress(game, "set_camera"))(v, proj, 1);
				grass::multiply(v, proj, constants);
				constants[16] = sea_at.x - 50.0f;
				constants[17] = sea_at.z - 40.0f;
				constants[19] = sea_at.y;
				for (int i = 0; i < 4; ++i)
					render(false);
				render(true);
				uint8_t *p = nullptr;
				rb->Map(0, nullptr, reinterpret_cast<void **>(&p));
				std::vector<uint8_t> q(size_t(W) * H * 4);
				for (UINT row = 0; row < H; ++row)
					std::memcpy(&q[size_t(row) * W * 4], p + size_t(row) * fp.Footprint.RowPitch, size_t(W) * 4);
				rb->Unmap(0, nullptr);
				sea_stats(ss);
				return q;
			};
			// (how many of the picture's pixels on the water from near to far yalms off (across it) differ by over 6 between two
			// pictures, of how many there are)
			auto sea_changed = [&](const std::vector<uint8_t> &a, const std::vector<uint8_t> &c, const float *v, const vec3 &from, float nearest, float farthest, uint32_t &n) {
				uint32_t changed = 0;
				n = 0;
				for (UINT row = 0; row < H; ++row)
					for (UINT col = 0; col < W; ++col)
					{
						const float rx = (col + 0.5f) / W * 2.0f - 1.0f, ry = 1.0f - (row + 0.5f) / H * 2.0f;
						const vec3 ray = { v[0] * rx / proj[0] + v[1] * ry / proj[5] - v[2], v[4] * rx / proj[0] + v[5] * ry / proj[5] - v[6],
							v[8] * rx / proj[0] + v[9] * ry / proj[5] - v[10] };
						if (ray.y <= 1e-6f)
							continue;
						const float s = (sea_level - from.y) / ray.y, along = std::sqrt(ray.x * ray.x + ray.z * ray.z) * s;
						if (along < nearest || along > farthest)
							continue;
						const size_t i = (size_t(row) * W + col) * 4;
						n++;
						changed += std::abs(a[i] - c[i]) + std::abs(a[i + 1] - c[i + 1]) + std::abs(a[i + 2] - c[i + 2]) > 6 ? 1 : 0;
					}
				return changed;
			};
			const vec3 low_eye = { sea_at.x, sea_level - 3.0f, sea_at.z }, low_to = { sea_at.x + 60.0f, sea_level, sea_at.z };
			const vec3 high_eye = { sea_at.x, sea_level - 60.0f, sea_at.z }, high_to = { sea_at.x + 300.0f, sea_level, sea_at.z };
			float low_view[16], high_view[16];
			const std::vector<uint8_t> low_on = sea_frames(low_eye, low_to, low_view);
			const uint32_t loaded = ss[0], drawn_on = ss[1];
			float reach = 0.0f;
			std::memcpy(&reach, &ss[3], 4);
			const std::vector<uint8_t> high_on = sea_frames(high_eye, high_to, high_view);
			const uint32_t high_drawn = ss[1];
			if (FILE *pic = nullptr; fopen_s(&pic, "e2e_sea.ppm", "wb") == 0 && pic != nullptr) // (both views, to look at)
			{
				std::fprintf(pic, "P6 %u %u 255\n", W, H * 2);
				for (size_t i = 0; i < size_t(W) * H * 2; ++i)
					std::fwrite(&(i < size_t(W) * H ? low_on : high_on)[(i % (size_t(W) * H)) * 4], 1, 3, pic);
				fclose(pic);
			}
			const uint32_t setting_known = set_setting("WaterSea", 0.0f);
			const std::vector<uint8_t> low_off = sea_frames(low_eye, low_to, low_view);
			const uint32_t drawn_off = ss[1];
			const std::vector<uint8_t> high_off = sea_frames(high_eye, high_to, high_view);
			set_setting("WaterSea", 1.0f);
			uint32_t near_n = 0, far_n = 0;
			const uint32_t near_changed = sea_changed(low_on, low_off, low_view, low_eye, 10.0f, 250.0f, near_n);
			const uint32_t far_changed = sea_changed(high_on, high_off, high_view, high_eye, 320.0f, 800.0f, far_n);
			std::printf("  the sea: %u corners loaded, %u drawn (%u from 60 yalms up; %u with WaterSea off), out to %.0f yalms; %u of %u pixels of it 10 to "
				"250 yalms off drawn, %u of %u from 320 to 800\n", loaded, drawn_on, high_drawn, drawn_off, reach, near_changed, near_n, far_changed, far_n);
			check(loaded == 6 && drawn_on == 6 && high_drawn == 6 && setting_known == 1 && drawn_off == 0, "the zone's sea loaded and drawn (none with WaterSea off)");
			check(reach >= 300.0f && reach <= 8000.0f, "the sea out to at least the water's distance");
			check(near_n > 20000 && near_changed > near_n * 95 / 100, "the open sea drawn out along it (10 to 250 yalms off)");
			check(far_n > 5000 && far_changed > far_n * 95 / 100, "the sea drawn past 300 yalms (320 to 800 yalms off, from 60 yalms up)");
			// (back where the far-from-grass checks had the camera)
			const vec3 far_eye = { far_at.x + 4.0f, far_at.y - 2.0f, far_at.z + 3.0f };
			const vec3 fz = normalize(sub(far_eye, far_at)), fx = normalize(cross({ 0.0f, -1.0f, 0.0f }, fz)), fy = cross(fz, fx);
			const float far_view[16] = { fx.x, fy.x, fz.x, 0, fx.y, fy.y, fz.y, 0, fx.z, fy.z, fz.z, 0, -dot(fx, far_eye), -dot(fy, far_eye), -dot(fz, far_eye), 1 };
			reinterpret_cast<set_camera_fn>(GetProcAddress(game, "set_camera"))(far_view, proj, 1);
			grass::multiply(far_view, proj, constants);
			constants[16] = far_at.x - 50.0f;
			constants[17] = far_at.z - 40.0f;
			constants[19] = far_at.y;
			render(false);
		}
	}

	// The weather: the stand-in game's environment (its weather object and record), its zone renderer's fog and the
	// weather's id, found by the code the Ashita addon looks for and read as it does (52 words; here before every frame)
	// for the add-on's worldfx_weather. Before any is sent: no fog, the shadows at the setting. Back on the grass in
	// sunshine, with a fog ending half a yalm off in a colour nothing in the scene has: every blade pixel is the fog's
	// colour through the tint, the terrain the game drew is left as drawn, the sun's shadows worked out. In rain (by the
	// game's second name for it) the sun pass doesn't run, the shading pass still does; changing from sunshine to rain,
	// halfway, half the shadows. Not sent for a while (the Ashita addon gone, or an older one): as without the weather.
	{
		const HMODULE addon = GetModuleHandleW(WFX_MODULE);
		using weather_fn = void(__cdecl *)(const uint32_t *, uint32_t);
		using weather_stats_fn = void(__cdecl *)(uint32_t *);
		using set_weather_fn = void (*)(uint16_t, const char *, const char *, float, const uint8_t *);
		using set_fog_fn = void (*)(uint32_t, float, float);
		const auto send_weather = reinterpret_cast<weather_fn>(GetProcAddress(addon, "worldfx_weather"));
		const auto weather_stats = reinterpret_cast<weather_stats_fn>(GetProcAddress(addon, "worldfx_weather_stats"));
		const auto set_weather = reinterpret_cast<set_weather_fn>(GetProcAddress(game, "set_weather"));
		using rain_stats_fn = void(__cdecl *)(uint32_t *);
		const auto rain_stats = reinterpret_cast<rain_stats_fn>(GetProcAddress(addon, "worldfx_rain_stats"));
		check(rain_stats != nullptr, "the add-on's rain export (worldfx_rain_stats)");
		uint32_t rs[6] = {};
		auto rain_now = [&]() {
			if (rain_stats != nullptr)
				rain_stats(rs);
			float amount = 0.0f;
			std::memcpy(&amount, rs + 2, 4);
			return amount;
		};
		const auto set_fog = reinterpret_cast<set_fog_fn>(GetProcAddress(game, "set_fog"));
		check(send_weather != nullptr && weather_stats != nullptr && set_weather != nullptr && set_fog != nullptr,
			"the add-on's weather exports, the stand-in game's weather");
		// (the Ashita addon's patterns: ?? any byte)
		auto find = [&](const char *pattern) -> const uint8_t * {
			std::vector<int> want;
			for (size_t i = 0; pattern[i] != 0 && pattern[i + 1] != 0; i += 2)
				want.push_back(pattern[i] == '?' ? -1 : int(std::strtoul(std::string(pattern + i, 2).c_str(), nullptr, 16)));
			const auto base = reinterpret_cast<const uint8_t *>(game);
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_lfanew);
			const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
			for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
			{
				if ((s->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
					continue;
				const uint8_t *p = base + s->VirtualAddress;
				for (size_t k = 0; k + want.size() <= s->Misc.VirtualSize; ++k)
				{
					bool same = true;
					for (size_t j = 0; j < want.size() && same; ++j)
						same = want[j] < 0 || p[k + j] == want[j];
					if (same)
						return p + k;
				}
			}
			return nullptr;
		};
		const uint8_t *env_code = find("A1????????83EC24568B701885");
		const uint8_t *renderer_code = find("8B0D????????8D94248C0000008D6F44528D4424605550E8????????84C07517");
		const uint8_t *fog_code = find("8B95????????526A228B400C508B08FF91C8000000A1????????8B95????????526A248B400C508B08FF91C8000000A1????????8B95????????526A25");
		const uint8_t *id_code = find("66A1????????663D0400727E663D1400");
		check(env_code != nullptr && renderer_code != nullptr && fog_code != nullptr && id_code != nullptr, "each found by the Ashita addon's pattern");
		auto word = [](uintptr_t at) {
			uint32_t v = 0;
			std::memcpy(&v, reinterpret_cast<const void *>(at), 4);
			return v;
		};
		// (as worldfx.lua's read_weather)
		auto send = [&]() {
			uint32_t w[grass::kWeatherWords] = {};
			uint32_t read = 8;
			w[0] = *reinterpret_cast<const uint16_t *>(uintptr_t(word(uintptr_t(id_code) + 2)));
			const uint32_t env = word(word(uintptr_t(env_code) + 1));
			if (env != 0)
			{
				const uint32_t object = word(env + 0x10), record = word(env + 0x18);
				uint32_t own = 0; // (the weather's own record: the one at +0x18 is sent only when it's that, not the game's stand-in)
				if (object != 0)
				{
					const uint32_t before = word(object + 0x08);
					w[1] = word(object + 0x10);
					w[2] = before != 0 ? word(before + 0x10) : 0;
					w[3] = word(object + 0x1C);
					own = word(object + 0x20);
					read |= 4;
				}
				if (record != 0 && record == own)
				{
					std::memcpy(&w[8], reinterpret_cast<const void *>(uintptr_t(record)), 0xB0);
					read |= 1;
				}
			}
			const uint32_t renderer = word(word(uintptr_t(renderer_code) + 2));
			if (renderer != 0)
			{
				w[4] = word(renderer + word(uintptr_t(fog_code) + 2));
				w[5] = word(renderer + word(uintptr_t(fog_code) + 0x1C));
				w[6] = word(renderer + word(uintptr_t(fog_code) + 0x36));
				read |= 2;
			}
			w[7] = read;
			send_weather(w, grass::kWeatherWords);
		};
		uint32_t wet_drawn = 0;
		float wetness = 0.0f;
		auto stats_now = [&](float *f) {
			uint32_t s[12] = {};
			weather_stats(s);
			std::memcpy(f, s + 1, 6 * sizeof(float));
			wet_drawn = s[10];
			std::memcpy(&wetness, s + 11, 4);
			return s[0];
		};
		if (send_weather != nullptr && weather_stats != nullptr && set_weather != nullptr && set_fog != nullptr && env_code != nullptr &&
			renderer_code != nullptr && fog_code != nullptr && id_code != nullptr)
		{
			float f[6] = {};
			render(false);
			const uint32_t haze_before = stats_now(f);
			check(haze_before == 0 && f[5] == 1.0f, "before any weather is sent: no fog, the shadows at the setting");

			// Back on the grass
			reinterpret_cast<set_camera_fn>(GetProcAddress(game, "set_camera"))(view, proj, 1);
			grass::multiply(view, proj, constants);
			constants[16] = at.x - 50.0f;
			constants[17] = at.z - 40.0f;
			constants[19] = at.y;
			// A noon record (its fog: 0x55 0x64 0x6F from 0 to 396, the zone drawn to 252 yalms, its sun's colour 0x80 0x80
			// 0x80 at light 1.5: zone 100's clear sky) and the renderer's fog: a colour nothing in the scene has, ending half
			// a yalm off. Other weathers' records: the same with the sun dimmed as the game dims it (light 1.5 times this).
			uint8_t record[0xB0] = {};
			auto record_dimmed = [&](float by) {
				const uint8_t fog_bytes[4] = { 0x55, 0x64, 0x6F, 0x80 }, sun_colour[4] = { 0x80, 0x80, 0x80, 0x80 };
				std::memcpy(record + 0x2C, sun_colour, 4);
				std::memcpy(record + 0x38, fog_bytes, 4);
				const float record_fog[2] = { 396.0f, 0.0f }, zone_light = 1.5f * by, drawn_to = 252.5f;
				std::memcpy(record + 0x3C, record_fog, 8);
				std::memcpy(record + 0x44, &zone_light, 4);
				std::memcpy(record + 0x58, &drawn_to, 4);
				return record;
			};
			using stand_in_fn = void (*)(int);
			const auto set_weather_stand_in = reinterpret_cast<stand_in_fn>(GetProcAddress(game, "set_weather_stand_in"));
			const auto set_area_environment = reinterpret_cast<stand_in_fn>(GetProcAddress(game, "set_area_environment"));
			check(set_weather_stand_in != nullptr && set_area_environment != nullptr, "the stand-in game's stand-in record and area environment");
			set_weather(1, "suny", nullptr, 1.0f, record_dimmed(1.0f));
			set_fog(0x00CC33E6, 0.0f, 0.5f); // (0.8 0.2 0.9)
			// Entering a zone: the weather has no record of its own yet and the game lights the zone with its stand-in (light 1,
			// fog to 100, drawn to 100): not taken as the zone's (the line in ReShade.log says the record isn't read yet); then
			// the zone's own: taken (logged again). Then another area's environment chained in (raining, a dim record of its
			// own): the default environment's weather still followed, the one the zone frame is drawn with.
			if (set_weather_stand_in != nullptr && set_area_environment != nullptr)
			{
				set_weather_stand_in(1);
				send();
				render(false);
				stats_now(f);
				const float share_stand_in = f[5];
				set_weather_stand_in(0);
				send();
				render(false);
				set_area_environment(1);
				send();
				render(false);
				stats_now(f);
				const float share_area = f[5];
				set_area_environment(0);
				std::printf("  entering a zone, on the game's stand-in record: shadows at %.2f; with an area environment raining in the chain: %.2f\n",
					share_stand_in, share_area);
				check(share_stand_in == 1.0f && share_area == 1.0f, "the stand-in record isn't the zone's; an area environment in the chain doesn't change it");
			}
			for (int i = 0; i < 4; ++i)
			{
				send();
				render(false);
			}
			send();
			render(true);
			read_picture();
			if (last_draw != nullptr)
				last_draw(stats);
			uint32_t fogged_px = 0;
			for (size_t i = 0; i < size_t(W) * H; ++i)
			{
				const uint8_t *px = &pixels[i * 4];
				fogged_px += std::abs(px[0] - 0.4f * 255.0f) <= 2.0f && std::abs(px[1] - 0.1f * 255.0f) <= 2.0f && std::abs(px[2] - 0.45f * 255.0f) <= 2.0f ? 1 : 0;
			}
			const uint32_t haze_on = stats_now(f);
			std::printf("  weather sunshine, fog 0.8 0.2 0.9 to half a yalm: the add-on's fog %s (%.2f %.2f %.2f from %.1f to %.1f), shadows at %.2f; "
				"%u blades, %u pixels of the fog's colour, %u blade, %u terrain, %u sky, %u other; sun shadows %s\n",
				haze_on ? "on" : "off", f[0], f[1], f[2], f[3], f[4], f[5], stats[0], fogged_px, blade_px, terrain_px, sky_px, other, stats[4] ? "worked out" : "not");
			check(haze_on == 1 && std::fabs(f[0] - 0.8f) < 0.01f && std::fabs(f[1] - 0.2f) < 0.01f && std::fabs(f[2] - 0.9f) < 0.01f && f[3] == 0.0f && f[4] == 0.5f,
				"the add-on fogs with the renderer's fog");
			check(f[5] == 1.0f && stats[4] != 0, "sunshine: the sun's shadows as set, worked out");
			check(stats[0] != 0 && fogged_px > 500 && blade_px == 0 && other == fogged_px, "every blade pixel is the fog's colour (blades past where it ends)");
			check(terrain_px > 1000 && sky_px > 1000, "the terrain and sky the game drew left as drawn");
			rain_now();
			check(rs[3] == 100 && rs[0] == 0 && rs[1] == 0, "sunshine: zone 100's rain map loaded, no rain drawn");

			// The sea's reach follows the game's fog (where it ends; never nearer than the water's 300, at most 8,000): the fog
			// ending half a yalm off, 300; at 1,500 yalms, 1,500; a fog too thin to see (to 100,000), 8,000
			{
				using sea_stats_fn = void(__cdecl *)(uint32_t *);
				const auto sea_stats = reinterpret_cast<sea_stats_fn>(GetProcAddress(GetModuleHandleW(WFX_MODULE), "worldfx_sea_stats"));
				auto reach_now = [&]() {
					uint32_t ss[4] = {};
					if (sea_stats != nullptr)
						sea_stats(ss);
					float v = 0.0f;
					std::memcpy(&v, &ss[3], 4);
					return v;
				};
				const float reach_close = reach_now();
				set_fog(0x00808080, 0.0f, 1500.0f);
				for (int i = 0; i < 3; ++i)
				{
					send();
					render(false);
				}
				const float reach_far = reach_now();
				set_fog(0x00808080, 0.0f, 100000.0f);
				for (int i = 0; i < 3; ++i)
				{
					send();
					render(false);
				}
				const float reach_thin = reach_now();
				std::printf("  the sea's reach: %.0f yalms with the fog to half a yalm, %.0f with it to 1,500, %.0f to 100,000\n", reach_close, reach_far, reach_thin);
				check(reach_close == 300.0f && reach_far == 1500.0f && reach_thin == 8000.0f, "the sea out to where the game's fog ends (at least the water's 300, at most 8,000)");
			}

			// Rain (the game's second set of names), under a thin grey fog: the sun's shadows as the game dims its sun, and the ground in the
			// zone's wet mask (the far half on screen) wet: darker than the same picture in sunshine, the near half as drawn
			set_fog(0x00808080, 0.0f, 100000.0f);
			for (int i = 0; i < 3; ++i)
			{
				send();
				render(false);
			}
			send();
			render(true);
			read_picture();
			const std::vector<uint8_t> sunny_picture = pixels;
			// Rain starting while the weather is known: drawn at once (rain's amount, 0.55), the ground wet only slowly
			set_weather(6, "rain", nullptr, 1.0f, record_dimmed(0.59f)); // (the game's rain: its sun at 0.59 of the clear sky's)
			for (int i = 0; i < 3; ++i)
			{
				send();
				render(false);
			}
			float amount = rain_now(), wet_start = 0.0f;
			std::memcpy(&wet_start, rs + 5, 4);
			std::printf("  rain starting: %.2f hard, %u drop places and %u splash places drawn, the rain map's window %s; wet %.3f\n",
				amount, rs[0], rs[1], rs[4] ? "written that frame" : "as it was", wet_start);
			check(std::fabs(amount - 0.55f) < 1e-4f && rs[0] > 0 && rs[1] > 0, "rain starting: the add-on's rain drawn, as hard as rain is (0.55), with splashes");
			check(wet_start > 0.0f && wet_start < 0.05f, "rain starting while you watch: the ground gets wet slowly");
			// The weather unknown for a while (the Ashita addon gone): no rain; then rain again (the second set's name): the
			// ground wet at once (already raining when the weather became known, as when zoning in)
			Sleep(1100);
			render(false);
			render(false);
			rain_now();
			check(rs[0] == 0 && rs[1] == 0, "the weather not sent lately: no rain drawn");
			set_weather(6, "rai1", nullptr, 1.0f, record_dimmed(0.59f));
			for (int i = 0; i < 3; ++i)
			{
				send();
				render(false);
			}
			send();
			render(true);
			read_picture();
			if (last_draw != nullptr)
				last_draw(stats);
			stats_now(f);
			uint32_t wet_px = 0, dry_px = 0;
			for (size_t i = 0; i < size_t(W) * H; ++i)
			{
				const uint8_t *was = &sunny_picture[i * 4], *now = &pixels[i * 4];
				auto terrain_coloured = [](const uint8_t *px) {
					return std::abs(px[0] - 0.15f * 255.0f) <= 2.0f && std::abs(px[1] - 0.175f * 255.0f) <= 2.0f && std::abs(px[2] - 0.1f * 255.0f) <= 2.0f;
				};
				if (!terrain_coloured(was))
					continue;
				const double luma_was = 0.3 * was[0] + 0.59 * was[1] + 0.11 * was[2], luma_now = 0.3 * now[0] + 0.59 * now[1] + 0.11 * now[2];
				wet_px += luma_now < luma_was - 4.0 ? 1 : 0;
				dry_px += terrain_coloured(now) ? 1 : 0;
			}
			std::printf("  rain: shadows at %.2f, shading pass %s, sun shadows %s; wet look %s, wet %.2f: %u terrain pixels darker than in sunshine, %u as they were\n",
				f[5], stats[2] ? "ran" : "didn't run", stats[4] ? "worked out" : "not", wet_drawn ? "drawn" : "not drawn", wetness, wet_px, dry_px);
			check(std::fabs(f[5] - 0.59f) < 1e-4f && stats[2] != 0 && stats[4] != 0, "rain: the sun's shadows at the share the game's sun is dimmed to (0.59)");
			check(wet_drawn != 0 && wetness == 1.0f && wet_px > 3000 && dry_px > 1000, "rain: the ground in the zone's wet mask darker (wet), the rest as drawn");
			rain_now();
			check(rs[0] > 0 && rs[1] > 0, "rain (rai1): the add-on's rain drawn");
			set_weather(6, "rain", "suny", 0.5f, record_dimmed(0.795f)); // (the game blends the two records: halfway)
			for (int i = 0; i < 3; ++i)
			{
				send();
				render(false);
			}
			if (last_draw != nullptr)
				last_draw(stats);
			stats_now(f);
			std::printf("  sunshine to rain, halfway: shadows at %.2f, sun shadows %s\n", f[5], stats[4] ? "worked out" : "not");
			check(std::fabs(f[5] - 0.795f) < 1e-4f && stats[4] != 0, "changing from sunshine to rain halfway: the shadows as the game's blended sun (0.795)");

			// Snow (id 12), then a blizzard (13, by the game's second name), then rain turning to snow halfway: the add-on's
			// snow as hard as each is (0.6, 1; half of snow's halfway, with half the rain's), the blizzard blown hard
			// across, the snow gently with its flakes swaying; no rain in either; the shadows as each record's sun
			using snow_stats_fn = void(__cdecl *)(uint32_t *);
			const auto snow_stats = reinterpret_cast<snow_stats_fn>(GetProcAddress(addon, "worldfx_snow_stats"));
			check(snow_stats != nullptr, "the add-on's snow export (worldfx_snow_stats)");
			uint32_t ss[4] = {};
			float slant = 0.0f, sway = 0.0f;
			auto snow_now = [&]() {
				if (snow_stats != nullptr)
					snow_stats(ss);
				float snowing = 0.0f;
				std::memcpy(&snowing, ss + 1, 4);
				std::memcpy(&slant, ss + 2, 4);
				std::memcpy(&sway, ss + 3, 4);
				return snowing;
			};
			auto weather_for = [&](uint16_t id, const char *name, const char *before, float change, float sun) {
				set_weather(id, name, before, change, record_dimmed(sun));
				for (int i = 0; i < 3; ++i)
				{
					send();
					render(false);
				}
				if (last_draw != nullptr)
					last_draw(stats);
				stats_now(f);
			};
			weather_for(12, "snow", nullptr, 1.0f, 0.7f);
			float snowing = snow_now(), raining = rain_now();
			std::printf("  snow: %.2f hard, %u flake places drawn, slanted %.2f, swaying %.2f; rain %.2f (%u drops); shadows at %.2f\n", snowing, ss[0], slant,
				sway, raining, rs[0], f[5]);
			check(std::fabs(snowing - 0.6f) < 1e-4f && ss[0] > 0 && rs[0] == 0 && rs[1] == 0 && raining == 0.0f, "snow: the add-on's snow drawn, as hard as snow is (0.6), no rain");
			check(slant > 0.05f && slant < 0.3f && sway > 0.3f && std::fabs(f[5] - 0.7f) < 1e-4f, "snow: slanted gently, its flakes swaying; the shadows as its sun (0.7)");
			weather_for(13, "bli1", nullptr, 1.0f, 0.5f);
			snowing = snow_now();
			raining = rain_now();
			std::printf("  blizzard (bli1): %.2f hard, %u flake places drawn, slanted %.2f, swaying %.2f; rain %.2f\n", snowing, ss[0], slant, sway, raining);
			check(std::fabs(snowing - 1.0f) < 1e-4f && ss[0] > 0 && rs[0] == 0 && raining == 0.0f, "blizzard (bli1): the add-on's snow drawn as hard as it goes (1), no rain");
			check(slant >= 1.0f && sway < 0.3f, "blizzard: blown hard across by the wind");
			weather_for(12, "snow", "rain", 0.5f, 0.65f);
			snowing = snow_now();
			raining = rain_now();
			std::printf("  rain to snow, halfway: snow %.2f (%u flakes), rain %.2f (%u drops)\n", snowing, ss[0], raining, rs[0]);
			check(std::fabs(snowing - 0.3f) < 1e-4f && std::fabs(raining - 0.275f) < 1e-4f && ss[0] > 0 && rs[0] > 0,
				"changing from rain to snow halfway: both drawn, each half as hard");

			// Not sent for over a second: as without the weather
			Sleep(1100);
			render(false);
			render(true);
			read_picture();
			if (last_draw != nullptr)
				last_draw(stats);
			const uint32_t haze_after = stats_now(f);
			uint32_t still_fogged = 0;
			for (size_t i = 0; i < size_t(W) * H; ++i)
			{
				const uint8_t *px = &pixels[i * 4];
				still_fogged += std::abs(px[0] - 0.4f * 255.0f) <= 2.0f && std::abs(px[1] - 0.1f * 255.0f) <= 2.0f && std::abs(px[2] - 0.45f * 255.0f) <= 2.0f ? 1 : 0;
			}
			std::printf("  weather not sent for a second: fog %s, shadows at %.2f, wet %.2f; %u blade pixels, %u of the old fog's colour, %u other\n",
				haze_after ? "on" : "off", f[5], wetness, blade_px, still_fogged, other);
			check(haze_after == 0 && f[5] == 1.0f && wetness == 0.0f && stats[4] != 0 && blade_px > 500 && still_fogged == 0,
				"the weather not sent lately: drawn as without it (no fog, the shadows as set, dry)");
			rain_now();
			snow_now();
			check(rs[0] == 0 && ss[0] == 0, "the weather not sent lately: no rain, no snow");

			// The sky of its own (ReShade.ini Sky=1): a sunshine record whose sky part has the colour the world is cleared to
			// at all 8 stops (so the "dome" is that colour everywhere), the game's light turned toward a sky pixel near the top
			// of the picture. With the record's dome segments 0 (no dome): nothing drawn; with 10: the sky repainted, the
			// sun's disc (by night the moon's, WFX_E2E_HOUR=22) where the light points, only sky pixels changed, sky far from
			// it as it was; under mist (the record's fog to 99 yalms) the sun and stars all but gone. Then the last weather
			// again and the weather left unsent, as the steps after this expect.
			using sky_stats_fn = void(__cdecl *)(uint32_t *);
			const auto sky_stats = reinterpret_cast<sky_stats_fn>(GetProcAddress(addon, "worldfx_sky_stats"));
			check(sky_stats != nullptr, "the add-on's sky export (worldfx_sky_stats)");
			uint32_t ks[5] = {};
			float sky_sun = 0.0f, sky_stars = 0.0f;
			auto sky_now = [&]() {
				if (sky_stats != nullptr)
					sky_stats(ks);
				std::memcpy(&sky_sun, ks + 2, 4);
				std::memcpy(&sky_stars, ks + 3, 4);
			};
			// (the sky pixel nearest the top left corner, and its view ray in the world)
			int sun_x = -1, sun_y = -1;
			for (int d = 8; d < int(W) && sun_x < 0; ++d)
				for (int row = 8; row <= d && row < int(H) && sun_x < 0; ++row)
				{
					const int col = d;
					const uint8_t *px = &pixels[(size_t(row) * W + col) * 4];
					if (std::abs(px[0] - 0.275f * 255.0f) <= 2.0f && std::abs(px[1] - 0.3f * 255.0f) <= 2.0f && std::abs(px[2] - 0.35f * 255.0f) <= 2.0f)
						sun_x = col, sun_y = row;
				}
			const float nx = (sun_x + 0.5f) / W * 2.0f - 1.0f, ny = 1.0f - (sun_y + 0.5f) / H * 2.0f;
			const vec3 sun_ray = normalize({ view[0] * nx / proj[0] + view[1] * ny / proj[5] - view[2], view[4] * nx / proj[0] + view[5] * ny / proj[5] - view[6],
				view[8] * nx / proj[0] + view[9] * ny / proj[5] - view[10] });
			const float toward_ground[3] = { -sun_ray.x, -sun_ray.y, -sun_ray.z };
			const auto set_light = reinterpret_cast<set_light_fn>(GetProcAddress(game, "set_light"));
			set_light(toward_ground);
			auto sky_record = [&](uint8_t segments, float fog_end) {
				uint8_t *rec = record_dimmed(1.0f);
				const uint8_t clear_bytes[4] = { 140, 153, 178, 0x80 }; // (0.55 0.6 0.7, the world's clear colour, as bytes)
				std::memcpy(rec + 0x4C, clear_bytes, 4);
				rec[0x5E] = segments;
				for (int i = 0; i < 8; ++i)
				{
					std::memcpy(rec + 0x6C + 4 * i, clear_bytes, 4);
					const float h = i / 7.0f;
					std::memcpy(rec + 0x8C + 4 * i, &h, 4);
				}
				std::memcpy(rec + 0x3C, &fog_end, 4);
				return rec;
			};
			auto sky_frames = [&](uint8_t segments, float fog_end) {
				set_weather(1, "suny", nullptr, 1.0f, sky_record(segments, fog_end));
				for (int i = 0; i < 3; ++i)
				{
					send();
					render(false);
				}
				send();
				render(true);
				read_picture();
				sky_now();
				return pixels;
			};
			// (as the add-on works them out from the sun's height by the hour: the moon below the horizon, its disc in once
			// the sun is 0.1 down, all of it at 0.2; stars from 0.02 down, all at 0.17)
			const bool night = e2e_night;
			const float want_sun = night ? float(std::clamp((-e2e_up - 0.1) / 0.1, 0.0, 1.0)) : 1.0f;
			const float want_stars = float(std::clamp((-e2e_up - 0.02) / 0.15, 0.0, 1.0));
			// (the clouds of its own off for these: sunshine has some, and the sky far from the sun is to stay as it was; the
			// weather of its own's checks below turn them on again, as a menu does: worldfx_set_setting)
			using set_setting_fn = uint32_t(__cdecl *)(const char *, float);
			const auto set_setting = reinterpret_cast<set_setting_fn>(GetProcAddress(addon, "worldfx_set_setting"));
			check(set_setting != nullptr && set_setting("Clouds", 0.0f) == 1 && set_setting("NoSuchSetting", 1.0f) == 0, "the add-on's settings changed as a menu changes them (worldfx_set_setting)");
			const std::vector<uint8_t> no_dome = sky_frames(0, 396.0f);
			const uint32_t no_dome_drawn = ks[0];
			const std::vector<uint8_t> domed = sky_frames(10, 396.0f);
			const uint32_t domed_drawn = ks[0], domed_segments = ks[1], domed_moon = ks[4];
			const float domed_sun = sky_sun, domed_stars = sky_stars;
			auto lum = [&](const std::vector<uint8_t> &q, int at_x, int at_y) {
				const uint8_t *c = &q[(size_t(at_y) * W + at_x) * 4];
				return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
			};
			// (blades sway and, by night, fireflies flash between frames: only the terrain the game drew is held to the same
			// colour; the sky may change)
			auto close_to = [](const uint8_t *px, float r, float g, float b) {
				return std::abs(px[0] - r * 255.0f) <= 2.0f && std::abs(px[1] - g * 255.0f) <= 2.0f && std::abs(px[2] - b * 255.0f) <= 2.0f;
			};
			uint32_t changed = 0, changed_not_sky = 0, far_changed = 0, far_brighter = 0, far_sky = 0;
			for (UINT row = 0; row < H; ++row)
				for (UINT col = 0; col < W; ++col)
				{
					const uint8_t *pa = &no_dome[(size_t(row) * W + col) * 4], *pb = &domed[(size_t(row) * W + col) * 4];
					const bool was_sky = close_to(pa, 0.275f, 0.3f, 0.35f);
					const bool b_terrain = close_to(pb, 0.15f, 0.175f, 0.1f), b_green = !b_terrain && pb[0] > 0 && pb[2] <= pb[1];
					const bool differs = std::memcmp(pa, pb, 3) != 0;
					changed += differs && was_sky ? 1 : 0;
					// (by more than a byte: a firefly's faint edge by night isn't the sky)
					const bool moved = std::abs(pa[0] - pb[0]) > 1 || std::abs(pa[1] - pb[1]) > 1 || std::abs(pa[2] - pb[2]) > 1;
					changed_not_sky += moved && close_to(pa, 0.15f, 0.175f, 0.1f) && !b_green ? 1 : 0;
					const float rx = (col + 0.5f) / W * 2.0f - 1.0f, ry = 1.0f - (row + 0.5f) / H * 2.0f;
					const vec3 ray = normalize({ view[0] * rx / proj[0] + view[1] * ry / proj[5] - view[2], view[4] * rx / proj[0] + view[5] * ry / proj[5] - view[6],
						view[8] * rx / proj[0] + view[9] * ry / proj[5] - view[10] });
					const bool away = dot(ray, sun_ray) < 0.342f; // (over 70 degrees from the light: its glow under half a byte there)
					// (a blade swaying into the sky isn't the sky; by night neither is a firefly in front of it: yellow-green, its blue
					// barely up, where a star's light is whitish; in either picture: one in the first left a sky pixel a byte or two
					// brighter there than the second's, in the add-on's run at 22 on 2026-10-22)
					const bool firefly = firefly_light(pb, pa[0], pa[1], pa[2]) || firefly_light(pa, pb[0], pb[1], pb[2]);
					if (away && was_sky && !b_green && !firefly)
					{
						far_sky++;
						const bool off = std::abs(pa[0] - pb[0]) > 1 || std::abs(pa[1] - pb[1]) > 1 || std::abs(pa[2] - pb[2]) > 1;
						if (off && far_changed < 3)
							std::printf("  (far sky pixel %u,%u: %u %u %u, then %u %u %u)\n", col, row, pa[0], pa[1], pa[2], pb[0], pb[1], pb[2]);
						far_changed += off ? 1 : 0;
						far_brighter += pb[0] + pb[1] + pb[2] > pa[0] + pa[1] + pa[2] + 6 ? 1 : 0;
					}
				}
			const double sun_gain = lum(domed, sun_x, sun_y) - lum(no_dome, sun_x, sun_y);
			const std::vector<uint8_t> misty = sky_frames(10, 99.0f);
			const float misty_sun = sky_sun, misty_stars = sky_stars;
			const double misty_gain = lum(misty, sun_x, sun_y) - lum(no_dome, sun_x, sun_y);
			std::printf("  sky of its own: the light toward pixel %d,%d; no dome: %s; 10 segments: %s (%u), %s %.2f, stars %.2f: %u sky pixels changed (%u of the terrain), "
				"%u of %u sky pixels over 70 degrees from it by over a byte (%u brighter), +%.1f at it; mist: %s %.2f, stars %.2f, +%.1f at it\n",
				sun_x, sun_y, no_dome_drawn ? "drawn" : "not drawn", domed_drawn ? "drawn" : "not drawn", domed_segments, domed_moon ? "moon" : "sun", domed_sun, domed_stars,
				changed, changed_not_sky, far_changed, far_sky, far_brighter, sun_gain, domed_moon ? "moon" : "sun", misty_sun, misty_stars, misty_gain);
			check(sun_x >= 0, "a sky pixel near the top of the picture");
			check(no_dome_drawn == 0 && domed_drawn == 1 && domed_segments == 10, "the sky repainted only where the record has a dome");
			check(domed_moon == (night ? 1u : 0u) && std::fabs(domed_sun - want_sun) < 0.01f && std::fabs(domed_stars - want_stars) < 0.01f,
				night ? "by night: the moon toward the game's light, the stars out (as the hour has them)" : "by day: the sun toward the game's light, no stars");
			check(sun_gain > 30.0 && changed_not_sky == 0 && changed > 20, "its disc where the light points; the terrain the game drew as it was");
			// (by night the stars are on, but this sky is as bright as a day's: they fade out on it, as through twilight;
			// grass_test draws them on a night sky)
			check(far_sky > 1000 && far_changed == 0, night ? "the sky over 70 degrees from it as it was: no stars on a sky this bright" :
				"the sky over 70 degrees from it as it was (the same colour: the record's)");
			check(misty_sun < 0.05f && misty_stars < 0.05f && misty_gain < 3.0, "under mist the sun (and stars) all but gone");

			// The weather of its own (WeatherOwn and its parts on by default, the clouds on again), on the sky record with a dome:
			// zone 100 under open sky, the sky of its own on, so all of its weather is World FX's own whatever the weather
			// (worldfx_weather_drawn: kWeatherFxAll), its rain and snow too (the zone's rain map); each weather's parts as it has
			// them (their bits and the look: worldfx_weather_fx_stats). Clouds over the sky away from the sun, more in clouds than
			// in fine weather; in thunder a flash: a frame brighter than the one before, then as before; dust tinting the ground
			// in a dust storm; heat shimmer drawn in a heat wave; an aurora's strength by night; gloom fogging the ground; the
			// stars brighter in stellar glare. A part turned off stops only World FX's own (All stays); WeatherOwn off or the sky
			// off: not all of it (rain and snow as before); a zone without open sky: not all of it, back in zone 100 all again.
			{
				using drawn_fn = uint32_t(__cdecl *)();
				using wx_stats_fn = void(__cdecl *)(uint32_t *);
				const auto weather_drawn = reinterpret_cast<drawn_fn>(GetProcAddress(addon, "worldfx_weather_drawn"));
				const auto wx_stats = reinterpret_cast<wx_stats_fn>(GetProcAddress(addon, "worldfx_weather_fx_stats"));
				check(weather_drawn != nullptr && wx_stats != nullptr && set_setting != nullptr, "the add-on's weather of its own exports");
				if (weather_drawn != nullptr && wx_stats != nullptr && set_setting != nullptr)
				{
					enum : uint32_t { kAll = 1, kSky = 2, kClouds = 4, kRain = 8, kSnow = 16, kLightning = 32, kDust = 64, kWind = 128, kHeat = 256, kAurora = 512, kGloom = 1024 };
					const uint32_t base = kAll | kSky | kRain | kSnow;
					set_setting("Clouds", 1.0f);
					uint32_t ws[16] = {};
					auto wx_float = [&](int i) {
						float v = 0.0f;
						std::memcpy(&v, &ws[i], 4);
						return v;
					};
					// (a weather on the dome's record: a few frames, the last captured; its bits)
					auto wx_frames = [&](uint16_t id, const char *name) {
						set_weather(id, name, nullptr, 1.0f, sky_record(10, 396.0f));
						for (int i = 0; i < 3; ++i)
						{
							send();
							render(false);
						}
						send();
						render(true);
						read_picture();
						wx_stats(ws);
						return weather_drawn();
					};
					// (sky pixels over 70 degrees from the sun, sky in the picture with no dome: how many differ by over 4 from the
					// same sky without clouds, the sunshine picture above)
					auto cloudy = [&](const std::vector<uint8_t> &q) {
						uint32_t n = 0;
						for (UINT row = 0; row < H; ++row)
							for (UINT col = 0; col < W; ++col)
							{
								const size_t i = (size_t(row) * W + col) * 4;
								const uint8_t *pa = &no_dome[i], *pb = &domed[i], *pq = &q[i];
								const float rx = (col + 0.5f) / W * 2.0f - 1.0f, ry = 1.0f - (row + 0.5f) / H * 2.0f;
								const vec3 ray = normalize({ view[0] * rx / proj[0] + view[1] * ry / proj[5] - view[2], view[4] * rx / proj[0] + view[5] * ry / proj[5] - view[6],
									view[8] * rx / proj[0] + view[9] * ry / proj[5] - view[10] });
								const bool green = pq[0] > 0 && pq[2] <= pq[1] && !close_to(pq, 0.275f, 0.3f, 0.35f);
								if (!close_to(pa, 0.275f, 0.3f, 0.35f) || dot(ray, sun_ray) >= 0.342f || green || firefly_light(pq, pb[0], pb[1], pb[2]) ||
								firefly_light(pb, pq[0], pq[1], pq[2]))
									continue;
								n += std::abs(pq[0] - pb[0]) > 4 || std::abs(pq[1] - pb[1]) > 4 || std::abs(pq[2] - pb[2]) > 4 ? 1 : 0;
							}
						return n;
					};
					// (the mean of the terrain's pixels, those the terrain's colour in the sunshine picture: luma, and red less green)
					auto terrain_mean = [&](const std::vector<uint8_t> &q, double *red_less_green) {
						double l = 0.0, rg = 0.0;
						uint32_t n = 0;
						for (size_t i = 0; i < size_t(W) * H; ++i)
							if (close_to(&domed[i * 4], 0.15f, 0.175f, 0.1f))
							{
								const uint8_t *px = &q[i * 4];
								l += 0.3 * px[0] + 0.59 * px[1] + 0.11 * px[2];
								rg += double(px[0]) - double(px[1]);
								n++;
							}
						if (red_less_green != nullptr)
							*red_less_green = n != 0 ? rg / n : 0.0;
						return n != 0 ? l / n : 0.0;
					};
					const uint32_t fine_bits = wx_frames(0, "fine");
					const uint32_t fine_cloudy = cloudy(pixels);
					const float fine_cover = wx_float(1);
					const uint32_t fine_drawn = ws[12];
					const uint32_t clod_bits = wx_frames(2, "clo1");
					const uint32_t clod_cloudy = cloudy(pixels);
					const float clod_cover = wx_float(1);
					const uint32_t clod_on = ws[0], clod_drawn = ws[12];
					if (FILE *pic = nullptr; fopen_s(&pic, "e2e_clouds.ppm", "wb") == 0 && pic != nullptr)
					{
						std::fprintf(pic, "P6 %u %u 255\n", W, H);
						for (size_t i = 0; i < size_t(W) * H; ++i)
							std::fwrite(&pixels[i * 4], 1, 3, pic);
						fclose(pic);
					}
					std::printf("  the weather of its own: fine weather bits 0x%X, cover %.2f, %u cloudy sky pixels; clouds (clo1) bits 0x%X, cover %.2f, %u cloudy; "
						"the clouds' own pass %s, %s (fine), %s, %s (clouds)\n", fine_bits, fine_cover, fine_cloudy, clod_bits, clod_cover, clod_cloudy,
						(fine_drawn & 16) != 0 ? "drawn" : "not drawn", (fine_drawn & 32) != 0 ? "carried over" : "marched whole",
						(clod_drawn & 16) != 0 ? "drawn" : "not drawn", (clod_drawn & 32) != 0 ? "carried over" : "marched whole");
					check(clod_on == 1 && fine_bits == (base | kClouds) && clod_bits == (base | kClouds),
						"all of the weather its own under open sky (with its sky, clouds, rain and snow), as the bits say");
					// (half as many again: this view's sky is a band low over the horizon, where even fine weather's heaps crowd
					// together, seen edge on)
					check(fine_cloudy > 20 && 2 * clod_cloudy > 3 * fine_cloudy && clod_cover > fine_cover, "clouds over the sky: more in clouds than in fine weather");
					check((fine_drawn & 16) != 0 && (clod_drawn & 16) != 0, "the clouds marched through their slab in their own pass (both weathers)");

					// Thunder: its bits; a flash comes within seconds (the first after 1.5 to 4.5 s): the frame with it brighter
					// than the one before it, the terrain back as it was after
					const uint32_t thunder_bits = wx_frames(14, "thdr");
					const double terrain_calm = terrain_mean(pixels, nullptr);
					std::vector<uint8_t> before_flash = pixels, flash_picture;
					float flash_seen = 0.0f;
					const ULONGLONG wait_from = GetTickCount64();
					// (every frame captured: a flash's pulses last about a tenth of a second each, a frame here a good part of that;
					// several flashes come within the 45 s, any of their frames will do)
					while (GetTickCount64() - wait_from < 45000 && flash_picture.empty())
					{
						send();
						render(true);
						read_picture();
						wx_stats(ws);
						if (wx_float(2) > 0.1f)
						{
							flash_picture = pixels;
							flash_seen = wx_float(2);
						}
						else if (wx_float(2) == 0.0f)
							before_flash = pixels;
					}
					const uint32_t flashes = ws[10];
					for (int i = 0; i < 100; ++i)
					{
						send();
						render(true);
						read_picture();
						wx_stats(ws);
						if (wx_float(2) == 0.0f)
							break;
					}
					const double terrain_before = terrain_mean(before_flash, nullptr), terrain_flash = flash_picture.empty() ? 0.0 : terrain_mean(flash_picture, nullptr);
					const double terrain_after = terrain_mean(pixels, nullptr);
					std::printf("  thunder: bits 0x%X; a flash %.2f after %.1f s (%u so far): the terrain %.1f the frame before, %.1f in it, %.1f after (%.1f before any)\n",
						thunder_bits, flash_seen, (GetTickCount64() - wait_from) / 1000.0, flashes, terrain_before, terrain_flash, terrain_after, terrain_calm);
					check(thunder_bits == (base | kClouds | kLightning), "thunder: lightning its own too (the bits)");
					// (lit by the flash's share of up to twice: 0.9 of it in cool white, the terrain's own colour lit by that)
					check(!flash_picture.empty() && terrain_flash > terrain_before * (1.0 + 0.4 * flash_seen) && terrain_flash > terrain_before + 1.0,
						"a lightning flash: the frame brighter than the one before");
					check(std::fabs(terrain_after - terrain_before) < 0.5, "... and then back as it was");
					set_setting("Lightning", 0.0f);
					const uint32_t no_lightning_bits = wx_frames(14, "thdr");
					const float no_lightning_flash = wx_float(2);
					const uint32_t no_lightning_next = ws[11];
					set_setting("Lightning", 1.0f);
					check(no_lightning_bits == (base | kClouds) && no_lightning_flash == 0.0f && no_lightning_next == UINT32_MAX,
						"Lightning off: none of its own (no flash waited for), the rest of the weather still its own (the game's stays left out)");

					// Dust and sand storms: the terrain tinted toward the dust's colour (redder: its brown against the terrain's green),
					// far off the most (the terrain's top third on screen: 15 to 50 yalms off)
					auto far_red_less_green = [&](const std::vector<uint8_t> &q) {
						UINT top = H, bottom = 0;
						for (size_t i = 0; i < size_t(W) * H; ++i)
							if (close_to(&domed[i * 4], 0.15f, 0.175f, 0.1f))
							{
								top = std::min(top, UINT(i / W));
								bottom = std::max(bottom, UINT(i / W));
							}
						double rg = 0.0;
						uint32_t n = 0;
						for (size_t i = 0; i < size_t(W) * H; ++i)
							if (close_to(&domed[i * 4], 0.15f, 0.175f, 0.1f) && i / W < top + (bottom - top) / 3)
							{
								rg += double(q[i * 4]) - double(q[i * 4 + 1]);
								n++;
							}
						return n != 0 ? rg / n : 0.0;
					};
					const double rg_calm = far_red_less_green(domed);
					const uint32_t dust_bits = wx_frames(8, "dust");
					const float dust_amount = wx_float(4);
					const uint32_t dust_drawn = ws[12];
					const double rg_dust = far_red_less_green(pixels);
					const uint32_t sand_bits = wx_frames(9, "san1");
					std::printf("  dust storm: bits 0x%X, dust %.2f, drawn 0x%X; the far terrain's red less green %.1f, %.1f in sunshine; sand storm bits 0x%X\n", dust_bits,
						dust_amount, dust_drawn, rg_dust, rg_calm, sand_bits);
					check(dust_bits == (base | kClouds | kDust) && sand_bits == (base | kClouds | kDust) && (dust_drawn & 2) != 0, "dust and sand storms: dust its own (the air pass drawn)");
					check(rg_dust > rg_calm + 2.0, "dust tinting the low view (the terrain browner, far off)");

					// Wind and gales, a heat wave, an aurora, gloom, stellar glare: their bits and parts
					const uint32_t wind_bits = wx_frames(10, "win1");
					const float wind_strength = wx_float(5), wind_leaves = wx_float(15);
					const uint32_t gale_bits = wx_frames(11, "stom");
					const float gale_strength = wx_float(5);
					const uint32_t heat_bits = wx_frames(5, "heat");
					const float heat = wx_float(6);
					const uint32_t heat_drawn = ws[12];
					const uint32_t aurora_bits = wx_frames(16, "aura");
					const float aurora = wx_float(7);
					const std::vector<uint8_t> aurora_picture = pixels;
					const uint32_t gloom_bits = wx_frames(18, "fogd");
					const float gloom = wx_float(8);
					// (how far the terrain's colour moved under gloom's fog: the three channels' differences, on average)
					double terrain_fogged = 0.0;
					uint32_t terrain_n = 0;
					for (size_t i = 0; i < size_t(W) * H; ++i)
						if (close_to(&domed[i * 4], 0.15f, 0.175f, 0.1f))
						{
							for (int k = 0; k < 3; ++k)
								terrain_fogged += std::abs(int(pixels[i * 4 + k]) - int(aurora_picture[i * 4 + k]));
							terrain_n++;
						}
					terrain_fogged /= std::max(1u, terrain_n);
					const uint32_t glare_bits = wx_frames(17, "ligt");
					const float stars = wx_float(9);
					std::printf("  wind bits 0x%X (%.2f, leaves %.2f), gales 0x%X (%.2f); heat wave 0x%X (%.2f, drawn 0x%X); aurora 0x%X (%.2f); gloom 0x%X (%.2f: the terrain's "
						"colour moved %.1f); stellar glare 0x%X (stars x%.1f)\n", wind_bits, wind_strength, wind_leaves, gale_bits, gale_strength, heat_bits, heat, heat_drawn, aurora_bits, aurora,
						gloom_bits, gloom, terrain_fogged, glare_bits, stars);
					check(wind_bits == (base | kClouds | kWind) && gale_bits == (base | kClouds | kWind) && gale_strength > wind_strength && wind_leaves > 0.0f,
						"wind and gales: their wind its own (gales harder), leaves");
					check(heat_bits == (base | kClouds | kHeat) && heat > 0.5f && (heat_drawn & 4) != 0, "a heat wave: heat shimmer drawn");
					check(aurora_bits == (base | kClouds | kAurora) && (night ? aurora > 0.5f : aurora < 0.3f && aurora > 0.0f),
						night ? "an aurora: its own, strong by night" : "an aurora: its own, faint by day");
					check(gloom_bits == (base | kClouds | kGloom) && gloom > 0.5f && terrain_fogged > 3.0, "gloom: fog banks its own, over the terrain");
					check(glare_bits == (base | kClouds) && stars > 2.0f, "stellar glare: brighter stars");

					// WeatherOwn off, then the sky off: not all of the weather World FX's own (its rain and snow still); a zone with no
					// open sky (worldfx-sunlit.txt listing zone 100 only: zone 101): not all of it, and back in zone 100, all again
					set_setting("WeatherOwn", 0.0f);
					const uint32_t own_off_bits = wx_frames(2, "clod");
					const uint32_t own_off_on = ws[0];
					set_setting("WeatherOwn", 1.0f);
					set_setting("Sky", 0.0f);
					const uint32_t sky_off_bits = wx_frames(2, "clod");
					set_setting("Sky", 1.0f);
					if (FILE *sunlit = nullptr; _wfopen_s(&sunlit, L"worldfx-sunlit.txt", L"wb") == 0 && sunlit != nullptr)
					{
						std::fprintf(sunlit, "# e2e: zone 100 only\n100\n");
						fclose(sunlit);
					}
					SetEnvironmentVariableA("FFXI_WORLDFX_ZONE", "101");
					for (int i = 0; i < 40; ++i)
					{
						send();
						render(false);
					}
					const uint32_t indoors_bits = wx_frames(2, "clod");
					SetEnvironmentVariableA("FFXI_WORLDFX_ZONE", "100");
					for (int i = 0; i < 40; ++i)
					{
						send();
						render(false);
					}
					const uint32_t back_bits = wx_frames(2, "clod");
					DeleteFileW(L"worldfx-sunlit.txt");
					// (the weather not sent for over 5 s: nothing of World FX's would stand in for the game's)
					Sleep(5200);
					render(false);
					render(false);
					const uint32_t stale_bits = weather_drawn();
					check(stale_bits == (kRain | kSnow), "the weather not sent for over 5 s: not all of it World FX's own (rain and snow as before)");
					std::printf("  WeatherOwn off: bits 0x%X; the sky off: 0x%X; zone 101 (no open sky): 0x%X; zone 100 again: 0x%X\n", own_off_bits, sky_off_bits, indoors_bits, back_bits);
					check(own_off_bits == (kSky | kRain | kSnow) && own_off_on == 0, "WeatherOwn off: not all of the weather its own (its sky, rain and snow as before)");
					check(sky_off_bits == (kRain | kSnow), "the sky of its own off: not all of the weather its own");
					check(indoors_bits == kSky, "a zone without open sky: the game keeps its weather (no All; no rain map there)");
					check(back_bits == (base | kClouds), "back under open sky: all of the weather its own again");
				}
			}
			set_light(light);
			std::memset(record + 0x4C, 0, 0xB0 - 0x4C); // (no sky in the records after this)
			// (the last weather again, then not sent for over a second: what the crash report check below expects)
			weather_for(12, "snow", "rain", 0.5f, 0.65f);
			Sleep(1100);
			render(false);
			render(false);
		}
	}

	// The add-on loaded worldfx.hlsl (its line in ReShade.log), compiled in the background: the frames went on meanwhile
	// (frames until it's in, two minutes at most), none of them taking seconds (it took over 11 s on the frame and
	// froze the game, 2026-10-21)
	{
		auto read_log = [] {
			std::string text;
			if (FILE *f = _fsopen(WFX_LOG, "rb", _SH_DENYNO); f != nullptr) // (ReShade still has it open)
			{
				char buffer[4096];
				size_t n;
				while ((n = fread(buffer, 1, sizeof(buffer), f)) != 0)
					text.append(buffer, n);
				fclose(f);
			}
			return text;
		};
		const auto wait_start = std::chrono::steady_clock::now();
		std::string log = read_log();
		while (log.find("shaders loaded from worldfx.hlsl") == std::string::npos &&
			std::chrono::steady_clock::now() - wait_start < std::chrono::seconds(120))
		{
			for (int i = 0; i < 30; ++i)
				render(false);
			log = read_log();
		}
		const size_t built_in_line = log.find("FFXI World FX: the built-in shaders, "), loaded_line = log.find("shaders loaded from worldfx.hlsl");
		std::printf("  %s\n  %s\n  the longest frame: %.2f s\n",
			built_in_line != std::string::npos ? log.substr(built_in_line, log.find('\n', built_in_line) - built_in_line).c_str() : "(no built-in shaders line)",
			loaded_line != std::string::npos ? log.substr(loaded_line, log.find('\n', loaded_line) - loaded_line).c_str() : "(no worldfx.hlsl line)",
			longest_frame_s);
		check(log.find("shaders loaded from worldfx.hlsl (compiled in the background") != std::string::npos, "the shaders next to the add-on loaded, compiled in the background (ReShade.log)");
		check(longest_frame_s < 3.0, "no frame took seconds while they compiled");
		check(built_in_line != std::string::npos, "the built-in shaders' line (from the shader cache, or compiled and kept there)");
		// (the first frames come with no characters sent: lamp shadows off until there are, said once; and the zone's
		// line 3 s in, with or without them)
		const size_t people_line = log.find("FFXI World FX: zone 100: ");
		std::printf("  %s\n", people_line != std::string::npos ? log.substr(people_line, log.find('\n', people_line) - people_line).c_str() : "(no zone characters line)");
		check(log.find("no characters from the worldfx addon (none sent, or none for half a second): no lamp shadows until there are") != std::string::npos,
			"no characters sent: lamp shadows off, said in ReShade.log");
		check(people_line != std::string::npos && (log.find("characters and monsters from the worldfx addon, the nearest", people_line) == people_line + 25 ||
			log.find("no characters from the worldfx addon", people_line) == people_line + 25), "the zone's characters, logged once it's in");
		// (the fake game's light, 0.3 0.8 0.2 toward the ground, turned round and made a unit vector)
		check(log.find("the game's light -0.342 -0.912 -0.228") != std::string::npos, "the sun from the game's light (ReShade.log)");
		check(log.find("FFXI World FX: the weather: suny (id 1); fog 204 51 230") != std::string::npos &&
			log.find("FFXI World FX: the weather: rai1 (id 6)") != std::string::npos, "each weather shown logged as it came (ReShade.log)");
		// (the weather's lines, as they came: entering on the stand-in record, then the zone's own)
		std::printf("  ReShade.log's weather lines:\n");
		size_t stand_in_line = std::string::npos, own_line = std::string::npos;
		for (size_t at_line = log.find("FFXI World FX: the weather: "); at_line != std::string::npos; at_line = log.find("FFXI World FX: the weather: ", at_line + 1))
		{
			const size_t end = log.find_first_of("\r\n", at_line);
			const std::string line = log.substr(at_line, end == std::string::npos ? std::string::npos : end - at_line);
			std::printf("    %s\n", line.c_str());
			if (line.find("suny (id 1)") != std::string::npos && line.find("the zone's record not read yet") != std::string::npos && stand_in_line == std::string::npos)
				stand_in_line = at_line;
			if (line.find("suny (id 1)") != std::string::npos && line.find("the zone's light 1.50") != std::string::npos &&
				line.find("sun shadows 100% (the sun's light 0.75 of the clear sky's 0.75)") != std::string::npos && own_line == std::string::npos)
				own_line = at_line;
		}
		check(stand_in_line != std::string::npos && own_line != std::string::npos && own_line > stand_in_line,
			"entering on the game's stand-in record logged as not read yet, then the zone's own record logged with its light (ReShade.log)");
	}

	int errors = 0;
	const UINT64 n = info->GetNumStoredMessages();
	for (UINT64 i = 0; i < n; ++i)
	{
		SIZE_T size = 0;
		info->GetMessage(i, nullptr, &size);
		std::vector<char> mem(size);
		auto *m = reinterpret_cast<D3D12_MESSAGE *>(mem.data());
		info->GetMessage(i, m, &size);
		if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
		{
			std::printf("debug layer: %s\n", m->pDescription);
			errors++;
		}
	}
	check(errors == 0, "no debug-layer errors");
	// (the add-on takes the messages as they come in its debug-layer mode: those are in its file)
	{
		std::string text;
		if (FILE *f = _fsopen("worldfx-d3d12-debug.txt", "rb", _SH_DENYNO); f != nullptr)
		{
			char buffer[4096];
			size_t got;
			while ((got = fread(buffer, 1, sizeof(buffer), f)) != 0)
				text.append(buffer, got);
			fclose(f);
		}
		check(text.find("Direct3D 12 debug layer messages") != std::string::npos, "the add-on's debug-layer file written (worldfx-d3d12-debug.txt)");
		const bool clean = text.find(" ERROR #") == std::string::npos && text.find(" CORRUPTION #") == std::string::npos;
		if (!clean)
			std::printf("%s\n", text.c_str());
		check(clean, "no debug-layer errors in the add-on's file");
		check(text.find(" #1328:") == std::string::npos, "buffers created in the state they start in (no #1328 warning)");
	}

	// The GPU crash report: on before the device was made (ReShade.log), written when the device is removed
	{
		std::string log;
		if (FILE *f = _fsopen(WFX_LOG, "rb", _SH_DENYNO); f != nullptr)
		{
			char buffer[4096];
			size_t got;
			while ((got = fread(buffer, 1, sizeof(buffer), f)) != 0)
				log.append(buffer, got);
			fclose(f);
		}
		check(log.find("GPU crash reports on") != std::string::npos, "GPU crash reports on (ReShade.log)");
		ID3D12Device5 *device5 = nullptr;
		if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5))))
		{
			device5->RemoveDevice();
			device5->Release();
		}
		std::string report;
		for (int i = 0; i < 50 && report.empty(); ++i)
		{
			Sleep(100);
			if (FILE *f = _fsopen("worldfx-gpu-crash.txt", "rb", _SH_DENYNO); f != nullptr)
			{
				char buffer[4096];
				size_t got;
				while ((got = fread(buffer, 1, sizeof(buffer), f)) != 0)
					report.append(buffer, got);
				fclose(f);
			}
		}
		std::printf("---- worldfx-gpu-crash.txt ----\n%s----\n", report.c_str());
		check(report.find("the GPU stopped") != std::string::npos, "the GPU crash report written when the device was removed");
		check(report.find("Command lists the GPU had") != std::string::npos, "the report has Direct3D's record of the command lists");
		check(report.find("The add-on at that moment") != std::string::npos && report.find("blades in") != std::string::npos,
			"the report has the add-on's state and its last drawings");
		check(report.find("frame pacing") != std::string::npos && report.find("presents behind") != std::string::npos,
			"the report has the frame pacing (how far behind the GPU ran)");
		check(report.find("weather: fog on, shadows by it on; snow (id 12), changing from rain") != std::string::npos, "the report has the weather last sent");
		check(report.find("rain on (amount 1.00), raining now 0.00; snow on (amount 1.00), snowing now 0.00; rain map: zone 100") != std::string::npos,
			"the report has the rain and snow settings and the rain map");
		check(report.find("sky of its own: on") != std::string::npos, "the report has the sky of its own");
	}

	// HiFi XI's settings file made from ReShade.ini's [FFXI_WORLDFX], ReShade.ini as it was
	{
		auto read_all = [](const wchar_t *path) {
			std::string s;
			if (FILE *f = nullptr; _wfopen_s(&f, path, L"rb") == 0 && f != nullptr)
			{
				char buffer[1024];
				size_t n;
				while ((n = fread(buffer, 1, sizeof(buffer), f)) != 0)
					s.append(buffer, n);
				fclose(f);
			}
			return s;
		};
		const std::string settings = read_all(settings_path);
		check(settings.find("[worldfx]") != std::string::npos && settings.find("SunShadows=0.6") != std::string::npos &&
			settings.find("D3D12DebugLayer=1") != std::string::npos && settings.find("\n[FFXI_WORLDFX]") == std::string::npos,
			"HiFi XI's own settings file made from ReShade.ini's World FX settings ([worldfx])");
		check(read_all(L"ReShade.ini") == reshade_ini_before, "ReShade.ini left as it was");
	}

	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures == 0 ? 0 : 1;
}
