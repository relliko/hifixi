/*
 * The texture pack's format, reader and writer (src/pack: pack.hpp, pack_write.hpp, validate.hpp, hash.hpp, bc2.hpp;
 * docs/texture-pack/format.md), offline, in packs written under build\test\pack_test:
 *   - CRC-32 is zlib's (the check value of "123456789", continued in two parts);
 *   - the copy layout (footprint) against GetCopyableFootprints on WARP: BC2, BC7 and BC4, widths that aren't
 *     multiples of 64 or of 256 bytes a row, every level count of each; and the rules refusing what a pack mustn't hold;
 *   - round trip: entries of many sizes written, loaded, found and read back whole and level by level, rows where the
 *     layout puts them and the padding zeros; hit and miss (another hash, size, width or height misses);
 *   - roll-over: with a small test limit the records go into several data files, none past the limit, also when the
 *     next file holds an interrupted run's bytes; an index naming a record past 1 GiB refused;
 *   - dedupe: a key added again with the same bytes or other ones isn't written (counted), unless told to replace it; a
 *     dropped key gone from the next index;
 *   - one writer a folder: a second writer (and a folder_lock) refused while pack.lock is held, gone after close;
 *   - an index refused whole: cut short, version 2, a damaged table, unsorted keys, a missing or shortened data file;
 *     a record whose stored bytes aren't the copy layout (wrong pitch), a data file cut short after loading, a damaged
 *     byte and two records' bytes swapped all miss;
 *   - resume: kept entries, pack.src's lines (only once committed; the overlay copy's size and time too; one forget()
 *     took out rewritten away), appending past what an interrupted run left;
 *     pack.idx replaced while a reader reads the old one; the folder renamed once nothing holds it;
 *   - cancel: a cancelled reader's reads fail until resume;
 *   - the client fingerprint: a mismatch only a status line;
 *   - mips: cut-out coverage within 2% of level 0 in every level of 256 texels and more (zone vertex alpha 0x80 and 0xFF,
 *     characters' 0x22) where the plain box loses it; blended, mask, mixed and none exactly smaller_levels' box;
 *   - tests\pack_oracle.py (python -I) reading a written pack, and refusing a damaged one.
 *
 *   build\pack_test.exe     (built by build.cmd; needs python on PATH for the oracle)
 *
 * Exit code: the number of failures.
 */

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../src/pack/bc2.hpp"
#include "../src/pack/hash.hpp"
#include "../src/pack/pack.hpp"
#include "../src/pack/pack_write.hpp"
#include "../src/pack/validate.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace pk = hifi::pack;

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

	std::wstring root;  // the repo (with its slash)
	std::wstring base;  // build\test\pack_test\ (with its slash)

	std::string narrow(const std::wstring &w)
	{
		std::string s;
		for (wchar_t c : w)
			s += char(c);
		return s;
	}

	void remove_tree(const std::wstring &folder)
	{
		WIN32_FIND_DATAW f;
		HANDLE h = FindFirstFileW((folder + L"*").c_str(), &f);
		if (h != INVALID_HANDLE_VALUE)
		{
			do
			{
				const std::wstring name = f.cFileName;
				if (name == L"." || name == L"..")
					continue;
				if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					remove_tree(folder + name + L"\\");
				else
					DeleteFileW((folder + name).c_str());
			} while (FindNextFileW(h, &f));
			FindClose(h);
		}
		RemoveDirectoryW(folder.substr(0, folder.size() - 1).c_str());
	}

	// A flat folder copied (a pack, to damage the copy)
	std::wstring copy_pack(const std::wstring &from, const wchar_t *name)
	{
		const std::wstring to = base + name + L"\\";
		remove_tree(to);
		CreateDirectoryW(to.c_str(), nullptr);
		WIN32_FIND_DATAW f;
		HANDLE h = FindFirstFileW((from + L"*").c_str(), &f);
		if (h != INVALID_HANDLE_VALUE)
		{
			do
				if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
					CopyFileW((from + f.cFileName).c_str(), (to + f.cFileName).c_str(), FALSE);
			while (FindNextFileW(h, &f));
			FindClose(h);
		}
		return to;
	}

	std::vector<uint8_t> read_all(const std::wstring &path)
	{
		std::vector<uint8_t> out;
		FILE *f = nullptr;
		_wfopen_s(&f, path.c_str(), L"rb");
		if (f == nullptr)
			return out;
		std::fseek(f, 0, SEEK_END);
		out.resize(size_t(std::ftell(f)));
		std::fseek(f, 0, SEEK_SET);
		if (!out.empty() && std::fread(out.data(), 1, out.size(), f) != out.size())
			out.clear();
		std::fclose(f);
		return out;
	}
	void write_all(const std::wstring &path, const std::vector<uint8_t> &bytes)
	{
		FILE *f = nullptr;
		_wfopen_s(&f, path.c_str(), L"wb");
		if (f == nullptr)
			return;
		if (!bytes.empty())
			std::fwrite(bytes.data(), 1, bytes.size(), f);
		std::fclose(f);
	}

	// An index written back after a change (the test's own writing of the format, CRCs made again)
	void rewrite_index(const std::wstring &folder, const pk::index &x)
	{
		pk::header h = x.head;
		std::vector<uint8_t> b(sizeof(pk::header) + x.file_bytes.size() * 8 + x.entries.size() * sizeof(pk::entry) + x.level_crcs.size() * 4);
		uint8_t *p = b.data() + sizeof(pk::header);
		std::memcpy(p, x.file_bytes.data(), x.file_bytes.size() * 8);
		p += x.file_bytes.size() * 8;
		std::memcpy(p, x.entries.data(), x.entries.size() * sizeof(pk::entry));
		p += x.entries.size() * sizeof(pk::entry);
		std::memcpy(p, x.level_crcs.data(), x.level_crcs.size() * 4);
		h.entry_count = uint32_t(x.entries.size());
		h.table_crc = pk::crc32(0, b.data() + sizeof(pk::header), b.size() - sizeof(pk::header));
		h.header_crc = pk::crc32(0, &h, offsetof(pk::header, header_crc));
		std::memcpy(b.data(), &h, sizeof(h));
		write_all(folder + L"pack.idx", b);
	}
	bool parse_folder(const std::wstring &folder, pk::index &x)
	{
		const std::vector<uint8_t> b = read_all(folder + L"pack.idx");
		std::string why;
		return pk::parse(b.data(), b.size(), x, why);
	}

	// Level 0 of a w x h BC2 texture: random blocks (any 16 bytes are a valid BC2 block)
	std::vector<uint8_t> random_bc2(uint32_t w, uint32_t h, uint32_t seed)
	{
		std::mt19937 rng(seed);
		std::vector<uint8_t> b(size_t(w / 4) * (h / 4) * 16);
		for (uint8_t &v : b)
			v = uint8_t(rng());
		return b;
	}

	struct made
	{
		pk::entry e;
		pk::map_in map;
	};
	// An entry for a texture the game has at w/f x h/f, its map w x h (all levels, plain box)
	made make(uint32_t w, uint32_t h, uint32_t seed, uint32_t f = 2)
	{
		made m;
		m.e.hash = 0x9E3779B97F4A7C15ull * (seed + 1);
		m.e.bytes = 40 + (w / f) * (h / f);
		m.e.w = w / f;
		m.e.h = h / f;
		m.e.orig_format = 0xA1;
		std::memcpy(&m.e.orig_fourcc, "3TXD", 4);
		m.e.alpha_kind = pk::kAlphaBlended;
		m.e.source_kind = pk::kSourceModel;
		m.e.file_id = 1000 + seed;
		m.e.chunk = seed % 7;
		std::snprintf(m.e.name, sizeof(m.e.name), "test%011u", seed);
		m.map.w = w;
		m.map.h = h;
		const std::vector<uint8_t> l0 = random_bc2(w, h, seed);
		m.map.levels = pk::bc2::full_chain(l0.data(), w, h, pk::kAlphaBlended, 0.0f);
		return m;
	}

	pk::writer_options options(const std::wstring &folder, uint64_t max_file = pk::kMaxFileBytes)
	{
		pk::writer_options o;
		o.folder = folder;
		o.maker_version = 7;
		o.source = "pack_test";
		o.client = { 1000, 2000, 3000, 4000 };
		o.max_file_bytes = max_file;
		return o;
	}

	// A record's level i read back as rows of tight blocks against the map's own, its padding zeros
	bool level_matches(const std::vector<uint8_t> &got, const pk::layout &l, uint32_t i, const std::vector<uint8_t> &tight)
	{
		const pk::level_layout &lv = l.level[i];
		if (got.size() != l.level_span(i))
			return false;
		for (uint32_t row = 0; row < lv.rows; ++row)
		{
			const uint8_t *g = got.data() + size_t(row) * lv.row_pitch;
			if (std::memcmp(g, tight.data() + size_t(row) * lv.row_bytes, lv.row_bytes) != 0)
				return false;
			// (the pitch's padding, and after the last row the zeros up to the next level)
			const size_t pad_end = row + 1 < lv.rows ? lv.row_pitch : got.size() - size_t(row) * lv.row_pitch;
			for (size_t k = lv.row_bytes; k < pad_end; ++k)
				if (g[k] != 0)
					return false;
		}
		return true;
	}

	std::string run_oracle(const std::wstring &folder, int &code)
	{
		// (the folder without its slash: a backslash before the closing quote would escape it)
		const std::string cmd = "python -I \"" + narrow(root) + "tests\\pack_oracle.py\" \"" + narrow(folder.substr(0, folder.size() - 1)) + "\" 2>&1";
		std::string out;
		FILE *p = _popen(cmd.c_str(), "r");
		if (p == nullptr)
		{
			code = -1;
			return out;
		}
		char line[512];
		while (std::fgets(line, sizeof(line), p))
			out += line;
		code = _pclose(p);
		return out;
	}

	std::string last_line(const std::string &s)
	{
		size_t end = s.find_last_not_of("\r\n");
		if (end == std::string::npos)
			return "";
		size_t start = s.find_last_of('\n', end);
		return s.substr(start == std::string::npos ? 0 : start + 1, end + 1 - (start == std::string::npos ? 0 : start + 1));
	}

	ID3D12Device *warp_device()
	{
		IDXGIFactory4 *factory = nullptr;
		IDXGIAdapter1 *adapter = nullptr;
		ID3D12Device *device = nullptr;
		if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))))
			D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
		if (adapter != nullptr)
			adapter->Release();
		if (factory != nullptr)
			factory->Release();
		return device;
	}

	// A cut-out picture as the game paints them: thin strokes and blobs of alpha 0x80 (the game's opaque) on 0, a soft
	// edge here and there
	std::vector<uint8_t> foliage(uint32_t w, uint32_t h, uint32_t seed)
	{
		std::mt19937 rng(seed);
		std::vector<uint8_t> px(size_t(w) * h * 4, 0);
		for (size_t i = 0; i < size_t(w) * h; ++i)
			px[i * 4] = uint8_t(rng()), px[i * 4 + 1] = uint8_t(rng()), px[i * 4 + 2] = uint8_t(rng());
		for (int s = 0; s < 220; ++s)
		{
			double x = rng() % w, y = rng() % h, a = (rng() % 628) / 100.0;
			const int len = 10 + rng() % 40, width = 1 + rng() % 2;
			for (int t = 0; t < len; ++t)
			{
				x += std::cos(a), y += std::sin(a);
				a += ((int(rng() % 21) - 10) / 100.0);
				for (int dy = 0; dy < width; ++dy)
					for (int dx = 0; dx < width; ++dx)
					{
						const uint32_t px_x = (uint32_t(x) + dx) % w, px_y = (uint32_t(y) + dy) % h;
						px[(size_t(px_y) * w + px_x) * 4 + 3] = (t % 9 == 0) ? 0x60 : 0x80;
					}
			}
		}
		return px;
	}
}

