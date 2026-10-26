/*
 * The native zone renderer's zone data (src/bake/zonemesh.cpp) against tests/zonemesh_oracle.py, an independent
 * reading of the same DATs in Python: per map, the counts (models by kind, pieces, vertices, triangles, textures,
 * pieces without one, normal maps, pieces with one, placements, alpha-tested, mirrored) and CRC-32s of the vertices,
 * the indices, the texture data and the normal maps, which must all be the same.
 *   - the FFXiMain dump mapped first (tests/game_dump.hpp), so key_tables finds the game's key tables in it;
 *   - sobel on made-up height maps: flat ground is straight up (128, 128, 255); a slope tilts the right channel;
 *   - the maps: West Ronfaure 100, Bibiki Bay 4 (swaying palms, kind 2), Southern San d'Oria 230, Port Windurst 240,
 *     its Fishermen's Guild 412 (a second map), Aht Urhgan Whitegate 50 (the MMBs' second key table), Zi'Tah 121
 *     (ROM2), and 1 (Phanauet Channel: moving scenery);
 *   - each map's load timed, and the vertex layouts' sanity: every index within the vertices, every texture's data
 *     its size, the models found by name.
 *
 *   build\zonemesh_test.exe              (from this folder)
 *   build\zonemesh_test.exe <map> ...    those maps instead
 *
 * Exit code: the number of failures.
 */

#include "../src/bake/zonemesh.hpp"
#include "game_dump.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
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

	uint32_t crc32(const void *data, size_t size, uint32_t crc = 0)
	{
		static uint32_t table[256];
		static bool made = false;
		if (!made)
		{
			for (uint32_t i = 0; i < 256; ++i)
			{
				uint32_t c = i;
				for (int k = 0; k < 8; ++k)
					c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
				table[i] = c;
			}
			made = true;
		}
		crc = ~crc;
		const uint8_t *p = static_cast<const uint8_t *>(data);
		for (size_t i = 0; i < size; ++i)
			crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
		return ~crc;
	}

	// The oracle's line for a loaded map
	std::string figures(const hifi::bake::zonemesh::zone &z)
	{
		using namespace hifi::bake::zonemesh;
		uint32_t kind1 = 0, kind2 = 0, mapped = 0, tested = 0, mirrored = 0;
		for (const model &m : z.models)
			(m.kind == 2 ? kind2 : kind1)++;
		for (const piece &p : z.pieces)
			mapped += p.normal_map != kNone;
		for (const placement &p : z.placements)
		{
			tested += (p.flags & kAlphaTested) != 0;
			mirrored += (p.flags & kMirrored) != 0;
		}
		const uint32_t crc_v = crc32(z.vertices.data(), z.vertices.size() * sizeof(vertex));
		const uint32_t crc_i = crc32(z.indices.data(), z.indices.size() * 4);
		uint32_t crc_t = 0, crc_n = 0;
		for (const texture &t : z.textures)
			crc_t = crc32(t.data.data(), t.data.size(), crc_t);
		for (const normal_map &n : z.normal_maps)
			crc_n = crc32(n.texels.data(), n.texels.size() * 4, crc_n);
		char line[512];
		std::snprintf(line, sizeof(line),
			"%u models %zu kind1 %u kind2 %u pieces %zu vertices %zu triangles %zu textures %zu missing %u normalmaps %zu mapped %u "
			"placed %zu tested %u mirrored %u crc %08x %08x %08x %08x",
			z.map, z.models.size(), kind1, kind2, z.pieces.size(), z.vertices.size(), z.indices.size() / 3, z.textures.size(), z.missing_textures,
			z.normal_maps.size(), mapped, z.placements.size(), tested, mirrored, crc_v, crc_i, crc_t, crc_n);
		return line;
	}

	void test_sobel()
	{
		using hifi::bake::zonemesh::sobel;
		std::vector<uint8_t> h(16 * 8, 100);
		std::vector<uint32_t> n(16 * 8);
		sobel(h.data(), 16, 8, n.data());
		bool flat = true;
		for (const uint32_t t : n)
			flat = flat && t == 0x008080FFu;
		check(flat, "sobel: flat ground is (128, 128, 255) everywhere");
		// (heights rising one a row downward: the row above is lower than the one below, so gy = 4 (y - 1) - 4 (y + 1) = -8
		// everywhere but the wrapping rows; R = 128 + trunc(-8 x 127 / sqrt(64 + 65025)) = 125)
		for (uint32_t y = 0; y < 8; ++y)
			for (uint32_t x = 0; x < 16; ++x)
				h[y * 16 + x] = uint8_t(100 + y);
		sobel(h.data(), 16, 8, n.data());
		const uint32_t t = n[3 * 16 + 5];
		check(((t >> 16) & 0xFF) == 125 && ((t >> 8) & 0xFF) == 128 && (t & 0xFF) == 254, "  heights rising downward: R 125, G 128, B 254 (" + std::to_string(t >> 16 & 0xFF) + " " +
			std::to_string(t >> 8 & 0xFF) + " " + std::to_string(t & 0xFF) + ")");
		// (the wrap: row 0's up neighbour is row 7, 7 higher)
		const uint32_t w = n[5];
		check(((w >> 16) & 0xFF) > 128, "  the edges wrap (row 0 sees row 7 above it)");
	}
}

