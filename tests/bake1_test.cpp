/*
 * HiFi XI's grass, water, wet-ground and rain bakers (src/bake: grass.cpp, water.cpp, wet.cpp, rain.cpp and the
 * collision reader under them, collision.cpp: reshade-worldfx's tools/bake_grass.py, bake_water.py, bake_wet.py,
 * bake_rain.py and zone_collision.py in C++) against the Python bakers themselves, byte for byte:
 *   - the FFXiMain dump mapped first (tests/game_dump.hpp), so key_tables finds the game's two key tables in it;
 *   - the Python run on every zone (tests/bake1_oracle.py, plain python: each baker as its command line runs it), each
 *     bake timed, into a folder; rain reads the water baked there, as tools/bake_all.py bakes water first;
 *   - each zone baked here, timed (with the process's memory after it), and its file compared with the Python's (a
 *     rain file that differs is unpacked to say how many squares do); a zone the Python writes nothing for (or a file
 *     with nothing in it, which bake_all deletes) must be `none` here, nothing written; rain here reads the water baked
 *     here;
 *   - then a few bakes again on threads of their own, all at once: the same bytes again.
 * Zones: West Ronfaure 100 (grass, water; no rain), La Theine 102, Jugner 104 (its lake, rain), the Sanctuary of Zi'Tah
 * 121 and Yuhtunga 123 (Zilart: ROM2; Zi'Tah's cut-out canopy), Southern San d'Oria 230 (a town: canals) and Norg 252
 * (a town with rain, its MMBs using the second key table's swaps, no grass), Port San d'Oria 232 and Kazham 250 (their
 * collision's water a floor under the game's sea: the water at the sea sheet's height), Phanauet Channel 1 (no
 * collision: none of anything).
 * And the water of Port San d'Oria and Kazham at their sea: every flat corner at the game's sea sheet's height
 * (research/ffxi-re/notes-sea-height.md), with no flow.
 *
 *   build\bake1_test.exe              (from this folder)
 *   build\bake1_test.exe 104 121      (those zones only)
 *   build\bake1_test.exe rain 102     (one kind: the peak memory then is that kind's)
 *   build\bake1_test.exe --no-python  (the Python's last files, from %TEMP%\hifixi_bake1_test\py)
 *   build\bake1_test.exe --any-weather 239  (wet and rain whatever the zone's own weather: bake_wet/bake_rain.py --any-weather)
 *
 * Exit code: the number of failures.
 */

