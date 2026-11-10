# HiFi XI 0.68

Since 0.67. Only `plugins\hifixi.dll` changed: the `d3d8.dll` and `dxgi.dll` in the zip are 0.67's, so unzipping the whole zip over 0.67 is safe.

## New

- **Sun shadows lit with the scenery.** The sun's and clouds' shadows are now worked out as HiFi XI lights the scenery and characters, each pixel at full size, instead of being darkened onto the finished picture afterwards. A shadow takes away only the sun's light, so lamps and the sky's light stay in it; edges are sharp where things touch and softer the higher what casts them; characters are shaded by trees and buildings the same way the ground is. On by default with per-pixel lighting (**The game's own** tab, **Sun shadows lit with the scenery**); off, shadows are drawn as in 0.67.
- **Texture packs with BC7 colour.** Packs made by [hifixi-enhanced-textures](https://github.com/relliko/hifixi-enhanced-textures) 0.2 (the HDCube upscaling model, BC7) are drawn as they are: cleaner colour and edges at the same size on the graphics card.
- **Relief from the texture pack.** A pack made with heights (`hifixi-texpack heights`) gives stones, bricks and bark their own bumps in the light: the game's own bump maps where it has them, else worked out from the texture. Only the light changes, never the colours. Ships, furniture and plants get relief for the first time. Packs without heights look as before.
- **Depth from the texture pack (experimental, off by default).** With that relief, cobbles, bricks and flagstones stand out of the surface: nearer stones hide the joints behind them and cast small shadows of their own. Scenery only.
- **The menu shows a pack being made.** While `hifixi-texpack make` works, the texture pack section says how far it is and about how long is left.

## Fixed

- **Frame generation no longer drags a fast game down.** Automatic mode started on and came back on after every fade or load without looking at the frame rate, holding a game that ran 190 fps alone to about 105. It now starts off, turns on only when the game alone runs well under half the screen's refresh rate (under 90 at 240 Hz), turns off when the doubled frames add too little, and checks again after a fade. The setting is now called **Only while the game is slow**.
- **Character detail maps show at once.** Turning **Character detail maps** on only reached characters loaded afterwards, so it seemed to do nothing until the next area. The characters on screen are now loaded again with them, in about a second.
- **World FX's shadows and light at window sizes that aren't a multiple of 128 pixels wide.** At such sizes World FX lost what the scenery's lighting told it about each pixel and fell back to guessing from depth.
- **The log no longer reads as if lamp shadows were off all session.** A zone-in's first second without characters was logged, and the line saying they were back was swallowed.

## Every change since 0.67

- 198e154 Texture pack M7: `hifixi-texpack make` and `get-upscaler`; the menu's progress.
- 315ab67, 06f022c Texture pack M8 and HDCube: BC7 colour and the HDCube upscaler by default in the pack maker.
- 63c2ca4 Texture pack M9: height records and per-pixel relief.
- c25b574 Characters: turning detail maps on remakes the textures already loaded.
- 173ccf9 Frame generation: automatic mode off unless the game is slow alone.
- ccd38e4 Texture pack: parallax occlusion on the scenery's relief.
- b1b89aa World FX: the lamp-shadow log lines.
- 97f7793 Native sun shadows: the zone and characters light each pixel with its sun and cloud shadow.
- 090c7fc, 641276d The README's link to hifixi-enhanced-textures, merged.
