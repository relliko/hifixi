# The frame rate's speed-ups on HiFi XI's own d3d8.dll (branch dev-fps, 2026-11-02)

The old hifps addon's experimental speed-ups were built to cut dgVoodoo's and Ashita's per-call cost. From 0.11 to
0.64 they were on in every preset, so every player ran them. On 2026-11-02 Alex made them off by default: no preset
turns one on, and each player's were turned off once (`speedups_off_once`, backend.cpp). A player can still turn each
on by hand. HiFi XI now ships its own `d3d8.dll`. This branch measures
what each one still saves, keeps the ones that pay in the simplest place, and removes the rest. The goal is fewer risky
layers under the GPU hangs players report (DXGI_ERROR_DEVICE_HUNG). None of these speed-ups is shown to cause those
hangs: they stop inside HiFi XI's own zone pass setup (memory: crash audit 11-01). The reports now also say what the
frame rate was doing.

| Speed-up | Before | Now |
|---|---|---|
| `batch` | asm proxy (`proxy.asm`) swapped onto Ashita's device wrapper's vtable | **in our d3d8.dll** (C++, `HifiFrameSetSpeedups`) |
| `dedupe` | the same asm proxy, shadowing device state | **in our d3d8.dll** (C++, exact: the device's own state) |
| `occlusion` | 12 bytes of the game's actor-test rotation | **kept as is**: the one rate for the characters' test |
| `walls` | asm (`walls.asm`) reading the zone's collision, no exception handling | **removed** (VisTest answers the characters' test) |
| `sse` | a byte of the client's CPU feature object | **removed** (no effect any more) |
| `ashita` | 4 bytes in Ashita.dll's code (`je` to `jmp`) | **kept as is** (not measurable offline; see below) |

`src/game/hifps/stubs.inc`, `tools/hifps_stubs.py` and the walls harness build are gone. HiFi XI lays no machine code
of its own over the game's device any more. hifps_exp_test's known failure (`stubs.inc` against the addon) went with
them.

## How it was measured

The game can't be run from here, so the measurements are offline, against the five real call captures of 2026-10-05
(`C:\Games\PhoenixXI\d3dlog\*.txt`: day_npcs, night, water, spell, menu; one frame of the game's own Direct3D 8 calls
each).

- **`tools/hifps_proxy_counts.py`** counts what batch and dedupe would do to each frame by the proxy's own rules.
- **`d3d8_frame.exe <d3d8.dll> bench <capture> [all|today] [rounds]`** (`tests/d3d8_bench.hpp`) turns the capture into
  ready device calls and plays them through our d3d8.dll on the GPU, many times, timing each frame on the calling thread
  with `QueryThreadCycleTime`. That counts CPU work only: waits for the GPU are left out. It runs these ways, in
  interleaved rounds:
  - `direct`;
  - `proxy`: the addon's own `proxy.blob` laid onto our device as HiFi XI laid it, with batch and dedupe each off or on;
  - `own`: the same two done inside our d3d8.dll.
- **"today"** keeps only the calls the game still makes now that HiFi XI draws the zone, the characters and their
  shadows, and VisTest answers the visibility tests. Calls are dropped by their FFXiMain caller. It's an approximation:
  the game's setup calls around those functions stay in.
- Not modelled: Ashita's wrapper between the game and the device. In game every call passes through it, and every draw
  goes through its plugin dispatch unless `ashita` is on.

Game thread CPU a frame, microseconds (GPU: RTX 4070 Ti; CPU: i7-13700K; 8 rounds of 12 frames, median):

| Capture (today) | Game draws (quads) | direct | proxy, flags off | proxy batch | own batch | own dedupe | own both |
|---|---|---|---|---|---|---|---|
| day_npcs | 1377 (1353) | 2710 | +81 | −1821 | **−1908** | −272 | −1900 |
| night | 624 (583) | 1371 | +51 | −594 | **−650** | −83 | −675 |
| water | 1450 (1415) | 2864 | +12 | −2085 | **−2125** | −399 | −2136 |
| spell | 1428 (1391) | 2838 | +40 | −1814 | **−1861** | −346 | −1852 |
| menu | 1741 (1668) | 3358 | +47 | −1959 | **−1990** | −320 | −1993 |

- The whole old frame (`all`: zone and characters drawn by the game, 2,000 to 3,700 draws): own batch saves −0.67 to
  −2.15 ms, and own dedupe another 0.03 to 0.24 ms on top.
- Batched draws: day 1353 quads into 225 draws, menu 1668 into 456, water 1415 into 155. The C++ batch ends up with the
  same or slightly fewer draws than the proxy: it keeps batching through a repeated value even with dedupe off.
- `tools/hifps_proxy_counts.py` on today's frames:
  - dedupe would drop 2,100 to 4,200 calls a frame;
  - of those, 800 to 2,000 our d3d8.dll already returned from (render and stage states);
  - the rest are mostly `SetVertexShader` and `SetTexture` set again before every glyph.

