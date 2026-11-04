// hifixi-texpack heights (plan M9): height records (BC4, kind 2) added to an existing pack's entries, and the height
// side make --heights shares. The header comment of texpack.cpp has the usage; this is how it goes:
//   - The pack must load; its header's maker version, source and client fingerprint are kept (heights doesn't make
//     colour, so pack.src's lines and what convert or make would redo stay as they were).
//   - The DATs are pack.src's (each a source an entry was made from), zones first, then the rest, each by path;
//     --kind (zone: a DAT whose entries are zone ones) and --only narrow them, --limit counts DATs.
//   - Each DAT's A1 textures with an entry (not no_pack) that has no height record yet (--redo: all of them), the first
//     DAT with a key doing it: the colour is the entry's colour record (BC2 level 0 decoded) or, with none, the original
//     decoded (chars tex); zones' textures with a height map of their name in the zone's bump DAT take that (flagged
//     height_from_game), the rest are made from the colour (height.hpp from_colour).
//   - In batches of --batch-mtexels (24) million map texels, made on --threads threads, put in with
//     writer::set_map, then one commit (pack.idx replaced; pack.src untouched). The stop file and the game are checked
//     between batches (waits as make does; --no-wait exit 4); progress.txt, texpack.log, pack.lock, below-normal
//     priority, and the free-space check (--reserve-gb plus the estimate).
//   - Resume needs nothing of its own: an entry that has a height record is done.

#include "heights.hpp"
#include "../../src/pack/bc2.hpp"
#include "../../src/pack/validate.hpp"
#include "../../src/chars/tex.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <thread>

namespace hifi::texpack::hgt
{
	std::unordered_map<std::string, game_map> game_maps(const std::vector<uint8_t> &bump)
	{
		std::unordered_map<std::string, game_map> out;
		for (const bk::chunk &c : bk::chunks(bump))
		{
			if (c.type != 0x5D || c.size < 0x20 || c.body[0] != 'b' || c.body[1] != 'p')
				continue;
			const uint32_t w = uint32_t(c.body[4] | (c.body[5] << 8)), h = uint32_t(c.body[6] | (c.body[7] << 8));
			if (w == 0 || h == 0 || 0x20 + size_t(w) * h > c.size)
				continue;
			const std::string name(reinterpret_cast<const char *>(c.body) + 0x18, 8);
			if (out.count(name) != 0)
				continue;
			game_map m;
			m.w = w, m.h = h;
			m.bytes.assign(c.body + 0x20, c.body + 0x20 + size_t(w) * h);
			out.emplace(name, std::move(m));
		}
		return out;
	}

	bool is_zone_id(uint32_t id)
	{
		return (id >= 100 && id < 400) || (id >= 0x144F7 + 600 && id < 0x144F7 + 1000);
	}

	uint32_t bump_id_of(uint32_t id)
	{
		if (id >= 100 && id < 400)
			return id - 100 + 0x9B97;
		if (id >= 0x144F7 + 600 && id < 0x144F7 + 1000)
			return id - 0x144F7 + 0x14817;
		return 0;
	}

	made make(const uint8_t *rgba, uint32_t cw, uint32_t ch, uint32_t w, uint32_t h, uint8_t alpha_kind, float cut_threshold, const game_map *game,
		pk::height::scale s)
	{
		made m;
		uint32_t mw = 0, mh = 0;
		if (!pk::height::size_for(s, cw, ch, w, h, mw, mh))
		{
			m.why = "no map size fits (sides not multiples of 4)";
			return m;
		}
		pk::height::field f;
		const bool ok = game != nullptr ? pk::height::from_game(game->bytes.data(), game->w, game->h, rgba, cw, ch, w, mw, mh, alpha_kind, cut_threshold, f, m.why)
										: pk::height::from_colour(rgba, cw, ch, w, mw, mh, alpha_kind, cut_threshold, f, m.why);
		if (!ok)
			return m;
		const pk::height::made e = pk::height::encode(f);
		if (e.levels.empty())
		{
			m.why = "no levels";
			return m;
		}
		m.map.kind = pk::kHeight;
		m.map.format = pk::kBC4;
		m.map.w = e.w, m.map.h = e.h;
		m.map.levels = e.levels;
		std::memcpy(&m.map.param, &e.span, 4);
		m.ok = true;
		m.from_game = game != nullptr;
		return m;
	}

