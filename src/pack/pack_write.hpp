#pragma once

// The texture pack written (docs/texture-pack/format.md "Writing"): what builds packs (texpack convert and make, the
// tests) adds entries, and commit() makes them visible. Never linked into the plugin, which only reads.
//   - One writer a folder: open() takes pack.lock (no sharing, deleted on close, so it goes with the process however
//     that ends) and keeps it until close(); a second writer, in this process or another, is refused. compact holds
//     the old pack's lock with folder_lock while it copies.
//   - Data files only grow: each record is appended at the next multiple of 512 after the file's end, in a file opened
//     with FILE_SHARE_READ only (a reader may read along, nothing else writes); while a record would pass
//     max_file_bytes the next pack-NNN.bin is started (a file an interrupted run left bytes in may not have room
//     either), and the file left is flushed first (commit flushes only the current one).
//   - Keys are unique: the first entry added with a key stays; a later one is counted (the same bytes, or other ones:
//     two bake runs that made the same texture differently) and not written, unless add() is told to replace it (the
//     same source DAT converted again from a re-baked copy).
//   - commit(): the data flushed, pack.idx written as pack.idx.new and moved over pack.idx (MOVEFILE_REPLACE_EXISTING),
//     and only then the source DATs finished since the last commit appended to pack.src: a line there always means
//     its entries are in pack.idx. Lines forget() took out are taken out of pack.src (rewritten whole) before pack.idx
//     is replaced: a missing line only means the DAT is converted again.
//   - open() on a folder that has a pack resumes it: its entries are kept, appending continues at the last data file's
//     real end (bytes an interrupted run wrote past the index's sizes are left unreferenced: compact drops them), and
//     done() answers from pack.src. A pack that can't be loaded is refused, never written over.

