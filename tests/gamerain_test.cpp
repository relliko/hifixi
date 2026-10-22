/*
 * Offline test of the game's rain and snow hidden (src/game/gamerain.cpp, the worldfx addon's /worldfx gamerain in
 * C++), as reshade-worldfx's tests/t_gamerain.py tests the addon: each FFXiMain dump mapped at its own base
 * (game_dump.hpp), the routine's memory from 0x30000000, World FX's settings and folder faked (worldfx_setting from a
 * table here, worldfx_folder a folder of the test's own with worldfx-rain\104.bin in it).
 *   - the pattern unique in the dump; 046070's "mov al, [ebp+17Ah]" (04609D) becoming a call to the routine and a nop
 *     and nothing else around it; the routine byte for byte the addon's (its gr_code for memory at 0x30000000);
 *   - its list in each case: no zone; zone 104 with its rain map, rain and snow on (the whole list); Rain or Snow off
 *     or their amounts 0 (the other's alone); both off; zone 100 without a rain map; World FX's folder missing or not
 *     known; the route out and in again (the same memory); the status and seen lines;
 *   - stop(): not while something else changed the code (its memory kept), then the image the dump again, byte for
 *     byte, the memory freed;
 *   - the routine's memory (the whole list filled, for zone 104) written to build\test\gamerain_cave.bin and run as the
 *     game would by reshade-worldfx's tests/gamerain_cave.c (build\test\gamerain_cave.exe: build.cmd).
 *
 *   build\gamerain_test.exe     (built by build.cmd)
 */

