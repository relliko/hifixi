# HiFi XI texture pack: final plan

Base: draft D1 (ship-first). Grafted in from D2: memory measurement and hardening, testing the streamer on its own, the hang report and kill switches. Grafted in from D3: the soul test, the game-size compare view and the shared surface model. Every valid checker finding is folded in below; none was judged wrong. Line numbers refer to `C:\code\LSB\hifixi-tex` (branch dev-tex) unless a path says otherwise.

---

## 1. Goal and what Alex gets

**Goal.** An **optional HiFi XI texture pack**.
- The game's DAT files stay original, so FFXI's 32-bit process keeps its normal memory size.
- HiFi XI reads high-resolution textures from its own pack and copies them straight to VRAM, outside the 4 GB address space.
- Any texture the pack lacks is drawn as the game's own.
- Without a pack, HiFi XI behaves exactly as today.

**What Alex gets, in order:**
1. **Scenery, ships and furniture from the pack (M4).** These are the textures the running bake makes, converted with no new GPU work. They sharpen a moment after zone-in. Zoning itself is unchanged.
2. **Gear, NPCs and monsters from the pack, and the overlay unlisted (M5).** `hifi-textures` comes out of `pivot.ini`, so the game loads 1x DATs again. The game heap and our d3d8.dll's managed CPU copies shrink back to normal. This is the memory win.
   - Things the game still draws itself go back to 1x, and the status counts them: type-1 meshes, models left to the game, and models before `mesh_ready`.
3. **A public release (M6).** The feature stays invisible to players without a pack.
4. **A "Make texture pack" builder for players (M7)**, then BC7 quality (M8).
5. **Extras the game's format can't hold:**
   - per-pixel relief (M9);
   - authored gloss, metal and porosity (M10);
   - rain that wets each material, with puddles (M11);
   - floor reflections (M12).

---

## 2. Principles

- **Optional.**
  - The setting "Use the texture pack" is off by default. The Off preset sets it to 0; the other presets leave it alone.
  - After a GPU stop, `backend::safe_start()` pauses the pack for one session.
  - Hidden `[textures]` keys switch off each consumer separately.
  - Turning the pack off closes every pack file handle, so the folder can be renamed or deleted while the game runs.
- **Fallback always possible.** A pack texture only replaces the picture behind an **existing** texture index. It never adds or reorders textures, so `model.missing` and the "left to the game" decisions stay the same (`src/game/modelactor.cpp` l.174-179, `fxdraw` `model_of`).
  - The game's own texture **stays in VRAM**, so switching back is instant.
