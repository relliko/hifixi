# Owning the whole frame (branch dev-frame, started 2026-10-30)

Alex: "do all the work to own the whole frame". Goal: every pixel FFXI shows is drawn by HiFi XI, and in the end the
game's Direct3D 8 calls go to a translator of our own instead of dgVoodoo.

Source of truth for what the game still draws: research/ffxi-re/notes-original-renderer.md (the table "Every pass that
still draws") and the five notes it points to; effects: notes-effects*.md; zone/characters: notes-zone.md,
notes-characters.md; slots and markers: notes-replace-slots.md, notes-offswitch.md.

## How a pass is taken over (the pattern the zone, characters and model actors already use)
1. **Game side** (the game's thread, src/game/...): a patch at the game's draw site (found by signature, both dumps
   tested) records what the game would have drawn into a ring slot keyed by a sequence number, skips the game's own
   D3D8 calls, and emits a slot marker (src/game/slotmark.hpp: a 1x1 stencil-only clear on A's depth, x = 16 x kind + seq)
   at the pass's slot.
2. **Direct3D 12 side** (dgVoodoo's recording thread, inside the hooks): at that marker the pass's module draws the
   published slot into the world (or wherever the pass belongs) and puts dgVoodoo's state back (grass::list_state).
3. A **gate**: the game keeps drawing the pass until the native side is ready and drawing (never a black hole).

## Shared plumbing on dev-frame (commit "Own frame: pass registry")
- `src/game/slotmark.hpp`: kinds 5-15 reserved: 5 fx_run, 6 sky_end, 7 names_end, 8 screen, 9 ui_begin, 10 ui_end,
  11 frame_end, 12-15 spare (ask before taking one: tell the integrator in your report).
- `src/host/passes.hpp/.cpp`: a module registers statically (`static const hifi::passes::registrar`) a marker handler
  per kind and/or a game-side module {name, present, update, status, stop, command, device}. The backend dispatches
  kinds >= 5 to the handler with the world's target/depth; the plugin calls the modules each present, every half
  second, for /hifi status, /hifi <name> ..., and at Release (stop first).
- `build.cmd`: one variable per part (PASSES_FX, PASSES_WORLD, PASSES_ACTORS, PASSES_2D): put your part's sources on
  your own line only. Add your tests in a block of your own just before the "rem The menu drawn with a fake GUI" block.

## The parts (each on its own branch and worktree, merged into dev-frame by the integrator)
| Part | Branch / worktree | What |
|---|---|---|
| fx | frame-fx / C:\code\LSB\hifixi-fx | every effect element of the priority-12 flush (all classes: D3M, MMB fallback, D3A, Vu, morph, rings, specular, light spots, distortion, lens flares, battle numbers), weapon trails, aura ring; in sorted order with the see-through characters |
| world | frame-world / C:\code\LSB\hifixi-world | sky dome, names (+ icons, star line), Mog House layout grid |
| actors | frame-actors / C:\code\LSB\hifixi-actors | cloth (type-1) meshes so no model falls back whole; cloak/overlay passes (actor +0x734) on native characters |
| 2d | frame-2d / C:\code\LSB\hifixi-2d | screen passes (crossfade, wash, afterimage, fade after menus, text queue, copyright, movie, UI-size target, screenshot) and the whole interface as a native 2D renderer, under Ashita's ImGui |
| d3d8 | frame-d3d8 / C:\code\LSB\hifixi-d3d8 | our own d3d8.dll in place of dgVoodoo: D3D8 -> D3D12, dgVoodoo-compatible where HiFi XI's hooks depend on it, general enough (fixed function + vs.1.1) for whatever is still drawn the old way (character creation's Dancer, fallbacks, Ashita) |

## Rules for every part
- **Never install anything, never touch the running game** (no attaching, no live memory, no packets). Reading the
  game's DATs under C:\Games\PhoenixXI and the FFXiMain dumps is fine. Everything is tested offline: both dumps, the
  DATs, WARP with the debug layer, the existing test style (tests/*_test.cpp, an oracle in Python where it helps).
- Nothing is "done" until `build.cmd` (full) builds and your tests plus hook_test pass. Commit on your branch as you go
  (messages in the repository's style, ending with the Co-Authored-By line).
- Keep to your own files. Shared files (main.cpp, backend.cpp, slotmark.hpp, chars/*, zone/*, gamedraw) only where you
  must, small and additive, and say so in your report.
- Match the code around you: comment density, naming, the "(R/D/I)" marking in notes, CRLF line endings.
- Write `docs/own-frame/<part>.md`: what's built, how it's tested, what's open, what needs a look in game.
- What can't be checked offline is said plainly; nothing here has been seen in game.
