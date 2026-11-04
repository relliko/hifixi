// hifixi-texpack make: the texture pack made straight from the player's game files, with no Python and no overlay
// (docs/texture-pack-plan.md M7; the steps are research\ffxi-re\tools\upscale's bakeall.py, upscale.py and
// keeptexels.py, here in C++). The header comment of texpack.cpp has the usage; this is how it goes:
//   - The game's folder as convert finds it (--ffxi; else Ashita's folder, given, above the game's or above this tool),
//     never the registry's guess. The upscaler: --upscaler, else <this tool's folder>\upscaler (get-upscaler fetches
//     it); missing: exit 1, saying so.
//   - The DATs: dats.tsv in the pack's folder (sortdats.py's columns), made by walking ROM*\ under the game's folder and
//     made again when VTABLE.DAT, FTABLE.DAT or pivot.ini change, a listed DAT's size isn't the file's, or --rescan.
//     zone: the game's zone DATs (file id 100 + zone 0..299 through VTABLE/FTABLE); model: a DAT with an Os2 mesh
//     (chunk 0x2A); other: left as it is, never made. A DAT an XIPivot overlay other than hifi-textures has (pivot.ini
//     found) is skipped: the game loads that copy. --kind zone makes a zones-only pack (the quick one, for weaker
//     graphics cards).
//   - Zones first, then models, each in path order; zones at --zone-factor (2), models at --model-factor (4), as the
//     overlay was baked. Only A1 (DXT) textures whose sides are multiples of 4, as bakeall: palette (B1) textures
//     are left (their alpha means something else to the game), so make writes no no_pack entry.
//   - In batches of whole DATs up to --batch-mtexels (24) million original texels of textures the pack hasn't got:
//     keys already in the pack, or earlier in the batch, are skipped BEFORE upscaling (a texture several DATs share is
//     made once; the first DAT's file id and chunk are the entry's). A key whose entry this DAT's chunk made (the DAT
//     done again: --redo, say with another factor, or a convert pack's line) is made again and replaces it. Each texture: decoded (chars tex), wrapped by 8,
//     written as a PNG; the upscaler run on the batch's folder (upscale.hpp: a child in a job object, below-normal
//     priority); cropped, Lanczos'd for 2x and for the alpha; keeptexels (8 rounds, EDGE 24, GRAIN 2); BC2; the soul
//     test against the original (convert's limits: over them, left out); coverage mips by alpha kind (tags from the
//     zone meshes and Os2, as convert); appended. Then the batch's DATs go into pack.src at a commit.
//   - Resume from pack.src (a DAT whose size, time and maker version match is skipped; --redo does them again), a stop
//     file in the pack's folder (also ends the upscaler), progress.txt ("key: value" lines, with a plain summary),
//     texpack.log, pack.lock, below-normal priority, and a free-space check (20 GB plus its estimate). The batch's
//     pictures go in <pack>\work, marked as make's: a work folder without the mark is never deleted (make refuses).
//   - The game: checked before each batch and every --game-poll-s (3) seconds while the upscaler runs. When it starts,
//     the upscaler is ended, its batch thrown away, and make waits (progress.txt: paused) until the game has been
//     closed for --game-quiet-s (60) seconds, then does that batch again. --no-wait: stop with exit 4 instead.
//   - --heights (M9; off by default until M9 is approved): each texture also gets its height record (heights.hpp, at
//     --height-scale), made from its BC2 colour decoded, zones' from the bump DAT's map of their name where there is one
//     (height_from_game): the same bytes make and then heights would write.

#include "shared.hpp"
#include "keys.hpp"
#include "image_io.hpp"
#include "keeptexels.hpp"
#include "upscale.hpp"
#include "heights.hpp"
#include "../../src/pack/pack_write.hpp"
#include "../../src/pack/bc2.hpp"
#include "../../src/pack/hash.hpp"
#include "../../src/chars/tex.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <thread>

namespace hifi::texpack
{
	namespace
	{
		// ---- The work folder: deleted only when make's mark is in it (a folder of the player's that happens to be
		// called work is never touched)

		const wchar_t *kWorkMark = L"\\made-by-hifixi-texpack-make.txt";

		// The work folder gone (true), or false: it's there without make's mark
		bool clear_work(const std::wstring &work)
		{
			if (GetFileAttributesW(work.c_str()) == INVALID_FILE_ATTRIBUTES)
				return true;
			if (!is_folder(work) || !is_file(work + kWorkMark))
				return false;
			remove_tree(work);
			return GetFileAttributesW(work.c_str()) == INVALID_FILE_ATTRIBUTES;
		}

		// The work folder made, with make's mark in it
		bool begin_work(const std::wstring &work)
		{
			const char text[] = "hifixi-texpack make's batch folder: deleted after each batch.\r\n";
			return CreateDirectoryW(work.c_str(), nullptr) && bk::write_file(work + kWorkMark, text, std::strlen(text)) &&
				CreateDirectoryW((work + L"\\in").c_str(), nullptr) && CreateDirectoryW((work + L"\\out").c_str(), nullptr);
		}

		// ---- dats.tsv

		// The overlays XIPivot's pivot.ini (Ashita's config\pivot) lists other than hifi-textures, lowest number first, as
		// folders; ini_time its last write time (0: no pivot.ini)
		std::vector<std::wstring> other_overlays(const std::wstring &ashita, uint64_t &ini_time)
		{
			std::vector<std::wstring> out;
			ini_time = 0;
			const std::wstring ini = ashita + L"\\config\\pivot\\pivot.ini";
			uint64_t size = 0;
			if (ashita.empty() || !file_info(ini, size, ini_time))
				return out;
			const std::wstring root = pivot_root(ashita);
			std::vector<wchar_t> section(32768, L'\0');
			GetPrivateProfileSectionW(L"overlays", section.data(), DWORD(section.size()), ini.c_str());
			std::map<int, std::wstring> by_number;
			for (const wchar_t *p = section.data(); *p != 0; p += wcslen(p) + 1)
			{
				const std::wstring line = p;
				const size_t eq = line.find(L'=');
				if (eq == std::wstring::npos)
					continue;
				std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
				while (!k.empty() && k.back() == L' ')
					k.pop_back();
				while (!v.empty() && v.front() == L' ')
					v.erase(v.begin());
				while (!v.empty() && (v.back() == L' ' || v.back() == L'\r'))
					v.pop_back();
				if (k.empty() || k.find_first_not_of(L"0123456789") != std::wstring::npos || _wcsicmp(v.c_str(), L"hifi-textures") == 0 || v.empty())
					continue;
				by_number[_wtoi(k.c_str())] = v;
			}
			for (const auto &[n, name] : by_number)
				out.push_back(root + L"\\" + name);
			return out;
		}

