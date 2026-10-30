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
| CMoDist (0x32B298, kind 0x22) | 0426F0 | a grab (the world so far into the 256 x 256 distortion target through the unshifted diamond's screen points) and the fan drawn shifted (below) | a subtractive letter (BLENDOP stays REVSUBTRACT), no room |
| CMoVuDist (0x32B2F8) | 043110 | the game's walk 063C80 run with a counting callback of ours, then that many CMoDist quads at the element's place (below) | more than 64 points |
| CMoDistModelElem (0x32B418) | 043320 | the D3M's vertices, their UVs by 042D00 through the draw matrix moved by +0x194, the whole world grabbed, one draw with 352740's stages (below) | - |
| CMoDistMorphElem (0x32B358) | 043590 | the morph blended (03F8A0), its positions' UVs by 042D00, the whole world grabbed, WORLD moved by +0x1C0, 352258's stages (below) | a blend handed in (+0x1BC); 042D00 answering 0 (it keeps the blend then) |
| CMoDistRingElem (0x32B4D8) | 043720 | its bands (angles 0, d .. (n-1)d, 0), the colours x the element's >> 7, UVs by 042D00, the whole world grabbed, 353378's stages (below) | lit (its normals aren't written), more than 4 rings |
| CMoD3mSpecularElem (0x32AD20) | 0412B0 | the D3M with an environment stage: its texture matrix by the game's math vt+0x58, the camera-space normals, the tint (below) | the stages elsewhere than the tables' (+0x1BC, +0x1BD), a texture wave (+0x148), the environment not to be had |
| lens flares (CMoLfdElem 0x32B538) | its own vt+0x2C 0548D0, its call of 03EBF0 at 054B81 | the pieces of the flare D3A pre-transformed as 03EBF0 places them (below); 0548D0 itself (the visibility test, the glare colour 014F70) still runs | a texture not found or not up |
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

**Distortion: CMoDist (0426F0) and CMoVuDist (043110).** 0426F0 (R) projects the diamond (0, 0, 0), (-1, 0, 0),
(0, -1, 0), (1, 0, 0), (0, 1, 0) with the draw matrix through math vt+0x80 (gfx +0xB10, the half sizes gfx +0xDC0 /
+0xDC4: x = (x/|w| + 1) x +0xDC0, y = (1 - y/|w|) x +0xDC4; fxdraw calls the game's own) into pixels of the game's
target (009E00: wrapper +0x98 + [+0x158] x 0x18, W +8, H +0xC); UV = those / (W, H). Without the world's texture
([VA 048E666C] +0x194: the world is drawn into a texture the main loop blits) it draws nothing. The five points' box
is moved to 0, each side scaled by 255 / size from 256 up; the grab (state blocks 3530D0 / 353028: no blend, no z,
colour the texture, alpha TFACTOR 0x80, LINEAR, MIRROR) draws the strip of points 1, 2, 4, 3 at the moved places with
their UVs into the game's 256 x 256 target (VA 0490BB88, made by 044270), sampling the world's texture as drawn so far.
Then the fan (353178 / 353108: SRCALPHA / INVSRCALPHA whatever the letter, z test, LIGHTING and fog off, colour
TEXTURE x DIFFUSE x 2, alpha DIFFUSE x TFACTOR x 4 with TFACTOR 0x80808080 as 3530D0 left it, MIRROR as 353028 left it)
of the centre (colour VA 0490BC0C) and points 1 2 3 4 1 (0x00808080: alpha 0), UVs the moved places / 255, WORLD the draw
matrix with +0x194 added to x and y: the world under the diamond, shown shifted, fading out to its edges. A letter that
set REVSUBTRACT keeps it (353178 doesn't set BLENDOP): the game's. **VuDist**: 063C80 (ecx +0x198) calls 0430D0 at each
point, which sets the point into the draw matrix and calls 0426F0, but 0426F0 builds the draw matrix again first: all
the quads land at the element's own place, each grabbing what the one before drew (R; reproduced). fxdraw runs the
walk with a callback of ours that only counts (the clock moves on as in the game), then records that many grabs and
fans. On the Direct3D 12 side (`fx_renderer::grab`) a draw with a grab first copies the world's target (resolved when
multisampled) into a copy of ours, draws the grab's quad of it into a 256 x 256 target of ours (pre-transformed corners
as D3D9 places them: + 0.5 pixel), puts the world's target back and draws with that target as its texture (textures'
reserved descriptors 1 and 2; static sampler s1 MIRROR).

**The 042D00 distortions (043320, 043590, 043720).** 042D00 (R; thiscall, (vertices, count, M, stage), ret 0x10)
projects each 36-byte vertex with math vt+0x84 (M, gfx +0xB10, the half sizes gfx +0xDC0 / +0xDC4) into its UV slots,
then UV = ((x + 0.5) / W, (H - 0.5 - y) / H) in the game's target (009E00); it answers 0 (nothing drawn) when W or H
is 0 or without the world's texture ([VA 048E666C] +0x194). Then it grabs **the whole world, flipped**, into the 256 x
256 target: the strip (0, 0) (256, 0) (0, 256) (256, 256) with UVs (-0.5/W, (H-0.5)/H), ((W-0.5)/W, (H-0.5)/H), (-0.5/W,
-0.5/H), ((W-0.5)/W, -0.5/H) (35322C / 3531C0: no blend, no z, colour the texture, alpha TFACTOR 0x80, MIRROR), sets
ALPHABLENDENABLE and ZENABLE 1 and binds the target at the stage handed in. fxdraw calls the game's own vt+0x84 on
the vertices it records (`dist_uvs`), and the grab is `fx::grab` as for CMoDist (`dist_full_grab`). Per class (R):
- **The mesh (043320):** a weather element 002D80 hides or an MMB (+0x179): nothing. The D3M's vertices (03FD70,
  triangles x 3) copied, M = (identity moved by +0x194 in x and y) x the draw matrix (math vt+0x24 on 06A4A0's cache),
  042D00 (the copy; from vertex 2, count - 2, when 03FCB0 answers: no textures and kinds 5-7, the first vertex
  overwritten by the load; stage 1), WORLD the plain draw matrix again (06A480), the copy into the distortion buffer
  (VA 0490BBB0), then 040670 (the resource, that buffer, VA 0490BC0C, the element, 0): ALPHATEST GREATER 0x60 with the
  data's +2 bit 0, TEXTUREFACTOR the colour (D: alpha >= 0x7F full), table 352740 (stage 0 CURRENT x TFACTOR x 2,
  colour and alpha; stage 1 TEXCOORDINDEX 0, colour TEXTURE, alpha TEXTURE x CURRENT x 4, WRAP; D: 352848, alpha x 2),
  stage 1's UV offset (+0x120 with +0x17B bits 0-1); no texture set (its last argument 0 skips 075700): each texture
  run, then the rest when +5; without textures only when +5, from triangle 1 when 03FCB0 answers. Drawn here as one
  draw (`stage_dist_model`). Lit as 0467E0 lights it (lit_ok), fog and blend the letter's.
