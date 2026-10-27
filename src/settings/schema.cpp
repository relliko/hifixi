#include "schema.hpp"
#include "../chars/charoptions.hpp" // (native characters)
#include "../zone/zoneoptions.hpp"
#include <cmath>
#include <cstring>

namespace hifi::settings
{
	namespace
	{
		constexpr float _ = NAN; // (a preset that leaves the setting as it is)

		// Off: the game as it draws itself. Balanced: what costs little (grass near, the sun's shadows, water, rain).
		// High: as much again, a little further (characters' shadows, the sky, lamp shadows, some ambient occlusion).
		// Ultra: everything, furthest. Custom: changed by hand since one was picked. The frame rate's options (above 60 fps
		// and its speed-ups, the no-wait visibility tests) are on in every preset, and by default.
		constexpr option_kind flag = option_kind::flag, number = option_kind::number, whole = option_kind::whole;

		const entry kEntries[] = {
			// Frame rate (hifps's, now HiFi XI's: [hifps])
			{ "hifps.enabled", tab::frame_rate, "Above 60 fps", "Run above 60 fps",
				"The game steps its world 60 times a second at most; this lets it run as fast as your PC draws, with movement, "
				"camera and effects kept at their own speed.", nullptr, nullptr, false, { 1.0f, 1.0f, 1.0f, 1.0f }, flag, 0.0f, 1.0f, 1.0f },
			{ "hifps.limit", tab::frame_rate, "Above 60 fps", "Frame cap", "The most frames a second (0: no cap).", "%d",
				"hifps.enabled", false, { _, _, _, _ }, whole, 0.0f, 360.0f, 240.0f },
			{ "hifps.counter", tab::frame_rate, "Above 60 fps", "Frame rate in the corner", "Frames a second, top left.", nullptr,
				nullptr, false, { _, _, _, _ }, flag, 0.0f, 1.0f, 1.0f },
			{ "hifps.exp_walls", tab::frame_rate, "Waiting on the GPU", "Hide characters by walls without waiting",
				"The game waits for the GPU to finish the frame so far to know whether a character is behind a wall (about 40% "
				"of the frame time). This answers it from the zone's collision instead; scenery without collision no longer "
				"hides characters, and you and your target are always shown.", nullptr, "hifps.enabled", false, { 1.0f, 1.0f, 1.0f, 1.0f },
				flag, 0.0f, 1.0f, 1.0f },
			{ "hifps.exp_occlusion", tab::frame_rate, "Waiting on the GPU", "Test hidden characters less often",
				"Above 60 fps the behind-a-wall test runs as often as at 60 fps, not every few frames more.", nullptr,
				"hifps.enabled", false, { 1.0f, 1.0f, 1.0f, 1.0f }, flag, 0.0f, 1.0f, 1.0f },
			{ "hifps.exp_ashita", tab::frame_rate, "Less work a frame", "Skip Ashita's per-draw dispatch",
				"Ashita hands each of the frame's ~3,000 draws to every plugin and addon. Its per-draw events stop; HiFi XI "
				"checks first that nothing loaded uses them.", nullptr, "hifps.enabled", false, { 1.0f, 1.0f, 1.0f, 1.0f }, flag, 0.0f, 1.0f, 1.0f },
			{ "hifps.ashita_force", tab::frame_rate, "Less work a frame", "Even if an addon or plugin uses them",
				"Skip them even when something loaded uses the per-draw events (it stops getting them).", nullptr, "hifps.exp_ashita",
				false, { _, _, _, _ }, flag, 0.0f, 1.0f, 0.0f },
			{ "hifps.exp_sse", tab::frame_rate, "Less work a frame", "SSE character skinning on any CPU",
				"The game uses its faster SSE code for characters only when the CPU says Intel; this turns it on for others.",
				nullptr, "hifps.enabled", false, { 1.0f, 1.0f, 1.0f, 1.0f }, flag, 0.0f, 1.0f, 1.0f },
			{ "hifps.exp_batch", tab::frame_rate, "Less work a frame", "Batch text and interface quads",
				"Consecutive text and interface quads with nothing changed between them become one draw. Same picture.", nullptr,
				"hifps.enabled", false, { 1.0f, 1.0f, 1.0f, 1.0f }, flag, 0.0f, 1.0f, 1.0f },
			{ "hifps.exp_dedupe", tab::frame_rate, "Less work a frame", "Drop repeated device calls",
				"About 40% of the game's device calls set a value already in effect; these are dropped.", nullptr, "hifps.enabled",
				false, { 1.0f, 1.0f, 1.0f, 1.0f }, flag, 0.0f, 1.0f, 1.0f },
			{ "VisTest", tab::frame_rate, "Waiting on the GPU", "Answer glow, lamp, lens flare and door tests without waiting",
				"The game asks the GPU whether glows, lamps, the lens flare and doors are hidden behind something, and waits for "
				"the answer every frame. This answers from a copy of the depth already drawn instead, a few frames old, so "
				"nothing waits.", nullptr, nullptr, false, { 1.0f, 1.0f, 1.0f, 1.0f } },

			// Lighting and shadows
			{ "SunShadows", tab::lighting, "Sun and moon", "Sun shadows", "How dark the shadows the sun casts are (0: none).",
				"%.2f", nullptr, false, { 0.0f, 0.5f, 0.55f, 0.6f } },
			{ "MoonShadows", tab::lighting, "Sun and moon", "Moon shadows", "By night, the moon's shadows as a share of the sun's (0: none).",
				"%.2f", nullptr, false, { 0.0f, 0.6f, 1.0f, 1.0f } },
			{ "SunMap", tab::lighting, "Sun and moon", "Shadows from the whole zone",
				"Shadows from a map of the zone's own meshes, so what's off the screen (a tree behind you, a cliff) casts "
				"too. Off: only what's on the screen casts.", nullptr, nullptr, false, { 0.0f, 1.0f, 1.0f, 1.0f } },
			{ "SunCharacters", tab::lighting, "Sun and moon", "Characters cast sun shadows",
				"Characters and monsters cast long shadows with the rest, in place of the game's own small ones (The game's own tab).",
				nullptr, nullptr, false, { 0.0f, 0.0f, 1.0f, 1.0f } },
			{ "WeatherShadows", tab::lighting, "Sun and moon", "Shadows fade under cloud and rain",
				"The shadows fade as the game dims its sun: weaker under cloud, gone in rain and fog.", nullptr, nullptr, false,
				{ _, 1.0f, 1.0f, 1.0f } },
			{ "AOStrength", tab::lighting, "Light on surfaces", "Ambient occlusion",
				"Darkens corners, creases and the ground where things meet it, as indirect light would (0: off).", "%.2f",
				nullptr, false, { 0.0f, 0.0f, 0.6f, 0.8f } },
			{ "AORadius", tab::lighting, "Light on surfaces", "Its reach", "How far from a corner the darkening reaches, in yalms.",
				"%.2f yalms", nullptr, false, { _, _, 0.6f, 1.0f } },
			{ "Relief", tab::lighting, "Light on surfaces", "Surface relief",
				"Brings out the shape of rock, bark and walls from the light across them (0: off).", "%.2f", nullptr, false,
				{ 0.0f, 0.0f, 0.4f, 0.6f } },
			{ "LampGlow", tab::lighting, "Lamps and glows", "Lamp glow",
				"Lit lamps, torches and street lights pool light on the ground and walls around them (0: off).", "%.2f",
				nullptr, false, { 0.0f, 0.5f, 0.5f, 0.6f } },
			{ "LampShadows", tab::lighting, "Lamps and glows", "Lamp shadows",
				"Characters near a lamp cast shadows away from it (0: none).", "%.2f", nullptr, false, { 0.0f, 0.0f, 0.5f, 0.6f } },
			{ "LampReach", tab::lighting, "Lamps and glows", "Lamp reach", "How far a lamp's light reaches, times its own range.",
				"%.2f", nullptr, false, { _, _, _, _ } },
			{ "LampsUnbaked", tab::lighting, "Lamps and glows", "Lights the zone's files don't list",
				"Also lights the game lights that aren't in the zone's lamp file (a fire someone made, an effect's light).",
				nullptr, nullptr, false, { _, _, _, _ } },
			{ "FixtureGlow", tab::lighting, "Lamps and glows", "Home Points, Survival Guides and such",
				"The game's glowing fixtures light what's around them (0: off).", "%.2f", nullptr, false, { 0.0f, 1.0f, 1.0f, 1.0f } },
			{ "EffectGlow", tab::lighting, "Lamps and glows", "Spells, abilities and fires",
				"Glowing effects light what's around them (0: off).", "%.2f", nullptr, false, { 0.0f, 0.6f, 0.8f, 1.0f } },
			// The look over the world (on in every preset: Alex wants new looks on by default)
			{ "Tonemap", tab::lighting, "Tone and colour", "Tone mapping",
				"A filmic curve over the world: bright parts roll off smoothly into white instead of clipping, a little more "
				"contrast through the middle, and an exposure that follows the scene slowly, so a bright noon isn't blown out "
				"and the night stays dark. The interface isn't touched.", nullptr, nullptr, false, { 1.0f, 1.0f, 1.0f, 1.0f } },
			{ "Bloom", tab::lighting, "Tone and colour", "Bloom",
				"A soft glow around the brightest things: the sun, lamps, glowing effects, a bright sky and the lit edges of "
				"clouds (0: none, 1: subtle).", "%.2f", nullptr, false, { 1.0f, 1.0f, 1.0f, 1.0f } },
			{ "ColourGrade", tab::lighting, "Tone and colour", "Colour grade",
				"The world's colours shift with the hour and the weather: warm at dawn and dusk, cool at night, grey in rain "
				"and gloom, a warm haze in heat and dust (0: off, 1: subtle). Not under a roof or underground.", "%.2f",
				nullptr, false, { 1.0f, 1.0f, 1.0f, 1.0f } },

			// The world: grass
			{ "Grass", tab::world, "Grass", "Grass", "Blades of grass over the ground wherever the zone's ground is grass.",
				nullptr, nullptr, false, { 0.0f, 1.0f, 1.0f, 1.0f } },
			{ "GrassDistance", tab::world, "Grass", "Distance", "How far from the camera grass is drawn.", "%.0f yalms", "Grass", false,
				{ _, 50.0f, 80.0f, 140.0f } },
			{ "GrassHeight", tab::world, "Grass", "Height", "How tall the blades are.", "%.2f yalms", "Grass", false,
				{ _, 0.6f, 0.75f, 0.75f } },
			{ "GrassTuft", tab::world, "Grass", "Blades in a tuft", "How thick the grass grows.", nullptr, "Grass", false,
				{ _, 6.0f, 10.0f, 12.0f } },
			{ "GrassWear", tab::world, "Grass", "Trodden down", "Grass bends under people walking and wears down where they fight, "
				"growing back over a few minutes (0: off).", "%.2f", "Grass", false, { 0.0f, 1.0f, 1.0f, 1.0f } },
			{ "GrassCuts", tab::world, "Grass", "Cut by weapon skills", "Weapon skills used near you cut a swath through the grass.",
				nullptr, "Grass", false, { 0.0f, 1.0f, 1.0f, 1.0f } },
			{ "Fireflies", tab::world, "Grass", "Fireflies at night", "Fireflies over the grass after dark (0: none).", "%.2f",
				"Grass", false, { 0.0f, 1.0f, 1.0f, 1.0f } },
			// Water
			{ "Water", tab::world, "Water", "Water", "Water of its own over the zone's: reflections of the sky and what's around, "
				"ripples, rings where people wade, darker where it's deep. The game's own water layers are hidden under it.",
				nullptr, nullptr, false, { 0.0f, 1.0f, 1.0f, 1.0f } },
			{ "WaterReflections", tab::world, "Water", "Reflections", "How strongly it reflects (1: as real water).", "%.2f", "Water",
				false, { _, 1.0f, 1.0f, 1.0f } },
			{ "WaterRipples", tab::world, "Water", "Ripples", "Waves on its surface (0: still).", "%.2f", "Water", false,
				{ _, 1.0f, 1.0f, 1.0f } },
			{ "WaterDepthTint", tab::world, "Water", "Darker where deep", "Deeper water darker and less clear (0: off).", "%.2f",
				"Water", false, { _, 0.5f, 0.5f, 0.5f } },
			{ "WaterRings", tab::world, "Water", "Rings from moving", "Rings spread where people and monsters move through it (0: none).",
				"%.2f", "Water", false, { _, 1.0f, 1.0f, 1.0f } },
			{ "WaterSea", tab::world, "Water", "The sea to the horizon", "The game's own sea past the zone's edge drawn as World FX "
				"water, out to the horizon.", nullptr, "Water", false, { 0.0f, 1.0f, 1.0f, 1.0f } },
			// Weather
			{ "WeatherFog", tab::world, "Weather", "Fog over the grass and water",
				"The grass, fireflies and water fade into the game's fog as the world does.", nullptr, nullptr, false,
				{ _, 1.0f, 1.0f, 1.0f } },
			{ "WetGround", tab::world, "Weather", "Wet paths and stone in rain",
				"Paths and stone darken and shine in rain, drying slowly after (0: off).", "%.2f", nullptr, false,
				{ 0.0f, 0.5f, 0.5f, 0.6f } },
			{ "Rain", tab::world, "Weather", "Rain", "Rain of its own that lands on the ground, with splashes and rings on water, in "
				"place of the game's.", nullptr, nullptr, false, { 0.0f, 1.0f, 1.0f, 1.0f } },
			{ "RainAmount", tab::world, "Weather", "How hard it rains", "Times the weather's own.", "%.2f", "Rain", false,
				{ _, 1.0f, 1.0f, 1.0f } },
			{ "RainSplashes", tab::world, "Weather", "Splashes", "Splashes where drops land (0: none).", "%.2f", "Rain", false,
				{ _, _, _, _ } },
			{ "RainRings", tab::world, "Weather", "Rings on the water", "Rings where drops hit water (0: none).", "%.2f", "Rain",
				false, { _, _, _, _ } },
			{ "Snow", tab::world, "Weather", "Snow", "Snow of its own, in place of the game's.", nullptr, nullptr, false,
				{ 0.0f, 1.0f, 1.0f, 1.0f } },
			{ "SnowAmount", tab::world, "Weather", "How hard it snows", "Times the weather's own.", "%.2f", "Snow", false,
				{ _, _, _, _ } },
			{ "SnowCover", tab::world, "Weather", "Snow settling on the ground",
				"While it snows, snow builds up over a few minutes on whatever faces up under open sky, and frosts and flattens "
				"the grass; it melts slowly after (0: off).", "%.2f", nullptr, false, { 0.0f, 1.0f, 1.0f, 1.0f } },
			{ "WeatherOwn", tab::world, "Weather", "All of the weather its own",
				"HiFi XI draws all of the weather itself, with its sky: clouds, lightning, dust and sand, wind, heat shimmer, "
				"auroras and gloom, as well as its rain and snow. The game's own weather effects are left out under open sky "
				"(they disturbed the shadows); their sound, light and fog stay.", nullptr, "Sky", false, { 0.0f, 0.0f, 1.0f, 1.0f } },
			{ "Clouds", tab::world, "Weather", "Clouds", "Clouds of its own, as thick and dark as the weather has them, drifting "
				"with the wind.", nullptr, "WeatherOwn", false, { _, _, 1.0f, 1.0f } },
			{ "CloudQuality", tab::world, "Weather", "Clouds' detail", "How finely the clouds are worked out (0 low .. 2 high): "
				"more steps through them, a little more GPU time.", nullptr, "Clouds", false, { _, _, 1.0f, 2.0f } },
			{ "CloudStyle", tab::world, "Weather", "Clouds' style", "0: game-like, thin soft layers in the sky's own colours as the "
				"game's are. 1: volumetric heaps lit by the sun, with dark bases and bright edges.", nullptr, "Clouds", false,
				{ 0.0f, 0.0f, 0.0f, 0.0f } },
			{ "Lightning", tab::world, "Weather", "Lightning", "In thunder and thunderstorms, flashes that light everything for a "
				"moment, and bolts in the sky.", nullptr, "WeatherOwn", false, { _, _, 1.0f, 1.0f } },
			{ "WeatherDust", tab::world, "Weather", "Dust and sand storms", "Dust and sand blown along the ground (0: none).",
				"%.2f", "WeatherOwn", false, { _, _, 1.0f, 1.0f } },
			{ "WeatherWind", tab::world, "Weather", "Wind and gales", "The grass bent hard with the wind, and streaks of dust and "
				"leaves (0: none).", "%.2f", "WeatherOwn", false, { _, _, 1.0f, 1.0f } },
			{ "HeatShimmer", tab::world, "Weather", "Heat shimmer", "In hot spells and heat waves, the air shimmering over distant "
				"ground (0: none).", "%.2f", "WeatherOwn", false, { _, _, 1.0f, 1.0f } },
			{ "Aurora", tab::world, "Weather", "Auroras", "Curtains of light across the sky in an aurora (0: none).", "%.2f",
				"WeatherOwn", false, { _, _, 1.0f, 1.0f } },
			{ "GloomFog", tab::world, "Weather", "Gloom", "In gloom and darkness, fog banks lying along the ground (0: none).",
				"%.2f", "WeatherOwn", false, { _, _, 1.0f, 1.0f } },
			// Sky
			{ "Sky", tab::world, "Sky", "Sky", "The game's sky gradient drawn smooth, with a sun, a moon and stars of its own.",
				nullptr, nullptr, false, { 0.0f, 0.0f, 1.0f, 1.0f } },
			{ "SkySun", tab::world, "Sky", "Its sun and moon", "How bright they are (0: none).", "%.2f", "Sky", false,
				{ _, _, 1.0f, 1.0f } },
			{ "SkyStars", tab::world, "Sky", "Its stars", "How bright the stars are at night (0: none).", "%.2f", "Sky", false,
				{ _, _, 1.0f, 1.0f } },

			// Distance and display (drawcull's, now HiFi XI's: [drawcull])
			{ "drawcull.world", tab::distance, "View distance", "World", "How far the zone is drawn, times the game's own.", "%.1fx",
				nullptr, false, { _, _, _, _ }, number, 1.0f, 10.0f, 1.0f },
			{ "drawcull.entity", tab::distance, "View distance", "Characters and monsters", "How far they're drawn, times the game's own.",
				"%.1fx", nullptr, false, { _, _, _, _ }, number, 1.0f, 10.0f, 1.0f },
			{ "drawcull.lod", tab::distance, "View distance", "Detail", "How far the zone's trees and props keep their finer meshes, "
				"times the game's own (0: as the world).", "%.1fx", nullptr, false, { _, _, _, _ }, number, 0.0f, 10.0f, 0.0f },
			{ "drawcull.vis", tab::distance, "View distance", "Ignore the zone's visibility lists",
				"The zone's own lists of what can be seen from where keep far terrain from being drawn however far the view "
				"goes, and it pops in as you move. Off: the lists as the game uses them.", nullptr, nullptr, false, { _, _, _, _ },
				flag, 0.0f, 1.0f, 1.0f },
			{ "drawcull.frustum", tab::distance, "View distance", "Cull at the camera's own height of view",
				"The game culls the zone against a view far taller than the camera's, so what's above and below the screen is "
				"drawn anyway. Nothing on screen is cut, and effects (water sheets, smoke, sparks) keep the game's own view.", nullptr,
				nullptr, false, { 1.0f, 1.0f, 1.0f, 1.0f }, flag, 0.0f, 1.0f, 1.0f },
			{ "drawcull.square", tab::distance, "Display", "Square pixels",
				"The game draws the world about 5.5% wider than the window: people look broad, round things oval. This draws it "
				"at the window's own shape.", nullptr, nullptr, false, { _, _, _, _ }, flag, 0.0f, 1.0f, 0.0f },

			// The game's own, left out where HiFi XI draws its own
			{ "HideFlare", tab::game, "Left out", "The game's lens flare",
				"The rings the game draws across the screen when you look toward the sun aren't drawn.", nullptr, nullptr, false,
				{ 0.0f, 1.0f, 1.0f, 1.0f } },
			{ "HideGameWater", tab::game, "Left out", "The game's water, where HiFi XI draws its own",
				"The game's scrolling water layers, and the ripples at a wading character's feet, aren't drawn where HiFi XI draws "
				"its water (in a zone it has water for, with its Water setting on).", nullptr, nullptr, false, { 0.0f, 1.0f, 1.0f, 1.0f } },
			{ "HideGameRain", tab::game, "Left out", "The game's weather, while HiFi XI draws its own",
				"With \"All of the weather its own\" on, none of the game's weather effects are drawn under open sky (its clouds, "
				"sun, moon and stars, rain, snow, lightning, dust...): HiFi XI draws them. Otherwise only the game's raindrops and "
				"snowflakes are left out while HiFi XI's rain or snow falls. The weather's sound, light and fog stay.", nullptr,
				nullptr, false, { 0.0f, 1.0f, 1.0f, 1.0f } },
			{ "HideGameShadows", tab::game, "Left out", "The game's character shadows, while HiFi XI casts its own",
				"The game's own shadow under each character (your silhouette, blobs under the rest) isn't drawn while HiFi XI's "
				"characters cast sun or moon shadows here: two at different angles looked like copies of one. Where HiFi XI casts "
				"none (no open sky, the sun down and no moon), the game's are drawn as before.", nullptr, "SunCharacters", false,
				{ 1.0f, 1.0f, 1.0f, 1.0f } },
			{ "zone.native", tab::game, "Drawn by HiFi XI (experimental)", "The zone's scenery",
				"HiFi XI draws the zone's buildings, ground and plants itself from your game's files, where the game drew them, "
				"lit as the game lights them. What it doesn't have yet the game still draws. (/hifi nativezone)", nullptr, nullptr,
				false, { 0.0f, zone::options::kNativeByDefault ? 1.0f : _, zone::options::kNativeByDefault ? 1.0f : _, zone::options::kNativeByDefault ? 1.0f : _ },
				flag, 0.0f, 1.0f, zone::options::kNativeByDefault ? 1.0f : 0.0f },
			{ "zone.perpixel", tab::game, "Drawn by HiFi XI (experimental)", "Light the scenery per pixel",
				"The sun and lamps worked out at each pixel instead of at each corner, and the normal maps used, "
				"for smoother light and sharper detail where a surface has one. (/hifi nativezone perpixel)", nullptr, "zone.native", false,
				{ _, zone::options::kPerPixelByDefault ? 1.0f : _, zone::options::kPerPixelByDefault ? 1.0f : _, zone::options::kPerPixelByDefault ? 1.0f : _ },
				flag, 0.0f, 1.0f, zone::options::kPerPixelByDefault ? 1.0f : 0.0f },
			// (native characters: their default in src/chars/charoptions.hpp)
			{ "chars.native", tab::game, "Drawn by HiFi XI (experimental)", "Characters and monsters",
				"HiFi XI draws the characters, monsters and NPCs itself from what your game has loaded, where the game drew them, "
				"posed and lit as the game poses and lights them. A model whose data isn't on the GPU yet the game still draws. "
				"(/hifi nativechars)", nullptr, nullptr, false,
				{ 0.0f, chars::options::kNativeByDefault ? 1.0f : _, chars::options::kNativeByDefault ? 1.0f : _, chars::options::kNativeByDefault ? 1.0f : _ },
				flag, 0.0f, 1.0f, chars::options::kNativeByDefault ? 1.0f : 0.0f },
			{ "chars.perpixel", tab::game, "Drawn by HiFi XI (experimental)", "Light the characters per pixel",
				"The sun, moon and lamps on characters worked out at each pixel instead of at each corner: smoother light on "
				"faces and folds. (/hifi nativechars perpixel)", nullptr, "chars.native", false,
				{ _, chars::options::kPerPixelByDefault ? 1.0f : _, chars::options::kPerPixelByDefault ? 1.0f : _, chars::options::kPerPixelByDefault ? 1.0f : _ },
				flag, 0.0f, 1.0f, chars::options::kPerPixelByDefault ? 1.0f : 0.0f },

			// Advanced
			{ "GrassHue", tab::advanced, "Grass colour", "Grass colour where the ground hasn't been seen",
				"Grass takes its colour from the ground under it as the ground comes into view; until then, this.", nullptr,
				"Grass", false, { _, _, _, _ } },
			{ "GrassRuleInFront", tab::advanced, "Grass colour", "Learn only from the ground itself",
				"Colours aren't learned from characters, trees, names or the sky in front of the ground.", nullptr, "Grass", false,
				{ _, _, _, _ } },
			{ "GrassRuleOutliers", tab::advanced, "Grass colour", "Drop colours far off the rest",
				"A sample far brighter or darker than the ones around it isn't learned.", nullptr, "Grass", false, { _, _, _, _ } },
			{ "FireflySize", tab::advanced, "Fine-tuning", "Firefly glow", "How big a firefly's glow is.", "%.2f yalms", "Grass", false,
				{ _, _, _, _ } },
			{ "GpuCrashReports", tab::advanced, "Diagnostics", "GPU crash reports",
				"If the GPU stops (a hang, a page fault), what it was doing goes to worldfx-gpu-crash.txt in the bootloader folder.",
				nullptr, nullptr, true, { _, _, _, _ } },
			{ "D3D12DebugLayer", tab::advanced, "Diagnostics", "Direct3D 12 debug layer",
				"Checks every graphics call and writes what's wrong to worldfx-d3d12-debug.txt. Slow: for tracking down a problem "
				"(Windows' Graphics Tools must be installed).", nullptr, nullptr, true, { _, _, _, _ } },
			{ "WaterDebug", tab::advanced, "Diagnostics", "Show the zone's water collision",
				"Blue deep, cyan shallow, yellow steep: at or behind what the game drew; red: something in front of it.", nullptr,
				nullptr, false, { _, _, _, _ } },
			{ "SkyDebug", tab::advanced, "Diagnostics", "Show where the sky is repainted",
				"Magenta: repainted; red: sky left as the game drew it (clouds, its sun and moon, effects over its dome).", nullptr,
				nullptr, false, { _, _, _, _ } },
		};
	}