int main()
{
	{
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring e = exe;
		e = e.substr(0, e.find_last_of(L'\\'));  // build
		root = e.substr(0, e.find_last_of(L'\\') + 1);
		base = e + L"\\test\\pack_test\\";
		remove_tree(base);
		CreateDirectoryW((e + L"\\test").c_str(), nullptr);
		CreateDirectoryW(base.c_str(), nullptr);
	}

	// ---- CRC-32
	{
		const char *nine = "123456789";
		check(pk::crc32(0, nine, 9) == 0xCBF43926u, "crc32(\"123456789\") is zlib's check value CBF43926");
		check(pk::crc32(pk::crc32(0, nine, 4), nine + 4, 5) == 0xCBF43926u && pk::crc32(0, nine, 0) == 0, "crc32 continued over two parts; of nothing 0");
		std::vector<uint8_t> big(100003);
		for (size_t i = 0; i < big.size(); ++i)
			big[i] = uint8_t(i * 7 + (i >> 9));
		uint32_t slow = 0xFFFFFFFFu;
		for (uint8_t b : big)
		{
			slow ^= b;
			for (int k = 0; k < 8; ++k)
				slow = (slow & 1) ? 0xEDB88320u ^ (slow >> 1) : slow >> 1;
		}
		check(pk::crc32(0, big.data(), big.size()) == ~slow, "crc32 (eight bytes a step) == the bitwise CRC over 100,003 bytes");
	}

	// ---- The copy layout against WARP, and the rules
	{
		ID3D12Device *device = warp_device();
		if (device == nullptr)
			check(false, "a WARP device for GetCopyableFootprints");
		else
		{
			const uint32_t formats[] = { pk::kBC2, pk::kBC7, pk::kBC4 };
			const uint32_t sizes[][2] = { { 4, 4 }, { 8, 4 }, { 12, 20 }, { 36, 8 }, { 60, 64 }, { 64, 64 }, { 100, 36 }, { 128, 64 }, { 260, 132 },
				{ 1024, 512 }, { 1028, 4 }, { 4096, 4096 }, { 4, 16384 }, { 16384, 8 } };
			int combos = 0, same = 0;
			std::string first_bad;
			for (uint32_t f : formats)
				for (const auto &s : sizes)
					for (uint32_t levels = 1; levels <= pk::full_levels(s[0], s[1]); ++levels)
					{
						pk::layout l;
						pk::footprint(f, s[0], s[1], levels, l);
						pk::record r;
						r.kind = pk::kColour;
						r.format = f;
						r.w = uint16_t(s[0]);
						r.h = uint16_t(s[1]);
						r.levels = uint8_t(levels);
						r.bytes = uint32_t(l.total);
						const char *bad = pk::check(r, device);
						combos++;
						if (bad == nullptr)
							same++;
						else if (first_bad.empty())
							first_bad = std::to_string(f) + " " + std::to_string(s[0]) + "x" + std::to_string(s[1]) + " levels " + std::to_string(levels) + ": " + bad;
					}
			check(same == combos, "footprint == GetCopyableFootprints on WARP (offsets, pitches, rows, total): " + std::to_string(same) + " of " +
				std::to_string(combos) + " (BC2/BC7/BC4, 14 sizes, every level count)" + (first_bad.empty() ? "" : "; first: " + first_bad));
			pk::layout l36;
			pk::footprint(pk::kBC2, 36, 20, 1, l36);
			check(l36.level[0].row_bytes == 144 && l36.level[0].row_pitch == 256 && l36.total == 256 * 4 + 144,
				"a 36-wide BC2 level: 144 bytes a row, 256 apart, total 1168 (no padding after the last row)");
			device->Release();
		}
		pk::record good;
		good.kind = pk::kColour;
		good.format = pk::kBC2;
		good.w = 64;
		good.h = 32;
		good.levels = 3;
		pk::layout l;
		pk::footprint(pk::kBC2, 64, 32, 3, l);
		good.bytes = uint32_t(l.total);
		struct bad_case
		{
			const char *what;
			void (*change)(pk::record &);
		};
		const bad_case cases[] = {
			{ "BC2 SRGB", [](pk::record &r) { r.format = 75; } },
			{ "BC1", [](pk::record &r) { r.format = 71; } },
			{ "RGBA8", [](pk::record &r) { r.format = 28; } },
			{ "width 66", [](pk::record &r) { r.w = 66; } },
			{ "height 30", [](pk::record &r) { r.h = 30; } },
			{ "width 16388", [](pk::record &r) { r.w = 16388; } },
			{ "8 levels of 64x32", [](pk::record &r) { r.levels = 8; } },
			{ "0 levels", [](pk::record &r) { r.levels = 0; } },
			{ "offset 100", [](pk::record &r) { r.offset = 100; } },
			{ "bytes as tight rows (wrong pitch)", [](pk::record &r) { r.bytes = 16 * 8 * 16 + 8 * 4 * 16 + 4 * 2 * 16; } },
			{ "bytes 512 more", [](pk::record &r) { r.bytes += 512; } },
			{ "kind 9", [](pk::record &r) { r.kind = 9; } },
		};
		bool all = pk::check(good) == nullptr;
		std::string missed;
		for (const bad_case &c : cases)
		{
			pk::record r = good;
			c.change(r);
			if (pk::check(r) == nullptr)
				all = false, missed += std::string(" ") + c.what;
		}
		check(all, "the rules: 64x32 BC2 with 3 levels passes; refused: SRGB, BC1, RGBA8, sides not multiples of 4 or over 16384, levels past the chain or 0, offset not 512, bytes off the layout, unknown kind" + missed);
		const pk::layout *p = &l;
		check(pk::full_levels(64, 32) == 7 && pk::full_levels(4, 4) == 3 && pk::full_levels(16384, 16384) == 15 && p->levels == 3,
			"full_levels: 64x32 7, 4x4 3, 16384 15");
	}

	// ---- Mip chains
	{
		check(pk::bc2::chain_levels(128, 64) == 5 && pk::bc2::chain_levels(36, 20) == 1 && pk::bc2::chain_levels(4, 4) == 1 &&
				  pk::bc2::chain_levels(6, 4) == 0 && pk::bc2::chain_levels(4096, 4096) == 11 && pk::bc2::chain_levels(24, 8) == 2,
			"chain_levels: 128x64 5 (to 8x4), 36x20 1, 4x4 1, 6x4 0, 4096 11, 24x8 2");
		check(pk::bc2::zone_cut_threshold(0x80) > 47.8f && pk::bc2::zone_cut_threshold(0x80) < 47.9f && pk::bc2::zone_cut_threshold(0xFF) == 24.0f &&
				  pk::bc2::zone_cut_threshold(0) == pk::bc2::zone_cut_threshold(0x80),
			"zone threshold 96*255/(4*vcol.a): 47.8 at 0x80, 24 at 0xFF, 0 taken as 0x80");
		const std::vector<uint8_t> l0 = random_bc2(256, 128, 77);
		bool box_same = true;
		for (uint8_t kind : { pk::kAlphaNone, pk::kAlphaBlended, pk::kAlphaMask, pk::kAlphaMixed })
		{
			const auto chain = pk::bc2::full_chain(l0.data(), 256, 128, kind, 34.0f);
			const auto box = pk::bc2::smaller_levels(l0, 256, 128, true, pk::bc2::chain_levels(256, 128));
			box_same = box_same && chain.size() == box.size() + 1 && chain[0] == l0;
			for (size_t i = 0; box_same && i < box.size(); ++i)
				box_same = chain[i + 1] == box[i];
		}
		check(box_same, "blended, mask, mixed and none: level 0 unchanged, the rest byte-identical to smaller_levels' 2x2 box (6 levels of 256x128)");

		struct cut_case
		{
			const char *what;
			float threshold;
		};
		const cut_case cuts[] = { { "zone, vertex alpha 0x80", pk::bc2::zone_cut_threshold(0x80) },
			{ "zone, vertex alpha 0xFF", pk::bc2::zone_cut_threshold(0xFF) }, { "characters, > 0x22", pk::bc2::kCharsCutThreshold } };
		for (const cut_case &c : cuts)
		{
			const std::vector<uint8_t> px = foliage(512, 256, 5);
			const std::vector<uint8_t> blocks = pk::bc2::encode_bc2(px.data(), 512, 256);
			const auto chain = pk::bc2::full_chain(blocks.data(), 512, 256, pk::kAlphaCut, c.threshold);
			const auto plain = pk::bc2::full_chain(blocks.data(), 512, 256, pk::kAlphaBlended, c.threshold);
			const std::vector<uint8_t> d0 = pk::bc2::decode_bc2(blocks.data(), 512, 256);
			const double c0 = pk::bc2::coverage(d0.data(), 512, 256, c.threshold);
			double worst = 0.0, plain_worst = 0.0;
			std::string levels;
			bool colour_same = true;
			uint32_t w = 512, h = 256;
			for (size_t i = 1; i < chain.size(); ++i)
			{
				w /= 2, h /= 2;
				const std::vector<uint8_t> d = pk::bc2::decode_bc2(chain[i].data(), w, h);
				const std::vector<uint8_t> dp = pk::bc2::decode_bc2(plain[i].data(), w, h);
				const double ci = pk::bc2::coverage(d.data(), w, h, c.threshold), cp = pk::bc2::coverage(dp.data(), w, h, c.threshold);
				char s[64];
				std::snprintf(s, sizeof(s), " %.3f/%.3f", ci, cp);
				levels += s;
				if (size_t(w) * h >= 256)
				{
					worst = std::max(worst, std::fabs(ci - c0));
					plain_worst = std::max(plain_worst, std::fabs(cp - c0));
				}
				// (the colour isn't touched: the same as the plain box's, block colours and indices)
				for (size_t b = 0; colour_same && b < chain[i].size(); b += 16)
					colour_same = std::memcmp(chain[i].data() + b + 8, plain[i].data() + b + 8, 8) == 0;
			}
			char head[160];
			std::snprintf(head, sizeof(head), "cut-out (%s): coverage within 2%% of level 0's %.3f in levels of 256 texels and more (worst %.4f; the plain box's %.4f); per level cut/plain:",
				c.what, c0, worst, plain_worst);
			check(worst <= 0.02 && plain_worst > 0.02 && colour_same, head + levels);
		}
	}

	// ---- Round trip, hit and miss, non-64 widths
	const std::wstring a = base + L"a\\";
	std::vector<made> made_a;
	{
		const uint32_t sizes[][2] = { { 256, 256 }, { 36, 20 }, { 12, 12 }, { 100, 36 }, { 128, 64 }, { 4, 4 }, { 1024, 512 }, { 260, 132 }, { 64, 64 },
			{ 512, 2048 }, { 24, 8 }, { 8, 1024 }, { 64, 64 } };
		pk::writer w;
		std::string why;
		check(w.open(options(a), why), "writer opens a new folder " + why);
		uint32_t seed = 1;
		bool all_ok = true;
		for (const auto &s : sizes)
		{
			made m = make(s[0], s[1], seed++);
			all_ok = w.add(m.e, { m.map }, why) == pk::add_result::ok && all_ok;
			made_a.push_back(std::move(m));
		}
		w.finished("ROM\\1\\2.DAT", 1234, 5678);
		w.finished("ROM\\1\\3.DAT", 99, 100);
		check(all_ok && w.commit(why), "13 entries added (36, 12, 100, 260, 24 and 8 wide among them) and committed " + why);
		w.close();

		auto x = pk::load(a, nullptr, why);
		check(x && x->entries.size() == 13 && x->head.file_count == 1 && x->status.empty(), "loaded: 13 entries in 1 data file " + why);
		if (x)
		{
			pk::reader r(x);
			int found = 0, whole = 0, levels_ok = 0, levels_all = 0;
			bool fields = true;
			for (const made &m : made_a)
			{
				const pk::entry *e = x->find(m.e.the_key());
				if (e == nullptr)
					continue;
				found++;
				fields = fields && e->file_id == m.e.file_id && e->chunk == m.e.chunk && std::memcmp(e->name, m.e.name, 16) == 0 &&
					e->alpha_kind == m.e.alpha_kind && e->orig_fourcc == m.e.orig_fourcc && e->orig_format == 0xA1 && e->maps[1].kind == pk::kUnused &&
					e->maps[2].kind == pk::kUnused;
				const pk::record *rec = e->map(pk::kColour);
				pk::layout l;
				pk::footprint(rec->format, rec->w, rec->h, rec->levels, l);
				std::vector<uint8_t> all(rec->bytes);
				if (r.read_record(*rec, all.data(), why) && rec->levels == m.map.levels.size())
				{
					bool same = true;
					for (uint32_t i = 0; i < l.levels; ++i)
						same = same && level_matches(std::vector<uint8_t>(all.begin() + l.level[i].offset, all.begin() + l.level[i].offset + size_t(l.level_span(i))), l, i, m.map.levels[i]);
					whole += same;
				}
				for (uint32_t i = 0; i < rec->levels; ++i)
				{
					std::vector<uint8_t> one(size_t(l.level_span(i)));
					levels_all++;
					levels_ok += r.read_level(*rec, i, one.data(), why) && level_matches(one, l, i, m.map.levels[i]);
				}
			}
			check(found == 13 && fields, "every key found; its diagnostics (file id, chunk, name) and flags as written, the unused maps zero");
			check(whole == 13, "read_record: all 13 back, each level's rows where the layout puts them, the padding zeros (" + std::to_string(whole) + ")");
			check(levels_ok == levels_all, "read_level: " + std::to_string(levels_ok) + " of " + std::to_string(levels_all) + " levels back, each against its level CRC");
			const pk::key k = made_a[3].e.the_key();
			pk::key other_hash = k, other_bytes = k, other_w = k, other_h = k;
			other_hash.hash ^= 1;
			other_bytes.bytes += 16;
			other_w.w *= 2;
			other_h.h += 4;
			check(x->find(k) != nullptr && x->find(other_hash) == nullptr && x->find(other_bytes) == nullptr && x->find(other_w) == nullptr &&
					  x->find(other_h) == nullptr && x->find(pk::key {}) == nullptr,
				"hit; miss with another hash, another size, another width, another height, the empty key");

			r.cancel();
			const pk::record *rec0 = x->find(made_a[0].e.the_key())->map(pk::kColour);
			std::vector<uint8_t> buf(rec0->bytes);
			const bool refused = !r.read_record(*rec0, buf.data(), why) && why == "cancelled";
			r.resume();
			check(refused && r.read_record(*rec0, buf.data(), why), "cancel: reads fail (\"cancelled\") until resume, then read again");
		}
		int code = 0;
		const std::string out = run_oracle(a, code);
		check(code == 0 && out.find("entries 13") != std::string::npos, "pack_oracle.py reads it: " + last_line(out));
		std::string why2;
		const pk::fingerprint same_client { 1000, 2000, 3000, 4000 }, other_client { 1000, 2001, 3000, 4000 };
		auto y = pk::load(a, &same_client, why2);
		auto z = pk::load(a, &other_client, why2);
		check(y && y->status.empty() && z && !z->status.empty(), "fingerprint: the same game files no status; another FTABLE time a status line (the pack still used): " + (z ? z->status : why2));
	}

	// ---- An index or data file that can't be used: nothing of it is
	{
		std::string why;
		auto refused = [&](const std::wstring &folder, const char *want) {
			auto x = pk::load(folder, nullptr, why);
			return !x && why.find(want) != std::string::npos;
		};
		{
			const std::wstring f = copy_pack(a, L"cut_idx");
			std::vector<uint8_t> b = read_all(f + L"pack.idx");
			b.pop_back();
			write_all(f + L"pack.idx", b);
			const bool short_one = refused(f, "cut short");
			b.resize(100);
			write_all(f + L"pack.idx", b);
			const bool header_short = refused(f, "cut short");
			check(short_one && header_short, "pack.idx a byte short, or shorter than a header: refused (" + why + ")");
		}
		{
			const std::wstring f = copy_pack(a, L"v2");
			std::vector<uint8_t> b = read_all(f + L"pack.idx");
			pk::header h;
			std::memcpy(&h, b.data(), sizeof(h));
			h.version = 2;
			h.header_crc = pk::crc32(0, &h, offsetof(pk::header, header_crc));
			std::memcpy(b.data(), &h, sizeof(h));
			write_all(f + L"pack.idx", b);
			const bool v2 = refused(f, "version 2");
			check(v2, "a version 2 header (its CRC right): refused (" + why + ")");
			h.version = 1;
			h.flags = 3;
			h.header_crc = pk::crc32(0, &h, offsetof(pk::header, header_crc));
			std::memcpy(b.data(), &h, sizeof(h));
			write_all(f + L"pack.idx", b);
			const bool span = refused(f, "whole texture chunk");
			check(span, "keys over another span (flags 3): refused (" + why + ")");
		}
		{
			const std::wstring f = copy_pack(a, L"damaged_idx");
			std::vector<uint8_t> b = read_all(f + L"pack.idx");
			b[sizeof(pk::header) + 8 + 40] ^= 0x20;
			write_all(f + L"pack.idx", b);
			const bool table = refused(f, "tables are damaged");
			b[sizeof(pk::header) + 8 + 40] ^= 0x20;
			b[20] ^= 1;
			write_all(f + L"pack.idx", b);
			check(table && refused(f, "header is damaged"), "a byte changed in an entry's name or in the header: refused (CRC)");
		}
		{
			const std::wstring f = copy_pack(a, L"unsorted");
			pk::index x;
			parse_folder(f, x);
			std::swap(x.entries[2], x.entries[7]);
			rewrite_index(f, x);
			const bool unsorted = refused(f, "sorted");
			check(unsorted, "entries out of key order (CRCs right): refused (" + why + ")");
		}
		{
			const std::wstring f = copy_pack(a, L"past_end");
			pk::index x;
			parse_folder(f, x);
			x.entries[4].maps[0].offset = uint32_t(x.file_bytes[0]);
			rewrite_index(f, x);
			const bool past = refused(f, "past the end");
			x.entries[4].maps[0].offset = 0;
			x.entries[4].maps[0].level_crc = uint32_t(x.level_crcs.size());
			rewrite_index(f, x);
			check(past && refused(f, "level CRCs"), "a map past its data file's size, or its level CRCs past the table: refused");
		}
		{
			// (the index's own size for the data file past 1 GiB: parse refuses the record before any data file is looked at)
			const std::wstring f = copy_pack(a, L"past_gib");
			pk::index x;
			parse_folder(f, x);
			x.file_bytes[0] = 3ull << 30;
			pk::entry *big = &x.entries[0];
			for (pk::entry &e : x.entries)
				if (e.maps[0].bytes > big->maps[0].bytes)
					big = &e;
			big->maps[0].offset = (1u << 30) - 512;
			rewrite_index(f, x);
			const bool past_gib = refused(f, "1 GiB");
			int code = 0;
			const std::string out = run_oracle(f, code);
			check(past_gib && code != 0 && out.find("1 GiB") != std::string::npos, "a map ending past a data file's 1 GiB: refused, by pack_oracle.py too (" + why + ")");
		}
		{
			const std::wstring f = copy_pack(a, L"cut_data");
			std::vector<uint8_t> b = read_all(f + L"pack-000.bin");
			b.pop_back();
			write_all(f + L"pack-000.bin", b);
			const bool cut = refused(f, "shorter");
			DeleteFileW((f + L"pack-000.bin").c_str());
			const bool missing = refused(f, "missing");
			check(cut && missing, "pack-000.bin a byte short, or missing: refused, every key misses (" + why + ")");
		}
		{
			// (a record whose stored bytes are tight rows: the index is sound, the record breaks the rules)
			const std::wstring f = copy_pack(a, L"wrong_pitch");
			pk::index x;
			parse_folder(f, x);
			pk::entry *e = nullptr;
			for (pk::entry &c : x.entries)
				if (c.maps[0].w == 36)
					e = &c;
			e->maps[0].bytes = 36 / 4 * 16 * (20 / 4);
			rewrite_index(f, x);
			auto y = pk::load(f, nullptr, why);
			pk::reader r(y);
			std::vector<uint8_t> buf(65536);
			const pk::entry *found = y ? y->find(e->the_key()) : nullptr;
			const bool miss = found && !r.read_record(found->maps[0], buf.data(), why) && !r.read_level(found->maps[0], 0, buf.data(), why);
			check(miss, "a 36-wide record stored as tight rows (wrong pitch): loads, but reading it misses (" + why + ")");
		}
		{
			// (the two 64x64 records: their bytes swapped in the data file)
			const std::wstring f = copy_pack(a, L"swapped");
			pk::index x;
			parse_folder(f, x);
			const pk::record r1 = *x.find(made_a[8].e.the_key())->map(pk::kColour);
			const pk::record r2 = *x.find(made_a[12].e.the_key())->map(pk::kColour);
			std::vector<uint8_t> b = read_all(f + L"pack-000.bin");
			std::swap_ranges(b.begin() + r1.offset, b.begin() + r1.offset + r1.bytes, b.begin() + r2.offset);
			write_all(f + L"pack-000.bin", b);
			auto y = pk::load(f, nullptr, why);
			pk::reader r(y);
			std::vector<uint8_t> buf(r1.bytes);
			const bool both = y && r1.bytes == r2.bytes && r1.crc != r2.crc && !r.read_record(r1, buf.data(), why) && !r.read_record(r2, buf.data(), why) &&
				!r.read_level(r1, 0, buf.data(), why);
			check(both, "two 64x64 records' bytes swapped in pack-000.bin: the index loads, both miss (" + why + ")");
			int code = 0;
			const std::string out = run_oracle(f, code);
			check(code != 0, "pack_oracle.py refuses it: " + last_line(out));
		}
		{
			const std::wstring f = copy_pack(a, L"damaged_data");
			pk::index x;
			parse_folder(f, x);
			const pk::record rec = *x.find(made_a[0].e.the_key())->map(pk::kColour);
			pk::layout l;
			pk::footprint(rec.format, rec.w, rec.h, rec.levels, l);
			std::vector<uint8_t> b = read_all(f + L"pack-000.bin");
			b[rec.offset + l.level[1].offset + 5] ^= 0x40;
			write_all(f + L"pack-000.bin", b);
			auto y = pk::load(f, nullptr, why);
			pk::reader r(y);
			std::vector<uint8_t> buf(rec.bytes);
			const bool whole = !r.read_record(rec, buf.data(), why) && why == "CRC mismatch";
			const bool level1 = !r.read_level(rec, 1, buf.data(), why) && why == "CRC mismatch";
			const bool others = r.read_level(rec, 0, buf.data(), why) && r.read_level(rec, 2, buf.data(), why);
			check(whole && level1 && others, "a byte damaged in a 256x256's level 1: the record and level 1 miss (CRC), levels 0 and 2 still read");
		}
		{
			const std::wstring f = copy_pack(a, L"cut_after_load");
			auto y = pk::load(f, nullptr, why);
			pk::reader r(y);
			const pk::record *last = nullptr;
			for (const pk::entry &e : y->entries)
				if (last == nullptr || e.maps[0].offset > last->offset)
					last = &e.maps[0];
			std::vector<uint8_t> buf(last->bytes);
			const bool before = r.read_record(*last, buf.data(), why);
			// (shortened while the reader has it open: its share mode lets others write)
			HANDLE t = CreateFileW((f + L"pack-000.bin").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
			LARGE_INTEGER size = {};
			const bool cut = t != INVALID_HANDLE_VALUE && GetFileSizeEx(t, &size) && (size.QuadPart -= 100, SetFilePointerEx(t, size, nullptr, FILE_BEGIN)) && SetEndOfFile(t);
			if (t != INVALID_HANDLE_VALUE)
				CloseHandle(t);
			const bool after = !r.read_record(*last, buf.data(), why) && why.find("cut short") != std::string::npos;
			check(before && cut && after, "pack-000.bin cut short after loading, while the reader has it open: the last record misses (" + why + ")");
		}
	}

	// ---- Roll-over
	{
		const std::wstring f = base + L"rollover\\";
		pk::writer w;
		std::string why;
		w.open(options(f, 256 << 10), why);
		std::vector<made> ms;
		bool ok = true;
		for (uint32_t i = 0; i < 24; ++i)
		{
			ms.push_back(make(256, 128 + 64 * (i % 3), 100 + i));
			ok = w.add(ms.back().e, { ms.back().map }, why) == pk::add_result::ok && ok;
		}
		made big = make(1024, 512, 199);
		const bool too_big = w.add(big.e, { big.map }, why) == pk::add_result::refused;
		ok = w.commit(why) && ok;
		w.close();
		auto x = pk::load(f, nullptr, why);
		bool inside = x != nullptr, back = x != nullptr;
		uint32_t used = 0;
		if (x)
		{
			pk::reader r(x);
			std::vector<bool> file_used(x->head.file_count);
			for (const made &m : ms)
			{
				const pk::entry *e = x->find(m.e.the_key());
				const pk::record *rec = e ? e->map(pk::kColour) : nullptr;
				if (rec == nullptr)
				{
					back = false;
					continue;
				}
				inside = inside && uint64_t(rec->offset) + rec->bytes <= (256u << 10) && rec->offset % 512 == 0;
				file_used[rec->file] = true;
				std::vector<uint8_t> buf(rec->bytes);
				back = back && r.read_record(*rec, buf.data(), why);
			}
			for (bool u : file_used)
				used += u;
		}
		check(ok && x && x->head.file_count >= 6 && used == x->head.file_count && inside && back,
			"roll-over at a 256 KB test limit: 24 entries in " + std::to_string(x ? x->head.file_count : 0) + " data files, every record inside the limit at a multiple of 512, all read back");
		check(too_big && x && x->find(big.e.the_key()) == nullptr, "a map bigger than a data file: refused (" + why + ")");
		int code = 0;
		const std::string out = run_oracle(f, code);
		check(code == 0 && out.find("entries 24") != std::string::npos, "pack_oracle.py reads it: " + last_line(out));
	}

	// ---- Roll-over into a data file an interrupted run left bytes in (past the index): no room there either, the next
	{
		const std::wstring f = base + L"rollover_left\\";
		std::string why;
		pk::writer w;
		bool ok = w.open(options(f, 256 << 10), why);
		made first = make(256, 256, 400);
		ok = ok && w.add(first.e, { first.map }, why) == pk::add_result::ok && w.commit(why);
		w.close();
		// (pack-001.bin: 200 KB no entry names)
		write_all(f + L"pack-001.bin", std::vector<uint8_t>(200 << 10, 0xAB));
		pk::writer w2;
		ok = ok && w2.open(options(f, 256 << 10), why);
		std::vector<made> ms;
		for (uint32_t i = 0; i < 4; ++i)
		{
			ms.push_back(make(256, 256, 410 + i));
			ok = w2.add(ms.back().e, { ms.back().map }, why) == pk::add_result::ok && ok;
		}
		ok = w2.commit(why) && ok;
		w2.close();
		auto x = pk::load(f, nullptr, why);
		bool inside = x != nullptr, none_in_1 = x != nullptr;
		if (x)
			for (const pk::entry &e : x->entries)
			{
				inside = inside && uint64_t(e.maps[0].offset) + e.maps[0].bytes <= (256u << 10);
				none_in_1 = none_in_1 && e.maps[0].file != 1;
			}
		int code = 0;
		const std::string out = run_oracle(f, code);
		check(ok && x && x->entries.size() == 5 && inside && none_in_1 && code == 0,
			"roll-over into pack-001.bin holding 200 KB of an interrupted run's: no 87 KB record fits, so pack-002.bin; every record inside the 256 KB limit (" +
				last_line(out) + ")");
	}

	// ---- One writer a folder (pack.lock)
	{
		const std::wstring f = base + L"locked\\";
		std::string why, why2, why3;
		pk::writer w;
		const bool opened = w.open(options(f), why);
		pk::writer second;
		const bool refused = !second.open(options(f), why2) && why2.find("another texpack is writing this pack") != std::string::npos;
		pk::folder_lock other;
		const bool lock_refused = !other.take(f, why3);
		const bool there = GetFileAttributesW((f + L"pack.lock").c_str()) != INVALID_FILE_ATTRIBUTES;
		w.close();
		const bool gone = GetFileAttributesW((f + L"pack.lock").c_str()) == INVALID_FILE_ATTRIBUTES;
		const bool after = second.open(options(f), why2);
		second.close();
		check(opened && refused && lock_refused && there && gone && after,
			"pack.lock: a second writer and a folder_lock refused while a writer holds the folder (" + why2 + "); gone at close, then a writer opens it");
	}

	// ---- Dedupe
	{
		const std::wstring f = base + L"dedupe\\";
		pk::writer w;
		std::string why;
		w.open(options(f), why);
		made m1 = make(64, 64, 500), m3 = make(64, 64, 501);
		m3.e.hash = m1.e.hash;
		const pk::add_result first = w.add(m1.e, { m1.map }, why), again = w.add(m1.e, { m1.map }, why), other = w.add(m3.e, { m3.map }, why);
		w.commit(why);
		w.close();
		auto x = pk::load(f, nullptr, why);
		bool kept_first = false;
		if (x && x->entries.size() == 1)
		{
			pk::reader r(x);
			const pk::record &rec = x->entries[0].maps[0];
			pk::layout l;
			pk::footprint(rec.format, rec.w, rec.h, rec.levels, l);
			std::vector<uint8_t> one(size_t(l.level_span(0)));
			kept_first = r.read_level(rec, 0, one.data(), why) && level_matches(one, l, 0, m1.map.levels[0]);
		}
		check(first == pk::add_result::ok && again == pk::add_result::same && other == pk::add_result::differs && w.stats().added == 1 &&
				  w.stats().same == 1 && w.stats().differs == 1 && kept_first,
			"dedupe: a key added again with the same bytes (same) and with other bytes (differs) isn't written; one entry, the first one's bytes");

		// (told to replace (the same source DAT converted again): the other bytes take the key; the same again is same)
		pk::writer rw;
		const bool reopened = rw.open(options(f), why);
		const pk::add_result replaced = rw.add(m3.e, { m3.map }, why, true), again_same = rw.add(m3.e, { m3.map }, why, true);
		const bool rw_committed = rw.commit(why);
		const uint32_t rw_replaced = rw.stats().replaced;
		rw.close();
		auto y = pk::load(f, nullptr, why);
		bool now_m3 = false;
		if (y && y->entries.size() == 1)
		{
			pk::reader r(y);
			const pk::record &rec = y->entries[0].maps[0];
			pk::layout l;
			pk::footprint(rec.format, rec.w, rec.h, rec.levels, l);
			std::vector<uint8_t> one(size_t(l.level_span(0)));
			now_m3 = r.read_level(rec, 0, one.data(), why) && level_matches(one, l, 0, m3.map.levels[0]);
		}
		check(reopened && replaced == pk::add_result::replaced && again_same == pk::add_result::same && rw_committed && rw_replaced == 1 && now_m3,
			"replace: a key added with other bytes and replace takes the key (one entry, the new bytes); the same bytes again: same");

		// (prune's drop: out of the next index, its bytes left)
		pk::writer p;
		const bool dropped = p.open(options(f), why) && !p.drop(pk::key { 1, 2, 3, 4 }) &&
			p.drop(m1.e.the_key()) && p.commit(why);
		p.close();
		uint64_t data_size = 0;
		{
			pk::index d;
			parse_folder(f, d);
			data_size = d.file_bytes.empty() ? 0 : d.file_bytes[0];
		}
		auto after = pk::load(f, nullptr, why);
		check(dropped && after && after->entries.empty() && data_size > 0, "drop: an unknown key not found; a dropped entry gone from the next index, the data file's bytes left (" +
			std::to_string(data_size) + ")");
	}

	// ---- Resume
	{
		const std::wstring f = copy_pack(a, L"resume");
		std::string why;
		pk::writer w;
		const bool opened = w.open(options(f), why);
		check(opened && w.stats().kept == 13 && w.done("ROM\\1\\2.DAT", 1234, 5678) && w.done("ROM\\1\\3.DAT", 99, 100) && !w.done("ROM\\1\\2.DAT", 1235, 5678) &&
				  !w.done("ROM\\1\\2.DAT", 1234, 5679) && !w.done("ROM\\1\\4.DAT", 1, 2),
			"resume: 13 entries kept; pack.src's two sources done at their size and mtime, not at another size or mtime, an unlisted one not");
		std::vector<made> more;
		for (uint32_t i = 0; i < 3; ++i)
		{
			more.push_back(make(128, 128, 300 + i));
			w.add(more.back().e, { more.back().map }, why);
		}
		w.finished("ROM\\1\\4.DAT", 1, 2);
		w.close(); // (an interrupted run: added and finished, never committed)
		uint64_t idx_size = 0;
		{
			pk::index x;
			parse_folder(f, x);
			idx_size = x.file_bytes[0];
		}
		const std::vector<uint8_t> data_now = read_all(f + L"pack-000.bin");
		pk::writer w2;
		w2.open(options(f), why);
		const bool crash_left = w2.stats().kept == 13 && !w2.done("ROM\\1\\4.DAT", 1, 2) && data_now.size() > idx_size;
		bool added = true;
		for (const made &m : more)
			added = w2.add(m.e, { m.map }, why) == pk::add_result::ok && added;
		w2.finished("ROM\\1\\4.DAT", 1, 2);
		added = w2.commit(why) && added;
		w2.close();
		auto x = pk::load(f, nullptr, why);
		bool back = x && x->entries.size() == 16;
		if (x)
		{
			pk::reader r(x);
			for (const pk::entry &e : x->entries)
			{
				std::vector<uint8_t> buf(e.maps[0].bytes);
				back = back && r.read_record(e.maps[0], buf.data(), why);
			}
			for (const made &m : more)
				back = back && x->find(m.e.the_key()) != nullptr && x->find(m.e.the_key())->maps[0].offset >= data_now.size();
		}
		pk::writer w3;
		w3.open(options(f), why);
		const bool done3 = w3.done("ROM\\1\\4.DAT", 1, 2);
		w3.close();
		pk::writer_options newer = options(f);
		newer.maker_version = 8;
		pk::writer w4;
		const bool open4 = w4.open(newer, why);
		check(crash_left && added && back && done3 && open4 && !w4.done("ROM\\1\\2.DAT", 1234, 5678),
			"an interrupted run: its entries and pack.src line absent, its bytes left; the rerun appends past them, 16 entries read back; then the source is done (not for a newer maker version)");
		w4.close();

		// (the overlay copy's size and time: another copy isn't done; forget() takes a line out, pack.src rewritten)
		pk::writer w5;
		w5.open(options(f), why);
		w5.finished("ROM\\1\\5.DAT", 10, 20, 30, 40);
		const bool committed = w5.commit(why);
		const bool overlay_done = w5.done("ROM\\1\\5.DAT", 10, 20, 30, 40) && !w5.done("ROM\\1\\5.DAT", 10, 20, 30, 41) && !w5.done("ROM\\1\\5.DAT", 10, 20, 31, 40) &&
			!w5.done("ROM\\1\\5.DAT", 10, 20);
		const bool forgot = w5.forget("ROM\\1\\2.DAT") && !w5.forget("ROM\\1\\9.DAT") && w5.commit(why);
		w5.close();
		const std::vector<uint8_t> src_bytes = read_all(f + L"pack.src");
		const std::string src(src_bytes.begin(), src_bytes.end());
		pk::writer w6;
		w6.open(options(f), why);
		check(committed && overlay_done && forgot && src.find("ROM\\1\\2.DAT") == std::string::npos && std::count(src.begin(), src.end(), '\n') == 3 &&
				w6.done("ROM\\1\\3.DAT", 99, 100) && w6.done("ROM\\1\\5.DAT", 10, 20, 30, 40) && !w6.done("ROM\\1\\2.DAT", 1234, 5678) && w6.stats().kept == 16,
			"pack.src: the overlay copy's size and time count for done; a line forget() took out is gone after the commit (pack.src rewritten, one line a source), "
			"the others kept, the entries untouched");
		w6.close();
		int code = 0;
		const std::string out = run_oracle(f, code);
		check(code == 0 && out.find("entries 16") != std::string::npos, "pack_oracle.py reads the resumed pack (unreferenced bytes in it): " + last_line(out));
	}

	// ---- pack.idx replaced while a reader reads; the folder renamed once nothing holds it
	{
		const std::wstring f = copy_pack(a, L"live");
		std::string why;
		auto old = pk::load(f, nullptr, why);
		pk::reader r(old);
		const pk::record rec = *old->find(made_a[1].e.the_key())->map(pk::kColour);
		std::vector<uint8_t> buf(rec.bytes);
		const bool before = r.read_record(rec, buf.data(), why);
		pk::writer w;
		made n = make(128, 64, 900);
		const bool wrote = w.open(options(f), why) && w.add(n.e, { n.map }, why) == pk::add_result::ok && w.commit(why);
		w.close();
		const bool still = r.read_record(rec, buf.data(), why);
		auto fresh = pk::load(f, nullptr, why);
		pk::reader r2(fresh);
		const pk::entry *ne = fresh ? fresh->find(n.e.the_key()) : nullptr;
		std::vector<uint8_t> nbuf(ne ? ne->maps[0].bytes : 0);
		const bool new_read = ne && r2.read_record(ne->maps[0], nbuf.data(), why);
		check(before && wrote && still && fresh && fresh->entries.size() == 14 && old->entries.size() == 13 && new_read,
			"a writer appends to pack-000.bin and replaces pack.idx while a reader has it open: the old snapshot reads on, a new load has the new entry and reads it");

		const std::wstring folder = f.substr(0, f.size() - 1), moved = base + L"live_moved";
		r.close();
		r2.close();
		const bool renamed_loaded = MoveFileW(folder.c_str(), moved.c_str()) && MoveFileW(moved.c_str(), folder.c_str());
		r.read_record(rec, buf.data(), why);
		const bool held = !MoveFileW(folder.c_str(), moved.c_str());
		if (!held)
			MoveFileW(moved.c_str(), folder.c_str());
		r.close();
		const bool renamed_closed = MoveFileW(folder.c_str(), moved.c_str());
		check(renamed_loaded && renamed_closed, "the pack folder renamed with snapshots loaded and the readers closed (pack.idx isn't held), and again after a read and close()");
		std::printf("--    while a reader held pack-000.bin open, renaming the folder %s\n", held ? "failed (close() is what frees it)" : "worked");
	}

	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
