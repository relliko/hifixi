#pragma once

// What hifixi-texpack's commands share (texpack.cpp: convert, verify, stats, compact, prune; make.cpp: make; fetch.cpp:
// get-upscaler): console and texpack.log lines, paths, the options, finding the game's folders, the game's file ids, the
// game check, the alpha tags and the soul test. Defined in texpack.cpp unless said otherwise.

#include <windows.h>
#include "../../src/pack/pack.hpp"
#include "../../src/bake/dat.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace hifi::bake::zonemesh {}
namespace hifi::chars::os2 {}
namespace hifi::chars::tex {}

namespace hifi::texpack
{
	namespace pk = hifi::pack;
	namespace bk = hifi::bake;
	namespace zm = hifi::bake::zonemesh;
	namespace os2 = hifi::chars::os2;
	namespace tex = hifi::chars::tex;

	// pack.src's and the header's maker version for convert: a pack.src line of another version is done again
	constexpr uint32_t kMakerVersion = 1;
	// make's (a pack.src line convert wrote is made again by make, and the other way round). From 3 (M8) pack.src and
	// the header hold make_maker's: kMakeVersion * 100 + the upscaler model's number * 10 + the colour's (0 BC2, 1 BC7),
	// so a DAT made with another model or colour format is made again (its own entries replaced): 311 = make 3, HDCube,
	// BC7; 300 = make 3, x4plus, BC2
	constexpr uint32_t kMakeVersion = 3;
	constexpr uint32_t make_maker(uint8_t model_number, bool bc7) { return kMakeVersion * 100 + model_number * 10u + (bc7 ? 1u : 0u); }
	constexpr int kDone = 0, kFailed = 1, kUsage = 2, kStopped = 3, kGameStarted = 4;

	// ---- Small things

	extern FILE *g_log; // texpack.log in the pack's folder (convert, make, compact, prune)
	// texpack.log opened to append (others may read it meanwhile); nullptr when it can't be
	FILE *open_log(const wchar_t *path);
	// A line to the console and texpack.log
	void say(const char *format, ...);
	std::wstring wide(const std::string &s);
	std::string narrow(const std::wstring &w);
	std::wstring no_slash(std::wstring f);
	std::wstring with_slash(const std::wstring &f);
	// The folder above (no slash at the end); "" at the top
	std::wstring up(const std::wstring &path);
	bool is_file(const std::wstring &p);
	bool is_folder(const std::wstring &p);
	// A folder and everything in it deleted; a junction or symbolic link (the folder itself or one below) only unlinked,
	// never followed
	void remove_tree(const std::wstring &folder);
	bool file_info(const std::wstring &p, uint64_t &size, uint64_t &mtime);
	std::vector<std::string> lines_of(const std::wstring &path, bool &ok);
	std::vector<std::string> split(const std::string &s, char by);
	bool all_digits(const std::string &s);
	// A texture's 16-byte name for a log line (trailing blanks and zeros off)
	std::string name16(const uint8_t *p);
	uint32_t u32_at(const uint8_t *p);
	int32_t i32_at(const uint8_t *p);
	double gb(uint64_t bytes);
	double seconds_since(std::chrono::steady_clock::time_point t);

	// ---- Options

	struct options
	{
		std::string command;
		std::wstring ffxi, overlay, lists, out, ashita, ffximain;
		std::string kind; // (convert, make: only zone or only model DATs)
		std::vector<std::string> only;
		uint32_t limit = 0, threads = 0, worst = 20;
		bool soul = false, ignore_game = false, redo = false, scan = false;
		double reserve_gb = 20.0;
		// (the soul test's limits, in 8-bit levels: an entry's colour error mean and 99th percentile over the texels
		// kept, and its alpha's. Set from the 292 baked zones' 4,750 entries, 2026-11-02: colour mean p50 1.17, max
		// 2.66; colour p99 p50 2.77, max 12.67; alpha mean max 5.16; alpha p99 p99 14.9, max 30.8. Each limit is about
		// 1.5 times the zones' worst, so a texture whose painting the bake lost stands out; models measured below them.
		// verify reports an entry over them; convert and make leave it out)
		double soul_mean = 4.0, soul_p99 = 20.0, alpha_mean = 8.0, alpha_p99 = 48.0;
		// make (make.cpp): the upscaler's folder, the factors, the batch's original texels (millions), the game check's
		// timing (seconds: closed this long before it goes on, polled this often while the upscaler runs), the
		// upscaler's longest stretch without a new picture before it counts as stuck, the process names that count as
		// the game (tests: their own), --no-wait (the game started: stop with exit 4, as convert), --rescan (dats.tsv
		// made again), --keep-work (the batch's pictures kept)
		std::wstring upscaler;
		uint32_t zone_factor = 2, model_factor = 4;
		double batch_mtexels = 24.0, game_quiet_s = 60.0, game_poll_s = 3.0, stall_s = 900.0;
		std::vector<std::wstring> game_names;
		bool no_wait = false, rescan = false, keep_work = false;
		// (M9) heights (heights.cpp) and make --heights: the height maps' resolution (height.hpp scale: half, full,
		// game; half until Alex picks, open question 11); make carries them only with --heights (off until M9 is
		// approved)
		std::string height_scale = "half";
		bool heights = false;
		// make (M8): --upscaler-model (hdcube, x4plus), --colour (bc7, bc2), --texconv (texconv.exe; default <this
		// tool's folder>\texconv.exe; missing: BC2, said), --texconv-args (measuring: more texconv options)
		std::string upscaler_model = "hdcube", colour = "bc7";
		std::wstring texconv, texconv_args;
		// get-upscaler (fetch.cpp): --yes (no question), --dir (where; default <exe folder>\upscaler), --url (tests: a
		// file:// URL in place of the official one; the size and SHA-256 checked are always the official file's)
		bool yes = false;
		std::wstring dir, url;
	};

