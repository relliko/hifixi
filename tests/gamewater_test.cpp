/*
 * Offline test of the game's own water hidden (src/game/gamewater.cpp, the worldfx addon's /worldfx gamewater in
 * C++), as reshade-worldfx's tests/t_gamewater.py tests the addon: each FFXiMain dump mapped at its own base
 * (game_dump.hpp), the routines' memory from 0x30000000, World FX's settings and folder faked (worldfx_setting from a
 * table here, worldfx_folder a folder of the test's own with worldfx-water\<zone>.bin files in it).
 *   - an image without the game's code: said why, nothing taken;
 *   - a dump whose sites something else had changed already (FFXiMain_041C0000.bin was dumped with the addon's
 *     routines in): said why, nothing written, nothing taken; then the game's own bytes put back there (in the mapped
 *     copy) and the rest run on it;
 *   - the call at 040DC4 going to the routine and nothing else around it changed; both routines byte for byte the
 *     addon's (its gw_code and step_code for memory at 0x30000000), the first jumping on to 183760;
 *   - the list in each case: no zone; zone 100 with its water file; Water off and on; zone 104 without a water file
 *     (then with one: looked at once a zone); zone 105 with a water file but no meshes listed; World FX's folder not
 *     known; the route out and in again (the same memory); the status and seen lines; the routines' skipped and steps
 *     counters (both run here on meshes and steps they skip) through status();
 *   - the footstep sites (0D5365 to the step routine, 0D64FA's je to jmp) patched exactly where World FX draws water
 *     (also in a zone with no meshes listed) and put back elsewhere;
 *   - stop(): not while something else changed the code (its memory kept), then the image the dump again, byte for
 *     byte, the memory freed;
 *   - the routines' memory (zone 100's list) written to build\test\gamewater_cave.bin and run as the game would by
 *     reshade-worldfx's tests/gamewater_cave.c (build\test\gamewater_cave.exe: build.cmd).
 *
 *   build\gamewater_test.exe     (built by build.cmd)
 */

