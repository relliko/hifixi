# Queues, fences and object lifetime

HiFi XI runs work on several Direct3D 12 queues: the game's direct queue (made by our d3d8.dll), our own copy queues
for zone, model-actor and character uploads and the texture pack's streamer, a direct queue for 2D uploads, and AMD
frame generation's present and async compute queues. This page says what orders work between them, and when a GPU object may be released. Read it
before adding a queue, a fence, an upload path or a `Release()` of anything a command list may still use.

## The rules

Queues and order

- To the app, a queue finishes its work in the order it was submitted (the GPU may overlap it inside). Two queues
  have no order between them at all. A copy queue can run before, during or after a direct queue's frame unless a fence says otherwise.
- `ExecuteCommandLists` may be called from several threads on one queue; the runtime serializes the calls in the order
  they arrive. That is order of submission, not of our intent: two threads racing give either order.
- Copy queues take copy lists only; compute queues take compute and copy work; direct queues take everything.

Signal and Wait

- `queue->Signal(fence, v)` sets the fence to `v` once everything submitted to that queue before it has finished on
  the GPU. It says nothing about any other queue.
- `queue->Wait(fence, v)` makes that queue (on the GPU) hold its later work until the fence reaches `v` or more. The
  CPU does not block. This is the only GPU-side way to order one queue after another.
- `fence->Signal(v)` from the CPU sets the value at once.
- A wait passes at "value or greater". Fences can be rewound (a lower value written later), and a racing pair of
  signals leaves either value. Keep one fence per timeline, one writer, values only going up.
- A CPU check is a valid substitute for a GPU Wait: if the CPU saw `GetCompletedValue() >= v` before it records and
  submits the dependent list, the work behind `v` is already done.

CPU side

- `GetCompletedValue()` returns the fence's current value. After device removal it returns `UINT64_MAX`, so every
  "has it passed v?" test answers yes. Code that frees on that answer frees everything at once; decide whether that is
  wanted (it is in retire.hpp, it is not in framegen.cpp).
- `SetEventOnCompletion(v, event)` fires the event when the fence reaches `v`. With a null handle it blocks until
  then. It is safe from several threads at once. Our habit: test `GetCompletedValue()` first and skip the event.
