# Grass colour from the ground itself (0.59, step 6 of the screen-space cull)

Alex, 2026-10-31: replace the grass's learned colour (World FX watching the screen under each blade's root: grass.hlsl
`ground_colour`, the memory u0, the rules GrassRuleInFront / GrassRuleOutliers, the fallback GrassHue) with the ground's
own colour from the native zone data.

## Plan

1. **The ground-colour map** (src/zone/zoneshadow, beside the sun cascades and the movers' map: it already places every
   object of the zone by instancing and samples each piece's texture for alpha testing). A pass from straight above
   around the camera (orthographic, world x/z to texels; e.g. 1024 x 1024 at a quarter yalm, 256 yalms across, drawn
   again when the camera has left its middle half or the zone's data changed). Each texel: the unlit ground,
   texture.rgb x vertex colour.rgb x 2 (the zone pass's MODULATE2X), and the surface's height. Only surfaces facing up
   (world normal y < -0.3: the game's y is down); where surfaces overlap, the lowest wins (a canopy or a roof over the
   ground loses). Blended pieces (water sheets, decals) left out, alpha-tested ones tested as the shadow pass tests them.
2. **The blades** read the map at their root (grass.hlsl, in ground_colour's place): when the map's height is within
   ~1 yalm of the root, its colour lit as the zone pass lights a fixed-function piece facing up: the zone env's ambient
   (hemi_ambient's sky above) + sun x max(0, N.L), from the zone's frame env (zone::frame); else a neutral fallback.
   World FX's own shading (sun shadows, lamps, fog) goes on the blades as before.
3. The learning, its memory's colour part, the two rules and GrassHue removed; the memory keeps the wear.

## The blades' own light (2026-11-10, native step 3)

The map holds the ground **lit whole and unshadowed** (the ground pass's own shader, `zoneshadow.cpp ps_ground`: the
vertex colour x (ambient + sun x N.L), then the texture x 2): never the sun's or clouds' shadows, never a lamp, with
ever. World FX's shading pass used to darken the finished blades afterwards (until the cleanup of 2026-11-11, which
took its sun darkening and lamp glow out altogether). The blades take their light themselves from World FX's block
(`src/render/sun_shadows.hpp` "The grass"), as the zone and the characters do:

- The map's drawer says the light it drew with (`sun::set_ground_light`, from `zone_shadow::record`); `draw_floor` takes
  it and writes into the map's header (words 6 to 12) what a ground facing up gets in all (ambient + sun x its height)
  and the sun's part of that. Not said: the game's usual daylight (ambient 0.39 of the sun's, grass.hlsl `floor_light`).
- Each blade looks up the sun's cascades, the casters' map and the clouds' map at its root (`blade_light`, the same
  HLSL as the zone's pass, compiled into grass.hlsl) and multiplies the map's colour by (all - sun's part x shadow) /
  all, a channel each: what the zone's pass leaves of the shaded ground beside it. Then the lamps' glow at the root
  goes onto the blade's colour as onto the ground (`sl_lit`).
- At the zone's end the blades write the light split's word on their pixels, marked: the shading pass leaves them.

- Brightness must match the ground the zone pass drew (per-pixel light with normal maps, the sky ambient, lamps): the
  blade's light is the fixed-function formula; tune against screenshots.
- Grass on a surface above another (a bridge, a ledge) takes the lowest: the height test sends it to the fallback.
- The map has no lamp light of the game's own (the zone pass's `lamp_light`) and none of its sky ambient: by a game
  lamp at night the blades are lit by World FX's glow alone, the ground by both.
- The ground pass clamps its light at 1 (`saturate`): on ground whose vertex colour times the day's light clips, the
  real ground loses less to a shadow than the blades' factor says (the blades a little darker than it in the shade).
