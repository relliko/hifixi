# Descriptor heaps and root signatures

How shaders find their views and samplers in Direct3D 12, and how HiFi XI (its own `d3d8.dll` plus the plugin's
passes) keeps them. Read this before adding a heap, a root parameter or a pass that binds its own heap. Everything
here holds for the 32-bit process (no DXR there; see memory no-dxr-32bit.md).

## The rules that matter here

**Heaps**

- Four heap types: CBV_SRV_UAV, SAMPLER, RTV, DSV. Only CBV_SRV_UAV and SAMPLER can be shader visible. RTV and DSV
  heaps are always CPU only. The list reads their handles when `OMSetRenderTargets` or a clear is recorded.
- A CPU-only (not shader visible) heap is for staging. Its size is limited only by memory. Shaders can't see it.
- A shader-visible heap can sit in write-combined or GPU memory. The CPU must not read it. So `CopyDescriptors` and
  `CopyDescriptorsSimple` must take their **sources from CPU-only heaps**. The destination can be shader visible.
- **One CBV_SRV_UAV heap and one sampler heap are bound at a time**, per list. `SetDescriptorHeaps` replaces both.
  A heap type you leave out of the call is unbound.
- **Switching heaps can cost a GPU wait for idle** on some hardware. Microsoft's advice: one shader-visible heap of
  each type, set once, and copy descriptors into it. Switch only where the GPU would idle anyway.
- **After `SetDescriptorHeaps`, set every descriptor table again** before the next draw. The spec makes table state
  undefined once the heaps change: a table is read against the heap that was bound when it was set. Root constants
  and root descriptors don't live in a heap.
- `SetGraphicsRootSignature` with a different root signature makes all root arguments stale (setting the same one
  again does not). Bind the signature, then its constants, descriptors and tables.
- So a safe order on a list: heaps, root signature, root arguments, draw.

**Size limits (resource binding tier)**

- Shader-visible CBV_SRV_UAV heap: up to 1,000,000 descriptors on tiers 1 and 2; tier 3 may allow more. The 4070 Ti
  is tier 3, but keep under 1,000,000 so tier 2 cards work.
- Shader-visible sampler heap: **2048 at most**, on every tier.
- Unique static samplers across all live root signatures: 2032.
- Per stage, in all tables: tier 1 allows 128 SRVs, 16 samplers, 14 CBVs; tier 2 lifts SRVs to the full heap and
  samplers to 2048.
- Tier 1 (and tier 2 for CBVs and UAVs): every slot a table covers must hold a valid descriptor when the shader runs,
  even if the shader never reads it. Fill empty slots with null or "white" views.
- Write-combined memory for descriptor heaps is shared by all processes (often near 96 MB). A 1,000,000-entry heap
  is about 32 MB. Don't make many large shader-visible heaps.

**Root signature: 64 DWORDs**

- Hard limit: 64 DWORDs. Costs: a root constant 1 each; a root descriptor (CBV/SRV/UAV by GPU address) 2; a
  descriptor table 1, whatever its size. Static samplers cost nothing.
- Speed is the other way round: a root constant has no indirection, a root descriptor one, a table two. On some
  hardware only the first 16 or so DWORDs are fast; put what changes most first.
- The driver versions all root arguments at each change. So changing a root constant between draws is safe. Heap
  contents and buffer contents are **not** versioned.
- Root descriptors are buffers only (CBV, raw or structured SRV/UAV). No textures, no typed buffers, **no bounds
  checking**: a read past the end is undefined, not zero.
- Static samplers live in the root signature: fixed state, no heap slot, no `SetDescriptorHeaps` needed for them.

**Descriptors in flight**

- A descriptor table is a pointer, not a snapshot. Whatever sits in the heap **when the GPU runs the draw** is what
  the shader reads.
- Do not write or copy over a shader-visible descriptor while a submitted list that uses it hasn't finished. Use a ring
  with a share per frame in flight, fenced, as the d3d8 heap does.
- Writing a new view into the same CPU slot doesn't change copies already made into a shader-visible heap. Copy again.

**Root signature 1.1 flags**

- 1.0 (all of HiFi XI today: `D3D_ROOT_SIGNATURE_VERSION_1`) = descriptors volatile and data volatile. Descriptors may
  change after recording, up to the moment the list executes; never while it runs.