	bool colour_of(pk::reader &rd, const pk::entry &e, const uint8_t *body, uint32_t size, std::vector<uint8_t> &rgba, uint32_t &cw, uint32_t &ch,
		std::string &why)
	{
		if (const pk::record *col = e.map(pk::kColour); col != nullptr && col->format == pk::kBC2)
		{
			std::vector<uint8_t> buf(col->bytes);
			std::vector<std::vector<uint8_t>> levels;
			if (rd.read_level(*col, 0, buf.data(), why) && untile(pk::record { col->kind, 1, col->file, col->w, col->h, col->format }, buf, levels))
			{
				rgba = pk::bc2::decode_bc2(levels[0].data(), col->w, col->h);
				cw = col->w, ch = col->h;
				return true;
			}
		}
		tex::texture t;
		if (!tex::decode(body, size, t, why) || t.levels.empty())
			return false;
		rgba = std::move(t.levels[0].rgba);
		cw = t.w, ch = t.h;
		return true;
	}
}

namespace hifi::texpack
{
	namespace
	{
		struct hjob
		{
			pk::key k;
			std::string rel;
			uint32_t chunk = 0;
			std::string name;
			std::vector<uint8_t> rgba; // the colour it's drawn with
			uint32_t cw = 0, ch = 0, w = 0, h = 0;
			uint8_t alpha_kind = 0;
			float threshold = 0;
			bool has_game = false;
			hgt::game_map game;
			hgt::made out;
		};
	}

