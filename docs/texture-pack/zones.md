# Zones and model actors from the texture pack (M4 design)

M4 of `docs/texture-pack-plan.md`: the zone, its second map and the model actors (ships, furniture, plants) drawn with
the pack's textures, streamed by `src/pack/stream.cpp` (`docs/texture-pack/streamer.md`). This page maps each M4 step
onto the code as it is at dev-pack 7e5879d (after the re-architecture: no D3D12 hooks, everything at our d3d8.dll's
marks and the plugin's present; `src/host/retire.hpp` the one fence retire list). The plan's line numbers are stale;
the anchors here are 7e5879d's. Where this page and the plan differ, this page wins and says why.

Read with it: `docs/d3d12/sync.md`, `descriptors.md`, `threads-pso.md`, `barriers.md`, `debugging.md`, and
`streamer.md` ("As built", "Notes for M4"). The rules M4 adds go onto those pages, with anchors, in the commit that
writes the code (last section).

## Settled before this page (Alex approved the plan's proposed answers)

- **Q16, the key:** from the ORIGINAL file on disk. In pack mode `zm::load_dat` reads `bake::overlaid(path,
  L"hifi-textures")`: the pivot overlays in order, hifi-textures left out, so EraDATs and zitah-canopy still apply,
  and a texture's key is `pack::key_of(body, size)` of the chunk read (M0's span).
- **Q4, zone levels:** the pack's levels from the top down to the game's own size.
- **Q5:** the Off preset sets `[textures] pack=0`; the other presets leave it.
- **Q6:** a blended piece whose entry is `no_pack` draws the game's texture (the streamer misses `no_pack` at the
  request: nothing to add).