- 1.1 with no flags means **descriptors static** (written before the table is set, unchanged until the list is done)
  and SRV/CBV data static while set at execute (UAV data stays volatile).
- Static descriptors also make out-of-bounds buffer view reads undefined (the driver may turn them into root
  descriptors). Moving to 1.1 means checking every ring and every buffer index, or marking ranges
  `DESCRIPTORS_VOLATILE`.
- Breaking a 1.1 promise is undefined behaviour that differs by driver. The debug layer can check it.
- 1.1 needs `D3D12SerializeVersionedRootSignature` and a `CheckFeatureSupport` (ROOT_SIGNATURE) check.

## Where HiFi XI relies on this

**Our d3d8.dll** (`src/d3d8/gpu.cpp`): the one pattern Microsoft recommends.

- `gpu.cpp:9-11`: `kSrvHeap` = 3 x 32768 shader visible, a third for each frame in flight (`kFrames` 3,
  `gpu.hpp:27`); `kSamplerHeap` = 2048, the sampler limit, cut into tables of 8 (`kMaxStages`).
- `gpu.cpp:88-92`: CPU-only pools (RTV 4096, DSV 1024, SRV 65536) and the two shader-visible heaps.
  `gpu::pool::take` (`gpu.cpp:24-41`) logs "out of CPU descriptors" and then **reuses the last slot**: a full pool
  corrupts views, it doesn't fail.
- `gpu.cpp:115-128`: white null views for empty stages, so every table slot is valid (tier 1 rule; it also matches
  Direct3D 8, where an empty stage reads 1 1 1 1).
- `gpu.cpp:139-163`: root signature 1.0: two root CBVs (vertex and pixel constants), an SRV table and a sampler
  table of 8 each. 6 DWORDs.
- `gpu.cpp:233-235`: `open_list` sets both heaps and the root signature first, after every reset.
- `gpu.cpp:411-441` `srv_table`: copies the draw's views from the CPU pool into the frame's share (`CopyDescriptorsSimple`,
  `gpu.cpp:431`); the same views as the last table reuse it (`gpu.cpp:413`, added in a602b96). When the share is full it logs
  "the shader-visible descriptors of a frame ran out: waited for the GPU" (`gpu.cpp:419`), submits, **waits**, and
  starts the share again. That is a frame hitch, not a crash.
- `gpu.cpp:444-460` `sampler_table`: tables keyed by a hash; when 256 combinations are used it waits for the GPU
  and starts the sampler heap over.
- `src/d3d8/resource.cpp:339`: `image::retarget` writes the float world's new SRV into the same CPU slot, then
  `forget_srv_table()` so the next draw copies it again.
- `src/d3d8/device.cpp:2224-2227`: after a mark's handler (HiFi XI's passes) runs, the d3d8 heaps and root signature
  are bound again and `bound.epoch = 0` makes the next draw bind every table.

**Plugin passes with their own heaps** (each switches away from the d3d8 heaps and must hand them back):

- Characters: `src/chars/cache.cpp:355-363` one shader-visible heap of `kDescriptors` 16384 (`cache.hpp:41`), the
  last `kReserved` 4 for the renderer's own targets (`cache.hpp:103`, `cache.cpp:1234`). A forgotten pair's slots go
  back to `free_slots` only behind the game queue's fence (`retire::then`, `cache.cpp:227-236`), never into a heap made
  since (`heap_generation`). Bound in `chardraw.cpp:1720`,
  `1933`, `1957`, `1985`, `2285`. Root signatures 1.0 with 2 static samplers (`chardraw.cpp:1571`) and 1 for the aura
  (`chardraw.cpp:2160`).
