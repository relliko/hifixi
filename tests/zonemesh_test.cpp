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
 *     its size, the models found by name;
 *   - the texture pack's mode (load's pack; docs/texture-pack/zones.md): with no pivot.ini, every map the same both ways
 *     (the oracle's figures, CRCs included), keys made for every texture only in pack mode, its hashing timed; with
 *     the PC's own pivot.ini (read only), pack mode passes hifi-textures over: the meshes, the textures' names and
 *     model.missing as without it, the textures at the game's size with one level and keyed, while a map another
 *     overlay serves (Zi'Tah's canopy) reads that and gets no keys; made-up overlays under build\test\zonemesh: an
 *     overlay listed after hifi-textures is honoured (its copy read, no keys), hifi-textures alone leaves the game's
 *     file (keys), overlaid(path, exclude) by itself and in any case; the real pack (read only, skipped without it):
 *     map 100's keys found in it, and where an entry came from map 100's own DAT its chunk ordinal is the texture's.
 *
 *   build\zonemesh_test.exe              (from this folder)
 *   build\zonemesh_test.exe <map> ...    those maps instead
 *
 * Exit code: the number of failures.
 */

#include "../src/bake/zonemesh.hpp"
#include "../src/bake/dat.hpp"
#include "../src/pack/pack.hpp"
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

	namespace zm = hifi::bake::zonemesh;

	// What pack mode must leave alone: the meshes, the textures' names and how many pieces miss one
	std::string geometry(const zm::zone &z)
	{
		uint32_t missing = 0;
		for (const zm::model &m : z.models)
			missing += m.missing;
		std::string names;
		for (const zm::texture &t : z.textures)
			names.append(t.name, 16);
		char line[300];
		std::snprintf(line, sizeof(line), "%zu models %zu pieces %zu vertices %zu indices missing %u/%u normal maps %zu placements %zu crc %08x %08x %08x", z.models.size(),
			z.pieces.size(), z.vertices.size(), z.indices.size(), z.missing_textures, missing, z.normal_maps.size(), z.placements.size(),
			crc32(z.vertices.data(), z.vertices.size() * sizeof(zm::vertex)), crc32(z.indices.data(), z.indices.size() * 4), crc32(names.data(), names.size()));
		return line;
	}

	uint32_t keyed(const zm::zone &z)
	{
		uint32_t n = 0;
		for (const zm::texture &t : z.textures)
			n += t.key.bytes != 0;
		return n;
	}

	bool one_level(const zm::zone &z)
	{
		for (const zm::texture &t : z.textures)
			if (t.levels != 1 || !t.mips.empty())
				return false;
		return true;
	}

	double ms_since(std::chrono::steady_clock::time_point t0)
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	}

	void make_folders(const std::wstring &path)
	{
		for (size_t at = path.find_first_of(L"\\/", 3); at != std::wstring::npos; at = path.find_first_of(L"\\/", at + 1))
			CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
	}

	std::wstring absolute(const std::wstring &rel)
	{
		wchar_t full[MAX_PATH] = L"";
		GetFullPathNameW(rel.c_str(), MAX_PATH, full, nullptr);
		return full;
	}

	bool write_text(const std::wstring &path, const std::string &text)
	{
		make_folders(path);
		FILE *f = nullptr;
		if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr)
			return false;
		std::fwrite(text.data(), 1, text.size(), f);
		std::fclose(f);
		return true;
	}

	std::string narrow(const std::wstring &w)
	{
		std::string s;
		for (wchar_t c : w)
			s += c < 128 ? char(c) : '?';
		return s;
	}

	void test_pack_mode(const std::vector<uint32_t> &maps)
	{
		std::printf("\nThe texture pack's mode\n");
		// No pivot.ini: the game's own files both ways
		SetEnvironmentVariableW(L"HIFIXI_PIVOT_INI", L"build\\test\\zonemesh\\none.ini");
		for (const uint32_t m : maps)
		{
			zm::zone a, b;
			hifi::bake::result ra, rb;
			const auto t0 = std::chrono::steady_clock::now();
			const bool oka = zm::load(m, a, ra);
			const double ms_off = ms_since(t0);
			const auto t1 = std::chrono::steady_clock::now();
			const bool okb = zm::load(m, b, rb, true);
			const double ms_pack = ms_since(t1);
			size_t bytes = 0;
			for (const zm::texture &t : b.textures)
				bytes += t.key.bytes;
			char head[200];
			std::snprintf(head, sizeof(head), "map %u, no pivot.ini: the same both ways, %u of %zu textures keyed only in pack mode (%.0f ms, %.0f without; %.1f MB hashed)", m,
				keyed(b), b.textures.size(), ms_pack, ms_off, double(bytes) / 1048576.0);
			check(oka == okb && (!oka || (figures(a) == figures(b) && keyed(a) == 0 && keyed(b) == b.textures.size() && b.pack && !b.overlaid && !a.pack &&
				one_level(b))), head);
		}

		// The PC's own pivot.ini (read only): hifi-textures passed over in pack mode, any other overlay honoured
		const std::wstring real_ini = L"C:\\Games\\PhoenixXI\\config\\pivot\\pivot.ini";
		if (GetFileAttributesW(real_ini.c_str()) == INVALID_FILE_ATTRIBUTES)
			std::printf("--    skipped: no %s\n", narrow(real_ini).c_str());
		else
		{
			SetEnvironmentVariableW(L"HIFIXI_PIVOT_INI", real_ini.c_str());
			for (const uint32_t m : maps)
			{
				zm::zone a, b;
				hifi::bake::result ra, rb;
				const bool oka = zm::load(m, a, ra), okb = zm::load(m, b, rb, true);
				bool smaller = a.textures.size() == b.textures.size();
				for (size_t i = 0; smaller && i < a.textures.size(); ++i)
					smaller = b.textures[i].w <= a.textures[i].w && b.textures[i].h <= a.textures[i].h;
				uint32_t bigger_off = 0, levels_off = 0;
				for (size_t i = 0; i < a.textures.size() && i < b.textures.size(); ++i)
				{
					bigger_off += a.textures[i].w > b.textures[i].w;
					levels_off = std::max(levels_off, a.textures[i].levels);
				}
				char head[300];
				if (okb && b.overlaid)
				{
					std::snprintf(head, sizeof(head), "map %u, the PC's pivot.ini: read from another overlay both ways (not hifi-textures): no keys, the same both ways", m);
					check(oka && figures(a) == figures(b) && keyed(b) == 0, head);
					continue;
				}
				std::snprintf(head, sizeof(head), "map %u, the PC's pivot.ini: %s without the pack (%u textures bigger, up to %u levels); in pack mode the game's own file: "
					"the same meshes, names and misses, %u of %zu keyed, at the game's size, one level", m, a.overlaid ? "an overlay's copy" : "the game's own", bigger_off,
					levels_off, keyed(b), b.textures.size());
				check(oka && okb && geometry(a) == geometry(b) && smaller && keyed(b) == b.textures.size() && one_level(b) && !b.overlaid, head);
			}
		}

		// Made-up overlays: copies of map 100's DAT under build\test\zonemesh\over\{hifi-textures,fake}
		std::string why;
		const std::wstring own = hifi::bake::dat_path(zm::map_dat(100), why);
		const std::wstring &ffxi = hifi::bake::ffxi_folder();
		const std::wstring rel = own.size() > ffxi.size() + 1 ? own.substr(ffxi.size() + 1) : L"";
		const std::wstring root = absolute(L"build\\test\\zonemesh\\over");
		const std::wstring hifi_copy = root + L"\\hifi-textures\\" + rel, fake_copy = root + L"\\fake\\" + rel;
		make_folders(hifi_copy);
		make_folders(fake_copy);
		const bool copied = !rel.empty() && CopyFileW(own.c_str(), hifi_copy.c_str(), FALSE) && CopyFileW(own.c_str(), fake_copy.c_str(), FALSE);
		const std::wstring ini_a = absolute(L"build\\test\\zonemesh\\a.ini"), ini_b = absolute(L"build\\test\\zonemesh\\b.ini");
		const bool inis = write_text(ini_a, "[settings]\r\nroot_path=" + narrow(root) + "\r\n[overlays]\r\n0=HIFI-TEXTURES\r\n1=fake\r\n") &&
			write_text(ini_b, "[settings]\r\nroot_path=" + narrow(root) + "\r\n[overlays]\r\n0=hifi-textures\r\n");
		check(copied && inis, "made-up overlays: map 100's DAT copied into hifi-textures and fake under build\\test\\zonemesh\\over");
		SetEnvironmentVariableW(L"HIFIXI_PIVOT_INI", ini_a.c_str());
		{
			zm::zone a, b;
			hifi::bake::result ra, rb;
			const bool oka = zm::load(100, a, ra), okb = zm::load(100, b, rb, true);
			// (the path as pivot.ini spells the overlay: any case)
			auto same = [](const std::wstring &x, const std::wstring &y) { return _wcsicmp(x.c_str(), y.c_str()) == 0; };
			const bool paths = same(hifi::bake::overlaid(own), hifi_copy) && same(hifi::bake::overlaid(own, L"hifi-textures"), fake_copy) &&
				same(hifi::bake::overlaid(own, L"Hifi-Textures"), fake_copy) && same(hifi::bake::overlaid(own, L"fake"), hifi_copy);
			check(paths, "  0=HIFI-TEXTURES 1=fake: overlaid(path) the hifi-textures copy; with hifi-textures left out (any case) fake's; with fake left out hifi-textures'");
			check(oka && okb && a.overlaid && b.overlaid && keyed(a) == 0 && keyed(b) == 0 && figures(a) == figures(b),
				"  pack mode reads the overlay after hifi-textures (fake's copy) and makes no keys (zones.md decision 6)");
		}
		SetEnvironmentVariableW(L"HIFIXI_PIVOT_INI", ini_b.c_str());
		{
			zm::zone a, b;
			hifi::bake::result ra, rb;
			const bool oka = zm::load(100, a, ra), okb = zm::load(100, b, rb, true);
			check(hifi::bake::overlaid(own, L"hifi-textures") == own && oka && okb && a.overlaid && !b.overlaid && keyed(b) == b.textures.size() && figures(a) == figures(b),
				"  0=hifi-textures alone: without the pack its copy, in pack mode the game's own file, keyed");
		}

		// The real pack (read only): map 100's keys found, an entry from map 100's own DAT at its texture's chunk ordinal
		SetEnvironmentVariableW(L"HIFIXI_PIVOT_INI", L"build\\test\\zonemesh\\none.ini");
		const std::wstring pack_folder = L"C:\\Games\\PhoenixXI\\bootloader\\hifixi-pack\\";
		if (GetFileAttributesW((pack_folder + L"pack.idx").c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			std::printf("--    skipped: no texture pack at %s\n", narrow(pack_folder).c_str());
			return;
		}
		std::shared_ptr<const hifi::pack::index> idx = hifi::pack::load(pack_folder, nullptr, why);
		zm::zone z;
		hifi::bake::result r;
		std::vector<uint8_t> dat;
		const bool ok = idx != nullptr && zm::load(100, z, r, true) && hifi::bake::read_file(own, dat);
		std::vector<std::pair<hifi::pack::key, uint32_t>> ordinals;
		const std::vector<hifi::bake::chunk> all = hifi::bake::chunks(dat);
		for (size_t i = 0; i < all.size(); ++i)
			if (all[i].type == 0x20)
				ordinals.emplace_back(hifi::pack::key_of(all[i].body, all[i].size), uint32_t(i));
		uint32_t hits = 0, from_it = 0, at_ordinal = 0;
		for (const zm::texture &t : z.textures)
		{
			const hifi::pack::entry *e = idx != nullptr ? idx->find(t.key) : nullptr;
			if (e == nullptr)
				continue;
			hits++;
			if (e->file_id != zm::map_dat(100))
				continue;
			from_it++;
			for (const auto &[k, i] : ordinals)
				if (k == t.key)
				{
					at_ordinal += e->chunk == i;
					break;
				}
		}
		char line[300];
		std::snprintf(line, sizeof(line), "the real pack (%zu entries, read only): %u of %zu of map 100's keys found; of the %u from its own DAT, %u at their chunk's ordinal",
			idx != nullptr ? idx->entries.size() : size_t(0), hits, z.textures.size(), from_it, at_ordinal);
		check(ok && hits > 0 && at_ordinal == from_it, line);
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
	test_pack_mode(maps);
	dump::unmap(base);
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
