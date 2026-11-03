/*
 * hifixi-texpack: the texture pack built from the player's own files (docs/texture-pack-plan.md section 7; the pack's format:
 * docs/texture-pack/format.md). 64-bit (tools\build_texpack.cmd), so it never shares the game's address space or its
 * limits; it reads the game's folder and the overlay and writes only the pack's folder.
 *
 *   hifixi-texpack convert --out <pack folder> [--ffxi <FINAL FANTASY XI>] [--overlay <hifi-textures>]
 *                          [--lists <hifi-textures\work\all>] [--ashita <folder>] [--ffximain <file>]
 *                          [--kind zone|model] [--only <rel> ...] [--limit N] [--threads N] [--redo] [--reserve-gb N]
 *                          [--ignore-game] [--soul-mean X] [--soul-p99 X] [--alpha-mean X] [--alpha-p99 X]
 *   hifixi-texpack verify  --pack <folder> [--soul] [--ffxi ...] [--threads N] [--soul-mean X] [--soul-p99 X]
 *                          [--alpha-mean X] [--alpha-p99 X] [--worst N]
 *   hifixi-texpack stats   --pack <folder> [--scan] [--ffxi ...] [--overlay ...]
 *   hifixi-texpack compact --pack <folder> [--ffxi ...]
 *   hifixi-texpack prune   --pack <folder> [--ffxi ...]
 *
 * convert: the textures XIPivot's hifi-textures overlay (research\ffxi-re\tools\upscale\bakeall.py, bakezone.py) has
 * made bigger, put into the pack, keyed by their ORIGINAL chunks (pack.hpp key_of).
 *   - Inputs: the DATs dats.tsv lists as zone or model, with no XIPivot overlay ahead of hifi-textures (its last column
 *     '-'), that the overlay has a copy of, plus every DAT the bakeall logs list with a number (ROM\0\104.DAT, baked by
 *     bakezone.py, is in no log). The logs alone missed 83 of the overlay's DATs (an earlier run's), so they are only a
 *     cross-check; their "palette only, left" rows are skipped (the game's own file stands for those). An original
 *     whose size isn't dats.tsv's (the file the bake read) is skipped.
 *   - Pairing: the overlay's chunk i with the original's chunk i. The same chunk count, and every chunk the bake didn't
 *     make bigger byte for byte the original's (bakeall rewrites only what it upscales), else the DAT is refused: the
 *     game's file changed since the bake. A texture chunk the same size in both was left (not upscaled: B1 palettes,
 *     2x1 DXTs) and gets no entry; a bigger one must have the same 16-byte name and be an A1 '3TXD' of a whole multiple
 *     (2, 4 or 8) of the original's size. Anything else is logged and skipped, never guessed. Its BC2 blocks (body+69)
 *     are level 0, unchanged; every entry field comes from the ORIGINAL chunk (the overlay's header bytes 17..57 are a
 *     template bakeall copied from ROM\0\104.DAT). Each one then passes verify's soul test against its original or is
 *     left out (an original repainted in place since the bake: same size and name, another painting); no_pack ones are
 *     kept.
 *   - Alpha kinds (bc2::full_chain's mips), tagged over every input of the kind, not only the DATs to do (a run narrowed
 *     by --limit, or resumed, tags as one whole run; --only narrows the inputs, and so the tags): zones from their
 *     meshes (zonemesh: a piece with the blend bit: blended; an unblended piece of a model whose name starts '_', which
 *     the zone pass alpha-tests: cut-out, with the median vertex alpha of those pieces as vcol_a; both: mixed); model
 *     DATs from their Os2 meshes (by name within the DAT, onto the keys: a group whose material has the shine dword:
 *     mask; any other group: cut-out; both: mixed; named by no group of any model DAT: none, the plain box, counted). A
 *     zone whose meshes can't be read has its textures marked blended (the plain box filter), counted.
 *   - B1 originals (Giddeus' palette textures bakezone.py made A1) are keyed by the B1 body and marked no_pack until a
 *     Giddeus A/B shows the same cut-outs and blending.
 *   - Zones first, then models, each in rel path order: the first entry of a key stays (pack_write), so a rerun makes
 *     the same pack. Resume from pack.src (a DAT whose original's size and time, overlay copy's size and time and maker
 *     version match is skipped; --redo does them again). A DAT done again replaces the entries its own chunks made
 *     before (a re-baked copy); between DATs the first stays. A stop file in the pack's folder stops it at the next DAT;
 *     progress.txt says how far it got; texpack.log has every line it printed. It runs at below-normal priority,
 *     refuses to start (and stops) while pol.exe or phoenix-loader.exe run or an Ashita log (logs\*.txt) was written in
 *     the last minute, and refuses when the disk has less than 20 GB plus its estimate free.
 *   - One builder a pack: convert, prune and compact hold its pack.lock (pack_write.hpp).
 * verify: every record read back (the rules, its CRC, each level's CRC); with --soul, the soul test (plan section 7): the
 *   pack's colour box-filtered to half the game's size against the original's texels averaged 2x2 (keeptexels.py's
 *   GRAIN 2), texels at strong edges left out (keeptexels' own fade, exp(-(grad/24)^2) < 0.5, from the pack's luma
 *   shrunk to the game's size and enlarged again), alpha against the original decoded by its own format (chars tex).
 *   The distribution is printed so thresholds can be set from it; an entry over them is reported.
 * stats: counts, bytes, sources; --scan walks the overlay's copies of pack.src's DATs for the dedupe saving and the
 *   coverage against the overlay's upscaled chunks.
 * compact: the referenced records copied into <pack>-compact, which then takes the pack's name (the old one is left as
 *   <pack>-old to delete). A record that can't be read is left out, and so are the pack.src lines of the DATs that have
 *   its key (found in the game's folder), so convert makes it again.
 * prune: entries no DAT of pack.src has any more are dropped; the lines of DATs gone or changed are taken out (convert
 *   does them again). A DAT that can't be read stops it, and so do more than 1% of them gone (the wrong folder?).
 * Without --ffxi, prune, verify and stats take a game folder from the registry only when dats.tsv's sizes match it.
 *
 * Exit codes: 0 done, 1 refused or failed (verify: an entry failed), 2 usage, 3 stopped by the stop file, 4 the game
 * started.
 */