#include "../src/game/gamerain.hpp"
#include "game_dump.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{
	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	// World FX's settings and folder, as the test sets them
	std::map<std::string, std::string> settings;
	std::string folder;

	// Settings as t_gamerain.py's set_ini writes ReShade.ini (nullptr: not set; RainAmount always is)
	void set_settings(const char *rain, const char *amount = "1.000000", const char *snow = nullptr, const char *snow_amount = nullptr)
	{
		settings.clear();
		if (rain != nullptr)
			settings["Rain"] = rain;
		settings["RainAmount"] = amount;
		if (snow != nullptr)
			settings["Snow"] = snow;
		if (snow_amount != nullptr)
			settings["SnowAmount"] = snow_amount;
	}

	constexpr uintptr_t kSite = 0x04609D;   // mov al, [ebp+17Ah] in 046070, every effect element's draw
	constexpr const char *kPattern = "8D8DF8000000D91C2450E8????????8B00A3????????8A857A01000084C00F85";
	constexpr uint32_t kSize = 0x260, kCode = 0x190, kCodeSize = 0xCF;

	// The addon's gr_code for its memory at 0x30000000 (worldfx.lua run under lupa)
	constexpr const char *kRoutine =
		"8A857A01000084C07401C36031DB8BB5"
		"FC00000085F6745A8B3685F674548A46"
		"24247F3C05754BF686DE000000807442"
		"F7850C01000000004000753631FF8B86"
		"E400000085C0740880380175038B780C"
		"B9040000008B462885C074168B3085F6"
		"74108A4624247F3C0174093C05750349"
		"75E3EB4E8B56208B0D0400003083E11F"
		"8914CD90000030893CCD94000030FF05"
		"04000030BE100000308B0685C0742339"
		"D0750B8B460485C0741139F8740D83C6"
		"0881FE9000003072E0EB07FF05000000"
		"3043881D0800003061A008000030C3";

	using pair_list = std::vector<std::pair<std::string, std::string>>; // (weather, model; "" for any)
	const pair_list kRainList = { { "rain", "" }, { "squl", "" }, { "rai1", "" }, { "squ1", "" },
		{ "bolt", "rain" }, { "bolt", "smok" }, { "bolt", "asi1" }, { "bolt", "hamo" },
		{ "bol1", "rain" }, { "bol1", "smok" }, { "bol1", "asi1" }, { "bol1", "hamo" } };
	const pair_list kSnowList = { { "snow", "" }, { "bliz", "" }, { "sno1", "" }, { "bli1", "" } };

	pair_list full_list()
	{
		pair_list all = kRainList;
		all.insert(all.end(), kSnowList.begin(), kSnowList.end());
		return all; // (16 pairs: the routine's whole list, no end marker)
	}

	std::string from_hex(const char *hex)
	{
		std::string out;
		for (size_t i = 0; hex[i] != 0 && hex[i + 1] != 0; i += 2)
		{
			const char two[3] = { hex[i], hex[i + 1], 0 };
			out += char(std::strtoul(two, nullptr, 16));
		}
		return out;
	}

	std::string bytes_at(uintptr_t at, size_t n)
	{
		return std::string(reinterpret_cast<const char *>(at), n);
	}

	uint32_t word_at(uintptr_t at)
	{
		uint32_t v = 0;
		std::memcpy(&v, reinterpret_cast<const void *>(at), 4);
		return v;
	}

	std::string name(uint32_t v)
	{
		return v != 0 ? std::string(reinterpret_cast<const char *>(&v), 4) : std::string();
	}

	// Where a pattern (hex, ?? for any byte) is in the image, every place
	std::vector<uintptr_t> find_all(uintptr_t base, size_t size, const char *pattern)
	{
		std::vector<uint8_t> want;
		std::vector<bool> known;
		for (size_t i = 0; pattern[i] != 0 && pattern[i + 1] != 0; i += 2)
		{
			const char two[3] = { pattern[i], pattern[i + 1], 0 };
			known.push_back(two[0] != '?');
			want.push_back(two[0] != '?' ? uint8_t(std::strtoul(two, nullptr, 16)) : uint8_t(0));
		}
		std::vector<uintptr_t> out;
		const uint8_t *image = reinterpret_cast<const uint8_t *>(base);
		for (size_t at = 0; at + want.size() <= size; ++at)
		{
			size_t i = 0;
			while (i < want.size() && (!known[i] || image[at + i] == want[i]))
				i++;
			if (i == want.size())
				out.push_back(base + at);
		}
		return out;
	}

	// The routine's list as it reads it: pairs up to the first 0 name
	pair_list pairs(uintptr_t mem)
	{
		pair_list out;
		for (uint32_t i = 0; i < 16; ++i)
		{
			const uint32_t w = word_at(mem + 0x10 + i * 8), m = word_at(mem + 0x10 + i * 8 + 4);
			if (w == 0)
				break;
			out.push_back({ name(w), name(m) });
		}
		return out;
	}

	// Every word of the list past its pairs 0 (nothing left over from a longer list)
	bool rest_empty(uintptr_t mem)
	{
		const size_t n = pairs(mem).size();
		for (size_t i = n * 2; i < 32; ++i)
			if (word_at(mem + 0x10 + i * 4) != 0)
				return false;
		return true;
	}

	bool memory_free(uintptr_t at)
	{
		MEMORY_BASIC_INFORMATION mbi = {};
		return VirtualQuery(reinterpret_cast<void *>(at), &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_FREE;
	}

	// A few of the dump's bytes as in its file (not the whole of it: a dump-sized buffer can land where the next dump
	// is to be mapped)
	std::string file_bytes(const std::string &path, size_t at, size_t n)
	{
		std::string out(n, '\0');
		FILE *f = nullptr;
		if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr)
			return std::string();
		const bool ok = fseek(f, long(at), SEEK_SET) == 0 && fread(out.data(), 1, n, f) == n;
		fclose(f);
		return ok ? out : std::string();
	}

	bool write_file(const std::string &path, const std::string &bytes)
	{
		FILE *f = nullptr;
		if (fopen_s(&f, path.c_str(), "wb") != 0 || f == nullptr)
			return false;
		const bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
		fclose(f);
		return ok;
	}

	bool contains(const std::string &s, const char *part)
	{
		return s.find(part) != std::string::npos;
	}

	// HiFi XI's folder (this exe is in its build folder)
	std::string root_folder()
	{
		char path[MAX_PATH] = "";
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		std::string s(path);
		for (int up = 0; up < 2; ++up)
		{
			const size_t at = s.find_last_of("\\/");
			s = at != std::string::npos ? s.substr(0, at) : std::string(".");
		}
		return s + "\\";
	}

	// The cave run on the image file: its lines printed, its exit code (-1 when it couldn't be run)
	int run_cave(const std::string &exe, const std::string &image)
	{
		const std::string command = "\"\"" + exe + "\" \"" + image + "\"\""; // (cmd /c takes the outer quotes off)
		FILE *p = _popen(command.c_str(), "r");
		if (p == nullptr)
			return -1;
		char line[512];
		while (fgets(line, sizeof(line), p) != nullptr)
		{
			std::string s(line);
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
				s.pop_back();
			std::printf("  32-bit: %s\n", s.c_str());
		}
		return _pclose(p);
	}

	// One dump: everything t_gamerain.py checks of the addon, on the port
	void test_dump(const std::string &path, const std::string &cave_image)
	{
		namespace gamerain = hifi::game::gamerain;
		const std::string file = path.substr(path.find_last_of('\\') + 1);
		std::printf("\n%s\n", file.c_str());
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		char line[256];
		std::snprintf(line, sizeof(line), "mapped at its base, 0x%08X", unsigned(base));
		check(size != 0, line);
		if (size == 0)
			return;

		const std::vector<uintptr_t> hits = find_all(base, size, kPattern);
		std::snprintf(line, sizeof(line), "the pattern is unique in %s, at 0x%06X (the mov at +0x16)", file.c_str(), unsigned(kSite - 0x16));
		check(hits.size() == 1 && hits[0] == base + kSite - 0x16, line);
		const uintptr_t site = base + kSite;
		const std::string original = bytes_at(site, 6);
		check(original == from_hex("8A857A010000") && bytes_at(site + 6, 4) == from_hex("84C00F85"),
			"the dump's 04609D: mov al, [ebp+17Ah]; test al, al; jne");
		check(gamerain::status() == "not hidden yet" && gamerain::where().memory == 0, "before the route: not hidden yet, no memory");

		// The route in
		std::string why;
		const bool in = gamerain::route(true, why);
		check(in && why.empty(), "route(true): in" + (why.empty() ? std::string() : " (" + why + ")"));
		const uintptr_t mem = gamerain::where().memory;
		check(mem == dump::kAllocAt && gamerain::where().site == site, "its memory at 0x30000000, the site found at 04609D");
		const std::string call = bytes_at(site, 6);
		const uint32_t target = uint32_t(site + 5 + word_at(site + 1));
		check(uint8_t(call[0]) == 0xE8 && target == mem + kCode && uint8_t(call[5]) == 0x90, "04609D calls the routine (then a nop)");
		check(bytes_at(site + 6, 32) == file_bytes(path, kSite + 6, 32) && bytes_at(site - 32, 32) == file_bytes(path, kSite - 32, 32),
			"only those 6 bytes changed around it");
		check(bytes_at(mem + kCode, kCodeSize) == from_hex(kRoutine), "the routine is the addon's, byte for byte (207 bytes)");
		check(bytes_at(mem + kCode + kCodeSize, kSize - kCode - kCodeSize) == std::string(kSize - kCode - kCodeSize, '\0'),
			"  and nothing past it");

		// The list in each case
		gamerain::update(0);
		check(pairs(mem).empty() && gamerain::status() == "drawn: no zone yet", "no zone yet: empty list (" + gamerain::status() + ")");

		set_settings(nullptr);
		gamerain::update(104);
		check(pairs(mem) == full_list(), "zone 104 with its rain map: the list is the rain's and the snow's (" + std::to_string(pairs(mem).size()) + " pairs)");
		check(gamerain::status() == "hidden while World FX draws its own here: rain and snow (0.0 of its effects skipped a frame)",
			"  status: " + gamerain::status());

		set_settings("0");
		gamerain::update(104);
		check(pairs(mem) == kSnowList && rest_empty(mem), "Rain=0: only the snow's entries (the rest of the list empty)");
		check(contains(gamerain::status(), "snow (World FX's Rain setting is off: rain drawn)"), "  status: " + gamerain::status());
		set_settings("1", "0.000000");
		gamerain::update(104);
		check(pairs(mem) == kSnowList && rest_empty(mem), "RainAmount=0 (World FX draws no rain): only the snow's entries");
		set_settings("1", "1.000000", "0");
		gamerain::update(104);
		check(pairs(mem) == kRainList && rest_empty(mem), "Snow=0: only the rain's entries");
		check(contains(gamerain::status(), "rain (World FX's Snow setting is off: snow drawn)"), "  status: " + gamerain::status());
		set_settings("1", "1.000000", "1", "0.000000");
		gamerain::update(104);
		check(pairs(mem) == kRainList && rest_empty(mem), "SnowAmount=0 (World FX draws no snow): only the rain's entries");
		set_settings("0", "1.000000", "0");
		gamerain::update(104);
		check(pairs(mem).empty() && rest_empty(mem), "Rain=0 and Snow=0: empty list");
		check(gamerain::status() == "drawn: World FX's Rain and Snow settings are off (or their amounts 0)", "  status: " + gamerain::status());
		set_settings("1");
		gamerain::update(104);
		check(pairs(mem) == full_list(), "Rain=1 (Snow not set: on): the whole list is back");

		// Elements skipped a frame: 30 over 10 frames
		*reinterpret_cast<volatile uint32_t *>(mem) += 30;
		for (int i = 0; i < 10; ++i)
			gamerain::count_frame();
		gamerain::update(104);
		check(contains(gamerain::status(), "(3.0 of its effects skipped a frame)"), "30 skipped over 10 frames: " + gamerain::status());
		gamerain::update(104);
		check(contains(gamerain::status(), "(3.0 of its effects"), "  an update with no frame since: the rate kept");
		for (int i = 0; i < 5; ++i)
			gamerain::count_frame();
		gamerain::update(104);
		check(contains(gamerain::status(), "(0.0 of its effects"), "  5 frames with none skipped: 0.0");

		gamerain::update(100);
		check(pairs(mem).empty() && rest_empty(mem), "zone 100 without a rain map: empty list");
		check(gamerain::status() == "drawn: World FX has no rain or snow for zone 100", "  status: " + gamerain::status());

		const std::string kept = folder;
		folder = kept.substr(0, kept.size() - 1) + "_missing\\";
		gamerain::update(104);
		check(pairs(mem).empty() && gamerain::status() == "drawn: World FX has no rain or snow for zone 104",
			"World FX's folder missing: empty list (" + gamerain::status() + ")");
		folder.clear();
		gamerain::update(104);
		check(pairs(mem).empty() && gamerain::status() == "drawn: World FX's folder isn't known",
			"World FX's folder not known: empty list (" + gamerain::status() + ")");
		folder = kept;
		gamerain::update(104);
		check(pairs(mem) == full_list(), "its folder back: the whole list");

		// The route out and in again
		check(gamerain::route(false, why) && why.empty(), "route(false): out");
		check(bytes_at(site, 6) == original, "  the game's 6 bytes are back");
		gamerain::update(104);
		check(pairs(mem).empty() && rest_empty(mem), "  the list emptied");
		check(gamerain::status() == "drawn (not left out: the \"The game's own\" tab of HiFi XI's menu)", "  status: " + gamerain::status());
		check(gamerain::seen() == "nothing seen: the game's rain and snow aren't being sorted (not left out: the \"The game's own\" tab of HiFi XI's menu)",
			"  seen: " + gamerain::seen());
		check(gamerain::route(true, why) && why.empty(), "route(true): in again");
		gamerain::update(104);
		check(bytes_at(site, 6) == call && gamerain::where().memory == mem && pairs(mem) == full_list(),
			"  the call again, the same memory, the list filled");
		check(memory_free(dump::kAllocAt + 0x10000), "  no second allocation");

		// The routine's memory as the game would run it (the list filled), for the 32-bit run
		char head[8];
		const uint32_t at = uint32_t(mem), n = kSize;
		std::memcpy(head, &at, 4);
		std::memcpy(head + 4, &n, 4);
		check(write_file(cave_image, std::string(head, 8) + bytes_at(mem, kSize)), "the routine's memory written to " + cave_image);

		check(gamerain::seen() == "none yet", "seen with nothing drawn: " + gamerain::seen());
		const std::string ring = std::string("rainsmokboltligh") + std::string("rain\0\0\0\0", 8) + "snowhit3";
		std::memcpy(reinterpret_cast<void *>(mem + 0x90), ring.data(), 32);
		*reinterpret_cast<volatile uint32_t *>(mem + 4) = 4;
		check(gamerain::seen() == "bolt/ligh, rain/- (hidden), rain/smok (hidden), snow/hit3 (hidden)", "  after four: " + gamerain::seen());

		// Put back: not over something else's change
		const uint8_t nops[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		hifi::game::write_code(site, nops, sizeof(nops));
		const bool stopped = gamerain::stop(why);
		check(!stopped && why == "something else has changed the game's code since: left as it is", "stop() with the code changed since: " + why);
		check(bytes_at(site, 6) == std::string(6, '\x90') && gamerain::where().memory == mem && !memory_free(mem), "  the code left as it is, the memory kept");
		hifi::game::write_code(site, call.data(), call.size());
		check(gamerain::stop(why), "stop(): put back");
		check(dump::same_as_file(path, base, size), "  the image is the dump again, byte for byte");
		check(memory_free(mem) && gamerain::where().memory == 0 && gamerain::where().site == 0, "  the routine's memory freed");
		check(gamerain::status() == "not hidden yet", "  status: " + gamerain::status());
		dump::unmap(base);
	}
}

