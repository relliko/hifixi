/*
 * The zone files the render tests read, made with HiFi XI's own bakers from this PC's game files (the FFXiMain dump
 * mapped first for the game's key tables: tests/game_dump.hpp), into build\testdata (not in the repo: they're made
 * from Square Enix's data). Only what's missing is made:
 *   build\testdata\grass\100.bin   West Ronfaure's blades (grass_test, the e2e, shade_bench)
 *   build\testdata\rain\104.bin    Jugner Forest's rain map (grass_test: a real rain map read whole)
 *
 *   build\test_data.exe     (from this folder; build.cmd runs it)
 *
 * Exit code: 0 when both are there.
 */

#include "../src/bake/bake.hpp"
#include "../src/bake/dat.hpp"
#include "game_dump.hpp"
#include <cstdio>
#include <cstring>
#include <string>

// World FX's functions and HiFi XI's options, which game.cpp refers to: stubbed
extern "C"
{
	void __cdecl worldfx_grass_actors2(const float *, const uint32_t *, uint32_t) {}
	void __cdecl worldfx_grass_cut(const float *, uint32_t) {}
	void __cdecl worldfx_lamps(const uint32_t *, uint32_t) {}
	void __cdecl worldfx_glows(const uint32_t *, uint32_t) {}
	void __cdecl worldfx_weather(const uint32_t *, uint32_t) {}
	void __cdecl worldfx_moon(uint32_t) {}
	uintptr_t __cdecl worldfx_vis_entry(uint32_t) { return 0; }
	void __cdecl worldfx_vis_setup(uintptr_t, uint32_t) {}
	void __cdecl worldfx_vis_stats(uint32_t out[12]) { std::memset(out, 0, 12 * 4); }
	int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
	const char *__cdecl worldfx_folder() { return ""; }
	int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
	void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
}

namespace
{
	bool exists(const std::wstring &path)
	{
		const DWORD a = GetFileAttributesW(path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const std::wstring folder = L"build\\testdata\\";
	const std::wstring grass = folder + L"grass\\100.bin", rain = folder + L"rain\\104.bin", water = folder + L"water\\104.bin";
	if (exists(grass) && exists(rain))
	{
		std::printf("test data: there already (build\\testdata)\n");
		return 0;
	}
	uintptr_t base = 0;
	if (dump::map(dump::kDumps[0], base) == 0)
	{
		std::printf("test data: the FFXiMain dump couldn't be mapped (its key tables are needed)\n");
		return 1;
	}
	for (const wchar_t *sub : { L"", L"grass", L"rain", L"water" })
		CreateDirectoryW((folder + sub).c_str(), nullptr);
	int failed = 0;
	hifi::bake::result r;
	if (!exists(grass))
	{
		const bool ok = hifi::bake::grass(100, grass, r);
		std::printf("%s  build\\testdata\\grass\\100.bin%s%s\n", ok ? "made" : "FAIL", ok ? "" : ": ", ok ? "" : r.why.c_str());
		failed += ok ? 0 : 1;
	}
	if (!exists(rain))
	{
		// (with the zone's water, as the runner makes it: the rain map marks its squares)
		r = hifi::bake::result();
		const bool water_ok = exists(water) || hifi::bake::water(104, water, r);
		r = hifi::bake::result();
		const bool ok = water_ok && hifi::bake::rain(104, rain, water, r, true);
		std::printf("%s  build\\testdata\\rain\\104.bin%s%s\n", ok ? "made" : "FAIL", ok ? "" : ": ", ok ? "" : r.why.c_str());
		failed += ok ? 0 : 1;
	}
	dump::unmap(base);
	return failed;
}
