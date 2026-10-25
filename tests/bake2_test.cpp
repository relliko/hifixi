/*
 * HiFi XI's shadow and lamp bakers (src/bake: shadow.cpp, lamps.cpp: reshade-worldfx's tools/bake_shadow_geo.py and
 * tools/bake_lamps.py in C++) against the Python bakers themselves, byte for byte:
 *   - the FFXiMain dump mapped first (tests/game_dump.hpp), so key_tables finds the game's two key tables in it (each
 *     where the Python reads it: 0x376AC0 and 0x376DE8);
 *   - the Python run once on every zone (tests/bake2_oracle.py, plain python), each bake timed, into a folder;
 *   - each zone baked here, timed, and its file compared with the Python's (an SHD2 file that differs is unpacked to
 *     say where); a zone the Python writes nothing for must be `none` here, nothing written;
 *   - then a few of them baked again on threads of their own, all at once (as "Build all zones" runs them): the same
 *     bytes again;
 *   - and the other dump (FFXiMain_041C0000.bin) mapped: key_tables finds the same two tables in it.
 * Shadow zones: West Ronfaure 100, Northern San d'Oria 231, Zi'Tah 121, Jugner 104 (cut-out leaves), and the towns
 * whose MMBs use the second key table's swaps (Whitegate 50, Al Zahbi 48, Norg 252); zone 0 places nothing that casts.
 * Lamp zones: towns (Southern and Northern San d'Oria, Port Jeuno, Whitegate, Al Zahbi's 71 with 229 ids without a
 * generator), fields (100, 102, 121) and two without lamps (Jugner 104, 1).
 *
 *   build\bake2_test.exe              (from this folder)
 *   build\bake2_test.exe --all        (every zone: the sunlit list for the shadow maps, 0-299 for the lamps)
 *   build\bake2_test.exe --no-python  (the Python's last files, from %TEMP%\hifixi_bake2_test\py)
 *
 * Exit code: the number of failures.
 */

