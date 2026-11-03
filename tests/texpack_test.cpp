/*
 * hifixi-texpack (tools\texpack\texpack.cpp) run on a few of the game's DATs: a 2x zone (ROM\0\100), Giddeus at 4x
 * with its five B1 textures bakezone.py made A1 (ROM\0\104), a model DAT (ROM\310\0: four A1 DXT3, two B1 left, five
 * Os2), a model DAT whose one texture its Os2 shines with (ROM3\5\110, 'mashou') and a palette-only zone (ROM\0\28),
 * against the installed hifi-textures overlay and against hand-made overlays:
 *   - pairing: the pack's keys are exactly the upscaled chunks' original keys; every entry field from the ORIGINAL chunk
 *     (format byte, DXT fourcc, size, name, file id and chunk ordinal; also with the overlay's template header bytes
 *     overwritten); level 0 the overlay's BC2 blocks unchanged; B1 originals no_pack; the palette-only row skipped;
 *   - alpha kinds: cut-out mips within 2% of level 0's coverage, the others the plain box (smaller_levels' bytes); the
 *     model DATs' kinds from their Os2 (mask for the shine);
 *   - refusals: a renamed chunk skipped, an extra chunk refuses its DAT, a B1 replaced by a bigger A1 keyed by the B1,
 *     two upscales swapped left out by the soul test; a changed overlay copy converted again, its entry replaced;
 *   - resume: a stop file, --limit 1, then the rest: the same pack as in one run; a rerun does nothing; two zones that
 *     share a texture used otherwise in each (ROM\1\66, ROM\1\72): --limit 1 and the rest tag it as one run;
 *   - refusing to run: an Ashita log written just now; too little free disk for the estimate;
 *   - verify (and --soul), a damaged byte found, two entries' maps swapped failed by the soul test; stats --scan;
 *     prune against a changed game folder (refused while another writer holds the pack), then compact, then convert
 *     against the real one: every entry back;
 *   - tests\pack_oracle.py (python -I) reads every pack it writes.
 *
 *   build\texpack_test.exe     (64-bit, built by tools\build_texpack.cmd; needs build\hifixi-texpack.exe, python, the
 *                               game's files in C:\Games\PhoenixXI and the overlay; work folder build\test\texpack)
 *
 * Exit code: the number of failures.
 */

