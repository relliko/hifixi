# The texture pack on disk (version 1)

The texture pack holds higher-resolution copies of the game's textures, made on the player's PC from their own game
files, for HiFi XI to copy straight to video memory (docs/texture-pack-plan.md). This page is the format as
`src/pack` writes and reads it, byte for byte. `src/pack/pack.hpp` holds the same tables as structs (with
`static_assert`ed sizes), and `tests/pack_oracle.py` is a second, independent reader of them. Change one, change all
three.

## The folder

`<backend::folder()>\hifixi-pack\`: the game's bootloader folder (`C:\Games\PhoenixXI\bootloader`), beside the
`worldfx-*` files (`pack::folder_in`). The environment variable `HIFIXI_PACK` names another folder (tests).

| File | What it is |
|---|---|
| `pack.idx` | The index: a header, each data file's size, the entries sorted by key, and the level CRC table. Written last, as `pack.idx.new`, then moved over `pack.idx`. |
| `pack-000.bin`, `pack-001.bin`, ... | Data files: the textures' levels in D3D12's copy layout. Append-only, at most 1 GiB each (`kMaxFileBytes`), at most 1000 of them. |
| `pack.src` | One text line per source DAT whose entries are in `pack.idx`: for resume, redo and prune. |
| `progress.txt` | The builder's progress, replaced whole each time (written beside it, then moved over it). |
| `stop` | Present: the builder stops at its next DAT. |
| `pack.lock` | Present while a builder (convert, prune, compact) writes the folder: opened with no sharing and deleted on close, so it goes with the builder however that ends. A second builder can't open it and is refused ("another texpack is writing this pack"). Readers ignore it. |
| `texpack.log` | Every line `hifixi-texpack` printed while converting, pruning or compacting there (appended; never read). |
| `pack.idx.new`, `progress.txt.new`, `pack.src.new`, `pack.src.old` | Left only by an interrupted builder (`.new`) or by a resume that found `pack.src` without an index (`.old`); never read. |

## Byte order and check sums

- Everything is little-endian, with no padding beyond what the tables show.
- **CRC-32** is zlib's `crc32`: polynomial 0xEDB88320 reflected, started and ended inverted, the check value of
  `"123456789"` is `CBF43926` (`pack::crc32` in `src/pack/hash.cpp`; Python: `zlib.crc32`).
- **Keys** use FNV-1a 64 (`pack::fnv1a64`: offset basis `CBF29CE484222325`, prime `100000001B3`).
- Times are Windows FILETIMEs (100 ns since 1601, UTC) as u64.

## pack.idx

The whole file, in this order, with nothing after it:

| Offset | Size | What |
|---|---|---|
| 0 | 192 | header |
| 192 | 8 x file_count | each data file's size when the index was written (u64) |
| 192 + 8 x file_count | 160 x entry_count | entries, sorted by key |
| then | 4 x crc_count | level CRCs (u32) |

The file's length must be exactly `192 + 8*file_count + 160*entry_count + 4*crc_count`.

### Header (192 bytes)

| Offset | Type | Field | Value |
|---|---|---|---|
| 0 | char[4] | magic | `HXP1` |
| 4 | u32 | version | 1 |
| 8 | u32 | header_bytes | 192 |
| 12 | u32 | flags | 1 = `kSpanWholeBody`: keys are over the whole original chunk body (below). v1 has no other flag. |
| 16 | u32 | entry_bytes | 160 |
| 20 | u32 | record_bytes | 32 |
| 24 | u32 | records_per_entry | 3 |
| 28 | u32 | entry_count | |
| 32 | u32 | file_count | data files, 0 to 1000 |
| 36 | u32 | crc_count | level CRCs |
| 40 | u32 | maker_version | the builder's version (also in `pack.src`) |
| 44 | u32 | reserved | 0 |
| 48 | u64 | build_time | FILETIME of the last commit |
| 56 | u64 | vtable_size | client fingerprint: `VTABLE.DAT`'s size... |
| 64 | u64 | vtable_mtime | ...and last write time |
| 72 | u64 | ftable_size | `FTABLE.DAT`'s size... |
| 80 | u64 | ftable_mtime | ...and last write time (all four 0: unknown) |
| 88 | char[96] | source | ASCII, zero-padded: `overlay convert`, `texpack make <ver>`, ... |
| 184 | u32 | table_crc | CRC-32 of everything after the header (file sizes, entries, level CRCs) |
| 188 | u32 | header_crc | CRC-32 of the header's bytes 0..187 |

### Entry (160 bytes)

One texture of the game's.

| Offset | Type | Field | What |
|---|---|---|---|
| 0 | u64 | hash | key: FNV-1a 64 of the original chunk body |
| 8 | u32 | bytes | key: that body's length |
| 12 | u32 | w | key: the original's width (int32 at body+21) |
| 16 | u32 | h | key: the original's height (int32 at body+25) |
| 20 | u32 | orig_fourcc | A1 originals: the 4 bytes at body+57 as a u32 (the file's bytes are `1TXD` or `3TXD`: DXT1, DXT3); B1: 0 |
| 24 | u8 | orig_format | body[0]: 0xA1 (DXT), 0xB1 (8-bit palette) |
| 25 | u8 | alpha_kind | 0 none, 1 cut-out, 2 blended, 3 mask (characters' shine), 4 mixed |
| 26 | u8 | vcol_a | zone cut-outs: the typical vertex alpha (0x80 or 0xFF; 0 unknown) |
| 27 | u8 | source_kind | 0 unknown, 1 zone, 2 model (`dats.tsv`'s kind; diagnostics) |
| 28 | u8 | alpha_lo | the original texels' lowest alpha (raw) |
| 29 | u8 | alpha_hi | and highest |
| 30 | u16 | flags | 1 `no_pack` (a hit the game's texture is drawn for), 2 `height_from_game` (M9) |
| 32 | u32 | file_id | diagnostics: the source DAT's file id (VTABLE/FTABLE); 0xFFFFFFFF when no table names the DAT (a few model DATs, e.g. `ROM4\10\1.DAT`) |
| 36 | u32 | chunk | diagnostics: the texture chunk's ordinal in that DAT (`bake::chunks` order) |
| 40 | char[16] | name | diagnostics: the chunk's 16-byte name (body+1..17) |
| 56 | u32[2] | reserved | 0 |
| 64 | record[3] | maps | colour, and later height and material, in any order; unused ones all zero |

Every field from offset 20 on describes the **original** chunk, never the overlay's: the overlay's header bytes 17..57
are a template copied from `ROM\0\104.DAT`. `file_id`, `chunk` and `name` are for diagnostics and for M4 to cross-check
a key made at run time from the original file on disk (path and chunk ordinal); nothing looks entries up by them.

**Sorting.** Entries are sorted by (hash, bytes, w, h), each ascending as unsigned, with no key twice. A lookup is a
binary search; it hits only when all four are equal.

### Map record (32 bytes)

| Offset | Type | Field | What |
|---|---|---|---|
| 0 | u8 | kind | 0 unused (the whole record zero), 1 colour, 2 height (M9), 3 material (M10); another value: a later version's, skipped |
| 1 | u8 | levels | levels stored, largest first |
| 2 | u16 | file | its data file: `pack-NNN.bin` with NNN = file, three digits |
| 4 | u16 | w | level 0's width |
| 6 | u16 | h | level 0's height |
| 8 | u32 | format | DXGI_FORMAT: 74 `BC2_UNORM`, 98 `BC7_UNORM` or 80 `BC4_UNORM` |
| 12 | u32 | offset | in the data file, a multiple of 512 |
| 16 | u32 | bytes | all its levels in the copy layout: GetCopyableFootprints' total |
| 20 | u32 | crc | CRC-32 of those bytes |
| 24 | u32 | level_crc | index of level 0's CRC in the level CRC table; level i's is at level_crc + i |
| 28 | u32 | reserved | 0 |

A record's levels have CRCs of their own so that a reader that uploads only some levels (the zones' levels down to the
game's size; the top level skipped under memory pressure, §3 of the plan) still checks every byte it uses.

## The key

The key is made from the **original** texture chunk (type 0x20) as the game's own DAT holds it (`pack::key_of`):

- FNV-1a 64 over the chunk's whole body, from body[0] (the format byte) to the next chunk's header:
  `((info >> 7) & 0x7FFFF) * 16 - 16` bytes. On disk that's `bake::chunks`' body and size. In the game's memory it's
  `charmodel.cpp`'s `chunk_bytes()`: the same bytes (M0 checked 74 of 74 live textures). This is header flag 1.
- That length.
- The width and height (int32 at body+21 and body+25).

**Not in v1: keys of the overlay's copies.** While XIPivot's `hifi-textures` overlay is listed, the game holds the
overlay's bigger copies, whose keys would miss. Open question 16 is answered as the plan proposed: no second entry
under the overlay copy's key. At run time (M4) a key is made from the original file on disk (same path under the
game's folder, same chunk ordinal), and the entry's `file_id` and `chunk` let M4 cross-check it.

## pack-NNN.bin

A data file is records' bytes and the zeros between them, nothing else: no header. Each record starts at a multiple of
512 within its file and lies wholly inside it (`offset + bytes` at most the file's size; at most 1 GiB, so u32 offsets
never overflow). A texture's records may be in different files. The bytes between records, and any past the size
`pack.idx` gives, belong to no entry (an interrupted run's, or ones `compact` would drop).

### Level layout

A record's levels are stored as D3D12 copies them with `CopyTextureRegion` from a placed footprint (offset 0 = the
record's start), exactly what `ID3D12Device::GetCopyableFootprints(desc, 0, levels, 0, ...)` returns for a 2D texture of
that format, size and level count:

- Level i is `max(1, w >> i)` by `max(1, h >> i)` texels, stored as rows of 4x4 blocks: `ceil(w_i / 4)` blocks a row
  (16 bytes a block for BC2 and BC7, 8 for BC4), `ceil(h_i / 4)` rows.
- Rows are `row_pitch` apart: the row's bytes rounded up to 256 (`D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`). The padding is
  zeros.
- Level 0 is at offset 0. Each next level is at the previous level's end rounded up to 512
  (`D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT`), where a level ends after its last row's bytes (not after a whole pitch).
  The gap is zeros.
- The record's `bytes` is the last level's end: `offset_last + row_pitch * (rows_last - 1) + row_bytes_last`.
- Level i's CRC covers its span: from its offset to the next level's offset (the last level: to `bytes`), so the
  level CRCs together cover the record exactly.

`pack::footprint` (`src/pack/validate.cpp`) computes this with no device; `pack_test` holds it equal to WARP's
`GetCopyableFootprints` for BC2, BC7 and BC4 over 14 sizes and every level count. Example: a 36x20 BC2 level 0 has
144-byte rows 256 apart, 5 rows, 1,168 bytes.

### Mips

v1 colour maps are `BC2_UNORM`, level 0 copied unchanged from the upscaled overlay's BC2 blocks (the game's raw alpha:
shaders compute `2*lit*t.rgb` on raw values). The pack stores levels halved while both sides stay multiples of 4, down
to 4x4 at the least (`bc2::chain_levels`; a 128x64 texture: 5 levels, to 8x4). Each level is the last one's 8-bit
texels averaged 2x2 and encoded again (`bc2::full_chain`), with alpha by the entry's alpha kind:

- **cut-out** (1): in each level alpha is scaled by the factor that brings the fraction of texels passing the game's
  test closest to level 0's (the texel's alpha as BC2 stores it, 4 bits times 17). Zones pass when
  `saturate(4*t.a*vcol.a) > 96/255` (zone `draw.cpp`), so a texel passes above `96*255 / (4*vcol_a)` (47.8 at 0x80,
  24 at 0xFF; `vcol_a` 0 is taken as 0x80). Characters pass above 0x22 (`2*a > 0x45`, `chardraw.cpp`).
- **blended (2), mask (3), mixed (4), none (0)**: the plain 2x2 box. Rescaling would change transparency at a distance
  or the shine's strength (`kSpec*saturate(2*t.a)`).

## Rules

**A pack that can't be used** (`pack::load`): nothing of it is read, and the reason is the status ("pack can't be
used: <why>"):

- `pack.idx` missing, unreadable, shorter than a header or than its tables, or longer;
- a wrong magic, a version other than 1, table sizes other than v1's, flags other than 1;
- a bad header CRC or tables CRC;
- more than 1000 data files;
- keys not strictly ascending;
- a known record (kind 1-3) naming a data file past file_count, ending past that file's size in the index or past
  1 GiB, or with level CRCs past the table;
- a data file missing, or shorter than the index says (longer is fine: a writer may be appending).

A **client fingerprint** that differs from the game's `VTABLE.DAT`/`FTABLE.DAT` (size and last write time) is only a
status line: textures the game changed simply miss.

**A record that can't be used** is a miss (the game's texture is drawn) plus a log line. Checked before upload
(`pack::check`) and after reading:

- kind 1-3; format `BC2_UNORM`, `BC7_UNORM` or `BC4_UNORM` (never SRGB);
- w and h multiples of 4 and at most 16384; 1 to the whole chain of levels (`1 + floor(log2(max(w, h)))`);
- offset a multiple of 512; `bytes` equal to the layout's total (and, with a device, to `GetCopyableFootprints`'
  offsets, pitches, rows and total);
- the bytes read equal their CRC (the record's, or each level's);
- (M3) no level may straddle the upload ring's wrap point.

## pack.src

ASCII text, one line per source DAT, `\r\n` ended, six fields split by tabs:

    ROM\0\104.DAT<TAB>11274112<TAB>134348236045712235<TAB>1<TAB>45368272<TAB>134380901443662604

- the DAT's path relative to the FINAL FANTASY XI folder (backslashes, as `dats.tsv` writes it);
- its size in bytes;
- its last write time (FILETIME, decimal);
- the maker version that converted it;
- the size and last write time of the overlay copy it was converted from (0 and 0: none). A line of only the first
  four fields is read with 0 and 0 there.

A line is appended only after the `pack.idx` that holds that DAT's entries has replaced the old one, so a line always
means its entries are there. The last line for a path counts. A source is done (skipped on resume) when its line's
sizes, times and maker version all match the files and the builder now: a re-baked overlay copy is converted again.

Lines are taken out only by rewriting the file whole (written as `pack.src.new`, then moved over it), and always
before the `pack.idx` that drops their entries replaces the old one: a line missing only means its DAT is converted
again, while a line whose entries were gone would mean they never come back. `prune` takes out the lines of DATs gone
or changed in the game's folder; `compact` leaves out those of DATs that have a key whose record it couldn't read.

## Who writes it

`build\hifixi-texpack.exe` (64-bit, `tools\texpack\texpack.cpp`, built by `tools\build_texpack.cmd`; its header comment
has the details):

- `convert --out <pack>`: the hifi-textures overlay's upscaled textures, paired chunk by chunk with the game's
  originals, keyed by the originals, with mips by alpha kind. Only from originals the overlay was baked from: the
  size `dats.tsv` has, every chunk the bake didn't upscale byte for byte the overlay copy's, and each upscale passing
  the soul test against its original (else left out). The first DAT (zones first, then models, each by path) that has
  a key writes its entry: `file_id` and `chunk` name that DAT, though other DATs hold the same texture; that DAT
  converted again (its overlay copy re-baked, or `--redo`) replaces it. Resumable (`pack.src`), stopped by `stop`,
  refuses to run beside the game and without 20 GB plus its estimate free.
- `verify [--soul]`: every record and level CRC read back; the soul test against the originals.
- `stats [--scan]`; `prune` (drops entries no DAT of `pack.src` has any more, takes out the lines of DATs gone or
  changed; stops when a DAT can't be read or more than 1% are gone); `compact` (holds the old pack's `pack.lock`,
  writes `<pack>-compact`, then renames: the old pack is left as `<pack>-old`).

## Writing (`src/pack/pack_write.cpp`)

- **One writer.** A writer takes `pack.lock` before it reads the folder's pack and keeps it until it closes, so what it
  loaded is what it commits over; `compact` holds the old pack's lock while it copies it.
- **Share modes.** A writer opens the data file it appends to with `GENERIC_WRITE` and `FILE_SHARE_READ` only (a
  reader may read along; nothing else may write) and holds one data file open at a time.
- **Append-only.** Each record goes at the next multiple of 512 after the file's end; while `offset + bytes` would
  pass 1 GiB (or the test limit) the next `pack-NNN.bin` is started (one an interrupted run left bytes in may have no
  room either), and the file left is flushed first. A record over the limit is refused.
- **Unique keys.** The first entry added with a key stays. A later one isn't written: counted as the same (format,
  size, levels, bytes and CRC equal) or as differing (two bake runs made it differently). Only the writer's caller
  can ask for a replace (convert: the entry the same DAT's chunk made before): the new bytes are appended and take
  the key, the old ones are left for `compact`.
- **Commit.** The data file is flushed; `pack.src` rewritten if lines were taken out (above); `pack.idx.new` written
  whole and flushed, then moved over `pack.idx` with `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`
  (retried for a second while a reader has the old one open); and only then the sources finished since the last commit
  are appended to `pack.src`.
- **Resume.** A folder whose pack loads is continued: its entries and level CRCs are kept, appending continues at the
  last data file's real end (past any bytes an interrupted run left), and `pack.src` says which sources are done. A
  folder whose pack can't be loaded is refused, never written over. A `pack.src` with no `pack.idx` is renamed
  `pack.src.old` and not trusted.
- **Prune and compact.** Dropping an entry takes it out of the next index; its bytes stay. `compact` writes a new pack
  from the referenced records only. Both take the `pack.src` lines of what they drop out with it (see pack.src).

## Reading (`src/pack/pack.cpp`)

- `pack.idx` is read whole into memory and closed at once (opened with `FILE_SHARE_READ | FILE_SHARE_WRITE |
  FILE_SHARE_DELETE`); every check above runs before the index is used. The loaded index never changes; a reload is a
  new snapshot.
- Data files are opened on first use with `GENERIC_READ`, `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` and
  `FILE_FLAG_OVERLAPPED`, and read with overlapped `ReadFile` in pieces of at most 4 MB, so `CancelIoEx` ends a read
  within milliseconds. They are never mapped and never read whole.
- `reader::close()` closes every data file: the pack folder can then be renamed or deleted while the game runs (a
  folder whose files are open can't be renamed).