extern "C" int __cdecl worldfx_setting(const char *key, char *out, uint32_t size)
{
	const auto v = settings.find(key != nullptr ? key : "");
	if (v == settings.end() || out == nullptr || size == 0)
		return 0;
	const size_t n = v->second.size() < size - 1 ? v->second.size() : size - 1;
	std::memcpy(out, v->second.data(), n);
	out[n] = 0;
	return 1;
}

// (HiFi XI's own options: none set here, each its default)
extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}

extern "C" const char *__cdecl worldfx_folder()
{
	return folder.c_str();
}

int main()
{
	namespace gamerain = hifi::game::gamerain;
	const std::string root = root_folder();
	const std::string cave_exe = root + "build\\test\\gamerain_cave.exe", cave_image = root + "build\\test\\gamerain_cave.bin";

	// World FX's folder: worldfx-rain\104.bin (only whether it's there is read)
	char temp[MAX_PATH] = "";
	GetTempPathA(MAX_PATH, temp);
	folder = std::string(temp) + "hifixi_gamerain_test\\";
	CreateDirectoryA(folder.c_str(), nullptr);
	CreateDirectoryA((folder + "worldfx-rain").c_str(), nullptr);
	check(write_file(folder + "worldfx-rain\\104.bin", "RHM1"), "World FX's folder: " + folder + " with worldfx-rain\\104.bin");
	set_settings(nullptr);

	// Not found: said why, nothing taken
	std::vector<uint8_t> empty(0x10000, 0);
	hifi::game::set_image(reinterpret_cast<uintptr_t>(empty.data()), empty.size());
	std::string why;
	const bool routed = gamerain::route(true, why);
	check(!routed && why == "the game's effect draw wasn't found", "an image without the draw: " + why);
	gamerain::update(104);
	check(gamerain::status() == "drawn: the game's effect draw wasn't found" && gamerain::where().memory == 0,
		"  status: " + gamerain::status() + " (no memory taken)");
	check(gamerain::stop(why), "  stop(): nothing to put back");

	for (const char *path : dump::kDumps)
	{
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		{
			check(false, std::string(path) + " is there");
			continue;
		}
		const int ok_before = passes, failed_before = failures;
		test_dump(path, cave_image);

		// The routine itself, 32-bit, as the game would run it
		if (GetFileAttributesA(cave_exe.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			check(false, cave_exe + " built (build.cmd)");
			continue;
		}
		const int code = run_cave(cave_exe, cave_image);
		check(code == 0, "the routine ran as the game would (32-bit): " + std::string(code == 0 ? "all passed" : "failures above"));
		std::printf("(%s: %d ok, %d failed)\n", path, passes - ok_before, failures - failed_before);
	}

	DeleteFileA((folder + "worldfx-rain\\104.bin").c_str());
	RemoveDirectoryA((folder + "worldfx-rain").c_str());
	RemoveDirectoryA(folder.c_str());
	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures;
}
