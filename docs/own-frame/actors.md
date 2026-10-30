# Owning the frame: the actors' part (branch frame-actors)

Two things the game still drew around characters (research/ffxi-re/notes-actor-extras.md 1 and 2) are now HiFi XI's:
cloth (type-1 meshes), so no model falls back whole any more, and the actor's extra passes (overlay and cloak, actor
+0x734) on native characters. Everything here is tested offline (both FFXiMain dumps, the DATs, WARP with the debug
layer); **nothing has been seen in game**.

## 1. Cloth (type-1 meshes)

### What the game does (notes-actor-extras.md 1)
52 Os2 chunks in 35 DATs are cloth: the outposts' conquest banners, Gigas loincloths, Treant, an NPC's coat (`teo04m`),
a hair piece and seven old equipment pieces. 0224D0 (the cloth node's draw, vtable 0x32A430 +0x1C = 025030) steps a
Verlet cloth on the CPU (023290 -> 023700, 023BD0, 024900: pinned vertices skinned, the free ones damped, gravity, the
environment's gusting wind, two constraint lists x4, bone spheres from the Sk2 tail, normals from the triangles) into a
work buffer of the node's own (60 bytes a vertex: +0x0C position, +0x30 normal, world space), then draws from it.

### The approach: the game simulates, HiFi XI draws
HiFi XI's model stub (src/game/charmodel.cpp, 02B7E3) leaves a native model through 02B6E0's early return, so the game's
0224D0 never runs for it. The stub therefore **runs the game's own step itself**, exactly as 0224D0 does: only on pose
frames ([0x48E7460] = 1), only once the model was posed (model +0x30 bit 4), with [0x48E6E40] = the aligned work buffer
and the palette pointer [0x48EEC20] = the instance's palette (put back after), then the node's reset flag cleared. The
call is `023290(actor, model, instance, buffer, node)`, thiscall on the resource (called as fastcall, edx unused). It
runs inside SEH; a fault leaves that model to the game.

Why not a port: the sphere collision (023BD0, 024020 and the 4.5 KB sweep 024410) is long and its exact behaviour
matters for the look; the game's own code is exact by construction, steps at the game's cadence (once a drawn pose
frame, throttled frames redraw the last cloth, off-screen cloth freezes) and keeps the actor Update's shift (0231C0 /
0231F0 on the same buffer) consistent. Calling it costs nothing new: the game ran it in the same place before.

The node's work vertices are then handed over as **an instance of their own**: palette row i = work vertex i (row 0 the
normal, the translation the position). The cloth mesh decodes (src/chars/os2.cpp, `mesh::cloth`) to one vertex per work
vertex, "bone i" alone at its origin with the normal (1, 0, 0), plus the triangles, UVs, textures and materials from
the command list (the same commands as Os2). So the unchanged character shader skins it to the game's positions and
normals, and everything the character renderer does applies to cloth as is:
- the same lighting (per vertex or per pixel, lamps, sky ambient, sun highlight), fog, alpha test, no culling (0224D0's
  CULLMODE NONE, the character pass's too);
- sun shadows (World FX casters: record_casters skins the same rows);
- FSR motion: the last frame's rows of the same model and instance give each vertex's move;
- see-through (the flush), hide groups, the light split.

Gates (the model is the game's, whole, as before): the cloth sites not found (signatures below), a node whose vtable
+0x1C isn't 025030 or whose buffer size doesn't match (+0x44 != n + 1), more than 8 cloth meshes or 1024 vertices, no
palette room, a step that faults. Before the model was posed once the cloth isn't drawn (as 0224D0 draws nothing then);
a buffer the step never filled (reset flag still set) isn't drawn either (the game would draw stale memory there).

Note (R, from the oracle): the game's cloth normals start at (0, 1, 0) and add the triangles' unnormalised cross
products, so on the small banner triangles they stay close to (0, 1, 0). HiFi XI takes the game's normals as they are.

### Sites (both dumps; src/game/charmodel.cpp)
| What | Pattern | Read |
|---|---|---|
| 0224D0 at 02251C | `8B464083C00F24F0A3...` | +9 [0x48E6E40] buffer global, +0x1B [0x48E7460] pose flag, +0x31 call 023290 |
| 023290's start | fixed 12 bytes | +0xC [0x48EEC20] palette pointer |
| 025030 | `8B41088B54240C51...C21800` | its call at +0x26 must reach 0224D0 (= the cloth node's vtable +0x1C) |

### Zone / model actors
Nothing to change: type-1 is an Os2 chunk kind only (02D7F0); model actors (src/game/modelactor, src/zone) draw MMBs,
which have no cloth. The 0.48 "type-1 left to the game" was the character path's os2 refusal, now gone.

## 2. The extra passes (actor +0x734)

### What the game does (notes-actor-extras.md 2, read again for this)
0D6D30 sets globals before 02B6E0 from actor +0x734; 02DC10 draws each mesh again after its own draw:
- **Overlay** (bit 0, 025C40): texture = [[[0x48E7268]] + 0x30] (the kind-10 texture reference's texture object),
  TCI CAMERASPACENORMAL, texture matrix = gGame +0x294 (3x3: the camera's own rotation, I: so the normal is world
  space) x scale/bias 0.5 x translation (+0x73C, +0x740), COUNT2, WRAP; LIGHTING 0, ALPHAFUNC ALWAYS, ZWRITE 0, blend
  SRCALPHA / ONE; stage 0 MODULATE2X(texture, diffuse = white), stage 1 MODULATE2X(current, TFACTOR = +0x74C), alpha
  alike. The blend letter ('H' / 'D', actor +0x730) is never read: both draw the same.
- **Cloak** (bit 1, 025870 + 025330): a full-viewport copy of the scene A before the model, then each mesh again with
  TCI CAMERASPACEPOSITION through PROJECTION x bias (0.5, -0.5, 0.5 / 0.5) with (a, b, 0) x PROJECTION's x and y added
  to the translation (a = [0x48E7458], b = [0x48E726C], both +0x744), COUNT4 | PROJECTED, WRAP; stage 0
  SELECTARG1(texture) with the mesh's alpha op MODULATE2X(texture, TFACTOR), stage 1 MODULATE2X / MODULATE4X with
  TFACTOR = (0x80 - actor alpha) << 24 | 0x808080; ALPHAFUNC ALWAYS, ZWRITE 0, SRCALPHA / ONE, flat shading.

### Built
- **Game side** (charmodel): the globals found in 02B6E0's code (table below) and read per model into
  `model_entry::passes` with the overlay's cache texture (handle -> reference -> object -> its 0x20 resource, copied
  as any character texture), offsets and factor, the cloak's offsets and factor, and PROJECTION ([0x48E7468]) read at
  the walk's begin into the frame. A pass that adds nothing isn't kept (overlay factor alpha 0, its default until ops
  46/47 raise it; a cloak on an opaque actor).
- **Renderer** (chardraw): `ps_overlay` and `ps_cloak` (the stages above, clamped per stage, Direct3D 8's fog before
  blending), two pipelines (one RT, depth test LESSEQUAL, no depth write, SRCALPHA / ONE on colour and alpha), a WRAP
  sampler s1. Drawn after the model's meshes (the game draws them mesh by mesh; with the depth test and no depth
  writes this comes to the same). The overlay's UV is computed per pixel from the interpolated normal, so the game's
  WRAP0 = 7 (cylindrical interpolation) isn't needed.
- **charinject**: at a see-through marker, a cloaked model first gets the world's target copied (resolved when
  multisampled) into `the scene's copy` (the cache's reserved descriptor 1), then is drawn into T with its passes, then
  composited. A copy that can't be made is counted (status) and the cloak left out.

Gates: a model with an overlay whose texture isn't on the GPU yet, or with a cloak that adds something in the opaque
walk (no copy is made there; it's rare: a fading actor goes through the flush), is the game's. If the globals aren't
found at all (a client this doesn't know), no pass is seen and models are drawn without them, as before; status says
so.

| Global | 0x04490000 dump | Pattern |
|---|---|---|
| overlay flag | +4570FC | `381D????????0F84????????B9????????E8????????E8????????50B9` +2 |
| overlay v / u / handle / factor | +4570F8 / +457100 / +457268 / +4574B0 | `8B15????????A1????????8B0D????????528B15????????5051528D8D8C000000E8` +2 / +7 / +0xD / +0x14 |
| cloak flag / PROJECTION / b / a | +4572B0 / +457468 / +45726C / +457458 | `381D????????0F84????????68????????B9????????E8????????8B0D????????A1????????` +2 / +0xD / +0x1D / +0x22 |
| cloak factor | +4574B4 | `8B15????????8B0D????????528B15????????5150528D8D90000000E8` +0xF |

### Which effects use them (D: every scheduler in every DAT; I for the names)
- **Overlay** (ops 40/41 'H', 54/55 'H' or 'D', with 42-45 scrolling the offsets and 46/47 the tint): the shared
  two-hour scheduler (`hit3` texture scrolled 0 -> 0.5 over 150 frames, tinted per job: Invincible ROM/15/107,
  Benediction ROM/15/124, Manafont, Chainspell, Perfect Dodge, Hundred Fists, Mijin Gakure ROM/98/9 `miz2`, Astral Flow,
  Meikyo Shisui, Familiar, Soul Voice, Blood Weapon, Spirit Surge); songs (Army's Paeon I-VII and Mage's Ballad `hp11`,
  Knight's Minne and Valor Minuet `du11`, Etudes `str4`); Blizzard IV/V, Thunder IV/V, Break/Breakga; the level-up
  effect (ROM/13/35 `lvup`); Spirits Within (ROM/97/105); many mob skills (the berserk family ROM/13/63 `gro`), curse
  ROM/3/27, toad ROM/170/15. 481 DATs.
- **Cloak** (ops 48/49): 21 DATs: NPC and boss warp-outs (`tco1` of `sel_`, `ult_`, `phe_`, `lady`; e.g. ROM/155/19:
  alpha 0x76 -> 0x0F over 80 frames with +0x744 at 0.1, then easing to 0), a "summon demon" `mot1` (0 -> 0.6 over 310
  frames while fading in), mob skill animations 125 and 219, item animations 110/118/210, and the system spawn fade-in
  `pop1` (ROM/0/0, played for entities with +0x144 = 6): its target 0.1 never clears bit 1, so (I) such actors keep the
  cloak armed and their plain alpha despawn `kesu` shows it too.
- **Neither**: Invisible, Sneak, Deodorize, Warp, Teleport, Recall, Retrace, Escape, Tractor (alpha fades only, op 2A),
  Raise, Reraise, Arise, Blink, Utsusemi, Stoneskin, avatar summons, Call Wyvern/Beast, Hide, Flee.
- Corrections for the notes: the "type-0x0A resource" isn't a chunk: the DAT loader (073430) makes a 0x40-byte kind-10
  texture reference for every 0x20/0x1B chunk (+0x30 the texture's name, made the texture object at fix-up 076770 ->
  075520); effects_sched.py's "scale" for ops 42-45 are UV offsets; ops 46/47 are the overlay's tint.

## Tests (offline)
| Test | What | Result |
|---|---|---|
| `cloth_test` (new; both dumps) | sites at their offsets; ROM/7/95 banner and ROM/203/121 Gigas (2 cloths, one- and two-bone) laid out as loaded (resource-manager table, 022470's pointers, nodes as 029F70 makes them, the wind and the step's pool stood in for); through charslot's walk and the model stub, cache on WARP: every frame native, the dump's own 023290 run once per pose frame, every cloth vertex and normal equal to `tests/cloth_oracle.py` (a Python port of 023290/023700/024900, spheres zeroed) over 48 frames (worst 2.6e-5 yalms); with spheres: pinned exact, all finite, unit normals; a bad node: the game's; the extra passes' globals, the overlay/cloak recorded, their gates; the dump byte for byte | 79 ok |
| `actors_draw_test` (new; WARP + debug layer) | the banner drawn from the oracle's rows: all 105 cloth vertices on drawn pixels, textured; FSR motion = the oracle's move per vertex (86 of 90); overlay: exactly (64, 32, 16) added on every pixel it touches, none beside; cloak into T: weight 0.504 for alpha 0x40, the copy's other half on 99.8% (the shift through PROJECTION and the wrap), T's alpha + w^2; none without a copy; charinject's see-through marker with overlay + cloak (the world's target copied first) | 17 ok |
| `chars_data_test` | all 52 cloth meshes decoded as cloth against `tests/chars_oracle.py`'s walk of 0224D0's commands (counts, pinned counts, triangles, UVs, textures, materials), live layout too | 751 ok |
| charmodel, charslot, charinject, modelactor, zone*, gamedraw, menu, hook_test (plain and --debug) | unchanged behaviour | see the commit |

## Shared files touched
- `build.cmd`: one block before "The menu drawn with a fake GUI" (cloth_oracle.py, cloth_test, actors_draw_test).
  PASSES_ACTORS stays empty: this part extends the character renderer and registers no pass of its own (no marker kind
  taken).
- `src/chars/*` (owned this round): os2 (cloth decode), charframe (passes, PROJECTION), chardraw (passes; model_t
  grew to 464 bytes; a second static sampler s1), charinject (the scene's copy; the cache's **reserved descriptor 1**
  is now used: tell the fx part if it wants one), cache untouched.
- `src/game/charmodel.*` (cloth step, passes).
- Not touched: main.cpp, backend.cpp, upscale, slotmark.hpp, src/zone, src/game/modelactor, charslot.

## Open
1. **The scene's alpha in the cloak**: the cloak's stage-0 alpha is MODULATE2X(copy.a, TFACTOR); the copy's alpha is
   taken as 1 (an X8R8G8B8-like scene). If the game's A carries a real alpha the weight differs. Next: a d3dlog of a
   warp-out (`tco1`) or a `pop1` spawn would show 025870's source format and the second DIPs returning to `02E9E4`.
2. **gGame +0x294** is read as the camera's own rotation (the inverse view), so the overlay maps world normals (I,
   from 04A980 using it as a billboard rotation). If it were VIEW itself, the glow would turn with the camera. Next:
   read 0x294's writer (the camera update) or compare an in-game screenshot of a two-hour glow with
   `/hifi nativechars off`.
3. **Overlay with no texture object** ([ref + 0x30] = 0: the game binds NULL) and a handle naming nothing (the mesh's
   texture stays bound): both left to the game (gate), not drawn natively.
4. **Cloak in the opaque walk** (a fading actor drawn by the walk, not the flush) is left to the game; a per-model
   scene copy there would need the opaque batch split at that model.
5. **The silhouette pass** 033800/033A70 that may draw a cloth mesh again (as for Os2) isn't drawn for native models,
   cloth or not (an original-renderer item, not this part's).
6. Cloth vertices above 1024 or more than 8 cloths a model: the game's (none in the data: the largest has 192).

## Needs a look in game
- Outpost conquest banners (West Ronfaure etc., NPC models 814-817), Gigas loincloths, Treants, the `teo` NPC's coat:
  drawn by HiFi XI (`/hifi status`: "cloth: N meshes in HiFi XI's models, N steps, 0 faulted"), flapping as with
  `/hifi nativechars off`, no shards, lit and shadowed like their poles and bodies, still when the wind drops indoors.
- A two-hour ability, a song (Army's Paeon), a level-up: the glow over native characters as the game drew it; status
  "extra passes: N overlays".
- An NPC warp-out, a summoning, a mob spawning in with `pop1`: the shimmer over the fading body; status "N cloaks drawn
  (0 scene copies couldn't be made)".
