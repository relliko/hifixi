/*
 * Offline test of drawcull (src/game/drawcull.cpp, the Ashita addon drawcull 1.5 in C++), as the addon's own
 * tests/t_square.py and t_frustum.py test it: each FFXiMain dump mapped at its own base (game_dump.hpp), its memory from
 * 0x30000000, HiFi XI's options from a table here, a gGame of the test's own behind the game's global.
 *   - an image without the game's code: said why, nothing done;
 *   - a dump whose sites something else had changed (FFXiMain_041C0000.bin was dumped with the drawcull addon's 1.4
 *     patches in: its culling call, mesh detail compares and visibility read; both with other addons' multipliers):
 *     those sites left alone and said why, no memory taken; then the game's own bytes put back in the mapped copy and
 *     the rest run on it;
 *   - the first update: its memory and every site byte for byte what drawcull.lua writes (run under lupa:
 *     tests/drawcull_golden.py), nothing else changed; the multipliers and the mesh detail factor for a list of options
 *     as the addon's own numbers; options it would refuse taken as its defaults; a multiplier changed by something else
 *     left until the option changes;
 *   - vis, frustum and square on and off; bytes (or a constant) that are neither the game's nor ours left alone, and
 *     ours put there by something else left alone too; a new camera object; no camera object yet;
 *   - stop(): everything back, the image as it was byte for byte, the memory freed; a restart; a stop with a site
 *     changed by something else since (left, its memory kept); at the end the image the dump again, byte for byte.
 *
 *   build\drawcull_test.exe     (built by tests\build_phase5_test.cmd)
 */