- A registration stays until the fence reaches its value, even after the wait that made it returned for another
  handle (a stop or wake event, a timeout). One auto-reset event registered for several values is therefore set by
  whichever value comes first: after waking on it, read `GetCompletedValue()` again and loop until the value you
  need or the deadline (the streamer's `wait_copies`).
- Never wait with `INFINITE` where a hung GPU or a lock held by another thread can stop the fence: give it a timeout
  and decide what to leak.

Object lifetime

- Do not release a resource, heap, PSO, root signature, query heap or allocator while any submitted list that used it
  may still run. Releasing is only safe once every queue that used it has passed a fence signalled after its last use.
  One queue's fence covers only that queue.
- A descriptor slot is the same: do not overwrite a shader-visible descriptor that a list in flight may still read.
- An upload buffer lives until the copy that reads it is done, on the queue that ran the copy. Microsoft's ring
  buffer example (fence-based resource management) reuses upload memory only once a fence says the frame is done.
- Never read an UPLOAD heap's mapping from the CPU. It is write-combined on the GPU: the 4070 Ti read it back at
  57-59 MB/s against 2,400 MB/s for cached memory (2026-11-02). Memory the CPU writes and then reads (a CRC over what
  was just read from disk) belongs in a CUSTOM heap with `CPU_PAGE_PROPERTY_WRITE_BACK` (on a discrete GPU, in L0).
  WARP hides this: its UPLOAD memory is cached.
- A mapped CPU-visible buffer's address space isn't given back on Release on the 4070 Ti: the driver keeps one
  freed range and reuses it for the next of that size (a 32 MB ring made and released in a loop takes 32 MB once;
  the streamer stopped and started again holds two rings' worth, then no more). Make such buffers once, keep them.

Command allocators and lists

- `allocator->Reset()` only when the GPU has finished every list recorded on it. Doing it earlier is undefined
  behaviour; the debug layer only warns when it cannot show the GPU is done. It returns E_FAIL while a list on it is
  still recording.
- One list records on one allocator at a time. Never `Reset` one allocator from two threads.
- A list may be `Reset` and re-recorded right after `ExecuteCommandLists`; its memory belongs to the allocator.
- `ExecuteCommandLists` drops the whole call if a list was not closed, its allocator was reset since recording, an
  earlier execution of the same list has not finished, or its barriers' before/after states do not match.

Resources shared between queues

- Resource states belong to a queue type class: direct and compute share one, copy is the other. COPY_DEST on a copy
  queue is not COPY_DEST on a direct queue.
- A resource used first on a copy queue should be created in COMMON. On a copy queue everything decays back to COMMON
  when the GPU finishes that `ExecuteCommandLists` (never between lists of one call). Buffers and simultaneous-access textures decay on any queue, and so does a
  texture that was implicitly promoted to a read state.
- A non-simultaneous texture in COMMON may be promoted implicitly only to a shader-resource or copy state; anything
  else needs a barrier.
- One writer at a time per resource. Another queue may read or write the bytes only after a fence says the write is
  done.

Not available here

- This is a 32-bit process: no DXR (memory no-dxr-32bit.md). Nothing on this page involves ray tracing queues.

## Where HiFi XI relies on this

The +2 rule (src/host/retire.hpp)

- `retire::on_present` (src/host/retire.hpp:97) signals one fence on the game's direct queue at every present and
  frees what that fence has passed. It is called from `backend::present` (src/backend/backend.cpp:766), skipped once
  `gpu_crash::lost()` (src/backend/backend.cpp:763).
- Ashita calls the plugin's present before our d3d8.dll submits the frame. So the signal made at present N sits in the
  queue before frame N's lists. An object handed over during frame N is freed at `signalled + 2`
  (src/host/retire.hpp:63): the next present's signal is the first one behind every list that may use it.
- What isn't a COM object or an owner goes as a closure: `retire::then(fn)` (src/host/retire.hpp:99) runs `fn` by the
  same rule, on the game thread inside `on_present`, outside the list's own lock (so `fn` may take a lock of its own,
  as long as `on_present` and `release_all` are never called with that lock held).
- New device: what the old device held is freed at once (src/host/retire.hpp:124). Device removed: the fence reads
  `UINT64_MAX` (src/host/retire.hpp:148), so everything is freed.
- At unload `retire::release_all` (src/host/retire.hpp:168, called at src/plugin/main.cpp:1075) signals once more and
  waits at most 2 s. What was handed over since the last present may be on the open list, so it is leaked on purpose,
  never freed under it, and a `then` closure is left unrun (src/host/retire.hpp:195).
- Users: zone slots (src/zone/native.cpp:66), model-actor DATs (src/zone/actordata.cpp:69, from :112 and :141), effect textures and
  upload buffers (src/fx/fxtextures.cpp:196, :350), effect distortion targets (src/fx/fxrender.cpp:494), character
  objects (src/chars/cache.cpp:225-226, :1764-1769), and the characters' descriptor pairs through `retire::then`
  (src/chars/cache.cpp:227-236): a pair let go goes back to `free_slots` only behind the fence, and only while
  `heap_generation` still matches (`make_gpu` and `stop()` make `free_slots` again for a new heap, so a late pair from
  the last heap would otherwise be handed out twice). The characters' texture pack tickets go the same way: `bury`
  hands them to the drop hook (`stream::drop`, src/chars/cache.cpp:221-223) beside the pairs, and the streamer hands a
  texture to `retire::later` at its last ticket's drop.
- World FX's shadow map copy as the native passes use it (`src/render/sun_shadows.hpp`): the published block holds one
  reference (`publish`, :93, the next publish or `release()` lets it go), and `latest` (:129) hands each pass one more,
  which the pass gives to `retire::later` right after binding it (`src/zone/draw.cpp:1049`, `src/chars/chardraw.cpp:1812`):
  a renderer World FX remakes, a device change or a stale block let go never frees the copy under a recorded list. The
  plugin's unload lets the block's own go before `retire::release_all` (`src/plugin/main.cpp`).
- retire.hpp covers the game's direct queue only. Anything that ran on another queue must already be finished (CPU
  wait or fence check) before it is handed to `retire::later`.

The zone's copy queue (src/zone/zone_gpu.cpp)

