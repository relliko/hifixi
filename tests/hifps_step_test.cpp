/*
 * Offline test of the frame step (src/game/hifps/step.cpp and sites.cpp: the hifps addon's hifps.lua 0.9.9 frame step
 * and its sites.lua / sigs.lua resolver, in C++) against both FFXiMain dumps, each mapped at its own base
 * (game_dump.hpp), our memory from 0x30000000, the game object (the fps divisor at +30h, the frame count at +34h) faked
 * at 0x2F000000. The addon itself runs under lupa against the same dump, set up the same way (tests/hifps_golden.py),
 * and is diffed with:
 *   - its signatures (sigs.lua) and what its resolver finds (sites.lua): every offset, the calls no table names;
 *   - what it patches, byte for byte: the two getters, every call site and its kind, the smoothing loops' immediates
 *     and operands, the name pulse's call, and our memory (the steps, the 1.0 stub, the slots, the pulse's count and
 *     its getter);
 *   - what each frame writes, over frames of all lengths (the step clamped both ways), and the frame rate measured;
 *   - the image after stop / unload: as before;
 *   - a scenario with things changed in the mapped copy: call sites hooked by something else, two an earlier hifps
 *     left (a call to memory that's gone, a call to a 'fld [x]; ret' stub), the camera follow's immediate and two
 *     operands changed by another addon (one to a value that's the same at any frame rate), the name pulse's code not
 *     there, a site's signature broken (its call on whole ticks as an extra);
 *   - refusals: nine sites hooked; a getter not there.
 * Then what only HiFi XI does: the options followed, the divisor's object not there yet (tried again), an operand
 * redirected and handed back while on, an immediate written over while on, the name pulse's call hooked by something
 * else (left alone), stop() over a site changed since (left, our memory kept), and the limiter on the real clock.
 * Each dump's sites are checked to be the game's own first; at the end the image is the dump again, byte for byte.
 *
 *   build\hifps_step_test.exe     (build.cmd; python with lupa on the PATH)
 */