#include <windows.h>
#include <tlhelp32.h>
#include <share.h>
#include "keys.hpp"
#include "../../src/pack/pack.hpp"
#include "../../src/pack/pack_write.hpp"
#include "../../src/pack/bc2.hpp"
#include "../../src/pack/hash.hpp"
#include "../../src/pack/validate.hpp"
#include "../../src/bake/bake.hpp"
#include "../../src/bake/dat.hpp"
#include "../../src/bake/zonemesh.hpp"
#include "../../src/chars/os2.hpp"
#include "../../src/chars/tex.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
	namespace pk = hifi::pack;
	namespace bk = hifi::bake;
	namespace zm = hifi::bake::zonemesh;
	namespace os2 = hifi::chars::os2;
	namespace tex = hifi::chars::tex;

	// pack.src's and the header's maker version: a pack.src line of another version is done again
	constexpr uint32_t kMakerVersion = 1;
	constexpr int kDone = 0, kFailed = 1, kUsage = 2, kStopped = 3, kGameStarted = 4;

	// ---- Small things

	FILE *g_log = nullptr; // texpack.log in the pack's folder (convert, compact, prune)

	// texpack.log opened to append (others may read it meanwhile); nullptr when it can't be
	FILE *open_log(const wchar_t *path)
	{
		return _wfsopen(path, L"ab", _SH_DENYWR);
	}

	// A line to the console and texpack.log
	void say(const char *format, ...)
	{
		char text[4096];
		va_list a;
		va_start(a, format);
		std::vsnprintf(text, sizeof(text), format, a);
		va_end(a);
		std::fputs(text, stdout);
		std::fputs("\n", stdout);
		std::fflush(stdout);
		if (g_log != nullptr)
		{
			std::fputs(text, g_log);
			std::fputs("\r\n", g_log);
			std::fflush(g_log);
		}
	}

	std::wstring wide(const std::string &s)
	{
		if (s.empty())
			return {};
		const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
		std::wstring w(size_t(n), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
		return w;
	}

	std::string narrow(const std::wstring &w)
	{
		if (w.empty())
			return {};
		const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
		std::string s(size_t(n), '\0');
		WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
		return s;
	}

	std::wstring no_slash(std::wstring f)
	{
		while (f.size() > 3 && (f.back() == L'\\' || f.back() == L'/'))
			f.pop_back();
		return f;
	}

	std::wstring with_slash(const std::wstring &f) { return no_slash(f) + L"\\"; }

	// The folder above (no slash at the end); "" at the top
	std::wstring up(const std::wstring &path)
	{
		const std::wstring p = no_slash(path);
		const size_t slash = p.find_last_of(L"\\/");
		return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
	}

	bool is_file(const std::wstring &p)
	{
		const DWORD a = GetFileAttributesW(p.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	bool is_folder(const std::wstring &p)
	{
		const DWORD a = GetFileAttributesW(p.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	bool file_info(const std::wstring &p, uint64_t &size, uint64_t &mtime)
	{
		WIN32_FILE_ATTRIBUTE_DATA a;
		if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			return false;
		size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
		mtime = (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
		return true;
	}

	std::vector<std::string> lines_of(const std::wstring &path, bool &ok)
	{
		std::vector<uint8_t> bytes;
		ok = bk::read_file(path, bytes);
		std::vector<std::string> out;
		std::string line;
		for (uint8_t b : bytes)
		{
			if (b == '\n')
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				out.push_back(line);
				line.clear();
			}
			else
				line += char(b);
		}
		if (!line.empty())
			out.push_back(line);
		return out;
	}

	std::vector<std::string> split(const std::string &s, char by)
	{
		std::vector<std::string> out;
		size_t at = 0;
		for (;;)
		{
			const size_t end = s.find(by, at);
			out.push_back(s.substr(at, end == std::string::npos ? std::string::npos : end - at));
			if (end == std::string::npos)
				return out;
			at = end + 1;
		}
	}

	bool all_digits(const std::string &s) { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; }

	// A texture's 16-byte name for a log line (trailing blanks and zeros off)
	std::string name16(const uint8_t *p)
	{
		std::string n(reinterpret_cast<const char *>(p), 16);
		for (char &c : n)
			if (uint8_t(c) < 32 || uint8_t(c) > 126)
				c = '?';
		return n.substr(0, n.find_last_not_of(" ?") + 1);
	}

	uint32_t u32_at(const uint8_t *p)
	{
		uint32_t v;
		std::memcpy(&v, p, 4);
		return v;
	}

	int32_t i32_at(const uint8_t *p)
	{
		int32_t v;
		std::memcpy(&v, p, 4);
		return v;
	}

	double gb(uint64_t bytes) { return double(bytes) / double(1ull << 30); }

	double seconds_since(std::chrono::steady_clock::time_point t)
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
	}

	// ---- Options

	struct options
	{
		std::string command;
		std::wstring ffxi, overlay, lists, out, ashita, ffximain;
		std::string kind; // (convert: only zone or only model DATs)
		std::vector<std::string> only;
		uint32_t limit = 0, threads = 0, worst = 20;
		bool soul = false, ignore_game = false, redo = false, scan = false;
		double reserve_gb = 20.0;
		// (the soul test's limits, in 8-bit levels: an entry's colour error mean and 99th percentile over the texels
		// kept, and its alpha's. Set from the 292 baked zones' 4,750 entries, 2026-11-02: colour mean p50 1.17, max
		// 2.66; colour p99 p50 2.77, max 12.67; alpha mean max 5.16; alpha p99 p99 14.9, max 30.8. Each limit is about
		// 1.5 times the zones' worst, so a texture whose painting the bake lost stands out; models measured below them.
		// verify reports an entry over them; convert leaves it out)
		double soul_mean = 4.0, soul_p99 = 20.0, alpha_mean = 8.0, alpha_p99 = 48.0;
	};

	void usage()
	{
		std::printf(
			"hifixi-texpack: HiFi XI's texture pack from the player's own files (docs\\texture-pack-plan.md)\n"
			"  convert --out <pack> [--ffxi <FINAL FANTASY XI>] [--overlay <hifi-textures>] [--lists <work\\all>]\n"
			"          [--ashita <folder>] [--ffximain <file>] [--kind zone|model] [--only <rel> ...]\n"
			"          [--limit N] [--threads N] [--redo] [--reserve-gb N] [--ignore-game]\n"
			"          [--soul-mean X] [--soul-p99 X] [--alpha-mean X] [--alpha-p99 X]\n"
			"  verify  --pack <pack> [--soul] [--ffxi ...] [--threads N] [--soul-mean X] [--soul-p99 X] [--alpha-mean X]\n"
			"          [--alpha-p99 X] [--worst N]\n"
			"  stats   --pack <pack> [--scan] [--ffxi ...] [--overlay ...]\n"
			"  compact --pack <pack> [--ffxi ...] [--ignore-game]\n"
			"  prune   --pack <pack> [--ffxi ...] [--ignore-game]\n");
	}

	bool parse(int argc, wchar_t **argv, options &o)
	{
		if (argc < 2)
			return false;
		o.command = narrow(argv[1]);
		for (int i = 2; i < argc; ++i)
		{
			const std::wstring a = argv[i];
			auto next = [&](std::wstring &v) {
				if (i + 1 >= argc)
					return false;
				v = argv[++i];
				return true;
			};
			auto number = [&](double &v) {
				std::wstring s;
				if (!next(s))
					return false;
				v = _wtof(s.c_str());
				return true;
			};
			std::wstring s;
			double d = 0;
			if (a == L"--ffxi")
			{
				if (!next(o.ffxi))
					return false;
			}
			else if (a == L"--overlay")
			{
				if (!next(o.overlay))
					return false;
			}
			else if (a == L"--lists")
			{
				if (!next(o.lists))
					return false;
			}
			else if (a == L"--out" || a == L"--pack")
			{
				if (!next(o.out))
					return false;
			}
			else if (a == L"--ashita")
			{
				if (!next(o.ashita))
					return false;
			}
			else if (a == L"--ffximain")
			{
				if (!next(o.ffximain))
					return false;
			}
			else if (a == L"--kind" && next(s))
				o.kind = narrow(s);
			else if (a == L"--only")
			{
				while (i + 1 < argc && std::wstring(argv[i + 1]).rfind(L"--", 0) != 0)
					o.only.push_back(narrow(argv[++i]));
			}
			else if (a == L"--limit" && number(d))
				o.limit = uint32_t(d);
			else if (a == L"--threads" && number(d))
				o.threads = uint32_t(d);
			else if (a == L"--worst" && number(d))
				o.worst = uint32_t(d);
			else if (a == L"--reserve-gb" && number(d))
				o.reserve_gb = d;
			else if (a == L"--soul-mean" && number(d))
				o.soul_mean = d;
			else if (a == L"--soul-p99" && number(d))
				o.soul_p99 = d;
			else if (a == L"--alpha-mean" && number(d))
				o.alpha_mean = d;
			else if (a == L"--alpha-p99" && number(d))
				o.alpha_p99 = d;
			else if (a == L"--soul")
				o.soul = true;
			else if (a == L"--scan")
				o.scan = true;
			else if (a == L"--redo")
				o.redo = true;
			else if (a == L"--ignore-game")
				o.ignore_game = true;
			else
			{
				std::printf("unknown option: %s\n", narrow(a).c_str());
				return false;
			}
		}
		if (o.threads == 0)
			o.threads = std::max(1u, std::thread::hardware_concurrency() > 2 ? std::thread::hardware_concurrency() - 2 : 1u);
		return true;
	}

	// ---- Where things are

	// Ashita's folder: one with config\pivot\pivot.ini (XIPivot's settings) or Ashita.dll
	bool ashita_like(const std::wstring &f)
	{
		return !f.empty() && (is_file(f + L"\\config\\pivot\\pivot.ini") || is_file(f + L"\\Ashita.dll"));
	}

	bool ffxi_like(const std::wstring &f) { return !f.empty() && is_file(f + L"\\VTABLE.DAT") && is_file(f + L"\\FTABLE.DAT"); }

	// PlayOnline's install folder for FINAL FANTASY XI from the registry ("" when none). Last resort only: on this PC
	// it names a retail install in Program Files (x86), not Phoenix's files, so what it gives is checked against
	// dats.tsv's sizes before it's used
	std::wstring registry_ffxi()
	{
		const wchar_t *keys[] = { L"SOFTWARE\\WOW6432Node\\PlayOnlineUS\\InstallFolder", L"SOFTWARE\\PlayOnlineUS\\InstallFolder",
			L"SOFTWARE\\WOW6432Node\\PlayOnlineEU\\InstallFolder", L"SOFTWARE\\WOW6432Node\\PlayOnline\\InstallFolder" };
		for (const wchar_t *k : keys)
		{
			wchar_t v[MAX_PATH] = L"";
			DWORD n = sizeof(v);
			if (RegGetValueW(HKEY_LOCAL_MACHINE, k, L"0001", RRF_RT_REG_SZ, nullptr, v, &n) == ERROR_SUCCESS && v[0] != 0)
				return no_slash(v);
		}
		return {};
	}

	// XIPivot's root folder from Ashita's pivot.ini ("" when there's none)
	std::wstring pivot_root(const std::wstring &ashita)
	{
		const std::wstring ini = ashita + L"\\config\\pivot\\pivot.ini";
		if (ashita.empty() || !is_file(ini))
			return {};
		wchar_t root[MAX_PATH] = L"";
		GetPrivateProfileStringW(L"settings", L"root_path", L"", root, MAX_PATH, ini.c_str());
		return no_slash(root);
	}

	std::wstring exe_folder()
	{
		wchar_t p[MAX_PATH] = L"";
		GetModuleFileNameW(nullptr, p, MAX_PATH);
		return up(p);
	}

	// dats.tsv (research\ffxi-re\tools\upscale\sortdats.py): rel path, kind (zone, model, other), textures, texels,
	// the original's bytes, the XIPivot overlay ahead of ours that has it ('-': none)
	struct tsv_row
	{
		std::string kind, shadow;
		uint64_t bytes = 0;
	};
	using tsv = std::unordered_map<std::string, tsv_row>;

	bool read_tsv(const std::wstring &lists, tsv &out)
	{
		bool ok = false;
		for (const std::string &line : lines_of(lists + L"\\dats.tsv", ok))
		{
			const std::vector<std::string> f = split(line, '\t');
			if (f.size() < 6)
				continue;
			tsv_row r;
			r.kind = f[1];
			r.bytes = std::strtoull(f[4].c_str(), nullptr, 10);
			r.shadow = f[5];
			out[f[0]] = r;
		}
		return ok && !out.empty();
	}

	// dats.tsv from --lists, or from the bake's folder beside the tool's (<LSB>\hifi-textures\work\all); false when
	// it isn't there (the game's folder found then can't be checked: find_folders takes no guess from the registry)
	bool read_rows(options &o, tsv &out)
	{
		std::wstring lists = o.lists.empty() ? up(up(exe_folder())) + L"\\hifi-textures\\work\\all" : o.lists;
		return read_tsv(no_slash(lists), out);
	}

	// The folders the command needs found, in the plan's order: what was given; else from Ashita's folder (given, or
	// the one above the game's or the overlay's: <ashita>\SquareEnix\FINAL FANTASY XI, <ashita>\polplugins\DATs\
	// hifi-textures through pivot.ini's root_path); the game's folder from the registry last, and any folder that
	// wasn't given checked against dats.tsv's sizes. Never the old hard-coded fallback (dat_core's game_folder): the
	// game's folder is always set before any DAT is read.
	bool find_folders(options &o, bool need_ffxi, bool need_overlay, const tsv *rows)
	{
		if (o.ashita.empty())
		{
			if (!o.ffxi.empty() && ashita_like(up(up(o.ffxi))))
				o.ashita = up(up(o.ffxi));
			else if (!o.overlay.empty() && ashita_like(up(up(up(o.overlay)))))
				o.ashita = up(up(up(o.overlay)));
		}
		o.ashita = no_slash(o.ashita);
		bool ffxi_given = !o.ffxi.empty(), from_registry = false;
		if (need_ffxi && o.ffxi.empty())
		{
			if (ffxi_like(o.ashita + L"\\SquareEnix\\FINAL FANTASY XI"))
				o.ffxi = o.ashita + L"\\SquareEnix\\FINAL FANTASY XI";
			else if (ffxi_like(registry_ffxi()))
				o.ffxi = registry_ffxi(), from_registry = true;
		}
		o.ffxi = no_slash(o.ffxi);
		if (need_ffxi)
		{
			if (!ffxi_like(o.ffxi))
			{
				say("The game's folder (FINAL FANTASY XI, with VTABLE.DAT) wasn't found: give it with --ffxi.");
				return false;
			}
			if (from_registry && rows == nullptr)
			{
				// (the registry's folder is a guess that must be checked: wrongly, prune would drop what it doesn't find)
				say("%s (from the registry) can't be checked without dats.tsv (--lists): give the game's folder with --ffxi.", narrow(o.ffxi).c_str());
				return false;
			}
			if (!ffxi_given && rows != nullptr)
			{
				// (the originals the overlay was baked from: their sizes as dats.tsv has them)
				uint32_t seen = 0, same = 0;
				for (const auto &[rel, r] : *rows)
				{
					if (r.kind != "zone" && r.kind != "model")
						continue;
					uint64_t size = 0, mtime = 0;
					same += file_info(o.ffxi + L"\\" + wide(rel), size, mtime) && size == r.bytes;
					if (++seen == 200)
						break;
				}
				if (seen == 0 || same * 10 < seen * 9)
				{
					say("%s (%s) doesn't hold the files the overlay was made from (%u of %u sizes match): give the game's folder with --ffxi.",
						narrow(o.ffxi).c_str(), from_registry ? "from the registry" : "found", same, seen);
					return false;
				}
			}
			bk::set_ffxi_folder(o.ffxi);
		}
		if (need_overlay)
		{
			if (o.overlay.empty() && !pivot_root(o.ashita).empty())
				o.overlay = pivot_root(o.ashita) + L"\\hifi-textures";
			o.overlay = no_slash(o.overlay);
			if (!is_folder(o.overlay))
			{
				say("The hifi-textures overlay's folder wasn't found: give it with --overlay.");
				return false;
			}
		}
		return true;
	}

	// ---- The game's file ids (VTABLE/FTABLE, then ROMn's VTABLEn/FTABLEn, in dat_path's order)

	struct id_table
	{
		std::vector<std::string> rel_of;           // by file id ("" none)
		std::unordered_map<std::string, uint32_t> id_of; // (the lowest id naming a file)

		bool load(const std::wstring &ffxi)
		{
			std::vector<uint8_t> vt, ft;
			if (!bk::read_file(ffxi + L"\\VTABLE.DAT", vt) || !bk::read_file(ffxi + L"\\FTABLE.DAT", ft))
				return false;
			rel_of.assign(vt.size(), std::string());
			auto name = [&](uint32_t id, const std::vector<uint8_t> &f, unsigned rom) {
				if ((size_t(id) + 1) * 2 > f.size())
					return;
				const uint32_t v = uint32_t(f[size_t(id) * 2] | (f[size_t(id) * 2 + 1] << 8));
				char text[64];
				if (rom == 1)
					std::snprintf(text, sizeof(text), "ROM\\%u\\%u.DAT", v >> 7, v & 0x7F);
				else
					std::snprintf(text, sizeof(text), "ROM%u\\%u\\%u.DAT", rom, v >> 7, v & 0x7F);
				rel_of[id] = text;
			};
			for (uint32_t id = 0; id < vt.size(); ++id)
				if (vt[id] == 1)
					name(id, ft, 1);
			for (unsigned n = 2; n < 10; ++n)
			{
				wchar_t v_name[MAX_PATH], f_name[MAX_PATH];
				std::swprintf(v_name, MAX_PATH, L"%s\\ROM%u\\VTABLE%u.DAT", ffxi.c_str(), n, n);
				std::swprintf(f_name, MAX_PATH, L"%s\\ROM%u\\FTABLE%u.DAT", ffxi.c_str(), n, n);
				std::vector<uint8_t> vn, fn;
				if (!bk::read_file(v_name, vn) || !bk::read_file(f_name, fn))
					continue;
				for (uint32_t id = 0; id < vn.size() && id < rel_of.size(); ++id)
					if (rel_of[id].empty() && vt[id] != 1 && vn[id] == n)
						name(id, fn, n);
			}
			for (uint32_t id = 0; id < rel_of.size(); ++id)
				if (!rel_of[id].empty())
					id_of.emplace(rel_of[id], id);
			return true;
		}
		uint32_t id(const std::string &rel) const
		{
			auto it = id_of.find(rel);
			return it == id_of.end() ? 0xFFFFFFFFu : it->second;
		}
	};

	// ---- Is the game running?

	// Why the game counts as running ("" when it doesn't): pol.exe or phoenix-loader.exe, or one of Ashita's logs
	// (logs\*.txt) written in the last minute
	std::string game_running(const std::wstring &ashita)
	{
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap != INVALID_HANDLE_VALUE)
		{
			PROCESSENTRY32W p = { sizeof(p) };
			for (BOOL more = Process32FirstW(snap, &p); more; more = Process32NextW(snap, &p))
				if (_wcsicmp(p.szExeFile, L"pol.exe") == 0 || _wcsicmp(p.szExeFile, L"phoenix-loader.exe") == 0)
				{
					CloseHandle(snap);
					return narrow(p.szExeFile) + " is running";
				}
			CloseHandle(snap);
		}
		if (!ashita.empty())
		{
			WIN32_FIND_DATAW fd;
			HANDLE f = FindFirstFileW((ashita + L"\\logs\\*.txt").c_str(), &fd);
			if (f != INVALID_HANDLE_VALUE)
			{
				FILETIME now_ft;
				GetSystemTimeAsFileTime(&now_ft);
				const uint64_t now = (uint64_t(now_ft.dwHighDateTime) << 32) | now_ft.dwLowDateTime;
				std::string why;
				do
				{
					const uint64_t t = (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
					if (t + 600000000ull > now)
						why = "Ashita's log " + narrow(fd.cFileName) + " was written in the last minute";
				} while (why.empty() && FindNextFileW(f, &fd));
				FindClose(f);
				if (!why.empty())
					return why;
			}
		}
		return {};
	}

	// ---- Alpha tags

	// What uses a texture: pieces (zones) or groups (models) by kind, and the vertex alpha of its cut-out pieces
	struct tag
	{
		uint32_t cut = 0, blended = 0, plain = 0, mask = 0;
		uint64_t vcol[256] = {};
		void merge(const tag &t)
		{
			cut += t.cut, blended += t.blended, plain += t.plain, mask += t.mask;
			for (int i = 0; i < 256; ++i)
				vcol[i] += t.vcol[i];
		}
		uint8_t median_vcol() const
		{
			uint64_t n = 0, seen = 0;
			for (uint64_t c : vcol)
				n += c;
			if (n == 0)
				return 0;
			for (int i = 0; i < 256; ++i)
				if ((seen += vcol[i]) * 2 >= n)
					return uint8_t(std::max(i, 1));
			return 0xFF;
		}
	};

	struct key_hash
	{
		size_t operator()(const pk::key &k) const { return size_t(k.hash ^ (k.hash >> 32) ^ k.bytes); }
	};
	using tag_map = std::unordered_map<pk::key, tag, key_hash>;

	// A zone DAT's textures tagged from its meshes (zonemesh, reading the original DAT: the overlay is switched off in
	// main), by the keys of the original's texture chunks of each name. false (why) when its meshes can't be read
	bool tag_zone(const std::wstring &ffxi, const std::string &rel, uint32_t id, tag_map &out, std::string &why)
	{
		if (id == 0xFFFFFFFFu || id < 100 || zm::map_dat(id - 100) != id)
		{
			why = "its file id isn't a zone map's";
			return false;
		}
		zm::zone z;
		bk::result r;
		if (!zm::load(id - 100, z, r))
		{
			why = r.why.empty() ? "zonemesh can't read it" : r.why;
			return false;
		}
		std::vector<tag> by_texture(z.textures.size());
		for (const zm::model &mo : z.models)
		{
			const bool tested = mo.name[0] == '_';
			for (uint32_t p = mo.first_piece; p < mo.first_piece + mo.pieces; ++p)
			{
				const zm::piece &pc = z.pieces[p];
				if (pc.texture == zm::kNone || pc.texture >= by_texture.size())
					continue;
				tag &t = by_texture[pc.texture];
				if ((pc.bits & zm::kBlended) != 0)
					t.blended++;
				else if (tested)
				{
					t.cut++;
					for (uint32_t i = pc.first_index; i < pc.first_index + pc.triangles * 3 && i < z.indices.size(); ++i)
						t.vcol[z.vertices[z.indices[i]].colour >> 24]++;
				}
				else
					t.plain++;
			}
		}
		std::unordered_map<std::string, const tag *> by_name;
		for (size_t i = 0; i < z.textures.size(); ++i)
			by_name.emplace(std::string(z.textures[i].name, 16), &by_texture[i]);
		std::vector<uint8_t> dat;
		if (!bk::read_file(ffxi + L"\\" + wide(rel), dat))
		{
			why = "the DAT can't be read";
			return false;
		}
		for (const bk::chunk &c : bk::chunks(dat))
		{
			if (c.type != 0x20 || c.size < 29)
				continue;
			auto it = by_name.find(std::string(reinterpret_cast<const char *>(c.body + 1), 16));
			if (it != by_name.end())
				out[pk::key_of(c.body, c.size)].merge(*it->second);
		}
		return true;
	}

	// A model DAT's textures tagged from its Os2 meshes: by name, a group whose material has the shine dword (chardraw:
	// has_material and float material[10] != 0) as mask, any other group as cut-out; each name's tag merged onto the
	// keys of the DAT's texture chunks of that name (convert merges them over every model DAT of the run: a texture's
	// bytes are often in several DATs, its mesh in only some)
	void tag_models(const std::vector<bk::chunk> &all, tag_map &out)
	{
		std::unordered_map<std::string, tag> by_name;
		for (const bk::chunk &c : all)
		{
			if (c.type != 0x2A)
				continue;
			os2::sections s;
			os2::mesh m;
			std::string why;
			if (!os2::sections_of_chunk(c.body, c.size, s) || !os2::decode(c.body, c.size, s, m, why))
				continue;
			for (const os2::group &g : m.groups)
			{
				if (g.texture < 0 || size_t(g.texture) >= m.textures.size())
					continue;
				float shine = 0.0f;
				std::memcpy(&shine, &g.material[10], 4);
				tag &t = by_name[std::string(reinterpret_cast<const char *>(m.textures[size_t(g.texture)].name), 16)];
				if (g.has_material && shine != 0.0f)
					t.mask++;
				else
					t.cut++;
			}
		}
		if (by_name.empty())
			return;
		for (const bk::chunk &c : all)
		{
			if (c.type != 0x20 || c.size < 29)
				continue;
			auto it = by_name.find(std::string(reinterpret_cast<const char *>(c.body + 1), 16));
			if (it != by_name.end())
				out[pk::key_of(c.body, c.size)].merge(it->second);
		}
	}

	uint8_t alpha_kind_of(const tag &t)
	{
		const int kinds = (t.cut > 0) + (t.blended > 0) + (t.mask > 0);
		if (kinds > 1)
			return pk::kAlphaMixed;
		if (t.cut > 0)
			return pk::kAlphaCut;
		if (t.blended > 0)
			return pk::kAlphaBlended;
		if (t.mask > 0)
			return pk::kAlphaMask;
		return pk::kAlphaNone;
	}

	// ---- Reading records back

	// A record's levels as tight BC blocks (the copy layout's row padding taken out)
	bool untile(const pk::record &r, const std::vector<uint8_t> &bytes, std::vector<std::vector<uint8_t>> &levels)
	{
		pk::layout l;
		if (!pk::footprint(r.format, r.w, r.h, r.levels, l) || bytes.size() < l.total)
			return false;
		levels.assign(l.levels, {});
		for (uint32_t i = 0; i < l.levels; ++i)
		{
			const pk::level_layout &lv = l.level[i];
			levels[i].resize(size_t(lv.row_bytes) * lv.rows);
			for (uint32_t row = 0; row < lv.rows; ++row)
				std::memcpy(levels[i].data() + size_t(row) * lv.row_bytes, bytes.data() + lv.offset + size_t(row) * lv.row_pitch, lv.row_bytes);
		}
		return true;
	}

	// Every level's CRC against the index's table ("" all right)
	std::string level_crcs(const pk::index &idx, const pk::record &r, const std::vector<uint8_t> &bytes)
	{
		pk::layout l;
		if (!pk::footprint(r.format, r.w, r.h, r.levels, l))
			return "no layout";
		for (uint32_t i = 0; i < l.levels; ++i)
			if (pk::crc32(0, bytes.data() + l.level[i].offset, size_t(l.level_span(i))) != idx.level_crc(r, i))
				return "level " + std::to_string(i) + "'s CRC";
		return {};
	}

	// ---- The soul test

	struct soul_result
	{
		bool done = false;
		std::string why;     // (not done: why)
		double c_mean = 0, c_p99 = 0, a_mean = 0, a_p99 = 0;
		double kept = 0;     // the fraction of texels compared for colour (not at a strong edge)
		uint32_t factor = 0;
		bool stored_rows = false; // (B1: the original's rows as stored matched better than tex's bottom-up order)
	};

	double p99_of(std::vector<float> &v)
	{
		if (v.empty())
			return 0;
		const size_t k = size_t(0.99 * double(v.size() - 1));
		std::nth_element(v.begin(), v.begin() + ptrdiff_t(k), v.end());
		return v[k];
	}

	// keeptexels.py's enlarge: bilinear S x, wrapping at the edges (the textures tile)
	std::vector<float> enlarge(const std::vector<float> &a, uint32_t w, uint32_t h, uint32_t s)
	{
		std::vector<float> out(size_t(w) * s * h * s);
		const uint32_t W = w * s, H = h * s;
		for (uint32_t Y = 0; Y < H; ++Y)
		{
			const double y = (Y + 0.5) / s - 0.5, y0 = std::floor(y), fy = y - y0;
			const uint32_t ya = uint32_t((int64_t(y0) % h + h) % h), yb = (ya + 1) % h;
			for (uint32_t X = 0; X < W; ++X)
			{
				const double x = (X + 0.5) / s - 0.5, x0 = std::floor(x), fx = x - x0;
				const uint32_t xa = uint32_t((int64_t(x0) % w + w) % w), xb = (xa + 1) % w;
				out[size_t(Y) * W + X] = float(a[size_t(ya) * w + xa] * (1 - fx) * (1 - fy) + a[size_t(ya) * w + xb] * fx * (1 - fy) +
					a[size_t(yb) * w + xa] * (1 - fx) * fy + a[size_t(yb) * w + xb] * fx * fy);
			}
		}
		return out;
	}

	// The pack's level 0 (tight BC2, W x H) against the original chunk (see the top)
	soul_result soul(const uint8_t *body, size_t size, const uint8_t *level0, uint32_t W, uint32_t H)
	{
		soul_result r;
		tex::texture t;
		if (!tex::decode(body, size, t, r.why) || t.levels.empty())
		{
			r.why = "the original doesn't decode: " + r.why;
			return r;
		}
		const uint32_t w = t.w, h = t.h;
		if (W % w != 0 || H % h != 0 || W / w != H / h)
		{
			r.why = "the pack's size isn't a whole multiple of the original's";
			return r;
		}
		const uint32_t s = W / w, g = (w % 2 == 0 && h % 2 == 0) ? 2 : 1, tw = w / g, th = h / g, S = s * g;
		r.factor = s;
		const std::vector<uint8_t> P = pk::bc2::decode_bc2(level0, W, H);
		// (the pack shrunk to half the game's size; its luma shrunk to the game's size)
		std::vector<float> Q(size_t(tw) * th * 4, 0.0f), L(size_t(w) * h, 0.0f);
		for (uint32_t y = 0; y < H; ++y)
			for (uint32_t x = 0; x < W; ++x)
			{
				const uint8_t *p = P.data() + (size_t(y) * W + x) * 4;
				float *q = Q.data() + (size_t(y / S) * tw + x / S) * 4;
				for (int c = 0; c < 4; ++c)
					q[c] += p[c];
				L[size_t(y / s) * w + x / s] += 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2];
			}
		for (float &q : Q)
			q /= float(S * S);
		for (float &l : L)
			l /= float(s * s);
		// (keeptexels' edge fade: the gradient of the luma enlarged again, in levels per original texel)
		const std::vector<float> E = enlarge(L, w, h, s);
		std::vector<float> flat(size_t(tw) * th, 0.0f);
		for (uint32_t y = 0; y < H; ++y)
			for (uint32_t x = 0; x < W; ++x)
			{
				auto at = [&](uint32_t yy, uint32_t xx) { return E[size_t(yy) * W + xx]; };
				const float gy = H < 2 ? 0.0f : y == 0 ? at(1, x) - at(0, x) : y == H - 1 ? at(H - 1, x) - at(H - 2, x) : (at(y + 1, x) - at(y - 1, x)) / 2;
				const float gx = W < 2 ? 0.0f : x == 0 ? at(y, 1) - at(y, 0) : x == W - 1 ? at(y, W - 1) - at(y, W - 2) : (at(y, x + 1) - at(y, x - 1)) / 2;
				const float grad = std::sqrt(gx * gx + gy * gy) * float(s) / 24.0f;
				flat[size_t(y / S) * tw + x / S] += std::exp(-grad * grad);
			}
		// (the original averaged g x g: rows as tex gives them, and for B1 also as stored)
		const std::vector<uint8_t> &O = t.levels[0].rgba;
		auto compare = [&](bool flip, soul_result &out) {
			std::vector<float> ce, ae;
			ce.reserve(size_t(tw) * th);
			ae.reserve(size_t(tw) * th);
			uint64_t kept = 0;
			for (uint32_t y = 0; y < th; ++y)
				for (uint32_t x = 0; x < tw; ++x)
				{
					float o[4] = {};
					for (uint32_t dy = 0; dy < g; ++dy)
						for (uint32_t dx = 0; dx < g; ++dx)
						{
							const uint32_t oy = flip ? h - 1 - (y * g + dy) : y * g + dy;
							const uint8_t *p = O.data() + (size_t(oy) * w + x * g + dx) * 4;
							for (int c = 0; c < 4; ++c)
								o[c] += p[c];
						}
					const float *q = Q.data() + (size_t(y) * tw + x) * 4;
					for (float &v : o)
						v /= float(g * g);
					ae.push_back(std::fabs(q[3] - o[3]));
					if (flat[size_t(y) * tw + x] / float(S * S) < 0.5f)
						continue;
					kept++;
					ce.push_back((std::fabs(q[0] - o[0]) + std::fabs(q[1] - o[1]) + std::fabs(q[2] - o[2])) / 3);
				}
			double cs = 0, as = 0;
			for (float v : ce)
				cs += v;
			for (float v : ae)
				as += v;
			out.c_mean = ce.empty() ? 0 : cs / double(ce.size());
			out.a_mean = ae.empty() ? 0 : as / double(ae.size());
			out.c_p99 = p99_of(ce);
			out.a_p99 = p99_of(ae);
			out.kept = double(kept) / double(size_t(tw) * th);
		};
		compare(false, r);
		if (body[0] == 0xB1)
		{
			soul_result other = r;
			compare(true, other);
			if (other.c_mean < r.c_mean)
			{
				r = other;
				r.stored_rows = true;
			}
		}
		r.done = true;
		return r;
	}

	// Whether a soul test's errors pass the limits (verify reports such an entry; convert leaves it out)
	bool over_limits(const options &o, const soul_result &s)
	{
		return s.c_mean > o.soul_mean || s.c_p99 > o.soul_p99 || s.a_mean > o.alpha_mean || s.a_p99 > o.alpha_p99;
	}

	// ---- Convert

	struct input
	{
		std::string rel;
		uint8_t kind = pk::kSourceUnknown;
		uint32_t id = 0xFFFFFFFFu;
		uint64_t size = 0, mtime = 0, overlay_size = 0, overlay_mtime = 0;
	};

	// What one DAT gives (made on a worker thread; added to the pack on the main one, in the inputs' order)
	struct prepared
	{
		struct texture_out
		{
			pk::entry e;
			pk::map_in map;
		};
		std::vector<texture_out> textures;
		std::vector<std::string> log;
		bool refused = false;
		uint32_t upscaled = 0, left = 0, mismatched = 0, b1 = 0, unused = 0, untagged = 0, walk_failed = 0, soul_failed = 0;
		uint64_t texels = 0;
	};

	struct counters
	{
		uint32_t dats = 0, refused = 0, upscaled = 0, left = 0, mismatched = 0, b1 = 0, unused = 0, untagged = 0, walk_failed = 0, soul_failed = 0;
		uint32_t added = 0, same = 0, differs = 0, replaced = 0, add_refused = 0;
		uint32_t by_kind[5] = {};
		uint64_t texels = 0;
	};

	prepared prepare(const options &o, const input &in, const tag_map &zone_tags, const std::set<std::string> &zone_walk_failed, const tag_map &model_tags)
	{
		prepared p;
		auto log = [&](const char *format, ...) {
			char text[1024];
			va_list a;
			va_start(a, format);
			std::vsnprintf(text, sizeof(text), format, a);
			va_end(a);
			p.log.push_back(in.rel + ": " + text);
		};
		std::vector<uint8_t> orig, over;
		if (!bk::read_file(o.ffxi + L"\\" + wide(in.rel), orig))
		{
			log("refused: the original can't be read");
			p.refused = true;
			return p;
		}
		if (!bk::read_file(o.overlay + L"\\" + wide(in.rel), over))
		{
			log("refused: the overlay's copy can't be read");
			p.refused = true;
			return p;
		}
		const std::vector<bk::chunk> a = bk::chunks(orig), b = bk::chunks(over);
		if (a.size() != b.size())
		{
			log("refused: %zu chunks in the original, %zu in the overlay's copy", a.size(), b.size());
			p.refused = true;
			return p;
		}
		const bool walk_failed = in.kind == pk::kSourceZone && zone_walk_failed.count(in.rel) != 0;
		// (every chunk the bake didn't make bigger is the original's, byte for byte: bakeall rewrites only the textures it
		// upscales. One that isn't means the game's file changed since the bake, and the overlay's upscales may be of
		// paintings no longer there: the DAT is refused, and stays out of pack.src so a rerun tries again)
		for (size_t i = 0; i < a.size(); ++i)
		{
			const bk::chunk &c = a[i], &v = b[i];
			const bool bigger = c.type == 0x20 && v.type == 0x20 && v.size > c.size;
			if (!bigger && (c.type != v.type || c.size != v.size || std::memcmp(c.body, v.body, c.size) != 0))
			{
				log("refused: chunk %zu (type 0x%02X) isn't the overlay copy's (%s): the game's file changed since the bake", i, c.type,
					c.type != v.type ? "another type" : c.size != v.size ? "another size" : "other bytes");
				p.refused = true;
				return p;
			}
		}
		for (size_t i = 0; i < a.size(); ++i)
		{
			const bk::chunk &c = a[i], &v = b[i];
			if (c.type != 0x20)
				continue;
			if (c.size < 29 || v.size < 29)
				continue;
			if (std::memcmp(c.body + 1, v.body + 1, 16) != 0)
			{
				log("chunk %zu: named '%s', the overlay's '%s': skipped", i, name16(c.body + 1).c_str(), name16(v.body + 1).c_str());
				p.mismatched++;
				continue;
			}
			if (c.size == v.size)
			{
				p.left++;
				continue;
			}
			const std::string nm = name16(c.body + 1);
			const int32_t ow = i32_at(c.body + 21), oh = i32_at(c.body + 25), vw = i32_at(v.body + 21), vh = i32_at(v.body + 25);
			const bool a1 = v.body[0] == 0xA1 && v.size >= 69 && std::memcmp(v.body + 57, "3TXD", 4) == 0;
			const uint32_t factor = ow > 0 && oh > 0 && vw > 0 ? uint32_t(vw / ow) : 0;
			if (v.size < c.size || !a1 || ow <= 0 || oh <= 0 || vw % 4 != 0 || vh % 4 != 0 || vw > int32_t(pk::kMaxSide) || vh > int32_t(pk::kMaxSide) ||
				(factor != 2 && factor != 4 && factor != 8) || vw != ow * int32_t(factor) || vh != oh * int32_t(factor) ||
				69 + size_t(vw / 4) * size_t(vh / 4) * 16 > v.size)
			{
				log("chunk %zu '%s': the overlay's copy isn't a whole 2x/4x A1 3TXD of it (%dx%d -> %dx%d): skipped", i, nm.c_str(), ow, oh, vw, vh);
				p.mismatched++;
				continue;
			}
			if (c.body[0] != 0xA1 && c.body[0] != 0xB1)
			{
				log("chunk %zu '%s': an original of format 0x%02X: skipped", i, nm.c_str(), c.body[0]);
				p.mismatched++;
				continue;
			}
			p.upscaled++;
			prepared::texture_out t;
			pk::entry &e = t.e;
			const pk::key k = pk::key_of(c.body, c.size);
			e.hash = k.hash, e.bytes = k.bytes, e.w = k.w, e.h = k.h;
			e.orig_format = c.body[0];
			e.orig_fourcc = c.body[0] == 0xA1 && c.size >= 61 ? u32_at(c.body + 57) : 0;
			e.source_kind = in.kind;
			e.file_id = in.id;
			e.chunk = uint32_t(i);
			std::memcpy(e.name, c.body + 1, 16);
			if (c.body[0] == 0xB1)
			{
				e.flags |= pk::kNoPack;
				p.b1++;
			}
			// (the original's alpha range, as the game has its texels)
			tex::texture d;
			std::string why;
			if (tex::decode(c.body, c.size, d, why) && !d.levels.empty())
			{
				uint8_t lo = 255, hi = 0;
				const std::vector<uint8_t> &px = d.levels[0].rgba;
				for (size_t q = 3; q < px.size(); q += 4)
					lo = std::min(lo, px[q]), hi = std::max(hi, px[q]);
				e.alpha_lo = lo, e.alpha_hi = hi;
			}
			else
				log("chunk %zu '%s': the original doesn't decode (%s): no alpha range", i, nm.c_str(), why.c_str());
			float threshold = pk::bc2::kCharsCutThreshold;
			if (in.kind == pk::kSourceZone)
			{
				if (walk_failed)
				{
					e.alpha_kind = pk::kAlphaBlended;
					p.walk_failed++;
				}
				else if (auto it = zone_tags.find(k); it != zone_tags.end())
				{
					e.alpha_kind = alpha_kind_of(it->second);
					if (it->second.cut > 0)
						e.vcol_a = it->second.median_vcol();
				}
				else
				{
					e.alpha_kind = pk::kAlphaNone;
					p.unused++;
				}
				threshold = pk::bc2::zone_cut_threshold(e.vcol_a);
			}
			else
			{
				// (named by no Os2 group of any model DAT of the run: none, the plain box. Cut-out would be the one kind
				// that changes alpha, and a shine mask's strength with it)
				if (auto it = model_tags.find(k); it != model_tags.end())
					e.alpha_kind = alpha_kind_of(it->second);
				else
				{
					e.alpha_kind = pk::kAlphaNone;
					p.untagged++;
				}
				threshold = pk::bc2::kCharsCutThreshold;
			}
			// (the soul test, verify's, against the original: an upscale not of this painting (the original repainted in
			// place since the bake, a pairing gone wrong) is left out. no_pack entries are kept: the game's texture is
			// drawn for them)
			const soul_result s = soul(c.body, c.size, v.body + 69, uint32_t(vw), uint32_t(vh));
			if ((e.flags & pk::kNoPack) == 0 && (!s.done || over_limits(o, s)))
			{
				if (s.done)
					log("chunk %zu '%s': not the original's painting (soul test: colour %.2f / p99 %.2f, alpha %.2f / p99 %.2f): left out", i, nm.c_str(), s.c_mean,
						s.c_p99, s.a_mean, s.a_p99);
				else
					log("chunk %zu '%s': the soul test can't run (%s): left out", i, nm.c_str(), s.why.c_str());
				p.soul_failed++;
				continue;
			}
			t.map.kind = pk::kColour;
			t.map.format = pk::kBC2;
			t.map.w = uint32_t(vw);
			t.map.h = uint32_t(vh);
			t.map.levels = pk::bc2::full_chain(v.body + 69, uint32_t(vw), uint32_t(vh), e.alpha_kind, threshold);
			p.texels += uint64_t(vw) * uint64_t(vh);
			p.textures.push_back(std::move(t));
		}
		return p;
	}

	int convert(options &o)
	{
		if (o.out.empty())
		{
			say("convert needs --out <the pack's folder> (the game's: <bootloader>\\hifixi-pack).");
			return kUsage;
		}
		if (o.lists.empty())
			o.lists = up(up(exe_folder())) + L"\\hifi-textures\\work\\all";
		o.lists = no_slash(o.lists);
		tsv rows;
		if (!read_tsv(o.lists, rows))
		{
			say("%s\\dats.tsv can't be read: give the bake's lists with --lists.", narrow(o.lists).c_str());
			return kFailed;
		}
		if (!find_folders(o, true, true, &rows))
			return kFailed;
		const std::wstring out = with_slash(o.out);
		if (!o.ignore_game)
			if (const std::string why = game_running(o.ashita); !why.empty())
			{
				say("Not now: %s. Convert runs with the game closed.", why.c_str());
				return kGameStarted;
			}
		std::string why;
		if (!hifi::texpack::load_key_tables(o.ffximain.empty() ? o.ffxi + L"\\FFXiMain.dll" : o.ffximain, why))
		{
			say("%s", why.c_str());
			return kFailed;
		}
		id_table ids;
		if (!ids.load(o.ffxi))
		{
			say("The game's VTABLE.DAT/FTABLE.DAT can't be read.");
			return kFailed;
		}

		// The inputs (see the top): the overlay walked against dats.tsv, the logs as a cross-check
		std::set<std::string> logged, palette;
		for (const wchar_t *name : { L"bakeall-zone.log", L"bakeall-model.log" })
		{
			bool ok = false;
			for (const std::string &line : lines_of(o.lists + L"\\" + name, ok))
			{
				const std::vector<std::string> f = split(line, '\t');
				if (f.size() < 2)
					continue;
				(all_digits(f[1]) ? logged : palette).insert(f[0]);
			}
		}
		std::set<std::string> candidates(logged.begin(), logged.end());
		candidates.insert("ROM\\0\\104.DAT");
		uint32_t walked = 0, only_walked = 0;
		if (o.only.empty())
		{
			for (const auto &[rel, r] : rows)
				if ((r.kind == "zone" || r.kind == "model") && r.shadow == "-" && is_file(o.overlay + L"\\" + wide(rel)))
				{
					walked++;
					if (candidates.insert(rel).second && palette.count(rel) == 0)
					{
						if (++only_walked <= 5)
							say("in the overlay, not in the logs: %s", rel.c_str());
					}
				}
		}
		else
			candidates = std::set<std::string>(o.only.begin(), o.only.end());
		std::vector<input> inputs;
		uint32_t skipped_palette = 0, skipped_other = 0;
		for (const std::string &rel : candidates)
		{
			if (palette.count(rel) != 0)
			{
				say("%s: palette only, left by the bake: skipped", rel.c_str());
				skipped_palette++;
				continue;
			}
			auto row = rows.find(rel);
			input in;
			in.rel = rel;
			const char *bad = nullptr;
			if (row == rows.end())
				bad = "not in dats.tsv";
			else if (row->second.kind != "zone" && row->second.kind != "model")
				bad = "neither zone nor model in dats.tsv";
			else if (!o.kind.empty() && row->second.kind != o.kind)
				continue;
			else if (row->second.shadow != "-")
				bad = "an XIPivot overlay ahead of hifi-textures has it";
			else if (!file_info(o.ffxi + L"\\" + wide(rel), in.size, in.mtime))
				bad = "no original";
			else if (in.size != row->second.bytes)
				bad = "the original isn't the size dats.tsv has (the file the bake read): the game's file changed since the bake";
			else if (!file_info(o.overlay + L"\\" + wide(rel), in.overlay_size, in.overlay_mtime))
				bad = "no copy in the overlay";
			if (bad != nullptr)
			{
				say("%s: %s: skipped", rel.c_str(), bad);
				skipped_other++;
				continue;
			}
			in.kind = row->second.kind == "zone" ? pk::kSourceZone : pk::kSourceModel;
			in.id = ids.id(rel);
			inputs.push_back(in);
		}
		std::sort(inputs.begin(), inputs.end(), [](const input &x, const input &y) { return x.kind != y.kind ? x.kind < y.kind : x.rel < y.rel; });
		if (o.only.empty())
			say("inputs: %zu DATs (the logs list %zu, the overlay walk found %u, %u of them in no log); the logs' %zu palette-only rows left out (%u of "
				"them with an overlay copy), %u others skipped",
				inputs.size(), logged.size(), walked, only_walked, palette.size(), skipped_palette, skipped_other);

		// The pack's folder: resumed, or begun
		pk::writer w;
		pk::writer_options wo;
		wo.folder = out;
		wo.maker_version = kMakerVersion;
		wo.source = "overlay convert";
		wo.client = pk::client_fingerprint(o.ffxi);
		if (!w.open(wo, why))
		{
			say("The pack's folder can't be used: %s.", why.c_str());
			return kFailed;
		}
		g_log = open_log((out + L"texpack.log").c_str());
		say("---- texpack convert, maker version %u: %s -> %s (overlay %s)", kMakerVersion, narrow(o.ffxi).c_str(), narrow(out).c_str(), narrow(o.overlay).c_str());
		std::vector<input> todo;
		uint32_t done_before = 0;
		for (const input &in : inputs)
		{
			if (!o.redo && w.done(in.rel, in.size, in.mtime, in.overlay_size, in.overlay_mtime))
			{
				done_before++;
				continue;
			}
			if (o.limit != 0 && todo.size() >= o.limit)
				break;
			todo.push_back(in);
		}
		say("%u DATs already done (pack.src), %zu to do; the pack has %u entries", done_before, todo.size(), w.stats().kept);

		// Free space: 20 GB plus the estimate (the overlay copies' growth with whole mip chains: at most what's added,
		// keys several DATs share stored once; the whole overlay measured 9.6 GB of level 0, 5.45 GB of it unique)
		uint64_t grow = 0;
		for (const input &in : todo)
			grow += in.overlay_size > in.size ? in.overlay_size - in.size : 0;
		const uint64_t estimate = uint64_t(double(grow) * 4.0 / 3.0 * 1.05);
		ULARGE_INTEGER free_bytes = {};
		std::wstring probe = no_slash(out);
		while (!probe.empty() && !is_folder(probe))
			probe = up(probe);
		GetDiskFreeSpaceExW(probe.c_str(), &free_bytes, nullptr, nullptr);
		say("estimate: at most %.2f GB more (about 57%% of that once keys shared between DATs are counted once); %.1f GB free",
			gb(estimate), gb(free_bytes.QuadPart));
		if (double(free_bytes.QuadPart) < o.reserve_gb * double(1ull << 30) + double(estimate))
		{
			say("Refused: the disk needs %.0f GB free beyond the estimate.", o.reserve_gb);
			return kFailed;
		}
		SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
		const auto start = std::chrono::steady_clock::now();

		// The tags first, over every input of the kind (not only the DATs to do: a run narrowed by --limit, or resumed,
		// tags a texture several DATs share as one whole run does, and the first entry of a key stays). --only narrows the
		// inputs themselves, and so the tags. Zones: zonemesh's walk; models: their Os2 meshes, onto the keys
		tag_map zone_tags, model_tags;
		std::set<std::string> walk_failed;
		for (const uint8_t kind : { pk::kSourceZone, pk::kSourceModel })
		{
			if (std::none_of(todo.begin(), todo.end(), [&](const input &in) { return in.kind == kind; }))
				continue;
			const auto began = std::chrono::steady_clock::now();
			std::vector<const input *> all;
			for (const input &in : inputs)
				if (in.kind == kind)
					all.push_back(&in);
			std::atomic<size_t> next { 0 };
			std::vector<tag_map> part(o.threads);
			std::vector<std::vector<std::string>> fails(o.threads);
			std::vector<std::thread> pool;
			for (uint32_t t = 0; t < o.threads; ++t)
				pool.emplace_back([&, t] {
					std::vector<uint8_t> dat;
					for (size_t i; (i = next++) < all.size();)
					{
						std::string why2;
						if (kind == pk::kSourceModel)
						{
							// (a DAT that can't be read is refused by prepare if it's to do; here it only names nothing)
							if (bk::read_file(o.ffxi + L"\\" + wide(all[i]->rel), dat))
								tag_models(bk::chunks(dat), part[t]);
							continue;
						}
						tag_map one;
						if (tag_zone(o.ffxi, all[i]->rel, all[i]->id, one, why2))
							for (auto &[k, tg] : one)
								part[t][k].merge(tg);
						else
							fails[t].push_back(all[i]->rel + "\t" + why2);
					}
				});
			for (std::thread &t : pool)
				t.join();
			tag_map &tags = kind == pk::kSourceZone ? zone_tags : model_tags;
			for (const tag_map &m : part)
				for (const auto &[k, tg] : m)
					tags[k].merge(tg);
			for (const auto &f : fails)
				for (const std::string &line : f)
				{
					const size_t tab = line.find('\t');
					walk_failed.insert(line.substr(0, tab));
					say("%s: its meshes can't be read (%s): its textures marked blended", line.substr(0, tab).c_str(), line.substr(tab + 1).c_str());
				}
			say("%s tags: %zu DATs read in %.1f s, %zu textures named", kind == pk::kSourceZone ? "zone" : "model", all.size(), seconds_since(began), tags.size());
		}

		// Each DAT prepared on the workers (o.threads ahead), added here in order
		counters n;
		std::deque<std::future<prepared>> ahead;
		size_t queued = 0;
		auto launch = [&] {
			while (queued < todo.size() && ahead.size() < o.threads)
			{
				const input *in = &todo[queued++];
				ahead.push_back(std::async(std::launch::async, [&, in] { return prepare(o, *in, zone_tags, walk_failed, model_tags); }));
			}
		};
		auto progress = [&](const std::string &state, const std::string &last) {
			char text[1024];
			std::snprintf(text, sizeof(text),
				"texpack convert (maker version %u): %s\r\nDATs done %u of %zu this run (%u refused); %u done before\r\n"
				"entries %zu (added %u, replaced %u, the same key again %u, differing %u, refused %u)\r\ntextures upscaled %u, left %u, skipped %u, "
				"failing the soul test %u\r\nwritten %.2f GB in %.0f s\r\nlast: %s\r\n",
				kMakerVersion, state.c_str(), n.dats, todo.size(), n.refused, done_before, w.entries().size(), n.added, n.replaced, n.same, n.differs,
				n.add_refused, n.upscaled, n.left, n.mismatched, n.soul_failed, gb(w.stats().bytes), seconds_since(start), last.c_str());
			pk::write_progress(out, text);
		};
		int code = kDone;
		auto last_commit = std::chrono::steady_clock::now(), last_game = last_commit;
		uint32_t since_commit = 0;
		std::string last;
		for (size_t i = 0; i < todo.size(); ++i)
		{
			if (pk::stop_requested(out))
			{
				say("Stopped by %sstop (delete it to go on).", narrow(out).c_str());
				code = kStopped;
				break;
			}
			if (!o.ignore_game && seconds_since(last_game) > 30)
			{
				last_game = std::chrono::steady_clock::now();
				if (const std::string g = game_running(o.ashita); !g.empty())
				{
					say("Stopped: %s. Run convert again with the game closed: it goes on where it stopped.", g.c_str());
					code = kGameStarted;
					break;
				}
			}
			launch();
			prepared p = ahead.front().get();
			ahead.pop_front();
			const input &in = todo[i];
			for (const std::string &line : p.log)
				say("%s", line.c_str());
			n.dats++;
			if (p.refused)
			{
				n.refused++;
				continue;
			}
			n.upscaled += p.upscaled, n.left += p.left, n.mismatched += p.mismatched, n.b1 += p.b1, n.unused += p.unused;
			n.untagged += p.untagged, n.walk_failed += p.walk_failed, n.soul_failed += p.soul_failed, n.texels += p.texels;
			bool failed = false;
			for (prepared::texture_out &t : p.textures)
			{
				// (an entry this DAT's chunk made at its last convert is replaced by what its overlay copy gives now: a
				// re-baked copy, or --redo. Between DATs the first entry of a key stays. A few model DATs have no file
				// id, so theirs can't be told apart and stay)
				const pk::entry *had = w.find(t.e.the_key());
				const bool own = had != nullptr && in.id != 0xFFFFFFFFu && had->file_id == in.id && had->chunk == t.e.chunk &&
					std::memcmp(had->name, t.e.name, 16) == 0;
				std::string why2;
				switch (w.add(t.e, { t.map }, why2, own))
				{
				case pk::add_result::ok:
					n.added++;
					n.by_kind[std::min<uint8_t>(t.e.alpha_kind, 4)]++;
					break;
				case pk::add_result::replaced:
					n.replaced++;
					n.by_kind[std::min<uint8_t>(t.e.alpha_kind, 4)]++;
					say("%s: chunk %u '%s': the entry this DAT made before replaced (its overlay copy changed)", in.rel.c_str(), t.e.chunk,
						name16(reinterpret_cast<const uint8_t *>(t.e.name)).c_str());
					break;
				case pk::add_result::same:
					n.same++;
					break;
				case pk::add_result::differs:
					n.differs++;
					say("%s: chunk %u '%s': its key is in the pack already with other bytes (another bake run's): the first kept", in.rel.c_str(), t.e.chunk,
						name16(reinterpret_cast<const uint8_t *>(t.e.name)).c_str());
					break;
				case pk::add_result::refused:
					n.add_refused++;
					say("%s: chunk %u '%s': not added: %s", in.rel.c_str(), t.e.chunk, name16(reinterpret_cast<const uint8_t *>(t.e.name)).c_str(), why2.c_str());
					failed = failed || why2.find("writing") != std::string::npos || why2.find("can't open") != std::string::npos;
					break;
				}
			}
			if (failed)
			{
				say("Stopped: the pack's data file can't be written.");
				code = kFailed;
				break;
			}
			w.finished(in.rel, in.size, in.mtime, in.overlay_size, in.overlay_mtime);
			last = in.rel;
			if (++since_commit >= 25 || seconds_since(last_commit) > 20)
			{
				if (!w.commit(why))
				{
					say("Stopped: %s.", why.c_str());
					code = kFailed;
					break;
				}
				since_commit = 0;
				last_commit = std::chrono::steady_clock::now();
				progress("running", last);
			}
		}
		// (what's still being prepared is let finish and dropped)
		for (auto &f : ahead)
			f.wait();
		if (code != kFailed && !w.commit(why))
		{
			say("The last commit failed: %s.", why.c_str());
			code = kFailed;
		}
		const double took = seconds_since(start);
		progress(code == kDone ? "finished" : code == kStopped ? "stopped (the stop file)" : code == kGameStarted ? "stopped (the game)" : "failed", last);
		say("DATs %u (%u refused) in %.1f s; textures upscaled %u (%.1f M texels), left %u, skipped %u, failing the soul test %u; B1 (no_pack) %u", n.dats,
			n.refused, took, n.upscaled, double(n.texels) / 1e6, n.left, n.mismatched, n.soul_failed, n.b1);
		say("entries: added %u, replaced %u (none %u, cut-out %u, blended %u, mask %u, mixed %u), the same key again %u, differing %u, refused %u; %zu in the "
			"pack",
			n.added, n.replaced, n.by_kind[0], n.by_kind[1], n.by_kind[2], n.by_kind[3], n.by_kind[4], n.same, n.differs, n.add_refused, w.entries().size());
		say("tags: zone textures no mesh uses %u, in zones whose meshes can't be read %u (blended); model textures no Os2 group names %u (none)",
			n.unused, n.walk_failed, n.untagged);
		say("written %.3f GB (%.1f MB/s)", gb(w.stats().bytes), took > 0 ? double(w.stats().bytes) / 1e6 / took : 0.0);
		w.close();
		return code;
	}

	std::shared_ptr<const pk::index> open_pack(options &o, std::string &why)
	{
		if (o.out.empty())
		{
			why = "give the pack's folder with --pack";
			return nullptr;
		}
		return pk::load(with_slash(o.out), nullptr, why);
	}

	const char *kind_name(uint8_t k)
	{
		static const char *names[] = { "none", "cut-out", "blended", "mask", "mixed" };
		return k < 5 ? names[k] : "?";
	}

	// pack.src's DATs (the last line for each counts), in order
	std::vector<std::string> sources_of(const std::wstring &folder)
	{
		bool ok = false;
		std::set<std::string> seen;
		for (const std::string &line : lines_of(with_slash(folder) + L"pack.src", ok))
		{
			const std::vector<std::string> f = split(line, '\t');
			if (f.size() >= 4 && !f[0].empty())
				seen.insert(f[0]);
		}
		return std::vector<std::string>(seen.begin(), seen.end());
	}

	int verify(options &o)
	{
		std::string why;
		std::shared_ptr<const pk::index> idx = open_pack(o, why);
		if (!idx)
		{
			std::printf("The pack can't be used: %s\n", why.c_str());
			return kFailed;
		}
		id_table ids;
		if (o.soul)
		{
			tsv rows;
			const bool have_rows = read_rows(o, rows);
			if (!find_folders(o, true, false, have_rows ? &rows : nullptr) || !ids.load(o.ffxi))
				return kFailed;
		}
		const auto start = std::chrono::steady_clock::now();
		const std::vector<pk::entry> &es = idx->entries;
		std::vector<std::string> problem(es.size());
		std::atomic<uint64_t> bytes_read { 0 };

		// Every record read back: the rules, its CRC, each level's (entries in pieces, one reader a thread)
		{
			std::atomic<size_t> next { 0 };
			std::vector<std::thread> pool;
			for (uint32_t t = 0; t < o.threads; ++t)
				pool.emplace_back([&] {
					pk::reader rd(idx);
					std::vector<uint8_t> buf;
					for (size_t first; (first = next.fetch_add(64)) < es.size();)
						for (size_t i = first; i < std::min(es.size(), first + 64); ++i)
							for (const pk::record &r : es[i].maps)
							{
								if (r.kind < pk::kColour || r.kind > pk::kMaterial)
									continue;
								buf.resize(r.bytes);
								std::string w2;
								if (!rd.read_record(r, buf.data(), w2))
								{
									problem[i] = "record " + std::to_string(r.kind) + ": " + w2;
									break;
								}
								bytes_read += r.bytes;
								if (const std::string bad = level_crcs(*idx, r, buf); !bad.empty())
								{
									problem[i] = "record " + std::to_string(r.kind) + ": " + bad;
									break;
								}
							}
				});
			for (std::thread &t : pool)
				t.join();
		}
		const double took = seconds_since(start);
		uint32_t bad = 0;
		for (size_t i = 0; i < es.size(); ++i)
			if (!problem[i].empty())
			{
				if (++bad <= 50)
					std::printf("FAIL %016llX:%u:%ux%u '%s' (file %u chunk %u): %s\n", static_cast<unsigned long long>(es[i].hash), es[i].bytes, es[i].w, es[i].h,
						name16(reinterpret_cast<const uint8_t *>(es[i].name)).c_str(), es[i].file_id, es[i].chunk, problem[i].c_str());
			}
		std::printf("records: %zu entries read back (%.2f GB) in %.1f s, %u bad\n", es.size(), gb(bytes_read), took, bad);
		if (!o.soul)
			return bad == 0 ? kDone : kFailed;

		// The soul test: pack.src's DATs read from the game's folder, each entry tested on the first texture chunk with
		// its key (any such chunk is the same bytes; a few model DATs have no file id to find them by)
		const std::vector<std::string> sources = sources_of(o.out);
		std::vector<soul_result> souls(es.size());
		std::vector<std::string> where(es.size());
		std::unique_ptr<std::atomic<bool>[]> claimed(new std::atomic<bool>[es.size()]);
		for (size_t i = 0; i < es.size(); ++i)
			claimed[i] = false;
		{
			std::atomic<size_t> next { 0 };
			std::vector<std::thread> pool;
			for (uint32_t t = 0; t < o.threads; ++t)
				pool.emplace_back([&] {
					pk::reader rd(idx);
					std::vector<uint8_t> buf, dat;
					for (size_t si; (si = next++) < sources.size();)
					{
						if (!bk::read_file(o.ffxi + L"\\" + wide(sources[si]), dat))
							continue;
						const std::vector<bk::chunk> cs = bk::chunks(dat);
						for (size_t c = 0; c < cs.size(); ++c)
						{
							if (cs[c].type != 0x20 || cs[c].size < 29)
								continue;
							const pk::entry *e = idx->find(pk::key_of(cs[c].body, cs[c].size));
							if (e == nullptr)
								continue;
							const size_t i = size_t(e - es.data());
							if (!problem[i].empty() || claimed[i].exchange(true))
								continue;
							where[i] = sources[si] + " chunk " + std::to_string(c);
							const pk::record *colour = e->map(pk::kColour);
							std::vector<std::vector<uint8_t>> levels;
							std::string w2;
							if (colour == nullptr)
							{
								souls[i].why = "no colour record";
								continue;
							}
							buf.resize(colour->bytes);
							if (!rd.read_record(*colour, buf.data(), w2) || !untile(*colour, buf, levels))
							{
								souls[i].why = "its colour record can't be read: " + w2;
								continue;
							}
							souls[i] = soul(cs[c].body, cs[c].size, levels[0].data(), colour->w, colour->h);
						}
					}
				});
			for (std::thread &t : pool)
				t.join();
		}
		for (size_t i = 0; i < es.size(); ++i)
			if (problem[i].empty() && !claimed[i] && souls[i].why.empty())
				souls[i].why = "no DAT of pack.src has its original chunk any more (prune)";

		// The distribution, by source kind (the zones' set the limits), and the entries over the limits
		auto pct = [](std::vector<double> v, double q) {
			if (v.empty())
				return 0.0;
			std::sort(v.begin(), v.end());
			return v[size_t(q * double(v.size() - 1))];
		};
		for (int kind = 1; kind <= 2; ++kind)
		{
			std::vector<double> cm, cp, am, ap, kept;
			for (size_t i = 0; i < es.size(); ++i)
				if (es[i].source_kind == kind && souls[i].done && (es[i].flags & pk::kNoPack) == 0)
				{
					cm.push_back(souls[i].c_mean), cp.push_back(souls[i].c_p99), am.push_back(souls[i].a_mean), ap.push_back(souls[i].a_p99);
					kept.push_back(souls[i].kept);
				}
			if (cm.empty())
				continue;
			std::printf("soul, %s entries (%zu): percentiles over entries   p50     p90     p99     p99.9   max\n", kind == 1 ? "zone" : "model", cm.size());
			auto row = [&](const char *what, const std::vector<double> &v) {
				std::printf("  %-34s %7.2f %7.2f %7.2f %7.2f %7.2f\n", what, pct(v, 0.5), pct(v, 0.9), pct(v, 0.99), pct(v, 0.999), pct(v, 1.0));
			};
			row("colour error, entry mean", cm);
			row("colour error, entry p99", cp);
			row("alpha error, entry mean", am);
			row("alpha error, entry p99", ap);
			row("texels compared (not at an edge)", kept);
		}
		std::printf("soul limits: colour mean %.2f, p99 %.2f; alpha mean %.2f, p99 %.2f (levels of 255)\n", o.soul_mean, o.soul_p99, o.alpha_mean, o.alpha_p99);
		auto over = [&](const soul_result &s) { return over_limits(o, s); };
		auto describe = [&](size_t i) {
			char text[512];
			const soul_result &s = souls[i];
			std::snprintf(text, sizeof(text), "'%s' (%s %s, %s, %ux%u x%u%s%s): colour %.2f / p99 %.2f, alpha %.2f / p99 %.2f, %.0f%% compared",
				name16(reinterpret_cast<const uint8_t *>(es[i].name)).c_str(), es[i].source_kind == 1 ? "zone" : "model", kind_name(es[i].alpha_kind), where[i].c_str(),
				es[i].w, es[i].h, s.factor, es[i].orig_format == 0xB1 ? ", B1" : es[i].orig_fourcc == 0x44585431u ? ", DXT1" : "", s.stored_rows ? ", rows as stored" : "",
				s.c_mean, s.c_p99, s.a_mean, s.a_p99, s.kept * 100);
			return std::string(text);
		};
		uint32_t soul_bad = 0, used_bad = 0, not_done = 0;
		std::vector<size_t> tested;
		for (size_t i = 0; i < es.size(); ++i)
		{
			if (!problem[i].empty())
				continue;
			if (!souls[i].done)
			{
				if (++not_done <= 20)
					std::printf("not tested: '%s' (file %u chunk %u): %s\n", name16(reinterpret_cast<const uint8_t *>(es[i].name)).c_str(), es[i].file_id, es[i].chunk,
						souls[i].why.c_str());
				continue;
			}
			tested.push_back(i);
			if (over(souls[i]))
			{
				soul_bad++;
				// (no_pack entries are reported but don't fail: the game's texture is drawn for them)
				const bool used = (es[i].flags & pk::kNoPack) == 0;
				used_bad += used;
				std::printf("SOUL %s %s\n", used ? "FAIL" : "(no_pack)", describe(i).c_str());
			}
		}
		std::sort(tested.begin(), tested.end(), [&](size_t a, size_t b) { return souls[a].c_mean > souls[b].c_mean; });
		for (size_t k = 0; k < tested.size() && k < o.worst; ++k)
			std::printf("worst %2zu: %s\n", k + 1, describe(tested[k]).c_str());
		std::printf("soul: %zu entries tested, %u over the limits (%u of them used), %u not tested; %.1f s in all\n", tested.size(), soul_bad, used_bad, not_done,
			seconds_since(start));
		return bad == 0 && used_bad == 0 ? kDone : kFailed;
	}

	// ---- Stats

	int stats(options &o)
	{
		std::string why;
		std::shared_ptr<const pk::index> idx = open_pack(o, why);
		if (!idx)
		{
			std::printf("The pack can't be used: %s\n", why.c_str());
			return kFailed;
		}
		const pk::index &x = *idx;
		const std::wstring folder = with_slash(o.out);
		uint32_t by_source[3] = {}, by_alpha[5] = {}, no_pack = 0, a1_dxt3 = 0, a1_dxt1 = 0, a1_other = 0, b1 = 0, vcol80 = 0, vcolff = 0, vcol_other = 0;
		uint64_t record_bytes = 0, level0_bytes = 0, texels = 0;
		uint32_t levels_hist[16] = {};
		for (const pk::entry &e : x.entries)
		{
			by_source[std::min<uint8_t>(e.source_kind, 2)]++;
			by_alpha[std::min<uint8_t>(e.alpha_kind, 4)]++;
			no_pack += (e.flags & pk::kNoPack) != 0;
			if (e.orig_format == 0xB1)
				b1++;
			else if (e.orig_fourcc == 0x44585433u) // ('3TXD' as the file's bytes)
				a1_dxt3++;
			else if (e.orig_fourcc == 0x44585431u)
				a1_dxt1++;
			else
				a1_other++;
			if (e.source_kind == pk::kSourceZone && (e.alpha_kind == pk::kAlphaCut))
				(e.vcol_a == 0x80 ? vcol80 : e.vcol_a == 0xFF ? vcolff : vcol_other)++;
			for (const pk::record &r : e.maps)
				if (r.kind == pk::kColour)
				{
					record_bytes += r.bytes;
					level0_bytes += uint64_t(r.w / 4) * (r.h / 4) * 16;
					texels += uint64_t(r.w) * r.h;
					levels_hist[std::min<uint8_t>(r.levels, 15)]++;
				}
		}
		uint64_t disk = 0;
		for (uint32_t f = 0; f < x.file_bytes.size(); ++f)
		{
			uint64_t size = 0, t = 0;
			file_info(pk::data_path(folder, f), size, t);
			disk += size;
		}
		uint64_t idx_size = 0, t0 = 0;
		file_info(folder + L"pack.idx", idx_size, t0);
		bool ok = false;
		std::map<std::string, std::string> sources;
		for (const std::string &line : lines_of(folder + L"pack.src", ok))
		{
			const std::vector<std::string> f = split(line, '\t');
			if (f.size() >= 4)
				sources[f[0]] = f[3];
		}
		char when[64] = "?";
		{
			FILETIME ft, lt;
			ft.dwLowDateTime = DWORD(x.head.build_time), ft.dwHighDateTime = DWORD(x.head.build_time >> 32);
			SYSTEMTIME st;
			if (FileTimeToLocalFileTime(&ft, &lt) && FileTimeToSystemTime(&lt, &st))
				std::snprintf(when, sizeof(when), "%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
		}
		std::printf("pack %s: '%.96s', maker version %u, written %s\n", narrow(folder).c_str(), x.head.source, x.head.maker_version, when);
		std::printf("entries %zu: zones %u, models %u, unknown %u; no_pack %u\n", x.entries.size(), by_source[1], by_source[2], by_source[0], no_pack);
		std::printf("alpha kinds: none %u, cut-out %u (zones at vcol 0x80 %u, 0xFF %u, other %u), blended %u, mask %u, mixed %u\n", by_alpha[0], by_alpha[1], vcol80,
			vcolff, vcol_other, by_alpha[2], by_alpha[3], by_alpha[4]);
		std::printf("originals: A1 DXT3 %u, A1 DXT1 %u, A1 other %u, B1 %u\n", a1_dxt3, a1_dxt1, a1_other, b1);
		std::printf("levels:");
		for (int i = 1; i < 16; ++i)
			if (levels_hist[i] != 0)
				std::printf(" %d: %u", i, levels_hist[i]);
		std::printf("\nbytes: level 0 %.3f GB (%.0f M texels), all levels in the copy layout %.3f GB; data files %zu, %.3f GB on disk (%.1f MB not in a record); "
					"pack.idx %.1f MB\n",
			gb(level0_bytes), double(texels) / 1e6, gb(record_bytes), x.file_bytes.size(), gb(disk), double(disk > record_bytes ? disk - record_bytes : 0) / 1e6,
			double(idx_size) / 1e6);
		std::printf("sources (pack.src): %zu DATs\n", sources.size());
		if (!o.scan)
			return kDone;

		// The overlay's copies of pack.src's DATs: their upscaled textures against the pack
		tsv rows;
		const bool have_rows = read_rows(o, rows);
		if (!find_folders(o, true, true, have_rows ? &rows : nullptr))
			return kFailed;
		uint64_t textures = 0, hits = 0, same = 0, differs = 0, bytes_all = 0, missing_dats = 0;
		std::set<pk::key> keys;
		for (const auto &[rel, maker] : sources)
		{
			std::vector<uint8_t> a, b;
			if (!bk::read_file(o.ffxi + L"\\" + wide(rel), a) || !bk::read_file(o.overlay + L"\\" + wide(rel), b))
			{
				missing_dats++;
				continue;
			}
			const std::vector<bk::chunk> ca = bk::chunks(a), cb = bk::chunks(b);
			if (ca.size() != cb.size())
				continue;
			for (size_t i = 0; i < ca.size(); ++i)
			{
				const bk::chunk &c = ca[i], &v = cb[i];
				if (c.type != 0x20 || v.type != 0x20 || c.size == v.size || v.size < 69 || c.size < 29 || v.body[0] != 0xA1 || std::memcmp(v.body + 57, "3TXD", 4) != 0)
					continue;
				textures++;
				const pk::key k = pk::key_of(c.body, c.size);
				keys.insert(k);
				const pk::entry *e = x.find(k);
				const pk::record *r = e != nullptr ? e->map(pk::kColour) : nullptr;
				if (r == nullptr)
					continue;
				hits++;
				bytes_all += r->bytes;
				// (the overlay's level 0 laid out as the pack stores it, against the pack's level 0 CRC)
				pk::layout l;
				const uint32_t vw = uint32_t(i32_at(v.body + 21)), vh = uint32_t(i32_at(v.body + 25));
				if (vw != r->w || vh != r->h || !pk::footprint(r->format, r->w, r->h, r->levels, l) || 69 + size_t(vw / 4) * (vh / 4) * 16 > v.size)
				{
					differs++;
					continue;
				}
				std::vector<uint8_t> level(size_t(l.level_span(0)), 0);
				for (uint32_t row = 0; row < l.level[0].rows; ++row)
					std::memcpy(level.data() + size_t(row) * l.level[0].row_pitch, v.body + 69 + size_t(row) * l.level[0].row_bytes, l.level[0].row_bytes);
				(pk::crc32(0, level.data(), level.size()) == x.level_crc(*r, 0) ? same : differs)++;
			}
		}
		uint64_t unique_bytes = 0;
		for (const pk::key &k : keys)
			if (const pk::entry *e = x.find(k))
				if (const pk::record *r = e->map(pk::kColour))
					unique_bytes += r->bytes;
		std::printf("overlay scan (%zu DATs, %llu unreadable): upscaled textures %llu, distinct keys %zu, in the pack %llu of %llu textures (%.2f%%), %zu keys missing\n",
			sources.size(), static_cast<unsigned long long>(missing_dats), static_cast<unsigned long long>(textures), keys.size(), static_cast<unsigned long long>(hits),
			static_cast<unsigned long long>(textures), textures ? 100.0 * double(hits) / double(textures) : 0.0,
			size_t(std::count_if(keys.begin(), keys.end(), [&](const pk::key &k) { return x.find(k) == nullptr; })));
		std::printf("dedupe: %llu textures stored as %zu entries, %.3f GB instead of %.3f GB (saved %.3f GB); level 0 the same as the overlay's %llu, other bytes %llu\n",
			static_cast<unsigned long long>(textures), keys.size(), gb(unique_bytes), gb(bytes_all), gb(bytes_all - std::min(bytes_all, unique_bytes)),
			static_cast<unsigned long long>(same), static_cast<unsigned long long>(differs));
		return kDone;
	}

	// ---- Compact and prune

	// pack.src's DATs as the game's folder has them now: each one's texture keys, or why it has none
	struct source_scan
	{
		enum state_t : uint8_t
		{
			read,
			gone,       // no such file (or folder): the game's file is gone
			unreadable, // there, but it can't be read (held by another program, a disk error)
		};
		std::vector<std::vector<pk::key>> keys;
		std::vector<state_t> state;
		std::vector<std::string> why;
	};

	source_scan scan_sources(const options &o, const std::vector<std::string> &sources)
	{
		source_scan s;
		s.keys.resize(sources.size());
		s.state.assign(sources.size(), source_scan::read);
		s.why.resize(sources.size());
		std::atomic<size_t> next { 0 };
		std::vector<std::thread> pool;
		for (uint32_t t = 0; t < o.threads; ++t)
			pool.emplace_back([&] {
				std::vector<uint8_t> dat;
				for (size_t si; (si = next++) < sources.size();)
				{
					const std::wstring path = o.ffxi + L"\\" + wide(sources[si]);
					if (!bk::read_file(path, dat))
					{
						const DWORD a = GetFileAttributesW(path.c_str()), e = a == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
						s.state[si] = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? source_scan::gone : source_scan::unreadable;
						s.why[si] = e != 0 ? "error " + std::to_string(e) : "it's there, but reading it failed";
						continue;
					}
					for (const bk::chunk &c : bk::chunks(dat))
						if (c.type == 0x20 && c.size >= 29)
							s.keys[si].push_back(pk::key_of(c.body, c.size));
				}
			});
		for (std::thread &t : pool)
			t.join();
		return s;
	}

	int compact(options &o)
	{
		std::string why;
		if (o.out.empty())
		{
			std::printf("give the pack's folder with --pack\n");
			return kUsage;
		}
		find_folders(o, false, false, nullptr);
		if (!o.ignore_game)
			if (const std::string g = game_running(o.ashita); !g.empty())
			{
				std::printf("Not now: %s.\n", g.c_str());
				return kGameStarted;
			}
		const std::wstring folder = no_slash(o.out), fresh = folder + L"-compact", old = folder + L"-old";
		if (is_folder(fresh) || is_folder(old))
		{
			std::printf("%s or %s is there already: delete or rename it first.\n", narrow(fresh).c_str(), narrow(old).c_str());
			return kFailed;
		}
		// (the old pack's pack.lock held while it's copied, before its index is read: a convert or prune meanwhile would
		// change it under the copy, and the copy would take its name without what they added)
		pk::folder_lock held;
		if (!held.take(folder, why))
		{
			std::printf("%s.\n", why.c_str());
			return kFailed;
		}
		std::shared_ptr<const pk::index> idx = open_pack(o, why);
		if (!idx)
		{
			std::printf("The pack can't be used: %s\n", why.c_str());
			return kFailed;
		}
		g_log = open_log((folder + L"\\texpack.log").c_str());
		say("---- texpack compact: %s", narrow(folder).c_str());
		pk::writer w;
		pk::writer_options wo;
		wo.folder = fresh;
		wo.maker_version = idx->head.maker_version;
		wo.source = std::string(idx->head.source, strnlen(idx->head.source, sizeof(idx->head.source)));
		wo.client = pk::fingerprint { idx->head.vtable_size, idx->head.vtable_mtime, idx->head.ftable_size, idx->head.ftable_mtime };
		if (!w.open(wo, why))
		{
			say("%s", why.c_str());
			return kFailed;
		}
		// (in the old files' order, so they're read front to back)
		std::vector<const pk::entry *> order;
		for (const pk::entry &e : idx->entries)
			order.push_back(&e);
		auto first = [](const pk::entry *e) {
			for (const pk::record &r : e->maps)
				if (r.kind != pk::kUnused)
					return (uint64_t(r.file) << 32) | r.offset;
			return ~0ull;
		};
		std::sort(order.begin(), order.end(), [&](const pk::entry *a, const pk::entry *b) { return first(a) < first(b); });
		pk::reader rd(idx);
		std::vector<uint8_t> buf;
		std::set<pk::key> lost;
		for (const pk::entry *e : order)
		{
			std::vector<pk::map_in> maps;
			for (const pk::record &r : e->maps)
			{
				if (r.kind < pk::kColour || r.kind > pk::kMaterial)
					continue;
				buf.resize(r.bytes);
				pk::map_in m;
				m.kind = r.kind, m.format = r.format, m.w = r.w, m.h = r.h;
				if (!rd.read_record(r, buf.data(), why) || !untile(r, buf, m.levels))
				{
					say("'%s' (file %u chunk %u): a record can't be read (%s): left out, and the DATs that have it converted again",
						name16(reinterpret_cast<const uint8_t *>(e->name)).c_str(), e->file_id, e->chunk, why.c_str());
					lost.insert(e->the_key());
					maps.clear();
					break;
				}
				maps.push_back(std::move(m));
			}
			if (!maps.empty() && w.add(*e, maps, why) == pk::add_result::refused)
			{
				say("Stopped: %s (the old pack is unchanged; delete %s).", why.c_str(), narrow(fresh).c_str());
				return kFailed;
			}
		}
		if (!w.commit(why))
		{
			say("Stopped: %s (the old pack is unchanged; delete %s).", why.c_str(), narrow(fresh).c_str());
			return kFailed;
		}
		w.close();
		rd.close();
		// pack.src copied, less the lines of the DATs that have a key left out: kept, they'd say those DATs are done, and
		// convert would never write the entries again. Which DATs: pack.src's, read from the game's folder (a key is the
		// texture's whole body: whichever DAT first gave it); one that can't be read is converted again too
		bool ok = false;
		const std::vector<std::string> lines = lines_of(folder + L"\\pack.src", ok);
		std::set<std::string> again;
		if (!lost.empty())
		{
			tsv rows;
			const bool have_rows = read_rows(o, rows);
			if (!find_folders(o, true, false, have_rows ? &rows : nullptr))
			{
				say("Stopped: %zu entries can't be read, and the DATs that have them can't be found without the game's folder: the old pack is unchanged; "
					"delete %s and compact again with --ffxi.",
					lost.size(), narrow(fresh).c_str());
				return kFailed;
			}
			std::set<std::string> listed;
			for (const std::string &line : lines)
				if (const std::vector<std::string> f = split(line, '\t'); f.size() >= 4 && !f[0].empty())
					listed.insert(f[0]);
			const std::vector<std::string> sources(listed.begin(), listed.end());
			const source_scan scan = scan_sources(o, sources);
			for (size_t i = 0; i < sources.size(); ++i)
			{
				bool has = scan.state[i] != source_scan::read;
				for (const pk::key &k : scan.keys[i])
					has = has || lost.count(k) != 0;
				if (has)
					again.insert(sources[i]);
			}
			say("%zu entries left out: %zu DATs of pack.src have them (or can't be read), their lines left out of pack.src", lost.size(), again.size());
		}
		std::string text;
		for (const std::string &line : lines)
			if (!line.empty() && again.count(split(line, '\t')[0]) == 0)
				text += line + "\r\n";
		if (!bk::write_file(fresh + L"\\pack.src", text.data(), text.size()))
		{
			say("Stopped: %s\\pack.src can't be written (the old pack is unchanged; delete %s).", narrow(fresh).c_str(), narrow(fresh).c_str());
			return kFailed;
		}
		uint64_t before = 0, after = 0, size = 0, t = 0;
		for (uint32_t f = 0; f < idx->file_bytes.size(); ++f)
			before += file_info(pk::data_path(folder + L"\\", f), size, t) ? size : 0;
		for (uint32_t f = 0; f < pk::kMaxFiles && file_info(pk::data_path(fresh + L"\\", f), size, t); ++f)
			after += size;
		const size_t kept = w.entries().size();
		idx.reset();
		say("compacted %zu entries: %.3f GB of data files -> %.3f GB", kept, gb(before), gb(after));
		std::fclose(g_log);
		g_log = nullptr;
		// (let go only now: a folder with a file open in it can't be renamed)
		held.release();
		if (!MoveFileExW(folder.c_str(), old.c_str(), 0))
		{
			std::printf("The pack can't be renamed (error %lu: is the game reading it, or a texpack writing it?): the compacted copy is %s.\n", GetLastError(),
				narrow(fresh).c_str());
			return kFailed;
		}
		if (!MoveFileExW(fresh.c_str(), folder.c_str(), 0))
		{
			MoveFileExW(old.c_str(), folder.c_str(), 0);
			std::printf("The compacted copy can't take the pack's name (error %lu): it's %s.\n", GetLastError(), narrow(fresh).c_str());
			return kFailed;
		}
		std::printf("The pack is compacted; the old one is %s (delete it when you're happy).\n", narrow(old).c_str());
		return kDone;
	}

	int prune(options &o)
	{
		std::string why;
		if (o.out.empty())
		{
			std::printf("give the pack's folder with --pack\n");
			return kUsage;
		}
		tsv rows;
		const bool have_rows = read_rows(o, rows);
		if (!find_folders(o, true, false, have_rows ? &rows : nullptr))
			return kFailed;
		if (!o.ignore_game)
			if (const std::string g = game_running(o.ashita); !g.empty())
			{
				std::printf("Not now: %s.\n", g.c_str());
				return kGameStarted;
			}
		std::shared_ptr<const pk::index> idx = open_pack(o, why);
		if (!idx)
		{
			std::printf("The pack can't be used: %s\n", why.c_str());
			return kFailed;
		}
		pk::writer w;
		pk::writer_options wo;
		wo.folder = with_slash(o.out);
		wo.maker_version = idx->head.maker_version;
		wo.source = std::string(idx->head.source, strnlen(idx->head.source, sizeof(idx->head.source)));
		wo.client = pk::client_fingerprint(o.ffxi);
		idx.reset();
		// (the writer holds pack.lock from here, and reads the pack after taking it: what's pruned is what no convert
		// changes meanwhile)
		if (!w.open(wo, why))
		{
			std::printf("%s\n", why.c_str());
			return kFailed;
		}
		g_log = open_log((with_slash(o.out) + L"texpack.log").c_str());
		say("---- texpack prune: %s against %s", narrow(o.out).c_str(), narrow(o.ffxi).c_str());
		// (the keys of every texture chunk pack.src's DATs have now: an entry none of them has is dropped. A key is a
		// texture's whole original body, so this is exact whichever DAT first gave it)
		std::vector<std::string> sources;
		for (const auto &[rel, s] : w.sources_done())
			sources.push_back(rel);
		std::sort(sources.begin(), sources.end());
		const source_scan scan = scan_sources(o, sources);
		std::set<pk::key> now;
		std::vector<std::pair<std::string, const char *>> forget; // (and why)
		uint32_t gone = 0, changed = 0, unreadable = 0;
		std::string first_unreadable;
		for (size_t i = 0; i < sources.size(); ++i)
		{
			if (scan.state[i] == source_scan::unreadable)
			{
				if (unreadable++ == 0)
					first_unreadable = sources[i] + " (" + scan.why[i] + ")";
				continue;
			}
			if (scan.state[i] == source_scan::gone)
			{
				gone++;
				forget.emplace_back(sources[i], "gone");
				continue;
			}
			now.insert(scan.keys[i].begin(), scan.keys[i].end());
			// (a DAT that changed has its line taken out, so convert does it again: kept, its line would say the DAT is
			// done while the entries its old textures gave are dropped)
			const pk::source_info &s = w.sources_done().at(sources[i]);
			uint64_t size = 0, mtime = 0;
			if (!file_info(o.ffxi + L"\\" + wide(sources[i]), size, mtime) || size != s.size || mtime != s.mtime)
			{
				changed++;
				forget.emplace_back(sources[i], "changed");
			}
		}
		if (unreadable > 0)
		{
			say("Stopped: %u DATs of pack.src can't be read (the first: %s): nothing dropped (their entries would be, though their textures may be "
				"there).",
				unreadable, first_unreadable.c_str());
			return kFailed;
		}
		if (gone > 5 && uint64_t(gone) * 100 > sources.size())
		{
			say("Stopped: %u of the %zu DATs of pack.src aren't in %s: is it the game's folder (--ffxi)? Nothing dropped.", gone, sources.size(),
				narrow(o.ffxi).c_str());
			return kFailed;
		}
		std::vector<pk::key> dropped;
		for (const pk::entry &e : w.entries())
			if (now.count(e.the_key()) == 0)
				dropped.push_back(e.the_key());
		for (const pk::key &k : dropped)
		{
			const pk::entry *e = w.find(k);
			say("dropped '%s' (file %u chunk %u): no DAT of pack.src has it now", name16(reinterpret_cast<const uint8_t *>(e->name)).c_str(), e->file_id, e->chunk);
			w.drop(k);
		}
		for (const auto &[rel, what] : forget)
		{
			say("%s: %s: its pack.src line taken out (convert does it again)", rel.c_str(), what);
			w.forget(rel);
		}
		if ((!dropped.empty() || !forget.empty()) && !w.commit(why))
		{
			say("Stopped: %s.", why.c_str());
			return kFailed;
		}
		say("pruned: %zu entries dropped, %zu kept (their bytes stay until compact); %zu DATs of pack.src read: %u gone, %u changed (convert does those "
			"again)",
			dropped.size(), w.entries().size(), sources.size(), gone, changed);
		return kDone;
	}

}

int wmain(int argc, wchar_t **argv)
{
	options o;
	if (!parse(argc, argv, o))
	{
		usage();
		return kUsage;
	}
	// (zonemesh reads a zone through XIPivot's overlays when pivot.ini lists them: the tags come from the original DATs,
	// so no pivot.ini is ever found here)
	SetEnvironmentVariableW(L"HIFIXI_PIVOT_INI", L"<none>");
	int code = kUsage;
	if (o.command == "convert")
		code = convert(o);
	else if (o.command == "verify")
		code = verify(o);
	else if (o.command == "stats")
		code = stats(o);
	else if (o.command == "compact")
		code = compact(o);
	else if (o.command == "prune")
		code = prune(o);
	else
		usage();
	if (g_log != nullptr)
		std::fclose(g_log);
	return code;
}