- The copy queue is made in src/zone/native.cpp:180 (model actors: src/zone/actordata.cpp:123). Resources are created
  in COMMON (src/zone/zone_gpu.cpp:196-208) so the copy queue can write them and the direct queue can read them after
  decay.
- `_copying` (src/zone/zone_gpu.cpp:253-256): 1 once the list is submitted, 2 once its fence signal is queued.
- `ready()` (src/zone/zone_gpu.cpp:314) is the cross-queue order: the zone is drawn only after the CPU saw the copy
  fence reach 1 (src/zone/native.cpp:198). There is no GPU `Wait`.
- `release()` (src/zone/zone_gpu.cpp:111) waits up to 5 s for a running copy (:118), because the game's fence says
  nothing about the copy queue. If the copy is still running or its signal never got queued, the objects are leaked,
  not freed.
- The texture pack's textures a zone draws (its pack region, docs/texture-pack/zones.md) are the streamer's, held by
  the zone's tickets. They are let go with the zone, at the same fence: native and actordata call
  `gpu_zone::pack_let_go()` (src/zone/zone_gpu.cpp:425) just before each `retire::later` of a gpu_zone
  (src/zone/native.cpp:65, src/zone/actordata.cpp:68), which runs the glue's drop hook (`drop_zone`,
  src/zone/zonepack.cpp:80: `stream::drop_owner`, every word spent), and the drop hands each texture whose last ticket
  it was to `retire::later` itself. A zone destroyed at once (a new device, `forget_all`, stop) runs the hook from
  `release()` (also inside `retire::on_present`'s free, where retire's mutex isn't held). Nothing waits: an owner may go
  right after `drop_owner`.

The characters' cache worker (src/chars/cache.cpp)

- Its own copy queue, allocator, list and fence (src/chars/cache.cpp:343-374), used from the worker thread only.
- Every batch waits for its fence, at most 10 s (src/chars/cache.cpp:599-615). Only after that are entries marked
  ready for the game thread, and the allocator is reset only at the start of the next batch (src/chars/cache.cpp:566).
- A batch is at most `kBatchBytes` of upload bytes (one entry bigger than that goes alone), and the worker holds at
  most one batch's textures decoded (`cache.hpp` `kBatchBytes`, `kEntryCap`): that bounds the upload buffer and the
  CPU copies in the game's 32-bit memory, not any GPU lifetime.
- When the device goes, the queue and fence go through `retire::later` (`stop()`, src/chars/cache.cpp:1883-1888). That is safe
  because nothing is left running on the copy queue.

The texture pack's streamer (src/pack/stream.cpp; docs/texture-pack/streamer.md; started by the module
src/pack/pack_module.cpp, docs/texture-pack/zones.md)

- Started once a session by the "textures" module at the first enable with a device (`begin_open`,
  src/pack/pack_module.cpp:319-335) and kept: turning the pack off closes it and pauses the streamer (`kOff`,
  `turn_off` :267-284), never stops it (its ring's address space isn't given back). `stop()` only in the module's stop
  (:696-723), first in the plugin's Release (src/plugin/main.cpp:1029-1035), before `retire::release_all` (:1075).
- Every `close`, `open` (on, reload) and `stop` the module makes is preceded in the same call by the owners'
  `clear_all` (each consumer's in turn, the zones' then the characters': src/zone/zonepack.cpp `clear_all`,
  src/chars/charpack.cpp `clear_all`; the loop :146-151; `begin_open` :249, `turn_off` :270, `stop` :700): no owner
  holds a ticket of the epoch the call ends when the worker gets to it. A device change: the owners cleared, then
  `set_device` (`device`, :778-798), the epoch the owners ask in read again at once.
