# HiFi XI

One Ashita v4 plugin for a better-looking, faster Final Fantasy XI:

- **Above 60 fps** with game speed tied to real time, and a frame cap. The game's visibility tests (glows, lamps, the lens flare, doors, characters) are answered from what's already drawn, so it never stops to wait on the GPU for them.
- **Longer view distance:**
  - the world, characters and the zone's finer meshes drawn further;
  - the zone's visibility lists ignored, so far terrain doesn't pop in;
  - culling at the camera's own height of view;
  - square pixels.
- **Smooth edges and upscaling with AMD FidelityFX Super Resolution:**
  - **FSR 3.1** builds each frame from the last ones, the world drawn a fraction of a pixel apart every frame: edges as smooth as a background resolution twice the screen's, for far less work. Swaying grass and moving characters carry their own motion, and spells, particles, water and weather are marked so FSR doesn't smear them;
  - with the background resolution at the screen's size it smooths edges; below it, it also upscales, for more frames;
  - **FSR 2** in its place if you prefer it, or where FSR 3.1 can't run; **FSR 1** (edge-aware upscaling and sharpening) when both are off;
  - **Frame generation (AMD FSR 3):** a frame made between each two the game draws, for motion as smooth as twice the frames, the menus and text kept sharp. By default only while the game on its own is slow for your screen;
  - the menus and text are always drawn at the screen's own size.
- **Its own Direct3D 8 translator** (`d3d8.dll`): the game's drawing goes straight to Direct3D 12 through HiFi XI's own code, in dgVoodoo's place. HiFi XI knows every pass of the frame from it, with its shaders compiled ahead and kept between sessions.
- **The zone, the characters and the zone's models drawn by HiFi XI itself** (ships and airships, Mog House furniture, garden plants), from your own game files, in the game's own look:
  - **Per-pixel light** with every texture's normal map, on the zone and on characters and monsters;
  - **Tone mapping, bloom and a colour grade** over the world;
  - **Spell and ability effects, weapon trails, battle numbers, lens flares and the glow spells cast on the ground**, the **sky dome**, **names**, the **Mog House layout grid**, **cloth** (banners, Gigas, Treants) and the **glows and shimmers over characters** (two-hours, songs, warping out);
  - **The interface** too, at the screen's own size after upscaling (off by default while it's new: `/hifi ui on`);
  - Always on: the game draws only what HiFi XI can't yet.
- **A richer world**, drawn into the game's own frame:
  - **Grass** coloured by the ground it grows from, that bends around characters, wears into paths and is cut by weapon skills;
  - **Light:** sun and moon shadows from the whole zone's own meshes, with characters and monsters casting their real shapes, swaying trees and opening doors casting theirs as they move, and shadows sharp where they touch and softer the further from what casts them, out to mountains at low sun. Shadows fall by the light the zone and characters were actually lit with, and on their real facing. Light from the sky above and bounced up from the ground, and soft highlights on stone and shiny gear;
  - **Light in the air:** sun shafts where sunlight comes past trees, buildings and cliffs, and fog lying low, patchy and drifting, thicker at dawn, dusk and in mist, rain and snow;
  - **Water** with reflections and ripples, carried on over the game's own sea past the zone's edge, out to the horizon;
  - **Snow that settles** while it snows, deep enough near you to hide feet and the bottoms of the grass, with footprints left by whoever walks through it, filling back in as it keeps snowing;
  - **Weather of its own:** rain and snow, wet ground, clouds, thunderstorms with lightning, dust and sand, wind that bends the grass, heat shimmer, auroras and gloom. While it's drawn, the game's own weather visuals are left out (its sounds, light and fog stay), and with them the birds, butterflies and ducks the game's weather brings;
  - **Sky:** a smooth sky behind the game's own sun, moon and constellations, kept as they are;
  - **Lamps and glows:** night lamps and glowing things lighting what's around them, the lamps fading in and out with the game's own at dusk and dawn, and the nearest lamps casting shadows of the zone around them.
- **En-spell effects:** while Enfire, Enblizzard, Enaero, Enstone, Enthunder, Enwater, Enlight or Endark is on you (the IIs too), your weapons carry its element: flames, frost and ice shards, wind, circling stones, arcs of lightning, flowing water, rays of light or dark smoke. On every melee weapon of every race: along a blade, on an axe's, scythe's, staff's or spear's head, all over hand-to-hand weapons.

Everything is in one menu (`/hifi`): presets in a dropdown (Off, Balanced, High, Ultra, and your own, saved by name), a search box, a tooltip on every setting and a right-click reset.

## Requirements

- Ashita v4.
- A graphics card with Direct3D 12 (feature level 11 or newer).
- HiFi XI's own `d3d8.dll` in the `bootloader` folder, not dgVoodoo's. If your launcher puts a Direct3D 8 wrapper there itself, switch that off: on Phoenix's launcher, **Settings -> Graphics -> My own wrapper**.
- No ReShade: HiFi XI draws into Direct3D 12 itself. If ReShade's `dxgi.dll` is in your `bootloader` folder, move it out.

