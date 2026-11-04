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
 * make (M7, make_tests): keeptexels.cpp and upscale.cpp's Lanczos against keeptexels.py and Pillow (tests\data\
 * texmake_ref.bin, from tests\texmake_oracle.py); wrap-pad, crop, finish; get-upscaler refusing a wrong size and a wrong
 * hash served from file:// (no network; HIFIXI_ESRGAN_ZIP naming the official zip also checks what is kept); make on a
 * fake game folder with build\fake_esrgan.exe and no Python on PATH: keys, fields, factors, the models' kinds as
 * convert's, verify --soul, dedupe, resume after a kill, the stop file, the fake game started mid-upscale (paused, the
 * child ended, goes on after it closes), --no-wait, a hung and a failing upscaler.
 *
 * heights (M9, heights_tests): BC4's round trip within its bound on noise, bands, gradients, 0/255 and flat blocks; the
 * height sizes per --height-scale; coverage (a cut-out's holes change nothing it shows); a flat colour's span 0; on a pack
 * of ROM\0\100 at 2x and ROM\310\0 at 4x made here: every entry gets a BC4 height in the rules, colour and fields kept,
 * height_from_game on exactly the textures ROM\92\51 has bump maps for, verify and the oracle read it, pack.src and the
 * source pack untouched; resume (--limit 1, then the rest: the same records), a rerun makes nothing; the stop file, the
 * game (--no-wait), no pack, a bad --height-scale; full and game sizes; the stored game-size heights made back into
 * normals against the game's own (angle bounds); make --heights equal to make then heights (in make_tests).
 *
 *   build\texpack_test.exe [make|heights]   (64-bit, built by tools\build_texpack.cmd; needs build\hifixi-texpack.exe,
 *                               build\fake_esrgan.exe, python, the game's files in C:\Games\PhoenixXI and the overlay;
 *                               work folder build\test\texpack. "make": only make's tests)
 *
 * Exit code: the number of failures.
 */

#include <windows.h>
#include "../src/pack/pack.hpp"
#include "../src/pack/pack_write.hpp"
#include "../src/pack/bc2.hpp"
#include "../src/pack/bc4.hpp"
#include "../src/pack/height.hpp"
#include "../src/pack/validate.hpp"
#include "../src/bake/dat.hpp"
#include "../tools/texpack/keeptexels.hpp"
#include "../tools/texpack/upscale.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <tuple>
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

// ---- make (M7): the C++ ports against the scripts, get-upscaler's refusals, and make run with build\fake_esrgan.exe
namespace
{
	namespace ktx = hifi::texpack::keeptexels;
	namespace ups = hifi::texpack::upscale;

	const char *kMakeZones[] = { "ROM\\1\\66.DAT", "ROM\\1\\72.DAT" };
	const char *kMakeModels[] = { "ROM\\310\\0.DAT", "ROM3\\5\\110.DAT", "ROM3\\5\\31.DAT" };
	const wchar_t *kFakeGame = L"texpack-fakegame.exe";

	bool is_there(const std::wstring &path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
	// (pack.lock is deleted when its writer's handle closes, however the writer ends)
	bool is_locked(const std::wstring &pack) { return is_there(pack + L"\\pack.lock"); }

	// The largest difference between two byte runs
	int max_diff(const uint8_t *a, const uint8_t *b, size_t n)
	{
		int m = 0;
		for (size_t i = 0; i < n; ++i)
			m = std::max(m, std::abs(int(a[i]) - int(b[i])));
		return m;
	}

	// tests\data\texmake_ref.bin (tests\texmake_oracle.py): keeptexels.py's outputs and Pillow's Lanczos
	void ported_steps()
	{
		const std::vector<uint8_t> ref = read(root + L"\\tests\\data\\texmake_ref.bin");
		size_t at = 0;
		auto u32 = [&]() {
			uint32_t v = 0;
			if (at + 4 <= ref.size())
				std::memcpy(&v, ref.data() + at, 4);
			at += 4;
			return v;
		};
		check(ref.size() > 8 && std::memcmp(ref.data(), "KTX1", 4) == 0, "tests\\data\\texmake_ref.bin is there");
		if (ref.size() <= 8 || std::memcmp(ref.data(), "KTX1", 4) != 0)
			return;
		at = 4;
		const uint32_t cases = u32();
		int worst = 0;
		uint32_t exact = 0, ok = 0;
		size_t off_by_one = 0, texels = 0;
		for (uint32_t c = 0; c < cases; ++c)
		{
			const uint32_t w = u32(), h = u32(), s = u32();
			const size_t n0 = size_t(w) * h * 4, n1 = n0 * s * s;
			if (at + n0 + 2 * n1 > ref.size())
				break;
			const uint8_t *o = ref.data() + at, *u = o + n0, *r = u + n1;
			at += n0 + 2 * n1;
			std::vector<uint8_t> mine(u, u + n1);
			const bool ran = ktx::apply(o, w, h, mine.data(), w * s, h * s);
			const int d = max_diff(mine.data(), r, n1);
			worst = std::max(worst, d);
			ok += ran && d <= 1;
			exact += ran && d == 0;
			for (size_t i = 0; i < n1; ++i)
				off_by_one += mine[i] != r[i];
			texels += n1;
		}
		check(cases == 20 && ok == 20, "keeptexels port within 1 level of keeptexels.py on 20 textures (12 at 4x, 8 at 2x): " + std::to_string(exact) +
				" exact, worst " + std::to_string(worst) + ", " + std::to_string(off_by_one) + " of " + std::to_string(texels) + " bytes off by one");
		check(at + 8 <= ref.size() && std::memcmp(ref.data() + at, "LNZ1", 4) == 0, "the Lanczos references follow");
		at += 4;
		const uint32_t lcases = u32();
		uint32_t lok = 0;
		int lworst = 0;
		for (uint32_t c = 0; c < lcases; ++c)
		{
			const uint32_t w = u32(), h = u32(), ch = u32(), W = u32(), H = u32();
			const size_t ni = size_t(w) * h * ch, no = size_t(W) * H * ch;
			if (at + ni + no > ref.size())
				break;
			const std::vector<uint8_t> mine = ups::lanczos(ref.data() + at, w, h, ch, W, H);
			const int d = mine.size() == no ? max_diff(mine.data(), ref.data() + at + ni, no) : 999;
			lworst = std::max(lworst, d);
			lok += d <= 1;
			at += ni + no;
		}
		check(lcases == 9 && lok == lcases, "Lanczos (colour 4x to 2x, alpha up 2x and 4x) as Pillow's: " + std::to_string(lok) + " of " + std::to_string(lcases) +
				" within 1 level (worst " + std::to_string(lworst) + ")");

		// wrap-pad, crop and finish on a 12x8 picture whose texels all differ: the upscaler stood in by nearest 4x
		const uint32_t w = 12, h = 8;
		std::vector<uint8_t> p(size_t(w) * h * 4);
		for (uint32_t i = 0; i < w * h; ++i)
			p[i * 4] = uint8_t(i), p[i * 4 + 1] = uint8_t(255 - i), p[i * 4 + 2] = uint8_t(i * 7), p[i * 4 + 3] = uint8_t(i % 2 ? 0x80 : 0);
		const std::vector<uint8_t> padded = ups::wrap_pad_rgb(p.data(), w, h);
		const uint32_t pw = w + 16, ph = h + 16;
		bool wrap_ok = padded.size() == size_t(pw) * ph * 3;
		for (uint32_t y = 0; wrap_ok && y < ph; ++y)
			for (uint32_t x = 0; x < pw; ++x)
			{
				const uint32_t sy = (y + h * 4 - 8) % h, sx = (x + w * 4 - 8) % w;
				wrap_ok = wrap_ok && std::memcmp(&padded[(size_t(y) * pw + x) * 3], &p[(size_t(sy) * w + sx) * 4], 3) == 0;
			}
		check(wrap_ok, "wrap-pad: 8 texels of wrap round a 12x8 picture (numpy.pad 'wrap')");
		std::vector<uint8_t> big(size_t(pw) * 4 * ph * 4 * 4);
		for (uint32_t y = 0; y < ph * 4; ++y)
			for (uint32_t x = 0; x < pw * 4; ++x)
			{
				std::memcpy(&big[(size_t(y) * pw * 4 + x) * 4], &padded[(size_t(y / 4) * pw + x / 4) * 3], 3);
				big[(size_t(y) * pw * 4 + x) * 4 + 3] = 255;
			}
		std::vector<uint8_t> f4, f2;
		std::string why;
		const bool done4 = ups::finish(p.data(), w, h, big.data(), pw * 4, ph * 4, 4, f4, why), done2 = ups::finish(p.data(), w, h, big.data(), pw * 4, ph * 4, 2, f2, why);
		bool crop_ok = done4 && f4.size() == size_t(w) * 4 * h * 4 * 4;
		for (uint32_t y = 0; crop_ok && y < h * 4; ++y)
			for (uint32_t x = 0; x < w * 4; ++x)
				crop_ok = crop_ok && std::memcmp(&f4[(size_t(y) * w * 4 + x) * 4], &p[(size_t(y / 4) * w + x / 4) * 4], 3) == 0;
		std::vector<uint8_t> a1(size_t(w) * h), rgb4(size_t(w) * 4 * h * 4 * 3);
		for (uint32_t i = 0; i < w * h; ++i)
			a1[i] = p[i * 4 + 3];
		const std::vector<uint8_t> a4 = ups::lanczos(a1.data(), w, h, 1, w * 4, h * 4);
		for (size_t i = 0; crop_ok && i < a4.size(); ++i)
			crop_ok = f4[i * 4 + 3] == a4[i];
		check(crop_ok, "finish at 4x: the upscaler's picture cropped back to the texture (nearest 4x of it), the alpha Lanczos'd up");
		for (uint32_t i = 0; i < w * 4 * h * 4; ++i)
			std::memcpy(&rgb4[i * 3], &f4[i * 4], 3);
		const std::vector<uint8_t> rgb2 = ups::lanczos(rgb4.data(), w * 4, h * 4, 3, w * 2, h * 2);
		bool two_ok = done2 && f2.size() == size_t(w) * 2 * h * 2 * 4;
		for (uint32_t i = 0; two_ok && i < w * 2 * h * 2; ++i)
			two_ok = std::memcmp(&f2[i * 4], &rgb2[i * 3], 3) == 0;
		check(two_ok, "finish at 2x: the cropped 4x Lanczos'd down to 2x");
		std::vector<uint8_t> none;
		why.clear();
		const bool refused = !ups::finish(p.data(), w, h, big.data(), pw * 4 - 4, ph * 4, 4, none, why);
		check(refused && why.find("upscaler gave") != std::string::npos, "finish refuses a picture of the wrong size (" + why + ")");
	}

	// A process started with its output to a file
	struct started
	{
		HANDLE process = nullptr;
		DWORD pid = 0;
	};

	started start(const std::wstring &exe, const std::wstring &args, const std::wstring &log)
	{
		started s;
		SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
		HANDLE out = log.empty() ? nullptr : CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, 0, nullptr);
		STARTUPINFOW si = { sizeof(si) };
		if (out != nullptr && out != INVALID_HANDLE_VALUE)
		{
			si.dwFlags = STARTF_USESTDHANDLES;
			si.hStdOutput = si.hStdError = out;
		}
		PROCESS_INFORMATION pi = {};
		std::wstring cmd = L"\"" + exe + L"\" " + args;
		if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		{
			CloseHandle(pi.hThread);
			s.process = pi.hProcess;
			s.pid = pi.dwProcessId;
		}
		if (out != nullptr && out != INVALID_HANDLE_VALUE)
			CloseHandle(out);
		return s;
	}

	// Its exit code once it ends within ms (-1: still running, then ended)
	int finish(started &s, DWORD ms)
	{
		if (s.process == nullptr)
			return -2;
		DWORD code = DWORD(-1);
		if (WaitForSingleObject(s.process, ms) == WAIT_OBJECT_0)
			GetExitCodeProcess(s.process, &code);
		else
		{
			TerminateProcess(s.process, 99);
			WaitForSingleObject(s.process, 5000);
		}
		CloseHandle(s.process);
		s.process = nullptr;
		return int(code);
	}

	std::string text_of(const std::wstring &path)
	{
		const std::vector<uint8_t> b = read(path);
		return std::string(b.begin(), b.end());
	}

	// Waits up to ms for what to be true (checked every 50 ms)
	template <class F> bool wait_for(F what, DWORD ms)
	{
		for (DWORD t = 0; t < ms; t += 50)
		{
			if (what())
				return true;
			Sleep(50);
		}
		return what();
	}

	bool alive(DWORD pid)
	{
		HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
		if (h == nullptr)
			return false;
		const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
		CloseHandle(h);
		return running;
	}

	// The fake upscaler's runs from its log: the pids that started, and those of them that got to their end
	void fake_runs(const std::wstring &log, std::vector<DWORD> &began, std::set<DWORD> &ended)
	{
		began.clear(), ended.clear();
		bool ok = true;
		std::string line;
		for (char c : text_of(log) + "\n")
		{
			if (c != '\n')
			{
				if (c != '\r')
					line += c;
				continue;
			}
			if (line.rfind("start ", 0) == 0)
				began.push_back(DWORD(std::strtoul(line.c_str() + 6, nullptr, 10)));
			else if (line.rfind("end ", 0) == 0)
				ended.insert(DWORD(std::strtoul(line.c_str() + 4, nullptr, 10)));
			line.clear();
		}
		(void)ok;
	}

	// The last fake upscaler run that didn't get to its end has gone (ended by make's job object)
	bool child_gone(const std::wstring &log, std::string &what)
	{
		std::vector<DWORD> began;
		std::set<DWORD> ended;
		fake_runs(log, began, ended);
		for (auto it = began.rbegin(); it != began.rend(); ++it)
			if (ended.count(*it) == 0)
			{
				const bool gone = wait_for([&] { return !alive(*it); }, 3000);
				what = "upscaler pid " + std::to_string(*it) + (gone ? " gone" : " still running");
				return gone;
			}
		what = "no unfinished upscaler run in its log";
		return false;
	}

	// The A1 textures make takes from a DAT (sides multiples of 4), by key, in chunk order
	struct a1_texture
	{
		pk::key k;
		uint32_t chunk = 0, w = 0, h = 0;
		std::string name;
	};
	std::vector<a1_texture> a1_textures(const std::string &rel)
	{
		std::vector<a1_texture> out;
		const std::vector<uint8_t> dat = read(kFFXI + L"\\" + wide(rel));
		const std::vector<bk::chunk> cs = bk::chunks(dat);
		for (size_t i = 0; i < cs.size(); ++i)
		{
			const bk::chunk &c = cs[i];
			if (c.type != 0x20 || c.size < 69 || c.body[0] != 0xA1)
				continue;
			const int32_t w = i32(c.body + 21), h = std::abs(i32(c.body + 25));
			if (w <= 0 || h <= 0 || w % 4 != 0 || h % 4 != 0)
				continue;
			a1_texture t;
			t.k = pk::key_of(c.body, c.size);
			t.chunk = uint32_t(i), t.w = uint32_t(w), t.h = uint32_t(h);
			t.name.assign(reinterpret_cast<const char *>(c.body + 1), 16);
			out.push_back(t);
		}
		return out;
	}

	bool copy_into(const std::wstring &from, const std::wstring &to)
	{
		make_folders(to.substr(0, to.find_last_of(L'\\')));
		return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
	}

	// A pack folder's files copied into a fresh folder (not its lock or work folder)
	bool copy_pack(const std::wstring &from, const std::wstring &to)
	{
		remove_tree(to);
		make_folders(to);
		bool ok = true;
		for (const wchar_t *n : { L"pack.idx", L"pack.src", L"pack-000.bin", L"pack-001.bin", L"dats.tsv" })
			if (is_there(from + L"\\" + n))
				ok = ok && copy_into(from + L"\\" + n, to + L"\\" + n);
		return ok;
	}

	// A record's level 0 as BC4 decoded to 8-bit texels
	std::vector<uint8_t> height_level0(const std::shared_ptr<const pk::index> &idx, const pk::record &r)
	{
		const std::vector<std::vector<uint8_t>> lv = levels_of(idx, r);
		return lv.empty() ? std::vector<uint8_t> {} : pk::bc4::decode(lv[0].data(), r.w, r.h);
	}

	// ---- (M9) Height records: BC4, the height maker, texpack heights, make --heights
	void heights_unit_tests()
	{
		// BC4 round trip: every texel within max_error of its block's range, on noise of several ranges, gradients,
		// blocks touching 0 and 255, flat blocks
		{
			std::mt19937 rng(20261103);
			uint32_t blocks = 0, over = 0, flat_bad = 0;
			double sum = 0;
			uint64_t n = 0;
			for (int pic = 0; pic < 24; ++pic)
			{
				const uint32_t w = 64, h = 32;
				std::vector<uint8_t> t(size_t(w) * h);
				const int kind = pic % 6;
				for (uint32_t y = 0; y < h; ++y)
					for (uint32_t x = 0; x < w; ++x)
					{
						int v = 0;
						switch (kind)
						{
						case 0: v = int(rng() % 256); break;                                       // noise, the whole range
						case 1: v = 120 + int(rng() % 16); break;                                   // a narrow band
						case 2: v = int((x * 4 + y * 2) % 256); break;                               // a gradient
						case 3: v = (rng() % 5 == 0) ? (rng() % 2 ? 0 : 255) : 100 + int(rng() % 40); break; // 0 and 255 among the rest
						case 4: v = 77; break;                                                       // flat
						default: v = 128 + int(60 * std::sin(x * 0.3) * std::cos(y * 0.2)); break;   // smooth
						}
						t[size_t(y) * w + x] = uint8_t(std::clamp(v, 0, 255));
					}
				const std::vector<uint8_t> enc = pk::bc4::encode(t.data(), w, h);
				const std::vector<uint8_t> dec = pk::bc4::decode(enc.data(), w, h);
				for (uint32_t by = 0; by < h / 4; ++by)
					for (uint32_t bx = 0; bx < w / 4; ++bx)
					{
						int lo = 255, hi = 0;
						for (int k = 0; k < 16; ++k)
						{
							const int v = t[(size_t(by) * 4 + k / 4) * w + bx * 4 + k % 4];
							lo = std::min(lo, v), hi = std::max(hi, v);
						}
						blocks++;
						for (int k = 0; k < 16; ++k)
						{
							const size_t i = (size_t(by) * 4 + k / 4) * w + bx * 4 + k % 4;
							const int d = std::abs(int(dec[i]) - int(t[i]));
							over += d > pk::bc4::max_error(lo, hi);
							flat_bad += lo == hi && d != 0;
							sum += d, n++;
						}
					}
			}
			check(over == 0 && flat_bad == 0 && pk::bc4::encode(nullptr, 6, 4).empty(),
				"BC4 round trip: " + std::to_string(blocks) + " blocks (noise, bands, gradients, 0/255, flat, smooth), every texel within (range+13)/14+1 levels, "
				"flat blocks exact; mean error " + std::to_string(sum / double(n)).substr(0, 5) + " levels");
		}
		// The maker's sizes (open question 11)
		{
			namespace ht = pk::height;
			uint32_t w = 0, h = 0;
			bool ok = ht::size_for(ht::scale::half, 128, 64, 64, 32, w, h) && w == 64 && h == 32;
			ok = ok && ht::size_for(ht::scale::half, 256, 128, 64, 32, w, h) && w == 128 && h == 64;
			ok = ok && ht::size_for(ht::scale::full, 256, 128, 64, 32, w, h) && w == 256 && h == 128;
			ok = ok && ht::size_for(ht::scale::game, 256, 128, 64, 32, w, h) && w == 64 && h == 32;
			ok = ok && ht::size_for(ht::scale::half, 64, 32, 64, 32, w, h) && w == 64 && h == 32; // (no pack colour: the original's size)
			ok = ok && !ht::size_for(ht::scale::game, 12, 8, 6, 4, w, h);                           // (6x4: no BC4 of that)
			check(ok, "height sizes: half = half the colour (never under the original), full = the colour, game = the original; none for sides not multiples of 4");
		}
		// Coverage: what's in a cut-out's holes changes nothing where the game shows the texture; flat: span 0
		{
			namespace ht = pk::height;
			const uint32_t w = 64, h = 64;
			std::vector<uint8_t> a(size_t(w) * h * 4), b;
			std::mt19937 rng(7);
			for (uint32_t i = 0; i < w * h; ++i)
			{
				const uint8_t v = uint8_t(100 + rng() % 60);
				a[i * 4] = a[i * 4 + 1] = a[i * 4 + 2] = v;
				a[i * 4 + 3] = (i % w) < 40 ? 0x80 : 0; // (the right part a hole)
			}
			b = a;
			for (uint32_t i = 0; i < w * h; ++i)
				if (b[i * 4 + 3] == 0)
					b[i * 4] = b[i * 4 + 1] = b[i * 4 + 2] = uint8_t(rng() % 2 ? 0 : 255);
			ht::field fa, fb;
			std::string why;
			const float cut = pk::bc2::zone_cut_threshold(0x80);
			const bool made = ht::from_colour(a.data(), w, h, w, w, h, pk::kAlphaCut, cut, fa, why) && ht::from_colour(b.data(), w, h, w, w, h, pk::kAlphaCut, cut, fb, why);
			float worst = 0, range = 0;
			for (uint32_t i = 0; made && i < w * h; ++i)
				if (fa.wt[i] > 0)
					worst = std::max(worst, std::fabs(fa.v[i] - fb.v[i])), range = std::max(range, std::fabs(fa.v[i]));
			// (and the plain blur would have let the holes in: the same with every texel counted)
			ht::field pa, pb;
			ht::from_colour(a.data(), w, h, w, w, h, pk::kAlphaNone, cut, pa, why);
			ht::from_colour(b.data(), w, h, w, w, h, pk::kAlphaNone, cut, pb, why);
			float plain = 0;
			for (uint32_t i = 0; i < w * h; ++i)
				if (fa.wt[i] > 0)
					plain = std::max(plain, std::fabs(pa.v[i] - pb.v[i]));
			std::vector<uint8_t> flat(size_t(w) * h * 4, 0x60);
			ht::field ff;
			ht::from_colour(flat.data(), w, h, w, w, h, pk::kAlphaNone, 0, ff, why);
			const ht::made fm = ht::encode(ff);
			const std::vector<uint8_t> f0 = fm.levels.empty() ? std::vector<uint8_t> {} : pk::bc4::decode(fm.levels[0].data(), w, h);
			check(made && worst < 1e-4f * std::max(range, 1e-3f) + 1e-6f && plain > 100 * worst && fm.span == 0 && !f0.empty() &&
					std::all_of(f0.begin(), f0.end(), [](uint8_t v) { return v == 128; }) && fm.levels.size() == pk::bc2::chain_levels(w, h),
				"heights are coverage-aware: a cut-out's holes painted black and white change the shown texels by " + std::to_string(worst) +
					" (counting every texel: " + std::to_string(plain) + "); a flat colour gives span 0, every texel 0.5, the whole chain");
		}
	}

	// The pack heights is tried on: ROM\0\100 (zone 150: twelve of its textures have bump maps in ROM\92\51) at 2x and
	// ROM\310\0 (a model DAT) at 4x, nearest-upscaled originals as the colour, made here with the writer
	bool write_height_test_pack(const std::wstring &folder, size_t &entries)
	{
		remove_tree(folder);
		pk::writer w;
		pk::writer_options wo;
		wo.folder = folder;
		wo.maker_version = 2;
		wo.source = "texpack_test heights";
		std::string why;
		if (!w.open(wo, why))
			return false;
		for (const auto &[rel, f, kind] : { std::tuple{ kZone, 2u, pk::kSourceZone }, std::tuple{ kModel, 4u, pk::kSourceModel } })
		{
			const std::vector<uint8_t> dat = read(kFFXI + L"\\" + wide(rel));
			const std::vector<bk::chunk> cs = bk::chunks(dat);
			for (size_t i = 0; i < cs.size(); ++i)
			{
				const bk::chunk &c = cs[i];
				if (c.type != 0x20 || c.size < 69 || c.body[0] != 0xA1 || std::memcmp(c.body + 57, "3TXD", 4) != 0)
					continue;
				const int32_t ow = i32(c.body + 21), oh = i32(c.body + 25);
				if (ow <= 0 || oh <= 0 || ow % 4 || oh % 4 || 69 + size_t(ow / 4) * (oh / 4) * 16 > c.size)
					continue;
				const std::vector<uint8_t> px = pk::bc2::decode_bc2(c.body + 69, uint32_t(ow), uint32_t(oh));
				const uint32_t W = uint32_t(ow) * f, H = uint32_t(oh) * f;
				std::vector<uint8_t> up(size_t(W) * H * 4);
				for (uint32_t y = 0; y < H; ++y)
					for (uint32_t x = 0; x < W; ++x)
						std::memcpy(&up[(size_t(y) * W + x) * 4], &px[(size_t(y / f) * uint32_t(ow) + x / f) * 4], 4);
				const std::vector<uint8_t> blocks = pk::bc2::encode_bc2(up.data(), W, H);
				pk::entry e;
				const pk::key k = pk::key_of(c.body, c.size);
				e.hash = k.hash, e.bytes = k.bytes, e.w = k.w, e.h = k.h;
				e.orig_format = 0xA1;
				std::memcpy(&e.orig_fourcc, c.body + 57, 4);
				e.alpha_kind = pk::kAlphaNone;
				e.source_kind = kind;
				e.file_id = 0xFFFFFFFFu;
				e.chunk = uint32_t(i);
				std::memcpy(e.name, c.body + 1, 16);
				pk::map_in m;
				m.w = W, m.h = H;
				m.levels = pk::bc2::full_chain(blocks.data(), W, H, pk::kAlphaNone, 0);
				w.add(e, { m }, why);
			}
			WIN32_FILE_ATTRIBUTE_DATA a = {};
			GetFileAttributesExW((kFFXI + L"\\" + wide(rel)).c_str(), GetFileExInfoStandard, &a);
			w.finished(rel, (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow, (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime);
		}
		entries = w.entries().size();
		const bool ok = w.commit(why);
		w.close();
		return ok;
	}

	void heights_tests()
	{
		heights_unit_tests();
		const std::wstring hw = work + L"\\heights", T0 = hw + L"\\T0";
		size_t n0 = 0;
		const bool written = write_height_test_pack(T0, n0);
		std::shared_ptr<const pk::index> t0 = load(T0);
		check(written && t0 != nullptr && n0 > 10, "a test pack of ROM\\0\\100 (2x) and ROM\\310\\0 (4x): " + std::to_string(n0) + " entries");
		if (!t0)
			return;
		const std::vector<uint8_t> t0_idx = read(T0 + L"\\pack.idx");
		// (the zone's textures the game has height maps for, read here independently: ROM\92\51's 0x5D chunks)
		std::map<std::string, std::tuple<uint32_t, uint32_t, std::vector<uint8_t>>> bumps;
		{
			const std::vector<uint8_t> b = read(kFFXI + L"\\ROM\\92\\51.DAT");
			for (const bk::chunk &c : bk::chunks(b))
				if (c.type == 0x5D && c.size >= 0x20 && c.body[0] == 'b' && c.body[1] == 'p')
				{
					const uint32_t w = c.body[4] | (c.body[5] << 8), h = c.body[6] | (c.body[7] << 8);
					const std::string nm(reinterpret_cast<const char *>(c.body) + 0x18, 8);
					if (w && h && 0x20 + size_t(w) * h <= c.size && bumps.count(nm) == 0)
						bumps[nm] = { w, h, std::vector<uint8_t>(c.body + 0x20, c.body + 0x20 + size_t(w) * h) };
				}
		}
		size_t want_game = 0;
		for (const pk::entry &e : t0->entries)
			want_game += e.source_kind == pk::kSourceZone && bumps.count(std::string(e.name + 8, 8)) != 0;
		auto heights_args = [&](const std::wstring &pack, const std::string &more) {
			return "heights --pack " + q(pack) + " --ffxi " + q(kFFXI) + " --threads 4 --reserve-gb 0 --ignore-game " + more;
		};

		// T1: heights at the default (half)
		const std::wstring T1 = hw + L"\\T1";
		copy_pack(T0, T1);
		int code = 0;
		std::string out = run(heights_args(T1, ""), code);
		std::shared_ptr<const pk::index> t1 = load(T1);
		check(code == 0 && t1 && t1->entries.size() == n0, "heights: exit 0, the pack loads (" + line_with(out, "heights: ") + ")");
		if (!t1)
		{
			std::printf("%s", out.c_str());
			return;
		}
		{
			bool all = true, colour_same = true, fields_same = true, rules = true;
			size_t flagged = 0, flagged_right = 0;
			for (const pk::entry &e : t1->entries)
			{
				const pk::entry *b = t0->find(e.the_key());
				const pk::record *h = e.map(pk::kHeight), *c = e.map(pk::kColour);
				const uint32_t f = e.source_kind == pk::kSourceZone ? 2 : 4;
				all = all && h != nullptr && h->format == pk::kBC4 && h->w == e.w * f / 2 && h->h == e.h * f / 2 && h->levels == pk::bc2::chain_levels(h->w, h->h) &&
					std::isfinite(h->span()) && h->span() >= 0;
				rules = rules && h != nullptr && pk::check(*h) == nullptr;
				colour_same = colour_same && b && c && std::memcmp(c, b->map(pk::kColour), sizeof(pk::record)) == 0;
				// (every field but the flags as they were)
				pk::entry x = e;
				if (b)
					x.flags = b->flags;
				fields_same = fields_same && b && std::memcmp(&x, b, 64) == 0;
				if (e.flags & pk::kHeightFromGame)
				{
					flagged++;
					flagged_right += e.source_kind == pk::kSourceZone && bumps.count(std::string(e.name + 8, 8)) != 0;
				}
			}
			check(all && rules, "every entry has a BC4 height record at half the colour (zones 1x, models 2x), the whole chain, a span 0 or above, within the rules");
			check(colour_same && fields_same, "colour records unchanged byte for byte (file, offset, CRC), and every entry field but the flags");
			check(flagged == want_game && flagged_right == flagged && want_game >= 10,
				"height_from_game on exactly the zone's textures with a bump map of their name: " + std::to_string(flagged) + " of " + std::to_string(want_game));
			int vc = 0;
			const std::string v = run("verify --pack " + q(T1) + " --ffxi " + q(kFFXI), vc);
			check(vc == 0, "verify reads every record back, heights too (" + last_line(v) + ")");
			const std::string o = oracle(T1, vc);
			check(vc == 0, "pack_oracle.py reads it: " + last_line(o));
			const std::string progress = text_of(T1 + L"\\progress.txt");
			check(progress.find("state: finished") != std::string::npos && progress.find("heights_from_game: " + std::to_string(want_game)) != std::string::npos &&
					!is_there(T1 + L"\\pack.lock") && pack_src(T1) == pack_src(T0),
				"progress.txt finished, pack.lock gone, pack.src untouched");
		}
		// The pack it was copied from: no heights, its index as written (packs without heights read as before)
		{
			bool none = true;
			for (const pk::entry &e : t0->entries)
				none = none && e.map(pk::kHeight) == nullptr && (e.flags & pk::kHeightFromGame) == 0;
			check(none && read(T0 + L"\\pack.idx") == t0_idx, "a pack without heights: none in it, its pack.idx unchanged");
		}
		// Resume: --limit 1 (the zone first), then the rest; the same records as in one run
		{
			const std::wstring T2 = hw + L"\\T2";
			copy_pack(T0, T2);
			int c1 = 0, c2 = 0;
			const std::string o1 = run(heights_args(T2, "--limit 1"), c1);
			std::shared_ptr<const pk::index> mid = load(T2);
			size_t zone_h = 0, model_h = 0, zones = 0;
			if (mid)
				for (const pk::entry &e : mid->entries)
				{
					zones += e.source_kind == pk::kSourceZone;
					(e.source_kind == pk::kSourceZone ? zone_h : model_h) += e.map(pk::kHeight) != nullptr;
				}
			const std::string o2 = run(heights_args(T2, ""), c2);
			std::shared_ptr<const pk::index> t2 = load(T2);
			bool same = t2 && t2->entries.size() == t1->entries.size();
			for (size_t i = 0; same && i < t2->entries.size(); ++i)
			{
				const pk::record *a = t2->entries[i].map(pk::kHeight), *b = t1->entries[i].map(pk::kHeight);
				same = a && b && a->crc == b->crc && a->param == b->param && a->bytes == b->bytes && t2->entries[i].flags == t1->entries[i].flags;
			}
			check(c1 == 0 && c2 == 0 && zone_h == zones && model_h == 0 && o2.find(std::to_string(zones) + " there already") != std::string::npos && same,
				"resume: --limit 1 does the zone (" + std::to_string(zone_h) + " heights, the models none), the next run the rest (" + line_with(o2, "there already") +
					"); the same records as one run");
			int c3 = 0;
			const std::string o3 = run(heights_args(T2, ""), c3);
			check(c3 == 0 && o3.find(": 0 made") != std::string::npos && o3.find(std::to_string(n0) + " there already") != std::string::npos,
				"a rerun makes nothing (" + line_with(o3, "there already") + ")");
		}
		// The stop file, the game, a missing pack
		{
			const std::wstring T3 = hw + L"\\T3";
			copy_pack(T0, T3);
			write_text(T3 + L"\\stop", "");
			int c = 0;
			std::string o = run("heights --pack " + q(T3) + " --ffxi " + q(kFFXI) + " --reserve-gb 0 --ignore-game", c);
			check(c == 3 && read(T3 + L"\\pack.idx") == t0_idx && !is_there(T3 + L"\\pack.lock"), "the stop file: exit 3, nothing written (" + line_with(o, "Stopped") + ")");
			DeleteFileW((T3 + L"\\stop").c_str());
			o = run("heights --pack " + q(T3) + " --ffxi " + q(kFFXI) + " --reserve-gb 0 --no-wait --game-names texpack_test.exe", c);
			check(c == 4 && read(T3 + L"\\pack.idx") == t0_idx, "the game running (--no-wait): exit 4, nothing written (" + line_with(o, "Stopped") + ")");
			o = run("heights --pack " + q(hw + L"\\nowhere") + " --ffxi " + q(kFFXI) + " --ignore-game", c);
			check(c == 1 && o.find("can't be used") != std::string::npos, "no pack there: exit 1 (" + last_line(o) + ")");
			o = run(heights_args(T3, "--height-scale quarter"), c);
			check(c == 2, "--height-scale quarter: a usage error");
		}
		// Full and game resolution; the game-height path against the game's own normals
		{
			const std::wstring T4 = hw + L"\\T4", T5 = hw + L"\\T5";
			copy_pack(T0, T4);
			copy_pack(T0, T5);
			int c4 = 0, c5 = 0;
			run(heights_args(T4, "--height-scale full"), c4);
			run(heights_args(T5, "--height-scale game --kind zone"), c5);
			std::shared_ptr<const pk::index> t4 = load(T4), t5 = load(T5);
			bool full = t4 != nullptr, game = t5 != nullptr, kind = true;
			uint64_t bytes[3] = {}, texels[3] = {};
			for (size_t i = 0; full && i < t4->entries.size(); ++i)
			{
				const pk::record *h = t4->entries[i].map(pk::kHeight), *c = t4->entries[i].map(pk::kColour);
				full = full && h && h->w == c->w && h->h == c->h;
				if (h)
					bytes[0] += h->bytes, texels[0] += uint64_t(h->w) * h->h;
			}
			for (const pk::entry &e : t1->entries)
				if (const pk::record *h = e.map(pk::kHeight))
					bytes[1] += h->bytes, texels[1] += uint64_t(h->w) * h->h;
			for (size_t i = 0; game && i < t5->entries.size(); ++i)
			{
				const pk::entry &e = t5->entries[i];
				const pk::record *h = e.map(pk::kHeight);
				kind = kind && (e.source_kind == pk::kSourceZone) == (h != nullptr);
				game = game && (!h || (h->w == e.w && h->h == e.h));
				if (h)
					bytes[2] += h->bytes, texels[2] += uint64_t(h->w) * h->h;
			}
			char sizes[256];
			std::snprintf(sizes, sizeof(sizes), "bytes a map texel with mips and pitch: full %.3f, half %.3f, game %.3f", double(bytes[0]) / double(texels[0] ? texels[0] : 1),
				double(bytes[1]) / double(texels[1] ? texels[1] : 1), double(bytes[2]) / double(texels[2] ? texels[2] : 1));
			check(c4 == 0 && c5 == 0 && full && game && kind, std::string("--height-scale full: the colour's size; game: the original's; --kind zone: the zone's only (") + sizes + ")");

			// (the stored map, at the game's size, made back into normals as the game makes them, against the game's own from
			// the bump bytes: the angle between them)
			double sum = 0, worst_mean = 0;
			std::vector<double> all;
			uint32_t compared = 0;
			for (const pk::entry &e : t5 ? t5->entries : std::vector<pk::entry> {})
			{
				const pk::record *h = e.map(pk::kHeight);
				auto b = bumps.find(std::string(e.name + 8, 8));
				if (!h || !(e.flags & pk::kHeightFromGame) || b == bumps.end() || std::get<0>(b->second) != h->w || std::get<1>(b->second) != h->h)
					continue;
				const std::vector<uint8_t> s = height_level0(t5, *h);
				const uint32_t w = h->w, hh = h->h;
				std::vector<float> ours(size_t(w) * hh), theirs(size_t(w) * hh);
				for (size_t i = 0; i < ours.size(); ++i)
					ours[i] = (float(s[i]) / 255.0f - 0.5f) * h->span(), theirs[i] = float(std::get<2>(b->second)[i]) / 255.0f;
				std::vector<float> na, nb;
				pk::height::normals(ours.data(), w, hh, 1.0f, na);
				pk::height::normals(theirs.data(), w, hh, 1.0f, nb);
				double m = 0;
				for (size_t i = 0; i < ours.size(); ++i)
				{
					const double d = std::clamp(double(na[i * 3] * nb[i * 3] + na[i * 3 + 1] * nb[i * 3 + 1] + na[i * 3 + 2] * nb[i * 3 + 2]), -1.0, 1.0);
					const double deg = std::acos(d) * 180.0 / 3.14159265358979;
					all.push_back(deg), m += deg;
				}
				m /= double(ours.size());
				sum += m, worst_mean = std::max(worst_mean, m);
				compared++;
			}
			std::sort(all.begin(), all.end());
			const double p99 = all.empty() ? 0 : all[size_t(double(all.size() - 1) * 0.99)];
			char msg[256];
			std::snprintf(msg, sizeof(msg), "the game-height path: %u of ROM\\0\\100's bump-mapped textures, their normals from the stored map against the game's: "
				"mean %.2f deg (worst texture %.2f), p99 %.2f deg", compared, compared ? sum / compared : 0.0, worst_mean, p99);
			check(compared >= 8 && sum / std::max(compared, 1u) < 2.0 && worst_mean < 4.0 && p99 < 10.0, msg);
		}
	}

	void make_tests(const std::shared_ptr<const pk::index> &convert_pack)
	{
		ported_steps();
		const std::wstring mw = work + L"\\make", ffxi = mw + L"\\ffxi", up = mw + L"\\upscaler", fake = root + L"\\build\\fake_esrgan.exe";
		const std::wstring fake_log = mw + L"\\fake-esrgan.log", game_exe = mw + L"\\game\\" + kFakeGame;
		remove_tree(mw);
		make_folders(mw);

		// get-upscaler: what it fetches said, a file of another size or another hash refused and deleted (no network:
		// a local file:// URL; the official file is never downloaded here)
		{
			int code = 0;
			const std::wstring bad = mw + L"\\bad.zip", dir = mw + L"\\got";
			write_text(bad, "not the release");
			std::string out = run("get-upscaler --yes --dir " + q(dir) + " --url \"file:///" + narrow(bad) + "\"", code);
			check(code == 1 && out.find("not the official 45,474,481") != std::string::npos && out.find("BSD-3-Clause") != std::string::npos &&
					out.find("abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d") != std::string::npos,
				"get-upscaler: says what, where from, the licences and the hash; a file of another size refused (" + line_with(out, "refused") + ")");
			check(!is_there(dir) && !is_there(dir + L".part"), "get-upscaler: nothing left of the refused file");
			// (the official size, other bytes)
			HANDLE f = CreateFileW(bad.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
			LARGE_INTEGER size;
			size.QuadPart = 45474481;
			SetFilePointerEx(f, size, nullptr, FILE_BEGIN);
			SetEndOfFile(f);
			CloseHandle(f);
			out = run("get-upscaler --yes --dir " + q(dir) + " --url \"file:///" + narrow(bad) + "\"", code);
			check(code == 1 && out.find("Its SHA-256 is ") != std::string::npos && out.find("refused, deleted") != std::string::npos && !is_there(dir) &&
					!is_there(dir + L".part"),
				"get-upscaler: the official size with another SHA-256 refused and deleted (" + line_with(out, "SHA-256 is") + ")");
			DeleteFileW(bad.c_str());
			out = run("get-upscaler --dir " + q(dir) + " --url \"file:///" + narrow(bad) + "\" < NUL", code);
			check(code == 1 && out.find("Nothing downloaded") != std::string::npos && !is_there(dir.c_str()), "get-upscaler: no answer, nothing downloaded");
			// (a <dir>.part that isn't get-upscaler's is left as it is)
			make_folders(dir + L".part");
			write_text(dir + L".part\\mine.txt", "the player's");
			out = run("get-upscaler --yes --dir " + q(dir) + " --url \"file:///" + narrow(bad) + "\"", code);
			check(code == 1 && out.find("isn't get-upscaler's") != std::string::npos && is_there(dir + L".part\\mine.txt") && !is_there(dir),
				"get-upscaler: a .part folder without its mark left alone, exit 1 (" + line_with(out, "isn't") + ")");
			remove_tree(dir + L".part");
			// (the official zip, only when HIFIXI_ESRGAN_ZIP names a copy of it: what's kept)
			wchar_t zip[MAX_PATH] = L"";
			if (GetEnvironmentVariableW(L"HIFIXI_ESRGAN_ZIP", zip, MAX_PATH) != 0)
			{
				std::wstring url = L"file:///";
				url += zip;
				out = run("get-upscaler --yes --dir " + q(dir) + " --url \"" + narrow(url) + "\"", code);
				check(code == 0 && is_there(dir + L"\\realesrgan-ncnn-vulkan.exe") && is_there(dir + L"\\vcomp140.dll") && !is_there(dir + L"\\vcomp140d.dll") &&
						is_there(dir + L"\\models\\realesrgan-x4plus.bin") && is_there(dir + L"\\models\\realesrgan-x4plus.param") &&
						!is_there(dir + L"\\models\\realesr-animevideov3-x2.bin") && !is_there(dir + L"\\input.jpg") && is_there(dir + L"\\PROVENANCE.txt") &&
						!is_there(dir + L".part"),
					"get-upscaler on the official zip (HIFIXI_ESRGAN_ZIP): the program, vcomp140.dll, x4plus and PROVENANCE.txt kept; no vcomp140d.dll, other models "
					"or samples (" + last_line(out) + ")");
			}
			else
				std::printf("      (get-upscaler on the official zip not tried: HIFIXI_ESRGAN_ZIP isn't set)\n");
		}

		// The fake game folder: the tables and five DATs (two zones that share textures, three model DATs)
		bool copied = copy_into(kFFXI + L"\\VTABLE.DAT", ffxi + L"\\VTABLE.DAT") && copy_into(kFFXI + L"\\FTABLE.DAT", ffxi + L"\\FTABLE.DAT");
		for (int r = 2; r < 10; ++r)
		{
			const std::wstring n = std::to_wstring(r);
			copy_into(kFFXI + L"\\ROM" + n + L"\\VTABLE" + n + L".DAT", ffxi + L"\\ROM" + n + L"\\VTABLE" + n + L".DAT");
			copy_into(kFFXI + L"\\ROM" + n + L"\\FTABLE" + n + L".DAT", ffxi + L"\\ROM" + n + L"\\FTABLE" + n + L".DAT");
		}
		for (const char *rel : kMakeZones)
			copied = copied && copy_into(kFFXI + L"\\" + wide(rel), ffxi + L"\\" + wide(rel));
		for (const char *rel : kMakeModels)
			copied = copied && copy_into(kFFXI + L"\\" + wide(rel), ffxi + L"\\" + wide(rel));
		copied = copied && copy_into(fake, up + L"\\realesrgan-ncnn-vulkan.exe") && write_text(up + L"\\vcomp140.dll", "fake") &&
			write_text(up + L"\\models\\realesrgan-x4plus.param", "fake") &&
			write_text(up + L"\\models\\realesrgan-x4plus.bin", "fake") && copy_into(fake, game_exe);
		check(copied, "a fake game folder (tables and five DATs) and the fake upscaler");

		// What make should give: every A1 texture of the five DATs once (the first DAT's), zones 2x, models 4x
		std::map<pk::key, std::pair<a1_texture, std::string>> want;
		std::set<pk::key> keys66, keys72;
		for (const char *rel : kMakeZones)
			for (const a1_texture &t : a1_textures(rel))
			{
				want.emplace(t.k, std::make_pair(t, std::string(rel)));
				(rel == kMakeZones[0] ? keys66 : keys72).insert(t.k);
			}
		for (const char *rel : kMakeModels)
			for (const a1_texture &t : a1_textures(rel))
				want.emplace(t.k, std::make_pair(t, std::string(rel)));
		size_t shared = 0;
		for (const pk::key &k : keys66)
			shared += keys72.count(k);

		// (every make here: no Python on PATH, the game the fake one, timings short)
		wchar_t old_path[32767] = L"";
		GetEnvironmentVariableW(L"PATH", old_path, 32767);
		wchar_t system[MAX_PATH] = L"";
		GetSystemDirectoryW(system, MAX_PATH);
		SetEnvironmentVariableW(L"PATH", system);
		SetEnvironmentVariableW(L"FAKE_ESRGAN_LOG", fake_log.c_str());
		SetEnvironmentVariableW(L"FAKE_ESRGAN_MODE", nullptr);
		auto args = [&](const std::wstring &pack, const std::string &more) {
			return "make --out " + q(pack) + " --ffxi " + q(ffxi) + " --ffximain " + q(kFFXI + L"\\FFXiMain.dll") + " --upscaler " + q(up) +
				" --threads 4 --reserve-gb 0 --game-names " + narrow(kFakeGame) + " --game-poll-s 0.2 --game-quiet-s 2 " + more;
		};
		auto make_run = [&](const std::wstring &pack, const std::string &more, std::string &out, DWORD ms = 300000) {
			started s = start(tool, wide(args(pack, more)), mw + L"\\make.out");
			const int code = finish(s, ms);
			out = text_of(mw + L"\\make.out");
			return code;
		};
		std::string out;
		int code = 0;
		{
			char py[MAX_PATH];
			check(SearchPathA(nullptr, "python.exe", nullptr, MAX_PATH, py, nullptr) == 0, "make runs with no Python on PATH (PATH is System32 only)");
		}

		// No upscaler: says to fetch it
		code = make_run(mw + L"\\none", "--upscaler " + q(mw + L"\\nowhere"), out);
		check(code == 1 && out.find("get-upscaler") != std::string::npos && !is_there(mw + L"\\none\\pack.idx"),
			"make without the upscaler: exit 1, says to run get-upscaler (" + line_with(out, "upscaler") + ")");

		// P1: the whole fake folder in one run
		const std::wstring P1 = mw + L"\\P1";
		code = make_run(P1, "", out);
		std::shared_ptr<const pk::index> p1 = load(P1);
		check(code == 0 && p1 != nullptr, "make: exit 0, the pack loads (" + line_with(out, "DATs 5") + ")");
		if (!p1)
		{
			std::printf("%s", out.c_str());
			SetEnvironmentVariableW(L"PATH", old_path);
			return;
		}
		{
			const std::string tsv = text_of(P1 + L"\\dats.tsv");
			check(tsv.rfind("# hifixi-texpack make's list of ", 0) == 0 && tsv.find("ROM\\1\\66.DAT\tzone\t") != std::string::npos &&
					tsv.find("ROM\\1\\72.DAT\tzone\t") != std::string::npos && tsv.find("ROM\\310\\0.DAT\tmodel\t") != std::string::npos &&
					tsv.find("ROM3\\5\\110.DAT\tmodel\t") != std::string::npos && count_lines(tsv) == 6,
				"dats.tsv made in the pack's folder from the game's ROM folders: the zones by file id, the models by their Os2");
			const std::string src = pack_src(P1);
			check(count_lines(src) == 5 && src.find("\t2\t0\t0") != std::string::npos, "pack.src: the five DATs, maker version 2, no overlay");
			check(std::string(p1->head.source).rfind("texpack make 2", 0) == 0 && p1->head.maker_version == 2, "the header says texpack make 2");
			bool keys = p1->entries.size() == want.size(), fields = true, sizes = true;
			for (const auto &[k, tw] : want)
			{
				const pk::entry *e = p1->find(k);
				if (e == nullptr)
				{
					keys = false;
					continue;
				}
				const bool zone = tw.second.find("ROM\\1\\") == 0;
				const pk::record *r = e->map(pk::kColour);
				fields = fields && e->chunk == tw.first.chunk && std::memcmp(e->name, tw.first.name.data(), 16) == 0 && e->orig_format == 0xA1 &&
					e->source_kind == (zone ? pk::kSourceZone : pk::kSourceModel) && (e->flags & pk::kNoPack) == 0;
				sizes = sizes && r != nullptr && r->w == tw.first.w * (zone ? 2 : 4) && r->h == tw.first.h * (zone ? 2 : 4) && r->format == pk::kBC2 &&
					r->levels == pk::bc2::chain_levels(r->w, r->h);
			}
			check(keys, "make's keys: every A1 texture of the five DATs once (" + std::to_string(p1->entries.size()) + " entries, " + std::to_string(want.size()) +
					" wanted; the two zones share " + std::to_string(shared) + ")");
			check(fields, "entries: chunk, name, format and kind from the first DAT with the key; none no_pack");
			check(sizes, "levels: zones 2x, models 4x, BC2, the whole chain of coverage mips");
			// (the model DATs' alpha kinds as convert tagged them: the same tags, the same keys)
			if (convert_pack)
			{
				uint32_t same = 0, compared = 0;
				for (const pk::entry &e : p1->entries)
					if (e.source_kind == pk::kSourceModel)
						if (const pk::entry *c = convert_pack->find(e.the_key()))
							compared++, same += c->alpha_kind == e.alpha_kind && c->file_id == e.file_id && c->chunk == e.chunk;
				check(compared >= 6 && same == compared, "model entries' alpha kinds, file ids and chunks as convert's pack has them (" + std::to_string(same) + " of " +
						std::to_string(compared) + ")");
			}
			check(out.find("already in the pack " + std::to_string(shared)) != std::string::npos,
				"the shared textures made once, skipped before upscaling (" + line_with(out, "already in the pack") + ")");
			int vc = 0;
			const std::string v = run("verify --pack " + q(P1) + " --soul --ffxi " + q(ffxi) + " --threads 4", vc);
			check(vc == 0 && v.find(", 0 bad") != std::string::npos && v.find("0 over the limits") != std::string::npos,
				"verify --soul passes it (" + line_with(v, "soul: ") + ")");
			// (M9) make without --heights writes none; make --heights writes what make and then heights write
			{
				bool none = true;
				for (const pk::entry &e : p1->entries)
					none = none && e.map(pk::kHeight) == nullptr && (e.flags & pk::kHeightFromGame) == 0;
				check(none, "make without --heights: no height records (off until M9 is approved)");
				const std::wstring PH = mw + L"\\PH", PC = mw + L"\\PC";
				copy_pack(P1, PC);
				int hc = 0;
				const std::string ho = run("heights --pack " + q(PC) + " --ffxi " + q(ffxi) + " --threads 4 --reserve-gb 0 --ignore-game", hc);
				std::string mo;
				const int mc = make_run(PH, "--heights", mo);
				std::shared_ptr<const pk::index> pc = load(PC), ph = load(PH);
				bool same = pc && ph && pc->entries.size() == ph->entries.size() && pc->entries.size() == p1->entries.size();
				for (size_t i = 0; same && i < pc->entries.size(); ++i)
				{
					const pk::entry &x = pc->entries[i], &y = ph->entries[i];
					const pk::record *hx = x.map(pk::kHeight), *hy = y.map(pk::kHeight);
					same = hx && hy && hx->crc == hy->crc && hx->param == hy->param && hx->bytes == hy->bytes && x.flags == y.flags &&
						x.map(pk::kColour)->crc == y.map(pk::kColour)->crc;
				}
				check(hc == 0 && mc == 0 && same, "make --heights: every entry's height record the same bytes and span as make, then heights, give (" +
						line_with(mo, "heights (") + ")");
			}
			const std::string progress = text_of(P1 + L"\\progress.txt");
			check(progress.find("state: finished") != std::string::npos && progress.find("dats_done: 5") != std::string::npos &&
					progress.find("dats_total: 5") != std::string::npos && progress.find("summary: 5 of 5 DATs done") != std::string::npos,
				"progress.txt: plain and key: value (state, dats_done, dats_total, summary)");
			code = make_run(P1, "", out);
			std::shared_ptr<const pk::index> again = load(P1);
			check(code == 0 && out.find("0 DATs (0 zones, 0 models) to make, 5 done before") != std::string::npos && out.find("dats.tsv made") == std::string::npos &&
					again && same_entries(*p1, *again),
				"a rerun: nothing to make, dats.tsv kept (" + line_with(out, "to make") + ")");
			size_t zone_keys = 0;
			for (const pk::entry &e : p1->entries)
				zone_keys += e.source_kind == pk::kSourceZone;
			code = make_run(P1, "--rescan --kind zone --redo", out);
			check(code == 0 && out.find("dats.tsv made (--rescan)") != std::string::npos && out.find("2 DATs (2 zones, 0 models) to make") != std::string::npos &&
					out.find("textures made 0") != std::string::npos && out.find("the same key again " + std::to_string(zone_keys) + ",") != std::string::npos,
				"--rescan --kind zone --redo: listed again, the zones only, each zone texture made again by the DAT whose entry it is, the same bytes (" +
					line_with(out, "the same key again") + ")");
			// (--redo with another factor: the zones' entries replaced at 4x, the models' left)
			code = make_run(P1, "--kind zone --redo --zone-factor 4", out);
			std::shared_ptr<const pk::index> p4x = load(P1);
			bool four = p4x != nullptr && p4x->entries.size() == p1->entries.size();
			if (four)
				for (const pk::entry &e : p4x->entries)
				{
					const pk::record *r = e.map(pk::kColour);
					four = four && r != nullptr && r->w == e.w * 4 && r->h == e.h * 4;
					if (!four)
						break;
				}
			check(code == 0 && four && out.find(std::to_string(zone_keys) + " of them replacing") != std::string::npos,
				"--kind zone --redo --zone-factor 4: every zone entry replaced at 4x, the models' kept (" + line_with(out, "of them replacing") + ")");
			// (a work folder: make's own is deleted with a link in it unlinked, never followed; one without make's mark is
			// left and make refuses)
			const std::wstring outside = mw + L"\\outside";
			make_folders(outside);
			write_text(outside + L"\\keep.txt", "not make's");
			make_folders(P1 + L"\\work");
			write_text(P1 + L"\\work\\made-by-hifixi-texpack-make.txt", "");
			const std::wstring link = P1 + L"\\work\\link";
			_wsystem((L"cmd /c mklink /J \"" + link + L"\" \"" + outside + L"\" > NUL").c_str());
			const bool linked = is_there(link + L"\\keep.txt");
			code = make_run(P1, "--kind zone", out);
			check(code == 0 && linked && !is_there(P1 + L"\\work") && is_there(outside + L"\\keep.txt"),
				"make's work folder deleted, a junction in it unlinked, the folder it points to kept");
			make_folders(P1 + L"\\work");
			write_text(P1 + L"\\work\\mine.txt", "the player's");
			code = make_run(P1, "--kind zone", out);
			check(code == 1 && out.find("isn't make's") != std::string::npos && is_there(P1 + L"\\work\\mine.txt") &&
					text_of(P1 + L"\\progress.txt").find("state: failed") != std::string::npos,
				"a work folder without make's mark: left, exit 1, progress.txt failed (" + line_with(out, "isn't make's") + ")");
			remove_tree(P1 + L"\\work");
		}

		// P2: dedupe across runs: 66, then 72, whose textures 66 shares aren't upscaled again
		{
			const std::wstring P2 = mw + L"\\P2";
			code = make_run(P2, "--only ROM\\1\\66.DAT", out);
			const int code2 = make_run(P2, "--only ROM\\1\\72.DAT", out);
			const std::string expect = std::to_string(keys72.size() - shared) + " textures to make";
			std::shared_ptr<const pk::index> p2 = load(P2);
			size_t zones = 0;
			for (const pk::entry &e : p1->entries)
				zones += e.source_kind == pk::kSourceZone;
			check(code == 0 && code2 == 0 && out.find(expect) != std::string::npos && p2 && p2->entries.size() == zones,
				"dedupe: 72 after 66 upscales only the " + std::to_string(keys72.size() - shared) + " textures 66 hasn't (" + line_with(out, "batch 1:") + ")");
		}

		// P3: killed after its first batch, then run again: the same pack as one run, the upscaler gone with it
		{
			const std::wstring P3 = mw + L"\\P3";
			DeleteFileW(fake_log.c_str());
			SetEnvironmentVariableW(L"FAKE_ESRGAN_MODE", L"slow");
			SetEnvironmentVariableW(L"FAKE_ESRGAN_SLOW_MS", L"150");
			started s = start(tool, wide(args(P3, "--batch-mtexels 0.05")), mw + L"\\make.out");
			const bool committed = wait_for([&] { return count_lines(pack_src(P3)) >= 1 && text_of(P3 + L"\\progress.txt").find("upscaling") != std::string::npos; }, 120000);
			Sleep(300);
			TerminateProcess(s.process, 1);
			finish(s, 5000);
			std::string what;
			const bool gone = child_gone(fake_log, what);
			const size_t lines = count_lines(pack_src(P3));
			check(committed && gone && lines >= 1 && lines < 5, "killed mid-build after a commit: " + std::to_string(lines) + " DATs in pack.src, " + what);
			check(load(P3) != nullptr && !is_locked(P3), "the killed run's pack loads, pack.lock went with it");
			SetEnvironmentVariableW(L"FAKE_ESRGAN_MODE", nullptr);
			code = make_run(P3, "--batch-mtexels 0.05", out);
			std::shared_ptr<const pk::index> p3 = load(P3);
			check(code == 0 && p3 && same_entries(*p1, *p3) && out.find(std::to_string(lines) + " done before") != std::string::npos,
				"resumed: the same pack as one run (" + line_with(out, "done before") + ")");
		}

		// P4: the stop file while the upscaler runs ends it; delete it and go on
		{
			const std::wstring P4 = mw + L"\\P4";
			DeleteFileW(fake_log.c_str());
			SetEnvironmentVariableW(L"FAKE_ESRGAN_MODE", L"slow");
			SetEnvironmentVariableW(L"FAKE_ESRGAN_SLOW_MS", L"300");
			started s = start(tool, wide(args(P4, "")), mw + L"\\make.out");
			const bool upscaling = wait_for([&] { return text_of(P4 + L"\\progress.txt").find("phase: batch 1: upscaling") != std::string::npos; }, 60000);
			write_text(P4 + L"\\stop", "");
			const int c = finish(s, 20000);
			std::string what;
			const bool gone = child_gone(fake_log, what);
			out = text_of(mw + L"\\make.out");
			check(upscaling && c == 3 && gone && out.find("the upscaler ended") != std::string::npos && text_of(P4 + L"\\progress.txt").find("stopped (the stop file)") != std::string::npos,
				"the stop file during the upscaler: exit 3, the upscaler ended (" + what + ")");
			DeleteFileW((P4 + L"\\stop").c_str());
			SetEnvironmentVariableW(L"FAKE_ESRGAN_MODE", nullptr);
			code = make_run(P4, "", out);
			std::shared_ptr<const pk::index> p4 = load(P4);
			check(code == 0 && p4 && same_entries(*p1, *p4), "after the stop file is deleted: the same pack as one run");
		}

		// P5: the game starts while the upscaler runs: it's ended, make waits (paused) until the game has been closed for
		// --game-quiet-s, then makes that batch again
		{
			const std::wstring P5 = mw + L"\\P5";
			DeleteFileW(fake_log.c_str());
			SetEnvironmentVariableW(L"FAKE_ESRGAN_MODE", L"slow");
			SetEnvironmentVariableW(L"FAKE_ESRGAN_SLOW_MS", L"300");
			started s = start(tool, wide(args(P5, "")), mw + L"\\make.out");
			const bool upscaling = wait_for([&] { return text_of(P5 + L"\\progress.txt").find("phase: batch 1: upscaling") != std::string::npos; }, 60000);
			started game = start(game_exe, L"", L"");
			const bool paused = wait_for([&] { return text_of(P5 + L"\\progress.txt").find("paused: game running (texpack-fakegame.exe is running)") != std::string::npos; }, 20000);
			std::string what;
			const bool gone = child_gone(fake_log, what);
			const std::string progress = text_of(P5 + L"\\progress.txt");
			check(upscaling && paused && gone && progress.find("state: paused") != std::string::npos,
				"the game started mid-batch: the upscaler ended (" + what + "), progress.txt says paused: game running");
			Sleep(1000);
			check(WaitForSingleObject(s.process, 0) == WAIT_TIMEOUT, "make waits while the game runs");
			finish(game, 0);
			const auto closed = GetTickCount64();
			SetEnvironmentVariableW(L"FAKE_ESRGAN_MODE", nullptr);
			const int c = finish(s, 120000);
			const double after = double(GetTickCount64() - closed) / 1000;
			out = text_of(mw + L"\\make.out");
			std::shared_ptr<const pk::index> p5 = load(P5);
			char text[64];
			std::snprintf(text, sizeof(text), "%.1f s", after);
			check(c == 0 && after >= 2.0 && out.find("The game has been closed for 2 s: going on.") != std::string::npos && p5 && same_entries(*p1, *p5),
				"the game closed: make went on after the quiet time (done " + std::string(text) + " later), that batch made again: the same pack as one run");
			// (--no-wait: the game running at the start stops it with exit 4, as convert)
			game = start(game_exe, L"", L"");
			wait_for([&] { return alive(game.pid); }, 2000);
			code = make_run(mw + L"\\P6", "--no-wait", out, 30000);
			finish(game, 0);
			check(code == 4 && out.find("texpack-fakegame.exe is running") != std::string::npos, "--no-wait with the game running: exit 4 (" + line_with(out, "Stopped") + ")");
		}

		// An upscaler that hangs is ended after --stall-s; one that fails stops make
		{
			DeleteFileW(fake_log.c_str());
			SetEnvironmentVariableW(L"FAKE_ESRGAN_MODE", L"hang");
			code = make_run(mw + L"\\P7", "--stall-s 2 --only ROM3\\5\\110.DAT", out, 60000);
			std::string what;
			const bool gone = child_gone(fake_log, what);
			check(code == 1 && out.find("the upscaler stalled") != std::string::npos && gone, "a hung upscaler: ended after --stall-s, exit 1 (" + what + ")");
			SetEnvironmentVariableW(L"FAKE_ESRGAN_MODE", L"fail");
			code = make_run(mw + L"\\P7", "--only ROM3\\5\\110.DAT", out, 60000);
			check(code == 1 && out.find("the upscaler failed") != std::string::npos && out.find("exited with code 3") != std::string::npos,
				"a failing upscaler: exit 1, its exit code said (" + line_with(out, "upscaler failed") + ")");
			SetEnvironmentVariableW(L"FAKE_ESRGAN_MODE", nullptr);
		}
		SetEnvironmentVariableW(L"PATH", old_path);
	}
}

int main(int argc, char **argv)
{
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	root = exe;
	root = root.substr(0, root.find_last_of(L'\\'));
	root = root.substr(0, root.find_last_of(L'\\'));
	tool = root + L"\\build\\hifixi-texpack.exe";
	work = root + L"\\build\\test\\texpack";
	// (texpack_test make: only make's tests, no convert pack to compare the models' tags with)
	if (argc > 1 && std::string(argv[1]) == "make")
	{
		make_folders(work);
		make_tests(nullptr);
		std::printf("texpack_test: %d passed, %d failed\n", passes, failures);
		return failures;
	}
	// (texpack_test heights: only M9's height tests)
	if (argc > 1 && std::string(argv[1]) == "heights")
	{
		make_folders(work);
		heights_tests();
		std::printf("texpack_test: %d passed, %d failed\n", passes, failures);
		return failures;
	}
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

	// ---- make (M7)
	make_tests(a);

	// ---- heights (M9)
	heights_tests();

	std::printf("texpack_test: %d passed, %d failed\n", passes, failures);
	return failures;
}
