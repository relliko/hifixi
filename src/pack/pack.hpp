#pragma once

// The texture pack (docs/texture-pack-plan.md): how a texture of the game's is known in it. A pack entry's key is its
// ORIGINAL texture chunk (type 0x20) as the game's own DAT holds it:
//   - FNV-1a 64 over the chunk's whole body: from body[0] (the format byte: 0xA1 DXT, 0xB1 8-bit palette, ...; the
//     16-byte name at body+1) to the next chunk's header, ((info >> 7) & 0x7FFFF) * 16 - 16 bytes. On disk that's
//     bake::chunks' body and size; in the game's memory it's charmodel.cpp's chunk_bytes() (the resource's +0x30,
//     data_size of the header copy at +0x24): the same bytes. The few bytes after the texels are part of it.
//   - that length;
//   - the width and height (int32 at body+21 and body+25).
// All three must match: a name isn't a key (hf_shie1 in ROM\55\43 and ROM\31\43 are different textures).
//
// Checked 2026-11-02 (M0, research\ffxi-re\tools\packkey_check.py): 74 of 74 textures copied from the running game
// (data\chartex\live-*, Oct 25 and 30) are byte-identical to their DATs' bodies over this span. While XIPivot's
// hifi-textures overlay is listed the game holds the overlay's copies instead (4x the size, other bytes): a key made
// from memory then misses, and only one made from the original file on disk (same path under the game's folder, same
// chunk) finds the entry.
//
// The pack's files and what's in them: docs/texture-pack/format.md (the structs below are its tables, byte for byte).
// Reading (pack.cpp): load() reads pack.idx whole, closes it and checks all of it before anything is used; a reader
// reads records with overlapped ReadFile in pieces of at most 4 MB from handles any process may write, delete or
// rename around (never mapped, never a data file read whole). Writing: pack_write.hpp. No Ashita or game:: code here:
// the 32-bit plugin and tests and the 64-bit texpack build it alike.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hifi::pack
{
	struct key
	{
		uint64_t hash = 0;
		uint32_t bytes = 0, w = 0, h = 0;
		bool operator==(const key &) const = default;
	};

	inline uint64_t fnv1a64(const uint8_t *p, size_t n)
	{
		uint64_t h = 0xCBF29CE484222325ull;
		for (size_t i = 0; i < n; ++i)
			h = (h ^ p[i]) * 0x100000001B3ull;
		return h;
	}

	// A texture chunk's key from its body (body[0] = the format byte) and its size; empty (bytes 0) when it's too small
	// to be a texture
	inline key key_of(const uint8_t *body, size_t size)
	{
		key k;
		if (body == nullptr || size < 29 || size > UINT32_MAX)
			return k;
		int32_t w = 0, h = 0;
		std::memcpy(&w, body + 21, 4);
		std::memcpy(&h, body + 25, 4);
		k.hash = fnv1a64(body, size);
		k.bytes = uint32_t(size);
		k.w = uint32_t(w > 0 ? w : 0);
		k.h = uint32_t(h > 0 ? h : 0);
		return k;
	}

	// Entries are sorted by key: hash, then bytes, w, h
	inline bool operator<(const key &a, const key &b)
	{
		if (a.hash != b.hash)
			return a.hash < b.hash;
		if (a.bytes != b.bytes)
			return a.bytes < b.bytes;
		if (a.w != b.w)
			return a.w < b.w;
		return a.h < b.h;
	}

	// ---- The format (format.md), version 1, little-endian

	constexpr char kMagic[4] = { 'H', 'X', 'P', '1' };
	constexpr uint32_t kVersion = 1;
	// Header flag: keys are key_of's, FNV-1a 64 over the whole original body from body[0] (M0's span). v1 has no other
	// flag; a pack with any other bit set is refused
	constexpr uint32_t kSpanWholeBody = 1;
	// DXGI_FORMAT values (validate.cpp checks them against dxgiformat.h): the only formats a record may hold, never SRGB
	constexpr uint32_t kBC2 = 74, kBC4 = 80, kBC7 = 98;
	// The copy layout (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT), the largest side
	// (D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) and so the most levels (16384 to 1: 15)
	constexpr uint32_t kRowPitch = 256, kLevelAlign = 512, kMaxSide = 16384, kMaxLevels = 15;
	// A data file's largest size (its offsets are u32; 1 GiB keeps every offset + bytes far from the limit), and the
	// most data files (pack-000.bin to pack-999.bin)
	constexpr uint64_t kMaxFileBytes = 1ull << 30;
	constexpr uint32_t kMaxFiles = 1000;
	// index::client_differs
	constexpr uint8_t kSameClient = 0, kClientDates = 1, kClientSizes = 2;
	// The largest piece one ReadFile asks for (CancelIoEx then ends a read within milliseconds)
	constexpr uint32_t kReadPiece = 4u << 20;

	// Record kinds (0: unused, all zero; a kind this build doesn't know is skipped)
	enum : uint8_t
	{
		kUnused = 0,
		kColour = 1,
		kHeight = 2,
		kMaterial = 3,
	};
	// Alpha kinds, from what uses the texture (the converter's tags): how its mips were made and what M4/M5 may do
	enum : uint8_t
	{
		kAlphaNone = 0,    // (nothing reads alpha, or nothing tagged it)
		kAlphaCut = 1,     // alpha-tested only: mips keep level 0's coverage (bc2::full_chain)
		kAlphaBlended = 2, // blended only
		kAlphaMask = 3,    // characters' shine mask (kSpec*saturate(2*t.a))
		kAlphaMixed = 4,   // more than one of those
	};
	// Entry flags
	enum : uint16_t
	{
		kNoPack = 1,         // the game's texture is drawn (a hit that isn't used)
		kHeightFromGame = 2, // (M9) the height map comes from the game's bump DAT
	};
	// What the source DAT is (diagnostics; dats.tsv's kind)
	enum : uint8_t
	{
		kSourceUnknown = 0,
		kSourceZone = 1,
		kSourceModel = 2,
	};

	// One map of an entry: where its levels are and their check sums. 32 bytes
	struct record
	{
		uint8_t kind = kUnused;
		uint8_t levels = 0;    // stored, largest first, at most the whole chain
		uint16_t file = 0;     // pack-NNN.bin
		uint16_t w = 0, h = 0; // level 0
		uint32_t format = 0;   // DXGI_FORMAT
		uint32_t offset = 0;   // in the data file, a multiple of 512
		uint32_t bytes = 0;    // all levels in the copy layout (validate.hpp footprint's total)
		uint32_t crc = 0;      // CRC-32 of those bytes
		uint32_t level_crc = 0; // index of level 0's CRC in the index's level CRC table (level i's at level_crc + i)
		// (M9) a height record's span, float32 bits (height.hpp: H = (s - 0.5) * span; 0 a flat map); other kinds 0.
		// Readers before M9 ignore it (it was reserved)
		uint32_t param = 0;
		float span() const
		{
			float f;
			std::memcpy(&f, &param, 4);
			return f;
		}
	};
	static_assert(sizeof(record) == 32);

	// One texture: its key, what the original was, and its maps. 160 bytes
	struct entry
	{
		// (the key)
		uint64_t hash = 0;
		uint32_t bytes = 0, w = 0, h = 0;
		// (all from the ORIGINAL chunk, never the overlay's: the overlay's header bytes are a template)
		uint32_t orig_fourcc = 0; // A1: the 4 bytes at body+57 as a u32 (the file's bytes '1TXD' or '3TXD'); B1: 0
		uint8_t orig_format = 0;  // body[0]: 0xA1, 0xB1
		uint8_t alpha_kind = kAlphaNone;
		uint8_t vcol_a = 0;       // zone cut-outs: the typical vertex alpha (0x80, 0xFF; 0 unknown)
		uint8_t source_kind = kSourceUnknown;
		uint8_t alpha_lo = 0, alpha_hi = 0; // the original texels' alpha range (raw values)
		uint16_t flags = 0;
		// (diagnostics: where the converter found it; M4 cross-checks a runtime key against file id and chunk ordinal)
		uint32_t file_id = 0;     // the DAT's file id (VTABLE/FTABLE)
		uint32_t chunk = 0;       // the chunk's ordinal in that DAT (bake::chunks' order)
		char name[16] = {};       // body+1..17
		uint32_t reserved[2] = {};
		record maps[3];
		key the_key() const { return key { hash, bytes, w, h }; }
		// This entry's record of a kind; nullptr when it has none
		const record *map(uint8_t kind) const
		{
			for (const record &r : maps)
				if (r.kind == kind && kind != kUnused)
					return &r;
			return nullptr;
		}
	};
	static_assert(sizeof(entry) == 160);

	// pack.idx's header. 192 bytes; then u64 data file sizes (file_count), entries (entry_count, sorted by key), u32
	// level CRCs (crc_count)
	struct header
	{
		char magic[4] = { 'H', 'X', 'P', '1' };
		uint32_t version = kVersion;
		uint32_t header_bytes = 192;
		uint32_t flags = kSpanWholeBody;
		uint32_t entry_bytes = sizeof(entry);
		uint32_t record_bytes = sizeof(record);
		uint32_t records_per_entry = 3;
		uint32_t entry_count = 0;
		uint32_t file_count = 0;
		uint32_t crc_count = 0;
		uint32_t maker_version = 0;
		uint32_t reserved0 = 0;
		uint64_t build_time = 0; // FILETIME, UTC
		// (the client fingerprint: VTABLE.DAT's and FTABLE.DAT's size and last write time, FILETIME; all 0 unknown)
		uint64_t vtable_size = 0, vtable_mtime = 0, ftable_size = 0, ftable_mtime = 0;
		char source[96] = {};    // ASCII, zero-padded: "overlay convert", "texpack make 1", ...
		uint32_t table_crc = 0;  // CRC-32 of everything after the header (file sizes, entries, level CRCs)
		uint32_t header_crc = 0; // CRC-32 of the header's first 188 bytes
	};
	static_assert(sizeof(header) == 192);

	struct fingerprint
	{
		uint64_t vtable_size = 0, vtable_mtime = 0, ftable_size = 0, ftable_mtime = 0;
		bool operator==(const fingerprint &) const = default;
	};
	// The game's VTABLE.DAT and FTABLE.DAT (in ffxi, the FINAL FANTASY XI folder) as a fingerprint; zeros for a file
	// that isn't there
	fingerprint client_fingerprint(const std::wstring &ffxi);

	// The pack's folder: HIFIXI_PACK when set (tests), else base\hifixi-pack\ (base: backend::folder(), the game's
	// bootloader folder). Always with a slash at the end
	std::wstring folder_in(const std::wstring &base);
	// A data file's name in a folder (with its slash): pack-007.bin
	std::wstring data_path(const std::wstring &folder, uint32_t file);

	// A pack's index, read whole and checked (load). Never changed once loaded: share it (M4 swaps snapshots)
	class index
	{
	public:
		header head;
		std::vector<uint64_t> file_bytes; // each data file's size when the index was written
		std::vector<entry> entries;       // sorted by key
		std::vector<uint32_t> level_crcs;
		std::wstring folder;              // with its slash
		// "" or a line for the status: the game's files differ from the ones the pack was made from
		std::string status;
		// How they differ (load's client given): kSameClient (or not known), kClientDates (VTABLE.DAT and FTABLE.DAT the
		// same sizes, other times: another PC, or the game's files written again; most likely the same game), kClientSizes
		// (other sizes: another version of the game, whose changed textures miss). Never a reason not to use the pack: a key
		// is the texture's own bytes, so a texture the game changed simply isn't found
		uint8_t client_differs = 0;
		// (parse fills them, for the menu's "Found" line: the module reads three numbers, never the entries) Entries by
		// source_kind (kSourceUnknown, kSourceZone, kSourceModel; a kind this build doesn't know counted as unknown), and
		// the colour records' bytes
		uint32_t by_source[3] = {};
		uint64_t colour_bytes = 0;

		// The entry with this key (hash, bytes, w and h all equal); nullptr: a miss
		const entry *find(const key &k) const;
		// A level's CRC (level < r.levels; parse checked the table holds them)
		uint32_t level_crc(const record &r, uint32_t level) const { return level_crcs[r.level_crc + level]; }
	};

	// pack.idx's bytes checked and taken apart (load's work once the file is read; the writer's resume and the tests use
	// it alone): the magic, version 1, the header's CRC and sizes, the file's exact length, the tables' CRC, keys sorted
	// with none twice, and every known record inside its data file's size and the level CRC table. false: why
	bool parse(const uint8_t *bytes, size_t size, index &out, std::string &why);
	// The pack in folder (with its slash): pack.idx read whole and closed at once, parsed, and every data file's real
	// size at least the index's (else nothing of it is used: "pack can't be used: <why>"). client: the game's files'
	// fingerprint, or nullptr; a mismatch only sets index::status. nullptr and why ("no pack.idx" when there's none).
	// cancel (the streamer's stop): checked before each piece of pack.idx (kIndexPiece) and each data file's size, so
	// it gives up within one piece's read ("cancelled")
	std::shared_ptr<const index> load(const std::wstring &folder, const fingerprint *client, std::string &why, const std::atomic<bool> *cancel = nullptr);
	constexpr uint32_t kIndexPiece = 256u << 10;
	// (packstream_test only) Called before each piece of pack.idx load reads: a slow disk
	extern void (*load_test_window)();

	// Reads records of one index's data files. Data files are opened on first use with FILE_SHARE_READ |
	// FILE_SHARE_WRITE | FILE_SHARE_DELETE (a writer may append, the folder's files may be deleted) and
	// FILE_FLAG_OVERLAPPED; each read goes in pieces of at most kReadPiece. One thread reads; cancel() from any.
	class reader
	{
	public:
		explicit reader(std::shared_ptr<const index> pack);
		~reader();
		reader(const reader &) = delete;
		reader &operator=(const reader &) = delete;

		// A record's r.bytes into out (all its levels in the copy layout), after the rules (validate.hpp check) and with
		// its CRC checked. false (why) on a broken rule, a short read, a cancel or a CRC mismatch: a miss
		bool read_record(const record &r, uint8_t *out, std::string &why);
		// One level (layout::level_span bytes: its rows, and the zeros up to the next level) into out, its level CRC
		// checked
		bool read_level(const record &r, uint32_t level, uint8_t *out, std::string &why);
		// Levels first to first + count - 1 as one span (from level first's offset, the stored layout kept: each level's
		// footprint offset is its own minus level first's) into out, in pieces of at most kReadPiece, each level's CRC
		// checked: the streamer's read into its ring. false (why) as read_record, or "no such levels"
		bool read_levels(const record &r, uint32_t first, uint32_t count, uint8_t *out, std::string &why);
		// The span read_levels reads (0 when the record or the range breaks the rules)
		static uint64_t levels_span(const record &r, uint32_t first, uint32_t count);
		// Any thread: reads in flight are cancelled (CancelIoEx) and every read fails until resume(). A cancel that comes
		// between a piece's check of the flag and its ReadFile is caught after the ReadFile (the reading thread checks the
		// flag again and cancels its own read)
		void cancel();
		void resume();
		// Every data file closed (on the reading thread, or once its read has returned): the folder can then be renamed
		// or deleted. Reads open them again
		void close();
		const index &pack() const { return *idx; }
		// (pack_test only) Called on the reading thread after a piece's cancel check, before its ReadFile: the window of
		// the cancel race
		void (*test_window)(reader &) = nullptr;

	private:
		bool read(uint32_t file, uint64_t offset, uint8_t *out, uint64_t n, std::string &why);
		std::shared_ptr<const index> idx;
		std::mutex mutex; // (the handles' list only, never held across a read)
		std::vector<void *> handles;
		void *event = nullptr;
		std::atomic<bool> cancelled { false };
	};
}