#include <windows.h>
#include "../src/pack/pack.hpp"
#include "../src/pack/pack_write.hpp"
#include "../src/pack/bc2.hpp"
#include "../src/pack/validate.hpp"
#include "../src/bake/dat.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
	namespace pk = hifi::pack;
	namespace bk = hifi::bake;

	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		std::fflush(stdout);
		if (!ok)
			failures++;
		else
			passes++;
	}

	const std::wstring kFFXI = L"C:\\Games\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI";
	const std::wstring kOverlay = L"C:\\Games\\PhoenixXI\\polplugins\\DATs\\hifi-textures";
	const char *kZone = "ROM\\0\\100.DAT", *kGiddeus = "ROM\\0\\104.DAT", *kModel = "ROM\\310\\0.DAT", *kPalette = "ROM\\0\\28.DAT", *kMask = "ROM3\\5\\110.DAT";
	// (a model DAT whose three textures no Os2 group of any DAT names: kind none)
	const char *kUntagged = "ROM3\\5\\31.DAT";
	// (two zones sharing three upscaled textures; one of them ROM\1\66's meshes don't use and ROM\1\72's blend)
	const char *kSharedA = "ROM\\1\\66.DAT", *kSharedB = "ROM\\1\\72.DAT";
	// (ROM\310\0's four upscaled textures by name and alpha kind, entries in key order, and how many upscaled textures
	// of the run no Os2 group names)
	const char *kModelKinds = "tim     axe_m01:4 tim     mori01:4 tim     mori03:1 tim     mori02:1 ", *kModelUntagged = "3";
	std::wstring root, work, tool;

	std::string narrow(const std::wstring &w)
	{
		std::string s;
		for (wchar_t c : w)
			s += char(c);
		return s;
	}
	std::wstring wide(const std::string &s) { return std::wstring(s.begin(), s.end()); }

	// The folder and everything in it deleted (only ever build\test\texpack's)
	void remove_tree(const std::wstring &folder)
	{
		WIN32_FIND_DATAW fd;
		HANDLE f = FindFirstFileW((folder + L"\\*").c_str(), &fd);
		if (f != INVALID_HANDLE_VALUE)
		{
			do
			{
				const std::wstring name = fd.cFileName;
				if (name == L"." || name == L"..")
					continue;
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					remove_tree(folder + L"\\" + name);
				else
					DeleteFileW((folder + L"\\" + name).c_str());
			} while (FindNextFileW(f, &fd));
			FindClose(f);
		}
		RemoveDirectoryW(folder.c_str());
	}

	void make_folders(const std::wstring &path)
	{
		for (size_t at = path.find_first_of(L"\\/", 3);; at = path.find_first_of(L"\\/", at + 1))
		{
			CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
			if (at == std::wstring::npos)
				break;
		}
	}

	bool write(const std::wstring &path, const std::vector<uint8_t> &bytes)
	{
		make_folders(path.substr(0, path.find_last_of(L'\\')));
		return bk::write_file(path, bytes.data(), bytes.size());
	}

	bool write_text(const std::wstring &path, const std::string &text) { return write(path, std::vector<uint8_t>(text.begin(), text.end())); }

	std::vector<uint8_t> read(const std::wstring &path)
	{
		std::vector<uint8_t> b;
		bk::read_file(path, b);
		return b;
	}

	// A command's output (stdout and stderr) and exit code
	std::string run(const std::string &args, int &code)
	{
		const std::string cmd = narrow(tool) + " " + args + " 2>&1";
		std::string out;
		FILE *p = _popen(cmd.c_str(), "r");
		if (p == nullptr)
		{
			code = -1;
			return out;
		}
		char line[2048];
		while (std::fgets(line, sizeof(line), p))
			out += line;
		code = _pclose(p);
		return out;
	}

	std::string oracle(const std::wstring &folder, int &code)
	{
		const std::string cmd = "python -I \"" + narrow(root) + "\\tests\\pack_oracle.py\" \"" + narrow(folder) + "\" 2>&1";
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
		const size_t end = s.find_last_not_of("\r\n");
		if (end == std::string::npos)
			return "";
		const size_t start = s.find_last_of('\n', end);
		return s.substr(start == std::string::npos ? 0 : start + 1, end + 1 - (start == std::string::npos ? 0 : start + 1));
	}

	// The output's first line holding what (for a check's message)
	std::string line_with(const std::string &s, const std::string &what)
	{
		const size_t at = s.find(what);
		if (at == std::string::npos)
			return "(no line with '" + what + "')";
		const size_t start = s.rfind('\n', at), end = s.find('\n', at);
		std::string l = s.substr(start == std::string::npos ? 0 : start + 1, end == std::string::npos ? std::string::npos : end - (start == std::string::npos ? 0 : start + 1));
		while (!l.empty() && (l.back() == '\r' || l.back() == '\n'))
			l.pop_back();
		return l;
	}

	std::string q(const std::wstring &path) { return "\"" + narrow(path) + "\""; }

	int32_t i32(const uint8_t *p)
	{
		int32_t v;
		std::memcpy(&v, p, 4);
		return v;
	}

	// One upscaled texture as an overlay copy has it, beside its original
	struct pair_info
	{
		std::string rel;
		uint32_t chunk = 0;
		pk::key k;
		uint8_t format = 0;
		uint32_t fourcc = 0;
		uint32_t vw = 0, vh = 0;
		std::vector<uint8_t> blocks; // (the overlay's BC2 blocks)
		std::string name;
	};

	// The upscaled textures of a DAT (an original and its overlay copy): texture chunks of other sizes, independently of
	// texpack's own pairing
	std::vector<pair_info> upscaled(const std::string &rel, const std::wstring &overlay)
	{
		std::vector<pair_info> out;
		const std::vector<uint8_t> a = read(kFFXI + L"\\" + wide(rel)), b = read(overlay + L"\\" + wide(rel));
		const std::vector<bk::chunk> ca = bk::chunks(a), cb = bk::chunks(b);
		for (size_t i = 0; i < ca.size() && i < cb.size(); ++i)
			if (ca[i].type == 0x20 && cb[i].type == 0x20 && ca[i].size != cb[i].size)
			{
				pair_info p;
				p.rel = rel;
				p.chunk = uint32_t(i);
				p.k = pk::key_of(ca[i].body, ca[i].size);
				p.format = ca[i].body[0];
				if (p.format == 0xA1)
					std::memcpy(&p.fourcc, ca[i].body + 57, 4);
				p.vw = uint32_t(i32(cb[i].body + 21));
				p.vh = uint32_t(i32(cb[i].body + 25));
				p.blocks.assign(cb[i].body + 69, cb[i].body + 69 + size_t(p.vw / 4) * (p.vh / 4) * 16);
				p.name.assign(reinterpret_cast<const char *>(ca[i].body + 1), 16);
				out.push_back(p);
			}
		return out;
	}

	// A DAT rebuilt from chunks (header kept, its size field set from the body)
	struct raw_chunk
	{
		uint8_t header[16];
		std::vector<uint8_t> body;
	};
	std::vector<raw_chunk> split_dat(const std::vector<uint8_t> &dat)
	{
		std::vector<raw_chunk> out;
		for (const bk::chunk &c : bk::chunks(dat))
		{
			raw_chunk r;
			std::memcpy(r.header, c.body - 16, 16);
			r.body.assign(c.body, c.body + c.size);
			out.push_back(std::move(r));
		}
		return out;
	}
	std::vector<uint8_t> join_dat(const std::vector<raw_chunk> &cs)
	{
		std::vector<uint8_t> out;
		for (raw_chunk c : cs)
		{
			c.body.resize((c.body.size() + 15) / 16 * 16, 0);
			uint32_t info;
			std::memcpy(&info, c.header + 4, 4);
			info = (info & 0x7F) | (uint32_t((c.body.size() + 16) / 16) << 7) | (info & 0xFC000000u);
			std::memcpy(c.header + 4, &info, 4);
			out.insert(out.end(), c.header, c.header + 16);
			out.insert(out.end(), c.body.begin(), c.body.end());
		}
		return out;
	}

	std::shared_ptr<const pk::index> load(const std::wstring &folder)
	{
		std::string why;
		return pk::load(folder + L"\\", nullptr, why);
	}

	// A record's levels as tight blocks
	std::vector<std::vector<uint8_t>> levels_of(const std::shared_ptr<const pk::index> &idx, const pk::record &r)
	{
		std::vector<std::vector<uint8_t>> out;
		pk::reader rd(idx);
		std::vector<uint8_t> buf(r.bytes);
		std::string why;
		pk::layout l;
		if (!rd.read_record(r, buf.data(), why) || !pk::footprint(r.format, r.w, r.h, r.levels, l))
			return out;
		for (uint32_t i = 0; i < l.levels; ++i)
		{
			std::vector<uint8_t> t(size_t(l.level[i].row_bytes) * l.level[i].rows);
			for (uint32_t row = 0; row < l.level[i].rows; ++row)
				std::memcpy(t.data() + size_t(row) * l.level[i].row_bytes, buf.data() + l.level[i].offset + size_t(row) * l.level[i].row_pitch, l.level[i].row_bytes);
			out.push_back(std::move(t));
		}
		return out;
	}

	std::string pack_src(const std::wstring &folder)
	{
		const std::vector<uint8_t> b = read(folder + L"\\pack.src");
		return std::string(b.begin(), b.end());
	}

	size_t count_lines(const std::string &s) { return size_t(std::count(s.begin(), s.end(), '\n')); }

	std::string size_of(const char *rel)
	{
		WIN32_FILE_ATTRIBUTE_DATA a = {};
		GetFileAttributesExW((kFFXI + L"\\" + wide(rel)).c_str(), GetFileExInfoStandard, &a);
		return std::to_string(a.nFileSizeLow);
	}

	// The lists texpack reads (dats.tsv and the bakeall logs) for these DATs, with the original sizes on disk (ROM3\5\110
	// in no log: the overlay walk finds it)
	void write_lists(const std::wstring &folder)
	{
		write_text(folder + L"\\dats.tsv", std::string(kZone) + "\tzone\t25\t1231872\t" + size_of(kZone) + "\t-\n" + kGiddeus + "\tzone\t42\t2273280\t" +
				size_of(kGiddeus) + "\t-\n" + kPalette + "\tzone\t25\t1187840\t" + size_of(kPalette) + "\t-\n" + kModel + "\tmodel\t6\t192512\t" + size_of(kModel) +
				"\t-\n" + kMask + "\tmodel\t1\t4096\t" + size_of(kMask) + "\t-\n" + kUntagged + "\tmodel\t3\t12288\t" +
				size_of(kUntagged) + "\t-\n");
		write_text(folder + L"\\bakeall-zone.log", std::string(kZone) + "\t14\t" + size_of(kZone) + "\t9878672\n" + kPalette + "\tpalette only, left\n");
		write_text(folder + L"\\bakeall-model.log", std::string(kModel) + "\t4\t" + size_of(kModel) + "\t4013600\n");
	}

	// The lists for the two zones that share textures
	void write_shared_lists(const std::wstring &folder)
	{
		write_text(folder + L"\\dats.tsv", std::string(kSharedA) + "\tzone\t10\t229376\t" + size_of(kSharedA) + "\t-\n" + kSharedB + "\tzone\t16\t237568\t" +
				size_of(kSharedB) + "\t-\n");
		write_text(folder + L"\\bakeall-zone.log", std::string(kSharedA) + "\t10\t" + size_of(kSharedA) + "\t1\n" + kSharedB + "\t16\t" + size_of(kSharedB) + "\t1\n");
		write_text(folder + L"\\bakeall-model.log", "");
	}

	// Entries the same in two packs: keys, kinds, vertex alpha, flags, sources, record CRCs and levels
	bool same_entries(const pk::index &x, const pk::index &y)
	{
		bool same = x.entries.size() == y.entries.size();
		for (size_t i = 0; same && i < x.entries.size(); ++i)
		{
			const pk::entry &a = x.entries[i], &b = y.entries[i];
			same = a.the_key() == b.the_key() && a.alpha_kind == b.alpha_kind && a.vcol_a == b.vcol_a && a.flags == b.flags && a.file_id == b.file_id &&
				a.chunk == b.chunk && a.maps[0].crc == b.maps[0].crc && a.maps[0].levels == b.maps[0].levels;
		}
		return same;
	}

	// A 16-byte name as texpack prints it (name16: unprintables as '?', trailing blanks and those off)
	std::string printed(const char *name)
	{
		std::string n(name, 16);
		for (char &c : n)
			if (uint8_t(c) < 32 || uint8_t(c) > 126)
				c = '?';
		return n.substr(0, n.find_last_not_of(" ?") + 1);
	}

	size_t count_of(const std::string &s, const std::string &what)
	{
		size_t n = 0;
		for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1))
			n++;
		return n;
	}

	std::string convert_args(const std::wstring &out, const std::wstring &overlay, const std::wstring &lists)
	{
		return "convert --out " + q(out) + " --ffxi " + q(kFFXI) + " --overlay " + q(overlay) + " --lists " + q(lists) + " --threads 4 --ignore-game";
	}
}

