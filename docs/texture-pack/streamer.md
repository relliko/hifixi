# The texture pack streamer (M3 design)

The streamer reads pack records (`format.md`) and copies them into video memory on a copy queue of its own, for owners
that draw them: zones and model actors (M4), characters (M5). This page is the design for M3, written against dev-pack
8818a7d (after the re-architecture: no D3D12 hooks, `src/host/retire.hpp` the one fence retire list). It replaces the
plan's §4 "Streamer" where they differ; each difference says why. Built in M3: "As built" at the end says where the
code differs and what it found; line numbers above point at the code it builds on.

Read with it: `docs/d3d12/sync.md`, `barriers.md`, `descriptors.md`, `threads-pso.md`, `debugging.md`. The rules this
page adds go onto those pages, with anchors, in the commit that writes `src/pack/stream.cpp` (task list, last section).

## What was measured (2026-11-02)

Scratch programs (`...\scratchpad\m3\probe.cpp`, `stall.cpp`), 32-bit and large-address-aware like phoenix-loader.exe,
on the RTX 4070 Ti (driver of that day, resource heap tier 2, binding tier 3) and on WARP, against the real pack
(`C:\Games\PhoenixXI\bootloader\hifixi-pack`, read only). The game was not running. "Random" means the pack's colour
records in a fixed shuffled order (mixed sizes as the pack holds them: median record 172 KB, 512x256, 7 levels).

**The pack's sizes.** 23,612 usable colour records, 6,982 MB stored. Largest side: 128 or less 1,641; 256 7,259; 512
10,091; 1024 4,572; 2048 49. The largest record is 5.3 MB (2048x2048, 10 levels; two over 4 MB, none over 8 MB). Zones
alone: 4,750 records, 1,087 MB.

**Address space and commit per texture (4070 Ti).** The same for committed and placed resources:

| Textures (random) | Stored | VA used | Commit | VA after all released |
|---|---|---|---|---|
| 250 | 67 MB | 0 MB | 1.0 MB | 0 |
| 1,000 | 295 MB | 8.0 MB | 5.0 MB (5.3 KB each) | 8 MB kept |
| 2,000 | 591 MB | 8.0 MB | 10.3 MB | 8 MB kept |
| 4,000 | 1,201 MB | 23.8 MB | 20.3 MB | 23.8 MB kept |
| 8,000 | 2,371 MB | 39.6 MB (5.2 KB each) | 41.4 MB (5.4 KB each) | 39.6 MB kept |

- The driver's bookkeeping grows in 8-16 MB steps of address space and **never gives it back** after the resources are
  released (commit mostly comes back: 1-3 MB kept). So the streamer's address space is a high-water mark: about 5.2 KB
  times the most textures ever resident at once, in 8 MB steps.
- Placed resources in 64 MB heaps cost the same address space and commit per texture. They don't save what the plan
  hoped they would.
- For scale: the 4070 Ti's device alone took 290 MB of address space and 30 MB commit in this process (the game's device
  already pays it); `pack.idx` loaded takes 4.3 MB.

**WARP is different: textures live in the process.** 1,000 random textures took 323 MB of address space (committed) or
387 MB (placed, whole heaps), and 195-271 MB stayed taken after everything was released. A 32-bit WARP test must keep
what's resident small (waves of 256 MB at most) and can't check the address space per texture: that check is hardware
only.

**Creation times (4070 Ti).** CreateCommittedResource median 0.135 ms, p99 0.3 ms, max 1.5-8 ms (1,000 in 150 ms; 8,000
in 1.2 s); Release 0.064 ms. CreatePlacedResource 0.007 ms, plus CreateHeap 1.9 ms per 64 MB heap. WARP: committed
0.03 ms, placed 0.003-0.005 ms, heaps 0.1 ms.

**Video memory.** Committed textures are allocated in 64 KB pieces: the whole pack would take 7,710 MB for 6,982 MB stored
(+10.4%), zones 1,242 MB for 1,087 MB (+14.2%). Placed with the 4 KB small-texture alignment where allowed (8,910 of the
23,612 qualify): 7,368 MB (+5.5%), zones 1,154 MB, but a 64 MB heap takes all its 64 MB at once (1,000 textures: 384 MB
for 311 MB of allocations).

**Reading into mapped memory.** A buffer in an UPLOAD heap is write-combined on the 4070 Ti: written at 9-20 GB/s, but
**read back by the CPU at 57-59 MB/s**, against 2,400 MB/s for cached memory. `reader::read_record` checks its CRC over
the bytes it just read, so reading straight into an UPLOAD ring ran the whole pipeline at 56 MB/s (400 MB in 7.1 s). A
CUSTOM heap buffer with `CPU_PAGE_PROPERTY_WRITE_BACK` and `MEMORY_POOL_L0` reads back at 2,400 MB/s like any memory and
costs the same address space and commit (32 MB ring: 32 MB of each). WARP hides this: its UPLOAD memory is cached (CRC
2,300 MB/s), so only a hardware run can catch a read of write-combined memory.

**Streaming (4070 Ti, pack files warm in the file cache, one copy list per texture, at most 4 in flight).** 400 MB, 1,346
random textures:

| Ring | Read path | End to end | Read + CRC | Other |
|---|---|---|---|---|
| 32 MB UPLOAD | into a cached buffer, then copied in | 1,143-1,169 MB/s | 1,464-1,512 MB/s | copy into the ring 21.9 GB/s |
| 32 MB UPLOAD | straight into the ring | 56 MB/s | 57 MB/s | (the CRC reads write-combined memory) |
| 32 MB CUSTOM write-back | straight into the ring | 1,113-1,151 MB/s | 1,340-1,380 MB/s | |
| 64 MB CUSTOM write-back | straight into the ring | 1,117 MB/s | 1,331 MB/s | 6 wraps |
| 8 MB CUSTOM write-back | straight into the ring | 1,134 MB/s | 1,364 MB/s | 51 wraps, 12 MB skipped at wraps |

