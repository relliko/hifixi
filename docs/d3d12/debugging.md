# Debug layer, GPU-based validation and DRED

How HiFi XI finds out what went wrong on the GPU: the debug layer and GPU-based validation (GBV) for offline runs
and the odd test session, DRED and our own breadcrumbs for crashes in normal play, and the hang report for a
frozen game thread. Read this before you read a crash report or change `src/render/gpu_crash.cpp`.

## The rules that matter here

Debug layer (CPU side):

- It checks every API call as it is made: bad arguments, wrong states named in barriers, descriptors and
  root signatures that don't match, objects released while the GPU still uses them, leaks.
- It must be turned on before `D3D12CreateDevice`. It changes the device: calls go through
  `D3D12SDKLayers.dll` wrapper objects, so vtables point into that DLL, not `D3D12Core.dll`.
- It needs Windows' optional feature "Graphics Tools". Players don't have it; `D3D12GetDebugInterface` fails.
- It costs CPU time on every call and takes its own locks. Fine for tests, too slow and too risky for play.
- It can't see what shaders do on the GPU, or the state a resource is really in at that point on the GPU.

GPU-based validation:

- It patches every shader with checks and adds its own work to command lists. It catches what the CPU layer
  can't: uninitialized or stale descriptors, indexing past the end of a heap, a resource read in the wrong
  state, state promotion and decay mistakes, bad samplers.
- It needs the debug layer, and must be set (`ID3D12Debug1::SetEnableGPUBasedValidation`) before the device is
  made. Toggling it on a live device removes the device.
- Its messages come after the GPU finishes the list, so they lag behind the CPU-side messages.
- Microsoft warns it is very slow and suggests small scenes or a nightly test machine. Here the game fell to
  4-17 fps.
- Copy queue lists run as compute lists under GBV. Results stay the same, timing doesn't.

Device removed reasons (`GetDeviceRemovedReason`):

- `DXGI_ERROR_DEVICE_HUNG` (0x887A0006): Microsoft files it as malformed commands from the app, a bug to fix
  before shipping. Our GPU hang reports so far all show this one.
- `DXGI_ERROR_DEVICE_RESET` (0x887A0007): also a bad command, but treated as a run-time failure: release the
  device and make a new one.
- `DXGI_ERROR_DEVICE_REMOVED` (0x887A0005): the GPU was physically removed or its driver was updated.
- `DXGI_ERROR_DRIVER_INTERNAL_ERROR` (0x887A0020): the driver hit its own problem and removed the device.
- A page fault has no reason code of its own. Look for a non-zero DRED `PageFaultVA` next to the reason.
- Once the device is removed, `ID3D12Fence::GetCompletedValue` returns `UINT64_MAX`. That is how we notice it
  off the game thread.

TDR (Timeout Detection and Recovery):

- Windows lets a GPU task run about 2 seconds. If the GPU can't finish or be preempted by then, it is reset and
  the app's device is removed. To the player: dark screen, music still playing.
- Several TDRs within a minute can turn into a bug check. Don't loop a hang test.
- The registry keys that change the delay are for driver testing. Never tell players to change them.
- Event Viewer: nvlddmkm event 153 (NVIDIA) at the TDR time confirms the GPU was reset.

DRED:

- Its settings are global to the process and only apply to devices made after they are set.
- Auto-breadcrumbs: the runtime writes a counter after each GPU operation (draw, dispatch, copy, clear,
  resolve, ...). The report gives each unfinished list's operations and how many finished. Ring of 65536 ops per
  list. Microsoft measured 2-5% cost.
- The counter is not exact: the GPU is pipelined and drivers reorder, prefetch and flush late. The first
  unfinished op is near the problem, not proof.
- Page fault data: the faulting GPU address and the live and recently freed objects there (names, types). Not
  every GPU reports page faults; some read zeros or just hang.
- DRED 1.2 adds context strings: marker and event text at each breadcrumb (`SetBreadcrumbContextEnablement`,
  read via `GetAutoBreadcrumbsOutput1` and `pBreadcrumbContexts`).
- Breadcrumbs need memory that survives removal (`D3D12_FEATURE_EXISTING_HEAPS`). Most drivers have it.
- The debug layer's own injected operations show up as breadcrumbs too.

Our own breadcrumbs (`WriteBufferImmediate`):

- `ID3D12GraphicsCommandList2::WriteBufferImmediate` writes a 32-bit value to a GPU address from the command
  stream. Check `D3D12_FEATURE_D3D12_OPTIONS3.WriteBufferImmediateSupportFlags` for the DIRECT bit first.