- **Q13:** zones served by another overlay get no pack textures (below: keys are made only for a DAT read from the
  game's own file).
- **Q14:** under FSR, while the pack is on, the zone's colour map is sampled with `log2(render_w / display_w)` more
  detail, the total (with the fixed -0.5 of the zone sampler) no lower than -1.5; pack off: no extra bias.
- The hifi-textures overlay **stays listed** in M4 (M5 unlists it). With the pack off, everything is bit-identical to
  today: zones load as now (the overlay's 2x and their `smaller_levels`), the shader takes the old path.
- From M3 (binding): the streamer is started once per session and kept (its 32 MB ring's address space isn't given
  back on `stop()`); "off" is the pause bit `kOff` plus `close()`; owners write their own views (`write_view`); an owner
  may go right after `drop_owner` (the plan's "wait for in-flight 0" is obsolete); `take_log` lines go to hifixi.log from
  the module's `update()`; `stop()` runs first in Release.

## Decisions taken here

1. **Gone tickets keep their texture until dropped** (the critic's open point). An open, close or reload (the
   epoch changes made on the worker at a time of its own) no longer lets go of resident textures that ready tickets
   hold: the texture leaves the residency map (a new request streams it again), the tickets read `gone`, and each keeps
   its reference until the owner drops it; the last drop hands the texture to `retire::later` as any drop does. So
   `view::resource` is valid exactly while the ticket is held, whatever the timing, and the "read tickets after
   `retire::on_present`" rule stops being load-bearing. Stop, a device change and device loss still let go at once
   (stop ends everything and `release_all` follows; retire frees a device's objects at once when it sees the next
   device, and native has destroyed that device's zones already; on a lost device nothing runs). Why not only the
   proof: M4 alone could keep the rule (below, every open/close/reload is preceded in the same call by dropping every
   zone owner), but M5 adds characters that poll while recording, and a forgotten drop must cost video memory (shown
   in the status), never a use-after-free.
2. **The module drops before it closes.** Every `close`, `open` (on, reload) and `stop` is preceded, in the same
   call on the game's thread, by `zonepack::clear_all()`: every gpu_zone's flags cleared, every pack owner dropped,
   every slot of the visit spent. With decision 1 this is belt and braces; without it, it is the proof (see "Texture
   lifetime").
3. **Zone glue in its own file, zone_gpu stays stream-free.** `zone_gpu.cpp` is linked by eight builds (zone_view,
   zonenative, zonedraw, modelactor, zoneinject, fx_render, gamewater, the plugin). It gains only data: the pack region,
   per-texture state words, `texture_srv`/`game_srv`, `compare`, a drop hook. All `stream::` calls live in the new
   `src/zone/zonepack.cpp/.hpp`, linked into the plugin (`PASSES_PACK`) and the tests that stream.
4. **Requests carry the game's size, the streamer counts the levels.** A new `want::down_to_w/down_to_h`: with
   `count == 0` and these set, the streamer takes the levels from `first` down to the first whose sides are at most the
   game's (computed under its lock in `request`, where it has the record). The game's thread never looks entries up.
5. **Pack mode is chosen at load.** A zone (or actor DAT) is read in pack mode when its load starts while the pack is
   on with a snapshot open; that mode stays with its gpu_zone. Turning the pack on takes effect at the next zone;
   turning it off shows that zone's 1x originals until the next zone (the help says "at the next area you enter").
   Model-actor DATs of the other mode, and those whose visit a reload or an off and on ended (`done`), are let go at
   the next zone change and read again as they're seen.
6. **Keys only for the game's own file** (Q13 made exact): in pack mode a DAT whose `overlaid(...)` path isn't the
   game's own gets no keys, so the three overlay-owned zones (ROM2\0\2 Zi'Tah canopy, ROM3\3\82, ROM3\5\1) and any DAT
   EraDATs serves never request, even where an overlay's chunk happens to equal the original's bytes.
7. **The FSR bias follows FSR 1 too.** AMD's FSR 1 guide gives the same negative mip bias per mode as FSR 2. The
   module reads the last drawn input/output widths from upscale (any path) and sets the bias while the pack is on;
   hidden key `fsr_bias=0` turns it off.
8. **A hidden `actors=1` key** beside `zone=1`: model actors are a consumer of their own (the plan's §2 "each consumer
   separately").
9. **Safe start leaves the pack off for the session** (not merely paused: a zone loaded in pack mode with streaming
   paused would stay at 1x). `/hifi textures on` overrides it for the session. `kSafeStart` stays unused in M4.
10. **kVersion 0.66** (main and dev-hdr are 0.65; if dev-hdr releases 0.66 first, the merge takes the next number).
11. **Dropped from M4:** the overlay staleness line (needs `dats.tsv`, a developer file: M5 with the unlist), the
    characters' status line (M5), "wait for in-flight 0" at destruction points (obsolete since M3).

## Each M4 step, against the code now

| Plan step | Where now | What M4 does |
|---|---|---|
| 1 `settings::entry::shown`, schema entry, section help | `src/settings/schema.hpp:29-43`, `schema.cpp:224-228` (chars.detail, the last Game entry), kHelp `schema.cpp:299` | `bool (*shown)() = nullptr;` after `fallback`; `textures.pack` after `chars.detail` (so its section is the Game tab's last and the hand-drawn lines follow it); section "Texture pack" help line; new `src/pack/packoptions.hpp` |
| 1 menu block, Files line | `src/ui/menu.cpp` `draw_tab_settings` :317, `draw_search` :357, `draw_game` :527, `draw_advanced` :564 | both lists skip `shown() == false`; `draw_game` draws `pack::menu_lines()` right after the tab's settings; Files gains "Texture pack: <folder>" from a cached string |
| 1 module live | `src/pack/pack_module.cpp` (M1: measures only), registered at :134 | the lifecycle below, commands, status, log; new `src/pack/pack_module.hpp` for the menu and the settings' `shown` |
| 2 `overlaid(path, exclude)` | `src/bake/dat_core.cpp:77`, `dat.hpp:25-28` | `std::wstring overlaid(const std::wstring &path, const wchar_t *exclude = nullptr)`: an overlay named `exclude` (case-insensitive) is skipped; callers unchanged |
| 2 zonemesh pack mode | `zm::load_dat` `src/bake/zonemesh.cpp:241`, width pass :255-265, texture block :402-427 | `load(map, z, r, bool pack = false)`, `load_file(file, z, r, bool pack = false)`; pack: exclude hifi-textures, no width pass (levels 1, no `smaller_levels`), `t.key = pack::key_of(im.body, im.size)` when the path is the game's own; `zm::zone::pack` says so |
| 3 pack region, write-once slots, flags | `gpu_zone::upload` `src/zone/zone_gpu.cpp:128`, heap :241-256, `texture_srv` :326 | region of `textures.size()` descriptors after white and flat, only for a pack-mode zone; per-texture state words; below |
| 3 `texture_srv` / `game_srv` | `zone_gpu.hpp:53` | `texture_srv(i)`: the pack slot when its word says drawn and `compare` is off; `game_srv(i)`: today's slot |
| 3 `release()` and destructor cancel | `zone_gpu.cpp:91-126` | a drop hook (`zonepack` sets it): `drop_owner` once, idempotent; no wait |
| 3 `bytes()` with pack | `zone_gpu.hpp:56` | `_bytes + _pack_bytes` (each ready ticket's `view::bytes`, a texture shared by two owners counted in each) |
| 4 game_srv users | `src/fx/fxrender.cpp:782`, `:822`; `src/zone/zoneshadow.cpp:508`, `:523` | `texture_srv` -> `game_srv` (fx samples level 0 only; shadow cut-outs keep the game's alpha until M10's parity check) |
| 5 requests after zone-in + delay | `src/game/feed.cpp:232`, `:1041-1056` | the streamer's `kQuiet` from a zone change until feed's "in" has held `zone_delay_s` and the old zone is retired or VRAM has headroom; requests are queued as soon as a zone is ready (they wait paused) |
| 5 destruction points wait for in-flight 0 | `native.cpp:58-75`, `actordata.cpp:67-123` | **obsolete**: `drop_owner` at each hand-over (`pack_let_go()` before `retire::later`), the hook at destruction |
| 5 shutdown order | `src/plugin/main.cpp:1016-1066` | module `stop()` (first, :1021-1026) = `clear_all` + `stream::stop` + memwatch; native and actordata are never stopped in Release (their data stays, :1034, :1040), `cache::stop` :1050, `retire::release_all` :1066 |
| 5 coverage in `native::status()` | `native.cpp:193` | "pack: 812 of 840 textures, 28 not in it, 0 failed, 3 waiting" per slot, from the gpu_zone's words |
| 6 FSR bias | zone shader `src/zone/draw.cpp:45-50`, `shade` :224, constants :336-339, :824-826; `inject.cpp:174-175` | `float4 tex_bias` after `envs`; `shade` takes a `[branch]` on it; `draw_options::texture_bias`; `inject::set_texture_bias`; `upscale::world_scale` |
| 7 kVersion | `main.cpp:102` | 0.66; help line :1217 and header comment :43 |

## The module's lifecycle (src/pack/pack_module.cpp, the game's thread)

The plugin calls each module's `update` every 0.5 s (`main.cpp:1291-1293`, after the `follow_*` calls), then each
present `native::frame` (:1305), `native_gate`, `actordata::frame` (:1309), the modules' `device` on a change
(:1312-1317), the modules' `present` (:1319-1321), and last `backend::present()` (:1330), whose
`retire::on_present` (`backend.cpp:766`) signals the retire fence. Our d3d8.dll submits the frame's lists after that.

States: `off`, `opening`, `on`, `failed`, `lost`.

- **want** = `option_on("textures", "pack", kPackByDefault)` and the folder's `pack.idx` exists (cached) and (not
  `backend::safe_start()` or the session's `/hifi textures on`) and a device and not `gpu_crash::lost()`.
- **off -> opening** (update): `stream::start(device, ring_mb)` if it hasn't run this session (once: ring_mb read
  then, clamped 8-64), `resume(kOff)`, `stream::open(folder)`; remember the number it returns.
- **opening -> on** (update): `stream::posts_done()` has reached that number and `stream::current()` is non-null. Not
  merely the epoch raised: an open still loading when a newer one is asked (reload, off and on) is done too, and
  raises the epoch first (found in review; packmodule_test holds an open on the worker to show it). Then
  `E = stream::epoch()`, `native::set_pack_mode(zone_key)`, `actordata::set_pack_mode(actors_key)`, the found line, a
  log line. `opening -> failed`: `open_status()` non-empty ("Texture pack can't be used: <why>"), `pause(kOff)`,
  loaders not in pack mode.
- **on -> off** (update, want false; `/hifi textures off`): in this order, in one call: `zonepack::clear_all()`,
  loaders' pack mode off, `stream::close()`, `pause(kOff)`, FSR bias 0. The streamer keeps running (its ring kept).
- **reload** (`/hifi textures reload`, on or failed): `clear_all()`, `folder_exists` checked again, `stream::open(folder)`
  -> opening. The zone in view stays on its game textures (its slots spent) until the next zone.
- **compare** (`/hifi textures compare`): `gpu_zone::set_compare(!compare)`. Frees nothing, writes no slot; a status
  line says it's on. Not saved.
- **device change** (`device(d)` with the streamer started): native and actordata have already destroyed the old
  device's gpu_zones in their `frame` (`native.cpp:157-160`, `actordata.cpp:101-102`; each destructor's hook dropped its
  owner). Then `clear_all()` (whatever's left), `stream::set_device(d)` (raises the epoch at once, under its lock),
  `E = stream::epoch()`. `device(nullptr)` (Release) does nothing here: `stop` handles it.
- **device lost** (present, `gpu_crash::lost()` first seen): `stream::device_lost()`, `clear_all()`, state lost: no
  requests, a status line. Nothing records any more (`backend.cpp:763`).
- **safe start**: want is false (decision 9); the status says "Paused after a graphics-card stop (/hifi textures on
  to use it this session)".
- **stop** (Release, first of the module loop): `clear_all()`, `stream::stop()` (joins, under 100 ms, resident textures
  to retire, which `release_all` then leaves: they were handed over after the last present), `memwatch::stop()`.
  Loaded again (the DLL is pinned), the next enable starts the streamer again: one more ring's address space, once
  (streamer.md "As built").
- **every update while started**: `set_limits` (video memory from `gpu_crash::video_memory`, `budget_mb`,
  `memwatch::read()`'s free address space and largest block, `per_present_mb`), the quiet bit, `take_log` into
  hifixi.log, the FSR bias, the status and menu strings, `folder_exists` on reload and at the first update only.
- **every present while started**: `stream::on_present()`, then `zonepack::frame(...)` with the zones to serve.

## Where tickets are read, against retire::on_present

```
 present F (the plugin's, game thread)                         frame F+1 (game thread)
 update (0.5 s): open/close/reload; clear_all FIRST
 native::frame, actordata::frame   (hand-overs: pack_let_go, retire::later)
 module present: stream::on_present; zonepack::frame
    requests (at most 128), take_done, write_view, word -> drawn
 backend::present: retire::on_present   (signal s_F)
 d3d8 Present: frame F's lists submitted (after s_F)           marks record: texture_srv reads the words
```

- Tickets are read (`take_done`) in the module's present, **before** that present's `retire::on_present`. With
  decision 1 that's safe: a ready ticket's texture lives until the owner drops it, and a drop hands it to retire at
  `signalled + 2`, past every list recorded before the drop.
- Without decision 1 it would also hold for M4, by construction: the only epoch changes are the module's own
  (open/close/reload/stop: preceded by `clear_all` in the same call, so no zone owner holds a ticket then), a device
  change (the old device's zones are destroyed by native first, and retire frees the old device's objects at once in
  any case), and loss (nothing runs). A view written in present F of a ticket read ready there is recorded from frame
  F+1; nothing can turn it gone before the owner's own drop.
- A view is written only for a ticket `take_done` hands over as `ready`; `write_view` refuses any other state.

## Zone loading in pack mode (src/bake/zonemesh.cpp, src/bake/dat_core.cpp)

- `native::set_pack_mode(bool)` / `actordata::set_pack_mode(bool)`: an atomic read when a load starts
  (`native.cpp:141`, `actordata.cpp:136`) and passed to `zm::load` / `zm::load_file`. The module sets it true only in
  state on and with `[textures] zone=1` / `actors=1`.
- `load_dat(..., pack)`: `path = overlaid(own, pack ? L"hifi-textures" : nullptr)`. Pack mode never runs the width
  pass (`own_width` stays empty: levels 1, no `bc2::smaller_levels`), so it reads one DAT and holds no mips: less
  address space than today's overlay load (M1's baseline: +33 MB peak for own textures against +120 MB with the 4x
  overlay, zone_view 145).
- Keys: `texture::key` (a `pack::key`, header-only from `src/pack/pack.hpp`, which zonemesh already reaches through
  `bc2.hpp`'s folder) is set at the texture block, from the `im` found (by name, or the last-8 fallback
  `zonemesh.cpp:385-392`), `key_of(im.body, im.size)`: `im.size` is `chunks`' body size, the span M0 checked. Only
  when `pack && path == own` (decision 6). `zone::pack = pack && path == own`.
- Pack off: `load(map, z, r)` is today's code path, byte for byte (the default argument), no hashing.
- Cost: FNV-1a over each texture's body on the loader thread (a zone's 1x textures: a few MB to ~20 MB, ~1 GB/s):
  zonemesh_test prints it per map.

## The gpu_zone's pack region and the write-once slot rule (src/zone/zone_gpu.cpp)

- **Layout** (the heap, `zone_gpu.cpp:239-256`): textures, normal maps, white, flat, then for a pack-mode zone
  (`_z.pack`) a pack region of `textures.size()` slots. `NumDescriptors = images.size() + (pack ? textures : 0)`. A
  pack-off zone's heap is exactly today's.
- **Initial views:** at upload each pack slot gets the game texture's own SRV (the same `CreateShaderResourceView` as
  its game slot), so even a slot bound by mistake reads a valid texture. No list can have read a pack slot yet: the
  heap is new.
- **State word per texture** (`std::unique_ptr<std::atomic<uint8_t>[]>`): `none` (no key, or not asked), `asked`,
  `drawn` (its pack slot holds the pack view: `texture_srv` returns it), `spent` (missed, gone, cleared, or the pack
  turned off in this visit: never asked again). `drawn` is stored with `memory_order_release` after `write_view`
  returned; `texture_srv` loads it with acquire. All of it happens on the game's thread today; the atomics keep it
  right if a mark ever moves thread.
- **Write-once:** a pack slot is written by `write_view` at most once per gpu_zone, before its word becomes `drawn`,
  and never again in that gpu_zone's life (off, then on: spent; reload: spent; a ticket gone: spent). So no
  descriptor a list in flight may read is ever overwritten (descriptors.md "Descriptors in flight"): until `drawn` no
  list references the slot (`texture_srv` returns the game slot), after it nothing writes it. A new visit is a new
  gpu_zone with a new heap.
- **Tickets and owner** live in the gpu_zone as plain data (`stream::ticket` is two `uint32_t`; stream.hpp is included
  for the types, nothing of stream.cpp is called from zone_gpu.cpp): `_pack_owner`, one ticket per texture, the epoch
  asked in, `_pack_bytes`.
- **Drop hook:** `static void gpu_zone::set_pack_drop(void (*fn)(gpu_zone &))`; `release()` (also from the
  destructor and at upload's start) calls it when `_pack_owner != 0`. `pack_let_go()` = the same call, made early by
  native and actordata just before they hand a gpu_zone to `retire::later`, so its pack textures and the gpu_zone are
  freed at the same fence. zonepack's hook: `stream::drop_owner(owner)`, every word to spent, `_pack_owner = 0`, the
  gpu_zone out of zonepack's registry. A hook run inside `retire::on_present`'s free (a destructor) is fine:
  retire's mutex isn't held there, and `drop_owner` hands its texture to `retire::later` after leaving the streamer's
  mutex (sync.md: `then` closures may take a lock of their own).
- **compare:** `static void set_compare(bool)`, an atomic read by `texture_srv`.
- **Test log:** `static void set_test_log(void (*fn)(const gpu_zone &, uint32_t texture, char what))`, called at each
  pack slot write ('w') and each word change ('a' asked, 'd' drawn, 's' spent), nullptr in the plugin.

## The zone's levels

A zone texture's pack record (`kColour`) starts at the overlay's size (2x; Giddeus 4x) and is chained down to 4x4
(`bc2::chain_levels`). The game's own size is the key's `w x h` (the original's, `body+21`/`+25`), which in pack mode is
also `zm::texture::w/h`.

- `want { k = t.key, kind = kColour, first = 0, count = 0, down_to_w = key.w, down_to_h = key.h, epoch = E }`.
- In `request`, with the record `r` in hand: `count = 1 + l` for the smallest `l` with `(r.w >> l) <= key.w` and
  `(r.h >> l) <= key.h`, at most `r.levels - first`. 2x: 2 levels, 4x: 3; a record no bigger than the game's: 1.
- So the SRV's `MipLevels` stops at the game's size, as today's overlay textures do (`zm::texture::levels`): sharper
  close up, unchanged where the game's own was seen. Over 85% of the video memory budget the streamer drops the top
  level itself (`first + 1` when that level's sides are multiples of 4).

## Requests: order, pacing and the quiet window (src/zone/zonepack.cpp)

- **Which:** a gpu_zone that is ready (`native::ready_zone(0/1)`, `actordata::ready_files()` new), `pack` set, not
  yet asked in epoch `E`, while the module is on. A texture with an empty key stays `none`.
- **Order:** each texture's uses = the triangles of the pieces naming it times its model's placements (at least 1;
  actor DATs have none); its distance = the nearest such placement's position to the camera of the last zone pass
  (new `game::zonedraw::camera(float[3])`, from `f.camera`, `zonedraw.cpp:251`). Sorted by `floor(log2(1 + uses))`
  descending, then distance ascending. Priority = base + (n - rank): the zone 2,000,000, its second map 1,000,000,
  actor DATs 0. So the most used textures near the player come first, and the streamer keeps that order across owners.
- **Pacing on the game's thread:** at most 128 `request` calls a present (each a binary search and a footprint under
  the streamer's mutex, ~2-5 us), carried over to the next present; `take_done` only for owners with tickets
  outstanding; `write_view` for every ready one (~1-3 us each; the streamer starts at most 64 MB of records a present).
- **The quiet window (`kQuiet`):** set from the present the zone changes (`feed::zone()` differs from the last seen,
  or new `feed::in()` false) until all of:
  - `feed::in()` (the zone-in condition: the zone-in packet, or `kZoneInWait` 3 s, `feed.cpp:232`, :1051) has held for
    `zone_delay_s` (default 2);
  - and the old zone is retired: native's slot 0 has shown its new gpu_zone ready for 3 presents (native hands the old
    one to retire just before the new upload, `native.cpp:108`, so its +2 has passed), or no zone is wanted, or video
    memory is under 85% of the budget (unknown: headroom).
  Requests are made during the window; nothing is read or copied until it ends. A pure function
  `zonepack::quiet(...)` with times and counts as inputs, so a test can drive it.
- **Ready:** `take_done(owner)` -> for `ready`: `write_view(t, pack_cpu(i))`, then the word to drawn, `_pack_bytes +=
  view.bytes`; for `missed` and `gone`: drop it, the word to spent, counted by reason (`stream::reason`).
- Zone textures aren't evicted during a visit (plan §4 4); under pressure the next zone's come without top levels.

## Destruction points

| Where | Today | M4 |
|---|---|---|
| native `retire(slot)` `native.cpp:58` (map changed, not wanted) | `retire::later(std::move(s.current))` | `s.current->pack_let_go()` first |
| native `forget_device` `:64-75` (new device, stop) | `s.current.reset()` at once | the destructor's hook |
| actordata not wanted `:94-100` | `retire::later` each | `pack_let_go()` first |
| actordata unused for 3600 presents `:117-123` | `retire::later` | `pack_let_go()` first |
| actordata `forget_all` / `forget_device` `:66-81` | `files.clear()` at once | the destructor's hook |
| actordata other-mode DATs at a zone change (new) | - | `pack_let_go()`, `retire::later` |
| Release | native and actordata keep their data (`main.cpp:1034`, :1040) | module `stop`: `clear_all()` drops every owner before `stream::stop` |

None waits: `drop_owner` never blocks, and the gpu_zone may go right after (streamer.md "Owners, tickets").

## Texture lifetime on gone tickets (the streamer change)

`src/pack/stream.cpp`, decision 1:

- `end_epoch_locked(bool lost)` (:367) gains a mode: **keep** (`do_open` :1191: open, close, reload) or **let go** (stop
  `shut_down` :1248, `set_device` :1423, `lose_locked` :639). Keep: a ready resident leaves the map (`in_map = false`)
  but stays with its `refs`; its ready tickets read `gone` with `d.r` kept (`finish` :248 must not clear it for them);
  the resident goes on a `held` list. Let go: today's behaviour, plus every resident on `held` let go and its tickets'
  `d.r` cleared.
- `drop_locked` (:330): a `gone` ticket with `d.r` set gives back its reference like a ready one (`let_go_locked` at 0).
- `get` and `write_view` stay ready-only. `counts` gains `held` and `held_bytes` (textures kept by gone tickets); the
  status shows them when non-zero ("kept for owners that haven't let go").
- `streamer.md` "As built" and "Notes for M4", `stream.hpp`'s `view` comment, and sync.md's paragraph on
  `end_epoch_locked` change with it.

## Selection, and who must keep game_srv

- `texture_srv(i)`: `drawn && !compare` -> the pack slot; else today's (`i` past the textures: white). Called by the
  zone pass and the model actors through `draw.cpp:947` (the only pack user).
- `game_srv(i)`: today's slot always. Users: `fxrender.cpp:782` and `:822` (the effects' MMB meshes sample level 0,
  `SampleLevel(..., 0)`: a pack chain would change their look, and effects stay out of the pack), `zoneshadow.cpp:508`
  and `:523` (cut-out casters and the ground map: the game's alpha until M10's alpha-parity check).
- Visible in pack mode: those users now read the 1x original where today's overlay mode reads the overlay's 2x (fx's
  zone meshes, leaf shadows' cut-outs): the game's own look, as the plan wants.
- The breadcrumbs: `draw.cpp`'s `piece_note` gains `pack` (the slot drawn was a pack slot), printed by
  `breadcrumb_report`, so a zone-pass hang report says whether a pack texture was bound.

## The FSR bias

- `upscale`: both paths store the last input and output widths and the time drawn in atomics where they set
  `g.in_w` (`upscale.cpp:446`, `:539`); new `bool world_scale(float &ratio)`: FSR (1, 2 or 3.1) drew in the last
  250 ms with `in_w < out_w`, ratio = in/out.
- The module (update, state on, `fsr_bias=1`): `bias = clamp(log2(ratio), -1.0, 0.0)` (with the sampler's -0.5: at
  least -1.5 in all), else 0; `zone::inject::set_texture_bias(bias)` (an atomic beside `per_pixel`, `inject.cpp:51`,
  :285).
- `draw_options::texture_bias`, set in `draw_world` (`inject.cpp:175`), written by `record` into a new
  `float4 tex_bias` at the end of `gpu_constants` (976 -> 992 bytes of `kConstantsBytes` 1024) and of the shader's
  `frame_constants`.
- `shade` (`draw.cpp:224`): `[branch] if (tex_bias.x != 0) t = colour_map.SampleBias(wrap, i.uv, tex_bias.x); else t =
  colour_map.Sample(wrap, i.uv);`: a uniform branch, so pack off runs today's instruction. Normal maps unchanged.
  Game textures in pack mode have one level, so the bias only touches pack textures. Quality 1.5x: -0.58 (total
  -1.08); Performance 2x: -1.0 (-1.5); Ultra Performance 3x: clamped -1.0.
- The zone shader's source changes, so its D3DCompile cache key does: the first start compiles it on the game thread
  once (shadercache.hpp; about 2 s at the title), as any shader change.

## Settings, menu, status, commands, log

**`src/pack/packoptions.hpp`** (like `zoneoptions.hpp`): `kPackByDefault = false`; the hidden keys' defaults.

**Schema entry** (after `chars.detail`):
`{ "textures.pack", tab::game, "Texture pack", "Use the texture pack", <help>, nullptr, nullptr, false, { 0.0f, _, _, _ },
flag, 0.0f, 1.0f, kPackByDefault ? 1.0f : 0.0f, &pack::folder_exists }`.
- Help (M4's, the plan's §6 text once M5 does characters): "Sharper scenery, ships and furniture, from a texture pack
  made on this PC from your own game files and kept on the graphics card. Anything the pack doesn't have is drawn as
  the game's own. Takes effect at the next area you enter. (/hifi textures)"
- Section help: "Higher-resolution textures kept on the graphics card, made on your own PC from your game's files."
  (M7: "..., from a texture pack made from your own game's files.", as a pack may come from elsewhere.)
- `pack::folder_exists()`: an atomic set by the module (the pack folder exists, with or without `pack.idx`: M6, so a
  folder copied only in part still shows the setting and the menu's "has no pack.idx" line) at its first update and at
  each on and reload; no file call in it.

**Menu block** (`draw_game`, right after `draw_tab_settings`, only while `folder_exists()`), every line a string the
module made in `update`:
- "Found: 23,617 textures (scenery 4,755, the rest 18,862), 6.8 GB" / "Texture pack can't be used: <why>" /
  "Opening the texture pack..." / "Off: turn it on above." / "The texture pack folder has no pack.idx (copied only in
  part?)..." (No pack folder: until M7 the section wasn't shown at all. Since M7 the menu draws its heading and help
  itself and the module's one line says how to get a pack: "No texture pack yet. You can use a pack you already have
  (put it in bootloader\hifixi-pack) or make one on this PC with hifixi-enhanced-textures:
  github.com/relliko/hifixi-enhanced-textures"; no Game memory or "To remove" line.)
- M7, a pack being made in the folder (memwatch's thread reads `progress.txt` and whether `pack.lock` is there every
  3 s; `pack::build_line`): "Making the texture pack: 812 of 2,140 areas and models, about 1 h 10 min left (paused:
  the game is running)." / "Making the texture pack stopped at ...: run hifixi-enhanced-textures again ..." /
  "... failed at ...: texpack.log in the pack folder says why." In place of the steps with no folder, and in place of
  the "has no pack.idx" line before its first commit. Also in the status and, as it appears, the log. A damaged
  progress.txt, convert's, or a finished build: nothing.
- "This area: 812 of 840 scenery textures from the pack (28 not in it); 3 coming" / "This area was entered before
  the pack was on: its textures switch at the next area." / "Comparing: the game's own textures are drawn
  (/hifi textures compare again)."
- "On the graphics card: 410 MB for the pack; 7,950 of 11,800 MB in use."
- "Game memory: 2.1 of 4.0 GB in use (largest free block 610 MB)."
- Pause lines: "Paused: the graphics card is nearly full." / "Paused: the game is low on memory." / "Paused after a
  graphics-card stop: /hifi textures on uses it this session." / "Waiting for the area to settle."
- While `pivot.ini` lists hifi-textures (parsed at the first update and at reload, never per frame): "The hifi-textures
  XIPivot overlay is still installed, so the game also loads its bigger files. Remove it to save the game's memory."
- "To remove the texture pack: turn it off, then delete the hifixi-pack folder (6.8 GB)."
- Files (`draw_advanced`): "Texture pack: <folder>".

**The index summary** (for "Found"): `pack::parse` already walks every entry; it fills new `index` fields (entries by
`source_kind`, colour bytes) on the worker, so the module reads three numbers.

**Hidden keys** (`[textures]`, read in update): `zone=1`, `actors=1` (new), `fsr_bias=1` (new), `budget_mb=0`,
`ring_mb=32` (read once, at the streamer's start), `per_present_mb=64`, `zone_delay_s=2`; `chars`, `height`,
`material`, `surface` are M5/M9-M11's and not read yet.

**Commands:** `/hifi textures` or `status` (the lines: state, found, this area, the streamer's `status()`, memory);
`on` / `off` (`game::set_option_on("textures", "pack", ...)`, as `/hifi nativezone perpixel` does; `on` also lifts
safe start for the session); `compare`; `reload`.

**Log** (hifixi.log through `backend::log`): the open ("texture pack: <folder>: 23,617 textures..., opened" or "can't
be used: <why>"); per zone once its requests settle or the zone changes ("texture pack in zone 230: 812 of 840
textures from the pack, 28 not in it, 0 failed, 96 MB, the last 3.4 s after zone-in"); `stream::take_log` lines; state
changes (on, off, reload, lost, safe start).

**`/hifi status`**: `native::status()` gains the per-slot pack line; `main.cpp`'s help line (:1217) and header comment
(:43) say `/hifi textures on|off|compare|reload|status`.

## Build

- `PASSES_PACK` (`build.cmd:78`): + `src\pack\stream.cpp src\pack\pack.cpp src\pack\validate.cpp src\pack\hash.cpp
  src\zone\zonepack.cpp`. menu_test links `%PASSES%`, so it gets them (the streamer is never started there).
- The build lines that link `zonemesh.cpp` or `zone_gpu.cpp` alone stay as they are (header-only `pack.hpp`; no
  `stream::` call in zone_gpu.cpp). Tests that stream (zonenative, zoneinject, modelactor, zone_view with a pack) add
  the stream files and `zonepack.cpp`.

## The d3d12 pages, in the same commit

- **sync.md:** the streamer is started by the module (no longer "M3: only packstream_test"); decision 1 replaces the
  paragraph on `end_epoch_locked` ("a gone ticket holds no reference"); drop at hand-over (`pack_let_go`) and the hook
  in `release()`; refresh the stale `zone_gpu.cpp` anchors (upload :128, the heap :239-256, `ready` and `release` moved
  since 6d321d5).
- **descriptors.md:** the zone heap's pack region, its initial views and the write-once rule; `write_view` called on the
  game's thread with no lock held (zonepack).
- **threads-pso.md:** the module's streamer calls on the game's thread (all non-blocking; `stop` joins in Release);
  `write_view` with no lock of ours held; the drop hook from retire's free.
- **barriers.md:** the zone pass reads pack textures by promotion, no barrier (unchanged rule, new user).
- **debugging.md:** the zone breadcrumbs' `pack` field; the texture pack section's streamer lines now live in the
  plugin.

## Test plan (every M4 test of the plan, and the new ones)

| Plan test | File | What it checks |
|---|---|---|
| zonemesh: indices and `model.missing` identical, pack on and off | `tests/zonemesh_test.cpp` | each map (100, 4, 230, 240, 412, 50, 121, 1) loaded both ways with `HIFIXI_PIVOT_INI` = a test pivot.ini over the real overlays (read only): models, pieces, vertices, indices, `missing` equal; textures' names equal, pack mode at the game's size with levels 1; keys only in pack mode; load times |
| zonemesh: hifi-textures ignored, a higher overlay honoured | `tests/zonemesh_test.cpp` | a made-up overlay under `build\test\zonemesh` (0=fake, 4=hifi-textures) with a copy of zone 100's DAT: pack mode reads fake's, makes no keys (decision 6); Zi'Tah 121 through zitah-canopy makes none; `overlaid(path, exclude)` alone |
| zonemesh: pack off byte-identical | `tests/zonemesh_test.cpp` | the oracle's CRCs as today with no pack argument |
| (new) keys match the pack | `tests/zonemesh_test.cpp` | with the real pack (read only; skipped without): map 100's keys hit; where `entry.file_id` is the map's DAT, `entry.chunk` equals the chunk ordinal |
| game textures first, then pack | `tests/zoneinject_test.cpp` | a made-up pack (pack_write under `build\test\zoneinject`) whose records are a flat colour: the zone pass draws the game's colours, then the pack's once ready; WARP, `ZONE_GPU=1`, `HIFIXI_GBV=1`, 0 messages |
| slot-write log: each slot once per gpu_zone, before its flag | `tests/zonenative_test.cpp` | `set_test_log`: per gpu_zone each texture's 'w' at most once and before its 'd'; never 'w' after 'd' or 's' |
| off mid-stream: game picture next frame; re-on waits for the next zone | `tests/zoneinject_test.cpp` (picture), `tests/zonenative_test.cpp` (words) | off: every `texture_srv` the game slot in the same call, picture equal to the game's next frame; on again: nothing asked for the current gpu_zone, the next zone asks |
| quiet window respected | `tests/packmodule_test.cpp` (new) | `zonepack::quiet` over made-up timelines (packet, 3 s fallback, delay, retired or headroom); nothing read while `kQuiet` (streamer counts) |
| 50-switch churn with copies in flight | `tests/zonenative_test.cpp` | on/off/reload 50 times with copies held (`stream::test::gate_copies`): no slot written twice, `leaked` 0, owners 0 at the end, 0 messages under GBV |
| device reset mid-copy | `tests/zonenative_test.cpp` (child process, as packstream's "lost") | `RemoveDevice` with copies held: every ticket the words wait on missed (`lost`) before the glue's `clear_all`, the words spent after, `stop()` fast, no crash |
| (new) the module's loss path | `tests/packmodule_test.cpp` (last) | `RemoveDevice` seen by `gpu_crash::watch`: at the next present `clear_all` first, the loaders out of pack mode, not in use, the consumer's frame not called again, status, menu and log say so |
| (new) an open overlapping another | `tests/packmodule_test.cpp` | the first open held on the worker (`stream::test::after_load`) while a reload asks another: not in use till the second is done, then in its epoch, requests ready; a device change while opening: one open loaded, in use after the change |
| (new) the request cap and the priorities | `tests/zonenative_test.cpp` | 230 and its second map 100 under a cap of 40 (`zonepack::set_request_cap`): 40, 40, 6, the zone's all first; the streamer takes none of the second map's while one of the zone's waits |
| (new) the FSR bias's chain | `tests/zoneinject_test.cpp`, `tests/packmodule_test.cpp`, `tests/upscale_test.cpp` | less red at -1 over red second levels through `inject::set_texture_bias`; `fsr_bias_on()` with the key, off, stopped; `world_scale` before, during and 260 ms after FSR draws |
| (new) a reload in a Mog House | `tests/modelactor_test.cpp` | a DAT whose visit a reload ended, the mode the same: kept in the zone, let go at the next, read again with an owner |
| stop during streaming | `tests/zonenative_test.cpp` | `clear_all` + `stream::stop` with 1,000 queued and four in flight, then `retire::release_all`: `leaked` 0, the device's references back |
| plugin load/unload with 1,000 queued and copies in flight (from M3) | `tests/packmodule_test.cpp` | the module's `update`/`present`/`stop` through `passes::modules()` on WARP: `counts().leaked` 0, references back; started again: works (one more ring, once) |
| zoneslot_test | `tests/zoneslot_test.cpp` | unchanged, must pass |
| modelactor: eviction and `forget_all` mid-stream | `tests/modelactor_test.cpp` | owners dropped at each (owners and `in_flight` 0), no use-after-free under the debug layer |
| fx_render_test, fx_test | `tests/fx_render_test.cpp`, `tests/fx_test.cpp` | an MMB mesh drawn with a word set to drawn binds `game_srv` (the test log counts pack selections: 0) |
| zone_view with `ZONE_VIEW_PACK` | `tests/zone_view.cpp` | the real pack (read only): East Ronfaure 101 and Giddeus 145 street, pack vs overlay vs plain; Zi'Tah 121 byte-identical to its non-pack render (no keys); no pack: byte-identical to the M0 bmps (`build\m0-baseline`, a short GPU run or `--warp`); `ZONE_VIEW_BIAS` + a 2/3 size for the FSR Quality A/B by eye |
| framegen_test | `tests/framegen_test.cpp` | unchanged, must pass |
| menu_test: hidden without a pack (tab and search) | `tests/menu_test.cpp` | `HIFIXI_PACK` at an empty folder: no "Texture pack" in the Game tab or a search for "texture"; with a `pack.idx` there (after the module's update): shown |
| menu_test: Off sets 0, the others leave it | `tests/menu_test.cpp` | presets applied: `textures.pack` 0 after Off, unchanged after Balanced/High/Ultra |
| menu_test: a click saves and sets Custom | `tests/menu_test.cpp` | the checkbox clicked: `[textures] pack=1`, preset custom |
| menu_test: no file or DXGI calls during menu draw | `tests/menu_test.cpp` | the module's I/O counter (`pack::test::io_calls()`, every file, DXGI and stream call it makes) unchanged across `draw_menu` |
| (new) gone tickets keep their texture | `tests/packstream_test.cpp` | a ready ticket across `open`/`close`: `gone`, the resource alive (AddRef/Release probe) until `drop`, then retired at +2; stop and `set_device` still let go; `held` counts |
| (new) `want::down_to` | `tests/packstream_test.cpp` | 2x and 4x records give 2 and 3 levels; a record at the game's size 1 |
| (new) index summary | `tests/pack_test.cpp` | entries by source kind and colour bytes equal a count over the entries |
| render tier | `build.cmd`, `python -I tests\run_suites.py` | all suites (known flakes: zonedraw_test "mapped at its base", runner_test "zone 100 entered", hifps_step_test timing) |

New suite entries in `tests/run_suites.py`: `packmodule_test`; `zonenative_test` also with `ZONE_GPU=1` if it gains GPU
checks. Hardware runs stay short while the game may be running (no soak over 2 minutes).

**In game** (the plan's M4 list): pack off first (as before); on: Port Jeuno -> Rolanberry -> Giddeus, coverage near
100%, Zi'Tah's canopy intact; `compare` and off instant; zone 10 times with no hang; note the city's "Game memory".

## Risks

- **A hang blamed on the pack** (the open zone-pass setup hang): run the same build with the pack off first; the
  breadcrumbs' `pack` field and the streamer lines in the report tell them apart.
- **1x for a moment after zone-in**: a pack-mode zone uploads the 1x originals, the pack's come after zone-in +
  `zone_delay_s`; today's overlay shows 2x at once. Mitigated by requests queued while quiet and most-used-near first.
- **Off inside a pack-mode zone shows 1x**, not the overlay's 2x, until the next area (the help says so).
- **fx zone meshes and leaf shadows at 1x** in pack mode (`game_srv`), where overlay mode shows 2x.
- **Changing M3's streamer** (decision 1): packstream_test's epoch checks must be redone; GBV on WARP and the 4070 Ti.
- **A forgotten drop holds video memory** (decision 1's price): `held` in the status and the log.
- **The zone shader's first compile** after install: a once-only pause at the title (cache key changes).
- **Overlay-served zones get nothing** (decision 6): if EraDATs serves a zone DAT, that zone is all game textures.
  (As built: `/hifi status`'s zone line says "read from an XIPivot overlay's copy: no texture pack textures" and the
  menu "This area is read from another XIPivot overlay"; hifixi.log has no line for such a zone, since its coverage
  line is written only for a zone that asked for pack textures.)
- **Game-thread bursts**: 128 requests and the ready views a present; measured in packmodule_test (the worst present).
- **Video memory**: committed textures +14% for zones (64 KB pieces); a 2x zone 100-250 MB.
- **Address space**: ring 32 MB + index 4.5 MB + 5.2 KB a texture; a plugin reload one more ring, once.
- **Memory not yet down**: the overlay is still listed, so the game still loads its copies (M5).

## As built, part A (2026-11-02: step 1 and the module live)

Part A is the settings, the menu and the module; part B the zones' side (zonemesh's pack mode, the gpu_zone's pack
region, `zonepack`, the streamer's decision 1, the FSR bias in the shader). Where part A differs from the design above:

- **The module calls the owners through a `pack::consumer`**, not `zonepack::` by name (`src/pack/pack_module.hpp`):
  `clear_all`, `set_mode(zones, actors)` (the loaders' pack mode: zonepack's calls `native::set_pack_mode` and
  `actordata::set_pack_mode`), `frame(now)` (each present while in use, after `stream::on_present`), `set_compare(on)`
  (at once on `/hifi textures compare`; false when the pack goes off) and `lines(out)` (update: "This area: ..."). Part B
  registers zonepack's with `pack::set_consumer` (constant-initialized, so a static registrar works). So the module
  links nothing of the zones', and packmodule_test stands a fake consumer in and checks the order: `clear_all` first,
  then `set_mode(false, false)`, then the close or open.
- **What owners read**: `pack::in_use()`, `pack::epoch_in_use()` (E; 0 while not in use), `comparing()`, `quiet()`,
  `fsr_bias_on()` (in use and `[textures] fsr_bias=1`) and the pure `fsr_bias(ratio)` = `clamp(log2(ratio), -1, 0)` (0
  at a ratio of 1 or more, or unknown). Part B's glue sets `inject::set_texture_bias(fsr_bias_on() ?
  fsr_bias(ratio) : 0)` from `upscale::world_scale` each present and 0 in `clear_all`.
- **The quiet window is the module's**: `pack::quiet_window(quiet_inputs)` (pure; `kQuietPresents` 3, `kQuietPercent`
  85), fed each present by `follow_quiet` (`pack_module.cpp:507`) from `feed::zone()`, the new `feed::in()`
  (`g_zone.in_game == 1`), native's slot 0 (the same ready gpu_zone counted over presents, from 0 at each zone change)
  and the last update's video memory. kQuiet is set or cleared only when it changes. No `zonepack::quiet`.
- **`device(nullptr)` with the streamer started**: the owners cleared and `stream::set_device(nullptr)` (a device gone
  mid-session; the next update turns the pack off for want of one, and a device again sets it and reopens). In Release
  the module's `stop` has already run, so it does nothing there, as designed. A device change while opening asks
  nothing again: the open posted is done before or after the change (the worker takes it on a pass of its own), and
  `check_open` reads the epoch once `posts_done()` reaches it, past the one the change raised. (Part A posted the open
  again and took the first epoch raised: with the first open still loading, the module went on in its epoch and the
  second open ended it, every request after read gone; found in review, fixed 2026-11-03.)
- **failed** goes to off when the pack is no longer wanted (turned off, folder gone); turned on again it's opened again.
  `/hifi textures reload` with the pack off only looks for the folder again (and opens it when it's now wanted).
- **The index summary** (step 5g) is in part A: `index::by_source[3]` and `colour_bytes`, filled by `pack::parse`
  (pack_test checks them against a count). The "Found" line's GB are the data files' sizes in the index, in GiB
  (as Explorer shows them); the removal line's are the folder's files at the last look.
- **The pivot.ini check**: `bake::overlays()` (`dat_core.cpp`), the names `[overlays]` lists; `overlaid` reads pivot.ini
  through the same helper, unchanged in what it returns (part B adds its `exclude`).
- **Settings**: `settings::entry::shown` (honoured by `draw_tab_settings` and `draw_search`); `textures.pack` the Game
  tab's last entry, section "Texture pack" with its help line; `src/pack/packoptions.hpp` (`hifi::pack::options`:
  `kPackByDefault` false and the hidden keys' defaults). `pack::folder_exists()` is an atomic the module sets at its
  first update, `/hifi textures on` and reload.
- **Menu**: `draw_game` draws `pack::menu_lines()` after the tab's settings (M7: always, with the section's heading
  drawn by the menu when there's no pack folder, so no setting); Advanced's Files
  "Texture pack: <folder>" (with "(none there)" without one) from `pack::folder_text()`. All cached in the module's
  update (`g_text_mutex`, held only to copy strings). The pause lines say "Waiting for the area to settle." while
  quiet; the "held" line waits for part B's counts.
- **The module's other lines**: `/hifi textures` with no argument is `status`; an unknown argument prints the usage.
  The open is logged with the counts and the loaders' modes; off with why; a miss at the worker as `take_log` gives it
  (level 1). kVersion 0.66, main.cpp's help line and header comment, README "Texture pack (optional)" with the removal
  steps.
- **A streamer fix**: the worker dropped an open posted together with a device change (it took both at its loop's top
  and returned after the change). The module's device change while opening, and `set_device` then `open` within one
  update, hit it on the 4070 Ti (packmodule_test). Now an open or close waits for a pass of its own
  (`stream.cpp:1355-1375`; streamer.md "As built", threads-pso.md, sync.md); packstream_test checks it.
- **Build**: `PASSES_PACK` gains `stream.cpp pack.cpp validate.cpp hash.cpp` (zonepack.cpp: part B); new
  `packmodule_test` (links like menu_test, plus pack_write and bc2 for its made-up pack).
- **Tests (part A)**: `packmodule_test` (new: the quiet window over made-up timelines; no folder, a folder found by
  reload, pack=0 not starting the streamer; a safe start held off until `/hifi textures on`; held while quiet, ready
  after; compare; a CRC miss logged from update; reload and off with the consumer cleared first and the epoch then
  still the old one, the streamer kept; a device gone and back; the plugin's unload with 1,000 queued and four copies
  held in flight by a gate opened 5 ms into stop: nothing left, the device's references back; loaded again: a second
  start; the menu's reads make no I/O call; the worst present), `menu_test` part 12 (hidden without a pack in the tab
  and a search, shown after a reload finds `pack.idx`, the Files line, Off sets 0 and the others leave it, a click
  saves and sets Custom, no module I/O during menu draw), `pack_test` (the index summary).

## As built, part B (2026-11-03: the zones' side)

Steps 2-7: zonemesh's pack mode, the gpu_zone's pack region, `src/zone/zonepack.cpp/.hpp`, the streamer's decision 1 and
`want::down_to`, the FSR bias in the zone shader, the breadcrumbs' pack field, the d3d12 pages. Where it differs from
the design above:

- **zonemesh** (`load(map, z, r, pack)`, `load_file(file, z, r, pack)`): pack mode reads `overlaid(own,
  L"hifi-textures")` and keys each texture from the chunk read (`hifi::pack::key_of(im.body, im.size)`) only when that
  is the game's own file. Nothing else changes: the width pass already runs only for an overlay's copy, so the game's
  own file has one level a texture, and a DAT another overlay serves (Zi'Tah's canopy) reads exactly as without the
  pack, smaller levels included (zonemesh_test: the same both ways, no keys). Two flags instead of one: `zone::pack`
  (read in pack mode) and the new `zone::overlaid` (the DAT read was an overlay's copy); keys exist only for `pack &&
  !overlaid`. Hashing costs 1-3 ms a zone (zonemesh_test: 0.1-2.1 MB hashed, e.g. 230 in 29 ms against 27).
- **gpu_zone**: the region only for `pack && !overlaid` with textures. The glue's plain data is one struct,
  `gpu_zone::pack_data` (owner, epoch, a ticket a texture, bytes, `done`); `done` marks a visit whose owner was dropped
  (off, reload, a device change), so the glue never takes it again: the zone keeps its game textures until the next.
  `texture_srv(i, bool *pack)` says whether it named the pack slot (the breadcrumbs), `pack_summary()` counts the words
  (the status lines). The drop hook is installed by the glue when it takes its first zone.
- **zonepack** registers nothing itself: main.cpp hands the module `zonepack::consumer()`, the camera source
  (`game::zonedraw::camera`, new) and the log (`backend::log`), so a test links zonepack without the module, the game
  side or the backend. The consumer's `frame` gained the epoch to ask in (`frame(now, epoch)`; part A's fake consumer
  follows). A zone's requests wait for a camera of its own (the last zone pass drawn here was its map's), at most
  `kCameraWaitS` 2 s, then go by use only; a model DAT's go at once (no placements). The quiet window stays the
  module's (part A): there's no `zonepack::quiet`.
- **actordata**: `frame(device, wanted, zone)` (main passes `feed::zone()`); a zone change lets go of the DATs read in
  the other mode, and (since the review) of those in pack mode whose visit is `done`: a reload or an off and on drops
  every owner and sets the mode back once the pack is open, so a DAT still drawn kept the game's 1x textures (pack
  mode skips hifi-textures) until it went unused for `kUnusedPresents`. `ready_files(out)` lists the DATs up.
- **The FSR bias** is set each present by main.cpp, right after the loaders' frames: `inject::set_texture_bias(
  fsr_bias_on() && upscale::world_scale(r) ? fsr_bias(r) : 0)`, not in `clear_all` (pack off reads `fsr_bias_on()`
  false at the next present: a frame's lag of a mip bias). It applies to every zone pass while the pack is in use, so
  also to a zone read before the pack was on (the overlay's 2x and their levels) until the next area; game textures
  read in pack mode have one level, where a bias changes nothing (zoneinject_test). FXC took the `[branch]` around
  `SampleBias`/`Sample` (a constant buffer's value) without a warning.
- **Status**: `native::status()` has a pack line a slot ("texture pack: N of M textures from it, K not (not in it, or
  failed), W waiting, U with no key"; "read from an XIPivot overlay's copy: no texture pack textures"); the menu's
  "This area" line and "Ships and furniture: ..." come from `zonepack::lines`; each zone's coverage is logged once
  its requests settle or it's let go ("texture pack in zone 230: 52 of 54 textures from the pack, 2 not in it, 0
  failed, 0 gone; 9 MB; settled N s after it was ready"); the module's video memory line notes held textures.
- **Not done in part B**: zone_view's `ZONE_VIEW_PACK` pictures; done since (next section).
- **Tests (part B)**:
  - zonemesh_test +21: every map the same both ways with no pivot.ini (the oracle's figures and CRCs), keys only in
    pack mode; with the PC's own pivot.ini (read only) the meshes, names and misses as without the pack, the textures
    at the game's size with one level and keyed, Zi'Tah 121 from zitah-canopy both ways with no keys; made-up
    overlays: an overlay after hifi-textures honoured (its copy, no keys), hifi-textures alone leaves the game's file,
    `overlaid(path, exclude)` in any case; the real pack (read only): 30 of map 100's 32 keys found, the 13 entries
    from its own DAT at their chunk's ordinal.
  - zonenative_test +34 (the texture pack's part: a made-up pack of 230's and 100's textures, 2x, level 0 red, the
    rest blue, one left out, one no_pack; WARP, the 4070 Ti, both with `HIFIXI_GBV=1`, 0 debug-layer messages): game
    textures first (the game's picture while the copies are held), then the pack's (34,120 of 40,960 pixels change),
    2 levels a view, the zone's bytes with the pack's, the order most used first, no copy while kQuiet; compare; the
    bias -1 (more red); a zone change drops the owner at the hand-over; off mid-stream (4 drawn, 48 asked, 4 copies
    in flight): the game's picture the next frame, all spent, nothing written after; on again: nothing asked for the
    zone in view, the next zone asks; 50 switches with copies in flight: the slot-write log valid, nothing left;
    the glue's worst present (0.2 ms on WARP); stop with copies held by a gate opened 5 ms in (16-19 ms, nothing
    left, the device's references back); the device removed mid-copy (a child: all words spent, stop 0.3 ms).
    `/DYNAMICBASE:NO` added: its dump mapping failed now and then once it grew. run_suites runs it again as
    `zonenative_gpu`.
  - packstream_test +7: gone tickets keep their texture (3 of them fail against the old `do_open`), `down_to`.
  - modelactor_test +6: the DAT in pack mode streamed; let go as unused and by `forget_all` with copies in flight
    (owner dropped, nothing queued or written after); read in pack mode, the pack off: kept in that zone, let go at
    the next, read again without it.
  - fx_render_test +1: the MMB meshes' DATs read in pack mode with every pack slot another texture's view and every
    word drawn: the pictures still match the oracle (with fxrender on `texture_srv` instead, fx_fountain fails).
  - zoneinject_test +1: the bias through `inject::set_texture_bias` on one-level game textures: the same picture
    (which can't fail: "Review fixes" below has the check that the bias reaches the pass).

## The pictures and the last tests (2026-11-03)

- **zone_view** (`tests/zone_view.cpp`): `ZONE_VIEW_PACK=<folder>` reads the map in pack mode and streams its keyed
  textures from that pack (read only) as the glue asks for them (the colour record down to the game's size, the most
  used first), writes each ready view into its pack slot and then sets its word drawn, and prints what came of them;
  `ZONE_VIEW_BIAS` sets `draw_options::texture_bias` for the FSR A/B. It makes its own requests rather than calling
  zonepack (the glue takes its zones from native's slots; zone_view uploads a gpu_zone of its own). Its build line
  links the streamer. Run on the 4070 Ti at 1600x900 with the PC's pivot.ini, from the street and from above:
  - **East Ronfaure (101)**: 29 of 32 textures from the pack (1 not in it, 2 no_pack), each a 2-level view (2x), 9.2
    MB, 30-46 ms from the requests to the last view. Against the overlay's picture 0.2% of the pixels differ by more than
    24 (of 765), all of them in distant leaf cut-outs: alpha-tested pixels at the threshold flip, because the pack's
    level 1 is the converter's (`bc2::chain_levels`) where the overlay's is the loader's (`bc2::smaller_levels`). No
    surface has the wrong texture, no cut-out lost its alpha, nothing is magenta; black pixels 506 against the overlay's
    478 (the same distant leaves).
  - **Giddeus (145)**: 27 of 29 from the pack (2 no_pack), 12 views of 2 levels and 15 of 3 (its 4x), 26.4 MB, about
    47 ms. Against the overlay 1.0% (street) and 0.6% (above) of the pixels differ, none by more than 24. No magenta.
  - **Zi'Tah (121)**: no pack region (read from zitah-canopy's copy, so no keys: decision 6). With the pack, the
    picture is byte for byte the overlay's, from the street and from above. ROM2\0\2 is untouched.
  - **FSR Quality A/B** (the pack at 1067x600, bias -0.585 against 0): with the bias the ground and rock show more of
    their texture (mean |Laplacian|: 101 24.8 against 23.6, 145 17.8 against 16.1), with no artefact in a still. Whether
    it shimmers in motion can only be judged in game.
- **Pack off is bit-identical**: zone_view (maps 101, 145, 121, 230 and 100; street and above, with the PC's pivot.ini
  and with none; per-pixel at night; WARP) and zoneinject_test's picture (WARP and the 4070 Ti), built both from
  7e5879d in a scratch worktree and from this tree: 32 of 32 BMPs byte-identical.
- **Tests**:
  - zonenative_test +2: the coverage as Alex reads it (`/hifi status`'s pack line, the menu's "This area" line, the
    zone's log line once its requests settle, all with the words' numbers) and the menu's line after off mid-stream.
  - modelactor_test: three waits fixed that failed under the suites' load. The wait for the pack's views counted
    presents without sleeping and stopped finding the DAT (so 3,000 presents could pass in well under a second), and
    the wait for the held copies after the gate opened was 50 presents with no sleep.
- **Where the plan's tests ended up**: "game textures first, then the pack's" and "off mid-stream: the game's
  picture" are in zonenative_test (the zone renderer's `record`), not zoneinject_test. The zone pass's selection is
  draw.cpp's in both. zoneinject_test checks that the bias set through `inject::set_texture_bias` reaches the pass,
  over a copy of the zone whose textures have a red second level (the review's fix: on one-level textures alone it
  couldn't fail).
  `HIFIXI_GBV=1` and `ZONE_GPU=1` are honoured by zonenative, zoneinject, packstream and packmodule: all passed with
  GPU-based validation on WARP and on the 4070 Ti, and the debug layer reported 0 messages. modelactor_test,
  fx_render_test and zoneslot_test use the debug layer only and ignore both variables.

## Review fixes (2026-11-03)

Seven findings on the uncommitted M4 change, each checked against the code: all real, all fixed with a test that fails
without the fix (each mutant built and run: the check named fails).

- **An open overlapping another left the pack dead for the session** (pack_module's `check_open` took the first epoch
  raised as its own open's; two findings). The streamer's `open`/`close` now return their number and `posts_done()`
  says the last the worker has done (set with the epoch in `do_open`); `check_open` waits for its own. The device
  change no longer posts the open again. packmodule_test: a reload while the first open is held after its pack.idx
  (`stream::test::after_load`, new) and a device change while opening; against the old code the module went on in
  epoch 4 with the streamer at 5, and the 50 requests after it read gone.
- **Model-actor DATs kept 1x textures after a reload or an off and on** (their visit `done`, the mode the same; two
  findings): actordata lets go of a pack-mode DAT whose visit is done at the next zone change. modelactor_test.
- **No test could see the FSR bias missing**: zoneinject_test draws a copy of the zone with a red second level at 0 and
  -1 through `inject::set_texture_bias` (less red at -1; with `inject.cpp`'s copy into `draw_options` removed, the
  same picture); packmodule_test checks `fsr_bias_on()` (the key, off, the unload); upscale_test checks `world_scale`
  (false before FSR draws, 0.5 while it does, false 260 ms after). main.cpp's one line joining them has no test.
- **The loss tests**: zonenative's child now checks, before the glue's `clear_all`, that every ticket the words wait
  on missed with `lost` (52 of 52). A `stream::device_lost` that does nothing still passes it (the worker sees the
  removed device on its own within microseconds and loses the same way); packstream_test's lost children catch that
  ("device loss with a read pending" fails). The module's `follow_lost` is tested in packmodule_test, last.
- **The request cap and the priorities never bound**: `zonepack::set_request_cap(n)` (tests) and a zonenative case
  with 230 and its second map 100 (54 and 32 keyed) under a cap of 40: 40, 40, 6 requests, the zone's 54 before the
  second map's; the streamer's first picks all the zone's. Without the cap: 86 in one present; the priorities swapped:
  the second map's first. The model actors' priority under both isn't checked (no test has a zone and a model DAT in
  the glue together).

## Completeness check (2026-11-03)

M4 against the plan's steps, tests and in-game check, sections 4 and 6, streamer.md "Notes for M4" and the d3d12 pages:

- **Added**: packmodule_test checks plan section 2's "turning the pack off closes every pack file handle" (the README
  says the folder can then be deleted while the game runs): with the pack on the folder can't be renamed (the worker's
  reader holds a data file), after off it can. Only pack_test (the reader alone) checked it before.
- **Anchors**: the d3d12 pages' line numbers into stream.cpp, pack_module.cpp, zonepack.cpp, native.cpp, inject.cpp,
  upscale.cpp, pack.cpp and the shader cache's callers moved by the review fixes; brought back to the code. This
  page's step table keeps 7e5879d's on purpose.
- **Not done, known**: the upload ring failing to be made (`stream::counts::failure`) shows in `/hifi textures
  status` and the log, but the menu still says "Found" with every texture "failed"; main.cpp's one line joining
  `fsr_bias_on` and `upscale::world_scale` and main.cpp's `set_consumer(zonepack::consumer())` wiring have no
  offline test (the module and the glue are tested apart: packmodule_test with a fake consumer, zonenative_test driving
  the glue as the module does); the model actors' priority against a zone's; the FSR bias in motion; everything in
  the in-game check.

## Pack normals (M9 steps 2, 4 and 5, 2026-11-03: the zones' and model actors' relief, World FX's fade, the switch)

The texture pack's height records (format.md "Height records") drawn as relief per pixel on the zone, its second map and
the model actors (ships, furniture, plants: the same gpu_zone and glue, so they get relief for the first time). Only the
light changes: the colour map, its alpha and the vertex colour are as before, nothing is multiplied into the colour.
Characters (step 3, the detail slot's tilt): characters.md "Pack relief".

- **The gpu_zone's heights' block** (`src/zone/zone_gpu.cpp/.hpp`): a pack-mode zone's pack region is `2 x textures`
  slots, the colours' then the heights'; each height slot starts as the flat stand-in's view (red 0x80, a level height),
  words of its own (`height_word`, `set_height_word`: none, asked, drawn, spent) under the same write-once rule:
  `write_view` and the relief scale (`height_written`) before the word is stored drawn with release. `height_srv(i,
  &relief)` names the slot only while the word reads drawn, heights are on (`set_heights`, every gpu_zone) and compare
  is off; else a null handle. `pack_summary` counts the height words apart (`heights_asked`, `_drawn`, `_spent`); the
  test log reports their events with `kHeightLog` in the texture's number, the same grammar (a, w, d, s).
- **Asking** (`src/zone/zonepack.cpp` `frame`): while heights are on, after the colours and under the same cap, each
  keyed texture in the same order whose entry has a height record in the pack (`stream::current()->find(key)`: a pack
  without heights asks nothing more, logs and counts as before) is asked for it, `kind kHeight`, its **whole chain**
  (no `down_to`: the relief is wanted mip-mapped at a distance so it doesn't sparkle, while the colour stops at the
  game's size). Asked and settled apart from the colour: an entry with a height and no colour (a materials-only pack)
  draws the game's texture with the pack's relief; a colour that missed doesn't stop its height.
- **The relief scale** (`settle_height`): `4 x span x view.w / key.w` (`stream::view::param` carries the record's span):
  the shader takes `s(u - e) - s(u + e)` one level-0 texel each way, and the game's Sobel on its own grid is 4 times
  the central difference, times span (H = (s - 0.5) x span), times the map's texels a game texel. So a height made from
  the game's bump map gives back the game's normals (zonenative_test's stripes and format.md's 0.61 degrees). A flat map
  (span 0) or a scale that isn't a number is let go and its word spent.
- **The shader** (`src/zone/draw.cpp`): a new piece flag `kPackRelief` (32) set when `height_srv` gives a view, per
  pixel only, never for doors or a pass without bump (`no_bump`): then table 7 (t5) binds the height in the normal map's
  place and the piece's fourth root constant (unused and 0 until now, b1 `relief`) holds the scale; no root signature,
  pipeline or heap binding changed. `pack_tilt` makes the tilt as the game's normal maps have it (red the slope along v,
  green along u) from four taps through the sampler (mip-mapped, anisotropic) and `GetDimensions`, then the same frame
  from the UVs' derivatives as `kMapped`. `kMapped` prefers it where both are there; the highlight (`piece_spec`) still
  follows `kMapped` only, so a piece that had no normal map gets the relief's light and no new shine (that's M10's).
  `kBump` and the per-vertex path are unchanged (the pack's height is never bound there).
- **Sobel in the shader or a stored BC5** (`shade_bench relief`, 3840x2160, 4070 Ti, 60 frames of 4 full-screen passes
  each, a ground in perspective with a 512x512 BC2 colour, anisotropic x8 and the -0.5 bias; ms a full-screen pass,
  median, and over no relief):

  | relief | ms | over none |
  |---|---|---|
  | none | 0.085 | |
  | the game's normal map, B8G8R8A8 256, 1 level (today's kMapped) | 0.107 | +0.022 |
  | the pack's height, BC4 256 (half), Sobel in the shader | 0.170 | +0.085 |
  | a stored normal, BC5 256 (half) | 0.101 | +0.016 |
  | the pack's height, BC4 512 (full), Sobel in the shader | 0.195 | +0.109 |
  | a stored normal, BC5 512 (full) | 0.097 | +0.012 |

  **Taken: Sobel in the shader.** It costs about 0.07-0.1 ms more than BC5 for a whole 4K screen of relief (a zone
  pass covers less, and only per pixel); BC5 would double the heights' bytes in the pack (16 bytes a block against 8),
  or need a BC5 encoder on the GPU at upload, and would fix the relief's scale at build time. Revisit only if a 4K
  frame budget ever finds the 0.1 ms.
- **The light split's marks** (`src/zone/light_split.hpp` "Marks"): World FX turns every facing toward the camera when
  it reads it, so the zone writes a pixel lit with the pack's relief with its facing turned away (`ps_light`); while
  `light::marks()` is set (the pack in use with heights there and on: `pack::heights_in_use`, set each present by
  `pack_wiring::follow_texture_bias`) the characters turn theirs toward the camera (camera.w 2 in chardraw's
  constants) and World FX is told by zinfo.w bit 64 (`grass::renderer::set_light_source`'s new `marks` function,
  through `worldfx::set_light_source`). With marks off every word is as before, bit for bit.
- **World FX's relief fades there** (`src/render/grass.hlsl`): `light_at` gives `marked` (a facing turned away by more
  than 0.02 of the view direction, past the 7-bit facing's error near edge on); the screen-space surface relief
  (shade.z) is left out on those pixels, so the two don't double up. Everything else reads the facing as before.
- **[textures] height** (`src/pack/packoptions.hpp`, on by default; the schema's `textures.height`, "Relief from the
  texture pack", in the Texture pack section under "Use the texture pack", shown with the pack folder): read by the
  module into `modes::heights`; off asks none and draws none from the next pass (heights already held stay for the
  visit, so on again draws them at once). `/hifi textures status`: "...; relief on|off, N textures with heights)"; the
  menu: "Relief: N textures have heights." once the pack has any; the zone's line "Relief from the pack on N of them";
  the open's log line "N textures with heights (relief on)"; the zone's coverage log line " (N with the pack's
  relief)" when any were asked.
- **Unchanged without heights, measured**: against 198e154's own build (M7; a scratch worktree), zone_view of 100, 230
  and 145 from the street (per vertex, per pixel, per pixel at night), zoneinject_test's picture (per vertex and per
  pixel), charinject_test's and, with the installed pack (no heights, read only), zone_view per pixel of 235, 245 and
  100 at 2560x1440: all byte for byte the same; grass_test prints the same numbers. With heights in the pack and
  `[textures] height=0` (`ZONE_VIEW_HEIGHT=0`), the picture is the installed pack's byte for byte; per pixel off, the
  same with heights or without.
- **The pictures for question 11** (`C:\code\LSB\hifi-textures\m9-ab`: each zone without heights, at half and at full,
  and crops of where they differ most, with the difference x16): from the m9-zones packs (the installed pack's colour,
  heights 100 of 119 from the game's bump maps), per pixel, the street view: Bastok Markets (235) 53% of pixels change
  at half (1.2 levels on average), 59% at full (1.6); Lower Jeuno (245) 46% / 58% (1.0 / 1.4); West Ronfaure (100) 61%
  / 67% (1.0 / 1.4); the light on the whole within 0.15% everywhere. `game` is the same as `half` for these 2x zones.
  Most of the zones' textures with game heights are the ones whose pieces already had the game's normal map, so the
  change there is small (the pack gives back the game's own); the relief made from the colour (grass, walls without a
  bump map) is where it shows.
- **Tests**: zonenative_test's heights part (`zonenative_test heights`, run by the main run in a process of its own: its
  streamer and loaders afresh; 14 checks): a made-up pack of 230's keyed textures, a third colour and height, a third a
  height alone, a third a colour alone (BC4 stripes, 4 texels wide, span 1): heights off asks none; on, each asked once
  and drawn (the colour-less ones too), the bytes counted, the menu's line; per pixel the picture changes (10% of
  pixels on WARP) with the light on the whole within 3%; per pixel off byte for byte the one without; the light split's
  words differ only on the relief's pixels, each with its depth and its facing turned away; off again byte for byte
  (picture and light split), on again at once, compare without relief; `clear_all` spends every height; the slot-write
  log valid for the heights' words; the debug layer silent. modelactor_test: the model DAT's heights drawn, each with
  a relief scale. grass_test: the marks (the stripes' spread with relief 28.76, marked 18.03 = without relief 18.03,
  facing away with marks off 28.76). shade_bench's `relief` mode.
- **Not done**: the in-game look, and Alex's answer to question 11 (the pack's `--height-scale` default stays `half`).
  Characters (step 3) since 2026-11-04: characters.md "Pack relief".