#include "../src/bake/bake.hpp"
#include "../src/bake/dat.hpp"
#include "game_dump.hpp"
#include <psapi.h>
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

	// Where two files first differ ("" when they don't); an SHD2 file is unpacked to say which part
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
		if (a.size() < 64 || b.size() < 64 || std::memcmp(a.data(), "SHD2", 4) != 0 || std::memcmp(b.data(), "SHD2", 4) != 0)
			return out;
		auto body = [](const std::vector<uint8_t> &f, std::vector<uint8_t> &raw) {
			const uint32_t packing = le32(f, 32), packed = le32(f, 36), size = le32(f, 40);
			if (64 + size_t(packed) > f.size())
				return false;
			if (packing == 0)
			{
				raw.assign(f.begin() + 64, f.begin() + 64 + packed);
				return true;
			}
			return hifi::bake::unxpress(f.data() + 64, packed, size, raw);
		};
		std::vector<uint8_t> ra, rb;
		for (size_t i = 4; i < 64; i += 4)
			if (le32(a, i) != le32(b, i))
			{
				std::snprintf(text, sizeof(text), "; header word %zu: %u here, %u in the Python's", i / 4, le32(a, i), le32(b, i));
				out += text;
				break;
			}
		if (!body(a, ra) || !body(b, rb))
			return out + "; a body doesn't unpack";
		size_t d = 0;
		while (d < ra.size() && d < rb.size() && ra[d] == rb[d])
			d++;
		if (d == ra.size() && d == rb.size())
			return out + "; the bodies are the same (only the packing differs)";
		const size_t models = le32(b, 12), tris = le32(b, 16), placements = le32(b, 20);
		const size_t t0 = models * 8, p0 = t0 + tris * 48, m0 = p0 + placements * 56;
		if (d < t0)
			std::snprintf(text, sizeof(text), "; unpacked: model range %zu", d / 8);
		else if (d < p0)
			std::snprintf(text, sizeof(text), "; unpacked: triangle %zu, corner %zu, word %zu", (d - t0) / 48, (d - t0) % 48 / 16, (d - t0) % 16 / 4);
		else if (d < m0)
			std::snprintf(text, sizeof(text), "; unpacked: placement %zu, word %zu", (d - p0) / 56, (d - p0) % 56 / 4);
		else
			std::snprintf(text, sizeof(text), "; unpacked: the masks, byte %zu", d - m0);
		return out + text;
	}

	typedef bool (*baker)(uint32_t, const std::wstring &, hifi::bake::result &);

	struct job
	{
		const char *kind;
		baker bake;
		uint32_t zone;
	};

	// One zone baked here and compared with the Python's
	void compare(const job &j, const std::string &py, const std::string &cpp, const std::map<std::string, oracle> &oracles, double &total_cpp, double &total_py)
	{
		char name[64];
		std::snprintf(name, sizeof(name), "%s %u", j.kind, j.zone);
		const auto o = oracles.find(name);
		if (o == oracles.end())
		{
			check(false, std::string(name) + ": the Python has no line for it");
			return;
		}
		const std::string ours = cpp + j.kind + "\\" + std::to_string(j.zone) + ".bin";
		const std::string theirs = py + j.kind + "\\" + std::to_string(j.zone) + ".bin";
		DeleteFileA(ours.c_str());
		hifi::bake::result r;
		const auto t = std::chrono::steady_clock::now();
		const bool wrote = j.bake(j.zone, wide(ours), r);
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
			const std::string d = difference(a, b);
			check(d.empty(), std::string(name) + (d.empty() ? ": the same " + std::to_string(a.size()) + " bytes (" + o->second.what + "); " + times : ": differs: " + d));
		}
		else if (o->second.status == "none")
			check(!wrote && r.none && !exists(ours), std::string(name) + ": none, nothing written (here: " + (r.why.empty() ? "?" : r.why) + "; the Python: " + o->second.what + "); " + times);
		else
			check(!wrote && !r.none && !exists(ours), std::string(name) + ": fails as the Python's does (here: " + r.why + "; the Python: " + o->second.what + ")");
	}

	std::vector<uint32_t> sunlit_zones(const std::string &list)
	{
		std::vector<uint32_t> out;
		std::ifstream f(list);
		std::string line;
		while (std::getline(f, line))
		{
			line = line.substr(0, line.find('#'));
			std::istringstream s(line);
			uint32_t z = 0;
			if (s >> z)
				out.push_back(z);
		}
		return out;
	}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	bool all = false, python = true;
	for (int i = 1; i < argc; ++i)
	{
		all = all || std::strcmp(argv[i], "--all") == 0;
		python = python && std::strcmp(argv[i], "--no-python") != 0;
	}

	// This folder (the exe's, build\, one up) and the test's own
	char exe[MAX_PATH] = "";
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	std::string root = exe;
	root = root.substr(0, root.find_last_of("\\/"));
	root = root.substr(0, root.find_last_of("\\/") + 1);
	char temp[MAX_PATH] = "";
	GetTempPathA(MAX_PATH, temp);
	const std::string folder = std::string(temp) + "hifixi_bake2_test\\", py = folder + "py\\", cpp = folder + "cpp\\";
	CreateDirectoryA(folder.c_str(), nullptr);

	std::vector<uint32_t> shadow_zones = { 100, 231, 121, 104, 50, 48, 252, 0 };
	std::vector<uint32_t> lamp_zones = { 230, 231, 246, 50, 71, 100, 102, 121, 104, 1 };
	if (all)
	{
		shadow_zones = sunlit_zones(root + "..\\reshade-worldfx\\build\\worldfx-sunlit.txt");
		lamp_zones.clear();
		for (uint32_t z = 0; z < 300; ++z)
			lamp_zones.push_back(z);
	}

	// 1. The dump mapped: the key tables found where the Python reads them. (The other dump's place, where it doesn't
	// overlap this one's, held until it's mapped: the bakes' big allocations can land there otherwise.)
	void *other_place = VirtualAlloc(reinterpret_cast<void *>(dump::base_of(dump::kDumps[1])), dump::base_of(dump::kDumps[0]) - dump::base_of(dump::kDumps[1]),
		MEM_RESERVE, PAGE_NOACCESS);
	uintptr_t base = 0;
	const size_t size = dump::map(dump::kDumps[0], base);
	check(size != 0, std::string("mapped ") + dump::kDumps[0]);
	if (size == 0)
		return failures;
	const uint8_t *first = nullptr, *second = nullptr;
	const bool found = hifi::bake::key_tables(first, second);
	check(found && reinterpret_cast<uintptr_t>(first) == base + 0x376AC0 && reinterpret_cast<uintptr_t>(second) == base + 0x376DE8,
		"key_tables: the first at 0x376AC0 (zone_collision.KEY_TABLE), the second at 0x376DE8 (bake_shadow_geo's KEY_TABLE2_AT)");

	// 2. The Python, every zone, timed
	if (python)
	{
		std::string command = "python \"" + root + "tests\\bake2_oracle.py\" \"" + folder + "py\" --shadow";
		for (const uint32_t z : shadow_zones)
			command += " " + std::to_string(z);
		command += " --lamps";
		for (const uint32_t z : lamp_zones)
			command += " " + std::to_string(z);
		command += " > \"" + folder + "oracle.log\" 2>&1";
		std::printf("running the Python (%zu shadow maps, %zu lamp files)...\n", shadow_zones.size(), lamp_zones.size());
		const int code = std::system(command.c_str());
		check(code == 0, "the Python ran (" + folder + "oracle.log)");
	}
	const std::map<std::string, oracle> oracles = read_oracle(py + "oracle.txt");
	check(!oracles.empty(), "the Python's results read (" + py + "oracle.txt)");

	// 3. Each zone here against the Python's
	CreateDirectoryA(cpp.c_str(), nullptr);
	CreateDirectoryA((cpp + "shadow").c_str(), nullptr);
	CreateDirectoryA((cpp + "lamps").c_str(), nullptr);
	std::vector<job> jobs;
	for (const uint32_t z : shadow_zones)
		jobs.push_back({ "shadow", hifi::bake::shadow, z });
	for (const uint32_t z : lamp_zones)
		jobs.push_back({ "lamps", hifi::bake::lamps, z });
	double cpp_shadow = 0.0, py_shadow = 0.0, cpp_lamps = 0.0, py_lamps = 0.0;
	for (const job &j : jobs)
	{
		const bool is_shadow = std::strcmp(j.kind, "shadow") == 0;
		compare(j, py, cpp, oracles, is_shadow ? cpp_shadow : cpp_lamps, is_shadow ? py_shadow : py_lamps);
	}
	std::printf("      all shadow maps: C++ %.2f s, Python %.2f s; all lamp files: C++ %.2f s, Python %.2f s\n", cpp_shadow, py_shadow, cpp_lamps, py_lamps);
	{
		// (this 32-bit process's peak so far: the dump mapped, 12 MB, plus the biggest bake)
		PROCESS_MEMORY_COUNTERS pm = {};
		pm.cb = sizeof(pm);
		if (GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof(pm)))
			std::printf("      peak memory %.1f MB (the dump mapped is %.1f MB of it)\n", pm.PeakPagefileUsage / 1048576.0, size / 1048576.0);
	}

	// 4. Several at once on threads of their own: the same bytes
	{
		const job parallel[] = { { "shadow", hifi::bake::shadow, 121 }, { "shadow", hifi::bake::shadow, 50 }, { "shadow", hifi::bake::shadow, 100 },
			{ "lamps", hifi::bake::lamps, 246 }, { "lamps", hifi::bake::lamps, 50 }, { "lamps", hifi::bake::lamps, 104 } };
		const std::string out = folder + "threads\\";
		CreateDirectoryA(out.c_str(), nullptr);
		CreateDirectoryA((out + "shadow").c_str(), nullptr);
		CreateDirectoryA((out + "lamps").c_str(), nullptr);
		bool wrote[6] = {};
		hifi::bake::result results[6];
		const auto t = std::chrono::steady_clock::now();
		std::vector<std::thread> threads;
		for (int i = 0; i < 6; ++i)
			threads.emplace_back([&, i] {
				const job &j = parallel[i];
				wrote[i] = j.bake(j.zone, wide(out + j.kind + "\\" + std::to_string(j.zone) + ".bin"), results[i]);
			});
		for (std::thread &th : threads)
			th.join();
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
		bool same = true;
		for (int i = 0; i < 6; ++i)
		{
			const job &j = parallel[i];
			const std::string mine = out + j.kind + "\\" + std::to_string(j.zone) + ".bin";
			const std::string theirs = py + j.kind + "\\" + std::to_string(j.zone) + ".bin";
			std::vector<uint8_t> a, b;
			if (exists(theirs))
				same = same && wrote[i] && read_bytes(mine, a) && read_bytes(theirs, b) && a == b;
			else
				same = same && !wrote[i] && results[i].none;
		}
		char what[160];
		std::snprintf(what, sizeof(what), "six bakes at once on their own threads (shadow 121, 50, 100; lamps 246, 50, 104): the Python's bytes, %.3f s in all", seconds);
		check(same, what);
	}

	// 5. The other dump (the 2026-10-18 client): the same two tables found in it
	{
		const std::vector<uint8_t> a(first, first + 256), b(second, second + 256);
		dump::unmap(base);
		if (other_place != nullptr)
			VirtualFree(other_place, 0, MEM_RELEASE);
		const size_t other = dump::map(dump::kDumps[1], base);
		const uint8_t *first2 = nullptr, *second2 = nullptr;
		const bool same = other != 0 && hifi::bake::key_tables(first2, second2) && std::memcmp(first2, a.data(), 256) == 0 &&
			std::memcmp(second2, b.data(), 256) == 0;
		check(same, std::string("key_tables in ") + dump::kDumps[1] + ": the same two tables");
		if (other != 0)
			dump::unmap(base);
	}

	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures;
}