Check your server's rules on client modifications before using it.

## Install

1. Unzip into your Ashita folder. You get:
   - `plugins\hifixi.dll`;
   - `bootloader\dxgi.dll`: HiFi XI's starter, which starts it before the game makes its Direct3D 12 device;
   - `bootloader\d3d8.dll`: HiFi XI's Direct3D 8 translator, and `bootloader\d3d8-shader-cache\`, its shaders compiled ahead;
   - `bootloader\worldfx-sunlit.txt` and `worldfx-clearsky.txt`.
   If a `d3d8.dll` (dgVoodoo's) was already in `bootloader`, keep it somewhere else in case you want it back.
2. Add `/load hifixi` to your boot script (`scripts\default.txt`).
3. If you used **hifps**, **drawcull** or the **worldfx** addon, leave them out: HiFi XI does their work. If they're loaded in a part of the script you can't edit, add `/addon unload <name>` for each before `/load hifixi`.

Your settings from hifps and drawcull are taken over the first time HiFi XI starts.

## Upscaling (FSR)

FSR works with the game's **background resolution** (the size the world is drawn at, set in your launcher or the game's config) at or below your screen's:

- the same as your screen (say 2560x1440 on a 2560x1440 screen): FSR smooths every edge, about as well as a background twice the screen's size;
- lower (1920x1080, or 1707x960 for the most frames): FSR also upscales to the screen;
- above the screen's size, HiFi XI leaves the game's own scaling alone.

In the menu's **Upscaling** tab: FSR on or off, FSR 3.1 or FSR 2 (or FSR 1), how much it sharpens, and whether effects, water and weather are kept crisp. What it's doing is shown under them.

### Frame generation

With **Frame generation** on, AMD FSR 3 makes a frame between each two the game draws while FSR 3.1 or FSR 2 draws the world. Your screen shows twice the frames; the game itself then runs at up to half your screen's refresh rate (122 at 244 Hz) and answers a little later. Making those frames also takes graphics-card time, so it pays only where the game on its own runs well below that: with **Only while the game is slow** on (the default), it stays off until the game alone runs under about three quarters of half the refresh rate (under 90 at 240 Hz), switches itself off again whenever the game can run faster alone or the doubled frames add too little, and doesn't come back on after a fade or a load without checking.