- The characters' texture pack pairs (docs/texture-pack/characters.md): a texture on the GPU may hold a second pair
  from the same `free_slots`, the pack's view of it. Taken only for a ticket `take_done` handed over ready
  (`pack_settle`, `cache.cpp:1491`, :1500), and only while more than `kPackSlotFloor` 1,024 pairs are free (:1489; asks
  are held then too, :1363, and never more are asked than the pairs over it less those on their way, :1370-1383): the meshes and the game's own textures always find one. Written once, on the game's
  thread, with the cache's mutex left (`src/chars/charpack.cpp:178-190`): descriptor `2s` is `stream::write_view` of the
  ticket (BC2, its levels), `2s + 1` a **new view** of the game texture's detail map (`write_pack_detail`,
  `cache.cpp:1592-1627`, its `MipLevels` from the resource's desc) or the null view the game's pair has. Never a
  `CopyDescriptors` from the game's pair: the cache's heap is shader visible, so it can't be a copy's source (the rule
  above). Since M9 (characters.md "Pack relief") a texture asked with a height has two tickets, and its pair is taken
  only once both are settled (`pack_settle`, `cache.cpp:1491`; `pack_begin` is its colour-only case): `2s + 1` is then
  `stream::write_view` of the height ticket (BC4, its whole chain; `src/chars/charpack.cpp:187`) in the detail map's
  place, and when the pack has no colour for it (a height alone) `2s` is a new view of the game's own texture
  (`write_pack_detail` again, its `MipLevels` from the resource's desc; the texture AddRef'd under the mutex,
  `cache.cpp:1567`), never a copy either. Still one pair, written once; the draw says which it holds by flags 4 (below).
  Only then does the state say ready (`pack_end`, :1586), and only a ready pair is handed out
  (`texture_view_of`, :1166; the shadow casters ask with the pack refused, `chardraw.cpp:1999`; counted, `char_stats::pack_draws`: charinject_test checks the casters draw none). Let go like a game
  pair (`release_pack`, `cache.cpp:286`, both tickets with it): behind the fence, never into a heap made since. charcache_test lets 1,000
  textures go while upgrading, between `pack_begin` and `pack_end` too, and finds no pair twice and none free before
  the fence. Table 5's layout and root signature 1 are unchanged by M9: a pair whose `2s + 1` is the height is drawn
  with draw-constant flags 4 and the relief scale in the composite's word (dc[5], unused and 0 in a model's draw until
  then; `chardraw.cpp:1879`), so the shader never reads a height as a detail map; charinject_test's heights part
  (a height alone, a colour and a height, a colour alone) is silent under the debug layer and GPU-based validation
  (WARP and the 4070 Ti).
- The native sun shadows (`src/render/sun_shadows.hpp`): no table, no heap. The zone's root
  signature has two more root parameters, 8 a CBV at b2 (the shadow block, in the zone's upload ring after the lamps,
  256-aligned: `draw.cpp` `kShadowAt`) and 9 an SRV at t6 (World FX's shadow map copy, a raw buffer), pixel shader only:
  10 parameters, 20 DWORDs (`draw.cpp:777`). The characters' the same at 7 (b2, after the ring slot's two halves:
  `chardraw.cpp` `kShadowAt`) and 8 (t7): 9 parameters, 23 DWORDs (`chardraw.cpp:1587`). Both are always set before a
  model or zone draw (`zone_renderer::record`, `char_renderer::begin`); with no block to apply the CBV says so (`ss_sun.w`
  0 and `ss_mode.z` 0, no lookup runs) and the SRV gets the ring's own address, valid and never read: a root SRV has no
  bounds and no null. The casters' and composite draws bind neither: their shaders don't read b2/t6/t7.
