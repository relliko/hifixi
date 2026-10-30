# Owning the frame: the effects (part fx, branch frame-fx)

HiFi XI draws the effect elements of the game's priority-12 flush itself, in the flush's own far-to-near order with the
see-through characters, class by class: what's reproduced faithfully is taken, everything else stays the game's,
element by element. Sources: research/ffxi-re/notes-effects*.md (draw, update, generator), notes-world-passes.md 2-5,
notes-actor-extras.md 3. Nothing here has been seen in game; everything is tested offline (below).

## What's built

### Game side: `src/game/fxdraw.cpp` (the game's thread)
Three sites, found by signature (both dumps), patched in by `start()` and put back byte for byte by `stop()`:
- **The flush** (055620's `call 055520` at 05562B) goes through a stub: `on_flush(0)` before the sorted list's walk,
  `on_flush(1)` after. At the begin a frame slot (`src/fx/fxframe.hpp`, ring of 16 by the flush's seq) is reset and the
  camera's PROJECTION (renderer +0x38DDC, jitter included) and VIEW (gfx +0xB50) copied into it.
- **The walk's callback call** (055539: `push eax; call [esi+8]; add esp, 4`, 7 bytes) becomes `call stub; nop; nop`:
  `on_entry(callback, arg)` sees every queued item before it draws. A weapon trail's callback (01C930) is recorded
  natively (when taking); anything that isn't an element drawn through 046070 (a see-through character's callback, a
  lens flare's own vt+0x2C, a trail left to the game) first **closes the run** of elements recorded so far.
- **The element setup** (046070's `call 0467E0` at 04612E) goes to `on_setup` (a `__fastcall` with 0467E0's stack). An
  element HiFi XI can draw is recorded and answered 1 (046070 jumps past the depth pull, the class draw and 0472F0's
  restore); anything else closes the run first, then goes on to 0467E0 and its class draw as before.

Closing a run publishes it (`frame.runs`, release) and sends an **fx_run marker** (slot marker kind 5, the flush's seq
in its low bits) through the game's D3D8 device, exactly as charslot sends its markers. The n-th fx_run marker of a
flush draws its run n, so native runs land between the see-through characters (kind 4 markers from charmodel, sent
inside their callbacks) and the game's own draws in the flush's order.

What 046070 does before the setup still runs: its colour into VA 0490BC0C (read as TEXTUREFACTOR), the hidden check at
04609D (World FX's gamerain routine hides weather there: those never reach us), BLENDOP ADD, vt+0x4C.

Recorded per element (all read straight from memory under a structured exception guard; a fault leaves the element
to the game, nothing of it kept):
- WORLD = +0xA0 after calling the game's own `045D40` (the draw matrix is built only in the setup path, so it's called
  as 0467E0 calls it);
- TEXTUREFACTOR with 0467E0's letter changes (d, h: alpha = min(255, high byte x 2); b: the high byte), then the
  class's (D3M's D: alpha >= 0x7F -> 0xFF; I: | 0xFFFFFF and DESTCOLOR / SRCALPHA);
- the blend by the letter (`fx::letter_blend`, the jump table at 04725C/047234); A/B/C/b (REVSUBTRACT) only when the
  device reports blend ops (gfx +0xDEC bit 11), as 046550 checks; letters without a case (@...) left to the game
  (their blend is inherited from the previous draw);
- z write = flags bit 12; PROJECTION: the depth pull (not bit 27, not a sky layer bit 22, +0x128 != 0, |view z| < 32:
  near and far less (1 - |z|/32)^3 x 0.03 x +0x128, plus the renderer's +0x39594 when +0x39598) or screen-fixed
  (bit 0, bits 6-8 clear, bits 2 and 3: fov 2 atan2(192, 350)); the D3D12 side rebuilds _33/_43 (and _11/_22) from them;
- fog unless bit 25: the game's own `18BA20` on the environment [VA 04AECB04] for the generator's area (colour, end,
  start), table fog (wrapper +0x92D) by view depth, vertex fog (+0x92E) by depth or distance (RANGEFOG +0x92F); H: fog
  colour black;
- TEXTURE0's offset (+0x120, +0x124) when +0x17B bits 0-1.

Classes taken (by the vtable's class draw, vt+0x48):
| Class | Draw | What's recorded | Left to the game |
|---|---|---|---|
| D3M (0x32AD80) | 040BA0 -> 040220 | vertices from the chunk in memory (03FEE0 doubled the colours in place), one draw per texture run (3524A8 / bit 28 3523D0 / I 352590), the untextured tail (352648), untextured (352348, D: alpha x 2) | MMB models (+0x179), lit (bit 16), untextured kinds 5-7 whose DAT chunk isn't found (below), a texture not on the GPU yet |
| D3A (CMoD3aElem) | 03EB30 -> 03E910 | the frame +0x170 (wrapped to 0 at the count and written back, as 03E910 does), its quads from the chunk, UVs x 1/(texture object +0x24, +0x26) when +0 >= 1 and +7 is 0 (as the load scales them), 351FA0 / bit 28 351EC8, no fog, no z write, unlit | a texture not up |
| rings (kind 0x24) | 0565A0 | strips per band (angles 0, 0, d .. (n-2)d, 0), colours through 03E280's byte swap, TEXTUREFACTOR the element's own +0xF8 (no fade), 353810 | lit, more than 4 rings |
| weapon trails | flush callback 01C930 -> 01C940 | the 80-sample ring (newest first), one strip (tip A, base B) or two from the middle line (+0x151C), through VIEW, vertex colour only, additive, no z write, no fog | - |
| everything else | | | all of it (below) |
Elements whose class draw returns early in the game (no model, a weather element 002D80 hides, no vertex buffer, fewer
than two trail samples, no segments) are taken with nothing drawn, as the game draws nothing.

**Untextured D3M, kinds 5-7: the first vertex's y from the DAT.** For them 03FCF0's "names" begin where the vertices
do, and the load stores the vertex buffer object at names + 4 *after* copying the vertices into the (write-only)
buffer: the first vertex's y is overwritten in the chunk and survives only in the DAT. `dat_first_y` finds the file id
at the resource's root (up the parents +0x28, the root's +0x34), reads that DAT once per resource (cached, a failure
too), and takes the chunk of the same name, type and size whose other vertex positions match memory. Not found: the
game's. (Kind 3 keeps its names at +0x10, clear of the vertices.)

### Textures: `src/fx/fxtextures.cpp`
Each texture a recorded element names (the texture object in the name's first dword, its resource at object +8, the
0x20 chunk at resource +0x30) is copied once on the game's thread, decoded on a thread of ours with `chars::tex`
(level 0 only: the effects set MIN/MAG LINEAR and no MIPFILTER) and copied to the GPU on dgVoodoo's own list at the
next fx_run marker (upload buffers let go two seconds later). Keyed by resource, handle, header and data word; unused
for 20 s: let go. An element is taken only once all its textures are on the GPU; while some wait, the flush ends with an
empty run (a marker) so they're copied in even when nothing is taken.

### Direct3D 12 side: `src/fx/fxinject.cpp`, `fxrender.cpp` (dgVoodoo's recording thread)
At each fx_run marker (`passes` dispatches kind 5 to `fxmodule.cpp`'s handler): the run's vertices are copied into an
upload ring (16 slots by the flush's seq), the textures waiting are copied in, the world's target and depth bound with
views of our own (made at each marker), the depth planes put in DEPTH_WRITE and back, our heap bound, and each element
drawn: one shader for every class, the vertex shader reading FVF 0x152 by SV_VertexID (no IA state of dgVoodoo's
touched), the pixel shader the two texture stages by the draw's stage bits with every MODULATE2X/4X clamped (the PS2
"0x80 = 1.0"), D3D8's fog, an alpha test bit (for morphs, not used yet), a diffuse-only bit (trails). Pipelines by
blend (ONE/ZERO, SRCALPHA/ONE, SRCALPHA/INVSRCALPHA, ZERO/INVSRCALPHA, REVSUBTRACT, DESTCOLOR/SRCALPHA) and z write;
z test LESSEQUAL, no culling. Then dgVoodoo's heaps and state are bound again.

### The module and the gate: `src/fx/fxmodule.cpp`
A `passes` module "fx" (`/hifi fx [on|off]`, option `[fx] native`, default on): patched in every half second while
wanted; markers sent while the plugin has handed us a device; **elements taken only** in a zone, with the renderer up
and nothing failed in the last two seconds, and once fx_run markers are seen coming through (empty runs are sent as a
probe until then; sent vs seen over a 4 s window, as the plugin's marker_flow). Otherwise the game draws every element.

### FSR
The reactive mask is the world at the characters' end against the world before World FX's passes: native effects are
drawn between the two, so they count as before. No motion vectors for effects (as before).

## Shared files touched (small, additive)
- `src/host/passes.hpp/.cpp`: `world` gained the depth planes' states (`planes_known`, `ds_planes`) and the list's
  descriptor heaps (`heap_count`, `heaps`), filled by `dispatch`; `passes::on_reset` / `on_heaps` follow each list's
  `SetDescriptorHeaps`. Any pass that binds its own heap or writes depth needs both.
- `src/backend/backend.cpp`: three lines calling the above (reset, heaps, and `zone::inject::depth_planes` at a pass
  marker).
- `build.cmd`: the `PASSES_FX` line and a test block before the menu's.

## Tests (offline; all pass)
- `build\fx_geo_test.exe build\test\fx_geo.txt`: `src/fx/fxgeo` (D3M, D3A, morph) against `effects_geo.py` through
  `tests\fx_oracle.py geo`: every chunk of eight DATs (Fire, Cure, Fast Blade, Foot Kick, ROM/0/0, San d'Oria's DAT with
  the fountain, West Ronfaure's weather, a morph DAT): 121 chunks, 734 checks.
- `build\fx_test.exe`: on both FFXiMain dumps, start/stop byte for byte, every site and global where the game has
  them; then **the game's own code run through our stubs**: the flush stub, the game's walk 055520, element callback
  0555A0 and element draw 046070 with made-up gfx/device/math/camera/renderer objects (the math object's vtable the
  game's, so 045D40 builds the matrices with the game's code): native off (all the game's, no marker), the probe
  (one empty run), textures not up (all the game's), then the run order with a see-through actor, a lit element, an
  untextured kind-6 D3M, a lens-flare-like draw and a depth-pulled element; a D3A (frame wrap written back, UV scaling),
  a weapon trail, a ring. 79 checks.
- `build\fx_render_test.exe build\test\fx_fire.bin build\test\fx_cure.bin build\test\fx_fastblade.bin`: the D3D12 side
  at its marker on WARP with the debug layer, on a list recorded as dgVoodoo records (its own heap and targets),
  against `effects_play.py` through `tests\fx_oracle.py play` (Fire frame 150, Cure frame 60, Fast Blade frame 38: D3M and
  D3A elements with their real chunks and textures): mean difference 0.00-0.01 of 255, no pixel off by more than 24;
  the debug layer silent. Plus the blend table (trail colour added, B subtracted, F darkening) by value.
- `hook_test` (plain and --debug) and `menu_test` pass.
The oracles are made when missing (`build.cmd`), about a minute.

## Open (exact next steps)
1. **MMB effect meshes** (+0x179: water sheets, cloud domes, fountains; 040C20 -> 183760). Left to the game, so World
   FX's gamewater routine keeps working on them. Next: the file id from the resource root (+0x34, as
   `modelactor::file_of`), the MMB's name at mesh +0x18, its geometry from `zonemesh::load_file` (CPU vertices, indices,
   pieces) copied per frame like the D3M's, textures through the game's texture objects (each part's +4) into
   fxtextures; 183760's stage rules (untextured CURRENT x TFACTOR x 2 / x 4; textured mode d or bit 28: TEXTURE x
   CURRENT, then x TFACTOR x 4; else TEXTURE with TFACTOR, x DIFFUSE x 4) as two new stage bits; the cull box test
   (182070); gamewater's list honoured (read its routine's names, `gamewater::where().memory`).
2. **Lit D3M/rings** (flags bit 16): record the area environment's lights as 0467E0 asks (`18B960`/`188B70`/`18B9A0`
   for lights 0-1, `18B9E0` AMBIENT, material VA 0490BC28) and do D3D8's fixed-function lighting in the vertex shader.
3. (Done: untextured D3M kinds 5-7, the first vertex's y from the DAT.) A DAT read on the game's thread the first time
   such a mesh is drawn; if that shows as a hitch, move it to the textures' decoder thread (the element is the game's
   until it's back).
4. **Morph** (03FBB0 / 03F440): weights +0x194 (5, normalized; none: the largest of +0x1A8), the chunk in memory,
   `fxgeo::morph::blend` (already checked against effects_geo), table 352258, alpha test bit 7 (`stage_alpha_test` is in
   the shader). The texture wave (+0x148 != 0: 075700 renders into a 64x64 target) left to the game.
5. **VuAnm / VuDist** (03F130, 043110): the points come from `063C80` with the instance's MT; call it with a collector of
   ours (it takes a callback) rather than re-deriving the random points.
6. **Distortion classes** (CMoDist, CMoDistModelElem, CMoDistMorphElem, CMoDistRingElem): copy the world target at the
   marker (a reserved descriptor in fxtextures' heap is ready: `textures::reserved`) and draw through the projected UVs
   (042D00, 0426F0).
7. **Specular D3M** (0412B0 with the tint target 0759C0): the env map `cubemap spec` (ROM/0/0 'spec') tinted in the
   shader (tint x 2, alpha x 4) instead of the target round trip.
8. **Light spots** (0x32B3B8 040E70, CMoD3aLightmapElem 03EE70): the element as now plus `182700`'s projection onto the
   zone (needs the zone renderer's triangles near the point).
9. **Lens flares** (0548D0: own vt+0x2C, so they close the run and the game draws them; the HideFlare patch 054AF1 keeps
   working): screen-space pieces with the occlusion from `src/render/vistest`.
10. **Battle numbers** (kind 0x5C, 041AF0 -> 041C20): text quads from the game font; whether Phoenix ever shows them is
   open (notes-world-passes 5).
11. **Aura ring** (038A80/038E10, inside the aura actor's see-through draw): N offset tinted copies of the native
   character's private target T (charinject's see-through path) before the composite; needs a hook in the actor's draw
   (02C19B/02C1A2) and chars' T: coordinate with the actors/chars owner.
12. **FSR motion** for effects (none today, as before).

## What needs a look in game
- Any spell, ability, weapon skill: the effect looks as before (colour, brightness, blending, order with see-through
  characters and names); `/hifi status` "effects:" line shows elements taken and why others are left.
- Weapon skills: the blade trails (additive swoosh), hidden behind walls.
- Night outdoors: moon (D3A), stars and sun discs (D3M) and clouds (MMB: still the game's) together; weather hidden by
  World FX stays hidden.
- Depth-pulled effects close to the camera (cures on yourself) not cut by the ground.
- Fog: distant glows (H) fading out, others to the fog colour.
- With FSR on: effects not smeared (reactive mask).
- Watch for: a missing effect for a frame or two when a new texture first appears (should not happen: the game draws
  it until it's up), anything drawn in the wrong order with see-through characters, and the "markers not seen" probe
  never ending (then the game keeps drawing everything).
