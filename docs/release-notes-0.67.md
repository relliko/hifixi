# HiFi XI 0.67

Since 0.65. Install the whole zip: this `hifixi.dll` works only with the `d3d8.dll` that comes with it (an older `d3d8.dll` is refused and HiFi XI's own drawing stays off until it's replaced).

## New

- **Optional texture pack.** HiFi XI can draw scenery and characters from a texture pack kept on the graphics card: sharper ground, walls, ships, furniture, gear, NPCs and monsters, while the game's own memory stays as small as without it. The menu shows it only when a pack is on the PC (`bootloader\hifixi-pack`); without one nothing changes. HiFi XI doesn't come with a pack and doesn't make one: put one you already have in `bootloader\hifixi-pack`. See the README's **Texture pack** section for turning it on, off and removing it.
  - Characters switch to the pack's textures at once; scenery, ships and furniture a moment after you enter an area, so zoning is exactly as before.
  - Anything the pack doesn't have is drawn as the game's own; `/hifi textures compare` shows the game's own again to compare.
  - A pack copied from another PC: if it's damaged, copied only in part or made for another HiFi XI, the menu says what's wrong and the game's own textures are drawn. One made from another version of the game's files still works for every texture the game hasn't changed.
  - When the graphics card is nearly full, the pack holds back first: new textures come without their sharpest level, then characters' pack textures go back to the game's own until there's room again.
  - After a graphics-card stop, the next session starts with the pack paused (`/hifi textures on` uses it anyway).
  - With FSR upscaling, the pack's textures are sampled for the screen's size rather than the background's, so they stay sharp.
- **Sun and moon fade at the horizon.** At 6:00 and 18:00 the game swapped the moon for the sun in about half a second, and whole zones (Giddeus most of all) changed colour at once. The light now fades out and back in over about 20 Vana'diel minutes.
- **Zone textures from XIPivot overlays.** HiFi XI's own zone drawing now honours texture overlays installed through XIPivot, with sharper filtering of the ground at a slant.

## Fixed

- **Settled snow and wet ground no longer vanish and come back whole after a stutter.** A one-second hitch while it snowed made the whole snow layer reappear at once; it now stays as it was through gaps of up to 10 seconds.
- **No grass from the last zone in the new one.** World FX is told the zone directly and draws nothing until it has read the new zone's files.
- **GPU objects let go only once the GPU is done with them**, behind a fence rather than after a guessed number of frames: zone data, model actors, character meshes and textures, effect uploads.
- **World FX's built-in shaders are compiled in the background** at the first start after an update: the world draws without its extras for those seconds instead of the game freezing for them. HiFi XI's other shaders (zone, characters, effects, upscaling) are still compiled once after each update, a pause of about two seconds at that first start.
- **World FX no longer holds its lock while it records its drawing**, which removes the shape of the freezes seen with Direct3D's debug layer.

## Changed

- **HiFi XI hooks nothing of Direct3D 12 any more.** Everything it draws goes in at the marks its own `d3d8.dll` places in the frame: the device and queue, the world's targets and their states, the fog colour and the place for FSR all come from there. Fewer moving parts, and no more following whichever queue submitted last. Started late (without the starter `dxgi.dll`), HiFi XI now still draws everything, only without GPU crash reports or frame generation.
- **The frame rate's speed-ups are out of the menu.** With the zone and characters drawn by HiFi XI the game sends about 115 draws a frame, so batch, dedupe and the Ashita and occlusion speed-ups had nothing left to save; `/hifi status` still reports them, and anyone who turned one on in 0.65 has it turned off once.
- **Memory and video memory are logged per zone**, and HiFi XI's log says what the texture pack is doing at start, per zone, and when something about a pack is wrong.
- **Character textures are decoded in smaller batches**, an oversized texture alone, and detail maps made only while **Character detail maps** is on, so a crowd doesn't spike the game's memory. A model whose texture can't be read is left to the game rather than drawn without it.
- **Slow presents** (over 200 ms on the game's thread) are logged step by step, to find the cause of rare stutters.
- **Crash and hang reports** also say what the texture streamer was doing.

## Every change since 0.65

For the record, the 27 commits since 0.65, then this release's own work:

- f48a070 The frame rate's speed-ups out of the menu.
- 8cc6855 Sun and moon faded about the horizon.
- a4fe822 Settled snow and wet ground kept through a hitch; slow presents logged step by step.
- 307ff12 Every offline test suite from one runner; the old end-to-end test retired.
- 6d321d5 The zone told to World FX directly; GPU objects let go behind a fence.
- 84222e3 Direct3D 12 reference pages for the developers (docs/d3d12).
- f7bc9ca The d3d8.dll hands over its device, queue and the world's states at every mark (channel 2).
- 384eaab rtprobe: a research probe of ray tracing from a 64-bit helper (not in the plugin).
- 1cea43a World FX's core pinned by a test before its rewrite.
- 735b493 World FX follows no Direct3D 12 stream: the world, its states and the fog come with the marks (channel 3).
- e4c2f9c World FX records without its lock.
- a7c032a World FX's built-in shaders compiled off the game's thread.
- 81ea82a FSR placed by the d3d8.dll's resolve mark (channel 4).
- 9b04ee8 No Direct3D 12 hooks: everything at the d3d8.dll's marks.
- 120cb36 Zone textures from XIPivot's overlays, drawn with their own mips.
- 3d3df70 zone_view: buffers for offline mock-ups (developers' tool only).
- 8273115 The texture pack's plan.
- 6de1bcb Texture pack M0: the key is the original texture's own bytes.
- d8cb795 Texture pack M1: the game's memory measured, the character cache hardened.
- cc6ff2f Texture pack M2: the pack format, its reader and writer, the pack maker.
- d0af060, 8818a7d The plan: the first real pack convert recorded.
- 7e5879d Texture pack M3: the streamer.
- 32e90fe Texture pack M4: zones and model actors from the pack (0.66).
- 77e1b65 Texture pack M5: characters from the pack (0.67).
- 6b9e8ab, d5e592a Merges of the work above.
- (M6, this release) The pack's log lines, packs copied from another PC made safe and said in the menu, the README's texture pack and uninstall sections, tests for an 8 GB card and the cloth and overlay passes from the pack.