#include "../src/bake/bake.hpp"
#include "../src/bake/dat.hpp"
#include "game_dump.hpp"
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
	int failures = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
	}
}

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
	const char *const kKinds[] = { "grass", "water", "wet", "rain" };

	// The Python's line for a bake: file, none or error, its time, what it said
	struct oracle
	{
		std::string status, what;
		double seconds = 0.0;
	};

	std::wstring wide(const std::string &s)
	{
		return std::wstring(s.begin(), s.end());
	}

	bool read_bytes(const std::string &path, std::vector<uint8_t> &out)
	{
		return hifi::bake::read_file(wide(path), out);
	}

	bool exists(const std::string &path)
	{
		const DWORD a = GetFileAttributesA(path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	std::map<std::string, oracle> read_oracle(const std::string &path)
	{
		std::map<std::string, oracle> out;
		std::ifstream f(path);
		std::string line;
		while (std::getline(f, line))
		{
			std::istringstream s(line);
			std::string kind, zone;
			oracle o;
			if (!(s >> kind >> zone >> o.status >> o.seconds))
				continue;
			std::getline(s, o.what);
			if (!o.what.empty() && o.what[0] == ' ')
				o.what.erase(0, 1);
			out[kind + " " + zone] = o;
		}
		return out;
	}

	uint32_t le32(const std::vector<uint8_t> &b, size_t at)
	{
		uint32_t v = 0;
		if (at + 4 <= b.size())
			std::memcpy(&v, b.data() + at, 4);
		return v;
	}

	// Where two files first differ ("" when they don't); a rain file is unpacked to say how many squares differ
	std::string difference(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
	{
		if (a == b)
			return "";
		char text[256];
		size_t at = 0;
		while (at < a.size() && at < b.size() && a[at] == b[at])
			at++;
		std::snprintf(text, sizeof(text), "%zu and %zu bytes, first differing at %zu", a.size(), b.size(), at);
		std::string out = text;
		if (a.size() < 48 || b.size() < 48 || std::memcmp(a.data(), "RHM1", 4) != 0 || std::memcmp(b.data(), "RHM1", 4) != 0 ||
			le32(a, 12) != le32(b, 12) || le32(a, 20) != le32(b, 20))
			return out;
		const size_t squares = size_t(le32(a, 12)) * le32(a, 20);
		std::vector<uint8_t> pa, pb;
		if (!hifi::bake::unxpress(a.data() + 48, le32(a, 44), squares * 3, pa) || !hifi::bake::unxpress(b.data() + 48, le32(b, 44), squares * 3, pb))
			return out + " (not unpacked)";
		size_t kinds = 0, qs = 0;
		for (size_t i = 0; i < squares; ++i)
		{
			kinds += pa[i] != pb[i];
			qs += pa[squares + 2 * i] != pb[squares + 2 * i] || pa[squares + 2 * i + 1] != pb[squares + 2 * i + 1];
		}
		std::snprintf(text, sizeof(text), "; unpacked: %zu of %zu squares' kinds differ, %zu height steps", kinds, squares, qs);
		return out + text;
	}

	double peak_mb()
	{
		PROCESS_MEMORY_COUNTERS_EX m = {};
		m.cb = sizeof(m);
		GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m), sizeof(m));
		return double(m.PeakPagefileUsage) / 1048576.0;
	}

	bool any_weather = false; // (--any-weather)

	bool bake(const std::string &kind, uint32_t zone, const std::string &cpp, hifi::bake::result &r)
	{
		const std::wstring out = wide(cpp + kind + "\\" + std::to_string(zone) + ".bin");
		if (kind == "grass")
			return hifi::bake::grass(zone, out, r);
		if (kind == "water")
			return hifi::bake::water(zone, out, r);
		if (kind == "wet")
			return hifi::bake::wet(zone, out, r, any_weather);
		return hifi::bake::rain(zone, out, wide(cpp + "water\\" + std::to_string(zone) + ".bin"), r, any_weather);
	}

	// One bake here against the Python's
	void compare(const std::string &kind, uint32_t zone, const std::string &py, const std::string &cpp, const std::map<std::string, oracle> &oracles,
		std::map<std::string, double> &total_cpp, std::map<std::string, double> &total_py)
	{
		const std::string name = kind + " " + std::to_string(zone);
		const auto o = oracles.find(name);
		if (o == oracles.end())
		{
			check(false, name + ": the Python has no line for it");
			return;
		}
		const std::string path = cpp + kind + "\\" + std::to_string(zone) + ".bin";
		DeleteFileA(path.c_str());
		hifi::bake::result r;
		const auto t0 = std::chrono::steady_clock::now();
		const bool ok = bake(kind, zone, cpp, r);
		const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		total_cpp[kind] += s;
		total_py[kind] += o->second.seconds;
		char times[160];
		std::snprintf(times, sizeof(times), "C++ %.2f s, Python %.2f s; process peak so far %.0f MB", s, o->second.seconds, peak_mb());
		if (o->second.status == "file")
		{
			std::vector<uint8_t> a, b;
			const bool read = read_bytes(py + kind + "\\" + std::to_string(zone) + ".bin", a) && ok && read_bytes(path, b);
			const std::string diff = read ? difference(a, b) : std::string("not written here: ") + (r.why.empty() ? "?" : r.why);
			char size[64];
			std::snprintf(size, sizeof(size), "%zu bytes", a.size());
			check(read && diff.empty(), name + ": " + (diff.empty() ? std::string("the same ") + size : diff) + " (" + times + ")");
		}
		else if (o->second.status == "none")
			check(!ok && r.none && !exists(path), name + ": none, as the Python's (" + o->second.what + "; here: " + r.why + ") (" + times + ")");
		else
			check(!ok && !exists(path), name + ": failed, as the Python did (" + o->second.what + "; here: " + r.why + ")");
	}
}