- The zones' glue reads its tickets (`take_done`, src/zone/zonepack.cpp:329) and writes their views in the module's
  present (src/pack/pack_module.cpp:603-606, each consumer's frame in turn), before `backend::present`'s
  `retire::on_present` (src/backend/backend.cpp:766). That is safe because a gone ticket keeps its texture (below): a
  view's texture lives while its ticket is held, whatever the epoch does meanwhile.
- The characters' glue (src/chars/charpack.cpp, docs/texture-pack/characters.md "When tickets are read") reads its
  tickets there too (`take_done`, :309) and writes the pack pairs' views (:173, :179) before the cache marks them ready.
  Since M9 a texture may hold two tickets (its colour and its height, characters.md "Pack relief"): both kept by the
  same entry and let go together (`release_pack`, src/chars/cache.cpp:286: each to the drop hook from `bury`), so the
  rules below hold for each.
  The game's thread records a character with a pack pair only while its texture reads ready (`texture_view_of`,
  src/chars/cache.cpp:1209), and nothing of the streamer's is read at record time. The ticket keeps its texture until
  it's dropped; the drop (from `bury`, the game's thread or, for textures no list was recorded with since, the cache's
  worker) and the pair's `retire::then` both land at `signalled + 2`, behind every list recorded before. An epoch
  change is preceded by the characters' `clear_all` (`cache::forget_pack_textures`: every ticket dropped, every pair
  let go, the game's textures kept) as for the zones. charinject_test turns it off between a frame's recording and
  its run 50 times (with copies held on the GPU every fifth time): each frame as recorded, 0 messages under GPU-based
  validation. Eviction under the streamer's pressure (`cache::pack_evict`, src/chars/cache.cpp:1665-1700, from the
  glue's frame, src/chars/charpack.cpp:94) lets go the same way (`release_pack`: the pair to `retire::then`, the
  ticket to the drop hook, both from `bury` once the mutex is left), so a list recorded with an evicted pair before
  the eviction still runs with its view and texture; charinject_test evicts every texture between a frame's
  recording and its run: that frame as recorded, the next with the game's, 0 messages under GPU-based validation.

- Its own copy queue, one fence and four allocators and lists (`kInFlight`), made on the worker thread
  (src/pack/stream.cpp:537-543) and used by it alone. The fence is signalled only by the worker, after each
  `ExecuteCommandLists` (:1125-1126): one writer, values only going up. An allocator is reset only once the fence has
  passed the value after its last list (`list_value`, :1084, reset :1095); at most 4 copies are in flight
  (`pick_locked`, :1159).
- No GPU `Wait`: a ticket is stored ready (`memory_order_release`) only after the worker read the copy fence past its
  copy (`complete`, :765, :798), so any list the game's thread records after it saw ready is submitted after the copy
  finished. Same scheme as the zone's `ready()`. packstream_test holds copies on the GPU (below) and checks that no
  ticket reads ready meanwhile.
- Every wait on the copy fence is `SetEventOnCompletion` plus `WaitForMultipleObjects` with the stop event and a
  timeout: 1 s while running (:835-838, :1415-1417), 1 s at a device change and 50 ms at stop (`wait_copies`, :643).
  Never INFINITE, never under the streamer's mutex.
- `wait_copies` loops on `GetCompletedValue()` until the last copy's value or the deadline, never trusting the event
  alone (:643-663): the one auto-reset event is registered for the oldest copy by the idle wait and `wait_oldest`, and
  when the stop event or a request ends that wait the registration stays and fires later, so a wait for the last
  copy returned at the oldest one's. Before the M3 review that leaked the ring, queue, lists and four textures at
  about half the stops with copies in flight. A spurious wake of the idle wait or `wait_oldest` only costs a pass.
- What is released where: textures that were ready may be on the game's lists, so they go through `retire::later`
  (:450-453), after the mutex is left. What only the copy queue saw (a texture never ready, the ring, queue, lists,
  fence) is released by the worker once its own fence passed (:811, :893, `drop_parts` :503), never through retire
  (wrong queue). At stop or a device change, if the copies don't finish within the wait, they are left (counted
  `leaked`), not freed under the GPU.
