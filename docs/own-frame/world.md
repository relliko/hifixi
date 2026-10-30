# Owning the frame: the world part (branch frame-world)

The sky dome, the names over characters (with their icons and the star line) and the Mog House layout grid, drawn by
HiFi XI instead of the game, with the pattern of PLAN-own-frame.md: a game-side recorder at the game's draw site, a slot
marker where the pass ends, the Direct3D 12 draw at that marker, a gate that leaves the pass to the game until HiFi XI's
side is ready and drawing. Sources: research/ffxi-re/notes-original-renderer.md, notes-world-passes.md (2, 6),
notes-ui-draw.md ("The names"), client-notes.md ("Sky", "Fonts", "Names over characters" and after, "Projection and
depth, complete", "Frame order, complete"). R = read in the dump, D = checked in the DATs, I = inferred.

**Nothing here has been seen in game.** Everything is tested offline: both FFXiMain dumps, the DATs, WARP with the debug
layer.

## What's built

| Pass | Game side (the game's thread) | Marker | Direct3D 12 side |
|---|---|---|---|
| Sky dome | `src/game/skydraw`: 03DD00's `call 03D640` -> `on_dome` | 6 `sky_end`, right after the dome | `src/world/world_render.cpp` `renderer::sky` |
| Names, icons, star line | `src/game/namedraw`: the names pass's vtable slot (0817F0), `0D08A3` (086210), `0D08C9` (086900), `0867D2` / `086D62` (120590), `08689D` / `086E27` (00CD00) | 7 `names_end`, after 0817F0 (in the field only) | `renderer::names` into A, or `renderer::names_over` over the upscaled screen |
| Mog House layout grid | `src/game/griddraw`: `1D5DE0` (1CF110), `1CF4BD` / `1CF607` (177B40) | **12 (spare, taken: `worldmark::kGrid`)**, after 1CF110 | `renderer::grid` |

- `src/game/worldmark`: the three markers (one-pixel stencil clears through the game's device, as charslot's), each kind
  with its own seq.
- `src/world/frames`: one ring of 16 per pass (`sky_frame`, `names_frame`, `grid_frame`), filled and published as
  `zone/frame.hpp`'s.
- `src/world/dome`: the dome as 03D640 builds it, on the CPU (bands of `max(1, ftol((h1 - h0) x 12.048))` strips, f
  and 1 - f as floats, 03DFE0's truncated bytes added by 03E1D0, elevations x 1.570796, azimuth `k x 6.283 / segments`).
- `src/world/font`: the names' two pages ('font font', 'menu ustatshd', DXT3 256x256) decoded from the menu DAT (file
  0x9A76) with chars/tex, and the 'font fontshp' set parsed (for the tests).
- `src/world/world_render`: one root signature with root constants and root SRVs only (no descriptor heaps for the
  draws into the world: dgVoodoo's stay bound; the pages are an RGBA8 buffer filtered in the shader as LINEAR + WRAP),
  a second with a table for A's depth when drawing over the screen; one upload ring per pass (16 slots).
- `src/world/world_module.cpp`: the three own-frame modules (`/hifi sky`, `/hifi names`, `/hifi grid`), the marker
  handlers, the gates, the upscale overlay.

### Sky dome
- Recorded: 03D640's arguments as 03DB60 passes them (radius = 0.8 x (far - near), 8, segments, the 8 colours as
  D3DCOLORs, the 8 heights) and the device's WORLD x VIEW x PROJECTION from the wrapper's cache (`[wrapper] +0x93C,
  +0x97C, +0x9BC`), which 03DB60 has set by the time of the call. Everything else of 03DB60 stays the game's: the clear
  colour (014D90, before and after), the state block, the transforms, the states it puts back.
- The session's first 21 strips: 03D640 leaves a strip out while `[VA 04AECB08]` <= 0x14 and counts it. With native
  drawing on, `on_dome` counts and leaves out the same strips (the counter moved on as the game would).
- Drawn: each strip one triangle strip of 2 x (segments + 1) vertices, colour and alpha the vertex's (the state block's
  SELECTARG1 DIFFUSE), no blending, no depth (the game's dome writes none, and the depth is the clear's 1.0 there), the
  viewport's corner at 0.5 (dgVoodoo's for Direct3D 8's pixel centres), as A's own target.
- **World FX's sky**: its repaint keys on pixels still the colour `dome_colour` gives (grass.hlsl, grass_render.cpp).
  The native dome is the game's to the byte (strip geometry and colours from 03D640's own arithmetic), so the repaint
  sees the same pixels with Sky on, and Sky off shows the game's dome as it was. Tested: on WARP the native dome is
  within 0.5 byte of `grass::dome_colour` everywhere (mean 0.15), the tolerance World FX's own sky was built against.

