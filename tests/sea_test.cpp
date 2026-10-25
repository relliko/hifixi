/*
 * HiFi XI's sea sheets (src/bake/sea.cpp: reshade-worldfx's tools/bake_sea.py in C++) against the Python baker itself,
 * byte for byte:
 *   - the FFXiMain dump mapped first (tests/game_dump.hpp), so key_tables finds the game's key tables (the MMBs');
 *   - the Python run once on every zone (tests/sea_oracle.py, plain python), each bake timed, into a folder, both
 *     reading the same water files: the installed ones (C:\Games\PhoenixXI\bootloader\worldfx-water, read only);
 *   - each zone baked here, timed, and its file compared with the Python's (where they differ: which corner and which
 *     of its numbers); a zone the Python writes nothing for must be `none` here, nothing written;
 *   - then three of them baked again on threads of their own, all at once: the same bytes again.
 * Zones: every zone with a water file there (Bibiki Bay, 4, among them: its open sea umi1), and two without one: 1 (no
 * water) and 3 (a water .none).
 *
 *   build\sea_test.exe                      (from this folder)
 *   build\sea_test.exe --quick              (4, 107, 126, 100, 104, 11, 49, 1, 3 only)
 *   build\sea_test.exe --no-python          (the Python's last files, from %TEMP%\hifixi_sea_test\py)
 *   build\sea_test.exe --water <folder>     (other water files)
 *
 * The test runs below normal priority (the Python too: it inherits it), the game may be running. Exit code: the number
 * of failures.
 */

