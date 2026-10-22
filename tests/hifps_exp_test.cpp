/*
 * Offline test of hifps's experimental toggles and graphics settings in C++ (src/game/hifps/exp.cpp, gfx.cpp), held
 * against the hifps addon itself (C:\Games\PhoenixXI\addons\hifps\hifps.lua) step by step, byte for byte.
 *   - stubs.inc is what tools/hifps_stubs.py makes of the addon's proxy/*.blob and *_code.lua now;
 *   - an image without the game's code: each says why, nothing taken;
 *   - each FFXiMain dump mapped at its own base (game_dump.hpp), our memory from 0x30000000; its sites checked to
 *     be the game's own (both dumps are, at every site here); fakes of what the dump only points at, at 0x38000000:
 *     the client's renderer and device (its vtable), Ashita's four draw methods, the code its per-draw loop calls
 *     and its plugin list (8 plugins: named, unnamed, faulted, flags read and not...), the CPU feature object; the
 *     dump's renderer and CPU feature pointers pointed at them (in the mapped copy, put back at the end); addons'
 *     Lua files in a folder of the test's own;
 *   - a scenario of 40 steps (the 'ashita' check refusing, forced, a newcomer kept and one turning it off, on with
 *     the proxy's vtable in; occlusion at 256, 128, 1024 and 64fps; walls refused while its call goes elsewhere,
 *     on, its kept actors and stats, left alone when repointed; sse on AMD and Intel; batch and dedupe with their
 *     stats; lamps, night lighting refused while its entry is held then on, the hour's factors, the lamp stats;
 *     hifps off and on again, the saved toggles restored; unload), run by the addon under lupa
 *     (tests/hifps_exp_lua.py) and by the port: after each step the image (where it isn't the dump file), the
 *     fakes and our memory (the stubs as laid down, their fields) the same byte for byte, and the addon's state
 *     (on, why not, what each is doing) the same text;
 *   - what only the port does: the toggles wait for the client's device; nothing put back over bytes that aren't
 *     ours (occlusion, sse, the device's vtable), the memory kept then; stop() frees our memory, and the image is
 *     the dump again, byte for byte;
 *   - the walls and gfx stubs as the port laid them down, run by the addon's own harnesses (proxy/walls_harness.c,
 *     gfx_harness.c, built 32-bit by build_hifps_exp_test.cmd into build\test_hifps_exp\).
 *
 *   build\hifps_exp_test.exe     (built by tests\build_hifps_exp_test.cmd)
 */