#include "../src/game/hifps/step.hpp"
#include "../src/game/hifps/sites.hpp"
#include "game_dump.hpp"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
	namespace hifps = hifi::game::hifps;
	namespace sites = hifi::game::hifps::sites;

	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (ok)
			passes++;
		else
			failures++;
	}

	std::map<std::string, std::string> g_options;
	void set_option(const char *key, const char *value)
	{
		g_options[std::string("hifps.") + key] = value;
	}

	constexpr uintptr_t kFake = 0x2F000000;   // the game object (and other memory the scenarios point at)
	constexpr uintptr_t kGone = 0x2E000000;   // memory that isn't there
	constexpr uint32_t kFrameCount = 0x00012345;
	constexpr const char *kGetter = "8B0D????????D94128D81D????????DFE0F6C4057A07D905????????C3D94128C3";
	constexpr const char *kDivisor = "81EC000100003BC174218B0D";
	constexpr const char *kPulse = "8A4E038BE9E8????????C1E00499B968010000";
	constexpr const char *kFrameGetter = "A1????????8B4034C3";
	constexpr uint32_t kMemSize = 144;

	// The smoothing loops' immediates in the analysed client and their k (hifps.lua's SMOOTH_LOOPS)
	const std::map<uint32_t, float> kImmediates = { { 0x01F5D4, 0.25f }, { 0x01F6C7, 0.05f }, { 0x0C684B, 0.125f }, { 0x0C6C2D, 0.125f } };

	std::string to_hex(const void *p, size_t n)
	{
		std::string out;
		char two[4];
		for (size_t i = 0; i < n; ++i)
		{
			std::snprintf(two, sizeof(two), "%02X", unsigned(static_cast<const uint8_t *>(p)[i]));
			out += two;
		}
		return out;
	}

	std::string from_hex(const std::string &hex)
	{
		std::string out;
		for (size_t i = 0; i + 1 < hex.size(); i += 2)
			out += char(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
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

	std::string fmt(const char *f, ...)
	{
		char line[1024];
		va_list a;
		va_start(a, f);
		std::vsnprintf(line, sizeof(line), f, a);
		va_end(a);
		return line;
	}

	bool memory_free(uintptr_t at)
	{
		MEMORY_BASIC_INFORMATION mbi = {};
		return VirtualQuery(reinterpret_cast<void *>(at), &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_FREE;
	}

	bool contains(const std::string &s, const std::string &part)
	{
		return s.find(part) != std::string::npos;
	}

	bool any_contains(const std::vector<std::string> &lines, const std::string &part)
	{
		for (const std::string &l : lines)
			if (contains(l, part))
				return true;
		return false;
	}

	std::string joined(const std::vector<std::string> &lines)
	{
		std::string s;
		for (const std::string &l : lines)
			s += "\n      " + l;
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

	// Bytes put into the mapped copy (at an offset from its base; the Lua's copy gets the same)
	struct change
	{
		uint32_t rva;
		std::string bytes;
	};

	// What hifps.lua did with the dump (tests/hifps_golden.py's lines)
	struct golden
	{
		bool ran = false, resolved = false, unloaded = false;
		std::vector<std::string> sigs, sites, ops, patches, frames, chat, unload;
		uint32_t callers = 0;
		std::map<uint32_t, uint32_t> map;
		std::vector<uint32_t> extra, getters, imms, freed;
		bool on = false;
		uint32_t pulse = 0, div_ptr = 0, div_orig = 0;
		std::string mem, fps;
	};

	std::string g_root;

	golden run_lua(const std::string &dump, const std::vector<change> &writes, double t0, const std::vector<double> &frames)
	{
		golden g;
		std::string command = "\"python \"" + g_root + "tests\\hifps_golden.py\" \"" + dump + "\" --t0 " + fmt("%.17g", t0);
		if (!frames.empty())
		{
			command += " --frames ";
			for (size_t i = 0; i < frames.size(); ++i)
				command += fmt(i == 0 ? "%.17g" : ",%.17g", frames[i]);
		}
		for (const change &w : writes)
			command += fmt(" --write %X:", w.rva) + to_hex(w.bytes.data(), w.bytes.size());
		command += "\"";
		if (GetEnvironmentVariableA("HIFPS_GOLDEN_SHOW", nullptr, 0) != 0) // (the command shown, to run it by hand)
			std::printf("(%s)\n", command.c_str());
		FILE *p = _popen(command.c_str(), "r");
		if (p == nullptr)
			return g;
		char line[4096];
		while (fgets(line, sizeof(line), p) != nullptr)
		{
			std::string s(line);
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
				s.pop_back();
			const size_t sp = s.find(' ');
			const std::string what = s.substr(0, sp), rest = sp == std::string::npos ? std::string() : s.substr(sp + 1);
			if (what == "sig")
				g.sigs.push_back(rest);
			else if (what == "callers")
			{
				g.resolved = true;
				g.callers = uint32_t(std::strtoul(rest.c_str(), nullptr, 10));
			}
			else if (what == "map")
				g.map[uint32_t(std::strtoul(rest.c_str(), nullptr, 16))] = uint32_t(std::strtoul(rest.substr(rest.find(' ') + 1).c_str(), nullptr, 16));
			else if (what == "extra")
				g.extra.push_back(uint32_t(std::strtoul(rest.c_str(), nullptr, 16)));
			else if (what == "on")
				g.on = rest == "1";
			else if (what == "getter")
				g.getters.push_back(uint32_t(std::strtoul(rest.c_str(), nullptr, 16)));
			else if (what == "site")
				g.sites.push_back(rest);
			else if (what == "imm")
				g.imms.push_back(uint32_t(std::strtoul(rest.c_str(), nullptr, 16)));
			else if (what == "op")
				g.ops.push_back(rest);
			else if (what == "pulse")
				g.pulse = uint32_t(std::strtoul(rest.c_str(), nullptr, 16));
			else if (what == "divisor")
			{
				g.div_ptr = uint32_t(std::strtoul(rest.c_str(), nullptr, 16));
				g.div_orig = uint32_t(std::strtoul(rest.substr(rest.find(' ') + 1).c_str(), nullptr, 16));
			}
			else if (what == "patch")
				g.patches.push_back(rest);
			else if (what == "mem")
				g.mem = rest;
			else if (what == "frame")
				g.frames.push_back(rest);
			else if (what == "fps")
				g.fps = rest;
			else if (what == "unload")
				g.unloaded = true;
			else if (what == "unloaded")
				g.unload.push_back(rest);
			else if (what == "dealloc")
				g.freed.push_back(uint32_t(std::strtoul(rest.c_str(), nullptr, 16)));
			else if (what == "chat")
				g.chat.push_back(rest);
		}
		g.ran = _pclose(p) == 0 && !g.sigs.empty();
		return g;
	}

	// The image's changed byte runs against `before`, as the golden lists them
	std::vector<std::string> diff_runs(uintptr_t base, const std::string &before)
	{
		std::vector<std::string> out;
		const auto now = reinterpret_cast<const uint8_t *>(base);
		size_t i = 0;
		while (i < before.size())
		{
			if (now[i] != uint8_t(before[i]))
			{
				size_t j = i;
				while (j < before.size() && now[j] != uint8_t(before[j]))
					++j;
				out.push_back(fmt("%08X ", unsigned(base + i)) + to_hex(now + i, j - i));
				i = j;
			}
			else
				++i;
		}
		return out;
	}

	// Lists compared: equal, or the first difference
	template <typename T, typename F>
	std::string first_difference(const std::vector<T> &mine, const std::vector<T> &lua, F show)
	{
		if (mine == lua)
			return std::string();
		for (size_t i = 0; i < mine.size() || i < lua.size(); ++i)
		{
			const std::string a = i < mine.size() ? show(mine[i]) : std::string("(none)");
			const std::string b = i < lua.size() ? show(lua[i]) : std::string("(none)");
			if (a != b)
				return fmt(" (#%u: %s here, %s in the Lua; %u here, %u there)", unsigned(i), a.c_str(), b.c_str(), unsigned(mine.size()), unsigned(lua.size()));
		}
		return " (differ)";
	}
	std::string same_text(const std::string &s) { return s; }
	std::string word_text(uint32_t v) { return fmt("%08X", v); }

	// The frames' times: t0, then every length of frame there is (a 1200 fps one clamped to 0.05 ticks, a 0.3 s hitch
	// to 4), three times over (past a second: the frame rate measured once)
	std::vector<double> frame_times(double t0)
	{
		const double dts[] = { 1.0 / 128, 1.0 / 90, 1.0 / 250, 1.0 / 60, 1.0 / 30, 0.3, 1.0 / 2000, 1.0 / 144, 1.0 / 75, 1.0 / 240, 1.0 / 57,
			1.0 / 61, 0.1, 1.0 / 165 };
		std::vector<double> out;
		double t = t0;
		for (int round = 0; round < 3; ++round)
			for (const double dt : dts)
			{
				t += dt;
				out.push_back(t);
			}
		return out;
	}

	// The 4 bytes of a float
	std::string float_bytes(float f)
	{
		return std::string(reinterpret_cast<const char *>(&f), 4);
	}

	// One dump: everything
	struct dump_state
	{
		std::string path, file;
		uintptr_t base = 0;
		size_t size = 0;
		uintptr_t global = 0;        // the game object's global
		uint32_t global_dumped = 0;
		sites::found found;          // (the resolver on the dump as it is)
		uintptr_t g1 = 0, g2 = 0;
	};

	uintptr_t at_of(const dump_state &d, uint32_t old)
	{
		const auto m = d.found.map.find(old);
		return m != d.found.map.end() ? d.base + m->second : 0;
	}

	uint32_t rva_of(const dump_state &d, uint32_t old)
	{
		const auto m = d.found.map.find(old);
		return m != d.found.map.end() ? m->second : 0;
	}

	// A call from the site at `rva` to `target`
	change call_write(const dump_state &d, uint32_t rva, uintptr_t target)
	{
		return { rva, "\xE8" + word_bytes(uint32_t(target - (d.base + rva + 5))) };
	}

	// Each of the dump's sites the game's own: the getters, every call site named calling one, the immediates their k
	// behind their push / mov, the operands an fmul/fsub ... (D8) of a constant in the image, the name pulse's call to the
	// frame count getter; what isn't, said
	std::vector<std::string> not_stock(const dump_state &d)
	{
		std::vector<std::string> out;
		const std::vector<uintptr_t> getters = sites::find_all(d.base, d.size, kGetter, 4);
		if (getters.size() != 2)
			out.push_back(fmt("%u getters", unsigned(getters.size())));
		size_t count = 0;
		const sites::signature *sigs = sites::signatures(count);
		for (size_t i = 0; i < count; ++i)
		{
			const uintptr_t a = at_of(d, sigs[i].offset);
			if (a == 0)
			{
				out.push_back(fmt("%06X not found", sigs[i].offset));
				continue;
			}
			if (sigs[i].what == sites::kind::site)
			{
				const uintptr_t t = hifi::game::rel_target(a);
				if (*reinterpret_cast<const uint8_t *>(a) != 0xE8 || (t != d.g1 && t != d.g2))
					out.push_back(fmt("site %06X calls %08X", sigs[i].offset, unsigned(t)));
			}
			else if (sigs[i].what == sites::kind::imm)
			{
				const float k = kImmediates.at(sigs[i].offset);
				const uint8_t pre = *reinterpret_cast<const uint8_t *>(a - (k == 0.125f ? 4 : 1));
				if (bytes_at(a, 4) != float_bytes(k) || pre != (k == 0.125f ? 0xC7 : 0x68))
					out.push_back(fmt("immediate %06X is %s", sigs[i].offset, to_hex(reinterpret_cast<const void *>(a - 1), 5).c_str()));
			}
			else
			{
				const uint32_t c = word_at(a);
				if (*reinterpret_cast<const uint8_t *>(a - 2) != 0xD8 || c < d.base || c >= d.base + d.size)
					out.push_back(fmt("operand %06X is %s", sigs[i].offset, to_hex(reinterpret_cast<const void *>(a - 2), 6).c_str()));
			}
		}
		const uintptr_t pulse = hifi::game::find(kPulse, 5);
		if (pulse == 0)
			out.push_back("the name pulse's code not there");
		else
		{
			const uintptr_t t = hifi::game::rel_target(pulse);
			if (t < d.base || t >= d.base + d.size || !hifi::game::code_is(t, "\xA1", 1) ||
				word_at(t + 5) != 0xC334408B)
				out.push_back(fmt("the name pulse calls %08X", unsigned(t)));
		}
		if (hifi::game::find(kDivisor) == 0)
			out.push_back("the divisor's code not there");
		return out;
	}

	// The golden run and the port compared, on the mapped copy as it is now (with `writes` already in it)
	void compare_run(dump_state &d, const char *name, const std::vector<change> &writes, bool frames_too)
	{
		std::printf("\n-- %s: %s\n", d.file.c_str(), name);
		const double t0 = 1000.0;
		const std::vector<double> times = frames_too ? frame_times(t0) : std::vector<double>();
		const golden lua = run_lua(d.path, writes, t0, times);
		check(lua.ran, "hifps.lua ran under lupa on the same dump");
		if (!lua.ran)
			return;

		// Its signatures and resolver
		size_t count = 0;
		const sites::signature *sigs = sites::signatures(count);
		std::vector<std::string> mine_sigs;
		for (size_t i = 0; i < count; ++i)
			mine_sigs.push_back(fmt("%06X %s %u %s", sigs[i].offset, sigs[i].what == sites::kind::site ? "site" : sigs[i].what == sites::kind::imm ? "imm" : "op",
				sigs[i].at, sigs[i].pattern));
		check(mine_sigs == lua.sigs, fmt("the %u signatures are sigs.lua's", unsigned(count)) + first_difference(mine_sigs, lua.sigs, same_text));
		if (!lua.resolved)
			check(sites::find_all(d.base, d.size, kGetter, 4).size() != 2, "  sites.lua not run: the getters aren't both there");
		const sites::found f = sites::resolve(reinterpret_cast<const uint8_t *>(d.base), d.size, uint32_t(d.g1 - d.base), uint32_t(d.g2 - d.base));
		uint32_t moved = 0;
		for (const auto &m : f.map)
			moved += m.first != m.second ? 1 : 0;
		std::vector<std::string> mine_map, lua_map;
		for (const auto &m : f.map)
			mine_map.push_back(fmt("%06X %06X", m.first, m.second));
		for (const auto &m : lua.map)
			lua_map.push_back(fmt("%06X %06X", m.first, m.second));
		if (lua.resolved)
			check(f.callers == lua.callers && mine_map == lua_map && f.extra == lua.extra,
			fmt("the resolver as sites.lua's: %u calls to the getters, %u of %u offsets found (%u moved), %u extra", f.callers, unsigned(f.map.size()),
				unsigned(count), moved, unsigned(f.extra.size())) + first_difference(mine_map, lua_map, same_text) + first_difference(f.extra, lua.extra, word_text));

		// Turned on: the time it counts from, then the options followed
		hifi::game::set_alloc_at(dump::kAllocAt);
		set_option("enabled", "1");
		set_option("limit", "0");
		const std::string before = bytes_at(d.base, d.size);
		hifps::present(t0);
		hifps::update();
		check(hifps::on() == lua.on, std::string("on: ") + (hifps::on() ? "yes" : "no") + (lua.on ? ", as the Lua" : ", as the Lua (not)") + joined(hifps::status()));
		const std::vector<std::string> status = hifps::status();
		// What the Lua said, said here too (its loop warnings, sites hooked, sites taken back, an updated client)
		for (const std::string &c : lua.chat)
		{
			std::string key;
			if (contains(c, "Warning: the ") && contains(c, "was changed by another addon"))
				key = c.substr(c.find("Warning: the ") + 13, c.find(") in a way") - c.find("Warning: the ") - 12);
			else if (contains(c, "Warning: the ") && contains(c, "was not found in this client"))
				key = c.substr(c.find("Warning: the ") + 13, c.find(") was not found") - c.find("Warning: the ") - 12);
			else if (contains(c, "is hooked by something else"))
				key = c.substr(c.find("Call site "), 18);
			else if (contains(c, "Updated client:") || contains(c, "Took back"))
				key = c.substr(c.find(contains(c, "Took back") ? "Took back" : "Updated client:"));
			else if (contains(c, "call sites do not call the step getter"))
				key = c.substr(c.find("ERROR: ") + 7, c.find(" call sites") - c.find("ERROR: ") - 7) + " call sites don't call the step getter";
			else if (contains(c, "Expected 2 frame step getters"))
				key = "expected 2 frame step getters, found " + c.substr(c.find("found ") + 6, 1);
			if (!key.empty())
				check(any_contains(status, key), "  said here too: " + key);
		}
		if (!lua.on)
		{
			check(!hifps::on() && hifps::where().memory == 0 && memory_free(dump::kAllocAt), "  nothing on, no memory taken");
			check(bytes_at(d.base, d.size) == before, "  nothing written: the image as it was");
			hifps::stop();
			return;
		}
		const hifps::patched p = hifps::where();
		check(p.memory == dump::kAllocAt && lua.mem.compare(0, 8, "30000000") == 0, fmt("  our memory at %08X, as the Lua's", unsigned(p.memory)));
		check(p.getters[0] == lua.getters.at(0) && p.getters[1] == lua.getters.at(1),
			fmt("  the getters at %08X (fractional) and %08X (whole ticks)", unsigned(p.getters[0]), unsigned(p.getters[1])));
		std::vector<std::string> mine_sites;
		uint32_t kinds[3] = {};
		for (const auto &s : p.sites)
		{
			mine_sites.push_back(fmt("%08X %c %08X", unsigned(s.at), s.kind, unsigned(s.target)));
			kinds[s.kind == 'f' ? 0 : s.kind == 'i' ? 1 : 2]++;
		}
		check(mine_sites == lua.sites, fmt("  the %u call sites, their kinds and targets, in order (%u fractional, %u whole ticks, %u on the stub)",
			unsigned(mine_sites.size()), kinds[0], kinds[1], kinds[2]) + first_difference(mine_sites, lua.sites, same_text));
		std::vector<uint32_t> mine_imms;
		for (const uintptr_t a : p.immediates)
			mine_imms.push_back(uint32_t(a));
		check(mine_imms == lua.imms, fmt("  the %u smoothing immediates", unsigned(mine_imms.size())) + first_difference(mine_imms, lua.imms, word_text));
		std::vector<std::string> mine_ops;
		for (const auto &o : p.operands)
			mine_ops.push_back(fmt("%08X %08X %c %08X", unsigned(o.at), unsigned(o.constant), o.mode, unsigned(o.slot)));
		check(mine_ops == lua.ops, fmt("  the %u operands, their constants, modes and slots", unsigned(mine_ops.size())) + first_difference(mine_ops, lua.ops, same_text));
		check(p.pulse == lua.pulse, fmt("  the name pulse's call: %08X", unsigned(p.pulse)));
		check(p.divisor == lua.div_ptr && p.divisor_was == lua.div_orig && word_at(kFake + 0x30) == 0,
			fmt("  the divisor (%08X, was %u) set to 0", unsigned(p.divisor), p.divisor_was));
		const std::vector<std::string> runs = diff_runs(d.base, before);
		size_t bytes = 0;
		for (const std::string &r : runs)
			bytes += (r.size() - 9) / 2;
		check(runs == lua.patches, fmt("  the image patched byte for byte as the Lua patched it (%u runs, %u bytes)", unsigned(runs.size()), unsigned(bytes)) +
			first_difference(runs, lua.patches, same_text));
		const std::string mem = fmt("%08X ", unsigned(p.memory)) + to_hex(reinterpret_cast<const void *>(p.memory), kMemSize);
		check(mem == lua.mem, "  our memory byte for byte the Lua's (the steps, the 1.0 stub, the operand slots, the pulse's count and getter)" +
			(mem == lua.mem ? std::string() : "\n      here " + mem + "\n      Lua  " + lua.mem));

		// The frames
		if (frames_too)
		{
			std::vector<std::string> mine_frames;
			double lo = 1e9, hi = 0.0;
			for (size_t i = 0; i < times.size(); ++i)
			{
				hifps::present(times[i]);
				mine_frames.push_back(fmt("%u mem ", unsigned(i)) + to_hex(reinterpret_cast<const void *>(p.memory), kMemSize));
				for (const uintptr_t a : p.immediates)
					mine_frames.push_back(fmt("%u imm %08X ", unsigned(i), unsigned(a)) + to_hex(reinterpret_cast<const void *>(a), 4));
				lo = std::min(lo, hifps::where().step);
				hi = std::max(hi, hifps::where().step);
			}
			check(mine_frames == lua.frames, fmt("  %u frames (steps %.2f to %.2f ticks): every frame's writes byte for byte the Lua's", unsigned(times.size()), lo, hi) +
				first_difference(mine_frames, lua.frames, same_text));
			check(fmt("%.17g", hifps::fps()) == lua.fps, fmt("  the frame rate measured as the Lua measured it: %.4f", hifps::fps()));
		}

		hifps::stop();
		const std::vector<std::string> left = diff_runs(d.base, before);
		check(lua.unloaded && left == lua.unload, (left.empty() ? std::string("  after stop: the image as before, as after the Lua's unload") :
			fmt("  after stop: the image as the Lua's unload left it (%u runs differ from before: sites an earlier hifps left, pointed at the getter)",
				unsigned(left.size()))) + first_difference(left, lua.unload, same_text));
		const bool freed = !lua.freed.empty();
		check(memory_free(dump::kAllocAt) == freed && !hifps::on() && hifps::where().memory == 0,
			freed ? "  our memory freed, everything forgotten" : "  our memory kept, as the Lua kept its (another addon holds an operand), everything forgotten");
		check(word_at(kFake + 0x30) == 1, "  the divisor put back");
		// (back as it was for the next run)
		if (!freed)
			VirtualFree(reinterpret_cast<void *>(dump::kAllocAt), 0, MEM_RELEASE);
		for (const std::string &r : left)
		{
			const uintptr_t at = std::strtoul(r.c_str(), nullptr, 16);
			const size_t n = (r.size() - 9) / 2;
			std::memcpy(reinterpret_cast<void *>(at), before.data() + (at - d.base), n);
		}
	}

	void test_dump(const std::string &path)
	{
		dump_state d;
		d.path = path;
		d.file = path.substr(path.find_last_of('\\') + 1);
		std::printf("\n%s\n", d.file.c_str());
		const int passes_before = passes, failures_before = failures;
		d.size = dump::map(path, d.base);
		check(d.size != 0, fmt("mapped at its base, 0x%08X", unsigned(d.base)));
		if (d.size == 0)
			return;

		// The game object faked behind its global
		const uintptr_t dp = hifi::game::find(kDivisor);
		d.global = dp != 0 ? word_at(dp + 0x0C) : 0;
		check(d.global >= d.base && d.global < d.base + d.size, fmt("the game object's global at %08X (the divisor's code at %08X)", unsigned(d.global), unsigned(dp)));
		if (d.global == 0)
		{
			dump::unmap(d.base);
			return;
		}
		d.global_dumped = word_at(d.global);
		put_word(d.global, uint32_t(kFake));
		put_word(kFake + 0x30, 1);
		put_word(kFake + 0x34, kFrameCount);

		// The image found from inside it, the getters, the resolver on the dump as it is
		uintptr_t base = 0;
		size_t size = 0;
		const std::vector<uintptr_t> getters = sites::find_all(d.base, d.size, kGetter, 4);
		const bool image = getters.size() == 2 && sites::image_of(getters[0], base, size);
		check(image && base == d.base && size == d.size, fmt("the image found from a getter inside it: %08X, %X bytes", unsigned(base), unsigned(size)));
		if (getters.size() != 2)
		{
			dump::unmap(d.base);
			return;
		}
		d.g1 = getters[0];
		d.g2 = getters[1];
		d.found = sites::resolve(reinterpret_cast<const uint8_t *>(d.base), d.size, uint32_t(d.g1 - d.base), uint32_t(d.g2 - d.base));

		// Each site the game's own (the dumps were made with addons' patches in)
		const std::vector<std::string> changed = not_stock(d);
		check(changed.empty(), changed.empty() ? "every site as the game has it: 2 getters, each call site calling one, the immediates and operands, "
			"the name pulse's call, the divisor's code" : "sites not the game's own:" + joined(changed));

		// The addon and the port, on the dump as it is
		compare_run(d, "as dumped", {}, true);

		// Things changed: sites hooked by something else (to the frame count getter, in the image), two an earlier
		// hifps left (a call to memory that's gone; one to a 'fld [x]; ret' stub), the camera follow's immediate made
		// 1.0, an operand pointed at our 1.0 (the same at any frame rate) and another at 0.5 (not), the name pulse's
		// code not there (its mov ecx, 360 made 361), the first fractional site's signature broken (81 C7 -> 80 C7)
		const uintptr_t frame_getter = d.base + 0x14D50;
		const uintptr_t pulse = hifi::game::find(kPulse);
		std::vector<change> w = {
			call_write(d, rva_of(d, 0x01A9AB), frame_getter), call_write(d, rva_of(d, 0x01B5D4), frame_getter),
			call_write(d, rva_of(d, 0x0C4878), frame_getter),
			call_write(d, rva_of(d, 0x0205B1), kGone),
			{ uint32_t(kFake + 0x100 - d.base), from_hex("D9050801002FC3") },
			call_write(d, rva_of(d, 0x005A50), kFake + 0x100),
			{ rva_of(d, 0x01F5D4), float_bytes(1.0f) },
			{ uint32_t(kFake + 0x200 - d.base), float_bytes(1.0f) + float_bytes(0.5f) },
			{ rva_of(d, 0x01FE38), word_bytes(uint32_t(kFake + 0x200)) },
			{ rva_of(d, 0x0C8B38), word_bytes(uint32_t(kFake + 0x204)) },
			{ uint32_t(pulse - d.base + 15), "\x69" },
			{ rva_of(d, 0x00766F) - 6, "\x80" },
		};
		check(hifi::game::code_is(frame_getter, "\xA1", 1) && pulse != 0 && rva_of(d, 0x00766F) != 0 &&
				*reinterpret_cast<const uint8_t *>(d.base + rva_of(d, 0x00766F) - 6) == 0x81 && memory_free(kGone),
			"the changes' places: the frame count getter at 014D50, the pulse's code, 81 C7 before the first site, nothing at 2E000000");
		std::vector<std::string> saved;
		for (const change &x : w)
		{
			saved.push_back(bytes_at(d.base + x.rva, x.bytes.size()));
			std::memcpy(reinterpret_cast<void *>(d.base + x.rva), x.bytes.data(), x.bytes.size());
		}
		compare_run(d, "things changed (3 sites hooked, 2 left by an earlier hifps, a loop's immediate and 2 operands, the pulse's code, a signature)", w, true);
		for (size_t i = w.size(); i-- > 0;)
			std::memcpy(reinterpret_cast<void *>(d.base + w[i].rva), saved[i].data(), saved[i].size());

		// Refused: nine sites hooked by something else
		const uint32_t hooked[] = { 0x01A9AB, 0x01B5D4, 0x01C5AF, 0x01F01F, 0x01F0F2, 0x01F82F, 0x01F87F, 0x01F8D5, 0x01F91D };
		w.clear();
		for (const uint32_t old : hooked)
			w.push_back(call_write(d, rva_of(d, old), frame_getter));
		saved.clear();
		for (const change &x : w)
		{
			saved.push_back(bytes_at(d.base + x.rva, x.bytes.size()));
			std::memcpy(reinterpret_cast<void *>(d.base + x.rva), x.bytes.data(), x.bytes.size());
		}
		compare_run(d, "refused: 9 sites hooked by something else", w, false);
		for (size_t i = w.size(); i-- > 0;)
			std::memcpy(reinterpret_cast<void *>(d.base + w[i].rva), saved[i].data(), saved[i].size());

		// Refused: the second getter not there (its fld [ecx+28] made fld [ecx+29])
		w = { { uint32_t(d.g2 - d.base + 8), "\x29" } };
		saved = { bytes_at(d.g2 + 8, 1) };
		std::memcpy(reinterpret_cast<void *>(d.g2 + 8), w[0].bytes.data(), 1);
		compare_run(d, "refused: one getter not there", w, false);
		std::memcpy(reinterpret_cast<void *>(d.g2 + 8), saved[0].data(), 1);

		// What only HiFi XI does, on the dump as it is
		std::printf("\n-- %s: HiFi XI's own\n", d.file.c_str());
		hifi::game::set_alloc_at(dump::kAllocAt);
		const std::string clean = bytes_at(d.base, d.size);
		set_option("enabled", "0");
		set_option("limit", "0");
		hifps::update();
		check(!hifps::on() && hifps::status().at(0) == "Off: the game's own step (60 fps at most)." && bytes_at(d.base, d.size) == clean,
			"enabled 0: off, nothing written (" + hifps::status().at(0) + ")");
		// The divisor's object not there yet: said, tried again
		put_word(d.global, 0);
		const std::string no_object = bytes_at(d.base, d.size);
		set_option("enabled", "1");
		hifps::update();
		check(!hifps::on() && hifps::status().at(0) == "Not on: the game's fps divisor isn't there yet." && bytes_at(d.base, d.size) == no_object &&
			memory_free(dump::kAllocAt), "the divisor's object not there yet: " + hifps::status().at(0) + " (nothing written, no memory)");
		put_word(d.global, uint32_t(kFake));
		const std::string before_on = bytes_at(d.base, d.size);
		hifps::update();
		check(hifps::on() && hifps::where().memory == dump::kAllocAt, "  there: on at the next update");
		hifps::present(2000.0);
		hifps::present(2000.0 + 1.0 / 120);
		check(std::fabs(hifps::where().step - 0.5) < 1e-9 && word_at(dump::kAllocAt) == 0x3F000000, "  a 1/120 s frame: step 0.5");
		set_option("limit", "200");
		hifps::update();
		check(contains(hifps::status().at(0), "cap 200."), "limit 200 followed: " + hifps::status().at(0));
		set_option("limit", "0");
		hifps::update();
		set_option("enabled", "0");
		hifps::update();
		check(!hifps::on() && bytes_at(d.base, d.size) == before_on && memory_free(dump::kAllocAt) && word_at(kFake + 0x30) == 1,
			"enabled 0 again: everything put back, the divisor too, our memory freed");
		set_option("enabled", "1");
		hifi::game::set_alloc_at(dump::kAllocAt);
		const sites::found *r1 = hifps::resolved();
		const size_t n_sites = hifps::where().sites.size();
		hifps::update();
		check(hifps::on() && hifps::resolved() == r1 && r1 != nullptr && hifps::where().sites.size() == r1->callers && n_sites == 0,
			fmt("enabled 1 again: on, the sites as resolved the first time (%u)", unsigned(hifps::where().sites.size())));
		if (!hifps::on() || hifps::where().operands.empty() || hifps::where().immediates.empty() || hifps::where().sites.empty())
		{
			hifps::stop();
			dump::unmap(d.base);
			return;
		}

		// An operand redirected while on, then handed back
		hifps::patched p = hifps::where();
		const hifps::patched::operand o0 = p.operands.at(0);
		put_word(kFake + 0x204, 0x3F000000);
		put_word(o0.at, uint32_t(kFake + 0x204));
		hifps::present(2001.0);
		hifps::present(2001.0 + 1.0 / 144);
		std::vector<std::string> st = hifps::status();
		check(any_contains(st, fmt("camera distance/collision code at %08X was changed by another addon (value 0.5)", unsigned(o0.at - 2))) &&
			word_at(o0.at) == kFake + 0x204, "an operand pointed at another addon's 0.5 while on: left to it, said" + joined(st));
		put_word(o0.at, uint32_t(o0.constant));
		hifps::present(2001.0 + 2.0 / 144);
		check(word_at(o0.at) == o0.slot && !any_contains(hifps::status(), "value 0.5"), "  handed back: ours again, nothing said");
		// An immediate written over while on
		const uintptr_t imm = p.immediates.at(0);
		put_word(imm, 0x3F800000);
		hifps::present(2001.0 + 3.0 / 144);
		check(word_at(imm) == 0x3F800000 && any_contains(hifps::status(), "its factor written over: found 00 00 80 3F"),
			"an immediate written over while on: left to it, said");
		// stop() with a site changed since: left as it is, our memory kept; the rest put back
		const auto site0 = p.sites.at(0);
		put_word(site0.at + 1, 0x11111111);
		hifps::stop();
		check(word_at(site0.at + 1) == 0x11111111 && word_at(imm) == 0x3F800000 && !memory_free(dump::kAllocAt),
			"stop() with a site and an immediate changed since: both left as they are, our memory kept");
		bool slots = true;
		for (const auto &o : p.operands)
			slots = slots && word_at(o.slot) == word_at(o.constant);
		check(word_at(dump::kAllocAt) == 0x3F800000 && word_at(dump::kAllocAt + 4) == 0x3F800000 && slots,
			"  its steps 1.0 and the operand slots the game's values (anything still reading them runs at stock)");
		std::memcpy(reinterpret_cast<void *>(site0.at), clean.data() + (site0.at - d.base), 5);
		std::memcpy(reinterpret_cast<void *>(imm), clean.data() + (imm - d.base), 4);
		check(bytes_at(d.base, d.size) == clean, "  everything else put back (the image as it was, those two bytes put back here)");
		VirtualFree(reinterpret_cast<void *>(dump::kAllocAt), 0, MEM_RELEASE);

		// The name pulse's call hooked by something else: left as it is (the Lua took it)
		const uintptr_t pulse_call = pulse + 5;
		const std::string pulse_own = bytes_at(pulse_call, 5);
		std::memcpy(reinterpret_cast<void *>(pulse_call), call_write(d, uint32_t(pulse_call - d.base), kFake + 0x300).bytes.data(), 5);
		const std::string hooked_pulse = bytes_at(pulse_call, 5);
		hifi::game::set_alloc_at(dump::kAllocAt);
		hifps::update();
		check(hifps::on() && hifps::where().pulse == 0 && bytes_at(pulse_call, 5) == hooked_pulse &&
				any_contains(hifps::status(), fmt("name pulse's call at %08X goes somewhere else", unsigned(pulse_call))),
			"the name pulse's call hooked by something else: left as it is, said");
		hifps::stop();
		std::memcpy(reinterpret_cast<void *>(pulse_call), pulse_own.data(), 5);
		check(bytes_at(d.base, d.size) == clean && memory_free(dump::kAllocAt), "  stop: the image as it was");

		// The name pulse counts whole ticks from the game's own count
		hifi::game::set_alloc_at(dump::kAllocAt);
		hifps::update();
		p = hifps::where();
		const uint32_t start = word_at(p.memory + 128);
		hifps::present(3000.0);
		const uint32_t first = word_at(p.memory + 128);
		for (int i = 1; i <= 256; ++i)
			hifps::present(3000.0 + i / 256.0);
		const uint32_t ticks = word_at(p.memory + 128) - first;
		check(start == kFrameCount && first == kFrameCount + 1 && ticks == 60 && p.pulse != 0 && hifi::game::rel_target(p.pulse) == p.memory + 132,
			fmt("the name pulse: from the game's count (%X), 60 ticks over a second at 256 fps (%u), its call to our getter", start, ticks));
		hifps::stop();

		put_word(d.global, d.global_dumped);
		check(dump::same_as_file(path, d.base, d.size), "the image the dump again, byte for byte (the game object's global put back)");
		dump::unmap(d.base);
		std::printf("(%s: %d ok, %d failed)\n", d.file.c_str(), passes - passes_before, failures - failures_before);
	}

	// The limiter on the real clock: frames as fast as the loop goes, capped
	void test_limiter()
	{
		std::printf("\n-- the limiter, on the real clock\n");
		uintptr_t base = 0;
		const size_t size = dump::map(dump::kDumps[0], base);
		if (size == 0)
		{
			check(false, "the first dump mapped");
			return;
		}
		const uintptr_t dp = hifi::game::find(kDivisor);
		const uintptr_t global = word_at(dp + 0x0C);
		const uint32_t dumped = word_at(global);
		put_word(global, uint32_t(kFake));
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		const auto now = [&]() -> double
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return double(t.QuadPart) / double(f.QuadPart);
		};
		for (const int cap : { 200, 60 })
		{
			hifi::game::set_alloc_at(dump::kAllocAt);
			set_option("enabled", "1");
			set_option("limit", std::to_string(cap).c_str());
			hifps::present(now());
			hifps::update();
			const int frames = cap == 200 ? 400 : 120;
			std::vector<double> ends;
			double timer = 0.0, spin = 0.0, spin_max = 0.0;
			for (int i = 0; i < frames; ++i)
			{
				hifps::present(now());
				ends.push_back(now());
				const hifps::patched p = hifps::where();
				timer += p.timer;
				spin += p.spin;
				if (i > 0)
					spin_max = std::max(spin_max, p.spin);
			}
			double lo = 1e9, hi = 0.0;
			for (size_t i = 1; i < ends.size(); ++i)
			{
				lo = std::min(lo, ends[i] - ends[i - 1]);
				hi = std::max(hi, ends[i] - ends[i - 1]);
			}
			const double mean = (ends.back() - ends.front()) / double(ends.size() - 1), want = 1.0 / cap;
			const bool high = hifps::where().high_resolution;
			check(std::fabs(mean - want) < want * 0.01 && lo > want * 0.9,
				fmt("cap %d: %d frames, %.3f ms a frame on average (%.3f to %.3f), %.3f ms on the %s timer and %.3f ms spinning a frame (at most %.3f)", cap, frames,
					mean * 1000, lo * 1000, hi * 1000, timer / frames * 1000, high ? "high-resolution" : "plain", spin / frames * 1000, spin_max * 1000));
			check(timer > spin * 3.0, "  most of the wait on the timer, not spinning");
			check(std::fabs(hifps::fps() - cap) < cap * 0.02, fmt("  the frame rate measured: %.2f", hifps::fps()));
			hifps::stop();
		}
		put_word(global, dumped);
		check(dump::same_as_file(dump::kDumps[0], base, size), "the image the dump again");
		dump::unmap(base);
	}
}

extern "C" int __cdecl hifixi_option(const char *section, const char *key, char *out, uint32_t size)
{
	const auto v = g_options.find(std::string(section != nullptr ? section : "") + "." + (key != nullptr ? key : ""));
	if (v == g_options.end() || out == nullptr || size == 0)
		return 0;
	const size_t n = v->second.size() < size - 1 ? v->second.size() : size - 1;
	std::memcpy(out, v->second.data(), n);
	out[n] = 0;
	return 1;
}

extern "C" void __cdecl hifixi_set_option(const char *section, const char *key, const char *value)
{
	g_options[std::string(section) + "." + key] = value != nullptr ? value : "";
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
	std::setvbuf(stdout, nullptr, _IONBF, 0); // (everything out before anything could go wrong)
	g_root = root_folder();
	// The game object and what the scenarios point at
	const bool fake = VirtualAlloc(reinterpret_cast<void *>(kFake), 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE) == reinterpret_cast<void *>(kFake);
	check(fake && memory_free(kGone), "the fake game object at 2F000000; nothing at 2E000000");
	if (!fake)
		return 1;

	// Not found: said why, nothing taken
	std::vector<uint8_t> empty(0x10000, 0);
	hifi::game::set_image(reinterpret_cast<uintptr_t>(empty.data()), empty.size());
	set_option("enabled", "1");
	hifps::update();
	check(!hifps::on() && hifps::status().at(0) == "Not on: expected 2 frame step getters, found 0; nothing patched.",
		"an image without the getters: " + hifps::status().at(0));
	hifps::present(1.0);
	hifps::update();
	check(!hifps::on() && hifps::resolved() == nullptr, "  not tried again");
	hifps::stop();

	for (const char *path : dump::kDumps)
	{
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		{
			check(false, std::string(path) + " is there");
			continue;
		}
		test_dump(path);
	}
	test_limiter();

	std::printf(failures == 0 ? "\nall passed (%d)\n" : "\n%d failed\n", failures == 0 ? passes : failures);
	return failures;
}
