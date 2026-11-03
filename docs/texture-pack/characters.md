# Characters from the texture pack; the overlay unlisted (M5 design)

M5 of `docs/texture-pack-plan.md`: gear, NPCs and monsters drawn with the pack's textures, streamed by
`src/pack/stream.cpp` (`streamer.md`) the way the zones' are (`zones.md`), and the `hifi-textures` XIPivot overlay taken
out of `pivot.ini`, so the game loads its own 1x DATs again (the memory win). This page maps each M5 step onto the code
as it is at dev-pack 32e90fe (M4, HiFi XI 0.66). The plan's line numbers are stale; the anchors here are 32e90fe's.
Where this page and the plan differ, this page wins and says why.

Read with it: `docs/d3d12/sync.md`, `descriptors.md`, `threads-pso.md`, `barriers.md`, `debugging.md`, `streamer.md`
("As built", "Notes for M4 and M5") and `zones.md` (decisions 1 and 2, "Where tickets are read"). The rules M5 adds go
onto those pages, with anchors, in the commit that writes the code (last section but one).

## Settled before this page (Alex approved the plan's proposed answers)

- **The key** is made from the bytes the game holds: `pack::key_of` over the chunk the cache was given, hashed in
  `decode_texture` before `e.raw` is freed. It hits only once the overlay is unlisted (M0: while it's listed the game
  holds the overlay's copies, other bytes). M5 unlists it: `install.py --unlist`, run by the caller at install with the
  game closed. The caller sets `[textures] pack=1` for Alex at install.
- **Q14:** the FSR bias in characters too while the pack is on: `pack::fsr_bias_on()` / `fsr_bias(ratio)`, the same
  clamp (at least -1 more).
- **Q15:** ship with the coverage the pack has (the bake is finished; the pack holds all 46,670 upscaled textures).
- **Q12** stays Alex's: the overlay folder is never moved or deleted by M5. `--undo` (it runs `rmtree`) isn't used.
- After the unlist: pack off = the game's 1x everywhere; pack on = zones (M4) and characters (M5) from the pack.
- From M4 (binding): decision 1 (a gone ticket keeps its texture until it's dropped), decision 2 (the module clears
  every owner before each close, open and stop, in the same call), the streamer started once a session, owners write
  their own views, `take_log` lines from the module, `stop()` first in Release. And M4's list for M5, every item of
  which is below.

## What was measured (2026-11-03, read only)

`...\scratchpad\m5\m5measure.cpp` (x86, linking `dat_core.cpp` and the pack's reader): every model DAT `dats.tsv` lists
(26,550), read as the game will hold it after the unlist (`bake::overlaid(path, L"hifi-textures")` with the PC's
`pivot.ini`: hifi-textures passed over, every other overlay honoured), every texture chunk keyed with `pack::key_of` and
looked up in the real pack (`C:\Games\PhoenixXI\bootloader\hifixi-pack`, 23,617 entries). `analyse.py` and `listed.py`
beside it.

- **No model DAT is served by another overlay** (`dats.tsv`'s last column is `-` for all 26,550; the tool found the
  same: 0 read from EraDATs or another). So what the game holds after the unlist is the original file's chunk, byte for
  byte (M0's check: 74/74 live textures equal their DAT bodies).
- **Every model texture chunk:** 38,041; **35,879 hit the pack (94.3%)**, none of them `no_pack`. The 2,162 misses: 1,984
  B1 palette chunks (the bake upscaled A1 only: "palette gear stays 1x", the plan's risk), 160 A1 2x1 stubs, 18 of
  format 0x81. By size the misses are mostly 64x64 (1,588) and 32x32 (225).
- **The hits' records** (18,867 unique keys): 4x for all but 5 (2x). Level 0 256x256 5,578, 512x256 5,032, 512x512
  3,091, 512x1024 3,024, 1024x1024 1,059, 128x128 849, 2048x1024 7. Levels 7 (10,617), 8 (6,189), 9 (1,066), 6 (989).
  A record is 320 KB on average (median 172 KB, p90 687 KB, max 2.7 MB); all of them 5,895 MB, 6,468 MB as committed
  textures (64 KB pieces). Alpha kinds: cut-out 12,348, mixed 5,004, none 1,279, shine mask 236.
- **A live scene:** the six captures `research\ffxi-re\data\chartex\live-*` (the bytes the game held, one player in
  gear: 31 distinct textures): **31 of 31 hit** (100%). Their records 10.9 MB (12.0 MB committed, 361 KB a texture);
  the same textures at the game's size as the cache keeps them (RGBA8 with mips) 2.7 MB, twice that with detail maps.
  The 30 DATs they come from: 49 texture chunks, 49 hit.
- **What it saves on the graphics card:** today, with the overlay listed, the cache decodes the 4x copies to RGBA8
  (4 bytes a texel, and as much again for a detail map): the last session's status had 22 character textures taking
  82.5 MB (Giddeus, `logs\hifixi-status.txt`). The pack holds them as BC2 (1 byte a texel), the cache the 1x
  originals: about 17 MB for the captured player against about 87 MB now. A crowded city's few hundred textures are
  of the order of 100 MB of pack textures.
- **While the overlay is listed nothing hits:** the overlay's copies of the captures' 30 DATs, keyed the same way: 49
  texture chunks, 0 keys in the pack (convert made no entry under an overlay copy's key: open question 16).
- **The hash costs nothing worth counting:** FNV-1a over all 38,041 bodies (479.5 MB) in 0.73 s, 653 MB/s; 12.6 KB
  and about 20 us a texture on the cache's worker, against milliseconds of decoding.

## Decisions taken here

1. **The characters' glue in its own file; cache.cpp stays stream-free** (as zones.md decision 3 kept zone_gpu.cpp).
   `cache.cpp` is linked by charmodel, charcache, charinject, enspell_view, cloth, actors_draw and the plugin. It
   gains only data and game-thread calls (the key, a ticket, a second pair, a state, counts) and includes
   `pack/pack.hpp` (header-only `key_of`) and `pack/stream.hpp` for the `ticket` type. Every `stream::` call lives in a
   new `src/chars/charpack.cpp/.hpp`, linked into the plugin (`PASSES_PACK`) and the tests that stream. The tickets the
   cache lets go are handed to a hook (`cache::set_pack_drop`, which charpack sets to `stream::drop` for each) from
   `bury`, outside the cache's mutex. No build line that links `cache.cpp` alone changes.
2. **`forget_all_textures` is `forget_pack_textures`: only the pack part goes.** M4's for-M5 list calls the characters'
   `clear_all` `cache::forget_all_textures`. Forgetting the game's textures too would leave every character to the game
   for the seconds the cache takes to copy and decode them again, at every pack toggle, reload and device change, and
   they hold nothing of the epoch being ended. So `clear_all` lets go of every texture's pack part (its ticket and its
   second pair, through the retire list) and puts it back to `none`; the game's textures and pairs stay.
3. **Characters switch at once, both ways.** Unlike a zone (a visit is read in one mode), a character's texture asks
   again in the new epoch as soon as the pack is on, reloaded or the `[textures] chars` key turned on, and is drawn
   with its game texture again in the same call that turns it off. The help says "scenery at the next area you enter".
4. **"Upgrading" doesn't stop a forget.** The plan kept an entry upgrading out of `forgettable()` and had the streamer
   "acknowledge" before the pair was retired. Since M3 the streamer writes nothing of an owner's (owners write their
   own views) and `drop` never blocks: an entry forgotten while its ticket is queued, read or copied drops it and goes;
   the worker finishes or abandons the copy and releases what only it saw behind its own fence. `forgettable()` is
   unchanged. "Cancelled" is no state an entry stays in: it's the moment the ticket is dropped.
5. **A pair is taken only when its ticket is ready**, not when it's asked: a queued ticket costs no descriptor. Taken
   under the cache's mutex from `free_slots` (only while more than `kPackSlotFloor` pairs are free), written once
   outside it (`stream::write_view` for the colour, a view of the game texture's detail map or a null view for the
   detail), and only then the state turns `ready`.
6. **The detail descriptor is a second view, never a copy.** The plan said "a copy of the game-size detail SRV". The
   cache's heap is shader visible and `CopyDescriptors` must take its sources from CPU-only heaps (descriptors.md
   "The rules", :14-15). So the pack pair's second descriptor is a new `CreateShaderResourceView` of the same detail
   resource (`e.d`), `MipLevels` from its own desc (the decoded levels are freed after the upload); with no detail map,
   or `[chars] detail` off when the texture was decoded, the null view the game pair has. `e.d` lives as long as the
   entry, and both pairs go to the retire list together, so the second view never outlives its resource.
7. **Levels: the plan's `6 + log2(scale)`, as the streamer's `down_to`.** The game's own texture has 6 levels, or 1 when
   a side is under 32 (`tex::mip_count`, tex.hpp:42-44). The glue asks `count 0` with `down_to_w/h` = the game's last
   level (`key.w >> (n - 1)`, `key.h >> (n - 1)`, n = `mip_count(key.w, key.h)`): a 4x record of a 6-level texture goes
   down to the game's smallest level, at most its stored chain (which stops at 4x4: 256x256 7 levels, 1024x1024 9); a
   texture the game draws with one level gets the pack's levels down to the game's own size and no smaller (as the
   game: none of its own mips). So `MipLevels` covers every distance the game's own covers, no further.
8. **The shadow casters keep the game's view** (as zones.md kept `game_srv` for `zoneshadow.cpp` until M10's alpha
   parity check): `record_casters` asks `texture_view_of` with the pack refused, so the sun shadow map's cut-outs are
   today's, bit for bit, and its pass reads 1x textures.
9. **The FSR bias is the pack's only.** Game textures in the cache have the game's six mips, so a bias on them would
   change their look (zones were spared this: their game textures have one level). The draw constants' `flags` gain
   bit 2, "the bound texture is the pack's", set where `texture_view_of` said so; `shade` takes a `[branch]` on it to
   `SampleBias(..., options.w)`. `options.w` is free today (chardraw.cpp:36: x per pixel, y models, z detail). The
   overlay pass, the detail map and the casters stay on `Sample`.
10. **One stream owner for all characters** ("characters"), made at the glue's first frame in use and dropped in
    `clear_all` (after `forget_pack_textures` dropped every ticket: belt and braces). Requests carry priority
    `kCharPriority` 1,500,000: after the zone's own (2,000,000 + rank) and before its second map's (1,000,000) and the
    model actors' (0); among characters in the order asked (the streamer's FIFO within a priority).
