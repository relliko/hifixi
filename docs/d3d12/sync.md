# Queues, fences and object lifetime

HiFi XI runs work on several Direct3D 12 queues: the game's direct queue (made by our d3d8.dll), our own copy queues
for zone, model-actor and character uploads, a direct queue for 2D uploads, and AMD frame generation's present and
async compute queues. This page says what orders work between them, and when a GPU object may be released. Read it
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
- Never wait with `INFINITE` where a hung GPU or a lock held by another thread can stop the fence: give it a timeout
  and decide what to leak.

Object lifetime

- Do not release a resource, heap, PSO, root signature, query heap or allocator while any submitted list that used it
  may still run. Releasing is only safe once every queue that used it has passed a fence signalled after its last use.
  One queue's fence covers only that queue.
- A descriptor slot is the same: do not overwrite a shader-visible descriptor that a list in flight may still read.
- An upload buffer lives until the copy that reads it is done, on the queue that ran the copy. Microsoft's ring
  buffer example (fence-based resource management) reuses upload memory only once a fence says the frame is done.

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
  frees what that fence has passed. It is called from `backend::present` (src/backend/backend.cpp:1044), skipped once
  `gpu_crash::lost()` (src/backend/backend.cpp:1040).
- Ashita calls the plugin's present before our d3d8.dll submits the frame. So the signal made at present N sits in the
  queue before frame N's lists. An object handed over during frame N is freed at `signalled + 2`
  (src/host/retire.hpp:63): the next present's signal is the first one behind every list that may use it.
- New device: what the old device held is freed at once (src/host/retire.hpp:112). Device removed: the fence reads
  `UINT64_MAX` (src/host/retire.hpp:136), so everything is freed.
- At unload `retire::release_all` (src/host/retire.hpp:156, called at src/plugin/main.cpp:1116) signals once more and
  waits at most 2 s. What was handed over since the last present may be on the open list, so it is leaked on purpose,
  never freed under it (src/host/retire.hpp:183).
- Users: zone slots (src/zone/native.cpp:60), model-actor DATs (src/zone/actordata.cpp:98, :120), effect textures and
  upload buffers (src/fx/fxtextures.cpp:196, :350), effect distortion targets (src/fx/fxrender.cpp:494), character
  objects (src/chars/cache.cpp:136, :1012-1015).
- retire.hpp covers the game's direct queue only. Anything that ran on another queue must already be finished (CPU
  wait or fence check) before it is handed to `retire::later`.

The zone's copy queue (src/zone/zone_gpu.cpp)

- The copy queue is made in src/zone/native.cpp:172 (model actors: src/zone/actordata.cpp:109). Resources are created
  in COMMON (src/zone/zone_gpu.cpp:159-170) so the copy queue can write them and the direct queue can read them after
  decay.
- `_copying` (src/zone/zone_gpu.cpp:212-215): 1 once the list is submitted, 2 once its fence signal is queued.
- `ready()` (src/zone/zone_gpu.cpp:249) is the cross-queue order: the zone is drawn only after the CPU saw the copy
  fence reach 1 (src/zone/native.cpp:190). There is no GPU `Wait`.
- `release()` (src/zone/zone_gpu.cpp:92) waits up to 5 s for a running copy, because the game's fence says nothing
  about the copy queue. If the copy is still running or its signal never got queued, the objects are leaked, not freed.

The characters' cache worker (src/chars/cache.cpp)

- Its own copy queue, allocator, list and fence (src/chars/cache.cpp:196-224), used from the worker thread only.
- Every batch waits for its fence, at most 10 s (src/chars/cache.cpp:437-452). Only after that are entries marked
  ready for the game thread, and the allocator is reset only at the start of the next batch (src/chars/cache.cpp:403).
- When the device goes, the queue and fence go through `retire::later` (src/chars/cache.cpp:1012-1015). That is safe
  because nothing is left running on the copy queue.

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
