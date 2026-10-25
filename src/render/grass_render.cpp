/*
 * Grass for FFXI's world on Direct3D 12 (prototype): the renderer. See grass_render.hpp.
 */

#include "grass_render.hpp"
#include "gpu_crash.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <chrono>
#include <cwchar>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace grass
{
	namespace
	{
		// Root parameters: 52 constants (b0), the blade buffer (t0), the copy of the world (t1), the actors
		// near (t2; the water pass's parameters there) and the copy of the world's depth (t3), root SRVs, and
		// the ground colours remembered (u0) and the sun's shadows (u1), root UAVs: no descriptor heap
		// needed, so the game's heaps stay bound
		constexpr UINT kConstants = 52;
		constexpr UINT kActorSlots = 4; // upload buffers for the actors, used in turn (frames in flight)
		// A frame's water parameters: 8 float4s, then two for each ring (kMaxRipples), rounded up to 256 bytes
		constexpr UINT kWaterParamBytes = (8 * 16 + kMaxRipples * 32 + 255) / 256 * 256;
		constexpr UINT kFirstBladeConstant = 27;
		// The sun's shadow map: two cascades of kSunMapSize texels square, side by side, reaching this many yalms
		// each way from where they're centred (near: 3.9 cm a texel, far: 15.6 cm)
		constexpr UINT kSunMapSize = 2048;
		constexpr float kSunMapReach[2] = { 40.0f, 160.0f };
		// ... drawn again, with the look's clock, at most this often a cascade (seconds), each drawing fading in over
		// the one before for as long (draw_sun_map)
		constexpr double kSunMapHold = 1.0;
		// A frame's actors and, after them, the shadow map's parameters for the sun pass (8 float4s), then the
		// lamps for the shading pass: a float4 (how many, how much of a lamp's glow its shadow takes), and two for
		// each lamp (always all kMaxLamps of them: the shader reads them all)
		constexpr UINT kSunMapParams = 32;
		constexpr UINT kLampParams = 4 + kMaxLamps * 8;
		// ... and after the lamps the game's fog, for the grass, the fireflies and the shading pass: its colour, then
		// where it starts and ends and 1 / (end - start)
		constexpr UINT kHazeParams = 8;
		// ... and after the fog the wet look's, for the shading pass: the view's first and third rows (a view-space
		// point's world x and z) with how wet, where the wet mask lies and its size, the light's and the sheen's colours
		constexpr UINT kWetParams = 24;
		// ... and after those the sky's, for the shading pass (grass.hlsl kSky): the record's 8 stops (colour in bytes and
		// height), its clear colour and the dome's segments, each band's strips, the view's first and third rows with
		// the stars' and the sun's strength, toward the light (view space) and whether it's the moon, the disc's colour
		// and the stars' turn, the time and how near the game's dome a pixel must be to be repainted, the stops' slopes
		// (with the moon's lit side and phase in the first four's spare places)
		constexpr UINT kSkyParams = 96;
		// ... and after the sky the grass's wear (grass.hlsl kWear): the memory's clock now, the frame's length, how much
		// the wear shows and who's fighting (bits); how many cuts; then each cut's swath (8 floats: from, half-width, to,
		// when on the memory's clock)
		constexpr UINT kWearParams = 8 + kMaxCuts * 8;
		// ... and after the wear's the shadow map's drawings before the last (grass.hlsl kSunMapPrev; sun_map_params)
		constexpr UINT kSunMapPrevParams = 32;
		// ... and after those the weather of its own's (grass.hlsl kWx, look::weather_fx; rule bit 512): the wind, the view's
		// first and third rows with the ground's height and the time, the clouds and their colours, the lightning, the
		// dust, the gloom and the heat
		constexpr UINT kWxParams = 64;
		constexpr UINT kWxOffset = kMaxActors * 4 + kSunMapParams + kLampParams + kHazeParams + kWetParams + kSkyParams + kWearParams + kSunMapPrevParams;
		constexpr UINT kActorBytes = (kWxOffset + kWxParams) * 4;
		// Each point's memory (grass.hlsl kMemoryStride): its ground colour (12 bytes), then when its trampling wears off
		// and when it has grown back from a cut (f32 seconds on the memory's clock, 0 never)
		constexpr UINT kMemoryStride = 20;
		constexpr UINT kSegments = 4;                              // per blade, the last one a triangle
		constexpr UINT kVerticesPerBlade = (kSegments - 1) * 6 + 3;
		// A firefly's home every this many grass points at the usual amount (3 points a square yalm: one in 20)
		constexpr float kFireflyStride = 60.0f;
		// Rain: the rain map's window around the camera (squares a side: 128 yalms at 0.5), moved in steps of
		// kRainWindowStep squares, with the camera kept at least kRainWindowMargin squares from its edges. Drops fall
		// in columns kRainSpacing yalms apart (two drops a column, kRainLayers), drawn within kRainRadius yalms of the
		// camera (across the ground), splashes within kSplashRadius, as fast as kRainSpeed yalms a second (each drop
		// 0.85 to 1.15 times that). The shaders know the window's size and the layers (grass.hlsl, kRainWindow).
		constexpr int32_t kRainWindow = 256, kRainWindowStep = 32, kRainWindowMargin = 96;
		constexpr float kRainSpacing = 0.5f, kRainRadius = 24.0f, kSplashRadius = 14.0f, kRainSpeed = 14.0f;
		constexpr UINT kRainLayers = 2;
		// Snow: flakes in the same columns (eight a column, kSnowLayers), drawn within kSnowRadius yalms, falling about
		// kSnowSpeed yalms a second (each flake 0.7 to 1.4 times that)
		constexpr float kSnowRadius = 20.0f, kSnowSpeed = 2.0f;
		constexpr UINT kSnowLayers = 8;
		// Leaves in wind and gales: in the same columns (two a column, kLeafLayers), drawn within kLeafRadius yalms, falling
		// about kLeafSpeed yalms a second and blown far across as they go (grass.hlsl, "Leaves"); at most kLeafShare of the
		// columns hold one
		constexpr float kLeafRadius = 20.0f, kLeafSpeed = 1.4f, kLeafShare = 0.2f;
		constexpr UINT kLeafLayers = 2;
		// The blades' own depth buffer, also the water pass's (the nearest water kept): with a stencil, where the baked water
		// marks the pixels it draws whole, so the sea isn't drawn there (D24: the blades are within 40 yalms, the depth only
		// decides which of the water's own surfaces is in front; the world's own depth is a copy of the game's D24 anyway)
		constexpr DXGI_FORMAT kBladeDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

		// The shaders: src/render/grass.hlsl, built in (tools/embed.py); worldfx.hlsl next to the add-on replaces them
		// while the game runs (compiled on a thread of its own, then set_shader_code)
		const char kShader[] =
#include "grass_hlsl.inc"
			;

		template <typename T>
		void release(T *&p)
		{
			if (p != nullptr)
				p->Release();
			p = nullptr;
		}

		// The renderer's shaders, in shader_code's order (set_shader_code puts each where it goes)
		struct shader_entry
		{
			const char *entry, *target;
		};
		constexpr shader_entry kShaderEntries[] = {
			{ "vs_main", "vs_5_0" }, { "ps_main", "ps_5_0" }, { "shade_vs", "vs_5_0" }, { "shade_ps", "ps_5_0" }, { "sun_ps", "ps_5_0" },
			{ "shadow_vs", "vs_5_0" }, { "shadow_ps", "ps_5_0" }, { "water_vs", "vs_5_0" }, { "water_ps", "ps_5_0" },
			{ "water_debug_vs", "vs_5_0" }, { "water_debug_ps", "ps_5_0" }, { "firefly_vs", "vs_5_0" }, { "firefly_ps", "ps_5_0" },
			{ "rain_vs", "vs_5_0" }, { "rain_ps", "ps_5_0" }, { "splash_vs", "vs_5_0" }, { "splash_ps", "ps_5_0" },
			{ "snow_vs", "vs_5_0" }, { "snow_ps", "ps_5_0" }, { "vis_ps", "ps_5_0" }, { "leaf_vs", "vs_5_0" }, { "leaf_ps", "ps_5_0" },
			{ "air_ps", "ps_5_0" }, { "flash_ps", "ps_5_0" } };
		constexpr size_t kShaderEntryCount = sizeof(kShaderEntries) / sizeof(kShaderEntries[0]);
		constexpr UINT kCompileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

		// D3DCompile, and which build of d3dcompiler_47.dll it is (its link time and size in memory: 0 when it isn't
		// there), looked up once
		struct compiler_dll
		{
			pD3DCompile compile = nullptr;
			uint64_t identity = 0;
		};
		const compiler_dll &compiler()
		{
			static const compiler_dll dll = [] {
				compiler_dll d;
				const HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
				if (module == nullptr)
					return d;
				d.compile = reinterpret_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile"));
				const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
				const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const uint8_t *>(module) + dos->e_lfanew);
				if (d.compile != nullptr)
					d.identity = (uint64_t(nt->FileHeader.TimeDateStamp) << 32) | nt->OptionalHeader.SizeOfImage;
				return d;
			}();
			return dll;
		}

		// FNV-1a, 64 bits
		uint64_t fnv(const void *data, size_t size, uint64_t h = 14695981039346656037ull)
		{
			const uint8_t *p = static_cast<const uint8_t *>(data);
			for (size_t i = 0; i < size; ++i)
				h = (h ^ p[i]) * 1099511628211ull;
			return h;
		}

		// The shader cache: a file a source, named for its key (16 hex digits, .bin): 'WFXS', the version, the key, how
		// many blobs; each blob's size and bytes; then the FNV-1a of all that. Written to a file of its own first and
		// renamed into place, so a reader never finds half of one.
		constexpr uint32_t kShaderCacheVersion = 1;
		constexpr uint32_t kShaderCacheMagic = 'W' | ('F' << 8) | ('X' << 16) | ('S' << 24);
		constexpr size_t kShaderCacheMax = size_t(64) << 20;

		std::wstring shader_cache_file(const std::wstring &folder, uint64_t key, const wchar_t *suffix = L".bin")
		{
			wchar_t name[48];
			std::swprintf(name, 48, L"\\%016llx%s", static_cast<unsigned long long>(key), suffix);
			return folder + name;
		}

		bool read_shader_cache(const std::wstring &path, uint64_t key, shader_code &out)
		{
			std::vector<uint8_t> data;
			if (FILE *f = nullptr; _wfopen_s(&f, path.c_str(), L"rb") == 0 && f != nullptr)
			{
				uint8_t buffer[65536];
				size_t n;
				while ((n = fread(buffer, 1, sizeof(buffer), f)) != 0 && data.size() <= kShaderCacheMax)
					data.insert(data.end(), buffer, buffer + n);
				fclose(f);
			}
			if (data.size() < 20 + 8 || data.size() > kShaderCacheMax)
				return false;
			const size_t body = data.size() - 8;
			uint64_t sum = 0;
			std::memcpy(&sum, &data[body], 8);
			if (sum != fnv(data.data(), body))
				return false;
			uint32_t magic = 0, version = 0, count = 0;
			uint64_t file_key = 0;
			std::memcpy(&magic, &data[0], 4);
			std::memcpy(&version, &data[4], 4);
			std::memcpy(&file_key, &data[8], 8);
			std::memcpy(&count, &data[16], 4);
			if (magic != kShaderCacheMagic || version != kShaderCacheVersion || file_key != key || count != kShaderEntryCount)
				return false;
			shader_code code;
			code.key = key;
			code.blobs.resize(count);
			size_t at = 20;
			for (uint32_t i = 0; i < count; ++i)
			{
				uint32_t size = 0;
				if (at + 4 > body)
					return false;
				std::memcpy(&size, &data[at], 4);
				at += 4;
				if (size == 0 || size > body - at)
					return false;
				code.blobs[i].assign(data.begin() + at, data.begin() + at + size);
				at += size;
			}
			if (at != body)
				return false;
			out = std::move(code);
			return true;
		}

		// (made the newest, as one just used: the oldest are the ones deleted)
		void touch(const std::wstring &path)
		{
			const HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (h == INVALID_HANDLE_VALUE)
				return;
			FILETIME now;
			GetSystemTimeAsFileTime(&now);
			SetFileTime(h, nullptr, nullptr, &now);
			CloseHandle(h);
		}

		// Keeps the kShaderCacheKeep files written or used last, deletes the rest
		void prune_shader_cache(const std::wstring &folder)
		{
			struct found
			{
				std::wstring name;
				uint64_t when;
			};
			std::vector<found> files;
			WIN32_FIND_DATAW fd;
			const HANDLE h = FindFirstFileW((folder + L"\\*.bin").c_str(), &fd);
			if (h == INVALID_HANDLE_VALUE)
				return;
			do
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
					files.push_back({ fd.cFileName, (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime });
			while (FindNextFileW(h, &fd));
			FindClose(h);
			std::sort(files.begin(), files.end(), [](const found &a, const found &b) { return a.when > b.when; });
			for (size_t i = kShaderCacheKeep; i < files.size(); ++i)
				DeleteFileW((folder + L"\\" + files[i].name).c_str());
		}

		void write_shader_cache(const std::wstring &folder, const shader_code &code)
		{
			std::vector<uint8_t> data(20);
			const uint32_t count = uint32_t(code.blobs.size());
			std::memcpy(&data[0], &kShaderCacheMagic, 4);
			std::memcpy(&data[4], &kShaderCacheVersion, 4);
			std::memcpy(&data[8], &code.key, 8);
			std::memcpy(&data[16], &count, 4);
			for (const std::vector<uint8_t> &blob : code.blobs)
			{
				const uint32_t size = uint32_t(blob.size());
				data.insert(data.end(), reinterpret_cast<const uint8_t *>(&size), reinterpret_cast<const uint8_t *>(&size) + 4);
				data.insert(data.end(), blob.begin(), blob.end());
			}
			const uint64_t sum = fnv(data.data(), data.size());
			data.insert(data.end(), reinterpret_cast<const uint8_t *>(&sum), reinterpret_cast<const uint8_t *>(&sum) + 8);
			CreateDirectoryW(folder.c_str(), nullptr);
			wchar_t suffix[32];
			std::swprintf(suffix, 32, L".%lu.part", static_cast<unsigned long>(GetCurrentThreadId()));
			const std::wstring part = shader_cache_file(folder, code.key, suffix), path = shader_cache_file(folder, code.key);
			bool written = false;
			if (FILE *f = nullptr; _wfopen_s(&f, part.c_str(), L"wb") == 0 && f != nullptr)
			{
				written = fwrite(data.data(), 1, data.size(), f) == data.size();
				written = fclose(f) == 0 && written;
			}
			if (!written || !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
			{
				DeleteFileW(part.c_str());
				return;
			}
			prune_shader_cache(folder);
		}

		bool compile(const std::string &source, const char *entry, const char *target, std::vector<uint8_t> &out, std::string &error)
		{
			const pD3DCompile compile_fn = compiler().compile;
			if (compile_fn == nullptr)
			{
				error = "d3dcompiler_47.dll not found";
				return false;
			}
			ID3DBlob *code = nullptr, *messages = nullptr;
			const HRESULT hr = compile_fn(source.data(), source.size(), "grass", nullptr, nullptr, entry, target, kCompileFlags, 0, &code, &messages);
			if (FAILED(hr))
			{
				error = "shader ";
				error += entry;
				error += ": ";
				error += messages != nullptr ? static_cast<const char *>(messages->GetBufferPointer()) : "compile failed";
				release(messages);
				release(code);
				return false;
			}
			const uint8_t *bytes = static_cast<const uint8_t *>(code->GetBufferPointer());
			out.assign(bytes, bytes + code->GetBufferSize());
			release(messages);
			release(code);
			return true;
		}

		struct cell_run
		{
			uint32_t first, count, tuft;
		};

		bool same_samples(const DXGI_SAMPLE_DESC &a, const DXGI_SAMPLE_DESC &b)
		{
			return a.Count == b.Count && a.Quality == b.Quality;
		}

		void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to,
			UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
		{
			if (res == nullptr || from == to)
				return;
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = res;
			b.Transition.Subresource = subresource;
			b.Transition.StateBefore = from;
			b.Transition.StateAfter = to;
			list->ResourceBarrier(1, &b);
		}
	}

	void multiply(const float a[16], const float b[16], float out[16])
	{
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 4; ++c)
			{
				float s = 0.0f;
				for (int k = 0; k < 4; ++k)
					s += a[r * 4 + k] * b[k * 4 + c];
				out[r * 4 + c] = s;
			}
	}

	// The view matrix takes v to v R + t, so the camera is at -t R^T
	void eye_from_view(const float view[16], float eye[3])
	{
		for (int i = 0; i < 3; ++i)
			eye[i] = -(view[12] * view[i * 4 + 0] + view[13] * view[i * 4 + 1] + view[14] * view[i * 4 + 2]);
	}

	namespace
	{
		float word_float(uint32_t w)
		{
			float f;
			std::memcpy(&f, &w, 4);
			return f;
		}

		// A colour as the record keeps it (R G B A bytes; the game lights by them over 255)
		void record_color(uint32_t w, float out[3])
		{
			for (int i = 0; i < 3; ++i)
				out[i] = float((w >> (8 * i)) & 0xFF) / 255.0f;
		}

		// Fog that makes sense: finite, ending past where it starts, within the far plane (the game's: 65535)
		bool fog_ok(float start, float end)
		{
			return std::isfinite(start) && std::isfinite(end) && start > -1000.0f && end > start && end > 0.0f && end < 1e6f;
		}

		// The weathers by id: the client's two sets of names (0x377BB8, 0x377C08) and whether it rains. (How much of the sun's
		// shadows a weather leaves isn't by name: weather_sunlight, from the game's own light.)
		struct weather_kind
		{
			char name[5], second[5];
			float rain, downpour, slant, snowfall, sway;
		};
		// (downpour and slant: the add-on's rain, squalls heavier and blown slanting, thunder the heaviest; in the game's own
		// folders thunder holds no rain at all, only cloud and lightning: client-notes "Hiding one weather's visuals". Snowfall
		// and sway: the add-on's snow, blizzards the heaviest and blown hard across, their flakes swaying less)
		const weather_kind kWeathers[] = {
			{ "fine", "fin1", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }, { "suny", "sun1", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
			{ "clod", "clo1", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }, { "mist", "mis1", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
			{ "dryw", "dry1", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }, { "heat", "hea1", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
			{ "rain", "rai1", 1.0f, 0.55f, 0.1f, 0.0f, 0.0f }, { "squl", "squ1", 1.0f, 0.8f, 0.45f, 0.0f, 0.0f },
			{ "dust", "dus1", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }, { "sand", "san1", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
			{ "wind", "win1", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }, { "stom", "sto1", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
			{ "snow", "sno1", 0.0f, 0.0f, 0.12f, 0.6f, 0.35f }, { "bliz", "bli1", 0.0f, 0.0f, 1.4f, 1.0f, 0.15f },
			{ "thdr", "thd1", 1.0f, 1.0f, 0.25f, 0.0f, 0.0f }, { "bolt", "bol1", 1.0f, 1.0f, 0.3f, 0.0f, 0.0f },
			{ "aura", "aur1", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }, { "ligt", "lig1", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
			{ "fogd", "fog1", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }, { "dark", "dar1", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
		};

		const weather_kind *weather_by_name(uint32_t name)
		{
			for (const weather_kind &k : kWeathers)
				if (weather_name(k.name) == name || weather_name(k.second) == name)
					return &k;
			return nullptr;
		}

		// What each weather brings of the add-on's own beyond rain and snow (weather_kind_fx, grass_render.hpp), by id as
		// kWeathers: its clouds (cover, thick, dark, veil, speed), its wind (wind, windy, streaks, leaves), dust and sand,
		// lightning, heat, aurora, gloom, a darker sky and the stars. Fine weather a few clouds, sunshine scattered ones,
		// clouds overcast, mist a low-contrast haze; rain, squalls and thunder dark overcast, snow and blizzards grey; dust
		// and sand storms a brown haze; wind and gales clouds racing; gloom and darkness heavy and dark.
		const weather_kind_fx kWeatherFx[] = {
			//  cover  thick  dark  veil  speed   wind windy streaks leaves  dust  sand  flash heat  aurora gloom  dark  stars
			{ 0.22f, 0.85f, 0.05f, 0.0f, 0.7f, 0.15f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f },   // fine
			{ 0.4f, 0.9f, 0.08f, 0.0f, 0.8f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f },     // sunshine
			{ 0.8f, 0.95f, 0.3f, 0.08f, 0.9f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.05f, 1.0f },   // clouds
			{ 0.55f, 0.55f, 0.15f, 0.7f, 0.4f, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f },   // mist
			{ 0.15f, 0.75f, 0.05f, 0.08f, 0.6f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f },  // hot spell
			{ 0.08f, 0.6f, 0.05f, 0.12f, 0.5f, 0.15f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f },  // heat wave
			{ 0.95f, 1.0f, 0.6f, 0.2f, 1.1f, 0.4f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.15f, 1.0f },    // rain
			{ 1.0f, 1.0f, 0.65f, 0.25f, 1.8f, 0.9f, 0.0f, 0.0f, 0.25f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.2f, 1.0f },   // squall
			{ 0.55f, 0.8f, 0.25f, 0.3f, 1.6f, 1.0f, 0.0f, 1.0f, 0.0f, 0.75f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.05f, 1.0f },  // dust storm
			{ 0.6f, 0.85f, 0.3f, 0.4f, 2.0f, 1.3f, 0.0f, 1.2f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.05f, 1.0f },    // sand storm
			{ 0.5f, 0.9f, 0.15f, 0.05f, 2.4f, 1.0f, 1.0f, 0.35f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f },   // wind
			{ 0.72f, 0.95f, 0.3f, 0.1f, 3.6f, 2.0f, 1.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.05f, 1.0f },   // gales
			{ 0.92f, 1.0f, 0.35f, 0.25f, 0.6f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.05f, 1.0f },  // snow
			{ 1.0f, 1.0f, 0.45f, 0.4f, 2.4f, 1.4f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.1f, 1.0f },     // blizzard
			{ 1.0f, 1.0f, 0.72f, 0.15f, 1.3f, 0.6f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.7f, 0.0f, 0.0f, 0.0f, 0.2f, 1.0f },    // thunder
			{ 1.0f, 1.0f, 0.8f, 0.2f, 1.6f, 0.9f, 0.0f, 0.0f, 0.15f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.25f, 1.0f },    // thunderstorm
			{ 0.1f, 0.7f, 0.1f, 0.0f, 0.5f, 0.15f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.3f },     // aurora
			{ 0.04f, 0.6f, 0.05f, 0.0f, 0.5f, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.2f },    // stellar glare
			{ 0.9f, 1.0f, 0.75f, 0.35f, 0.5f, 0.15f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.75f, 0.35f, 1.0f }, // gloom
			{ 1.0f, 1.0f, 0.9f, 0.45f, 0.6f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.55f, 1.0f },    // darkness
		};
		static_assert(sizeof(kWeatherFx) / sizeof(kWeatherFx[0]) == sizeof(kWeathers) / sizeof(kWeathers[0]), "a look for each weather");

		float luma(const float c[3]) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }

		// The way the game's own clouds drift in each zone and weather (research/ffxi-re/data/zone_wind.txt, from
		// tools/zone_wind.py: the bearing they go toward, degrees from north toward east; client-notes "Wind"): the zone,
		// the weather's folder, the bearing
		struct zone_wind
		{
			uint16_t zone;
			char name[5];
			float bearing;
		};
		const zone_wind kZoneWinds[] = {
			{ 1, "clod", 280.8f }, { 1, "mist", 290.0f }, { 1, "rain", 236.4f }, { 1, "suny", 192.8f }, { 1, "thdr", 290.0f },
			{ 2, "clod", 281.1f }, { 2, "mist", 290.0f }, { 2, "rain", 236.4f }, { 2, "suny", 276.1f }, { 2, "thdr", 284.7f },
			{ 3, "clod", 280.8f }, { 3, "mist", 290.0f }, { 3, "suny", 192.8f }, { 4, "clod", 270.0f }, { 4, "fine", 180.0f },
			{ 4, "mist", 281.1f }, { 4, "suny", 237.4f }, { 5, "clod", 282.0f }, { 5, "mist", 280.8f }, { 5, "snow", 290.0f },
			{ 5, "suny", 276.2f }, { 6, "fine", 218.6f }, { 6, "suny", 258.7f }, { 7, "clod", 280.8f }, { 7, "dust", 197.3f },
			{ 7, "mist", 280.8f }, { 7, "suny", 276.1f }, { 7, "wind", 276.9f }, { 8, "clod", 280.8f }, { 8, "dust", 197.3f },
			{ 8, "mist", 280.8f }, { 8, "suny", 276.1f }, { 8, "wind", 276.9f }, { 24, "clod", 282.6f },
			{ 24, "fogd", 270.0f }, { 24, "mist", 281.1f }, { 24, "suny", 283.5f }, { 24, "thdr", 283.6f },
			{ 24, "wind", 280.0f }, { 25, "clod", 291.4f }, { 25, "fogd", 270.0f }, { 25, "mist", 281.1f },
			{ 25, "suny", 288.7f }, { 25, "thdr", 286.2f }, { 25, "wind", 281.2f }, { 26, "clod", 288.5f },
			{ 26, "fogd", 287.4f }, { 26, "mist", 287.4f }, { 26, "suny", 279.0f }, { 26, "thdr", 283.8f },
			{ 26, "wind", 279.0f }, { 29, "clod", 215.4f }, { 29, "stom", 88.1f }, { 29, "thdr", 215.4f },
			{ 30, "clod", 215.4f }, { 30, "stom", 88.1f }, { 30, "thdr", 215.4f }, { 31, "clod", 215.4f },
			{ 31, "stom", 88.1f }, { 31, "thdr", 215.4f }, { 33, "clod", 15.4f }, { 33, "fine", 15.4f },
			{ 33, "mist", 15.4f }, { 33, "suny", 15.4f }, { 39, "dark", 281.4f }, { 40, "dark", 281.4f },
			{ 41, "dark", 281.4f }, { 42, "dark", 281.4f }, { 43, "suny", 273.6f }, { 44, "suny", 273.6f },
			{ 46, "clod", 270.0f }, { 46, "thdr", 191.6f }, { 47, "clod", 270.0f }, { 47, "thdr", 191.6f },
			{ 49, "clod", 281.1f }, { 49, "mist", 281.1f }, { 49, "suny", 276.1f }, { 51, "clod", 270.0f },
			{ 52, "clod", 270.0f }, { 58, "clod", 270.0f }, { 58, "mis1", 281.1f }, { 58, "suny", 310.0f },
			{ 58, "thdr", 281.9f }, { 58, "wind", 270.0f }, { 59, "clod", 270.0f }, { 59, "mis1", 281.1f },
			{ 59, "suny", 310.0f }, { 59, "thdr", 281.9f }, { 59, "wind", 270.0f }, { 61, "mist", 348.7f },
			{ 65, "clod", 270.0f }, { 66, "clod", 270.0f }, { 67, "clod", 270.0f }, { 70, "clod", 281.4f },
			{ 70, "mist", 281.1f }, { 70, "rain", 260.8f }, { 70, "squl", 236.4f }, { 70, "stom", 197.1f },
			{ 70, "suny", 276.1f }, { 73, "clod", 270.0f }, { 73, "suny", 254.2f }, { 74, "clod", 268.9f },
			{ 74, "suny", 254.2f }, { 74, "thdr", 281.0f }, { 75, "clod", 270.0f }, { 75, "suny", 94.8f },
			{ 76, "clod", 270.0f }, { 76, "suny", 254.2f }, { 76, "thdr", 281.9f }, { 77, "aura", 203.8f },
			{ 77, "suny", 203.8f }, { 81, "clod", 290.0f }, { 82, "rain", 262.4f }, { 83, "bolt", 283.8f },
			{ 83, "clod", 282.3f }, { 83, "wind", 290.0f }, { 84, "clod", 278.8f }, { 84, "suny", 250.0f },
			{ 85, "clod", 278.8f }, { 85, "rain", 276.3f }, { 85, "thdr", 285.3f }, { 87, "clod", 278.8f },
			{ 89, "clod", 278.8f }, { 89, "thdr", 285.3f }, { 90, "clod", 278.8f }, { 90, "rain", 285.8f },
			{ 90, "thdr", 285.3f }, { 91, "clod", 281.1f }, { 91, "mist", 281.1f }, { 91, "rain", 285.8f },
			{ 92, "clod", 281.6f }, { 92, "rain", 262.4f }, { 92, "thdr", 282.7f }, { 94, "clod", 278.8f },
			{ 95, "clod", 283.6f }, { 95, "mist", 280.0f }, { 97, "suny", 250.0f }, { 98, "clod", 278.8f },
			{ 98, "suny", 250.0f }, { 98, "thdr", 284.3f }, { 99, "suny", 250.0f }, { 100, "clod", 281.1f },
			{ 100, "mist", 281.1f }, { 100, "suny", 276.1f }, { 101, "clod", 281.1f }, { 101, "mist", 281.1f },
			{ 101, "suny", 276.1f }, { 102, "clod", 281.4f }, { 102, "mist", 281.1f }, { 102, "rain", 260.8f },
			{ 102, "suny", 276.1f }, { 102, "wind", 276.9f }, { 103, "clod", 281.1f }, { 103, "dust", 197.3f },
			{ 103, "mist", 281.1f }, { 103, "suny", 276.1f }, { 104, "clod", 281.1f }, { 104, "mist", 280.0f },
			{ 104, "rain", 262.4f }, { 104, "suny", 276.1f }, { 104, "thdr", 280.7f }, { 105, "clod", 281.1f },
			{ 105, "dust", 197.3f }, { 105, "mist", 281.1f }, { 105, "snow", 281.1f }, { 105, "suny", 276.1f },
			{ 106, "clod", 281.1f }, { 106, "mist", 281.1f }, { 106, "suny", 276.1f }, { 107, "clod", 281.1f },
			{ 107, "mist", 281.1f }, { 107, "suny", 276.1f }, { 108, "clod", 281.3f }, { 108, "dust", 197.3f },
			{ 108, "mist", 281.1f }, { 108, "suny", 276.1f }, { 108, "thdr", 284.0f }, { 109, "clod", 281.6f },
			{ 109, "mist", 281.1f }, { 109, "rain", 261.0f }, { 109, "suny", 276.1f }, { 109, "thdr", 283.6f },
			{ 110, "clod", 281.1f }, { 110, "mist", 281.1f }, { 110, "rain", 260.8f }, { 110, "suny", 276.1f },
			{ 111, "clod", 281.4f }, { 111, "fogd", 270.0f }, { 111, "mist", 280.6f }, { 111, "snow", 280.0f },
			{ 112, "clod", 281.4f }, { 112, "dark", 281.4f }, { 112, "fogd", 270.0f }, { 112, "mist", 280.8f },
			{ 112, "snow", 290.0f }, { 113, "clod", 281.1f }, { 113, "mist", 281.1f }, { 113, "stom", 197.1f },
			{ 113, "suny", 276.1f }, { 113, "wind", 197.3f }, { 114, "dust", 197.3f }, { 114, "sand", 196.4f },
			{ 114, "suny", 276.1f }, { 115, "clod", 281.3f }, { 115, "mist", 280.0f }, { 115, "suny", 276.6f },
			{ 116, "clod", 281.3f }, { 116, "mist", 280.0f }, { 116, "suny", 276.6f }, { 117, "clod", 280.8f },
			{ 117, "dust", 197.3f }, { 117, "mist", 280.8f }, { 117, "suny", 276.1f }, { 117, "wind", 276.9f },
			{ 118, "clod", 281.1f }, { 118, "mist", 281.1f }, { 118, "rain", 262.4f }, { 118, "squl", 262.4f },
			{ 118, "suny", 276.1f }, { 118, "wind", 276.9f }, { 119, "clod", 280.8f }, { 119, "dust", 197.3f },
			{ 119, "suny", 276.1f }, { 120, "clod", 281.1f }, { 120, "dust", 197.3f }, { 120, "suny", 276.1f },
			{ 120, "thdr", 284.0f }, { 122, "clod", 281.6f }, { 122, "mist", 281.5f }, { 122, "suny", 276.2f },
			{ 122, "thdr", 284.7f }, { 123, "clod", 281.1f }, { 123, "mist", 280.0f }, { 123, "rain", 236.4f },
			{ 123, "squl", 236.4f }, { 123, "suny", 276.1f }, { 124, "clod", 281.1f }, { 124, "mist", 280.0f },
			{ 124, "rain", 236.4f }, { 124, "squl", 236.4f }, { 124, "suny", 276.1f }, { 125, "dust", 197.3f },
			{ 125, "sand", 196.4f }, { 125, "suny", 276.1f }, { 126, "clod", 281.5f }, { 126, "ligt", 277.5f },
			{ 126, "mist", 281.5f }, { 126, "suny", 276.2f }, { 126, "thdr", 283.6f }, { 127, "clod", 281.5f },
			{ 127, "ligt", 277.5f }, { 127, "mist", 281.5f }, { 127, "suny", 276.1f }, { 127, "thdr", 283.6f },
			{ 128, "clod", 281.1f }, { 128, "mist", 281.1f }, { 128, "stom", 197.1f }, { 128, "suny", 276.1f },
			{ 128, "wind", 197.3f }, { 130, "aura", 270.0f }, { 130, "fine", 270.0f }, { 130, "suny", 270.0f },
			{ 130, "wind", 270.0f }, { 134, "dark", 281.4f }, { 135, "dark", 281.4f }, { 136, "clod", 282.8f },
			{ 136, "mist", 280.8f }, { 136, "snow", 290.0f }, { 137, "clod", 282.8f }, { 137, "mist", 280.8f },
			{ 137, "snow", 290.0f }, { 138, "clod", 277.4f }, { 138, "dark", 270.0f }, { 139, "clod", 279.2f },
			{ 139, "mist", 281.3f }, { 139, "suny", 276.3f }, { 140, "clod", 281.1f }, { 140, "mist", 281.1f },
			{ 140, "suny", 276.1f }, { 141, "clod", 281.1f }, { 141, "mist", 281.1f }, { 141, "suny", 276.1f },
			{ 142, "clod", 279.2f }, { 142, "mist", 281.3f }, { 142, "suny", 276.3f }, { 145, "clod", 281.1f },
			{ 145, "mist", 281.1f }, { 145, "suny", 275.2f }, { 146, "clod", 281.1f }, { 146, "mist", 281.1f },
			{ 146, "suny", 275.2f }, { 147, "clod", 281.6f }, { 147, "mist", 281.1f }, { 147, "rain", 261.0f },
			{ 147, "suny", 276.1f }, { 147, "thdr", 280.7f }, { 149, "clod", 279.8f }, { 149, "mist", 280.3f },
			{ 149, "rain", 261.0f }, { 149, "suny", 276.4f }, { 149, "thdr", 280.7f }, { 151, "clod", 282.0f },
			{ 151, "dust", 197.3f }, { 151, "suny", 276.1f }, { 156, "clod", 277.4f }, { 156, "dark", 270.0f },
			{ 161, "clod", 283.4f }, { 161, "dark", 281.4f }, { 161, "fogd", 270.0f }, { 161, "mist", 280.0f },
			{ 161, "snow", 270.0f }, { 165, "clod", 282.6f }, { 165, "dark", 282.4f }, { 165, "fogd", 270.0f },
			{ 165, "mist", 283.2f }, { 165, "snow", 279.3f }, { 177, "aura", 270.0f }, { 177, "fine", 276.3f },
			{ 177, "suny", 275.3f }, { 177, "wind", 280.7f }, { 178, "aura", 270.0f }, { 178, "fine", 276.3f },
			{ 178, "suny", 275.3f }, { 178, "wind", 280.7f }, { 185, "dark", 281.4f }, { 186, "dark", 281.4f },
			{ 187, "dark", 281.4f }, { 188, "dark", 281.4f }, { 190, "clod", 281.1f }, { 190, "mist", 279.9f },
			{ 190, "suny", 276.1f }, { 191, "clod", 281.1f }, { 191, "mist", 278.9f }, { 191, "suny", 276.1f },
			{ 198, "clod", 281.2f }, { 198, "dust", 190.4f }, { 198, "mist", 281.3f }, { 198, "suny", 274.9f },
			{ 198, "wind", 270.0f }, { 205, "clod", 281.1f }, { 205, "mist", 280.0f }, { 205, "rain", 236.4f },
			{ 205, "squl", 236.4f }, { 205, "suny", 276.1f }, { 206, "bliz", 280.0f }, { 206, "clod", 281.1f },
			{ 206, "fogd", 270.0f }, { 206, "mist", 280.0f }, { 206, "snow", 280.0f }, { 210, "clod", 281.1f },
			{ 210, "mist", 281.1f }, { 210, "suny", 276.1f }, { 220, "clod", 280.8f }, { 220, "mist", 281.7f },
			{ 220, "rain", 261.0f }, { 220, "suny", 192.1f }, { 220, "thdr", 191.6f }, { 221, "clod", 280.8f },
			{ 221, "mist", 281.7f }, { 221, "rain", 261.0f }, { 221, "suny", 192.1f }, { 221, "thdr", 191.6f },
			{ 223, "suny", 191.5f }, { 224, "suny", 191.5f }, { 225, "suny", 191.5f }, { 226, "suny", 191.9f },
			{ 227, "clod", 280.8f }, { 227, "mist", 281.7f }, { 227, "rain", 261.0f }, { 227, "suny", 192.1f },
			{ 227, "thdr", 191.6f }, { 228, "clod", 280.8f }, { 228, "mist", 281.7f }, { 228, "rain", 261.0f },
			{ 228, "suny", 192.1f }, { 228, "thdr", 191.6f }, { 229, "clod", 277.4f }, { 229, "dark", 270.0f },
			{ 230, "clod", 281.3f }, { 230, "mist", 280.3f }, { 230, "suny", 276.4f }, { 231, "clod", 281.3f },
			{ 231, "mist", 280.3f }, { 231, "suny", 276.4f }, { 232, "clod", 281.3f }, { 232, "mist", 280.3f },
			{ 232, "suny", 276.4f }, { 233, "clod", 281.3f }, { 233, "mist", 280.3f }, { 233, "suny", 276.4f },
			{ 234, "clod", 281.6f }, { 234, "mist", 281.1f }, { 234, "suny", 276.1f }, { 235, "clod", 281.3f },
			{ 235, "mist", 280.6f }, { 235, "suny", 276.4f }, { 236, "clod", 281.3f }, { 236, "mist", 280.3f },
			{ 236, "suny", 276.4f }, { 237, "clod", 281.3f }, { 237, "mist", 280.3f }, { 237, "suny", 276.4f },
			{ 238, "clod", 281.4f }, { 238, "mist", 281.1f }, { 238, "suny", 276.1f }, { 239, "clod", 281.4f },
			{ 239, "mist", 281.4f }, { 239, "suny", 276.2f }, { 240, "clod", 280.3f }, { 240, "mist", 281.1f },
			{ 240, "suny", 276.1f }, { 241, "clod", 281.6f }, { 241, "mist", 281.1f }, { 241, "suny", 276.1f },
			{ 243, "clod", 279.8f }, { 243, "mist", 280.3f }, { 243, "suny", 276.4f }, { 244, "clod", 280.0f },
			{ 244, "mist", 280.8f }, { 244, "suny", 276.4f }, { 245, "clod", 279.9f }, { 245, "mist", 280.6f },
			{ 245, "suny", 276.4f }, { 246, "clod", 279.9f }, { 246, "mist", 280.8f }, { 246, "suny", 276.4f },
			{ 247, "dust", 197.3f }, { 247, "sand", 196.4f }, { 247, "suny", 276.1f }, { 248, "clod", 283.1f },
			{ 248, "dust", 197.3f }, { 248, "mist", 281.1f }, { 248, "suny", 276.1f }, { 249, "clod", 281.6f },
			{ 249, "mist", 281.1f }, { 249, "rain", 262.4f }, { 249, "squl", 262.4f }, { 249, "suny", 276.1f },
			{ 249, "wind", 276.9f }, { 250, "clod", 281.4f }, { 250, "mist", 281.1f }, { 250, "suny", 276.1f },
			{ 251, "bolt", 275.3f }, { 251, "clod", 275.3f }, { 251, "fine", 275.3f }, { 251, "mist", 275.3f },
			{ 251, "rain", 275.3f }, { 251, "suny", 275.3f }, { 251, "thdr", 275.3f }, { 252, "clod", 281.4f },
			{ 252, "mist", 281.1f }, { 252, "suny", 276.1f }, { 252, "thdr", 284.0f }, { 272, "clod", 281.1f },
			{ 272, "mist", 281.1f }, { 272, "suny", 276.1f }, { 273, "clod", 281.1f }, { 273, "mist", 281.1f },
			{ 273, "suny", 276.1f }
		};
		// (where the zone's clouds say nothing: west, the way they go in most zones)
		constexpr float kWindDefault = 275.0f;

		// For the weather now, blended with the one it's changing from
		float blended(const weather &w, float (*of)(uint32_t), float unknown)
		{
			if (!w.known || w.name == 0)
				return unknown;
			const float now = of(w.name);
			return w.before != 0 && w.change < 1.0f ? of(w.before) + (now - of(w.before)) * std::clamp(w.change, 0.0f, 1.0f) : now;
		}
	}

	bool decode_weather(const uint32_t *words, uint32_t count, weather &out)
	{
		out = weather();
		if (words == nullptr || count < 8)
			return false;
		const uint32_t read = words[7];
		if ((read & 8) != 0 && words[0] < 0x10000)
			out.id = words[0];
		if ((read & 4) != 0)
		{
			out.name = words[1];
			out.before = words[2];
			const float change = word_float(words[3]);
			out.change = std::isfinite(change) ? std::clamp(change, 0.0f, 1.0f) : 1.0f;
		}
		if ((read & 2) != 0 && fog_ok(word_float(words[5]), word_float(words[6])))
		{
			out.fog = true;
			for (int i = 0; i < 3; ++i)
				out.fog_color[i] = float((words[4] >> (16 - 8 * i)) & 0xFF) / 255.0f; // (0x00RRGGBB)
			out.fog_start = word_float(words[5]);
			out.fog_end = word_float(words[6]);
		}
		if ((read & 1) != 0 && count >= kWeatherWords)
		{
			const uint32_t *r = words + 8; // (the record: offsets / 4)
			const float intensity = word_float(r[0x44 / 4]), distance = word_float(r[0x58 / 4]);
			const float fog_end = word_float(r[0x3C / 4]), fog_start = word_float(r[0x40 / 4]);
			if (std::isfinite(intensity) && intensity >= 0.0f && intensity < 100.0f && std::isfinite(distance) && distance >= 0.0f && distance < 1e6f &&
				fog_ok(fog_start, fog_end))
			{
				out.record = true;
				record_color(r[0x2C / 4], out.sun_color);
				record_color(r[0x34 / 4], out.ambient);
				record_color(r[0x38 / 4], out.record_fog_color);
				record_color(r[0x4C / 4], out.sky);
				out.intensity = intensity;
				out.draw_distance = distance;
				out.record_fog_start = fog_start;
				out.record_fog_end = fog_end;
				// (not the fog to draw with when the renderer's isn't read: the record's is before the draw distance
				// setting, five times nearer than the game's own with drawdistance's 5)
				// Its sky (sky part = record +0x4C): +0 the clear colour, +0x12 the dome's segments (a byte), +0x20 the 8
				// gradient colours (R G B A bytes), +0x40 their 8 heights (floats, 0 first, 1 last, rising)
				bool heights_ok = true;
				for (int i = 0; i < 8; ++i)
				{
					const float h = word_float(r[0x8C / 4 + i]);
					heights_ok = heights_ok && std::isfinite(h) && h >= -1e-4f && h <= 1.0001f && (i == 0 || h >= out.sky_heights[i - 1]);
					out.sky_heights[i] = heights_ok ? std::clamp(h, 0.0f, 1.0f) : 0.0f;
					for (int k = 0; k < 3; ++k)
						out.sky_colors[i][k] = float((r[0x6C / 4 + i] >> (8 * k)) & 0xFF);
				}
				out.sky_known = heights_ok && out.sky_heights[0] < 0.01f && out.sky_heights[7] > 0.99f;
				out.sky_segments = (r[0x5C / 4] >> 16) & 0xFF;
				for (int k = 0; k < 3; ++k)
					out.sky_clear[k] = float((r[0x4C / 4] >> (8 * k)) & 0xFF);
			}
		}
		out.known = out.id != 0xFFFFFFFF || out.name != 0 || out.fog || out.record;
		return out.known;
	}

	float weather_rain(uint32_t name)
	{
		const weather_kind *k = weather_by_name(name);
		return k != nullptr ? k->rain : 0.0f;
	}

	// (03D640: _ftol((h1 - h0) * 12.048f), 0 taken as 1)
	uint32_t dome_strips(float h0, float h1)
	{
		const float n = (h1 - h0) * 12.048f;
		return n >= 1.0f ? uint32_t(n) : 1u;
	}

	// The ring of strips is drawn with vertices at azimuth k * 6.283 / segments from north toward east (the last one
	// closing at the first's, 2 pi), each strip's lower and upper edge at elevation (h0 + f (h1 - h0)) * 1.570796 for
	// f = m / n and (m + 1) / n, coloured c0 * (1 - f) + c1 * f a byte at a time, each part cut to a whole byte (03DFE0,
	// 03E1D0). A strip's facet is flat: across the segment's middle (u along it, v up) its two edges lie at (cos e cos
	// half, sin e), half the segment's angle; a ray at phi from the middle meets the facet where (cos el cos phi, sin el
	// cos half) points, scaled so the edges lie on the unit circle, and Gouraud there is linear in its height.
	void dome_colour(const look::sky_dome &d, const float dir[3], float out[3])
	{
		const float up = -dir[1], horiz = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
		if (up <= 0.0f || d.segments <= 2)
		{
			for (int k = 0; k < 3; ++k)
				out[k] = d.clear[k];
			return;
		}
		const float two_pi = 6.2831853f, segments = float(d.segments), seg = 6.283f / segments;
		float az = std::atan2(dir[0], dir[2]);
		if (az < 0.0f)
			az += two_pi;
		const float k = std::min(std::floor(az / seg), segments - 1.0f);
		const float a0 = k * seg, a1 = k >= segments - 1.0f ? two_pi : (k + 1.0f) * seg;
		const float phi = az - 0.5f * (a0 + a1), half = 0.5f * (a1 - a0);
		const float rx = horiz * std::cos(phi), ry = up * std::cos(half);
		const float height = std::atan2(ry, rx) / 1.570796f;
		int band = 0;
		for (int i = 0; i < 7; ++i)
			if (height >= d.stops[i][3] && d.stops[i + 1][3] > d.stops[i][3])
				band = i;
		const float h0 = d.stops[band][3], h1 = d.stops[band + 1][3];
		const float n = float(dome_strips(h0, h1));
		const float f = h1 > h0 ? std::clamp((height - h0) / (h1 - h0), 0.0f, 1.0f) : 0.0f;
		const float m = std::clamp(std::floor(f * n), 0.0f, n - 1.0f);
		const float f1 = m / n, f2 = (m + 1.0f) / n;
		const float e_lo = (f1 * h1 + (1.0f - f1) * h0) * 1.570796f, e_hi = (f2 * h1 + (1.0f - f2) * h0) * 1.570796f;
		const float lo_x = std::cos(e_lo), lo_y = std::sin(e_lo), dx = std::cos(e_hi) - lo_x, dy = std::sin(e_hi) - lo_y;
		const float den = rx * dy - ry * dx;
		const float s = den != 0.0f ? std::clamp((ry * lo_x - rx * lo_y) / den, 0.0f, 1.0f) : 0.0f;
		for (int c = 0; c < 3; ++c)
		{
			const float c0 = d.stops[band][c], c1 = d.stops[band + 1][c];
			const float lo = std::floor(c0 * (1.0f - f1)) + std::floor(c1 * f1), hi = std::floor(c0 * (1.0f - f2)) + std::floor(c1 * f2);
			out[c] = lo + (hi - lo) * s;
		}
	}

	// Each stop's slope (bytes per unit of height), so the gradient passes through every stop and bends smoothly there
	// without overshooting (monotone: Fritsch and Butland's weighted harmonic mean of the two sides' slopes; none where
	// they disagree in sign; one side's alone at the ends and next to two stops at one height, where the colour jumps)
	void sky_slopes(const look::sky_dome &d, float out[8][3])
	{
		for (int i = 0; i < 8; ++i)
			for (int c = 0; c < 3; ++c)
			{
				const bool left = i > 0 && d.stops[i][3] > d.stops[i - 1][3], right = i < 7 && d.stops[i + 1][3] > d.stops[i][3];
				const float hl = left ? d.stops[i][3] - d.stops[i - 1][3] : 0.0f, hr = right ? d.stops[i + 1][3] - d.stops[i][3] : 0.0f;
				const float sl = left ? (d.stops[i][c] - d.stops[i - 1][c]) / hl : 0.0f, sr = right ? (d.stops[i + 1][c] - d.stops[i][c]) / hr : 0.0f;
				float m = 0.0f;
				if (left && right)
				{
					if (sl * sr > 0.0f)
					{
						const float w1 = 2.0f * hr + hl, w2 = hr + 2.0f * hl;
						m = (w1 + w2) / (w1 / sl + w2 / sr);
					}
				}
				else
					m = left ? sl : sr;
				out[i][c] = m;
			}
	}

	void sky_gradient(const look::sky_dome &d, float height, float out[3])
	{
		float slopes[8][3];
		sky_slopes(d, slopes);
		int k = 0;
		for (int i = 0; i < 7; ++i)
			if (height >= d.stops[i][3] && d.stops[i + 1][3] > d.stops[i][3])
				k = i;
		const float h0 = d.stops[k][3], dh = d.stops[k + 1][3] - h0;
		const float t = dh > 0.0f ? std::clamp((height - h0) / dh, 0.0f, 1.0f) : 0.0f, t2 = t * t, t3 = t2 * t;
		for (int c = 0; c < 3; ++c)
			out[c] = (2.0f * t3 - 3.0f * t2 + 1.0f) * d.stops[k][c] + (t3 - 2.0f * t2 + t) * dh * slopes[k][c] +
				(3.0f * t2 - 2.0f * t3) * d.stops[k + 1][c] + (t3 - t2) * dh * slopes[k + 1][c];
	}

	bool read_clear_sky_file(const wchar_t *path, std::vector<clear_sky> &out, std::string &error)
	{
		out.clear();
		FILE *f = nullptr;
		if (_wfopen_s(&f, path, L"r") != 0 || f == nullptr)
		{
			error = "no worldfx-clearsky.txt next to the add-on";
			return false;
		}
		char line[1024];
		uint32_t zones = 0;
		while (fgets(line, sizeof(line), f) != nullptr)
		{
			if (line[0] == '#' || line[0] < '0' || line[0] > '9')
				continue;
			char *at = line;
			const unsigned long zone = std::strtoul(at, &at, 10);
			clear_sky sky;
			int n = 0;
			for (; n < 24; ++n)
			{
				char *end = nullptr;
				const float v = std::strtof(at, &end);
				if (end == at || !std::isfinite(v) || v < 0.0f || v > 100.0f)
					break;
				sky.light[n] = v;
				at = end;
			}
			if (n != 24 || zone >= 4096)
				continue;
			sky.known = true;
			if (out.size() <= zone)
				out.resize(zone + 1);
			out[zone] = sky;
			zones++;
		}
		fclose(f);
		if (zones == 0)
		{
			error = "worldfx-clearsky.txt holds no zones";
			return false;
		}
		return true;
	}

	float clear_sky_light(const clear_sky &sky, double hour)
	{
		if (!sky.known || !std::isfinite(hour))
			return 0.0f;
		const double h = std::fmod(std::fmod(hour, 24.0) + 24.0, 24.0);
		const int a = int(h) % 24, b = (a + 1) % 24;
		const float f = float(h - std::floor(h));
		return sky.light[a] + (sky.light[b] - sky.light[a]) * f;
	}

	float weather_light(const weather &w)
	{
		if (!w.record)
			return 0.0f;
		return (0.3f * w.sun_color[0] + 0.59f * w.sun_color[1] + 0.11f * w.sun_color[2]) * w.intensity;
	}

	float weather_sunlight(const weather &w, float clear_light)
	{
		// (below this the clear sky's own light is too faint to compare with: the darkest hours of the night)
		constexpr float kFaintest = 0.05f;
		if (!w.record || !std::isfinite(clear_light) || clear_light < kFaintest)
			return 1.0f;
		const float share = weather_light(w) / clear_light;
		return std::isfinite(share) ? std::clamp(share, 0.0f, 1.0f) : 1.0f;
	}

	float weather_rain(const weather &w)
	{
		return blended(w, static_cast<float (*)(uint32_t)>(weather_rain), 0.0f);
	}

	float weather_downpour(uint32_t name)
	{
		const weather_kind *k = weather_by_name(name);
		return k != nullptr ? k->downpour : 0.0f;
	}

	float weather_slant(uint32_t name)
	{
		const weather_kind *k = weather_by_name(name);
		return k != nullptr ? k->slant : 0.0f;
	}

	float weather_downpour(const weather &w)
	{
		return blended(w, static_cast<float (*)(uint32_t)>(weather_downpour), 0.0f);
	}

	float weather_slant(const weather &w)
	{
		return blended(w, static_cast<float (*)(uint32_t)>(weather_slant), 0.0f);
	}

	float weather_snowfall(uint32_t name)
	{
		const weather_kind *k = weather_by_name(name);
		return k != nullptr ? k->snowfall : 0.0f;
	}

	float weather_sway(uint32_t name)
	{
		const weather_kind *k = weather_by_name(name);
		return k != nullptr ? k->sway : 0.0f;
	}

	float weather_snowfall(const weather &w)
	{
		return blended(w, static_cast<float (*)(uint32_t)>(weather_snowfall), 0.0f);
	}

	float weather_sway(const weather &w)
	{
		return blended(w, static_cast<float (*)(uint32_t)>(weather_sway), 0.0f);
	}

	weather_kind_fx weather_fx_of(uint32_t name)
	{
		const weather_kind *k = weather_by_name(name);
		return k != nullptr ? kWeatherFx[k - kWeathers] : weather_kind_fx();
	}

	weather_kind_fx weather_fx_of(const weather &w)
	{
		if (!w.known || w.name == 0)
			return weather_kind_fx();
		const weather_kind_fx now = weather_fx_of(w.name);
		if (w.before == 0 || w.change >= 1.0f)
			return now;
		const weather_kind_fx was = weather_fx_of(w.before);
		const float f = std::clamp(w.change, 0.0f, 1.0f);
		auto mix = [f](float a, float b) { return a + (b - a) * f; };
		weather_kind_fx out;
		out.cover = mix(was.cover, now.cover);
		out.thick = mix(was.thick, now.thick);
		out.dark = mix(was.dark, now.dark);
		out.veil = mix(was.veil, now.veil);
		out.speed = mix(was.speed, now.speed);
		out.wind = mix(was.wind, now.wind);
		out.windy = mix(was.windy, now.windy);
		out.streaks = mix(was.streaks, now.streaks);
		out.leaves = mix(was.leaves, now.leaves);
		out.dust = mix(was.dust, now.dust);
		// (sand's share of the dust in the air: each weather's by how much dust it has)
		const float d0 = was.dust * (1.0f - f), d1 = now.dust * f;
		out.sand = d0 + d1 > 0.0f ? (was.sand * d0 + now.sand * d1) / (d0 + d1) : 0.0f;
		out.flashes = mix(was.flashes, now.flashes);
		out.heat = mix(was.heat, now.heat);
		out.aurora = mix(was.aurora, now.aurora);
		out.gloom = mix(was.gloom, now.gloom);
		out.sky_dark = mix(was.sky_dark, now.sky_dark);
		out.stars = mix(was.stars, now.stars);
		return out;
	}

	float wind_bearing(uint32_t zone, uint32_t name)
	{
		const weather_kind *k = weather_by_name(name);
		float x = 0.0f, z = 0.0f;
		for (const zone_wind &w : kZoneWinds)
		{
			if (w.zone != zone)
				continue;
			const uint32_t n = weather_name(w.name);
			if (n == name || (k != nullptr && (n == weather_name(k->name) || n == weather_name(k->second))))
				return w.bearing * 0.017453293f;
			x += std::sin(w.bearing * 0.017453293f);
			z += std::cos(w.bearing * 0.017453293f);
		}
		return x * x + z * z > 1e-6f ? std::atan2(x, z) : kWindDefault * 0.017453293f;
	}

	float wind_bearing(uint32_t zone, const weather &w)
	{
		const float now = wind_bearing(zone, w.known ? w.name : 0u);
		if (!w.known || w.before == 0 || w.change >= 1.0f)
			return now;
		const float was = wind_bearing(zone, w.before), f = std::clamp(w.change, 0.0f, 1.0f);
		const float x = std::sin(was) * (1.0f - f) + std::sin(now) * f, z = std::cos(was) * (1.0f - f) + std::cos(now) * f;
		return x * x + z * z > 1e-6f ? std::atan2(x, z) : now;
	}

	// The colours as the zone is lit now. The sky's own colours (the record's gradient: its horizon's and, a third of the
	// way up, its sky's, bytes) say how bright it is out there, by day or night; the record's sun (or moon, by night: the
	// game's light then) its colour. A cloud's top where the light reaches it is the horizon's colour with that light on
	// it, a little brighter than the sky by day, dim grey-blue by night; its underside the horizon's, greyer, as dark as
	// the weather's; dust and sand their own colours at the sky's brightness, gloom's fog the game's fog darkened and greyed.
	// Without a sky (a zone with none, or the record not read) the game's fog stands for the horizon.
	void weather_look(const weather &w, const look::sky_dome &dome, look::weather_fx &wx)
	{
		const weather_kind_fx k = weather_fx_of(w);
		wx.cloud_cover = k.cover;
		wx.cloud_thick = k.thick;
		wx.cloud_veil = k.veil;
		wx.wind_strength = k.wind;
		wx.dust = k.dust;
		wx.streaks = k.streaks;
		wx.gloom = k.gloom;
		wx.heat = k.heat;
		wx.aurora = k.aurora;
		wx.sky_dark = k.sky_dark;
		wx.star_boost = k.stars;
		wx.leaves = k.leaves;
		float fog[3] = { 0.55f, 0.58f, 0.62f };
		for (int i = 0; i < 3; ++i)
			fog[i] = w.fog ? w.fog_color[i] : w.record ? w.record_fog_color[i] : fog[i];
		float horizon[3], high[3];
		const bool sky = dome.on && dome.segments > 2;
		if (sky)
		{
			sky_gradient(dome, 0.02f, horizon);
			sky_gradient(dome, 0.33f, high);
			for (int i = 0; i < 3; ++i)
			{
				horizon[i] = std::clamp(horizon[i] / 255.0f, 0.0f, 1.0f);
				high[i] = std::clamp(high[i] / 255.0f, 0.0f, 1.0f);
			}
		}
		else
			for (int i = 0; i < 3; ++i)
				horizon[i] = high[i] = fog[i];
		// (the light's colour, its brightest part 1: how bright comes from the sky)
		float light[3] = { 1.0f, 0.97f, 0.92f };
		if (w.record)
		{
			const float most = std::max({ w.sun_color[0], w.sun_color[1], w.sun_color[2], 0.05f });
			for (int i = 0; i < 3; ++i)
				light[i] = w.sun_color[i] / most;
		}
		const float bright = std::clamp(std::pow(std::max(luma(horizon), 0.0f), 1.5f) * 1.8f, 0.02f, 1.1f);
		const float grey_h = luma(horizon);
		for (int i = 0; i < 3; ++i)
		{
			const float shade = (horizon[i] + (grey_h - horizon[i]) * 0.5f) * (0.95f - 0.75f * k.dark);
			const float lit = std::min(horizon[i] * 0.55f + light[i] * bright * 0.7f, 1.0f);
			wx.cloud_shade[i] = shade;
			wx.cloud_lit[i] = lit + (shade - lit) * 0.65f * k.dark;
			wx.veil_colour[i] = horizon[i] + (fog[i] - horizon[i]) * 0.5f;
		}
		// Dust (brown-grey) and sand (tan) at the sky's brightness, a little of the fog's colour in them; the clouds take
		// some of their colour too
		static const float dust_albedo[3] = { 0.62f, 0.54f, 0.43f }, sand_albedo[3] = { 0.82f, 0.69f, 0.47f };
		const float sky_bright = std::clamp(luma(high) * 0.4f + luma(horizon) * 0.6f, 0.03f, 1.0f) / 0.62f;
		for (int i = 0; i < 3; ++i)
		{
			const float albedo = dust_albedo[i] + (sand_albedo[i] - dust_albedo[i]) * std::clamp(k.sand, 0.0f, 1.0f);
			wx.dust_colour[i] = std::clamp(albedo * sky_bright * 0.8f + fog[i] * 0.2f, 0.0f, 1.0f);
			const float tint = std::min(k.dust, 1.0f) * 0.6f;
			wx.cloud_lit[i] += (wx.dust_colour[i] * 1.1f - wx.cloud_lit[i]) * tint;
			wx.cloud_shade[i] += (wx.dust_colour[i] * 0.8f - wx.cloud_shade[i]) * tint;
			wx.veil_colour[i] += (wx.dust_colour[i] - wx.veil_colour[i]) * tint;
		}
		// Gloom's fog: the game's own fog, darker and greyer
		const float grey_f = luma(fog);
		for (int i = 0; i < 3; ++i)
		{
			wx.gloom_colour[i] = (fog[i] + (grey_f - fog[i]) * 0.4f) * 0.8f;
			wx.leaf_light[i] = std::clamp(horizon[i] * 0.5f + light[i] * bright * 0.6f, 0.03f, 1.2f);
		}
	}

	bool read_blade_file(const wchar_t *path, blade_file &out, std::string &error, const char *magic, uint32_t stride)
	{
		FILE *f = nullptr;
		if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr)
		{
			error = "no file for this zone";
			return false;
		}
		struct header
		{
			char magic[4];
			uint32_t zone;
			float cell_size;
			uint32_t cells;
			uint32_t blades;
		} h = {};
		bool ok = fread(&h, sizeof(h), 1, f) == 1 && std::memcmp(h.magic, magic, 4) == 0 && h.cells < 1000000 && h.blades < 16000000;
		if (ok)
		{
			out.zone = h.zone;
			out.cell_size = h.cell_size;
			out.cells.resize(h.cells);
			out.stride = stride;
			out.blades.resize(size_t(h.blades) * stride);
			ok = (h.cells == 0 || fread(out.cells.data(), sizeof(blade_file::cell), h.cells, f) == h.cells) &&
				(h.blades == 0 || fread(out.blades.data(), size_t(4) * stride, h.blades, f) == h.blades);
		}
		fclose(f);
		if (ok)
			for (const blade_file::cell &c : out.cells)
				if (uint64_t(c.first) + c.count > out.blade_count())
					ok = false;
		if (!ok)
		{
			error = "the file is damaged";
			out = {};
		}
		return ok;
	}

	bool read_sea_file(const wchar_t *path, blade_file &out, uint32_t &sheets, std::string &error)
	{
		out = {};
		sheets = 0;
		FILE *f = nullptr;
		if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr)
		{
			error = "no file for this zone";
			return false;
		}
		struct header
		{
			char magic[4];
			uint32_t zone, sheets, corners;
		} h = {};
		bool ok = fread(&h, sizeof(h), 1, f) == 1 && std::memcmp(h.magic, "SEA1", 4) == 0 && h.corners >= 3 && h.corners % 3 == 0 &&
			h.corners < 16000000 && h.sheets != 0;
		if (ok)
		{
			out.blades.resize(size_t(h.corners) * 8);
			ok = fread(out.blades.data(), 32, h.corners, f) == h.corners;
		}
		fclose(f);
		for (size_t i = 0; ok && i < out.blades.size(); ++i)
			ok = std::isfinite(out.blades[i]) && std::fabs(out.blades[i]) < 1e6f;
		if (!ok)
		{
			error = "the file is damaged";
			out = {};
			return false;
		}
		out.zone = h.zone;
		out.stride = 8;
		sheets = h.sheets;
		return true;
	}

	bool read_wet_file(const wchar_t *path, wet_file &out, std::string &error)
	{
		out = {};
		FILE *f = nullptr;
		if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr)
		{
			error = "no file for this zone";
			return false;
		}
		struct header
		{
			char magic[4];
			uint32_t zone;
			float square;
			uint32_t width, set, height;
			float x0, z0;
			uint32_t words_a_row;
		} h = {};
		bool ok = fread(&h, sizeof(h), 1, f) == 1 && std::memcmp(h.magic, "WET1", 4) == 0 && h.width > 0 && h.width <= 40000 && h.height > 0 &&
			h.height <= 40000 && h.words_a_row == (h.width + 31) / 32 && std::isfinite(h.square) && h.square > 0.01f && std::isfinite(h.x0) &&
			std::isfinite(h.z0);
		if (ok)
		{
			out.words.resize(size_t(h.height) * h.words_a_row);
			ok = fread(out.words.data(), 4, out.words.size(), f) == out.words.size();
		}
		fclose(f);
		if (!ok)
		{
			error = "the file is damaged";
			out = {};
			return false;
		}
		out.zone = h.zone;
		out.square = h.square;
		out.x0 = h.x0;
		out.z0 = h.z0;
		out.width = h.width;
		out.height = h.height;
		out.words_a_row = h.words_a_row;
		out.set = h.set;
		return true;
	}

	// Windows' own XPRESS (with Huffman): ntdll, there on every Windows since 8. raw bytes out of the packed ones
	static bool xpress_unpack(const std::vector<uint8_t> &packed, size_t raw, std::vector<uint8_t> &out, std::string &error)
	{
		using workspace_fn = LONG(NTAPI *)(USHORT, PULONG, PULONG);
		using unpack_fn = LONG(NTAPI *)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG, PVOID);
		HMODULE nt = GetModuleHandleW(L"ntdll.dll");
		const auto workspace_size = nt != nullptr ? reinterpret_cast<workspace_fn>(GetProcAddress(nt, "RtlGetCompressionWorkSpaceSize")) : nullptr;
		const auto unpack = nt != nullptr ? reinterpret_cast<unpack_fn>(GetProcAddress(nt, "RtlDecompressBufferEx")) : nullptr;
		ULONG work = 0, fragment = 0, got = 0;
		if (workspace_size == nullptr || unpack == nullptr || workspace_size(4, &work, &fragment) != 0)
		{
			error = "Windows' XPRESS unpacking isn't there";
			return false;
		}
		std::vector<uint8_t> workspace(work + 16);
		out.resize(raw);
		if (unpack(4, out.data(), static_cast<ULONG>(raw), const_cast<PUCHAR>(packed.data()), static_cast<ULONG>(packed.size()), &got, workspace.data()) != 0 || got != raw)
		{
			error = "the file is damaged";
			return false;
		}
		return true;
	}

	bool read_rain_file(const wchar_t *path, rain_file &out, std::string &error)
	{
		out = {};
		FILE *f = nullptr;
		if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr)
		{
			error = "no file for this zone";
			return false;
		}
		struct header
		{
			char magic[4];
			uint32_t zone;
			float square;
			uint32_t width, covered, height;
			float x0, z0, y0, step;
			uint32_t packing, packed;
		} h = {};
		static_assert(sizeof(header) == 48, "the rain file's header is 48 bytes");
		bool ok = fread(&h, sizeof(h), 1, f) == 1 && std::memcmp(h.magic, "RHM1", 4) == 0 && h.width > 0 && h.width <= 20000 && h.height > 0 &&
			h.height <= 20000 && std::isfinite(h.square) && h.square > 0.01f && std::isfinite(h.x0) && std::isfinite(h.z0) && std::isfinite(h.y0) &&
			std::isfinite(h.step) && h.step > 0.0f && (h.packing == 0 || h.packing == 4) && h.packed < 0x40000000u;
		const size_t squares = size_t(h.width) * h.height, raw = squares * 3;
		std::vector<uint8_t> packed;
		if (ok)
		{
			packed.resize(h.packed);
			ok = h.packed == 0 || fread(packed.data(), 1, packed.size(), f) == packed.size();
		}
		fclose(f);
		std::vector<uint8_t> planes;
		if (ok && h.packing == 0)
		{
			ok = packed.size() == raw;
			planes.swap(packed);
		}
		else if (ok)
		{
			std::string why;
			ok = xpress_unpack(packed, raw, planes, why);
			if (!ok && why != "the file is damaged")
			{
				error = why;
				return false;
			}
		}
		if (!ok)
		{
			error = "the file is damaged";
			return false;
		}
		// The kinds plane, then each row's heights as differences from the square before (mod 65536)
		out.kinds.assign(planes.begin(), planes.begin() + squares);
		out.q.resize(squares);
		const uint8_t *d = planes.data() + squares;
		for (uint32_t j = 0; j < h.height; ++j)
		{
			uint16_t q = 0;
			for (uint32_t i = 0; i < h.width; ++i)
			{
				const size_t k = size_t(j) * h.width + i;
				q = static_cast<uint16_t>(q + (d[k * 2] | d[k * 2 + 1] << 8));
				out.q[k] = q;
				if (out.kinds[k] > kRainWater)
					ok = false;
			}
		}
		if (!ok)
		{
			error = "the file is damaged";
			out = {};
			return false;
		}
		out.zone = h.zone;
		out.square = h.square;
		out.x0 = h.x0;
		out.z0 = h.z0;
		out.y0 = h.y0;
		out.step = h.step;
		out.width = h.width;
		out.height = h.height;
		out.covered = h.covered;
		return true;
	}

	// The compact file ('SHD2', tools/bake_shadow_geo.py write_compact): each placed model's casting triangles once,
	// in its own space, and the zone's placements, packed; expanded here into what an SHD1 file holds (the same cells,
	// the triangles in the same order, cut-out words kept only on alpha-tested placements). f is past the first 32
	// bytes of its 64-byte header (in head); this closes it
	static bool read_compact_shadow(FILE *f, const void *head, shadow_file &out, std::string &error)
	{
		struct header
		{
			char magic[4];
			uint32_t zone;
			float cell_size;
			uint32_t models, model_triangles, placements, masks, words, packing, packed, raw, expanded, reserved[4];
		} h = {};
		static_assert(sizeof(header) == 64, "the compact shadow file's header is 64 bytes");
		std::memcpy(&h, head, 32);
		bool ok = fread(reinterpret_cast<char *>(&h) + 32, 32, 1, f) == 1 && h.models < 1000000 && h.model_triangles < 8000000 &&
			h.placements < 2000000 && h.masks < 256 && h.words < 16000000 && (h.packing == 0 || h.packing == 4) && h.packed < 0x40000000u &&
			h.expanded < 8000000 && std::isfinite(h.cell_size) && h.cell_size > 0.0f &&
			uint64_t(h.raw) == uint64_t(h.models) * 8 + uint64_t(h.model_triangles) * 48 + uint64_t(h.placements) * 56 + uint64_t(h.masks) * 16 + uint64_t(h.words) * 4;
		std::vector<uint8_t> packed, body;
		if (ok)
		{
			packed.resize(h.packed);
			ok = h.packed == 0 || fread(packed.data(), 1, packed.size(), f) == packed.size();
		}
		fclose(f);
		if (ok && h.packing == 0)
		{
			ok = packed.size() == h.raw;
			body.swap(packed);
		}
		else if (ok)
		{
			std::string why;
			if (!xpress_unpack(packed, h.raw, body, why))
			{
				error = why;
				return false;
			}
		}
		if (!ok)
		{
			error = "the file is damaged";
			return false;
		}
		const uint8_t *p = body.data();
		const uint32_t *ranges = reinterpret_cast<const uint32_t *>(p);
		const uint32_t *model_tris = ranges + size_t(h.models) * 2;
		const uint8_t *placements = reinterpret_cast<const uint8_t *>(model_tris + size_t(h.model_triangles) * 12);
		const uint32_t *masks = reinterpret_cast<const uint32_t *>(placements + size_t(h.placements) * 56);
		const uint32_t *words = masks + size_t(h.masks) * 4;
		for (uint32_t m = 0; m < h.models && ok; ++m)
			ok = uint64_t(ranges[m * 2]) + ranges[m * 2 + 1] <= h.model_triangles;
		for (size_t i = 0; i < size_t(h.model_triangles) * 12 && ok; i += 4) // (no corner names a mask that isn't there)
			ok = (model_tris[i + 3] >> 24) <= h.masks;
		uint64_t expanded = 0;
		for (uint32_t i = 0; i < h.placements && ok; ++i)
		{
			uint32_t model;
			std::memcpy(&model, placements + size_t(i) * 56, 4);
			ok = model < h.models;
			if (ok)
				expanded += ranges[model * 2 + 1];
		}
		ok = ok && expanded == h.expanded;
		for (uint32_t m = 0; m < h.masks && ok; ++m) // (each mask's bits within the words)
		{
			const uint32_t *t = masks + m * 4;
			ok = uint64_t(t[2]) + uint64_t(t[1]) * t[3] <= h.words && t[3] >= (t[0] + 31) / 32 && t[0] != 0 && t[1] != 0;
		}
		if (!ok)
		{
			error = "the file is damaged";
			return false;
		}

		// Every placement's triangles into the world (row vectors: corner x m + t, in double as the bake's check does);
		// what isn't finite is left out, as the bake leaves it out
		std::vector<uint32_t> soup;
		soup.reserve(size_t(h.expanded) * 12);
		for (uint32_t i = 0; i < h.placements; ++i)
		{
			const uint8_t *rec = placements + size_t(i) * 56;
			uint32_t model, flags;
			float mt[12];
			std::memcpy(&model, rec, 4);
			std::memcpy(&flags, rec + 4, 4);
			std::memcpy(mt, rec + 8, sizeof(mt));
			const bool tested = (flags & 1) != 0;
			const uint32_t first = ranges[model * 2], count = ranges[model * 2 + 1];
			for (uint32_t k = 0; k < count; ++k)
			{
				const uint32_t *src = model_tris + size_t(first + k) * 12;
				uint32_t dst[12];
				bool finite = true;
				for (int c = 0; c < 3; ++c)
				{
					float v[3];
					std::memcpy(v, src + c * 4, sizeof(v));
					for (int a = 0; a < 3; ++a)
					{
						const double w = double(v[0]) * mt[a] + double(v[1]) * mt[3 + a] + double(v[2]) * mt[6 + a] + mt[9 + a];
						const float wf = static_cast<float>(w);
						finite = finite && std::isfinite(wf);
						std::memcpy(dst + c * 4 + a, &wf, 4);
					}
					dst[c * 4 + 3] = tested ? src[c * 4 + 3] : 0u;
				}
				if (finite)
					soup.insert(soup.end(), dst, dst + 12);
			}
		}

		// Into square cells by each triangle's centre, cells in rows (z, then x), each cell's triangles in the order
		// above: what SHD1 holds
		const size_t n = soup.size() / 12;
		std::vector<int64_t> key(n);
		for (size_t i = 0; i < n; ++i)
		{
			float c[9];
			std::memcpy(c, soup.data() + i * 12, 12);
			std::memcpy(c + 3, soup.data() + i * 12 + 4, 12);
			std::memcpy(c + 6, soup.data() + i * 12 + 8, 12);
			const float cx = (c[0] + c[3] + c[6]) / 3.0f, cz = (c[2] + c[5] + c[8]) / 3.0f;
			const int32_t ix = static_cast<int32_t>(std::floor(cx / h.cell_size)), iz = static_cast<int32_t>(std::floor(cz / h.cell_size));
			key[i] = (int64_t(iz) << 32) | int64_t(uint32_t(ix) ^ 0x80000000u); // (sorts by z, then x, both signed)
		}
		std::vector<int64_t> cells(key);
		std::sort(cells.begin(), cells.end());
		cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
		std::vector<uint32_t> at(cells.size() + 1, 0), cell_of(n);
		for (size_t i = 0; i < n; ++i)
		{
			cell_of[i] = static_cast<uint32_t>(std::lower_bound(cells.begin(), cells.end(), key[i]) - cells.begin());
			++at[cell_of[i] + 1];
		}
		for (size_t c = 0; c < cells.size(); ++c)
			at[c + 1] += at[c];
		out = {};
		out.zone = h.zone;
		out.cell_size = h.cell_size;
		out.cells.resize(cells.size());
		for (size_t c = 0; c < cells.size(); ++c)
			out.cells[c] = { int32_t(uint32_t(cells[c]) ^ 0x80000000u), int32_t(cells[c] >> 32), at[c], at[c + 1] - at[c] };
		out.triangles.resize(soup.size());
		std::vector<uint32_t> next(at.begin(), at.end() - 1);
		for (size_t i = 0; i < n; ++i)
			std::memcpy(out.triangles.data() + size_t(next[cell_of[i]]++) * 12, soup.data() + i * 12, 48);
		out.masks.assign(masks, masks + size_t(h.masks) * 4);
		out.words.assign(words, words + h.words);
		return true;
	}

	bool read_shadow_file(const wchar_t *path, shadow_file &out, std::string &error)
	{
		FILE *f = nullptr;
		if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr)
		{
			error = "no file for this zone";
			return false;
		}
		struct header
		{
			char magic[4];
			uint32_t zone;
			float cell_size;
			uint32_t cells, triangles, masks, words, reserved;
		} h = {};
		bool ok = fread(&h, sizeof(h), 1, f) == 1;
		if (ok && std::memcmp(h.magic, "SHD2", 4) == 0)
			return read_compact_shadow(f, &h, out, error);
		ok = ok && std::memcmp(h.magic, "SHD1", 4) == 0 && h.cells < 1000000 &&
			h.triangles < 8000000 && h.masks < 256 && h.words < 16000000 && h.cell_size > 0.0f;
		if (ok)
		{
			out.zone = h.zone;
			out.cell_size = h.cell_size;
			out.cells.resize(h.cells);
			out.triangles.resize(size_t(h.triangles) * 12);
			out.masks.resize(size_t(h.masks) * 4);
			out.words.resize(h.words);
			ok = (h.cells == 0 || fread(out.cells.data(), sizeof(blade_file::cell), h.cells, f) == h.cells) &&
				(h.triangles == 0 || fread(out.triangles.data(), 48, h.triangles, f) == h.triangles) &&
				(h.masks == 0 || fread(out.masks.data(), 16, h.masks, f) == h.masks) &&
				(h.words == 0 || fread(out.words.data(), 4, h.words, f) == h.words);
		}
		fclose(f);
		if (ok)
			for (const blade_file::cell &c : out.cells)
				if (uint64_t(c.first) + c.count > out.triangle_count())
					ok = false;
		if (ok)
			for (size_t m = 0; m < out.masks.size(); m += 4) // (each mask's bits within the words)
				if (uint64_t(out.masks[m + 2]) + uint64_t(out.masks[m + 1]) * out.masks[m + 3] > out.words.size() ||
					out.masks[m + 3] < (out.masks[m] + 31) / 32 || out.masks[m] == 0 || out.masks[m + 1] == 0)
					ok = false;
		if (ok)
			for (size_t i = 0; i < out.triangles.size(); i += 4) // (no corner names a mask that isn't there)
				if ((out.triangles[i + 3] >> 24) > h.masks)
					ok = false;
		if (!ok)
		{
			error = "the file is damaged";
			out = {};
		}
		return ok;
	}

	// list_state

	void list_state::clear()
	{
		*this = list_state();
	}

	void list_state::set_root(ID3D12RootSignature *signature)
	{
		// Setting the same root signature again keeps the arguments; another one makes them stale
		if (signature != root)
			args.clear();
		root = signature;
	}

	root_arg &list_state::arg(uint32_t param)
	{
		if (args.size() <= param)
			args.resize(param + 1);
		return args[param];
	}

	void list_state::set_constants(uint32_t param, uint32_t first, uint32_t count, const uint32_t *values)
	{
		root_arg &a = arg(param);
		if (a.kind != root_arg::root_constants)
		{
			a = root_arg();
			a.kind = root_arg::root_constants;
		}
		if (a.constants.size() < first + count)
		{
			a.constants.resize(first + count);
			a.constant_set.resize(first + count);
		}
		for (uint32_t i = 0; i < count; ++i)
		{
			a.constants[first + i] = values[i];
			a.constant_set[first + i] = 1;
		}
	}

	void list_state::restore(ID3D12GraphicsCommandList *list) const
	{
		list->OMSetRenderTargets(rtv_count, rtv_count != 0 ? rtvs : nullptr, FALSE, dsv.ptr != 0 ? &dsv : nullptr);
		if (pipeline != nullptr)
			list->SetPipelineState(pipeline);
		if (topology != D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)
			list->IASetPrimitiveTopology(topology);
		if (viewport_count != 0)
			list->RSSetViewports(viewport_count, viewports);
		if (scissor_count != 0)
			list->RSSetScissorRects(scissor_count, scissors);
		if (root == nullptr)
			return;
		list->SetGraphicsRootSignature(root);
		for (UINT i = 0; i < static_cast<UINT>(args.size()); ++i)
		{
			const root_arg &a = args[i];
			switch (a.kind)
			{
			case root_arg::table:
				list->SetGraphicsRootDescriptorTable(i, D3D12_GPU_DESCRIPTOR_HANDLE { a.value });
				break;
			case root_arg::cbv:
				list->SetGraphicsRootConstantBufferView(i, a.value);
				break;
			case root_arg::srv:
				list->SetGraphicsRootShaderResourceView(i, a.value);
				break;
			case root_arg::uav:
				list->SetGraphicsRootUnorderedAccessView(i, a.value);
				break;
			case root_arg::root_constants:
				for (UINT k = 0; k < static_cast<UINT>(a.constants.size());)
				{
					if (!a.constant_set[k])
					{
						++k;
						continue;
					}
					UINT n = 1;
					while (k + n < a.constants.size() && a.constant_set[k + n])
						++n;
					list->SetGraphicsRoot32BitConstants(i, n, &a.constants[k], k);
					k += n;
				}
				break;
			default:
				break;
			}
		}
	}

	// renderer

	renderer::~renderer()
	{
		unload();
		unload_water();
		unload_sea();
		unload_wet();
		unload_rain();
		release(_ground.buffer);
		release(_zeros);
		release(_blade_depth);
		release(_dsv_heap);
		release(_rtv_heap);
		release(_depth.buffer);
		release(_sun_mask);
		release(_sun_pso);
		release(_vis_pso);
		release(_vis_points);
		release(_vis_read);
		if (_vis_boxes != nullptr && _vis_box_data != nullptr)
			_vis_boxes->Unmap(0, nullptr);
		_vis_box_data = nullptr;
		release(_vis_boxes);
		unload_shadow();
		release(_sun_map);
		release(_sun_map_heap);
		release(_sun_map_copy);
		release(_shadow_pso);
		release(_queries);
		release(_times);
		for (ID3D12Resource *&a : _actors)
			release(a);
		release(_water_params);
		for (old_buffer &o : _old)
			release(o.buffer);
		_old.clear();
		for (pso_entry &e : _psos)
			release(e.pso);
		_psos.clear();
		for (pso_entry &e : _shade_psos)
			release(e.pso);
		_shade_psos.clear();
		for (pso_entry &e : _water_psos)
			release(e.pso);
		_water_psos.clear();
		for (pso_entry &e : _water_debug_psos)
			release(e.pso);
		_water_debug_psos.clear();
		for (std::vector<pso_entry> *v : { &_water_whole_psos, &_sea_psos, &_water_fading_psos })
		{
			for (pso_entry &e : *v)
				release(e.pso);
			v->clear();
		}
		for (pso_entry &e : _firefly_psos)
			release(e.pso);
		_firefly_psos.clear();
		for (pso_entry &e : _rain_psos)
			release(e.pso);
		_rain_psos.clear();
		for (pso_entry &e : _splash_psos)
			release(e.pso);
		_splash_psos.clear();
		for (pso_entry &e : _snow_psos)
			release(e.pso);
		_snow_psos.clear();
		for (pso_entry &e : _leaf_psos)
			release(e.pso);
		_leaf_psos.clear();
		for (pso_entry &e : _air_psos)
			release(e.pso);
		_air_psos.clear();
		for (pso_entry &e : _flash_psos)
			release(e.pso);
		_flash_psos.clear();
		release(_root);
	}

	uint64_t shader_key(const std::string &source)
	{
		const uint64_t identity = compiler().identity;
		if (identity == 0)
			return 0;
		uint64_t h = fnv(&kShaderCacheVersion, sizeof(kShaderCacheVersion));
		h = fnv(&identity, sizeof(identity), h);
		h = fnv(&kCompileFlags, sizeof(kCompileFlags), h);
		for (const shader_entry &e : kShaderEntries)
		{
			h = fnv(e.entry, std::strlen(e.entry) + 1, h);
			h = fnv(e.target, std::strlen(e.target) + 1, h);
		}
		return fnv(source.data(), source.size(), h);
	}

	bool compile_shaders(const std::string &source, const std::wstring &cache_folder, shader_code &out, bool &from_cache, std::string &error)
	{
		from_cache = false;
		const uint64_t key = shader_key(source);
		const bool cached = !cache_folder.empty() && key != 0;
		if (cached && read_shader_cache(shader_cache_file(cache_folder, key), key, out))
		{
			touch(shader_cache_file(cache_folder, key));
			from_cache = true;
			return true;
		}
		shader_code code;
		code.key = key;
		code.blobs.resize(kShaderEntryCount);
		for (size_t i = 0; i < kShaderEntryCount; ++i)
			if (!compile(source, kShaderEntries[i].entry, kShaderEntries[i].target, code.blobs[i], error))
				return false;
		if (cached)
			write_shader_cache(cache_folder, code);
		out = std::move(code);
		return true;
	}

	const std::string &built_in_shader_source()
	{
		static const std::string source(kShader);
		return source;
	}

	bool renderer::init(ID3D12Device *device, std::string &error, const std::wstring &shader_cache)
	{
		if (_root != nullptr)
			return true;
		_device = device;
		{
			const auto start = std::chrono::steady_clock::now();
			shader_code code;
			if (!compile_shaders(built_in_shader_source(), shader_cache, code, _shaders_from_cache, error))
				return false;
			set_shader_code(std::move(code));
			_shader_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		}

		HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
		if (d3d12 == nullptr)
			d3d12 = LoadLibraryW(L"d3d12.dll");
		const auto serialize = d3d12 != nullptr ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
		if (serialize == nullptr)
		{
			error = "D3D12SerializeRootSignature not found";
			return false;
		}

		D3D12_ROOT_PARAMETER params[7] = {};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.ShaderRegister = 0;
		params[0].Constants.Num32BitValues = kConstants;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[1].Descriptor.ShaderRegister = 0;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // (the sun pass reads the shadow map's copy there)
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[2].Descriptor.ShaderRegister = 1;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // (and the shading pass's pixels)
		params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
		params[3].Descriptor.ShaderRegister = 0;
		params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		D3D12_ROOT_SIGNATURE_DESC desc = {};
		params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[4].Descriptor.ShaderRegister = 2;
		params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // (the water's parameters, in its pixels)
		params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[5].Descriptor.ShaderRegister = 3;
		params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
		params[6].Descriptor.ShaderRegister = 1;
		params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; // (the sun's shadows, in the shading passes)
		desc.NumParameters = 7;
		desc.pParameters = params;
		desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

		ID3DBlob *blob = nullptr, *messages = nullptr;
		HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &messages);
		if (SUCCEEDED(hr))
			hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&_root));
		if (SUCCEEDED(hr))
			_root->SetName(L"worldfx: root signature");
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "root signature failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
		}
		release(blob);
		release(messages);
		if (FAILED(hr))
			return false;

		// GPU time: a timestamp at each section's marker and one at the end (kTimingMarks at most a frame), four frames
		// in turn
		D3D12_QUERY_HEAP_DESC qd = {};
		qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		qd.Count = kTimingMarks * kTimingSlots;
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC desc_rb = {};
		desc_rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc_rb.Width = 8 * kTimingMarks * kTimingSlots;
		desc_rb.Height = 1;
		desc_rb.DepthOrArraySize = 1;
		desc_rb.MipLevels = 1;
		desc_rb.SampleDesc.Count = 1;
		desc_rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&_queries))) ||
			FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc_rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_times))))
		{
			release(_queries);
			release(_times); // (no timing: everything else works without it)
		}
		else
		{
			_queries->SetName(L"worldfx: timestamps");
			_times->SetName(L"worldfx: timestamps read back");
		}
		return true;
	}

	// A section starting (see grass_render.hpp): the GPU crash report's marker, and a timestamp (the last place kept for
	// the end's)
	void renderer::section(ID3D12GraphicsCommandList *list, const wchar_t *name)
	{
		gpu_crash::mark(list, name);
		if (_queries == nullptr || _times == nullptr)
			return;
		UINT &n = _mark_count[_time_slot];
		if (n + 1 >= kTimingMarks)
			return;
		_mark_names[_time_slot][n] = name;
		list->EndQuery(_queries, D3D12_QUERY_TYPE_TIMESTAMP, _time_slot * kTimingMarks + n);
		++n;
	}

	// The GPU time of a frame's work (ms), from timestamps resolved a few frames ago (smoothed): all of it split at
	// where the copies and shading part ended (no shading: all of it was), and each section's
	void renderer::finish_timing(ID3D12GraphicsCommandList *list, bool shaded)
	{
		(void)shaded; // (where the shading part ended is kept by draw: _shade_end)
		if (_queries == nullptr || _times == nullptr)
			return;
		const UINT slot = _time_slot, n = _mark_count[slot];
		if (n == 0)
			return;
		_mark_names[slot][n] = nullptr;
		list->EndQuery(_queries, D3D12_QUERY_TYPE_TIMESTAMP, slot * kTimingMarks + n);
		_mark_count[slot] = n + 1;
		list->ResolveQueryData(_queries, D3D12_QUERY_TYPE_TIMESTAMP, slot * kTimingMarks, n + 1, _times, UINT64(slot) * kTimingMarks * 8);
		_time_slot = (_time_slot + 1) % kTimingSlots;
		// (the oldest slot: written kTimingSlots - 1 frames ago, done by now as a rule)
		if (_frequency == 0 || ++_timed < kTimingSlots)
			return;
		const UINT old = _time_slot, m = _mark_count[old];
		if (m < 2)
			return;
		const D3D12_RANGE range = { SIZE_T(old) * kTimingMarks * 8, (SIZE_T(old) * kTimingMarks + m) * 8 };
		void *mapped = nullptr;
		if (FAILED(_times->Map(0, &range, &mapped)))
			return;
		const uint64_t *t = reinterpret_cast<const uint64_t *>(static_cast<const uint8_t *>(mapped) + range.Begin);
		bool in_order = t[m - 1] - t[0] < _frequency;
		for (UINT i = 1; i < m && in_order; ++i)
			in_order = t[i] >= t[i - 1];
		if (in_order)
		{
			const double to_ms = 1000.0 / double(_frequency);
			const UINT split = std::min(_shade_end[old], m - 1);
			const float shading = float(double(t[split] - t[0]) * to_ms);
			const float grass = float(double(t[m - 1] - t[split]) * to_ms);
			_shading_ms = _shading_ms * 0.9f + shading * 0.1f;
			_last_shading_ms = shading;
			_grass_ms = _grass_ms * 0.9f + grass * 0.1f;
			_frames_timed++;
			for (UINT i = 0; i + 1 < m; ++i)
			{
				const wchar_t *const name = _mark_names[old][i];
				const float ms = float(double(t[i + 1] - t[i]) * to_ms);
				gpu_section *sec = nullptr;
				for (gpu_section &g : _sections)
					if (g.name == name)
						sec = &g;
				if (sec == nullptr)
				{
					_sections.push_back({ name, ms, 0 });
					sec = &_sections.back();
				}
				sec->ms = sec->frames == 0 ? ms : sec->ms * 0.95f + ms * 0.05f;
				sec->frames++;
			}
		}
		const D3D12_RANGE none = { 0, 0 };
		_times->Unmap(0, &none);
	}

	ID3D12PipelineState *renderer::pipeline_for(const target &t, std::string &error)
	{
		const D3D12_RESOURCE_DESC rt_desc = t.rt->GetDesc();
		for (const pso_entry &e : _psos)
			if (e.rtv == t.rtv_format && same_samples(e.samples, rt_desc.SampleDesc))
				return e.pso;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = _root;
		desc.VS = { _vs.data(), _vs.size() };
		desc.PS = { _ps.data(), _ps.size() };
		desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE; // the world's alpha is left alone
		desc.SampleMask = UINT_MAX;
		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.RasterizerState.MultisampleEnable = rt_desc.SampleDesc.Count > 1;
		desc.DepthStencilState.DepthEnable = TRUE;
		desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = t.rtv_format;
		desc.DSVFormat = kBladeDepthFormat; // (the blades' own depth buffer)
		desc.SampleDesc = rt_desc.SampleDesc;

		ID3D12PipelineState *pso = nullptr;
		const HRESULT hr = _device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
		if (FAILED(hr))
		{
			char text[96];
			std::snprintf(text, sizeof(text), "pipeline failed (0x%08lX, format %u, %u samples)",
				static_cast<unsigned long>(hr), static_cast<unsigned>(t.rtv_format), rt_desc.SampleDesc.Count);
			error = text;
			return nullptr;
		}
		pso->SetName(L"worldfx: grass pipeline");
		_psos.push_back({ t.rtv_format, kBladeDepthFormat, rt_desc.SampleDesc, pso });
		return pso;
	}

	// Compiles the shaders from source; on success they replace the ones in use (the pipelines built from
	// those are released a few frames later: lists recorded with them may still be running)
	bool renderer::set_shader_source(const std::string &source, std::string &error)
	{
		shader_code code;
		bool from_cache = false;
		if (!compile_shaders(source, std::wstring(), code, from_cache, error))
			return false;
		set_shader_code(std::move(code));
		return true;
	}

	void renderer::set_shader_code(shader_code &&code)
	{
		std::vector<uint8_t> *const into[kShaderEntryCount] = { &_vs, &_ps, &_shade_vs, &_shade_ps, &_sun_ps, &_shadow_vs, &_shadow_ps,
			&_water_vs, &_water_ps, &_water_debug_vs, &_water_debug_ps, &_firefly_vs, &_firefly_ps, &_rain_vs, &_rain_ps,
			&_splash_vs, &_splash_ps, &_snow_vs, &_snow_ps, &_vis_ps, &_leaf_vs, &_leaf_ps, &_air_ps, &_flash_ps };
		if (code.blobs.size() != kShaderEntryCount)
			return;
		for (size_t i = 0; i < kShaderEntryCount; ++i)
			into[i]->swap(code.blobs[i]);
		_shader_key = code.key;
		if (_sun_pso != nullptr)
			_old.push_back({ _sun_pso, 0 });
		_sun_pso = nullptr;
		if (_vis_pso != nullptr)
			_old.push_back({ _vis_pso, 0 });
		_vis_pso = nullptr;
		if (_shadow_pso != nullptr)
			_old.push_back({ _shadow_pso, 0 });
		_shadow_pso = nullptr;
		for (cascade &c : _cascades)
			c.forget(); // (drawn again with the new shaders)
		for (pso_entry &e : _psos)
			_old.push_back({ e.pso, 0 });
		for (pso_entry &e : _shade_psos)
			_old.push_back({ e.pso, 0 });
		for (pso_entry &e : _water_psos)
			_old.push_back({ e.pso, 0 });
		for (pso_entry &e : _water_debug_psos)
			_old.push_back({ e.pso, 0 });
		for (std::vector<pso_entry> *v : { &_water_whole_psos, &_sea_psos, &_water_fading_psos })
		{
			for (pso_entry &e : *v)
				_old.push_back({ e.pso, 0 });
			v->clear();
		}
		for (pso_entry &e : _firefly_psos)
			_old.push_back({ e.pso, 0 });
		for (pso_entry &e : _rain_psos)
			_old.push_back({ e.pso, 0 });
		for (pso_entry &e : _splash_psos)
			_old.push_back({ e.pso, 0 });
		for (pso_entry &e : _snow_psos)
			_old.push_back({ e.pso, 0 });
		for (pso_entry &e : _leaf_psos)
			_old.push_back({ e.pso, 0 });
		for (pso_entry &e : _air_psos)
			_old.push_back({ e.pso, 0 });
		for (pso_entry &e : _flash_psos)
			_old.push_back({ e.pso, 0 });
		_leaf_psos.clear();
		_air_psos.clear();
		_flash_psos.clear();
		_psos.clear();
		_shade_psos.clear();
		_water_psos.clear();
		_water_debug_psos.clear();
		_firefly_psos.clear();
		_rain_psos.clear();
		_splash_psos.clear();
		_snow_psos.clear();
	}

	ID3D12PipelineState *renderer::shade_pipeline_for(const target &t, std::string &error)
	{
		const D3D12_RESOURCE_DESC rt_desc = t.rt->GetDesc();
		for (const pso_entry &e : _shade_psos)
			if (e.rtv == t.rtv_format && same_samples(e.samples, rt_desc.SampleDesc))
				return e.pso;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = _root;
		desc.VS = { _shade_vs.data(), _shade_vs.size() };
		desc.PS = { _shade_ps.data(), _shade_ps.size() };
		desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
		desc.SampleMask = UINT_MAX;
		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = t.rtv_format;
		desc.SampleDesc = rt_desc.SampleDesc;
		ID3D12PipelineState *pso = nullptr;
		const HRESULT hr = _device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
		if (FAILED(hr))
		{
			char text[96];
			std::snprintf(text, sizeof(text), "shading pipeline failed (0x%08lX, format %u)", static_cast<unsigned long>(hr), static_cast<unsigned>(t.rtv_format));
			error = text;
			return nullptr;
		}
		pso->SetName(L"worldfx: shading pipeline");
		_shade_psos.push_back({ t.rtv_format, DXGI_FORMAT_UNKNOWN, rt_desc.SampleDesc, pso });
		return pso;
	}

	// The sun's shadows at half size: a full-screen triangle with no render target (its pixels write the mask)
	ID3D12PipelineState *renderer::sun_pipeline(std::string &error)
	{
		if (_sun_pso != nullptr)
			return _sun_pso;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = _root;
		desc.VS = { _shade_vs.data(), _shade_vs.size() };
		desc.PS = { _sun_ps.data(), _sun_ps.size() };
		desc.SampleMask = UINT_MAX;
		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 0;
		desc.SampleDesc.Count = 1;
		const HRESULT hr = _device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_sun_pso));
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "sun shadow pipeline failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			_sun_pso = nullptr;
			return nullptr;
		}
		_sun_pso->SetName(L"worldfx: sun shadow pipeline");
		return _sun_pso;
	}

	// The mask the sun's shadows go into: a uint for each pixel of the world at half its width and height
	// (rounded up), made again when the world's size grows. Buffers start in COMMON and go back to it after
	// each list: the passes use it as a UAV straight away (a buffer is promoted to that by itself).
	bool renderer::sun_mask_for(const D3D12_RESOURCE_DESC &rt_desc, std::string &error)
	{
		const UINT64 size = ((rt_desc.Width + 1) / 2) * ((UINT64(rt_desc.Height) + 1) / 2) * 4;
		if (_sun_mask != nullptr && _sun_mask_size >= size)
			return true;
		if (_sun_mask != nullptr)
			_old.push_back({ _sun_mask, 0 }); // (a list recorded this frame may still use it)
		_sun_mask = nullptr;
		_sun_mask_size = 0;
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		const HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&_sun_mask));
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "sun shadow mask failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			_sun_mask = nullptr;
			return false;
		}
		_sun_mask->SetName(L"worldfx: sun shadows");
		_sun_mask_size = size;
		return true;
	}

	// The game's visibility tests: the boxes' slots (upload heap, mapped once), their points (UAV) and the slots'
	// copies of them (readback heap), made the first time boxes come
	bool renderer::vis_buffers(std::string &error)
	{
		if (_vis_boxes != nullptr && _vis_points != nullptr && _vis_read != nullptr)
			return true;
		const UINT64 box_slot = UINT64(vistest::kMaxBoxes) * sizeof(vistest::box);
		const UINT64 points = UINT64(vistest::kMaxBoxes) * vistest::kPoints * 4;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		D3D12_HEAP_PROPERTIES heap = {};
		HRESULT hr = S_OK;
		if (_vis_boxes == nullptr)
		{
			heap.Type = D3D12_HEAP_TYPE_UPLOAD;
			desc.Width = box_slot * kVisSlots;
			desc.Flags = D3D12_RESOURCE_FLAG_NONE;
			hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_vis_boxes));
			void *mapped = nullptr;
			const D3D12_RANGE none = { 0, 0 };
			if (SUCCEEDED(hr))
				hr = _vis_boxes->Map(0, &none, &mapped);
			_vis_box_data = SUCCEEDED(hr) ? static_cast<uint8_t *>(mapped) : nullptr;
			if (SUCCEEDED(hr))
				_vis_boxes->SetName(L"worldfx: visibility test boxes");
		}
		if (SUCCEEDED(hr) && _vis_points == nullptr)
		{
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			desc.Width = points;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&_vis_points));
			if (SUCCEEDED(hr))
				_vis_points->SetName(L"worldfx: visibility test points");
		}
		if (SUCCEEDED(hr) && _vis_read == nullptr)
		{
			heap.Type = D3D12_HEAP_TYPE_READBACK;
			desc.Width = points * kVisSlots;
			desc.Flags = D3D12_RESOURCE_FLAG_NONE;
			hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_vis_read));
			if (SUCCEEDED(hr))
				_vis_read->SetName(L"worldfx: visibility test points read back");
		}
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "visibility test buffers failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			if (_vis_boxes != nullptr && _vis_box_data != nullptr)
				_vis_boxes->Unmap(0, nullptr);
			_vis_box_data = nullptr;
			release(_vis_boxes);
			release(_vis_points);
			release(_vis_read);
			return false;
		}
		return true;
	}

	bool renderer::set_vis_boxes(const uint32_t *objects, const vistest::box *boxes, uint32_t n, uint64_t stamp, std::string &error)
	{
		n = std::min<uint32_t>(n, vistest::kMaxBoxes);
		if (n == 0 || _device == nullptr || !vis_buffers(error))
			return false;
		// A slot set and not drawn yet is set again; else an empty one. None: the GPU has them all still.
		vis_slot *slot = nullptr;
		for (vis_slot &s : _vis_slots)
			if (s.state == vis_slot::state_t::set)
				slot = &s;
		for (vis_slot &s : _vis_slots)
			if (slot == nullptr && s.state == vis_slot::state_t::empty)
				slot = &s;
		if (slot == nullptr)
			return false;
		const size_t i = size_t(slot - _vis_slots);
		std::memcpy(_vis_box_data + i * vistest::kMaxBoxes * sizeof(vistest::box), boxes, n * sizeof(vistest::box));
		std::memcpy(slot->objects, objects, n * sizeof(uint32_t));
		slot->n = n;
		slot->stamp = stamp;
		slot->state = vis_slot::state_t::set;
		return true;
	}

	uint32_t renderer::read_vis(uint64_t done, uint32_t objects[vistest::kMaxBoxes], uint32_t points[vistest::kMaxBoxes])
	{
		vis_slot *oldest = nullptr;
		for (vis_slot &s : _vis_slots)
			if (s.state == vis_slot::state_t::recorded && s.stamp + 2 <= done && (oldest == nullptr || s.stamp < oldest->stamp))
				oldest = &s;
		if (oldest == nullptr || _vis_read == nullptr)
			return 0;
		const size_t i = size_t(oldest - _vis_slots);
		const SIZE_T slot_bytes = SIZE_T(vistest::kMaxBoxes) * vistest::kPoints * 4;
		const D3D12_RANGE range = { i * slot_bytes, i * slot_bytes + SIZE_T(oldest->n) * vistest::kPoints * 4 };
		void *mapped = nullptr;
		const uint32_t n = oldest->n;
		oldest->state = vis_slot::state_t::empty;
		if (FAILED(_vis_read->Map(0, &range, &mapped)))
			return 0;
		const uint32_t *p = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(mapped) + range.Begin);
		for (uint32_t b = 0; b < n; ++b)
		{
			uint32_t passed = 0;
			for (uint32_t k = 0; k < vistest::kPoints; ++k)
				passed += p[b * vistest::kPoints + k] != 0 ? 1u : 0u;
			objects[b] = oldest->objects[b];
			points[b] = passed;
		}
		const D3D12_RANGE none = { 0, 0 };
		_vis_read->Unmap(0, &none);
		return n;
	}

	// The visibility tests' pipeline: no render target, a pixel a point of a box writing whether it passed
	ID3D12PipelineState *renderer::vis_pipeline(std::string &error)
	{
		if (_vis_pso != nullptr)
			return _vis_pso;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = _root;
		desc.VS = { _shade_vs.data(), _shade_vs.size() };
		desc.PS = { _vis_ps.data(), _vis_ps.size() };
		desc.SampleMask = UINT_MAX;
		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 0;
		desc.SampleDesc.Count = 1;
		const HRESULT hr = _device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_vis_pso));
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "visibility test pipeline failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			_vis_pso = nullptr;
			return nullptr;
		}
		_vis_pso->SetName(L"worldfx: visibility test pipeline");
		return _vis_pso;
	}

	// The boxes set for this frame (set_vis_boxes), tested against the depth copy: a pixel a point (kPoints wide, a row
	// a box), then the points copied to the slot's part of the readback buffer. The root signature, constants and depth
	// copy (t3) are as the caller bound them. Returns the boxes tested.
	uint32_t renderer::draw_vis(ID3D12GraphicsCommandList *list, const float *constants, std::string &error)
	{
		vis_slot *slot = nullptr;
		for (vis_slot &s : _vis_slots)
			if (s.state == vis_slot::state_t::set)
				slot = &s;
		if (slot == nullptr || slot->n == 0)
			return 0;
		ID3D12PipelineState *const pso = vis_pipeline(error);
		if (pso == nullptr)
			return 0;
		const size_t i = size_t(slot - _vis_slots);
		section(list, L"worldfx: visibility tests");
		list->SetGraphicsRootSignature(_root);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->SetGraphicsRoot32BitConstants(0, kConstants, constants, 0);
		list->SetGraphicsRootShaderResourceView(1, _depth.buffer->GetGPUVirtualAddress());
		list->SetGraphicsRootShaderResourceView(2, _depth.buffer->GetGPUVirtualAddress());
		list->SetGraphicsRootShaderResourceView(4, _vis_boxes->GetGPUVirtualAddress() + i * vistest::kMaxBoxes * sizeof(vistest::box));
		list->SetGraphicsRootShaderResourceView(5, _depth.buffer->GetGPUVirtualAddress());
		list->SetGraphicsRootUnorderedAccessView(3, _vis_points->GetGPUVirtualAddress());
		list->SetGraphicsRootUnorderedAccessView(6, _vis_points->GetGPUVirtualAddress());
		const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, float(vistest::kPoints), float(slot->n), 0.0f, 1.0f };
		const D3D12_RECT scissor = { 0, 0, LONG(vistest::kPoints), LONG(slot->n) };
		list->SetPipelineState(pso);
		list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
		list->RSSetViewports(1, &viewport);
		list->RSSetScissorRects(1, &scissor);
		list->DrawInstanced(3, 1, 0, 0);
		// (the points: a UAV now, promoted from COMMON; to the copy's source, then their slot's part of the readback)
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = _vis_points;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		list->ResourceBarrier(1, &b);
		const UINT64 slot_bytes = UINT64(vistest::kMaxBoxes) * vistest::kPoints * 4;
		list->CopyBufferRegion(_vis_read, i * slot_bytes, _vis_points, 0, UINT64(slot->n) * vistest::kPoints * 4);
		std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
		list->ResourceBarrier(1, &b);
		slot->state = vis_slot::state_t::recorded;
		return slot->n;
	}

	// The water: written whole (its pixels work out what's under them from the copy of the world), with the
	// blades' depth buffer so the nearest water is the one kept. The debug view: the collision drawn over the
	// world with alpha, no depth buffer (the pixel shader tests against the depth copy). The world's alpha is
	// left alone.
	// Fireflies: added to the world's colour (its alpha left alone), behind the blades hidden (their depth buffer,
	// not written)
	ID3D12PipelineState *renderer::firefly_pipeline_for(const target &t, std::string &error)
	{
		const D3D12_RESOURCE_DESC rt_desc = t.rt->GetDesc();
		for (const pso_entry &e : _firefly_psos)
			if (e.rtv == t.rtv_format && same_samples(e.samples, rt_desc.SampleDesc))
				return e.pso;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = _root;
		desc.VS = { _firefly_vs.data(), _firefly_vs.size() };
		desc.PS = { _firefly_ps.data(), _firefly_ps.size() };
		D3D12_RENDER_TARGET_BLEND_DESC &b = desc.BlendState.RenderTarget[0];
		b.BlendEnable = TRUE;
		b.SrcBlend = D3D12_BLEND_ONE;
		b.DestBlend = D3D12_BLEND_ONE;
		b.BlendOp = D3D12_BLEND_OP_ADD;
		b.SrcBlendAlpha = D3D12_BLEND_ZERO;
		b.DestBlendAlpha = D3D12_BLEND_ONE;
		b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
		desc.SampleMask = UINT_MAX;
		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.RasterizerState.MultisampleEnable = rt_desc.SampleDesc.Count > 1;
		desc.DepthStencilState.DepthEnable = TRUE;
		desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.DSVFormat = kBladeDepthFormat; // (the blades' depth buffer)
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = t.rtv_format;
		desc.SampleDesc = rt_desc.SampleDesc;
		ID3D12PipelineState *pso = nullptr;
		const HRESULT hr = _device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
		if (FAILED(hr))
		{
			char text[96];
			std::snprintf(text, sizeof(text), "firefly pipeline failed (0x%08lX, format %u)", static_cast<unsigned long>(hr), static_cast<unsigned>(t.rtv_format));
			error = text;
			return nullptr;
		}
		pso->SetName(L"worldfx: firefly pipeline");
		_firefly_psos.push_back({ t.rtv_format, kBladeDepthFormat, rt_desc.SampleDesc, pso });
		return pso;
	}

	// Rain, its splashes and snow: blended over the world's colour (its alpha left alone), behind the blades hidden (their
	// depth buffer, not written; the pixel shaders test against the depth copy for the rest)
	ID3D12PipelineState *renderer::rain_pipeline_for(const target &t, fall kind, std::string &error)
	{
		const D3D12_RESOURCE_DESC rt_desc = t.rt->GetDesc();
		std::vector<pso_entry> &psos = kind == fall::splash ? _splash_psos : kind == fall::snow ? _snow_psos : kind == fall::leaf ? _leaf_psos : _rain_psos;
		for (const pso_entry &e : psos)
			if (e.rtv == t.rtv_format && same_samples(e.samples, rt_desc.SampleDesc))
				return e.pso;
		const std::vector<uint8_t> &vs = kind == fall::splash ? _splash_vs : kind == fall::snow ? _snow_vs : kind == fall::leaf ? _leaf_vs : _rain_vs;
		const std::vector<uint8_t> &ps = kind == fall::splash ? _splash_ps : kind == fall::snow ? _snow_ps : kind == fall::leaf ? _leaf_ps : _rain_ps;
		const char *const what = kind == fall::splash ? "splash" : kind == fall::snow ? "snow" : kind == fall::leaf ? "leaf" : "rain";
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = _root;
		desc.VS = { vs.data(), vs.size() };
		desc.PS = { ps.data(), ps.size() };
		D3D12_RENDER_TARGET_BLEND_DESC &b = desc.BlendState.RenderTarget[0];
		b.BlendEnable = TRUE;
		b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
		b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		b.BlendOp = D3D12_BLEND_OP_ADD;
		b.SrcBlendAlpha = D3D12_BLEND_ZERO;
		b.DestBlendAlpha = D3D12_BLEND_ONE;
		b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
		desc.SampleMask = UINT_MAX;
		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.RasterizerState.MultisampleEnable = rt_desc.SampleDesc.Count > 1;
		desc.DepthStencilState.DepthEnable = TRUE;
		desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.DSVFormat = kBladeDepthFormat; // (the blades' depth buffer)
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = t.rtv_format;
		desc.SampleDesc = rt_desc.SampleDesc;
		ID3D12PipelineState *pso = nullptr;
		const HRESULT hr = _device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
		if (FAILED(hr))
		{
			char text[96];
			std::snprintf(text, sizeof(text), "%s pipeline failed (0x%08lX, format %u)", what, static_cast<unsigned long>(hr), static_cast<unsigned>(t.rtv_format));
			error = text;
			return nullptr;
		}
		pso->SetName(kind == fall::splash ? L"worldfx: splash pipeline" : kind == fall::snow ? L"worldfx: snow pipeline" :
			kind == fall::leaf ? L"worldfx: leaf pipeline" : L"worldfx: rain pipeline");
		psos.push_back({ t.rtv_format, kBladeDepthFormat, rt_desc.SampleDesc, pso });
		return pso;
	}

	// The air pass (dust, sand and wind streaks, gloom's fog banks): a full-screen triangle over the world as everything has
	// been drawn into it, blended with two of its pixel's outputs (dual-source): the world times the second plus the first
	// (a fog's colour over what it hides). The lightning's flash (flash): the world plus the world times its pixel's
	// colour, up to twice as bright (the factors a pixel gives are kept to 0..1 on the world's UNORM target: the world
	// times more than 1 can't be had any other way). No depth: their pixels read the depth copy. The world's alpha is left
	// alone.
	ID3D12PipelineState *renderer::air_pipeline_for(const target &t, bool flash, std::string &error)
	{
		const D3D12_RESOURCE_DESC rt_desc = t.rt->GetDesc();
		std::vector<pso_entry> &psos = flash ? _flash_psos : _air_psos;
		for (const pso_entry &e : psos)
			if (e.rtv == t.rtv_format && same_samples(e.samples, rt_desc.SampleDesc))
				return e.pso;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = _root;
		desc.VS = { _shade_vs.data(), _shade_vs.size() };
		desc.PS = flash ? D3D12_SHADER_BYTECODE { _flash_ps.data(), _flash_ps.size() } : D3D12_SHADER_BYTECODE { _air_ps.data(), _air_ps.size() };
		D3D12_RENDER_TARGET_BLEND_DESC &b = desc.BlendState.RenderTarget[0];
		b.BlendEnable = TRUE;
		b.SrcBlend = flash ? D3D12_BLEND_DEST_COLOR : D3D12_BLEND_ONE;
		b.DestBlend = flash ? D3D12_BLEND_ONE : D3D12_BLEND_SRC1_COLOR;
		b.BlendOp = D3D12_BLEND_OP_ADD;
		b.SrcBlendAlpha = D3D12_BLEND_ZERO;
		b.DestBlendAlpha = D3D12_BLEND_ONE;
		b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
		desc.SampleMask = UINT_MAX;
		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = t.rtv_format;
		desc.SampleDesc = rt_desc.SampleDesc;
		ID3D12PipelineState *pso = nullptr;
		const HRESULT hr = _device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
		if (FAILED(hr))
		{
			char text[96];
			std::snprintf(text, sizeof(text), "%s pipeline failed (0x%08lX, format %u)", flash ? "flash" : "air", static_cast<unsigned long>(hr), static_cast<unsigned>(t.rtv_format));
			error = text;
			return nullptr;
		}
		pso->SetName(flash ? L"worldfx: flash pipeline" : L"worldfx: air pipeline");
		psos.push_back({ t.rtv_format, DXGI_FORMAT_UNKNOWN, rt_desc.SampleDesc, pso });
		return pso;
	}

	bool renderer::upload_rain(rain_file &&file, std::string &error)
	{
		unload_rain();
		if (file.width == 0 || file.height == 0 || file.q.size() != size_t(file.width) * file.height || file.kinds.size() != file.q.size())
		{
			error = "the rain map is empty";
			return false;
		}
		_rain = std::move(file);
		return true;
	}

	void renderer::unload_rain()
	{
		if (_rain_window != nullptr)
			_old.push_back({ _rain_window, 0 }); // (as the water)
		_rain_window = nullptr;
		_rain = rain_file();
	}

	bool renderer::rain_surface(float x, float z, float &y, uint8_t &kind) const
	{
		if (_rain.q.empty())
			return false;
		const float fi = std::floor((x - _rain.x0) / _rain.square), fj = std::floor((z - _rain.z0) / _rain.square);
		if (!(fi >= 0.0f && fj >= 0.0f && fi < float(_rain.width) && fj < float(_rain.height)))
			return false;
		const size_t k = size_t(fj) * _rain.width + size_t(fi);
		kind = _rain.kinds[k];
		y = _rain.y0 + float(_rain.q[k]) * _rain.step;
		return true;
	}

	// The rain map's window around the camera, written into a new upload buffer when the camera has moved toward its
	// edge (each square a uint: the first surface's y, float bits, with what it is in the low 2 bits; nothing known
	// past the map's edges). The old one goes like the blades' (a list recorded this frame may still read it).
	bool renderer::rain_window_for(const float eye[3], bool &moved, std::string &error)
	{
		moved = false;
		const float fi = std::floor((eye[0] - _rain.x0) / _rain.square), fj = std::floor((eye[2] - _rain.z0) / _rain.square);
		if (!std::isfinite(fi) || !std::isfinite(fj) || std::fabs(fi) > 1e6f || std::fabs(fj) > 1e6f)
			return false;
		const int32_t ci = static_cast<int32_t>(fi), cj = static_cast<int32_t>(fj);
		const auto snap = [](int32_t c) {
			const int32_t step = kRainWindowStep;
			return (c >= 0 ? c / step : -((-c + step - 1) / step)) * step - kRainWindowMargin - (kRainWindowStep / 2);
		};
		const int32_t wi = snap(ci), wj = snap(cj);
		if (_rain_window != nullptr && wi == _rain_window_i && wj == _rain_window_j)
			return true;
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = UINT64(kRainWindow) * kRainWindow * 4;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *buffer = nullptr;
		HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer));
		void *mapped = nullptr;
		const D3D12_RANGE none = { 0, 0 };
		if (SUCCEEDED(hr))
			hr = buffer->Map(0, &none, &mapped);
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "rain map buffer failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			release(buffer);
			return false;
		}
		uint32_t *o = static_cast<uint32_t *>(mapped);
		for (int32_t j = 0; j < kRainWindow; ++j)
			for (int32_t i = 0; i < kRainWindow; ++i)
			{
				const int32_t gi = wi + i, gj = wj + j;
				uint32_t v = 0; // (nothing known: drops fall on through)
				if (gi >= 0 && gj >= 0 && gi < int32_t(_rain.width) && gj < int32_t(_rain.height))
				{
					const size_t k = size_t(gj) * _rain.width + size_t(gi);
					const uint8_t kind = _rain.kinds[k];
					if (kind != kRainNothing)
					{
						const float y = _rain.y0 + float(_rain.q[k]) * _rain.step;
						std::memcpy(&v, &y, 4);
						v = (v & ~3u) | kind;
					}
				}
				o[j * kRainWindow + i] = v;
			}
		buffer->Unmap(0, nullptr);
		buffer->SetName(L"worldfx: rain map around the camera");
		if (_rain_window != nullptr)
			_old.push_back({ _rain_window, 0 });
		_rain_window = buffer;
		_rain_window_i = wi;
		_rain_window_j = wj;
		moved = true;
		return true;
	}

	// The water's pipelines. plain: the water (a zone without a sea), written whole (its pixels work out what's under them
	// from the copy of the world), with the blades' depth buffer so the nearest water is the one kept. With a sea, three:
	// whole, the baked water's pixels it draws at their whole (its pixel shader leaves the rest: blade.z 1), each marked in
	// the stencil as it's written (from the clear's 0 down, wrapping); sea, the sea's sheets only where the stencil isn't marked;
	// fading, the baked water's pixels it fades in at (its shore, round what stands in it, toward its distance: blade.z 2),
	// blended over what's there by then (the sea, or the game's own), its depth tested but not written. So the two never
	// stack, the baked water keeps its fades and flow, and where it fades out by the sea it fades into the sea, not into the
	// game's own. (The stencil reference is the game's, set or not: none is set here, so the game's isn't changed. The sea's
	// test passes wherever the stencil is 0 whatever the reference, and fails where it's marked unless that is 255, or
	// 254 for water drawn twice over itself, and so on.) debug: the
	// collision drawn over the world with alpha, no depth buffer (the pixel shader tests against the depth copy). The
	// world's alpha is left alone.
	ID3D12PipelineState *renderer::water_pipeline_for(const target &t, water_kind kind, std::string &error)
	{
		const D3D12_RESOURCE_DESC rt_desc = t.rt->GetDesc();
		const bool debug = kind == water_kind::debug;
		std::vector<pso_entry> &psos = kind == water_kind::debug ? _water_debug_psos : kind == water_kind::whole ? _water_whole_psos :
			kind == water_kind::sea ? _sea_psos : kind == water_kind::fading ? _water_fading_psos : _water_psos;
		for (const pso_entry &e : psos)
			if (e.rtv == t.rtv_format && same_samples(e.samples, rt_desc.SampleDesc))
				return e.pso;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = _root;
		desc.VS = debug ? D3D12_SHADER_BYTECODE { _water_debug_vs.data(), _water_debug_vs.size() } : D3D12_SHADER_BYTECODE { _water_vs.data(), _water_vs.size() };
		desc.PS = debug ? D3D12_SHADER_BYTECODE { _water_debug_ps.data(), _water_debug_ps.size() } : D3D12_SHADER_BYTECODE { _water_ps.data(), _water_ps.size() };
		D3D12_RENDER_TARGET_BLEND_DESC &b = desc.BlendState.RenderTarget[0];
		if (debug || kind == water_kind::fading)
		{
			b.BlendEnable = TRUE;
			b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
			b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			b.BlendOp = D3D12_BLEND_OP_ADD;
			b.SrcBlendAlpha = D3D12_BLEND_ZERO;
			b.DestBlendAlpha = D3D12_BLEND_ONE;
			b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		}
		if (!debug)
		{
			D3D12_DEPTH_STENCIL_DESC &ds = desc.DepthStencilState;
			ds.DepthEnable = TRUE;
			ds.DepthWriteMask = kind == water_kind::fading ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
			ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
			if (kind == water_kind::whole || kind == water_kind::sea)
			{
				ds.StencilEnable = TRUE;
				ds.StencilReadMask = 0xFF;
				ds.StencilWriteMask = kind == water_kind::whole ? 0xFF : 0x00;
				D3D12_DEPTH_STENCILOP_DESC op = {};
				op.StencilFailOp = D3D12_STENCIL_OP_KEEP;
				op.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
				// (a mark: one less, wrapping: 0 to 255 the first time, 254 where the collision overlaps itself and it's
				// drawn twice, and so on; an inversion turned a second mark back to 0, the sea then drawn over it. The
				// reference against the stencil: ref >= 0 always, ref >= 255 only for a reference of 255)
				op.StencilPassOp = kind == water_kind::whole ? D3D12_STENCIL_OP_DECR : D3D12_STENCIL_OP_KEEP;
				op.StencilFunc = kind == water_kind::whole ? D3D12_COMPARISON_FUNC_ALWAYS : D3D12_COMPARISON_FUNC_GREATER_EQUAL;
				ds.FrontFace = op;
				ds.BackFace = op;
			}
			desc.DSVFormat = kBladeDepthFormat; // (the blades' depth buffer)
			desc.RasterizerState.MultisampleEnable = rt_desc.SampleDesc.Count > 1;
		}
		b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
		desc.SampleMask = UINT_MAX;
		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = t.rtv_format;
		desc.SampleDesc = rt_desc.SampleDesc;
		ID3D12PipelineState *pso = nullptr;
		const HRESULT hr = _device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
		const char *const what = debug ? "water view" : kind == water_kind::whole ? "water (whole)" : kind == water_kind::sea ? "sea" :
			kind == water_kind::fading ? "water (fading)" : "water";
		if (FAILED(hr))
		{
			char text[96];
			std::snprintf(text, sizeof(text), "%s pipeline failed (0x%08lX, format %u)", what, static_cast<unsigned long>(hr), static_cast<unsigned>(t.rtv_format));
			error = text;
			return nullptr;
		}
		pso->SetName(debug ? L"worldfx: water view pipeline" : kind == water_kind::whole ? L"worldfx: water pipeline (whole, marked)" :
			kind == water_kind::sea ? L"worldfx: sea pipeline" : kind == water_kind::fading ? L"worldfx: water pipeline (fading)" : L"worldfx: water pipeline");
		psos.push_back({ t.rtv_format, debug ? DXGI_FORMAT_UNKNOWN : kBladeDepthFormat, rt_desc.SampleDesc, pso });
		return pso;
	}

	bool renderer::upload_water(const blade_file &file, std::string &error)
	{
		unload_water();
		if (file.blade_count() == 0)
		{
			error = "the water file has no triangles";
			return false;
		}
		if (file.stride != 8)
		{
			error = "not a water file (8 floats a corner)";
			return false;
		}
		const UINT64 size = UINT64(file.blades.size()) * 4;
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *buffer = nullptr;
		HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer));
		void *mapped = nullptr;
		const D3D12_RANGE none = { 0, 0 };
		if (SUCCEEDED(hr))
			hr = buffer->Map(0, &none, &mapped);
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "water buffer failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			release(buffer);
			return false;
		}
		std::memcpy(mapped, file.blades.data(), static_cast<size_t>(size));
		buffer->Unmap(0, nullptr);
		buffer->SetName(L"worldfx: water");
		_water = buffer;
		_water_zone = file.zone;
		_water_corners = file.blade_count();
		_water_cell_size = file.cell_size;
		_water_cells = file.cells;
		return true;
	}

	void renderer::unload_water()
	{
		if (_water != nullptr)
			_old.push_back({ _water, 0 }); // a list recorded this frame may still use it
		_water = nullptr;
		_water_zone = 0;
		_water_corners = 0;
		_water_cells.clear();
	}

	// The sea's sheets into an upload buffer, as the water's corners (no cells: all of them are drawn)
	bool renderer::upload_sea(const blade_file &file, std::string &error)
	{
		unload_sea();
		if (file.blade_count() < 3 || file.stride != 8)
		{
			error = "the sea file has no triangles";
			return false;
		}
		const UINT64 size = UINT64(file.blade_count() / 3 * 3) * 32;
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *buffer = nullptr;
		HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer));
		void *mapped = nullptr;
		const D3D12_RANGE none = { 0, 0 };
		if (SUCCEEDED(hr))
			hr = buffer->Map(0, &none, &mapped);
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "sea buffer failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			release(buffer);
			return false;
		}
		std::memcpy(mapped, file.blades.data(), static_cast<size_t>(size));
		buffer->Unmap(0, nullptr);
		buffer->SetName(L"worldfx: sea");
		_sea = buffer;
		_sea_corners = file.blade_count() / 3 * 3;
		_sea_box[0] = _sea_box[1] = 1e30f;
		_sea_box[2] = _sea_box[3] = -1e30f;
		for (uint32_t i = 0; i < _sea_corners; ++i)
		{
			const float x = file.blades[size_t(i) * 8], z = file.blades[size_t(i) * 8 + 2];
			_sea_box[0] = std::min(_sea_box[0], x);
			_sea_box[1] = std::min(_sea_box[1], z);
			_sea_box[2] = std::max(_sea_box[2], x);
			_sea_box[3] = std::max(_sea_box[3], z);
		}
		return true;
	}

	void renderer::unload_sea()
	{
		if (_sea != nullptr)
			_old.push_back({ _sea, 0 }); // (as the water)
		_sea = nullptr;
		_sea_corners = 0;
	}

	bool renderer::upload_wet(const wet_file &file, std::string &error)
	{
		unload_wet();
		if (file.words.empty() || file.words.size() != size_t(file.height) * file.words_a_row)
		{
			error = "the wet mask is empty";
			return false;
		}
		const UINT64 size = UINT64(file.words.size()) * 4;
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *buffer = nullptr;
		HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer));
		void *mapped = nullptr;
		const D3D12_RANGE none = { 0, 0 };
		if (SUCCEEDED(hr))
			hr = buffer->Map(0, &none, &mapped);
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "wet mask buffer failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			release(buffer);
			return false;
		}
		std::memcpy(mapped, file.words.data(), static_cast<size_t>(size));
		buffer->Unmap(0, nullptr);
		buffer->SetName(L"worldfx: wet mask");
		_wet = buffer;
		_wet_x0 = file.x0;
		_wet_z0 = file.z0;
		_wet_square = file.square;
		_wet_width = file.width;
		_wet_height = file.height;
		_wet_words_a_row = file.words_a_row;
		return true;
	}

	void renderer::unload_wet()
	{
		if (_wet != nullptr)
			_old.push_back({ _wet, 0 }); // (as the water)
		_wet = nullptr;
		_wet_width = _wet_height = _wet_words_a_row = 0;
	}

	// The zone's meshes for the sun's shadow map: the triangles into an upload buffer, copied into a default-heap
	// one when the map is first drawn (it's drawn from them again now and then, each time reading many megabytes);
	// the cut-out textures' alpha into an upload buffer, its table's first words counted from the buffer's start
	bool renderer::upload_shadow(const shadow_file &file, std::string &error)
	{
		unload_shadow();
		const uint32_t count = file.triangle_count();
		if (count == 0)
		{
			error = "the shadow file has no triangles";
			return false;
		}
		D3D12_HEAP_PROPERTIES upload = {}, local = {};
		upload.Type = D3D12_HEAP_TYPE_UPLOAD;
		local.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = UINT64(count) * 48;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *staging = nullptr, *geo = nullptr, *masks = nullptr;
		void *mapped = nullptr;
		const D3D12_RANGE none = { 0, 0 };
		HRESULT hr = _device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging));
		if (SUCCEEDED(hr) && SUCCEEDED(hr = staging->Map(0, &none, &mapped)))
		{
			std::memcpy(mapped, file.triangles.data(), file.triangles.size() * 4);
			staging->Unmap(0, nullptr);
		}
		if (SUCCEEDED(hr))
			hr = _device->CreateCommittedResource(&local, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&geo));
		const size_t mask_words = file.masks.size() + file.words.size();
		if (SUCCEEDED(hr) && mask_words != 0)
		{
			desc.Width = UINT64(mask_words) * 4;
			hr = _device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&masks));
			if (SUCCEEDED(hr) && SUCCEEDED(hr = masks->Map(0, &none, &mapped)))
			{
				uint32_t *words = static_cast<uint32_t *>(mapped);
				for (size_t i = 0; i < file.masks.size(); ++i)
					words[i] = (i & 3) == 2 ? file.masks[i] + static_cast<uint32_t>(file.masks.size()) : file.masks[i];
				if (!file.words.empty())
					std::memcpy(words + file.masks.size(), file.words.data(), file.words.size() * 4);
				masks->Unmap(0, nullptr);
			}
		}
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "shadow mesh buffer failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			release(staging);
			release(geo);
			release(masks);
			return false;
		}
		staging->SetName(L"worldfx: shadow meshes (upload)");
		geo->SetName(L"worldfx: shadow meshes");
		if (masks != nullptr)
			masks->SetName(L"worldfx: shadow cut-outs");
		_shadow_staging = staging;
		_shadow_geo = geo;
		_shadow_masks = masks;
		_shadow_zone = file.zone;
		_shadow_triangles = count;
		_shadow_cell_size = file.cell_size;
		_shadow_cells = file.cells;
		// Each cell's box (its triangles can reach past the cell), and the zone's
		_shadow_bounds.assign(file.cells.size() * 6, 0.0f);
		for (int i = 0; i < 3; ++i)
		{
			_shadow_lo[i] = 1e30f;
			_shadow_hi[i] = -1e30f;
		}
		for (size_t c = 0; c < file.cells.size(); ++c)
		{
			float *b = &_shadow_bounds[c * 6];
			b[0] = b[1] = b[2] = 1e30f;
			b[3] = b[4] = b[5] = -1e30f;
			const uint32_t *t = &file.triangles[size_t(file.cells[c].first) * 12];
			for (uint32_t k = 0; k < file.cells[c].count * 3; ++k)
				for (int i = 0; i < 3; ++i)
				{
					float v;
					std::memcpy(&v, &t[k * 4 + i], 4);
					b[i] = std::min(b[i], v);
					b[3 + i] = std::max(b[3 + i], v);
				}
			for (int i = 0; i < 3; ++i)
			{
				_shadow_lo[i] = std::min(_shadow_lo[i], b[i]);
				_shadow_hi[i] = std::max(_shadow_hi[i], b[3 + i]);
			}
		}
		for (cascade &c : _cascades)
			c.forget();
		return true;
	}

	void renderer::unload_shadow()
	{
		for (ID3D12Resource *r : { _shadow_geo, _shadow_staging, _shadow_masks })
			if (r != nullptr)
				_old.push_back({ r, 0 }); // a list recorded this frame may still use them
		_shadow_geo = _shadow_staging = _shadow_masks = nullptr;
		_shadow_zone = 0;
		_shadow_triangles = 0;
		_shadow_cells.clear();
		_shadow_bounds.clear();
		for (cascade &c : _cascades)
			c.forget();
	}

	// The shadow map: one D32 texture array (the near cascade, then the far one, kSunMapSize square each, in
	// DEPTH_WRITE between draws), a view of each, and the buffer they're copied into: two halves, each the near
	// cascade's texels then the far one's, a cascade's latest drawing in one half and the one before in the other
	// (draw_sun_map), made once
	bool renderer::sun_map_for(std::string &error)
	{
		if (_sun_map != nullptr && _sun_map_copy != nullptr)
			return true;
		HRESULT hr = S_OK;
		if (_sun_map_heap == nullptr)
		{
			D3D12_DESCRIPTOR_HEAP_DESC hd = {};
			hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
			hd.NumDescriptors = 2;
			hr = _device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_sun_map_heap));
		}
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = kSunMapSize;
		desc.Height = kSunMapSize;
		desc.DepthOrArraySize = 2;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_D32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		D3D12_CLEAR_VALUE clear = {};
		clear.Format = DXGI_FORMAT_D32_FLOAT;
		clear.DepthStencil.Depth = 1.0f;
		if (SUCCEEDED(hr) && _sun_map == nullptr)
			hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&_sun_map));
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2] = {};
		UINT64 size = 0;
		if (SUCCEEDED(hr))
		{
			_device->GetCopyableFootprints(&desc, 0, 2, 0, footprints, nullptr, nullptr, &size);
			// (a placed footprint starts on a 512-byte boundary: the second half too)
			const UINT64 align = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
			_sun_map_half = (size + align - 1) / align * align;
			_sun_map_offset[0] = footprints[0].Offset;
			_sun_map_offset[1] = footprints[1].Offset;
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = 2 * _sun_map_half;
			bd.Height = 1;
			bd.DepthOrArraySize = 1;
			bd.MipLevels = 1;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&_sun_map_copy));
		}
		// (the sun pass finds the far cascade right after the near one: pitch x size uints in)
		if (SUCCEEDED(hr) && (footprints[0].Footprint.RowPitch % 4 != 0 || footprints[1].Offset != UINT64(footprints[0].Footprint.RowPitch) * kSunMapSize))
			hr = E_UNEXPECTED;
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "sun shadow map failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			release(_sun_map);
			release(_sun_map_copy);
			return false;
		}
		_sun_map->SetName(L"worldfx: sun shadow map");
		_sun_map_copy->SetName(L"worldfx: sun shadow map (copy)");
		_sun_map_copy_state = D3D12_RESOURCE_STATE_COMMON;
		_sun_map_pitch = footprints[0].Footprint.RowPitch / 4;
		const UINT step = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
		for (UINT k = 0; k < 2; ++k)
		{
			D3D12_DEPTH_STENCIL_VIEW_DESC vd = {};
			vd.Format = DXGI_FORMAT_D32_FLOAT;
			vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
			vd.Texture2DArray.FirstArraySlice = k;
			vd.Texture2DArray.ArraySize = 1;
			D3D12_CPU_DESCRIPTOR_HANDLE h = _sun_map_heap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += SIZE_T(k) * step;
			_device->CreateDepthStencilView(_sun_map, &vd, h);
		}
		for (cascade &c : _cascades)
			c.forget();
		return true;
	}

	// The shadow map's pipeline: the zone's triangles, depth only (LESS, D32), no culling (cards and the
	// ground are single sheets); its pixel shader drops the cut-outs' clear texels
	ID3D12PipelineState *renderer::shadow_pipeline(std::string &error)
	{
		if (_shadow_pso != nullptr)
			return _shadow_pso;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = _root;
		desc.VS = { _shadow_vs.data(), _shadow_vs.size() };
		desc.PS = { _shadow_ps.data(), _shadow_ps.size() };
		desc.SampleMask = UINT_MAX;
		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.DepthStencilState.DepthEnable = TRUE;
		desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 0;
		desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		desc.SampleDesc.Count = 1;
		const HRESULT hr = _device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_shadow_pso));
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "sun shadow map pipeline failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			_shadow_pso = nullptr;
			return nullptr;
		}
		_shadow_pso->SetName(L"worldfx: sun shadow map pipeline");
		return _shadow_pso;
	}

	namespace
	{
		// Across the sun's rays: two unit vectors square to toward-the-sun and to each other
		void sun_basis(const float f[3], float r[3], float u[3])
		{
			// (the world's up is -y; with the sun straight overhead, east will do)
			const bool high = std::fabs(f[1]) >= 0.99f;
			const float ref[3] = { high ? 1.0f : 0.0f, high ? 0.0f : -1.0f, 0.0f };
			r[0] = ref[1] * f[2] - ref[2] * f[1];
			r[1] = ref[2] * f[0] - ref[0] * f[2];
			r[2] = ref[0] * f[1] - ref[1] * f[0];
			const float len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
			for (int i = 0; i < 3; ++i)
				r[i] /= len;
			u[0] = f[1] * r[2] - f[2] * r[1];
			u[1] = f[2] * r[0] - f[0] * r[2];
			u[2] = f[0] * r[1] - f[1] * r[0];
		}
	}

	// Draws again the cascades whose picture changed and copies them for the sun pass. Each looks down the sun's
	// rays at the camera, kSunMapReach yalms each way across them, and takes in the whole zone along them: anything
	// between the sun and a point in the map shades it, however far off or high up. Its depth is how far from the
	// sun, 0 at the zone's nearest corner, 1 at its farthest. Draws only the cells whose box meets the cascade
	// across the rays. Returns which were drawn (bits); leaves the list bound to its own.
	// When: the world is still, and each cascade's centre sits on its own texel grid across the rays, so the camera
	// moving changes nothing in a cascade but how far it reaches; only the sun turning moves shadows. So a cascade is
	// drawn again as soon as the sun has turned enough to move a shadow 100 yalms long by a quarter of its texel
	// (near: about a centimetre, 0.006 degrees; far: four times that), and when the camera has left its middle half.
	// At the game's pace (it eases its light, about a tenth of a degree a second) that's the near one every tenth of
	// a second or so and the far one every half second, each time by less than a texel: the shadows glide. The far
	// one waits for every other frame (four times the triangles), unless the camera has left its middle half.
	// (Drawn only past a whole degree, a long shadow jumped a yalm at a time: on a chocobo, a noticeable skip.)
	// With the look's clock (as the add-on has it), a cascade the sun has turned for is drawn again only once
	// kSunMapHold has passed since its last drawing (the far one half that out of step with the near one), and the
	// sun pass fades from the drawing before to the new one over the next kSunMapHold: each texel's shadow changes
	// as a fade, never at once. Drawn again some 16 times a second, each drawing on a slightly different grid, a
	// canopy's leaves (their masks as coarse as the map's texels) flipped texels along every edge each time: the
	// dapples under trees reshaped as if blown by wind (in game, 2026-10-21). The camera leaving a cascade's middle
	// half still draws it again at once (the fade then from the drawing before, where it covers the point).
	uint32_t renderer::draw_sun_map(ID3D12GraphicsCommandList *list, const float eye[3], const float sun[3], double clock, uint32_t &triangles, std::string &error)
	{
		const bool timed = std::isfinite(clock) && clock > 0.0;
		const float len = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
		if (!(len > 0.5f) || _shadow_cells.empty())
			return 0;
		const float f[3] = { sun[0] / len, sun[1] / len, sun[2] / len };
		float r[3], u[3];
		sun_basis(f, r, u);
		uint32_t todo = 0;
		const bool far_turn = (++_sun_map_turn & 1) == 0;
		for (uint32_t k = 0; k < 2; ++k)
		{
			const cascade &c = _cascades[k];
			const float texel = 2.0f * kSunMapReach[k] / kSunMapSize;
			// (how far the sun has turned since, in radians: the cross product's length, exact for small angles
			// where 1 - the dot product is lost in a float's rounding)
			const float tx = f[1] * c.sun[2] - f[2] * c.sun[1], ty = f[2] * c.sun[0] - f[0] * c.sun[2], tz = f[0] * c.sun[1] - f[1] * c.sun[0];
			const bool turned = std::sqrt(tx * tx + ty * ty + tz * tz) * 100.0f > 0.25f * texel;
			const float *m = c.world_to_clip;
			const float x = eye[0] * m[0] + eye[1] * m[4] + eye[2] * m[8] + m[12], y = eye[0] * m[1] + eye[1] * m[5] + eye[2] * m[9] + m[13];
			const bool left = std::fabs(x) > 0.25f || std::fabs(y) > 0.25f;
			const bool due = timed ? clock - c.drawn_at >= kSunMapHold : (k == 0 || far_turn);
			if (!c.valid || left || (turned && due))
				todo |= 1u << k;
		}
		if (todo == 0)
			return 0;
		ID3D12PipelineState *const pso = sun_map_for(error) ? shadow_pipeline(error) : nullptr;
		if (pso == nullptr)
			return 0;

		// (the zone along the rays, a yalm to spare each end)
		float middle = 0.0f, half = 0.0f;
		for (int i = 0; i < 3; ++i)
		{
			middle += 0.5f * (_shadow_lo[i] + _shadow_hi[i]) * f[i];
			half += 0.5f * (_shadow_hi[i] - _shadow_lo[i]) * std::fabs(f[i]);
		}
		const float nearest = middle + half + 1.0f, span = 2.0f * half + 2.0f;

		if (_shadow_staging != nullptr) // (the triangles into their default-heap buffer, once)
		{
			transition(list, _shadow_geo, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			list->CopyBufferRegion(_shadow_geo, 0, _shadow_staging, 0, UINT64(_shadow_triangles) * 48);
			transition(list, _shadow_geo, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			_old.push_back({ _shadow_staging, 0 });
			_shadow_staging = nullptr;
		}
		section(list, L"worldfx: sun shadow map");
		list->SetGraphicsRootSignature(_root);
		list->SetPipelineState(pso);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->SetGraphicsRootShaderResourceView(1, _shadow_geo->GetGPUVirtualAddress());
		// (no cut-outs: t1 still gets a valid address, the triangles', never read)
		list->SetGraphicsRootShaderResourceView(2, (_shadow_masks != nullptr ? _shadow_masks : _shadow_geo)->GetGPUVirtualAddress());
		const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, float(kSunMapSize), float(kSunMapSize), 0.0f, 1.0f };
		const D3D12_RECT scissor = { 0, 0, LONG(kSunMapSize), LONG(kSunMapSize) };
		list->RSSetViewports(1, &viewport);
		list->RSSetScissorRects(1, &scissor);
		const UINT step = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
		std::vector<cell_run> runs;
		for (uint32_t k = 0; k < 2; ++k)
		{
			if ((todo & (1u << k)) == 0)
				continue;
			cascade &c = _cascades[k];
			const float reach = kSunMapReach[k], texel = 2.0f * reach / kSunMapSize;
			// (with the clock: the last drawing becomes the one before, the new one goes into the copy's other half; the
			// far cascade's first drawing dated half a hold back, so the two take turns)
			if (timed && c.valid)
			{
				std::memcpy(c.prev_world_to_clip, c.world_to_clip, sizeof(c.world_to_clip));
				c.prev_valid = true;
				c.slot ^= 1u;
				c.drawn_at = clock;
			}
			else
			{
				c.prev_valid = false;
				c.drawn_at = clock - (k == 1 ? 0.5 * kSunMapHold : 0.0);
			}
			// (centred on the camera, on its own texel grid across the rays)
			c.centre[0] = std::round((eye[0] * r[0] + eye[1] * r[1] + eye[2] * r[2]) / texel) * texel;
			c.centre[1] = std::round((eye[0] * u[0] + eye[1] * u[1] + eye[2] * u[2]) / texel) * texel;
			float *m = c.world_to_clip;
			for (int i = 0; i < 3; ++i)
			{
				m[i * 4 + 0] = r[i] / reach;
				m[i * 4 + 1] = u[i] / reach;
				m[i * 4 + 2] = -f[i] / span;
				m[i * 4 + 3] = 0.0f;
			}
			m[12] = -c.centre[0] / reach;
			m[13] = -c.centre[1] / reach;
			m[14] = nearest / span;
			m[15] = 1.0f;
			for (int i = 0; i < 3; ++i)
				c.sun[i] = f[i];
			runs.clear();
			for (size_t i = 0; i < _shadow_cells.size(); ++i)
			{
				const blade_file::cell &cell = _shadow_cells[i];
				const float *b = &_shadow_bounds[i * 6];
				float cs = 0.0f, hs = 0.0f, ct = 0.0f, ht = 0.0f;
				for (int a = 0; a < 3; ++a)
				{
					const float mid = 0.5f * (b[a] + b[3 + a]), ext = 0.5f * (b[3 + a] - b[a]);
					cs += mid * r[a];
					hs += ext * std::fabs(r[a]);
					ct += mid * u[a];
					ht += ext * std::fabs(u[a]);
				}
				if (std::fabs(cs - c.centre[0]) > reach + hs || std::fabs(ct - c.centre[1]) > reach + ht || cell.count == 0)
					continue;
				if (!runs.empty() && runs.back().first + runs.back().count == cell.first)
					runs.back().count += cell.count;
				else
					runs.push_back({ cell.first, cell.count, 1 });
			}
			D3D12_CPU_DESCRIPTOR_HANDLE dsv = _sun_map_heap->GetCPUDescriptorHandleForHeapStart();
			dsv.ptr += SIZE_T(k) * step;
			list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
			list->SetGraphicsRoot32BitConstants(0, 16, m, 0);
			// (SV_VertexID counts from 0 in each draw, whatever the start vertex, on WARP and on the GPU alike:
			// the run's first corner goes in as blade.w)
			for (const cell_run &run : runs)
			{
				list->SetGraphicsRoot32BitConstant(0, run.first * 3, kFirstBladeConstant);
				list->DrawInstanced(run.count * 3, 1, 0, 0);
				triangles += run.count;
			}
			c.valid = true;
		}
		// Copied for the sun pass: each cascade drawn, whole, into the copy's half its new drawing goes in
		const D3D12_RESOURCE_DESC desc = _sun_map->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2] = {};
		_device->GetCopyableFootprints(&desc, 0, 2, 0, footprints, nullptr, nullptr, nullptr);
		transition(list, _sun_map_copy, _sun_map_copy_state, D3D12_RESOURCE_STATE_COPY_DEST);
		for (uint32_t k = 0; k < 2; ++k)
			if ((todo & (1u << k)) != 0)
			{
				transition(list, _sun_map, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE, k);
				D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
				dst.pResource = _sun_map_copy;
				dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				dst.PlacedFootprint = footprints[k];
				dst.PlacedFootprint.Offset += _cascades[k].slot * _sun_map_half;
				src.pResource = _sun_map;
				src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				src.SubresourceIndex = k;
				list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
				transition(list, _sun_map, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE, k);
			}
		const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		transition(list, _sun_map_copy, D3D12_RESOURCE_STATE_COPY_DEST, readable);
		_sun_map_copy_state = readable;
		return todo;
	}

	// The sun pass's parameters for the map (kSunMapParams floats, after the actors): for each cascade, three
	// float4s taking a view-space point (x, y, z, 1) to its texel's column and row and its depth there; then
	// texels a side, how much depth a yalm is in each cascade, the copy's row pitch (uints); a texel's size
	// (yalms) in each, and where in the copy each cascade's last drawing starts (uints, as their bits: far, near).
	// prev (kSunMapPrevParams floats, after the wear's): the same rows for each cascade's drawing before; how far the
	// last has faded in over it (1: wholly, the one before not read; 1 without the clock), where each one before
	// starts (uints, as bits: near, far); their depth a yalm (near, far)
	void renderer::sun_map_params(const camera &cam, double clock, float out[32], float prev[32]) const
	{
		// View space back to the world: v (x, y, z, 1) x [R^T 0; -t R^T 1], the view being [R 0; t 1]
		const float *v = cam.view;
		float inv[16] = {};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				inv[i * 4 + j] = v[j * 4 + i];
		for (int j = 0; j < 3; ++j)
			inv[12 + j] = -(v[12] * v[j * 4 + 0] + v[13] * v[j * 4 + 1] + v[14] * v[j * 4 + 2]);
		inv[15] = 1.0f;
		const float n = float(kSunMapSize);
		auto rows = [&](const float *world_to_clip, float *o) {
			float s[16];
			multiply(inv, world_to_clip, s);
			for (int i = 0; i < 4; ++i)
			{
				o[i] = s[i * 4 + 0] * 0.5f * n + (i == 3 ? 0.5f * n : 0.0f);
				o[4 + i] = -s[i * 4 + 1] * 0.5f * n + (i == 3 ? 0.5f * n : 0.0f);
				o[8 + i] = s[i * 4 + 2];
			}
		};
		// (the depth column's length: 1 / the zone's span along the rays)
		auto per_yalm = [](const float *m) { return std::sqrt(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]); };
		auto start = [&](uint32_t k, uint32_t slot) {
			const uint32_t u = uint32_t((slot * _sun_map_half + _sun_map_offset[k]) / 4);
			float f;
			std::memcpy(&f, &u, 4);
			return f;
		};
		const bool timed = std::isfinite(clock) && clock > 0.0;
		for (uint32_t k = 0; k < 2; ++k)
		{
			const cascade &c = _cascades[k];
			rows(c.world_to_clip, out + k * 12);
			out[25 + k] = per_yalm(c.world_to_clip);
			const bool fading = timed && c.prev_valid;
			rows(fading ? c.prev_world_to_clip : c.world_to_clip, prev + k * 12);
			prev[24 + k] = fading ? float(std::clamp((clock - c.drawn_at) / kSunMapHold, 0.0, 1.0)) : 1.0f;
			prev[26 + k] = start(k, c.slot ^ 1u);
			prev[28 + k] = per_yalm(fading ? c.prev_world_to_clip : c.world_to_clip);
		}
		out[24] = n;
		out[27] = float(_sun_map_pitch);
		out[28] = 2.0f * kSunMapReach[0] / n;
		out[29] = 2.0f * kSunMapReach[1] / n;
		out[30] = start(1, _cascades[1].slot);
		out[31] = start(0, _cascades[0].slot);
		prev[30] = prev[31] = 0.0f;
	}

	bool renderer::upload(const blade_file &file, std::string &error)
	{
		unload();
		if (file.blade_count() == 0)
		{
			error = "the grass file has no blades";
			return false;
		}
		const UINT64 size = UINT64(file.blade_count()) * 16;
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *buffer = nullptr;
		HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer));
		void *mapped = nullptr;
		const D3D12_RANGE none = { 0, 0 };
		if (SUCCEEDED(hr))
			hr = buffer->Map(0, &none, &mapped);
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "blade buffer failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			release(buffer);
			return false;
		}
		std::memcpy(mapped, file.blades.data(), static_cast<size_t>(size));
		buffer->SetName(L"worldfx: blades");
		_buffer = buffer;
		buffer->Unmap(0, nullptr);
		_zone = file.zone;
		_blade_count = file.blade_count();
		_cell_size = file.cell_size;
		_cells = file.cells;
		if (!forget_colours(error))
		{
			unload();
			return false;
		}
		return true;
	}

	// A new memory of the ground's colours and wear (kMemoryStride bytes a point, zeroed: nothing known yet, nothing worn)
	bool renderer::forget_colours(std::string &error)
	{
		if (_memory != nullptr)
			_old.push_back({ _memory, 0 });
		_memory = nullptr;
		if (_blade_count == 0)
			return true;
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = UINT64(_blade_count) * kMemoryStride;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		const HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&_memory));
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "colour memory failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			_memory = nullptr;
			return false;
		}
		_memory->SetName(L"worldfx: colour memory");
		_memory_fresh = true; // (zeroed at the next draw)
		return true;
	}

	void renderer::unload()
	{
		if (_buffer != nullptr)
			_old.push_back({ _buffer, 0 }); // a list recorded this frame may still use it
		if (_memory != nullptr)
			_old.push_back({ _memory, 0 });
		_memory = nullptr;
		_buffer = nullptr;
		_zone = 0;
		_blade_count = 0;
		_cells.clear();
	}

	void renderer::release_old(uint32_t frames_to_keep)
	{
		for (size_t i = 0; i < _old.size();)
		{
			if (++_old[i].frames > frames_to_keep)
			{
				release(_old[i].buffer);
				_old.erase(_old.begin() + i);
			}
			else
				++i;
		}
	}

	// A buffer of at least size bytes for a copy (of the world or its depth); a smaller one goes to the old buffers
	bool renderer::copy_buffer(copy &c, UINT64 size, std::string &error)
	{
		if (c.buffer != nullptr && c.size >= size)
			return true;
		if (c.buffer != nullptr)
			_old.push_back({ c.buffer, 0 });
		c.buffer = nullptr;
		c.size = 0;
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		const HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&c.buffer));
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "copy buffer failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			c.buffer = nullptr;
			return false;
		}
		c.buffer->SetName(&c == &_ground ? L"worldfx: copy of the world" : L"worldfx: copy of the world's depth");
		c.size = size;
		c.state = D3D12_RESOURCE_STATE_COMMON; // (buffers start in COMMON, whatever is asked)
		return true;
	}

	// Copies subresource 0 (plane 0 of a depth buffer) of res, in state, into c; that subresource alone is
	// left in COPY_SOURCE (a depth buffer's stencil plane isn't touched). The row pitch in 4-byte units, or 0
	// when it couldn't be copied.
	uint32_t renderer::copy_out(ID3D12GraphicsCommandList *list, copy &c, ID3D12Resource *res, D3D12_RESOURCE_STATES state)
	{
		const D3D12_RESOURCE_DESC desc = res->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
		UINT64 size = 0;
		_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
		std::string ignored; // without the copy, grass goes on without what it gives rather than not at all
		if (size == 0 || size == UINT64(-1) || !copy_buffer(c, size, ignored))
			return 0;
		transition(list, res, state, D3D12_RESOURCE_STATE_COPY_SOURCE, 0);
		transition(list, c.buffer, c.state, D3D12_RESOURCE_STATE_COPY_DEST);
		D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
		dst.pResource = c.buffer;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint = footprint;
		src.pResource = res;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = 0;
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		transition(list, c.buffer, D3D12_RESOURCE_STATE_COPY_DEST, readable);
		c.state = readable;
		return footprint.Footprint.RowPitch / 4;
	}

	// The blades' own depth buffer, for each other (the world's depth they test against its copy): made, or
	// made again, for the world target's size and samples
	bool renderer::blade_depth_for(const D3D12_RESOURCE_DESC &rt_desc, std::string &error)
	{
		if (_blade_depth != nullptr)
		{
			const D3D12_RESOURCE_DESC have = _blade_depth->GetDesc();
			if (have.Width == rt_desc.Width && have.Height == rt_desc.Height && same_samples(have.SampleDesc, rt_desc.SampleDesc))
				return true;
			_old.push_back({ _blade_depth, 0 }); // (a list recorded this frame may still use it)
			_blade_depth = nullptr;
		}
		HRESULT hr = S_OK;
		if (_dsv_heap == nullptr)
		{
			D3D12_DESCRIPTOR_HEAP_DESC hd = {};
			hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
			hd.NumDescriptors = 1;
			hr = _device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_dsv_heap));
		}
		if (SUCCEEDED(hr))
		{
			D3D12_HEAP_PROPERTIES heap = {};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = rt_desc.Width;
			desc.Height = rt_desc.Height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = kBladeDepthFormat;
			desc.SampleDesc = rt_desc.SampleDesc;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
			D3D12_CLEAR_VALUE clear = {};
			clear.Format = kBladeDepthFormat;
			clear.DepthStencil.Depth = 1.0f;
			hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&_blade_depth));
		}
		if (FAILED(hr))
		{
			char text[64];
			std::snprintf(text, sizeof(text), "the blades' depth buffer failed (0x%08lX)", static_cast<unsigned long>(hr));
			error = text;
			_blade_depth = nullptr;
			return false;
		}
		_blade_depth->SetName(L"worldfx: the blades' depth");
		_device->CreateDepthStencilView(_blade_depth, nullptr, _dsv_heap->GetCPUDescriptorHandleForHeapStart());
		return true;
	}

	// The add-on's own view of the world's target, made again at each draw: the add-on draws only through
	// this one. dgVoodoo's view (t.rtv) is a slot in its own table, which it may fill again in ways ReShade
	// doesn't see (a copied descriptor): a full-screen pass through a stale one would write into another,
	// smaller target, past its end, into whatever lies there (the game's next draws then read garbage)
	bool renderer::world_rtv_for(const target &t, D3D12_CPU_DESCRIPTOR_HANDLE &rtv, std::string &error)
	{
		if (_rtv_heap == nullptr)
		{
			D3D12_DESCRIPTOR_HEAP_DESC hd = {};
			hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			hd.NumDescriptors = 1;
			if (FAILED(_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_rtv_heap))))
			{
				_rtv_heap = nullptr;
				error = "no descriptor heap for the world's view";
				return false;
			}
		}
		const D3D12_RESOURCE_DESC rt_desc = t.rt->GetDesc();
		D3D12_RENDER_TARGET_VIEW_DESC vd = {};
		vd.Format = t.rtv_format != DXGI_FORMAT_UNKNOWN ? t.rtv_format : rt_desc.Format;
		vd.ViewDimension = rt_desc.SampleDesc.Count > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
		rtv = _rtv_heap->GetCPUDescriptorHandleForHeapStart();
		_device->CreateRenderTargetView(t.rt, &vd, rtv);
		return true;
	}

	draw_stats renderer::draw(ID3D12GraphicsCommandList *list, const target &t, const camera &cam, const look &l, std::string &error)
	{
		draw_stats stats;
		if (_root == nullptr || t.rt == nullptr || t.ds == nullptr)
			return stats;
		const bool grass = l.grass && _buffer != nullptr && _memory != nullptr;
		const uint32_t lamp_count = l.lamps != nullptr ? std::min<uint32_t>(l.lamp_count, kMaxLamps) : 0;
		// (the sun's shadows and glints as the weather leaves them: none under an overcast sky, the sun pass skipped)
		const float weather_sun = std::isfinite(l.weather_sun) ? std::clamp(l.weather_sun, 0.0f, 1.0f) : 1.0f;
		const float sun_shadow = l.sun_shadow * weather_sun;
		// A sky of its own: with the record's sky known and a dome to repaint
		bool dome = l.dome.on && l.dome.segments > 2;
		for (int i = 0; i < 8 && dome; ++i)
			dome = std::isfinite(l.dome.stops[i][3]) && std::isfinite(l.dome.stops[i][0]) && std::isfinite(l.dome.stops[i][1]) && std::isfinite(l.dome.stops[i][2]);
		// The weather of its own (look::wx): its parts in the sky (with the dome: clouds, the veil, the flash and the bolt,
		// the aurora, a darker or starrier sky, dust and gloom over it), the heat's shimmer in the shading pass, the air pass
		// at the end (dust, streaks, fog banks, the flash), the grass bent by the wind, the leaves (with the rain map)
		auto part = [](float v, float most) { return std::isfinite(v) ? std::clamp(v, 0.0f, most) : 0.0f; };
		const look::weather_fx &wx = l.wx;
		const float wx_flash = part(wx.flash, 4.0f), wx_bolt = part(wx.bolt, 4.0f), wx_dust = part(wx.dust, 2.0f), wx_streaks = part(wx.streaks, 2.0f);
		const float wx_gloom = part(wx.gloom, 2.0f), wx_heat = part(wx.heat, 2.0f), wx_wind = part(wx.wind_strength, 3.0f), wx_leaves = part(wx.leaves, 2.0f);
		const float wx_stars = std::isfinite(wx.star_boost) ? std::clamp(wx.star_boost, 0.0f, 4.0f) : 1.0f;
		const bool wx_sky = dome && (part(wx.cloud_cover, 1.0f) > 0.0f || part(wx.cloud_veil, 1.0f) > 0.0f || wx_flash > 0.0f || wx_bolt > 0.0f ||
			part(wx.aurora, 2.0f) > 0.0f || part(wx.sky_dark, 1.0f) > 0.0f || wx_stars != 1.0f || wx_dust > 0.0f || wx_gloom > 0.0f);
		const bool shimmer = wx_heat > 0.0f;
		const bool air_wanted = wx_flash > 0.0f || wx_dust > 0.0f || wx_streaks > 0.0f || wx_gloom > 0.0f; // (the flash's pass with it)
		const bool leaves = wx_leaves > 0.0f && !_rain.q.empty();
		const bool shading_wanted = l.ao_strength > 0.0f || l.relief > 0.0f || sun_shadow > 0.0f || lamp_count != 0 || dome || shimmer;
		// The game's fog, when it makes sense (ending past where it starts)
		const bool haze = l.haze && std::isfinite(l.haze_start) && std::isfinite(l.haze_end) && l.haze_end > l.haze_start + 0.01f && l.haze_end > 0.0f &&
			std::isfinite(l.haze_color[0]) && std::isfinite(l.haze_color[1]) && std::isfinite(l.haze_color[2]);
		const float haze_params[kHazeParams] = { haze ? std::clamp(l.haze_color[0], 0.0f, 1.0f) : 0.0f, haze ? std::clamp(l.haze_color[1], 0.0f, 1.0f) : 0.0f,
			haze ? std::clamp(l.haze_color[2], 0.0f, 1.0f) : 0.0f, haze ? 1.0f : 0.0f,
			l.haze_start, l.haze_end, haze ? 1.0f / (l.haze_end - l.haze_start) : 0.0f, 0.0f };
		// The wet look on path and stone, with the zone's mask loaded (it's read in the shading pass)
		const bool wet = std::isfinite(l.wet) && l.wet > 0.0f && _wet != nullptr;
		const bool shading = shading_wanted || wet;
		// The sea (the zone's sea sheets, with the water): the water then reaches as far as it does (its distance), so the
		// baked water doesn't fade out where the sea goes on
		const bool sea = l.water && l.sea && _sea != nullptr;
		const bool water_shade = l.water && (_water != nullptr || sea);
		const bool water = (l.water || l.water_debug) && (_water != nullptr || sea);
		const float sea_reach = std::isfinite(l.sea_distance) ? std::clamp(l.sea_distance, 0.0f, 20000.0f) : 0.0f;
		const float water_reach = sea ? std::max(l.water_distance, sea_reach) : l.water_distance;
		const bool fireflies = l.fireflies > 0.0f && l.night > 0.0f && _buffer != nullptr;
		// Rain, with the zone's rain map: how hard (the share of the columns with drops in them)
		const float rain_amount = std::isfinite(l.rain) ? std::clamp(l.rain, 0.0f, 2.0f) : 0.0f;
		const bool rain = rain_amount > 0.0f && !_rain.q.empty();
		// Snow, with the same map: how hard (the share of the columns with flakes in them)
		const float snow_amount = std::isfinite(l.snow) ? std::clamp(l.snow, 0.0f, 2.0f) : 0.0f;
		const bool snow = snow_amount > 0.0f && !_rain.q.empty();
		const bool falling = rain || snow || leaves;
		// The grass's wear: with the blades drawn, a clock, and wear to show or a cut to make
		const bool wearing = grass && std::isfinite(l.clock) && l.clock > 0.0 &&
			((std::isfinite(l.wear) && l.wear > 0.0f) || (l.cuts != nullptr && l.cut_count != 0));
		// The game's visibility tests set for this frame (set_vis_boxes): they need the depth copy alone
		bool vis = false;
		for (const vis_slot &s : _vis_slots)
			vis = vis || s.state == vis_slot::state_t::set;
		// (the weather's parameters go to the passes with anything of it to draw, the grass's wind too)
		const bool wx_any = wx_sky || shimmer || air_wanted || leaves || (grass && wx_wind > 0.0f);
		if (!grass && !shading && !water && !fireflies && !falling && !vis && !air_wanted)
			return stats;

		float eye[3];
		eye_from_view(cam.view, eye);
		if (!std::isfinite(eye[0]) || !std::isfinite(eye[1]) || !std::isfinite(eye[2]))
			return stats;
		// (the sea drawn with any of its sheets within the water's reach: not with the camera far from all of them)
		bool sea_here = false;
		if (sea && water_shade)
		{
			const float dx = std::max({ _sea_box[0] - eye[0], eye[0] - _sea_box[2], 0.0f });
			const float dz = std::max({ _sea_box[1] - eye[2], eye[2] - _sea_box[3], 0.0f });
			sea_here = dx * dx + dz * dz <= water_reach * water_reach;
		}

		// The cells that reach within the draw distance of the camera (across the ground), merged into runs
		// Far grass is thinner: tufts are whole within 40% of the distance, half to 70%, a quarter beyond, each
		// band drawn with its own tuft size (so far grass costs less, rather than being drawn and hidden)
		const UINT tuft = l.tuft < 1 ? 1 : static_cast<UINT>(l.tuft);
		const UINT tufts[3] = { tuft, std::max(1u, (tuft + 1) / 2), std::max(1u, (tuft + 3) / 4) };
		cell_run runs[512];
		uint32_t run_count = 0;
		const float reach2 = l.distance * l.distance;
		for (const blade_file::cell &c : _cells)
		{
			if (!grass)
				break;
			const float x0 = c.cx * _cell_size, z0 = c.cz * _cell_size;
			const float dx = eye[0] < x0 ? x0 - eye[0] : eye[0] > x0 + _cell_size ? eye[0] - x0 - _cell_size : 0.0f;
			const float dz = eye[2] < z0 ? z0 - eye[2] : eye[2] > z0 + _cell_size ? eye[2] - z0 - _cell_size : 0.0f;
			const float d2 = dx * dx + dz * dz;
			if (d2 > reach2 || c.count == 0)
				continue;
			const UINT band = d2 < reach2 * 0.16f ? 0 : d2 < reach2 * 0.49f ? 1 : 2;
			if (run_count != 0 && runs[run_count - 1].first + runs[run_count - 1].count == c.first && runs[run_count - 1].tuft == tufts[band])
				runs[run_count - 1].count += c.count;
			else if (run_count < 512)
				runs[run_count++] = { c.first, c.count, tufts[band] };
		}
		// The water cells within its distance, merged into runs (corners in a row)
		cell_run water_runs[256];
		uint32_t water_run_count = 0;
		const float water_reach2 = water_reach * water_reach;
		for (const blade_file::cell &c : _water_cells)
		{
			if (!water)
				break;
			const float x0 = c.cx * _water_cell_size, z0 = c.cz * _water_cell_size;
			const float dx = eye[0] < x0 ? x0 - eye[0] : eye[0] > x0 + _water_cell_size ? eye[0] - x0 - _water_cell_size : 0.0f;
			const float dz = eye[2] < z0 ? z0 - eye[2] : eye[2] > z0 + _water_cell_size ? eye[2] - z0 - _water_cell_size : 0.0f;
			if (dx * dx + dz * dz > water_reach2 || c.count == 0)
				continue;
			if (water_run_count != 0 && water_runs[water_run_count - 1].first + water_runs[water_run_count - 1].count == c.first)
				water_runs[water_run_count - 1].count += c.count;
			else if (water_run_count < 256)
				water_runs[water_run_count++] = { c.first, c.count, 1 };
		}
		// The fireflies' cells: every kFireflyStride-th point (fewer with a lower amount) within their distance
		cell_run firefly_runs[256];
		uint32_t firefly_run_count = 0;
		const UINT firefly_stride = fireflies ? static_cast<UINT>(std::clamp(kFireflyStride / l.fireflies, 20.0f, 100000.0f)) : 0;
		const float firefly_reach2 = l.firefly_distance * l.firefly_distance;
		for (const blade_file::cell &c : _cells)
		{
			if (!fireflies)
				break;
			const float x0 = c.cx * _cell_size, z0 = c.cz * _cell_size;
			const float dx = eye[0] < x0 ? x0 - eye[0] : eye[0] > x0 + _cell_size ? eye[0] - x0 - _cell_size : 0.0f;
			const float dz = eye[2] < z0 ? z0 - eye[2] : eye[2] > z0 + _cell_size ? eye[2] - z0 - _cell_size : 0.0f;
			if (dx * dx + dz * dz > firefly_reach2 || c.count == 0)
				continue;
			if (firefly_run_count != 0 && firefly_runs[firefly_run_count - 1].first + firefly_runs[firefly_run_count - 1].count == c.first)
				firefly_runs[firefly_run_count - 1].count += c.count;
			else if (firefly_run_count < 256)
				firefly_runs[firefly_run_count++] = { c.first, c.count, firefly_stride };
		}
		if (run_count == 0 && !shading && water_run_count == 0 && !sea_here && firefly_run_count == 0 && !falling && !vis && !air_wanted)
			return stats;

		ID3D12PipelineState *pso = run_count != 0 ? pipeline_for(t, error) : nullptr;
		if (run_count != 0 && pso == nullptr)
			return stats;

		_mark_count[_time_slot] = 0;
		_shade_end[_time_slot] = kTimingMarks;
		section(list, L"worldfx: begin (copies of the world)");

		// The world so far into a buffer, for the ground's colour under each blade (RGBA8/BGRA8, one sample)
		const D3D12_RESOURCE_DESC rt_desc = t.rt->GetDesc();
		uint32_t ground_kind = 0, ground_pitch = 0;
		const DXGI_FORMAT f = rt_desc.Format;
		const bool rgba = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		const bool bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
			f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8X8_TYPELESS || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
		D3D12_RESOURCE_STATES rt_now = t.rt_state;
		if ((rgba || bgra) && rt_desc.SampleDesc.Count == 1 && rt_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
			if ((ground_pitch = copy_out(list, _ground, t.rt, rt_now)) != 0)
			{
				ground_kind = rgba ? 1 : 2;
				rt_now = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
		transition(list, t.rt, rt_now, D3D12_RESOURCE_STATE_RENDER_TARGET);

		// And its depth, to tell the ground under a root from what's in front of it, and for the blades' depth
		// test (D24 or D32, one sample): the depth plane alone, back in its state straight after; nothing else
		// touches the game's depth buffer
		const D3D12_RESOURCE_DESC ds_desc = t.ds->GetDesc();
		uint32_t depth_kind = 0, depth_pitch = 0;
		const DXGI_FORMAT df = ds_desc.Format;
		const bool d24 = df == DXGI_FORMAT_D24_UNORM_S8_UINT || df == DXGI_FORMAT_R24G8_TYPELESS;
		const bool d32 = df == DXGI_FORMAT_D32_FLOAT || df == DXGI_FORMAT_R32_TYPELESS || df == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || df == DXGI_FORMAT_R32G8X24_TYPELESS;
		if ((d24 || d32) && ds_desc.SampleDesc.Count == 1 && ds_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
			if ((depth_pitch = copy_out(list, _depth, t.ds, t.ds_state)) != 0)
			{
				depth_kind = d24 ? 1 : 2;
				transition(list, t.ds, D3D12_RESOURCE_STATE_COPY_SOURCE, t.ds_state, 0);
			}

		float constants[kConstants] = {};
		multiply(cam.view, cam.proj, constants);
		constants[16] = eye[0];
		constants[17] = eye[1];
		constants[18] = eye[2];
		constants[19] = l.distance;
		constants[20] = l.color[0];
		constants[21] = l.color[1];
		constants[22] = l.color[2];
		constants[23] = l.time;
		constants[24] = l.height;
		constants[25] = l.width;
		constants[26] = static_cast<float>(tuft);
		const uint32_t ground_info[4] = { ground_pitch, static_cast<uint32_t>(rt_desc.Width), rt_desc.Height, ground_kind };
		std::memcpy(&constants[28], ground_info, sizeof(ground_info));
		// The actors near, into this frame's upload buffer (the slots go round: earlier frames may still read theirs)
		uint32_t actor_count = 0;
		ID3D12Resource *actor_buffer = nullptr;
		float *actor_data = nullptr;
		const bool sun_map = l.sun_map && sun_shadow > 0.0f && _shadow_geo != nullptr && shading;
		if ((l.actors != nullptr && l.actor_count != 0) || sun_map || lamp_count != 0 || haze || wet || falling || dome || wearing || wx_any)
		{
			const uint32_t slot = _actor_slot++ % kActorSlots;
			if (_actors[slot] == nullptr)
			{
				D3D12_HEAP_PROPERTIES heap = {};
				heap.Type = D3D12_HEAP_TYPE_UPLOAD;
				D3D12_RESOURCE_DESC desc = {};
				desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				desc.Width = kActorBytes;
				desc.Height = 1;
				desc.DepthOrArraySize = 1;
				desc.MipLevels = 1;
				desc.SampleDesc.Count = 1;
				desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				void *mapped = nullptr;
				const D3D12_RANGE none = { 0, 0 };
				if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_actors[slot]))) ||
					FAILED(_actors[slot]->Map(0, &none, &mapped)))
					release(_actors[slot]);
				else
				{
					_actors[slot]->SetName(L"worldfx: actors");
					_actor_data[slot] = static_cast<float *>(mapped); // (stays mapped: an upload heap may)
				}
			}
			if (_actors[slot] != nullptr)
			{
				actor_count = l.actors != nullptr ? std::min<uint32_t>(l.actor_count, kMaxActors) : 0;
				if (actor_count != 0)
					std::memcpy(_actor_data[slot], l.actors, size_t(actor_count) * 16);
				actor_buffer = _actors[slot];
				actor_data = _actor_data[slot];
			}
		}
		// The lamps after the shadow map's parameters, in view space (as the shading pass rebuilds each pixel's
		// point), all kMaxLamps of them: the ones not lit far off with no glow (crowd's rule bit 16 says they're there)
		uint32_t lamps_written = 0;
		if (lamp_count != 0 && actor_data != nullptr && shading)
		{
			float *o = actor_data + kMaxActors * 4 + kSunMapParams;
			o[0] = float(lamp_count);
			o[1] = std::clamp(l.lamp_shadows, 0.0f, 1.0f);
			o[2] = o[3] = 0.0f;
			for (uint32_t i = 0; i < kMaxLamps; ++i)
			{
				float *q = o + 4 + i * 8;
				const float *s = i < lamp_count ? l.lamps + size_t(i) * 8 : nullptr;
				for (int k = 0; s != nullptr && k < 7; ++k)
					if (!std::isfinite(s[k]))
						s = nullptr;
				if (s == nullptr)
				{
					const float unused[8] = { 0.0f, 0.0f, 1e6f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
					std::memcpy(q, unused, sizeof(unused));
					continue;
				}
				for (int k = 0; k < 3; ++k)
					q[k] = s[0] * cam.view[0 * 4 + k] + s[1] * cam.view[1 * 4 + k] + s[2] * cam.view[2 * 4 + k] + cam.view[3 * 4 + k];
				q[3] = std::max(s[3], 0.25f);
				for (int k = 4; k < 7; ++k)
					q[k] = std::clamp(s[k], 0.0f, 4.0f);
				q[7] = std::isfinite(s[7]) && s[7] > 0.5f ? 1.0f : 0.0f; // (casts lamp shadows)
				lamps_written++;
			}
		}
		// The game's fog after the lamps (crowd's rule bit 32 says it's there)
		const bool haze_written = haze && actor_data != nullptr;
		if (haze_written)
			std::memcpy(actor_data + kMaxActors * 4 + kSunMapParams + kLampParams, haze_params, sizeof(haze_params));
		// The wet look's after it (rule bit 64), for the shading pass, with the mask in t0 there
		const bool wet_written = wet && shading && actor_data != nullptr;
		if (wet_written)
		{
			auto colour = [](const float *c, int i) { return std::isfinite(c[i]) ? std::clamp(c[i], 0.0f, 2.0f) : 0.0f; };
			const float params[kWetParams] = {
				cam.view[0], cam.view[1], cam.view[2], std::clamp(l.wet, 0.0f, 1.0f),
				cam.view[8], cam.view[9], cam.view[10], 0.0f,
				_wet_x0, _wet_z0, 1.0f / _wet_square, 0.0f,
				float(_wet_width), float(_wet_height), float(_wet_words_a_row), 0.0f,
				colour(l.wet_light, 0), colour(l.wet_light, 1), colour(l.wet_light, 2), 0.0f,
				colour(l.wet_sheen, 0), colour(l.wet_sheen, 1), colour(l.wet_sheen, 2), 0.0f };
			std::memcpy(actor_data + kMaxActors * 4 + kSunMapParams + kLampParams + kHazeParams, params, sizeof(params));
		}
		// The sky's after that (rule bit 128), for the shading pass
		const bool sky_written = dome && shading && actor_data != nullptr;
		if (sky_written)
		{
			const look::sky_dome &d = l.dome;
			auto amount = [](float v, float most) { return std::isfinite(v) ? std::clamp(v, 0.0f, most) : 0.0f; };
			float params[kSkyParams] = {};
			for (int i = 0; i < 8; ++i)
				for (int k = 0; k < 4; ++k)
					params[i * 4 + k] = k < 3 ? std::clamp(d.stops[i][k], 0.0f, 255.0f) : std::clamp(d.stops[i][3], 0.0f, 1.0f);
			for (int k = 0; k < 3; ++k)
				params[32 + k] = std::isfinite(d.clear[k]) ? std::clamp(d.clear[k], 0.0f, 255.0f) : 0.0f;
			params[35] = float(std::min<uint32_t>(d.segments, 255));
			for (int i = 0; i < 7; ++i)
				params[36 + i] = float(dome_strips(params[i * 4 + 3], params[i * 4 + 7]));
			// (toward the light in view space, as the sun's shadows have it)
			float to_light[3];
			for (int i = 0; i < 3; ++i)
				to_light[i] = l.sun[0] * cam.view[0 * 4 + i] + l.sun[1] * cam.view[1 * 4 + i] + l.sun[2] * cam.view[2 * 4 + i];
			const float len = std::sqrt(to_light[0] * to_light[0] + to_light[1] * to_light[1] + to_light[2] * to_light[2]);
			const bool light_ok = std::isfinite(len) && len > 0.5f;
			const float sky_rows[12] = {
				cam.view[0], cam.view[1], cam.view[2], amount(d.stars, 4.0f),
				cam.view[8], cam.view[9], cam.view[10], light_ok ? amount(d.sun, 4.0f) : 0.0f,
				light_ok ? to_light[0] / len : 0.0f, light_ok ? to_light[1] / len : 0.0f, light_ok ? to_light[2] / len : 1.0f, amount(d.moon, 1.0f) };
			std::memcpy(&params[44], sky_rows, sizeof(sky_rows));
			for (int k = 0; k < 3; ++k)
				params[56 + k] = amount(d.disc[k], 2.0f);
			params[59] = std::isfinite(d.star_turn) ? d.star_turn : 0.0f;
			params[60] = l.time;
			params[61] = d.debug ? 1.0f : 0.0f;
			params[62] = 3.0f; // (a pixel within 3 bytes of the game's dome's colour is repainted, from 3 to 8 less and less)
			params[63] = 8.0f;
			float slopes[8][3];
			sky_slopes(d, slopes);
			for (int i = 0; i < 8; ++i)
				for (int k = 0; k < 3; ++k)
					params[64 + i * 4 + k] = std::isfinite(slopes[i][k]) ? slopes[i][k] : 0.0f;
			// (in the slopes' spare fourth places: the moon's lit side in view space, then how much of it is lit)
			float side[3];
			for (int i = 0; i < 3; ++i)
				side[i] = d.moon_side[0] * cam.view[0 * 4 + i] + d.moon_side[1] * cam.view[1 * 4 + i] + d.moon_side[2] * cam.view[2 * 4 + i];
			for (int i = 0; i < 3; ++i)
				params[64 + i * 4 + 3] = std::isfinite(side[i]) ? side[i] : 0.0f;
			params[64 + 3 * 4 + 3] = amount(d.moon_lit, 1.0f);
			std::memcpy(actor_data + kMaxActors * 4 + kSunMapParams + kLampParams + kHazeParams + kWetParams, params, sizeof(params));
		}
		// The grass's wear after the sky's (rule bit 256), its times on the memory's clock: seconds since the memory was
		// zeroed (a new one at this draw), from 1 (0 is "never")
		const bool wear_written = wearing && actor_data != nullptr;
		if (wear_written)
		{
			if (_memory_fresh || _memory_epoch == 0.0)
				_memory_epoch = l.clock;
			const float now = float(l.clock - _memory_epoch) + 1.0f;
			float params[kWearParams] = {};
			params[0] = now;
			params[1] = float(std::clamp(l.clock - _last_clock, 0.0, 0.25)); // (the frame's length, a stall counted as a quarter second)
			params[2] = std::isfinite(l.wear) ? std::clamp(l.wear, 0.0f, 2.0f) : 0.0f;
			const uint32_t fighting = actor_count >= 32 ? l.fighting : l.fighting & ((1u << actor_count) - 1u);
			std::memcpy(&params[3], &fighting, 4);
			uint32_t cuts = 0;
			for (uint32_t i = 0; l.cuts != nullptr && i < std::min<uint32_t>(l.cut_count, kMaxCuts); ++i)
			{
				const float *c = l.cuts + size_t(i) * 8;
				bool ok = c[7] >= 0.0f && c[7] <= float(kCutKeep);
				for (int k = 0; k < 8 && ok; ++k)
					ok = std::isfinite(c[k]);
				if (!ok)
					continue;
				float *q = &params[8 + cuts * 8];
				std::memcpy(q, c, 7 * sizeof(float));
				q[3] = std::clamp(c[3], 0.1f, 20.0f);
				q[7] = now - c[7];
				cuts++;
			}
			params[4] = float(cuts);
			std::memcpy(actor_data + kMaxActors * 4 + kSunMapParams + kLampParams + kHazeParams + kWetParams + kSkyParams, params, sizeof(params));
			stats.wear = true;
			stats.cuts = cuts;
		}
		if (std::isfinite(l.clock) && l.clock > 0.0)
			_last_clock = l.clock;
		// The weather of its own's after the shadow map's drawings before the last (rule bit 512): grass.hlsl kWx has the
		// layout (each part as look::weather_fx has it, kept to its range; what isn't a number, none)
		const bool wx_written = wx_any && actor_data != nullptr;
		if (wx_written)
		{
			auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
			auto colour = [](const float *c, int i) { return std::isfinite(c[i]) ? std::clamp(c[i], 0.0f, 2.0f) : 0.0f; };
			float params[kWxParams] = {};
			const float wl = std::sqrt(wx.wind[0] * wx.wind[0] + wx.wind[1] * wx.wind[1]);
			const bool wind_ok = std::isfinite(wl) && wl > 1e-3f;
			params[0] = (wind_ok ? wx.wind[0] / wl : 1.0f) * wx_wind;
			params[1] = (wind_ok ? wx.wind[1] / wl : 0.0f) * wx_wind;
			params[2] = wx_wind;
			for (int i = 0; i < 3; ++i)
			{
				params[4 + i] = cam.view[i];
				params[8 + i] = cam.view[8 + i];
			}
			params[7] = finite(wx.ground_y, eye[1] + 2.0f); // (y is down: the ground under the camera, below it)
			params[11] = l.time;
			params[12] = part(wx.cloud_cover, 1.0f);
			params[13] = part(wx.cloud_thick, 1.0f);
			params[14] = part(wx.cloud_veil, 1.0f);
			params[15] = wx_stars;
			for (int i = 0; i < 4; ++i)
				params[16 + i] = finite(wx.cloud_drift[i], 0.0f);
			for (int i = 0; i < 3; ++i)
			{
				params[20 + i] = colour(wx.cloud_lit, i);
				params[24 + i] = colour(wx.cloud_shade, i);
				params[28 + i] = colour(wx.veil_colour, i);
				params[40 + i] = colour(wx.dust_colour, i);
				params[48 + i] = colour(wx.gloom_colour, i);
			}
			params[23] = part(wx.sky_dark, 1.0f);
			params[27] = part(wx.aurora, 2.0f);
			params[32] = wx_flash;
			params[33] = wx_bolt;
			params[34] = finite(wx.bolt_bearing, 0.0f);
			params[35] = std::clamp(finite(wx.bolt_top, 0.3f), 0.05f, 1.2f);
			params[36] = finite(wx.bolt_seed, 0.0f);
			params[43] = wx_dust;
			params[44] = wx_streaks;
			params[45] = 3.0f + 4.0f * wx_wind; // (how fast the streaks blow along the ground: yalms a second)
			params[51] = wx_gloom;
			params[52] = wx_heat;
			params[53] = 35.0f; // (the shimmer from 35 yalms off, whole by 110)
			params[54] = 110.0f;
			params[56] = finite(wx.bank_drift[0], 0.0f);
			params[57] = finite(wx.bank_drift[1], 0.0f);
			std::memcpy(actor_data + kWxOffset, params, sizeof(params));
			stats.weather = true;
		}
		const uint32_t rules = (l.rule_in_front ? 1u : 0u) | (l.rule_outliers ? 2u : 0u) | (l.sun_characters ? 4u : 0u) | (lamps_written != 0 ? 16u : 0u) |
			(haze_written ? 32u : 0u) | (wet_written ? 64u : 0u) | (sky_written ? 128u : 0u) | (wear_written ? 256u : 0u) | (wx_written ? 512u : 0u);
		const uint32_t crowd = actor_count | (rules << 16);
		std::memcpy(&constants[32], &crowd, 4);
		// Toward the sun in view space (row vectors: v * the view's rotation)
		for (int i = 0; i < 3; ++i)
			constants[33 + i] = l.sun[0] * cam.view[0 * 4 + i] + l.sun[1] * cam.view[1 * 4 + i] + l.sun[2] * cam.view[2 * 4 + i];
		const uint32_t depth_info[4] = { depth_pitch, static_cast<uint32_t>(ds_desc.Width), ds_desc.Height, depth_kind };
		std::memcpy(&constants[36], depth_info, sizeof(depth_info));
		// (d = _33 / _34 + _43 / w for w = view z * _34; the game's projection is right-handed: _33 = _34 = -1)
		constants[40] = cam.proj[11] != 0.0f ? cam.proj[10] / cam.proj[11] : cam.proj[10];
		constants[41] = cam.proj[14];
		constants[42] = cam.proj[0];
		constants[43] = cam.proj[5];
		constants[44] = cam.view[4];
		constants[45] = cam.view[5];
		constants[46] = cam.view[6];
		constants[47] = cam.proj[11];
		constants[48] = l.ao_strength;
		constants[49] = l.ao_radius;
		constants[50] = l.relief;
		constants[51] = sun_shadow;

		// The game's visibility tests, against the world's depth as it ends (before anything of ours is drawn)
		if (vis && depth_kind != 0)
		{
			stats.vis_boxes = draw_vis(list, constants, error);
			stats.bound = stats.bound || stats.vis_boxes != 0;
		}

		D3D12_CPU_DESCRIPTOR_HANDLE world_rtv = {};
		if (!world_rtv_for(t, world_rtv, error))
		{
			transition(list, t.rt, D3D12_RESOURCE_STATE_RENDER_TARGET, t.rt_state);
			finish_timing(list, false);
			return stats;
		}
		const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(rt_desc.Width), static_cast<float>(rt_desc.Height), 0.0f, 1.0f };
		const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(rt_desc.Width), static_cast<LONG>(rt_desc.Height) };
		// The air pass (dust, sand and wind streaks, gloom's fog banks), then the lightning's flash, last of all: over the
		// world as everything has been drawn into it (the grass, the water, the rain), so all of it is in the same air and
		// light; the grass learned its colours before them (the copy after the shading pass), so none of it is fogged or
		// lit twice. Their pixels read the depth copy (t3), the parameters (t2) and the world's copy for names facelift drew
		// (t1: names are left as they are, as the shading pass leaves them).
		auto air_pass = [&]() {
			if (!air_wanted || !wx_written || depth_kind == 0)
				return;
			const bool air = wx_dust > 0.0f || wx_streaks > 0.0f || wx_gloom > 0.0f;
			ID3D12PipelineState *const air_pso = air ? air_pipeline_for(t, false, error) : nullptr;
			ID3D12PipelineState *const flash_pso = wx_flash > 0.0f ? air_pipeline_for(t, true, error) : nullptr;
			if (air_pso == nullptr && flash_pso == nullptr)
				return;
			section(list, L"worldfx: air");
			stats.bound = true;
			list->SetGraphicsRootSignature(_root);
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			list->OMSetRenderTargets(1, &world_rtv, FALSE, nullptr);
			list->RSSetViewports(1, &viewport);
			list->RSSetScissorRects(1, &scissor);
			list->SetGraphicsRoot32BitConstants(0, kConstants, constants, 0);
			// (t0 isn't read: the depth copy's address; t1 without a copy of the world isn't read either, ground.w being 0)
			list->SetGraphicsRootShaderResourceView(1, _depth.buffer->GetGPUVirtualAddress());
			list->SetGraphicsRootShaderResourceView(2, (ground_kind != 0 ? _ground.buffer : _depth.buffer)->GetGPUVirtualAddress());
			list->SetGraphicsRootShaderResourceView(4, actor_buffer->GetGPUVirtualAddress());
			list->SetGraphicsRootShaderResourceView(5, _depth.buffer->GetGPUVirtualAddress());
			for (ID3D12PipelineState *pso : { air_pso, flash_pso })
				if (pso != nullptr)
				{
					list->SetPipelineState(pso);
					list->DrawInstanced(3, 1, 0, 0);
				}
			stats.air = true;
		};

		// The shading pass: ambient occlusion, surface relief and the sun's shadows into the world, from the
		// copies; then the world copied again, so the grass learns its colours from the shaded ground. The sun's
		// shadows are worked out first, at half size, into their mask (no render target), which the shading
		// pass blurs in.
		if (shading && ground_kind != 0 && depth_kind != 0)
		{
			ID3D12PipelineState *shade_pso = shade_pipeline_for(t, error);
			if (shade_pso == nullptr || !sun_mask_for(rt_desc, error))
			{
				transition(list, t.rt, D3D12_RESOURCE_STATE_RENDER_TARGET, t.rt_state);
				finish_timing(list, false);
				return stats;
			}
			ID3D12PipelineState *const sun_pso = sun_shadow > 0.0f ? sun_pipeline(error) : nullptr;
			if (sun_pso == nullptr)
				constants[51] = 0.0f; // (no mask worked out: none read)
			stats.bound = true;
			// The sun's shadow map: drawn again when it has to be, then its parameters for this camera after the
			// actors (crowd's rule bit 8 says they're there)
			if (sun_pso != nullptr && sun_map && actor_data != nullptr)
			{
				stats.sun_map_cascades = draw_sun_map(list, eye, l.sun, l.clock, stats.sun_map_triangles, error);
				if (_cascades[0].valid && _cascades[1].valid && _sun_map_copy != nullptr)
				{
					float *const prev = actor_data + kMaxActors * 4 + kSunMapParams + kLampParams + kHazeParams + kWetParams + kSkyParams + kWearParams;
					sun_map_params(cam, l.clock, actor_data + kMaxActors * 4, prev);
					stats.sun_map_fade[0] = prev[24];
					stats.sun_map_fade[1] = prev[25];
					const uint32_t with_map = crowd | (8u << 16);
					std::memcpy(&constants[32], &with_map, 4);
					stats.sun_map = true;
				}
			}
			list->SetGraphicsRootSignature(_root);
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			list->SetGraphicsRoot32BitConstants(0, kConstants, constants, 0);
			list->SetGraphicsRootShaderResourceView(2, _ground.buffer->GetGPUVirtualAddress());
			list->SetGraphicsRootUnorderedAccessView(6, _sun_mask->GetGPUVirtualAddress());
			// (no actors: t2 still gets a valid address, the depth copy's, never read)
			list->SetGraphicsRootShaderResourceView(4, actor_buffer != nullptr ? actor_buffer->GetGPUVirtualAddress() : _depth.buffer->GetGPUVirtualAddress());
			list->SetGraphicsRootShaderResourceView(5, _depth.buffer->GetGPUVirtualAddress());
			if (sun_pso != nullptr)
			{
				section(list, L"worldfx: sun shadows");
				// (t0: the shadow map's copy; without it, still a valid address, the depth copy's, never read)
				list->SetGraphicsRootShaderResourceView(1, (stats.sun_map ? _sun_map_copy : _depth.buffer)->GetGPUVirtualAddress());
				const UINT half_w = UINT((rt_desc.Width + 1) / 2), half_h = (rt_desc.Height + 1) / 2;
				const D3D12_VIEWPORT half_viewport = { 0.0f, 0.0f, static_cast<float>(half_w), static_cast<float>(half_h), 0.0f, 1.0f };
				const D3D12_RECT half_scissor = { 0, 0, static_cast<LONG>(half_w), static_cast<LONG>(half_h) };
				list->SetPipelineState(sun_pso);
				list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
				list->RSSetViewports(1, &half_viewport);
				list->RSSetScissorRects(1, &half_scissor);
				list->DrawInstanced(3, 1, 0, 0);
				D3D12_RESOURCE_BARRIER written = {};
				written.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
				written.UAV.pResource = _sun_mask;
				list->ResourceBarrier(1, &written);
				stats.sun = true;
			}
			section(list, L"worldfx: shading pass");
			// (t0 in the shading pass: the wet mask when it's read; else as the sun pass left it, or still unread)
			if (wet_written)
				list->SetGraphicsRootShaderResourceView(1, _wet->GetGPUVirtualAddress());
			else if (sun_pso == nullptr)
				list->SetGraphicsRootShaderResourceView(1, _depth.buffer->GetGPUVirtualAddress());
			list->SetPipelineState(shade_pso);
			list->OMSetRenderTargets(1, &world_rtv, FALSE, nullptr);
			list->RSSetViewports(1, &viewport);
			list->RSSetScissorRects(1, &scissor);
			list->DrawInstanced(3, 1, 0, 0);
			stats.shaded = true;
			stats.lamps = lamps_written;
			stats.wet = wet_written;
			stats.sky = sky_written;
			stats.shimmer = shimmer && wx_written;
			if ((run_count != 0 || (water_shade && (water_run_count != 0 || sea_here))) && copy_out(list, _ground, t.rt, D3D12_RESOURCE_STATE_RENDER_TARGET) != 0)
				transition(list, t.rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}
		// The water: on the surface rebuilt over the zone's water collision (tools/bake_water.py), each pixel
		// worked out from the copies (what the game drew there and around it) and written whole, so where the
		// collision overlaps itself, drawing it twice comes out the same. With a sea: the baked water's whole pixels
		// (marked), the sea where they aren't, the baked water's fading ones over that (water_pipeline_for)
		ID3D12PipelineState *const water_pso = water_shade && (water_run_count != 0 || sea_here) && ground_kind != 0 && depth_kind != 0 &&
			blade_depth_for(rt_desc, error) ? water_pipeline_for(t, sea_here ? water_kind::whole : water_kind::plain, error) : nullptr;
		ID3D12PipelineState *const sea_pso = water_pso != nullptr && sea_here ? water_pipeline_for(t, water_kind::sea, error) : nullptr;
		ID3D12PipelineState *const fading_pso = sea_pso != nullptr ? water_pipeline_for(t, water_kind::fading, error) : nullptr;
		if (water_pso != nullptr)
			if (!sea_here || fading_pso != nullptr)
			{
				// Its parameters, into this frame's slot (they go round: earlier frames may still read theirs)
				if (_water_params == nullptr)
				{
					D3D12_HEAP_PROPERTIES heap = {};
					heap.Type = D3D12_HEAP_TYPE_UPLOAD;
					D3D12_RESOURCE_DESC desc = {};
					desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
					desc.Width = kWaterParamBytes * kActorSlots;
					desc.Height = 1;
					desc.DepthOrArraySize = 1;
					desc.MipLevels = 1;
					desc.SampleDesc.Count = 1;
					desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
					void *mapped = nullptr;
					const D3D12_RANGE none = { 0, 0 };
					if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_water_params))) ||
						FAILED(_water_params->Map(0, &none, &mapped)))
						release(_water_params);
					else
					{
						_water_params->SetName(L"worldfx: water parameters");
						_water_param_data = static_cast<uint8_t *>(mapped); // (stays mapped)
					}
				}
				if (_water_params != nullptr)
				{
					const uint32_t slot = _water_param_slot++ % kActorSlots;
					// ([3].w flags: 1 the fog colour in [4] is known, 2 the rings are in [8..] (as many as [5].x),
					// two float4s each, the add-on's 8 floats as they are; 4 the game's fog in [6] and [7], as in the
					// actors' buffer)
					const uint32_t ripple_count = l.ripples != nullptr ? std::min<uint32_t>(l.ripple_count, kMaxRipples) : 0;
					float params[kWaterParamBytes / 4] = {
						l.sun[0], l.sun[1], l.sun[2], l.glint * weather_sun,
						l.sky[0], l.sky[1], l.sky[2], l.water_reflect,
						l.water_ripples, l.water_tint, l.time, water_reach,
						l.sun_color[0], l.sun_color[1], l.sun_color[2], float((l.fog_known ? 1 : 0) | 2 | (haze ? 4 : 0)),
						l.fog[0], l.fog[1], l.fog[2], 0.0f,
						float(ripple_count), l.water_rings, rain ? rain_amount * std::clamp(l.rain_rings, 0.0f, 2.0f) : 0.0f, 0.0f,
						haze_params[0], haze_params[1], haze_params[2], haze_params[3],
						haze_params[4], haze_params[5], haze_params[6], haze_params[7] };
					if (ripple_count != 0)
						std::memcpy(&params[32], l.ripples, size_t(ripple_count) * 8 * sizeof(float));
					std::memcpy(_water_param_data + size_t(slot) * kWaterParamBytes, params, sizeof(params));
					section(list, L"worldfx: water");
					stats.bound = true;
					const D3D12_CPU_DESCRIPTOR_HANDLE water_dsv = _dsv_heap->GetCPUDescriptorHandleForHeapStart();
					list->ClearDepthStencilView(water_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
					list->SetGraphicsRootSignature(_root);
					list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					list->OMSetRenderTargets(1, &world_rtv, FALSE, &water_dsv);
					list->RSSetViewports(1, &viewport);
					list->RSSetScissorRects(1, &scissor);
					list->SetGraphicsRoot32BitConstants(0, kConstants, constants, 0);
					list->SetGraphicsRootShaderResourceView(2, _ground.buffer->GetGPUVirtualAddress());
					list->SetGraphicsRootShaderResourceView(4, _water_params->GetGPUVirtualAddress() + UINT64(slot) * kWaterParamBytes);
					list->SetGraphicsRootShaderResourceView(5, _depth.buffer->GetGPUVirtualAddress());
					// The baked water's runs with blade.z its part (0 all of it; with a sea 1 its whole pixels, 2 its fading
					// ones: grass.hlsl water_ps). (SV_VertexID counts from 0 in every draw, whatever its start vertex, on
					// WARP and the GPU alike: each run's first corner goes in as a constant, blade.w)
					auto baked = [&](ID3D12PipelineState *pso, float part, bool count) {
						if (_water == nullptr || water_run_count == 0)
							return;
						UINT bits = 0;
						std::memcpy(&bits, &part, 4);
						list->SetPipelineState(pso);
						list->SetGraphicsRootShaderResourceView(1, _water->GetGPUVirtualAddress());
						list->SetGraphicsRoot32BitConstant(0, bits, kFirstBladeConstant - 1);
						for (uint32_t i = 0; i < water_run_count; ++i)
						{
							list->SetGraphicsRoot32BitConstant(0, water_runs[i].first, kFirstBladeConstant);
							list->DrawInstanced(water_runs[i].count, 1, 0, 0);
							if (count)
								stats.water_corners += water_runs[i].count;
						}
					};
					baked(water_pso, sea_here ? 1.0f : 0.0f, true);
					if (sea_here)
					{
						// The sea's sheets, whole (they're few), where the baked water didn't mark the stencil
						section(list, L"worldfx: sea");
						list->SetPipelineState(sea_pso);
						list->SetGraphicsRootShaderResourceView(1, _sea->GetGPUVirtualAddress());
						list->SetGraphicsRoot32BitConstant(0, 0u, kFirstBladeConstant - 1);
						list->SetGraphicsRoot32BitConstant(0, 0u, kFirstBladeConstant);
						list->DrawInstanced(_sea_corners, 1, 0, 0);
						stats.sea_corners = _sea_corners;
						baked(fading_pso, 2.0f, false);
					}
				}
			}
		// The water view: the zone's water collision over the world, each spot tinted by whether what the game
		// drew there is at the water (blue, cyan) or in front of it (red)
		if (l.water_debug && water_run_count != 0 && depth_kind != 0)
			if (ID3D12PipelineState *const debug_pso = water_pipeline_for(t, water_kind::debug, error))
			{
				section(list, L"worldfx: water view");
				stats.bound = true;
				list->SetGraphicsRootSignature(_root);
				list->SetPipelineState(debug_pso);
				list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				list->OMSetRenderTargets(1, &world_rtv, FALSE, nullptr);
				list->RSSetViewports(1, &viewport);
				list->RSSetScissorRects(1, &scissor);
				list->SetGraphicsRoot32BitConstants(0, kConstants, constants, 0);
				list->SetGraphicsRootShaderResourceView(1, _water->GetGPUVirtualAddress());
				list->SetGraphicsRootShaderResourceView(5, _depth.buffer->GetGPUVirtualAddress());
				// (each run's first corner as a constant, blade.w: see the water pass)
				for (uint32_t i = 0; i < water_run_count; ++i)
				{
					list->SetGraphicsRoot32BitConstant(0, water_runs[i].first, kFirstBladeConstant);
					list->DrawInstanced(water_runs[i].count, 1, 0, 0);
					if (!water_shade)
						stats.water_corners += water_runs[i].count;
				}
			}
		_shade_end[_time_slot] = _mark_count[_time_slot]; // (the copies and shading part ends at the next marker)
		// (no grass, fireflies or rain without the depth copy: it's their test against the world)
		if ((run_count == 0 && firefly_run_count == 0 && !falling) || depth_kind == 0 || !blade_depth_for(rt_desc, error))
		{
			air_pass();
			transition(list, t.rt, D3D12_RESOURCE_STATE_RENDER_TARGET, t.rt_state);
			gpu_crash::mark(list, L"worldfx: end");
			finish_timing(list, true);
			return stats;
		}
		const D3D12_CPU_DESCRIPTOR_HANDLE blade_dsv = _dsv_heap->GetCPUDescriptorHandleForHeapStart();
		list->ClearDepthStencilView(blade_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		if (run_count != 0)
		{
			// A new colour memory starts at zero (nothing known). A new buffer isn't reliably zeroed (WARP hands
			// back freed memory), so zeros are copied in from an upload buffer of them.
			const UINT64 zero_chunk = 1 << 20;
			if (_memory_fresh && _zeros == nullptr)
			{
				D3D12_HEAP_PROPERTIES heap = {};
				heap.Type = D3D12_HEAP_TYPE_UPLOAD;
				D3D12_RESOURCE_DESC desc = {};
				desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				desc.Width = zero_chunk;
				desc.Height = 1;
				desc.DepthOrArraySize = 1;
				desc.MipLevels = 1;
				desc.SampleDesc.Count = 1;
				desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				void *mapped = nullptr;
				const D3D12_RANGE none = { 0, 0 };
				if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_zeros))) ||
					FAILED(_zeros->Map(0, &none, &mapped)))
				{
					release(_zeros);
					error = "colour memory: no buffer of zeros";
					transition(list, t.rt, D3D12_RESOURCE_STATE_RENDER_TARGET, t.rt_state);
					finish_timing(list, true);
					return stats;
				}
				std::memset(mapped, 0, static_cast<size_t>(zero_chunk));
				_zeros->Unmap(0, nullptr);
				_zeros->SetName(L"worldfx: zeros");
			}

			section(list, L"worldfx: grass");
			stats.bound = true;
			list->SetGraphicsRootSignature(_root);
			list->SetPipelineState(pso);
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			list->OMSetRenderTargets(1, &world_rtv, FALSE, &blade_dsv);
			list->RSSetViewports(1, &viewport);
			list->RSSetScissorRects(1, &scissor);
			list->SetGraphicsRoot32BitConstants(0, kConstants, constants, 0);
			list->SetGraphicsRootShaderResourceView(1, _buffer->GetGPUVirtualAddress());
			// (without a copy, t1 still gets a valid address, the blades', never read since ground.w is 0)
			list->SetGraphicsRootShaderResourceView(2, ground_kind != 0 ? _ground.buffer->GetGPUVirtualAddress() : _buffer->GetGPUVirtualAddress());
			if (_memory_fresh)
			{
				transition(list, _memory, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
				const UINT64 size = UINT64(_blade_count) * kMemoryStride;
				for (UINT64 at = 0; at < size; at += zero_chunk)
					list->CopyBufferRegion(_memory, at, _zeros, 0, std::min(zero_chunk, size - at));
				transition(list, _memory, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				_memory_fresh = false;
			}

			// The colour memory: last frame's writes before this frame's reads
			D3D12_RESOURCE_BARRIER uav = {};
			uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			uav.UAV.pResource = _memory;
			list->ResourceBarrier(1, &uav);
			list->SetGraphicsRootUnorderedAccessView(3, _memory->GetGPUVirtualAddress());
			// (no actors: t2 still gets a valid address, the blades', never read)
			list->SetGraphicsRootShaderResourceView(4, actor_buffer != nullptr ? actor_buffer->GetGPUVirtualAddress() : _buffer->GetGPUVirtualAddress());
			list->SetGraphicsRootShaderResourceView(5, _depth.buffer->GetGPUVirtualAddress());
			for (uint32_t i = 0; i < run_count; ++i)
			{
				const float run_tuft = static_cast<float>(runs[i].tuft);
				UINT bits = 0;
				std::memcpy(&bits, &run_tuft, 4);
				list->SetGraphicsRoot32BitConstant(0, bits, kFirstBladeConstant - 1);
				list->SetGraphicsRoot32BitConstant(0, runs[i].first, kFirstBladeConstant);
				list->DrawInstanced(kVerticesPerBlade, runs[i].count * runs[i].tuft, 0, 0);
				stats.blades += runs[i].count * runs[i].tuft;
				stats.draws++;
			}
		}

		// Fireflies, after the blades (hidden behind them): their own constants in the shading pass's place
		if (firefly_run_count != 0)
			if (ID3D12PipelineState *const firefly_pso = firefly_pipeline_for(t, error))
			{
				section(list, L"worldfx: fireflies");
				stats.bound = true;
				float firefly_constants[kConstants];
				std::memcpy(firefly_constants, constants, sizeof(constants));
				firefly_constants[48] = l.night;
				firefly_constants[49] = l.firefly_size;
				firefly_constants[50] = l.firefly_distance;
				firefly_constants[51] = 0.0f;
				list->SetGraphicsRootSignature(_root);
				list->SetPipelineState(firefly_pso);
				list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				list->OMSetRenderTargets(1, &world_rtv, FALSE, &blade_dsv);
				list->RSSetViewports(1, &viewport);
				list->RSSetScissorRects(1, &scissor);
				list->SetGraphicsRoot32BitConstants(0, kConstants, firefly_constants, 0);
				list->SetGraphicsRootShaderResourceView(1, _buffer->GetGPUVirtualAddress());
				// (t1 isn't read; t2 without actors gets the blades' address, never read)
				list->SetGraphicsRootShaderResourceView(2, ground_kind != 0 ? _ground.buffer->GetGPUVirtualAddress() : _buffer->GetGPUVirtualAddress());
				list->SetGraphicsRootShaderResourceView(4, actor_buffer != nullptr ? actor_buffer->GetGPUVirtualAddress() : _buffer->GetGPUVirtualAddress());
				list->SetGraphicsRootShaderResourceView(5, _depth.buffer->GetGPUVirtualAddress());
				const float stride = static_cast<float>(firefly_stride);
				UINT bits = 0;
				std::memcpy(&bits, &stride, 4);
				list->SetGraphicsRoot32BitConstant(0, bits, kFirstBladeConstant - 1);
				for (uint32_t i = 0; i < firefly_run_count; ++i)
				{
					const UINT count = firefly_runs[i].count / firefly_stride;
					if (count == 0)
						continue;
					list->SetGraphicsRoot32BitConstant(0, firefly_runs[i].first, kFirstBladeConstant);
					list->DrawInstanced(6, count, 0, 0);
					stats.fireflies += count;
				}
			}

		// Rain: drops falling in columns around the camera, each ending at the first surface under it (the zone's rain
		// map: its window around the camera in t0) or on someone near (t2), hidden by what's in front (the depth copy
		// and the blades); then the splashes where they end near the camera. Their own constants in the grass's place
		// (grass.hlsl, "Rain").
		if (rain && actor_buffer != nullptr)
		{
			bool moved = false;
			ID3D12PipelineState *const rain_pso = rain_window_for(eye, moved, error) ? rain_pipeline_for(t, fall::rain, error) : nullptr;
			ID3D12PipelineState *const splash_pso = rain_pso != nullptr && l.rain_splashes > 0.0f ? rain_pipeline_for(t, fall::splash, error) : nullptr;
			if (rain_pso != nullptr)
			{
				section(list, L"worldfx: rain");
				stats.bound = true;
				stats.rain_map_moved = moved;
				float rain_constants[kConstants];
				std::memcpy(rain_constants, constants, sizeof(constants));
				auto finite = [](float v, float lo, float hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.0f; };
				const UINT columns = static_cast<UINT>(std::ceil(2.0f * kRainRadius / kRainSpacing));
				const UINT splash_columns = static_cast<UINT>(std::ceil(2.0f * kSplashRadius / kRainSpacing));
				rain_constants[19] = kRainRadius;
				for (int i = 0; i < 3; ++i)
					rain_constants[20 + i] = finite(l.rain_light[i], 0.0f, 2.0f);
				rain_constants[24] = kRainSpeed;
				rain_constants[25] = finite(l.rain_slant[0], -2.0f, 2.0f);
				rain_constants[26] = finite(l.rain_slant[1], -2.0f, 2.0f);
				std::memcpy(&rain_constants[27], &columns, 4);
				// (crowd.yzw: the window's corner, x and z, and its squares' size)
				rain_constants[33] = _rain.x0 + float(_rain_window_i) * _rain.square;
				rain_constants[34] = _rain.z0 + float(_rain_window_j) * _rain.square;
				rain_constants[35] = _rain.square;
				rain_constants[48] = rain_amount;
				rain_constants[49] = finite(l.rain_splashes, 0.0f, 2.0f);
				rain_constants[50] = kRainSpacing;
				rain_constants[51] = 1.0f;
				list->SetGraphicsRootSignature(_root);
				list->SetPipelineState(rain_pso);
				list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				list->OMSetRenderTargets(1, &world_rtv, FALSE, &blade_dsv);
				list->RSSetViewports(1, &viewport);
				list->RSSetScissorRects(1, &scissor);
				list->SetGraphicsRoot32BitConstants(0, kConstants, rain_constants, 0);
				list->SetGraphicsRootShaderResourceView(1, _rain_window->GetGPUVirtualAddress());
				// (t1 isn't read)
				list->SetGraphicsRootShaderResourceView(2, ground_kind != 0 ? _ground.buffer->GetGPUVirtualAddress() : _rain_window->GetGPUVirtualAddress());
				list->SetGraphicsRootShaderResourceView(4, actor_buffer->GetGPUVirtualAddress());
				list->SetGraphicsRootShaderResourceView(5, _depth.buffer->GetGPUVirtualAddress());
				list->DrawInstanced(6, columns * columns * kRainLayers, 0, 0);
				stats.rain_drops = columns * columns * kRainLayers;
				if (splash_pso != nullptr)
				{
					section(list, L"worldfx: splashes");
					rain_constants[19] = kSplashRadius;
					std::memcpy(&rain_constants[27], &splash_columns, 4);
					list->SetPipelineState(splash_pso);
					list->SetGraphicsRoot32BitConstants(0, kConstants, rain_constants, 0);
					list->DrawInstanced(12, splash_columns * splash_columns * kRainLayers, 0, 0);
					stats.splashes = splash_columns * splash_columns * kRainLayers;
				}
			}
		}

		// Snow: flakes in the rain's columns, ending where drops do (the same map and people), hidden the same way; the
		// rain's constants with the snow's speed, sway, light and amount (grass.hlsl, "Snow")
		if (snow && actor_buffer != nullptr)
		{
			bool moved = false;
			ID3D12PipelineState *const snow_pso = rain_window_for(eye, moved, error) ? rain_pipeline_for(t, fall::snow, error) : nullptr;
			if (snow_pso != nullptr)
			{
				section(list, L"worldfx: snow");
				stats.bound = true;
				stats.rain_map_moved = stats.rain_map_moved || moved;
				float snow_constants[kConstants];
				std::memcpy(snow_constants, constants, sizeof(constants));
				auto finite = [](float v, float lo, float hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.0f; };
				const UINT columns = static_cast<UINT>(std::ceil(2.0f * kSnowRadius / kRainSpacing));
				snow_constants[19] = kSnowRadius;
				for (int i = 0; i < 3; ++i)
					snow_constants[20 + i] = finite(l.snow_light[i], 0.0f, 2.0f);
				snow_constants[24] = kSnowSpeed;
				snow_constants[25] = finite(l.rain_slant[0], -2.0f, 2.0f);
				snow_constants[26] = finite(l.rain_slant[1], -2.0f, 2.0f);
				std::memcpy(&snow_constants[27], &columns, 4);
				snow_constants[33] = _rain.x0 + float(_rain_window_i) * _rain.square;
				snow_constants[34] = _rain.z0 + float(_rain_window_j) * _rain.square;
				snow_constants[35] = _rain.square;
				snow_constants[48] = snow_amount;
				snow_constants[49] = finite(l.snow_sway, 0.0f, 1.0f);
				snow_constants[50] = kRainSpacing;
				snow_constants[51] = 1.0f;
				list->SetGraphicsRootSignature(_root);
				list->SetPipelineState(snow_pso);
				list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				list->OMSetRenderTargets(1, &world_rtv, FALSE, &blade_dsv);
				list->RSSetViewports(1, &viewport);
				list->RSSetScissorRects(1, &scissor);
				list->SetGraphicsRoot32BitConstants(0, kConstants, snow_constants, 0);
				list->SetGraphicsRootShaderResourceView(1, _rain_window->GetGPUVirtualAddress());
				// (t1 isn't read)
				list->SetGraphicsRootShaderResourceView(2, ground_kind != 0 ? _ground.buffer->GetGPUVirtualAddress() : _rain_window->GetGPUVirtualAddress());
				list->SetGraphicsRootShaderResourceView(4, actor_buffer->GetGPUVirtualAddress());
				list->SetGraphicsRootShaderResourceView(5, _depth.buffer->GetGPUVirtualAddress());
				list->DrawInstanced(6, columns * columns * kSnowLayers, 0, 0);
				stats.snow_flakes = columns * columns * kSnowLayers;
			}
		}

		// Leaves in wind and gales: in the rain's columns (the same map and people), blown far across as they fall, near
		// the ground only, hidden as the snow; the snow's constants with the leaves' speed, the wind's slant, their light
		// and amount (grass.hlsl, "Leaves")
		if (leaves && actor_buffer != nullptr)
		{
			bool moved = false;
			ID3D12PipelineState *const leaf_pso = rain_window_for(eye, moved, error) ? rain_pipeline_for(t, fall::leaf, error) : nullptr;
			if (leaf_pso != nullptr)
			{
				section(list, L"worldfx: leaves");
				stats.bound = true;
				stats.rain_map_moved = stats.rain_map_moved || moved;
				float leaf_constants[kConstants];
				std::memcpy(leaf_constants, constants, sizeof(constants));
				auto finite = [](float v, float lo, float hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.0f; };
				const UINT columns = static_cast<UINT>(std::ceil(2.0f * kLeafRadius / kRainSpacing));
				// (blown across as they fall: 3 yalms for each yalm fallen in a wind, about 4 in gales, toward where it blows)
				const float wl = std::sqrt(wx.wind[0] * wx.wind[0] + wx.wind[1] * wx.wind[1]);
				const float across = std::isfinite(wl) && wl > 1e-3f ? (2.0f + std::min(wx_wind, 2.0f)) / wl : 0.0f;
				leaf_constants[19] = kLeafRadius;
				for (int i = 0; i < 3; ++i)
					leaf_constants[20 + i] = finite(wx.leaf_light[i], 0.0f, 2.0f);
				leaf_constants[24] = kLeafSpeed;
				leaf_constants[25] = finite(wx.wind[0] * across, -6.0f, 6.0f);
				leaf_constants[26] = finite(wx.wind[1] * across, -6.0f, 6.0f);
				std::memcpy(&leaf_constants[27], &columns, 4);
				leaf_constants[33] = _rain.x0 + float(_rain_window_i) * _rain.square;
				leaf_constants[34] = _rain.z0 + float(_rain_window_j) * _rain.square;
				leaf_constants[35] = _rain.square;
				leaf_constants[48] = std::min(wx_leaves * kLeafShare, 1.0f);
				leaf_constants[49] = 0.6f; // (how far they tumble about their path: yalms)
				leaf_constants[50] = kRainSpacing;
				leaf_constants[51] = 1.0f;
				list->SetGraphicsRootSignature(_root);
				list->SetPipelineState(leaf_pso);
				list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				list->OMSetRenderTargets(1, &world_rtv, FALSE, &blade_dsv);
				list->RSSetViewports(1, &viewport);
				list->RSSetScissorRects(1, &scissor);
				list->SetGraphicsRoot32BitConstants(0, kConstants, leaf_constants, 0);
				list->SetGraphicsRootShaderResourceView(1, _rain_window->GetGPUVirtualAddress());
				// (t1 isn't read)
				list->SetGraphicsRootShaderResourceView(2, ground_kind != 0 ? _ground.buffer->GetGPUVirtualAddress() : _rain_window->GetGPUVirtualAddress());
				list->SetGraphicsRootShaderResourceView(4, actor_buffer->GetGPUVirtualAddress());
				list->SetGraphicsRootShaderResourceView(5, _depth.buffer->GetGPUVirtualAddress());
				list->DrawInstanced(6, columns * columns * kLeafLayers, 0, 0);
				stats.leaves = columns * columns * kLeafLayers;
			}
		}

		air_pass();
		transition(list, t.rt, D3D12_RESOURCE_STATE_RENDER_TARGET, t.rt_state);
		gpu_crash::mark(list, L"worldfx: end");
		finish_timing(list, true);
		return stats;
	}
}
