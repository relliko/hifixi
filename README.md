# HiFi XI

One Ashita v4 plugin for a better-looking, faster Final Fantasy XI:

- **Above 60 fps** with game speed tied to real time, a frame cap, and speed-ups. Each speed-up removes a stall or repeated work from a frame:
  - hidden characters found from the zone's walls instead of waiting on the GPU;
  - Ashita's per-draw dispatch skipped;
  - SSE skinning on any CPU;
  - batched text;
  - repeated device calls dropped.
- **Longer view distance:**
  - the world, characters and the zone's finer meshes drawn further;
  - the zone's visibility lists ignored, so far terrain doesn't pop in;
  - culling at the camera's own height of view;
  - square pixels.
- **A richer world**, drawn into the game's own frame:
  - **Grass** that bends around characters, wears into paths and is cut by weapon skills;
  - **Light:** ambient occlusion, surface relief, and sun and moon shadows from the whole zone, with characters casting long shadows;
  - **Water** with reflections and ripples, carried on over the game's own sea past the zone's edge, out to the horizon;
  - **Weather of its own:** rain and snow, wet ground, clouds, thunderstorms with lightning, dust and sand, wind that bends the grass, heat shimmer, auroras and gloom. While it's drawn, the game's own weather visuals are left out (its sounds, light and fog stay), and with them the birds, butterflies and ducks the game's weather brings;
  - **Sky:** a smooth sky with a sun, stars and a moon in the game's own phase;
  - **Lamps and glows:** night lamps and glowing things lighting what's around them, the lamps fading in and out with the game's own at dusk and dawn.

Everything is in one menu (`/hifi`), with presets (Off, Balanced, High, Ultra) and a tooltip on every setting.

## Requirements

- Ashita v4.
- For the richer world (grass, shadows, water, weather, sky, lamps and glows): dgVoodoo's output on Direct3D 12. In dgVoodoo's config set `OutputAPI = d3d12_fl11_0`; the game's own Windows default is Direct3D 11, and the menu tells you when it isn't 12.
  - With any other output (dgVoodoo on Direct3D 11, DXVK...), the frame rate and the view distance still work, and the game keeps its own water, rain and shadows.
- No ReShade: HiFi XI draws into Direct3D 12 itself. If ReShade's `dxgi.dll` is in your `bootloader` folder, move it out.

Check your server's rules on client modifications before using it.

## Install

1. Unzip into your Ashita folder. You get:
   - `plugins\hifixi.dll`;
   - `bootloader\dxgi.dll`: HiFi XI's starter, which starts it before the game makes its Direct3D 12 device;
   - `bootloader\worldfx-sunlit.txt` and `worldfx-clearsky.txt`.
2. Add `/load hifixi` to your boot script (`scripts\default.txt`).
3. If you used **hifps**, **drawcull** or the **worldfx** addon, leave them out: HiFi XI does their work. If they're loaded in a part of the script you can't edit, add `/addon unload <name>` for each before `/load hifixi`.

Your settings from hifps and drawcull are taken over the first time HiFi XI starts.

## Zone data

Grass, water, the sea past the zone's edge, rain, wet ground, the sun's shadow map and the lamps come from each zone's own files in your game. HiFi XI makes them on your PC, in the background, the first time you enter a zone; nothing from the game's data ships with HiFi XI. To make every zone's at once: **Grass, water, weather -> Make every zone's files now**.

They're kept in the `bootloader` folder (`worldfx-grass\` and the others).

## Commands

- `/hifi`: the menu.
- `/hifi status`: what HiFi XI is doing, part by part.
- `/hifi glows`: the glows lighting the scene now, and the glowing effects met lately.
- `/hifi gpu`: the GPU time each part of the richer world takes, the costliest first (also in the menu's **Performance** tab).
- `/hifi off`, `/hifi on`: its Direct3D 12 hooks out and back in.
- `/hifi reset`: its counts started again.

Settings are saved as you change them, in `config\hifixi\settings.ini`.

## Diagnostics

With **Advanced -> GPU crash reports** on (the default), a GPU hang or page fault writes what the GPU was doing to `bootloader\worldfx-gpu-crash.txt`, with your GPU, its driver and the video memory in use at that moment. HiFi XI's log is `bootloader\hifixi.log`; it names your GPU, its driver and its video memory at start.

If the GPU hangs (`DXGI_ERROR_DEVICE_HUNG`):

- Set the game's profile in your graphics driver back to its defaults (NVIDIA Control Panel or AMD Software), with anisotropic filtering, antialiasing and texture filtering left to the application. For sharper textures at an angle, use dgVoodoo's own `Filtering` setting (an anisotropy level, 1 to 16) instead.
- Run your GPU at its stock clocks and voltage.
- Send the crash report and the log when you ask for help.