### Names
- The game keeps all of its own logic: the actor walk and every check of DrawActorName (first person, /names, the
  model's occlusion cull +0xB2, the distance scale), the string with its icons (rank, linkshell pearl, GM, Ballista...),
  the projection with the frame's camera and jitter, the layout and **the glyph scale, whichever code is at 086378 /
  086A5C**: nameplate.dll's (both dumps have it) or the vanilla code. What reaches the device per glyph is recorded
  instead of drawn: the 4 XYZRHW vertices (A's pixels, z the name point's NDC z), colour after 086210's conversions,
  UVs; the page from 120590's choice (the last of the font's first codes at +0x24/+0x28 at or below the character);
  ALPHAREF 0x60 when the colour 120590 handed back is the linkshell colour (086210 0867E6, 086900 086D77), else 0. Each
  name's arguments are kept too (point, text, scale, colour, linkshell colour; the star line marked).
- The PNG font 'font high' (config 0x24) is the text object's (11BAE0: chat, menus); the names' font object binds only
  its two DAT pages (120590 -> +0x1C / +0x20, loaded by name by 120660). R.
- **Into A** (no FSR, or `/hifi names over off`): colour = texture x vertex x 2, alpha = texture x vertex x 4,
  saturated; alpha test GREATER ALPHAREF on the 8-bit alpha; blended SRCALPHA / INVSRCALPHA (alpha too); depth test
  LESSEQUAL with the name point's z, written where the glyph passes (the depth contract of notes-ui-draw); glyphs in the
  game's order. The world's depth planes are taken to DEPTH_WRITE and back from the states zone/inject follows.
- **Over the screen** (while the upscale draws: it called the overlay in the last 250 ms): the names' frame is kept at
  their marker and drawn by the upscale's overlay right after the upscaled world lands on the back buffer, before the UI:
  each vertex `(x + 0.5 - jitter) x screen / A`, so the glyphs are where A's would be without the jitter, sampled at the
  screen's resolution; the depth test against A's depth at the matching pixel (`screen x A / screen + jitter`, the
  name's z as a 24-bit depth); no depth to write there, so glyphs are drawn far to near (by z) instead.

### Mog House layout grid
- Recorded per outline: the 4 corners (177B40's first argument, stride 16), the colour packed as 177B40 packs it
  (A = 5th argument, R G B the 2nd to 4th), the depth flag (6th); VIEW x PROJECTION from the wrapper's cache (WORLD is
  identity: 1779C0). Drawn as 4 lines each (a line list), the colour as is, no blending (I: the state the names pass
  leaves), depth off unless the flag is set (then LESSEQUAL with writes, the inherited state).

### Gates and options
- Per pass: native only when its option is on (`[world] sky / names / grid`, default on), its game side is in, the
  renderer is made (and the pages read, for the names), its markers come through (32 half-seconds of counts: at least one
  seen, no more than 8 fewer seen than sent, as the plugin's zone check), none of its draws failed in the last 2 s, and
  for the sky the world's target is known (the sky's marker comes before the zone's, from which it is learnt). Until
  then the game draws it; a frame recorded as native that can't be drawn is a failure (2 s of the game's again).
- `[world] names_over` (default on): the names over the upscaled screen while FSR draws.
- `/hifi sky|names|grid` status; `... on|off`; `/hifi names over on|off`.

## Shared files touched (small, additive)
- `build.cmd`: `PASSES_WORLD` (my line), and the world_test block before the menu test's.
- `src/upscale/upscale.hpp/.cpp`: `set_overlay(fn)`, called in both FSR paths right after the upscaled picture is drawn
  onto the screen's target (the target in RENDER_TARGET), before the upscale puts dgVoodoo's heaps and state back.
- **Marker kind 12 is taken for the grid** (`worldmark::kGrid`). For the integrator: name it in slotmark.hpp
  (`grid_end = 12`) when merging.

## Tests (`build\world_test.exe build\test\world_oracle.txt`, 81 checks, all pass)
- Both dumps: every place where research/ffxi-re has it (03DD00 -> 03D640, the counter VA +65CB08, the wrapper's global
  +45666C; the names pass's slot +329C38, 0D08A3 -> 086210, 0D08C9 -> 086900, 0867D2 / 086D62 -> 120590, 08689D /
  086E27 -> 00CD00, the phase global; 1D5DE0 -> 1CF110, 1CF4BD / 1CF607 -> 177B40), nameplate.dll's call at 086378 left
  alone, ours in, stop() leaving the dump byte for byte.
- Recording with stand-ins for the game's callees: the sky (arguments, transforms, the skipped strips and the counter,
  03D640 called only with native off, segments <= 2 left to the game), the names (outside the field nothing; each name,
  each glyph, the pages and ALPHAREF of letters, the pearl and the stars; native off: every glyph to the game), the grid
  (colours, corners, depth flag, transforms); each end marker with its seq.