- Mode `MARKER_IN`: written when the command processor reaches it. That is ahead of the drawing.
- Mode `MARKER_OUT`: written only when all work before it has finished.
- The buffer must stay readable after removal: a CUSTOM heap, `WRITE_BACK`, `L0` (system memory), mapped
  once, read through a `volatile` pointer.

## Where HiFi XI relies on this

Turning things on:

- `D3D12DebugLayer=1` in settings.ini: read in `before_device`, `src/render/worldfx_core.cpp:2384`. Only when the
  host's hooks accept the debug layer's classes (`debug_layer_allowed`, :2387); `is_runtime`
  (`src/host/hooks.cpp:590`) lets `D3D12SDKLayers.dll` count as the runtime.
- `gpu_crash::enable_debug_layer`, `src/render/gpu_crash.cpp:194`: `EnableDebugLayer` (:204), and GBV only when
  the environment variable `HIFIXI_GBV=1` (:209-211). GBV is never in a setting.
- Messages: `write_debug_messages` (`gpu_crash.cpp:219`), called each present (`worldfx_core.cpp:2809`), drops
  INFO and MESSAGE, writes the first 4 of each kind in full to `bootloader\worldfx-d3d12-debug.txt`, then counts.
  The file is shared-read, so it can be read while the game runs.
- Our d3d8.dll has its own switch: `D3D8_DEBUG=1` (`src/d3d8/d3d8.cpp:172`), used in `gpu::init`
  (`src/d3d8/gpu.cpp:55-79`); its messages go to d3d8.log (`gpu.cpp:314`).
- DRED: `gpu_crash::enable`, `gpu_crash.cpp:162-191`: auto-breadcrumbs, page faults and contexts all FORCED_ON,
  before the device. `GpuCrashReports=0` turns it off (`worldfx_core.cpp:2377`). On by default.

When the device is lost:

- `gpu_crash::watch` (`gpu_crash.cpp:301`): a fence waiting for `UINT64_MAX` and `RegisterWaitForSingleObject`.
  `on_removed` (:136) runs on a thread pool thread: sets `lost()`, writes `report()` plus the add-on state plus
  the added sections to `bootloader\worldfx-gpu-crash.txt` (`worldfx_core.cpp:2502`).
- `report()` (`gpu_crash.cpp:470-584`): reason, GPU and video memory, page fault and the objects there, then each
  unfinished list's ops with `>` at the first one not done.
- After `lost()` HiFi XI records nothing and calls no AMD code (`src/plugin/main.cpp:1314-1322`).
- Our d3d8.dll logs the removed reason when Present fails (`src/d3d8/present.cpp:286`).

Breadcrumbs:

- Markers: `gpu_crash::mark` (`src/render/gpu_crash.hpp:59`) is `SetMarker` with metadata 0, a UTF-16 string.
  World FX's sections ("worldfx: grass", ...) are these.
- Native zone pass: `src/zone/draw.cpp`. Buffer made in `make_breadcrumbs` (:444-472). Word 2 = setup steps
  (`step`, :850-855; steps in `src/zone/draw.hpp:120`). Word 0 = `MARKER_IN` before each piece's draw, word 1 =
  `MARKER_OUT` after it (:924-935). Each word is frame low 16 bits << 16 | draw. Read in `breadcrumbs()`
  (:479-520); added to the crash report by `src/plugin/main.cpp:1060`.
- World FX: `src/render/grass_render.cpp:2214-2322`. `crumb()` writes both IN and OUT at every section marker;
  the report names the section the GPU stopped in, or says the stop came outside World FX.
- `draw.cpp` must not link `gpu_crash` (tests link draw without it): hence `add_section`.

Hang report (the game thread, not the GPU):

- `src/plugin/hangwatch.cpp`: no present for 4 s (`kHangMs`, :23) writes `bootloader\hifixi-hang.txt` (:250):
  every game thread's instruction and return addresses as module + offset (look them up in `build\hifixi.map`
  and `build\d3d8\d3d8.map`), and whether the device is lost (:215).
- A hang file with "device removed: no" is a CPU stall or deadlock. With "yes", read worldfx-gpu-crash.txt.

## How to read a crash report quickly

1. Which build ran? Check Ashita's session log (`logs\MM_DD_YYYY HH.MM.SS.txt`) start time against the install
   time. An install watcher once copied a new DLL while the old one kept running.
2. First line: the reason. HUNG with "No page fault" = a timeout or a stuck command. A `PageFaultVA` = a bad
   address: look at the objects listed there, live and freed.
3. Our breadcrumbs next, they are the most exact:
   - zone setup "got as far as" a step = stuck before the first draw (binding, heaps, PSO);
   - reached draw N, finished N-1 = draw N itself;
   - "stop came outside the zone pass" / "outside World FX's pass" = look elsewhere.