- **Local data only.** The pack is Square Enix's data, made on the player's PC and never shipped. `tools/package.py` holds no pack content.
- **Keep the painted look ("soul").**
  - **Colour.** The pack's colour, box-filtered to keeptexels' own grain, must match the original within a measured tolerance (the **soul test**, §7).
  - **Alpha** must keep the original's meaning: cut-out coverage, blend strength and the characters' shine mask.
  - **Height and material maps** change only the light response, never the painted colour: no AO or cavity term is multiplied into colour.
  - **Highlight caps** stay at today's levels: zone `kSpec` 0.05, characters 0.08.
  - Effects, sky, UI and name plates stay out of the pack (Sneak Attack's beams, and the lore-sky rule).
- **Stability first: zoning is where the GPU hangs were.**
  - Pack work never runs on the zoning critical path; a zone comes up exactly as today.
  - Nothing is recorded on the game's direct command list.
  - The game thread never reads pack files, parses the index, decodes, creates resources, enumerates DXGI adapters or waits on a fence or on the streamer.
  - **Lifetime rule:** no descriptor or resource that recorded frames may still reference is ever rewritten or released.
    - All such frees go through one **fence-tagged retire list** (§4), shared by the pack and the chars cache.
    - "kFrames presents" is not used as a GPU-progress proxy.
  - Every bound descriptor points at a valid resource.
  - Root-signature and render-target changes come late, each as its own install with its own kill switch.
  - PSO variants are built in the background, never on the present path.

---

## 3. Pack format

**Folder.** `<backend::folder()>\hifixi-pack\` (the game exe's folder, beside the `worldfx-*` files). Tests use the env var `HIFIXI_PACK`.

| File | Contents |
|---|---|
| `pack.idx` | Header plus entries sorted by key. Written last: write `pack.idx.new`, then `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`. |
| `pack-000.bin` ... | Append-only data files of at most 1 GiB each (u32 offsets). A texture's levels are stored together, largest first. |
| `pack.src` | One line per finished source DAT: rel path, source size, mtime, maker version. Used for resume, redo and prune. |
| `progress.txt`, `stop` | Builder progress and stop request. |

**Plugin file access.**
- The plugin reads `pack.idx` whole into memory and closes it at once.
- It opens data files with `FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE` and reads them with overlapped `ReadFile` in chunks of 4 MB or less (so `CancelIoEx` ends a read within milliseconds).
- It never maps data files and never reads them whole.
- **Writers** (convert, make) only append, open with `FILE_SHARE_READ`, and replace `pack.idx` last.

**Index header:**
- magic `HXP1`, format version, flags (which span is hashed), entry count, data-file count and each data file's expected size;
- maker version, build time, source ("overlay convert" / "texpack make <ver>");
- client fingerprint: size and mtime of VTABLE.DAT and FTABLE.DAT;
- CRC32 of the entry table.

**When the pack can't be used.**
- A wrong version, a bad CRC, a missing data file, or any record whose `offset+bytes` exceeds its data file's real size means "pack can't be used: <why>". The pack is never read partially.
- A fingerprint mismatch only adds a status line.

**Entry** (fixed size, about 96 bytes; about 7 MB for 70k entries):
- **Key:** FNV-1a 64 over the ORIGINAL 0x20 chunk body (`units*16-16` bytes from `body[0]`), plus that length, plus the original w/h. All must match for a hit.
  - The span is the one `bake::chunks` yields (`src/bake/dat.cpp`) and `chunk_bytes` copies from game memory (`src/game/charmodel.cpp` l.307-318).
  - M0 confirms byte identity, or narrows the span to `body+1..end` or the texel payload. The header records which.
- **Diagnostics only:** DAT file id, chunk index, kind, 16-byte name.
- **Flags, all taken from the ORIGINAL chunk, never the overlay's:** the overlay's bytes 17..57 are a template copied from `ROM\0\104.DAT` by bakeall's `dxt3_chunk`.
  - original format (DXT1, DXT3 or B1), original header flags, alpha range;
  - alpha kind: none, cut-out, blended, mask (characters' shine), or mixed;
  - `no_pack` (the game draws it), `height_from_game`;
  - typical vertex alpha for zone cut-out textures.
- **Map records:** colour, optional height, optional material.
  - Each record holds the DXGI format, top w/h, level count, data file, offset, byte count and CRC32.
  - An unused record is zero; unknown record kinds are skipped.

**Level layout.** As D3D12 copies it: rows padded to `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` (256), each level 512-aligned.

**Validation before upload (on the worker):**
- The format must be BC2, BC7 or BC4 (`_UNORM`, never SRGB).
- w and h must be multiples of 4 and at most 16384, and the level count at most the full chain.
- `device->GetCopyableFootprints(format, w, h, levels)` must equal the stored byte count.
- No level may straddle the ring's wrap point.
- The record's CRC32 is checked after `ReadFile` and before `CopyTextureRegion`.
- Any failure counts as a miss (the game texture is drawn) plus a log line.

**Mips.** Full chains are stored down to 4x4 with a 2x2 box filter. The alpha treatment depends on the alpha kind:
- **Pure cut-out:** alpha is rescaled so the fraction passing the effective test matches level 0.
  - Zone test: `saturate(4*t.a*i.vcol.a) > 96/255` (`draw.cpp` l.225-226), so the texel threshold is `96/(4*255*vcol.a)`, using the entry's typical vcol.a measured by the converter.
  - Characters: `> 0x22`.
- **Blended, mask or mixed:** plain box-filtered alpha. Rescaling would change transparency at distance or the shine strength (`chardraw.cpp` l.175 `kSpec*saturate(2*t.a)`).

**Uploaded levels** (a runtime choice):
- zones: down to the game's own size (today's dev-tex rule);
- characters: `6 + log2(scale)` levels;
- under VRAM pressure: the top level is skipped.

**Formats.**
- **v1 colour:** `BC2_UNORM`, copied unchanged from the overlay, keeping the game's raw alpha (shaders do `2*lit*t.rgb` on raw values).
- **M8:** `BC7_UNORM`.
- **Height:** `BC4_UNORM` (M9).
- **Material:** `BC7_UNORM` (M10), with channels:
  - R = gloss, with Toksvig normal variance folded in;
  - G = porosity;
  - B = metal;
  - A = local height (high-passed, 0.5 = mean).

**Sizes** (from `C:\code\LSB\hifi-textures\work\all\dats.tsv`):
- zones at 2x: about 2.0 GB of colour; models at 4x: about 8 GB; plus mips;
- height and material: about 1 byte per texel more at full size (half size decided in M9).

**Disk.**
- The data files never shrink by themselves. `texpack compact` rewrites them without unreferenced bytes. `texpack prune` drops entries whose source DAT no longer matches `pack.src`.

---

## 4. How HiFi XI finds and uses it

**Snapshot and loading.**
- `pack::current()` (new `src/pack/pack.hpp`) returns `std::shared_ptr<const pack::index>`, published by an atomic swap.
- The index is opened, CRC-checked and reloaded **on the streamer worker**, never on the game thread.
- The snapshot is null while the option is off, when no pack is found, during safe start, or after a failed load.
- A generation counter changes on every on, off or reload. Each loader takes the snapshot once at start.

**Streamer** (new `src/pack/stream.cpp`).
- **Worker.** One `std::thread` with its own D3D12 COPY queue and fence, following the pattern in `src/zone/native.cpp` l.183-194 and `src/chars/cache.cpp`.
- **Ring.** One permanently mapped UPLOAD ring.
  - `[textures] ring_mb`, default 32, clamped to 8-64.
  - Created once on the worker at the first enable and kept until unload or device loss (not freed on toggle-off).
  - If creation fails, the pack stays off with a status line.
  - A level bigger than the ring is refused, and that texture falls back to the game's.
- **Resources.**
  - Pack textures are placed resources, sub-allocated from 64 MB DEFAULT heaps, or committed if M3's measurement shows the address-space cost is small.
  - Budget minus the planned bytes is checked **before** creation.
- **Upload sequence, per texture:**
  1. chunked overlapped reads into ring space;
  2. validation (§3);
  3. `CopyTextureRegion` for each level;
  4. fence wait;
  5. SRV written into a slot nothing has referenced since its last fence-retire;
  6. ready flag set with a release-store.
- **Shared residency.** A map from pack key to {resource, SRV, refcount} is shared by every owner: characters, zones and model actors. A request for a key that is already resident takes a reference and reuses the SRV. The last reference frees it through the retire list. `status()` reports the saving.
- **Ownership.** Requests and ready flags are addressed by (owner id, generation), never by raw entry pointers. The worker AddRefs any heap or resource it is about to write and Releases it through the retire list.
- **Locking.** The request mutex covers queue operations only. It is never held across `ReadFile`, a device call or a fence wait (`cache.cpp` l.9-11).
- **Cancel.** Cancelling is non-blocking: it marks the owner's requests cancelled. The owner reads the streamer's per-owner in-flight count on later presents.

**Fence-tagged retire list** (new `src/pack/retire.cpp`, shared with `src/chars/cache.cpp`).
- Entries are (resource, descriptor slot, owner pool), tagged with a direct-queue fence value.
- The fence is the one `src/d3d8/gpu.hpp` already keeps per frame, or a plugin fence signalled on the game queue at each present.
- A resource is released, and a slot returned to its free list, only after `GetCompletedValue() >= tag`. The check is made from `update()` and cannot block.
- Routed through it:
  - chars `release_texture` and `forget_all_textures`;
  - every pack free;
  - pack off and reload;
  - eviction;
  - zone and actor destruction.

**Shutdown and device order.**
1. Streamer pause, cancel and **join**. `stop()` cancels outstanding I/O with `CancelIoEx` and always joins, in under 100 ms.
2. Then `native::forget_device` / `native::stop`, `actordata` `forget_all`, and `cache::stop`.

On a device change, the streamer drops every request for the old device first. When `gpu_crash::lost()` is set, the worker stops and never waits on a fence again.

**Zones and model actors** (`zm::load_dat` in `src/bake/zonemesh.cpp`, then `gpu_zone` in `src/zone/zone_gpu.cpp`).
1. **Loading.**
   - With a snapshot, `load_dat` reads `bake::overlaid(path, L"hifi-textures")`: the pivot overlays in order, **excluding** hifi-textures, so EraDATs and zitah-canopy are still honoured.
   - The width pass (l.248-262) runs only when the result is a hifi-textures copy, so it is off in pack mode.
   - Zones served by another overlay (ROM2\0\2.DAT Zi'Tah canopy, ROM3\3\82.DAT, ROM3\5\1.DAT) simply miss the pack. That is correct.
2. **Keying.** In the texture block (l.399-423), after `im` is found (by name, or the last-8 fallback l.382-389), key = hash of `im.body`/`im.size`. On a non-`no_pack` hit, `zm::texture` stores the entry. The 1x bytes upload as today.
3. **Upload.** `gpu_zone::upload` (l.232-372) is unchanged except that the heap gets `images + textures` slots (l.342-360). The extra block is the pack region.
4. **Lifetime per zone visit.** One pack slot per index, **written at most once per gpu_zone**.
   - Zone pack textures are not evicted during a visit. Under pressure the next zone uploads without top levels.
   - Pack off or reload inside a zone clears the flags (game textures at once) and retires that zone's pack resources through the retire list. The slots are marked spent.
   - Turning on again takes effect at the next zone (the help text says so).
   - `compare` only flips the selection and frees nothing.
5. **Streaming.** After the zone is ready, the module queues its requests: most-used textures first, then nearest. They start once `game::feed`'s zone-in condition is met (the packet, or the 3 s `kZoneInWait` fallback, `src/game/feed.cpp` l.232-233, l.1050), plus a pack delay (`[textures] zone_delay_s`, default 2), and only once the old zone is retired or there is VRAM headroom.
6. **Selection.** `texture_srv(i)` (l.426-440), evaluated on the CPU at `draw.cpp` l.928, returns the pack slot when its flag is set and compare is off.
7. **`game_srv(i)`** always returns today's slot. It is used by `src/fx/fxrender.cpp` l.784 and l.824 (`SampleLevel(...,0)`) and by `src/zone/zoneshadow.cpp` l.508-523 until M10's alpha-parity check.
8. **Destruction.**
   - `gpu_zone::release()` and the destructor cancel the zone's requests.
   - The owning list (native `g.old`, actordata's old list, its 3600-present unused eviction, `forget_all`, native `forget_device`) keeps the entry until the per-owner in-flight count reads 0 on a later present. It never waits.
   - Device and stop paths follow the shutdown order above.
9. **Unchanged.** The bump DAT and the game normal maps (l.300-356, l.424-445). Model actors share the path (`src/zone/actordata.cpp` l.141-200).

**Characters and cloth** (`src/chars/cache.cpp`).
1. **Keying.** `decode_texture` (l.287) hashes `e.raw` on the worker before freeing it.
2. **Game texture first.** Today's game-size decode and `mesh_ready` behaviour are unchanged.
3. **Upgrade.** On a hit after ready, an upgrade request fills a **second descriptor pair** from `free_slots` (through the shared residency map). `texture_view_of` (l.894) returns the pack pair when ready.
   - **Detail view until M9:** with `[chars] detail` on, the pair's detail descriptor is a copy of the game-size detail SRV; otherwise null.
4. **"upgrading" state.** The entry is not `forgettable()` (l.689-692) while upgrading. Forget sets "cancelled"; the streamer keeps the pack pair until it acknowledges, then hands it to the retire list.
5. **Slot limit.** Upgrades are skipped (and counted) below about 1,024 free slots.
6. **Freeing.** `forget_texture`, the 20 s sweep and the new `cache::forget_all_textures()` (on a generation change) free both pairs **only through the retire list**. `release_texture` (l.153-161) no longer pushes to `free_slots` directly.
7. **Overlay still listed.** Live bytes are the upscaled ones, so characters miss and the status explains why.

**Texture LOD and FSR.**
- While the pack is on, zone `ps_light` and characters add `log2(render_w/display_w)` (clamped to ≥ -1.5 in total) through `SampleBias`, with the value from the draw constants.
- This sits on top of the fixed -0.5 zone bias.
- The pack off means no extra bias, so the look is unchanged.

**Effects.** `fx::textures` (`src/fx/fxtextures.cpp`), name plates (`src/world/world_font.cpp`) and the 2D UI (`src/game/uidraw.cpp`, `src/ui2d/fonthigh.cpp`) are untouched. Blended zone pieces whose entry is `no_pack` draw the game texture. An effects allow-list is a later follow-up only.

---

## 5. Memory and VRAM strategy

**32-bit address space** (phoenix-loader.exe is large-address-aware, 4 GB, shared with the game, Ashita, D3D12 and the driver).
- **Game side.** It returns to 1x once the overlay is unlisted (M5), shrinking the game heap and the d3d8 `image::cpu` copies (never freed for MANAGED, `src/d3d8/resource.cpp` l.460-464).
- **Pack side.** Ring (8-64 MB), index (about 7 MB), bookkeeping, plus measured per-resource driver overhead (M3 sets a limit). No `MapViewOfFile`, no whole-file reads, no RGBA decode of pack textures.
- **Zone loading in pack mode** reads one DAT with no width pass. The mapped upload buffer (`zone_gpu.cpp` l.268-314) shrinks back to geometry, normal maps and 1x textures.

**M1 hardening (helps the overlay now):**
- the chars worker (l.465-550) decodes only the next batch;
- `tex::make_detail` runs only with `[chars] detail` on;
- batches are counted in real bytes, an entry over `kBatchBytes` gets its own batch, and an entry over a hard cap fails;
- `mesh_ready` (l.877-886) leaves models with failed textures to the game;
- `smaller_levels` (`zone_gpu.cpp` l.91-205) moves to the loader thread, freeing each texture's temporaries before the next.

**Probes** (new `src/pack/memwatch.cpp`):
- `GlobalMemoryStatusEx` every 0.5 s;
- a `VirtualQuery` largest-free-block walk every 5 s on the worker;
- VRAM through a **cached `IDXGIAdapter3`** (as `memory_over`, `gpu_crash.cpp` l.467-490), never `gpu_memory`'s per-call factory;
- low-water marks logged once per zone change;
- whether `bakeall.py`/`realesrgan-ncnn-vulkan` is running (cached process check every 10 s, logged and put in the hang report).

Streaming pauses below 512 MB free or a 64 MB largest block (starting values, set from M1's baseline).

**VRAM.**
- Pack textures stay block-compressed (today chars hold about 10.7 B/texel as RGBA8 + detail).
- The budget comes from `QueryVideoMemoryInfo` LOCAL:
  - above 85%, new uploads skip their top level;
  - above 95%, no new uploads, and the least recently drawn **character** pack textures drop back to the game's through the retire list;
  - zones only skip top levels (§4 point 4).
- `[textures] budget_mb` forces a lower budget.
- Rough estimate on 12 GB: one 2x zone 100-250 MB; a crowded 4x city 0.5-1 GB.

**Pacing.** At most 4 copies in flight and 64 MB per present (configurable).

**Hang report.**
- `gpu_crash::add_section(&pack::crash_section)`, registered in `src/plugin/main.cpp` beside `zone::breadcrumb_report` (l.1059).
- `crash_section()` returns the text with its own heading. It only reads atomics/seqlock fields the worker writes: in flight, last key and level, last signalled fence, ring head and tail, and the bake-running flag.
- It takes no lock and makes no device or fence calls (it runs in `on_removed`, `gpu_crash.cpp` l.136-147).

---

## 6. Settings, menu, status

**Schema.**
- New field `bool (*shown)()` on `settings::entry`, honoured by `draw_tab_settings` (`menu.cpp` l.334-352) and `draw_search`. A null value means always shown.
- Entry in `src/settings/schema.cpp` kEntries after `chars.detail` (about l.229):
  `{ "textures.pack", tab::game, "Texture pack", "Use the texture pack", <help>, nullptr, nullptr, false, { 0, _, _, _ }, flag, 0, 1, 0, &pack::folder_exists }`
  - Saved as `[textures] pack=0/1`; the default lives in a new `src/pack/packoptions.hpp`.
  - `pack::folder_exists` returns a value cached at module start and on reload.
- **Help text:** "Sharper scenery and characters, from a texture pack made on this PC from your own game files. It is kept on the graphics card, so the game's own memory stays small. Anything the pack doesn't have is drawn as the game's own. Scenery switches at the next area you enter."
- **Section help** (kHelp l.297-325): "Higher-resolution textures kept on the graphics card, made on your own PC from your game's files."

**Module.** New `src/pack/pack_module.cpp`, a `passes::module` named "textures", registered like `src/fx/fxmodule.cpp:151`.
- `update()` every 0.5 s reads `hifi::game::option_on("textures","pack",false)` and `backend::safe_start()`. It posts open, close and reload to the worker and drains the retire list.
- `status()` shows only cached strings.
- `command()`: `/hifi textures on|off|compare|status|reload` (later `make|stop`).
- `device()`, and `stop()` (join, §4).
- `pivot.ini` is parsed once at start and on reload, never per menu frame.
- `main.cpp` changes only its help line (l.1268) and header comment.

**Hidden keys:** `[textures] zone=1 chars=1 height=1 material=1 surface=1 budget_mb=0 ring_mb=32 per_present_mb=64 zone_delay_s=2`.

**Menu block.** Hand-drawn in `src/ui/menu.cpp` `draw_game` (l.550-585), modelled on `draw_world`'s "Zone files" (l.479-497). It is shown when a pack folder exists, and to everyone from M7. All lines come from cached strings:
- "Found: 41,233 textures (scenery 18,400, characters 22,833), 9.8 GB", "No texture pack on this PC" or "Texture pack can't be used: <why>";
- "This area: 812 of 840 scenery textures from the pack" (also `native::status()`, l.210-239);
- "Characters: 210 of 260 textures from the pack; 14 drawn by the game itself" (also `cache::status()`, l.950);
- "On the graphics card: 1,240 MB for the pack; the game uses X of Y MB";
- "Game memory: 2.1 of 4 GB in use";
- pause lines (graphics card nearly full, game low on memory, paused after a graphics-card stop);
- overlay lines, while `pivot.ini` lists `hifi-textures`:
  - "The hifi-textures XIPivot overlay is still installed, so the game also loads its bigger files. Remove it to save the game's memory."
  - "The hifi-textures overlay is out of date for N files; remove it or rebake them." This compares overlay sources with the current ROM size/mtime from `dats.tsv`, computed once at start.
- removal: "To remove the texture pack: turn it off, then delete the hifixi-pack folder (N GB)."

`draw_advanced`'s Files list (l.595-598) gains "Texture pack: <folder>". The README's uninstall section gets the same removal steps.

---

## 7. Building the pack

**Shared code.**
- `src/bake/dat.cpp` is split: `dat_core.cpp` holds `dat_path`, `read_file`, `chunks` and `overlaid`, with no `game::find`. `key_tables` stays in `dat.cpp`.
- texpack takes `--ffxi <folder>` (plus registry and launcher discovery) and feeds `set_ffxi_folder`. The hard-coded fallback (dat.cpp l.17-27) is not used.
- The BC2 codec moves to `src/pack/bc2.cpp`.

**Now (M2): `hifixi-texpack.exe convert`.**
- New `tools/texpack/texpack.cpp`, built 64-bit by `tools/build_texpack.cmd` (`build.cmd` stays x86).
- **Inputs.**
  - Rel paths listed in `C:\code\LSB\hifi-textures\work\all\bakeall-zone.log` / `bakeall-model.log` (written after `write_dat`). Rows whose second field isn't a number (`palette only, left`) are skipped.
  - Giddeus `ROM\0\104.DAT`.
  - `--overlay <folder>` (default: the pivot root's `hifi-textures`), so convert keeps working after the folder is unlisted or archived.
- **Kind.** From `dats.tsv` column 2. File-id ranges are only a cross-check after an explicit VTABLE/FTABLE inversion.
- **Pairing.** Overlay chunk i pairs with original chunk i. It requires the same chunk count, the same 16-byte name, A1 `3TXD` in the overlay and a larger overlay size, then copies the BC2 blocks (`body+69`) unchanged. All flags come from the original chunk. A mismatch is logged and skipped, never guessed.
- **Alpha kinds.** texpack links `src/bake/zonemesh` and walks zone pieces to tag each zone texture: cut-out, blended or mixed, plus typical vcol.a. Model textures read the Os2 shine flag to mark `mask`.
- **B1 originals** (Giddeus palette chunks turned into A1 by `bakezone.py`) are keyed by the original B1 body and marked `no_pack` until a Giddeus zone_view A/B shows identical cut-outs and blending.
- **Output.**
  - Coverage-correct mips (§3), copy layout, dedupe by key, resume from `pack.src`.
  - Written straight into the final folder (no staging), `pack.idx` last.
  - Free-space precheck: refuse below 20 GB plus the estimated output, with the estimate printed from `dats.tsv` and bakeall-model.log ratios.
- **`texpack verify`** re-reads every entry (footprint and CRC) and runs the **soul test**:
  - colour box-filtered to **half the game's size** (2x2 original texels, keeptexels' GRAIN=2) vs the original averaged 2x2;
  - texels with keeptexels' strong-edge fade (`exp(-(grad/EDGE)^2)` < 0.5) are excluded;
  - alpha is compared per original format against the decoded original (`src/chars/tex.cpp` / the zone palette path);
  - thresholds (mean and 99th percentile) are set from the measured distribution on the already-baked zones;
  - failures are reported per entry.
- **`texpack stats`** reports counts, bytes, dedupe saving, and coverage against the overlay's chunk count. **`compact`** and **`prune`** as in §3.
- **When it runs.** With the game closed until `pack_test` covers appending while a reader is open. After that, live reruns are picked up by `/hifi textures reload`.

A converted pack can't beat the overlay's single BC2 encode (kept PNGs are deleted, `bakeall.py` l.67); M8 rebuilds.

**Later (M7): `hifixi-texpack.exe make`.**
- Pipeline:
  1. decode originals (`src/chars/tex.cpp`) and dedupe by key before upscaling;
  2. port of `upscale.py`: wrap-pad 8, crop, Lanczos for 2x and alpha;
  3. Real-ESRGAN x4plus as a `realesrgan-ncnn-vulkan.exe` child process (CLI as `upscale.py` l.24) on PNG batches of about 24 M texels;
  4. port of `keeptexels.py` (8 rounds, EDGE 24, GRAIN 2);
  5. coverage mips, BC2 (BC7 from M8), append.
- Never includes "other" in `dats.tsv`.
- Resume from `pack.src`, `progress.txt`, the `stop` file, BELOW_NORMAL priority.
- **Game check.** It re-checks before **every batch** (pol.exe, the FFXI process, phoenix-loader.exe, and Ashita `logs\*.txt` written in the last minute). While the game runs it pauses and ends the ESRGAN child.
- A menu button comes only after a measured trial of builder and game together.
- About 2 h for zones and 2 h for models on the 4070 Ti.

**Licence and the binary.**
- **Now (M0):** copy Real-ESRGAN out of the scratchpad (`...\scratchpad\esrgan-dl\x`) to `C:\code\LSB\research\ffxi-re\tools\upscale\esrgan`, with its download URL, version and archive SHA-256. Point `upscale.py` l.9 there for later runs. Copying does not disturb the running bake.
- Before shipping, read the upstream LICENSE files of xinntao/Real-ESRGAN (code and the x4plus weights), Real-ESRGAN-ncnn-vulkan and ncnn (the exe embeds Tencent BSD-3).
- Never ship `vcomp140d.dll`. If shipping isn't clearly allowed, download the official release on the player's consent.

---

## 8. Extras that use it

- **Per-pixel relief (M9).**
  - Height comes from the game's bump-DAT heights (map+0x9B97 / +0x14817, chunk 0x5D, `zonemesh.cpp` l.300-320), else a port of `nmmockup.py` `nmap_multi`.
  - Zone: a pack normal per piece, used only on the `kMapped` path. `kBump` and `mo.bump` (`draw.cpp` l.923) are unchanged. It streams into the t5 range's pack region.
  - Model actors get relief for the first time.
  - Characters: the detail slot's rg tilt.
- **Materials (M10).**
  - Shared `src/zone/surface.hpp` (beside `hemi_spec.hpp`) maps gloss and metal to highlight power and strength, capped at kSpec, for `draw.cpp` `piece_spec` (l.151-158) and `chardraw.cpp` `char_spec` (l.168-176).
  - Zone: a t6 table (root parameter 8) with a "no material" stand-in.
  - Characters: the detail descriptor plus a flag bit.
  - Priors: collision terrain kinds (`reshade-worldfx\tools\bake_wet.py`), the Os2 shine dword, a `matmap` port, and `data/texpack_tags.txt` (Alex's table, which wins).
- **Wet stone and puddles (M11).** A surface target, porosity darkening, gloss sheen, puddles below the height mean on up-facing open-sky surfaces, and a lagging fill level. Characters write porosity 0.
- **Floor reflections (M12).** Reuse `water_ps`'s 16+5 march and `sky_toward` (`src/render/grass.hlsl` l.3124-3168, l.2954), gated on wetness × gloss.

---

## 9. Milestones

Work happens in a new worktree, `C:\code\LSB\hifixi-pack`, branch `dev-pack`.

**Pre-install checklist** (every install):
- `git worktree list` (about 24): merge dev-hdr, dev-dawn and any other branch that is installed or about to be;
- record the installed plugin DLL hash;
- install only with the game closed: several empty process checks, and Ashita `logs\*.txt` not being written.

**Test tiers:**
- small: `build.cmd quick` + `hook_test` + one focused test;
- render: plus `build.cmd` (all offline tests) and `build.cmd e2e` with the zone tests;
- release: `build.cmd` + `build.cmd e2e` + every suite.

**In-game checks** run only after `bakeall.py` and every `realesrgan-ncnn-vulkan` process have ended. Otherwise the session is logged as "bake running" and kept out of the hang analysis. Release notes describe features and never say "untested".

### M0 — Baseline, key check, safety copies (nothing installed)
- **Steps:**
  1. Copy Real-ESRGAN to `research\ffxi-re\tools\upscale\esrgan` with its provenance (§7).
  2. Find the installed build (DLL hash vs build outputs, memory notes) and take it as the pixel baseline.
  3. In `hifixi-tex`:
     - fix the stale "one mip" comment in `src\zone\zone_gpu.hpp` (l.10-11);
     - run the focused tests;
     - commit the texture work once, with the sampler change (`draw.cpp` l.656-664) in its own commit.
  4. The identical staged copy in `C:\code\LSB\hifixi-dawntex` (detached at dev-dawn 8cc6855): check with the session that owns it, then drop it or rebase it onto that commit.
  5. Merge dev-hdr and dev-dawn, then create `dev-pack`.
  6. New `research\ffxi-re\tools\packkey_check.py`:
     - compare the live span (`((header>>7)&0x7FFFF)*16-16`) in `data\chartex\live-*\raw.pkl` with the chunk bodies in `ROM_310_0.DAT`, `ROM_148_92.DAT` and `ROM2_0_52.DAT`;
     - record the span in `src\pack\pack.hpp`.
  7. Check the B1 palette offset in `chars.py` (+57 vs +61) against `src\chars\tex.cpp`.
  8. Estimate the final overlay size from bakeall-model.log ratios and record it.
- **Rule:** nothing built from dev-tex is installed before M1 has moved `smaller_levels` off the game thread (today it runs from `native::follow()`, `native.cpp` l.97-115).
- **Tests:** `build.cmd quick` + `hook_test`; `zonemesh_test`; `zonedraw_test`; `zoneinject_test`; `zone_view` Giddeus; `packkey_check` matched/total.
- **Done when:** dev-tex is committed once, dev-pack exists, the key matches 100% with a recorded span, and the ESRGAN copy is safe.
- **Risks:** live bytes differ (narrow the span, last resort a root-file-id + name key); `draw.cpp` merge conflicts.
- **Done 2026-11-02** (branch `dev-pack` in `C:\code\LSB\hifixi-tex`, from dev-tex 6b9e8ab = dev-hdr 9b04ee8 + the texture work, with dev-dawn 8cc6855 merged):
  1. ESRGAN copy: already in place with `PROVENANCE.txt` (licence check still M7).
  2. Installed build: dev-tex 6b9e8ab (plugin A7649384, d3d8 D77DB7A8, installed 14:12). Pixel baseline: `build\m0-baseline\giddeus-street-{own,overlay}.bmp` (zone_view 145 street, 2560x1440; own textures vs `HIFIXI_PIVOT_INI` = the overlay).
  3. The zone_gpu comment was already right. The texture work went in as 120cb36 with the sampler change in the same commit (not split: it was merged and installed before M0). Suites 44/44 (zonedraw_test's address clash passes alone).
  4. `hifixi-dawntex` holds an older copy of the same work (codec on the game thread), wholly superseded by 120cb36. Left in place for the session that made it; safe to remove.
  5. dev-hdr and dev-dawn merged, `dev-pack` made.
  6. **Key: the full body matches.** `packkey_check.py`: 74/74 live textures (6 captures, 29 names, 31 DATs) byte-identical to their DAT bodies over `((info>>7)&0x7FFFF)*16-16` bytes from body[0], the span `chunk_bytes()` copies. Recorded in `src\pack\pack.hpp` (`key_of`), checked by `tests\packkey_test.cpp` against the Python keys. A name is not a key (hf_shie1 in ROM\55\43 and ROM\31\43 differ). **While the overlay is listed the game holds the overlay's copies** (4x, other bytes): a key from memory misses then; one from the original file on disk still hits. See open question 16.
  7. B1 palette: body+61 (a class dword at +57 first, layout 3). `src\chars\tex.cpp`, `zonemesh.cpp`, `chartex.py`, `bakezone.py` were right; `tools\chars.py` read +57 and is fixed. The overlay is unaffected: its only B1-to-A1 textures (5, ROM\0\104) match the +61 decode.
  8. Overlay size, final: 26,764 DATs, 13.1 GB (zones 2x: 292, models 4x: 26,168 baked; 74 palette-only left).
- **Rule check:** `smaller_levels` already runs only in `zm::load_dat` on the loader thread (`zonemesh.cpp`), so today's install was within the rule.

### M1 — Measure and harden today's memory paths (install)
- **Steps:**
  1. `src\pack\memwatch.cpp/.hpp` (§5 probes, cached adapter).
  2. `src\pack\pack_module.cpp` skeleton that only reports: VA, VRAM, chars MB, zone MB (`gpu_zone::bytes()`), per-zone low-water marks, and the bake-running flag.
  3. Register `gpu_crash::add_section(&pack::crash_section)` (lock-free, mostly empty for now).
  4. `src\chars\cache.cpp`:
     - per-batch decode;
     - real-byte batches, own batch for oversized entries, hard-cap failure;
     - `make_detail` only with detail on (setter fed from `main.cpp` l.779-780);
     - `mesh_ready` (l.877-886) leaves failed models to the game, with a log line.
  5. New `src\pack\retire.cpp` (the fence-tagged list). Chars `release_texture`/`bury` (l.153-161, l.815-843) move onto it.
  6. Move `smaller_levels` to `zm::load_dat` on the loader thread, freeing temporaries per texture. Measure the loader's peak VA in Giddeus at 4x before and after.
  7. Counters in `src\chars\cache.hpp` (l.84-92).
- **Tests:**
  - new `tests\memwatch_test.cpp`: a 256 MB reservation drops free VA by about that; the walk takes under 5 ms;
  - the gpu_crash report builder called from `engine_check` shows the section;
  - `charmodel_test`: 2,000 queued 1024² textures never hold more than one batch decoded; detail off makes no detail resources; oversize gets its own batch; over the cap it fails and the model is left to the game;
  - `charinject_test`, including forget during a frame under `HIFIXI_GBV=1` with 0 messages;
  - `zone_view` Giddeus pixel-identical; `zoneinject_test` under GBV with 0 messages; `zonenative_test`.
- **In-game check** (bake ended or logged):
  1. Port Jeuno at a busy hour with the overlay: lower chars MB, higher free VA.
  2. Zone 145↔115 five times and into Giddeus with no hitch.
  3. No missing parts.
  4. Send `hifixi.log` for the baseline.
- **Done when:** the baseline is recorded in memory notes, chars VRAM and peak decode are lower, nothing BC2-related runs on the game thread, and the suites pass.
- **Risks:** `mesh_ready` changes which models go native (tested and logged); slower pacing in crowds (the game draws meanwhile).

### M2 — Pack format, reader and converter (plugin behaviour unchanged)
- **Steps:**
  1. `docs\texture-pack\format.md` (§3).
  2. `src\pack\`:
     - `pack.hpp/.cpp`: load, header, CRC, file-size checks, `find`, `read_level` (chunked overlapped);
     - `pack_write.cpp`: roll-over, `pack.src`, idx last, share modes;
     - `bc2.cpp`: coverage mips by alpha kind;
     - `hash.cpp`;
     - `validate.cpp`: footprint and format rules.
  3. `dat_core.cpp` split; `tools\texpack\texpack.cpp` + `tools\build_texpack.cmd` with `convert`, `verify`, `stats`, `compact`, `prune` (§7).
  4. With Alex's go-ahead and the game closed, run convert: read-only on the overlay and FFXI folders, writing straight into `<bootloader>\hifixi-pack`. Rerun after the model phase, game closed.
- **Tests:**
  - new `tests\pack_test.cpp` (x86):
    - round trip; roll-over; non-64 width padding; hit and miss; wrong size or w/h misses;
    - CRC failure misses; truncated index or a v2 header refused;
    - truncated data file, swapped records and wrong pitch all miss;
    - level offsets equal `GetCopyableFootprints` on WARP;
    - dedupe; idx replace while a reader is open; rename of a closed pack folder;
  - new `tests\pack_oracle.py`;
  - convert on a hand-made overlay of `ROM_310_0.DAT`: pairing, name refusal, B1 case, palette-only rows skipped, resume, flags from the original;
  - soul test (colour and alpha);
  - coverage within 2% on cut-out only; blended/mask alpha box-filtered;
  - `zone_view` identical after the `bc2.cpp` move.
- **In-game check:** none. Alex reviews `texpack stats`.
- **Done when:** the tests pass and `verify` is clean on zones (models when that phase ends).
- **Risks:** disk (precheck, compact); DATs over the overlay's 8 MiB chunk limit are absent; collisions are negligible.

### M3 — Streamer on its own (offline only)
- **Steps:**
  1. `src\pack\stream.cpp/.hpp` (§4):
     - worker resources (placed heaps or committed), ring clamp and keep;
     - validation, shared residency map;
     - `request` / `cancel` / `pause` / `stop` with join;
     - per-owner in-flight counts; AddRef of the targets;
     - retire-list integration.
  2. Budget, VA floor and quiet-window hooks.
  3. Device-lost stop.
  4. Hang section filled from atomics.
- **Tests:**
  - new `tests\packstream_test.cpp` on WARP and hardware (`ZONE_GPU=1`):
    - 2,000 entries read back by checksum;
    - ring wrap with mixed sizes; too-big refusal;
    - cancel then destroy with no use-after-free; pause/resume; per-present cap;
    - simulated device loss; `stop()` under 100 ms during a throttled read;
    - a forced low budget skips the top level, then falls back;
    - evict-then-restream of a character key with 0 GBV messages;
    - two owners of one key share one resource;
    - crash section called while the worker holds its mutex;
    - VA growth per 1,000 BC resources measured with a pass/fail limit;
  - plugin load/unload with 1,000 requests queued;
  - GBV run with 0 messages;
  - `hook_test`: streamer lists never reach `hk_reset`/`worldfx::on_reset` (rule of 0072203);
  - 30-minute hardware stress run with flat VA, after the bake has ended.
- **Done when:** everything passes, and the streamer's VA equals ring + index + the measured per-resource cost.
- **Risks:** debug-layer locks (the GBV run is mandatory); slow disks only delay upgrades.

### M4 — Zones and model actors from the pack (first in-game win)
- **Steps:**
  1. `settings::entry::shown`; schema entry; section help; `draw_game` block; Files line. The module goes live with the snapshot on the worker and the commands.
  2. `src\bake\dat_core.cpp` `overlaid(path, exclude)`. `zonemesh.cpp/.hpp`: pack mode reads with hifi-textures excluded, no width pass, entries on hits.
  3. `src\zone\zone_gpu.cpp/.hpp`:
     - pack region, write-once slots per visit;
     - atomic flags;
     - `texture_srv` / `game_srv`;
     - `release()` and destructor cancel;
     - `bytes()` includes pack resources.
  4. `fxrender.cpp` l.784/l.824 and `zoneshadow.cpp` l.508-523 use `game_srv`.
  5. `native.cpp` and `actordata.cpp`:
     - requests queued after feed's zone-in plus `zone_delay_s`;
     - every destruction point (g.old expiry, unused eviction, `forget_all`, `forget_device`, `stop`) waits for in-flight 0 without blocking;
     - shutdown order;
     - coverage in `native::status()`.
  6. FSR bias in zone `ps_light` (pack on only).
  7. Bump `kVersion`.
- **Tests:**
  - `zonemesh_test`: indices and `model.missing` identical with the pack on and off; hifi-textures ignored while a higher-priority overlay from `HIFIXI_PIVOT_INI` is honoured; pack off byte-identical;
  - `zoneinject_test` / `zonenative_test` (WARP, hardware, GBV):
    - game textures first, then pack textures;
    - the slot-write log shows each slot written once per gpu_zone, before its flag;
    - off mid-stream gives the game picture on the next frame, and re-on waits for the next zone;
    - quiet window respected;
    - 50-switch churn with copies in flight;
    - a device reset mid-copy;
    - stop during streaming;
  - `zoneslot_test`;
  - `modelactor_test`: actordata eviction and `forget_all` mid-stream;
  - `fx_render_test`, `fx_test`;
  - `zone_view` with `ZONE_VIEW_PACK` (East Ronfaure, Giddeus, Zi'Tah ROM2\0\2.DAT unchanged) vs overlay and plain; FSR Quality vs native A/B;
  - `framegen_test`;
  - `menu_test`: hidden without a pack in the tab and in search; Off sets 0, the others leave it; a click saves and sets Custom; no file or DXGI calls during menu draw;
  - render tier.
- **In-game check:**
  1. Pack off first: everything as before.
  2. On: Port Jeuno → Rolanberry → Giddeus, coverage near 100%; Zi'Tah canopy intact.
  3. `compare` and off switch instantly.
  4. Zone 10 times with no hang.
  5. Note the city "Game memory" reading.
- **Done when:** pack off is bit-identical to the M0 baseline, Alex sees coverage, and zoning is clean.
- **Risks:** confusing the pack with the open zone-pass setup hang (test the same build with the pack off first and keep the breadcrumbs); brief 1x→2x after zone-in; memory not yet down while the overlay is listed.

### M5 — Characters from the pack; overlay unlisted (memory win)
- **Steps:**
  1. `src\chars\cache.cpp/.hpp`:
     - hash in `decode_texture`;
     - `texture_entry` (l.77-89) gains the pack key, the pair and the "upgrading" state;
     - upgrade via the shared map;
     - format and levels taken from the resource (l.181-193, l.578-589);
     - `GetCopyableFootprints` rows (l.389-398);
     - slot skip rule;
     - all frees through the retire list; `forget_all_textures()`;
     - pre-M9 detail copy;
     - FSR bias in characters;
     - status: hits/misses, pack MB, "drawn by the game itself".
  2. The module calls `forget_all_textures()` on a generation change.
  3. Sampler s0 (`chardraw.cpp` l.1465-1469) unchanged.
  4. Retire the overlay in two steps:
     - **(a)** New `install.py --unlist` with the game-closed gate built in: back up `pivot.ini` and remove only `4=hifi-textures`. The folder stays where it is: bakeall resumes from it and convert reads it.
     - **(b)** A separate later decision (Q12): after the bake has finished, `verify` is clean for zones and models and coverage equals the overlay's chunk count, move the folder to an archive under `C:\code\LSB\hifi-textures\work` (or off C:). Delete it only on Alex's explicit OK. `--undo` (which runs `rmtree`, l.66-73) is not used.
  5. Until (a): the overlay staleness warning (§6).
- **Tests:**
  - `charinject_test`: a hit gives BC2 with pack MipLevels via the second pair; a miss is unchanged; toggle under GBV with 0 messages; hair/lace coverage; shine-mask alpha unchanged; two resources with the same content give one pack resource;
  - `charmodel_test` l.656 plus a pack case;
  - `charslot_test`: exhaustion; 1,000 forgets during an upgrade with no slot handed out twice;
  - `chartest.cpp` with and without the pack; `cloth_test`; `actors_draw_test`;
  - `packstream_test` with 4x 1024² entries; churn with characters under GBV; `framegen_test`.
- **In-game check:**
  1. Overlay unlisted.
  2. Gear sharp; character coverage shown, including the game-drawn count.
  3. Lower Jeuno at a busy hour: "Game memory" below the M1/M4 readings, no missing parts.
  4. Off/on instant.
- **Done when:** the suites pass, the overlay is unlisted, the city memory is below the baseline, the bake has finished, the model convert is verify-clean, and Alex has decided the folder's fate.
- **Risks:** palette gear stays 1x; the descriptor heap (16384) is shared (skip rule, shared map); things the game draws itself are 1x (counted).

### M6 — Hardening and public release
- **Steps:**
  1. Tune budget and floors from the logs.
  2. Log lines at open and per zone.
  3. Check the safe-start pause.
  4. Release from a clean detached worktree; `tools\package.py` holds no pack content.
  5. README uninstall section.
  6. Check the Phoenix launcher's repair on a test folder beside the game.
  7. Release note: "Optional texture pack: HiFi XI can draw scenery and characters from a texture pack kept on the graphics card (the menu shows it when a pack is on the PC)."
- **Tests:** release tier; `budget_mb` simulating an 8 GB card; a no-pack run vs v0.65 goldens.
- **In-game check:** an evening with the pack on (cities, zoning, fights); one session with no folder: no menu section, same look as before.
- **Done when:** released, plus a full session with no hangs. Write memory note `hifixi-texture-pack.md`.
- **Risks:** a too-strict budget silently shows 1x (the fallback count is in the status).

### M7 — "Make texture pack" for players
- **Steps:**
  1. `texpack make` with `tools\texpack\upscale.cpp` and `keeptexels.cpp`.
  2. Per-batch game check and pause; `progress.txt`; `stop`; `--ffxi` and discovery.
  3. Licence decision written down; ship or download on consent.
  4. The module reads `progress.txt`; the menu section is shown to everyone with plain steps.
  5. `package.py`: builder exe and notices.
- **Tests:**
  - new `tests\texpack_test.cpp` (x64): keeptexels port within 1 LSB of Python on 20 textures; fake-ESRGAN pipeline; resume after a kill; dedupe; game start mid-build pauses and ends the child; no Python on PATH; `--ffxi` on a fake folder;
  - status and `menu_test`.
- **In-game check:** with the game closed, build one zone and some models; in game they come from the pack; stop and resume once.
- **Done when:** a fresh pack loads and verifies, and the licence is settled in writing.
- **Risks:** unclear weights licence (download instead); hours of work on weak GPUs (offer zones only).

### M8 — BC7 colour
- **Steps:** vendor bc7enc (licence check); format-2 colour keeping the 0..0x80 alpha; optional cached kept-PNG stage; reader and streamer accept `BC7_UNORM`; v1 packs keep working.
- **Tests:** `pack_test` v2; BC7 PSNR above BC2 on 10 textures; alpha coverage and soul tests; zone_view/chartest foliage and lace.
- **Done when:** Alex prefers v2 and the suites pass.

### M9 — Height → pack normals
- **Steps:**
  1. BC4 record (`src\pack\bc4.cpp`).
  2. Zone: a pack normal per piece; `kMapped` prefers it, `kBump` unchanged; Sobel in the shader or stored BC5 (by `shade_bench`); in the t5 pack region.
  3. Characters: the detail rg tilt (`chardraw.cpp` `detail_at` l.97-123) with a dc[2] flag. The pack pair's detail descriptor now holds it.
  4. `grass.hlsl` relief (l.2070-2096) fades on pack-normal pixels.
  5. `[textures] height=0`.
- **Tests:** `zonedraw_test` (per-pixel off byte-identical); zone_view A/B (luminance within 3%); chartest; `charinject_test`; `shade_bench` at 4K; `grass_test`; GBV churn.
- **Done when:** approved and the suites pass.
- **Risks:** painted shading mistaken for relief (game heights first); pair with M10.

### M10 — Material and gloss (own install, kill switch)
- **Steps:**
  1. BC7 material record from priors plus tags.
  2. `src\zone\surface.hpp`.
  3. `draw.cpp` root parameter 8 (t6 table, l.637-665), bound for every piece with a stand-in; `material_srv()`.
  4. `piece_spec` mask; flag bit 32.
  5. Characters: the detail descriptor plus a flag; gloss drives `sun_spec`.
  6. `zoneshadow.cpp` moves to `texture_srv` after an alpha-parity check.
  7. `material=0`.
  8. Lands only once the zone-pass breadcrumbs have been stable for a while.
  9. PSO variants built in the background at start.
- **Tests:** `zoneinject_test` (valid t6, `material=0` equals M9, GBV, churn); `zonedraw_test` breadcrumbs; `charinject_test` (unflagged byte-identical); caster coverage; `shade_bench`.
- **Done when:** approved, `material=0` equals M9, and several sessions pass without hangs.

### M11 — World FX surface target: wet stone and puddles
- **Steps:**
  1. `src\zone\surface_split.hpp` (modelled on `light_split.hpp`): R8G8B8A8, with a = the **low 8 bits** of the light split's 11-bit depth code. World FX compares it modulo 256 with the same ±2.5 tolerance and accepts the rare alias. It exists only with a material pack and `surface=1`.
  2. Every zone-pass pipeline gets the target:
     - zone `ps_light`: Target2, `WriteMask` 0 for blended pieces;
     - characters `ps_light` (`chardraw.cpp` l.307-308): Target2;
     - `ps_light_motion` (l.318-332): Target3;
     - the colour+motion variant (l.293-294): relies on the check byte.

     Both PSO variants are prebuilt in the background (`src/d3d8/compile.cpp` threads). The switch happens by flag only once a variant exists, and the pass draws without the target until then.
  3. `grass_render.cpp` `copy_out` (l.5194-5245): second extra, `zinfo.w` bit 32, `set_material_source` (`grass_render.hpp` l.729-739, `main.cpp` about l.826).
  4. `grass.hlsl`: `material_at`; the wet block (l.2150-2168) uses porosity, gloss and puddles (WET1); `rain_rings`.
  5. Fill level computed in `worldfx_core.cpp` `apply_weather` (l.1412-1452) into a new `look` field. It is written in `grass_render.cpp` to a free `.w` of the kWet block (`kWetParams` l.81, filled at l.5696-5703) and read as `actors[kWet + k].w`.
- **Tests:** `grass_test` (bit 32 off identical; characters not darkened; the check byte rejects stale texels and aliasing stays rare); `worldpass_test`; `zonedraw_test` / `charinject_test` (every pipeline records, blended writes nothing, no compile on the present path); zone_view in rain; `shade_bench` under 0.3 ms at 4K; e2e in rain.
- **Done when:** approved in two zones, pack off unchanged.
- **Risks:** about 66 MB at 4K with a pack only; the copy follows the zone guard.

### M12 — Floor reflections
- **Steps:** `reflect_floor()` reusing `water_ps`'s march (fixed start l.3121-3122, `sky_toward` fallback); a `[branch]` gate; half resolution if needed; setting `textures.reflections` (0/1/2, presets `{0,_,1,2}`, safe start forces 0).
- **Tests:** `grass_test`; `shade_bench` under 1 ms at 4K; `framegen_test`.
- **In-game check:** Chateau d'Oraguille at 2; Bastok Markets in rain at 1; no shimmer with frame generation.
- **Risks:** cost in rainy cities; polygon-flat reflections; no World FX shadows in reflections.

---

## 10. Open questions for Alex

1. **Approvals needed:**
   - running `texpack convert` (read-only on `C:\Games\PhoenixXI`, writing 10+ GB into the bootloader folder, game closed);
   - `install.py --unlist` in M5.
2. Convert the model phase as it lands (partial pack, reruns), or only at the end of the bake?
3. Pack folder: fixed `<bootloader>\hifixi-pack`, or another drive (needs an ini key)? To be decided after the launcher-repair check.
4. Zone mip floor: the game's own size (proposed), or the whole chain?
5. Off preset sets the pack off (proposed), or leaves it alone?
6. Blended zone pieces: the game's texture (proposed), or the pack's?
7. Effects in the pack: never (proposed), or an allow-list later?
8. Real-ESRGAN for players: ship (after the licence check), or download on consent?
9. Builder: standalone with the game closed (proposed), or a menu button after a trial?
10. Rain extras: a third render target (proposed, needed for reflections), or wetness in the zone shader only?
11. Height and material at full or half resolution (from M9 A/B images)?
12. The overlay folder after M5's unlist: keep, archive to `work\` or off C:, or delete (permanent, 10-15 GB). Only after the bake, a clean verify and full coverage.
13. The three overlay-owned zones (Zi'Tah canopy, ROM3\3\82, ROM3\5\1): no pack textures (proposed), or their own bake from the overlay copies?
14. Raise texture detail under FSR while the pack is on (proposed; the look changes at the same settings), or not?
15. Must M5 wait for the whole model bake (proposed in Done when), or ship with partial model coverage?
16. (from M0) While the overlay is listed, textures the game holds are the overlay's copies, so a key made from memory misses. Until M5 unlists it: key characters and model actors from the original file on disk (proposed: path + chunk ordinal, read on the loader thread), or have `texpack convert` also index each entry under the overlay copy's key (cheap: a second entry, same records)?

---

## 11. What is already done

- **dev-tex**, committed 120cb36 and merged with dev-hdr 9b04ee8 (6b9e8ab, installed 2026-11-02); `dev-pack` builds on it (M0):
  - `bake::overlaid`, `zm::texture::levels`;
  - the CPU BC2 codec in `zone_gpu.cpp`;
  - the zone sampler (anisotropic x8, bias -0.5);
  - `ZONE_VIEW_SIZE`.

  A byte-identical staged copy sits in `C:\code\LSB\hifixi-dawntex` (resolved in M0). The codec moves to `src\pack\bc2.cpp`.
- **Overlay installed:** `C:\Games\PhoenixXI\polplugins\DATs\hifi-textures`, `pivot.ini` `4=hifi-textures`, behind `0=EraDATs`, `1=XICombinedMaps-2048`, `2=ui-mods` and `3=zitah-canopy`. Giddeus is at 4x (B1 chunks turned into A1).
- **The bake, finished 2026-11-02** (`research\ffxi-re\tools\upscale\bakeall.py`): zones at 2x, model DATs at 4x; 26,764 DATs, 13.1 GB.
  - It keeps chunk order and names, but writes template headers.
  - It resumes by checking for files in the overlay folder.
  - Logs: `work\all\bakeall-*.log`. Inventory: `work\all\dats.tsv`.
- **Reusable pieces:**
  - `bake::dat_path` / `read_file` / `chunks` / `overlaid`;
  - `zm::sobel` and the bump-DAT reader;
  - `chars::tex::decode`;
  - `gpu_crash::memory_over` / `add_section`;
  - `backend::safe_start()`;
  - `passes::module`;
  - the copy-queue patterns in `native.cpp` and `cache.cpp`;
  - the d3d8 frame fence (`src/d3d8/gpu.hpp`);
  - `nmmockup.py`;
  - `install.py` (gains `--unlist` and a game-closed gate).
- **Real-ESRGAN:** in the session scratchpad (`C:\Users\d14b1\AppData\Local\Temp\claude\C--code-LSB\ba904db8-0fb8-4ce2-acf8-50425129fa42\scratchpad\esrgan-dl\x`). M0 copies it to `research\ffxi-re\tools\upscale\esrgan` right away.