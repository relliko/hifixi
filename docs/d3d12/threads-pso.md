# Threading, command lists and pipelines

What may run on which thread in HiFi XI's Direct3D 12 code, why our own locks must never be held across a D3D12/DXGI
call that another thread also makes, and what pipelines (PSOs) cost. The process is 32-bit: no DXR, no ray tracing
pipelines (memory: no-dxr-32bit).

## The rules that matter here

Free-threaded and not:

- Device calls (Create*, CopyDescriptors, CheckFeatureSupport, ...) are made from several threads at once in
  Microsoft's own multithreading sample and in our loaders. The docs never put this on one page; treat the device as
  thread-safe and everything below as not.
- A command list is NOT free-threaded. Many lists may record at once, but each on one thread at a time.
- A command allocator is NOT free-threaded either, and only one list may be recording into it at a time.
  `allocator->Reset()` fails unless the GPU has finished every list recorded into it (wait on a fence first).
  `list->Reset()` is allowed while that list is still running on the GPU.
- Any thread may call `ExecuteCommandLists` on any queue. The runtime serialises the calls in the order they arrive.
  If two threads submit to one queue and the order matters, the app has to order them.
- One fence per timeline. A fence may be rewound: two threads (or two queues) signalling the same fence race, and
  whichever lands last wins, so the value can go down.
- Command list methods hold no reference to what they're given. Keeping a resource, heap or PSO alive until the GPU
  is past it is the app's job (fence, then release).
- Most list methods return nothing; their errors come back from `Close()`. Device removal can surface at any
  resource creation, Map, Present and `GetDeviceRemovedReason`.

Locks:

- Not in Microsoft's docs, but seen in our hang stacks: the runtime, the debug layer and DXGI take locks of their own
  inside calls. With the debug layer on, list calls (SetGraphicsRootSignature, Reset) waited on a debug-layer lock.
- DXGI's Present (also on AMD's frame generation threads) and the debug layer reset command lists of their own while
  holding those locks. Until step 5b HiFi XI hooked list Reset/barrier calls in the whole process, so our code ran
  inside someone else's lock; nothing is hooked now.
- So: never hold one of our mutexes while calling into D3D12 or DXGI if another thread can take that mutex while it's
  inside a D3D12 or DXGI call. Drop the lock before the call.
- Never call a swap chain method (GetDesc1, ResizeBuffers, Present) while holding one of our locks.

Recording on several threads:

- Each recording thread needs its own list and its own allocator per frame in flight. Microsoft suggests about
  `recording threads x frames in flight` allocators, reset once a fence says the GPU is past them.
- Lists recorded on several threads run in the order they reach the queue. A barrier's before-state must match the
  state the resource is in when the list runs, i.e. what the lists before it on that queue left.
- HiFi XI records the game's world on one thread (the game's). Loaders record only copy lists on their own copy queues.

Pipelines (PSOs):

- A PSO bakes the shaders, input layout, blend/raster/depth state, target formats and sample count into one object.
  Creating one makes the driver turn the shader bytecode into GPU code. That can take milliseconds (measure it:
  `lf.pso_ms` below).