- The native lamps (sun_shadows.hpp's lamps) add **no root parameter**: the same block carries the
  lamp list, the cubes' place and the characters' capsules (`sun::gpu_block` 1168 bytes, the CBV's slot `kBlockBytes`
  1280: both rings' slots grow with it), and the same SRV carries the lamps' cubes (in World FX's copy after the
  clouds' map, or in the cubes' own buffer when there's no sun map). A block with lamps and no cube has no buffer at
  all: the SRV then gets the ring's address as with no block, and the shader reads no cube (`ss_lamp_col[i].w` below 2).
  Every index into it is clamped in the shader (`sl_from_map`: the cube 0..3, the face 0..5, the texel to the face).
- Zone: `src/zone/zone_gpu.cpp:258-299` one heap per loaded zone, sized to its textures. `src/zone/draw.cpp:1016-1027`
  switches heap only when the next object's zone differs (the second map), and binds that zone's vertex and index
  buffers as root SRVs with it; the breadcrumb
  step `kStepHeap` marks it. `zoneshadow.cpp:494` binds it for the shadow casters.
- The zone heap's texture pack region (docs/texture-pack/zones.md): a zone read in the pack's mode from the game's own
  file gets `textures.size()` more slots after white and flat (`zone_gpu.cpp:261-266`), each written at upload with the
  game texture's own view (:281-297), so a slot bound by mistake still reads a valid texture. A pack slot is written
  once at most in that gpu_zone's life, by the glue (`src/zone/zonepack.cpp:183`: `stream::write_view` of a ticket
  `take_done` handed over ready, on the game's thread, no lock of ours held), before its word is stored drawn with
  release (`set_pack_word`, `zone_gpu.cpp:402`); `texture_srv` (:367) names it only once it reads drawn (acquire) and
  compare is off. Never written again: off, reload, a ticket gone all turn the word spent, and a new visit is a new
  gpu_zone with a new heap. So the rule above holds: until drawn no list references the slot, after it nothing writes
  it. `game_srv` (:381) is the game's slot always: the effects' MMB meshes (`src/fx/fxrender.cpp:782`, :822) and the
  shadow cut-outs (`zoneshadow.cpp:508`, :523) use it. zonenative_test's slot-write log checks the order per gpu_zone.
  `tests/zone_view.cpp:232` (`ZONE_VIEW_PACK`) writes the slots the same way, each before its word, all before its one
  list is recorded.
- Its heights' block (M9, docs/texture-pack/zones.md "Pack normals"): the pack region is `2 x textures.size()` slots
  (`zone_gpu.cpp:268`), the colours' and then the heights', each height slot written at upload with the flat stand-in's
  view (:300-313: red 0x80, a level height). The same write-once rule with words of its own: the glue's `write_view`
  of a ready height ticket (`zonepack.cpp:203`) and the relief scale (`height_written`, `zone_gpu.cpp:467`) before the
  height word is stored drawn with release (`set_height_word`, :449); `height_srv` (:476) names the slot only once it
  reads drawn (acquire), heights are on and compare is off, else a null handle. The zone pass then binds it at table 7
  (t5) in place of the normal map (`draw.cpp:995`) and puts the relief scale in the piece's fourth root constant (b1,
  `draw.cpp:991`: it was unused and 0, so no root signature changed); per pixel only, never with the bump shader's path.
  zonenative_test's slot-write log checks the heights' words and writes as the colours'.
- Effects: `src/fx/fxtextures.cpp:285-287` one heap of `kReserved` 8 + `kMax` 4096. `src/fx/fxrender.cpp:777-783`
  switches between its heap and a zone's heap for MMB meshes and **sets table 6 again right after**;
  `fxrender.cpp:846-847` ends on its own heap. Root signature with 3 static samplers (`fxrender.cpp:402`).
- World (sky, names, grid): `src/world/world_render.cpp:271` a 16-slot heap for drawing over the screen only;
  world draws use root constants and root SRVs, no tables (`docs/own-frame/world.md`). `world_render.cpp:571` binds it.
- World FX grass/shading: `src/render/grass_render.cpp:2131-2161` root signature of 52 root constants (`kConstants`,
  line 48) + 6 root SRV/UAVs x 2 = **64 DWORDs, full**. No tables at all, so no heap. Its other heaps (lamp map DSVs
  `4818`, sun map DSVs `4113`, RTV/DSV) are CPU only or used by its own passes.
- 2D: `src/ui2d/render2d.cpp:667-671` its own SRV and sampler heaps, bound at `render2d.cpp:1066`.
- Upscale: `upscale.cpp:294`, `temporal.cpp:309/348` own heaps; FSR 2 binds its own, so `upscale.cpp:419` and `525`
  put the list's heaps back (`upscale::on_heaps`, fed by `ev_heaps`, `backend.cpp:778-782`).
- The texture pack's streamer (`src/pack/stream.cpp`) has **no shader-visible heap** and writes into nobody's.
  An owner makes the view of a ready ticket itself with `stream::write_view(ticket, cpu_handle)` (`stream.cpp:1880`):
  `CreateShaderResourceView` with the ticket's format, the levels uploaded, most detailed 0, on the owner's thread,
  with none of the owner's locks held (a device call), into a slot the owner alone decides is free (the rule above:
  no list in flight may read it). It holds the texture and the device for the call (AddRef'd with the ticket checked,
  under the streamer's mutex), so another thread's epoch change can't free them under it; a view made just before an
  epoch change keeps its texture till the owner drops the ticket (zones.md decision 1). One texture shared by several
  owners gets one view in each owner's heap. packstream_test samples through such views in its own heap; the zones'
  glue writes them into the zone heap's pack region (above).
- Views made or copied by anyone are followed by the backend (`backend.cpp:808` `ev_view`, `814` `ev_copy`), so
  `port_resource_of_view` (`backend.cpp:223`) stays right after a slot is refilled.

## Mistakes this project made

- **World FX at the 64-DWORD limit.** No root slot is left. Each new input had to ride in an existing one: the sun
  shadow map is read through t0 with param 1 made ALL-visible; the snow trails live at the end of `_sun_mask` (u1,
  visibility changed to ALL for the vertex shader); the ground colour map is copied into the start of the blades'
  memory u0 (3db2a26, `kMemoryBase`). Adding a parameter means moving constants into a buffer or a table first.
  The blades' own light (sun_shadows.hpp "The grass", 2026-11-10) rode in the same way: the native passes' block
  (`sun::gpu_block`, 73 float4) goes after the actors in t2 (`kNativeOffset`, grass.hlsl `kNative`), read by the
  blades' vertex shader into globals, and the block's buffer (the cascades, the clouds' map, the lamps' cubes) is bound
  in t1 for the blades' draws, the world's copy's slot, which they don't read; a block with no buffer leaves t1 as it
  was. Read only behind a `[branch]` on the pass's flag (`shade.x`, the blades' own copy of the constants): without the
  block t2 may be the small blades' buffer. No root parameter added; the light split's word goes out as a render
  target (`light_pipeline_for`: the world, the motion when FSR asks, the split last), not through the root signature.
- **A stale descriptor someone else refilled.** World FX drew through dgVoodoo's RTV slot, which dgVoodoo filled again
  by a copied descriptor. A full-screen pass wrote into a smaller target, past its end. Fixed by making its own view at
  each draw (`grass_render.cpp:5314-5326`). Never keep another layer's descriptor handle across calls.
- **Heaps put back after the root tables.** The first native zone pass restored the game's heaps after its tables, so
  the tables pointed into the wrong heap. zoneinject_test (WARP + debug layer) caught it. Heaps first, then tables.
- **Too few reserved slots.** The aura ring needed its own target view in the character heap: `kReserved` 2 -> 4
  (89949f3). Reserved slots sit at the heap's end; mesh/texture pairs fill from below.
- **Same slot, new view, old copy.** The float world remakes the world target in the same descriptors; the d3d8
  table cache would have kept the old copy. `forget_srv_table` was added for it (`resource.cpp:339`).
- **Not a heap bug (yet).** A user's 2026-11-01 GPU hangs (gone with per-pixel zone lighting off) stopped in the
  zone pass setup, which includes the zone's `SetDescriptorHeaps` (memory: hifixi-crash-audit-1101). Descriptor
  indices clamp and root SRVs are ALL-visible; the cause is still open. (The 11-02 Windurst hangs were World FX
  drawing the old zone, fixed by the zone guard in v0.65: not descriptors either.) Don't blame or "fix" heap switching without a breadcrumb that names it.

## Sources

- https://learn.microsoft.com/windows/win32/direct3d12/descriptor-heaps
- https://learn.microsoft.com/windows/win32/direct3d12/shader-visible-descriptor-heaps
- https://learn.microsoft.com/windows/win32/direct3d12/non-shader-visible-descriptor-heaps
- https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setdescriptorheaps
- https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device-copydescriptors
- https://learn.microsoft.com/windows/win32/direct3d12/hardware-support
- https://learn.microsoft.com/windows/win32/direct3d12/root-signatures
- https://learn.microsoft.com/windows/win32/direct3d12/root-signature-limits
- https://learn.microsoft.com/windows/win32/direct3d12/using-descriptors-directly-in-the-root-signature
- https://learn.microsoft.com/windows/win32/direct3d12/root-signature-version-1-1
- https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html
