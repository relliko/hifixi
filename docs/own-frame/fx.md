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
| D3M (0x32AD80) | 040BA0 -> 040220 | vertices from the chunk in memory (03FEE0 doubled the colours in place), one draw per texture run (3524A8 / bit 28 3523D0 / I 352590), the untextured tail (352648), untextured (352348, D: alpha x 2) | untextured kinds 5-7 whose DAT chunk isn't found (below), a texture not on the GPU yet |
| MMB meshes (D3M, +0x179) | 040BA0 -> 040C20 -> 183760 | the zone's own model (below): its pieces from the native zone renderer's data, 183760's tables, WORLD plus the mesh's +0xE0 | a model not in the zone's data on the GPU (or not as the game has it: below); kind 2, bump models |
| D3A (CMoD3aElem) | 03EB30 -> 03E910 | the frame +0x170 (wrapped to 0 at the count and written back, as 03E910 does), its quads from the chunk, UVs x 1/(texture object +0x24, +0x26) when +0 >= 1 and +7 is 0 (as the load scales them), 351FA0 / bit 28 351EC8, no fog, no z write, unlit | a texture not up |
| VuAnm (CMoVuAnmElem 0x32AC10) | 03F130 | the game's own point walk 063C80 run once as 03F130 runs it (below), its 6 vertices a point written by 03EFC0 into a buffer of ours, the game's strip (from vertex 1, points x 4 - 2 triangles) cut into a list; D3A stages (351FA0 / bit 28 351EC8), WORLD math +0x810, no fog, no z write, unlit | its first draw (+0x1DC 0: it makes the corner table the walk reads), a texture wave (+0x148), no texture object (+0x140), a texture not up |
| morphs (0x32AC90) | 03FBB0 -> 03F440 | T x 3 corners: positions and normals blended as 03F8A0 does (below), colours doubled, the UVs; table 352258 (as the D3M's 3524A8), the header's bit 7 the alpha test GREATER 0x60; 03FBB0's side effects done (+0x194 -> +0x1A8, +0x1BC cleared) | a blend handed in (+0x1BC), the texture wave (+0x148), a texture not up |
| rings (kind 0x24) | 0565A0 | strips per band (angles 0, 0, d .. (n-2)d, 0), colours through 03E280's byte swap, TEXTUREFACTOR the element's own +0xF8 (no fade), 353810 | more than 4 rings |
| weapon trails | flush callback 01C930 -> 01C940 | the 80-sample ring (newest first), one strip (tip A, base B) or two from the middle line (+0x151C), through VIEW, vertex colour only, additive, no z write, no fog | - |
| everything else | | | all of it (below) |
Elements whose class draw returns early in the game (no model, a weather element 002D80 hides, no vertex buffer, fewer
than two trail samples, no segments) are taken with nothing drawn, as the game draws nothing.

**Lit elements (flags bit 16: D3M, MMB meshes, rings).** 0467E0 turns D3D8's fixed-function lighting on for them;
fxdraw works the lights out as it does, calling the game's own functions for each step (`record_light`): L = the
element's frame (045C50) without its translation; unless an MMB or the generator's +0xDF bit 1, with an attach matrix
(0473F0): L turned by 3 pi / 2 (math vt+0x78), the attach matrix fixed up (045800), L = L x it (vt+0x20); bits 6-8 1
or 2 with bit 0: VIEW without its translation through vt+0x24, L = that, turned by pi. Then one light (18B9A0, light
1 off) when the renderer's path +0x3949C is set, else two (18B960 with the generator's area, 188B70 without), each
direction through L (vt+0x50); AMBIENT 18B9E0; the material VA 0490BC28. The calls are found at their call sites in
0467E0. The device's own defaults (009A1C, 009A42: COLORVERTEX 1, DIFFUSE- and AMBIENTMATERIALSOURCE COLOR1,
EMISSIVE MATERIAL, NORMALIZENORMALS 0; I: nothing in the frame changes them before the flush) make the vertex colour
both materials, so the vertex shader computes colour = vertex x (AMBIENT + each light's ambient + each light's diffuse
x max(0, N.L)) + emissive, alpha the vertex's, N by WORLD's inverse transpose and not normalized, L toward the light
(the direction negated, normalized). Every light the environment gives is directional (188CA0 and 188B70 set type 3).
A ring has no normals (FVF XYZ | DIFFUSE): N = 0, so only the ambient and emissive light it, as D3D does. The
lights travel in the frame (`frame::lights`, after the vertices in the upload ring, a root view t4). Left to the
game: no environment or math object, a WORLD that can't be inverted.