- The copy queue alone (lists of up to the ring's size, ring to textures): 13.2-14.5 GB/s. Recording and submitting one
  texture's list: 0.04 ms. An empty list's round trip to its fence: 0.004-0.009 ms. `pack::check` with the device
  (GetCopyableFootprints): 2.6 us a record.
- So reading and the CRC are the limit, never the copy; the ring's size hardly matters from 8 MB up. Cold files weren't
  measured (no way to empty the file cache without admin rights); a hard disk would make reads the limit by far.
- WARP, 150 MB: 917-1,433 MB/s end to end; its copy queue alone 4.1-6.2 GB/s; round trip 0.03-0.04 ms.

**A read that never finishes.** `stall.cpp` makes a named pipe the reader opens as `pack-000.bin` (an index whose folder
is `\\.\pipe\hxp-stall-<pid>\`) and writes nothing, so the overlapped ReadFile stays pending. `reader::cancel()` from
another thread returned it in 0.16-0.85 ms ("cancelled") in each of five rounds. This is how packstream_test stalls a read
on purpose. (A byte pipe can't throttle a read: ReadFile returns what's there, and the reader takes a short read as
"cut short". A stalled read is the stricter test anyway.)

## Decisions

### Committed resources, not placed

Pack textures are **committed** resources (`CreateCommittedResource`, DEFAULT heap, `D3D12_HEAP_FLAG_NONE`, made in
COMMON), one per uploaded record.

- The plan (§4 "Resources") asked for placed resources in 64 MB heaps unless M3 showed the address-space cost of
  committed ones to be small. It is not just small, it is the same: 5.2 KB of address space and 5.4 KB of commit a
  texture either way on the 4070 Ti, from the driver's bookkeeping.
- Committed gives each texture's memory back when it is released; no sub-allocator, no fragmentation, no partly used
  64 MB heaps held against the video memory budget, no aliasing.
- The cost: creation is 0.135 ms instead of 0.007 ms (on the worker, never the game's thread: 300 textures 40 ms), and
  +10% video memory (+14% for zones: small textures in 64 KB pieces). Accepted. M6 may move textures of 64 KB and less
  into placed heaps if the logs show the budget pinched; nothing in the API depends on which.
- Every texture is named (`SetName`, "hifixi: pack <entry name>") so a DRED page fault names it.

### One copy queue, one fence, one worker thread

- **Thread.** One `std::thread`, `stream::worker`, started by `stream::start` and joined by `stream::stop`. Below
  normal priority; raised above normal in `stop()` before the join (memwatch's lesson: below normal it waited seconds
  for a core while the CPU was busy, memwatch.cpp:217-219).
- **Queue.** Its own `D3D12_COMMAND_LIST_TYPE_COPY` queue ("hifixi: pack copy queue"), made on the worker once there's a
  device, like the characters' (`cache.cpp:265-287`), never on the game's thread.
- **Lists.** Four allocators and four lists, one per copy in flight (`kInFlight` 4, the plan's pacing). Slot k's
  allocator is reset only when the fence has passed the value signalled after slot k's last list (sync.md: Reset only
  when the GPU has finished every list on it).
- **Fence.** One fence, "hifixi: pack copies", signalled only by the worker after each `ExecuteCommandLists`, values only
  going up (sync.md: one fence per timeline, one writer). Nothing else signals or waits on it except the worker's own
  CPU checks.
- **No GPU Wait.** The direct queue never `Wait`s on the copy fence. A texture is marked ready only after the worker has
  seen `GetCompletedValue() >= v` for its copy, so every list the game's thread records after it read the ready flag is
  submitted after the copy finished (sync.md: a CPU check is a valid substitute for a GPU Wait). Same scheme as
  `gpu_zone::ready()` (zone_gpu.cpp:273-286).
- **Waits.** Every CPU wait on the copy fence is `SetEventOnCompletion` plus `WaitForMultipleObjects({fence event, stop
  event})` with a timeout (1 s while running, 50 ms in `stop()`), never INFINITE, never under a lock. After device
  removal the fence reads `UINT64_MAX`, so every wait passes at once.

### The upload ring

- **One buffer in a CUSTOM heap: `CPU_PAGE_PROPERTY_WRITE_BACK`, `MEMORY_POOL_L0`,** made in COMMON on the worker at the
  first `start` with a device, mapped once and kept mapped until `stop()` or device loss (not freed when the pack is
  turned off). Named "hifixi: pack ring". Not an UPLOAD heap: the reader reads each record straight into the ring and
  checks its CRC there, and reading write-combined UPLOAD memory runs at 57 MB/s (above). A cached bounce buffer in
  front of an UPLOAD ring is as fast but costs an extra copy and its own address space; not taken.
- **Size.** `[textures] ring_mb`, default 32, clamped to 8-64. The address space it costs is exactly its size (the
  mapping is counted when the buffer is made). 8 MB streamed as fast as 64 (above); 32 keeps the plan's default and
  six 5.3 MB records in flight.
- **A record's span is contiguous in the ring.** The levels to upload (`first` to `first + count - 1`) are read as one
  span from the record's level `first` offset: it starts at a multiple of 512 and keeps the stored layout, so each
  level's footprint offset in the ring is `at + level[i].offset - level[first].offset`, still a multiple of 512.
  The ring hands out `[head, head + span)` rounded to 512, or wraps to 0 when the span doesn't fit before the end; the
  bytes skipped at the end are freed with the last copy before the wrap. A span never straddles the wrap point (the
  format.md M3 rule, by construction).
- **A span bigger than the ring is refused:** the ticket misses ("bigger than the upload ring") and the game's texture
  is drawn. With 8 MB the smallest ring holds the pack's largest record (5.3 MB).
- **Ring space is reused** only once the copy fence has passed the value of the copy that read it (sync.md: an upload
  buffer lives until the copy that reads it is done, on the queue that ran it).
- The ring is touched by the copy queue only, so it is never handed to `retire::later` (the game queue's fence says
  nothing about the copy queue, sync.md:99-100).

### Resource states and barriers: none

- A pack texture is made in COMMON. On the copy queue `CopyTextureRegion` promotes it to COPY_DEST implicitly; when that
  `ExecuteCommandLists` finishes it decays to COMMON (everything used on a copy queue decays: barriers.md "Implicit
  promotion and decay").
- On the direct queue a shader read promotes it implicitly from COMMON to PIXEL_/NON_PIXEL_SHADER_RESOURCE (a
  non-simultaneous texture may be promoted to a shader-resource or copy state), and since it was only promoted to a read
  state it decays to COMMON again at the end of each `ExecuteCommandLists`. Tests read it back with a direct-queue copy
  (promotion to COPY_SOURCE, same rule).
- **So nobody ever puts a barrier on a pack texture.** An explicit transition on the direct queue would stop it
  decaying. A texture is written exactly once, on the copy queue, before anything reads it; it is never written again
  (a key streamed again gets a new resource).
- The ring is a buffer made in COMMON: buffers promote to COPY_SOURCE on the copy queue and decay. (An UPLOAD-type heap
  would have to be GENERIC_READ for life; a CUSTOM heap buffer is made in COMMON.)
- GPU-based validation checks these promotions at GPU time; the GBV run is part of M3's tests.

### Descriptors: the streamer owns no shader-visible heap

- The plan had the streamer write each texture's SRV into "a slot nothing has referenced since its last fence-retire"
  and AddRef the owner's heap it writes into. Instead **the owner writes the view**: `stream::write_view(ticket,
  cpu_handle)` makes the SRV (`CreateShaderResourceView`: the ticket's format, `MipLevels` = levels uploaded, most
  detailed 0) into a slot the owner names, on the owner's thread, with none of the owner's locks held (a device call,
  threads-pso.md).
- So the streamer never writes into another module's heap, never holds a reference to one, and the owner alone decides
  when its slot is free: zones a write-once pack region per `gpu_zone` (plan §4 "Lifetime per zone visit"), characters
  a second pair from `free_slots`, which only come back behind the fence (`cache.cpp:173-186`). The owner publishes its
  own flag after writing.
- A slot is still only written while no list in flight can read it (descriptors.md "Descriptors in flight"): the
  owner's rule, unchanged.
- The "shared SRV" of the plan becomes a shared **resource**: each owner has its own view of it in its own heap (one
  view can't be in two heaps, and zones, characters and model actors have three).

### Owners, tickets, generations

- **Owner.** `stream::owner add_owner(const char *name)`: one per user (a `gpu_zone`, the model actors, the
  characters' cache). `drop_owner(o)` drops every ticket it holds. Owner ids aren't reused within an epoch.
- **Ticket.** `stream::ticket request(owner, const want &)` returns `{ slot, generation }`, an index into the streamer's
  own table and that slot's generation. Never a pointer into an index snapshot (a reload frees the old one) or into
  the owner. A slot is reused only after the worker is done with it, with its generation raised, so an old ticket reads
  `gone`.
- **Epoch.** One counter raised by `open`, `close`, a reload, a new device and device loss. A ticket of an older epoch
  reads `gone`; a request made against an older index snapshot (its epoch passed in `want`) is `gone` at once.
- **What's asked:** `want { key, kind (colour; height and material from M9/M10: any kind the entry has a record of
  streams the same way, a height record as BC4_UNORM; the zones' glue asks for heights since M9 step 2, the whole
  chain), first (top levels never wanted),
  count (0: all the rest), priority }`. Zones ask down to the game's own size, characters `6 + log2(scale)` levels
  (plan §3 "Uploaded levels"). `first > 0` is refused when level `first` isn't a multiple of 4 on both sides (a BC
  texture's top level must be).
- **States:** queued, reading, copying, ready, missed (not in the pack, `no_pack`, a broken rule, a read or CRC failure,
  too big, video memory: counted by reason), dropped, gone. Each slot has one `std::atomic<uint32_t>` word, generation
  << 8 | state: the worker stores ready with `memory_order_release` after it saw the copy fence; `poll(ticket)`
  loads it with `memory_order_acquire`, takes no lock and makes no device call (the characters ask while recording).
- **Many at once:** `take_done(owner, out)` hands an owner the tickets that changed since its last call (under the
  streamer's mutex, O(changed)): what M4's zones use at each present for hundreds of textures.
- **Ready means:** `get(ticket, view)` gives the resource, format, size, levels and first level; the resource stays
  valid while the ticket is held and until the retire fence after `drop`.
- **`drop(ticket)`** in any state: queued is removed; reading or copying is marked dropped and the worker finishes or
  abandons it; ready gives back the ticket's reference (below). Non-blocking, any thread.
- **No waiting for the worker before an owner goes.** The plan had owners keep their object until the streamer's
  per-owner in-flight count read 0, because the worker would write into it. Here the worker writes nothing the owner
  owns (no views, no flags of the owner's), so an owner may be destroyed right after `drop_owner`. `in_flight(owner)`
  stays, for the status and the tests (cancel then destroy).

### Shared residency and the retire path

- **The map.** Resident textures by (pack key, kind, first, count): one committed resource, its byte size (allocation
  info), the tickets holding it (a reference count), the last present it was asked for. A request for a resident entry
  takes a reference and is ready at once, no copy. A request for one being copied joins it and is ready with it. Two
  owners asking the same key and levels share one resource; a zone and a character asking different levels get two
  (rare: zones and models are mostly different DATs).
- **References.** Each ready ticket holds one. `drop` of the last moves the resource out of the map at once (a later
  request streams it again, into a new resource) and hands it to `retire::later` (retire.hpp:69): lists recorded with
  it are on the game's direct queue, so the game queue's fence is the right one, by the +2 rule (retire.hpp:63).
- **What never was ready** (a ticket dropped while reading or copying, a missed CRC after the resource was made): only
  the copy queue ever saw it, so the worker releases it itself once its copy fence has passed, never through retire.
- **The ring, queue, allocators, lists and fence** are used by the copy queue only: released by the worker once the
  copy fence has passed everything, or left (leaked on purpose, logged) if that doesn't happen within the wait; never
  through retire (wrong queue).
- **Calls into retire are made with no streamer lock held**: what is let go under the lock is collected and handed over
  once it's left (the characters' `dead_things` pattern, cache.cpp:155-186). No `retire::then` is needed.
- **Eviction is the owner's**: the streamer never takes a texture away from a ticket. Under video memory pressure it
  says so (`pressure()`), and the characters' cache drops its least recently drawn pack textures back to the game's
  (M5).
- There is no `src/pack/retire.cpp` and there won't be one: `src/host/retire.hpp` is the retire list.

### Index snapshots

- `open(folder, client)` (client: the game's fingerprint, M6) and `close()` are posted to the worker: `pack::load` (pack.cpp:175) runs there, never on the game's
  thread (4.5 MB read and CRC-checked). The snapshot is published as a `std::shared_ptr<const index>` under the
  streamer's mutex; `current()` copies it (plan §4 "Snapshot and loading"). Each open, close or reload raises the epoch.
- The worker reads data files through one `pack::reader` per snapshot. `close()` cancels its reads and closes its
  handles (`reader::close`), so the folder can be renamed or deleted while the game runs.
- Tests use `open(std::shared_ptr<const index>)` to stream from an index they built (the stalled pipe).

### Locks

- **One mutex**, `g.mutex` in stream.cpp: the request table, owners, the residency map, the snapshot pointer, the limits
  and the pause bits. Held only for queue work. **Never across** `ReadFile`, a D3D12 or DXGI call (`Create*`,
  `GetCompletedValue`, `ExecuteCommandLists`, `Signal`, `SetEventOnCompletion`, `CreateShaderResourceView`,
  `Release`), a fence wait, `pack::load` or a call into retire (cache.cpp:9-17's rule, threads-pso.md).
- **A leaf.** The streamer calls back into nobody: no callbacks, no owner code. So an owner may call it with its own lock
  held (owner lock, then the streamer's), never the other way round.
- `reader`'s own mutex guards its handle list only (pack.hpp:273) and is never held across a read.
- The crash section takes no lock (below).

### Budget, address-space floor, quiet window

- **Limits from the module.** `set_limits(limits)` on the game's thread at each `update()` (every 0.5 s): video memory
  used and its budget (`gpu_crash::video_memory`, gpu_crash.hpp:53, its adapter kept: game thread only),
  `[textures] budget_mb`, the address space free and the largest free block (`memwatch::read()`), the per-present cap.
  The worker never calls DXGI and doesn't read memwatch itself: one input, which tests set directly.
- **Video memory, checked before each texture is made:** the planned size is `GetResourceAllocationInfo` of the desc.
  Usage is estimated as the last published usage plus what the worker made since it was published (the usage lags).
  Over 85% of the budget the texture is made without its top level (`first + 1`, if that level is a multiple of 4 and
  more than one level is left), counted "skipped top level". Over 95% nothing new is made: the worker sets the pause bit
  `kVideoMemory`, the ticket stays queued (the owner keeps drawing the game's texture), and `pressure()` is true.
- **Address space:** below 512 MB free or a 64 MB largest block (plan §5's starting values) the worker sets
  `kAddressSpace` and makes nothing new until both are above again. Zero (not measured yet, the tests) means no limit.
- **Pause bits** (a `uint32_t`): set by callers: `kOff` (the option), `kSafeStart` (`backend::safe_start()`),
  `kQuiet` (the module from a zone change until feed's zone-in plus `zone_delay_s`: pack work stays off the zoning
  critical path), `kTest`; set by the worker: `kVideoMemory`, `kAddressSpace`, `kLost`. While any is set the worker
  starts nothing new: copies already submitted finish and become ready; reads in progress finish (pieces of 4 MB).
- The streamer knows nothing about zones: the quiet window is the module's.

### Pacing

- At most 4 copies in flight (`kInFlight`): a fifth texture waits for the oldest to pass the fence.
- At most `per_present_mb` (64) of record bytes started per present: `on_present()` (the module's present, game thread)
  raises an atomic present counter and wakes the worker; the worker counts the bytes it has started since the counter
  last changed and waits for the next present once over. A record bigger than the cap is the only one of its present.
  Before the first `on_present` (and in tests that set it so) there's no cap.
- One list per texture: recording and submitting is 0.04 ms, so batching small textures would buy nothing.

### Device loss

- The module calls `device_lost()` when `gpu_crash::lost()` is set (backend.cpp:763 stops presents then); the worker
  also takes its own fence reading `UINT64_MAX` as loss.
- On loss the worker sets `kLost`, submits nothing more, never waits on a fence again, marks every queued, reading and
  copying ticket missed, releases what it made (the device is gone: nothing runs on it) and idles until `stop()` or a new
  device. Resident textures are left to the owners' drops; retire frees them at once (the fence reads `UINT64_MAX`).
  (As built: what a copy not seen done may still use is left, not released; and resident textures go to the retire
  list with the epoch change. "As built" below.)
- A new device (`set_device`): the epoch is raised first, so every old ticket reads `gone`; the worker waits at most 1 s
  for its copies on the old queue, then releases ring, queue and lists (or leaks them if the wait runs out), and makes
  new ones for the new device. Old resident textures go to `retire::later`; retire lets them go at once when it sees the
  new device (retire.hpp:121-124).

### Stop

- `stop()` (the module's `stop`, first in the plugin's Release: main.cpp:1019-1026, before `cache::stop` at :1050 and
  `retire::release_all` at :1066; plan §4 "Shutdown and device order"): sets quit, `reader::cancel()`s the read in
  flight (CancelIoEx), sets the stop event, raises the worker's priority and joins it. **Under 100 ms** even with a read
  that never finishes (0.16-0.85 ms measured for the cancel).
- The worker, on quit: submits nothing more; waits at most 50 ms for its copies in flight (four of the largest records
  take about 2 ms at 14 GB/s); hands the resident textures to `retire::later` (owners' lists may still read them);
  releases the ring, queue, lists and fence, or leaks them if the copies haven't finished; closes the reader; returns.
- **The cancel race in today's reader** (pack.cpp:290-310): `cancelled` is checked before each piece's `ReadFile`, and
  `cancel()` sets it and then calls `CancelIoEx` on every handle. A cancel landing between the check and the `ReadFile`
  finds no I/O to cancel, and that piece (up to 4 MB) is read whole: on a slow disk, longer than 100 ms. Fix: after
  `ReadFile` returns `ERROR_IO_PENDING`, check `cancelled` again and, if set, `CancelIoEx(h, &ov)` from the reading
  thread itself. With `cancelled` a sequentially consistent atomic stored before `cancel()`'s CancelIoEx, either that
  CancelIoEx sees the pending read or the reader sees the flag.
- `stop()` with 1,000 tickets queued drops them all (no I/O for queued ones): same 100 ms.

### The crash and hang section

- New header `src/pack/stream_crumbs.hpp`: one static struct of `std::atomic` fields (64-bit ones lock-free on x86,
  `static_assert`ed as memwatch.cpp:23 does), read by anyone. As built, the worker's state, the last record, the fence
  and the ring are written by the worker (and `start`/`stop`); the counts, epoch and pause bits by whoever holds the
  streamer's mutex (`publish_locked`, also the callers' `request`, `drop`, ...):
  - started, worker state (idle, opening, reading, making, recording, waiting for the ring, waiting for the fence,
    paused, lost, stopped), pause bits, epoch;
  - queued, in flight, resident count and bytes, refused by reason;
  - the last record's key hash, data file, offset, bytes, first level and count;
  - the copy fence's last signalled value and the last completed value the worker read;
  - ring size, head and tail;
  - the tick of the last progress.
- `pack::crash_section()` (memwatch.cpp:369, registered at main.cpp:1012 and at the end of the hang report's lines)
  reads them and replaces today's "streamer: none yet" line. It takes no lock and calls no device or fence method: it
  runs on the report's thread pool thread with the game's thread perhaps stuck, maybe inside the streamer's mutex.
- The header has no code of stream.cpp's: memwatch.cpp includes it without linking the streamer, so in M3 (nothing
  starts the streamer) the section reads "streamer: not started".

## The API (src/pack/stream.hpp, sketch)

```cpp
namespace hifi::pack::stream
{
	using owner = uint32_t;
	struct ticket
	{
		uint32_t slot = 0, generation = 0; // (generation 0: no ticket)
	};
	enum class state : uint8_t { none, queued, reading, copying, ready, missed, dropped, gone };
	struct want
	{
		key k;
		uint8_t kind = kColour;
		uint8_t first = 0, count = 0; // levels: the top `first` never wanted; count 0: all the rest
		int32_t priority = 0;         // (higher first; then the order asked)
		uint32_t epoch = 0;           // (the snapshot's the key was found in)
	};
	struct view
	{
		ID3D12Resource *resource = nullptr; // (valid while the ticket is held)
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		uint32_t w = 0, h = 0, levels = 0, first = 0;
		uint64_t bytes = 0;
		uint32_t param = 0; // (M9: the record's param, a height record's span as float bits)
	};
	struct limits
	{
		uint64_t vram_usage = 0, vram_budget = 0, budget_cap = 0; // (0: unknown or none)
		uint64_t avail_virtual = 0, largest_free = 0;
		uint64_t per_present = 64ull << 20;
	};
	enum : uint32_t { kOff = 1, kSafeStart = 2, kQuiet = 4, kTest = 8, kVideoMemory = 0x100, kAddressSpace = 0x200, kLost = 0x400 };

	// (the module, game thread) The worker started with the device and ring size; set_device on a new device or nullptr
	bool start(ID3D12Device *device, uint32_t ring_mb);
	void set_device(ID3D12Device *device);
	void device_lost();
	void stop(); // (under 100 ms; joins)
	// The pack: loaded and checked on the worker; the index now (nullptr: none) and its epoch
	void open(const std::wstring &folder);
	void open(std::shared_ptr<const index> pack); // (tests)
	void close();
	std::shared_ptr<const index> current();
	uint32_t epoch();
	// Owners and their textures (any thread; no device call, no I/O, no wait)
	owner add_owner(const char *name);
	void drop_owner(owner o);
	ticket request(owner o, const want &w);
	state poll(ticket t); // (no lock)
	void take_done(owner o, std::vector<std::pair<ticket, state>> &out);
	bool get(ticket t, view &out);
	void drop(ticket t);
	uint32_t in_flight(owner o);
	// (the owner's thread, none of its locks held) The ticket's SRV into the owner's slot
	bool write_view(ticket t, D3D12_CPU_DESCRIPTOR_HANDLE slot);
	// Pacing, limits, pausing
	void on_present();
	void set_limits(const limits &l);
	void pause(uint32_t bits);
	void resume(uint32_t bits);
	bool pressure(); // (over 95% of the video memory budget: owners drop what they can)
	// Status
	struct counts { /* asked, hits shared, resident, resident_bytes, refused by reason, skipped top, bytes read, ms reading, ... */ };
	counts get_counts();
	std::vector<std::string> status(); // (cached strings)
}
```

## The worker's loop

1. Under the lock: if quit, leave. Pick the highest-priority queued ticket whose epoch is current, unless a pause bit
   is set, 4 copies are in flight, or this present's bytes are used up; else wait on the condition variable (woken by
   requests, `on_present`, `resume`, `stop`; at most 1 ms while copies are in flight, to poll their fence). Mark it
   reading; note the record (from the current snapshot) and the levels; copy the limits.
2. Outside the lock: `pack::check(r, device)`; the level range (refused if `first` isn't a multiple of 4, or the span is
   bigger than the ring); the video memory and address space checks; `CreateCommittedResource`.
3. Ring space: wait (fence event or stop event) for the oldest copies until the span fits.
4. `reader::read_levels(r, first, count, ring + at)`: the span in pieces of at most 4 MB, each level's CRC from the
   level CRC table. A cancel, short read or CRC mismatch: missed (the resource released at once: no copy ran on it).
5. Record: reset this slot's allocator once its fence value has passed; `GetCopyableFootprints` of the made desc, each
   footprint's offset moved to the ring; one `CopyTextureRegion` per level; `Close`; `ExecuteCommandLists`; `Signal`.
   Update the crumbs.
6. Under the lock: copying, its fence value. Then, outside it, read `GetCompletedValue()` once; under the lock, every
   copy it passed: ready (release-store), into the residency map with the references of every ticket waiting on that
   key; dropped ones collected for release; ring space freed. Release what was collected after unlocking.

## Tests (tests/packstream_test.cpp, x86, /LARGEADDRESSAWARE)

On WARP by default, on the GPU with `ZONE_GPU=1` (run_suites gets a `packstream_gpu` entry beside `zoneinject_gpu`),
with the debug layer, and GPU-based validation with `HIFIXI_GBV=1` (as zoneinject_test.cpp:206-217); every check counts
the debug layer's messages and wants 0. A made-up pack is written under `build\test\packstream` with `pack_write` (BC2,
BC7 and BC4 records, sizes 4x4 to 4096x4096); the real pack is used when present (`HIFIXI_PACK` or the bootloader path),
read only, and its lines say "skipped" otherwise. A direct queue stands in for the game's: `retire::on_present` is
called with it each test frame.

- 2,000 entries streamed and read back by checksum: each texture copied on the direct queue into a readback buffer
  (promotion to COPY_SOURCE, no barrier) and compared row by row with the record's bytes; a subset sampled by a compute
  shader through views `write_view` made in the test's own shader-visible heap (promotion to NON_PIXEL_SHADER_RESOURCE).
  On WARP in waves of at most 256 MB resident.
- Ring wraps with mixed sizes (an 8 MB ring); a span bigger than the ring missed as "bigger than the upload ring"; a
  `first` that isn't a multiple of 4 refused.
- Drop then destroy: 500 requests, the owner dropped mid-stream and its heap released at once; `in_flight` reaches 0,
  every ticket reads dropped, everything the worker made for them released (the debug layer's live objects at the end),
  0 messages.
- Pause and resume: nothing read while paused, everything ready after.
- The per-present cap: bytes started per `on_present` at most the cap, a record bigger than it alone.
- Device loss: `ID3D12Device5::RemoveDevice` on a second WARP device with copies in flight and a read pending: tickets
  missed, no wait, `stop()` under 100 ms.
- `stop()` under 100 ms with a read stalled on a named pipe (stall.cpp's trick), and with 1,000 tickets queued.
- A forced low budget: over 85% the texture comes without its top level; over 95% it stays queued (`pressure()`), then
  completes once the budget is raised.
- Evict and stream again: a character key dropped while a direct-queue list reading it is in flight, then asked again: a
  new resource, the old one let go only behind the retire fence, 0 messages under GBV.
- Two owners of one key share one resource; it goes when the second drops it.
- `crash_section()` from another thread while the test holds the streamer's mutex (a test hook): it returns within
  10 ms with the streamer's lines.
- Address space per 1,000 textures, hardware only: at most 16 MB for 1,000 random textures (measured 8 MB; WARP holds
  textures in the process and is skipped). And the streamer's address space at most ring + index + 5.2 KB per texture,
  rounded up to the driver's 8 MB steps.
- The ring's heap properties read back as CUSTOM, write-back, L0 (`GetHeapProperties`): WARP can't show a
  write-combined read, so this guard stands in for it.
- `PACKSTREAM_SOAK=<minutes>` (hardware, by hand, after the bake has ended): random requests, drops and cancels; the
  address space flat after the first minutes (within one 8 MB step).

Elsewhere: `pack_test` gains the reader's cancel race (a cancel set just before the read's pending ReadFile ends it) and
`read_levels`; `memwatch_test` expects "streamer: not started" in the section.

## Notes for M4 and M5 (not M3)

- Owners write views with `write_view` when `take_done`/`poll` says ready, then set their own flag; zones once per slot
  per `gpu_zone`, characters into a second pair.
- An epoch change (pack off, reload, device) is asked for on the game's thread (`close`, `open`, `set_device`), and
  every owner drops its pack views before it next records (zones clear their flags; characters
  `forget_all_textures`). As built in M3, an open, close or reload is done on the worker, whenever it gets there: that
  was when every resident texture went to `retire::later`, even with gone tickets still held (a gone ticket held no
  reference). Retire's +2 rule then covered the lists recorded up to the next `retire::on_present`
  (backend.cpp:766), not the frame after. So (M3's rules; since M4 part B a gone ticket keeps its texture, "As built"
  below, and they are no longer load-bearing, though the module still keeps the first):
  - the module clears the owners' pack views in the same update that calls `close` or `open` (pack off, reload),
    before anything more is recorded;
  - an owner reads its tickets (`take_done`, `poll`) after the last present's `retire::on_present` and before it
    records, and records no view of a ticket that isn't ready (a look from the module's present taken before
    backend::present's `retire::on_present` can miss an epoch change landing in between);
  - (an alternative for M4 to weigh: a gone ticket keeps its reference until the owner drops it, so the texture lives
    exactly as long as `view::resource` says, whatever the timing. Taken: docs/texture-pack/zones.md decision 1.)
- `gpu_zone::upload` today makes its resources on the game's thread (zone_gpu.cpp:128-271, from native.cpp:111): M4
  adds no pack work there; pack textures come only from the worker.
- The module: `stream::start` at the first enable with a device; `set_limits` and the pause bits in `update()`;
  `on_present` and `device_lost` in `present()`; `stop` in its `stop()`; `take_log`'s lines into hifixi.log from
  `update()` (plan §3: a miss at the worker is a log line; the streamer logs nothing itself).
  (Built in M4 part A, src/pack/pack_module.cpp: docs/texture-pack/zones.md "As built, part A".)
- Destruction points needn't wait for `in_flight(owner)` to read 0 (plan §4 "Zones" 8 and M4 step 5 still say they
  do): `drop_owner` and the owner may go at once (above). `in_flight` is for the status and the tests. Until then (M3) `PASSES_PACK` (build.cmd:78)
  stays as it is: the plugin doesn't link stream.cpp. (M4 links it: `PASSES_PACK` gains the streamer, the reader and
  `src/zone/zonepack.cpp`.)

## What goes onto the d3d12 pages (with the code)

- sync.md, "Where HiFi XI relies on this": the streamer's copy queue, fence and four allocators (stream.cpp lines);
  ready only after the CPU saw the copy fence; never-ready textures and the ring, queue and lists released by the worker
  behind its own fence (or leaked), published textures through `retire::later`; waits with a stop event and timeouts.
  And a rule: never read an UPLOAD heap's mapping from the CPU (write-combined: 57 MB/s on the 4070 Ti; WARP hides it).
- barriers.md, "Our own resources": pack textures made in COMMON, written once on the copy queue, read by promotion,
  never a barrier; the ring a CUSTOM write-back buffer made in COMMON.
- descriptors.md: the streamer has no shader-visible heap; `write_view` writes into the owner's slot on the owner's
  thread.
- threads-pso.md: the worker thread, its leaf mutex and what it never holds it across; below normal, raised for the
  join; `stop()` under 100 ms; the reader's cancel race and its fix.
- debugging.md: the texture pack section's streamer lines from `stream_crumbs.hpp`.

## As built (M3, 2026-11-02)

`src/pack/stream.hpp/.cpp`, `src/pack/stream_crumbs.hpp`, the reader's `read_levels` and cancel-race fix
(`src/pack/pack.cpp`), the crash section's streamer lines (`src/pack/memwatch.cpp`), `tests/packstream_test.cpp`
(build.cmd; run_suites runs it on WARP and as `packstream_gpu`). The plugin doesn't link stream.cpp (`PASSES_PACK`
unchanged). The rules went onto the d3d12 pages with anchors. Where the code differs from the design above, or found
something it didn't know:

- **Device loss leaves what a copy may still use.** The design released everything the worker made at once ("the
  device is gone: nothing runs on it"). On WARP a copy submitted before `RemoveDevice` went on running, and the test
  crashed releasing its ring and texture (4 of 6 runs). Now `lose_parts` releases only when every submitted copy was
  seen done before the loss; otherwise the in-flight textures, ring, queue and lists are left and counted (`leaked`).
  A lost device ends the session anyway. Ready textures still go to `retire::later`, which frees them at once (the
  fence reads `UINT64_MAX`): retire's own rule, unchanged.
- **The ring's address space outlives `stop()`.** On the 4070 Ti a mapped (or not) CPU-visible buffer's address range
  isn't given back on Release: the driver keeps one freed range and reuses it for the next of that size. A streamer
  stopped and started again holds two rings' worth (32 + 32 MB), then no more over any number of restarts
  (packstream_test's "va" child). So M4 starts the streamer once per session and keeps it (the pack turned off pauses
  it, `kOff`), as planned; a device change costs one ring's address space more, once. (`drop_parts` unmaps before
  releasing; it changes nothing measurable, kept as hygiene.)
- **The debug layer's own address space.** 1,000 textures took 16.5 MB of address space with the debug layer on and
  8.7 MB without it (the probe's 8 MB). The address-space checks run in a child process with the debug layer off.
- **Measured on the 4070 Ti, the real pack:** 1,000 random textures resident: 8.6-8.7 MB of address space; the
  streamer's whole address space with them 46.2-46.3 MB (ring 32 + index 4.3 + parts and bookkeeping), under the bound
  of ring + index + 5.2 KB a texture rounded up to 8 MB (48 MB). 2,000 random real records (581 MB) streamed and read
  back in 1.7 s (with the debug layer, the readback compare included).
- **Refused at the request, not by the worker:** a key or kind not in the pack, a no_pack entry, a first level past the
  chain or not a multiple of 4, a span bigger than the ring are missed at once in `request()` (pure CPU under the lock:
  `find`, `footprint`). The worker checks the record again (`pack::check` with the device) before it makes anything.
- **The per-present cap counts from the first `on_present` after `start()`**, not the first ever (a restarted
  streamer has no cap until the module's next present). Its own counts (`presents`, `present_max_bytes`,
  `present_alone`, `present_breaks`) are kept only while a cap applies.
- **An epoch change lets go of every resident texture at once** (to the retire list, the +2 rule covers lists already
  recorded); a gone ticket holds no reference, and dropping it only frees its slot. Textures on their way are left to
  their jobs, which release them behind the copy fence. So "Ready means" above holds only until the epoch changes:
  an open, close or reload is done on the worker at a time of its own, and owners keep the rule in "Notes for M4".
  (M3. Changed in M4 part B: the next bullet.)
- **Gone tickets keep their texture** (M4 part B, docs/texture-pack/zones.md decision 1; the critic's open point). An
  open, close or reload (`do_open`) ends the epoch in keep mode (`end_epoch_locked(lost, keep)`): each ready ticket
  turns gone but keeps `d.r` (`finish`'s keep), its texture leaves the residency map (a new request streams the key
  again, into a new texture) for a `held` list with its references, and the last drop of its tickets
  (`drop_locked`'s gone case) hands it to `retire::later` as any drop. So `view::resource` is valid exactly while the
  ticket is held, whatever the timing; `get` and `write_view` still answer only for a ready ticket. Stop, a device
  change and a loss end it in let-go mode: every ready texture, and every held one, to the retire list at once, the
  gone tickets' hold forgotten (their drop frees nothing twice). `counts` gains `held` and `held_bytes`, the status a
  line while any are held ("kept for owners that haven't let go"), the module's menu a note on the video memory line.
  A forgotten drop now costs video memory (shown), never a use-after-free. packstream_test "gone tickets keep their
  texture" (a reload: gone, held, refs alive after 4 presents, dropped: retired, alive one present, freed behind the
  fence; a close: both held; a device change lets go of the held one at once) fails 3 checks against the old do_open.
- **`want::down_to_w/down_to_h`** (M4 part B): with `count` 0 and both set, `request` counts the levels from `first`
  down to the first whose sides are at most them, from the record under the lock (zones ask down to the game's own
  size: a 2x record 2 levels, 4x 3, the game's own size 1; packstream_test "down_to"). The owner never looks entries
  up.
- **kAddressSpace** is evaluated by the worker at each pick from the last `set_limits` (only the limits drive it).
- **kVideoMemory, what `pressure()` reads, is a level** (M3 review): `set_limits` sets it when its numbers alone are
  over 95% of the budget, and keeps it while the texture the worker last refused still wouldn't fit under them; else
  clears it, and the worker tries the 95% check again. (As first built, only a refusal set it and every `set_limits`
  cleared it: with everything resident and the usage at 97%, `pressure()` read false, so M5's characters would never
  have dropped anything.)
- **Tickets:** slot words live in chunks of 1,024 that are never freed (poll reads them with no lock); a freed slot is
  reused only once 4,096 are waiting, so a dropped ticket reads `dropped` for a while before `gone`. `take_done` hands
  over the tickets that became ready, missed or gone (not the reading/copying steps), each once, in no particular
  order: an owner's list is a set of its live slots, which a slot given back leaves, so an owner that only polls costs
  nothing. An epoch change turns ready and waiting tickets gone (or missed, "lost") but leaves a missed one missed
  with its reason. (As first built, the list grew by one entry per ticket for an owner that never called `take_done`,
  and an epoch change handed a ready or missed ticket over a second time, the miss's reason overwritten.)
- **A device change, an open or stop starts nothing new** (M3 review): `pick_locked` checks them again under the
  mutex, since the worker lets it go between its loop's top and the pick. As first built, a `set_device` plus a request
  landing in that window copied the new epoch's ticket on the old device's queue and ring; `switch_device` then let
  them go and left the entry in the map, its ticket "copying" for good.
- **The worker's waits for its last copy loop on the fence's value** (`wait_copies`): the fence event is auto-reset
  and the idle wait's registration for the oldest copy, left behind when the stop event or a request woke it, set it
  early, so stop and a device change leaked what they would have released 1 ms later. A fence reading `UINT64_MAX`
  there counts only what was seen done before (as `lose_parts`). See sync.md.
- **`stop()` gives up an open loading `pack.idx`**: `pack::load` takes a cancel flag, checked before each 256 KB piece
  and each data file; `stop()` sets it. (A read that hangs inside one piece still holds it: only the reader's data
  reads are overlapped and cancellable.)
- **`write_view` holds the texture and device for its call**: AddRef'd under the mutex with the ticket checked,
  released after the view is made, so an epoch change on another thread (with retire freeing at once on a new device)
  can't free them under it.
- **A test hook holds copies on the GPU** (`test::gate_copies`: the copy queue `Wait`s on a test fence first), so
  packstream_test sees that nothing reads ready and no ring space is used again before a copy has run, and can stop or
  remove the device with copies held; another (`test::before_pick`) runs test code between the worker's loop top and
  its pick.
- **Waits:** the worker waits on its fence event, its wake event (requests, presents, resume, limits) and the stop
  event together, 1 s at most, rather than a condition variable with a 1 ms poll.
- **Completeness check (same day, before commit):**
  - A reload or close during a read missed its tickets as "a read failed": `open`/`close` cancel the old reader, the
    read ended "cancelled" before the worker got to the open, and `fail_job` counted it and left the tickets missed
    (an epoch change leaves a miss missed). Now `requeue_cancelled` puts them back in the queue while an open, close or
    stop is pending, and the epoch it ends makes them gone. packstream_test reopens the pack under a read stalled on a
    pipe (and fails with the fix undone).
  - Plan §3's "a miss plus a log line": the worker kept only the reason's kind and threw the reader's why away. Now
    each miss at the worker of a ticket still asking (rule, create, read, CRC; not a device loss) and the GPU parts not
    made are lines for `take_log` (at most 64 between calls, then a count), with the entry's name, key, data file,
    levels and the why. The module logs them (M4).
  - Plan §4's "`status()` reports the saving": `counts::shared_bytes`, video memory not taken twice now (each resident
    texture's bytes times its references past the first), in the status's first line.
  - A miss at the worker (a CRC mismatch, a data file that can't be opened) wasn't tested in packstream_test (only
    the reader's in pack_test): now it is, with the texture made for it released at once and the ring's space given
    back (a good texture between them comes).
- **An open or close waits for its own pass** (found in M4 part A): the worker's loop top took a pending open with a
  pending device change (or loss) and returned after the change, dropping the open; the module's `set_device` then
  `open` in one update never opened the pack (packmodule_test on the 4070 Ti). Now the post is taken only on a pass
  that isn't a device change or loss (stream.cpp:1365-1375); packstream_test makes both on the worker at once.
- **Whose open is done** (found in M4's review): an open still in `pack::load` isn't cancelled by a newer open or
  close (only `stop` cancels it), so it raises the epoch too, before the newer one does. A caller that took the
  first epoch raised as its own open's went on in an epoch the next open ended. `open` and `close` now return their
  number and `posts_done()` the last the worker has done (set with the epoch, under the lock, in `do_open`); the
  module waits for its own. `test::after_load` holds an open on the worker after its pack.idx is loaded
  (packmodule_test).
- **Tests not as planned:** device loss runs in child processes (D3D12 hands out one device per adapter and process:
  `RemoveDevice` on "a second WARP device" would remove the test's own); `hook_test` is gone (no hooks since step 5b);
  the plugin load/unload with 1,000 queued is `stop()` with 1,000 queued plus `retire::release_all` here, and `stop()`
  with four copies in flight (nothing left, the device's references back), the plugin's version in M4's test list. The
  soak (`PACKSTREAM_SOAK=<minutes>`, or `packstream_test soak`) runs with the debug layer off.