11. **The consumer slot becomes a list, and the mode a struct.** `pack::add_consumer(const consumer &)` (at most
    `kMaxConsumers` 4, a constant-initialized array; registration order is call order); `set_consumer` goes.
    `consumer::set_mode(const modes &)` with `struct modes { bool zones, actors, chars; }`: each consumer takes its own.
    The module reads `[textures] chars` (hidden, default 1) beside `zone` and `actors`.
12. **The wiring moves out of main.cpp into one tested function.** New `src/plugin/pack_wiring.cpp/.hpp`:
    `wire(camera, log, game_drawn)` registers the zones' consumer then the characters', the camera and log sources;
    `follow_texture_bias()` is main.cpp's FSR line (main.cpp:1320-1325) for both injects. main.cpp calls the two;
    packmodule_test calls the same two (M4's completeness check: "main.cpp's wiring has no offline test").
13. **The overlay staleness line is dropped** (M4 decision 11 left it to M5). It compares overlay copies with the
    ROM's size and time from `dats.tsv`, a developer file no player has; and M5's install unlists the overlay, so the
    game no longer loads it. The existing line ("The hifi-textures XIPivot overlay is still installed...") stays for
    as long as `pivot.ini` lists it, and the characters' line says why nothing hits (below).
14. **The glue only streams onto the cache's device.** The cache never follows a device change (`g.device` is set once,
    cache.cpp:975-986; stop() is the only way back). The module publishes its device (`pack::device_in_use()`, an
    atomic); the glue asks nothing while it isn't the cache's (`cache::device()`), counted and said in the status. A
    view of one device's texture is never written into another device's heap.
15. **kVersion 0.67** (main.cpp:106; 0.66 is M4, installed).

## Each M5 step, against the code now

