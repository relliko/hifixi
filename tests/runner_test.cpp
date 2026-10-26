/*
 * HiFi XI's zone files made on the player's PC (src/bake/runner: the bakers on a thread of their own), into a folder of
 * the test's own, the FFXiMain dump mapped (tests/game_dump.hpp) for the game's key tables:
 *   - a zone entered (West Ronfaure, 100): its grass, water, shadow map and lamps made; its wet ground, rain and sea
 *     (its pond lies inside the zone: no sheet goes on past it), which it has none of, marked .none (not tried again);
 *     the shadow map only because the zone is in worldfx-sunlit.txt;
 *   - a zone with only meshes under open sky (1): its shadow map, every other kind .none;
 *   - entered again: nothing made twice; but a lamps file from before the hour (LMP1) made again, as LMP2;
 *   - each file the same bytes as the baker's own, made straight (the bakers' tests check those against the Python);
 *   - status while it works and after, and stop() with the thread ending.
 *
 *   build\runner_test.exe     (from this folder)
 */

#include "../src/bake/bake.hpp"
#include "../src/bake/dat.hpp"
#include "../src/bake/runner.hpp"
#include "../src/render/worldfx_core.hpp"
#include "game_dump.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
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

	bool exists(const std::wstring &path)
	{
		const DWORD a = GetFileAttributesW(path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	FILETIME written(const std::wstring &path)
	{
		WIN32_FILE_ATTRIBUTE_DATA a = {};
		GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a);
		return a.ftLastWriteTime;
	}

	// Waits for the runner to finish what it has (at most `seconds`)
	bool wait_idle(double seconds)
	{
		const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
		while (std::chrono::steady_clock::now() < until)
		{
			if (!hifi::bake::runner::status().busy)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		return false;
	}

	void empty_folder(const std::wstring &folder)
	{
		for (const wchar_t *kind : { L"worldfx-grass", L"worldfx-water", L"worldfx-wet", L"worldfx-rain", L"worldfx-sea", L"worldfx-shadow", L"worldfx-lamps" })
		{
			WIN32_FIND_DATAW d;
			const std::wstring sub = folder + kind + L"\\";
			HANDLE h = FindFirstFileW((sub + L"*").c_str(), &d);
			if (h != INVALID_HANDLE_VALUE)
			{
				do
					DeleteFileW((sub + d.cFileName).c_str());
				while (FindNextFileW(h, &d));
				FindClose(h);
			}
		}
	}
}

// (World FX told to read the zone's files again: counted here)
namespace
{
	int g_reloads = 0;
}
void worldfx::reload_zone()
{
	g_reloads++;
}

// (World FX's functions and HiFi XI's options, as game.cpp refers to them: nothing set here)
extern "C"
{
	void __cdecl worldfx_grass_actors2(const float *, const uint32_t *, uint32_t) {}
	void __cdecl worldfx_grass_cut(const float *, uint32_t) {}
	void __cdecl worldfx_lamps(const uint32_t *, uint32_t) {}
	void __cdecl worldfx_glows(const uint32_t *, uint32_t) {}
	void __cdecl worldfx_weather(const uint32_t *, uint32_t) {}
	uintptr_t __cdecl worldfx_vis_entry(uint32_t) { return 0; }
	void __cdecl worldfx_vis_setup(uintptr_t, uint32_t) {}
	void __cdecl worldfx_vis_stats(uint32_t out[12]) { std::memset(out, 0, 48); }
	int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
	const char *__cdecl worldfx_folder() { return ""; }
	int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
	void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	uintptr_t base = 0;
	check(dump::map(dump::kDumps[0], base) != 0, "the dump mapped (the game's key tables in it)");
	wchar_t here[MAX_PATH] = L"";
	GetFullPathNameW(L"build\\test\\runner\\", MAX_PATH, here, nullptr);
	const std::wstring folder = here;
	CreateDirectoryW(L"build\\test", nullptr);
	CreateDirectoryW(folder.c_str(), nullptr);
	empty_folder(folder);
	// (the zones with open sky: West Ronfaure's among them, as shipped)
	CopyFileW(L"data\\worldfx-sunlit.txt", (folder + L"worldfx-sunlit.txt").c_str(), FALSE);

	hifi::bake::runner::start(folder);
	const auto t0 = std::chrono::steady_clock::now();
	hifi::bake::runner::zone_entered(100);
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const hifi::bake::runner_status busy = hifi::bake::runner::status();
	check(busy.busy && busy.zone == 100, "zone 100 entered: being made");
	check(wait_idle(120.0), "and done within two minutes");
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	auto bin = [&](const wchar_t *kind, uint32_t zone) { return folder + kind + L"\\" + std::to_wstring(zone) + L".bin"; };
	auto none = [&](const wchar_t *kind, uint32_t zone) { return folder + kind + L"\\" + std::to_wstring(zone) + L".none"; };
	char line[200];
	// (wet ground and rain for every zone the sun lights, whatever its own weather: West Ronfaure has no rain of its own)
	std::snprintf(line, sizeof(line), "zone 100: its grass, water, wet ground, rain, shadow map and lamps made (%.1f s); its sea marked none",
		seconds);
	check(exists(bin(L"worldfx-grass", 100)) && exists(bin(L"worldfx-water", 100)) && exists(bin(L"worldfx-shadow", 100)) &&
		exists(bin(L"worldfx-lamps", 100)) && exists(bin(L"worldfx-wet", 100)) && exists(bin(L"worldfx-rain", 100)) &&
		exists(none(L"worldfx-sea", 100)) && !exists(bin(L"worldfx-sea", 100)), line);
	check(g_reloads == 1, "World FX told to read zone 100's files again (it's the zone you're in)");

	// The same bytes as each baker's own, made straight
	{
		const std::wstring straight = folder + L"straight\\";
		CreateDirectoryW(straight.c_str(), nullptr);
		hifi::bake::result r;
		bool same = true;
		struct one { const wchar_t *kind; bool (*bake)(uint32_t, const std::wstring &, hifi::bake::result &); };
		for (const one &o : { one{ L"worldfx-grass", hifi::bake::grass }, one{ L"worldfx-water", hifi::bake::water },
				 one{ L"worldfx-shadow", hifi::bake::shadow }, one{ L"worldfx-lamps", hifi::bake::lamps } })
		{
			const std::wstring mine = straight + o.kind + L".bin";
			std::vector<uint8_t> a, b;
			same = same && o.bake(100, mine, r) && hifi::bake::read_file(mine, a) && hifi::bake::read_file(bin(o.kind, 100), b) && a == b;
		}
		{
			// (the sea from the water the runner made: none, as the runner found)
			const std::wstring mine = straight + L"worldfx-sea.bin";
			DeleteFileW(mine.c_str()); // (an earlier run's)
			same = same && !hifi::bake::sea(100, mine, bin(L"worldfx-water", 100), r) && r.none && !exists(mine);
		}
		check(same, "each the same bytes as its baker's own (the sea: none from the baker too)");
	}

	// A zone with no collision and no lamps (Phanauet Channel, 1: the barge's), only meshes under open sky
	hifi::bake::runner::zone_entered(1);
	check(wait_idle(60.0), "zone 1 entered: done");
	bool all_none = true;
	for (const wchar_t *kind : { L"worldfx-grass", L"worldfx-water", L"worldfx-wet", L"worldfx-rain", L"worldfx-sea", L"worldfx-lamps" })
		all_none = all_none && exists(none(kind, 1)) && !exists(bin(kind, 1));
	check(all_none && exists(bin(L"worldfx-shadow", 1)), "zone 1: its shadow map made, every other kind marked none, nothing written for them");

	// Entered again: nothing made twice
	const FILETIME grass_at = written(bin(L"worldfx-grass", 100));
	const hifi::bake::runner_status before = hifi::bake::runner::status();
	hifi::bake::runner::zone_entered(100);
	check(wait_idle(10.0), "zone 100 entered again: done at once");
	const FILETIME grass_again = written(bin(L"worldfx-grass", 100));
	const hifi::bake::runner_status after = hifi::bake::runner::status();
	check(CompareFileTime(&grass_at, &grass_again) == 0 && after.lines.front() == before.lines.front(), "nothing made twice");
	std::printf("  status: %s\n", after.lines.front().c_str());

	// A lamps file from before the hour (LMP1): made again as LMP2 when the zone is entered, nothing else
	{
		std::vector<uint8_t> lamps;
		check(hifi::bake::read_file(bin(L"worldfx-lamps", 100), lamps) && lamps.size() > 4 && std::memcmp(lamps.data(), "LMP2", 4) == 0,
			"zone 100's lamps made as LMP2 (their brightness fully lit, by their curves over the day)");
		std::vector<uint8_t> older = lamps;
		if (older.size() > 4)
			std::memcpy(older.data(), "LMP1", 4);
		hifi::bake::write_file(bin(L"worldfx-lamps", 100), older.data(), older.size());
		hifi::bake::runner::zone_entered(100);
		check(wait_idle(30.0), "zone 100 entered with an LMP1 lamps file: done");
		std::vector<uint8_t> again;
		const FILETIME grass_now = written(bin(L"worldfx-grass", 100));
		check(hifi::bake::read_file(bin(L"worldfx-lamps", 100), again) && again == lamps && CompareFileTime(&grass_at, &grass_now) == 0,
			"its lamps made again (the same LMP2 bytes), its grass left as it was");
	}

	// Water from before the sea rule (no <zone>.r2 beside it): it, the rain and the sea made again (their none marks
	// too), nothing else
	{
		const std::wstring mark = folder + L"worldfx-water\\100.r2";
		check(exists(mark), "zone 100's water made with the sea rule's mark (100.r2)");
		std::vector<uint8_t> water;
		hifi::bake::read_file(bin(L"worldfx-water", 100), water);
		DeleteFileW(mark.c_str());
		const FILETIME rain_none_at = written(bin(L"worldfx-rain", 100)), grass_now = written(bin(L"worldfx-grass", 100));
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		hifi::bake::runner::zone_entered(100);
		check(wait_idle(60.0), "zone 100 entered with water from before the sea rule: done");
		std::vector<uint8_t> again;
		const FILETIME rain_none_again = written(bin(L"worldfx-rain", 100)), grass_again2 = written(bin(L"worldfx-grass", 100));
		check(exists(mark) && hifi::bake::read_file(bin(L"worldfx-water", 100), again) && again == water && exists(bin(L"worldfx-rain", 100)) &&
				exists(none(L"worldfx-sea", 100)) && CompareFileTime(&rain_none_at, &rain_none_again) != 0 &&
				CompareFileTime(&grass_now, &grass_again2) == 0,
			"its water made again (the same bytes here, the mark back), its rain made again and its sea looked at again (none), its grass left as it was");
		hifi::bake::runner::zone_entered(100);
		const bool idle = wait_idle(10.0);
		const FILETIME rain_none_third = written(bin(L"worldfx-rain", 100));
		check(idle && CompareFileTime(&rain_none_again, &rain_none_third) == 0, "and entered once more: nothing made");
	}

	// A rain none mark from before the any-weather rule (no <zone>.aw beside it) in a zone the sun lights: made again
	{
		DeleteFileW(bin(L"worldfx-rain", 100).c_str());
		hifi::bake::write_file(none(L"worldfx-rain", 100), "", 0);
		hifi::bake::runner::zone_entered(100);
		check(wait_idle(60.0) && exists(bin(L"worldfx-rain", 100)) && !exists(none(L"worldfx-rain", 100)),
			"zone 100 with a rain none mark from before (no 100.aw): its rain made, the mark gone");
	}

	// A Mog House (1000 + its room): nothing to make
	hifi::bake::runner::zone_entered(1261);
	check(!hifi::bake::runner::status().busy, "a Mog House: nothing to make");

	hifi::bake::runner::stop();
	check(true, "stopped: the thread ended");
	dump::unmap(base);
	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures;
}