4. Then DRED's `>` op. Read the ops before it (clears, copies, markers, barriers) to find whose pass it is. The
   list names and our markers' order tell game list vs World FX vs AMD's.
5. The add-on state at the end: zone, settings, frame rate. Compare it with what the player was doing.
6. worldfx-d3d12-debug.txt if the debug layer was on; errors just before the time of the crash matter most.

## Mistakes this project made

- 0.27 (2026-10-25): the zone shader read `lamps[0xFFFFFFFF]` for "no lamp". The load ran before the branch and
  a root SRV has no bounds: NVIDIA hung, WARP didn't care. Found only with `HIFIXI_GBV=1` +
  `D3D12DebugLayer=1`, then bisected by draw count. Lesson: run new GPU code on the real GPU with GBV, not
  only on WARP.
- Debug layer deadlocks: device calls or Release under a mutex the recording hooks also take deadlock with the
  debug layer's lock (zone shadows, inject::prepare). Hooks call the original with the thread quiet
  (`src/host/hooks.cpp:92-104`), or the layer's wrapper calling the real object counts every call twice.
- The GBV session of 2026-11-02 deadlocked twice, not a GPU hang. GBV's slowness turned automatic frame generation
  on; AMD's presenter thread and then the character cache worker reset internal lists, our hooks took
  `g.mutex` for them while the game thread held `g.mutex` and waited on the debug layer. Fixed by ignoring list
  events from any thread but the game's (commit 0072203). Remember to run gbv_off after a session.
- DRED marker text was always empty, even with `BreadcrumbContext` FORCED_ON and on the real GPU with a forced
  `RemoveDevice` probe. Microsoft's DRED 1.2 post only ever talks about PIX marker and event strings; no page
  says which encodings are kept. A likely cause, not verified: it
  reads only PIX-format markers (WinPixEventRuntime's encoding), and ours are raw metadata-0 strings. Workaround:
  our own breadcrumbs name the sections; DRED's ops still show where the markers were.
- 0.62 reports pointed at the zone per-pixel lighting (turning it off "fixed" it twice). The 0.63 zone
  breadcrumbs (a2508c3) then showed the hung frame never reached the first draw's MARKER_IN: the stall was in
  the pass setup, so per-pixel was chance. 0.64 (f77601e) added the setup-step word.
- Breadcrumb test on the 4070 Ti (2026-11-02, one TDR on Alex's OK): a slow pixel shader on draw 40. DRED's first
  not-done op = draw 40, `MARKER_OUT` finished = 39, `MARKER_IN` reached = 199. The command processor runs far
  ahead of the drawing, so "reached" is never where it stopped; "finished" is.
- Windurst hang (2026-11-02 03:17): zone breadcrumbs said the zone pass finished. DRED's stuck draw came right
  after the "worldfx: grass" marker: World FX drew zone 115's blades in Windurst because the party zone lagged
  after zoning. Fixed with the zone-in packet's zone, plus World FX breadcrumbs (296ef2f, v0.65).
- The 04:07 hang the same day looked like the fix had failed. It ran the old build (session started before the
  install). Check the build first (step 1 above).
- The 10-29 frame generation hang: DRED said a dgVoodoo list at a DrawInstanced; cause never found. The many
  TDRs before it (10-14 to 10-27) predate frame generation. Alex's crashes stopped for a while when he turned off
  the NVIDIA driver's forced anisotropic filtering, yet Windows still logged GPU timeouts after that. Check the driver's overrides before blaming our code.

## Sources

- Use DRED to diagnose GPU faults: https://learn.microsoft.com/en-us/windows/win32/direct3d12/use-dred
- DRED spec: https://microsoft.github.io/DirectX-Specs/d3d/DeviceRemovedExtendedData.html
- DRED v1.2 and PIX marker strings (blog): https://devblogs.microsoft.com/directx/dred-v1-2-supports-pix-marker-and-event-strings-in-auto-breadcrumbs/
- GPU-based validation and the debug layer: https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-d3d12-debug-layer-gpu-based-validation
- SetEnableGPUBasedValidation: https://learn.microsoft.com/en-us/windows/win32/api/d3d12sdklayers/nf-d3d12sdklayers-id3d12debug1-setenablegpubasedvalidation
- WriteBufferImmediate: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist2-writebufferimmediate
- DXGI error codes: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-error
- Fence value after removal: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue
- WriteBufferImmediate modes: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_writebufferimmediate_mode
- TDR: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/timeout-detection-and-recovery