| Plan step | Where now (32e90fe) | What M5 does |
|---|---|---|
| 1 hash in `decode_texture` | `src/chars/cache.cpp:352-380`; raw freed at :377 | before the swap: `e.pack = hifi::pack::key_of(e.raw.data(), e.raw.size())` when `ok` (the worker's own entry, outside the lock: published by the mutex that marks it decoded, :726-739). The too-big path (:574-582) and the thrown path (:674-681) leave it empty |
| 1 `texture_entry` gains key, pair, state | `cache.cpp:99-114` | `hifi::pack::key pack;` `pack_state ps = none;` `uint32_t pack_slot = kNoSlot;` `hifi::pack::stream::ticket ticket;` `uint32_t pack_epoch = 0;` `uint32_t serial;` (ids are reused: `add`, :793-810) `uint64_t pack_bytes;` `double pack_drawn, evicted_at;` |
| 1 upgrade via the shared map | the streamer's residency map (stream.cpp, M3) | nothing more: the same key and levels share one texture (two cache entries holding the same bytes, a zone and a model DAT); each entry has its own ticket and pair |
| 1 format and levels from the resource | `cache.cpp:181-193` and `:578-589` in the plan's numbering are `texture_desc` (:242-253) and the worker's views (:709-720) | the pack's colour view is `stream::write_view` (the ticket's format, BC2, and its levels: M3); the pack pair's detail view takes `MipLevels` from `e.d->GetDesc()`, as `e.detail.levels` is cleared after the upload (:767) |
| 1 `GetCopyableFootprints` rows | `upload` (:404-420, :470-471) | done: the rows come from `GetCopyableFootprints` already; pack textures are copied by the streamer, never by the cache |
| 1 slot skip rule | `free_slots` (:142), made at :314-317 (8,190 pairs) | `kPackSlotFloor` 1,024 pairs: no request, and no pair taken for a ticket ready, while `free_slots.size() <= kPackSlotFloor` (counted `skipped_slots`; a ready ticket dropped then, the entry `none` again) |
| 1 all frees through the retire list; `forget_all_textures()` | `release_texture` :211-216, `release_slot` :196-204, `bury` :173-186, `forget_texture` :826-839, the sweep :992-994, `stop` :1165-1214 | `release_texture` also hands the pack pair to `slots_dead` and the ticket to a new `tickets_dead`; `bury` gives the tickets to the drop hook (outside the lock) and the pairs to `retire::then` with the heap generation as today. New `forget_pack_textures()` (decision 2) |
| 1 pre-M9 detail copy | `make_detail` only with `[chars] detail` (`tex.cpp`; `cache.cpp:572`) | a second view of `e.d` (decision 6) |
| 1 FSR bias in characters | shader `chardraw.cpp:246-284` (`Sample` :249), draw constants :40-51, `gpu_constants` :1150-1159, `write_frame` :1662-1694, `draw_model` :1786-1810 | decision 9; `char_options::texture_bias` (chardraw.hpp:94-103), `inject::set_texture_bias` beside `set_options` (charinject.cpp:458), copied where the options are built (:543, :685) |
| 1 status | `cache::status` :1127-1144, `counts` cache.hpp:97-114 | counts: keyed, asked, ready, not in it, failed, gone, evicted, skipped (slots, device), pairs, bytes; a status line; charpack's menu line |
| 2 the module forgets on a generation change | `pack_module.cpp` `clear_all` :142-146 (one consumer) | the consumer list (decision 11): characters' `clear_all` = `forget_pack_textures` + `drop_owner`, before every close, open, reload, stop, device change and loss, in the same calls as the zones' |
| 3 sampler s0 unchanged | `chardraw.cpp:79`, the root signature :1458-1479 | unchanged (static, linear, clamped, no anisotropy) |
| 4a `install.py --unlist` | `C:\code\LSB\hifi-textures\tools\install.py` (not git) | below; copied to `install.py.before-m5` first |
| 4b archive the folder | - | not M5: Q12 is Alex's |
| 5 staleness warning | - | dropped (decision 13) |

## The upgrade state machine (src/chars/cache.cpp, the game's thread)

The pack part of a `texture_entry`. Only the game's thread changes it (the glue's frame in the module's present, the
module's update and commands, `frame_tick`, `texture()`/`mesh()` lookups at the marks, `stop`); the cache's worker
never reads or writes it, except to set the key before the entry is decoded and to put a keyed entry on `pack_pending`
when it marks it ready. Every change is made under the cache's mutex.

| State | Holds | `texture_view_of` returns | Leaves by |
|---|---|---|---|
| `none` | a key (or none: never asked) | the game pair | asked: `asked`; unkeyed: stays |
| `asked` ("upgrading") | a ticket (queued, reading, copying or ready, not yet seen) | the game pair | ready seen and a pair taken and written: `ready`; missed: `spent`; gone: `spent`; forgotten, cleared, chars off: dropped, `none` or the entry gone |
| `ready` | the ticket (ready) and the pack pair, written | the pack pair (unless compare, or the casters) | forgotten, cleared, chars off: pair and ticket let go, `none` or gone; under pressure: `evicted` |
| `spent` | nothing | the game pair | a new epoch (reload, on again): `none` (`pack_epoch` differs) |
| `evicted` | nothing | the game pair | `pressure()` false for `kEvictHoldS` 10 s: `none`, asked again |

- **cancelled** is the moment a ticket in `asked` is dropped: no pair was taken yet, nothing waits (decision 4).
- `pack_epoch` is the epoch the entry last asked in; an entry whose `pack_epoch` isn't the glue's reads as `none`
  (also after `clear_all`, which sets `none` itself).
- `ready` is stored only after the pair's two views are written; `texture_view_of` reads it under the mutex, as it
  reads `s` today. All of it on the game's thread: the mutex orders it against the worker's own writes to the entry.

**When each descriptor pair may be used again.** A pack pair is taken from `free_slots` like a game pair. It is written
exactly once, between its taking and the store of `ready`, and never again while it's the entry's. It leaves the entry
only through `release_slot` (forget, `forget_pack_textures`, eviction, a write that failed): into `slots_dead`, then
`bury`'s `retire::then` closure gives it back to `free_slots` once the game queue's fence has passed `signalled + 2`
(retire.hpp:63, :99), and only while `heap_generation` is still the one it was let go in (a pair of a heap since made
again never comes back: `make_gpu` :313, `stop` :1183-1184). So no list in flight reads a descriptor that is written
again (descriptors.md "Descriptors in flight"), by the same rule the game pairs follow since M1 (cache.cpp:16-17).

**When the texture goes.** The ticket is dropped at the same moments (the drop hook from `bury`, outside the cache's
mutex; any thread: `bury` also runs on the cache's worker, :630-635, and `stream::drop` is any thread's). The drop of a
texture's last ticket hands it to `retire::later` after the streamer's mutex is left: the same fence and the same +2 as
the pair that viewed it.

## When tickets are read, and why a view recorded while the game draws is safe

Characters are recorded at the game's character marks, on the game's thread, inside the game's frame
(threads-pso.md "The game-thread-only rule"; chardraw.cpp:1788 picks the view with `cache::texture_view_of` for each
group as it records). The glue runs in the module's present, between two frames:

```
 frame F (game thread): marks -> draw_model -> texture_view_of: the pack pair only if the entry reads ready
 present F: update (0.5 s): open/close/reload; clear_all FIRST (zones, then characters)
            frame_tick (0.5 s): the 20 s sweep: forget_texture -> pair to retire::then, ticket to the drop hook
            module present: stream::on_present; zonepack::frame; charpack::frame:
               requests (at most 64), take_done, pair taken, write_view + detail view, ready; eviction
            backend::present: retire::on_present (signal s_F)
 d3d8 Present: frame F's lists submitted (after s_F)
 frame F+1: marks record with what the entries read now
```

- **A view is recorded only while its entry holds a ready ticket.** `texture_view_of` returns the pack pair only in
  `ready`, which the glue sets only after `take_done` handed the ticket over `ready` and `write_view` accepted it
  (`write_view` refuses any other state). Nothing on the streamer's side is read at record time: no `poll`, no `get`.
