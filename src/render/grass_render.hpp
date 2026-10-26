/*
 * Grass for FFXI's world on Direct3D 12 (prototype): the renderer, free of ReShade so a test can drive
 * it on its own device (tests/grass_test.cpp).
 *
 * Blades come from a baked file (tools/bake_grass.py: points on the zone's grass, from its collision,
 * grouped in square cells). Each point is a tuft of curved blades, drawn with the game's own camera into
 * the world's render target and depth buffer. Their colour comes from the ground under them: the world
 * as drawn is copied into a buffer first, so grass takes the zone's texture, lighting, fog and time of
 * day, and each point remembers it across frames. Only cells near the camera are drawn.
 *
 * Drawing changes the command list's root signature, root arguments, pipeline, topology, render
 * targets, viewport and scissor; list_state holds the game's and puts them back.
 */

#pragma once

#include "vistest.hpp"
#include <d3d12.h>
#include <cstdint>
#include <string>
#include <vector>

namespace grass
{
	struct blade_file
	{
		struct cell
		{
			int32_t cx, cz;
			uint32_t first, count;
		};
		uint32_t zone = 0;
		float cell_size = 16.0f;
		std::vector<cell> cells;
		std::vector<float> blades; // x, y, z, seed per blade (the game's world: y is height, down is positive)
		uint32_t stride = 4;       // floats a record
		uint32_t blade_count() const { return static_cast<uint32_t>(blades.size() / stride); }
	};

	// Also the water files (tools/bake_water.py, tag 'WTR2'): the same layout, a record (8 floats) a triangle
	// corner, three a triangle: the collision's x, y, z, kind (8 shallow, 9 deep, +100 steep), then the water's
	// surface y over it, how far the water's edge is (yalms), and its flow (x, z, yalms a second)
	bool read_blade_file(const wchar_t *path, blade_file &out, std::string &error, const char *magic = "GRS2", uint32_t stride = 4);

	// The game's own sea sheets that carry on past the zone's water (tools/bake_sea.py, HiFi XI's src/bake/sea.cpp: 'SEA1':
	// the zone, how many sheets, how many corners, then the corners in the water file's layout: x, bed y (the surface's +
	// 30), z, kind 200; the surface's y (already at the baked water's level), edge 8 (open water), no flow). No cells: a few
	// big sheets, drawn whole. Read into out as a water file with no cells (stride 8); sheets: how many. "no file for this
	// zone" when there's none (a zone the bake keeps none for has a .none file instead: the same).
	bool read_sea_file(const wchar_t *path, blade_file &out, uint32_t &sheets, std::string &error);

	// The zone's visual meshes as one triangle soup, for the sun's shadow map (tools/bake_shadow_geo.py, tag
	// 'SHD1'): triangles grouped in square cells by their centre, 12 words each: three corners of x, y, z (floats)
	// and w (0 on a solid triangle; on a cut-out one, u | v << 12 | mask << 24: 12-bit texture coordinates and
	// the cut-out texture's index + 1). The cut-out textures' alpha as bits: 4 words each (width, height, first
	// word, words a row), then the words. A compact file ('SHD2': each model once and the zone's placements, packed)
	// is expanded into the same when read.
	struct shadow_file
	{
		uint32_t zone = 0;
		float cell_size = 32.0f;
		std::vector<blade_file::cell> cells;
		std::vector<uint32_t> triangles;
		std::vector<uint32_t> masks;
		std::vector<uint32_t> words;
		uint32_t triangle_count() const { return static_cast<uint32_t>(triangles.size() / 12); }
	};
	bool read_shadow_file(const wchar_t *path, shadow_file &out, std::string &error);

	// Where a zone's ground is path or stone (tools/bake_wet.py, tag 'WET1'), for the wet look in rain: a mask seen
	// from above, a bit a square (square yalms across), set where the topmost surface facing up there is path or
	// stone; rows along x from the corner (x0, z0), words_a_row words each (bit i of word k: the square 32 k + i)
	struct wet_file
	{
		uint32_t zone = 0;
		float square = 0.5f;
		float x0 = 0.0f, z0 = 0.0f;
		uint32_t width = 0, height = 0, words_a_row = 0, set = 0;
		std::vector<uint32_t> words;
	};
	bool read_wet_file(const wchar_t *path, wet_file &out, std::string &error);

	// Where rain stops in a zone (tools/bake_rain.py, tag 'RHM1'): seen from above, the first surface a drop falling
	// straight down meets over each square (square yalms across), from the corner (x0, z0): its height y0 + q * step
	// (the game's y, down positive) and what it is (kRain*: nothing under it at all, something that stops rain without
	// a splash (steep, leaves), ground facing up, the add-on's water). Rows along x, z first.
	enum : uint8_t { kRainNothing = 0, kRainStops = 1, kRainGround = 2, kRainWater = 3 };
	struct rain_file
	{
		uint32_t zone = 0;
		float square = 0.5f;
		float x0 = 0.0f, z0 = 0.0f;
		float y0 = 0.0f, step = 1.0f / 16.0f;
		uint32_t width = 0, height = 0, covered = 0;
		std::vector<uint16_t> q;
		std::vector<uint8_t> kinds;
	};
	// (unpacked with Windows' own XPRESS: ntdll's RtlDecompressBufferEx)
	bool read_rain_file(const wchar_t *path, rain_file &out, std::string &error);


	// The game's camera: D3D row-vector matrices, as the game keeps them (gGame +0x94 and +0x154)
	struct camera
	{
		float view[16];
		float proj[16];
	};

	// The game's weather (client-notes "Weather and sky"), as the worldfx Ashita addon reads it and sends it
	// (worldfx_weather): kWeatherWords words, as the game keeps them (floats as their bits):
	//   [0] the weather's id (the server's: 0 fine .. 19 dark; 0xFFFFFFFF not read)
	//   [1] the weather shown (its 4-character name: the zone's weather folder in use; 0 none)
	//   [2] while it changes, the one before (its name; 0 none), [3] how far the change is (f32 0..1: the new one's share)
	//   [4] the fog the game draws the world with this frame (D3DCOLOR 0x00RRGGBB), [5] where it starts, [6] where it
	//       ends (f32, yalms of view depth: the record's, times the game's draw distance setting)
	//   [7] what was read: 1 the record, 2 the fog, 4 the weather object, 8 the id
	//   [8..51] the record the game lights the zone by now (0xB0 bytes: its weather's keyframes blended by the hour),
	//       its sky part (record +0x4C, words [27..50]) with it: the clear colour, the dome's segments, its 8 gradient
	//       colours and their 8 heights (client-notes "Sky")
	constexpr uint32_t kWeatherWords = 52;

