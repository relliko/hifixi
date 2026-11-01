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

## Risks

- Brightness must match the ground the zone pass drew (per-pixel light with normal maps, the sky ambient, lamps): the
  blade's light is the fixed-function formula; tune against screenshots.
- Grass on a surface above another (a bridge, a ledge) takes the lowest: the height test sends it to the fallback.