- An epoch change made on the worker (`do_open`, :1244: an open, close or reload) happens whenever the worker gets
  there, while owners may still record views of resident textures. So it lets none of them go (`end_epoch_locked`,
  :401, keep, called at :1269): each ready ticket turns gone but keeps its texture (`finish`'s keep, :260), the texture
  leaves the residency map (a new request streams it again) for the `held` list, and the last drop of its tickets
  hands it to `retire::later` as any drop does (`drop_locked`'s gone case, :379). Its +2 rule then covers every list
  recorded before the drop (docs/texture-pack/zones.md decision 1). Stop, a device change and a loss let go at once
  (let-go mode, :1314, :1501, :697): the owners are cleared before stop, retire frees a device's objects at once when it sees
  the next device, and nothing runs on a lost one; a gone ticket's hold is forgotten then, so its drop frees nothing
  twice. Until M4 part B an open, close or reload let go at once too, and owners had to read their tickets after the
  last present's `retire::on_present` and stop recording views before calling `open`/`close` (a gone ticket held no
  reference); packstream_test's "gone tickets keep their texture" fails against that code.
- Device removed: the worker stops submitting and never waits again, and releases what the copy queue used only if it
  had seen every copy done before the loss. All three paths decide the same way: `lose_parts` (:670-677), and
  `wait_copies` (:652-653) for `switch_device` and `shut_down`, which can run first (stop() or set_device() right after
  device_lost()). A fence reading `UINT64_MAX` there means "removed", not "done": only `w.completed`, the last real
  value read, counts. A removed device needn't stop running what was submitted: WARP's `RemoveDevice` doesn't, and
  packstream_test crashed when the worker released a ring and a texture a copy still used. Otherwise they are left: a
  lost device ends the session anyway.
- A device change stops new work at once: `pick_locked` starts nothing while `set_device`, an open or stop is pending
  or the parts were made on another device (:1163), so no ticket of the new epoch is copied on the old device's queue
  and ring, which `switch_device` (:1283) then lets go. Each job still in flight there leaves the residency map with
  it (`let_flight_go_locked`, :616).
- The ring is a CUSTOM heap buffer, CPU write-back in L0 (:552-564), because the reader reads the CPU's copy back for
  its CRC.
- Tests only: `stream::test::gate_copies` makes the copy queue `Wait` on a test fence before each copy (:1124), the one
  GPU wait in the streamer. packstream_test holds copies there to see nothing ready and the ring not reused under a
  copy, and to stop or remove the device with copies held; it signals the fence from the CPU.

2D uploads (src/ui2d/render2d.cpp)

- A separate direct queue (src/ui2d/render2d.cpp:702) that also waits for each upload's fence, 10 s at most
  (src/ui2d/render2d.cpp:505-514, :743, :952).

Our d3d8.dll frame sets (src/d3d8/gpu.cpp)

- `kFrames = 3` (src/d3d8/gpu.hpp:27). Each `frame_set` (src/d3d8/gpu.hpp:133) has an allocator, upload pages, a share
  of the shader-visible heap and the fence value of its last submit.
- `end_frame` (src/d3d8/gpu.cpp:319) moves to the next set, waits for that set's fence (:328), then reuses its pages and
  descriptor range and resets its allocator (:343). Up to two frames are in flight.
- `submit` (src/d3d8/gpu.cpp:240) signals after every list; `run_deferred` (:277) frees what the queue has passed.
  `wait_for` (:257) waits `INFINITE`; after device removal the fence reads `UINT64_MAX` so it returns at once.

AMD frame generation (src/framegen)

- AMD's swap chain proxy makes its own present queue and async compute queue
  (src/framegen/ffx/src/backends/dx12/FrameInterpolationSwapchain/FrameInterpolationSwapchainDX12.cpp:735, :794), an
  interpolation thread and a presenter thread. It orders them against the game's queue with GPU `Wait`s
  (same file :1312, :1377, :1431, :1643, :1665).
- Our kits (contexts sized for a pair of resolutions) are retired behind a fence on the game's queue
  (src/framegen/framegen.cpp:300-303) and destroyed by `sweep` (:272), which never waits and leaves them alone once
  the fence reads `UINT64_MAX` (:277).
- The comment at src/framegen/framegen.cpp:300 assumes every list that used a kit was on the game's queue. AMD's
  interpolation may run on its async compute queue. Check that path is ordered behind the game queue's signal before
  relying on this.
- Nothing in framegen calls the swap chain or waits for the GPU while holding `gen.mutex`
  (src/framegen/framegen.cpp:65-70): DXGI's present and AMD's presenter thread take locks of their own. (Until step 5b
  they also called back into HiFi XI's list hooks.)

## Mistakes this project made

- Timers in place of fences. Before 6d321d5 (2026-11-02) zone data was kept 240 presents, effect uploads 2 s, and
  characters' meshes and the effects' distortion target were released at once (address reused, format change). The
  crash audit of 11-01 listed the instant `_dist` release and the 2 s upload timer as open faults. retire.hpp replaced
  all of them with one fence.
- A stale slot drew freed data. 0.33 (d7a1be1): a zone pass the renderer skipped left its ring slot published; the end
  marker drew the last zone's data, which the 240-present timer then let go. Lifetime and "which data is current" are
  separate checks; both are needed.
- One queue's fence trusted for another. The 6d321d5 review found that `gpu_zone` could be freed while its copy still
  ran on the copy queue. The game's fence does not cover that queue. Fixed by the wait in `release()`.
- Waits under locks. The first frame generation run (10-29) hung: `prepare()` held upscale's lock and called the swap
  chain, while DXGI's present held the swap chain's lock and went through our hooks for upscale's lock. Fixed in
  2847014: no swap chain calls or GPU waits under HiFi XI's locks. On 11-02 two GPU-validation runs deadlocked the
  same way: the game thread held World FX's `g.mutex` and waited on the debug layer's lock, while another thread
  (AMD's presenter inside DXGI's present, then the character cache worker) reset a list through our Reset hook and
  waited for `g.mutex`. Fixed in 0072203: lists from other threads never take World FX's lock.
- Leaked contexts. Contexts for sizes gone are retired behind the game queue's fence (2847014). But when the swap
  chain is made again the old kits are still left, not destroyed (src/framegen/framegen.cpp:135-137; memory
  hifixi-framegen.md lists it as open).
- Descriptor slots reused under lists in flight. Until M1 of the texture pack (2026-11-02) the characters' cache gave
  a forgotten mesh's or texture's pair of descriptors straight back to `free_slots`, and the worker wrote the next
  upload's views into it at once, while lists recorded with the old views could still run (the objects themselves
  already went through `retire::later`). Now the pair waits for the fence too (`retire::then`); charinject_test lets
  the Mithra go between a frame's recording and its run and checks a new texture gets another pair.
- Freed under a removed device's last copy (M3, 2026-11-02, packstream_test only). The streamer released its ring
  and textures at once on device loss, trusting "removed: nothing runs". On WARP the submitted copy went on running
  and the test crashed in 4 of 6 runs. Now what a copy not yet seen done uses is left (src/pack/stream.cpp:671-678).
  The M3 review found `switch_device` and `shut_down` still read `UINT64_MAX` as "done" when they ran before the
  worker handled the loss (stop() or set_device() right after device_lost()): a scratch run crashed 3 times in 22.
  Fixed in `wait_copies` (:652-653); the "lost-stop" and "lost-switch" children hold the copies on the GPU and check
  nothing is released.
- A stale fence event ended a wait early (M3 review, packstream_test only). `shut_down` and `switch_device` waited
  once on the auto-reset event, which an older registration set at the oldest copy, so they saw the last copy not
  done and leaked the ring, queue, lists and textures (9 of 20 probe stops). Now `wait_copies` loops on the value.
- Work started on the old device during a device change (M3 review). `pick_locked` didn't look at a pending
  `set_device`, so a ticket asked right after it could be copied on the old queue, and `switch_device` then left its
  entry in the residency map pointing at a released texture, the ticket "copying" for good. Fixed in `pick_locked`
  (:1163); packstream_test makes the change on the worker in exactly that window (`test::before_pick`).
- An open lost to a device change (M4, packmodule_test on the 4070 Ti). The streamer's worker took a pending open
  together with a pending `set_device` and returned after the device change, so the open was never done and the
  module waited for its epoch for good. Now the open stays posted for the next pass (src/pack/stream.cpp:1365-1375).
- Running on after the device was removed. Before 0.62 (a923d56) HiFi XI kept recording and submitting after a GPU
  hang and ended in "abnormal program termination". Now `gpu_crash::lost()` stops our passes, FSR and frame
  generation, and `retire::on_present` is no longer called.

## Sources

- Multi-engine synchronization:
  https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization
- Executing and synchronizing command lists:
  https://learn.microsoft.com/en-us/windows/win32/direct3d12/executing-and-synchronizing-command-lists
- ID3D12Fence::GetCompletedValue:
  https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue
- ID3D12Fence::SetEventOnCompletion:
  https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion
- ID3D12CommandAllocator::Reset:
  https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandallocator-reset
- Fence-based resource management:
  https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management
- Using resource barriers (implicit promotion and decay):
  https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12
