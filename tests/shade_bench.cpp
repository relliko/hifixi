/*
 * The shading pass (ambient occlusion + sun shadows) timed on the real GPU, not WARP: the first hardware
 * adapter, no debug layer. A flat ground in the depth buffer with a box standing on it (as grass_test 1j),
 * the sun to the camera's right and up. Each frame waits for the GPU (5 s at most: then it says so and stops).
 *
 *   build\shade_bench.exe <width> <height> [shader file, default: the built-in shaders] [frames, default 40] [continuous]
 *                          [map=<shadow file>] [at=x,y,z] [redraw[=degrees a frame]] [lamps=<n>] [actors=<n>] [rain[=amount]] [snow[=amount]] [sky]
 *                          [grass=<blade file>] [wear] [fade] [water=<water file>] [sea[=<sea file>]] [seaoff] [reach=<yalms>]
 *                          [horizon[=degrees[,height,down]]] [level=<y>] [picture=<ppm file>]
 *
 * "continuous": three frames queued at a time instead of a wait after each, so the GPU never idles (the load
 * of the game running uncapped): a stability test of the card at full power as much as a timing.
 * "map=shadow\100.bin": the sun's shadow map from that zone's meshes (tools/bake_shadow_geo.py), the camera in
 * West Ronfaure's woods (-112, 0.2, -496: at=x,y,z elsewhere), the sun 50 degrees up; drawn once and then read
 * each frame, or with "redraw" the sun turning a tenth of a degree each frame (the near cascade drawn again every
 * frame, the far one every other frame: the most it costs), or with "redraw=0.000833" as much as given (that one:
 * the game's tenth of a degree a second at 120 frames a second). The difference is what keeping the map up to date
 * costs. Also printed, frame by frame: the median and tenth percentile of the pass's GPU time.
 * "lamps=16": that many lit street lamps (at most kMaxLamps) 2.5 yalms over the ground on a 3-yalm grid around the
 * looked-at point, their glow reaching 6 yalms, lamp shadows at 0.5: most of the ground near the camera in reach of
 * several (the march toward the strongest runs there), the most the lamps cost.
 * "actors=8": that many characters near, casting sun shadows ("Characters cast sun shadows too"): the box as one
 * (size 2), the rest (size 1.2) on a 3-yalm grid across the ground in view, nothing of them drawn (their shapes are
 * looked for along every ray through them and not found: the most it costs). With the sun this low (19 degrees), each
 * one's shadow runs about 7 yalms across the screen.
 * "rain" (or "rain=0.55", rain's own amount): the add-on's rain over a flat rain map of the ground 60 yalms each way,
 * a thunderstorm's (every column with a drop, all its splashes): its GPU time is printed as what follows the shading.
 * "snow" (or "snow=0.6", snow's own amount): the add-on's snow over the same map, a blizzard's (a flake in every
 * column, blown hard across); with "rain" too, both (as while one weather changes into the other).
 * "sky": the add-on's own sky, the camera looking 20 degrees up over the ground (most of the picture sky), every sky
 * pixel repainted (the record's 8 stops the colour the world is cleared to, so it's all still the game's "dome"), its
 * sun in view and its stars on: the most it costs. Compare with the same camera without it ("sky=0").
 * "grass=grass\100.bin": that zone's blades drawn too (the camera at=x,y,z among them: West Ronfaure's woods by
 * default), their GPU time printed. "wear": with the grass's wear on, someone fighting where the camera looks and a
 * weapon skill's cut there every 2 s: what the wear costs over the same grass without it.
 * "fade": the look's clock, 60 frames a second, as the add-on has it: the shadow map drawn about once a second and
 * faded between drawings (the sun pass reading two of each); with redraw=..., against the same without it.
 * "weather=clod" (any weather's name): the weather of its own as the core has it for that weather (weather_look on a noon
 * record: its clouds, wind, dust, heat, aurora (as by day), gloom, stars), with "sky" over the sky of its own; "flash":
 * its lightning at a flash's peak, a bolt in view. The air pass and the flash's are in what follows the shading
 * (printed); the clouds, aurora, bolt and heat shimmer in the shading pass. Compare with the same without it.
 * "dome=noon" (or dusk, night: with "sky"): the game's sky dome drawn each frame from a record's gradient (West Ronfaure's
 * sunshine at noon; a dusk and a night one made up for the bench), as the game draws it (grass_test 1x), the sun (or moon)
 * where that hour has it, so the sky of its own and its clouds come out as in game: for pictures. "clouds=2": the clouds'
 * detail (CloudQuality, 0..2; 1 by default); "style=1": the volumetric clouds (CloudStyle; 0 game-like, the default). "pitch=35": the sky's camera that many degrees up (20 by default); "spin=0.5":
 * turning that many degrees a frame (the clouds' picture carried over as it turns). With clouds, their pass's own GPU
 * time is printed (and every section's). "sun=30,90": the sun (or moon) that many degrees up at that bearing (degrees from
 * north toward east) instead of where the hour has it. "over=60": the sky's camera that many yalms over the ground (2 by
 * default; the clouds' shadows on the ground show from up there).
 * "warp": on WARP, Direct3D 12's software device, rather than the GPU (while the game runs on it: the costs relative to
 * each other, at a smaller size).
 * "water=water\4.bin": that zone's baked water (WTR2) drawn; "sea=sea\4.bin" its sea sheets (SEA1: HiFi XI's bake 'sea')
 * with it, out to reach=<yalms> (1000 by default: where the game's fog would end), "seaoff" loaded but WaterSea off; "sea"
 * alone, one sheet 8,000 yalms square half a yalm over the flat ground. "horizon" (or "horizon=235": the bearing, degrees
 * from +x toward +z): the camera 4 yalms over the water (level=<y>, else the sea file's, else the flat ground's less half a yalm)
 * at=x,y,z (y the flat ground: under the water), looking out along it 2 degrees down ("horizon=227,40,8": 40 yalms
 * over it, 8 degrees down), the water's cost alone (no AO, no
 * sun shadows): what the far water costs, out to the horizon. Bibiki Bay (4): water=... sea=... at=-380,3.3,-360
 * horizon=227 level=0.3, the camera on its baked water looking out to sea. "picture=sea.ppm": the last frame written there.
 */

#include "grass_render.hpp" // (HiFi XI's render engine: ..\hifixi\src\render)
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace grass;

namespace
{
	template <typename T>
	void release(T *&p)
	{
		if (p != nullptr)
			p->Release();
		p = nullptr;
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

	void look_at(vec3 eye, vec3 at, vec3 up, float m[16])
	{
		const vec3 z = normalize(sub(eye, at));
		const vec3 x = normalize(cross(up, z));
		const vec3 y = cross(z, x);
		const float v[16] = {
			x.x, y.x, z.x, 0,
			x.y, y.y, z.y, 0,
			x.z, y.z, z.z, 0,
			-dot(x, eye), -dot(y, eye), -dot(z, eye), 1 };
		std::memcpy(m, v, sizeof(v));
	}

	void projection(float aspect, float m[16])
	{
		const float b = 1.26f;
		const float p[16] = {
			b / aspect, 0, 0, 0,
			0, b, 0, 0,
			0, 0, -1, -1,
			0, 0, -0.1f, 0 };
		std::memcpy(m, p, sizeof(p));
	}

	bool invert(const float m[16], float out[16])
	{
		double a[4][8];
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 8; ++c)
				a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
		for (int c = 0; c < 4; ++c)
		{
			int best = c;
			for (int r = c + 1; r < 4; ++r)
				if (std::fabs(a[r][c]) > std::fabs(a[best][c]))
					best = r;
			if (std::fabs(a[best][c]) < 1e-12)
				return false;
			for (int k = 0; k < 8; ++k)
				std::swap(a[c][k], a[best][k]);
			const double d = a[c][c];
			for (int k = 0; k < 8; ++k)
				a[c][k] /= d;
			for (int r = 0; r < 4; ++r)
				if (r != c)
				{
					const double f = a[r][c];
					for (int k = 0; k < 8; ++k)
						a[r][k] -= f * a[c][k];
				}
		}
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 4; ++c)
				out[r * 4 + c] = float(a[r][c + 4]);
		return true;
	}

	const char kGroundShader[] = R"(
