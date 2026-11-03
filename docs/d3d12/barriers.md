# Resource states and barriers

How Direct3D 12 wants a resource's state told to it, and how HiFi XI and our d3d8.dll do it. Read before adding a pass
that touches the world's target or depth, or a new resource of our own.

## The rules that matter here

States, legacy `ResourceBarrier` (the only kind this project uses):

- Each subresource is in exactly one state at a time. The app tracks it, not the driver. A transition's `StateBefore`
  must be what the subresource is really in. The runtime only checks that within one command list; the debug layer
  tracks further, but not every mistake.
- A state is either one write bit alone (RENDER_TARGET, DEPTH_WRITE, UNORDERED_ACCESS, COPY_DEST, ...) or any OR of
  read bits (DEPTH_READ, PIXEL_SHADER_RESOURCE, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE, ...). Never a write bit with
  anything else. So a depth can be tested and sampled at once (`DEPTH_READ | PIXEL_SHADER_RESOURCE`), never written and
  sampled at once.
- A barrier array runs in order, as if one call per element. Batch barriers into one call where you can.
- Same state before and after is rejected by the runtime (the list's `Close` returns E_INVALIDARG). Skip it.
- `D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES` moves every subresource, and needs them all in `StateBefore`. If the
  planes or mips are in different states, transition each subresource by its own index.

Planes (depth + stencil):

- A depth-stencil format with stencil (our D24S8, view 45 on typeless 44) has two planes. Each plane is its own
  subresource: index = mip + array * mips + plane * mips * arrays. With one mip and one slice: 0 = depth, 1 = stencil.
- The two planes can be in different states. Our d3d8.dll uses that: depth DEPTH_READ while stencil is DEPTH_WRITE is
  normal (the slot markers' stencil clears, stencil-only passes).
- A pass that writes depth or stencil needs each plane it writes in DEPTH_WRITE; ours move both planes, and put each
  back to its own old state after. Reading "the depth's state" from plane 0 alone hides the stencil plane's state.
- A shader view of a D24S8 depth (R24_UNORM_X8_TYPELESS) reads plane 0. Only plane 0 needs a shader-read bit for it.

Implicit promotion and decay:

- Only out of COMMON (promotion) and only back into COMMON (decay). No other implicit moves.
- Buffers, and textures made with `ALLOW_SIMULTANEOUS_ACCESS`, promote from COMMON to almost any state on first use.
- Other textures promote only to the shader-resource and copy states. Depth-stencil textures can't be simultaneous
  access, so they never promote to DEPTH_READ/DEPTH_WRITE: they always need explicit barriers.
- Decay happens when an `ExecuteCommandLists` finishes on the GPU, not between lists in the same call. What decays:
  everything used on a copy queue, all buffers, simultaneous-access textures, and anything that was only promoted to a
  read state. A texture you transitioned explicitly on a direct queue does not decay.
- A copy queue needs its resources in COMMON. Transitioning to COMMON on the direct queue costs: do it only for that.
- PRESENT == COMMON (both 0). A back buffer must be in it at Present.
- Upload heap resources stay GENERIC_READ for life; readback heap resources stay COPY_DEST. Never transition them.

Other barrier kinds:

- UAV barrier: between two dispatches/draws that touch the same UAV where the second depends on the first's writes.
  Not needed between reads only, or between writes whose order doesn't matter. `pResource = nullptr` covers all UAVs.
- Aliasing barrier: between using two placed/reserved resources that share heap memory. We don't alias today.
- Split barrier: `BEGIN_ONLY` then later the same before/after with `END_ONLY`. Between them the subresource can't be
  used at all. A hint for the GPU, worth it only with real work in between. We don't use them.

Resource flags:

- `DENY_SHADER_RESOURCE` (only allowed with ALLOW_DEPTH_STENCIL): no SRV may be made of the resource, and it can't go
  to PIXEL_/NON_PIXEL_SHADER_RESOURCE. The docs promise no error code; in our game run an SRV of one removed the device
  (`DXGI_ERROR_INVALID_CALL`). Only set it on a depth nobody will sample.

What the debug layer flags (D3D12DebugLayer=1):

- A `StateBefore` that doesn't match what it tracked; invalid state combinations; a resource used in the wrong state for
  a draw/clear/copy (much of this only with GPU-based validation, which checks at GPU time); a back buffer not in
  PRESENT at Present; views that the resource's flags forbid. Its tracking is conservative: no message does not prove
  the states are right.
- GPU-based validation is slow (here 4-17 fps). In this project it exposed a deadlock between the debug layer's lock
  and World FX's mutex (memory: crash audit 11-01). Use it for a session, then turn it off.

Enhanced barriers:

- A newer, separate barrier API (`ID3D12GraphicsCommandList7::Barrier`): layouts, access and sync split apart, no
  implicit promotion or decay. Optional, not a driver requirement: check
  `D3D12_FEATURE_D3D12_OPTIONS12.EnhancedBarriersSupported`. The driver docs name WDDM 3.0 (Windows 11 22H2) drivers and
  the 1.706.4-preview Agility SDK. We ship no Agility SDK, so it would depend on the OS runtime and the driver.
- Mixing the two models on one subresource needs it in COMMON (state or layout) at the switch, and the spec advises
  against it: it costs. Not recommended here: the
  game's d3d8 calls, FSR 2/3's backends and frame generation all speak legacy states. Stay on legacy barriers.

## Where HiFi XI relies on this

Our d3d8.dll (owns the states of everything the game draws):

- `src/d3d8/device.hpp:17-21`: the state rules on the list. Depth planes by what the draw writes (DEPTH_WRITE while
  written, else DEPTH_READ, stencil the same); a target PIXEL_SHADER_RESOURCE when read, RENDER_TARGET when drawn to;
  the back buffer COPY_SOURCE at Present.
- `src/d3d8/gpu.cpp:481`: `queue_barrier` merges a second move of the same subresource into the queued one, and drops
  it when it ends where it began (no same-state transitions, no A->B->A pairs).
- `src/d3d8/gpu.cpp:503`: `transition` moves a depth plane by plane, anything else with ALL_SUBRESOURCES.
- `src/d3d8/gpu.cpp:519`: `transition_plane`, one plane by its subresource index. `:527` `flush_barriers` sends the
  whole pending batch in one call.
- `src/d3d8/device_draw.cpp:283,285`: a Clear moves only the plane it clears to DEPTH_WRITE.
- `src/d3d8/device_draw.cpp:828-829`: each draw sets plane 0 by Z write and plane 1 by whether stencil is written.
- `src/d3d8/device.cpp:710,778`: copies out of a surface move it to COPY_SOURCE.
- `src/d3d8/resource.cpp:164-176`: the game's depth: no DENY_SHADER_RESOURCE (World FX and FSR sample it), made in
  DEPTH_READ, `planes = 2` for D24S8.

The direct channel (exact states, no guessing):

- `src/host/frameapi.hpp:6-9`: at `HifiFrameMark` the d3d8.dll flushes its pending barriers, then calls our handler with
  its list, target, depth and their states. The handler may change states but must leave them as it found them.
- `src/host/frameapi.hpp:18-28`: `hifi_frame_mark` carries `rt_state`, `ds_planes[2]` (plane 1 = stencil) and
  `ds_plane_count`.
- `src/d3d8/device.cpp:2190-2211`: where the d3d8.dll fills them from its own tracking.
- `src/backend/backend.cpp:659` (`exact_planes`): the bound depth's planes to `zone::inject::set_depth_planes` and
  `chars::inject::set_depth_planes`. A one-plane depth repeats plane 0.
- Channel version 2 (`src/host/frameapi.hpp:32`, `kHifiFrameChannel` at :59): every mark also carries the world's
  target and depth (named at the zone's begin) and their states now, bound or not (`src/d3d8/device.cpp:2219`; the
  images forgotten when the game frees them, `device::forget`). `world_planes` (`src/backend/backend.cpp:901`) hands the
  world depth's planes over at every mark, the world's end and the back buffer's marks included: FSR at the resolve
  and the world module after it read them there. An older d3d8.dll (channel 1, no world fields) is refused by
  `direct_channel`: its passes stay off rather than read planes nothing keeps current.
- Since step 3 of the re-architecture nothing follows barriers for the zone or the characters: `on_barrier` is gone
  from both. `on_marker` (`src/zone/inject.cpp:342`, `src/chars/charinject.cpp:511`) still resets a NEW depth's planes
  to unknown; the mark's exact planes come right after, or the pass refuses ("the world's depth planes' states aren't
  known yet").

No barrier inference is left (step 5b, 2026-11-02): HiFi XI hooks nothing of Direct3D 12's.

- FSR is placed by our d3d8.dll's resolve mark (kind 14, channel 4: right after the first draw that reads the world
  into a target at least its width after its end, a copy before it or not; `upscale::on_resolve`, nothing put back).
  The first in-game run with the old placement compared beside it (81ea82a) placed it 5786 times with 0
  disagreements; the old placement and its hooks then went.
- World FX follows no barrier since step 4: the world's target and depth, their states and the fog (the world's last
  whole clear, channel 3) come with every mark (`worldfx::world_mark`, built by the backend's `world_of`).

Our own passes on the game's depth:

- `src/chars/charinject.cpp:570-573,717-719`: both planes to DEPTH_WRITE from their known states, the target to
  RENDER_TARGET from `w.rt_state`; put back after.
- `src/upscale/temporal.cpp:459-460,529`: plane 0 only to `DEPTH_READ | NON_PIXEL_SHADER_RESOURCE` for FSR's motion pass,
  then back. `:430` refuses a depth made with DENY_SHADER_RESOURCE.
- `src/world/world_module.cpp:346-350`: plane 0 to `DEPTH_READ | PIXEL_SHADER_RESOURCE` for the names, then back.

Our own resources:

- `src/render/grass_render.cpp:4692,5295`, `src/fx/fxrender.cpp:596`: our private depths (floor map, blade depth, fx)
  are DENY_SHADER_RESOURCE on purpose: nothing samples them. Keep it that way, or drop the flag first.
- `src/render/grass_render.cpp:3564,6120`: UAV barriers between World FX's passes over the same buffer.
- The texture pack's textures (`src/pack/stream.cpp:1002`): committed, made in COMMON, written once on the streamer's
  copy queue (promoted to COPY_DEST there, decayed when that `ExecuteCommandLists` ends), then only read on the direct
  queue by implicit promotion (a shader read, or COPY_SOURCE for a copy), decaying after each `ExecuteCommandLists`.
  **Nobody ever puts a barrier on a pack texture**: an explicit transition on the direct queue would stop it decaying,
  and the next list's promotion would no longer match. A key streamed again gets a new texture, never a second write.
  packstream_test reads them back both ways with no barrier, 0 messages under GPU-based validation (WARP and the
  4070 Ti). Since M4 the zone pass reads them too (the colour map of a piece whose pack word says drawn,
  `src/zone/draw.cpp:959`): a pixel shader read, promoted from COMMON, no barrier, as the zone's own textures are read;
  zonenative_test draws a zone from them, 0 messages under GPU-based validation (WARP and the 4070 Ti).
- The streamer's ring (`src/pack/stream.cpp:548-560`): a CUSTOM heap buffer (CPU write-back, L0) made in COMMON; a
  buffer promotes to COPY_SOURCE on the copy queue and decays. Not an UPLOAD heap (GENERIC_READ for life, and
  write-combined: docs/d3d12/sync.md).
- `src/d3d8/gpu.cpp:60`, `src/render/gpu_crash.cpp:204,211`, `src/render/worldfx_core.cpp:2384`: debug layer and GPU
  validation switches.

## Mistakes this project made

- **DENY_SHADER_RESOURCE on the game's depth (2026-10-30).** The first in-game run of our d3d8.dll removed the device
  (DXGI_ERROR_INVALID_CALL) 0.04 s after World FX's first grass draw; then every texture creation failed (black world).
  A debug-layer run named it: our depth was made with DENY_SHADER_RESOURCE, and World FX's SRV of it was invalid.
  dgVoodoo's depth had been readable. Fix c08fb81 (`src/d3d8/resource.cpp:164`) and a d3d8_hooks check (now tests/d3d8_host.cpp). Lesson: a
  resource someone else samples must be made readable; check flags before making a view (as temporal.cpp:430 does).
- **Waiting for a barrier that never came (v0.57).** The character renderer learned the depth's planes only from
  barriers. With the direct channel, at a mark the planes were already where the draw needed them, so no barrier came
  and the planes stayed unknown: native characters drew 0 frames (blurry, sometimes gone). Fix 27d4d8d:
  `chars::inject::set_depth_planes` from `ds_planes`. Lesson: never infer a state from the barrier stream when the owner
  can say it.
- **World FX guessing from what it saw on a list.** On our d3d8.dll (which splits lists) World FX shaded only 7455 of
  11692 frames: it waited to see the game's root signature and pipeline on the same list, so sun shadows came and
  went (memory: own frame, 2026-10-30). Fix 10d01c3. Then it stopped inferring the world's end from barriers: the
  `world_end` mark (6ad2463), and a8c0acd (marks only, nothing guessed or put back).
- **Reading plane 0 as the whole depth.** Stencil-only moves (subresource 1) used to look like the depth's state to
  World FX's tracker; hence the subresource filter at `backend.cpp:615-626` (in since
  e0921d5, 0.24-0.32).
- **The debug layer run that hung.** With GPU-based validation on, World FX's mutex, taken for lists on any thread
  (AMD's frame-gen presenter, the character cache worker), deadlocked against the debug layer's lock. Fix 0072203: list
  events only from the game's thread (memory: crash audit 11-01). Debug runs are for one session, then D3D12DebugLayer back to 0.

## Sources

- https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-resourcebarrier
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_states
- https://learn.microsoft.com/en-us/windows/win32/direct3d12/subresources
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_flags
- https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-d3d12-debug-layer-gpu-based-validation
- https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_d3d12_options12
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/enhanced-barriers