- **The morph (043590):** the blend 03F8A0 into +0x1BC unless one's there (then the game's); the draw matrix; 042D00
  on the blend's P positions (M the plain draw matrix; stage 0: the target is the morph's texture); when it answers 0
  the element is the game's (it keeps +0x1BC then). WORLD = (identity moved by +0x1C0) x the draw matrix (06A480), then
  03F440 with the blend and flag 1: no texture set, the colours as they are (not doubled), each corner's UV its
  position's; +0x1BC cleared, +0x194 copied to +0x1A8. Table 352258 as the plain morph (sampled WRAP).
- **The rings (043720):** n segments (+0x1B4), up to 4 rings (+0x1B5: the game's stack has room for 4); each ring's
  colour its bytes +0x1A4 + 4i (R G B A) x VA 0490BC0C's >> 7, kept to the low byte (no clamp); per band 2n + 2 vertices:
  at angles 0, d .. (n - 1) d (added up) the pair (sin a x r, cos a x r, 0), inner then outer, then the first pair
  again (unlike 0565A0's 0, 0, d ..); normals not written (lit: the game's). 042D00 on all of them with the plain draw
  matrix (no shift: the world under the ring through the 256 x 256 copy), 353378 (colour CURRENT x TEXTURE x 2, alpha
  DIFFUSE x TEXTURE x 4, MIRROR, stage 1 off; `stage_dist_ring`), a DrawPrimitiveUP strip of 2n triangles per band.

**The specular D3M (0412B0).** Its pre-draw (vt+0x4C 041160, run by 046070 before the setup, so before fxdraw sees
it) clears flags bit 16 (unlit) and sets bit 25 (no fog), sets the stages +0x1BC 0 and +0x1BD 1 for a D3M without
textures, and binds the environment at stage +0x1BD with 0759C0: the element's own (+0x1A8's resource +0x30 when
its first dword is set) or the global 'cubemap spec' (VA 0490BB90, 044250); its tint +0x1AC, unless +0x198 bit 0
its R G B x VA 0490BC0C's / 128, truncated to the low byte: 0x80808080 binds the texture as it is, anything else a
copy of it x the tint x 2 (`stage_spec_tint`). 0412B0 (R): an MMB goes to 040C20 (record_mmb, as the plain D3M);
ZENABLE 1, TEXTUREFACTOR VA 0490BC0C as it is (no D change); stage +0x1BD's texture matrix: math vt+0x58 of (0, -pi/2,
0), or with +0x198 bit 0 of (-+0x19C, -(+0x1A0 + pi/2), -+0x1A4) times (bit 1 too) the camera's matrix ([VA
048E68FC] +0xD4, 015230) without its translation (math vt+0x24); fxdraw calls the game's own. Without textures:
3529B0 / 3528F0 (NORMALIZENORMALS 1; stage 0 CURRENT x TFACTOR x 2, alpha x 4; stage 1 TEXCOORDINDEX
CAMERASPACENORMAL, COUNT3, colour MODULATEALPHA_ADDCOLOR (CURRENT + CURRENT's alpha x the environment), alpha
CURRENT), stage 1 ADDRESS CLAMP with +0x198 bit 3, else WRAP. With textures: +0x1B8 bit 1 352B24 / 3529C8 (TEXTURE x
DIFFUSE x 2, alpha x 2; x TFACTOR x 2; the environment at stage 2) else 352C60 / 352B40 (TEXTURE x DIFFUSE x 2,
alpha x 4; the environment at stage 1), the alpha after the texture's stage SELECTARG1 with letter d or flags bit
28, else MODULATE4X with TFACTOR; the UV scroll at stage +0x1BC; each texture run (075700: a wave when +0x148, the
game's), the rest with 352C78 (the untextured table, no environment). On the Direct3D 12 side the vertex shader
makes the environment's coordinates from the element's record in the frame's lights (`element::spec`: WORLD's 3x3
inverted and transposed, the texture matrix's columns 0 and 1, the tint) and the pixel shader samples it (root
table t5, `element::env`; static sampler s2 CLAMP). Taken only with the stages where the tables put them (+0x1BC 0;
+0x1BD 1, or 2 with three stages). Kinds 5-7 without textures take the first vertex's y from the DAT as the D3M does.

**Lens flares (0548D0 -> 03EBF0).** The flare's own draw (vt+0x2C, so the run is closed before it as before) works
out the light's screen point, runs its visibility test (06C280 at 054AE2: the game's readback, or World FX's VisTest
answer when that's on) and, when it passes and the flare has a model, calls 03EBF0 (thiscall on the flare D3A's
resource, (visibility, x, y, W - x, H - y, 0), ret 0x18) at 054B81; World FX's HideFlare (054AF1, on by default) jumps
past that call, so a hidden flare never reaches us. That call goes to `on_flare`: in a flush that's taking, the pieces
are recorded (R, 03EBF0): resource +0x35 pieces from +0x48, each a 4-byte head (+2 its quads), the float t at +4, then
quads x 6 vertices of 24 bytes; each vertex x = W / 16 x vx + t x ((W - x) - x) + x (y the same with H: the pieces lie
on the line from the light through the screen's centre), alpha min(255, its alpha x visibility >> 8); the texture as
075690 finds it (the 16 bytes +0x44 points at: their first dword a texture object, else the texture manager [VA
0490B970] +8 list searched by name bytes 8-15 at node +0x14, next +0x38); 352180 / 3520A8: SRCALPHA / ONE, no z, no
lighting or fog, colour DIFFUSE x TEXTURE x 2, alpha that x TFACTOR x 2 (TFACTOR VA 0490BC0C, set by 0548D0), WRAP.
They're drawn pre-transformed (`proj_identity`: WORLD takes the game's pixels, centres on whole numbers as D3D9 has
them, to clip space). Anything not found: the game's 03EBF0, the run closed first. The rest of 0548D0 (the glare 014F70)
runs as before.

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
  its first draw left to the game); a CMoDist (the diamond by the game's vt+0x80 into 640 x 480: the grab's corners and
  UVs, the fan, WORLD shifted; nothing without the world's texture) and a CMoVuDist (the walk counting 3: three grabs and
  quads, the clock moved on); the 042D00 distortions (Fire's 'fa04' as a distortion mesh: its UVs by the game's
  vt+0x84 through the draw matrix moved by +0x194, against the projection by hand; nothing without the world's texture;
  'rng ' as a distortion morph at 1:3: the corners' UVs their positions', the colours not doubled, WORLD moved by +0x1C0,
  the side effects; rings of 8 segments and 3 rings: the bands, the colours x the element's >> 7, the UVs; a lit one
  left to the game; the whole-world grab each time); a lens flare (ROM/0/40 'lf01', 17 pieces: on_flare from a flush, its 108 vertices placed
  as 03EBF0 places them, alpha x visibility, its texture found by name in a made-up texture manager; the call at 054B81
  patched and put back); a specular D3M (Fire's 'fa04' with the pre-draw's fields set by hand: the two-stage table, the
  environment's id and record, the texture matrix's columns from the game's vt+0x58; its own angles, CLAMP and a tint;
  three stages with the environment elsewhere left to the game; 06DCF0's rotation statics made by hand: their first use
  registers with the game's atexit, not up offline). 155 checks. The dumps' place is kept reserved around each dump while it's tested (WARP's and the decoders'
  threads put their stacks there otherwise).