**VuAnm (03F130): the game's own walk.** 03F130 (R) makes, at its first draw (+0x1DC 0), the corner table +0x22D
(frames x 0x30: each D3A frame's first quad's vertices 0, 1, 2, 5, table 352219) and a vertex buffer of frames x
(points +0x1D8 x 6) vertices whose template it fills once (each point's 6 vertices the UV and colour of quad vertices
0, 0, 1, 2, 5, 5, table 352218, the D3A's own UVs unscaled; normal (0, 0, -1)). That first draw stays the game's.
After it, fxdraw does what 03F130 does: the frame +0x170 wrapped (written back), the template rebuilt from the frame,
the attach matrix (0473F0, else identity) without its translation into VA 0490BF34, VA 0490B9D4 pointed at a buffer of
ours, then **the game's walk 063C80 called once** with 03F130's arguments (ecx the element's +0x194, the callback
03EFC0, the element). 063C80 reads WORLD through the wrapper's cache (06A4A0: wrapper +0x93C), which 0467E0 would have
set to the draw matrix: the cache is set to it for the call and put back after, with the cursor (`vu_walk`, also on a
fault). The walk (R): inverse(gfx +0xB90) x the attach matrix = P; WORLD without its translation T; each point =
record (+0x1D4, 0x24 bytes each) + t x its velocity + t^2 / 2 x (the walk's push +0x1BC + scale +0x1AC x its push)
(+ the 0x3E point list's point: fixed index +0x1A8, or random from an MT seeded from +0x1A4 each call), through P, plus
WORLD's translation; 03EFC0 writes corners 0, 0, 1, 2, 3, 3 through T moved to it; the walk's clock +0x198 moves on by
gfx +0xEB0. Its count is +0x1D8, or from 20 up with +0x1DA bit 0 that x 002D80's factor x 0.5. Then 03E9A0's D3A
states, WORLD math +0x810, and one TRIANGLESTRIP **from vertex 1 of n x 4 - 2 triangles**: it covers vertices 1 .. 4n
of the 6n written, so the game shows about two points in three (R: 043F40's start 1 plus the lock's start, the count
063C80's answer x 4 - 2; reproduced as it is). The strip is cut into a list, its degenerate joins left out.

**Morph weights (03F8A0).** K targets (header +2's low 7 bits, signed), the weights +0x194 over their sum; when the sum
is under FLT_MIN, the largest of the last frame's weights (+0x1A8; the first of equals) is set to 1.0 in +0x194 and
the weights are taken as they are. Positions accumulate target by target (weight x position + sum), normals the same
from their packed words (bits 0-9, 10-19, 20-29 signed, x weight x 1/512). Not normalized.

**MMB meshes: the zone's own models.** An MMB effect mesh (water sheets, rivers, cloud domes, the fountains) isn't built
for the effect: 049570 asks 183420 for the model of that name in the zone renderer's map slots (0, then 1; 170F30 on
the slot's model list), and element +0x104 holds that mesh itself. So it's drawn from the native zone renderer's data
of the slot (`zone::native::ready_zone(0/1)`, handed to fxdraw each present by the module): the model by its name
(mesh +8, 16 characters), taken only when the mesh is as that model was read (FVF 0x152 in 36-byte strips: +0x20,
+0x24, +0x2C = 5, kind 1 and not the bump shader's; as many parts as the MMB has pieces: groups +0x60 of 0x24 bytes,
+0x44 of them, parts of 0xC0 bytes; each part's vertex buffer +0x28 made, a texture object's (+4) Direct3D texture
there (+0x40, else +0x44); no texture the DAT doesn't have). The mesh-to-model answer is cached per mesh, dropped when
the zone data changes. The draws: the model's pieces in file order (runs of the same texture merged), read on the
GPU from the zone's vertex and index buffers (root views; the zone's heap bound for the element's textures, ours
again after), 183760's tables as two stage bits: untextured CURRENT x TFACTOR x 2, alpha x 4; textured TEXTURE x
TFACTOR, then x DIFFUSE x 4, alpha TEXTURE x TFACTOR x 2, then x DIFFUSE x 4; with the textured mode (letter d, or
flags bit 28) TEXTURE x DIFFUSE, then x TFACTOR x 4, alpha DIFFUSE x TFACTOR x 4 (stage 1's alpha off). WORLD = the
draw matrix with the mesh's +0xE0..+0xEC added to its last row, as 183760 does. 182070's cull (all eight corners of
the mesh's box past one side of the projection) isn't done: what it culls draws nothing on the GPU either, but for a
mesh wholly within 0.2 of the eye.

**World FX's gamewater.** Its routine sits on 183760's only call (040DC4) and skips the zone's water meshes by their
4-character name (mesh +0x18). A mesh taken here never reaches that call, so fxdraw asks `gamewater::hides_mesh` (the
routine's own list in its memory, counted and kept as seen as the routine does): a mesh on it is taken with nothing
drawn. gamerain's hidden check (04609D) comes before the setup, so weather it hides never reaches us.

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
- `src/game/gamewater.hpp/.cpp`: `hides_mesh(name)` (whether its routine's list has a mesh now, counted as the routine
  counts), for the MMB meshes drawn natively.

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
  a weapon trail, a ring; MMB meshes against a made-up map's data uploaded on WARP (the pieces, both tables by the
  letter, the +0xE0 offset, a mesh not in the data left to the game) and gamewater's list honoured (on the dump whose
  040DC4 is the game's); a lit D3M on a made-up environment (the game's own lighting calls: 045C50, 0473F0, the math
  object, 18B9A0, 18B9E0; one white light of 255 down +y, AMBIENT and its ambient, the material's emissive, normals by
  WORLD); a morph (ROM3/9/111 'rng ': its 360 corners against `fxgeo::morph::blend` at weights 1:3, then with no
  weights the last frame's largest set to 1.0, the side effects on +0x194/+0x1A8/+0x1BC; one handed a blend left to
  the game); a VuAnm (Fire's 'fai0' frame 2 at 3 points: the game's 063C80 and 03EFC0 run, every corner against the
  walk's formula with the point list's point, the strip's first two quads, the clock moved on, the cursor put back;
  its first draw left to the game). 119 checks. The dumps' place is kept reserved around each dump while it's tested (WARP's and the decoders'
  threads put their stacks there otherwise).
- `build\fx_render_test.exe build\test\fx_fire.bin build\test\fx_cure.bin build\test\fx_fastblade.bin`: the D3D12 side
  at its marker on WARP with the debug layer, on a list recorded as dgVoodoo records (its own heap and targets),
  against `effects_play.py` through `tests\fx_oracle.py play` (Fire frame 150, Cure frame 60, Fast Blade frame 38: D3M and
  D3A elements with their real chunks and textures; Southern San d'Oria's fountain frame 120: 36 MMB meshes drawn from
  DAT 330's data as the native zone renderer loads it, `zonemesh::load_file` and a gpu_zone on WARP): mean difference
  0.00-0.05 of 255, no pixel off by more than 24; the debug layer silent. Plus the blend table (trail colour added, B
  subtracted, F darkening), 183760's three tables (plain, the textured mode, untextured) and a lit element (vertex x
  (ambient + diffuse x N.L, N not normalized) + emissive) by value.
- `hook_test` (plain and --debug) and `menu_test` pass.
The oracles are made when missing (`build.cmd`), about a minute.

## Open (exact next steps)
1. (Done: MMB effect meshes, from the native zone renderer's data.) Kind-2 (swaying) and bump models stay the game's:
   183760 draws a part's +0x28 buffer with the mesh's stride, which for those isn't what the zone data holds.
2. (Done: lit D3M, MMB meshes and rings.) Not checked against a real environment's lights offline (the test's
   environment has no records loaded: 188CA0's fallback); a stream log of a lit effect would confirm the light colours
   (188CA0 can give 255) and that the material sources are the device's defaults at the flush.
3. (Done: untextured D3M kinds 5-7, the first vertex's y from the DAT.) A DAT read on the game's thread the first time
   such a mesh is drawn; if that shows as a hitch, move it to the textures' decoder thread (the element is the game's
   until it's back).
4. (Done: morphs.) The texture wave (+0x148 != 0: 075700 renders a wave of the texture into a 64x64 target) and a
   blend handed in ready (+0x1BC: who sets it isn't traced; I: the distortion morph) stay the game's.
5. (Done: VuAnm, the game's own walk.) **VuDist** (043110): the same walk 063C80 with 0426F0's distortion quad at each
   point; needs the distortion copy (6.).
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
- Night outdoors: moon (D3A), stars and sun discs (D3M) and clouds (MMB) together; weather hidden by World FX stays
  hidden.
- Water: rivers, sea sheets, waterfalls and fountains (MMB meshes) scroll as before, and where World FX draws its own
  water the game's sheets stay hidden (`/hifi status` "water meshes World FX hides" counts them). The native zone
  renderer must be up (its data on the GPU) for them to be taken.
- Depth-pulled effects close to the camera (cures on yourself) not cut by the ground.
- Fog: distant glows (H) fading out, others to the fog colour.
- Lit effects (shaded by the area's sun and ambient; spells and abilities with solid-looking pieces): as bright and as
  shaded as before, the shading turning with the camera as the game's does.
- Morphs (meshes changing shape: rings, waves, the 'rng'/'morp' pieces of some spells and abilities): moving smoothly as
  before, cut-out ones (alpha test) with the same edges.
- With FSR on: effects not smeared (reactive mask).
- Watch for: a missing effect for a frame or two when a new texture first appears (should not happen: the game draws
  it until it's up), anything drawn in the wrong order with see-through characters, and the "markers not seen" probe
  never ending (then the game keeps drawing everything).