	struct weather
	{
		bool known = false;            // anything read at all
		uint32_t id = 0xFFFFFFFF;      // (the server's weather)
		uint32_t name = 0, before = 0; // the weather shown, and the one it's changing from (4 characters each; 0 none)
		float change = 1.0f;           // the new one's share while it changes (1: done)
		// The fog the world is drawn with: linear by view depth from start to end (yalms), toward colour (0..1); only from
		// the renderer (the record's own is before the draw distance setting)
		bool fog = false;
		float fog_color[3] = {};
		float fog_start = 0.0f, fog_end = 0.0f;
		// From the record: the zone's light (the sun's or moon's colour and intensity, the ambient), the sky's colour,
		// how far the zone is drawn (yalms) and the record's own fog (before the draw distance setting)
		bool record = false;
		float sun_color[3] = {}, ambient[3] = {}, sky[3] = {};
		float intensity = 0.0f, draw_distance = 0.0f;
		float record_fog_color[3] = {};
		float record_fog_start = 0.0f, record_fog_end = 0.0f;
		// The record's sky (its sky part, record +0x4C; client-notes "Sky"): the game draws a dome around the camera from
		// it when segments > 2, the 8 gradient colours (bytes, 0..255) at their 8 heights (0 the horizon .. 1 overhead:
		// the elevation over 90 degrees), and clears the world to the clear colour (bytes) behind it. sky_known when the
		// heights make sense (0 to 1, rising).
		bool sky_known = false;
		uint32_t sky_segments = 0;
		float sky_clear[3] = {};
		float sky_colors[8][3] = {};
		float sky_heights[8] = {};
	};
	// The weather from the addon's words (false when they hold nothing usable: then out is "not known")
	bool decode_weather(const uint32_t *words, uint32_t count, weather &out);
	// A weather's 4-character name as the word the game keeps it in ('fine' -> 0x656E6966)
	constexpr uint32_t weather_name(const char (&s)[5])
	{
		return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24;
	}
	// A zone's clear sky through the day (tools/bake_clear_sky.py, worldfx-clearsky.txt next to the add-on): the light of
	// its sun (or moon) in its clear weather (fine or sunshine, the brighter; a zone with neither, its brightest weather),
	// the record's sun colour's luminance times its light intensity, at each Vana'diel hour
	struct clear_sky
	{
		bool known = false;
		float light[24] = {};
	};
	// The file's zones, indexed by zone id (false and why when it can't be read)
	bool read_clear_sky_file(const wchar_t *path, std::vector<clear_sky> &out, std::string &error);
	// Its light at a Vana'diel hour (0..24): linear between the hours, round midnight, as the game blends its keyframes
	float clear_sky_light(const clear_sky &sky, double hour);
	// The game's own sun (or moon) light in the weather's record: its colour's luminance times the zone's intensity (0
	// when the record isn't known)
	float weather_light(const weather &w);
	// How much of the sun's (and the moon's) shadows the weather leaves: the game's own light in the record now as a share
	// of the zone's clear sky's at the same hour, 1 at most. The game dims its sun that much (over all zones, at the
	// median: cloud 0.89, mist 0.84, rain and squalls 0.59, thunder 0.65; a change between two is blended in its record
	// already). 1 when the record isn't known or the clear sky's light is too faint to compare with (the darkest hours).
	float weather_sunlight(const weather &w, float clear_light);
	// How much it rains in a weather (1: rain, squalls, thunder, thunderstorms; else 0), blended the same way
	float weather_rain(uint32_t name);
	float weather_rain(const weather &w);
	// How hard it rains (rain 0.55, squalls 0.8, thunder and thunderstorms 1; else 0) and how much the wind slants the
	// drops and flakes (yalms across for each yalm fallen: blizzards the most, then squalls), blended the same way
	float weather_downpour(uint32_t name);
	float weather_downpour(const weather &w);
	float weather_slant(uint32_t name);
	float weather_slant(const weather &w);
	// How hard it snows (snow 0.6, blizzards 1; else 0) and how far its flakes sway (yalms: snow 0.35, blizzards
	// 0.15, the wind carrying them), blended the same way
	float weather_snowfall(uint32_t name);
	float weather_snowfall(const weather &w);
	float weather_sway(uint32_t name);
	float weather_sway(const weather &w);

	// What a weather brings of World FX's own beyond rain and snow (the game's weather folders drew these: its clouds,
	// lightning, dust, wind, heat haze, aurora, gloom, a starry sky), by its name (either set), each 0 when it has none:
	//   cover: how much of the sky its clouds cover (0 none .. 1 overcast), thick: how opaque they are at most, dark: how
	//   dark their undersides (0 white .. 1 near black), veil: a low-contrast haze over the whole sky (mist), speed: how fast
	//   they drift (1 a light breeze's);
	//   wind: how hard the wind blows near the ground (0 calm .. 1 wind .. 2 gales: the grass bends with it), windy: 1 in
	//   wind and gales (their streaks and leaves), streaks: dust blown along the ground in streaks, leaves: leaves blown
	//   along it;
	//   dust: dust or sand thick in the air (a storm's: 1), sand: 0 dust (brown-grey) .. 1 sand (tan);
	//   flashes: lightning (thunder 0.7, thunderstorms 1), heat: heat shimmer (hot spells 0.5, heat waves 1), aurora: its
	//   curtains, gloom: fog banks along the ground, sky_dark: how much darker the sky (0 as the record has it), stars: the
	//   stars' brightness and number (1 as they are; stellar glare's more);
	//   bottom, top: where its clouds' slab lies (yalms over the camera: fine weather's heaps high, rain's deck low and deep),
	//   shape: 0 heaps (flat bases, rounded tops) .. 1 a flat deck, erosion: how much the fine noise eats their edges (wisps),
	//   dense: how dense (1 fair weather's heaps; a rain deck's twice that, mist's a third)
	struct weather_kind_fx
	{
		float cover = 0.0f, thick = 0.0f, dark = 0.0f, veil = 0.0f, speed = 0.0f;
		float wind = 0.0f, windy = 0.0f, streaks = 0.0f, leaves = 0.0f;
		float dust = 0.0f, sand = 0.0f;
		float flashes = 0.0f, heat = 0.0f, aurora = 0.0f, gloom = 0.0f, sky_dark = 0.0f, stars = 1.0f;
		float bottom = 250.0f, top = 400.0f, shape = 0.0f, erosion = 1.0f, dense = 1.0f;
	};
	weather_kind_fx weather_fx_of(uint32_t name);
	// The same blended through a change (the one before's by the share left), as the game blends its records; nothing (all
	// 0, stars 1) when the weather isn't known
	weather_kind_fx weather_fx_of(const weather &w);
	// Which way the wind blows in a zone and weather (radians: the bearing it blows toward, from north toward east; x east,
	// z north). The game has no wind of its own: this is the way its own clouds drifted there (research/ffxi-re/client-notes.md
	// "Wind": each weather's sky layers scroll; 459 zone and weather pairs, either name of a weather); for a weather the
	// zone's clouds don't say, the way its other weathers' go (their mean), for a zone with none, west (the way they go in
	// most zones). The second blends through a change (the two ways by their shares).
	float wind_bearing(uint32_t zone, uint32_t name);
	float wind_bearing(uint32_t zone, const weather &w);

	// Characters and monsters that push grass aside, at most
	constexpr uint32_t kMaxActors = 32;
	// Weapon-skill cuts drawn into the grass at once (look::cuts), and how long each is kept for the frames to find
	// (the points it covers are cut on the first frame they're drawn in that time; again changes nothing)
	constexpr uint32_t kMaxCuts = 8;
	constexpr double kCutKeep = 3.0;
	// Rings spreading on the water from them as they move through it, at most at once
	constexpr uint32_t kMaxRipples = 40;
	// Lamps whose glow the shading pass adds, at most (the nearest lit ones in view: the add-on picks them)
	constexpr uint32_t kMaxLamps = 16;