**What it says.**
- The game draws its text one glyph per `DrawPrimitiveUP` call. On today's frames those quads are almost every draw
  the game still makes.
- Each draw costs our device about 1.6 to 1.7 µs of the game thread (what batching saves per draw it takes away):
  shader keys hashed, descriptor tables, pipeline lookup, upload and the list calls. Batching them saves 0.65 to 2.1 ms
  a frame, 47 to 74% of what the game's calls cost.
- Dedupe alone saves 0.08 to 0.4 ms a frame, mostly fixed-function constants uploaded again after a repeated
  `SetVertexShader`. With batch on it adds nothing measurable on today's frames.
- The asm proxy cost 12 to 280 µs a frame by itself, for its shadow copies and flushes.

## batch and dedupe in the d3d8.dll (`src/d3d8`)

- `HifiFrameSetSpeedups(flags)` and `HifiFrameSpeedupStats(out[4])` are new exports (`src/host/frameapi.hpp`), called
  on the game's thread. `exp.cpp` follows the `exp_batch` and `exp_dedupe` options through them. With another
  d3d8.dll loaded, the toggle says it needs HiFi XI's own.
- **batch** (`device::take_quad`, `flush_quads`, `quads.hpp`):
  - It takes a `DrawPrimitiveUP` of a two-triangle strip with a stride of 12 to 64 bytes, not flat shaded, not
    recording a state block, and not sampling a render target. That last rule keeps the resolve and the screen passes
    single draws, as they were: the world's end stays marked at the resolve itself, and FSR still finds the resolve as
    a quad. The strip becomes list vertices v0 v1 v2, v2 v1 v3 (the proxy's order): same triangles, windings and order.
  - The waiting quads are drawn first, as one list, before anything that could change what they look like:
    - a Set* call whose value changes (every setter checks; a repeated value doesn't flush);
    - any other draw, Clear, SetRenderTarget, CopyRects, UpdateTexture, GetFrontBuffer;
    - ApplyStateBlock, BeginScene/EndScene, Present, Reset, a different palette;
    - `HifiFrameMark` (HiFi XI's marks record on the same list);
    - a surface or volume `Lock` (a texture rewritten mid-run keeps its old texels for the quads before);
    - turning batch off.
  - Unlike the proxy, it sees Ashita's own GUI draws (they come straight to the device) and texture locks: the proxy
    couldn't, so it had to forget its shadow at BeginScene, EndScene and Present.
- **dedupe** (`device::unchanged`): with it on, a setter whose value is already in effect returns at once. This covers
  textures, transforms, viewport, material, lights, light enables, clip planes, streams, indices, shaders, shader
  constants and the palette. Render and stage states always did. It reads the device's real state, so nothing needs
  forgetting. One exception: `SetVertexShader` with the same handle still runs when the shader's declaration has
  constants, because setting it loads them again (the proxy dropped that call).
- **Tests:**
  - `d3d8_unit`: the quad rule and the strip-to-list order.
  - `d3d8_hooks`: an interface-like run of 88 overlapping, blended quads on WARP, with a stage op changed mid-run, the
    texture locked and rewritten mid-run, flat shading, a fan, a marker clear and two quads sampling a render target. It
    reads back the same pixel for pixel with the speed-ups off, with both on and with batch alone. Batched, 81 quads
    become 7 draws (74 fewer on the list); the flat ones and the render target's stay single; dedupe drops the repeated
    shader and texture; turning batch off draws the waiting quads at once.

**The trade-off.** The proxy sat above Ashita's wrapper, so Ashita only saw the merged draws. Batching in the d3d8.dll
happens below it, so Ashita's per-draw plugin dispatch sees every quad again, unless `ashita` is on too. Both are off by
default now, so a player who turns batch on should turn `ashita` on with it. With `ashita` off or refused (a loaded addon
or plugin uses `d3d_dp`/`dip`/`dpup`/`dipup`), Ashita dispatches all 600 to 1,700 quads a frame, which the proxy's
merge used to spare it. client-notes estimates that dispatch at 2 to 3 ms for 3,000 draws with 28 addons, so roughly
0.5 to 1.5 ms a frame (inferred, not measured).

## The characters' visibility test: one path

There were three ways to answer "is this character hidden" (the actor draw's call at `0CC688`):

- the game's own GPU readback, stretched out by `occlusion`;
- `walls`, from the zone's collision;
- World FX's `VisTest`, from its depth copy, one to three frames late, never waiting.

`walls` had to be handed the call and take it back (`walls_holding`, `set_actors_elsewhere`).

Now there is one path:
- **VisTest answers it while World FX draws** (`src/game/feed.cpp`). It is the same depth test the game does.
- **While World FX doesn't draw** (or VisTest is off), the game's own test runs at `occlusion`'s rate.
- `occlusion` tests every character every frame while VisTest has the call, since an answer costs nothing there. The
  feed now tells it at once (`set_actor_handoff(elsewhere)`); nothing waits for a hand-off.
- **What occlusion saves** (only while VisTest doesn't answer): the game's test is a synchronous readback, and every one
  waits for the GPU to finish everything queued. Through our d3d8.dll that is 3.0 ms a frame for the day capture's 15
  tests (`docs/own-frame/d3d8.md`, replays). The game tests each character every 4th frame; at 240 fps occlusion makes
  that every 16th, so the characters' share of those waits drops by three quarters. It is 12 bytes of the game's own
  code, checked to be the stock bytes before and after, so it stays.

**walls is removed:**
- It was wrong in game: on 2026-10-26 it hid characters behind collision that wasn't drawn there.
- It no longer ran when World FX drew.
- It was the riskiest layer: game memory read from asm with no exception handling. It read the camera's matrices, the
  actor's test rect and, through the game's own segment test `1815B0`, the collision grid.
- **During a zone unload:** it fell back to the game's test only when the collision map's grid field was 0. If a zone
  ever freed its collision before clearing that field while actors were still drawn, walls would have read freed
  memory in the game's own routine, and an access violation there takes the game down. The camera's own segment test
  runs over the same data every frame on the same thread, which is why it never showed. It is no longer a question.

## sse: removed

The flag (`[[0x4566E8]+9]`, the client's CPU feature object) picked the SSE skinning code `0300B0` over x87 on CPUs
that don't say Intel. That skinning (`02F1D0`) runs inside the game's character draw `02DC10`. HiFi XI's native
characters skip that draw at `02B7E3` for every model they draw, and skin in their own vertex shader
(notes-characters.md, "Pose throttle side effect").

So the flag now only reaches models the native renderer leaves to the game. On Intel CPUs the flag is already 1 (Alex's
i7-13700K: the toggle never changed anything).

What could go wrong with it:
- It writes the game's data, a byte other code may read for other SSE paths (not traced).
- The addon put the stock byte back unconditionally.
- With no measurable win, it goes.

## ashita: kept, measure in game

It makes the wrapper's four draw methods skip `PluginManager::Direct3D_*`:
- 4 bytes in Ashita.dll's code (`je` to `jmp`), found by pattern from the client device's vtable slots 70-73;
- it refuses while a loaded addon or plugin handles the draw events, unless forced;
- a newcomer that handles them turns it off again (checked once a second).

What it saves can't be measured offline: Ashita's wrapper only runs in the game. Now that batch is below Ashita, it is
what keeps Ashita's dispatch off the 600 to 1,700 quads a frame (above).

What can go wrong:
- **An Ashita update** that changes those methods: the pattern check refuses (nothing written).
- **Another tool patching the same bytes:** it is put back only while the byte is still ours.
- **An addon or plugin that wants the draw events:** it is checked at turn-on and on every change of the loaded set.
  An addon is found by the event's name in quotes in its Lua files; one that builds the name some other way isn't
  found, and loses the events.
- **The write itself:** it happens on the game's thread, in Present, never while a draw method runs.

Measure it in game: in a town with a busy chat log, read the frame rate (`/hifi status`, or the counter) with "Skip
Ashita's per-draw dispatch" on and off. Or run `research\ffxi-re\tools\frameprof.cmd ashita_on 30` and `ashita_off 30`
and compare the main thread's share in Ashita.dll and addons.dll.

## What players' reports now say

- **hifixi.log**, at start and whenever it changes. By default:
  `HiFi XI: frame rate: frame step unlocked (N call sites), cap 240 fps; speed-ups on: none; off: ashita, occlusion,
  batch, dedupe`. With some turned on, e.g. `on: occlusion (every frame: VisTest answers the characters' test), batch;
  off: ...`. A refused toggle names why.
- **worldfx-gpu-crash.txt**: a section `The frame rate at that moment: ...` (`gpu_crash::add_section`), with the frame
  rate then.
- **hifixi-hang.txt**: the same line in the hang report's context (`src/plugin/hangwatch.cpp`).
- **The debug overlay** (Advanced, "Show what's drawing on screen") shows it too.
- **`/hifi status`**: batch now shows `N quads a frame in M draws; K draws a frame in all`, and dedupe the calls dropped
  a frame.

The line is made on the game's thread every half second. The reports, written on threads of their own, read a copy
under a lock they wait on for 50 ms at most.

## Not changed

- `src/game/hifps/step.cpp` (the frame step) was only reviewed, lightly: signature-resolved sites, only its own bytes
  put back, everything on the game's thread. Nothing was found to change.
- The defaults are Alex's (2026-11-02, from the stability session): every speed-up is off by default and no preset
  turns one on. This branch builds on that; the measurements above say what each is worth when turned on.
- `exp_walls` and `exp_sse` left in an old settings file are ignored.
- The one-time import of hifps's settings.lua still copies them; nothing reads them.

## Open

- Run in game: the batch's look (text, menus, the chat log, Ashita's ImGui on top) and `/hifi status` showing quads
  and draws.
- The `ashita` A/B above.
- d3d8.log's draw counts now count batched draws as one.