#include "../src/game/drawcull.hpp"
#include "game_dump.hpp"
#include <bit>
#include <cmath>
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

	// HiFi XI's options, as the test sets them ("section.key")
	std::map<std::string, std::string> g_options;
	void option(const char *key, const std::string &value)
	{
		g_options[std::string("drawcull.") + key] = value;
	}

	// Image offsets (the same client in both dumps)
	constexpr uintptr_t kWorld = 0x456AB8, kEntity = 0x456AA8, kGameGlobal = 0x4568FC;
	constexpr uintptr_t kCulling = 0x17BF53, kScale = 0x1892B0;
	constexpr uintptr_t kLod[4] = { 0x17CB69, 0x17CB81, 0x17DFDE, 0x17DFFA };
	constexpr uintptr_t kVis = 0x17C0E5, kFrustum = 0x17BF75;
	const char *const kLodStock[4] = { "D9442414D899CC000000", "D9442414D899C8000000", "D9442458D89FCC000000", "D9442458D89FC8000000" };
	constexpr const char *kVisStock = "8B852C910300";
	constexpr const char *kFrustumStock = "8B85B4550300";
	constexpr uint32_t kStock = 0x3F4A7EFA, kSquare = 0x3F400000, kOne = 0x3F800000;

	// drawcull.lua's own bytes (tests/drawcull_golden.py) for its memory at 0x30000000: its memory after load (defaults),
	// the culling call, the mesh detail sites
	struct lua_bytes
	{
		uintptr_t base;
		const char *memory, *culling, *lod[4];
	};
	constexpr lua_bytes kLua[] = {
		{ 0x04490000,
			"FF742404E8A79261D4D80DB86A8E04C2040000000000000000000000000000000000803F000000000000000000000000"
			"D9442418D80D20000030D899CC000000C300000000000000D9442418D80D20000030D899C8000000C300000000000000"
			"D944245CD80D20000030D89FCC000000C300000000000000D944245CD80D20000030D89FC8000000C300000000000000"
			"00000000000000000000000000000000",
			"E8A8409F2B", { "E8C2349F2B9090909090", "E8C2349F2B9090909090", "E87D209F2B9090909090", "E879209F2B9090909090" } },
		{ 0x041C0000,
			"FF742404E8A79234D4D80DB86A6104C2040000000000000000000000000000000000803F000000000000000000000000"
			"D9442418D80D20000030D899CC000000C300000000000000D9442418D80D20000030D899C8000000C300000000000000"
			"D944245CD80D20000030D89FCC000000C300000000000000D944245CD80D20000030D89FC8000000C300000000000000"
			"00000000000000000000000000000000",
			"E8A840CC2B", { "E8C234CC2B9090909090", "E8C234CC2B9090909090", "E87D20CC2B9090909090", "E87920CC2B9090909090" } },
	};
	constexpr const char *kLuaVis = "31C090909090", *kLuaFrustum = "B80000C04390";
	constexpr uint32_t kLuaSquare = 0x3F400000;

	// world, lod: the world multiplier's bits and the mesh detail factor's, as drawcull.lua writes them
	struct world_lod
	{
		const char *world, *lod;
		uint32_t world_bits, factor_bits;
	};
	constexpr world_lod kWorldLod[] = {
		{ "1", "0", 0x3F800000, 0x3F800000 },
		{ "10", "0", 0x41200000, 0x3C23D70A },
		{ "3", "0", 0x40400000, 0x3DE38E39 },
		{ "2.5", "0", 0x40200000, 0x3E23D70A },
		{ "1.3", "0", 0x3FA66666, 0x3F177AB3 },
		{ "0.7", "0", 0x3F333333, 0x3F800000 },
		{ "7.77", "0", 0x40F8A3D7, 0x3C87B0A5 },
		{ "5", "2", 0x40A00000, 0x3E800000 },
		{ "5", "0.5", 0x40A00000, 0x3F800000 },
		{ "5", "1.5", 0x40A00000, 0x3EE38E39 },
		{ "1", "12.34", 0x3F800000, 0x3BD73054 },
		{ "0.3", "1.7", 0x3E99999A, 0x3EB129A2 },
	};
	struct entity_row
	{
		const char *entity;
		uint32_t bits;
	};
	constexpr entity_row kEntityRows[] = {
		{ "1", 0x3F800000 },
		{ "3", 0x40400000 },
		{ "0.25", 0x3E800000 },
		{ "1.3", 0x3FA66666 },
		{ "7.77", 0x40F8A3D7 },
	};

	// gGame, as the test makes it (two: a new camera object), each with the game's 0.791 at +0x2E8
	alignas(16) uint8_t g_game[0x400], g_game2[0x400];

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

	void put_bytes(uintptr_t at, const std::string &b)
	{
		std::memcpy(reinterpret_cast<void *>(at), b.data(), b.size());
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

	bool memory_free(uintptr_t at)
	{
		MEMORY_BASIC_INFORMATION mbi = {};
		return VirtualQuery(reinterpret_cast<void *>(at), &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_FREE;
	}

	bool contains(const std::string &s, const char *part)
	{
		return s.find(part) != std::string::npos;
	}

	std::string lines(const std::vector<std::string> &list)
	{
		std::string s;
		for (const std::string &l : list)
			s += (s.empty() ? "" : " | ") + l;
		return s;
	}

	bool any_line(const std::vector<std::string> &list, const char *part)
	{
		for (const std::string &l : list)
			if (contains(l, part))
				return true;
		return false;
	}

	// The image's offsets that differ from a copy of it
	std::vector<uintptr_t> diff(uintptr_t base, const std::string &copy)
	{
		std::vector<uintptr_t> out;
		const char *p = reinterpret_cast<const char *>(base);
		for (size_t at = 0; at < copy.size(); at += 4096)
		{
			const size_t n = copy.size() - at < 4096 ? copy.size() - at : 4096;
			if (std::memcmp(p + at, copy.data() + at, n) == 0)
				continue;
			for (size_t i = at; i < at + n; ++i)
				if (p[i] != copy[i])
					out.push_back(i);
		}
		return out;
	}

	std::string offsets(const std::vector<uintptr_t> &list)
	{
		std::string s;
		char one[16];
		for (size_t i = 0; i < list.size() && i < 24; ++i)
		{
			std::snprintf(one, sizeof(one), "%s%06X", i > 0 ? " " : "", unsigned(list[i]));
			s += one;
		}
		return s.empty() ? std::string("none") : s;
	}

	// Whether every offset is in one of the ranges (start, length)
	bool only_in(const std::vector<uintptr_t> &list, const std::vector<std::pair<uintptr_t, size_t>> &ranges)
	{
		for (const uintptr_t o : list)
		{
			bool in = false;
			for (const auto &r : ranges)
				in = in || (o >= r.first && o < r.first + r.second);
			if (!in)
				return false;
		}
		return true;
	}

	// One dump: everything t_square.py and t_frustum.py check of the addon, and the rest of its work, on the port
	void test_dump(const std::string &path)
	{
		namespace drawcull = hifi::game::drawcull;
		const std::string file = path.substr(path.find_last_of('\\') + 1);
		std::printf("\n%s\n", file.c_str());
		const int passes_before = passes, failures_before = failures;
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		char line[512];
		std::snprintf(line, sizeof(line), "mapped at its base, 0x%08X", unsigned(base));
		check(size != 0, line);
		if (size == 0)
			return;
		const lua_bytes *lua = nullptr;
		for (const lua_bytes &l : kLua)
			if (l.base == base)
				lua = &l;
		check(lua != nullptr, "drawcull.lua's bytes known for this base");
		if (lua == nullptr)
		{
			dump::unmap(base);
			return;
		}
		g_options.clear();

		const uintptr_t world = base + kWorld, entity = base + kEntity, global = base + kGameGlobal, cull = base + kCulling;
		const uintptr_t vis = base + kVis, frustum = base + kFrustum, mem = dump::kAllocAt;
		const uintptr_t game = reinterpret_cast<uintptr_t>(g_game), game2 = reinterpret_cast<uintptr_t>(g_game2);
		// The game's own bytes at each site, and the dump's
		const std::string own_cull = "\xE8" + word_bytes(uint32_t(base + kScale - (cull + 5)));
		std::string own_lod[4], dumped_lod[4];
		bool lod_changed = false;
		for (int i = 0; i < 4; ++i)
		{
			own_lod[i] = from_hex(kLodStock[i]);
			dumped_lod[i] = bytes_at(base + kLod[i], 10);
			lod_changed = lod_changed || dumped_lod[i] != own_lod[i];
		}
		const std::string own_vis = from_hex(kVisStock), own_frustum = from_hex(kFrustumStock), one = word_bytes(kOne);
		const std::string dumped_cull = bytes_at(cull, 5), dumped_vis = bytes_at(vis, 6), dumped_frustum = bytes_at(frustum, 6);
		const std::string dumped_world = bytes_at(world, 4), dumped_entity = bytes_at(entity, 4), dumped_global = bytes_at(global, 4);
		const bool code_changed = dumped_cull != own_cull || lod_changed || dumped_vis != own_vis || dumped_frustum != own_frustum;
		const bool data_changed = dumped_world != one || dumped_entity != one;
		std::printf("(the dump: culling call %s, mesh detail %s, vis %s, frustum %s; multipliers %s %s)\n", dumped_cull == own_cull ? "the game's" : to_hex(dumped_cull).c_str(),
			lod_changed ? "changed" : "the game's", dumped_vis == own_vis ? "the game's" : to_hex(dumped_vis).c_str(), dumped_frustum == own_frustum ? "the game's" : to_hex(dumped_frustum).c_str(),
			to_hex(dumped_world).c_str(), to_hex(dumped_entity).c_str());

		// gGame behind the game's global: ours, with the game's 0.791
		std::memset(g_game, 0, sizeof(g_game));
		std::memset(g_game2, 0, sizeof(g_game2));
		put_word(game + 0x2E8, kStock);
		put_word(game2 + 0x2E8, kStock);
		put_word(global, uint32_t(game));

		const auto sites_as_dumped = [&]() {
			bool same = bytes_at(cull, 5) == dumped_cull && bytes_at(vis, 6) == dumped_vis && bytes_at(frustum, 6) == dumped_frustum;
			for (int i = 0; i < 4; ++i)
				same = same && bytes_at(base + kLod[i], 10) == dumped_lod[i];
			return same;
		};


		if (code_changed || data_changed)
		{
			// Something else had the sites or the multipliers when this was dumped
			drawcull::update();
			const std::vector<std::string> s = drawcull::status();
			if (code_changed)
			{
				check(sites_as_dumped(), "update(): every code site left as it was dumped");
				check(drawcull::where().memory == 0 && memory_free(mem), "  no memory taken (no call of the game's own to patch)");
				check(any_line(s, "object ranges not scaled: the object culling call goes elsewhere already") &&
					any_line(s, "4 of 4 mesh detail sites not patched") && any_line(s, "vis: on through something else's patch"),
					"  status says why: " + lines(s));
				option("vis", "0");
				drawcull::update();
				check(bytes_at(vis, 6) == dumped_vis && any_line(drawcull::status(), "vis: something else's patch is in"),
					"  vis off: the dump's visibility read left alone (" + lines(drawcull::status()) + ")");
				g_options.clear();
			}
			if (data_changed)
				check(word_at(world) == kOne && word_at(entity) == kOne, "update(): the multipliers written as the options say (1.0, over the dump's " +
					to_hex(dumped_world) + " " + to_hex(dumped_entity) + "), as the addon writes them");
			drawcull::stop();
			check(sites_as_dumped() && word_at(world) == kOne && word_at(entity) == kOne && memory_free(mem),
				"stop(): the code sites as dumped, the multipliers 1.0, no memory left");
			// The game's own bytes put back in the mapped copy, for the rest
			put_bytes(cull, own_cull);
			for (int i = 0; i < 4; ++i)
				put_bytes(base + kLod[i], own_lod[i]);
			put_bytes(vis, own_vis);
			put_bytes(frustum, own_frustum);
			put_bytes(world, one);
			put_bytes(entity, one);
			hifi::game::set_alloc_at(mem);
		}
		const std::string clean = bytes_at(base, size);

		// 1. The first update, with the addon's defaults
		check(drawcull::status() == std::vector<std::string>{ "drawcull: not started yet" }, "before the first update: " + lines(drawcull::status()));
		drawcull::update();
		const drawcull::sites w = drawcull::where();
		bool lod_found = true;
		for (int i = 0; i < 4; ++i)
			lod_found = lod_found && w.lod[i] == base + kLod[i];
		check(w.world == world && w.entity == entity && w.culling == cull && w.scale == base + kScale && lod_found && w.vis == vis && w.frustum == frustum &&
			w.game == global, "found: the multipliers at 456AB8 and 456AA8, the culling call at 17BF53 (to 1892B0), the mesh detail compares, the visibility "
			"read, the frustum's setup, gGame's global");
		check(w.memory == mem, "its memory at 0x30000000");
		const std::string memory = bytes_at(mem, 160);
		check(memory == from_hex(lua->memory), "  byte for byte drawcull.lua's (160 bytes: the culling stub, the factor 1.0, the four detail stubs)" +
			(memory == from_hex(lua->memory) ? std::string() : ": " + to_hex(memory)));
		check(bytes_at(cull, 5) == from_hex(lua->culling), "the culling call goes to the stub, as drawcull.lua's: " + to_hex(bytes_at(cull, 5)));
		bool lod_ok = true;
		std::string lod_hex;
		for (int i = 0; i < 4; ++i)
		{
			lod_ok = lod_ok && bytes_at(base + kLod[i], 10) == from_hex(lua->lod[i]);
			lod_hex += (i > 0 ? " " : "") + to_hex(bytes_at(base + kLod[i], 10));
		}
		check(lod_ok, "the four mesh detail compares call their stubs, as drawcull.lua's: " + lod_hex);
		check(bytes_at(vis, 6) == from_hex(kLuaVis), "vis on by default: xor eax, eax as drawcull.lua's");
		check(bytes_at(frustum, 6) == own_frustum, "frustum off by default: the setup the game's");
		check(word_at(game + 0x2E8) == kStock, "square off by default: gGame+0x2E8 still 0.791");
		check(word_at(world) == kOne && word_at(entity) == kOne, "the multipliers 1.0");
		std::vector<uintptr_t> changed = diff(base, clean);
		check(only_in(changed, { { kCulling + 1, 4 }, { kLod[0], 10 }, { kLod[1], 10 }, { kLod[2], 10 }, { kLod[3], 10 }, { kVis, 6 } }) && !changed.empty(),
			"only those sites changed: " + offsets(changed));
		check(drawcull::status() == std::vector<std::string>{ "drawcull: world 1.00, entity 1.00, detail 1.00 (same as world), vis on, square off, frustum off" },
			"status: " + lines(drawcull::status()));
		const std::string after_first = bytes_at(base, size);
		drawcull::update();
		check(bytes_at(base, size) == after_first && memory_free(mem + 0x10000), "update again: nothing more changed, no second allocation");

		// 2. The multipliers and the mesh detail factor, as the addon's own numbers
		for (const world_lod &r : kWorldLod)
		{
			option("world", r.world);
			option("lod", r.lod);
			drawcull::update();
			const uint32_t wb = word_at(world), kb = word_at(mem + 32);
			std::snprintf(line, sizeof(line), "world %s, lod %s: world %08X, detail factor %08X (drawcull.lua: %08X, %08X)", r.world, r.lod, wb, kb, r.world_bits, r.factor_bits);
			check(wb == r.world_bits && kb == r.factor_bits, line);
		}
		for (const entity_row &r : kEntityRows)
		{
			option("entity", r.entity);
			drawcull::update();
			std::snprintf(line, sizeof(line), "entity %s: %08X (drawcull.lua: %08X)", r.entity, word_at(entity), r.bits);
			check(word_at(entity) == r.bits, line);
		}
		option("world", "10");
		option("lod", "0");
		option("entity", "3");
		drawcull::update();
		check(drawcull::status().front() == "drawcull: world 10.00, entity 3.00, detail 10.00 (same as world), vis on, square off, frustum off",
			"status: " + drawcull::status().front());
		option("lod", "2.5");
		drawcull::update();
		check(drawcull::status().front() == "drawcull: world 10.00, entity 3.00, detail 2.50, vis on, square off, frustum off", "status: " + drawcull::status().front());
		// (what the addon's command refused: taken as its defaults)
		option("world", "0");
		option("entity", "-2");
		option("lod", "-1");
		drawcull::update();
		check(word_at(world) == kOne && word_at(entity) == kOne && word_at(mem + 32) == kOne, "world 0, entity -2, lod -1: taken as 1, 1 and 0");
		option("world", "far");
		drawcull::update();
		check(word_at(world) == kOne, "world 'far': taken as 1");
		// A multiplier changed by something else: left until its option changes
		put_word(world, 0x40000000);
		drawcull::update();
		check(word_at(world) == 0x40000000 && any_line(drawcull::status(), "the world multiplier is 2.00 now (changed by something else since)"),
			"the world multiplier set to 2.0 by something else: left, and said (" + lines(drawcull::status()) + ")");
		option("world", "3");
		drawcull::update();
		check(word_at(world) == 0x40400000 && drawcull::status().size() == 1, "world 3: written again");
		g_options.clear();
		drawcull::update();
		check(word_at(world) == kOne && word_at(entity) == kOne && word_at(mem + 32) == kOne && bytes_at(base, size) == after_first,
			"the options cleared: the defaults again, the image as after the first update");

		// 3. vis
		option("vis", "0");
		drawcull::update();
		check(bytes_at(vis, 6) == own_vis && contains(drawcull::status().front(), "vis off"), "vis 0: the game's visibility read back");
		option("vis", "1");
		drawcull::update();
		check(bytes_at(vis, 6) == from_hex(kLuaVis), "vis 1: xor eax, eax again");
		option("vis", "false");
		drawcull::update();
		check(bytes_at(vis, 6) == own_vis, "vis false: the game's again");
		const std::string foreign = from_hex("E9112233448B");
		put_bytes(vis, foreign);
		option("vis", "1");
		drawcull::update();
		check(bytes_at(vis, 6) == foreign && any_line(drawcull::status(), "vis: the game's code there is changed by something else: left alone"),
			"foreign bytes at the visibility read: left alone by vis on, and said");
		put_bytes(vis, own_vis);
		drawcull::update();
		check(bytes_at(vis, 6) == from_hex(kLuaVis) && drawcull::status().size() == 1, "the game's bytes back there: vis on again");

		// 4. frustum (t_frustum.py)
		const std::string before = bytes_at(base, size);
		option("frustum", "1");
		drawcull::update();
		check(bytes_at(frustum, 6) == from_hex(kLuaFrustum), "frustum on: mov eax, 384.0; nop, as drawcull.lua's");
		float imm = 0.0f;
		hifi::game::float_of(word_at(frustum + 1), imm);
		check(imm == 384.0f, "  the immediate is 384.0");
		changed = diff(base, before);
		check(only_in(changed, { { kFrustum, 6 } }) && !changed.empty(), "  only those 6 bytes changed: " + offsets(changed));
		check(bytes_at(frustum + 6, 19) == from_hex("5168CDCC4C3E518DB55C8E0300D995B0550300"), "  the code after it is untouched");
		check(contains(drawcull::status().front(), "frustum on"), "  status: " + drawcull::status().front());
		option("frustum", "0");
		drawcull::update();
		check(bytes_at(frustum, 6) == own_frustum && bytes_at(base, size) == before, "frustum off: the game's again, the image as before");
		put_bytes(frustum, foreign);
		drawcull::update();
		check(bytes_at(frustum, 6) == foreign && any_line(drawcull::status(), "frustum: the game's code there is changed by something else: left alone"),
			"foreign bytes: left alone by frustum off, and said");
		option("frustum", "1");
		drawcull::update();
		check(bytes_at(frustum, 6) == foreign, "  and by frustum on");
		put_bytes(frustum, from_hex(kLuaFrustum));
		option("frustum", "0");
		drawcull::update();
		check(bytes_at(frustum, 6) == from_hex(kLuaFrustum) && any_line(drawcull::status(), "frustum: something else's patch is in"),
			"384.0 put there by something else: left alone by frustum off");
		option("frustum", "1");
		drawcull::update();
		check(bytes_at(frustum, 6) == from_hex(kLuaFrustum) && any_line(drawcull::status(), "frustum: on through something else's patch"),
			"  and left as it is by frustum on");
		put_bytes(frustum, own_frustum);
		drawcull::update();
		check(bytes_at(frustum, 6) == from_hex(kLuaFrustum) && drawcull::status().size() == 1, "the game's bytes back there: ours in");
		option("frustum", "0");
		drawcull::update();
		check(bytes_at(frustum, 6) == own_frustum, "frustum off");

		// 5. square (t_square.py)
		option("square", "1");
		drawcull::update();
		check(word_at(game + 0x2E8) == kLuaSquare, "square on: gGame+0x2E8 = 0.75, as drawcull.lua's");
		float k = 0.0f;
		hifi::game::float_of(word_at(game + 0x2E8), k);
		const double a = 9.0 / (0.75 * 16.0); // (what aspect writes at 16:9)
		check(std::abs(1.0 / (a * k) - 16.0 / 9.0) < 1e-6, "  the projection's aspect 16/9 exactly");
		check(contains(drawcull::status().front(), "square on") && drawcull::status().size() == 1, "  status: " + drawcull::status().front());
		put_word(game + 0x2E8, kStock);
		drawcull::update();
		check(word_at(game + 0x2E8) == kSquare, "reset to 0.791 (a new camera object's): 0.75 again at the next update");
		put_word(game + 0x2E8, kOne);
		drawcull::update();
		check(word_at(game + 0x2E8) == kOne && any_line(drawcull::status(), "square: the camera's aspect constant is changed by something else: left alone"),
			"another value (1.0): left alone by square on, and said");
		option("square", "0");
		drawcull::update();
		check(word_at(game + 0x2E8) == kOne, "  and by square off");
		put_word(game + 0x2E8, kSquare);
		drawcull::update();
		check(word_at(game + 0x2E8) == kSquare && any_line(drawcull::status(), "square: 0.75 set by something else (the drawcull addon?): left alone"),
			"0.75 set by something else: left alone by square off");
		put_word(game + 0x2E8, kStock);
		option("square", "1");
		drawcull::update();
		check(word_at(game + 0x2E8) == kSquare, "0.791 again: square on puts 0.75");
		put_word(global, uint32_t(game2));
		drawcull::update();
		check(word_at(game2 + 0x2E8) == kSquare && word_at(game + 0x2E8) == kSquare, "a new camera object: 0.75 in it too");
		option("square", "0");
		drawcull::update();
		check(word_at(game2 + 0x2E8) == kStock && word_at(game + 0x2E8) == kSquare, "square off: the new one's 0.791 back, the old one left alone");
		put_word(game + 0x2E8, kStock);
		put_word(global, 0);
		option("square", "1");
		drawcull::update();
		check(word_at(game + 0x2E8) == kStock && word_at(game2 + 0x2E8) == kStock && any_line(drawcull::status(), "square: waiting for the camera object"),
			"no gGame yet: nothing written, and said");
		put_word(global, uint32_t(game));
		drawcull::update();
		check(word_at(game + 0x2E8) == kSquare && drawcull::status().size() == 1, "gGame there: 0.75 at the next update");

		// 6. stop() with everything on: the image as it was, byte for byte
		option("world", "3");
		option("entity", "2");
		option("lod", "2");
		option("frustum", "1");
		drawcull::update();
		check(word_at(world) == 0x40400000 && word_at(entity) == 0x40000000 && bytes_at(frustum, 6) == from_hex(kLuaFrustum) && word_at(game + 0x2E8) == kSquare,
			"world 3, entity 2, lod 2, frustum on, square on");
		drawcull::stop();
		changed = diff(base, clean);
		check(changed.empty(), "stop(): the image as it was before the first update, byte for byte (" + offsets(changed) + ")");
		check(word_at(game + 0x2E8) == kStock, "  gGame+0x2E8 0.791 again");
		check(memory_free(mem) && drawcull::where().memory == 0 && drawcull::where().world == 0, "  its memory freed, everything forgotten");
		check(drawcull::status() == std::vector<std::string>{ "drawcull: not started yet" }, "  status: " + lines(drawcull::status()));
		drawcull::stop();
		check(diff(base, clean).empty(), "stop() again: nothing to do");

		// 7. Started again, then stopped with a site changed by something else since
		hifi::game::set_alloc_at(mem);
		g_options.clear();
		drawcull::update();
		check(drawcull::where().memory == mem && bytes_at(mem, 160) == from_hex(lua->memory) && bytes_at(base, size) == after_first,
			"update after stop: started afresh, the same memory and patches");
		const std::string nops(10, '\x90');
		put_bytes(base + kLod[1], nops);
		drawcull::stop();
		check(bytes_at(base + kLod[1], 10) == nops && bytes_at(base + kLod[0], 10) == own_lod[0] && bytes_at(cull, 5) == own_cull && bytes_at(vis, 6) == own_vis,
			"stop() with a mesh detail compare changed since: that one left as it is, the rest put back");
		check(!memory_free(mem), "  its memory kept (something may still call into it)");
		const std::vector<std::string> s = drawcull::status();
		check(s.size() == 2 && s[1] == "drawcull: at its last stop, a mesh detail compare was changed by something else since: left as it was; its stubs' memory "
			"is left (something may still call into it)", "  status: " + lines(s));
		put_bytes(base + kLod[1], own_lod[1]);
		VirtualFree(reinterpret_cast<void *>(mem), 0, MEM_RELEASE);
		check(diff(base, clean).empty(), "  (the test puts that one back and frees the memory: the image as it was)");
		drawcull::stop();
		check(drawcull::status() == std::vector<std::string>{ "drawcull: not started yet" }, "the next stop: the note gone");

		// The dump's own bytes back
		put_bytes(cull, dumped_cull);
		for (int i = 0; i < 4; ++i)
			put_bytes(base + kLod[i], dumped_lod[i]);
		put_bytes(vis, dumped_vis);
		put_bytes(frustum, dumped_frustum);
		put_bytes(world, dumped_world);
		put_bytes(entity, dumped_entity);
		put_bytes(global, dumped_global);
		check(dump::same_as_file(path, base, size), code_changed || data_changed ? "the image the dump again, byte for byte (its sites and multipliers as dumped put back)" :
			"the image the dump again, byte for byte");
		dump::unmap(base);
		std::printf("(%s: %d ok, %d failed)\n", file.c_str(), passes - passes_before, failures - failures_before);
	}
}

extern "C"
{
	int __cdecl hifixi_option(const char *section, const char *key, char *out, uint32_t size)
	{
		const auto v = g_options.find(std::string(section != nullptr ? section : "") + "." + (key != nullptr ? key : ""));
		if (v == g_options.end() || out == nullptr || size == 0)
			return 0;
		const size_t n = v->second.size() < size - 1 ? v->second.size() : size - 1;
		std::memcpy(out, v->second.data(), n);
		out[n] = 0;
		return 1;
	}
	void __cdecl hifixi_set_option(const char *section, const char *key, const char *value)
	{
		g_options[std::string(section != nullptr ? section : "") + "." + (key != nullptr ? key : "")] = value != nullptr ? value : "";
	}
	int __cdecl worldfx_setting(const char *, char *, uint32_t)
	{
		return 0;
	}
	const char *__cdecl worldfx_folder()
	{
		return "";
	}
}

int main()
{
	namespace drawcull = hifi::game::drawcull;

	// Not found: said why, nothing done
	std::vector<uint8_t> empty(0x10000, 0);
	hifi::game::set_image(reinterpret_cast<uintptr_t>(empty.data()), empty.size());
	drawcull::update();
	check(drawcull::status() == std::vector<std::string>{ "drawcull: the draw distance values weren't found: nothing done" } && drawcull::where().memory == 0,
		"an image without the game's code: " + lines(drawcull::status()));
	drawcull::update();
	drawcull::stop();
	check(drawcull::status() == std::vector<std::string>{ "drawcull: not started yet" }, "  stop(): nothing to put back, " + lines(drawcull::status()));

	for (const char *path : dump::kDumps)
	{
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		{
			check(false, std::string(path) + " is there");
			continue;
		}
		test_dump(path);
	}
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