// (HiFi XI's options and World FX's settings: not looked at)
extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main(int argc, char **argv)
{
	namespace zm = hifi::bake::zonemesh;
	test_sobel();
	uintptr_t base = 0;
	if (dump::map(dump::kDumps[0], base) == 0)
	{
		check(false, std::string(dump::kDumps[0]) + " mapped");
		return failures;
	}
	std::vector<uint32_t> maps = { 100, 4, 230, 240, 412, 50, 121, 1 };
	if (argc > 1)
	{
		maps.clear();
		for (int i = 1; i < argc; ++i)
			maps.push_back(uint32_t(std::strtoul(argv[i], nullptr, 10)));
	}

	// The oracle, all maps in one run
	char exe[MAX_PATH] = "";
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	std::string root = exe;
	root = root.substr(0, root.find_last_of("\\/"));
	root = root.substr(0, root.find_last_of("\\/") + 1);
	std::string command = "python \"" + root + "tests\\zonemesh_oracle.py\"";
	for (const uint32_t m : maps)
		command += " " + std::to_string(m);
	std::map<uint32_t, std::string> oracle;
	const auto py_start = std::chrono::steady_clock::now();
	if (FILE *p = _popen(command.c_str(), "r"))
	{
		char line[1024];
		while (std::fgets(line, sizeof(line), p) != nullptr)
		{
			std::string s = line;
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
				s.pop_back();
			oracle[uint32_t(std::strtoul(s.c_str(), nullptr, 10))] = s;
		}
		_pclose(p);
	}
	const double py_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - py_start).count();
	check(oracle.size() == maps.size(), "the oracle ran (" + std::to_string(oracle.size()) + " maps, " + std::to_string(int(py_s)) + " s)");

	for (const uint32_t m : maps)
	{
		zm::zone z;
		hifi::bake::result r;
		const auto t0 = std::chrono::steady_clock::now();
		const bool ok = zm::load(m, z, r);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		const std::string mine = ok ? figures(z) : std::to_string(m) + " none";
		const auto o = oracle.find(m);
		char head[128];
		std::snprintf(head, sizeof(head), "map %u: loaded in %.0f ms, %.1f MB", m, ms, double(z.bytes()) / 1048576.0);
		check(o != oracle.end() && o->second == mine, std::string(head) + (ok ? "" : " (" + r.why + ")") + "\n        mine   " + mine +
			"\n        oracle " + (o != oracle.end() ? o->second : std::string("-")));
		if (!ok)
			continue;
		bool in_range = true;
		for (const uint32_t i : z.indices)
			in_range = in_range && i < z.vertices.size();
		bool sizes = true;
		for (const zm::texture &t : z.textures)
			sizes = sizes && t.data.size() == (t.format == zm::kBC2 ? size_t(t.w / 4) * (t.h / 4) * 16 : size_t(t.w) * t.h * 4);
		bool named = true;
		for (uint32_t i = 0; i < z.models.size(); ++i)
			named = named && z.find(z.models[i].name) == i;
		bool pieces = true;
		for (const zm::model &mo : z.models)
			for (uint32_t k = mo.first_piece; k < mo.first_piece + mo.pieces; ++k)
			{
				const zm::piece &p = z.pieces[k];
				for (uint32_t i = p.first_index; i < p.first_index + p.triangles * 3; ++i)
					pieces = pieces && z.indices[i] >= mo.first_vertex && z.indices[i] < mo.first_vertex + mo.vertices;
			}
		check(in_range && sizes && named && pieces, "  indices within their model's vertices, texture sizes right, models found by name");
	}
	dump::unmap(base);
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