cbuffer c : register(b0) { row_major float4x4 vp; row_major float4x4 inv_vp; float4 info; };
float4 vs_main(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float ps_main(float4 pos : SV_Position) : SV_Depth
{
	float2 ndc = float2(pos.x / info.y * 2.0 - 1.0, 1.0 - pos.y / info.z * 2.0);
	float4 a = mul(float4(ndc, 0.0, 1.0), inv_vp);
	float4 b = mul(float4(ndc, 0.5, 1.0), inv_vp);
	a /= a.w;
	b /= b.w;
	float3 dir = b.xyz - a.xyz;
	if (abs(dir.y) < 1e-6)
		return 1.0;
	float t = (info.x - a.y) / dir.y;
	if (t <= 0.0)
		return 1.0;
	float4 c = mul(float4(a.xyz + dir * t, 1.0), vp);
	return saturate(c.z / c.w);
}
)";
}

int main(int argc, char **argv)
{
	// (map=..., at=... and redraw anywhere; the rest by position)
	const char *map_file = nullptr;
	bool at_given = false;
	float at_xyz[3] = { -112.0f, 0.2f, -496.0f };
	float turn = 0.0f; // (degrees the sun turns a frame: "redraw")
	int lamp_count = 0, actor_count = 0;
	int sky = -1; // (-1: the usual camera; 0: the sky's camera, the sky off; 1: on)
	float rain = 0.0f, snow = 0.0f;
	const char *grass_file = nullptr;
	bool wear = false, fade = false, warp = false, flash = false;
	const char *weather_name_given = nullptr;
	const char *water_file = nullptr, *sea_file = nullptr;
	bool sea_synthetic = false, sea_off = false;
	float reach = 1000.0f, horizon = -1000.0f, level_given = -1e30f; // (horizon: the bearing in degrees, or none)
	float horizon_height = 4.0f, horizon_down = 2.0f;
	const char *picture_file = nullptr;
	const char *dome_given = nullptr;
	int cloud_quality = 1, cloud_style = 0;
	float pitch = 20.0f, spin = 0.0f, sun_elev = -1000.0f, sun_bearing = 0.0f, over = 2.0f;
	std::vector<char *> pos;
	for (int i = 1; i < argc; ++i)
		if (std::strncmp(argv[i], "map=", 4) == 0)
			map_file = argv[i] + 4;
		else if (std::strncmp(argv[i], "at=", 3) == 0)
			at_given = sscanf_s(argv[i] + 3, "%f,%f,%f", &at_xyz[0], &at_xyz[1], &at_xyz[2]) == 3;
		else if (std::strcmp(argv[i], "redraw") == 0)
			turn = 0.1f;
		else if (std::strncmp(argv[i], "redraw=", 7) == 0)
			turn = float(std::atof(argv[i] + 7));
		else if (std::strncmp(argv[i], "lamps=", 6) == 0)
			lamp_count = std::clamp(std::atoi(argv[i] + 6), 0, int(kMaxLamps));
		else if (std::strncmp(argv[i], "actors=", 7) == 0)
			actor_count = std::clamp(std::atoi(argv[i] + 7), 0, int(kMaxActors));
		else if (std::strcmp(argv[i], "rain") == 0)
			rain = 1.0f;
		else if (std::strncmp(argv[i], "rain=", 5) == 0)
			rain = std::clamp(float(std::atof(argv[i] + 5)), 0.0f, 2.0f);
		else if (std::strcmp(argv[i], "sky") == 0)
			sky = 1;
		else if (std::strcmp(argv[i], "sky=0") == 0)
			sky = 0;
		else if (std::strcmp(argv[i], "snow") == 0)
			snow = 1.0f;
		else if (std::strncmp(argv[i], "snow=", 5) == 0)
			snow = std::clamp(float(std::atof(argv[i] + 5)), 0.0f, 2.0f);
		else if (std::strncmp(argv[i], "grass=", 6) == 0)
			grass_file = argv[i] + 6;
		else if (std::strcmp(argv[i], "wear") == 0)
			wear = true;
		else if (std::strcmp(argv[i], "fade") == 0)
			fade = true;
		else if (std::strcmp(argv[i], "warp") == 0)
			warp = true;
		else if (std::strcmp(argv[i], "flash") == 0)
			flash = true;
		else if (std::strncmp(argv[i], "weather=", 8) == 0 && std::strlen(argv[i]) == 12)
			weather_name_given = argv[i] + 8;
		else if (std::strncmp(argv[i], "water=", 6) == 0)
			water_file = argv[i] + 6;
		else if (std::strcmp(argv[i], "sea") == 0)
			sea_synthetic = true;
		else if (std::strncmp(argv[i], "sea=", 4) == 0)
			sea_file = argv[i] + 4;
		else if (std::strcmp(argv[i], "seaoff") == 0)
			sea_off = true;
		else if (std::strncmp(argv[i], "reach=", 6) == 0)
			reach = float(std::atof(argv[i] + 6));
		else if (std::strncmp(argv[i], "picture=", 8) == 0)
			picture_file = argv[i] + 8;
		else if (std::strncmp(argv[i], "dome=", 5) == 0)
			dome_given = argv[i] + 5;
		else if (std::strncmp(argv[i], "style=", 6) == 0)
			cloud_style = std::clamp(std::atoi(argv[i] + 6), 0, 1);
		else if (std::strncmp(argv[i], "clouds=", 7) == 0)
			cloud_quality = std::clamp(std::atoi(argv[i] + 7), 0, 2);
		else if (std::strncmp(argv[i], "pitch=", 6) == 0)
			pitch = float(std::atof(argv[i] + 6));
		else if (std::strncmp(argv[i], "spin=", 5) == 0)
			spin = float(std::atof(argv[i] + 5));
		else if (std::strncmp(argv[i], "over=", 5) == 0)
			over = float(std::atof(argv[i] + 5));
		else if (std::strncmp(argv[i], "sun=", 4) == 0)
			sscanf_s(argv[i] + 4, "%f,%f", &sun_elev, &sun_bearing);
		else if (std::strncmp(argv[i], "level=", 6) == 0)
			level_given = float(std::atof(argv[i] + 6));
		else if (std::strcmp(argv[i], "horizon") == 0)
			horizon = 0.0f;
		else if (std::strncmp(argv[i], "horizon=", 8) == 0)
			sscanf_s(argv[i] + 8, "%f,%f,%f", &horizon, &horizon_height, &horizon_down);
		else
			pos.push_back(argv[i]);
	if (pos.size() < 2)
	{
		std::printf("usage: shade_bench <width> <height> [shader file|-] [frames] [continuous] [map=<file>] [at=x,y,z] [redraw[=degrees a frame]] [lamps=<n>] [actors=<n>] [rain[=amount]] [snow[=amount]] [sky|sky=0] [grass=<file>] [wear] [fade] [weather=<name>] [flash] [water=<file>] [sea[=<file>]] [seaoff] [reach=<yalms>] [horizon[=degrees[,height,down]]] [level=<y>] [picture=<ppm>] [dome=noon|dusk|night] [clouds=<0..2>] [pitch=<degrees>] [spin=<degrees>] [warp]\n");
		return 2;
	}
	const UINT W = UINT(std::atoi(pos[0])), H = UINT(std::atoi(pos[1]));
	const char *shader_file = pos.size() > 2 && std::strcmp(pos[2], "-") != 0 ? pos[2] : nullptr;
	const int frames = pos.size() > 3 ? std::atoi(pos[3]) : 40;
	const bool continuous = pos.size() > 4 && std::strcmp(pos[4], "continuous") == 0;
	constexpr UINT kQueued = 3;

	const auto compile_fn = reinterpret_cast<pD3DCompile>(GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile"));
	HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
	const auto create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
	using create_factory_fn = HRESULT(WINAPI *)(REFIID, void **);
	const auto create_factory = reinterpret_cast<create_factory_fn>(GetProcAddress(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1"));

	IDXGIFactory1 *factory = nullptr;
	IDXGIAdapter1 *adapter = nullptr;
	ID3D12Device *device = nullptr;
	create_factory(IID_PPV_ARGS(&factory));
	if (warp)
	{
		IDXGIFactory4 *factory4 = nullptr;
		if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory4))) && SUCCEEDED(factory4->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
			SUCCEEDED(create_device(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
			std::printf("adapter: WARP\n");
		release(factory4);
	}
	for (UINT i = 0; device == nullptr && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
	{
		DXGI_ADAPTER_DESC1 d = {};
		adapter->GetDesc1(&d);
		if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && SUCCEEDED(create_device(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
		{
			std::printf("adapter: %ls\n", d.Description);
			break;
		}
		release(adapter);
	}
	if (device == nullptr)
	{
		std::printf("no hardware device\n");
		return 1;
	}
	ID3D12CommandQueue *queue = nullptr;
	ID3D12CommandAllocator *allocator = nullptr;
	ID3D12CommandAllocator *queued[kQueued] = {};
	UINT64 queued_value[kQueued] = {};
	ID3D12GraphicsCommandList *list = nullptr;
	ID3D12Fence *fence = nullptr;
	UINT64 fence_value = 0;
	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
	for (ID3D12CommandAllocator *&a : queued)
		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	// (continuous: submit, then take the next of the queued allocators once the GPU is done with its last frame)
	UINT slot = 0;
	auto submit_queued = [&]() {
		list->Close();
		ID3D12CommandList *lists[] = { list };
		queue->ExecuteCommandLists(1, lists);
		queue->Signal(fence, ++fence_value);
		queued_value[slot] = fence_value;
		slot = (slot + 1) % kQueued;
		if (fence->GetCompletedValue() < queued_value[slot])
		{
			fence->SetEventOnCompletion(queued_value[slot], event);
			if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
				return false;
		}
		if (device->GetDeviceRemovedReason() != S_OK)
			return false;
		queued[slot]->Reset();
		list->Reset(queued[slot], nullptr);
		return true;
	};
	auto run = [&]() {
		if (continuous)
			return submit_queued();
		list->Close();
		ID3D12CommandList *lists[] = { list };
		queue->ExecuteCommandLists(1, lists);
		queue->Signal(fence, ++fence_value);
		fence->SetEventOnCompletion(fence_value, event);
		if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
			return false;
		allocator->Reset();
		list->Reset(allocator, nullptr);
		return true;
	};

	std::string error;
	renderer r;
	if (!r.init(device, error))
	{
		std::printf("renderer init: %s\n", error.c_str());
		return 1;
	}
	if (shader_file != nullptr)
	{
		std::string source;
		if (FILE *f = nullptr; fopen_s(&f, shader_file, "rb") == 0 && f != nullptr)
		{
			char buf[4096];
			for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) != 0;)
				source.append(buf, n);
			std::fclose(f);
		}
		if (source.empty() || !r.set_shader_source(source, error))
		{
			std::printf("shader %s: %s\n", shader_file, error.c_str());
			return 1;
		}
		std::printf("shaders: %s\n", shader_file);
	}
	UINT64 ticks = 0;
	queue->GetTimestampFrequency(&ticks);
	r.set_timestamp_frequency(ticks);

	// "look=<ppm>": the look pass alone (tone mapping, bloom, the colour grade) over a picture (P6, scaled to width x
	// height), nothing else of World FX's on: its GPU time by section over the frames (a sixtieth of a second apart on
	// the look's clock, the exposure set at once on the first), the last frame written to picture=<ppm>. "hour=<h>": the
	// grade's Vana'diel hour (12 by default; the night as the core has it), "weather=<name>" its weather (fine by
	// default), "indoors": no open sky (no grade); "tonemap=0", "bloom=<x>", "grade=<x>" the settings (1 by default).
	for (int i = 1; i < argc; ++i)
		if (std::strncmp(argv[i], "look=", 5) == 0)
		{
			float hour = 12.0f, bloom_amount = 1.0f, grade_amount = 1.0f;
			bool tonemap = true, sunlit = true;
			for (int k = 1; k < argc; ++k)
				if (std::strncmp(argv[k], "hour=", 5) == 0)
					hour = float(std::atof(argv[k] + 5));
				else if (std::strcmp(argv[k], "indoors") == 0)
					sunlit = false;
				else if (std::strcmp(argv[k], "tonemap=0") == 0)
					tonemap = false;
				else if (std::strncmp(argv[k], "bloom=", 6) == 0)
					bloom_amount = float(std::atof(argv[k] + 6));
				else if (std::strncmp(argv[k], "grade=", 6) == 0)
					grade_amount = float(std::atof(argv[k] + 6));
			// The picture
			UINT pw = 0, ph = 0;
			std::vector<uint8_t> rgb;
			if (FILE *f = nullptr; fopen_s(&f, argv[i] + 5, "rb") == 0 && f != nullptr)
			{
				int maxval = 0;
				if (fscanf_s(f, "P6 %u %u %d", &pw, &ph, &maxval) == 3 && maxval == 255 && pw > 0 && ph > 0)
				{
					std::fgetc(f);
					rgb.resize(size_t(pw) * ph * 3);
					if (std::fread(rgb.data(), 1, rgb.size(), f) != rgb.size())
						rgb.clear();
				}
				std::fclose(f);
			}
			if (rgb.empty())
			{
				std::printf("look: can't read %s (a P6 picture)\n", argv[i] + 5);
				return 1;
			}
			D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, upl = { D3D12_HEAP_TYPE_UPLOAD }, rbk = { D3D12_HEAP_TYPE_READBACK };
			D3D12_RESOURCE_DESC td = {};
			td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			td.Width = W;
			td.Height = H;
			td.DepthOrArraySize = 1;
			td.MipLevels = 1;
			td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // (dgVoodoo's)
			td.SampleDesc.Count = 1;
			td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			ID3D12Resource *world = nullptr, *depth = nullptr, *up = nullptr, *back = nullptr;
			device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&world));
			D3D12_RESOURCE_DESC dd = td;
			dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			D3D12_CLEAR_VALUE dc = { DXGI_FORMAT_D24_UNORM_S8_UINT };
			dc.DepthStencil.Depth = 1.0f;
			device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc, IID_PPV_ARGS(&depth));
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
			UINT64 total = 0;
			device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = total;
			bd.Height = 1;
			bd.DepthOrArraySize = 1;
			bd.MipLevels = 1;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			device->CreateCommittedResource(&upl, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up));
			device->CreateCommittedResource(&rbk, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&back));
			if (world == nullptr || depth == nullptr || up == nullptr || back == nullptr)
			{
				std::printf("look: no resources\n");
				return 1;
			}
			{
				void *m = nullptr;
				up->Map(0, nullptr, &m);
				for (UINT y = 0; y < H; ++y)
				{
					uint8_t *row = static_cast<uint8_t *>(m) + fp.Offset + size_t(y) * fp.Footprint.RowPitch;
					for (UINT x = 0; x < W; ++x)
					{
						const uint8_t *s = &rgb[(size_t(y * ph / H) * pw + x * pw / W) * 3];
						row[x * 4 + 0] = s[2];
						row[x * 4 + 1] = s[1];
						row[x * 4 + 2] = s[0];
						row[x * 4 + 3] = 255;
					}
				}
				up->Unmap(0, nullptr);
			}
			ID3D12DescriptorHeap *rtv_heap = nullptr;
			D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
			device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
			device->CreateRenderTargetView(world, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
			target t;
			t.rt = world;
			t.ds = depth;
			t.rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
			t.rtv_format = DXGI_FORMAT_B8G8R8A8_UNORM;
			t.dsv_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			camera cam;
			look_at({ 0.0f, -2.0f, 0.0f }, { 0.0f, -2.0f, 10.0f }, { 0.0f, -1.0f, 0.0f }, cam.view);
			projection(float(W) / H, cam.proj);
			look l;
			l.grass = false;
			l.ao_strength = l.relief = l.sun_shadow = 0.0f;
			l.water = l.sea = false;
			l.fireflies = 0.0f;
			l.post.tonemap = tonemap;
			l.post.bloom = bloom_amount;
			// (the core's: the sun's height by the hour, the night from it; the exposure no brighter by night)
			const double a = 3.14159265358979 * (hour - 6.0) / 12.0;
			const float sun_up = float(std::sin(a)), night = std::clamp(-sun_up / 0.2f, 0.0f, 1.0f);
			l.post.exposure_max += (1.0f - l.post.exposure_max) * night;
			weather w;
			const char *wn = weather_name_given != nullptr ? weather_name_given : "fine";
			w.known = true;
			w.id = 0;
			w.name = uint32_t(uint8_t(wn[0])) | uint32_t(uint8_t(wn[1])) << 8 | uint32_t(uint8_t(wn[2])) << 16 | uint32_t(uint8_t(wn[3])) << 24;
			float parts[8] = {};
			colour_grade(sun_up, night, sunlit, &w, grade_amount, l.post, parts);
			std::printf("look: %ux%u from %s (%ux%u), hour %.1f, %s%s; grade: gain %.3f %.3f %.3f, saturation %.3f, contrast %.3f, lift %.3f %.3f %.3f%s\n",
				W, H, argv[i] + 5, pw, ph, hour, wn, sunlit ? "" : ", indoors", l.post.gain[0], l.post.gain[1], l.post.gain[2], l.post.saturation,
				l.post.contrast, l.post.lift[0], l.post.lift[1], l.post.lift[2], l.post.grade ? "" : " (off)");
			const int n = std::max(frames, 1);
			for (int f = 0; f < n; ++f)
			{
				{
					D3D12_RESOURCE_BARRIER b = {};
					b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					b.Transition.pResource = world;
					b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
					b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
					list->ResourceBarrier(1, &b);
					D3D12_TEXTURE_COPY_LOCATION dst = { world, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
					D3D12_TEXTURE_COPY_LOCATION src = { up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
					src.PlacedFootprint = fp;
					list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
					std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
					list->ResourceBarrier(1, &b);
				}
				l.clock = 100.0 + f / 60.0;
				l.post.snap = f == 0;
				t.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
				t.ds_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
				const draw_stats s = r.draw(list, t, cam, l, error);
				if (!error.empty() || !s.look)
				{
					std::printf("look: not drawn (%s)\n", error.c_str());
					return 1;
				}
				if (f + 1 == n)
				{
					D3D12_RESOURCE_BARRIER b = {};
					b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					b.Transition.pResource = world;
					b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
					b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
					list->ResourceBarrier(1, &b);
					D3D12_TEXTURE_COPY_LOCATION dst = { back, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
					dst.PlacedFootprint = fp;
					D3D12_TEXTURE_COPY_LOCATION src = { world, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
					list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
					std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
					list->ResourceBarrier(1, &b);
				}
				if (!run())
				{
					std::printf("look: the GPU didn't finish a frame\n");
					return 1;
				}
				r.release_old();
			}
			std::printf("look: %d frames\n", n);
			for (const auto &sct : r.gpu_sections())
				std::printf("  %-36ls %.3f ms (%u frames)\n", sct.name.c_str(), sct.ms, sct.frames);
			if (picture_file != nullptr)
				if (FILE *f = nullptr; fopen_s(&f, picture_file, "wb") == 0 && f != nullptr)
				{
					void *m = nullptr;
					back->Map(0, nullptr, &m);
					std::fprintf(f, "P6 %u %u 255\n", W, H);
					std::vector<uint8_t> row(size_t(W) * 3);
					for (UINT y = 0; y < H; ++y)
					{
						const uint8_t *s = static_cast<const uint8_t *>(m) + fp.Offset + size_t(y) * fp.Footprint.RowPitch;
						for (UINT x = 0; x < W; ++x)
						{
							row[x * 3 + 0] = s[x * 4 + 2];
							row[x * 3 + 1] = s[x * 4 + 1];
							row[x * 3 + 2] = s[x * 4 + 0];
						}
						std::fwrite(row.data(), 1, row.size(), f);
					}
					back->Unmap(0, nullptr);
					std::fclose(f);
					std::printf("picture: %s\n", picture_file);
				}
			return 0;
		}

	const vec3 at = map_file != nullptr || at_given || grass_file != nullptr ? vec3 { at_xyz[0], at_xyz[1], at_xyz[2] } : vec3 { 0.0f, 0.0f, 0.0f };
	uint32_t map_triangles = 0;
	if (map_file != nullptr)
	{
		shadow_file zone;
		const std::wstring wide(map_file, map_file + std::strlen(map_file));
		if (!read_shadow_file(wide.c_str(), zone, error) || !r.upload_shadow(zone, error))
		{
			std::printf("shadow map %s: %s\n", map_file, error.c_str());
			return 1;
		}
		map_triangles = zone.triangle_count();
		std::printf("shadow map: %s, %u triangles\n", map_file, map_triangles);
	}
	if (grass_file != nullptr)
	{
		blade_file blades;
		const std::wstring wide(grass_file, grass_file + std::strlen(grass_file));
		if (!read_blade_file(wide.c_str(), blades, error) || !r.upload(blades, error))
		{
			std::printf("grass %s: %s\n", grass_file, error.c_str());
			return 1;
		}
		std::printf("grass: %s, %u blades%s\n", grass_file, blades.blade_count(), wear ? ", its wear on" : "");
	}
	// The zone's water and its sea (or the sea alone, one sheet over the flat ground): the water's level, the camera's for the
	// horizon
	uint32_t water_corners = 0, sea_corners = 0, sea_sheets = 0;
	float level = at.y - 0.5f;
	if (water_file != nullptr)
	{
		blade_file w;
		const std::wstring wide(water_file, water_file + std::strlen(water_file));
		if (!read_blade_file(wide.c_str(), w, error, "WTR2", 8) || !r.upload_water(w, error))
		{
			std::printf("water %s: %s\n", water_file, error.c_str());
			return 1;
		}
		water_corners = w.blade_count();
	}
	if (sea_file != nullptr || sea_synthetic)
	{
		blade_file sea;
		if (sea_file != nullptr)
		{
			const std::wstring wide(sea_file, sea_file + std::strlen(sea_file));
			if (!read_sea_file(wide.c_str(), sea, sea_sheets, error))
			{
				std::printf("sea %s: %s\n", sea_file, error.c_str());
				return 1;
			}
			level = sea.blades[4]; // (its first corner's surface)
		}
		else
		{
			sea.zone = 1;
			sea.stride = 8;
			sea_sheets = 1;
			const float x0 = at.x - 4000.0f, x1 = at.x + 4000.0f, z0 = at.z - 4000.0f, z1 = at.z + 4000.0f;
			const float q6[6][2] = { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z0 }, { x1, z1 }, { x0, z1 } };
			for (const auto &c : q6)
				sea.blades.insert(sea.blades.end(), { c[0], level + 30.0f, c[1], 200.0f, level, 8.0f, 0.0f, 0.0f });
		}
		if (!r.upload_sea(sea, error))
		{
			std::printf("sea: %s\n", error.c_str());
			return 1;
		}
		sea_corners = sea.blade_count();
	}
	if (level_given > -1e29f)
		level = level_given;
	if (water_file != nullptr || sea_corners != 0)
		std::printf("water: %u corners; sea: %u sheets, %u corners%s, out to %.0f yalms; its level %.2f\n", water_corners, sea_sheets, sea_corners,
			sea_off ? " (WaterSea off)" : "", reach, level);
	vec3 eye = { at.x + 4.0f, at.y - 2.0f, at.z + 3.0f };
	camera cam;
	look_at(eye, at, { 0.0f, -1.0f, 0.0f }, cam.view);
	if (horizon > -999.0f) // (4 yalms over the water, along the bearing, 2 degrees down: as given)
	{
		const float a = horizon * 3.14159265f / 180.0f, down = std::tan(horizon_down * 3.14159265f / 180.0f);
		eye = { at.x, level - horizon_height, at.z };
		look_at(eye, { eye.x + 100.0f * std::cos(a), eye.y + 100.0f * down, eye.z + 100.0f * std::sin(a) }, { 0.0f, -1.0f, 0.0f }, cam.view);
	}
	// (the sky's camera: 2 yalms over the ground looking `pitch` degrees up, toward a bearing of 37 degrees, turned by
	// `spin` each frame)
	auto sky_view = [&](int frame) {
		const float up = pitch * 3.14159265f / 180.0f, b = (36.87f + spin * frame) * 3.14159265f / 180.0f;
		look_at({ at.x, at.y - over, at.z }, { at.x + 10.0f * std::sin(b) * std::cos(up), at.y - over - 10.0f * std::sin(up), at.z + 10.0f * std::cos(b) * std::cos(up) },
			{ 0.0f, -1.0f, 0.0f }, cam.view);
	};
	if (sky >= 0)
		sky_view(0);
	projection(float(W) / H, cam.proj);
	float vp[16];
	multiply(cam.view, cam.proj, vp);
	const float cx = at.x * vp[0] + at.y * vp[4] + at.z * vp[8] + vp[12], cy = at.x * vp[1] + at.y * vp[5] + at.z * vp[9] + vp[13];
	const float cw = at.x * vp[3] + at.y * vp[7] + at.z * vp[11] + vp[15];
	const float px = (cx / cw + 1.0f) * 0.5f * W, py = (1.0f - cy / cw) * 0.5f * H;
	const vec3 q = { at.x + (eye.x - at.x) * 0.1f, at.y + (eye.y - at.y) * 0.1f, at.z + (eye.z - at.z) * 0.1f };
	const float qz = q.x * vp[2] + q.y * vp[6] + q.z * vp[10] + vp[14], qw = q.x * vp[3] + q.y * vp[7] + q.z * vp[11] + vp[15];
	const float s = W / 640.0f;
	const D3D12_RECT box = { LONG(px - 60 * s), LONG(py - 90 * s), LONG(px + 60 * s), LONG(py - 12 * s) };

	D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = W;
	desc.Height = H;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	const D3D12_CLEAR_VALUE rt_clear = { DXGI_FORMAT_R8G8B8A8_UNORM, { 0.45f, 0.42f, 0.36f, 0.5f } };
	ID3D12Resource *world = nullptr, *depth = nullptr;
	device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &rt_clear, IID_PPV_ARGS(&world));
	desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	D3D12_CLEAR_VALUE ds_clear = { DXGI_FORMAT_D24_UNORM_S8_UINT };
	ds_clear.DepthStencil.Depth = 1.0f;
	device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &ds_clear, IID_PPV_ARGS(&depth));
	ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap));
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart(), dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
	device->CreateRenderTargetView(world, nullptr, rtv);
	device->CreateDepthStencilView(depth, nullptr, dsv);

	ID3D12RootSignature *ground_root = nullptr;
	ID3D12PipelineState *ground_pso = nullptr;
	{
		D3D12_ROOT_PARAMETER gp = {};
		gp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		gp.Constants = { 0, 0, 36 };
		D3D12_ROOT_SIGNATURE_DESC grs = { 1, &gp };
		ID3DBlob *blob = nullptr, *err = nullptr;
		reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))(&grs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
		device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&ground_root));
		release(blob);
		release(err);
		ID3DBlob *vs = nullptr, *ps = nullptr;
		compile_fn(kGroundShader, sizeof(kGroundShader) - 1, "ground", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vs, nullptr);
		compile_fn(kGroundShader, sizeof(kGroundShader) - 1, "ground", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &ps, nullptr);
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
		d.pRootSignature = ground_root;
		d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		d.SampleMask = UINT_MAX;
		d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		d.DepthStencilState.DepthEnable = TRUE;
		d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		d.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
		d.SampleDesc.Count = 1;
		device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&ground_pso));
		release(vs);
		release(ps);
	}

	// The game's sky dome (dome=...): its pipeline (position and a byte colour a vertex, Gouraud, triangle strips, no depth)
	// and its strips as 03D640 makes them (grass_test 1x draws it the same way)
	const char kDomeShader[] = R"(
