# Own frame: where it stands (handoff, 2026-10-30)

Written because the session was about to run out of usage. A new session picks up from here.

## Branches and worktrees
- **dev-frame** (C:\code\LSB\hifixi-frame): the integration branch. All five parts merged; last merge b17a428 (fx round 5).
  Base: dev-0.55 2118bfd (0.55 model actors + dev-fade + dev-light). Also holds b2cbfee (sunlit zone list fix).
- Part branches (each its own worktree C:\code\LSB\hifixi-<part>): frame-fx, frame-world, frame-actors, frame-2d,
  frame-d3d8. world, actors, 2d and d3d8 are fully merged and idle. frame-fx was fast-forwarded to b17a428.
- **In flight when this was written:**
  - An agent on frame-fx doing **light spots** (040E70 / 03EE70 -> 182700). Check `git log dev-frame..frame-fx`
    and `git -C C:\code\LSB\hifixi-fx status`. If it committed, merge frame-fx into dev-frame. If it left
    uncommitted work, read docs\own-frame\fx.md item 8 and finish or discard it.
  - A full build + test run of b17a428 in hifixi-frame (results were not yet read). Rerun it (below).
- Nothing pushed, nothing installed, nothing seen in game.

## How to build and test dev-frame
- `build.cmd` (full) from the worktree root; `build.cmd quick` = plugin + starter + hook_test (+ d3d8.dll).
- Tests and their arguments (from the worktree root unless said):
  - fx_test; fx_render_test build\test\fx_fire.bin build\test\fx_cure.bin build\test\fx_fastblade.bin build\test\fx_fountain.bin
    (needs all four, else "the oracle read" fails); fx_geo_test build\test\fx_geo.txt
  - world_test build\test\world_oracle.txt; worldpass_test
  - cloth_test build\test\cloth_oracle.bin; actors_draw_test build\test\cloth_oracle.bin
  - charinject_test build\test\chars_pose.bin; charslot_test; zoneslot_test; charmodel_test; modelactor_test;
    zoneinject_test; upscale_test; gamewater_test; uidraw_test; hook_test (and --debug); menu_test (never from build\)
  - ui2d_dgv: run from build\dgv (uses dgVoodoo's d3d8.dll copied there by build.cmd)
  - d3d8: `build_d3d8.cmd` (full, not quick) builds build\d3d8\d3d8_unit.exe, d3d8_hooks.exe, d3d8_frame.exe; run
    from build\d3d8: `d3d8_unit.exe`, `d3d8_hooks.exe .\d3d8.dll [--debug]`
- Last fully read result (5906ee9 and 51b95a2): everything green.

## What each part did and what's open (details in docs\own-frame\<part>.md)
- **world** (world.md): sky dome, names (after the upscale when FSR is on; `/hifi names over off` to go back), Mog
  House grid (marker 12 = grid_end). Open: dome one frame behind (as the game), font from disk only (mods not seen).
- **actors** (actors.md): cloth stepped by the game's own 023290 at the model stub; overlay/cloak passes (+0x734).
  Open: cloak assumes alpha 1; overlay normals' space (gGame+0x294) unconfirmed.
- **2d** (2d.md): the whole UI, menu fade, text queue, wash, copyright, crossfade, afterimage, menu-res UI at screen
  size, `/hifi ui font on`. **Off by default**: `/hifi ui shadow`, then `/hifi ui on`. Movie stays the game's
  (DirectShow PMV filter, scrambled data). Open: lobby captures, paletted formats.
- **fx** (fx.md): D3M (all kinds, lit), D3A, rings, trails, MMB effect meshes, morphs, VuAnm, all distortions,
  specular, lens flares (only with HideFlare off), aura ring (in src\chars), battle numbers. `/hifi fx on|off`.
  Open: light spots (in flight), FSR motion (decided against: blended, no depth), cubemap-spec and damage-digit
  decodes unchecked, aura not with MSAA.
- **d3d8** (d3d8.md): build\d3d8\d3d8.dll replaces dgVoodoo; HiFi XI's hooks unchanged on it; replays 99.84-100% vs
  dgVoodoo. Options in d3d8.ini beside it: AsyncReadback=1 (visibility tests 3.0 -> 0.08 ms, answers 1-3 frames
  old; off by default), PerfLog=1. Ship build\d3d8\d3d8-shader-cache with it. No exclusive full screen (by choice).
  Install plan in d3d8.md: rename dgVoodoo's bootloader\d3d8.dll, put ours + cache in; **only with Alex's OK**.

## Shared plumbing (for anyone adding a pass)
src\host\passes.hpp/.cpp (marker handlers + modules; world carries depth planes, list heaps, bound targets),
src\game\slotmark.hpp (kinds 5 fx_run, 6 sky_end, 7 names_end, 8 screen, 9-11 ui/frame, 12 grid_end, 13-15 spare),
build.cmd PASSES_* lines. Character cache reserved descriptors: 1 cloak/overlay scene copy, 2 aura (4 reserved).

## Next steps after light spots
1. Full build + every test above on dev-frame.
2. Ask Alex before installing anything: first the plugin from dev-frame (game closed, the usual staged
   install/restore scripts and a -Game dry run, see memory "install-asap" / "shared-folder-release"), then, separately,
   the d3d8.dll.
3. In game: each part's "to check in game" list in its doc.

## Gotchas met on the way
- sed -i drops CRLF (the repository is CRLF; core.autocrlf=true): use the Edit tool or Python with bytes.
- Two parts each added descriptor-heap tracking to passes.cpp; dev-frame keeps fx's (lists()/heaps). Merges so far
  only conflicted in build.cmd test blocks (keep all) and that.
- Other sessions work in other hifixi worktrees (hifixi, hifixi-055, hifixi-light...): never touch them, never `git stash`.