	struct look
	{
		// Which of the passes this draw makes: all of them (the world's end, as before); the blades alone, into the
		// world's own depth (at the zone pass's end marker, before characters and effects, so they depth-test against
		// them: target::world_dsv); or the rest without the blades (the world's end, after blades drawn at the marker)
		enum class phase_t : uint8_t { all, blades, rest };
		phase_t phase = phase_t::all;
		float distance = 40.0f; // yalms from the camera
		float height = 0.5f;    // yalms, before the per-blade variation
		float width = 0.03f;    // half the base, yalms
		float color[3] = { 0.33f, 0.52f, 0.16f }; // the hue; brightness comes from the ground
		int tuft = 5;           // blades per baked point
		float time = 0.0f;      // seconds, for the wind
		const float *actors = nullptr; // characters and monsters near: x, y, z (their feet), radius each
		uint32_t actor_count = 0;      // (at most kMaxActors are used)
		// Wear: the grass trodden down where they stand, more where they fight, and cut by weapon skills, growing back
		// over minutes. clock: seconds on a steady clock (0: no wear at all); wear: how much it shows (0 none, up to 2);
		// fighting: which of the actors are fighting (bit i: actor i); cuts: kMaxCuts at most, 8 floats each: a swath from
		// x, y, z (the game's world) with its half-width (yalms), to x, y, z, and how long ago it was cut (seconds; those
		// over kCutKeep ago are left out)
		double clock = 0.0;
		float wear = 0.0f;
		uint32_t fighting = 0;
		const float *cuts = nullptr;
		uint32_t cut_count = 0;
		// Colour rules: a root's colour samples leave out what isn't at the ground's height (characters, trees,
		// names, the background) and samples far brighter or darker than the rest
		bool rule_in_front = true, rule_outliers = true;
		bool grass = true;              // the blades (off: only the shading below)
		// The shading pass, before the grass and the interface: ambient occlusion (0 off) within a radius,
		// and surface relief from the textures' own detail (0 off)
		float ao_strength = 0.8f, ao_radius = 1.0f, relief = 0.6f;
		// Sun shadows: toward the sun (the game's world: x east, y height with down positive, z north, a unit
		// vector) and how dark its shadows are (0: none, the sun down or no sky)
		float sun[3] = { 0.0f, -1.0f, 0.0f };
		float sun_shadow = 0.0f;
		// Whether characters and monsters (actors) cast sun shadows too: the game draws theirs itself
		bool sun_characters = false;
		// The sun's shadow map from the zone's own meshes (upload_shadow), when loaded: what's off screen casts
		// too, and the march across the screen only reaches a couple of yalms there (contact shadows)
		bool sun_map = true;
		// Water, on the surface rebuilt over the zone's water collision: reflections (0 none, 1 as real water,
		// up to 2), ripples (0 a still mirror) and darker where deep (0 none), within this many yalms. The sky
		// when nothing on screen shows it (rgb), and the sun's glints (0 none: down or no sky) in its colour.
		bool water = true;
		float water_reflect = 1.0f, water_ripples = 1.0f, water_tint = 1.0f;
		float water_distance = 300.0f;
		// The sea (with the zone's sea sheets uploaded: upload_sea): the game's own sea past the zone's water drawn as the
		// water is, where the baked water isn't, out to sea_distance yalms (the game's fog's end); the baked water then
		// reaches as far, so nothing changes where one would have faded out
		bool sea = true;
		float sea_distance = 1000.0f;
		float sky[3] = { 0.4f, 0.52f, 0.66f };
		float glint = 0.0f;
		float sun_color[3] = { 1.0f, 0.95f, 0.85f };
		// The zone's fog colour (what the game clears the world to), for the sky along the horizon; not yet
		// seen, the sky above is used there too
		float fog[3] = { 0.0f, 0.0f, 0.0f };
		bool fog_known = false;
		// Rings on the water from characters and monsters moving through it, 8 floats each: where they started
		// (x, the water's surface y, z), how strong (0..1), how old (seconds) and the ring's first radius (yalms)
		const float *ripples = nullptr;
		uint32_t ripple_count = 0; // (at most kMaxRipples are used)
		float water_rings = 1.0f;  // how strong (0 none, up to 2)
		// The zone's water collision drawn over the world (a debug view)
		bool water_debug = false;
		// Fireflies over the grass points at night: how many (0 none, 1 the usual, up to 2), how deep into the
		// night it is (0 day .. 1 full night: they fade in at dusk), the glow's radius and how far they're drawn
		float fireflies = 1.0f, night = 0.0f;
		float firefly_size = 0.15f, firefly_distance = 40.0f;
		// Lamps lit now (the zone's, from its bake, on or off as the game's light table says), their glow added
		// by the shading pass on the ground and walls around them, 8 floats each: where the light is (x, y, z,
		// the game's world), how far its glow reaches (yalms), its colour times how strong its glow is (rgb, the
		// amount setting in it already), 1 when it casts lamp shadows (the zone's lights; a glow 0). And how much of a lamp's glow a surface it can't see loses (0 none:
		// no lamp shadows). (At most kMaxLamps are used.)
		const float *lamps = nullptr;
		uint32_t lamp_count = 0;
		float lamp_shadows = 0.0f;
		// The game's own fog, as its weather and the hour have it now: linear by view depth, none at haze_start
		// yalms, all haze_color at haze_end (the game's fog render states). The grass, the fireflies and the water
		// fade into it as the scenery does, and what the shading pass darkens (ambient occlusion, relief, shadows)
		// only the scene under it, not the fog over it. Off: drawn clear, as before.
		bool haze = false;
		float haze_color[3] = {};
		float haze_start = 0.0f, haze_end = 0.0f;
		// How much of the sun's (or moon's) shadows and glints the weather leaves (weather_sunlight: the game's light now
		// against the zone's clear sky's, 1 at most), times sun_shadow and glint
		float weather_sun = 1.0f;
		// Wet ground in rain (0 dry .. 1 soaked), where the zone's wet mask (upload_wet) has path or stone, in the
		// shading pass: darker, the sky mirrored at grazing angles (wet_sheen: its colour) and a little highlight
		// toward the game's light (wet_light: its colour)
		float wet = 0.0f;
		float wet_sheen[3] = { 0.4f, 0.45f, 0.5f };
		float wet_light[3] = { 0.6f, 0.6f, 0.6f };
		// Rain (with the zone's rain map loaded: upload_rain): how hard (0 none .. 1 a thunderstorm's, times the
		// amount setting), the drops' slant (yalms across for each yalm fallen, x and z: the snow's too), the light
		// they catch (rgb: the overcast sky's), how many splashes (0 none, up to 2) and how strong the rings on the
		// water (0 none, up to 2). Drops fall in a volume around the camera and end at the first surface under them
		// (the map, and the tops of the characters and monsters near); splashes where they end near the camera.
		float rain = 0.0f;
		float rain_slant[2] = { 0.0f, 0.0f };
		float rain_light[3] = { 0.55f, 0.58f, 0.62f };
		float rain_splashes = 1.0f, rain_rings = 1.0f;
		// Snow (with the zone's rain map loaded too: flakes end where drops do): how hard (0 none .. 1 a blizzard's,
		// times the amount setting), how far a flake sways (yalms), the light the flakes catch (rgb). Slanted by
		// rain_slant; no splashes.
		float snow = 0.0f;
		float snow_sway = 0.35f;
		float snow_light[3] = { 0.85f, 0.87f, 0.9f };
		// A sky of its own, in the shading pass, on the pixels where the zone drew nothing (the game's sky dome doesn't
		// write depth: what's left there is the cleared depth): the weather record's gradient again, evaluated for each
		// pixel's view ray (smooth: no strips, dithered), with a sun or moon toward the game's light and stars at night.
		// Only where a pixel is still the colour the game's dome gives there (dome_colour): what the game drew over its
		// dome later in the frame (clouds, its own sun and moon, effects) stays as drawn.
		struct sky_dome
		{
			bool on = false;       // (the record's sky known, with a dome: segments > 2)
			uint32_t segments = 0; // the game's dome's segments around
			float stops[8][4] = {}; // its gradient: colour (bytes, 0..255) and height (0..1), the horizon first
			float clear[3] = {};    // the clear colour (bytes): what's behind the dome, below the horizon
			float sun = 0.0f;       // how strong the sun's (or the moon's) disc and glow (0 none: down, or no clear sky)
			float moon = 0.0f;      // 0 the sun, 1 the moon (the game's light by night), between through twilight
			float disc[3] = { 1.0f, 0.95f, 0.85f }; // their colour
			float stars = 0.0f;     // how bright the stars (0 none: by day, or no clear sky)
			float star_turn = 0.0f; // how far the stars have turned about the pole (radians: the Vana'diel hour's)
			float moon_lit = 1.0f;  // how much of the moon is lit (the game's phase: 0 new .. 1 full)
			float moon_side[3] = { -1.0f, 0.0f, 0.0f }; // which way its lit side faces (the world's: west waxing, east waning)
			bool debug = false;     // what's repainted tinted magenta, sky left as the game drew it (clouds...) red
		} dome;
		// The weather of its own beyond rain and snow (weather_fx_of; the core fills it in while World FX draws all of the
		// zone's weather, its settings in it): each part 0 when it isn't there. The sky's parts (clouds, aurora, the bolt, a
		// darker or starrier sky) are drawn with the sky of its own (dome.on); heat shimmer in the shading pass; dust, sand
		// and wind streaks, gloom's fog banks and the lightning's flash over everything drawn at the end (the air pass:
		// over the grass, the water and the rain too); the grass bends with the wind; leaves fly on the rain map.
		struct weather_fx
		{
			// Toward where the wind blows (x, z: the game's world, a unit vector) and how hard (0 calm .. 1 wind .. 2 gales)
			float wind[2] = { 1.0f, 0.0f };
			float wind_strength = 0.0f;
			// The ground under the camera (the game's y, down positive): fog banks and dust lie on it, heat shimmers over it
			float ground_y = 0.0f;
			// Clouds (with dome.on): how much of the sky they cover (0 none .. 1 overcast) and how opaque at most; a veil over
			// all of the sky (mist) in its colour; where they've drifted to (the noise's units, each pair wrapped to its
			// period: the broad shapes', then the detail's, which drifts faster, so they change as they go); their colour
			// where the sun or moon lights them and underneath
			float cloud_cover = 0.0f, cloud_thick = 0.0f, cloud_veil = 0.0f;
			float veil_colour[3] = { 0.6f, 0.62f, 0.66f };
			float cloud_drift[4] = {};
			float cloud_lit[3] = { 1.0f, 1.0f, 1.0f }, cloud_shade[3] = { 0.6f, 0.62f, 0.66f };
			// ... their slab (weather_kind_fx's bottom, top, shape, erosion and dense), how strongly the sun or moon lights them (1
			// the sun well up; less through twilight, the moon by its phase; 0 only the sky's light), and how finely they're
			// marched (0 low: 16 steps along a ray, 3 toward the light; 1: 24 and 4; 2 high: 32 and 6)
			float cloud_bottom = 250.0f, cloud_top = 400.0f, cloud_shape = 0.0f, cloud_erosion = 1.0f, cloud_dense = 1.0f, cloud_light = 1.0f;
			int cloud_quality = 1;
			// ... and their style: 0 game-like (thin, soft, stretched layers in the sky's own colours, as the game's are), 1
			// volumetric (heaps lit by the sun, dark bases, bright edges)
			int cloud_style = 0;
			// Lightning: how bright its flash is now (0 none: the scene lit cool white for a moment, the sky and clouds with
			// it), and its bolt's (0 none) with where it is: its bearing (radians from north toward east), how high it
			// starts (radians over the horizon), its shape (a number of its own)
			float flash = 0.0f, bolt = 0.0f, bolt_bearing = 0.0f, bolt_top = 0.3f, bolt_seed = 0.0f;
			// Dust or sand in the air (0 none .. 1 a storm's, up to 2), its colour as lit now, and its streaks blown along
			// the ground (0 none); the wind's own streaks (wind and gales) are these with no dust
			float dust = 0.0f, streaks = 0.0f;
			float dust_colour[3] = { 0.5f, 0.44f, 0.36f };
			// Gloom's fog banks along the ground (0 none .. 1 darkness's, up to 2), their colour as lit now, and how far
			// they've drifted with the wind (the noise's units, 48 yalms each, wrapped to its period of 256)
			float gloom = 0.0f;
			float gloom_colour[3] = { 0.3f, 0.31f, 0.33f };
			float bank_drift[2] = {};
			// Heat shimmer over distant ground (0 none .. 1 a heat wave's, up to 2)
			float heat = 0.0f;
			// An aurora's curtains in the sky (0 none .. 1, already as strong as the night makes them)
			float aurora = 0.0f;
			// The sky darker (0 as the record has it .. 1 black), the stars brighter and more of them (1 as they are)
			float sky_dark = 0.0f, star_boost = 1.0f;
			// Leaves blown along the ground (with the rain map: the share of the rain's columns with one) in this light
			float leaves = 0.0f;
			float leaf_light[3] = { 0.6f, 0.6f, 0.6f };
		} wx;
		// The look over the world as it ends, after everything else of World FX (the air pass too) and before the game's
		// interface: only at the world's end (phase all or rest), never with the blades alone. Each part off by default here
		// (the core turns them on from its settings); all off: the pass doesn't run and the world is left byte for byte.
		//   tonemap: the world's exposure following the scene (a slow auto-exposure from its mean luminance at a small size,
		//   a share of the way toward key, kept between exposure_min and exposure_max), then a filmic curve: highlights
		//   rolled off into white, a little more contrast through the middle. snap: the exposure set at once this frame (a
		//   new zone) rather than moving toward it over seconds (it's set at once too when the look's clock jumps).
		//   bloom: how much light glows around what's brighter than the threshold (0 none, 1 subtle, up to 2), from a few
		//   smaller sizes of it (a quarter of the world's and less).
		//   grade: the colour grade on (the core works it out from the hour and the weather, its strength in it already):
		//   gain (white balance, linear light), then as the screen shows it saturation (1 as is), contrast (1 as is, around
		//   the middle) and lift (the shadows tinted toward it: rgb, 0 none)
		struct look_pass
		{
			bool tonemap = false;
			float bloom = 0.0f;
			bool grade = false;
			float gain[3] = { 1.0f, 1.0f, 1.0f };
			float saturation = 1.0f, contrast = 1.0f;
			float lift[3] = {};
			float key = 0.07f, exposure_min = 0.78f, exposure_max = 1.06f;
			float threshold = 0.55f, knee = 0.25f;
			bool snap = false;
		} post;
	};