- `tests/world_oracle.py` (03D640's arithmetic as read, written independently; chartex.py, ui_sprites.py): 359 domes /
  3,281 strips from the weather keyframes of ten zones, every band, elevation and colour to the bit and the vertices;
  both name pages' pixels; all 146 'font fontshp' records.
- WARP with the debug layer: the dome against World FX's `grass::dome_colour`; names into A (each pixel the combiner's
  result, a glyph behind a wall hidden, a farther glyph kept off a nearer one's depth writes, an icon's 0x60 alpha test);
  names over a screen twice A's size (placed without the jitter, hidden by A's depth); the grid's outlines in their
  colours exactly.
- hook_test, upscale_test, menu_test pass with the change.

## Open (with next steps)
1. **The dome's camera lags one frame (I).** The game draws it with the effect system's copy of VIEW, taken at priority 8
   of the frame before (0441C0), so the dome turns a frame behind the zone. Kept, so the result is the game's. Next: in
   `skydraw::on_dome`, build the matrix from gGame +0x94 (VIEW, translation zeroed) and +0x154 (PROJECTION, jittered)
   behind an option, and check with World FX's Sky on that its repaint (which uses its own camera) still keys on every
   bare pixel.
2. **Names and mods.** The pages are read from the DAT on disk (`bake::dat_path`): a mod that replaces the menu DAT
   (an XIPivot overlay, a HD font) shows in the game's names but not in HiFi XI's. Next: resolve the DAT through the
   overlay's path, or take the pages from the game's own texture objects (font object [VA 04971BF8] +0x1C / +0x20,
   texture +0x40 / +0x44).
3. **Names over the screen**: (a) frame generation (FSR 3 FG) takes FSR's picture without the names, so in generated
   frames the names are UI (not moved with the world); (b) occlusion is A's depth at A's resolution (glyph edges sharp,
   the hiding edge as coarse as A's); (c) overlapping names are painter-sorted instead of depth-written; (d) World FX's
   passes after the world (fog, sky repaint) no longer cover the names in this mode, as they did in A. Each needs a look
   in game; `/hifi names over off` gives the game's way.
4. **A's depth for the overlay must be typeless** (R24G8 / R32G8X24) and single-sampled; dgVoodoo's actual depth format
   isn't checked offline. If it isn't, the names stay in A (`/hifi names` says why under "over the upscaled screen").
5. **dgVoodoo scaling A** (its resolution override): pre-transformed glyphs are mapped as `(x + 0.5) x target / game`
   (I); not tried.
6. **The grid's blend state** is inherited in the game (I: off after the names pass); drawn opaque. Which value 1CF4BD's
   caller passes as the depth flag isn't traced (both are handled).
7. The font is read (the menu DAT, 4.3 MB, and two 256x256 DXT3 decodes) on the game's thread when the Direct3D 12 device is first seen.
8. Battle numbers (element kind 0x5C) use the same font object but belong to the fx part.

## What needs checking in game
- `/hifi sky`, `/hifi names`, `/hifi grid`: "HiFi XI draws it" once in the field; no "failed" counts climbing.
- Sky: identical with `/hifi sky off` and on, with World FX's Sky on and off, by day and night, in a weather change; the
  dome present from the first frames after login (the 21 skipped strips); interiors (no dome) unaffected.
- Names: same place and size as the game's (nameplate.dll in), icons (linkshell pearl in its colour, GM, Ballista),
  the star line, names hidden behind walls, trees and characters pixel by pixel, the distance fade; targeting colours.
- With FSR on: names sharp at the screen's resolution, steady while the camera moves (no jitter shimmer), still hidden
  behind walls; with FSR 3 frame generation: how names look in generated frames.
- Mog House layout mode: the grid's cells in red / green and the item's footprint in blue or grey, as before.
- facelift's own names (ImGui) unaffected (they turn the game's off; nothing is recorded then).