int main(int argc, char **argv)
{
	bool python = true;
	std::vector<uint32_t> zones;
	std::vector<std::string> kinds;
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--no-python") == 0)
			python = false;
		else if (std::strcmp(argv[i], "--any-weather") == 0)
			any_weather = true;
		else if (argv[i][0] >= '0' && argv[i][0] <= '9')
			zones.push_back(uint32_t(std::strtoul(argv[i], nullptr, 10)));
		else
			kinds.push_back(argv[i]);
	}
	if (kinds.empty())
		kinds.assign(std::begin(kKinds), std::end(kKinds));
	if (zones.empty())
		zones = { 100, 102, 104, 121, 123, 230, 252, 232, 250, 1 };

	// This folder (the exe's, build\, one up), and ours under %TEMP%
	char exe[MAX_PATH] = "";
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	std::string root = exe;
	root = root.substr(0, root.find_last_of("\\/"));
	root = root.substr(0, root.find_last_of("\\/") + 1);
	char temp[MAX_PATH] = "";
	GetTempPathA(MAX_PATH, temp);
	const std::string folder = std::string(temp) + "hifixi_bake1_test\\", py = folder + "py\\", cpp = folder + "cpp\\";
	CreateDirectoryA(folder.c_str(), nullptr);
	CreateDirectoryA(cpp.c_str(), nullptr);
	for (const char *k : kKinds)
		CreateDirectoryA((cpp + k).c_str(), nullptr);

	uintptr_t base = 0;
	const size_t size = dump::map(dump::kDumps[0], base);
	check(size != 0, std::string("the dump mapped (") + dump::kDumps[0] + ")");
	if (size == 0)
		return failures;
	const uint8_t *first = nullptr, *second = nullptr;
	check(hifi::bake::key_tables(first, second) && reinterpret_cast<uintptr_t>(first) - base == 0x376AC0 &&
		reinterpret_cast<uintptr_t>(second) - base == 0x376DE8, "the key tables found where the Python reads them (0x376AC0, 0x376DE8)");

	if (python)
	{
		std::string command = "python \"" + root + "tests\\bake1_oracle.py\" \"" + folder + "py\"" + (any_weather ? " --any-weather" : "");
		for (const uint32_t z : zones)
			command += " " + std::to_string(z);
		command += " > \"" + folder + "oracle.log\" 2>&1";
		std::printf("running the Python bakers (%s)...\n", (folder + "oracle.log").c_str());
		const int code = std::system(command.c_str());
		check(code == 0, "the Python ran (" + folder + "oracle.log)");
	}
	const std::map<std::string, oracle> oracles = read_oracle(py + "oracle.txt");
	check(!oracles.empty(), "the Python's results read (" + py + "oracle.txt)");

	std::map<std::string, double> total_cpp, total_py;
	for (const uint32_t zone : zones)
		for (const std::string &kind : kinds)
			compare(kind, zone, py, cpp, oracles, total_cpp, total_py);
	for (const std::string &kind : kinds)
		std::printf("      %-5s: C++ %.2f s, Python %.2f s in all\n", kind.c_str(), total_cpp[kind], total_py[kind]);

	// The water at the game's sea where the collision lies under it: every flat corner (kind under 100) at the sea sheet's
	// placed height, no flow (before: Port San d'Oria's 4.5 yalms under it, Kazham's 0.05-4.05)
	struct sea_check
	{
		uint32_t zone;
		const char *what;
		double y; // (the sheet's corners' y, float32 to float64)
		uint32_t corners;
	};
	const sea_check kSea[] = {
		{ 232, "Port San d'Oria, t_sa/effe/sea /sea1", 1.499999761581421, 1575 },
		{ 250, "Kazham, t_ka/effe/sea /sea2 (model 'sea1')", -0.05000017210841179, 2922 },
	};
	for (const sea_check &c : kSea)
	{
		if (std::find(zones.begin(), zones.end(), c.zone) == zones.end() || std::find(kinds.begin(), kinds.end(), "water") == kinds.end())
			continue;
		std::vector<uint8_t> b;
		uint32_t flat = 0, at_sea = 0, flowing = 0;
		double worst = 0.0;
		if (read_bytes(cpp + "water\\" + std::to_string(c.zone) + ".bin", b) && b.size() >= 20 && std::memcmp(b.data(), "WTR2", 4) == 0)
		{
			const size_t start = 20 + size_t(le32(b, 12)) * 16, corners = le32(b, 16);
			for (size_t i = 0; i < corners && start + (i + 1) * 32 <= b.size(); ++i)
			{
				float v[8];
				std::memcpy(v, b.data() + start + i * 32, 32);
				if (v[3] >= 100.0f)
					continue;
				flat++;
				const double d = std::fabs(double(v[4]) - c.y);
				worst = std::max(worst, d);
				at_sea += d < 1e-6;
				flowing += v[6] != 0.0f || v[7] != 0.0f;
			}
		}
		char text[256];
		std::snprintf(text, sizeof(text), "water %u at the game's sea (%s, y %.3f): %u of %u flat corners on it (%u wanted), furthest %.6f, %u flowing",
			c.zone, c.what, c.y, at_sea, flat, c.corners, worst, flowing);
		check(flat == c.corners && at_sea == flat && flowing == 0, text);
	}

	// Some again, on threads of their own all at once (as several bakes may run): the same bytes
	if (kinds.size() == std::size(kKinds))
	{
		struct job
		{
			std::string kind;
			uint32_t zone;
			std::vector<uint8_t> before;
			bool ok = false;
		};
		std::vector<job> jobs;
		for (const auto &[kind, zone] : std::vector<std::pair<std::string, uint32_t>>{ { "grass", 123 }, { "water", 104 }, { "wet", 102 }, { "rain", 252 } })
		{
			job j;
			j.kind = kind;
			j.zone = zone;
			if (read_bytes(cpp + kind + "\\" + std::to_string(zone) + ".bin", j.before))
				jobs.push_back(j);
		}
		std::vector<std::thread> threads;
		const std::string again = folder + "again\\";
		CreateDirectoryA(again.c_str(), nullptr);
		CreateDirectoryA((again + "water").c_str(), nullptr);
		CopyFileA((cpp + "water\\252.bin").c_str(), (again + "water\\252.bin").c_str(), FALSE);
		for (job &j : jobs)
			threads.emplace_back([&j, &again] {
				hifi::bake::result r;
				j.ok = bake(j.kind, j.zone, again, r);
			});
		for (std::thread &t : threads)
			t.join();
		for (const job &j : jobs)
		{
			std::vector<uint8_t> after;
			check(j.ok && read_bytes(again + j.kind + "\\" + std::to_string(j.zone) + ".bin", after) && after == j.before,
				j.kind + " " + std::to_string(j.zone) + " baked again beside the others at once: the same bytes");
		}
	}

	dump::unmap(base);
	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures;
}