	// The look's colour grade (look::post: gain, saturation, contrast, lift, grade) for the hour and the weather: sun_up the
	// sine of the sun's height by the hour (below 0 at night), night how deep into it (0 day .. 1), sunlit whether the zone
	// has open sky (none: no grade at all, the game lights interiors and dungeons itself), w the weather (nullptr: not
	// known, the hour's alone), strength the setting (0 none, 1 subtle, up to 2). Each part's way from neutral times its
	// share now: warm at dawn and dusk, cool and a little darker and less saturated by night, grey and desaturated in rain
	// and gloom, a warm haze in heat and dust, cool in snow, flatter in mist. parts (8, optional): the shares of twilight,
	// night, rain, gloom, heat, dust, snow and mist.
	void colour_grade(float sun_up, float night, bool sunlit, const weather *w, float strength, look::look_pass &out, float *parts = nullptr);

	// The weather of its own's colours and amounts from a weather and its record (weather_fx_of, then the clouds, dust and
	// gloom lit as its record lights the zone, the veil in its fog's colour), into wx; what moves (the wind's direction,
	// the clouds' drift, the lightning, the ground's height) and the settings are the caller's
	void weather_look(const weather &w, const look::sky_dome &dome, look::weather_fx &wx);

	// The game's sky dome as it draws it (client-notes "Sky": 03D640): each band between two stops cut into this many
	// strips (one per 0.083 of height, at least one), each strip's edges coloured by the stops' bytes scaled and cut to
	// whole bytes. The colour it gives along a direction (the game's world: x east, y down, z north; bytes): Gouraud
	// across the facet of the ring of segments that direction meets, the clear colour below the horizon.
	uint32_t dome_strips(float h0, float h1);
	void dome_colour(const look::sky_dome &d, const float dir[3], float out[3]);
	// The add-on's own gradient for a height (0 the horizon .. 1 overhead; bytes): through every stop's colour at its
	// height, smooth between them (monotone cubic: no overshoot), as the shading pass draws it
	void sky_slopes(const look::sky_dome &d, float out[8][3]);
	void sky_gradient(const look::sky_dome &d, float height, float out[3]);