- `build\fx_render_test.exe build\test\fx_fire.bin build\test\fx_cure.bin build\test\fx_fastblade.bin`: the D3D12 side
  at its marker on WARP with the debug layer, on a list recorded as dgVoodoo records (its own heap and targets),
  against `effects_play.py` through `tests\fx_oracle.py play` (Fire frame 150, Cure frame 60, Fast Blade frame 38: D3M and
  D3A elements with their real chunks and textures; Southern San d'Oria's fountain frame 120: 36 MMB meshes drawn from
  DAT 330's data as the native zone renderer loads it, `zonemesh::load_file` and a gpu_zone on WARP): mean difference
  0.00-0.05 of 255, no pixel off by more than 24; the debug layer silent. Plus the blend table (trail colour added, B
  subtracted, F darkening), 183760's three tables (plain, the textured mode, untextured) and a lit element (vertex x
  (ambient + diffuse x N.L, N not normalized) + emissive) by value, and a distortion quad by value (the world red and
  blue, the fan shifted 8 pixels shows red where the world is blue: the grab, the copy and the 256 x 256 target), the
  042D00 distortions by value (the whole world grabbed flipped; the mesh's stages sampling 16 pixels left and up, WRAP;
  the ring's sampling 16 pixels left, MIRROR) and a
  lens flare's stage pre-transformed (a quad over the screen's left half: 128 64 32, the right half untouched), a
  specular D3M's stages (untextured, the environment white: 160; tinted 0x40: 96). 45
  checks (with the fountain's DAT).
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
5. (Done: VuAnm, VuDist.)
6. (Done: CMoDist, CMoVuDist, the grab on the Direct3D 12 side, and the 042D00 distortions 043320, 043590, 043720.)
   Each 042D00 element copies the whole world target and draws it into the 256 x 256 target, as the game does; a
   busy scene with many of them costs a full copy each (the game's own cost too). The half-pixel of D3D9's pixel
   centres in 042D00's UVs is kept as the game computes it (the world's geometry isn't shifted by it either).
7. (Done: the specular D3M, 0412B0.) The global environment is the texture named 'cubemap spec' (a 2D picture, sampled
   by the camera-space normal through the texture matrix: D3D8 takes (u, v) of the 3-vector for a 2D texture); its
   decode isn't checked on real data (should it fail, the element stays the game's). The tinted copy 0759C0 makes
   (0755A0, the source's size) is done as a tint in the shader: a filtered sample x the tint rather than the filtered
   tinted texture (the same up to the saturation at 1).
8. **Light spots** (0x32B3B8 040E70, CMoD3aLightmapElem 03EE70) stay the game's (not started this round: two new
   pieces of plumbing, below). Read (R, 2026-10-30): 040E70 pushes WORLD = the local matrix +0x60 (06A480), 054D70
   makes the 256 x 256 target (VA 0490BB88) the render target, clears it (Clear flags 3: colour and depth, 0, z 1),
   pushes VIEW and PROJECTION (06A420, 06A4C0), sets the viewport 256 x 256, VIEW = a look-at from WORLD's translation
   to 2 yalms below it, up (0, 0, -1) (2D7FF9 with VA 0490BC7C), and WORLD = scale 0.25 x WORLD (math vt+0x24); then
   the D3M (040220) or the MMB (040C20) is drawn into it as usual (the 03EE70 kind: the D3A 03EB30, then the point
   from the draw matrix +0xD0..+0xD8); 0472F0 puts the states back, 06A450 / 06A4F0 the transforms, 06A0E0 the
   target; 0467E0 runs again (the element's states for what follows), and 054F80 sets VIEW = the camera's (gfx +0xB50,
   06A520) and calls 182700 (thiscall on the zone renderer [VA 04AC1F24]; the point, the 256 x 256 target's texture,
   10.0): texture 0 the target, ADDRESS CLAMP, FVF 0x142; a planar map (1 / 10 a yalm, the point at the middle:
   VA 04807338 / 04807340 / 04807344 / 0480734C) composed through 16C060; then **the collision map's** cells (renderer
   +0x178, 167940; cell = int((x + map +0x38) x scale), the 3 x 3 cells round the point, clamped to the map's +0x40 x
   +0x44; cell lists at +0x58) walked: for each entry (mesh +0 positions, +4 normals, +8 triangles of 4 words, +0xC
   count; instance +0x100 its WORLD, skipped when its +0xBC is the renderer's +0x17C or the mesh's +0xE is set),
   WORLD = the instance's matrix (00BD50 0x100), and each triangle whose normal faces the map's down (dot <= 0) as 3
   vertices (position, 0xFF808080, UV = the map of the position) into VA 04AC9088 (at most 1500 a mesh), one
   DrawPrimitiveUP list a mesh. Blend, z and fog are 0467E0's. Next: (a) a run drawn into a 256 x 256 target of ours
   (the distortion target's resources: it is the same game target) with the look-at VIEW and the element's
   PROJECTION, the element's own draws as now; (b) 182700 done on the game side over the same collision cells in
   memory (pure reads, as faithful as the game), its triangles recorded as a draw with that target as texture, WORLD
   the instance's matrix through the camera's VIEW; the element's blend letter. The game's own 054D70 and 182700
   can't be called for us (they draw).
9. (Done: lens flares, their pieces.) Their occlusion stays where it is: the game's readback at 054AE2, or World FX's
   VisTest answer (src/render/vistest, kind flare) when that setting is on; HideFlare (on by default) still hides
   them, so they're drawn natively only with it off. Not checked: the flare's texture lookup on real data (the name's
   first dword: an object, or 0 and the list).
10. **Battle numbers** (kind 0x5C, 041AF0 -> 041C20) stay the game's: text quads from the game font (glyphs bound by
   1207B0 from the font object [VA 04971BF8], not texture chunks, so fxtextures can't take them as they are); whether
   Phoenix ever starts them is open (notes-world-passes 5). Next, if they show: the glyphs' Direct3D textures (the font
   object's) copied once, 041C20's projection and centring on the game side.
11. **Aura ring** (038A80/038E10, inside the aura actor's see-through draw): N offset tinted copies of the native
   character's private target T (charinject's see-through path) before the composite; needs a hook in the actor's draw
   (02C19B/02C1A2) and chars' T: coordinate with the actors/chars owner.
12. **FSR motion** for effects: none (as before); the effects stay in the reactive mask (drawn between the world before
   World FX's passes and the characters' end). Not started.

## What needs a look in game
- Any spell, ability, weapon skill: the effect looks as before (colour, brightness, blending, order with see-through
  characters and names); `/hifi status` "effects:" line shows elements taken and why others are left.
- Weapon skills: the blade trails (additive swoosh), hidden behind walls.
- Night outdoors: moon (D3A), stars and sun discs (D3M) and clouds (MMB) together; weather hidden by World FX stays
  hidden.
- Water: rivers, sea sheets, waterfalls and fountains (MMB meshes) scroll as before, and where World FX draws its own
  water the game's sheets stay hidden (`/hifi status` "water meshes World FX hides" counts them). The native zone
  renderer must be up (its data on the GPU) for them to be taken.
- Effects that scatter one picture over many points (VuAnm: sparks, dust): about two in three of the points show, as
  in the game; they move on as before.
- Heat haze (CMoDist, VuDist: fire spells, some abilities): the world under a diamond shown shifted and fading to its
  edges, as before; with MSAA on in dgVoodoo the world is resolved for it. `/hifi status`: "distortion grabs".
- Shiny effect pieces (the specular D3M: an environment picture that turns with the camera added over them, as
  before; `/hifi status` "specular").
- Distortion meshes, morphs and rings (the 042D00 kinds: shock waves, ripples, see-through shapes): the world seen
  through them low-resolution (256 x 256) and shifted, as before; `/hifi status` counts them under "distortion (meshes,
  morphs, rings)".
- With HiFi XI's "Hide the lens flare" off: the sun's lens flare, its pieces along the line through the screen's
  centre, fading as the sun is hidden (with VisTest on, a few frames late, as before).
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