#include "pack.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hifi::pack
{
	// One map to add: its levels as tight BC blocks (rows of blocks back to back), largest first
	struct map_in
	{
		uint8_t kind = kColour;
		uint32_t format = kBC2;
		uint32_t w = 0, h = 0;
		std::vector<std::vector<uint8_t>> levels;
		uint32_t param = 0; // (record::param: a height record's span)
	};

	struct writer_options
	{
		std::wstring folder;      // (made when missing)
		uint32_t maker_version = 1;
		std::string source;       // the header's source ("overlay convert"), at most 95 characters kept
		fingerprint client;       // the game's files the pack is made from
		uint64_t max_file_bytes = kMaxFileBytes; // (tests: smaller, to see the roll-over; never more than kMaxFileBytes)
	};

	struct writer_stats
	{
		uint32_t kept = 0;     // entries the folder's pack already had at open()
		uint32_t added = 0;
		uint32_t same = 0;     // a key already there, with the same bytes (CRC, size and format)
		uint32_t differs = 0;  // a key already there, with other bytes (the first stays)
		uint32_t replaced = 0; // a key already there, with other bytes, replaced as add() was told
		uint32_t refused = 0;
		uint32_t dropped = 0;
		uint32_t maps_added = 0, maps_replaced = 0; // (set_map: a map put in a free slot, or over one of its kind)
		uint64_t bytes = 0;    // record bytes written by this writer
		uint32_t commits = 0;
	};

	enum class add_result
	{
		ok,
		same,
		differs,
		replaced,
		refused,
	};

	// A source DAT as pack.src has it: the original's size and last write time, and the overlay copy's it was converted
	// from (0 and 0 when it had none, or for a line written before they were kept)
	struct source_info
	{
		uint64_t size = 0, mtime = 0;
		uint64_t overlay_size = 0, overlay_mtime = 0;
		uint32_t maker = 0;
	};

	// A pack folder's pack.lock: held by one builder at a time (see the top)
	class folder_lock
	{
	public:
		folder_lock() = default;
		~folder_lock() { release(); }
		folder_lock(const folder_lock &) = delete;
		folder_lock &operator=(const folder_lock &) = delete;
		// false (why) when another builder holds it, or the folder has no room for it
		bool take(const std::wstring &folder, std::string &why);
		void release();

	private:
		void *handle = nullptr;
	};

	class writer
	{
	public:
		writer() = default;
		~writer();
		writer(const writer &) = delete;
		writer &operator=(const writer &) = delete;

		// The folder's pack resumed, or a new one begun; false (why) when another writer has the folder, its pack can't be
		// loaded or the folder can't be made
		bool open(const writer_options &options, std::string &why);
		// Whether pack.src says rel was finished from these files (the original's size and mtime, the overlay copy's) by
		// this maker version (the last line for rel counts): resume skips it
		bool done(const std::string &rel, uint64_t size, uint64_t mtime, uint64_t overlay_size = 0, uint64_t overlay_mtime = 0) const;
		// An entry: e's key and its other fields (diagnostics, flags, alpha) as given, its maps laid out in the copy
		// layout and appended. A key already there isn't written again (same or differs), unless replace and its bytes
		// differ: then the new entry takes the key and the old one's bytes are left for compact (replaced); refused (why)
		// when a map breaks the rules or a level's size isn't its blocks'
		add_result add(const entry &e, const std::vector<map_in> &maps, std::string &why, bool replace = false);
		// (M9: texpack heights) One map put into the entry that has key k, its other maps kept: in place of its record of
		// that kind (whose bytes are then left for compact), else in its first unused slot. flags_set is or'ed into the
		// entry's flags and flags_clear taken out (kHeightFromGame). same when the entry's record of that kind already
		// has these bytes (the flags still set); refused (why) when there's no such entry, no free slot, or the map breaks
		// the rules
		add_result set_map(const key &k, const map_in &map, uint16_t flags_set, uint16_t flags_clear, std::string &why);
		// rel finished (all its entries added): its pack.src line goes out with the next commit
		void finished(const std::string &rel, uint64_t size, uint64_t mtime, uint64_t overlay_size = 0, uint64_t overlay_mtime = 0);
		// rel's pack.src line taken out at the next commit (prune: its DAT changed or is gone), so it's converted again
		bool forget(const std::string &rel);
		// The entry with this key taken out of the index at the next commit (prune); its bytes stay until compact
		bool drop(const key &k);
		// Everything added made visible (see the top); false (why) when a write fails: the last pack.idx stays
		bool commit(std::string &why);
		// The data file closed and pack.lock let go (no commit: what was added since the last commit isn't in pack.idx)
		void close();

		const writer_stats &stats() const { return counts; }
		const std::vector<entry> &entries() const { return list; }
		const entry *find(const key &k) const;
		// pack.src's sources as of the last commit (or open)
		const std::unordered_map<std::string, source_info> &sources_done() const { return sources; }

	private:
		bool open_file(uint32_t file, std::string &why);
		void close_file();
		bool append(const uint8_t *data, uint64_t size, uint32_t &offset, uint16_t &file, std::string &why);
		// A map in the copy layout, its record (all but offset, file and level_crc) and level CRCs; nullptr or why not
		struct laid
		{
			record r;
			std::vector<uint8_t> bytes;
			std::vector<uint32_t> level_crcs;
		};
		static const char *lay_out(const map_in &in, laid &o, std::string &why);
		struct key_hash
		{
			size_t operator()(const key &k) const { return size_t(k.hash ^ (k.hash >> 32)); }
		};
		writer_options opt;
		folder_lock lock;
		std::vector<entry> list;
		std::unordered_map<key, size_t, key_hash> where;
		std::vector<uint32_t> crcs;
		std::vector<uint64_t> ends;  // each data file's end (the next record goes at the next multiple of 512)
		uint32_t current = 0;        // the data file being appended to
		void *handle = nullptr;      // (its handle; nullptr until the first record)
		std::unordered_map<std::string, source_info> sources;
		std::string pending;         // pack.src lines for the next commit
		bool rewrite_sources = false; // (forget() took a line out: pack.src rewritten at the next commit)
		writer_stats counts;
		bool opened = false;
	};

	// Whether a stop file is in the folder (the builder stops at its next DAT)
	bool stop_requested(const std::wstring &folder);
	// progress.txt replaced with text (written beside it, then moved over it)
	bool write_progress(const std::wstring &folder, const std::string &text);
}