#include "../src/game/hifps/exp.hpp"
#include "../src/game/hifps/gfx.hpp"
#include "game_dump.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
#include "../src/game/hifps/stubs.inc"

	namespace exp = hifi::game::hifps::exp;
	namespace gfx = hifi::game::hifps::gfx;

	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (ok)
			passes++;
		else
			failures++;
	}

	// ---------------------------------------------------------------- HiFi XI's options, the host, the clock

	std::map<std::string, std::string> options;
	void set_option(const char *key, const std::string &value)
	{
		options[std::string("hifps.") + key] = value;
	}

	std::vector<std::string> host_addons;
	bool host_addons_known = false;
	bool list_addons(std::vector<std::string> &names)
	{
		names = host_addons;
		return host_addons_known;
	}
	uint32_t actor_me = 0, actor_target = 0, actor_status = 0;
	bool actors_known = false;
	bool get_actors(uint32_t &me, uint32_t &target, uint32_t &status)
	{
		me = actor_me;
		target = actor_target;
		status = actor_status;
		return actors_known;
	}
	exp::host host;
	uint32_t clock_now = 0;
	// hifps's frame step (step.hpp), stubbed: on, and its frames a second
	bool step_on = false;
	double step_fps = 0.0;
	uint32_t __cdecl test_clock()
	{
		return clock_now;
	}
	// (the game clock's timer stood in for: the game's import isn't resolved here)
	DWORD WINAPI stand_in_now()
	{
		return 123456;
	}
	// (a place nothing is at: reserved, then given back)
	uintptr_t gone_place()
	{
		static const uintptr_t at = [] {
			void *a = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
			VirtualFree(a, 0, MEM_RELEASE);
			return reinterpret_cast<uintptr_t>(a);
		}();
		return at;
	}

	// ---------------------------------------------------------------- bytes

	std::string hex(const void *p, size_t n)
	{
		static const char d[] = "0123456789ABCDEF";
		std::string s;
		for (size_t i = 0; i < n; ++i)
		{
			const uint8_t b = static_cast<const uint8_t *>(p)[i];
			s += d[b >> 4];
			s += d[b & 15];
		}
		return s;
	}
	std::string hex(const std::string &b)
	{
		return hex(b.data(), b.size());
	}
	std::string word_bytes(uint32_t v)
	{
		return std::string(reinterpret_cast<const char *>(&v), 4);
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
	void put_bytes(uintptr_t at, const std::string &b)
	{
		std::memcpy(reinterpret_cast<void *>(at), b.data(), b.size());
	}
	std::string from_hex(const char *h)
	{
		std::string out;
		for (size_t i = 0; h[i] != 0 && h[i + 1] != 0; i += 2)
		{
			const char two[3] = { h[i], h[i + 1], 0 };
			out += char(std::strtoul(two, nullptr, 16));
		}
		return out;
	}
	std::string f32_bytes(float f)
	{
		return std::string(reinterpret_cast<const char *>(&f), 4);
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
	std::vector<std::string> split(const std::string &s, char sep)
	{
		std::vector<std::string> out;
		size_t at = 0;
		while (at <= s.size())
		{
			const size_t next = std::min(s.find(sep, at), s.size());
			out.push_back(s.substr(at, next - at));
			at = next + 1;
		}
		return out;
	}
	std::string joined(const std::vector<std::string> &l, const char *sep)
	{
		std::string s;
		for (size_t i = 0; i < l.size(); ++i)
			s += (i > 0 ? sep : "") + l[i];
		return s;
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

	// A command's output lines (printed, indented) and exit code (-1: not run)
	int run(const std::string &command, std::vector<std::string> &lines, bool print)
	{
		lines.clear();
		FILE *p = _popen(("\"" + command + "\" 2>&1").c_str(), "r");
		if (p == nullptr)
			return -1;
		char line[4096];
		while (fgets(line, sizeof(line), p) != nullptr)
		{
			std::string s(line);
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
				s.pop_back();
			lines.push_back(s);
			if (print)
				std::printf("  | %s\n", s.c_str());
		}
		return _pclose(p);
	}

	// ---------------------------------------------------------------- the fakes (what the dump only points at)

	constexpr uintptr_t kFakes = 0x38000000;
	constexpr size_t kFakesSize = 0x4000;
	constexpr uintptr_t kRenderer = kFakes + 0x0000; // +8: the device
	constexpr uintptr_t kDevice = kFakes + 0x0100;   // +0: its vtable
	constexpr uintptr_t kRtti = kFakes + 0x0400;     // vtable[-1]
	constexpr uintptr_t kVtable = kFakes + 0x0404;   // 128 methods
	constexpr uintptr_t kCpu = kFakes + 0x0700;      // +9: SSE skinning
	// Ashita.dll: its four draw methods, the core getter and the plugins' draw loop they call, the core, the plugin
	// manager, its entries, the plugins, their vtables, their functions, their names
	constexpr uintptr_t kAsh = kFakes + 0x1000;
	constexpr size_t kAshSize = 0x3000;
	constexpr uintptr_t kMethods[4] = { kAsh + 0x000, kAsh + 0x100, kAsh + 0x200, kAsh + 0x300 };
	constexpr uint32_t kPatAt[4] = { 0x05, 0x08, 0x20, 0x3A }; // where each one's plugin check starts
	constexpr uintptr_t kGetter = kAsh + 0x400, kPmFn = kAsh + 0x500, kCore = kAsh + 0x700, kPm = kAsh + 0x900;
	constexpr uintptr_t kEntries = kAsh + 0xA00;  // 0x40 each: +0Ch faulted, +10h the plugin, +1Ch the next
	constexpr uintptr_t kPlugs = kAsh + 0xD00;    // 0x10 each: +0 its vtable
	constexpr uintptr_t kPluginVts = kAsh + 0xE00; // 0x60 each: +0 name, +18h flags, +44h..+50h the draw callbacks
	constexpr uintptr_t kFuncs = kAsh + 0x1200;   // 0x10 each
	constexpr uintptr_t kStrings = kAsh + 0x1800; // 0x20 each
	enum
	{
		F_NAME0 = 0, // 0-7: 'mov eax, offset name; ret'
		F_FLAGS8 = 8,
		F_FLAGS8B,
		F_FLAGS0,
		F_ODD,
		F_FALSE1,
		F_FALSE2,
		F_FALSE3,
		F_FALSE4,
		F_DRAW,
		F_COUNT
	};
	constexpr const char *kPluginNames[8] = { "addons", "quiet", "drawer", "noflag", "oddflags", "faulty", "", "newbie" };
	constexpr uintptr_t func(int i) { return kFuncs + 0x10 * uintptr_t(i); }
	constexpr uintptr_t entry(int i) { return kEntries + 0x40 * uintptr_t(i); }
	constexpr uintptr_t plug(int i) { return kPlugs + 0x10 * uintptr_t(i); }
	constexpr uintptr_t plugin_vt(int i) { return kPluginVts + 0x60 * uintptr_t(i); }
	constexpr uintptr_t callback(int plugin, int q) { return plugin_vt(plugin) + 0x44 + 4 * uintptr_t(q); }

	void put32(uintptr_t at, uint32_t v)
	{
		std::memcpy(reinterpret_cast<void *>(at), &v, 4);
	}
	void put_hex(uintptr_t at, const char *h)
	{
		put_bytes(at, from_hex(h));
	}
	void put_rel(uintptr_t at, uintptr_t to) // E8 rel32 at `at`
	{
		*reinterpret_cast<uint8_t *>(at) = 0xE8;
		put32(at + 1, uint32_t(to - (at + 5)));
	}

	bool map_fakes()
	{
		return VirtualAlloc(reinterpret_cast<void *>(kFakes), kFakesSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE) ==
			reinterpret_cast<void *>(kFakes);
	}

	void build_fakes()
	{
		std::memset(reinterpret_cast<void *>(kFakes), 0xCC, kFakesSize);
		std::memset(reinterpret_cast<void *>(kRenderer), 0, 0x40);
		put32(kRenderer + 8, uint32_t(kDevice));
		std::memset(reinterpret_cast<void *>(kDevice), 0, 0x10);
		put32(kDevice, uint32_t(kVtable));
		put32(kRtti, 0x12345678);
		for (uint32_t i = 0; i < 128; ++i)
			put32(kVtable + 4 * i, i >= 70 && i <= 73 ? uint32_t(kMethods[i - 70]) : 0x7E000000 + 0x10 * i);
		std::memset(reinterpret_cast<void *>(kCpu), 0, 0x20);
		// The draw methods: 'cmp dword [eax+10Ch], 0; je; mov ecx, [eax+134h]; test ecx, ecx; je' in each; the first
		// (DrawPrimitive) calls the core's getter before it and the plugins' draw loop after it
		for (int m = 0; m < 4; ++m)
			put_hex(kMethods[m] + kPatAt[m], "83B80C0100000074208B883401000085C97410");
		put_rel(kMethods[0], kGetter);
		put_rel(kMethods[0] + 24, kPmFn);
		put_hex(kMethods[0] + 29, "84C07405C21400");
		put_hex(kGetter, "558BEC");
		put_hex(kGetter + 3, "B8");
		put32(kGetter + 4, uint32_t(kCore));
		put_hex(kGetter + 8, "8B4DF45DC3");
		// The plugins' draw loop: head +8, faulted +0Ch, the plugin +10h, its GetFlags +18h, its draw callbacks
		// +44h, the link at +4
		put_hex(kPmFn, "568BF18B4608A801754032C938480C75308B48108B018B4018FFD0A80874108B48108B018B4044FF7508FFD08B4E048B04085EC3");
		std::memset(reinterpret_cast<void *>(kCore), 0, 0x200);
		put32(kCore + 0x134, uint32_t(kPm));
		std::memset(reinterpret_cast<void *>(kPm), 0, 0x40);
		put32(kPm + 4, 0x1C);
		put32(kPm + 8, uint32_t(entry(0)));
		// Their functions
		for (int i = 0; i < 8; ++i)
		{
			put_hex(func(F_NAME0 + i), "B8");
			put32(func(F_NAME0 + i) + 1, uint32_t(kStrings + 0x20 * uintptr_t(i)));
			put_hex(func(F_NAME0 + i) + 5, "C3");
			std::memset(reinterpret_cast<void *>(kStrings + 0x20 * uintptr_t(i)), 0, 0x20);
			std::memcpy(reinterpret_cast<void *>(kStrings + 0x20 * uintptr_t(i)), kPluginNames[i], std::strlen(kPluginNames[i]));
		}
		put_hex(func(F_FLAGS8), "B808000000C3");
		put_hex(func(F_FLAGS8B), "6A0858C3");
		put_hex(func(F_FLAGS0), "33C0C3");
		put_hex(func(F_ODD), "558BEC8B45085DC3");
		put_hex(func(F_FALSE1), "32C0C3");
		put_hex(func(F_FALSE2), "30C0C21400");
		put_hex(func(F_FALSE3), "B000C3");
		put_hex(func(F_FALSE4), "33C0C21800");
		put_hex(func(F_DRAW), "558BEC8B450885C00F95C05DC21400");
		// The plugins: name, flags, the four draw callbacks
		struct plugin
		{
			int name, flags, cb[4];
			bool faulted;
		};
		const plugin plugins[8] = {
			{ F_NAME0 + 0, F_FLAGS8, { F_DRAW, F_DRAW, F_DRAW, F_DRAW }, false },             // addons: left out by name
			{ F_NAME0 + 1, F_FLAGS8B, { F_FALSE1, F_FALSE2, F_FALSE3, F_FALSE4 }, false },     // quiet: only returns false
			{ F_NAME0 + 2, F_FLAGS8, { F_FALSE1, F_FALSE1, F_DRAW, F_FALSE1 }, false },        // drawer
			{ F_NAME0 + 3, F_FLAGS0, { F_DRAW, F_DRAW, F_DRAW, F_DRAW }, false },              // noflag: not flagged for draws
			{ F_NAME0 + 4, F_ODD, { F_FALSE2, F_FALSE2, F_FALSE2, F_FALSE2 }, false },         // oddflags: flags not readable
			{ F_NAME0 + 5, F_FLAGS8, { F_DRAW, F_DRAW, F_DRAW, F_DRAW }, true },               // faulty: faulted
			{ F_ODD, F_FLAGS8, { F_FALSE1, F_FALSE1, F_FALSE1, F_DRAW }, false },              // (no name: 'plugin at ...')
			{ F_NAME0 + 7, F_FLAGS8B, { F_DRAW, F_FALSE1, F_FALSE1, F_FALSE1 }, false },       // newbie: not in the list yet
		};
		for (int i = 0; i < 8; ++i)
		{
			std::memset(reinterpret_cast<void *>(entry(i)), 0, 0x40);
			reinterpret_cast<uint8_t *>(entry(i))[0x0C] = plugins[i].faulted ? 1 : 0;
			put32(entry(i) + 0x10, uint32_t(plug(i)));
			put32(entry(i) + 0x1C, i < 6 ? uint32_t(entry(i + 1)) : 0);
			std::memset(reinterpret_cast<void *>(plug(i)), 0, 0x10);
			put32(plug(i), uint32_t(plugin_vt(i)));
			std::memset(reinterpret_cast<void *>(plugin_vt(i)), 0, 0x60);
			put32(plugin_vt(i), uint32_t(func(plugins[i].name)));
			put32(plugin_vt(i) + 0x18, uint32_t(func(plugins[i].flags)));
			for (int q = 0; q < 4; ++q)
				put32(callback(i, q), uint32_t(func(plugins[i].cb[q])));
		}
	}

	// ---------------------------------------------------------------- the scenario

	struct step
	{
		std::string action;
		int fps = 0, plugins = 8;
		std::string addons = "alpha,zeta,hifps", actors = "0A001000,0A002000,0";
		uint32_t clock = 0;
	};
	struct edit
	{
		int step;
		std::string where; // abs, walls, gfx, proxy
		uint32_t off;
		std::string bytes;
	};
	struct file_edit
	{
		int step;
		std::string path, content;
	};
	struct scenario
	{
		std::vector<step> steps;
		std::vector<edit> edits; // (step -1: before the first, in the dump)
		std::vector<file_edit> files;
	};

	constexpr uint32_t kDay = 3456; // Earth seconds a Vana'diel day (144 an hour)
	const char *const kAlpha = "-- alpha: uses 'd3d_dp' only in a comment\nlocal x = 1\nashita.events.register('d3d_dip', 'alpha_dip', function () end);\n";
	const char *const kHelper = "  -- \"d3d_dpup\"\nashita.events.register(\"d3d_dipup\", 'h', function () end);\n";
	const char *const kQuiet = "-- nothing here\nlocal y = 'd3d_dp_not'\n";

	// The sites the scenario changes in the dump (as another addon would)
	struct dump_sites
	{
		uintptr_t walls_call = 0, amb = 0, renderer_global = 0, cpu_global = 0;
		std::string walls_rel, amb_byte;
	};

	scenario make_scenario(const dump_sites &d, const std::string &install)
	{
		scenario s;
		step cur;
		cur.clock = kDay * 1000 + 22 * 144;
		const auto next = [&]() { return int(s.steps.size()); };
		const auto add = [&](const std::string &action) { cur.action = action; s.steps.push_back(cur); };
		const auto edit_at = [&](const std::string &where, uintptr_t off, const std::string &b) { s.edits.push_back({ next(), where, uint32_t(off), b }); };
		const auto file_at = [&](const std::string &rel, const char *content) { s.files.push_back({ next(), install + "addons\\" + rel, content }); };
		// (before the first step: the dump's pointers to the renderer and the CPU feature object at the fakes)
		s.edits.push_back({ -1, "abs", uint32_t(d.renderer_global), word_bytes(uint32_t(kRenderer)) });
		s.edits.push_back({ -1, "abs", uint32_t(d.cpu_global), word_bytes(uint32_t(kCpu)) });

		file_at("alpha\\alpha.lua", kAlpha);
		file_at("alpha\\lib\\helper.lua", kHelper);
		file_at("zeta\\zeta.lua", kQuiet);
		add("start");
		add("on ashita");                                   // refused: alpha, drawer and the unnamed one draw
		add("force");                                       // forced on
		cur.fps = 256;
		add("tick");
		cur.plugins = 9;
		edit_at("abs", entry(6) + 0x1C, word_bytes(uint32_t(entry(7))));
		add("tick");                                        // newbie: a newcomer, kept (forced)
		add("off ashita");
		edit_at("abs", callback(2, 2), word_bytes(uint32_t(func(F_FALSE1))));
		edit_at("abs", callback(6, 3), word_bytes(uint32_t(func(F_FALSE3))));
		edit_at("abs", callback(7, 0), word_bytes(uint32_t(func(F_FALSE4))));
		file_at("alpha\\alpha.lua", kQuiet);
		file_at("alpha\\lib\\helper.lua", kQuiet);
		add("on ashita");                                   // nothing uses them now
		cur.plugins = 10;
		edit_at("abs", callback(7, 0), word_bytes(uint32_t(func(F_DRAW))));
		add("tick");                                        // newbie draws: turned off
		add("off ashita");
		add("on occlusion");                                // at 256fps: every 16 frames
		cur.fps = 128;
		add("tick");
		cur.fps = 1024;
		add("tick");
		edit_at("abs", d.walls_call + 1, word_bytes(uint32_t(d.walls_call + 0x40 - (d.walls_call + 5))));
		add("on walls");                                    // refused: its call goes elsewhere
		edit_at("abs", d.walls_call + 1, d.walls_rel);
		add("off walls");
		add("on walls");
		cur.fps = 64;
		cur.actors = "0A001000,0,4";
		add("tick");                                        // a cutscene, no target
		const uint32_t walls_counts[7] = { 640, 1280, 320, 64, 5, 3, 640000 };
		for (int i = 0; i < 7; ++i)
			edit_at("walls", stubs::walls::S_TESTS + 4 * i, word_bytes(walls_counts[i]));
		add("tick");
		add("on sse");                                      // an AMD CPU
		add("off sse");
		edit_at("abs", kCpu + 9, "\x01");
		add("on sse");                                      // an Intel CPU: nothing to change
		edit_at("abs", kCpu + 9, std::string(1, '\0'));
		add("off sse");
		add("on batch");
		add("on dedupe");
		add("on ashita");                                   // with the proxy's vtable in: refused, newbie draws
		add("off ashita");
		cur.fps = 256;
		add("tick");
		edit_at("proxy", stubs::proxy::S_SKIPPED, word_bytes(25600));
		edit_at("proxy", stubs::proxy::S_MERGED, word_bytes(51200));
		edit_at("proxy", stubs::proxy::S_BATCHES, word_bytes(2560));
		add("tick");
		add("off batch");
		add("off dedupe");
		add("gfx 1.5 1.25 1 1");
		edit_at("abs", d.amb, "\xE9");
		add("gfx 1.3 1.7 1.8 1.4");                         // night lighting refused: its entry is held
		edit_at("abs", d.amb, d.amb_byte);
		add("gfx 1.3 1.7 1.75 1.4");
		cur.fps = 128;
		cur.clock = kDay * 1001 + 792;                      // 5:30
		add("tick");
		cur.clock = kDay * 1002 + 2628;                     // 18:15
		edit_at("gfx", stubs::gfx::S_VSC, word_bytes(1280));
		edit_at("gfx", stubs::gfx::S_LIGHT, word_bytes(5120));
		edit_at("gfx", stubs::gfx::S_ACTOR, word_bytes(384));
		add("tick");
		cur.clock = kDay * 1003 + 12 * 144;                 // noon
		add("tick");
		add("disable");
		add("enable");                                      // occlusion and walls back on, the lamps and night lighting
		cur.fps = 256;
		cur.actors = "none";
		add("tick");
		edit_at("abs", d.walls_call + 1, word_bytes(uint32_t(d.walls_call + 0x40 - (d.walls_call + 5))));
		add("off walls");                                   // its call repointed by something else since: left
		add("unload");
		return s;
	}

	// ---------------------------------------------------------------- the addon's run (tests/hifps_exp_lua.py)

	struct chunk
	{
		uint32_t addr;
		std::string bytes;
	};
	struct snapshot
	{
		std::string action;
		std::vector<chunk> chunks;
		std::vector<std::string> texts;
	};

	bool read_out(const std::string &path, std::vector<snapshot> &out)
	{
		const std::string d = read_file(path);
		size_t at = 0;
		const auto u32 = [&](uint32_t &v) {
			if (at + 4 > d.size())
				return false;
			std::memcpy(&v, d.data() + at, 4);
			at += 4;
			return true;
		};
		const auto str = [&](uint32_t n, std::string &s) {
			if (at + n > d.size())
				return false;
			s.assign(d.data() + at, n);
			at += n;
			return true;
		};
		while (at < d.size())
		{
			snapshot s;
			uint32_t index = 0, n = 0, count = 0;
			if (!u32(index) || !u32(n) || !str(n, s.action) || !u32(count))
				return false;
			for (uint32_t i = 0; i < count; ++i)
			{
				chunk c;
				if (!u32(c.addr) || !u32(n) || !str(n, c.bytes))
					return false;
				s.chunks.push_back(c);
			}
			if (!u32(count))
				return false;
			for (uint32_t i = 0; i < count; ++i)
			{
				std::string t;
				if (!u32(n) || !str(n, t))
					return false;
				s.texts.push_back(t);
			}
			out.push_back(s);
		}
		return true;
	}

	std::string step_line(int i, const step &s)
	{
		std::string a = s.action;
		std::replace(a.begin(), a.end(), ' ', '_');
		char line[256];
		std::snprintf(line, sizeof(line), "step %d %s fps=%d plugins=%d addons=%s actors=%s clock=%u\n", i, a.c_str(), s.fps, s.plugins, s.addons.c_str(),
			s.actors.c_str(), s.clock);
		return line;
	}

	// ---------------------------------------------------------------- the port's run

	// The image where it isn't the dump file, the fakes, our memory (as the addon's run lists them)
	snapshot take(const std::string &file, uintptr_t base, size_t size, bool memory)
	{
		snapshot s;
		const auto live = reinterpret_cast<const uint8_t *>(base);
		const auto was = reinterpret_cast<const uint8_t *>(file.data());
		size_t i = 0;
		while (i < size)
		{
			const size_t n = std::min<size_t>(4096, size - i);
			if (std::memcmp(was + i, live + i, n) == 0)
			{
				i += n;
				continue;
			}
			const size_t end = i + n;
			while (i < end)
			{
				if (was[i] != live[i])
				{
					size_t j = i;
					while (j < size && was[j] != live[j])
						++j;
					s.chunks.push_back({ uint32_t(base + i), std::string(reinterpret_cast<const char *>(live + i), j - i) });
					i = j;
				}
				else
					++i;
			}
		}
		s.chunks.push_back({ uint32_t(kFakes), bytes_at(kFakes, kFakesSize) });
		if (memory)
		{
			std::vector<std::pair<uintptr_t, size_t>> mem;
			if (exp::where().walls != 0)
				mem.push_back({ exp::where().walls, stubs::walls::size });
			if (exp::where().proxy != 0)
				mem.push_back({ exp::where().proxy, stubs::proxy::size + 16 + 0x20000 });
			if (gfx::where().memory != 0)
				mem.push_back({ gfx::where().memory, stubs::gfx::size });
			std::sort(mem.begin(), mem.end());
			for (const auto &m : mem)
				s.chunks.push_back({ uint32_t(m.first), bytes_at(m.first, m.second) });
		}
		const char *names[6] = { "ashita", "occlusion", "walls", "sse", "batch", "dedupe" };
		for (const char *n : names)
		{
			const exp::toggle_state t = exp::toggle(n);
			s.texts.push_back(std::string("on.") + n + "=" + (t.on ? "1" : "0"));
			s.texts.push_back(std::string("err.") + n + "=" + t.err);
			s.texts.push_back(std::string("stat.") + n + "=" + t.stat);
		}
		const exp::ashita_view a = exp::ashita_state();
		s.texts.push_back("ashita.users=" + joined(a.users, "; "));
		s.texts.push_back("ashita.checked=" + a.checked);
		s.texts.push_back("ashita.notes=" + joined(a.notes, "; "));
		s.texts.push_back(std::string("ashita.force=") + (a.force ? "1" : "0"));
		const gfx::view v = gfx::state();
		s.texts.push_back(std::string("gfx.lamps=") + (v.lamps ? "1" : "0"));
		s.texts.push_back(std::string("gfx.night=") + (v.night ? "1" : "0"));
		s.texts.push_back("gfx.err=" + v.lamps_err);
		s.texts.push_back("gfx.nerr=" + v.night_err);
		s.texts.push_back("gfx.stat=" + v.lamps_stat);
		s.texts.push_back("night.stat=" + v.night_stat);
		return s;
	}

	// Where the two differ first, empty when they don't
	std::string differ(const snapshot &lua, const snapshot &port, bool texts)
	{
		const size_t n = std::max(lua.chunks.size(), port.chunks.size());
		for (size_t i = 0; i < n; ++i)
		{
			if (i >= lua.chunks.size() || i >= port.chunks.size())
			{
				const chunk &c = i < lua.chunks.size() ? lua.chunks[i] : port.chunks[i];
				char s[160];
				std::snprintf(s, sizeof(s), "%s only has %u bytes at %08X: %s", i < lua.chunks.size() ? "the addon" : "the port",
					unsigned(c.bytes.size()), c.addr, hex(c.bytes.substr(0, 16)).c_str());
				return s;
			}
			const chunk &a = lua.chunks[i], &b = port.chunks[i];
			if (a.addr != b.addr || a.bytes.size() != b.bytes.size())
			{
				char s[200];
				std::snprintf(s, sizeof(s), "the addon has %u bytes at %08X (%s), the port %u at %08X (%s)", unsigned(a.bytes.size()), a.addr,
					hex(a.bytes.substr(0, 12)).c_str(), unsigned(b.bytes.size()), b.addr, hex(b.bytes.substr(0, 12)).c_str());
				return s;
			}
			if (a.bytes != b.bytes)
			{
				size_t k = 0;
				while (a.bytes[k] == b.bytes[k])
					++k;
				const size_t from = k & ~size_t(3);
				char s[200];
				std::snprintf(s, sizeof(s), "at %08X: the addon %s, the port %s", unsigned(a.addr + from), hex(a.bytes.substr(from, 12)).c_str(),
					hex(b.bytes.substr(from, 12)).c_str());
				return s;
			}
		}
		if (texts)
		{
			for (size_t i = 0; i < std::max(lua.texts.size(), port.texts.size()); ++i)
			{
				const std::string a = i < lua.texts.size() ? lua.texts[i] : std::string("(none)");
				const std::string b = i < port.texts.size() ? port.texts[i] : std::string("(none)");
				if (a != b)
					return "the addon's " + a + "; the port's " + b;
			}
		}
		return std::string();
	}

	void set_host_now(const step &st)
	{
		host.plugins = st.plugins;
		host_addons_known = st.addons != "?";
		host_addons.clear();
		size_t at = 0;
		while (host_addons_known && at <= st.addons.size())
		{
			const size_t comma = std::min(st.addons.find(',', at), st.addons.size());
			if (comma > at)
				host_addons.push_back(st.addons.substr(at, comma - at));
			at = comma + 1;
		}
		actors_known = st.actors != "none";
		if (actors_known)
		{
			char *end = nullptr;
			actor_me = std::strtoul(st.actors.c_str(), &end, 16);
			actor_target = std::strtoul(end + 1, &end, 16);
			actor_status = std::strtoul(end + 1, &end, 16);
		}
		clock_now = st.clock;
	}

	double t_now = 1000.0;
	bool timers = false; // (both modules' frame rate timers going)

	// A second of presents at fps (exactly: 1000 + whole seconds, steps of 1/fps)
	void tick(int fps)
	{
		step_fps = fps;
		if (!timers)
		{
			exp::present(t_now);
			gfx::present(t_now);
			timers = true;
		}
		exp::present(t_now + 0.5);
		gfx::present(t_now + 0.5);
		exp::present(t_now + 1.0);
		gfx::present(t_now + 1.0);
		t_now += 1.0;
	}

	void act(const std::string &action, const step &st)
	{
		const std::vector<std::string> words = split(action, ' ');
		const std::string name = words.size() > 1 ? words[1] : std::string();
		if (action == "start")
		{
			step_on = true;
			for (const char *n : { "exp_ashita", "exp_occlusion", "exp_walls", "exp_sse", "exp_batch", "exp_dedupe", "ashita_force" })
				set_option(n, "0");
			for (const char *n : { "gfx_lamps", "gfx_reach", "gfx_moon", "gfx_fill" })
				set_option(n, "1");
			timers = false;
			exp::update();
			gfx::update();
		}
		else if (words[0] == "on")
		{
			set_option(("exp_" + name).c_str(), "1");
			exp::update();
		}
		else if (words[0] == "off")
		{
			set_option(("exp_" + name).c_str(), "0");
			if (name == "ashita")
				set_option("ashita_force", "0");
			exp::update();
		}
		else if (action == "force")
		{
			set_option("ashita_force", "1");
			exp::update();
		}
		else if (action == "tick")
			tick(st.fps);
		else if (words[0] == "gfx" && words.size() == 5)
		{
			set_option("gfx_lamps", words[1]);
			set_option("gfx_reach", words[2]);
			set_option("gfx_moon", words[3]);
			set_option("gfx_fill", words[4]);
			gfx::update();
		}
		else if (action == "disable" || action == "enable")
		{
			step_on = action == "enable";
			timers = false;
			exp::update();
			gfx::update();
		}
		else if (action == "unload")
		{
			exp::stop();
			gfx::stop();
		}
		else
			check(false, "unknown action " + action);
	}

	uintptr_t base_of(const std::string &where)
	{
		if (where == "walls")
			return exp::where().walls;
		if (where == "proxy")
			return exp::where().proxy;
		if (where == "gfx")
			return gfx::where().memory;
		return 0;
	}

	// ---------------------------------------------------------------- the addon's harnesses

	// A stub as the port laid it down, back to the blob its harness reads (the copy's base taken off each relocated
	// word): its size, relocations, symbols, bytes
	std::string blob(const std::string &laid, uintptr_t base, const uint32_t *relocs, size_t nrel, const uint32_t *syms, size_t nsym)
	{
		std::string b = laid;
		for (size_t i = 0; i < nrel; ++i)
		{
			uint32_t v = 0;
			std::memcpy(&v, b.data() + relocs[i], 4);
			v -= uint32_t(base);
			std::memcpy(b.data() + relocs[i], &v, 4);
		}
		std::string out = word_bytes(uint32_t(b.size())) + word_bytes(uint32_t(nrel)) + word_bytes(uint32_t(nsym));
		for (size_t i = 0; i < nrel; ++i)
			out += word_bytes(relocs[i]);
		for (size_t i = 0; i < nsym; ++i)
			out += word_bytes(syms[i]);
		return out + b;
	}

	void run_harness(const std::string &folder, const char *name, const std::string &blob_bytes)
	{
		const std::string exe = folder + name + "_harness.exe";
		if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			check(false, exe + " built (tests\\build_hifps_exp_test.cmd)");
			return;
		}
		if (!write_file(folder + name + ".blob", blob_bytes))
		{
			check(false, std::string("writing ") + name + ".blob");
			return;
		}
		std::vector<std::string> lines;
		// (in its folder, where it reads its blob; by full path: cmd may not look in the current folder)
		const int code = run("cd /d \"" + folder + "\" && \"" + exe + "\"", lines, true);
		const bool said_ok = !lines.empty() && starts(lines.back(), "OK");
		check(code == 0 && said_ok, std::string("the ") + name + " stub as the port laid it down, run by the addon's " + name + "_harness.c (32-bit): " +
			(code == 0 && said_ok ? "passed" : "failed (exit " + std::to_string(code) + ")"));
	}

	// ---------------------------------------------------------------- one dump

	struct dump_run
	{
		std::string walls_laid, gfx_laid;
		uintptr_t walls_base = 0, gfx_base = 0;
	};

	void test_dump(const std::string &path, const std::string &root, dump_run &laid)
	{
		const std::string file = path.substr(path.find_last_of('\\') + 1);
		std::printf("\n%s\n", file.c_str());
		const int passes_before = passes, failures_before = failures;
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		const std::string dumped = size != 0 ? read_file(path) : std::string();
		char line[512];
		std::snprintf(line, sizeof(line), "mapped at its base, 0x%08X", unsigned(base));
		check(size != 0 && size == dumped.size(), line);
		if (size == 0)
			return;
		using hifi::game::find;
		using hifi::game::rel_target;
		using hifi::game::word;

		// The sites, the game's own in this dump? (FFXiMain_041C0000.bin was dumped with addons' patches in)
		dump_sites d;
		const uintptr_t walls = find("8D8EF0080000E8????????3BC58986F4");
		const uintptr_t vis = find("A1????????53568BF18A9828080000");
		d.walls_call = walls != 0 ? walls + 6 : 0;
		d.amb = find("8B41E483EC2083F8015674");
		const uintptr_t dev = find("8BC8E8????????85C0A3????????0F84????????8B5C241C");
		const uintptr_t cpu = find("A1????????8A4009C3");
		d.renderer_global = dev != 0 ? word(dev + 10) : 0;
		d.cpu_global = cpu != 0 ? word(cpu + 1) : 0;
		const bool stock_walls = d.walls_call != 0 && rel_target(d.walls_call) == vis;
		const bool stock_occl = find("8BBEF0090000E8????????250300008079054883C8FC403BC7") != 0;
		const uintptr_t setlight = find("83EC68538B5C2474");
		const uintptr_t actor_lamp = find("8B441C4C83C6C45056E8");
		const uintptr_t scale = actor_lamp != 0 ? rel_target(actor_lamp + 9) : 0;
		bool stock_lamps = find("6A0468????????6A0F8B400C508B08FF913C010000") != 0;
		for (const char *pat : { "8B95B09403008B0D????????5652E8", "8B95B49403008B0D????????5652E8", "8B95B89403008B0D????????5652E8",
				 "8B85BC9403008B0D????????5650E8" })
			stock_lamps = stock_lamps && find(pat) != 0 && rel_target(find(pat) + 14) == setlight;
		stock_lamps = stock_lamps && scale != 0 && hifi::game::code_is(scale, from_hex("8B442404D9442408D808D918").data(), 12);
		const bool stock_night = find("83EC3053568B74243C578BD933FF897E34") != 0 && d.amb != 0;
		std::snprintf(line, sizeof(line), "the dump's sites are the game's own: walls %s, occlusion %s, lamps %s, night lighting %s",
			stock_walls ? "yes" : "NO", stock_occl ? "yes" : "NO", stock_lamps ? "yes" : "NO", stock_night ? "yes" : "NO");
		check(stock_walls && stock_occl && stock_lamps && stock_night, line);
		check(d.renderer_global != 0 && d.cpu_global != 0, "the client's renderer and CPU feature pointers found");
		if (!stock_walls || !stock_occl || !stock_lamps || !stock_night || d.renderer_global == 0 || d.cpu_global == 0)
		{
			check(false, "(the scenario needs the game's own bytes at every site: not run on this dump)");
			dump::unmap(base);
			return;
		}
		d.walls_rel = bytes_at(d.walls_call + 1, 4);
		d.amb_byte = bytes_at(d.amb, 1);
		const std::string renderer_was = bytes_at(d.renderer_global, 4), cpu_was = bytes_at(d.cpu_global, 4);
		build_fakes();

		// Before the client's device exists (no renderer yet: its pointer 0, in the mapped copy; the dump's own points at
		// the game's heap, which may be anything here): the saved toggles wait
		options.clear();
		step_on = true;
		set_option("exp_occlusion", "1");
		put_bytes(d.renderer_global, std::string(4, '\0'));
		exp::update();
		check(!exp::toggle("occlusion").on && exp::status().size() > 1 && contains(exp::status()[0], "once the client's device exists"),
			"no client device yet: the saved toggles wait (" + exp::status()[0] + ")");
		put_bytes(d.renderer_global, renderer_was);
		check(dump::same_as_file(path, base, size), "  nothing written");
		exp::stop();
		options.clear();
		hifi::game::set_alloc_at(dump::kAllocAt);

		// The addon's run
		const std::string install = root + "build\\test_hifps_exp\\install\\";
		const scenario sc = make_scenario(d, install);
		for (const edit &e : sc.edits)
			if (e.step < 0)
				put_bytes(e.off, e.bytes);
		HMODULE k32 = GetModuleHandleA("kernel32.dll");
		const uint32_t qpc = uint32_t(reinterpret_cast<uintptr_t>(GetProcAddress(k32, "QueryPerformanceCounter")));
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		std::string in;
		in += "dump " + path + "\n";
		in += "install " + install + "\n";
		std::snprintf(line, sizeof(line), "qpc %08X\nfreq %lld\nalloc %08X\nashita %08X %X\n", qpc, freq.QuadPart, unsigned(dump::kAllocAt),
			unsigned(kAsh), unsigned(kAshSize));
		in += line;
		std::snprintf(line, sizeof(line), "region %08X ", unsigned(kFakes));
		in += line + hex(reinterpret_cast<const void *>(kFakes), kFakesSize) + "\n";
		for (const edit &e : sc.edits)
		{
			std::snprintf(line, sizeof(line), "edit %d %s %X ", e.step, e.where.c_str(), e.off);
			in += line + hex(e.bytes) + "\n";
		}
		for (const file_edit &f : sc.files)
			in += "file " + std::to_string(f.step) + " " + f.path + " " + hex(f.content) + "\n";
		for (size_t i = 0; i < sc.steps.size(); ++i)
			in += step_line(int(i), sc.steps[i]);
		const std::string folder = root + "build\\test_hifps_exp\\";
		const std::string in_path = folder + "lua_in.txt", out_path = folder + "lua_out.bin";
		write_file(in_path, in);
		DeleteFileA(out_path.c_str());
		std::vector<std::string> lines;
		const int code = run("python \"" + root + "tests\\hifps_exp_lua.py\" \"" + in_path + "\" \"" + out_path + "\"", lines, true);
		std::vector<snapshot> lua;
		check(code == 0 && read_out(out_path, lua) && lua.size() == sc.steps.size(),
			"the addon ran the " + std::to_string(sc.steps.size()) + " steps under lupa (tests\\hifps_exp_lua.py)");

		// The port's run, step by step against the addon's
		exp::set_host(&host);
		gfx::set_clock(test_clock);
		t_now = 1000.0;
		options.clear();
		for (size_t i = 0; i < sc.steps.size(); ++i)
		{
			const step &st = sc.steps[i];
			set_host_now(st);
			for (const edit &e : sc.edits)
				if (e.step == int(i))
					put_bytes(base_of(e.where) + e.off, e.bytes);
			for (const file_edit &f : sc.files)
				if (f.step == int(i))
				{
					CreateDirectoryA((install + "addons").c_str(), nullptr);
					const std::string dir = f.path.substr(0, f.path.find_last_of('\\'));
					CreateDirectoryA(dir.substr(0, dir.find_last_of('\\')).c_str(), nullptr);
					CreateDirectoryA(dir.c_str(), nullptr);
					write_file(f.path, f.content);
				}
			act(st.action, st);
			const bool unloaded = st.action == "unload";
			const snapshot port = take(dumped, base, size, !unloaded);
			if (i < lua.size())
			{
				snapshot l = lua[i];
				if (unloaded)
					l.chunks.resize(std::min(l.chunks.size(), port.chunks.size()));
				const std::string why = differ(l, port, !unloaded);
				std::snprintf(line, sizeof(line), "step %2u %-24s", unsigned(i), st.action.c_str());
				check(why.empty() && l.action == st.action, std::string(line) + (why.empty() ? " the same as the addon's" : ": " + why));
			}
			if (st.action == "on walls" && exp::toggle("walls").on && laid.walls_laid.empty())
			{
				laid.walls_base = exp::where().walls;
				laid.walls_laid = bytes_at(laid.walls_base, stubs::walls::size);
			}
			if (gfx::where().memory != 0 && laid.gfx_laid.empty())
			{
				laid.gfx_base = gfx::where().memory;
				laid.gfx_laid = bytes_at(laid.gfx_base, stubs::gfx::size);
			}
		}
		const exp::sites w = exp::where();
		check(w.walls == 0 && w.proxy == 0 && gfx::where().memory == 0, "unload: everything forgotten");
		check(laid.walls_base == dump::kAllocAt && !memory_free(laid.walls_base),
			"  the walls stub's memory kept (its call was repointed by something else, and is still)");
		check(memory_free(laid.gfx_base) && memory_free(dump::kAllocAt + 0x10000), "  the gfx and proxy stubs' memory freed");
		check(bytes_at(d.walls_call + 1, 4) != d.walls_rel, "  the repointed call left as it is");
		put_bytes(d.walls_call + 1, d.walls_rel);
		VirtualFree(reinterpret_cast<void *>(laid.walls_base), 0, MEM_RELEASE);

		// What only the port does: nothing put back over bytes that aren't ours, the memory kept then
		hifi::game::set_alloc_at(dump::kAllocAt + 0x100000);
		options.clear();
		step_on = true;
		// (the others off: each is on by default)
		for (const char *n : { "exp_ashita", "exp_walls", "exp_dedupe" })
			set_option(n, "0");
		set_option("exp_occlusion", "1");
		set_option("exp_sse", "1");
		set_option("exp_batch", "1");
		exp::update();
		check(exp::toggle("occlusion").on && exp::toggle("sse").on && exp::toggle("batch").on, "occlusion, sse and batch on again");
		const uintptr_t occl = exp::where().occlusion, proxy = exp::where().proxy;
		put_bytes(occl, std::string(12, '\x90'));
		put_bytes(kCpu + 9, "\x02");
		put32(kDevice, 0x7E7E7E7C);
		set_option("exp_occlusion", "0");
		set_option("exp_sse", "0");
		set_option("exp_batch", "0");
		exp::update();
		const std::vector<std::string> st = exp::status();
		check(!exp::toggle("occlusion").on && !exp::toggle("sse").on && !exp::toggle("batch").on && bytes_at(occl, 12) == std::string(12, '\x90') &&
			bytes_at(kCpu + 9, 1) == "\x02" && word_at(kDevice) == 0x7E7E7E7C,
			"turned off after something else changed their bytes: left as they are");
		check(st.size() >= 3 && contains(joined(st, "|"), "occlusion: something else has changed the occlusion test since: left as it is") &&
			contains(joined(st, "|"), "sse: something else has changed the CPU's SSE flag since: left as it is") &&
			contains(joined(st, "|"), "batch/dedupe: something else has replaced the client device's vtable since: left as it is"),
			"  and the status says so");
		exp::stop();
		check(!memory_free(proxy), "  stop(): the proxy's memory kept (the device may still use our vtable)");
		VirtualFree(reinterpret_cast<void *>(proxy), 0, MEM_RELEASE);
		put_bytes(occl, from_hex("250300008079054883C8FC40"));
		put_bytes(kCpu + 9, std::string(1, '\0'));
		put32(kDevice, uint32_t(kVtable));

		// hifps 0.9.10's launch crash: the game's clock reads through its object's pointer, 0 until the client starts
		// its clock. Night lighting turns on then, but the clock isn't called: its first call is through the game's
		// import table (not resolved here: called, the test would crash). Then a clock object that isn't there: the
		// guard catches the fault, the clock is left alone after.
		{
			gfx::stop();
			gfx::set_clock(nullptr);
			const uintptr_t clock = hifi::game::find("E8????????0305????????C3");
			const uintptr_t tick = clock != 0 ? hifi::game::rel_target(hifi::game::rel_target(clock)) : 0;
			const uintptr_t object_at = tick != 0 ? hifi::game::word(tick + 9) : 0;
			const uintptr_t now_import = tick != 0 ? hifi::game::word(hifi::game::rel_target(tick + 2) + 2) : 0; // (jmp [import])
			check(object_at >= base && object_at < base + size && now_import >= base && now_import < base + size,
				"the clock's object pointer and its timer's import found in the image");
			if (object_at >= base && object_at < base + size && now_import >= base && now_import < base + size)
			{
				const uint32_t object_was = hifi::game::word(object_at), import_was = hifi::game::word(now_import);
				put32(object_at, 0);
				options.clear();
				step_on = true;
				set_option("gfx_moon", "1.5");
				gfx::update();
				for (int f = 0; f < 3; ++f)
					gfx::present(10.0 + f * 1.1);
				const std::string s = joined(gfx::status(), "|");
				check(gfx::state().night && contains(s, "Night lighting starts once the game's clock runs (after logging in)"),
					"its object not set yet: night lighting on, the clock not called, stock until it runs (" + s + ")");
				// (the timer a stand-in; the object somewhere that isn't there)
				put32(now_import, uint32_t(reinterpret_cast<uintptr_t>(&stand_in_now)));
				put32(object_at, uint32_t(gone_place() + 0x80));
				for (int f = 0; f < 3; ++f)
					gfx::present(20.0 + f * 1.1);
				const std::string t = joined(gfx::status(), "|");
				check(contains(t, "The game's clock faulted when asked the hour: night lighting stays stock"),
					"its object gone: the fault caught, the clock left alone (" + t + ")");
				gfx::stop();
				put32(object_at, object_was);
				put32(now_import, import_was);
			}
			gfx::set_clock(test_clock);
		}

		// stop() with everything on: the image the dump again, our memory freed
		hifi::game::set_alloc_at(dump::kAllocAt + 0x200000);
		options.clear();
		step_on = true;
		for (const char *n : { "exp_occlusion", "exp_walls", "exp_sse", "exp_batch", "exp_dedupe" })
			set_option(n, "1");
		set_option("gfx_lamps", "2");
		set_option("gfx_moon", "1.5");
		host_addons_known = false;
		exp::update();
		gfx::update();
		const uintptr_t mw = exp::where().walls, mp = exp::where().proxy, mg = gfx::where().memory;
		check(exp::toggle("occlusion").on && exp::toggle("walls").on && exp::toggle("sse").on && exp::toggle("batch").on && exp::toggle("dedupe").on &&
			gfx::state().lamps && gfx::state().night, "everything on");
		exp::stop();
		gfx::stop();
		check(memory_free(mw) && memory_free(mp) && memory_free(mg), "stop(): our memory freed");
		put_bytes(d.renderer_global, renderer_was);
		put_bytes(d.cpu_global, cpu_was);
		check(dump::same_as_file(path, base, size), "  the image the dump again, byte for byte (the test's pointers to its fakes put back)");
		dump::unmap(base);
		std::printf("(%s: %d ok, %d failed)\n", file.c_str(), passes - passes_before, failures - failures_before);
	}
}

// hifps's frame step (src/game/hifps/step.cpp), stubbed: the toggles and settings follow these
namespace hifi::game::hifps
{
	bool on()
	{
		return step_on;
	}
	double fps()
	{
		return step_fps;
	}
}

extern "C" int __cdecl hifixi_option(const char *section, const char *key, char *out, uint32_t size)
{
	const auto v = options.find(std::string(section != nullptr ? section : "") + "." + (key != nullptr ? key : ""));
	if (v == options.end() || out == nullptr || size == 0)
		return 0;
	const size_t n = v->second.size() < size - 1 ? v->second.size() : size - 1;
	std::memcpy(out, v->second.data(), n);
	out[n] = 0;
	return 1;
}
extern "C" void __cdecl hifixi_set_option(const char *section, const char *key, const char *value)
{
	options[std::string(section) + "." + key] = value != nullptr ? value : "";
}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t)
{
	return 0;
}
extern "C" const char *__cdecl worldfx_folder()
{
	return "";
}