	int heights(options &o)
	{
		namespace ht = pk::height;
		if (o.out.empty())
		{
			say("heights needs --pack <the pack's folder>.");
			return kUsage;
		}
		if (!o.kind.empty() && o.kind != "zone" && o.kind != "model")
		{
			say("--kind is zone or model.");
			return kUsage;
		}
		ht::scale scale;
		if (!ht::parse(o.height_scale, scale))
		{
			say("--height-scale is half, full or game.");
			return kUsage;
		}
		if (o.ashita.empty() && o.ffxi.empty())
			for (std::wstring f = exe_folder(); !f.empty(); f = up(f))
				if (ashita_like(f))
				{
					o.ashita = f;
					break;
				}
		if (!find_folders(o, true, false, nullptr))
			return kFailed;
		id_table ids;
		if (!ids.load(o.ffxi))
		{
			say("The game's VTABLE.DAT/FTABLE.DAT can't be read.");
			return kFailed;
		}
		const std::wstring out = with_slash(o.out);
		std::string why;
		// (the pack as it is: its header's maker version, source and fingerprint are kept; its colour read through it)
		std::shared_ptr<const pk::index> pack = pk::load(out, nullptr, why);
		if (!pack)
		{
			say("The pack in %s can't be used: %s.", narrow(out).c_str(), why.c_str());
			return kFailed;
		}
		pk::writer w;
		pk::writer_options wo;
		wo.folder = out;
		wo.maker_version = pack->head.maker_version;
		wo.source = std::string(pack->head.source, strnlen(pack->head.source, sizeof(pack->head.source)));
		wo.client = pk::fingerprint { pack->head.vtable_size, pack->head.vtable_mtime, pack->head.ftable_size, pack->head.ftable_mtime };
		if (!w.open(wo, why))
		{
			say("The pack's folder can't be used: %s.", why.c_str());
			return kFailed;
		}
		// (loaded again under the lock: what the writer holds, if another builder committed in between)
		pack = pk::load(out, nullptr, why);
		if (!pack)
		{
			say("The pack in %s can't be used: %s.", narrow(out).c_str(), why.c_str());
			w.close();
			return kFailed;
		}
		g_log = open_log((out + L"texpack.log").c_str());
		say("---- texpack heights: %s (%s resolution; game heights from the zones' bump DATs in %s)", narrow(out).c_str(), ht::name(scale), narrow(o.ffxi).c_str());

		const auto start = std::chrono::steady_clock::now();
		uint32_t made = 0, from_game = 0, already = 0, failed = 0, no_colour_read = 0, pauses = 0;
		uint64_t texels = 0;
		double waited_s = 0;
		size_t total = 0, done_dats = 0;
		std::string phase = "starting", paused, last;
		auto progress = [&](const char *state) {
			char text[2048];
			std::snprintf(text, sizeof(text),
				"texpack heights: %s\r\n"
				"summary: %zu of %zu DATs done%s\r\n"
				"state: %s\r\n"
				"phase: %s\r\n"
				"paused: %s\r\n"
				"dats_done: %zu\r\n"
				"dats_total: %zu\r\n"
				"heights_made: %u\r\n"
				"heights_from_game: %u\r\n"
				"heights_there_already: %u\r\n"
				"heights_failed: %u\r\n"
				"entries: %zu\r\n"
				"written_gb: %.3f\r\n"
				"elapsed_s: %.0f\r\n"
				"waited_s: %.0f\r\n"
				"last: %s\r\n",
				state, done_dats, total, paused.empty() ? "" : ("; paused: " + paused).c_str(), state, phase.c_str(), paused.empty() ? "-" : paused.c_str(),
				done_dats, total, made, from_game, already, failed, w.entries().size(), gb(w.stats().bytes), seconds_since(start), waited_s, last.c_str());
			pk::write_progress(out, text);
		};
		auto game_gate = [&]() -> int {
			if (o.ignore_game)
				return kDone;
			std::string reason = game_running(o.ashita, &o.game_names);
			if (reason.empty())
				return kDone;
			if (o.no_wait)
			{
				say("Stopped: %s. Run heights again with the game closed: it goes on where it stopped.", reason.c_str());
				return kGameStarted;
			}
			pauses++;
			say("Paused: %s. Goes on once the game has been closed for %.0f s.", reason.c_str(), o.game_quiet_s);
			paused = "game running (" + reason + ")";
			progress("paused");
			const auto began = std::chrono::steady_clock::now();
			auto quiet = began;
			bool closed = false;
			for (;;)
			{
				Sleep(DWORD(o.game_poll_s * 1000));
				if (pk::stop_requested(out))
				{
					waited_s += seconds_since(began);
					paused.clear();
					return kStopped;
				}
				if (!game_running(o.ashita, &o.game_names).empty())
				{
					closed = false;
					continue;
				}
				if (!closed)
					closed = true, quiet = std::chrono::steady_clock::now();
				else if (seconds_since(quiet) >= o.game_quiet_s)
					break;
			}
			waited_s += seconds_since(began);
			paused.clear();
			say("The game has been closed for %.0f s: going on.", o.game_quiet_s);
			return kDone;
		};
		int code = game_gate();
		if (code != kDone)
		{
			progress(code == kStopped ? "stopped (the stop file)" : "stopped (the game)");
			w.close();
			return code;
		}

		// The DATs: pack.src's, zones (a zone's file id) first, by path
		struct dat
		{
			std::string rel;
			uint32_t id = 0xFFFFFFFFu;
			bool zone = false;
		};
		std::vector<dat> dats;
		{
			std::set<std::string> only(o.only.begin(), o.only.end());
			for (const auto &[rel, s] : w.sources_done())
			{
				if (!only.empty() && only.count(rel) == 0)
					continue;
				dat d;
				d.rel = rel;
				d.id = ids.id(rel);
				d.zone = hgt::is_zone_id(d.id);
				if (!o.kind.empty() && (o.kind == "zone") != d.zone)
					continue;
				dats.push_back(d);
			}
			for (const std::string &rel : o.only)
				if (w.sources_done().count(rel) == 0)
					say("%s: not in pack.src: skipped", rel.c_str());
			std::sort(dats.begin(), dats.end(), [](const dat &a, const dat &b) { return a.zone != b.zone ? a.zone : a.rel < b.rel; });
			if (o.limit != 0 && dats.size() > o.limit)
				dats.resize(o.limit);
		}
		total = dats.size();

		// Free space: the reserve plus BC4's half a byte a texel (with mips) of every entry's colour, at most
		{
			uint64_t estimate = 0;
			for (const pk::entry &e : w.entries())
				if (const pk::record *c = e.map(pk::kColour))
					estimate += uint64_t(double(c->w) * c->h * 0.5 * 4.0 / 3.0 + 512);
				else
					estimate += uint64_t(double(e.w) * e.h * 0.5 * 4.0 / 3.0 + 512);
			ULARGE_INTEGER free_bytes = {};
			std::wstring probe = no_slash(out);
			GetDiskFreeSpaceExW(probe.c_str(), &free_bytes, nullptr, nullptr);
			say("%zu DATs to go through; the pack has %zu entries; at most %.2f GB more; %.1f GB free", dats.size(), w.entries().size(), gb(estimate),
				gb(free_bytes.QuadPart));
			if (double(free_bytes.QuadPart) < o.reserve_gb * double(1ull << 30) + double(estimate))
			{
				say("Refused: the disk needs %.0f GB free beyond the estimate.", o.reserve_gb);
				phase = "the disk is too full";
				progress("failed");
				w.close();
				return kFailed;
			}
		}
		SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

		pk::reader rd(pack);
		// (a key seen already, and whether its height is the game's: a texture two zones share (one key) is made once,
		// from the first DAT by path; a later zone whose bump DAT has the game's map for it makes it again from that map
		// when the first had none: game heights first, whichever zone sorts first)
		std::map<pk::key, bool> seen;
		uint32_t to_game = 0;
		const uint64_t budget = uint64_t(o.batch_mtexels * 1e6);
		size_t next = 0;
		while (next < dats.size())
		{
			if (pk::stop_requested(out))
			{
				say("Stopped by %sstop (delete it to go on).", narrow(out).c_str());
				code = kStopped;
				break;
			}
			if ((code = game_gate()) != kDone)
				break;
			phase = "reading DATs";
			progress("running");
			std::vector<hjob> jobs;
			uint64_t batch_texels = 0;
			const size_t first = next;
			while (next < dats.size() && (next == first || batch_texels < budget))
			{
				const dat &d = dats[next++];
				std::vector<uint8_t> bytes;
				if (!bk::read_file(o.ffxi + L"\\" + wide(d.rel), bytes))
				{
					say("%s: can't be read: skipped", d.rel.c_str());
					continue;
				}
				std::unordered_map<std::string, hgt::game_map> games;
				if (d.zone)
				{
					const uint32_t bump = hgt::bump_id_of(d.id);
					std::vector<uint8_t> bump_bytes;
					if (bump < ids.rel_of.size() && !ids.rel_of[bump].empty() && bk::read_file(o.ffxi + L"\\" + wide(ids.rel_of[bump]), bump_bytes))
						games = hgt::game_maps(bump_bytes);
				}
				const std::vector<bk::chunk> cs = bk::chunks(bytes);
				for (size_t i = 0; i < cs.size(); ++i)
				{
					const bk::chunk &c = cs[i];
					if (c.type != 0x20 || c.size < 69 || c.body[0] != 0xA1)
						continue;
					const pk::key k = pk::key_of(c.body, c.size);
					const pk::entry *e = w.find(k);
					if (e == nullptr || (e->flags & pk::kNoPack) != 0)
						continue;
					const auto game_here = d.zone ? games.find(hgt::bump_name(c.body + 1)) : games.end();
					const bool has_game_here = game_here != games.end();
					const auto was = seen.find(k);
					if (was != seen.end() && (was->second || !has_game_here))
						continue;
					// (its height there already: kept, unless it was made from the colour and this zone has the game's map)
					const bool game_already = e->map(pk::kHeight) != nullptr && (e->flags & pk::kHeightFromGame) != 0;
					if (e->map(pk::kHeight) != nullptr && !o.redo && (game_already || !has_game_here))
					{
						if (was == seen.end())
							already++;
						seen[k] = game_already;
						continue;
					}
					if (was != seen.end())
					{
						to_game++;
						say("%s: chunk %u '%s': its bump DAT has the game's map for this texture, which a zone before it shares without one: made "
							"again from the game's", d.rel.c_str(), uint32_t(i), name16(c.body + 1).c_str());
					}
					seen[k] = has_game_here;
					hjob j;
					j.k = k;
					j.rel = d.rel;
					j.chunk = uint32_t(i);
					j.name = name16(c.body + 1);
					j.w = e->w, j.h = e->h;
					j.alpha_kind = e->alpha_kind;
					j.threshold = e->source_kind == pk::kSourceZone ? pk::bc2::zone_cut_threshold(e->vcol_a) : pk::bc2::kCharsCutThreshold;
					// (the colour it's drawn with: the pack's, else the original)
					bool have = false;
					if (const pk::record *col = e->map(pk::kColour); col != nullptr && col->format == pk::kBC2)
					{
						std::vector<uint8_t> buf(col->bytes);
						std::vector<std::vector<uint8_t>> levels;
						std::string w2;
						if (rd.read_level(*col, 0, buf.data(), w2) && untile(pk::record { col->kind, 1, col->file, col->w, col->h, col->format }, buf, levels))
						{
							j.rgba = pk::bc2::decode_bc2(levels[0].data(), col->w, col->h);
							j.cw = col->w, j.ch = col->h;
							have = true;
						}
						else
							no_colour_read++, say("%s: chunk %u '%s': its colour record can't be read (%s): made from the original", d.rel.c_str(), j.chunk,
								j.name.c_str(), w2.c_str());
					}
					if (!have)
					{
						tex::texture t;
						std::string w2;
						if (!tex::decode(c.body, c.size, t, w2) || t.levels.empty())
						{
							failed++;
							say("%s: chunk %u '%s': can't be decoded (%s)", d.rel.c_str(), j.chunk, j.name.c_str(), w2.c_str());
							continue;
						}
						j.rgba = std::move(t.levels[0].rgba);
						j.cw = t.w, j.ch = t.h;
					}
					if (has_game_here)
						j.has_game = true, j.game = game_here->second;
					batch_texels += uint64_t(j.cw) * j.ch;
					jobs.push_back(std::move(j));
				}
				last = d.rel;
			}
			phase = "making " + std::to_string(jobs.size()) + " height maps";
			progress("running");
			{
				std::atomic<size_t> at { 0 };
				std::vector<std::thread> pool;
				for (uint32_t t = 0; t < std::max(1u, o.threads); ++t)
					pool.emplace_back([&] {
						for (size_t i; (i = at++) < jobs.size();)
						{
							hjob &j = jobs[i];
							j.out = hgt::make(j.rgba.data(), j.cw, j.ch, j.w, j.h, j.alpha_kind, j.threshold, j.has_game ? &j.game : nullptr, scale);
							j.rgba.clear();
							j.rgba.shrink_to_fit();
						}
					});
				for (std::thread &t : pool)
					t.join();
			}
			bool write_failed = false;
			for (hjob &j : jobs)
			{
				if (!j.out.ok)
				{
					failed++;
					say("%s: chunk %u '%s': no height (%s)", j.rel.c_str(), j.chunk, j.name.c_str(), j.out.why.c_str());
					continue;
				}
				std::string w2;
				const pk::add_result r = w.set_map(j.k, j.out.map, j.out.from_game ? pk::kHeightFromGame : 0, j.out.from_game ? 0 : pk::kHeightFromGame, w2);
				if (r == pk::add_result::refused)
				{
					failed++;
					say("%s: chunk %u '%s': not added: %s", j.rel.c_str(), j.chunk, j.name.c_str(), w2.c_str());
					write_failed = write_failed || w2.find("writing") != std::string::npos || w2.find("can't open") != std::string::npos;
					continue;
				}
				made++;
				from_game += j.out.from_game;
				texels += uint64_t(j.out.map.w) * j.out.map.h;
			}
			if (write_failed)
			{
				say("Stopped: the pack's data file can't be written.");
				code = kFailed;
				break;
			}
			if (!w.commit(why))
			{
				say("Stopped: %s.", why.c_str());
				code = kFailed;
				break;
			}
			done_dats = next;
			say("batch: DATs %zu-%zu of %zu, %zu height maps; %u made so far (%u from the game's), %.1f s", first + 1, next, total, jobs.size(), made, from_game,
				seconds_since(start));
			phase = "between batches";
			progress("running");
		}
		const double took = seconds_since(start);
		phase = code == kDone ? "done" : "stopped";
		progress(code == kDone ? "finished" : code == kStopped ? "stopped (the stop file)" : code == kGameStarted ? "stopped (the game)" : "failed");
		say("heights: %zu of %zu DATs in %.1f s (%.0f s waiting for the game, %u pauses): %u made (%u from the zones' bump DATs, %u from the colour), "
			"%u there already, %u made again from the game's map a zone after it has, %u failed, %u colour records unreadable (made from the original); "
			"%.2f M map texels, %.3f GB written (%.3f bytes a map "
			"texel)",
			done_dats, total, took, waited_s, pauses, made, from_game, made - from_game, already, to_game, failed, no_colour_read, double(texels) / 1e6, gb(w.stats().bytes),
			texels > 0 ? double(w.stats().bytes) / double(texels) : 0.0);
		w.close();
		return code;
	}
}