- **The texture lives at least as long as the ticket is held** (zones.md decision 1): an open, close or reload done on
  the worker at a time of its own turns the ticket `gone` but leaves it its texture until the owner drops it. The only
  let-go-at-once epoch changes are stop (the module's `clear_all` drops every character ticket first, and `release_all`
  follows), a new device (retire frees the old device's objects at once when it sees the new one; the cache's own
  objects were made on the old device, and the glue streams only onto the cache's device: decision 14) and loss
  (nothing runs).
- **After the ticket is dropped, every list recorded before the drop is covered.** The drop happens on the game's thread
  (or the cache's worker, for entries the worker buried, which no list was recorded with since): frame F's lists,
  recorded before the drop in present F or during frame F, are submitted after `s_F`; the texture and the pair go at
  `signalled + 2` = `s_(F+1)`, signalled behind them. A drop during frame F's marks (`texture()` finding an address
  reused, cache.cpp:899-905) is the same: its lists of frame F are recorded "so far".
- So tickets may be read in the module's present, before `retire::on_present`, exactly as the zones' are (sync.md, the
  streamer's paragraph): the order "after `retire::on_present`" from M3's notes isn't needed, and neither is any wait.

## Requests, levels and pacing (src/chars/charpack.cpp)

- **Which:** entries on `cache::pack_pending` (ids with their serial, pushed when a keyed texture turns ready, by
  `forget_pack_textures`, and by an eviction's hold running out), still `none` in this epoch, while the module is in
  use with `chars` on, the cache's device the module's, `pressure()` false and `free_slots` above the floor.
  `cache::take_pack_asks(max, out)` hands them over under the mutex (O(new), not a walk of every entry).
- **The want:** `{ k = e.pack, kind = kColour, first = 0, count = 0, down_to_w/h` = the game's last level (decision 7),
  `priority = kCharPriority`, `epoch = E }`. `cache::pack_asked(id, serial, ticket, epoch)` stores it, or says the entry
  went meanwhile (the glue drops the ticket).
- **At most `kCharRequestsPerPresent` 64 a present**, the rest the next (a city's arrival is hundreds at most).
- **Ready:** `take_done(owner)`: for `ready`, `cache::pack_begin(id, serial, slot)` takes a pair (or refuses: the
  floor, the entry gone or no longer `asked`); outside the mutex `stream::write_view(t, pair cpu 0)` and
  `cache::write_pack_detail(slot, e.d)` (the second view, a device call, the cache's mutex not held); then
  `cache::pack_end(id, serial, ok, view.bytes)`: `ready`, or the pair to `slots_dead` and the ticket to the drop list.
  For `missed` and `gone`: `spent`, the ticket dropped, counted by `stream::reason`. The glue keeps a map ticket slot ->
  (id, serial), as zonepack's `by_slot`.
- **Pauses:** `kQuiet` holds the whole streamer from a zone change till the area settles (pack_module.cpp:397-439), so
  characters' textures wait too: pack work stays off zoning's path. Requests are made meanwhile and wait queued.
- **Compare:** `cache::set_pack_compare(bool)`, an atomic `texture_view_of` reads: the game pair while it's on. Frees
  nothing, writes nothing.

## Eviction under pressure

Plan section 5: over 95% of the video memory budget the streamer makes nothing new (`kVideoMemory`, `pressure()`), and
the least recently drawn character pack textures drop back to the game's. Zones never are evicted (plan section 4.4).

- **Drawn when:** `texture_view_of` stores `pack_drawn = g.pack_now` when it returns a pack pair (the glue sets
  `pack_now` each present: `cache::pack_tick(now)`). So "least recently drawn" is the last frame a model drew it.
- **What:** once per half second while `pressure()` reads true (it's a level: streamer.md "As built"), the `ready`
  entries with the oldest `pack_drawn`, until their view bytes reach `kEvictBytes` 64 MB (or none is left): each
  `evicted` (pair to `slots_dead`, ticket to the drop list: through the retire list like any let go). The texture's
  memory comes back only with its last ticket (a texture shared with a zone or another entry stays): the next
  `set_limits` sees what came back, and the next half second evicts more if it's still not enough.
- **Hold:** evicted entries aren't asked again until `pressure()` has read false for the hold, `kEvictHoldS` 10 s (no
  thrash between eviction and the streamer's 95% check, which compares the usage Windows reports half a second late).
  Then they go back on the asks' list a round each half second, `kEvictBytes` of them, the ones drawn last first
  (`cache::pack_unevict(max_bytes, left)`; an evicted entry keeps its `pack_bytes` for this), so a pressure that comes
  back stops them part way.
- **Backoff** (2026-11-03, the review's thrash finding): a crowd whose textures don't fit would otherwise flip between
  the pack and the game's every 10-11 s. A pressure that comes back within `kBackoffWindowS` 30 s of a round given back
  doubles the hold (20, 40, 80, up to `kEvictHoldMaxS` 160 s; logged once each time); it's 10 s again once every entry
  is back and the pressure has stayed away 30 s, and at every `clear_all`. `charpack::counts::hold_s` reads it. A middle
  ground: no usage arithmetic (the budget less the usage is half a second late and shared with the zones), only
  pacing and the backoff.
- No requests are made while `pressure()` is true (they'd only wait queued).

## The detail view (until M9)

The pack pair's two descriptors are what table 5 reads (t2, then t6 "the descriptor after it": chardraw.cpp:1458-1461):

- descriptor `2 * slot`: the pack's colour, `stream::write_view` (BC2_UNORM, the levels the ticket got);
- descriptor `2 * slot + 1`: with a detail map (`e.d != nullptr`: `[chars] detail` was on when the texture was decoded)
  a new view of `e.d`, R8G8B8A8_UNORM, `MipLevels` from `e.d->GetDesc()`; else the null view the game pair has
  (`CreateShaderResourceView(nullptr, R8G8B8A8, 1 level)`, as cache.cpp:718-719). Decision 6.

The detail map stays the game's size; `detail_at` reads it with the same `uv` and its tangent frame from the screen
derivatives (chardraw.cpp:97-123), so the 4x colour and the 1x detail line up. M9 puts the pack's own relief there.

## The FSR bias in chardraw

- **The constant:** `options.w` = `char_options::texture_bias` (0 with the pack off, or no FSR), written in
  `write_frame` (chardraw.cpp:1674-1676). `char_options` (chardraw.hpp:94-103) gains `float texture_bias`;
  `chars::inject::set_texture_bias(float)` (an atomic beside the options, charinject.cpp:458) is copied into it where the
  options are built (:543, :685).
- **The flag:** `draw_model` (chardraw.cpp:1786-1810) asks `texture_view_of(tid, h, allow_pack, &is_pack)`; `dc[2]`
  (the `flags` root constant, 1: shine) gains `2` when the bound texture is the pack's. Pass 0 only (the overlay and
  cloak passes keep `Sample`), and `record_casters` (:1900-1929) asks with `allow_pack` false (decision 8):
  `draw_model` gains that argument.
- **The shader:** `shade` (:249): `[branch] if ((flags & 2) != 0) t = colour_map.SampleBias(clamp_linear, i.uv,
  options.w); else t = colour_map.Sample(clamp_linear, i.uv);`. A root constant is uniform per draw, so pack off runs
  today's instruction. `sps` (:367-376) unchanged (casters: game views). The detail map unchanged.
- **Who sets it:** `pack_wiring::follow_texture_bias()` each present after the loaders' frames (main.cpp:1320-1325's
  line, moved): `const float bias = pack::fsr_bias_on() && upscale::world_scale(r) ? pack::fsr_bias(r) : 0;` into
  `zone::inject::set_texture_bias` and `chars::inject::set_texture_bias`.
- The character shader's source changes, so its D3DCompile cache key does: the first start after install compiles it
  on the game thread once (shadercache.hpp), as any shader change.

## The module (src/pack/pack_module.cpp/.hpp)

- **Consumers** (decision 11): `consumer g_consumers[kMaxConsumers]; uint32_t g_count;` constant-initialized;
  `add_consumer` appends (a duplicate `clear_all` pointer is ignored, so a test that wires twice registers once);
  `test::clear_consumers()` for packmodule_test's fakes. Every call site loops in registration order: `clear_all`
  (:142-146), `set_mode` (:148-154), `set_compare` (:156-161), `frame` (:467-468), `lines` (:475-476).
- **Modes:** `follow_mode` (:212-217) reads `zone`, `actors` and the new `chars` (`options::kCharsByDefault` true);
  `set_mode(modes)`. The open's log line says "zones on, model actors on, characters on".
- **New owners' reads:** `pack::device_in_use()` (the module's device while on, else nullptr; decision 14) and
  `pack::overlay_listed()` (`g.overlay_listed`, looked for at the first update, `on` and reload: the characters' line).
- **The ring failure in the menu** (M4's completeness check): `make_lines` (:472-608) with `c.failure` non-empty while
  on shows "Texture pack can't be used: <failure>. Restart the game to try again." in place of "Found: ...", and the
  status the same, so the menu no longer says "Found" with every texture failed.
- **Help text and README** (plan section 6, scenery and characters): the schema's help (schema.cpp:234-238)
  "Sharper scenery and characters, from a texture pack made on this PC from your own game files. It is kept on the
  graphics card, so the game's own memory stays small. Anything the pack doesn't have is drawn as the game's own.
  Scenery switches at the next area you enter. (/hifi textures)"; `/hifi textures on`'s reply (:715-716): "...:
  characters switch now; scenery, ships and furniture at the next area you enter"; README "Texture pack (optional)"
  (README.md:81-85) the same, and that the hifi-textures overlay, if installed, should be removed first.
- **Order at every close, open, stop, device change and loss:** unchanged (decision 2), now for both consumers, zones
  first. The device change also has the characters ask nothing until the cache's device and the module's agree.

## Status and menu lines

From `charpack::lines` (the module's update, every half second; strings only):

- in use, overlay not listed: **"Characters: 210 of 260 textures from the pack (38 not in it, 12 coming); 14 models
  drawn by the game itself."** N = entries `ready`; M = game textures on the GPU (`textures_ready`); "not in it" =
  `spent` as `not_in_pack` or `no_pack_flag`, plus textures with no key; "coming" = `asked`; "failed" and "evicted"
  added when non-zero. K = models the game drew itself per frame over the last half second (charmodel's `not_ready`,
  `cloth_refused`, `passes_refused` deltas over its frames), from a source `pack_wiring::wire` hands the glue
  (`charpack::set_game_drawn_source`), so charpack links nothing of the game's.
- overlay listed (`pack::overlay_listed()`) and nothing ready: **"Characters: none of 260 textures from the pack: the
  hifi-textures overlay is still installed, so the game holds its own bigger copies, and the pack is keyed from the
  game's original files."** (the M4 line below it says to remove it).
- `[textures] chars=0`: "Characters: the game's own textures ([textures] chars is off)."; the device differs:
  "Characters: not from the pack until the game is restarted (the graphics device changed)."; slots short: "...;
  K skipped (the characters' descriptors are nearly all in use)".
- `/hifi status` (`cache::status`, cache.cpp:1127-1144): a third line, "character data: texture pack: N ready (M MB),
  A asked, S not in it, F failed, G gone, E evicted, P skipped for want of descriptors, D for another device; P pairs
  for pack views".
- hifixi.log: the glue logs a line when it first finds the overlay listed with keys missing (once a session), each
  eviction round ("texture pack: N character textures back to the game's (video memory over 95%), M MB"), and the
  streamer's `take_log` lines as today.
- The hang report gains nothing: the streamer's lines (stream_crumbs.hpp) already say what it reads and copies; the
  characters' pairs aren't bound at a GPU-visible breadcrumb.

## While the overlay is still listed

The game holds the overlay's 4x copies, so every key the cache makes is a copy's: the streamer misses each at the
request (`not_in_pack`, a binary search under its mutex, nothing read), the entry goes `spent`, and the characters are
drawn with their game textures, which are the overlay's 4x anyway. Measured: 0 of 49 keys of the captures' DATs'
overlay copies are in the pack. The status says why (above). Nothing else changes: the overlay's memory cost stays until
the unlist. Tests that need "listed" key an overlay copy's chunk; tests that need "unlisted" give the cache the original
file's bytes (characters) or a scratch `pivot.ini` through `HIFIXI_PIVOT_INI` (zones, as zonemesh_test does).

## install.py --unlist / --relist

`C:\code\LSB\hifi-textures\tools\install.py` (105 lines, not a git repository): copied to `install.py.before-m5` in the
same folder before it's edited.

- **`--ini <path>`** (default `C:\Games\PhoenixXI\config\pivot\pivot.ini`, the module constant `PIVOT_INI` today): every
  mode reads and writes that file. Tests pass a copy; **nobody but the caller at install runs `--unlist` or `--relist`
  against the real one**.
- **`--logs <folder>`** (default `C:\Games\PhoenixXI\logs`) for the gate's log check.
- **The game-closed gate**, built into `--unlist` and `--relist` (and run before anything is read for writing): refuse
  while `phoenix-loader.exe` or `pol.exe` runs (a process snapshot: `tasklist /FO CSV /NH`, names compared without
  case), and while the newest `<logs>\*.txt` was written in the last 60 s ("the game wrote its log N s ago"). The
  process check is the same as `texpack`'s (texpack.cpp's game check). `--ignore-game` exists for tests and is refused
  when `--ini` is the real pivot.ini (resolved paths compared).
- **`--unlist`:** read the ini (`read_ini`); not listed: say so, change nothing, exit 0. Else copy the file byte for
  byte to `<ini>.<YYYYmmdd-HHMMSS>.before-unlist` beside it; remove only the line(s) whose overlay is `hifi-textures`
  (any case), leaving every other line as it was (no renumbering: N=hifi-textures is the last today, `4=`; a gap is
  what XIPivot sees after any removal, and bake::overlaid reads the lowest N first whatever the gaps); write
  `<ini>.tmp` as ASCII with CRLF (`write_ini`) and `os.replace` it; read it again and check the overlays listed are the
  old ones less hifi-textures, else put the backup back and fail. The folder is never touched; the line printed says
  where it is and that it's kept ("polplugins\DATs\hifi-textures kept (13.1 GB): Q12").
- **`--relist`:** the newest `<ini>.*.before-unlist` (or `--from <backup>`); gated; refused unless the current file is
  the backup less its hifi-textures line (byte for byte: someone changed pivot.ini since, so a copy back would undo it:
  "pivot.ini changed since the unlist; put 4=hifi-textures back by hand"); then the backup's bytes written through a
  temp file and `os.replace`, and checked.
- **`--undo`** (it runs `shutil.rmtree` on the folder, install.py:66-73) is left for the bake's own use but refuses
  unless `--delete-folder` is given too, and its message names `--unlist` (Q12: the folder's fate is Alex's).
- **`--status`** prints listed or not, the newest backup, and the gate's verdict.

## Shutdown and device order

Unchanged in shape (plan section 4 "Shutdown and device order", zones.md "The module's lifecycle"):

1. Release: the modules' `stop` first (main.cpp:1030-1036): the textures module's `clear_all` (zones, then characters:
   `forget_pack_textures`, `drop_owner`), `stream::stop` (joins, under 100 ms), memwatch.
2. Then `charmodel::stop`, `chars::clear_frames`, `cache::stop` (:1058-1059): no character ticket is left; `stop`'s
   `release_texture` would hand any to the hook, whose `stream::drop` of a ticket the stopped streamer forgot frees
   nothing (generation and `live` checks, stream.cpp:1852, :355).
3. `retire::release_all` last (:1075).

A device change: native and actordata let go of the old device's zones in their frames; the module's `device`
(:754-774) clears both consumers, then `stream::set_device`. A loss: `follow_lost` (:376-394) `device_lost`, then
`clear_all`.

## Build

- `PASSES_PACK` (build.cmd:80): `+ src\chars\charpack.cpp src\plugin\pack_wiring.cpp`.
- `PACK_CHARS` (new, beside `PACK_ZONES` :83): the streamer, the reader, pack_write, bc2 and `src\chars\charpack.cpp`,
  for charinject_test and charcache_test, which then stream (their lines gain it; enspell_view, charmodel_test,
  cloth_test and actors_draw_test link `cache.cpp` without it, unchanged: decision 1).
- packmodule_test links `%PASSES%` already (so charpack and pack_wiring), plus `/DYNAMICBASE:NO` if its join part loads
  zone 230 through native (the FFXiMain dump at its base, as zonenative_test).
- New `tests\install_unlist_test.py` in run_suites (Python, no build).

## The d3d12 pages, in the same commit

- **descriptors.md:** the characters' pack pairs (taken from `free_slots` only with a ticket ready and over
  `kPackSlotFloor`, written once before `ready`, given back behind the fence with the heap generation like the game
  pairs); the detail descriptor a second view of the detail resource, never a copy (the shader-visible heap can't be a
  copy source: the rule at :14-15 now has a user); `write_view` into the cache's heap on the game's thread with the
  cache's mutex not held.
- **sync.md:** "Where HiFi XI relies on this": the characters' pack textures held by their tickets; dropped with their
  pairs (`bury`: the hook, then `retire::then`); the glue reads its tickets in the module's present before
  `retire::on_present`, safe by decision 1 (this page's proof); eviction through the same path. The `users` list of the
  +2 rule gains the characters' tickets.
- **threads-pso.md:** charpack's calls on the game's thread with no lock of ours held (the cache's mutex is left before
  `write_view`, `write_pack_detail`, `request`, `take_done`); the drop hook from `bury` also on the cache's worker
  (`stream::drop` is any thread's; the streamer's mutex is a leaf); `texture_view_of`'s new stores under the cache's
  mutex (no device call).
- **barriers.md:** the character pass reads pack textures by promotion, no barrier (the rule unchanged, a new user).
- **debugging.md:** nothing new in the hang report (decision: the streamer's lines cover it); the characters' status
  line for a session's log.

## Test plan (every M5 test of the plan, and the new ones)

| Plan test | File | What it checks |
|---|---|---|
| charinject: a hit gives BC2 with the pack's MipLevels through the second pair | `tests/charinject_test.cpp` | the Mithra (ROM2/0/52.DAT, its original bytes: "unlisted") and a made-up pack (pack_write under `build\test\charinject`) whose records for its keys are flat colours, level 0 red and the rest blue: the game's picture first, then the pack's once ready (`charpack::frame` driven as the module does); the pair's views read back as BC2 with the levels `down_to` gives (6-level game textures: the record's chain to the game's last level; a one-level one: down to the game's size); WARP, `ZONE_GPU=1`, `HIFIXI_GBV=1`, 0 messages |
| charinject: a miss is unchanged | `tests/charinject_test.cpp` | a key left out of the pack and a `no_pack` one: those groups' pixels equal the picture with no pack, byte for byte |
| charinject: toggle under GBV with 0 messages | `tests/charinject_test.cpp` | off (`clear_all`) between a frame's recording and its run, on again, 50 times with copies held (`stream::test::gate_copies`): the picture each frame either all game or all pack per texture, no pair handed out twice, 0 messages with `HIFIXI_GBV=1` |
| charinject: hair/lace coverage | `tests/charinject_test.cpp` | a cut-out texture of the Mithra's (hair): the pixels passing the alpha test with the pack within 2% of the game's, at the near size and at a distance where level 2 is sampled |
| charinject: shine-mask alpha unchanged | `tests/charinject_test.cpp` | a shine group (flags 1): its highlight (`kSpec * saturate(2 * t.a)`) from the pack within 2 LSB of the game's on average (a mask record's plain box mips) |
| charinject: two resources with the same content give one pack resource | `tests/charinject_test.cpp` | the same chunk added under two game resources: two entries, two tickets, `stream::get_counts().resident` 1 and `shared` 1, one texture; one forgotten: the other still draws it |
| (new) the overlay still listed | `tests/charinject_test.cpp` | the Mithra's overlay copy's chunks (`polplugins\DATs\hifi-textures\ROM2\0\52.DAT`, read only; skipped without): every key missed `not_in_pack`, the picture the game textures', `charpack::lines` says the overlay is listed |
| (new) the FSR bias | `tests/charinject_test.cpp` | `chars::inject::set_texture_bias(-1)` over the red-level-0 pack: more red; the same bias on a game texture (no pack): the same picture (flag 2 not set); casters' depth map byte-identical with and without the pack |
| charmodel_test l.656 plus a pack case | `tests/charmodel_test.cpp` | the Mithra laid out as the game holds it loaded, through charmodel's copy: each texture's cache key (`cache::pack_key_of`, test accessor) equals `key_of` of the DAT chunk (the keying path from game memory to the pack's key); the existing read-back of level 0 unchanged |
| charslot_test: exhaustion; 1,000 forgets during an upgrade, no slot twice | `tests/charcache_test.cpp` | charslot_test tests the game's slot markers, not descriptors: the pairs are charcache_test's. Free pairs brought to the floor: no request, no pair taken, `skipped_slots` counted, asked again above it; 1,000 entries forgotten while asked, ready, and between `pack_begin` and `pack_end`: no pair in `free_slots` twice, none handed out before its fence, all back once the retire list is empty; a pair of a heap since made again never back |
| chartest.cpp with and without the pack | `src/chars/chartest.cpp` (the dgvprobe's frame, not a test) | unchanged: it has no module, so it draws the game textures as today; the pictures with and without the pack are charinject_test's |
| cloth_test | `tests/cloth_test.cpp` | unchanged, must pass (links cache.cpp without the streamer: decision 1) |
| actors_draw_test | `tests/actors_draw_test.cpp` | unchanged, must pass |
| packstream_test with 4x 1024² entries | `tests/packstream_test.cpp` | the real pack's 1024x1024 character records (read only; skipped without; 1,059 of them, all 4x of a 256x256 game texture): four at once with `down_to` 8x8 (the game's sixth level), 8 levels each (the stored 9 less the 4x4), read back level by level; a 64x64 record of a 16x16 game texture (one level in the game) with `down_to` 16x16: 3 levels; on WARP and `ZONE_GPU=1` |
| churn with characters under GBV | `tests/charinject_test.cpp` | (above) the 50 toggles; plus evict and stream again under `pressure()` (the budget forced low through `set_limits`) with a list in flight, 0 messages |
| framegen_test | `tests/framegen_test.cpp` | unchanged, must pass |
| (new) eviction | `tests/charcache_test.cpp` + `tests/packmodule_test.cpp` | least recently drawn first (`pack_drawn` from `texture_view_of`), 64 MB a half second, held 10 s after `pressure()` clears, then asked again; nothing asked while pressure holds |
| (new) the consumer list and the wiring (M4's open item) | `tests/packmodule_test.cpp` | two fakes: `clear_all`, `set_mode`, `set_compare`, `frame`, `lines` in registration order; the `chars` key; then `pack_wiring::wire` and `follow_texture_bias` as main.cpp calls them: the zones' and the characters' consumers registered in that order, a Mithra in the cache and the made-up pack: characters from the pack, off clears both in one call (the character pairs back to the game's at once, the zone's words spent), reload, a device change (characters wait for the cache's device), unload with copies in flight: nothing leaked; the bias reaching both injects; the menu lines from both. With the FFXiMain dump, zone 230 through native too: the zone's requests before the characters' |
| (new) the ring failure in the menu | `tests/packmodule_test.cpp` + `tests/menu_test.cpp` | the ring refused (`ring_mb` past what the test device gives, or a test hook): "Texture pack can't be used: ..." in the menu and status, no "Found" |
| (new) help text and the characters' line | `tests/menu_test.cpp` | the Game tab's help says scenery and characters; the characters' line under the setting; no file, DXGI or streamer call while drawn |
| (new) install.py --unlist / --relist | `tests/install_unlist_test.py` | copies of the real pivot.ini (read only) under `build\test\install`: unlist removes only `4=hifi-textures`, the other bytes and CRLF kept, the backup byte-identical to the old file; unlist again: nothing; relist: byte-identical to the original; relist refused after another change; the gate: refused with a fresh `.txt` in `--logs`, and with a running `pol.exe` (a copy of PING.EXE named so, `-n 30`); `--ignore-game` refused against the real path; `--undo` refused without `--delete-folder`. Skipped (said so) while the real game runs |
| (new) the key path with no pack | `tests/charcache_test.cpp` | a cache with no glue (the hook null): keys made, nothing asked, every view the game pair, `stop()` clean |
| render tier | `build.cmd`, `python -I tests\run_suites.py` | all suites (known flakes: zonedraw_test "mapped at its base", runner_test "zone 100 entered", hifps_step_test timing, packmodule_test's unload under load) |

run_suites gains `install_unlist` and, if charinject_test grows GPU checks, `charinject_gpu` (`ZONE_GPU=1`).

**In game** (the plan's M5 list): overlay unlisted (the caller's `install.py --unlist`, game closed), `[textures] pack=1`;
gear sharp, the characters' line with its "drawn by the game itself"; Lower Jeuno at a busy hour: "Game memory" below the
M1/M4 readings, no missing parts; off and on instant for characters; the first start's one-off shader compile.

## Risks

- **Palette gear stays 1x**: 1,984 B1 chunks of 38,041 (5.2%), 160 2x1 stubs and 18 of format 0x81 have no entry; drawn
  by HiFi XI from the game's 1x. The status counts them as "not in it".
- **What the game draws itself is 1x after the unlist** (models before `mesh_ready`, cloth or extra passes refused, a
  texture that couldn't be read): counted in the line. With the overlay they were 4x.
- **Pack off is now 1x everywhere** (zones too: M4 read hifi-textures out in pack mode, and the overlay is gone); the
  effects' MMB meshes and leaf shadows were 1x in pack mode already (M4's `game_srv`).
- **Characters' upgrades wait for the quiet window** after each zone change (zone-in plus `zone_delay_s`): a moment of
  1x on arrival, as for scenery.
- **The descriptor heap is shared** (16,384: 8,190 pairs, meshes, game textures and pack views): the floor keeps 1,024
  pairs for the game's own; a crowd past that shows 1x for the rest (counted).
- **Eviction can show 1x on a crowded 8 GB card**: least recently drawn first, held 10 s; logged.
- **A device change mid-session** leaves characters at 1x until a restart (the cache doesn't follow devices: decision
  14); said in the menu.
- **A forgotten drop holds video memory** (decision 1's price): the module's "kept for areas that haven't let go" note
  covers character textures too.
- **The first start compiles the character shader** once on the game thread (its cache key changes).
- **The measured scene is one player** (the only live captures: 31 textures, all hits); the whole-set figure (94.3% of
  texture chunks) is the scene-independent one. A crowd's real mix is seen in game.
- **install.py's gate** trusts process names and the logs folder: an Ashita whose logs live elsewhere passes the log
  check (the process check still holds); `--logs` names another folder.
- **pivot.ini's numbering** after the unlist: an overlay listed after hifi-textures moves down a number (pivot.dll stops
  at the first missing one; after review, 2026-11-03). None follows it today.
- **The join test needs the FFXiMain dump** for its zone part (as zonenative_test: an address clash now and then); the
  characters' part doesn't.

## As built, part A (2026-11-03: the module's consumer list and the characters' cache side)

Part A is the consumer list, the characters as its second consumer and the cache's pack part; part B the FSR bias in
the shader, eviction, `pack_wiring`, the ring failure line, the help, README and `/hifi textures on` wording, install.py,
kVersion and the join test. Where part A differs from the design above:

- **No ticket map in charpack.** The cache keeps `pack_by_ticket` (a held ticket's slot -> its texture) under its own
  mutex, erased whenever it lets a ticket go (`release_pack`), so a ticket `take_done` hands over is looked up there and
  one the cache let go meanwhile is no one's (`pack_holder`): nothing in the glue goes stale when the cache forgets a
  texture on its own (the 20 s sweep, an address reused). `pack_begin`, `pack_end` and `pack_spent` take the ticket.
- **A "writing" state** between `pack_begin` (pair taken) and `pack_end` (ready), so a texture forgotten while its views
  are written is let go like any other and `pack_end` then does nothing.
- **Asks only while wanted.** The cache puts a texture on the asks' list only while `set_pack_on(true)` (charpack, each
  frame in use on the cache's device; off in `clear_all` and with `[textures] chars=0`), so a plugin with the pack off
  or the hidden key off keeps no list; `set_pack_on(true)` after a stop or a clear walks the textures once.
- **The glue's outside reads are sources** (`charpack::set_sources`: the device in use, `overlay_listed`, the game-drawn
  count, the log), not calls into pack_module.cpp, so charinject_test and charcache_test link the glue without the
  module. main.cpp sets the module's `device_in_use` and `overlay_listed` and `backend::log` for now and adds the zones'
  consumer, then the characters' (part B moves this into `pack_wiring`). The "K models drawn by the game itself" part
  of the line appears once a `game_drawn` source is set (part B).
- **`texture_view_of(id, out, allow_pack, is_pack)`** with defaults (`true`, nullptr), so charmodel's overlay check and
  the tests are unchanged; `draw_model` gains `allow_pack` (false from `record_casters`). The shader's `flags` bit and
  the bias are part B: the pack's texture is drawn with `Sample` until then.
- **`set_mode(const modes &)`**: zonepack's own `set_mode(zones, actors)` stays (its tests call it); its consumer adapts.
- **The status line is in** (`cache::status`'s third line), with "tickets let go" in place of "evicted".
- **The tests:** charcache_test (keys with no glue; 64 hits through their own pairs, BC2 down to the game's last level at
  most the chain; compare; two resources sharing one texture; `clear_all`; exhaustion at the floor, asks held and ready
  tickets let go; 1,089 textures let go while asked, between `pack_begin` and `pack_end`, and ready: no pair twice, none
  free before the fence; `stop()` drops every ticket), charinject_test (as in the table, but the two-resources check is
  charcache_test's; the hair/lace and shine checks are coverage within 2% near and four times as far, and brightness
  within 2, over the whole Mithra, no shine group singled out), charmodel_test (the keys),
  packstream_test (the real pack's 1024x1024 character records, 8 levels, and a 16x16 game texture's 64x64 record, 3),
  packmodule_test (a second fake consumer: order, the chars key, the device published).
- **Measured** (WARP): the Mithra's 6 textures all in the real pack from its original bytes, none from the overlay's
  copies; the flat pack's hits 7-8 levels (the chain stops at 4x4: a 64x64 game texture's 256x256 record has 7 of the 8
  that would reach the game's 2x2); the cut-outs' coverage -0.27% near, -0.24% four times as far; brightness 62.99
  against 62.78.

## As built, part B (2026-11-03: eviction, the FSR bias, the wiring, the lines, install.py)

Where part B differs from the design above, or adds to it:

- **Eviction** is `cache::pack_evict(max_bytes, bytes)` (the ready textures by `pack_drawn`, oldest first, each let go
  through `release_pack` and marked `evicted`) and `cache::pack_unevict()` (all of them `none` and asked again), called
  from `charpack`'s `follow_pressure` each frame: a round each `kEvictEveryS` 0.5 s while `pressure()` holds, up to
  `kEvictBytes` 64 MB, logged; asked again once the pressure has been gone `kEvictHoldS` 10 s. `clear_all` ends a hold
  with the rest (the evicted textures are `none` again in the next epoch). The line says "N back to the game's while the
  graphics card is nearly full"; the status counts evicted now and in all. `charpack::test::set_evict_bytes` lets the
  tests evict a few at a time.
- **The FSR bias** as designed (flag 2 in pass 0 only, `SampleBias` behind a `[branch]`, `options.w`). The shader's
  source is split into two raw strings (`)" R"(` after `lit`): MSVC's limit on one string literal.
- **`pack_wiring`** (`src/plugin/pack_wiring.cpp`): `wire(camera, log, game_drawn)`, `follow_texture_bias()` (also
  counts presents) and `game_drawn_models()` (charmodel's `not_ready + cloth_refused + passes_refused`, per present
  since its last call, rounded up). main.cpp calls the three. The join test is its own `tests/packjoin_test.cpp`, not
  a part of packmodule_test: the module's consumer list is process-wide, and packmodule_test fills it with fakes.
  It doesn't load zone 230 through native (no FFXiMain dump needed): the zones' consumer is registered and called in
  its turn, and zonenative_test and packmodule_test cover its zone work. Its device change goes to another adapter's
  device (the 4070 Ti while the rest is on WARP): D3D12 makes one device per adapter, so a second WARP device is the
  same object and would never read as "another device"; without a second adapter that part is skipped.
- **The ring failure** is `stream::counts::failure` while on; tests force it with `stream::test::fail_parts(why)` (the
  worker's GPU parts refused from their next making, a start or a device change).
- **`pack::test::consumers()`** hands the tests the consumer list as added (packjoin_test checks wire's order).
- **install.py**: as designed, plus `--from` for `--relist`, `--delete-folder` for `--undo`, and `--status` printing the
  folder and the overlay's files in place only when a bake's file list is there. `tests/install_unlist_test.py` runs it
  on copies under `build\test\install` (their `root_path` a scratch folder); with the game running it uses
  `--ignore-game` on the copies and checks the log half of the gate through `log_recent` directly.
- **The tests:** charcache_test (eviction: the three drawn longest ago first, the next three half a second on, nothing
  asked meanwhile, the line; held 9 s after the pressure, asked again after 10 s, all ready), charinject_test (the bias:
  more of level 0 from 0.3 to 0.75 of the test's distance, where the 4x textures cross from level 0 to 1, the game's
  textures and the casters' map unchanged with it; every texture evicted between a frame's recording and its run: that
  frame as recorded, the next the game's, back after the hold; 0 messages under GPU-based validation), packjoin_test
  (26 checks: eviction through the module's present too, held 10 s, back from the pack), menu_test (the help names scenery and characters), install_unlist_test.py (11 checks).

## After review (2026-11-03)

Eight findings on the uncommitted change; each fixed with a check that fails without it:

- **Asks over the spare pairs:** `take_pack_asks` handed out up to 64 whenever more than the floor was free, so near
  the floor most ready tickets were read, uploaded and let go for want of a pair, then asked again. It now hands out at
  most the pairs over `kPackSlotFloor` less the textures already `asked` or `writing` (a walk of the entries, only while
  asks wait). charcache_test: five pairs over the floor, five asked, none let go, none asked after.
- **Thrash after the hold:** see "Eviction under pressure": the evicted come back `kEvictBytes` a half second, drawn
  last first, and a pressure back within 30 s doubles the hold up to 160 s. charcache_test: the first round after the
  hold the three drawn last, the rest half a second on; the pressure back 1 s later: held 20 s; 10 s again after 31 s
  away. The middle ground between "give everything back at once" and a byte budget from the usage (half a second late,
  shared with the zones): pacing and the backoff, no usage arithmetic.
- **Least recently drawn, tested:** the test drew in the order the textures turned ready; it now draws the first three
  again last, and the first round must take the next three.
- **No asks under the pressure, tested:** a texture in the pack added while the pressure holds stays `none` with
  nothing asked, and is asked in the first present after it's gone.
- **The casters keep the game's views, tested:** `char_stats::pack_draws` (draws with a pack view bound) and
  `inject::counts::caster_pack_draws`; charinject_test checks the casters' frame drew none (the map alone can't tell:
  the flat records' alpha passes the cut-out test wherever the game's does).
- **`[textures] chars` off with the pack on**, tested with the real consumer in packjoin_test.
- **install.py `--unlist` closes the gap:** pivot.dll reads `[overlays]` 0, 1, 2 ... and stops at the first missing
  number (its config load, 0x10006ee7), so an overlay listed after hifi-textures would silently stop loading. It now
  moves those down one; `--relist`'s check applies the same, so it still puts the backup back byte for byte; `--status`
  names a gap; `--undo` uses the same removal. install_unlist_test.py: `5=later-mod` becomes `4=later-mod`, back at 5.
- **sync.md's anchors** for the characters' queue and fence pointed into `settle()`: now `stop()`'s `later` calls, and
  every cache.cpp anchor on the d3d12 pages re-checked.