		// The line dats.tsv starts with: what it was made from (no tabs, so read_tsv passes over it)
		std::string list_fingerprint(const std::wstring &ffxi, uint64_t ini_time)
		{
			const pk::fingerprint f = pk::client_fingerprint(ffxi);
			char text[512];
			std::snprintf(text, sizeof(text), "# hifixi-texpack make's list of %s: VTABLE.DAT %llu %llu, FTABLE.DAT %llu %llu, pivot.ini %llu", narrow(ffxi).c_str(),
				static_cast<unsigned long long>(f.vtable_size), static_cast<unsigned long long>(f.vtable_mtime), static_cast<unsigned long long>(f.ftable_size),
				static_cast<unsigned long long>(f.ftable_mtime), static_cast<unsigned long long>(ini_time));
			return text;
		}

		void dat_files(const std::wstring &ffxi, const std::wstring &rel, std::vector<std::wstring> &out)
		{
			WIN32_FIND_DATAW fd;
			HANDLE f = FindFirstFileW((ffxi + L"\\" + rel + L"\\*").c_str(), &fd);
			if (f == INVALID_HANDLE_VALUE)
				return;
			do
			{
				const std::wstring name = fd.cFileName;
				if (name == L"." || name == L"..")
					continue;
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					dat_files(ffxi, rel + L"\\" + name, out);
				else if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".DAT") == 0)
				{
					std::wstring upper = name;
					for (wchar_t &c : upper)
						c = wchar_t(towupper(c));
					if (upper.find(L"TABLE") == std::wstring::npos)
						out.push_back(rel + L"\\" + name);
				}
			} while (FindNextFileW(f, &fd));
			FindClose(f);
		}

		// dats.tsv made as sortdats.py makes it: every DAT under ROM*\ with textures (a chunk 0x20 of at least 69 bytes
		// whose sides are 1..4096 and multiples of 4), its kind, texture count, texels, bytes and the overlay that has it
		bool scan_dats(const options &o, const id_table &ids, const std::vector<std::wstring> &overlays, const std::string &first_line, std::string &text,
			uint32_t &dats_seen)
		{
			std::vector<std::wstring> files;
			WIN32_FIND_DATAW fd;
			HANDLE f = FindFirstFileW((o.ffxi + L"\\ROM*").c_str(), &fd);
			if (f == INVALID_HANDLE_VALUE)
				return false;
			do
				if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
					dat_files(o.ffxi, fd.cFileName, files);
			while (FindNextFileW(f, &fd));
			FindClose(f);
			std::sort(files.begin(), files.end());
			dats_seen = uint32_t(files.size());
			std::set<std::string> zones;
			for (uint32_t z = 0; z < 300; ++z)
				if (100 + z < ids.rel_of.size() && !ids.rel_of[100 + z].empty())
					zones.insert(ids.rel_of[100 + z]);
			std::vector<std::string> rows(files.size());
			std::atomic<size_t> next { 0 };
			std::vector<std::thread> pool;
			for (uint32_t t = 0; t < o.threads; ++t)
				pool.emplace_back([&] {
					std::vector<uint8_t> dat;
					for (size_t i; (i = next++) < files.size();)
					{
						if (!bk::read_file(o.ffxi + L"\\" + files[i], dat))
							continue;
						size_t off = 0;
						uint32_t n = 0;
						uint64_t texels = 0;
						bool meshes = false;
						while (off + 16 <= dat.size())
						{
							const uint32_t w = u32_at(dat.data() + off + 4), size = ((w >> 7) & 0x7FFFF) * 16, kind = w & 0x7F;
							if (size < 16 || off + size > dat.size())
								break;
							if (kind == 0x2A)
								meshes = true;
							else if (kind == 0x20 && size >= 16 + 69)
							{
								const int32_t tw = i32_at(dat.data() + off + 16 + 21), th = std::abs(i32_at(dat.data() + off + 16 + 25));
								if (tw > 0 && tw <= 4096 && th > 0 && th <= 4096 && tw % 4 == 0 && th % 4 == 0)
									n++, texels += uint64_t(tw) * uint64_t(th);
							}
							off += size;
						}
						if (n == 0)
							continue;
						const std::string rel = narrow(files[i]);
						std::string shadow = "-";
						for (const std::wstring &ov : overlays)
							if (is_file(ov + L"\\" + files[i]))
							{
								shadow = narrow(ov.substr(ov.find_last_of(L'\\') + 1));
								break;
							}
						char line[512];
						std::snprintf(line, sizeof(line), "%s\t%s\t%u\t%llu\t%zu\t%s\n", rel.c_str(), zones.count(rel) ? "zone" : meshes ? "model" : "other", n,
							static_cast<unsigned long long>(texels), dat.size(), shadow.c_str());
						rows[i] = line;
					}
				});
			for (std::thread &t : pool)
				t.join();
			text = first_line + "\n";
			for (const std::string &r : rows)
				text += r;
			return true;
		}

		// ---- One batch

		struct dat_in
		{
			std::string rel;
			uint8_t kind = pk::kSourceUnknown;
			uint32_t id = 0xFFFFFFFFu;
			uint64_t size = 0, mtime = 0, texels = 0;
		};

		// One texture to make: where it is, its original, and (once made) its map or why it isn't
		struct job
		{
			size_t dat = 0; // (the batch's DAT)
			uint32_t chunk = 0;
			std::vector<uint8_t> body; // the original chunk
			uint32_t w = 0, h = 0, factor = 0;
			std::vector<uint8_t> rgba; // decoded
			pk::entry e;
			float threshold = 0;
			// (after the upscaler)
			pk::map_in map;
			bool made = false, no_picture = false, soul_failed = false;
			bool own = false; // (the pack's entry for the key is this DAT chunk's: replaced)
			// (--heights: the zone's bump map of the texture's name, if any; the height record once made)
			bool has_game = false;
			hgt::game_map game;
			hgt::made height;
			std::string why;
			soul_result s;
		};

		// (--heights) A key in the pack from before whose height was made from the colour, met again in a zone whose bump
		// DAT has the game's map for it: its height made again from that map after the batch's adds
		struct to_game_later
		{
			pk::key k;
			std::string rel;
			uint32_t chunk = 0;
			std::vector<uint8_t> body; // the original chunk (the colour when the pack's can't be read)
			hgt::game_map game;
		};

		struct batch_dat
		{
			const dat_in *in = nullptr;
			bool refused = false;
			uint32_t textures = 0, deduped = 0, skipped = 0; // (A1 textures; already in the pack or the batch; not made: not DXT, sides)
		};

		struct counts
		{
			uint32_t dats = 0, refused = 0, made = 0, deduped = 0, skipped = 0, soul_failed = 0, no_picture = 0, unused = 0, untagged = 0, walk_failed = 0;
			uint32_t same = 0, differs = 0, add_refused = 0, replaced = 0, batches = 0, pauses = 0;
			uint32_t heights = 0, heights_game = 0, no_height = 0; // (--heights)
			uint32_t heights_to_game = 0; // (--heights: shared with a zone before it that has no game map, made from the game's)
			uint32_t by_kind[5] = {};
			uint64_t texels_in = 0, texels_out = 0;
			double upscaler_s = 0, cpu_s = 0, waited_s = 0;
		};

		std::string duration(double s)
		{
			char text[64];
			if (s < 90)
				std::snprintf(text, sizeof(text), "%.0f s", s);
			else if (s < 5400)
				std::snprintf(text, sizeof(text), "%.0f min", s / 60);
			else
				std::snprintf(text, sizeof(text), "%.0f h %.0f min", std::floor(s / 3600), std::fmod(s, 3600) / 60);
			return text;
		}

		// Work over n items on o.threads threads (each with COM for WIC)
		template <class F> void parallel(uint32_t threads, size_t n, F fn)
		{
			std::atomic<size_t> next { 0 };
			std::vector<std::thread> pool;
			for (uint32_t t = 0; t < std::max(1u, threads); ++t)
				pool.emplace_back([&] {
					image::com_scope com;
					for (size_t i; (i = next++) < n;)
						fn(i);
				});
			for (std::thread &t : pool)
				t.join();
		}
	}

	int make(options &o)
	{
		if (o.out.empty())
		{
			say("make needs --out <the pack's folder> (the game's: <bootloader>\\hifixi-pack).");
			return kUsage;
		}
		if (!o.kind.empty() && o.kind != "zone" && o.kind != "model")
		{
			say("--kind is zone or model.");
			return kUsage;
		}
		pk::height::scale height_scale;
		if (!pk::height::parse(o.height_scale, height_scale))
		{
			say("--height-scale is half, full or game.");
			return kUsage;
		}
		// (Ashita's folder: given, above the game's folder (find_folders), or above this tool's)
		if (o.ashita.empty() && o.ffxi.empty())
			for (std::wstring f = exe_folder(); !f.empty(); f = up(f))
				if (ashita_like(f))
				{
					o.ashita = f;
					break;
				}
		if (!find_folders(o, true, false, nullptr))
			return kFailed;
		o.upscaler = no_slash(o.upscaler.empty() ? exe_folder() + L"\\upscaler" : o.upscaler);
		if (const std::string missing = upscale::missing_files(o.upscaler); !missing.empty())
		{
			say("The upscaler isn't in %s (%s missing): run \"hifixi-texpack get-upscaler\" first (or give its folder with --upscaler).", narrow(o.upscaler).c_str(),
				missing.c_str());
			return kFailed;
		}
		std::string why;
		if (!load_key_tables(o.ffximain.empty() ? o.ffxi + L"\\FFXiMain.dll" : o.ffximain, why))
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
		const std::wstring out = with_slash(o.out), work = out + L"work";

		// The pack's folder: resumed, or begun (pack.lock held from here)
		pk::writer w;
		pk::writer_options wo;
		wo.folder = out;
		wo.maker_version = kMakeVersion;
		wo.source = "texpack make " + std::to_string(kMakeVersion);
		wo.client = pk::client_fingerprint(o.ffxi);
		if (!w.open(wo, why))
		{
			say("The pack's folder can't be used: %s.", why.c_str());
			return kFailed;
		}
		g_log = open_log((out + L"texpack.log").c_str());
		say("---- texpack make, maker version %u: %s -> %s (upscaler %s; zones %ux, models %ux)", kMakeVersion, narrow(o.ffxi).c_str(), narrow(out).c_str(),
			narrow(o.upscaler).c_str(), o.zone_factor, o.model_factor);

		const auto start = std::chrono::steady_clock::now();
		counts n;
		std::string phase = "starting", paused, last;
		uint32_t done_before = 0;
		size_t total = 0, done_dats = 0;
		uint64_t texels_todo = 0, texels_done = 0; // (dats.tsv's texels of the DATs to do, and of those done this run)
		auto progress = [&](const char *state) {
			const double busy = seconds_since(start) - n.waited_s;
			const double eta = texels_done > 0 && texels_todo > texels_done ? busy / double(texels_done) * double(texels_todo - texels_done) : -1;
			char text[2048];
			std::snprintf(text, sizeof(text),
				"texpack make (maker version %u): %s\r\n"
				"summary: %zu of %zu DATs done%s%s\r\n"
				"state: %s\r\n"
				"phase: %s\r\n"
				"paused: %s\r\n"
				"dats_done: %zu\r\n"
				"dats_total: %zu\r\n"
				"dats_done_before: %u\r\n"
				"eta_s: %.0f\r\n"
				"textures_made: %u\r\n"
				"textures_in_pack_already: %u\r\n"
				"textures_left_out: %u\r\n"
				"entries: %zu\r\n"
				"written_gb: %.3f\r\n"
				"elapsed_s: %.0f\r\n"
				"waited_s: %.0f\r\n"
				"last: %s\r\n",
				kMakeVersion, state, done_dats + done_before, total, eta >= 0 ? ("; about " + duration(eta) + " to go").c_str() : "",
				paused.empty() ? "" : ("; paused: " + paused).c_str(), state, phase.c_str(), paused.empty() ? "-" : paused.c_str(), done_dats + done_before, total,
				done_before, eta, n.made, n.deduped, n.soul_failed, w.entries().size(), gb(w.stats().bytes), seconds_since(start), n.waited_s, last.c_str());
			pk::write_progress(out, text);
		};
		if (!clear_work(work))
		{
			say("%s is there and isn't make's (no %ls in it): move it, or give --out another folder.", narrow(work).c_str(), kWorkMark + 1);
			progress("failed");
			w.close();
			return kFailed;
		}

		// The game: before anything else, and before each batch. Waits (or, --no-wait, says to stop) while it runs; kDone
		// to go on, kGameStarted or kStopped
		auto game_gate = [&](std::string reason) -> int {
			if (o.ignore_game)
				return kDone;
			if (reason.empty())
				reason = game_running(o.ashita, &o.game_names);
			if (reason.empty())
				return kDone;
			if (o.no_wait)
			{
				say("Stopped: %s. Run make again with the game closed: it goes on where it stopped.", reason.c_str());
				return kGameStarted;
			}
			n.pauses++;
			say("Paused: %s. Making goes on once the game has been closed for %.0f s.", reason.c_str(), o.game_quiet_s);
			paused = "game running (" + reason + ")";
			progress("paused");
			const auto began = std::chrono::steady_clock::now();
			auto quiet = std::chrono::steady_clock::now();
			bool closed = false;
			for (;;)
			{
				Sleep(DWORD(o.game_poll_s * 1000));
				if (pk::stop_requested(out))
				{
					n.waited_s += seconds_since(began);
					paused.clear();
					return kStopped;
				}
				const std::string g = game_running(o.ashita, &o.game_names);
				if (!g.empty())
				{
					closed = false;
					continue;
				}
				if (!closed)
					closed = true, quiet = std::chrono::steady_clock::now();
				else if (seconds_since(quiet) >= o.game_quiet_s)
					break;
			}
			n.waited_s += seconds_since(began);
			paused.clear();
			say("The game has been closed for %.0f s: going on.", o.game_quiet_s);
			return kDone;
		};
		int code = game_gate("");
		if (code != kDone)
		{
			progress(code == kStopped ? "stopped (the stop file)" : "stopped (the game)");
			w.close();
			return code;
		}

		// dats.tsv: the pack folder's, or made now
		uint64_t ini_time = 0;
		const std::vector<std::wstring> overlays = other_overlays(o.ashita, ini_time);
		const std::string fingerprint = list_fingerprint(o.ffxi, ini_time);
		tsv rows;
		auto make_list = [&](const char *because) {
			phase = "listing the game's DATs";
			progress("running");
			const auto began = std::chrono::steady_clock::now();
			std::string text;
			uint32_t seen = 0;
			rows.clear();
			if (!scan_dats(o, ids, overlays, fingerprint, text, seen) || !bk::write_file(out + L"dats.tsv", text.data(), text.size()) || !read_tsv(no_slash(out), rows))
				return false;
			say("dats.tsv made (%s): %u DATs read, %zu with textures, in %.1f s%s", because, seen, rows.size(), seconds_since(began),
				overlays.empty() ? "" : "; DATs another XIPivot overlay has are skipped");
			return true;
		};
		{
			bool ok = false;
			const std::vector<std::string> head = lines_of(out + L"dats.tsv", ok);
			const bool fresh = ok && !head.empty() && head[0] == fingerprint && !o.rescan && read_tsv(no_slash(out), rows);
			if (!fresh && !make_list(o.rescan ? "--rescan" : ok ? "the game's files changed" : "none yet"))
			{
				say("The game's DATs can't be listed (%s\\ROM*).", narrow(o.ffxi).c_str());
				progress("failed");
				w.close();
				return kFailed;
			}
		}

		// The inputs: zone and model DATs no other overlay has (--kind, --only), zones first
		std::vector<dat_in> inputs;
		for (int attempt = 0; attempt < 2; ++attempt)
		{
			inputs.clear();
			bool stale = false;
			std::vector<std::string> wanted;
			if (o.only.empty())
				for (const auto &[rel, r] : rows)
					wanted.push_back(rel);
			else
				wanted = o.only;
			for (const std::string &rel : wanted)
			{
				auto row = rows.find(rel);
				const char *bad = nullptr;
				dat_in in;
				in.rel = rel;
				if (row == rows.end())
					bad = "not a DAT with textures under ROM*\\ (dats.tsv)";
				else if (row->second.kind != "zone" && row->second.kind != "model")
					bad = o.only.empty() ? "" : "neither zone nor model (effects, sky, menus: left as they are)";
				else if (!o.kind.empty() && row->second.kind != o.kind)
					bad = "";
				else if (row->second.shadow != "-")
					bad = o.only.empty() ? "" : "another XIPivot overlay has it (the game loads that copy)";
				else if (!file_info(o.ffxi + L"\\" + wide(rel), in.size, in.mtime))
					bad = "no such file";
				else if (in.size != row->second.bytes)
				{
					stale = true;
					break;
				}
				if (bad != nullptr)
				{
					if (*bad != 0)
						say("%s: %s: skipped", rel.c_str(), bad);
					continue;
				}
				in.kind = row->second.kind == "zone" ? pk::kSourceZone : pk::kSourceModel;
				in.id = ids.id(rel);
				in.texels = row->second.texels;
				inputs.push_back(in);
			}
			if (!stale)
				break;
			if (attempt == 1 || !make_list("a DAT's size changed"))
			{
				say("The game's DATs don't match their list even after listing them again.");
				progress("failed");
				w.close();
				return kFailed;
			}
		}
		std::sort(inputs.begin(), inputs.end(), [](const dat_in &x, const dat_in &y) { return x.kind != y.kind ? x.kind < y.kind : x.rel < y.rel; });
		std::vector<const dat_in *> todo;
		for (const dat_in &in : inputs)
		{
			if (!o.redo && w.done(in.rel, in.size, in.mtime))
			{
				done_before++;
				continue;
			}
			if (o.limit != 0 && todo.size() >= o.limit)
				continue;
			todo.push_back(&in);
			texels_todo += in.texels;
		}
		total = done_before + todo.size();
		uint32_t zones_todo = 0;
		for (const dat_in *in : todo)
			zones_todo += in->kind == pk::kSourceZone;
		say("%zu DATs (%u zones, %zu models) to make, %u done before (pack.src); the pack has %u entries", todo.size(), zones_todo, todo.size() - zones_todo,
			done_before, w.stats().kept);

		// Free space: 20 GB (--reserve-gb) plus the estimate: the DATs' texels at their factor, one byte a texel (BC2)
		// with mips, at most (keys several DATs share are made once), and the batch's pictures
		uint64_t estimate = uint64_t(2) << 30;
		for (const dat_in *in : todo)
		{
			const uint64_t f = in->kind == pk::kSourceZone ? o.zone_factor : o.model_factor;
			// (--heights: BC4 at the colour's size at most, half a byte a texel)
			estimate += uint64_t(double(in->texels * f * f) * 4.0 / 3.0 * 1.05 * (o.heights ? 1.5 : 1.0));
		}
		ULARGE_INTEGER free_bytes = {};
		std::wstring probe = no_slash(out);
		while (!probe.empty() && !is_folder(probe))
			probe = up(probe);
		GetDiskFreeSpaceExW(probe.c_str(), &free_bytes, nullptr, nullptr);
		say("estimate: at most %.2f GB more (less where DATs share textures); %.1f GB free", gb(estimate), gb(free_bytes.QuadPart));
		if (double(free_bytes.QuadPart) < o.reserve_gb * double(1ull << 30) + double(estimate))
		{
			say("Refused: the disk needs %.0f GB free beyond the estimate.", o.reserve_gb);
			phase = "the disk is too full";
			progress("failed");
			w.close();
			return kFailed;
		}
		SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

		// The tags, as convert's: over every input of the kind (zones from their meshes, models from their Os2)
		tag_map zone_tags, model_tags;
		std::set<std::string> walk_failed;
		for (const uint8_t kind : { pk::kSourceZone, pk::kSourceModel })
		{
			if (std::none_of(todo.begin(), todo.end(), [&](const dat_in *in) { return in->kind == kind; }))
				continue;
			phase = kind == pk::kSourceZone ? "reading the zones' meshes" : "reading the models' meshes";
			progress("running");
			const auto began = std::chrono::steady_clock::now();
			std::vector<const dat_in *> all;
			for (const dat_in &in : inputs)
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

		// The batches
		const uint64_t budget = uint64_t(o.batch_mtexels * 1e6);
		size_t next = 0;
		auto last_progress = std::chrono::steady_clock::now();
		while (next < todo.size())
		{
			if (pk::stop_requested(out))
			{
				say("Stopped by %sstop (delete it to go on).", narrow(out).c_str());
				code = kStopped;
				break;
			}
			if ((code = game_gate("")) != kDone)
				break;

			// Whole DATs until the budget: each one's A1 textures the pack (and the batch) hasn't got
			const size_t first = next;
			std::vector<batch_dat> dats;
			std::vector<job> jobs;
			std::set<pk::key> queued;
			std::map<pk::key, size_t> job_of; // (--heights: a key's job in the batch)
			std::vector<to_game_later> upgrades;
			std::set<pk::key> upgrading;
			uint64_t texels = 0, tsv_texels = 0;
			phase = "reading DATs";
			while (next < todo.size() && (dats.empty() || texels < budget))
			{
				const dat_in &in = *todo[next++];
				batch_dat d;
				d.in = &in;
				tsv_texels += in.texels;
				std::vector<uint8_t> dat;
				if (!bk::read_file(o.ffxi + L"\\" + wide(in.rel), dat))
				{
					say("%s: refused: it can't be read", in.rel.c_str());
					d.refused = true;
					dats.push_back(d);
					continue;
				}
				const std::vector<bk::chunk> cs = bk::chunks(dat);
				const uint32_t factor = in.kind == pk::kSourceZone ? o.zone_factor : o.model_factor;
				const bool failed_walk = in.kind == pk::kSourceZone && walk_failed.count(in.rel) != 0;
				// (--heights: the zone's bump DAT's height maps, by name)
				std::unordered_map<std::string, hgt::game_map> games;
				if (o.heights && in.kind == pk::kSourceZone)
				{
					const uint32_t bump = hgt::bump_id_of(in.id);
					std::vector<uint8_t> bump_bytes;
					if (bump < ids.rel_of.size() && !ids.rel_of[bump].empty() && bk::read_file(o.ffxi + L"\\" + wide(ids.rel_of[bump]), bump_bytes))
						games = hgt::game_maps(bump_bytes);
				}
				for (size_t i = 0; i < cs.size(); ++i)
				{
					const bk::chunk &c = cs[i];
					if (c.type != 0x20 || c.size < 69 || c.body[0] != 0xA1)
						continue;
					d.textures++;
					tex::texture t;
					std::string why2;
					if (!tex::decode(c.body, c.size, t, why2) || t.levels.empty() || t.w % 4 != 0 || t.h % 4 != 0 || t.w * factor > pk::kMaxSide ||
						t.h * factor > pk::kMaxSide)
					{
						d.skipped++;
						continue;
					}
					// (a key the pack has is skipped, unless this DAT's chunk made it: the DAT is done again (--redo, a
					// factor given with it, or a convert pack's line), so it's made again and replaces it, as convert's own entry.
					// A few model DATs have no file id: theirs can't be told apart and stay)
					const pk::key k = pk::key_of(c.body, c.size);
					const pk::entry *had = w.find(k);
					const bool own = had != nullptr && in.id != 0xFFFFFFFFu && had->file_id == in.id && had->chunk == uint32_t(i) &&
						std::memcmp(had->name, c.body + 1, 16) == 0;
					const auto game_here = games.find(hgt::bump_name(c.body + 1));
					if ((had != nullptr && !own) || !queued.insert(k).second)
					{
						d.deduped++;
						// (--heights, game heights first: a texture two zones share (one key) is made in the first; when only
						// this one's bump DAT has the game's map for it, its height is the game's all the same. Still in the
						// batch: the job takes the map; in the pack from before, made from the colour: made again from the
						// map after the batch's adds)
						if (game_here != games.end())
						{
							if (const auto q = job_of.find(k); q != job_of.end())
							{
								if (!jobs[q->second].has_game)
								{
									jobs[q->second].has_game = true, jobs[q->second].game = game_here->second;
									n.heights_to_game++;
								}
							}
							else if (had != nullptr && had->map(pk::kHeight) != nullptr && (had->flags & pk::kHeightFromGame) == 0 && upgrading.insert(k).second)
							{
								to_game_later u;
								u.k = k;
								u.rel = in.rel;
								u.chunk = uint32_t(i);
								u.body.assign(c.body, c.body + c.size);
								u.game = game_here->second;
								upgrades.push_back(std::move(u));
							}
						}
						continue;
					}
					job j;
					j.own = own;
					if (game_here != games.end())
						j.has_game = true, j.game = game_here->second;
					job_of[k] = jobs.size();
					j.dat = dats.size();
					j.chunk = uint32_t(i);
					j.body.assign(c.body, c.body + c.size);
					j.w = t.w, j.h = t.h, j.factor = factor;
					j.rgba = std::move(t.levels[0].rgba);
					pk::entry &e = j.e;
					e.hash = k.hash, e.bytes = k.bytes, e.w = k.w, e.h = k.h;
					e.orig_format = c.body[0];
					e.orig_fourcc = c.size >= 61 ? u32_at(c.body + 57) : 0;
					e.source_kind = in.kind;
					e.file_id = in.id;
					e.chunk = uint32_t(i);
					std::memcpy(e.name, c.body + 1, 16);
					uint8_t lo = 255, hi = 0;
					for (size_t q = 3; q < j.rgba.size(); q += 4)
						lo = std::min(lo, j.rgba[q]), hi = std::max(hi, j.rgba[q]);
					e.alpha_lo = lo, e.alpha_hi = hi;
					j.threshold = pk::bc2::kCharsCutThreshold;
					if (in.kind == pk::kSourceZone)
					{
						if (failed_walk)
						{
							e.alpha_kind = pk::kAlphaBlended;
							n.walk_failed++;
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
							n.unused++;
						}
						j.threshold = pk::bc2::zone_cut_threshold(e.vcol_a);
					}
					else if (auto it = model_tags.find(k); it != model_tags.end())
						e.alpha_kind = alpha_kind_of(it->second);
					else
					{
						e.alpha_kind = pk::kAlphaNone;
						n.untagged++;
					}
					texels += uint64_t(t.w) * t.h;
					jobs.push_back(std::move(j));
				}
				dats.push_back(d);
			}
			n.batches++;
			char batch_name[64];
			std::snprintf(batch_name, sizeof(batch_name), "batch %u", n.batches);

			// The upscaler on the batch's pictures, then each one finished on the CPU
			bool redo = false;
			if (!jobs.empty())
			{
				const std::wstring in_dir = work + L"\\in", out_dir = work + L"\\out";
				if (!clear_work(work) || !begin_work(work))
				{
					say("Stopped: the batch's folder %s can't be made afresh.", narrow(work).c_str());
					code = kFailed;
					break;
				}
				phase = std::string(batch_name) + ": writing " + std::to_string(jobs.size()) + " pictures";
				progress("running");
				std::atomic<uint32_t> write_fails { 0 };
				std::string write_why;
				parallel(o.threads, jobs.size(), [&](size_t i) {
					const std::vector<uint8_t> padded = upscale::wrap_pad_rgb(jobs[i].rgba.data(), jobs[i].w, jobs[i].h);
					std::string w2;
					if (!image::write_png(in_dir + L"\\" + std::to_wstring(i) + L".png", padded.data(), jobs[i].w + 2 * upscale::kPad, jobs[i].h + 2 * upscale::kPad, 3, w2))
						if (write_fails++ == 0)
							write_why = w2;
				});
				if (write_fails > 0)
				{
					say("Stopped: the batch's pictures can't be written to %s (%s).", narrow(in_dir).c_str(), write_why.c_str());
					code = kFailed;
					break;
				}
				phase = std::string(batch_name) + ": upscaling " + std::to_string(jobs.size()) + " textures (" + std::to_string(texels / 1000) + " k texels)";
				progress("running");
				upscale::run_options ro;
				ro.folder = o.upscaler, ro.in = in_dir, ro.out = out_dir, ro.log = work + L"\\upscaler.txt";
				ro.poll_s = o.game_poll_s, ro.stall_s = o.stall_s;
				bool stopped = false;
				std::string game;
				ro.should_end = [&]() -> std::string {
					if (pk::stop_requested(out))
					{
						stopped = true;
						return "the stop file";
					}
					if (!o.ignore_game)
						if (std::string g = game_running(o.ashita, &o.game_names); !g.empty())
						{
							game = g;
							return "the game started (" + g + ")";
						}
					return {};
				};
				const auto began = std::chrono::steady_clock::now();
				std::string why2;
				const upscale::end ended = upscale::run(ro, why2);
				const double took = seconds_since(began);
				if (ended == upscale::end::ended)
				{
					say("%s: the upscaler ended after %.0f s (%s): the batch is thrown away.", batch_name, took, why2.c_str());
					if (stopped)
					{
						say("Stopped by %sstop (delete it to go on).", narrow(out).c_str());
						code = kStopped;
						break;
					}
					if ((code = game_gate(game)) != kDone)
						break;
					redo = true;
				}
				else if (ended != upscale::end::done)
				{
					bool ok = false;
					std::string tail;
					for (const std::string &l : lines_of(ro.log, ok))
						if (!l.empty())
							tail = l;
					say("Stopped: the upscaler %s on %s (%s; its last line: %s). Is the graphics card's driver up to date (it needs Vulkan)?",
						ended == upscale::end::stalled ? "stalled" : "failed", batch_name, why2.c_str(), tail.empty() ? "none" : tail.c_str());
					code = kFailed;
					break;
				}
				else
				{
					n.upscaler_s += took;
					phase = std::string(batch_name) + ": keeping the texels, BC2, mips";
					progress("running");
					const auto cpu_began = std::chrono::steady_clock::now();
					parallel(o.threads, jobs.size(), [&](size_t i) {
						job &j = jobs[i];
						image::picture p;
						std::string w3;
						std::vector<uint8_t> up;
						if (!image::read(out_dir + L"\\" + std::to_wstring(i) + L".png", p, w3) || !upscale::finish(j.rgba.data(), j.w, j.h, p.px.data(), p.w, p.h, j.factor, up, w3))
						{
							j.no_picture = true;
							j.why = w3;
							return;
						}
						const uint32_t W = j.w * j.factor, H = j.h * j.factor;
						keeptexels::apply(j.rgba.data(), j.w, j.h, up.data(), W, H);
						const std::vector<uint8_t> blocks = pk::bc2::encode_bc2(up.data(), W, H);
						j.s = soul(j.body.data(), j.body.size(), blocks.data(), W, H);
						if (!j.s.done || over_limits(o, j.s))
						{
							j.soul_failed = true;
							return;
						}
						j.map.kind = pk::kColour;
						j.map.format = pk::kBC2;
						j.map.w = W, j.map.h = H;
						j.map.levels = pk::bc2::full_chain(blocks.data(), W, H, j.e.alpha_kind, j.threshold);
						j.made = !j.map.levels.empty();
						if (!j.made)
							j.why = "no mips";
						// (the height from the colour as stored, BC2 decoded: make --heights writes what make then heights would)
						if (j.made && o.heights)
						{
							const std::vector<uint8_t> stored = pk::bc2::decode_bc2(blocks.data(), W, H);
							j.height = hgt::make(stored.data(), W, H, j.w, j.h, j.e.alpha_kind, j.threshold, j.has_game ? &j.game : nullptr, height_scale);
							if (j.height.ok && j.height.from_game)
								j.e.flags |= pk::kHeightFromGame;
						}
						j.game.bytes.clear();
						j.rgba.clear();
						j.rgba.shrink_to_fit();
					});
					n.cpu_s += seconds_since(cpu_began);
				}
			}
			if (redo)
			{
				next = first;
				n.batches--;
				continue;
			}

			// Added in order; a DAT goes into pack.src unless a texture of it got no picture from the upscaler
			bool failed = false;
			std::vector<bool> missing(dats.size(), false);
			uint32_t made = 0, left_out = 0;
			for (job &j : jobs)
			{
				const std::string &rel = dats[j.dat].in->rel;
				if (j.no_picture)
				{
					missing[j.dat] = true;
					n.no_picture++;
					say("%s: chunk %u '%s': no upscaled picture (%s): the DAT is made again next run", rel.c_str(), j.chunk,
						name16(reinterpret_cast<const uint8_t *>(j.e.name)).c_str(), j.why.c_str());
					continue;
				}
				if (j.soul_failed || !j.made)
				{
					n.soul_failed++, left_out++;
					if (j.s.done)
						say("%s: chunk %u '%s': not the original's painting (soul test: colour %.2f / p99 %.2f, alpha %.2f / p99 %.2f): left out", rel.c_str(), j.chunk,
							name16(reinterpret_cast<const uint8_t *>(j.e.name)).c_str(), j.s.c_mean, j.s.c_p99, j.s.a_mean, j.s.a_p99);
					else
						say("%s: chunk %u '%s': left out (%s)", rel.c_str(), j.chunk, name16(reinterpret_cast<const uint8_t *>(j.e.name)).c_str(),
							j.s.why.empty() ? j.why.c_str() : j.s.why.c_str());
					continue;
				}
				std::string why2;
				std::vector<pk::map_in> maps { j.map };
				if (j.height.ok)
					maps.push_back(std::move(j.height.map));
				else if (o.heights)
				{
					n.no_height++;
					say("%s: chunk %u '%s': no height (%s)", rel.c_str(), j.chunk, name16(reinterpret_cast<const uint8_t *>(j.e.name)).c_str(), j.height.why.c_str());
				}
				n.heights += j.height.ok, n.heights_game += j.height.ok && j.height.from_game;
				switch (w.add(j.e, maps, why2, j.own))
				{
				case pk::add_result::replaced:
					n.replaced++;
					[[fallthrough]];
				case pk::add_result::ok:
					n.made++, made++;
					n.by_kind[std::min<uint8_t>(j.e.alpha_kind, 4)]++;
					n.texels_in += uint64_t(j.w) * j.h;
					n.texels_out += uint64_t(j.map.w) * j.map.h;
					break;
				case pk::add_result::same:
					n.same++;
					break;
				case pk::add_result::differs:
					n.differs++;
					break;
				case pk::add_result::refused:
					n.add_refused++;
					say("%s: chunk %u '%s': not added: %s", rel.c_str(), j.chunk, name16(reinterpret_cast<const uint8_t *>(j.e.name)).c_str(), why2.c_str());
					failed = failed || why2.find("writing") != std::string::npos || why2.find("can't open") != std::string::npos;
					break;
				}
				j.map.levels.clear();
			}
			// (--heights, game heights first: the keys from before whose height a zone in this batch has the game's map for)
			if (!upgrades.empty() && !failed)
			{
				std::string why2;
				const std::shared_ptr<const pk::index> idx = pk::load(out, nullptr, why2);
				std::unique_ptr<pk::reader> rd = idx ? std::make_unique<pk::reader>(idx) : nullptr;
				for (to_game_later &u : upgrades)
				{
					const pk::entry *pe = idx ? idx->find(u.k) : nullptr;
					std::vector<uint8_t> rgba;
					uint32_t cw = 0, ch = 0;
					hgt::made m;
					if (pe != nullptr && hgt::colour_of(*rd, *pe, u.body.data(), uint32_t(u.body.size()), rgba, cw, ch, why2))
						m = hgt::make(rgba.data(), cw, ch, pe->w, pe->h, pe->alpha_kind, pk::bc2::zone_cut_threshold(pe->vcol_a), &u.game, height_scale);
					else
						m.why = pe == nullptr ? "not in the pack as loaded (" + why2 + ")" : why2;
					if (!m.ok)
					{
						say("%s: chunk %u: its height not made again from the game's map (%s)", u.rel.c_str(), u.chunk, m.why.c_str());
						continue;
					}
					const pk::add_result r = w.set_map(u.k, m.map, pk::kHeightFromGame, 0, why2);
					if (r == pk::add_result::refused)
					{
						say("%s: chunk %u: its height from the game's map not added: %s", u.rel.c_str(), u.chunk, why2.c_str());
						failed = failed || why2.find("writing") != std::string::npos || why2.find("can't open") != std::string::npos;
						continue;
					}
					n.heights_to_game++;
					say("%s: chunk %u: its bump DAT has the game's map for this texture, which a zone before it shares without one: its height made again "
						"from the game's", u.rel.c_str(), u.chunk);
				}
			}
			if (failed)
			{
				say("Stopped: the pack's data file can't be written.");
				code = kFailed;
				break;
			}
			uint32_t batch_deduped = 0;
			for (size_t d = 0; d < dats.size(); ++d)
			{
				n.dats++;
				n.deduped += dats[d].deduped, batch_deduped += dats[d].deduped;
				n.skipped += dats[d].skipped;
				if (dats[d].refused)
				{
					n.refused++;
					continue;
				}
				if (!missing[d])
					w.finished(dats[d].in->rel, dats[d].in->size, dats[d].in->mtime);
				last = dats[d].in->rel;
			}
			if (!w.commit(why))
			{
				say("Stopped: %s.", why.c_str());
				code = kFailed;
				break;
			}
			done_dats += dats.size();
			texels_done += tsv_texels;
			say("%s: %zu DATs, %zu textures to make (%.2f M texels): %u made, %u left out, %u already in the pack; %.1f s so far", batch_name, dats.size(),
				jobs.size(), double(texels) / 1e6, made, left_out, batch_deduped, seconds_since(start));
			if (!o.keep_work)
				clear_work(work);
			if (seconds_since(last_progress) > 1 || next >= todo.size())
			{
				phase = "between batches";
				progress("running");
				last_progress = std::chrono::steady_clock::now();
			}
		}
		if (!o.keep_work)
			clear_work(work);
		const double took = seconds_since(start);
		phase = code == kDone ? "done" : "stopped";
		progress(code == kDone ? "finished" : code == kStopped ? "stopped (the stop file)" : code == kGameStarted ? "stopped (the game)" : "failed");
		say("DATs %u (%u refused) in %s (%s waiting for the game to close, %u pauses); textures made %u, already in the pack %u, left out by the soul test "
			"%u, without a picture %u, not DXT or odd sides %u",
			n.dats, n.refused, duration(took).c_str(), duration(n.waited_s).c_str(), n.pauses, n.made, n.deduped, n.soul_failed, n.no_picture, n.skipped);
		say("entries: made %u (none %u, cut-out %u, blended %u, mask %u, mixed %u; %u of them replacing what the DAT made before), the same key again %u, "
			"differing %u, refused %u; %zu in the pack",
			n.made, n.by_kind[0], n.by_kind[1], n.by_kind[2], n.by_kind[3], n.by_kind[4], n.replaced, n.same, n.differs, n.add_refused, w.entries().size());
		if (o.heights)
			say("heights (%s resolution): %u made, %u of them from the zones' bump DATs, %u without one; %u from the game's map of a zone after the "
				"first that shares the texture", pk::height::name(height_scale), n.heights, n.heights_game, n.no_height, n.heights_to_game);
		say("tags: zone textures no mesh uses %u, in zones whose meshes can't be read %u (blended); model textures no Os2 group names %u (none)", n.unused,
			n.walk_failed, n.untagged);
		say("time: upscaler %.1f s, keeping texels/BC2/mips %.1f s for %.2f M original texels (%.2f M made): %.2f us per original texel; written %.3f GB",
			n.upscaler_s, n.cpu_s, double(n.texels_in) / 1e6, double(n.texels_out) / 1e6,
			n.texels_in > 0 ? (n.upscaler_s + n.cpu_s) * 1e6 / double(n.texels_in) : 0.0, gb(w.stats().bytes));
		w.close();
		return code;
	}
}