	// What a root parameter was last set to by the game
	struct root_arg
	{
		enum kind_t : uint8_t { unset, table, root_constants, cbv, srv, uav };
		kind_t kind = unset;
		uint64_t value = 0;               // table: GPU descriptor handle; cbv, srv, uav: GPU address
		std::vector<uint32_t> constants;  // constants: values by offset
		std::vector<uint8_t> constant_set; // which of them were set
	};

	// The game's graphics state on one command list, as far as drawing grass changes it
	struct list_state
	{
		D3D12_CPU_DESCRIPTOR_HANDLE rtvs[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		UINT rtv_count = 0;
		D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
		ID3D12PipelineState *pipeline = nullptr;
		D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
		D3D12_VIEWPORT viewports[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		UINT viewport_count = 0;
		D3D12_RECT scissors[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		UINT scissor_count = 0;
		ID3D12RootSignature *root = nullptr;
		std::vector<root_arg> args;

		void clear();
		void set_root(ID3D12RootSignature *signature);
		root_arg &arg(uint32_t param);
		void set_constants(uint32_t param, uint32_t first, uint32_t count, const uint32_t *values);
		// Binds all of it again on the list
		void restore(ID3D12GraphicsCommandList *list) const;
	};

	struct draw_stats
	{
		uint32_t blades = 0;
		uint32_t draws = 0;
		bool shaded = false; // the shading pass ran
		bool sun = false;    // with the sun's shadows (their half-size pass ran before it)
		bool sun_map = false;           // ... and the shadow map read in it
		uint32_t sun_map_cascades = 0;  // cascades of the shadow map drawn again this frame (bits: 1 near, 2 far)
		float sun_map_fade[2] = { 1.0f, 1.0f }; // how far each cascade's last drawing has faded in over the one before
		uint32_t sun_map_triangles = 0; // triangles drawn into them
		bool casters = false;           // the host's casters drew into their map, read with it (set_casters)
		bool sun_map_host = false;      // the cascades are the host's zone casters' (set_zone_casters), not the uploaded meshes'
		uint32_t water_corners = 0; // of the water drawn (the water, or the debug view)
		uint32_t sea_corners = 0;   // of the sea's sheets drawn
		uint32_t fireflies = 0;     // firefly homes drawn (most of them dark or empty at any moment)
		uint32_t lamps = 0;         // lamps whose glow the shading pass added
		bool wet = false;           // the wet look on path and stone in the shading pass
		uint32_t rain_drops = 0;    // raindrops drawn (instances: most of them dead or out of view at any moment)
		uint32_t splashes = 0;      // splash places drawn (instances: a splash shows at each for a moment)
		bool rain_map_moved = false; // the rain map's window around the camera was written again this frame
		uint32_t snow_flakes = 0;   // snowflake places drawn (instances, as the drops)
		bool sky = false;           // the sky of its own in the shading pass
		bool weather = false;       // the weather of its own's parameters went to the passes (look::wx)
		bool shimmer = false;       // heat shimmer in the shading pass
		bool air = false;           // the air pass at the end (dust, streaks, fog banks) or the lightning's flash after it
		bool clouds = false;        // the clouds' own pass ran (cloud_ps, at a quarter of the world's size)
		bool clouds_carried = false; // ... carrying the last frame's picture over (false: every pixel of it marched)
		bool cloud_shadows = false; // ... and their shadows' map made, for the shading pass
		uint32_t leaves = 0;        // leaf places drawn (instances, as the snow's)
		bool wear = false;          // the grass's wear (trodden down, cut, growing back) in the blades
		uint32_t cuts = 0;          // weapon-skill cuts sent to the blades this frame
		uint32_t vis_boxes = 0;     // the game's visibility tests answered from the depth copy this frame (vistest)
		bool look = false;          // the look pass ran (look::post: tone mapping, bloom, the grade)
		uint32_t look_levels = 0;   // ... with this many smaller sizes of the world (the bloom's and the exposure's)
		bool bound = false;  // the renderer bound its own pipeline, root signature, targets and viewport in the list:
		                     // the caller puts the game's back (D3D12 doesn't by itself)
	};

	// The world's target and depth buffer at the moment grass is drawn into the target. The depth buffer is
	// only copied, its depth plane alone (the game may keep its stencil plane in another state, and ReShade
	// doesn't say which plane a barrier was for): the blades test against that copy, and have a depth buffer
	// of their own for each other.
	struct target
	{
		ID3D12Resource *rt = nullptr;
		ID3D12Resource *ds = nullptr;
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
		DXGI_FORMAT rtv_format = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT dsv_format = DXGI_FORMAT_UNKNOWN; // (the game's, for the log)
		D3D12_RESOURCE_STATES rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		D3D12_RESOURCE_STATES ds_state = D3D12_RESOURCE_STATE_DEPTH_WRITE; // (of its depth plane)
		// look::phase_t::blades: the blades drawn with ds itself as their depth (both planes DEPTH_WRITE meanwhile: the
		// caller's), through a view of the renderer's own
		bool blades_into_world_depth = false;
	};

	// The shaders compiled from a source: a blob for each of the renderer's entry points (vs_main, ps_main, shade_vs ...
	// snow_ps: grass_render.cpp's kShaderEntries, in its order), and the source's key (shader_key)
	struct shader_code
	{
		std::vector<std::vector<uint8_t>> blobs;
		uint64_t key = 0;
	};
	// A shader source's key: a hash of it, of the compiler (d3dcompiler_47.dll's build) and of how it's compiled.
	// 0 when there's no compiler.
	uint64_t shader_key(const std::string &source);
	// Compiles a source's shaders, on any thread (it uses nothing of a renderer's). With a cache folder, they're first
	// looked for there (a file named for the key, its contents checked), and written there once compiled; the
	// kShaderCacheKeep files used last are kept, older ones deleted. from_cache says where they came from. False, with
	// the compiler's messages in error, when the source doesn't compile (nothing is written then).
	constexpr size_t kShaderCacheKeep = 4;
	bool compile_shaders(const std::string &source, const std::wstring &cache_folder, shader_code &out, bool &from_cache, std::string &error);
	// The built-in shaders' source (src/render/grass.hlsl, as the engine was built with it)
	const std::string &built_in_shader_source();

	class renderer
	{
	public:
		~renderer();

		// Compiles the built-in shaders and makes the root signature (once per device). With a cache folder the
		// shaders come from there when it has them (compile_shaders); shaders_from_cache and shader_seconds then say
		// how they came and how long it took.
		bool init(ID3D12Device *device, std::string &error, const std::wstring &shader_cache = std::wstring());
		bool shaders_from_cache() const { return _shaders_from_cache; }
		double shader_seconds() const { return _shader_seconds; }
		uint64_t shader_key() const { return _shader_key; } // (the source of the shaders in use)
		bool ready() const { return _root != nullptr; }
		// GPU time (ms, smoothed) of the copies and the shading pass, and of the grass; 0 until known
		void set_timestamp_frequency(uint64_t ticks_per_second) { _frequency = ticks_per_second; }
		float shading_ms() const { return _shading_ms; }
		float last_shading_ms() const { return _last_shading_ms; } // (the newest frame's alone, unsmoothed: for the bench)
		float grass_ms() const { return _grass_ms; }
		// GPU time by section (each marker's name, from it to the next marker: "worldfx: water" ...), smoothed over the
		// frames it ran in, and in how many of the frames timed since the last reset it ran
		struct gpu_section
		{
			std::wstring name;
			float ms = 0.0f;
			uint32_t frames = 0;
		};
		const std::vector<gpu_section> &gpu_sections() const { return _sections; }
		uint32_t frames_timed() const { return _frames_timed; }
		void reset_timing() { _shading_ms = _grass_ms = 0.0f; _sections.clear(); _frames_timed = 0; }
		// Replaces the shaders with ones compiled from this source (the built-in ones: src/render/grass.hlsl); false
		// and the compiler's messages in error, keeping the ones in use, when it doesn't compile
		bool set_shader_source(const std::string &source, std::string &error);
		// The same with shaders compiled already (compile_shaders, on another thread: compiling all of them takes
		// seconds)
		void set_shader_code(shader_code &&code);
		// Puts the blades in a GPU buffer; the previous one is kept until release_old says it's unused
		bool upload(const blade_file &file, std::string &error);
		void unload();
		bool loaded() const { return _buffer != nullptr; }
		// The zone's water (tools/bake_water.py), for the water view; the previous one goes like the blades'
		bool upload_water(const blade_file &file, std::string &error);
		void unload_water();
		bool water_loaded() const { return _water != nullptr; }
		uint32_t water_zone() const { return _water_zone; }
		uint32_t water_corners() const { return _water_corners; }
		// The zone's sea sheets (read_sea_file), for the sea; the previous ones go like the blades'
		bool upload_sea(const blade_file &file, std::string &error);
		void unload_sea();
		bool sea_loaded() const { return _sea != nullptr; }
		uint32_t sea_corners() const { return _sea_corners; }
		// The zone's wet mask (tools/bake_wet.py), for the wet look in rain; the previous one goes like the blades'
		bool upload_wet(const wet_file &file, std::string &error);
		void unload_wet();
		bool wet_loaded() const { return _wet != nullptr; }
		// The zone's rain map (tools/bake_rain.py), kept here whole; the part around the camera (kRainWindow squares a
		// side) goes to the GPU, written again into a new buffer when the camera nears its edge (the previous one goes
		// like the blades')
		bool upload_rain(rain_file &&file, std::string &error);
		void unload_rain();
		bool rain_loaded() const { return !_rain.q.empty(); }
		uint32_t rain_zone() const { return _rain.zone; }
		// (for tests: the first surface's height and what it is at a world x, z, from the map; false off it)
		bool rain_surface(float x, float z, float &y, uint8_t &kind) const;
		// The zone's meshes for the sun's shadow map; the previous ones go like the blades'. A cascade of the map is
		// drawn from them again as soon as the sun's turn would move a long shadow by a quarter of its texel, or the
		// camera leaves its middle half: the shadows glide rather than skip.
		bool upload_shadow(const shadow_file &file, std::string &error);
		void unload_shadow();
		bool shadow_loaded() const { return _shadow_geo != nullptr; }
		uint32_t shadow_zone() const { return _shadow_zone; }
		uint32_t shadow_triangles() const { return _shadow_triangles; }
		// (for tests: the map's copy, two cascades of kSunMapSize rows of this many floats, the near one first)
		ID3D12Resource *sun_map_copy() const { return _sun_map_copy; }
		uint32_t sun_map_pitch() const { return _sun_map_pitch; }
		// Shadow casters the host draws itself (HiFi XI: the characters and monsters from their own meshes), into a map of
		// their own each frame the sun's shadow map is read: called in the shading pass with the list, the map's
		// world-to-clip matrix (the sun's rays down its depth, as the cascades'), its depth view (cleared to 1, in
		// DEPTH_WRITE) and its size in texels; it draws depth only and returns whether it drew anything. It may change
		// any of the list's state, its descriptor heaps too if it puts the game's back. nullptr: none.
		using caster_fn = bool (*)(void *context, ID3D12GraphicsCommandList *list, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size);
		void set_casters(caster_fn fn, void *context)
		{
			_casters = fn;
			_casters_context = context;
		}
		// The zone's casters the host draws itself (HiFi XI: its native zone data), in place of the uploaded meshes
		// (upload_shadow) whenever bounds says it has them: the box around what it draws (world) and a number that
		// changes with its data (the cascades are drawn again when it does), then draw into a cascade as caster_fn does
		// (world_to_clip, its view cleared to 1, its size), returning the triangles drawn. nullptr: none.
		using zone_bounds_fn = bool (*)(void *context, float lo[3], float hi[3], uint64_t *generation);
		using zone_draw_fn = uint32_t (*)(void *context, ID3D12GraphicsCommandList *list, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size);
		void set_zone_casters(zone_bounds_fn bounds, zone_draw_fn draw, void *context)
		{
			_zone_bounds = bounds;
			_zone_draw = draw;
			_zone_casters_context = context;
		}
		// Forgets the clouds' last picture: the next frame marches every pixel of it again (as after a weather jump)
		void forget_clouds() { _cloud_known = false; }
		// Forgets the ground colours seen so far (each blade point remembers its own, see the shader)
		bool forget_colours(std::string &error);
		uint32_t zone() const { return _zone; }
		uint32_t blade_count() const { return _blade_count; }
		// Called once a frame: buffers replaced this many frames ago are released
		void release_old(uint32_t frames_to_keep = 30);

		// The game's visibility tests (vistest.hpp): the boxes taken this frame, tested against the depth copy in the
		// next draw. stamp: a count that grows by one a frame (the presents so far), for read_vis. False when every
		// slot is still the GPU's (the boxes are dropped: the game asks again next frame).
		bool set_vis_boxes(const uint32_t *objects, const vistest::box *boxes, uint32_t n, uint64_t stamp, std::string &error);
		// The points passed in the oldest slot the GPU is done with (its stamp + 2 <= done, done being the stamps the
		// GPU has finished), each slot read once: the test objects and their counts (0..vistest::kPoints). Returns how
		// many; 0 when no slot is ready.
		uint32_t read_vis(uint64_t done, uint32_t objects[vistest::kMaxBoxes], uint32_t points[vistest::kMaxBoxes]);

		// Draws the blades near the camera into the target, leaving the target in its given states.
		// The list's root signature, arguments, pipeline, topology, targets, viewport and scissor are
		// left set to grass's: restore the game's afterwards (list_state::restore).
		draw_stats draw(ID3D12GraphicsCommandList *list, const target &t, const camera &cam, const look &l, std::string &error);

	private:
		ID3D12PipelineState *pipeline_for(const target &t, std::string &error);
		struct copy;
		bool copy_buffer(copy &c, UINT64 size, std::string &error);
		void finish_timing(ID3D12GraphicsCommandList *list, bool shaded);
		ID3D12PipelineState *shade_pipeline_for(const target &t, std::string &error);
		ID3D12PipelineState *sun_pipeline(std::string &error);
		bool sun_mask_for(const D3D12_RESOURCE_DESC &rt_desc, UINT64 clouds, std::string &error);
		ID3D12PipelineState *cloud_pipeline(std::string &error);
		bool cloud_noise_for(std::string &error);
		// The water's pipelines: the water alone (a zone without a sea), the debug view, and with a sea the baked water's
		// whole pixels (marking them in the stencil), the sea where they aren't, the baked water's fading pixels over that
		enum class water_kind { plain, debug, whole, sea, fading };
		ID3D12PipelineState *water_pipeline_for(const target &t, water_kind kind, std::string &error);
		ID3D12PipelineState *firefly_pipeline_for(const target &t, std::string &error);
		ID3D12PipelineState *air_pipeline_for(const target &t, bool flash, std::string &error);
		// The look pass (look::post) over the world, its target in RENDER_TARGET before and after; false when it didn't run
		bool draw_look(ID3D12GraphicsCommandList *list, const target &t, const look &l, draw_stats &stats, std::string &error);
		ID3D12PipelineState *look_pipeline_for(const target &t, std::string &error);
		enum class fall { rain, splash, snow, leaf };
		ID3D12PipelineState *rain_pipeline_for(const target &t, fall kind, std::string &error);
		bool rain_window_for(const float eye[3], bool &moved, std::string &error);
		uint32_t copy_out(ID3D12GraphicsCommandList *list, copy &c, ID3D12Resource *res, D3D12_RESOURCE_STATES state);
		bool blade_depth_for(const D3D12_RESOURCE_DESC &rt_desc, std::string &error);
		bool world_rtv_for(const target &t, D3D12_CPU_DESCRIPTOR_HANDLE &rtv, std::string &error);
		bool world_dsv_for(const target &t, D3D12_CPU_DESCRIPTOR_HANDLE &dsv, std::string &error);
		bool sun_map_for(std::string &error);
		ID3D12PipelineState *shadow_pipeline(std::string &error);
		uint32_t draw_sun_map(ID3D12GraphicsCommandList *list, const float eye[3], const float sun[3], double clock, uint32_t &triangles, std::string &error);
		void sun_map_params(const camera &cam, double clock, float out[32], float prev[32]) const;
		bool draw_casters(ID3D12GraphicsCommandList *list, const float eye[3], const float sun[3]);
		void caster_params(const camera &cam, float out[16]) const;
		ID3D12PipelineState *vis_pipeline(std::string &error);
		bool vis_buffers(std::string &error);
		uint32_t draw_vis(ID3D12GraphicsCommandList *list, const float *constants, std::string &error);

		ID3D12Device *_device = nullptr;
		ID3D12RootSignature *_root = nullptr;
		std::vector<uint8_t> _vs, _ps;
		struct pso_entry
		{
			DXGI_FORMAT rtv, dsv;
			DXGI_SAMPLE_DESC samples;
			ID3D12PipelineState *pso;
		};
		std::vector<pso_entry> _psos, _shade_psos, _water_psos, _water_debug_psos, _firefly_psos, _rain_psos, _splash_psos, _snow_psos, _leaf_psos, _air_psos, _flash_psos;
		std::vector<pso_entry> _water_whole_psos, _sea_psos, _water_fading_psos;
		std::vector<uint8_t> _shade_vs, _shade_ps, _water_vs, _water_ps, _water_debug_vs, _water_debug_ps, _sun_ps, _shadow_vs, _shadow_ps, _firefly_vs, _firefly_ps;
		std::vector<uint8_t> _rain_vs, _rain_ps, _splash_vs, _splash_ps, _snow_vs, _snow_ps, _leaf_vs, _leaf_ps, _air_ps, _flash_ps, _cloud_ps, _cloud_shadow_ps;
		// The look pass: its shaders, its pipelines (the small sizes' with no render target, made once; the world's by its
		// format), its buffer (the exposure at its start, then the levels: kLookExposureBytes on) and the look's clock at
		// its last frame (how far the exposure moves)
		std::vector<uint8_t> _look_first_ps, _look_down_ps, _look_exposure_ps, _look_up_ps, _look_ps;
		ID3D12PipelineState *_look_first_pso = nullptr, *_look_down_pso = nullptr, *_look_exposure_pso = nullptr, *_look_up_pso = nullptr;
		std::vector<pso_entry> _look_psos;
		ID3D12Resource *_look_buf = nullptr;
		UINT64 _look_buf_size = 0;
		double _look_clock = 0.0;
		uint64_t _shader_key = 0;
		bool _shaders_from_cache = false;
		double _shader_seconds = 0.0;
		// The rain map (whole, on the CPU) and its window around the camera (an upload buffer, a uint a square: the
		// surface's y as float bits with what it is in the low 2 bits), where that window's corner square is
		rain_file _rain;
		ID3D12Resource *_rain_window = nullptr;
		int32_t _rain_window_i = 0, _rain_window_j = 0;
		ID3D12PipelineState *_sun_pso = nullptr; // the sun's shadows at half size (no render target: it writes _sun_mask)
		ID3D12Resource *_sun_mask = nullptr;     // what it works out, a uint for each of the world's pixels in four (UAV)
		UINT64 _sun_mask_size = 0;
		// The clouds (cloud_ps, at a quarter of the world's width and height, no render target): their two pictures after
		// the sun's mask in _sun_mask (this frame's and the last's, in turn), their noise (default heap, from its upload
		// copy at the first draw with clouds), and what carrying the last picture over needs: whether there is one that
		// fits (its size, the buffer, the slab close to this frame's), the last frame's view rows and projection, which
		// picture is the last's, and the frames marched so far (the 2 x 2 order's turn, the jitter)
		ID3D12PipelineState *_cloud_pso = nullptr, *_cloud_shadow_pso = nullptr;
		ID3D12Resource *_cloud_noise = nullptr, *_cloud_noise_staging = nullptr;
		bool _cloud_known = false;
		UINT _cloud_w = 0, _cloud_h = 0;
		ID3D12Resource *_cloud_buffer = nullptr;
		float _cloud_key[5] = {};
		float _cloud_view[16] = {}, _cloud_proj[2] = {};
		uint32_t _cloud_picture = 0, _cloud_frames = 0;

		// The sun's shadow map: two cascades side by side in one D32 texture (each kSunMapSize square, around the
		// camera, the near one kSunMapReach[0] yalms each way, the far one kSunMapReach[1]), drawn from the zone's
		// meshes looking down the sun's rays, and copied into a buffer the sun pass reads (t0: root SRVs only take
		// buffers, and the add-on binds no descriptor heap of its own over the game's)
		ID3D12Resource *_shadow_geo = nullptr;     // the triangles, 48 bytes each (default heap)
		ID3D12Resource *_shadow_staging = nullptr; // ... on their way there (upload heap; copied at the next draw)
		ID3D12Resource *_shadow_masks = nullptr;   // the cut-out textures' alpha (upload heap): 4 words a mask, then the bits
		uint32_t _shadow_zone = 0, _shadow_triangles = 0;
		float _shadow_cell_size = 32.0f;
		std::vector<blade_file::cell> _shadow_cells;
		std::vector<float> _shadow_bounds; // each cell's triangles: the least x, y, z, then the most
		float _shadow_lo[3] = {}, _shadow_hi[3] = {}; // all of them
		ID3D12Resource *_sun_map = nullptr;           // the map (D32, DEPTH_WRITE between draws)
		ID3D12DescriptorHeap *_sun_map_heap = nullptr; // its view
		ID3D12Resource *_sun_map_copy = nullptr;      // its copy, a float a texel, rows of _sun_map_pitch uints
		D3D12_RESOURCE_STATES _sun_map_copy_state = D3D12_RESOURCE_STATE_COMMON;
		UINT _sun_map_pitch = 0;
		UINT64 _sun_map_half = 0;        // (bytes from the copy's first half to its second: each a drawing of both cascades)
		UINT64 _sun_map_offset[2] = {};  // (where each cascade starts within a half)
		ID3D12PipelineState *_shadow_pso = nullptr;
		struct cascade
		{
			bool valid = false;
			float sun[3] = {};    // toward the sun when drawn
			float centre[2] = {}; // where it's centred across the sun's rays (yalms, on its texel grid)
			float world_to_clip[16] = {};
			// The drawing before the last, which the last fades in over (with the look's clock), kept in the copy's other
			// half: whether there is one, its matrix, when the last was drawn (the look's clock), and which half the last is in
			bool prev_valid = false;
			float prev_world_to_clip[16] = {};
			double drawn_at = 0.0;
			uint32_t slot = 0;
			void forget() { valid = prev_valid = false; }
		};
		cascade _cascades[2];
		uint32_t _sun_map_turn = 0; // (counts the map's frames: the far cascade's turn on every other one)
		// The host's casters (set_casters): their map (D32, kSunMapSize square, DEPTH_WRITE between draws; its view the
		// sun map heap's third), copied after the sun map's two halves in its copy; its matrix this frame, whether drawn
		caster_fn _casters = nullptr;
		void *_casters_context = nullptr;
		// The host's zone casters (set_zone_casters), and what the cascades were last drawn from: its data's number, or
		// the uploaded meshes' (kFromMeshes), and the box the map's depth spans (the zone's)
		zone_bounds_fn _zone_bounds = nullptr;
		zone_draw_fn _zone_draw = nullptr;
		void *_zone_casters_context = nullptr;
		uint64_t _map_source = 0;
		float _map_lo[3] = {}, _map_hi[3] = {};
		bool _map_bounds = false;
		bool _map_host = false;
		ID3D12Resource *_caster_map = nullptr;
		float _caster_world_to_clip[16] = {};

		ID3D12Resource *_buffer = nullptr;
		ID3D12Resource *_memory = nullptr; // the ground colour each blade point remembers (12 bytes a point)
		bool _memory_fresh = false;         // zeroed at the next draw
		// (the look's clock when the memory was zeroed, the wear's times count from it; and at the last draw)
		double _memory_epoch = 0.0, _last_clock = 0.0;
		ID3D12Resource *_zeros = nullptr;  // 1 MB of zeros (upload heap), for that
		ID3D12Resource *_blade_depth = nullptr;     // the blades' own depth (D24S8, the world target's size, in DEPTH_WRITE;
		                                            // the water pass marks the baked water's pixels in its stencil)
		ID3D12DescriptorHeap *_dsv_heap = nullptr;  // its view
		ID3D12DescriptorHeap *_rtv_heap = nullptr;  // the add-on's own view of the world's target (made each draw)
		ID3D12DescriptorHeap *_world_dsv_heap = nullptr; // ... and of its depth, for the blades drawn into it (look::phase_t::blades)
		static constexpr UINT kTimingSlots = 8, kTimingMarks = 32; // (two draws a frame with the blades at the zone's end)
		ID3D12QueryHeap *_queries = nullptr; // timestamps: one at each section's marker and one at the end; a frame's in each slot in turn
		ID3D12Resource *_times = nullptr;    // where they're resolved (readback heap)
		UINT _time_slot = 0, _timed = 0;
		uint64_t _frequency = 0;
		float _shading_ms = 0.0f, _grass_ms = 0.0f, _last_shading_ms = 0.0f;
		// Each slot's markers: their names (nullptr: the end), how many, where the copies and shading part ended
		// (kTimingMarks: it didn't: all of it was)
		const wchar_t *_mark_names[kTimingSlots][kTimingMarks] = {};
		UINT _mark_count[kTimingSlots] = {}, _shade_end[kTimingSlots] = {};
		std::vector<gpu_section> _sections;
		uint32_t _frames_timed = 0;
		// A section starting: the GPU crash report's marker, and a timestamp
		void section(ID3D12GraphicsCommandList *list, const wchar_t *name);
		static constexpr UINT kActorSlots = 8; // upload buffers for the actors, used in turn (frames in flight; two draws a frame)
		ID3D12Resource *_actors[kActorSlots] = {}; // the actors near, a draw's in each in turn (upload heaps, mapped)
		float *_actor_data[kActorSlots] = {};
		uint32_t _actor_slot = 0;
		ID3D12Resource *_water_params = nullptr; // the water pass's parameters, a frame's in each 256 bytes in turn (upload heap, mapped)
		uint8_t *_water_param_data = nullptr;
		uint32_t _water_param_slot = 0;
		uint32_t _zone = 0;
		uint32_t _blade_count = 0;
		float _cell_size = 16.0f;
		std::vector<blade_file::cell> _cells;
		ID3D12Resource *_wet = nullptr;    // the zone's wet mask's words (upload heap), and where it lies
		float _wet_x0 = 0.0f, _wet_z0 = 0.0f, _wet_square = 0.5f;
		uint32_t _wet_width = 0, _wet_height = 0, _wet_words_a_row = 0;
		ID3D12Resource *_water = nullptr;  // the zone's water corners, 32 bytes each (upload heap)
		uint32_t _water_zone = 0, _water_corners = 0;
		ID3D12Resource *_sea = nullptr;    // the zone's sea sheets' corners, the same (upload heap)
		uint32_t _sea_corners = 0;
		float _sea_box[4] = {};            // its corners' x and z, least and most: none drawn while it's all past the water's reach
		float _water_cell_size = 32.0f;
		std::vector<blade_file::cell> _water_cells;
		struct copy
		{
			ID3D12Resource *buffer = nullptr;
			UINT64 size = 0;
			D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
		};
		copy _ground, _depth; // the world as drawn, and its depth
		// The game's visibility tests: each frame's boxes in a slot of _vis_boxes (upload heap, mapped: kVisSlots x
		// kMaxBoxes x 32 bytes), tested by the vis pass into _vis_points (a uint a point, 0 or 1: UAV), which is
		// copied to the slot's part of _vis_read (readback heap) and read once the GPU is done with it.
		static constexpr UINT kVisSlots = 4;
		struct vis_slot
		{
			enum class state_t { empty, set, recorded } state = state_t::empty;
			uint32_t n = 0;
			uint64_t stamp = 0;
			uint32_t objects[vistest::kMaxBoxes] = {};
		};
		vis_slot _vis_slots[kVisSlots];
		ID3D12Resource *_vis_boxes = nullptr;
		uint8_t *_vis_box_data = nullptr;
		ID3D12Resource *_vis_points = nullptr;
		ID3D12Resource *_vis_read = nullptr;
		ID3D12PipelineState *_vis_pso = nullptr;
		std::vector<uint8_t> _vis_ps;
		struct old_buffer
		{
			ID3D12DeviceChild *buffer; // (a buffer, or a pipeline)
			uint32_t frames;
		};
		std::vector<old_buffer> _old;
	};

	// The clouds' noise as the GPU reads it (grass.hlsl cloud_tex): the broad one, 64 texels a side (a Perlin-Worley mix:
	// billowing masses), then the fine one, 32 a side (Worley: what eats away their edges), both repeating; a word a texel
	// (y, z, x order), holding its byte and its neighbours' along x, z and both. Made once (the same every time).
	const std::vector<uint32_t> &cloud_noise();

	// Matrix helpers (row vectors, D3D layout), shared with the test
	void multiply(const float a[16], const float b[16], float out[16]);
	void eye_from_view(const float view[16], float eye[3]);
}