	void usage();
	bool parse(int argc, wchar_t **argv, options &o);

	// ---- Where things are

	// Ashita's folder: one with config\pivot\pivot.ini (XIPivot's settings) or Ashita.dll
	bool ashita_like(const std::wstring &f);
	bool ffxi_like(const std::wstring &f);
	// PlayOnline's install folder for FINAL FANTASY XI from the registry ("" when none); a guess to be checked
	std::wstring registry_ffxi();
	// XIPivot's root folder from Ashita's pivot.ini ("" when there's none)
	std::wstring pivot_root(const std::wstring &ashita);
	std::wstring exe_folder();

	// dats.tsv (research\ffxi-re\tools\upscale\sortdats.py; make writes its own in the pack's folder): rel path, kind
	// (zone, model, other), textures, texels, the original's bytes, the XIPivot overlay ahead of ours that has it ('-':
	// none)
	struct tsv_row
	{
		std::string kind, shadow;
		uint64_t bytes = 0;
		uint64_t texels = 0;
	};
	using tsv = std::unordered_map<std::string, tsv_row>;
	bool read_tsv(const std::wstring &lists, tsv &out);
	// dats.tsv from --lists, or from the bake's folder beside the tool's
	bool read_rows(options &o, tsv &out);
	// The folders the command needs found (texpack.cpp has the order); false (said why) when they aren't
	bool find_folders(options &o, bool need_ffxi, bool need_overlay, const tsv *rows);

	// ---- The game's file ids (VTABLE/FTABLE, then ROMn's VTABLEn/FTABLEn, in dat_path's order)

	struct id_table
	{
		std::vector<std::string> rel_of;                 // by file id ("" none)
		std::unordered_map<std::string, uint32_t> id_of; // (the lowest id naming a file)

		bool load(const std::wstring &ffxi);
		uint32_t id(const std::string &rel) const
		{
			auto it = id_of.find(rel);
			return it == id_of.end() ? 0xFFFFFFFFu : it->second;
		}
	};

	// ---- Is the game running?

	// Why the game counts as running ("" when it doesn't): pol.exe or phoenix-loader.exe (or the names given: tests),
	// or one of Ashita's logs (logs\*.txt) written in the last minute
	std::string game_running(const std::wstring &ashita, const std::vector<std::wstring> *names = nullptr);

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
					return uint8_t(i < 1 ? 1 : i);
			return 0xFF;
		}
	};

	struct key_hash
	{
		size_t operator()(const pk::key &k) const { return size_t(k.hash ^ (k.hash >> 32) ^ k.bytes); }
	};
	using tag_map = std::unordered_map<pk::key, tag, key_hash>;

	// A zone DAT's textures tagged from its meshes; false (why) when its meshes can't be read
	bool tag_zone(const std::wstring &ffxi, const std::string &rel, uint32_t id, tag_map &out, std::string &why);
	// A model DAT's textures tagged from its Os2 meshes
	void tag_models(const std::vector<bk::chunk> &all, tag_map &out);
	uint8_t alpha_kind_of(const tag &t);

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
	// The pack's level 0 (tight BC2 or BC7 blocks, by format, W x H) against the original chunk
	soul_result soul(const uint8_t *body, size_t size, const uint8_t *level0, uint32_t W, uint32_t H, uint32_t format = pk::kBC2);
	// Whether a soul test's errors pass the limits (verify reports such an entry; convert and make leave it out)
	bool over_limits(const options &o, const soul_result &s);

	// A record's levels as tight BC blocks (the copy layout's row padding taken out)
	bool untile(const pk::record &r, const std::vector<uint8_t> &bytes, std::vector<std::vector<uint8_t>> &levels);

	// ---- The other files' commands

	int make(options &o);         // make.cpp
	int get_upscaler(options &o); // fetch.cpp
	int heights(options &o);      // heights.cpp
	// (fetch.cpp) The HDCube model that ships beside the tool (<tool folder>\models-hdcube: realesrgan-hdcube-x4.param,
	// .bin and PROVENANCE.txt) copied into an upscaler folder's models\, each file's SHA-256 checked first: get-upscaler
	// does it after the download, make when the upscaler folder lacks it. false (why) when it isn't there or differs
	bool place_hdcube(const std::wstring &upscaler, std::string &why);
}
