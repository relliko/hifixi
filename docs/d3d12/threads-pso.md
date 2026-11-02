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
  holding those locks. HiFi XI hooks list Reset/barrier calls in the whole process, so our hook code runs inside
  someone else's lock.
- So: never hold one of our mutexes while calling into D3D12 or DXGI if any hook path can try to take that mutex from
  another thread. Either drop the lock before the call, or make the hook path not take it (see quiet threads below).
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

- `src/backend/backend.cpp:123-134`: `g_render_thread` is the thread our d3d8.dll records the game's frames on (set at
  the first frame mark). `elsewhere()` is true on every other thread.
- `src/backend/backend.cpp:137-140`: `live()` gives the core a list's data only on the game thread.
- `src/backend/backend.cpp:520-523`: `ev_reset` returns at once on any other thread, so no World FX or upscale lock is
  taken from a loader's or DXGI's list.
- `src/host/hooks.cpp:664-667`: `host::quiet` (scoped) and `quiet_thread()` (for good) make the hooks pass a thread's
  calls straight through. backend.cpp wraps its own D3D12 calls in `host::quiet q;` (e.g. lines 531, 538, 550).
- `src/framegen/fg_threads.h`: the hook AMD's presenter and interpolation threads call first.
  `FrameInterpolationSwapchainDX12.cpp:421` and `:511` call it; `src/backend/backend.cpp:979` sets it to
  `host::quiet_thread`.
- `src/framegen/framegen.hpp:37-40`: `framegen::present()` runs on the game thread with no HiFi XI lock held.
- `src/render/worldfx_core.cpp` holds World FX's `g.mutex`. The hook paths that used to take it from any thread
  (`on_reset`, `on_barrier`, `on_targets`, `on_clear_target`) are gone since step 4: World FX is entered only at the
  marks, the present and the game side's calls, all on the game's thread. Since step 4c the renderer records without
  `g.mutex`: `record()` in worldfx_core.cpp holds `g_render_mutex` instead while it records (the look's arrays copied
  first). Order: `g.mutex`, then `g_render_mutex`, never the other way (the recording lets `g_render_mutex` go before
  taking `g.mutex` again). Readers off the recording path that need the renderer take `g_render_mutex` after
  `g.mutex`; the crash report, the status and `gpu_times` only try it. `tests/worldfx_core_test.cpp` pins this with a
  probe: a feed call made from another thread while the blades record completes.

Lists, allocators, queues:

- `src/d3d8/gpu.hpp:12-27`: three frames in flight, each with its own allocator, upload pages and shader-visible
  descriptors. `src/d3d8/gpu.cpp:318-328` (`end_frame`) waits on the next set's fence before reusing its allocator.
- Own copy queues for loaders: `src/chars/cache.cpp:205-224` (the characters' worker: queue, allocator, list, fence
  of its own), `src/zone/native.cpp:172`, `src/zone/actordata.cpp:109`.
- `src/ui2d/render2d.cpp:702-704`: the 2D pass's own queue, allocator and list.

Shader and PSO creation:

- `src/host/shadercache.hpp:1-10, 103`: HiFi XI's own shaders' D3DCompile results kept on disk
  (`bootloader\hifixi-shader-cache`), keyed by source, entry, target, flags, defines and the build stamp. Folder set
  at `src/backend/backend.cpp:475`. Callers: chardraw.cpp:1430, fxrender.cpp:356, render2d.cpp:627, temporal.cpp:275,
  upscale.cpp:263, world_render.cpp:218, zone/draw.cpp:621, zoneshadow.cpp:192. These still compile on the game thread when nothing is cached.
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