Turning it on takes effect the next time the game starts (it replaces the game's swap chain as the game starts); turning it off is immediate. Frame counters in the game count the game's own frames; the menu's **Performance** tab shows both. With a G-Sync or FreeSync screen, a frame cap a little under half your refresh rate (116 at 244 Hz) keeps the pacing even.

## Zone data

Grass, water, the sea past the zone's edge, rain, wet ground and the lamps come from each zone's own files in your game. HiFi XI makes them on your PC, in the background, the first time you enter a zone; nothing from the game's data ships with HiFi XI. To make every zone's at once: **Grass, water, weather -> Make every zone's files now**.

They're kept in the `bootloader` folder (`worldfx-grass\` and the others).

## Texture pack (optional)

A texture pack is a folder of sharper, higher-resolution copies of the game's textures, made from your own game's files. HiFi XI doesn't come with one and never downloads one: nothing of the game's data ships with it. If you don't have a pack, there is nothing to do and nothing changes: HiFi XI looks exactly as it does without this section, and the menu's **Texture pack** section only says how to get one.

**Getting one.** If you already have a pack, put it in `bootloader\hifixi-pack`. Or make one on your own PC from your own game's files with [hifixi-enhanced-textures](https://github.com/relliko/hifixi-enhanced-textures): its tool, `hifixi-texpack`, writes the pack straight into that folder (about 45 minutes on a fast graphics card, longer on a slower one; a quicker pack with the scenery only is an option). While it works, the menu shows how far it has got and about how long is left (it pauses while the game is running).

**With a pack.** Put the `hifixi-pack` folder in your `bootloader` folder (`bootloader\hifixi-pack\pack.idx` and the `pack-000.bin`... files beside it). The menu's **The game's own** tab then shows **Use the texture pack** (off by default) and what it found. Turned on:

- scenery, ships, furniture and plants get the pack's textures a moment after you enter an area; characters, gear, NPCs and monsters at once;
- the pack's textures are kept on the graphics card, not in the game's own memory, so the game stays as light as without it;
- anything the pack doesn't have is drawn as the game's own, so nothing goes missing;
- `/hifi textures compare` shows the game's own textures again until you use it a second time, to compare;
- a pack made with heights (`hifixi-texpack heights`) also gives **Relief from the texture pack**: stones, bricks and bark catch the light by their own bumps. **Depth from the texture pack (experimental)**, off by default, makes cobbles and bricks stand out of the surface.

A pack copied from another PC works as long as it's whole and made for this version of HiFi XI. If it isn't (a file missing or cut short, or damaged), the menu says what's wrong and HiFi XI simply draws the game's own textures: copy the folder again, then `/hifi textures reload`. A pack made for an older or newer HiFi XI is refused the same way, and the menu says which: update HiFi XI, or get a pack made for this version. A pack made from another version of the game's files still works; only the textures the game changed since are drawn as the game's own, and the menu says so.

If your graphics card stopped (crashed) last time, HiFi XI starts the next session with the pack paused, along with its other heavier effects, and the menu says so. `/hifi textures on` uses it anyway for that session.

If you have the older `hifi-textures` XIPivot overlay installed (the big game files the pack replaces), remove it from XIPivot: while it's listed the game loads those big files into its own memory, and characters keep them instead of the pack's. The menu says so while it's listed.

**Turning it off:** untick **Use the texture pack**, or `/hifi textures off`. The **Off** preset turns it off too. The game's own textures are back at once.

**Removing it:** turn it off, then delete the `bootloader\hifixi-pack` folder. Once it's off HiFi XI has none of its files open, so you can delete it while the game runs. HiFi XI never writes into that folder.

## Commands

- `/hifi`: the menu.
- `/hifi status`: what HiFi XI is doing, part by part.
- `/hifi glows`: the glows lighting the scene now, and the glowing effects met lately.
- `/hifi gpu`: the GPU time each part of the richer world takes, the costliest first (also in the menu's **Performance** tab).
- `/hifi nativezone`, `/hifi nativechars`: what HiFi XI's own drawing of the zone, or the characters, is doing; `perpixel on|off` after either for its per-pixel light.
- `/hifi upscale [on|off] [sharpen <0..1>] [framegen on|off]`: FSR on or off, its sharpening, frame generation (on from the next start, off at once), and whether they're drawing now.
- `/hifi enspell [auto|off|fire|ice|wind|earth|thunder|water|light|dark]`: the en-spell effect on your weapons. `auto` (the default) follows the en-spell on you; an element shows that one now, to see it. The same buttons are in the menu's Game tab.
- `/hifi textures [on|off|compare|reload|status]`: the texture pack on or off (saved), the game's own textures drawn instead to compare (not saved), the pack looked for and opened again, or what it's doing.
- `/hifi gamedraw zone|chars on|off`: the game's own drawing of the zone or the characters switched off, to compare (back on at every start).
- `/hifi off`, `/hifi on`: HiFi XI's passes off (the game draws as without it) and back on.
- `/hifi reset`: its counts started again.

Settings are saved as you change them, in `config\hifixi\settings.ini`.

## Diagnostics

With **Advanced -> GPU crash reports** on (the default), a GPU hang or page fault writes what the GPU was doing to `bootloader\worldfx-gpu-crash.txt`, with your GPU, its driver and the video memory in use at that moment. HiFi XI's log is `bootloader\hifixi.log`; it names your GPU, its driver and its video memory at start. The Direct3D 8 translator's is `bootloader\d3d8.log`: it notes any frame that took more than a quarter of a second, and where the time went.

In `bootloader\d3d8.ini` (optional): `[d3d8]` `AsyncReadback=1` answers the game's own visibility tests from a frame or so before instead of waiting for the GPU, which keeps a hitch from stalling the game.

If the GPU hangs (`DXGI_ERROR_DEVICE_HUNG`):

- Set the game's profile in your graphics driver back to its defaults (NVIDIA Control Panel or AMD Software), with anisotropic filtering, antialiasing and texture filtering left to the application.
- Run your GPU at its stock clocks and voltage.
- Send the crash report and the log when you ask for help.

## Uninstall

1. Take `/load hifixi` out of your boot script.
2. Delete `plugins\hifixi.dll`, and in `bootloader`: `dxgi.dll` (HiFi XI's starter), `d3d8.dll` and `d3d8-shader-cache\` (put dgVoodoo's `d3d8.dll` back if you kept it, or switch your launcher back to its own wrapper).
3. What HiFi XI made as you played, all in `bootloader`, safe to delete: the `worldfx-*` files and folders (grass, water, rain, lamps, shadows, its shader cache), `hifixi-shader-cache\`, `hifixi.log`, `hifixi.previous.log`, `d3d8.log`, `d3d8.ini` if you made one, and `worldfx-gpu-crash.txt` and `hifixi-hang.txt` if there are any.
4. A texture pack, if you have one: the `bootloader\hifixi-pack` folder.
5. Your settings: `config\hifixi\`, and `logs\hifixi-status.txt`.

## Licences

HiFi XI includes AMD FidelityFX Super Resolution 1 and 2 (Copyright (c) 2021-2023 Advanced Micro Devices, Inc., MIT licence) and, from the AMD FidelityFX SDK v1.1.4, FSR 3.1's upscaler and frame generation (Copyright (C) 2024 Advanced Micro Devices, Inc., MIT licence). Their licence texts are in `docs\hifixi\`.