- Make PSOs ahead (at start, or on a worker thread), not at the draw that first needs one.
- Two levels of cache: HLSL -> DXBC (our D3DCompile caches, below), and DXBC -> GPU code (the driver's own disk cache,
  plus the app's options: `GetCachedBlob` / `CachedPSO`, or an `ID3D12PipelineLibrary`).
- A pipeline library syncs itself, except two threads loading the same PSO at once. Its blob is refused with
  `D3D12_ERROR_DRIVER_VERSION_MISMATCH` (old driver or runtime) or `D3D12_ERROR_ADAPTER_NOT_FOUND` (other GPU): treat
  that as "empty, rebuild", never as an error. The library keeps a pointer to the blob, not a copy: the memory has to
  stay valid for the library's lifetime.
- Check `D3D12_FEATURE_SHADER_CACHE` for `D3D12_SHADER_CACHE_SUPPORT_LIBRARY` before using a library.

Present and frame latency:

- Flip model only (`FLIP_DISCARD` / `FLIP_SEQUENTIAL`). Back buffers must be in `COMMON` (`PRESENT`) when presented.
- Present blocks when too many frames are queued. The default limit is 3 frames (`SetMaximumFrameLatency`).
- A swap chain made with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` gives a handle to wait on before starting
  a frame. Its default latency is 1. The flag can't be added later by ResizeBuffers.
- Tearing (`ALLOW_TEARING` on the swap chain and in Present flags) needs the feature check, sync interval 0, and no
  exclusive fullscreen.

## Where HiFi XI relies on this

The game-thread-only rule:

- Since step 5b nothing of Direct3D 12's is hooked: HiFi XI's passes run only at our d3d8.dll's marks, which come on
  the game's thread (the one the game's Direct3D 8 calls come on), and at the plugin's present. No loader's, DXGI's
  or AMD's thread ever runs HiFi XI's code inside a D3D12 call. (`elsewhere()`, `host::quiet`, `quiet_thread` and the
  edit to AMD's FrameInterpolationSwapchainDX12.cpp that called it went with the hooks.)
- `src/framegen/framegen.hpp:37-40`: `framegen::present()` runs on the game thread with no HiFi XI lock held.
- `gpu_crash::video_memory` (src/render/gpu_crash.cpp, behind `memory_over` too) keeps one adapter under a static
  mutex held across DXGI's `QueryVideoMemoryInfo`: a leaf, taken only on the game thread (World FX's present,
  src/render/worldfx_core.cpp:2769, and the texture pack module's `update`). The texture pack's memory watch
  (src/pack/memwatch.cpp) has a thread of its own that makes no D3D12 or DXGI call at all (VirtualQuery,
  GlobalMemoryStatusEx, a process snapshot); it is joined in the module's `stop()`, raised above normal first
  (below normal, a probe still running waited seconds for a core while the CPU was busy, the game's thread joining).
- `src/render/worldfx_core.cpp` holds World FX's `g.mutex`. The hook paths that used to take it from any thread
  (`on_reset`, `on_barrier`, `on_targets`, `on_clear_target`) are gone since step 4: World FX is entered only at the
  marks, the present and the game side's calls, all on the game's thread. Since step 4c the renderer records without
  `g.mutex`: `record()` in worldfx_core.cpp holds `g_render_mutex` instead while it records (the look's arrays copied
  first). Order: `g.mutex`, then `g_render_mutex`, never the other way (the recording lets `g_render_mutex` go before
  taking `g.mutex` again). Readers off the recording path that need the renderer take `g_render_mutex` after
  `g.mutex`; the crash report, the status and `gpu_times` only try it. `tests/worldfx_core_test.cpp` pins this with a
  probe: a feed call made from another thread while the blades record completes.

- The texture pack's module (`src/pack/pack_module.cpp`) makes every streamer call on the game's thread: `start`,
  `open`, `close`, `pause`/`resume`, `set_limits`, `take_log`, `set_device`, `device_lost` and `on_present` take only
  the streamer's leaf mutex for queue work and never wait on the worker (an open or close is posted and returns its
  number; the module reads `posts_done()` at its next updates to see its own done, then `epoch()`/`current()`,
  `check_open` :278). `stop` alone joins (under 100 ms), in
  the module's stop, first in the plugin's Release. The module's own lock (`g_text_mutex`) only guards the cached
  status and menu strings: held to copy them, never across a streamer, D3D12 or DXGI call, so the menu (drawn on the
  game's thread) reads them with no file, DXGI or streamer call (menu_test and packmodule_test count the module's).
  `gpu_crash::video_memory` (DXGI) is called from the module's `update` with no lock of ours held.
- The texture pack's zones' glue (`src/zone/zonepack.cpp`, the module's consumer) makes its streamer calls on the
  game's thread too, with no lock of ours held (it has none): `add_owner`, `request` (at most 128 a present, :303),
  `take_done` (:329), `get` and `write_view` (:183: `CreateShaderResourceView` into the zone heap's pack slot), `drop`,
  `drop_owner` (the drop hook, :89). The hook also runs from `gpu_zone::release()` when retire's `on_present` frees a
  zone (retire's mutex isn't held then: src/host/retire.hpp, `then`'s rule) and from native's and actordata's
  `forget_device`; it only takes the streamer's leaf mutex and hands what it lets go to `retire::later` after leaving
  it. zonenative_test measures the worst of those presents with copies in flight.
- The texture pack's streamer (`src/pack/stream.cpp`; started by the module above, once a session): one worker thread
  (`worker`, :1334), below normal (:1336), raised above normal in `stop()` before the join (:1562, memwatch's lesson).
  Its mutex `g.mutex` (:150) is a leaf, held only for the ticket table, owners, the residency map, the snapshot
  pointer, limits and pause bits: never across ReadFile, any D3D12 or DXGI call (`Create*`, `GetCompletedValue`,
  `Signal`, `SetEventOnCompletion`, `ExecuteCommandLists`, `CreateShaderResourceView`, `Release`), a fence wait,
  `pack::load` or a call into retire. One exception, `write_view` (:1872): it AddRefs the texture and the device under
  the mutex (an interlocked count, no lock of the runtime's), so an epoch change on another thread can't free them
  before its `CreateShaderResourceView`; their `Release` comes after the mutex is left. It calls back into nobody, so
  an owner may call it with its own lock held (owner lock, then the streamer's), never the other way. `poll()` takes
  no lock (:1784). `stop()` cancels the reader's read (CancelIoEx), and an open loading `pack.idx` at its next 256 KB
  piece (`g_cancel_open`, :1554; `pack::load`'s cancel), sets the stop event and joins: 0.9-6 ms measured, also with a
  read stalled on a pipe and with 1,000 queued; 18-21 ms with four copies in flight held until 5 ms in; 46-50 ms with
  an open reading a slow `pack.idx` at 40 ms a piece (packstream_test, WARP and the 4070 Ti, also under GPU-based
  validation).
- What the worker reads at its loop's top (a device change, an open, stop) can change before it picks the next
  texture: the mutex is let go in between (it makes its parts, sees copies done). So `pick_locked` checks them again
  under the mutex and starts nothing while one is pending (:1159); packstream_test calls `set_device` and `request`
  on the worker in that window (`test::before_pick`). A device change (or a loss) and an open or close waiting
  together at the loop's top: the change is done on this pass and the open or close left posted for the next
  (:1359-1369). Until M4 the loop took both and returned after the change, so the open was dropped: the module's
  `set_device` then `open` in one update left the pack unopened for good on the 4070 Ti (packmodule_test's device
  change; packstream_test now makes both on the worker in that window).
- The reader's cancel race (`src/pack/pack.cpp:317`, the fix :334-337): `cancel()` sets the flag and then CancelIoEx's every handle;
  a cancel landing between a piece's check of the flag and its ReadFile found nothing to cancel, and that piece (up
  to 4 MB, or forever on a pipe) was read whole. The reading thread now checks the flag again after a pending
  ReadFile and cancels its own read. pack_test's test window lands a cancel exactly there.

Lists, allocators, queues:

- `src/d3d8/gpu.hpp:12-27`: three frames in flight, each with its own allocator, upload pages and shader-visible
  descriptors. `src/d3d8/gpu.cpp:318-328` (`end_frame`) waits on the next set's fence before reusing its allocator.
- Own copy queues for loaders: `src/chars/cache.cpp:265-287` (the characters' worker: queue, allocator, list, fence
  of its own), `src/zone/native.cpp:180`, `src/zone/actordata.cpp:123`, the texture pack's streamer
  `src/pack/stream.cpp:534-540` (four allocators and lists, one per copy in flight).
- `src/ui2d/render2d.cpp:702-704`: the 2D pass's own queue, allocator and list.

Shader and PSO creation:

- `src/host/shadercache.hpp:1-10, 103`: HiFi XI's own shaders' D3DCompile results kept on disk
  (`bootloader\hifixi-shader-cache`), keyed by source, entry, target, flags, defines and the build stamp. Folder set
  at `src/backend/backend.cpp:397`. Callers: chardraw.cpp:1445, fxrender.cpp:356, render2d.cpp:627, temporal.cpp:275,
  upscale.cpp:253, world_render.cpp:218, zone/draw.cpp:651, zoneshadow.cpp:192. These still compile on the game thread when nothing is cached.
- `src/d3d8/compile.cpp:1-10, 316-324`: our d3d8.dll keeps every shader key met in `keys.bin` and compiles missing
  ones on up to 4 below-normal worker threads at device creation (`src/d3d8/device.cpp:233`). A shipped cache covers
  the first session.
- `src/render/grass_render.cpp:2081-2087`: World FX compiles its shader entries on every core but two at once.
  World FX's HLSL is compiled on a CreateThread worker (`shader_thread`, started by `start_shader_job` in
  worldfx_core.cpp, below normal; its helpers take its priority), cached in `worldfx-shader-cache`, and the worker
  pins the DLL (`FreeLibraryAndExitThread`). Since step 4d that includes the built-in shaders at attach when the cache
  hasn't them (the first start after a release): `renderer::init(..., shaders_now = false)` takes them from the cache
  or leaves the renderer not `ready()` till `check_shaders` hands them over at a present (polled at every present).
  Nothing draws and no zone's files are read till then; `worldfx_drawing()` stays 0, so the game keeps its own water
  and weather meanwhile.
- `src/d3d8/device_draw.cpp:759-818`: our d3d8.dll makes PSOs lazily at the draw that needs one, keyed by state, and
  times each (`lf.pso_ms`, `lf.psos`; PerfLog=1 reports them). No `CachedPSO` or pipeline library is used anywhere:
  only the driver's own cache saves the DXBC -> GPU step on the next start.

Present:

- `src/d3d8/present.cpp:66-101`: FLIP_DISCARD, 2-3 buffers, tearing when supported. No waitable object and no
  `SetMaximumFrameLatency`: DXGI's default queue applies, on top of our three frames in flight.
- `src/d3d8/present.cpp:272-281`: windowed presents use interval 0; fullscreen takes the game's presentation
  interval. `ALLOW_TEARING` only at 0.

## Mistakes this project made

- **2026-10-21, World FX shader hot-load froze the game.** All 19 entry points compiled at O3 on the present thread,
  with the mutex held: about 11.5 s. Windows marked the window not responding and the game was closed. Fix: compile on
  a worker thread, cache by key (memory: worldfx-shader-hotload-freeze).
- **2026-10-29, frame generation hang a few seconds into the world.** `framegen::prepare()`, holding upscale's lock,
  called AMD's swap chain `GetDesc1` (DXGI's swap chain lock). DXGI's present work held that lock and reset a list
  through our hooks, which waited for upscale's lock. Fix 2847014: no swap chain calls or waits under HiFi XI's locks;
  the size comes from creation and each present; turning generation on/off happens in the game thread's present.
- **2026-10-30, 27 s of World FX compiling one shader after another at start.** Fix dda94d4: all cores at once.
- **2026-11-01, seconds of frozen title screen at every start.** The native renderers compiled their HLSL on the game
  thread each boot (about 4 s, 2.3 s in the zone renderer). Fix aaaf1ee: `shadercache.hpp`. A progress bar can't be
  drawn while a synchronous compile blocks the frame.
- **2026-11-02 01:31, deadlock with GPU-based validation on (not a GPU hang).** Game thread: `on_frame_mark` ->
  `worldfx::on_world_end` holding `g.mutex` -> grass draw -> SetGraphicsRootSignature -> waits on the debug layer's
  lock. AMD FSR presenter thread: DXGI Present (holding the debug layer's lock) -> DXGI's own list Reset -> our
  `hk_reset` -> `worldfx::on_reset` -> waits on `g.mutex`.
- **2026-11-02 02:19, the same deadlock from another thread.** The characters' loader's upload went through the debug
  layer -> an internal list Reset -> `hk_reset` -> `g.mutex`, while the game thread held `g.mutex` and waited on the
  debug layer. Fixing only AMD's threads was not enough. Fix 0072203: list events from any thread but the game's go no
  further (`elsewhere()`), and AMD's threads are quiet. A 10-minute validation run with frame generation on was clean.
- **2026-10-26, 0.36 mesh shadows: a device call made under the hooks' mutex deadlocked with the debug layer.** Fix:
  the zone shadow got a mutex of its own (memory: hifixi-mesh-shadows).
- Lesson from all three: the debug layer makes lock inversions that a release run hides. Test threading changes with the
  debug layer and GBV on (`zoneinject_test` with `HIFIXI_GBV=1`).

## Sources

- https://learn.microsoft.com/windows/win32/direct3d12/recording-command-lists-and-bundles
- https://learn.microsoft.com/windows/win32/direct3d12/executing-and-synchronizing-command-lists
- https://learn.microsoft.com/windows/win32/direct3d12/user-mode-heap-synchronization
- https://learn.microsoft.com/windows/win32/direct3d12/porting-from-direct3d-11-to-direct3d-12
- https://learn.microsoft.com/windows/win32/direct3d12/design-philosophy-of-command-queues-and-command-lists
- https://learn.microsoft.com/samples/microsoft/directx-graphics-samples/d3d12-multithreading-sample-win32/
- https://learn.microsoft.com/windows/win32/direct3d12/managing-graphics-pipeline-state-in-direct3d-12
- https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device1-createpipelinelibrary
- https://learn.microsoft.com/windows/win32/api/d3d12/nn-d3d12-id3d12pipelinelibrary
- https://learn.microsoft.com/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains
- https://learn.microsoft.com/windows/win32/api/dxgi/nf-dxgi-idxgidevice1-setmaximumframelatency
- https://learn.microsoft.com/windows/win32/direct3ddxgi/variable-refresh-rate-displays
- https://microsoft.github.io/DirectX-Specs/