#include "../src/game/gamewater.hpp"
#include "game_dump.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (ok)
			passes++;
		else
			failures++;
	}

	// World FX's settings and folder, as the test sets them
	std::map<std::string, std::string> settings;
	std::string folder, folder_given;

	// Settings as t_gamewater.py's set_ini has them for HiFi XI (nullptr: Water not set)
	void set_settings(const char *water)
	{
		settings.clear();
		settings["Grass"] = "1";
		settings["WaterDebug"] = "0";
		settings["WaterDepthTint"] = "0.5";
		if (water != nullptr)
			settings["Water"] = water;
	}

	constexpr uintptr_t kSite = 0x040DC4;  // the call to 183760
	constexpr uintptr_t kDraw = 0x183760;
	constexpr uintptr_t kStep = 0x0D5365;  // push eax; mov ecx, esi; call 0D63F0 (a step's effect)
	constexpr uintptr_t kRing = 0x0D64FA;  // je past the deep-water ring's spawn
	constexpr uintptr_t kSpawn = 0x0D63F0;
	constexpr uint32_t kSize = 0x280, kCode = 0x210, kStepCode = 0x260;

	// The addon's gw_code and step_code for its memory at 0x30000000 (worldfx.lua run under lupa), for each dump's base
	struct lua_bytes
	{
		uintptr_t base;
		const char *routine, *step;
	};
	constexpr lua_bytes kLua[] = {
		{ 0x04490000,
			"8B44240885C074368B40188B15080000"
			"3083E23F89049510010030FF05080000"
			"308B150000003085D274134A3B049510"
			"00003075F2FF0504000030C21000E90D"
			"3561D4",
			"8BD7C1EA0880FA38740E80FA39740950"
			"8BCEE8796156D4C3FF050C000030C3" },
		{ 0x041C0000,
			"8B44240885C074368B40188B15080000"
			"3083E23F89049510010030FF05080000"
			"308B150000003085D274134A3B049510"
			"00003075F2FF0504000030C21000E90D"
			"3534D4",
			"8BD7C1EA0880FA38740E80FA39740950"
			"8BCEE8796129D4C3FF050C000030C3" },
	};

	// gamewater.lua's names for zone 104 (all 13)
	const std::vector<std::string> kZone104 = { "awa1", "ba_3", "chib", "deka", "scl ", "wa_1", "wa_2", "wa_4", "wa_5", "wa_7",
		"wa_8", "wa_9", "wa_l" };

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

	std::string to_hex(const std::string &bytes)
	{
		std::string out;
		char two[4];
		for (const char c : bytes)
		{
			std::snprintf(two, sizeof(two), "%02X", unsigned(uint8_t(c)));
			out += two;
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

	void put_word(uintptr_t at, uint32_t v)
	{
		std::memcpy(reinterpret_cast<void *>(at), &v, 4);
	}

	std::string word_bytes(uint32_t v)
	{
		return std::string(reinterpret_cast<const char *>(&v), 4);
	}

	// The routine's list as it reads it: count names
	std::vector<std::string> names(uintptr_t mem)
	{
		std::vector<std::string> out;
		const uint32_t n = word_at(mem);
		for (uint32_t i = 0; i < n && i < 64; ++i)
			out.push_back(bytes_at(mem + 0x10 + i * 4, 4));
		return out;
	}

	std::string joined(const std::vector<std::string> &list)
	{
		std::string s;
		for (const std::string &n : list)
			s += (s.empty() ? "'" : ", '") + n + "'";
		return s.empty() ? std::string("none") : s;
	}

	bool memory_free(uintptr_t at)
	{
		MEMORY_BASIC_INFORMATION mbi = {};
		return VirtualQuery(reinterpret_cast<void *>(at), &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_FREE;
	}

	std::string read_file(const std::string &path)
	{
		std::string out;
		FILE *f = nullptr;
		if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr)
			return out;
		char buffer[65536];
		size_t n;
		while ((n = fread(buffer, 1, sizeof(buffer), f)) != 0)
			out.append(buffer, n);
		fclose(f);
		return out;
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

	bool starts(const std::string &s, const char *part)
	{
		return s.compare(0, std::strlen(part), part) == 0;
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

	// The cave run on the image file: its lines printed and counted, its exit code (-1 when it couldn't be run)
	int run_cave(const std::string &exe, const std::string &image, int &ok_lines, int &fail_lines)
	{
		ok_lines = fail_lines = 0;
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
			ok_lines += starts(s, "ok    ") ? 1 : 0;
			fail_lines += starts(s, "FAIL  ") ? 1 : 0;
			std::printf("  32-bit: %s\n", s.c_str());
		}
		return _pclose(p);
	}

	// The game's call to 183760 at 040DC4, as its draw makes it
	using routine_fn = void(__fastcall *)(void *self, void *edx, void *matrix, void *mesh, int a3, int a4);

	// The step routine called as 0D5365 calls it (edi = the effect's name; eax and esi anything: only names it skips
	// are given, so 0D63F0 isn't reached): esp after minus esp before (0 when balanced)
	int run_step(uintptr_t routine, uint32_t name)
	{
		uint32_t before = 0, after = 0;
		__asm
		{
			push esi
			push edi
			mov edi, name
			xor esi, esi
			xor eax, eax
			mov before, esp
			call routine
			mov after, esp
			pop edi
			pop esi
		}
		return int(after - before);
	}

	// One dump: everything t_gamewater.py checks of the addon, on the port
	void test_dump(const std::string &path, const std::string &cave_exe, const std::string &cave_image)
	{
		namespace gamewater = hifi::game::gamewater;
		const std::string file = path.substr(path.find_last_of('\\') + 1);
		std::printf("\n%s\n", file.c_str());
		const int passes_before = passes, failures_before = failures;
		// (mapped before the file is read: a copy of it on the heap could sit where the dump must go)
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		const std::string dumped = size != 0 ? read_file(path) : std::string();
		char line[512];
		std::snprintf(line, sizeof(line), "mapped at its base, 0x%08X", unsigned(base));
		check(size != 0 && size == dumped.size(), line);
		if (size == 0)
			return;
		const lua_bytes *lua = nullptr;
		for (const lua_bytes &l : kLua)
			if (l.base == base)
				lua = &l;
		check(lua != nullptr, "the addon's routines known for this base");
		if (lua == nullptr)
		{
			dump::unmap(base);
			return;
		}
		DeleteFileA((folder + "worldfx-water\\104.bin").c_str());
		DeleteFileA((folder + "worldfx-water\\105.bin").c_str());
		set_settings(nullptr);
		folder_given = folder;

		const uintptr_t site = base + kSite, draw = base + kDraw, step = base + kStep, ring = base + kRing, spawn = base + kSpawn;
		// The game's own bytes at the three sites
		const std::string own_call = "\xE8" + word_bytes(uint32_t(draw - (site + 5)));
		const std::string own_step = "\x50\x8B\xCE\xE8" + word_bytes(uint32_t(spawn - (step + 8)));
		const std::string own_ring = "\x74";
		const std::string dumped_call = bytes_at(site, 5), dumped_step = bytes_at(step, 8), dumped_ring = bytes_at(ring, 1);
		const bool changed = dumped_call != own_call || dumped_step != own_step || dumped_ring != own_ring;
		std::string why;
		if (changed)
		{
			// Something else had the sites when this was dumped (the addon's routines, elsewhere): left alone
			std::printf("(the dump's sites aren't the game's own: 040DC4 %s, 0D5365 %s, 0D64FA %s)\n", to_hex(dumped_call).c_str(),
				to_hex(dumped_step).c_str(), to_hex(dumped_ring).c_str());
			const bool routed = gamewater::route(true, why);
			check(!routed && why == "the game's water draw isn't as expected (changed by something else?)", "route(true) with the call going elsewhere: " + why);
			gamewater::update(100);
			const std::string s = gamewater::status();
			check(starts(s, "drawn: the game's water draw isn't as expected (changed by something else?); wading ripples drawn: the game's footstep code "),
				"  update(100): " + s);
			check(gamewater::where().memory == 0 && memory_free(dump::kAllocAt), "  no memory taken");
			check(dump::same_as_file(path, base, size), "  nothing written: the image is the dump");
			check(gamewater::stop(why) && dump::same_as_file(path, base, size), "  stop(): nothing to put back");
			// The game's own bytes put back in the mapped copy, for the rest
			std::memcpy(reinterpret_cast<void *>(site), own_call.data(), 5);
			std::memcpy(reinterpret_cast<void *>(step), own_step.data(), 8);
			std::memcpy(reinterpret_cast<void *>(ring), own_ring.data(), 1);
		}
		const std::string clean = bytes_at(base, size);

		check(bytes_at(site, 5) == own_call, "the call at 040DC4 goes to 183760");
		check(bytes_at(step, 8) == own_step && bytes_at(ring, 1) == own_ring,
			"0D5365 is push eax; mov ecx, esi; call 0D63F0, and 0D64FA the je past the deep-water ring");
		check(gamewater::status() == "not hidden yet" && gamewater::where().memory == 0, "before the route: not hidden yet, no memory");

		// The 8 bytes at 0D5365 and the 1 at 0D64FA: "hidden" (both ours), "shown" (both the game's), else what's there
		const auto feet = [&](uintptr_t mem) -> std::string
		{
			const std::string s = bytes_at(step, 8), r = bytes_at(ring, 1);
			if (s == own_step && r == own_ring)
				return "shown";
			if (uint8_t(s[0]) == 0xE8 && uint32_t(step + 5 + word_at(step + 1)) == mem + kStepCode && s.substr(5) == "\x90\x90\x90" && r == "\xEB")
				return "hidden";
			return "mixed " + to_hex(s) + " " + to_hex(r);
		};

		// The route in
		const bool routed = gamewater::route(true, why);
		check(routed && why.empty(), "route(true): in" + (why.empty() ? std::string() : " (" + why + ")"));
		const uintptr_t mem = gamewater::where().memory;
		check(mem == dump::kAllocAt && gamewater::where().call == site && gamewater::where().draw == draw,
			"its memory at 0x30000000, the call found at 040DC4, 183760 its draw");
		const std::string call = bytes_at(site, 5);
		check(uint8_t(call[0]) == 0xE8 && uint32_t(site + 5 + word_at(site + 1)) == mem + kCode, "the call at 040DC4 goes to the routine");
		check(bytes_at(site + 5, 16) == clean.substr(kSite + 5, 16) && bytes_at(site - 20, 20) == clean.substr(kSite - 20, 20),
			"  only those 5 bytes changed around it");
		const std::string routine = bytes_at(mem + kCode, 67);
		check(routine == from_hex(lua->routine), "  the routine byte for byte the addon's (67 bytes)" +
			(routine == from_hex(lua->routine) ? std::string() : ": " + to_hex(routine)));
		check(uint8_t(routine[62]) == 0xE9 && uint32_t(mem + kCode + 67 + word_at(mem + kCode + 63)) == draw, "  and it jumps on to 183760");
		check(feet(mem) == "shown" && names(mem).empty(), "no zone yet: the footstep code as it was, an empty list (" + feet(mem) + ")");

		gamewater::update(0);
		check(names(mem).empty() && feet(mem) == "shown" && gamewater::status() == "drawn: no zone yet", "update(0): " + gamewater::status());

		gamewater::update(100);
		const std::vector<std::string> zone100 = { "ike ", "ikm " };
		check(names(mem) == zone100, "zone 100 with its water file: the list is " + joined(names(mem)));
		check(feet(mem) == "hidden", "  and the wading ripples hidden (" + feet(mem) + ")");
		check(gamewater::where().step == step && gamewater::where().spawn == spawn && gamewater::where().ring == ring,
			"  the step's call found at 0D5365 (0D63F0 its spawn), the ring's jump at 0D64FA");
		check(bytes_at(step + 8, 16) == clean.substr(kStep + 8, 16) && bytes_at(step - 16, 16) == clean.substr(kStep - 16, 16) &&
			bytes_at(ring + 1, 16) == clean.substr(kRing + 1, 16) && bytes_at(ring - 16, 16) == clean.substr(kRing - 16, 16),
			"  only those 8 bytes and 1 byte changed around them");
		const std::string step_routine = bytes_at(mem + kStepCode, 31);
		check(step_routine == from_hex(lua->step), "  the step routine byte for byte the addon's (31 bytes)" +
			(step_routine == from_hex(lua->step) ? std::string() : ": " + to_hex(step_routine)));
		check(gamewater::status() == "hidden here (2 meshes: ike ikm; 0.0 layers skipped a frame); wading ripples hidden (0 steps in water so far)",
			"  status: " + gamewater::status());

		set_settings("0");
		gamewater::update(100);
		check(names(mem).empty() && feet(mem) == "shown", "Water 0: empty list, footsteps as they were");
		check(gamewater::status() == "drawn: World FX's Water setting is off", "  status: " + gamewater::status());
		set_settings("false");
		gamewater::update(100);
		check(names(mem).empty() && feet(mem) == "shown", "Water false: the same");
		set_settings("1");
		gamewater::update(100);
		check(names(mem) == zone100 && feet(mem) == "hidden", "Water 1: the list and the footstep patch are back");

		gamewater::update(104);
		check(names(mem).empty() && feet(mem) == "shown" && gamewater::status() == "drawn: World FX has no water for zone 104",
			"zone 104 without a water file: empty list, footsteps as they were (" + gamewater::status() + ")");
		write_file(folder + "worldfx-water\\104.bin", "WTR2");
		gamewater::update(104);
		check(names(mem).empty(), "zone 104's file added: not seen while still in the zone (looked at once a zone)");
		gamewater::update(101);
		gamewater::update(104);
		check(names(mem) == kZone104 && feet(mem) == "hidden", "back in zone 104: its " + std::to_string(names(mem).size()) + " names (" +
			joined(names(mem)) + ")");

		write_file(folder + "worldfx-water\\105.bin", "WTR2");
		gamewater::update(105);
		check(names(mem).empty() && feet(mem) == "hidden" &&
			gamewater::status() == "drawn: no water meshes known for zone 105; wading ripples hidden (0 steps in water so far)",
			"zone 105 with a water file but no meshes listed: empty list, wading ripples still hidden (" + gamewater::status() + ")");

		folder_given.clear();
		gamewater::update(100);
		check(names(mem).empty() && feet(mem) == "shown" && gamewater::status() == "drawn: World FX's folder isn't known",
			"World FX's folder not known: empty list, footsteps as they were (" + gamewater::status() + ")");
		folder_given = folder;
		gamewater::update(100);
		check(names(mem) == zone100 && feet(mem) == "hidden", "its folder known again: zone 100's list, the wading ripples hidden");

		// The route out and in again
		check(gamewater::route(false, why) && why.empty(), "route(false): out");
		check(bytes_at(site, 5) == own_call, "  the game's call is back");
		gamewater::update(100);
		check(names(mem).empty() && feet(mem) == "shown", "  update(100): empty list, the footstep code back");
		check(gamewater::status() == "drawn (not left out: the \"The game's own\" tab of HiFi XI's menu)", "  status: " + gamewater::status());
		check(gamewater::seen() == "nothing seen: the game's water isn't being sorted (not left out: the \"The game's own\" tab of HiFi XI's menu)",
			"  seen: " + gamewater::seen());
		check(gamewater::route(true, why) && why.empty(), "route(true): in again");
		gamewater::update(100);
		check(bytes_at(site, 5) == call && gamewater::where().memory == mem && names(mem) == zone100 && feet(mem) == "hidden",
			"  both routines again, the same memory, the list filled");
		check(memory_free(dump::kAllocAt + 0x10000), "  no second allocation");
		check(starts(gamewater::status(), "hidden here (2 meshes: ike ikm; ") && contains(gamewater::status(), "wading ripples hidden"),
			"  status: " + gamewater::status());

		// The routines' memory as the game would run them (zone 100's list), for the 32-bit run
		char head[16];
		const uint32_t header[4] = { uint32_t(mem), uint32_t(draw), uint32_t(spawn), kSize };
		std::memcpy(head, header, sizeof(head));
		check(write_file(cave_image, std::string(head, sizeof(head)) + bytes_at(mem, kSize)), "the routines' memory written to " + cave_image);

		check(gamewater::seen() == "none yet", "seen with nothing drawn: " + gamewater::seen());
		std::memcpy(reinterpret_cast<void *>(mem + 0x110), "ike clodikm ", 12);
		put_word(mem + 0x8, 3);
		check(gamewater::seen() == "clod, ike (hidden), ikm (hidden)", "  after three: " + gamewater::seen());

		// The routines run here on what they skip (the rest would go on into the game's code): the counters through status()
		if (routine == from_hex(lua->routine) && step_routine == from_hex(lua->step))
		{
			uint32_t mesh[16] = {};
			std::memcpy(&mesh[6], "ike ", 4); // (+0x18: its name)
			const routine_fn run = reinterpret_cast<routine_fn>(mem + kCode);
			for (int i = 0; i < 4; ++i)
				gamewater::count_frame();
			for (int i = 0; i < 10; ++i)
				run(reinterpret_cast<void *>(0x12345678), nullptr, reinterpret_cast<void *>(0x1111), mesh, 1, 0);
			gamewater::update(100);
			check(word_at(mem + 0x4) == 10 && word_at(mem + 0x8) == 13, "10 draws of 'ike ' skipped by the routine (and seen)");
			check(contains(gamewater::status(), "; 2.5 layers skipped a frame)"), "  over 4 frames: " + gamewater::status());
			int moved = 0;
			for (const char *n : { "w800", "0800", "w900" })
			{
				uint32_t v = 0;
				std::memcpy(&v, n, 4);
				moved |= run_step(mem + kStepCode, v);
			}
			check(word_at(mem + 0xC) == 3 && moved == 0, "3 steps in water skipped by the step routine, the stack as it was");
			check(contains(gamewater::status(), "; wading ripples hidden (3 steps in water so far)"), "  status: " + gamewater::status());
			gamewater::update(100);
			check(contains(gamewater::status(), "; 0.0 layers skipped a frame)"), "  no frames since: " + gamewater::status());
		}
		else
			check(false, "the routines not run here (not the addon's)");

		// Put back: not over something else's change
		const uint8_t nops[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
		hifi::game::write_code(site, nops, sizeof(nops));
		why.clear();
		const bool stopped = gamewater::stop(why);
		check(!stopped && why == "something else has changed the game's call since: left as it is", "stop() with the call changed since: " + why);
		check(bytes_at(site, 5) == std::string(5, '\x90') && gamewater::where().memory == mem && !memory_free(mem),
			"  the call left as it is, the memory kept");
		check(feet(mem) == "shown", "  the footstep code put back all the same (" + feet(mem) + ")");
		hifi::game::write_code(site, call.data(), call.size());
		why.clear();
		const bool put_back = gamewater::stop(why);
		check(put_back && why.empty(), "stop(): put back" + (why.empty() ? std::string() : " (" + why + ")"));
		check(bytes_at(base, size) == clean, "  the image as it was before the route, byte for byte");
		if (changed)
		{
			std::memcpy(reinterpret_cast<void *>(site), dumped_call.data(), 5);
			std::memcpy(reinterpret_cast<void *>(step), dumped_step.data(), 8);
			std::memcpy(reinterpret_cast<void *>(ring), dumped_ring.data(), 1);
		}
		check(dump::same_as_file(path, base, size), changed ? "  the image the dump again, byte for byte (its sites as dumped put back)" :
			"  the image the dump again, byte for byte");
		check(memory_free(mem) && gamewater::where().memory == 0 && gamewater::where().call == 0 && gamewater::where().step == 0,
			"  the routines' memory freed, everything forgotten");
		check(gamewater::status() == "not hidden yet", "  status: " + gamewater::status());
		dump::unmap(base);

		// The routines themselves, 32-bit, as the game would run them
		if (GetFileAttributesA(cave_exe.c_str()) == INVALID_FILE_ATTRIBUTES)
			check(false, cave_exe + " built (build.cmd)");
		else
		{
			int ok_lines = 0, fail_lines = 0;
			const int code = run_cave(cave_exe, cave_image, ok_lines, fail_lines);
			std::snprintf(line, sizeof(line), "the routines ran as the game would (32-bit): %s (%d ok, %d failed)",
				code == 0 ? "all passed" : "failures above", ok_lines, fail_lines);
			check(code == 0 && ok_lines > 0 && fail_lines == 0, line);
		}
		DeleteFileA((folder + "worldfx-water\\104.bin").c_str());
		DeleteFileA((folder + "worldfx-water\\105.bin").c_str());
		std::printf("(%s: %d ok, %d failed)\n", file.c_str(), passes - passes_before, failures - failures_before);
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
	return folder_given.c_str();
}

int main()
{
	namespace gamewater = hifi::game::gamewater;
	const std::string root = root_folder();
	const std::string cave_exe = root + "build\\test\\gamewater_cave.exe", cave_image = root + "build\\test\\gamewater_cave.bin";

	// World FX's folder: worldfx-water\100.bin (only whether it's there is read)
	char temp[MAX_PATH] = "";
	GetTempPathA(MAX_PATH, temp);
	folder = std::string(temp) + "hifixi_gamewater_test\\";
	folder_given = folder;
	CreateDirectoryA(folder.c_str(), nullptr);
	CreateDirectoryA((folder + "worldfx-water").c_str(), nullptr);
	check(write_file(folder + "worldfx-water\\100.bin", "WTR2"), "World FX's folder: " + folder + " with worldfx-water\\100.bin");
	set_settings(nullptr);

	// Not found: said why, nothing taken
	std::vector<uint8_t> empty(0x10000, 0);
	hifi::game::set_image(reinterpret_cast<uintptr_t>(empty.data()), empty.size());
	std::string why;
	const bool routed = gamewater::route(true, why);
	check(!routed && why == "the game's water draw wasn't found", "an image without the draw: " + why);
	gamewater::update(100);
	check(gamewater::status() == "drawn: the game's water draw wasn't found; wading ripples drawn: the game's footstep code wasn't found" &&
		gamewater::where().memory == 0, "  status: " + gamewater::status() + " (no memory taken)");
	check(gamewater::stop(why), "  stop(): nothing to put back");

	for (const char *path : dump::kDumps)
	{
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		{
			check(false, std::string(path) + " is there");
			continue;
		}
		test_dump(path, cave_exe, cave_image);
	}

	DeleteFileA((folder + "worldfx-water\\100.bin").c_str());
	RemoveDirectoryA((folder + "worldfx-water").c_str());
	RemoveDirectoryA(folder.c_str());
	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures;
}