cbuffer c : register(b0) { row_major float4x4 vp; };
struct vin { float3 p : POSITION; float4 c : COLOR; };
struct vout { float4 pos : SV_Position; float4 c : COLOR; };
vout vs_main(vin i) { vout o; o.pos = mul(float4(i.p, 1.0), vp); o.c = i.c; return o; }
float4 ps_main(vout i) : SV_Target { return i.c; }
)";
	ID3D12RootSignature *dome_root = nullptr;
	ID3D12PipelineState *dome_pso = nullptr;
	ID3D12Resource *dome_vb = nullptr;
	struct dome_vertex
	{
		float x, y, z;
		uint8_t c[4];
	};
	std::vector<dome_vertex> dome_vertices;
	std::vector<std::pair<UINT, UINT>> dome_runs;
	if (dome_given != nullptr)
	{
		D3D12_ROOT_PARAMETER dp = {};
		dp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		dp.Constants = { 0, 0, 16 };
		D3D12_ROOT_SIGNATURE_DESC drs = { 1, &dp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
		ID3DBlob *blob = nullptr, *err = nullptr;
		reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))(&drs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
		device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&dome_root));
		release(blob);
		release(err);
		ID3DBlob *vs = nullptr, *ps = nullptr;
		compile_fn(kDomeShader, sizeof(kDomeShader) - 1, "dome", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vs, nullptr);
		compile_fn(kDomeShader, sizeof(kDomeShader) - 1, "dome", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &ps, nullptr);
		const D3D12_INPUT_ELEMENT_DESC layout[2] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
		d.pRootSignature = dome_root;
		d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		d.SampleMask = UINT_MAX;
		d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		d.RasterizerState.DepthClipEnable = TRUE;
		d.InputLayout = { layout, 2 };
		d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		d.NumRenderTargets = 1;
		d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		d.SampleDesc.Count = 1;
		device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&dome_pso));
		release(vs);
		release(ps);
	}

	target t;
	t.rt = world;
	t.ds = depth;
	t.rtv = rtv;
	t.rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
	t.dsv_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	t.rt_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	t.ds_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	look l;
	l.grass = grass_file != nullptr;
	l.ao_strength = 1.0f;
	l.relief = 0.0f;
	const vec3 to_sun = normalize({ cam.view[0], cam.view[4] - 0.35f, cam.view[8] });
	l.sun[0] = to_sun.x;
	l.sun[1] = to_sun.y;
	l.sun[2] = to_sun.z;
	l.sun_shadow = 0.5f;
	l.water = water_file != nullptr || sea_corners != 0;
	l.sea = !sea_off;
	l.sea_distance = reach;
	l.glint = 1.0f;
	l.time = 3.0f;
	if (horizon > -999.0f) // (the water's cost alone)
	{
		l.ao_strength = 0.0f;
		l.sun_shadow = 0.0f;
	}
	l.sun_map = map_file != nullptr;
	std::vector<float> lamps;
	for (int i = 0; i < lamp_count; ++i)
	{
		const float strength = 0.5f * (1.0f - std::exp(-6.0f / 4.0f));
		const float lamp[8] = { at.x + 3.0f * float(i % 4 - 2), at.y - 2.5f, at.z + 3.0f * float(i / 4 - 2), 6.0f, 1.469f * strength, strength, 0.219f * strength, 0.0f };
		lamps.insert(lamps.end(), lamp, lamp + 8);
	}
	l.lamps = lamps.data();
	l.lamp_count = uint32_t(lamp_count);
	l.lamp_shadows = 0.5f;
	std::vector<float> actors;
	{
		const vec3 right = { cam.view[0], cam.view[4], cam.view[8] }, ahead = normalize({ -cam.view[2], 0.0f, -cam.view[10] });
		for (int i = 0; i < actor_count; ++i)
		{
			const float side = 3.0f * float(i % 5 - 2), on = 3.0f * float((i - 1) / 5) - 1.0f;
			const float someone[4] = { at.x + right.x * side + ahead.x * on, at.y, at.z + right.z * side + ahead.z * on, 1.2f };
			const float the_box[4] = { q.x, at.y, q.z, 2.0f };
			actors.insert(actors.end(), i == 0 ? the_box : someone, (i == 0 ? the_box : someone) + 4);
		}
	}
	l.sun_characters = actor_count > 0;
	// (with the wear: someone fighting where the camera looks, after any others, and a cut there every 2 s)
	if (wear)
	{
		const float fighter[4] = { at.x, at.y, at.z, 1.2f };
		actors.insert(actors.end(), fighter, fighter + 4);
		l.fighting = 1u << (actors.size() / 4 - 1);
		l.wear = 1.0f;
	}
	l.actors = actors.data();
	l.actor_count = uint32_t(actors.size() / 4);
	float cut[8] = { at.x - 2.0f, at.y, at.z, 1.0f, at.x + 2.0f, at.y, at.z, 0.0f };
	if (rain > 0.0f || snow > 0.0f)
	{
		rain_file m;
		m.zone = 1;
		m.square = 0.5f;
		m.x0 = at.x - 60.0f;
		m.z0 = at.z - 60.0f;
		m.width = m.height = 240;
		m.y0 = at.y;
		m.q.assign(size_t(m.width) * m.height, 0);
		m.kinds.assign(m.q.size(), kRainGround);
		m.covered = m.width * m.height;
		if (!r.upload_rain(std::move(m), error))
		{
			std::printf("rain map: %s\n", error.c_str());
			return 1;
		}
		l.rain = rain;
		l.rain_slant[0] = snow > 0.0f ? 1.4f : 0.2f;
		l.snow = snow;
		l.snow_sway = 0.15f;
		l.time = 100.0f;
	}
	if (sky == 1 && dome_given != nullptr)
	{
		// West Ronfaure's sunshine at noon (grass_test 1x), and a dusk and a night made up for the bench: the record's 8
		// stops (bytes, heights), the sun (or the moon) where that hour has it, its colour
		static const float noon_colours[8][3] = { { 154, 185, 218 }, { 103, 149, 218 }, { 85, 120, 191 }, { 61, 72, 173 }, { 45, 51, 125 }, { 45, 49, 125 }, { 45, 49, 125 }, { 45, 49, 125 } };
		static const float dusk_colours[8][3] = { { 236, 152, 96 }, { 214, 136, 106 }, { 168, 118, 128 }, { 112, 100, 148 }, { 72, 76, 140 }, { 52, 58, 120 }, { 46, 50, 110 }, { 42, 46, 104 } };
		static const float night_colours[8][3] = { { 71, 66, 111 }, { 53, 41, 86 }, { 30, 29, 56 }, { 24, 28, 50 }, { 20, 22, 44 }, { 18, 19, 46 }, { 18, 19, 46 }, { 18, 19, 46 } };
		static const float day_heights[8] = { 0.0f, 0.086f, 0.184f, 0.307f, 0.456f, 0.623f, 0.796f, 1.0f };
		static const float night_heights[8] = { 0.0f, 0.07f, 0.206f, 0.342f, 0.504f, 0.671f, 0.833f, 1.0f };
		const bool dusk = std::strcmp(dome_given, "dusk") == 0, night = std::strcmp(dome_given, "night") == 0;
		const float (&colours)[8][3] = dusk ? dusk_colours : night ? night_colours : noon_colours;
		const float (&heights)[8] = night ? night_heights : day_heights;
		l.dome.on = true;
		l.dome.segments = night ? 16 : 10;
		for (int i = 0; i < 8; ++i)
		{
			for (int k = 0; k < 3; ++k)
				l.dome.stops[i][k] = colours[i][k];
			l.dome.stops[i][3] = heights[i];
		}
		for (int k = 0; k < 3; ++k)
			l.dome.clear[k] = colours[0][k];
		// (noon: 65 degrees up behind the camera's left; dusk: 4 degrees up a little right of where it looks; night: the
		// moon 40 degrees up to its right)
		const float elev = (sun_elev > -999.0f ? sun_elev : dusk ? 4.0f : night ? 40.0f : 65.0f) * 3.14159265f / 180.0f;
		const float bearing = (sun_elev > -999.0f ? sun_bearing : dusk ? 52.0f : night ? 95.0f : 200.0f) * 3.14159265f / 180.0f;
		l.sun[0] = std::sin(bearing) * std::cos(elev);
		l.sun[1] = -std::sin(elev);
		l.sun[2] = std::cos(bearing) * std::cos(elev);
		l.dome.sun = night ? 0.6f : 1.0f;
		l.dome.moon = night ? 1.0f : 0.0f;
		const float disc[3][3] = { { 1.0f, 0.95f, 0.85f }, { 1.0f, 0.62f, 0.38f }, { 0.8f, 0.85f, 1.0f } };
		for (int k = 0; k < 3; ++k)
			l.dome.disc[k] = disc[dusk ? 1 : night ? 2 : 0][k];
		l.dome.stars = night ? 1.0f : 0.0f;
		l.dome.star_turn = 1.0f;
		l.dome.moon_lit = 1.0f;
		// The game's dome's strips for this record (radius 0.8 x (65535 - 0.1), around the camera)
		const float radius = 0.8f * (65535.0f - 0.1f);
		for (int i = 0; i < 7; ++i)
		{
			const float h0 = l.dome.stops[i][3], h1 = l.dome.stops[i + 1][3];
			const uint32_t n = dome_strips(h0, h1);
			for (uint32_t m = 0; m < n; ++m)
			{
				const float f1 = float(m) / float(n), f2 = float(m + 1) / float(n);
				const float e1 = (f1 * h1 + (1.0f - f1) * h0) * 1.570796f, e2 = (f2 * h1 + (1.0f - f2) * h0) * 1.570796f;
				dome_vertex lo = {}, hi = {};
				for (int k = 0; k < 3; ++k)
				{
					lo.c[k] = uint8_t(std::floor(l.dome.stops[i][k] * (1.0f - f1)) + std::floor(l.dome.stops[i + 1][k] * f1));
					hi.c[k] = uint8_t(std::floor(l.dome.stops[i][k] * (1.0f - f2)) + std::floor(l.dome.stops[i + 1][k] * f2));
				}
				lo.c[3] = hi.c[3] = 128;
				const UINT first = UINT(dome_vertices.size());
				for (uint32_t k = 0; k <= l.dome.segments; ++k)
				{
					const float a = k == 0 || k == l.dome.segments ? 0.0f : float(k) * 6.283f / float(l.dome.segments);
					lo.x = radius * std::cos(e1) * std::sin(a), lo.y = -radius * std::sin(e1), lo.z = radius * std::cos(e1) * std::cos(a);
					hi.x = radius * std::cos(e2) * std::sin(a), hi.y = -radius * std::sin(e2), hi.z = radius * std::cos(e2) * std::cos(a);
					dome_vertices.push_back(lo);
					dome_vertices.push_back(hi);
				}
				dome_runs.push_back({ first, UINT(dome_vertices.size()) - first });
			}
		}
		D3D12_HEAP_PROPERTIES up = { D3D12_HEAP_TYPE_UPLOAD };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = dome_vertices.size() * sizeof(dome_vertex);
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		void *mapped = nullptr;
		if (SUCCEEDED(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&dome_vb))) &&
			SUCCEEDED(dome_vb->Map(0, nullptr, &mapped)))
		{
			std::memcpy(mapped, dome_vertices.data(), dome_vertices.size() * sizeof(dome_vertex));
			dome_vb->Unmap(0, nullptr);
		}
	}
	else if (sky == 1)
	{
		l.dome.on = true;
		l.dome.segments = 10;
		for (int i = 0; i < 8; ++i)
		{
			for (int k = 0; k < 3; ++k)
				l.dome.stops[i][k] = std::round(rt_clear.Color[k] * 255.0f);
			l.dome.stops[i][3] = i / 7.0f;
		}
		for (int k = 0; k < 3; ++k)
			l.dome.clear[k] = l.dome.stops[0][k];
		l.dome.sun = 1.0f;
		l.dome.stars = 1.0f;
		l.dome.star_turn = 1.0f;
		const vec3 ahead = normalize({ -cam.view[2], -cam.view[6], -cam.view[10] }); // (the sun where the camera looks)
		l.sun[0] = ahead.x;
		l.sun[1] = ahead.y;
		l.sun[2] = ahead.z;
	}
	// The weather of its own, as the core has it for that weather on a noon record (its sky the dome's above), the wind
	// blowing across the picture, the ground under the camera the flat ground's
	if (weather_name_given != nullptr)
	{
		weather w;
		w.known = true;
		std::memcpy(&w.name, weather_name_given, 4);
		w.record = true;
		w.sun_color[0] = w.sun_color[1] = w.sun_color[2] = 0.5f;
		w.intensity = 1.5f;
		w.record_fog_color[0] = 0.6f, w.record_fog_color[1] = 0.63f, w.record_fog_color[2] = 0.68f;
		if (dome_given != nullptr && std::strcmp(dome_given, "dusk") == 0) // (the low sun's orange, the fog warm)
		{
			w.sun_color[0] = 0.9f, w.sun_color[1] = 0.5f, w.sun_color[2] = 0.28f;
			w.record_fog_color[0] = 0.7f, w.record_fog_color[1] = 0.52f, w.record_fog_color[2] = 0.46f;
		}
		else if (dome_given != nullptr && std::strcmp(dome_given, "night") == 0) // (the moon's cool light, the fog dark)
		{
			w.sun_color[0] = 0.25f, w.sun_color[1] = 0.28f, w.sun_color[2] = 0.38f;
			w.record_fog_color[0] = 0.12f, w.record_fog_color[1] = 0.13f, w.record_fog_color[2] = 0.2f;
		}
		weather_look(w, l.dome, l.wx);
		// (the clouds lit as the core has it: the game's light this high up)
		l.wx.cloud_quality = cloud_quality;
		l.wx.cloud_style = cloud_style;
		l.wx.cloud_light = std::clamp((-l.sun[1] + 0.04f) / 0.12f, 0.0f, 1.0f);
		l.wx.wind[0] = cam.view[0];
		l.wx.wind[1] = cam.view[8];
		l.wx.ground_y = at.y;
		l.wx.aurora *= 0.15f; // (by day, as the core has it)
		if (flash)
		{
			l.wx.flash = 1.0f;
			l.wx.bolt = 2.5f;
			l.wx.bolt_bearing = std::atan2(-cam.view[2], -cam.view[10]);
			l.wx.bolt_top = 0.35f;
		}
	}
	auto map_sun = [&](int frame) { // (50 degrees up, from the south-west; turning with redraw)
		const float a = float(225.0 + double(turn) * frame) * 3.14159265f / 180.0f, e = 50.0f * 3.14159265f / 180.0f;
		l.sun[0] = std::sin(a) * std::cos(e);
		l.sun[1] = -std::sin(e);
		l.sun[2] = std::cos(a) * std::cos(e);
	};
	uint32_t redrawn = 0, drawn_triangles = 0, read_frames = 0, near_drawn = 0, far_drawn = 0, blades_drawn = 0, water_drawn = 0, sea_drawn = 0;

	auto barrier = [&](ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = res;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		list->ResourceBarrier(1, &b);
	};
	double worst_ms = 0.0;
	std::vector<double> samples;
	for (int frame = 0; frame < frames; ++frame)
	{
		if (sky >= 0 && spin != 0.0f)
			sky_view(frame);
		barrier(world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		const float dome_clear[4] = { l.dome.clear[0] / 255.0f, l.dome.clear[1] / 255.0f, l.dome.clear[2] / 255.0f, 0.5f };
		list->ClearRenderTargetView(rtv, dome_vb != nullptr ? dome_clear : rt_clear.Color, 0, nullptr);
		list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		float constants[36] = {};
		multiply(cam.view, cam.proj, constants);
		invert(constants, constants + 16);
		constants[32] = at.y;
		constants[33] = float(W);
		constants[34] = float(H);
		const D3D12_VIEWPORT gv = { 0.0f, 0.0f, float(W), float(H), 0.0f, 1.0f };
		const D3D12_RECT gs = { 0, 0, LONG(W), LONG(H) };
		list->SetGraphicsRootSignature(ground_root);
		list->SetPipelineState(ground_pso);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
		list->RSSetViewports(1, &gv);
		list->RSSetScissorRects(1, &gs);
		list->SetGraphicsRoot32BitConstants(0, 36, constants, 0);
		list->DrawInstanced(3, 1, 0, 0);
		const float dark[4] = { 0.2f, 0.2f, 0.2f, 0.5f };
		if (sky < 0 && horizon <= -999.0f) // (the box: under the sky's camera, or out to the horizon, it's behind it)
		{
			list->ClearRenderTargetView(rtv, dark, 1, &box);
			list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, qz / qw, 0, 1, &box);
		}
		if (dome_vb != nullptr) // (the game's dome, centred on the camera: the view's translation zeroed)
		{
			float view0[16], dvp[16];
			std::memcpy(view0, cam.view, sizeof(view0));
			view0[12] = view0[13] = view0[14] = 0.0f;
			multiply(view0, cam.proj, dvp);
			const D3D12_VERTEX_BUFFER_VIEW vbv = { dome_vb->GetGPUVirtualAddress(), UINT(dome_vertices.size() * sizeof(dome_vertex)), UINT(sizeof(dome_vertex)) };
			list->SetGraphicsRootSignature(dome_root);
			list->SetPipelineState(dome_pso);
			list->SetGraphicsRoot32BitConstants(0, 16, dvp, 0);
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			list->IASetVertexBuffers(0, 1, &vbv);
			list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			list->RSSetViewports(1, &gv);
			list->RSSetScissorRects(1, &gs);
			for (const auto &run_of : dome_runs)
				list->DrawInstanced(run_of.second, 1, run_of.first, 0);
		}
		barrier(world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		if (map_file != nullptr)
			map_sun(frame);
		if (fade) // (the look's clock, 60 frames a second: the map drawn about once a second, faded between drawings)
			l.clock = 100.0 + frame / 60.0;
		if (wear) // (60 frames a second)
		{
			l.clock = 100.0 + frame / 60.0;
			cut[7] = float(frame % 120) / 60.0f;
			l.cuts = cut;
			l.cut_count = cut[7] <= float(kCutKeep) ? 1 : 0;
		}
		const draw_stats st = r.draw(list, t, cam, l, error);
		blades_drawn = std::max(blades_drawn, st.blades);
		water_drawn = std::max(water_drawn, st.water_corners);
		sea_drawn = std::max(sea_drawn, st.sea_corners);
		redrawn += st.sun_map_cascades != 0 ? 1 : 0;
		near_drawn += st.sun_map_cascades & 1;
		far_drawn += (st.sun_map_cascades >> 1) & 1;
		drawn_triangles = std::max(drawn_triangles, st.sun_map_triangles);
		read_frames += st.sun_map ? 1 : 0;
		if (frame >= 8) // (a frame's own time, a few frames late: after the first, every frame's)
			samples.push_back(r.last_shading_ms());
		const auto start = std::chrono::steady_clock::now();
		if (!run())
		{
			std::printf("frame %d: the GPU didn't finish in 5 s, or the device was lost (0x%08lX): stopping\n", frame,
				static_cast<unsigned long>(device->GetDeviceRemovedReason()));
			return 1;
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		worst_ms = ms > worst_ms ? ms : worst_ms;
	}
	std::printf("%u x %u, %d frames%s, %d lamps, %d characters casting%s: copies + shading on the GPU %.2f ms (smoothed); slowest frame round trip %.1f ms\n", W, H, frames,
		continuous ? " (continuous)" : "", lamp_count, actor_count, sky == 1 ? ", its own sky" : sky == 0 ? ", the sky's camera without it" : "", r.shading_ms(), worst_ms);
	if (picture_file != nullptr) // (the last frame, read back once all of them are done)
	{
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
		UINT64 size = 0;
		const D3D12_RESOURCE_DESC wd = world->GetDesc();
		device->GetCopyableFootprints(&wd, 0, 1, 0, &fp, nullptr, nullptr, &size);
		D3D12_HEAP_PROPERTIES rb_heap = { D3D12_HEAP_TYPE_READBACK };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = size;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *rb = nullptr;
		if (SUCCEEDED(device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))))
		{
			barrier(world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
			dst.PlacedFootprint = fp;
			D3D12_TEXTURE_COPY_LOCATION src = { world, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			barrier(world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			uint8_t *p = nullptr;
			FILE *f = nullptr;
			if (run() && SUCCEEDED(rb->Map(0, nullptr, reinterpret_cast<void **>(&p))) && fopen_s(&f, picture_file, "wb") == 0 && f != nullptr)
			{
				std::fprintf(f, "P6 %u %u 255\n", W, H);
				for (UINT y = 0; y < H; ++y)
					for (UINT x = 0; x < W; ++x)
						std::fwrite(p + size_t(y) * fp.Footprint.RowPitch + size_t(x) * 4, 1, 3, f);
				fclose(f);
				rb->Unmap(0, nullptr);
				std::printf("picture: %s\n", picture_file);
			}
			release(rb);
		}
	}
	if (water_file != nullptr || sea_corners != 0)
		std::printf("water: %u of its corners drawn a frame, %u of the sea's%s (in the copies + shading above)\n", water_drawn, sea_drawn,
			horizon > -999.0f ? ", looking out along it: the water's alone" : "");
	if (weather_name_given != nullptr)
		std::printf("weather %s%s: what follows the shading (the air pass, the flash's) %.2f ms (smoothed)\n", weather_name_given, flash ? " at a flash" : "", r.grass_ms());
	if (rain > 0.0f || snow > 0.0f)
		std::printf("rain %.2f, snow %.2f: its drops, splashes and flakes on the GPU %.2f ms (smoothed)\n", rain, snow, r.grass_ms());
	else if (grass_file != nullptr)
		std::printf("grass%s: %u blades a frame, on the GPU %.2f ms (smoothed)\n", wear ? " with its wear" : "", blades_drawn, r.grass_ms());
	// (frame by frame: the median, and the tenth percentile, which another program sharing the GPU (the game)
	// sways least)
	if (samples.size() >= 10)
	{
		std::sort(samples.begin(), samples.end());
		std::printf("frame by frame: median %.2f ms, tenth percentile %.2f ms (%zu frames)\n", samples[samples.size() / 2], samples[samples.size() / 10], samples.size());
	}
	if (l.wx.cloud_cover > 0.0f && l.dome.on)
	{
		std::printf("sections (GPU ms, smoothed):");
		for (const renderer::gpu_section &s : r.gpu_sections())
			std::printf(" %ls %.3f;", s.name.c_str(), s.ms);
		std::printf("\n");
	}
	if (map_file != nullptr)
		std::printf("shadow map read in %u frames, drawn again in %u (near %u, far %u; at most %u of %u triangles a time)%s%s\n", read_frames, redrawn, near_drawn, far_drawn, drawn_triangles, map_triangles,
			error.empty() ? "" : ": ", error.c_str());
	return 0;
}