int main()
{
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	root = exe;
	root = root.substr(0, root.find_last_of(L'\\'));
	root = root.substr(0, root.find_last_of(L'\\'));
	tool = root + L"\\build\\hifixi-texpack.exe";
	work = root + L"\\build\\test\\texpack";
	if (GetFileAttributesW((kFFXI + L"\\" + wide(kGiddeus)).c_str()) == INVALID_FILE_ATTRIBUTES ||
		GetFileAttributesW((kOverlay + L"\\" + wide(kGiddeus)).c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		std::printf("skipped: the game's files or the hifi-textures overlay aren't on this PC\n");
		return 0;
	}
	remove_tree(work);
	make_folders(work);
	const std::wstring lists = work + L"\\lists";
	write_lists(lists);

	// What the DATs' upscaled textures are, read here independently
	std::vector<pair_info> expected;
	for (const char *rel : { kZone, kGiddeus, kModel, kMask, kUntagged })
	{
		std::vector<pair_info> u = upscaled(rel, kOverlay);
		expected.insert(expected.end(), u.begin(), u.end());
	}
	std::set<pk::key> expected_keys;
	for (const pair_info &p : expected)
		expected_keys.insert(p.k);

	// ---- A: the installed overlay
	const std::wstring A = work + L"\\A";
	int code = 0;
	std::string out = run(convert_args(A, kOverlay, lists), code);
	check(code == 0, "convert: exit 0 (" + line_with(out, "DATs ") + ")");
	check(out.find("the logs' 1 palette-only rows left out") != std::string::npos, "the palette-only row is left out: " + line_with(out, "inputs:"));
	std::shared_ptr<const pk::index> a = load(A);
	check(a != nullptr, "the pack loads");
	if (!a)
	{
		std::printf("%s", out.c_str());
		return failures + 1;
	}
	{
		std::string o = oracle(A, code);
		check(code == 0, "pack_oracle.py reads it: " + last_line(o));
	}
	const std::string src = pack_src(A);
	check(count_lines(src) == 5 && src.find(kPalette) == std::string::npos && src.find(kGiddeus) != std::string::npos && src.find(kMask) != std::string::npos &&
			src.find(kUntagged) != std::string::npos,
		"pack.src: the five DATs, not the palette-only one");
	{
		bool same = a->entries.size() == expected_keys.size();
		for (const pk::key &k : expected_keys)
			same = same && a->find(k) != nullptr;
		check(same, "the pack's keys are the upscaled chunks' original keys: " + std::to_string(a->entries.size()) + " entries, " +
			std::to_string(expected.size()) + " upscaled textures, " + std::to_string(expected_keys.size()) + " keys");
	}
	// (each texture against its entry: fields from the original, level 0 the overlay's blocks, the right number of levels)
	uint32_t b1 = 0, field_bad = 0, level0_bad = 0, chain_bad = 0;
	std::string first_bad;
	std::map<std::string, uint32_t> ids;
	for (const pair_info &p : expected)
	{
		const pk::entry *e = a->find(p.k);
		if (e == nullptr)
			continue;
		const pk::record *r = e->map(pk::kColour);
		// (the first DAT with a key wrote its entry: fields that name the DAT are checked only there)
		const bool own = e->chunk == p.chunk && std::memcmp(e->name, p.name.data(), 16) == 0;
		if (e->orig_format != p.format || e->orig_fourcc != p.fourcc || r == nullptr || r->w != p.vw || r->h != p.vh ||
			((e->flags & pk::kNoPack) != 0) != (p.format == 0xB1) || std::memcmp(e->name, p.name.data(), 16) != 0)
		{
			if (field_bad++ == 0)
				first_bad = p.rel + " chunk " + std::to_string(p.chunk);
			continue;
		}
		if (own)
		{
			if (ids.count(p.rel) == 0)
				ids[p.rel] = e->file_id;
			if (ids[p.rel] != e->file_id || e->source_kind != (p.rel == kModel || p.rel == kMask || p.rel == kUntagged ? pk::kSourceModel : pk::kSourceZone))
				field_bad++;
		}
		b1 += own && p.format == 0xB1;
		const std::vector<std::vector<uint8_t>> lv = levels_of(a, *r);
		if (lv.empty() || lv[0] != p.blocks)
			level0_bad++;
		if (lv.size() != pk::bc2::chain_levels(p.vw, p.vh))
			chain_bad++;
	}
	check(field_bad == 0, "every entry's fields are its original chunk's (format, fourcc, name, no_pack for B1, file id, source kind)" +
		(first_bad.empty() ? std::string() : ": first wrong " + first_bad));
	check(b1 == 5, "Giddeus' five B1 textures: keyed by the B1 body, no_pack (" + std::to_string(b1) + ")");
	check(level0_bad == 0, "level 0 is the overlay's BC2 blocks, unchanged (" + std::to_string(level0_bad) + " differ)");
	check(chain_bad == 0, "every colour record has bc2::chain_levels levels (" + std::to_string(chain_bad) + " don't)");
	check(ids.size() == 5 && ids[kZone] == 250 && ids[kModel] != 0 && ids[kMask] != 0 && ids[kUntagged] != 0, "file ids: ROM\\0\\100.DAT is 250 (zone 150), the others found");
	// (alpha kinds and their mips)
	{
		uint32_t kinds[5] = {}, cut_bad = 0, box_bad = 0, vcol_bad = 0;
		double worst = 0;
		for (const pk::entry &e : a->entries)
		{
			kinds[std::min<uint8_t>(e.alpha_kind, 4)]++;
			const pk::record *r = e.map(pk::kColour);
			const std::vector<std::vector<uint8_t>> lv = levels_of(a, *r);
			if (lv.size() < 2)
				continue;
			if (e.alpha_kind == pk::kAlphaCut)
			{
				if (e.source_kind == pk::kSourceZone && e.vcol_a == 0)
					vcol_bad++;
				const float t = e.source_kind == pk::kSourceZone ? pk::bc2::zone_cut_threshold(e.vcol_a) : pk::bc2::kCharsCutThreshold;
				const std::vector<uint8_t> px0 = pk::bc2::decode_bc2(lv[0].data(), r->w, r->h);
				const double c0 = pk::bc2::coverage(px0.data(), r->w, r->h, t);
				for (uint32_t i = 1; i < lv.size(); ++i)
				{
					const uint32_t w = r->w >> i, h = r->h >> i;
					if (w * h < 256)
						break;
					const std::vector<uint8_t> px = pk::bc2::decode_bc2(lv[i].data(), w, h);
					const double d = std::abs(pk::bc2::coverage(px.data(), w, h, t) - c0);
					worst = std::max(worst, d);
					cut_bad += d > 0.02;
				}
			}
			else
			{
				const std::vector<std::vector<uint8_t>> box = pk::bc2::smaller_levels(lv[0], r->w, r->h, true, uint32_t(lv.size()));
				for (size_t i = 1; i < lv.size(); ++i)
					box_bad += box[i - 1] != lv[i];
			}
		}
		check(kinds[pk::kAlphaCut] > 0 && kinds[pk::kAlphaBlended] > 0 && kinds[pk::kAlphaMixed] > 0,
			"alpha kinds from the meshes: none " + std::to_string(kinds[0]) + ", cut-out " + std::to_string(kinds[1]) + ", blended " + std::to_string(kinds[2]) +
				", mask " + std::to_string(kinds[3]) + ", mixed " + std::to_string(kinds[4]));
		check(vcol_bad == 0, "every zone cut-out entry has its typical vertex alpha");
		char text[160];
		std::snprintf(text, sizeof(text), "cut-out mips keep level 0's coverage within 2%% (levels of 256 texels and more; worst %.2f%%)", worst * 100);
		check(cut_bad == 0, text);
		check(box_bad == 0, "blended, mask, mixed and none mips are smaller_levels' box, byte for byte (" + std::to_string(box_bad) + " levels differ)");
	}
	// (the model DATs' kinds from their Os2: ROM3\5\110's texture its mesh shines with is a mask, whose mips the box
	// above held to smaller_levels'; ROM\310\0's, as its five Os2 name them)
	{
		std::string mask, model;
		uint32_t untagged = 0, untagged_none = 0;
		for (const pk::entry &e : a->entries)
		{
			const std::string n = printed(e.name);
			if (e.file_id == ids[kMask])
				mask += n + ":" + std::to_string(e.alpha_kind) + " ";
			else if (e.file_id == ids[kModel])
				model += n + ":" + std::to_string(e.alpha_kind) + " ";
			else if (e.file_id == ids[kUntagged])
				untagged++, untagged_none += e.alpha_kind == pk::kAlphaNone;
		}
		check(untagged == 3 && untagged_none == 3, "ROM3\\5\\31's three textures no Os2 names: none, the plain box (" + std::to_string(untagged_none) + " of " +
			std::to_string(untagged) + ")");
		check(mask == "tim     mashou:3 ", "ROM3\\5\\110's shine texture: mask (" + mask + ")");
		check(model == kModelKinds, "ROM\\310\\0's entries' kinds as its Os2 name them (" + model + ")");
		check(out.find("model textures no Os2 group names " + std::string(kModelUntagged) + " (none)") != std::string::npos,
			"model textures no Os2 group names: none, counted (" + line_with(out, "tags: zone textures") + ")");
	}
	// (Giddeus' cut-outs: survey of zone 145, cut-out pieces' vertex alpha 0x80)
	{
		uint32_t gid_cut = 0, gid_80 = 0;
		for (const pk::entry &e : a->entries)
			if (e.file_id == ids[kGiddeus] && e.alpha_kind == pk::kAlphaCut)
				gid_cut++, gid_80 += e.vcol_a == 0x80;
		check(gid_cut > 0 && gid_80 == gid_cut, "Giddeus' cut-out textures at vertex alpha 0x80 (" + std::to_string(gid_80) + " of " + std::to_string(gid_cut) + ")");
	}

	// verify, stats
	{
		out = run("verify --pack " + q(A) + " --soul --ffxi " + q(kFFXI) + " --threads 4", code);
		check(code == 0 && out.find(", 0 bad") != std::string::npos && out.find("soul: " + std::to_string(a->entries.size()) + " entries tested, 0 over") != std::string::npos,
			"verify --soul: every record and level CRC right, the soul test within its limits (" + line_with(out, "soul: ") + ")");
		out = run("stats --pack " + q(A) + " --scan --ffxi " + q(kFFXI) + " --overlay " + q(kOverlay), code);
		const std::string n = std::to_string(expected.size());
		check(code == 0 && out.find("in the pack " + n + " of " + n + " textures (100.00%)") != std::string::npos, "stats --scan: " + line_with(out, "overlay scan"));
	}

	// ---- B: resume (a stop file, --limit 1, the rest), the same pack as one run
	{
		const std::wstring B = work + L"\\B";
		make_folders(B);
		write_text(B + L"\\stop", "");
		out = run(convert_args(B, kOverlay, lists), code);
		std::shared_ptr<const pk::index> b = load(B);
		check(code == 3 && b != nullptr && b->entries.empty() && count_lines(pack_src(B)) == 0, "a stop file: stopped before the first DAT, an empty pack");
		DeleteFileW((B + L"\\stop").c_str());
		out = run(convert_args(B, kOverlay, lists) + " --limit 1", code);
		b = load(B);
		const std::string s1 = pack_src(B);
		check(code == 0 && count_lines(s1) == 1 && s1.rfind("ROM\\0\\100.DAT\t", 0) == 0 && b && !b->entries.empty(),
			"--limit 1: the first DAT (zones first, by path: ROM\\0\\100.DAT) in pack.src and the pack");
		out = run(convert_args(B, kOverlay, lists), code);
		check(code == 0 && out.find("1 DATs already done (pack.src), 4 to do") != std::string::npos, "resumed: " + line_with(out, "already done"));
		b = load(B);
		check(b && same_entries(*a, *b), "the resumed pack is the one-run pack (keys, kinds, flags, sources, record CRCs)");
		std::string o = oracle(B, code);
		check(code == 0, "pack_oracle.py reads the resumed pack: " + last_line(o));
		out = run(convert_args(B, kOverlay, lists), code);
		check(code == 0 && out.find("5 DATs already done (pack.src), 0 to do") != std::string::npos && load(B)->entries.size() == a->entries.size(),
			"a rerun does nothing: " + line_with(out, "already done"));
	}

	// ---- B2: two zones sharing textures, one used otherwise in each: --limit 1 then the rest tags them as one run
	{
		const std::wstring lists_s = work + L"\\listsS", one = work + L"\\S1", parts = work + L"\\S2", alone = work + L"\\S3";
		write_shared_lists(lists_s);
		out = run(convert_args(one, kOverlay, lists_s), code);
		const bool one_ok = code == 0;
		run(convert_args(parts, kOverlay, lists_s) + " --limit 1", code);
		out = run(convert_args(parts, kOverlay, lists_s), code);
		const bool parts_ok = code == 0 && out.find("1 DATs already done (pack.src), 1 to do") != std::string::npos;
		run(convert_args(alone, kOverlay, lists_s) + " --only " + kSharedA, code);
		std::shared_ptr<const pk::index> s1 = load(one), s2 = load(parts), s3 = load(alone);
		// (the keys both zones upscale, and how ROM\1\66 alone tags them: at least one otherwise, or this proves nothing)
		std::set<pk::key> in_a, shared;
		for (const pair_info &p : upscaled(kSharedA, kOverlay))
			in_a.insert(p.k);
		for (const pair_info &p : upscaled(kSharedB, kOverlay))
			if (in_a.count(p.k) != 0)
				shared.insert(p.k);
		uint32_t otherwise = 0;
		for (const pk::key &k : shared)
		{
			const pk::entry *x = s1 ? s1->find(k) : nullptr, *y = s3 ? s3->find(k) : nullptr;
			otherwise += x != nullptr && y != nullptr && (x->alpha_kind != y->alpha_kind || x->vcol_a != y->vcol_a);
		}
		check(one_ok && parts_ok && s1 && s2 && s3 && shared.size() == 3 && otherwise > 0 && same_entries(*s1, *s2),
			"ROM\\1\\66 and ROM\\1\\72 (" + std::to_string(shared.size()) + " textures shared, " + std::to_string(otherwise) +
				" tagged otherwise by ROM\\1\\66 alone): --limit 1 and the rest make the one-run pack, kinds and all");
	}

	// ---- C: hand-made overlays
	const pair_info *sx = nullptr, *sy = nullptr, *tweaked = nullptr; // (Giddeus' swapped pair, and its changed one: F uses the pair too)
	{
		const std::wstring overlay = work + L"\\overlayC", C = work + L"\\C";
		// (the model DAT: its first upscaled chunk renamed, a B1 replaced by a bigger A1, every A1's template bytes
		// overwritten)
		std::vector<raw_chunk> orig = split_dat(read(kFFXI + L"\\" + wide(kModel))), over = split_dat(read(kOverlay + L"\\" + wide(kModel)));
		uint32_t renamed = 0xFFFFFFFFu, replaced = 0xFFFFFFFFu;
		for (uint32_t i = 0; i < over.size(); ++i)
		{
			raw_chunk &c = over[i];
			if ((c.header[4] & 0x7F) != 0x20 || c.body.size() < 69)
				continue;
			if (c.body.size() != orig[i].body.size())
			{
				if (renamed == 0xFFFFFFFFu)
				{
					renamed = i;
					c.body[1] ^= 0x20;
				}
				for (size_t k = 29; k < 57; ++k)
					c.body[k] = 0xEE;
			}
			else if (c.body[0] == 0xB1 && replaced == 0xFFFFFFFFu)
			{
				replaced = i;
				const uint32_t w = uint32_t(i32(c.body.data() + 21)) * 2, h = uint32_t(i32(c.body.data() + 25)) * 2;
				std::vector<uint8_t> body(69 + size_t(w / 4) * (h / 4) * 16, 0);
				body[0] = 0xA1;
				std::memcpy(body.data() + 1, c.body.data() + 1, 16);
				const int32_t wh[2] = { int32_t(w), int32_t(h) };
				std::memcpy(body.data() + 21, wh, 8);
				std::memcpy(body.data() + 57, "3TXD", 4);
				for (size_t k = 69; k < body.size(); k += 16)
				{
					std::memset(body.data() + k, 0xFF, 8);  // (alpha 15: opaque)
					body[k + 8] = 0x1F, body[k + 10] = 0x1F; // (blue)
				}
				c.body = body;
			}
		}
		write(overlay + L"\\" + wide(kModel), join_dat(over));
		// (the 2x zone: one chunk more than the original)
		std::vector<raw_chunk> zone = split_dat(read(kOverlay + L"\\" + wide(kZone)));
		zone.push_back(zone.back());
		write(overlay + L"\\" + wide(kZone), join_dat(zone));
		// (the palette-only zone: a copy in the overlay, which its log row still leaves out)
		write(overlay + L"\\" + wide(kPalette), read(kFFXI + L"\\" + wide(kPalette)));
		// (Giddeus: two upscaled A1 textures of one size swapped, which the soul test leaves out, and one alpha nibble of a
		// third changed, which it passes; each key once in the DAT)
		std::vector<raw_chunk> gid = split_dat(read(kOverlay + L"\\" + wide(kGiddeus)));
		{
			std::vector<const pair_info *> a1;
			for (const pair_info &p : expected)
				if (p.rel == kGiddeus && p.format == 0xA1 &&
					std::count_if(expected.begin(), expected.end(), [&](const pair_info &x) { return x.k == p.k; }) == 1)
					a1.push_back(&p);
			for (size_t i = 0; i < a1.size() && sx == nullptr; ++i)
				for (size_t j = i + 1; j < a1.size() && sx == nullptr; ++j)
					if (a1[i]->vw == a1[j]->vw && a1[i]->vh == a1[j]->vh && a1[i]->blocks != a1[j]->blocks &&
						gid[a1[i]->chunk].body.size() == gid[a1[j]->chunk].body.size())
						sx = a1[i], sy = a1[j];
			for (const pair_info *p : a1)
				if (tweaked == nullptr && p != sx && p != sy)
					tweaked = p;
		}
		check(sx != nullptr && tweaked != nullptr, "Giddeus has two upscaled A1 textures of one size, and a third");
		if (sx == nullptr || tweaked == nullptr)
			return failures;
		std::swap_ranges(gid[sx->chunk].body.begin() + 69, gid[sx->chunk].body.end(), gid[sy->chunk].body.begin() + 69);
		gid[tweaked->chunk].body[69 + 16 * 5] ^= 0x01;
		write(overlay + L"\\" + wide(kGiddeus), join_dat(gid));
		const std::wstring lists_c = work + L"\\listsC";
		write_lists(lists_c);
		out = run(convert_args(C, overlay, lists_c), code);
		std::shared_ptr<const pk::index> c = load(C);
		{
			const std::string swapped_x = line_with(out, "ROM\\0\\104.DAT: chunk " + std::to_string(sx->chunk) + " '"),
							  swapped_y = line_with(out, "ROM\\0\\104.DAT: chunk " + std::to_string(sy->chunk) + " '");
			const pk::entry *t = c ? c->find(tweaked->k) : nullptr;
			check(c && count_of(out, "not the original's painting") == 2 && swapped_x.find("not the original's painting") != std::string::npos &&
					swapped_y.find("not the original's painting") != std::string::npos && c->find(sx->k) == nullptr && c->find(sy->k) == nullptr && t != nullptr &&
					t->maps[0].crc != a->find(tweaked->k)->maps[0].crc && pack_src(C).find(kGiddeus) != std::string::npos,
				"two upscales swapped: both left out by the soul test (" + swapped_x + "); one alpha nibble changed: passes; Giddeus in pack.src");
		}
		check(code == 0 && c != nullptr, "convert on the hand-made overlay: exit 0 (" + line_with(out, "DATs ") + ")");
		check(out.find("ROM\\0\\100.DAT: refused: 1140 chunks in the original, 1141 in the overlay's copy") != std::string::npos &&
				pack_src(C).find(kZone) == std::string::npos,
			"an extra chunk: the DAT refused, not in pack.src: " + line_with(out, "refused"));
		check(out.find("ROM\\0\\28.DAT: palette only, left by the bake: skipped") != std::string::npos && pack_src(C).find(kPalette) == std::string::npos,
			"a palette-only row whose DAT the overlay has: skipped");
		check(renamed != 0xFFFFFFFFu && out.find("ROM\\310\\0.DAT: chunk " + std::to_string(renamed) + ": named") != std::string::npos,
			"a renamed chunk: logged and skipped: " + line_with(out, ": named"));
		const std::vector<raw_chunk> &o = orig;
		if (c && renamed != 0xFFFFFFFFu && replaced != 0xFFFFFFFFu)
		{
			const pk::key kr = pk::key_of(o[renamed].body.data(), o[renamed].body.size()), kb = pk::key_of(o[replaced].body.data(), o[replaced].body.size());
			check(c->find(kr) == nullptr, "the renamed chunk has no entry");
			const pk::entry *e = c->find(kb);
			check(e != nullptr && e->orig_format == 0xB1 && e->orig_fourcc == 0 && (e->flags & pk::kNoPack) != 0 && e->w == uint32_t(i32(o[replaced].body.data() + 21)) &&
					e->chunk == replaced && e->map(pk::kColour) != nullptr && e->map(pk::kColour)->w == e->w * 2,
				"a B1 replaced by a bigger A1: keyed by the B1 body, B1 and no_pack from the original, the A1 as its colour (the soul test fails it, no_pack keeps it)");
			// (Giddeus' copy re-baked: the real one, written now. Converted again without --redo (the copy's time changed),
			// the swapped two added, the changed one's entry replaced)
			write(overlay + L"\\" + wide(kGiddeus), read(kOverlay + L"\\" + wide(kGiddeus)));
			out = run(convert_args(C, overlay, lists_c), code);
			c = load(C);
			const pk::entry *t = c ? c->find(tweaked->k) : nullptr;
			check(code == 0 && out.find("1 DATs already done (pack.src), 2 to do") != std::string::npos && out.find("added 2, replaced 1 ") != std::string::npos && t &&
					t->maps[0].crc == a->find(tweaked->k)->maps[0].crc && c->find(sx->k) && c->find(sy->k),
				"a changed overlay copy converted again: the two added, the changed entry replaced (" + line_with(out, "entries: ") + ")");
			// (the others: as in A, though the overlay's template bytes were overwritten)
			uint32_t others = 0, same = 0;
			for (const pk::entry &x : c->entries)
			{
				if (x.the_key() == kb)
					continue;
				others++;
				const pk::entry *y = a->find(x.the_key());
				same += y != nullptr && y->orig_format == x.orig_format && y->orig_fourcc == x.orig_fourcc && y->alpha_kind == x.alpha_kind &&
					y->alpha_lo == x.alpha_lo && y->alpha_hi == x.alpha_hi && y->flags == x.flags && y->w == x.w && y->h == x.h && y->maps[0].crc == x.maps[0].crc;
			}
			check(others > 0 && same == others, "with the overlay's template bytes overwritten, every other entry is A's (" +
				std::to_string(same) + " of " + std::to_string(others) + ")");
		}
		std::string or2 = oracle(C, code);
		check(code == 0, "pack_oracle.py reads it: " + last_line(or2));
	}

	// ---- D: refusals before anything is written
	{
		const std::wstring ashita = work + L"\\ashita", D = work + L"\\D";
		write_text(ashita + L"\\Ashita.dll", "");
		write_text(ashita + L"\\logs\\now.txt", "written just now");
		out = run("convert --out " + q(D) + " --ffxi " + q(kFFXI) + " --overlay " + q(kOverlay) + " --lists " + q(lists) + " --ashita " + q(ashita), code);
		check(code == 4 && GetFileAttributesW((D + L"\\pack.idx").c_str()) == INVALID_FILE_ATTRIBUTES, "an Ashita log written just now: refused, nothing written (" +
			last_line(out) + ")");
		out = run(convert_args(D, kOverlay, lists) + " --reserve-gb 100000000", code);
		check(code == 1 && out.find("estimate: at most") != std::string::npos && out.find("Refused: the disk needs") != std::string::npos &&
				GetFileAttributesW((D + L"\\pack.idx").c_str()) == INVALID_FILE_ATTRIBUTES,
			"too little free disk for the estimate: refused, the estimate printed (" + line_with(out, "estimate") + ")");
	}

	// ---- E: verify finds a damaged byte; prune against a changed game folder; compact
	{
		const std::wstring E = work + L"\\E", ffxi = work + L"\\ffxi";
		make_folders(E);
		WIN32_FIND_DATAW fd;
		HANDLE f = FindFirstFileW((A + L"\\*").c_str(), &fd);
		do
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				CopyFileW((A + L"\\" + fd.cFileName).c_str(), (E + L"\\" + fd.cFileName).c_str(), FALSE);
		while (FindNextFileW(f, &fd));
		FindClose(f);
		// (one byte of the first record of pack-000.bin flipped, then back)
		std::vector<uint8_t> data = read(E + L"\\pack-000.bin");
		data[100] ^= 0x55;
		write(E + L"\\pack-000.bin", data);
		out = run("verify --pack " + q(E), code);
		check(code == 1 && out.find("FAIL ") != std::string::npos && out.find("CRC") != std::string::npos, "verify: a damaged byte fails its entry: " + line_with(out, "FAIL"));
		data[100] ^= 0x55;
		write(E + L"\\pack-000.bin", data);
		// (a game folder with the tables, the 2x zone and the mask model as they are (copied with their times), the model
		// DAT with one upscaled texture changed (written now), no Giddeus)
		for (const wchar_t *name : { L"VTABLE.DAT", L"FTABLE.DAT" })
			write(ffxi + L"\\" + name, read(kFFXI + L"\\" + name));
		for (const char *rel : { kZone, kMask, kUntagged })
		{
			make_folders((ffxi + L"\\" + wide(rel)).substr(0, (ffxi + L"\\" + wide(rel)).find_last_of(L'\\')));
			CopyFileW((kFFXI + L"\\" + wide(rel)).c_str(), (ffxi + L"\\" + wide(rel)).c_str(), FALSE);
		}
		std::vector<raw_chunk> model = split_dat(read(kFFXI + L"\\" + wide(kModel)));
		uint32_t changed = 0xFFFFFFFFu;
		for (const pair_info &p : expected)
			if (p.rel == kModel)
			{
				changed = p.chunk;
				model[changed].body[100] ^= 0xFF;
				break;
			}
		write(ffxi + L"\\" + wide(kModel), join_dat(model));
		uint32_t giddeus = 0, own_model = 0;
		for (const pk::entry &e : a->entries)
			giddeus += e.file_id == ids[kGiddeus], own_model += e.file_id == ids[kModel] && e.chunk == changed;
		// (one builder a pack: prune and compact refused while a writer holds it)
		{
			pk::writer held;
			pk::writer_options wo;
			wo.folder = E;
			std::string why;
			const bool opened = held.open(wo, why);
			const std::string pruned = run("prune --pack " + q(E) + " --ffxi " + q(ffxi) + " --ignore-game", code);
			const int prune_code = code;
			const std::string compacted = run("compact --pack " + q(E) + " --ignore-game", code);
			pk::writer second;
			const bool second_refused = !second.open(wo, why) && why.find("another texpack") != std::string::npos;
			check(opened && prune_code == 1 && pruned.find("another texpack is writing this pack") != std::string::npos && code == 1 &&
					compacted.find("another texpack is writing this pack") != std::string::npos && second_refused &&
					GetFileAttributesW((E + L"-compact").c_str()) == INVALID_FILE_ATTRIBUTES,
				"pack.lock: prune, compact and a second writer refused while a writer holds the pack (" + last_line(pruned) + ")");
		}
		check(GetFileAttributesW((E + L"\\pack.lock").c_str()) == INVALID_FILE_ATTRIBUTES, "pack.lock gone once its writer closed");
		out = run("prune --pack " + q(E) + " --ffxi " + q(ffxi) + " --ignore-game", code);
		std::shared_ptr<const pk::index> e = load(E);
		const std::string src_pruned = pack_src(E);
		check(code == 0 && e != nullptr && e->entries.size() == a->entries.size() - giddeus - own_model && own_model == 1,
			"prune: Giddeus' entries (no file) and the changed texture dropped (" + line_with(out, "pruned") + ")");
		check(count_lines(src_pruned) == 3 && src_pruned.find(kZone) != std::string::npos && src_pruned.find(kMask) != std::string::npos &&
				out.find("1 gone, 1 changed") != std::string::npos,
			"prune: the gone and the changed DATs' pack.src lines taken out, the unchanged three kept");
		uint64_t before = 0;
		for (uint64_t s : e->file_bytes)
			before += s;
		e.reset();
		out = run("compact --pack " + q(E) + " --ignore-game", code);
		e = load(E);
		uint64_t after = 0;
		if (e)
			for (uint64_t s : e->file_bytes)
				after += s;
		check(code == 0 && e != nullptr && e->entries.size() == a->entries.size() - giddeus - own_model && after < before &&
				GetFileAttributesW((E + L"-old\\pack.idx").c_str()) != INVALID_FILE_ATTRIBUTES && pack_src(E) == src_pruned,
			"compact: the same entries in fewer bytes (" + std::to_string(before) + " -> " + std::to_string(after) + "), the old pack left as E-old, pack.src kept");
		std::string o = oracle(E, code);
		check(code == 0, "pack_oracle.py reads the compacted pack: " + last_line(o));
		out = run("verify --pack " + q(E), code);
		check(code == 0, "verify on the compacted pack: " + line_with(out, "records:"));
		// (convert against the real game folder: the two DATs prune took out done again, every entry back)
		e.reset();
		out = run(convert_args(E, kOverlay, lists), code);
		e = load(E);
		check(code == 0 && out.find("3 DATs already done (pack.src), 2 to do") != std::string::npos && e && same_entries(*a, *e),
			"convert after prune and compact: the DATs prune took out converted again, the pack A's (" + line_with(out, "already done") + ")");
	}

	// ---- E2: compact with a record that can't be read: it's left out, and so are its DATs' pack.src lines; convert then
	// makes it again
	{
		const std::wstring E2 = work + L"\\E2";
		make_folders(E2);
		WIN32_FIND_DATAW fd;
		HANDLE f = FindFirstFileW((A + L"\\*").c_str(), &fd);
		do
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				CopyFileW((A + L"\\" + fd.cFileName).c_str(), (E2 + L"\\" + fd.cFileName).c_str(), FALSE);
		while (FindNextFileW(f, &fd));
		FindClose(f);
		// (a byte of the record at the data file's start damaged: its entry, and the DATs with its key)
		const pk::entry *hit = nullptr;
		for (const pk::entry &x : a->entries)
			if (x.maps[0].file == 0 && x.maps[0].offset == 0)
				hit = &x;
		std::set<std::string> with_key;
		for (const pair_info &p : expected)
			if (hit != nullptr && p.k == hit->the_key())
				with_key.insert(p.rel);
		std::vector<uint8_t> data = read(E2 + L"\\pack-000.bin");
		data[100] ^= 0x55;
		write(E2 + L"\\pack-000.bin", data);
		out = run("compact --pack " + q(E2) + " --ffxi " + q(kFFXI) + " --ignore-game", code);
		std::shared_ptr<const pk::index> e2 = load(E2);
		const std::string src2 = pack_src(E2);
		bool lines_right = count_lines(src2) == 5 - with_key.size();
		for (const std::string &rel : with_key)
			lines_right = lines_right && src2.find(rel + "\t") == std::string::npos;
		check(hit != nullptr && !with_key.empty() && code == 0 && e2 && e2->entries.size() == a->entries.size() - 1 && e2->find(hit->the_key()) == nullptr &&
				lines_right,
			"compact: the damaged record left out, the pack.src lines of the " + std::to_string(with_key.size()) + " DATs with its key too (" +
				line_with(out, "entries left out") + ")");
		e2.reset();
		out = run(convert_args(E2, kOverlay, lists), code);
		e2 = load(E2);
		check(code == 0 && e2 && same_entries(*a, *e2), "convert after it: the entry made again, the pack A's (" + line_with(out, "already done") + ")");
	}

	// ---- F: verify --soul fails entries whose maps aren't their originals' (two same-size entries' maps swapped)
	{
		const std::wstring F = work + L"\\F";
		pk::writer w;
		pk::writer_options wo;
		wo.folder = F;
		std::string why;
		bool ok = w.open(wo, why);
		const pk::key kx = sx->k, ky = sy->k;
		for (const pk::entry &x : a->entries)
		{
			const pk::entry *from = x.the_key() == kx ? a->find(ky) : x.the_key() == ky ? a->find(kx) : &x;
			const pk::record &r = from->maps[0];
			pk::map_in m;
			m.kind = r.kind, m.format = r.format, m.w = r.w, m.h = r.h;
			m.levels = levels_of(a, r);
			ok = ok && w.add(x, { m }, why) == pk::add_result::ok;
		}
		ok = ok && w.commit(why);
		w.close();
		write_text(F + L"\\pack.src", pack_src(A));
		out = run("verify --pack " + q(F) + " --soul --ffxi " + q(kFFXI) + " --threads 4", code);
		const std::string name_x = printed(sx->name.data()), name_y = printed(sy->name.data());
		check(ok && code == 1 && count_of(out, "SOUL FAIL") == 2 && out.find("SOUL FAIL '" + name_x + "'") != std::string::npos &&
				out.find("SOUL FAIL '" + name_y + "'") != std::string::npos && out.find(", 0 bad") != std::string::npos,
			"verify --soul: two entries' maps swapped (records sound): exactly those two fail (" + line_with(out, "SOUL FAIL") + ")");
	}

	std::printf("texpack_test: %d passed, %d failed\n", passes, failures);
	return failures;
}
