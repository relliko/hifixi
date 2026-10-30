# HiFi XI without dgVoodoo

Alex, 2026-10-30: "we should assume going forward that we will not use dgvoodoo and not build around it". HiFi XI runs
on its own d3d8.dll (docs/own-frame/d3d8.md). This is the list of what was built around dgVoodoo and what replaces it.

## How HiFi XI was built around dgVoodoo

dgVoodoo was a closed box: HiFi XI watched its Direct3D 12 output from outside.

1. **Hooks on Direct3D 12's own classes** (src/host/hooks): every list, queue and device call dgVoodoo made, seen by
   swapping slots in D3D12Core's vtables; descriptor views followed from their making (backend `ev_view`, `ev_copy`) to
   learn which resource a CPU handle names.
2. **Markers smuggled through the game** (src/game/slotmark.hpp, zoneslot, charslot, worldmark, uidraw...): the game
   side clears one stencil pixel through the game's Direct3D 8 device; the Direct3D 12 side spots that clear in the
   stream (backend `ev_clear_depth` -> `on_mark`) and calls the pass's handler. The pixel's x carries kind and seq.
3. **Guessed state, put back after** (grass::list_state, passes::world): what dgVoodoo had bound on the list (targets,
   pipeline, root signature and arguments, heaps) recorded from the hooks and restored after each of our passes;
   resource states followed from its barriers ("may be older than the marker": it binds lazily).
4. **Guessed frame boundaries**: World FX finds the world's end from the world target's RENDER_TARGET -> read barrier,
   per command list (`on_barrier`, `grass_done`, `due()`, vertex-count "world size"); present isn't the boundary
   because dgVoodoo records the next frame on another thread.
5. **A thread a frame behind**: everything game-side is handed over in rings of 16 slots keyed by seq (zone/frame,
   charframe, fxframe, frames2d, world/frames) because dgVoodoo's thread read them a frame or so later. Resources let
   go seconds late "while dgVoodoo's lists may still hold them".
6. **dgVoodoo's quirks copied**: the half-pixel offset (also ours: Direct3D 8's rule), its depth not readable through a
   view (FSR copies A's depth plane into a buffer), dgVoodoo.conf's forced filtering (ui2d `follow_dgvoodoo`), its
   settings in /hifi streamlog, "OutputAPI = d3d12_fl11_0" messages, late start without the starter.

## Found broken on our d3d8.dll

- World FX shaded only when the game's pipeline had been seen on the list (item 4): on ours a list it hasn't drawn on
  yet has none, so 1 frame in 12 had no sun shadows (fixed 10d01c3: only the root signature is needed).
- Our depth was made DENY_SHADER_RESOURCE; World FX samples it (fixed c08fb81).

## Plan

- **Step 1, dead weight** (no behaviour change on ours): dgVoodoo.conf readers (ui2d filtering, streamlog), messages and
  menu text naming dgVoodoo / OutputAPI, the late-start path's dgVoodoo wording.
- **Step 2, a direct channel** (d3d8.dll <-> HiFi XI): d3d8.dll exports a small API; HiFi XI registers callbacks.
  - `mark(kind, seq)` from the game side instead of the stencil clear: d3d8.dll flushes its pending barriers and calls
    HiFi XI's handler right then, on its list, with the world's target and depth and their exact states (it tracks
    them), its heaps and root signature.
  - After any handler, d3d8.dll forgets what it had bound (the next draw binds all of it again) and sets its heaps and
    root signature itself: no capture and restore (item 3).
  - `world_end`: d3d8.dll says when the world's target is first read after its drawing (the resolve): World FX's pass
    is called there (item 4 gone: no per-list barrier inference, no vertex counts).
- **Step 3, drop the guessing**: the stencil-pixel markers and their decoding, list_state capture/restore for our own
  passes, `due()`/world-size learning, the per-list `grass_done`, the hooks' view following where d3d8.dll can name
  resources directly.
- **Step 4, same thread**: our d3d8.dll records on the game's thread, so the handoff rings and the "seconds late"
  releases can shrink (kept until each module is moved over, they're harmless meanwhile).
- **Step 5**: FSR reads A's depth through a view instead of copying it; tests rewritten against the direct channel;
  tests\ui2d_dgv (the 2D renderer against dgVoodoo's own d3d8.dll) moved to ours, then render2d's dgVoodoo.conf
  filtering parser (`filtering::read`) goes with it.

## Done

- Step 1 (2026-10-30): ui2d's and streamlog's dgVoodoo.conf readers removed; status, menu and log text name HiFi XI's
  d3d8.dll instead of dgVoodoo's OutputAPI.

Each step lands with every suite green and is installed only after an in-game look.