#include "../src/bake/bake.hpp"
#include "../src/bake/dat.hpp"
#include "game_dump.hpp"
#include <algorithm>
#include <chrono>
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

	std::map<uint32_t, oracle> read_oracle(const std::string &path)
	{
		std::map<uint32_t, oracle> out;
		std::ifstream f(path);
		std::string line;
		while (std::getline(f, line))
		{
			std::istringstream s(line);
			std::string kind;
			uint32_t zone = 0;
			oracle o;
			if (!(s >> kind >> zone >> o.status >> o.seconds) || kind != "sea")
				continue;
			std::getline(s, o.what);
			if (!o.what.empty() && o.what[0] == ' ')
				o.what.erase(0, 1);
			out[zone] = o;
		}
		return out;
	}

	// Where two SEA1 files first differ ("" when they don't): the header's word, or the corner and its number
	std::string difference(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
	{
		if (a == b)
			return "";
		size_t at = 0;
		while (at < a.size() && at < b.size() && a[at] == b[at])
			at++;
		char text[256];
		std::snprintf(text, sizeof(text), "%zu and %zu bytes, first differing at %zu", a.size(), b.size(), at);
		std::string out = text;
		if (at < 16)
		{
			static const char *const names[] = { "the magic", "the zone", "the sheet count", "the corner count" };
			return out + "; " + names[at / 4];
		}
		if (at + 4 <= a.size() && at + 4 <= b.size())
		{
			static const char *const names[] = { "x", "bed y", "z", "kind", "surface y", "edge distance", "flow x", "flow z" };
			const size_t corner = (at - 16) / 32, field = (at - 16) % 32 / 4;
			float fa = 0.0f, fb = 0.0f;
			std::memcpy(&fa, a.data() + 16 + corner * 32 + field * 4, 4);
			std::memcpy(&fb, b.data() + 16 + corner * 32 + field * 4, 4);
			std::snprintf(text, sizeof(text), "; corner %zu (triangle %zu), its %s: %.9g here, %.9g in the Python's", corner, corner / 3, names[field], fa, fb);
			out += text;
		}
		return out;
	}

	// One zone baked here and compared with the Python's
	void compare(uint32_t zone, const std::string &water, const std::string &py, const std::string &cpp, const std::map<uint32_t, oracle> &oracles,
		double &total_cpp, double &total_py, uint32_t &files)
	{
		char name[32];
		std::snprintf(name, sizeof(name), "sea %u", zone);
		const auto o = oracles.find(zone);
		if (o == oracles.end())
		{
			check(false, std::string(name) + ": the Python has no line for it");
			return;
		}
		const std::string ours = cpp + "sea\\" + std::to_string(zone) + ".bin";
		const std::string theirs = py + "sea\\" + std::to_string(zone) + ".bin";
		DeleteFileA(ours.c_str());
		hifi::bake::result r;
		const auto t = std::chrono::steady_clock::now();
		const bool wrote = hifi::bake::sea(zone, wide(ours), wide(water + std::to_string(zone) + ".bin"), r);
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
		total_cpp += seconds;
		total_py += o->second.seconds;
		char times[96];
		std::snprintf(times, sizeof(times), "C++ %.3f s, Python %.3f s", seconds, o->second.seconds);
		if (o->second.status == "file")
		{
			std::vector<uint8_t> a, b;
			if (!wrote || !read_bytes(ours, a) || !read_bytes(theirs, b))
			{
				check(false, std::string(name) + ": not baked here (" + r.why + "); the Python: " + o->second.what);
				return;
			}
			files++;
			const std::string d = difference(a, b);
			check(d.empty(), std::string(name) + (d.empty() ? ": the same " + std::to_string(a.size()) + " bytes (" + o->second.what + "); " + times : ": differs: " + d));
		}
		else if (o->second.status == "none")
			check(!wrote && r.none && !exists(ours), std::string(name) + ": none, nothing written (here: " + (r.why.empty() ? "?" : r.why) + "; the Python: " + o->second.what + "); " + times);
		else
			check(!wrote && !r.none && !exists(ours), std::string(name) + ": fails as the Python's does (here: " + r.why + "; the Python: " + o->second.what + ")");
	}

	// The zones with a water file in the folder (<zone>.bin), in order
	std::vector<uint32_t> water_zones(const std::string &folder)
	{
		std::vector<uint32_t> out;
		WIN32_FIND_DATAA d;
		HANDLE h = FindFirstFileA((folder + "*.bin").c_str(), &d);
		if (h == INVALID_HANDLE_VALUE)
			return out;
		do
		{
			char *end = nullptr;
			const unsigned long z = std::strtoul(d.cFileName, &end, 10);
			if (end != d.cFileName && std::strcmp(end, ".bin") == 0)
				out.push_back(uint32_t(z));
		} while (FindNextFileA(h, &d));
		FindClose(h);
		std::sort(out.begin(), out.end());
		return out;
	}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	// (below normal: the game may be running; the Python started from here inherits it)
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
	bool quick = false, python = true;
	std::string water = "C:\\Games\\PhoenixXI\\bootloader\\worldfx-water\\";
	for (int i = 1; i < argc; ++i)
	{
		quick = quick || std::strcmp(argv[i], "--quick") == 0;
		python = python && std::strcmp(argv[i], "--no-python") != 0;
		if (std::strcmp(argv[i], "--water") == 0 && i + 1 < argc)
		{
			water = argv[++i];
			if (!water.empty() && water.back() != '\\' && water.back() != '/')
				water += "\\";
		}
	}

	char exe[MAX_PATH] = "";
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	std::string root = exe;
	root = root.substr(0, root.find_last_of("\\/"));
	root = root.substr(0, root.find_last_of("\\/") + 1);
	char temp[MAX_PATH] = "";
	GetTempPathA(MAX_PATH, temp);
	const std::string folder = std::string(temp) + "hifixi_sea_test\\", py = folder + "py\\", cpp = folder + "cpp\\";
	CreateDirectoryA(folder.c_str(), nullptr);

	std::vector<uint32_t> zones = quick ? std::vector<uint32_t>{ 4, 107, 126, 100, 104, 11, 49 } : water_zones(water);
	check(std::find(zones.begin(), zones.end(), 4u) != zones.end(), "Bibiki Bay (4) has a water file in " + water + " (" + std::to_string(zones.size()) + " zones with one)");
	for (const uint32_t z : { 1u, 3u })
		if (!exists(water + std::to_string(z) + ".bin"))
			zones.push_back(z);

	// 1. The dump mapped: the key tables found
	uintptr_t base = 0;
	const size_t size = dump::map(dump::kDumps[0], base);
	check(size != 0, std::string("mapped ") + dump::kDumps[0]);
	if (size == 0)
		return failures;
	const uint8_t *first = nullptr, *second = nullptr;
	check(hifi::bake::key_tables(first, second), "key_tables found the game's two key tables");

	// 2. The Python, every zone, timed
	if (python)
	{
		// (--water first: argparse takes the zones only when they follow the folder)
		std::string command = "python \"" + root + "tests\\sea_oracle.py\" --water \"" + water.substr(0, water.size() - 1) + "\" \"" + folder + "py\"";
		for (const uint32_t z : zones)
			command += " " + std::to_string(z);
		command += " > \"" + folder + "oracle.log\" 2>&1";
		std::printf("running the Python (%zu zones)...\n", zones.size());
		const int code = std::system(command.c_str());
		check(code == 0, "the Python ran (" + folder + "oracle.log)");
	}
	const std::map<uint32_t, oracle> oracles = read_oracle(py + "oracle.txt");
	check(!oracles.empty(), "the Python's results read (" + py + "oracle.txt)");

	// 3. Each zone here against the Python's
	CreateDirectoryA(cpp.c_str(), nullptr);
	CreateDirectoryA((cpp + "sea").c_str(), nullptr);
	double total_cpp = 0.0, total_py = 0.0;
	uint32_t files = 0;
	for (const uint32_t z : zones)
		compare(z, water, py, cpp, oracles, total_cpp, total_py, files);
	std::printf("      %u files, %zu zones: C++ %.2f s, Python %.2f s\n", files, zones.size(), total_cpp, total_py);
	{
		const auto bibiki = oracles.find(4);
		check(bibiki != oracles.end() && bibiki->second.status == "file", "Bibiki Bay (4) has sea sheets");
		bool umi1 = false, clouds = false;
		std::ifstream f(py + "sheets.txt");
		std::string line;
		while (std::getline(f, line))
		{
			umi1 = umi1 || (line.rfind("4 ", 0) == 0 && line.find("'umi1' D3M") != std::string::npos);
			for (const char *skip : { " kumo", "/kumo", " kiri", "/kiri" })
				clouds = clouds || line.find(skip) != std::string::npos;
		}
		check(umi1, "and its open sea, the D3M umi1, is one of them (" + py + "sheets.txt)");
		check(!clouds, "no sheet kept from under a kumo (cloud) or kiri (fog) folder (Beaucedine Glacier's kum0-kum2: 111, 134, 136)");
	}

	// 4. Several at once on threads of their own: the same bytes
	{
		const uint32_t parallel[] = { 4, 107, 104 };
		const std::string out = folder + "threads\\";
		CreateDirectoryA(out.c_str(), nullptr);
		bool wrote[3] = {};
		hifi::bake::result results[3];
		std::vector<std::thread> threads;
		for (int i = 0; i < 3; ++i)
			threads.emplace_back([&, i] {
				wrote[i] = hifi::bake::sea(parallel[i], wide(out + std::to_string(parallel[i]) + ".bin"), wide(water + std::to_string(parallel[i]) + ".bin"), results[i]);
			});
		for (std::thread &th : threads)
			th.join();
		bool same = true;
		for (int i = 0; i < 3; ++i)
		{
			const std::string mine = out + std::to_string(parallel[i]) + ".bin";
			const std::string theirs = py + "sea\\" + std::to_string(parallel[i]) + ".bin";
			std::vector<uint8_t> a, b;
			if (exists(theirs))
				same = same && wrote[i] && read_bytes(mine, a) && read_bytes(theirs, b) && a == b;
			else
				same = same && !wrote[i] && results[i].none;
		}
		check(same, "three bakes at once on their own threads (4, 107, 104): the Python's bytes");
	}

	dump::unmap(base);
	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures;
}