int main()
{
	const std::string root = root_folder();
	const std::string folder = root + "build\\test_hifps_exp\\";
	CreateDirectoryA(folder.c_str(), nullptr);
	CreateDirectoryA((folder + "install").c_str(), nullptr);
	host.addons = list_addons;
	host.actors = get_actors;
	host.install = folder + "install\\";
	host.ashita_base = kAsh;
	host.ashita_size = kAshSize;

	// stubs.inc is what tools/hifps_stubs.py makes of the addon's blobs now
	{
		std::vector<std::string> lines;
		const std::string made = folder + "stubs.inc";
		const int code = run("python \"" + root + "tools\\hifps_stubs.py\" C:\\Games\\PhoenixXI\\addons\\hifps \"" + made + "\"", lines, false);
		check(code == 0 && read_file(made) == read_file(root + "src\\game\\hifps\\stubs.inc"),
			"src\\game\\hifps\\stubs.inc is what tools\\hifps_stubs.py makes of the addon's proxy/*.blob and *_code.lua now");
	}

	// An image without the game's code: each says why, nothing taken
	{
		std::vector<uint8_t> empty(0x10000, 0);
		hifi::game::set_image(reinterpret_cast<uintptr_t>(empty.data()), empty.size());
		hifi::game::set_alloc_at(dump::kAllocAt);
		exp::set_host(&host);
		gfx::set_clock(test_clock);
		step_on = true;
		set_option("gfx_lamps", "1.5");
		set_option("gfx_moon", "1.5");
		exp::update();
		gfx::update();
		const gfx::view v = gfx::state();
		check(!v.lamps && v.lamps_err == "the lamp constants call was not found (changed client, or another addon patched it)" && !v.night &&
			v.night_err == "the light builder was not found (changed client, or another addon hooks it)",
			"an image without the game's code: lamps: " + v.lamps_err + "; night: " + v.night_err);
		check(contains(exp::status()[0], "once the client's device exists"), "  the toggles wait for the client's device: " + exp::status()[0]);
		check(memory_free(dump::kAllocAt), "  no memory taken");
		exp::stop();
		gfx::stop();
		options.clear();
	}

	if (!map_fakes())
	{
		check(false, "the fakes mapped at 0x38000000");
		return failures;
	}
	dump_run laid;
	for (const char *path : dump::kDumps)
	{
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		{
			check(false, std::string(path) + " is there");
			continue;
		}
		dump_run one;
		test_dump(path, root, one);
		if (laid.walls_laid.empty() && !one.walls_laid.empty() && !one.gfx_laid.empty())
			laid = one;
	}

	// The stubs as the port laid them down, run by the addon's own harnesses
	std::printf("\nthe addon's harnesses\n");
	if (laid.walls_laid.empty() || laid.gfx_laid.empty())
		check(false, "the stubs laid down by the port (no dump ran)");
	else
	{
		run_harness(folder, "walls", blob(laid.walls_laid, laid.walls_base, stubs::walls::relocs, std::size(stubs::walls::relocs), stubs::walls::symbols,
			std::size(stubs::walls::symbols)));
		run_harness(folder, "gfx", blob(laid.gfx_laid, laid.gfx_base, stubs::gfx::relocs, std::size(stubs::gfx::relocs), stubs::gfx::symbols,
			std::size(stubs::gfx::symbols)));
	}

	std::printf(failures == 0 ? "\nall passed (%d)\n" : "\n%d failed\n", failures == 0 ? passes : failures);
	return failures;
}
