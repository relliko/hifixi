# HiFi XI 0.69

Since 0.68. Only `plugins\hifixi.dll` changed: the `d3d8.dll` and `dxgi.dll` in the zip are 0.67's, so unzipping the whole zip over 0.68 is safe.

## New

- **Lamp light worked out with the scenery.** The glow of lamps, torches and street lights is now added as HiFi XI lights the scenery and characters, each pixel by the way it really faces (the texture pack's relief too), instead of being painted over the finished picture. A lamp's light is a wider, softer pool with a brighter core at the lamp, fading out to nothing with no ring at its edge. Mog Houses keep their gentler light.
- **Every lamp near you casts shadows.** All sixteen lamps HiFi XI lights at once have their own shadows now, not just the four strongest: a lamp behind a wall no longer lights the street side, and light no longer leaks through a floor to the level below. A lamp coming into reach starts as a small pool and widens with its shadows over about a third of a second. Walls always hide a lamp fully; **Lamp shadows** now only sets how much characters shadow a lamp.
- **Characters in lamp light.** Characters are lit by lamps by their own shape, and shadow a lamp by where they stand rather than by a guess from the screen.
- **Grass lit and shadowed like its ground.** Each blade looks up the sun's and clouds' shadows and the lamps itself: grass in a tree's shadow goes dark with the ground it stands on instead of staying bright, and grass round a campfire or under a street lamp at night glows with it instead of standing black.
- **Shadows and lamps with per-corner lighting.** With **Light the scenery per pixel** or **Light the characters per pixel** off, the scenery and characters now get the same sun shadows and lamp light as with it on.

## Changed

- **One way of lighting.** Sun shadows, cloud shadows and lamp light are now always worked out with the scenery, characters and grass. The older way, darkening and lighting the finished picture afterwards, is gone, and with it **Sun shadows lit with the scenery** (always on now), the stand-in character shadows and the baked sun maps (`worldfx-shadow` in `bootloader` is no longer read and can be deleted). Old settings in `hifixi.ini` are ignored.
- **Characters cast sun shadows** now only switches the characters' own shadows, cast by their real shape.
- Effects, and the few models and water the game still draws itself, no longer take the sun's shadow from the maps; contact shadows and ambient occlusion still reach them.

## Fixed

- **Bright blades standing in shadow.** Some grass kept its sunlit colour inside a tree's or a building's shadow.
- **A missed frame of shadows after a hitch.** What the sun's shadows and the lamps are worked out from now stays good for a second, not a quarter of one, and is let go when you change zone.

## Every change since 0.68

- 17e7fb2 The offline zone view draws a zone's lamps at night.
- b31abaf Native lamps: the zone and characters add the lamps' glow per pixel.
- b6b33b8 Native grass: the blades take the sun's shadow and the lamps' glow themselves.
- cee40ac One lighting path: World FX's old sun darkening and lamp light deleted.
- 905e8e3 Grass lit by the game's lamps; a shadow cube for every lamp.