	const char *tab_name(tab t)
	{
		switch (t)
		{
		case tab::overview: return "Overview";
		case tab::frame_rate: return "Frame rate";
		case tab::distance: return "Distance and display";
		case tab::lighting: return "Light and shadow";
		case tab::world: return "Grass, water, weather";
		case tab::game: return "The game's own";
		case tab::performance: return "Performance";
		case tab::advanced: return "Advanced";
		default: return "?";
		}
	}

	const char *preset_key(preset p)
	{
		switch (p)
		{
		case preset::off: return "off";
		case preset::balanced: return "balanced";
		case preset::high: return "high";
		case preset::ultra: return "ultra";
		default: return "custom";
		}
	}

	const char *preset_label(preset p)
	{
		switch (p)
		{
		case preset::off: return "Off";
		case preset::balanced: return "Balanced";
		case preset::high: return "High";
		case preset::ultra: return "Ultra";
		default: return "Custom";
		}
	}

	const char *preset_help(preset p)
	{
		switch (p)
		{
		case preset::off: return "The game as it draws itself: no grass, shadows, water, rain or sky of HiFi XI's (its frame rate speed-ups, "
			"tone mapping, bloom and colour grade stay on).";
		case preset::balanced: return "What costs little: grass near you, the sun's shadows, water, rain and snow, lamp glow.";
		case preset::high: return "More of it, further: characters' shadows, the sky, lamp shadows, some ambient occlusion.";
		case preset::ultra: return "Everything, at its furthest and thickest.";
		default: return "Changed by hand since a preset was picked.";
		}
	}

	preset preset_from(const char *key)
	{
		for (int i = 0; i <= int(preset::custom); ++i)
			if (key != nullptr && std::strcmp(key, preset_key(preset(i))) == 0)
				return preset(i);
		return preset::custom;
	}

	const entry *entries(size_t &count)
	{
		count = sizeof(kEntries) / sizeof(kEntries[0]);
		return kEntries;
	}

	const entry *find(const char *key)
	{
		for (const entry &e : kEntries)
			if (key != nullptr && std::strcmp(e.key, key) == 0)
				return &e;
		return nullptr;
	}

	bool split_option(const char *key, char *section, size_t section_size, const char **name)
	{
		const char *dot = key != nullptr ? std::strchr(key, '.') : nullptr;
		if (dot == nullptr || size_t(dot - key) + 1 > section_size)
			return false;
		std::memcpy(section, key, size_t(dot - key));
		section[dot - key] = 0;
		*name = dot + 1;
		return true;
	}

	bool is_option(const entry &e)
	{
		return std::strchr(e.key, '.') != nullptr;
	}
}
