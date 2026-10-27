/*
 * World FX's core (worldfx_core.hpp): grass for FFXI's world and all that's drawn with it (Direct3D 12 only), for
 * whichever host feeds it the events (the World FX ReShade add-on, HiFi XI's plugin).
 *
 * dgVoodoo turns the game's Direct3D 8 into Direct3D 12 command lists. This follows what dgVoodoo binds
 * on each list (the host's events: targets, pipeline, topology, viewports, scissors, root signature and
 * every root argument) and once a frame draws grass (grass_render) into the world's render target and
 * depth buffer with plain Direct3D 12 calls, then binds dgVoodoo's state again. The host never sees
 * those calls as dgVoodoo's, so its own idea of what is bound stays right.
 *
 * Which target: the world, as for the effects: the one that took the most vertices last frame with a
 * depth buffer bound. When: the first time this frame dgVoodoo moves off it (or closes the list while
 * on it) after 40% of last frame's world vertices have gone in since its depth was cleared. The zone's
 * meshes come first in a frame (client-notes "Frame anatomy"), so they're in by then, and whatever is
 * drawn later is depth-tested against the blades.
 *
 * Camera: the game's own (gGame +0x94 view, +0x154 projection, found in FFXiMain.dll by facelift's
 * 'game.sys' pattern). Zone: the worldfx Ashita addon puts it in the process environment
 * (FFXI_WORLDFX_ZONE); blades for zone N are read from worldfx-grass\N.bin in the host's folder.
 *
 * Lamps: the zone's from worldfx-lamps\N.bin (tools/bake_lamps.py); which are lit now, from the game's light
 * table, which the Ashita addon reads and sends (worldfx_lamps). Their glow goes in with the shading pass.
 * Glows: lights for glowing things the game lights nothing with (Home Point crystals and such, and its additive
 * effects), worked out by the Ashita addon and sent (worldfx_glows); they glow as lamps do.
 *
 * Weather: the game's, as the Ashita addon (or HiFi XI's game side) reads and sends it (worldfx_weather). World FX draws
 * rain and snow of its own; and, with its sky on under open sky (weather_all), all of the zone's weather (look::wx:
 * clouds, lightning, dust and sand, wind, heat shimmer, auroras, gloom, a starry sky), so that HiFi XI's game side can
 * leave all of the game's weather out, its sky layers too (worldfx_weather_drawn says what's World FX's own).
 */

#include "worldfx_core.hpp"
#include "gpu_crash.hpp"
#include "vistest.hpp"
#include "lamp_glow.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	constexpr const char *kZoneVariable = "FFXI_WORLDFX_ZONE";
	// The zone's files to be read again (worldfx::reload_zone: HiFi XI just made them)
	std::atomic<bool> g_reload_zone{ false };
	// The game's world found since its device was attached: World FX draws there (worldfx_drawing: HiFi XI's game side
	// leaves the game's own water, rain... out only then; never on another output, DXVK or dgVoodoo's Direct3D 11)
	std::atomic<bool> g_has_drawn{ false };
	// A frame whose busiest depth-tested target took fewer vertices than this has no world in it
	constexpr uint64_t kMinWorldVertices = 10000;
	// Grass goes in at the world's end: the barrier that makes it readable (on_barrier). Without one for a
	// while (kBarrierFrames), it goes in at the first move off the world once as much of it is drawn as the
	// smallest recent frame's, or at a move to a target of another size (the resolve) past kResolveShare.
	constexpr uint32_t kBarrierFrames = 60;
	// At most kMaxPasses drawings between two presents: a full-screen pass at twice the screen's size many
	// times in one frame is enough to make the GPU time out
	constexpr uint32_t kMaxPasses = 2;
	// Nothing is drawn unless the worldfx Ashita addon says a character is in the game (FFXI_WORLDFX_INGAME
	// = 1: logged in, not zoning), from the first frame it does. The shading pass (ambient occlusion, relief,
	// sun shadows) pauses kShadePauseMs whenever the GPU timer says it took more than kShadeMaxMs
	// (TestShadeMaxMs in ReShade.ini: another limit, for the end-to-end test on WARP, a CPU many times slower
	// than the GPU). (Until 2026-10-17 everything also waited 60 frames in the field, and the shading pass
	// 3 s more, against GPU hangs at zone-in; those were the add-on leaving its own state bound after a
	// shading-only pass and the NVIDIA driver's forced anisotropic filtering, and happened with both waits.)
	constexpr const char *kInGameVariable = "FFXI_WORLDFX_INGAME";
	constexpr ULONGLONG kShadePauseMs = 5000;
	constexpr float kShadeMaxMs = 30.0f;
	constexpr double kResolveShare = 0.4;
	// The first frame of moves off the world goes to ReShade.log after this many frames with grass
	constexpr uint32_t kLogMovesAfter = 300;
	// Frames whose world vertex counts the share is measured against (the smallest)
	constexpr uint32_t kRecent = 8;
	constexpr uint32_t kZoneCheckFrames = 30;
	constexpr uint32_t kHistory = 120;

	// What dgVoodoo has bound on one command list (worldfx_core.hpp)
	using worldfx::list_data;

	struct target_use
	{
		ID3D12Resource *rt, *ds;
		uint64_t vertices;
	};

	// worldfx.hlsl compiled on a thread of its own (shader_thread): compiling all the shaders takes over 11 s, and done
	// on the game's frame it froze the game until Windows closed it (in game, 2026-10-21). The frame picks the result
	// up once done is set (check_shaders).
	struct shader_job
	{
		std::string source;
		std::wstring cache; // (the shader cache folder)
		HMODULE module = nullptr; // (the add-on, held while the thread runs: it can't be unloaded under it)
		grass::shader_code code;
		bool ok = false, from_cache = false;
		std::string error;
		double seconds = 0.0;
		std::atomic<bool> done { false };
	};

	struct addon_state
	{
		std::mutex mutex;
		ID3D12Device *dev = nullptr; // the game window's Direct3D 12 device
		std::unique_ptr<grass::renderer> renderer;
		std::string error; // the renderer's, which turns grass off
		// The host's shadow casters (set_casters), given to each renderer made
		grass::renderer::caster_fn casters = nullptr;
		void *casters_context = nullptr;
		grass::renderer::zone_bounds_fn zone_bounds = nullptr;
		grass::renderer::zone_draw_fn zone_draw = nullptr;
		void *zone_casters_context = nullptr;

		// Settings ([FFXI_WORLDFX] in ReShade.ini, through the host)
		bool enabled = true;
		grass::look look;

		// One frame's moves off the world, for the log
		uint32_t frames_with_grass = 0;
		bool logging_moves = false, logged_moves = false;
		uint32_t logged_frames = 0;
		std::string moves;

		// The first draw logged (with dgVoodoo's root signature)
		bool logged_first = false;

		// Learnt from earlier frames
		ID3D12Resource *world_rt = nullptr;
		ID3D12Resource *world_ds = nullptr;
		// The world as the game side's zone marker names it (on_world_marker): its depth, the target bound with it there;
		// at which present the last one came, how many since the start. While they come, they decide the world, not the
		// vertex counts (with the game's zone off, its UI outweighs what's left in the world).
		ID3D12Resource *marker_rt = nullptr;
		ID3D12Resource *marker_ds = nullptr;
		uint64_t marker_present = 0; // (g.presents then)
		uint64_t markers = 0;
		bool marker_logged = false;
		uint64_t world_vertices_last = 0; // the smallest of the recent frames' (what the share is of)
		uint64_t recent[8] = {};
		uint32_t recent_pos = 0, recent_count = 0;
		bool world_cleared = false; // the world started over (a clear) since the last present
		bool color_clears = false;  // the world's colour clear has been seen (else its depth clear is the boundary)

		// This frame
		std::vector<target_use> uses;
		uint64_t world_vertices = 0; // into the world since its depth was cleared
		bool tried = false;
		bool drew = false;

		// Zone and blades
		uint32_t frames = 0;
		uint32_t last_resolve_frame = 0;      // the frame grass last went in at the world's end (its read barrier)
		// The world's views as last bound, for grass at its end on a list that has moved off it already
		uint64_t world_rtv = 0, world_dsv = 0;
		// The states the last barrier (on any list) put the world's target and depth in
		D3D12_RESOURCE_STATES last_rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		D3D12_RESOURCE_STATES last_ds_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		bool zone_known = false;
		uint32_t zone = 0;
		bool zone_tried = false;
		std::string blades_status = "-";
		std::string water_status = "-";   // the zone's water file (worldfx-water\<zone>.bin)
		std::string sea_status = "-";     // the zone's sea sheets (worldfx-sea\<zone>.bin: HiFi XI's bake 'sea')
		std::string shadow_status = "-";  // the zone's meshes for the sun's shadow map (worldfx-shadow\<zone>.bin)
		uint32_t sun_map_redraws = 0;     // times the shadow map was drawn again (since the zone loaded)

		// Safeguard (kMaxPasses)
		uint32_t passes = 0;          // world ends drawn at since the last present
		uint32_t passes_wanted = 0;   // and wanted
		uint32_t most_wanted = 0;     // the most wanted between two presents so far (logged when it grows past kMaxPasses)
		bool logged_stale_views = false;
		bool crash_reports_tried = false;
		std::string crash_reports = "off (Direct3D 12's device was made before the add-on loaded)";
		bool debug_layer = false;         // D3D12DebugLayer=1: its messages go to worldfx-d3d12-debug.txt
		std::string debug_layer_status = "off (D3D12DebugLayer=1 turns it on, for a session)";
		bool shading_logged = false;      // "shading pass on" logged since the game last said in game

		// The last drawings at the world's end, for the crash report
		struct drawing
		{
			ULONGLONG at = 0;             // (GetTickCount64)
			uint32_t frame = 0;
			uint32_t blades = 0, draws = 0;
			bool shaded = false, held = false, bound = false;
			uint32_t rt_state = 0, ds_state = 0;
		};
		static constexpr uint32_t kDrawings = 8;
		drawing drawings[kDrawings];
		uint32_t drawing_count = 0;
		bool in_game = false;             // the Ashita addon says so (FFXI_WORLDFX_INGAME)
		ULONGLONG in_game_since = 0;      // (GetTickCount64)
		ULONGLONG shading_paused_until = 0;
		const char *shading_held = nullptr; // why the shading pass isn't running now (nullptr: it is)

		// Frame pacing, for the crash report and the panel: a fence of ours signalled on the presenting queue at
		// each present, so how many presents the GPU hadn't finished yet (how far behind the CPU it runs) is
		// known at the next one; and the time between presents
		ID3D12Device *pace_device = nullptr;
		ID3D12Fence *pace_fence = nullptr;
		uint64_t presents = 0;
		LARGE_INTEGER last_present = {};
		struct pace
		{
			ULONGLONG at = 0;   // (GetTickCount64)
			uint64_t present = 0;
			uint32_t behind = 0;  // presents the GPU hadn't finished then
			float ms = 0.0f;      // since the present before
		};
		static constexpr uint32_t kPaces = 16;
		pace paces[kPaces];
		uint32_t pace_count = 0;
		uint32_t most_behind = 0; // since the game last said a character is in

		// The game's visibility tests answered from the depth copy (vistest.hpp): on while the worldfx Ashita addon has
		// pointed the game's calls at the entries below (worldfx_vis_setup); boxes the renderer had no free slot for
		bool vis_on = false;
		bool vis_option = true; // VisTest (HiFi XI's game side reads it: on by default; off, the game's own test runs)
		uint64_t vis_skipped = 0;
		uint32_t vis_objects[vistest::kMaxBoxes] = {};
		uint32_t vis_points[vistest::kMaxBoxes] = {};
		vistest::box vis_boxes[vistest::kMaxBoxes] = {};

		// Sun shadows: how dark (the setting), and whether this zone has open sky (worldfx-sunlit.txt next to
		// the add-on, from tools/sunlit_zones.py; without the file, every zone has)
		float sun_shadows = 0.0f; // (off until its cost is known in game)
		float moon_shadows = 0.6f; // by night, the moon's: this share of the sun's strength (0 off)
		// The look over the world as it ends (look::post): tone mapping (with its exposure following the scene), the bloom's
		// strength (0 none) and the colour grade's (0 none: by the hour and the weather); the zone the exposure was last
		// set in at once (a new zone sets it again at once)
		bool tonemap = true;
		float bloom = 1.0f, colour_grade = 1.0f;
		uint32_t look_zone = 0xFFFFFFFF;
		// What the grade is made of now, for the status: the share of twilight, night, rain, gloom, heat, dust, snow and mist
		float grade_parts[8] = {};
		bool moon_now = false;     // (the moon's shadows drawn this frame: for the panel)
		bool zone_sunlit = true;
		// Where the sun is: as the game lights the world (its environment object: the light's direction at
		// +0x14C, client-notes "Lighting"), so the shadows fall the way its lighting and its own character
		// shadow say; from the hour when that can't be read or makes no sense
		uintptr_t env_global = 0; // the global holding the environment object (0: not found)
		bool env_scanned = false;
		bool sun_from_game = false;
		float game_sun[3] = {}, hour_sun[3] = {}; // toward the sun, each way (the game's world)
		ULONGLONG sun_logged_at = 0;
		float test_hour = -1.0f; // (a fixed Vana'diel hour for tests; -1: the real one)
		float test_shade_max_ms = 0.0f; // (the shading pass's time limit for tests; 0: kShadeMaxMs)

		// The shaders: worldfx.hlsl next to the add-on when there is one, compiled again (on a thread of its own) when
		// it changes; the one compiling
		uint64_t shader_stamp = 0;
		std::string shader_status = "built in";
		std::shared_ptr<shader_job> shader_compiling;

		// Characters and monsters near (from the worldfx Ashita addon): x, y, z, radius each, and when sent
		float actors[grass::kMaxActors * 4] = {};
		uint32_t actor_count = 0;
		ULONGLONG actors_at = 0;
		// Which of them are fighting (bit i: actor i; from worldfx_grass_actors2)
		uint32_t fighting = 0;
		// The grass's wear: how much it shows (GrassWear, 0 off), whether weapon skills cut it (GrassCuts); the cuts
		// sent lately (worldfx_grass_cut: a swath from, half-width, to; when it came), a ring, and how many came in all
		float grass_wear = 1.0f;
		bool grass_cuts = true;
		struct cut_sent
		{
			float swath[7];
			ULONGLONG at;
		};
		cut_sent cuts[grass::kMaxCuts] = {};
		uint32_t cuts_next = 0, cuts_total = 0;
		ULONGLONG last_cut_at = 0;
		float cut_upload[grass::kMaxCuts * 8] = {};
		// (whether there were some at the last draw, when that was last logged, and the zone whose were logged)
		bool people = true;
		ULONGLONG people_logged_at = 0;
		uint32_t people_zone_logged = 0xFFFFFFFFu;

		// Rings on the water from them moving through it. Where the zone's water is: its surface's y in each
		// 2-yalm square its triangles reach into (the highest there: y is down). Last time's actors, matched
		// to this time's by position, for how fast each moves and when it last made a ring; the rings
		// themselves, the oldest replaced by the newest; and what goes to the renderer (with their ages).
		std::unordered_map<uint64_t, float> water_map;
		struct ripple_track { float x, y, z, speed; ULONGLONG last_ring; };
		ripple_track tracks[grass::kMaxActors] = {};
		uint32_t track_count = 0;
		ULONGLONG tracks_at = 0;
		struct ripple { float x, y, z, strength, radius; ULONGLONG born; };
		ripple ripples[grass::kMaxRipples] = {};
		uint32_t ripple_next = 0;
		float ripple_upload[grass::kMaxRipples * 8] = {};
		uint32_t ripples_made = 0; // (since the add-on loaded: the panel shows it)

		// Lamps: the zone's (worldfx-lamps\<zone>.bin, tools/bake_lamps.py: where each is, its colour, how far its
		// light reaches and how bright, by id), and the lights lit in the game's global light table as the worldfx
		// Ashita addon last sent them (by id: which lamps are on, how bright, where they are now). Each frame the
		// nearest lit ones in view go to the renderer (pick_lamps). Settings: how strong the glow is (0 off), how
		// far it reaches (times the lamp's own range), how much of it a lamp's shadow takes, and whether lit lights
		// the bake doesn't know count too.
		struct baked_lamp { float x, y, z, range, r, g, b, brightness; uint32_t id, slot, flags; float window; };
		std::vector<baked_lamp> lamps;
		std::unordered_map<uint32_t, size_t> lamp_by_id;
		std::string lamps_status = "-";
		struct live_light { uint32_t id; float r, g, b, x, y, z, range, brightness; };
		std::vector<live_light> live_lights;
		ULONGLONG live_at = 0;
		uint32_t live_sends = 0;
		float lamp_glow = 0.5f, lamp_reach = 1.5f, lamp_shadows = 0.5f;
		bool lamps_unbaked = true;
		float lamp_upload[grass::kMaxLamps * 8] = {};
		// (the last frame's, for the panel and the tests: baked lamps lit, lit lights not in the bake used, lit
		// lights left out for reaching too far, and how many went to the renderer)
		uint32_t lamps_lit = 0, lamps_unbaked_used = 0, lamps_area = 0, lamps_picked = 0;

		// Glows: lights for glowing things the game draws but lights nothing with, as the worldfx Ashita addon (0.10)
		// last sent them (worldfx_glows): glowing fixtures (kind 2: Home Point crystals, Survival Guides, waypoints...,
		// found by name, each its kind's colour, pulsing) and glowing effects (kind 3: the additive effect elements alive,
		// a light for what made them; client-notes "Effects that light"). They join the lamps in pick_lamps. Settings:
		// how strongly each kind glows (0 off). An effect's glow by a lit lamp is that lamp's own flame: left out.
		struct glow_light { uint32_t id, kind; float r, g, b, x, y, z, range, brightness; };
		std::vector<glow_light> glows;
		ULONGLONG glows_at = 0;
		uint32_t glow_sends = 0;
		float fixture_glow = 1.0f, effect_glow = 0.6f;
		// (the last frame's: glows that went to the renderer, effects' left out by a lamp)
		uint32_t glows_used = 0, glows_by_lamps = 0;

		// The game's weather, as the worldfx Ashita addon (0.7) last sent it (worldfx_weather: client-notes "Weather
		// and sky"), and when. Settings: its fog over the grass, the fireflies and the water (and under what the
		// shading pass darkens), and the sun's and moon's shadows faded by it.
		grass::weather weather;
		ULONGLONG weather_at = 0;
		uint32_t weather_sends = 0;
		// (debug: World FX's own weather forced to another, worldfx::force_weather. weather_real is the weather as sent;
		// weather is it with the forced one over it (weather_forced_over). name 0: none forced; from: the weather it fades
		// from (0: no fade left). Not saved: none at every start. A manual flash of lightning until flash_until.)
		grass::weather weather_real;
		struct forced_weather
		{
			uint32_t name = 0, from = 0;
			ULONGLONG since = 0, fade_ms = 0;
		} forced;
		ULONGLONG flash_until = 0;
		bool flash_bolt = false;
		// The game's Vana'diel clock (Earth seconds; worldfx_moon, from HiFi XI's game side each frame) and when it came:
		// the moon's phase (moon_phase); without it, the same clock from the PC's (moon_clock 0 or old)
		uint32_t moon_clock = 0;
		ULONGLONG moon_clock_at = 0;
		float test_moon = -1.0f; // (a fixed moon for tests: how far through its 84 days, 0..1; -1: the real one)
		// (what was last logged: the weather shown, whether its record was read, the zone)
		uint64_t weather_logged = ~0ull;
		bool weather_fog = true, weather_shadows = true;
		// Each zone's clear sky through the day (worldfx-clearsky.txt next to the add-on, tools/bake_clear_sky.py), read
		// once: the sun's shadows the weather leaves are the game's light now against it (weather_sunlight). The clear
		// sky's light for this zone and hour, and the share last worked out (kept while the record isn't read: the first
		// frames in a zone, before its weather has a record)
		std::vector<grass::clear_sky> clear_skies;
		bool clear_skies_read = false;
		std::string clear_sky_status = "not read yet";
		float clear_light = 0.0f;
		float weather_share = 1.0f;
		// Wet path and stone in rain: the setting (how wet at most, 0 off), how wet the ground is now (as the rain, the
		// game's change blending it in; drying slowly after), when that was last worked out, and the zone's mask
		// (worldfx-wet\<zone>.bin, tools/bake_wet.py: only zones with rain of their own have one)
		float wet_ground = 0.0f;
		float wetness = 0.0f;
		ULONGLONG wet_at = 0;
		bool wet_weather_known = false; // (the weather was known at the last update: rain starting then wets the ground slowly)
		std::string wet_status = "-";
		// Settled snow: the setting (how much at most, 0 off), how much lies now (building up while it snows, melting
		// after), when that was last worked out, and whether the weather was known then; when this zone was entered (the
		// ground's wetness and snow go straight to its weather for kZoneWeatherMs after: none carried over from the last)
		float snow_cover_amount = 1.0f;
		float snow_cover = 0.0f;
		// ... with depth (0 flat, 1 a whole layer grass_render's kSnowDepth deep) and trails trodden through it
		float snow_depth = 1.0f;
		bool snow_trails = true;
		ULONGLONG snow_at = 0;
		bool snow_weather_known = false;
		ULONGLONG zone_entered_at = 0;
		// Rain of the add-on's own, in rain, squalls and thunder: on or off, how hard at most (times the weather's own:
		// rain 0.55, squalls 0.8, thunder 1), how many splashes and how strong the rings on the water; the zone's rain
		// map (worldfx-rain\<zone>.bin, tools/bake_rain.py: where drops stop); how hard it rains now (for the panel)
		bool rain_on = true;
		float rain_amount = 1.0f, rain_splashes = 1.0f, rain_rings = 1.0f;
		std::string rain_status = "-";
		float raining = 0.0f;
		// Snow of the add-on's own, in snow and blizzards (ending on the same map): on or off, how hard at most (times
		// the weather's own: snow 0.6, blizzards 1); how hard it snows now (for the panel)
		bool snow_on = true;
		float snow_amount = 1.0f;
		float snowing = 0.0f;
		// A sky of the add-on's own (client-notes "Sky": the weather record's gradient, which the game draws as a dome of
		// strips): on or off (off until Alex has seen it), how strong its sun and moon and how bright its stars (0 none);
		// how high the sun is by the hour (its sine: night below 0) and whether the game's light is above the horizon
		// (place_sun), for them
		bool sky_on = false;
		float sky_sun = 1.0f, sky_stars = 1.0f;
		bool sky_debug = false; // (the debug view: what's repainted, what's left)
		float hour_up = 0.0f;
		bool light_up = false;
		// The weather of its own beyond rain and snow (look::wx), drawn while all of the zone's weather is World FX's own
		// (weather_all: the game's left out whole, its sky layers too). Settings: the master (WeatherOwn), the clouds and the
		// lightning on or off, how strong the dust and sand storms, the wind and gales, the heat shimmer, the auroras and
		// gloom's fog banks (0 none, 1 as the weather has them, up to 2). Turning a part off only stops World FX's own: with
		// WeatherOwn on, the game's weather stays left out.
		bool weather_own = true, clouds_on = true, lightning_on = true;
		int cloud_quality = 1; // (how finely the clouds are marched: 0 low .. 2 high, look::weather_fx::cloud_quality)
		// Light in the air: the sun's shafts (0 none), the fog's thickness over clear air's (0 clear air only) and how
		// finely it's marched (0 low .. 2 high: 16, 24 or 32 steps a pixel)
		float vol_light = 1.0f, vol_fog = 1.0f;
		int vol_quality = 1;
		int cloud_style = 0;   // (0 game-like, 1 volumetric: look::weather_fx::cloud_style)
		float weather_dust = 1.0f, weather_wind = 1.0f, heat_shimmer = 1.0f, aurora_amount = 1.0f, gloom_fog = 1.0f;
		// What moves: how far the clouds have drifted (the broad shapes', the detail's: the noise's units, each wrapped to its
		// period) and the fog banks (in theirs), when that was last worked out; the ground under the camera (the game's y),
		// eased toward where the player stands (the first actor sent, near the camera) or a little under the camera, and the
		// zone it was for; the camera's bearing (radians from north toward east: the bolts come mostly where it looks)
		double cloud_drift[4] = {}, bank_drift[2] = {};
		ULONGLONG wx_at = 0;
		float ground_y = 0.0f;
		bool ground_known = false;
		uint32_t ground_zone = 0;
		float cam_bearing = 0.0f;
		float wind_bearing = 0.0f; // (the way the wind blows now, the same: grass::wind_bearing for the zone and weather)
		bool wx_on = false; // (all of the weather its own at the last drawing)
		// Lightning (thunder, thunderstorms): when the next flash comes (0: none waited for: no thunder), when the last began,
		// its two or three pulses (seconds after its start, how bright), whether it brings a bolt and where (bearing, how high
		// it starts, its shape), how many came since the add-on loaded; a random number's state
		struct lightning
		{
			ULONGLONG next = 0, start = 0;
			float pulse_at[3] = {}, pulse_strength[3] = {};
			uint32_t pulses = 0;
			bool bolt = false;
			float bearing = 0.0f, top = 0.3f, seed = 0.0f;
			uint32_t count = 0;
			uint32_t random = 0x6D2B79F5u;
		} flashes;

		// The global holding gGame (0: not found)
		uintptr_t game_global = 0;
		bool game_scanned = false;

		// Status
		const char *skip = "not tried yet";
		grass::draw_stats last;
		// The blades at the zone's end marker (on_zone_end): drawn there this frame (the world's end draws the rest), the
		// frame's look worked out there, what they drew, how many times
		bool blades_at_marker = false, look_at_marker = false;
		bool last_at_marker = false; // (the last world's end drew the rest, its blades at the zone's end)
		grass::draw_stats marker_blades;
		uint64_t blades_at_markers = 0;
		uint64_t at_vertices = 0;
		uint64_t of_vertices = 0;
		bool history[kHistory] = {};
		uint32_t history_pos = 0;
		uint32_t history_count = 0;
	};
	addon_state g;

	// The host (worldfx::port): its log, its settings, where the files are, which resource a view is of
	worldfx::port host;

	void log_message(int level, const char *text)
	{
		if (host.log != nullptr)
			host.log(level, text);
	}

	void config_value(const char *key, bool &value)
	{
		if (host.get_bool != nullptr)
			host.get_bool(key, value);
	}
	void config_value(const char *key, float &value)
	{
		if (host.get_float != nullptr)
			host.get_float(key, value);
	}
	void config_value(const char *key, int &value)
	{
		if (host.get_int != nullptr)
			host.get_int(key, value);
	}
	void set_config(const char *key, bool value)
	{
		if (host.set_bool != nullptr)
			host.set_bool(key, value);
	}
	void set_config(const char *key, float value)
	{
		if (host.set_float != nullptr)
			host.set_float(key, value);
	}
	void set_config(const char *key, int value)
	{
		if (host.set_int != nullptr)
			host.set_int(key, value);
	}

	// The resource a view is of now (nullptr: none, or not known)
	ID3D12Resource *resource_of(uint64_t view)
	{
		return view != 0 && host.resource_of_view != nullptr ? host.resource_of_view(view) : nullptr;
	}

	const char *arg_name(grass::root_arg::kind_t kind)
	{
		switch (kind)
		{
		case grass::root_arg::table: return "table";
		case grass::root_arg::root_constants: return "constants";
		case grass::root_arg::cbv: return "CBV";
		case grass::root_arg::srv: return "SRV";
		case grass::root_arg::uav: return "UAV";
		default: return "unset";
		}
	}

	// Game memory

	bool safe_copy(void *dst, uintptr_t src, size_t size)
	{
		__try
		{
			std::memcpy(dst, reinterpret_cast<const void *>(src), size);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// The global in the first 'mov eax, [global]' in FFXiMain.dll's code followed by these bytes (0: not found)
	uintptr_t find_global(const uint8_t *tail, size_t tail_size)
	{
		const auto base = reinterpret_cast<const uint8_t *>(GetModuleHandleW(L"FFXiMain.dll"));
		if (base == nullptr)
			return 0;
		const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_lfanew);
		const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
		{
			if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
				continue;
			const uint8_t *p = base + section->VirtualAddress;
			const size_t size = section->Misc.VirtualSize;
			for (size_t k = 0; k + 5 + tail_size <= size; ++k)
			{
				if (p[k] != 0xA1 || std::memcmp(p + k + 5, tail, tail_size) != 0)
					continue;
				uint32_t address = 0;
				std::memcpy(&address, p + k + 1, 4);
				return address;
			}
		}
		return 0;
	}

	// facelift's 'game.sys': mov eax, [gGame]; add eax, 0x94; ret (the view matrix getter)
	uintptr_t find_game_global()
	{
		static const uint8_t tail[] = { 0x05, 0x94, 0x00, 0x00, 0x00, 0xC3 };
		return find_global(tail, sizeof(tail));
	}

	// The environment object's global (FFXiMain+0x65CB04 in the dump), from the function at FFXiMain+0x3D9A0:
	// mov eax, [env]; sub esp, 24h; push esi; mov esi, [eax+18h]; test ... (the only match)
	uintptr_t find_env_global()
	{
		static const uint8_t tail[] = { 0x83, 0xEC, 0x24, 0x56, 0x8B, 0x70, 0x18, 0x85 };
		return find_global(tail, sizeof(tail));
	}

	// The game's camera, or why not
	const char *read_camera(grass::camera &cam)
	{
		if (!g.game_scanned)
		{
			g.game_scanned = true;
			g.game_global = find_game_global();
		}
		if (g.game_global == 0)
			return "the game's camera wasn't found in FFXiMain.dll";
		uint32_t game = 0;
		uint8_t in_field = 0;
		if (!safe_copy(&game, g.game_global, 4) || game == 0 || !safe_copy(&in_field, game + 8, 1))
			return "no game object";
		if (in_field == 0)
			return "not in the field";
		// (+9 isn't a zoning flag to draw by: it stays set in the field in some zones, e.g. Jugner Forest. Frames
		// without the world are skipped anyway, since nothing passes the world target's vertex count then.)
		if (!safe_copy(cam.view, game + 0x94, 64) || !safe_copy(cam.proj, game + 0x154, 64))
			return "the camera couldn't be read";
		for (int i = 0; i < 16; ++i)
			if (!std::isfinite(cam.view[i]) || !std::isfinite(cam.proj[i]))
				return "the camera isn't set";
		if (cam.proj[11] == 0.0f)
			return "the camera isn't set";
		return nullptr;
	}

	// Zone and blades (with the mutex held)

	// The host's folder (worldfx::port::folder: the add-on's, the game's bootloader folder)
	std::wstring addon_folder()
	{
		return host.folder;
	}

	// The water map's key for the 2-yalm square holding x, z
	uint64_t water_key(float x, float z)
	{
		const int32_t ix = static_cast<int32_t>(std::floor(x * 0.5f)), iz = static_cast<int32_t>(std::floor(z * 0.5f));
		return (uint64_t(uint32_t(ix)) << 32) | uint32_t(iz);
	}

	// Where the zone's water is, from its file (mutex held): each triangle marks the squares its bounds reach
	// into with its surface's y (the highest wins: y is down)
	void map_water(const grass::blade_file &water)
	{
		g.water_map.clear();
		const float *const v = water.blades.data();
		for (uint32_t t = 0; t + 2 < water.blade_count(); t += 3)
		{
			const float *a = v + size_t(t) * 8, *b = a + 8, *c = b + 8;
			const float surface = std::min({ a[4], b[4], c[4] });
			const float x0 = std::min({ a[0], b[0], c[0] }), x1 = std::max({ a[0], b[0], c[0] });
			const float z0 = std::min({ a[2], b[2], c[2] }), z1 = std::max({ a[2], b[2], c[2] });
			if (x1 - x0 > 64.0f || z1 - z0 > 64.0f || !std::isfinite(surface))
				continue; // (not a 1-yalm grid triangle)
			for (float x = std::floor(x0 * 0.5f) * 2.0f; x <= x1; x += 2.0f)
				for (float z = std::floor(z0 * 0.5f) * 2.0f; z <= z1; z += 2.0f)
				{
					auto [it, added] = g.water_map.try_emplace(water_key(x + 1.0f, z + 1.0f), surface);
					if (!added)
						it->second = std::min(it->second, surface);
				}
		}
	}

	// Rings from the actors just sent (mutex held): each matched to the nearest of last time's (within a
	// yalm and a half), its speed followed; one in the water (its feet from a third of a yalm over the surface
	// to 3 under it) makes a ring every quarter second while it moves, a faint one every 1.7 seconds while
	// it stands
	void make_ripples(ULONGLONG now)
	{
		const float dt = g.tracks_at != 0 ? std::max(1.0f, float(now - g.tracks_at)) / 1000.0f : 0.0f;
		addon_state::ripple_track next[grass::kMaxActors] = {};
		for (uint32_t i = 0; i < g.actor_count; ++i)
		{
			const float *a = &g.actors[i * 4];
			addon_state::ripple_track t = { a[0], a[1], a[2], 0.0f, 0 };
			float best = 1.5f * 1.5f;
			for (uint32_t j = 0; j < g.track_count; ++j)
			{
				const addon_state::ripple_track &o = g.tracks[j];
				const float dx = a[0] - o.x, dz = a[2] - o.z, d2 = dx * dx + dz * dz;
				if (d2 < best && std::fabs(a[1] - o.y) < 2.0f && dt > 0.0f && dt < 0.5f)
				{
					best = d2;
					t.speed = o.speed + (std::sqrt(d2) / dt - o.speed) * 0.3f;
					t.last_ring = o.last_ring;
				}
			}
			const auto water = g.water_map.find(water_key(a[0], a[2]));
			if (water != g.water_map.end() && a[1] > water->second - 0.35f && a[1] < water->second + 3.0f)
			{
				const bool moving = t.speed > 0.8f;
				if (now - t.last_ring >= (moving ? 250u : 1700u))
				{
					addon_state::ripple &r = g.ripples[g.ripple_next++ % grass::kMaxRipples];
					r = { a[0], water->second, a[2], moving ? std::clamp(0.45f + t.speed / 10.0f, 0.45f, 1.0f) : 0.25f,
						std::clamp(a[3] * 0.45f, 0.35f, 2.2f), now };
					t.last_ring = now;
					g.ripples_made++;
				}
			}
			next[i] = t;
		}
		std::memcpy(g.tracks, next, sizeof(next));
		g.track_count = g.actor_count;
		g.tracks_at = now;
	}

	// The rings still spreading, with their ages, for the renderer (mutex held)
	uint32_t ripples_for_drawing(ULONGLONG now)
	{
		uint32_t n = 0;
		for (const addon_state::ripple &r : g.ripples)
		{
			const float age = float(now - r.born) / 1000.0f;
			if (r.born == 0 || age > 2.6f)
				continue;
			float *o = &g.ripple_upload[n++ * 8];
			o[0] = r.x, o[1] = r.y, o[2] = r.z, o[3] = r.strength;
			o[4] = age, o[5] = r.radius, o[6] = 0.0f, o[7] = 0.0f;
		}
		return n;
	}

	void check_zone()
	{
		char text[16] = "";
		const DWORD n = GetEnvironmentVariableA(kZoneVariable, text, sizeof(text));
		const bool known = n > 0 && n < sizeof(text);
		const uint32_t zone = known ? std::strtoul(text, nullptr, 10) : 0;
		// (the zone's files just made: read again as if it had just been entered)
		if (g_reload_zone.exchange(false))
			g.zone_tried = false;
		if (g.zone_tried && known == g.zone_known && zone == g.zone)
			return;
		g.zone_tried = true;
		g.zone_known = known;
		g.zone = zone;
		g.zone_sunlit = true;
		if (FILE *f = nullptr; _wfopen_s(&f, (addon_folder() + L"worldfx-sunlit.txt").c_str(), L"r") == 0 && f != nullptr)
		{
			g.zone_sunlit = false;
			char line[128];
			while (fgets(line, sizeof(line), f) != nullptr)
				if (line[0] >= '0' && line[0] <= '9' && std::strtoul(line, nullptr, 10) == zone)
					g.zone_sunlit = true;
			fclose(f);
		}
		// Each zone's clear sky (read once), and the shadows' share as without weather until this zone's record is read
		if (!g.clear_skies_read)
		{
			g.clear_skies_read = true;
			std::string why;
			if (grass::read_clear_sky_file((addon_folder() + L"worldfx-clearsky.txt").c_str(), g.clear_skies, why))
			{
				size_t count = 0;
				for (const grass::clear_sky &c : g.clear_skies)
					count += c.known ? 1 : 0;
				g.clear_sky_status = std::to_string(count) + " zones' clear sky (worldfx-clearsky.txt)";
			}
			else
				g.clear_sky_status = why + ": shadows aren't faded by the weather";
		}
		g.weather_share = 1.0f;
		g.zone_entered_at = GetTickCount64();
		g.ground_known = false; // (the ground under the camera found again in the new zone)
		g.renderer->unload();
		g.renderer->unload_water();
		g.renderer->unload_sea();
		g.renderer->unload_shadow();
		g.renderer->unload_wet();
		g.renderer->unload_rain();
		g.water_map.clear();
		g.sun_map_redraws = 0;
		g.lamps.clear();
		g.lamp_by_id.clear();
		g.glows.clear(); // (the last zone's: the addon sends this one's)
		for (auto &c : g.cuts)
			c.at = 0; // (the last zone's places)
		if (!known)
		{
			g.blades_status = "no zone: load the worldfx addon in Ashita (/addon load worldfx)";
			g.water_status = "-";
			g.sea_status = "-";
			g.shadow_status = "-";
			g.lamps_status = "-";
			g.wet_status = "-";
			g.rain_status = "-";
			return;
		}
		char name[32];
		std::snprintf(name, sizeof(name), "%u.bin", zone);
		// The zone's wet mask (tools/bake_wet.py): where its ground is path or stone
		{
			const std::wstring wet_path = addon_folder() + L"worldfx-wet\\" + std::wstring(name, name + std::strlen(name));
			grass::wet_file mask;
			std::string why;
			char wet_status[128];
			if (grass::read_wet_file(wet_path.c_str(), mask, why) && g.renderer->upload_wet(mask, why))
				std::snprintf(wet_status, sizeof(wet_status), "zone %u: %.0f square yalms of path and stone", zone, double(mask.set) * mask.square * mask.square);
			else
				std::snprintf(wet_status, sizeof(wet_status), "zone %u: %s", zone, why == "no file for this zone" ? "no mask (no rain of its own)" : why.c_str());
			g.wet_status = wet_status;
		}
		// The zone's rain map (tools/bake_rain.py): where drops and flakes stop
		{
			const std::wstring rain_path = addon_folder() + L"worldfx-rain\\" + std::wstring(name, name + std::strlen(name));
			grass::rain_file map;
			std::string why;
			char rain_status[160];
			if (grass::read_rain_file(rain_path.c_str(), map, why))
			{
				const uint32_t width = map.width, height = map.height, covered = map.covered;
				const float square = map.square;
				if (g.renderer->upload_rain(std::move(map), why))
					std::snprintf(rain_status, sizeof(rain_status), "zone %u: %u x %u squares of %.1f yalms, %u with a surface", zone, width, height, square, covered);
				else
					std::snprintf(rain_status, sizeof(rain_status), "zone %u: %s", zone, why.c_str());
			}
			else
				std::snprintf(rain_status, sizeof(rain_status), "zone %u: %s", zone, why == "no file for this zone" ? "no rain map (no rain or snow of its own)" : why.c_str());
			g.rain_status = rain_status;
		}
		// The zone's lamps (tools/bake_lamps.py): 12 words each, the last four of them not floats (id, slot, flags)
		// but for the far end of the lamp's distance window. An LMP1 file (from before the hour) is read with its
		// brightness unknown (0: a lamp's glow as bright as the game's light now, as then) until it's made again
		{
			const std::wstring lamp_path = addon_folder() + L"worldfx-lamps\\" + std::wstring(name, name + std::strlen(name));
			grass::blade_file file;
			std::string why, why_older;
			char lamps_status[128];
			bool read = grass::read_blade_file(lamp_path.c_str(), file, why, "LMP2", 12), older = false;
			if (!read && why != "no file for this zone")
				read = older = grass::read_blade_file(lamp_path.c_str(), file, why_older, "LMP1", 12);
			if (read)
			{
				for (uint32_t i = 0; i < file.blade_count(); ++i)
				{
					addon_state::baked_lamp lamp;
					static_assert(sizeof(lamp) == 48, "a baked lamp is 12 words");
					std::memcpy(&lamp, &file.blades[size_t(i) * 12], sizeof(lamp));
					if (older)
						lamp.brightness = 0.0f;
					if (!std::isfinite(lamp.x) || !std::isfinite(lamp.y) || !std::isfinite(lamp.z) || !std::isfinite(lamp.range) ||
						!std::isfinite(lamp.r) || !std::isfinite(lamp.g) || !std::isfinite(lamp.b) || !std::isfinite(lamp.brightness))
						continue;
					g.lamp_by_id.emplace(lamp.id, g.lamps.size());
					g.lamps.push_back(lamp);
				}
				std::snprintf(lamps_status, sizeof(lamps_status), "zone %u: %zu lamps%s", zone, g.lamps.size(), older ? " (an older file, without their hours)" : "");
			}
			else
				std::snprintf(lamps_status, sizeof(lamps_status), "zone %u: %s", zone, why == "no file for this zone" ? "no lamps baked" : why.c_str());
			g.lamps_status = lamps_status;
		}
		// The zone's water (tools/bake_water.py)
		{
			const std::wstring water_path = addon_folder() + L"worldfx-water\\" + std::wstring(name, name + std::strlen(name));
			grass::blade_file water;
			std::string why;
			char water_status[128];
			if (grass::read_blade_file(water_path.c_str(), water, why, "WTR2", 8) && g.renderer->upload_water(water, why))
			{
				std::snprintf(water_status, sizeof(water_status), "zone %u: %u triangles in %zu cells", zone, water.blade_count() / 3, water.cells.size());
				map_water(water);
			}
			else
			{
				std::snprintf(water_status, sizeof(water_status), "zone %u: %s", zone, why == "no file for this zone" ? "no water" : why.c_str());
				g.water_map.clear();
			}
			g.water_status = water_status;
		}
		// The zone's sea: the game's own sea sheets past its water (HiFi XI's bake 'sea', worldfx-sea\<zone>.bin; a zone the
		// bake keeps none for has a .none file: as none), drawn as the water where the baked water isn't, out to the fog
		{
			const std::wstring sea_path = addon_folder() + L"worldfx-sea\\" + std::wstring(name, name + std::strlen(name));
			grass::blade_file sea;
			uint32_t sheets = 0;
			std::string why;
			char sea_status[160];
			if (grass::read_sea_file(sea_path.c_str(), sea, sheets, why) && g.renderer->upload_sea(sea, why))
				std::snprintf(sea_status, sizeof(sea_status), "zone %u: %u sheets, %u triangles", zone, sheets, sea.blade_count() / 3);
			else
				std::snprintf(sea_status, sizeof(sea_status), "zone %u: %s", zone, why == "no file for this zone" ? "no sea" : why.c_str());
			g.sea_status = sea_status;
		}
		// The zone's meshes for the sun's shadow map (tools/bake_shadow_geo.py), unless the host draws the zone's casters
		// itself (set_zone_casters: HiFi XI's native zone)
		if (g.zone_bounds != nullptr && g.zone_draw != nullptr)
			g.shadow_status = "zone " + std::to_string(zone) + ": the host's own zone casts the shadows";
		else
		{
			const std::wstring shadow_path = addon_folder() + L"worldfx-shadow\\" + std::wstring(name, name + std::strlen(name));
			grass::shadow_file meshes;
			std::string why;
			char shadow_status[128];
			if (grass::read_shadow_file(shadow_path.c_str(), meshes, why) && g.renderer->upload_shadow(meshes, why))
				std::snprintf(shadow_status, sizeof(shadow_status), "zone %u: %u triangles, %zu cut-out textures", zone, meshes.triangle_count(), meshes.masks.size() / 4);
			else
				std::snprintf(shadow_status, sizeof(shadow_status), "zone %u: %s", zone, why == "no file for this zone" ? "no meshes (screen-space shadows only)" : why.c_str());
			g.shadow_status = shadow_status;
		}
		const std::wstring path = addon_folder() + L"worldfx-grass\\" + std::wstring(name, name + std::strlen(name));
		grass::blade_file file;
		std::string error;
		char status[160];
		if (!grass::read_blade_file(path.c_str(), file, error) || !g.renderer->upload(file, error))
		{
			std::snprintf(status, sizeof(status), "zone %u: %s", zone, error.c_str());
			g.blades_status = status;
			return;
		}
		std::snprintf(status, sizeof(status), "zone %u: %u blades in %zu cells", zone, file.blade_count(), file.cells.size());
		g.blades_status = status;
		log_message(0, ("FFXI World FX: grass for " + g.blades_status).c_str());
	}

	// The compiled shaders kept between runs (grass::compile_shaders): the built-in ones and worldfx.hlsl's
	std::wstring shader_cache_folder() { return addon_folder() + L"worldfx-shader-cache"; }

	std::string clock_now()
	{
		SYSTEMTIME now;
		GetLocalTime(&now);
		char when[16];
		std::snprintf(when, sizeof(when), "%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
		return when;
	}

	DWORD WINAPI shader_thread(void *param)
	{
		auto *const held = static_cast<std::shared_ptr<shader_job> *>(param);
		const HMODULE module = (*held)->module;
		{
			shader_job &job = **held;
			const auto start = std::chrono::steady_clock::now();
			job.ok = grass::compile_shaders(job.source, job.cache, job.code, job.from_cache, job.error);
			job.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			job.done.store(true, std::memory_order_release);
		}
		delete held;
		FreeLibraryAndExitThread(module, 0);
	}

	// Shaders from worldfx.hlsl next to the add-on, compiled again whenever the file changes (its time or size): edits
	// show while the game runs. The shaders in use stay while it compiles, on a thread of its own (one at a time: a
	// change meanwhile is seen once it's done); then it's swapped in on a frame. The same source as the shaders in use
	// (the installed worldfx.hlsl is the built-in one) isn't compiled again, and one compiled before comes from the
	// shader cache. One that doesn't compile leaves the last good ones in use. (mutex held)
	void check_shaders()
	{
		if (g.shader_compiling != nullptr)
		{
			shader_job &job = *g.shader_compiling;
			if (!job.done.load(std::memory_order_acquire))
				return;
			const std::string when = clock_now();
			if (job.ok)
			{
				char how[64];
				if (job.from_cache)
					std::snprintf(how, sizeof(how), "from the shader cache");
				else
					std::snprintf(how, sizeof(how), "compiled in the background in %.1f s", job.seconds);
				if (g.renderer != nullptr)
					g.renderer->set_shader_code(std::move(job.code));
				g.shader_status = "worldfx.hlsl, loaded at " + when + " (" + how + ")";
				log_message(0, (std::string("FFXI World FX: shaders loaded from worldfx.hlsl (") + how + ")").c_str());
			}
			else
			{
				g.shader_status = "worldfx.hlsl doesn't compile (" + when + "; the last good shaders stay): " + job.error.substr(0, 300);
				log_message(2, ("FFXI World FX: worldfx.hlsl: " + job.error).c_str());
			}
			g.shader_compiling.reset();
			return;
		}
		const std::wstring path = addon_folder() + L"worldfx.hlsl";
		WIN32_FILE_ATTRIBUTE_DATA a = {};
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
			return; // (none: the built-in ones, or the last loaded)
		const uint64_t stamp = ((uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime) ^ a.nFileSizeLow;
		if (stamp == g.shader_stamp)
			return;
		g.shader_stamp = stamp;
		std::string source;
		if (FILE *f = nullptr; _wfopen_s(&f, path.c_str(), L"rb") == 0 && f != nullptr)
		{
			char buffer[4096];
			size_t n;
			while ((n = fread(buffer, 1, sizeof(buffer), f)) != 0)
				source.append(buffer, n);
			fclose(f);
		}
		const std::string when = clock_now();
		if (source.empty())
		{
			g.shader_status = "worldfx.hlsl doesn't compile (" + when + "; the last good shaders stay): empty or unreadable";
			log_message(2, "FFXI World FX: worldfx.hlsl: empty or unreadable");
			return;
		}
		const uint64_t key = grass::shader_key(source);
		if (key != 0 && g.renderer != nullptr && key == g.renderer->shader_key())
		{
			g.shader_status = "worldfx.hlsl, the same as the shaders in use (" + when + ")";
			log_message(0, "FFXI World FX: worldfx.hlsl is the same as the shaders in use");
			return;
		}
		auto job = std::make_shared<shader_job>();
		job->source = std::move(source);
		job->cache = shader_cache_folder();
		HANDLE thread = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&shader_thread), &job->module))
		{
			auto *const held = new std::shared_ptr<shader_job>(job);
			thread = CreateThread(nullptr, 0, shader_thread, held, 0, nullptr);
			if (thread == nullptr)
			{
				delete held;
				FreeLibrary(job->module);
			}
		}
		if (thread == nullptr)
		{
			g.shader_status = "worldfx.hlsl couldn't be compiled (" + when + ": no thread for it); the last good shaders stay";
			log_message(2, "FFXI World FX: worldfx.hlsl: no thread to compile it on");
			return;
		}
		SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
		CloseHandle(thread);
		g.shader_compiling = std::move(job);
		g.shader_status = "worldfx.hlsl changed at " + when + ": compiling in the background (the shaders in use stay until it's done)";
		log_message(0, "FFXI World FX: worldfx.hlsl changed: compiling it in the background");
	}

	DXGI_FORMAT view_format(uint64_t view, ID3D12Resource *res)
	{
		DXGI_FORMAT fmt = view != 0 && host.format_of_view != nullptr ? host.format_of_view(view) : DXGI_FORMAT_UNKNOWN;
		if (fmt == DXGI_FORMAT_UNKNOWN && res != nullptr)
			fmt = res->GetDesc().Format;
		return fmt;
	}

	// The first draw goes to the log with dgVoodoo's root signature, so a crash can be traced
	void log_first(const list_data &d, const grass::target &t)
	{
		g.logged_first = true;
		std::string text = "FFXI World FX: first grass draw. Root parameters of dgVoodoo's root signature:";
		const size_t known = host.root_param_count != nullptr ? host.root_param_count(d.state.root) : 0;
		const size_t count = known != 0 ? known : d.state.args.size();
		for (size_t i = 0; i < count; ++i)
		{
			const char *const param = known != 0 && host.root_param != nullptr ? host.root_param(d.state.root, i) : nullptr;
			char item[64];
			std::snprintf(item, sizeof(item), " [%zu] %s (%s)", i, param != nullptr ? param : "?",
				i < d.state.args.size() ? arg_name(d.state.args[i].kind) : "unset");
			text += item;
		}
		char tail[320];
		const D3D12_VIEWPORT &vp = d.state.viewports[0];
		const D3D12_RESOURCE_DESC rd = t.rt->GetDesc(), dd = t.ds->GetDesc();
		std::snprintf(tail, sizeof(tail), ". World formats %u/%u (resources %u/%u, %llux%u / %llux%u), states 0x%X/0x%X, %u viewport(s) (first %.0f,%.0f %.0fx%.0f depth %.3f..%.3f), topology %u.",
			static_cast<unsigned>(t.rtv_format), static_cast<unsigned>(t.dsv_format), static_cast<unsigned>(rd.Format), static_cast<unsigned>(dd.Format),
			static_cast<unsigned long long>(rd.Width), rd.Height, static_cast<unsigned long long>(dd.Width), dd.Height,
			static_cast<unsigned>(t.rt_state), static_cast<unsigned>(t.ds_state),
			d.state.viewport_count, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth, static_cast<unsigned>(d.state.topology));
		text += tail;
		log_message(0, text.c_str());
	}

	// Whether grass goes in now: the list is leaving (or closing on) the world after enough of it (mutex held)
	// A finished world's vertex count. The share is of the smallest of the last few: a world drawing less than
	// the one before (turning, a monster gone) still reaches it, so grass doesn't skip it even at 100%.
	void learn_world_size(uint64_t vertices)
	{
		if (vertices < kMinWorldVertices)
			return;
		g.recent[g.recent_pos] = vertices;
		g.recent_pos = (g.recent_pos + 1) % kRecent;
		if (g.recent_count < kRecent)
			g.recent_count++;
		g.world_vertices_last = vertices;
		for (uint32_t i = 0; i < g.recent_count; ++i)
			g.world_vertices_last = std::min(g.world_vertices_last, g.recent[i]);
	}

	// Whether grass went in at the world's read barrier lately (else the fallbacks in due)
	bool barrier_seen()
	{
		return g.frames - g.last_resolve_frame <= kBarrierFrames;
	}

	// to: the target moved to (0 at list close)
	bool due(ID3D12Resource *rt, ID3D12Resource *ds, ID3D12Resource *to = nullptr)
	{
		if (g.world_rt == nullptr || rt != g.world_rt || ds != g.world_ds)
			return false;
		const double share = g.world_vertices_last != 0 ? double(g.world_vertices) / double(g.world_vertices_last) : 0.0;
		if (g.logging_moves && g.moves.size() < 1500)
		{
			char item[96] = "";
			if (to != nullptr && g.dev != nullptr)
			{
				const D3D12_RESOURCE_DESC rd = to->GetDesc();
				std::snprintf(item, sizeof(item), " %.0f%%->%ux%u", share * 100.0, unsigned(rd.Width), unsigned(rd.Height));
			}
			else
				std::snprintf(item, sizeof(item), " %.0f%%->close", share * 100.0);
			g.moves += item;
		}
		// (the world's read barrier is where grass goes in; this is for when there's none)
		if (barrier_seen() || g.tried || g.world_vertices < kMinWorldVertices)
			return false;
		if (share >= 1.0)
			return true;
		// A move to a target of another size than the world's, past the first part: the resolve
		if (to != nullptr && share >= kResolveShare && g.dev != nullptr)
		{
			const D3D12_RESOURCE_DESC a = to->GetDesc(), w = g.world_rt->GetDesc();
			return a.Width >= 640 && (a.Width != w.Width || a.Height != w.Height);
		}
		return false;
	}

	// Lit lights from the game's light table are this fresh at most (ms): older, the worldfx Ashita addon is gone,
	// and whether a lamp is on isn't known (none are drawn)
	constexpr ULONGLONG kLiveLightsMs = 1000;

	// Whether the game's lit lights are known (the Ashita addon has sent some lately)
	bool lights_fresh()
	{
		return !g.live_lights.empty() && GetTickCount64() - g.live_at < kLiveLightsMs;
	}

	// Whether some glow may be on now (a kind of them on, and the Ashita addon has sent some lately: as fresh as the
	// lit lights must be)
	bool glows_on()
	{
		return (g.fixture_glow > 0.0f || g.effect_glow > 0.0f) && !g.glows.empty() && GetTickCount64() - g.glows_at < kLiveLightsMs;
	}

	// Whether some lamp (or glow) may be lit now (the glow on, and the Ashita addon has sent lit lights lately)
	bool lamps_on()
	{
		return (g.lamp_glow > 0.0f && lights_fresh()) || glows_on();
	}

	// The weather from the Ashita addon is this fresh at most (ms): older, it's gone (or an older one that doesn't send
	// it), and everything is drawn as it was without the weather: no fog on the grass, the shadows at the setting
	constexpr ULONGLONG kWeatherMs = 1000;
	// Wet ground dries over this long after the rain stops (seconds), and gets wet over this long once it starts
	constexpr float kDryingSeconds = 90.0f;
	constexpr float kWettingSeconds = 60.0f;
	// Settled snow: a whole layer after this long of snow (a blizzard's, deeper than snow's), gone this long after it stops
	constexpr float kSnowSettleSeconds = 240.0f;
	constexpr float kSnowMeltSeconds = 600.0f;
	// For this long after entering a zone (ms) its ground goes straight to its weather's wetness and snow: the last zone's
	// doesn't come along (the game's weather may still be the last zone's for the first frames)
	constexpr ULONGLONG kZoneWeatherMs = 5000;

	bool weather_fresh()
	{
		return g.weather.known && GetTickCount64() - g.weather_at < kWeatherMs;
	}

	// (debug: worldfx::force_weather) The weather World FX draws by: the real one as sent, or with a forced weather's
	// name over it, cross-faded from the one before as a change of the game's (before, change); the rest (the fog, the
	// record: the zone's light and its sky's colours) stays the real weather's. Fading back to the real one, the real
	// one's own change is left out until the fade is done. (mutex held)
	grass::weather weather_forced_over(const grass::weather &real)
	{
		addon_state::forced_weather &f = g.forced;
		if (f.name == 0 && f.from == 0)
			return real;
		grass::weather w = real;
		const uint32_t target = f.name != 0 ? f.name : real.name;
		const float share = f.fade_ms == 0 ? 1.0f : std::clamp(float(GetTickCount64() - f.since) / float(f.fade_ms), 0.0f, 1.0f);
		if (share >= 1.0f || f.from == 0 || f.from == target || target == 0)
		{
			f.from = 0;
			if (f.name == 0)
				return real;
			w.name = target;
			w.before = 0;
			w.change = 1.0f;
			return w;
		}
		w.name = target;
		w.before = f.from;
		w.change = share;
		return w;
	}

	std::string weather_name_text(uint32_t name)
	{
		char s[5] = {};
		for (int i = 0; i < 4; ++i)
		{
			const char c = char((name >> (8 * i)) & 0xFF);
			s[i] = c >= 0x20 && c < 0x7F ? c : '.';
		}
		return s;
	}

	// The weather in a line, for the panel and the log
	std::string weather_text()
	{
		const grass::weather &w = g.weather;
		std::string s = w.name != 0 ? weather_name_text(w.name) : "not read";
		char part[200];
		if (g.forced.name != 0 || g.forced.from != 0)
			s += " (forced for World FX's own weather, debug: the real one " + (g.weather_real.name != 0 ? weather_name_text(g.weather_real.name) : std::string("not read")) + ")";
		if (w.id != 0xFFFFFFFF)
		{
			std::snprintf(part, sizeof(part), " (id %u)", w.id);
			s += part;
		}
		if (w.before != 0 && w.change < 1.0f)
		{
			std::snprintf(part, sizeof(part), ", changing from %s, %.0f%% there", weather_name_text(w.before).c_str(), w.change * 100.0f);
			s += part;
		}
		if (w.fog)
		{
			std::snprintf(part, sizeof(part), "; fog %.0f %.0f %.0f from %.0f to %.0f yalms", w.fog_color[0] * 255.0f, w.fog_color[1] * 255.0f,
				w.fog_color[2] * 255.0f, w.fog_start, w.fog_end);
			s += part;
		}
		if (w.record)
		{
			std::snprintf(part, sizeof(part), "; the zone's light %.2f, its fog to %.0f, drawn to %.0f yalms", w.intensity, w.record_fog_end, w.draw_distance);
			s += part;
			// (its sky: the dome the game draws from it, client-notes "Sky")
			if (!w.sky_known)
				std::snprintf(part, sizeof(part), "; its sky's heights make no sense");
			else if (w.sky_segments <= 2)
				std::snprintf(part, sizeof(part), "; no sky dome (segments %u)", w.sky_segments);
			else
				std::snprintf(part, sizeof(part), "; its sky dome %u segments, %.0f %.0f %.0f at the horizon to %.0f %.0f %.0f overhead", w.sky_segments,
					w.sky_colors[0][0], w.sky_colors[0][1], w.sky_colors[0][2], w.sky_colors[7][0], w.sky_colors[7][1], w.sky_colors[7][2]);
			s += part;
		}
		else
			s += "; the zone's record not read yet";
		// (the shadows' share and what it comes from: the game's sun light now against the zone's clear sky's)
		if (!g.weather_shadows)
			std::snprintf(part, sizeof(part), "; sun shadows not by the weather (WeatherShadows off)");
		else if (w.record && g.clear_light >= 0.05f)
			std::snprintf(part, sizeof(part), "; sun shadows %.0f%% (the sun's light %.2f of the clear sky's %.2f)", g.weather_share * 100.0f,
				grass::weather_light(w), g.clear_light);
		else if (w.record)
			std::snprintf(part, sizeof(part), "; sun shadows %.0f%% (%s)", g.weather_share * 100.0f,
				g.clear_light > 0.0f ? "the clear sky's light too faint to compare with now" : "no clear sky known for this zone");
		else
			std::snprintf(part, sizeof(part), "; sun shadows %.0f%% (kept until the record is read)", g.weather_share * 100.0f);
		return s + part;
	}

	// The add-on's own sky in a line, for the panel and the crash report (mutex held)
	std::string sky_text()
	{
		if (!g.sky_on)
			return "off";
		const grass::weather &w = g.weather;
		if (!weather_fresh())
			return "on; the weather isn't known (the worldfx Ashita addon sends it)";
		if (!w.record)
			return "on; the zone's weather record not read yet";
		if (!w.sky_known)
			return "on; the record's sky makes no sense (its heights): not drawn";
		if (w.sky_segments <= 2)
			return "on; no sky dome in this zone's weather (the game draws none): not drawn";
		const grass::look::sky_dome &d = g.look.dome;
		char s[256];
		std::snprintf(s, sizeof(s), "on, %s; the game's dome of %u segments repainted where nothing covers it; %s %.0f%%, stars %.0f%%",
			g.last.sky ? "drawn last frame" : "not drawn last frame", w.sky_segments, d.moon > 0.5f ? "moon" : "sun", d.sun * 100.0f, d.stars * 100.0f);
		return s;
	}

	double vanadiel_hour();
	// The moon's phase (moon_now, below)
	struct moon_phase
	{
		float lit = 1.0f;     // (0 new .. 1 full)
		bool waxing = false;
		float cycle = 0.5f;   // (0..1 through the 84 days from day 0)
		bool from_game = false;
	};
	moon_phase moon_now();

	// Whether World FX draws all of the zone's weather of its own now (worldfx_weather_drawn's kWeatherFxAll: HiFi XI's game
	// side then leaves all of the game's weather out, its sky layers too): it draws into the game's world, its WeatherOwn
	// and its sky are on, and the zone has open sky; the weather has been sent lately (kWeatherAllMs: none for longer,
	// nothing of World FX's would stand in for the game's; a moment without it, zoning, doesn't bring the game's back for a
	// frame or two), and once its record is read, it has a dome the sky of its own repaints (none in it: nothing would
	// stand in for the game's sky layers). In a zone without open sky the game keeps its weather. (mutex held)
	constexpr ULONGLONG kWeatherAllMs = 5000;
	bool weather_all()
	{
		if (!g_has_drawn || g.renderer == nullptr || !g.weather_own || !g.sky_on || !g.zone_sunlit)
			return false;
		const grass::weather &w = g.weather;
		if (!w.known || g.weather_at == 0 || GetTickCount64() - g.weather_at > kWeatherAllMs)
			return false;
		return !(w.record && (!w.sky_known || w.sky_segments <= 2));
	}

	// A random number (0..1) from the lightning's own sequence (xorshift, started from the clock: each session's its own)
	float lightning_random()
	{
		static bool seeded = false;
		uint32_t &x = g.flashes.random;
		if (!seeded)
		{
			seeded = true;
			x ^= uint32_t(GetTickCount64() * 2654435761ull);
			x = x != 0 ? x : 0x6D2B79F5u;
		}
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		return float(x >> 8) / 16777216.0f;
	}

	// Lightning in thunder (thunder: 0.7, thunderstorms 1; 0 none) at now (GetTickCount64): a flash every few seconds to
	// about 20 s, at random (thunderstorms the more often), the first a few seconds after it starts; each a quick double or
	// triple pulse, each pulse rising in 15 ms and dying away over about a tenth of a second; the bolt with two flashes in
	// three in a thunderstorm, fewer in thunder, mostly within 50 degrees of where the camera looks. Into the look: the
	// flash's brightness now, the bolt's (brighter, as long). (mutex held)
	void next_flash(float thunder, ULONGLONG now, grass::look::weather_fx &x)
	{
		addon_state::lightning &f = g.flashes;
		x.flash = x.bolt = 0.0f;
		if (thunder <= 0.0f)
		{
			f.next = 0;
			f.pulses = 0;
			return;
		}
		const float storm = std::clamp((thunder - 0.7f) / 0.3f, 0.0f, 1.0f);
		if (f.next == 0)
			f.next = now + 1500 + ULONGLONG(3000.0f * lightning_random());
		if (now >= f.next)
		{
			f.start = now;
			f.pulses = lightning_random() < 0.45f ? 3u : 2u;
			f.pulse_at[0] = 0.0f;
			f.pulse_at[1] = 0.09f + 0.1f * lightning_random();
			f.pulse_at[2] = f.pulse_at[1] + 0.1f + 0.15f * lightning_random();
			f.pulse_strength[0] = 1.0f;
			f.pulse_strength[1] = 0.5f + 0.5f * lightning_random();
			f.pulse_strength[2] = 0.3f + 0.5f * lightning_random();
			f.bolt = lightning_random() < 0.4f + 0.27f * storm || g.flash_bolt; // (a manual flash, worldfx::flash_lightning: always its bolt)
			g.flash_bolt = false;
			const float aside = lightning_random() < 0.75f ? (lightning_random() - 0.5f) * 1.75f : (lightning_random() - 0.5f) * 6.2831853f;
			f.bearing = g.cam_bearing + aside;
			f.top = 0.18f + 0.2f * lightning_random();
			f.seed = std::floor(lightning_random() * 997.0f);
			f.count++;
			const float lo = 6000.0f + (4000.0f - 6000.0f) * storm, hi = 20000.0f + (14000.0f - 20000.0f) * storm;
			f.next = now + ULONGLONG(lo + (hi - lo) * lightning_random());
		}
		const float s = float(now - f.start) / 1000.0f;
		float light = 0.0f;
		for (uint32_t i = 0; i < f.pulses; ++i)
		{
			const float dt = s - f.pulse_at[i];
			if (dt >= 0.0f)
				light = std::max(light, f.pulse_strength[i] * std::min(dt / 0.015f, 1.0f) * std::exp(-std::max(dt - 0.015f, 0.0f) / 0.1f));
		}
		if (light < 0.004f)
			light = 0.0f;
		x.flash = thunder * light;
		x.bolt = f.bolt ? 2.5f * light : 0.0f;
		x.bolt_bearing = f.bearing;
		x.bolt_top = f.top;
		x.bolt_seed = f.seed;
	}

	// The weather of its own (look::wx) from the weather now (mutex held; after place_sun and the sky's dome): while World
	// FX draws all of the zone's weather (weather_all) and the weather is known, the weather's own look (weather_look: its
	// clouds, wind, dust, heat, aurora, gloom and stars, lit as its record lights the zone), each part times its setting
	// (the aurora the more the darker the night: hardly any by day); the wind blowing the rain's way (the way the game's own
	// clouds drifted there); the clouds and the fog banks drifting with it, the faster the harder it blows (wind and gales
	// racing); the lightning
	// (next_flash); the ground under the camera (where fog lies and heat shimmers): where the player stands when they're
	// sent and near the camera, else a couple of yalms under it, eased over a second or two so a step or a jump doesn't
	// move it. Nothing of it otherwise.
	void apply_weather_fx(bool fresh, const float eye[3])
	{
		grass::look::weather_fx &x = g.look.wx;
		x = grass::look::weather_fx();
		const grass::weather &w = g.weather;
		g.wx_on = fresh && weather_all();
		const ULONGLONG now = GetTickCount64();
		const double dt = g.wx_at != 0 ? std::min(double(now - g.wx_at) / 1000.0, 0.25) : 0.0;
		g.wx_at = now;
		if (!g.wx_on)
		{
			next_flash(0.0f, now, x);
			return;
		}
		grass::weather_look(w, g.look.dome, x);
		const grass::weather_kind_fx k = grass::weather_fx_of(w);
		x.wind[0] = std::sin(g.wind_bearing); // (apply_weather's: the rain's)
		x.wind[1] = std::cos(g.wind_bearing);
		if (!g.clouds_on)
			x.cloud_cover = x.cloud_thick = x.cloud_veil = 0.0f;
		// (the clouds lit by the game's light as it stands: the sun, full a little way up, still catching them just after it
		// sets (they're high); by night the moon, the more of it lit the more)
		x.cloud_quality = std::clamp(g.cloud_quality, 0, 2);
		x.cloud_style = std::clamp(g.cloud_style, 0, 1);
		x.cloud_light = std::clamp((-g.look.sun[1] + 0.04f) / 0.12f, 0.0f, 1.0f);
		if (g.hour_up < 0.0f)
			x.cloud_light *= 0.35f + 0.65f * std::clamp(g.look.dome.moon_lit, 0.0f, 1.0f);
		x.wind_strength *= g.weather_wind;
		// (the streaks: a dust storm's by its setting, a wind's by the wind's)
		x.streaks = k.streaks * (k.windy * g.weather_wind + (1.0f - k.windy) * g.weather_dust);
		x.leaves *= g.weather_wind;
		x.dust *= g.weather_dust;
		x.heat *= g.heat_shimmer;
		x.gloom *= g.gloom_fog;
		x.aurora *= g.aurora_amount * (0.15f + 0.85f * g.look.night);
		// The clouds' drift (the noise's units a second, the detail half as fast again) and the fog banks' (48 yalms a unit)
		const double cloud_speed = 0.008 * k.speed, bank_speed = (0.4 + 0.6 * std::min(k.wind, 2.0f)) / 48.0;
		for (int i = 0; i < 2; ++i)
		{
			const double way = i == 0 ? x.wind[0] : x.wind[1];
			g.cloud_drift[i] = std::fmod(g.cloud_drift[i] + way * cloud_speed * dt, 64.0);
			g.cloud_drift[2 + i] = std::fmod(g.cloud_drift[2 + i] + way * cloud_speed * 1.6 * dt, 64.0);
			g.bank_drift[i] = std::fmod(g.bank_drift[i] + way * bank_speed * dt, 256.0);
		}
		for (int i = 0; i < 4; ++i)
			x.cloud_drift[i] = float(g.cloud_drift[i]);
		x.bank_drift[0] = float(g.bank_drift[0]);
		x.bank_drift[1] = float(g.bank_drift[1]);
		// (a manual flash, worldfx::flash_lightning: as in a thunderstorm while it lasts, the Lightning setting or not)
		next_flash(now < g.flash_until ? std::max(k.flashes, 1.0f) : g.lightning_on ? k.flashes : 0.0f, now, x);
		// The ground under the camera
		float ground = eye[1] + 2.0f;
		if (g.actor_count != 0 && now - g.actors_at < 500)
		{
			const float *a = &g.actors[0];
			const float dx = a[0] - eye[0], dz = a[2] - eye[2];
			if (dx * dx + dz * dz < 15.0f * 15.0f && a[1] > eye[1] - 1.0f && a[1] < eye[1] + 12.0f)
				ground = a[1];
		}
		if (!g.ground_known || g.ground_zone != g.zone || !std::isfinite(g.ground_y))
			g.ground_y = ground;
		else
			g.ground_y += (ground - g.ground_y) * float(std::min(dt / 1.5, 1.0));
		g.ground_known = true;
		g.ground_zone = g.zone;
		x.ground_y = g.ground_y;
	}

	// The weather of its own in a line, for the panel and the crash report (mutex held)
	std::string weather_fx_text()
	{
		if (!g.weather_own)
			return "off (WeatherOwn): the game draws its own weather but for World FX's rain and snow";
		if (!g.sky_on)
			return "off: it needs the sky of its own (Sky)";
		if (!g.zone_sunlit)
			return "not in this zone (no open sky: the game draws its own weather here)";
		if (!g_has_drawn)
			return "on; not drawing yet (the game's world not found)";
		if (!weather_all())
			return g.weather.known && g.weather_at != 0 && GetTickCount64() - g.weather_at <= kWeatherAllMs ?
				"on; the zone's record has no sky dome to repaint: the game draws its own weather here" :
				"on; the weather isn't known (not sent lately): the game draws its own";
		if (!g.wx_on)
			return "on; the weather isn't known yet";
		const grass::look::weather_fx &x = g.look.wx;
		char s[400];
		std::snprintf(s, sizeof(s), "all of the zone's weather its own; clouds %.0f%% cover%s, wind %.2f%s, lightning %s (%u flashes), dust %.2f, heat %.2f, aurora %.2f, "
			"fog banks %.2f, stars x%.1f; last frame: %s%s%s",
			x.cloud_cover * 100.0f, g.clouds_on ? "" : " (off)", x.wind_strength, x.streaks > 0.0f ? " with streaks" : "",
			g.flashes.next != 0 ? "coming" : "none", g.flashes.count, x.dust, x.heat, x.aurora, x.gloom, x.star_boost,
			g.last.weather ? "drawn" : "not drawn", g.last.air ? ", the air pass" : "", g.last.shimmer ? ", shimmer" : "");
		return s;
	}

	// The grass's, the fireflies' and the water's fog, and how much of the sun's shadows the weather leaves, from the
	// weather now (mutex held). Logged when the weather shown changes.
	void apply_weather()
	{
		g.weather = weather_forced_over(g.weather_real); // (a forced weather's fade moves on each frame)
		const bool fresh = weather_fresh();
		const grass::weather &w = g.weather;
		g.look.haze = g.weather_fog && fresh && w.fog;
		for (int i = 0; i < 3; ++i)
			g.look.haze_color[i] = w.fog_color[i];
		g.look.haze_start = w.fog_start;
		g.look.haze_end = w.fog_end;
		// The sun's (and moon's) shadows the weather leaves: the game's sun light in the record now against the zone's
		// clear sky's at this hour (the game dims its sun under cloud and rain by that much); the last share while the
		// record isn't read (the first frames in a zone: the game lights it with a stand-in record then)
		const grass::clear_sky *sky = g.zone < g.clear_skies.size() && g.clear_skies[g.zone].known ? &g.clear_skies[g.zone] : nullptr;
		g.clear_light = sky != nullptr ? grass::clear_sky_light(*sky, vanadiel_hour()) : 0.0f;
		if (fresh && w.record)
			g.weather_share = grass::weather_sunlight(w, g.clear_light);
		g.look.weather_sun = g.weather_shadows && fresh ? g.weather_share : 1.0f;
		// The ground as wet as the rain, drying over kDryingSeconds after it stops; dry while the weather isn't known.
		// Its highlight in the zone's light (the record's sun or moon colour times its intensity, over 2 as the game's
		// modulate-2x lighting), the sky it mirrors the fog's colour (an overcast sky's)
		const ULONGLONG now = GetTickCount64();
		const float dt = g.wet_at != 0 ? std::min(float(now - g.wet_at) / 1000.0f, 1.0f) : 0.0f;
		g.wet_at = now;
		const float rain = fresh ? grass::weather_rain(w) : 0.0f;
		// (rain that starts while you watch wets the ground over kWettingSeconds; already raining when the weather
		// becomes known (logging in), it's wet at once; in a zone just entered, as its weather has it at once)
		const bool zone_new = now - g.zone_entered_at < kZoneWeatherMs;
		if (!fresh)
			g.wetness = 0.0f;
		else if (zone_new)
			g.wetness = rain;
		else if (rain >= g.wetness)
			g.wetness = g.wet_weather_known ? std::min(rain, g.wetness + dt / kWettingSeconds) : rain;
		else
			g.wetness = std::max(rain, g.wetness - dt / kDryingSeconds);
		g.wet_weather_known = fresh;
		g.look.wet = g.zone_sunlit ? g.wet_ground * g.wetness : 0.0f; // (only under open sky, as the sun's shadows)
		// The add-on's rain: as hard as the weather's (blended across a change, as the game blends its lights), slanted
		// by its wind, which blows the way the game's own clouds drifted in that zone and weather (grass::wind_bearing; the
		// clouds, grass, streaks and leaves of its own go the same way); the light the drops catch, the overcast sky's
		// (the game's fog colour), a little brighter. Only in zones with a rain map (with rain of their own: where the game
		// shows rain for the server's).
		const float downpour = fresh && g.rain_on ? grass::weather_downpour(w) : 0.0f;
		g.raining = downpour;
		g.look.rain = g.renderer != nullptr && g.renderer->rain_loaded() ? downpour * g.rain_amount : 0.0f;
		const float slant = fresh ? grass::weather_slant(w) : 0.0f;
		g.wind_bearing = grass::wind_bearing(g.zone, w);
		g.look.rain_slant[0] = slant * std::sin(g.wind_bearing);
		g.look.rain_slant[1] = slant * std::cos(g.wind_bearing);
		g.look.rain_splashes = g.rain_splashes;
		g.look.rain_rings = g.rain_rings;
		for (int i = 0; i < 3; ++i)
			g.look.rain_light[i] = std::clamp((g.look.haze ? g.look.haze_color[i] : g.look.sky[i]) * 1.25f + 0.05f, 0.0f, 1.5f);
		// The add-on's snow: as hard as the weather's, blended the same way, slanted by the same wind (blizzards blow it
		// hard across); its flakes white in the same overcast light, brighter than the drops
		const float snowfall = fresh && g.snow_on ? grass::weather_snowfall(w) : 0.0f;
		g.snowing = snowfall;
		g.look.snow = g.renderer != nullptr && g.renderer->rain_loaded() ? snowfall * g.snow_amount : 0.0f;
		g.look.snow_sway = fresh ? grass::weather_sway(w) : 0.0f;
		// The snow that settles: towards the weather's (snow 0.84 of a whole layer, a blizzard all of it) over
		// kSnowSettleSeconds, melting over kSnowMeltSeconds after; already snowing when the weather becomes known (logging
		// in), it lies there at once; in a zone just entered, as its weather has it at once (none from the last zone's).
		// Only with the snow of its own on: switched off, none lies (gone at once).
		{
			const float target = fresh && g.snow_on ? std::min(grass::weather_snowfall(w) * 1.4f, 1.0f) : 0.0f;
			const float sdt = g.snow_at != 0 ? std::min(float(now - g.snow_at) / 1000.0f, 1.0f) : 0.0f;
			g.snow_at = now;
			if (!fresh || !g.snow_on)
				g.snow_cover = 0.0f;
			else if (zone_new)
				g.snow_cover = target;
			else if (target >= g.snow_cover)
				g.snow_cover = g.snow_weather_known ? std::min(target, g.snow_cover + sdt / kSnowSettleSeconds) : target;
			else
				g.snow_cover = std::max(target, g.snow_cover - sdt / kSnowMeltSeconds);
			g.snow_weather_known = fresh;
			g.look.snow_cover = g.renderer != nullptr && g.renderer->rain_loaded() ? g.snow_cover_amount * g.snow_cover : 0.0f;
			g.look.snow_depth = g.snow_depth;
			g.look.snow_trails = g.snow_trails;
			g.look.snowfall = target; // (trails fill back in as hard as it snows)
		}
		for (int i = 0; i < 3; ++i)
			g.look.snow_light[i] = std::clamp((g.look.haze ? g.look.haze_color[i] : g.look.sky[i]) * 1.45f + 0.12f, 0.0f, 1.6f);
		// Light in the air (look::vol_*), under open sky: clear air carries the shafts, more of it in mist, rain, snow and
		// cloud, and lying thicker at dawn and dusk; the shafts as strong as the sun gets through (none under a whole
		// overcast), in its colour (the moon's by night, fainter); the air in the fog's colour, or the sky's
		{
			const grass::weather_kind_fx x = fresh ? grass::weather_fx_of(w) : grass::weather_kind_fx();
			const float snow_now = fresh ? grass::weather_snowfall(w) : 0.0f;
			const float low = 1.0f - std::clamp(std::fabs(g.hour_up) / 0.3f, 0.0f, 1.0f);
			const float thicker = 1.0f + low * low * (3.0f - 2.0f * low); // (twice at sunrise and sunset)
			const bool any = g.zone_sunlit && (g.vol_light > 0.0f || g.vol_fog > 0.0f);
			const float extra = 0.012f * x.veil + 0.005f * rain + 0.007f * snow_now + 0.003f * x.cover;
			g.look.vol_density = any ? (0.0016f + g.vol_fog * extra) * thicker : 0.0f;
			const float through = (1.0f - 0.85f * std::clamp(x.cover, 0.0f, 1.0f)) * (fresh && w.record ? std::clamp(g.weather_share, 0.0f, 1.0f) : 1.0f);
			g.look.vol_light = any ? g.vol_light * 0.35f * through * (g.hour_up < 0.0f ? 0.5f : 1.0f) : 0.0f;
			for (int i = 0; i < 3; ++i)
			{
				g.look.vol_light_color[i] = g.look.sun_color[i];
				g.look.vol_air[i] = g.look.haze ? g.look.haze_color[i] : g.look.sky[i];
			}
			g.look.vol_steps = uint32_t(16 + 8 * std::clamp(g.vol_quality, 0, 2));
		}
		for (int i = 0; i < 3; ++i)
		{
			g.look.wet_light[i] = w.record ? std::min(w.sun_color[i] * w.intensity * 0.5f, 1.5f) : g.look.sun_color[i];
			g.look.wet_sheen[i] = g.look.haze ? g.look.haze_color[i] : g.look.sky[i];
		}
		// The sea's reach (look::sea_distance): out to where the game's fog ends (the world is all fog past it), or the zone's
		// drawn distance without the fog read; 1000 yalms while the weather isn't known; never nearer than the water's own
		{
			float reach = 1000.0f;
			if (fresh && w.fog && w.fog_end > 0.0f)
				reach = w.fog_end;
			else if (fresh && w.record && w.draw_distance > 0.0f)
				reach = w.draw_distance;
			g.look.sea_distance = std::clamp(reach, g.look.water_distance, 8000.0f);
		}
		// The add-on's own sky: from the record the zone is lit by now (its sky part: the gradient the game draws its dome
		// from, which the shading pass repaints smooth), only where the game draws a dome (segments > 2: none in dungeons
		// and interiors). Its sun by day toward the game's light (cut by the horizon in the shader), the moon by night once
		// the sun is well down (the game's light is the moon then; through twilight it swings from one to the other: no
		// disc), the stars from a little after sunset; both fade with the weather: as the game dims its sun (the share
		// worked out for the shadows, 1 at night) and as its fog closes in (the record's own fog end, before the draw
		// distance setting: clear sunshine about 400 yalms, cloud 250, mist and rain 100).
		grass::look::sky_dome &d = g.look.dome;
		d.on = g.sky_on && fresh && w.record && w.sky_known && w.sky_segments > 2;
		if (d.on)
		{
			d.segments = w.sky_segments;
			for (int i = 0; i < 8; ++i)
			{
				for (int k = 0; k < 3; ++k)
					d.stops[i][k] = w.sky_colors[i][k];
				d.stops[i][3] = w.sky_heights[i];
			}
			for (int k = 0; k < 3; ++k)
				d.clear[k] = w.sky_clear[k];
			const float fog_clear = std::clamp((w.record_fog_end - 100.0f) / 200.0f, 0.0f, 1.0f);
			const float clarity = fog_clear * fog_clear * (3.0f - 2.0f * fog_clear) * std::clamp(g.weather_share, 0.0f, 1.0f);
			const float up = g.hour_up;
			const float moon_in = std::clamp((-up - 0.1f) / 0.1f, 0.0f, 1.0f);
			d.moon = up < 0.0f ? 1.0f : 0.0f;
			d.sun = g.light_up ? g.sky_sun * clarity * (up >= 0.0f ? 1.0f : moon_in * (g.sun_from_game ? 1.0f : 0.0f)) : 0.0f;
			static const float low_sun[3] = { 1.0f, 0.62f, 0.38f }, high_sun[3] = { 1.0f, 0.95f, 0.85f }, moon_light[3] = { 0.8f, 0.85f, 1.0f };
			const float high = std::clamp(up / 0.4f, 0.0f, 1.0f);
			for (int k = 0; k < 3; ++k)
				d.disc[k] = up >= 0.0f ? low_sun[k] + (high_sun[k] - low_sun[k]) * high : moon_light[k];
			d.stars = g.sky_stars * clarity * std::clamp((-up - 0.02f) / 0.15f, 0.0f, 1.0f);
			d.star_turn = float(vanadiel_hour() / 24.0 * 6.283185307179586);
			// (the moon's phase, its lit side as a northern sky has it: toward the west, the evening sun's side, while
			// it waxes, toward the east while it wanes; the world's x is east)
			const moon_phase m = moon_now();
			d.moon_lit = m.lit;
			d.moon_side[0] = m.waxing ? -1.0f : 1.0f;
			d.moon_side[1] = d.moon_side[2] = 0.0f;
			d.debug = g.sky_debug;
		}
		// (logged when the weather shown or the zone changes, and again when its record comes in: entering a zone, the first
		// read has none, the game lighting it with a stand-in record, the real one comes a frame later. A record dropping
		// out for a moment under the same weather isn't logged.)
		const uint64_t logged = uint64_t(w.name) | uint64_t(w.record ? 1 : 0) << 32 | uint64_t(g.zone) << 33;
		const bool same_weather = (logged & ~(1ull << 32)) == (g.weather_logged & ~(1ull << 32));
		if (fresh && logged != g.weather_logged && !(same_weather && !w.record))
		{
			g.weather_logged = logged;
			log_message(0, ("FFXI World FX: the weather: " + weather_text()).c_str());
		}
	}

	// Whether the look (tone mapping, bloom, the colour grade) is on: it runs at the world's end with or without the rest
	bool look_on()
	{
		return g.tonemap || g.bloom > 0.0f || g.colour_grade > 0.0f;
	}

	// The look over the world as it ends (mutex held; after place_sun, apply_weather and apply_weather_fx): its settings,
	// the exposure kept from brightening anything by night (the night as the game has it), and the colour grade from the
	// hour and the weather (grass::colour_grade). A new zone sets the exposure at once.
	void place_look()
	{
		grass::look::look_pass &p = g.look.post;
		p = grass::look::look_pass();
		p.tonemap = g.tonemap;
		p.bloom = std::isfinite(g.bloom) ? std::clamp(g.bloom, 0.0f, 2.0f) : 0.0f;
		p.snap = g.zone != g.look_zone;
		g.look_zone = g.zone;
		p.exposure_max += (1.0f - p.exposure_max) * std::clamp(g.look.night, 0.0f, 1.0f);
		grass::colour_grade(g.hour_up, g.look.night, g.zone_sunlit, weather_fresh() && g.weather.known ? &g.weather : nullptr, g.colour_grade, p, g.grade_parts);
	}

	// Whether the shading pass (ambient occlusion, surface relief, sun shadows, lamps) is on: it runs with or without grass
	bool shading_on()
	{
		return g.look.ao_strength > 0.0f || g.look.relief > 0.0f || g.sun_shadows > 0.0f || lamps_on() || g.sky_on ||
			(g.wet_ground > 0.0f && (g.wetness > 0.0f || (weather_fresh() && grass::weather_rain(g.weather) > 0.0f))) ||
			(g.snow_on && g.snow_cover_amount > 0.0f && (g.snow_cover > 0.0f || (weather_fresh() && grass::weather_snowfall(g.weather) > 0.0f)));
	}

	// Lamps: lights reaching further than this (yalms) are a whole area's fill light (dungeons have many, reaching
	// 30 to 800), not a lamp: the game's own light covers those, and a glow that wide would tint everything. A glow
	// reaches the lamp's range (at most kLampMostRange) times the reach setting. Lamps further from the camera
	// than kLampFar aren't drawn (faded out over the last kLampFade yalms before it).
	constexpr float kLampMaxRange = 20.0f, kLampMostRange = 12.0f, kLampFar = 150.0f, kLampFade = 30.0f;

	// The lamps lit now, near the camera and in view, for the renderer (mutex held): each baked lamp the game's light
	// table has lit (with its brightness and where it is now: a few move), and with lamps_unbaked the lit lights the
	// bake doesn't know (ids with no generator of their name in the zone's DAT, lights of effects); not lights the
	// game keeps for characters only (ids starting with 'c') nor whole-area ones (kLampMaxRange). How strong a glow
	// is grows with the lamp's brightness and levels off (the game's light is colour x brightness, mostly 2-20, but
	// saturates within its range at any of those), and dims with the game's light below the lamp's full brightness, as
	// its curve over the day ramps at dusk and dawn (lamp_glow.hpp). The nearest kMaxLamps in view are kept, the furthest of them
	// fading out as the next one nears, so none pops in or out. The glows join them (their own strength settings;
	// an effect's within kGlowByLamp of a lit lamp left out). Fills g.lamp_upload (8 floats each: x, y, z, reach,
	// colour x strength, 1 when it casts lamp shadows: the zone's lights do, glows don't); returns how many.
	// Into ReShade.log (mutex held): when the characters from the Ashita addon stop coming or come again (lamp shadows
	// go and come with them; at most once in 10 s), and once a zone, 3 s after the game says a character is in, how many
	// came and how far the nearest stands from the camera: a character sent where the game doesn't draw it shows there
	void log_people(const grass::camera &cam, bool people)
	{
		const ULONGLONG now = GetTickCount64();
		if (people != g.people)
		{
			g.people = people;
			if (now - g.people_logged_at >= 10000)
			{
				g.people_logged_at = now;
				log_message(0, people ? "FFXI World FX: characters from the worldfx addon again: lamp shadows back on"
					: "FFXI World FX: no characters from the worldfx addon (none sent, or none for half a second): no lamp shadows until there are");
			}
		}
		if (g.zone == g.people_zone_logged || !g.in_game || now - g.in_game_since < 3000)
			return;
		g.people_zone_logged = g.zone;
		char text[160];
		if (!people)
			std::snprintf(text, sizeof(text), "FFXI World FX: zone %u: no characters from the worldfx addon (no lamp shadows)", g.zone);
		else
		{
			float eye[3];
			grass::eye_from_view(cam.view, eye);
			float nearest = 1e30f;
			for (uint32_t i = 0; i < g.actor_count; ++i)
			{
				const float *a = &g.actors[i * 4];
				nearest = std::min(nearest, std::sqrt((a[0] - eye[0]) * (a[0] - eye[0]) + (a[1] - eye[1]) * (a[1] - eye[1]) + (a[2] - eye[2]) * (a[2] - eye[2])));
			}
			std::snprintf(text, sizeof(text), "FFXI World FX: zone %u: %u characters and monsters from the worldfx addon, the nearest %.1f yalms from the camera",
				g.zone, g.actor_count, nearest);
		}
		log_message(0, text);
	}

	constexpr float kGlowByLamp = 2.0f;
	uint32_t pick_lamps(const grass::camera &cam)
	{
		g.lamps_lit = g.lamps_unbaked_used = g.lamps_area = g.lamps_picked = 0;
		g.glows_used = g.glows_by_lamps = 0;
		if (!lamps_on())
			return 0;
		float eye[3];
		grass::eye_from_view(cam.view, eye);
		const float p11 = cam.proj[0], p22 = cam.proj[5];
		const float side_x = std::sqrt(p11 * p11 + 1.0f), side_y = std::sqrt(p22 * p22 + 1.0f);
		struct candidate { float d, x, y, z, reach, r, g, b; bool glow; };
		std::vector<candidate> found;
		found.reserve(g.live_lights.size() + g.glows.size());
		// A light near enough and in view (its glow's sphere against the view's sides: view space, row vectors, in
		// front is -z) as a candidate, its colour times its strength; false if not
		auto consider = [&](float x, float y, float z, float range, const float colour[3], float strength, bool glow) {
			const float reach = std::clamp(range, 1.0f, kLampMostRange) * g.lamp_reach;
			const float dx = x - eye[0], dy = y - eye[1], dz = z - eye[2];
			const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (d > kLampFar + reach)
				return false;
			float v[3];
			for (int k = 0; k < 3; ++k)
				v[k] = x * cam.view[0 * 4 + k] + y * cam.view[1 * 4 + k] + z * cam.view[2 * 4 + k] + cam.view[3 * 4 + k];
			if (v[2] > reach || (std::fabs(v[0]) * p11 + v[2]) / side_x > reach || (std::fabs(v[1]) * p22 + v[2]) / side_y > reach)
				return false;
			found.push_back({ d, x, y, z, reach,
				std::clamp(colour[0], 0.0f, 2.0f) * strength, std::clamp(colour[1], 0.0f, 2.0f) * strength, std::clamp(colour[2], 0.0f, 2.0f) * strength, glow });
			return true;
		};
		const bool lamps = g.lamp_glow > 0.0f && lights_fresh();
		for (const addon_state::live_light &light : g.live_lights)
		{
			if (!lamps)
				break;
			const auto at = g.lamp_by_id.find(light.id);
			const addon_state::baked_lamp *baked = at != g.lamp_by_id.end() ? &g.lamps[at->second] : nullptr;
			const char first = static_cast<char>(light.id & 0xFF);
			if ((baked != nullptr && (baked->flags & 1) != 0) || first == 'c' || first == 'C')
				continue;
			if (baked == nullptr && !g.lamps_unbaked)
				continue;
			// (the baked colour: as the zone made it; the game's table has the same once lit)
			const float colour[3] = { baked != nullptr ? baked->r : light.r, baked != nullptr ? baked->g : light.g, baked != nullptr ? baked->b : light.b };
			if (light.range > kLampMaxRange)
			{
				g.lamps_area++;
				continue;
			}
			if (baked != nullptr)
				g.lamps_lit++;
			// (as bright as the game's light for it: grass::lamp_glow_strength, by its brightness now and fully lit)
			const float strength = grass::lamp_glow_strength(g.lamp_glow, light.brightness, baked != nullptr ? baked->brightness : 0.0f);
			if (consider(light.x, light.y, light.z, light.range, colour, strength, false) && baked == nullptr)
				g.lamps_unbaked_used++;
		}
		if (glows_on())
			for (const addon_state::glow_light &glow : g.glows)
			{
				const float setting = glow.kind == 2 ? g.fixture_glow : g.effect_glow;
				if (setting <= 0.0f)
					continue;
				// (an effect's glow by a lit lamp: that lamp's flames, which it lights already)
				bool by_lamp = false;
				if (glow.kind == 3 && lights_fresh())
					for (const addon_state::live_light &light : g.live_lights)
					{
						const float dx = light.x - glow.x, dy = light.y - glow.y, dz = light.z - glow.z;
						if (light.range <= kLampMaxRange && dx * dx + dy * dy + dz * dz < kGlowByLamp * kGlowByLamp)
						{
							by_lamp = true;
							break;
						}
					}
				if (by_lamp)
				{
					g.glows_by_lamps++;
					continue;
				}
				const float colour[3] = { glow.r, glow.g, glow.b };
				consider(glow.x, glow.y, glow.z, glow.range, colour, setting * (1.0f - std::exp(-glow.brightness / 4.0f)), true);
			}
		std::sort(found.begin(), found.end(), [](const candidate &a, const candidate &b) { return a.d < b.d; });
		const float next = found.size() > grass::kMaxLamps ? found[grass::kMaxLamps].d : 1e9f;
		uint32_t n = 0;
		for (const candidate &c : found)
		{
			if (n == grass::kMaxLamps)
				break;
			const float weight = std::clamp((next - c.d) / 8.0f, 0.0f, 1.0f) * std::clamp((kLampFar - c.d) / kLampFade, 0.0f, 1.0f);
			if (weight <= 0.0f)
				continue;
			float *o = &g.lamp_upload[n++ * 8];
			o[0] = c.x, o[1] = c.y, o[2] = c.z, o[3] = c.reach;
			o[4] = c.r * weight, o[5] = c.g * weight, o[6] = c.b * weight;
			o[7] = c.glow ? 0.0f : 1.0f; // (casts lamp shadows: the zone's lights do, a Home Point's or an effect's glow doesn't)
			g.glows_used += c.glow;
		}
		g.lamps_picked = n;
		return n;
	}

	// The Vana'diel hour (0..24): a Vana'diel minute is 2.4 Earth seconds from 2002-01-01 00:00 JST (Unix
	// 1009810800), as the server counts it (LSB common/vanadiel_clock.h)
	double vanadiel_hour()
	{
		if (g.test_hour >= 0.0f) // (TestVanadielHour in ReShade.ini: the end-to-end test's fixed hour)
			return g.test_hour;
		FILETIME ft;
		GetSystemTimeAsFileTime(&ft);
		const double unix_seconds = ((uint64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000ULL) / 1e7;
		const double minutes = (unix_seconds - 1009810800.0) / 2.4;
		return std::fmod(minutes, 1440.0) / 60.0;
	}

	// The moon as the game has it (research/ffxi-re/notes-moon-phase.md): from its Vana'diel clock t (Earth seconds),
	// day d = t / 3456 mod 84 of the moon's cycle, the picture (t / 24192) mod 12 (FFXiMain 103B60: 7 days each) and how
	// lit, 0..100 (103BA0): none on day 4 (new), whole on day 46 (full), in a straight line between, up while waxing
	// (days 4 to 46), down while waning. Here as smooth as the clock: lit 0..1, waxing or not, and how far through the
	// cycle (0..1). The game's clock when it's been sent in the last 5 seconds, else the PC's: the same count from
	// 2002-01-01 00:00 JST (Unix 1009810800), as the server keeps it (LSB vana_time.h moon::get_phase agrees).
	moon_phase moon_now()
	{
		moon_phase m;
		double day = 0.0;
		if (g.test_moon >= 0.0f) // (TestMoon in the settings: the end-to-end test's fixed moon)
			day = std::fmod(double(g.test_moon), 1.0) * 84.0;
		else
		{
			const ULONGLONG now = GetTickCount64();
			double t = 0.0;
			if (g.moon_clock != 0 && now - g.moon_clock_at < 5000)
			{
				t = double(g.moon_clock) + double(now - g.moon_clock_at) / 1000.0;
				m.from_game = true;
			}
			else
			{
				FILETIME ft;
				GetSystemTimeAsFileTime(&ft);
				t = ((uint64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000ULL) / 1e7 - 1009810800.0;
			}
			day = std::fmod(t / 3456.0, 84.0);
		}
		m.cycle = float(day / 84.0);
		m.waxing = day >= 4.0 && day < 46.0;
		m.lit = float(std::clamp(day < 4.0 ? (4.0 - day) / 42.0 : day < 46.0 ? (day - 4.0) / 42.0 : (88.0 - day) / 42.0, 0.0, 1.0));
		return m;
	}

	// Toward the sun as the game lights the world: its environment object's light direction (+0x14C, toward
	// the ground) turned round; false when it can't be read or isn't a direction (mutex held)
	bool read_game_sun(float out[3])
	{
		if (!g.env_scanned)
		{
			g.env_scanned = true;
			g.env_global = find_env_global();
		}
		uint32_t env = 0;
		float dir[3] = {};
		if (g.env_global == 0 || !safe_copy(&env, g.env_global, 4) || env == 0 || !safe_copy(dir, env + 0x14C, sizeof(dir)))
			return false;
		const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
		if (!std::isfinite(len) || len < 0.5f || len > 2.0f)
			return false;
		for (int i = 0; i < 3; ++i)
			out[i] = -dir[i] / len;
		return true;
	}

	// The sun from the hour: up at 6 in the east, high in the south at noon, down at 18 in the west; the
	// shadows' strength fades in and out near the horizon, none at night or without open sky. Its direction
	// is the game's own when that's known and above the horizon (mutex held).
	void place_sun()
	{
		const double a = 3.14159265358979 * (vanadiel_hour() - 6.0) / 12.0;
		const double up = std::sin(a);
		float sun[3] = { float(std::cos(a)), float(-up), float(-0.35 * up) };
		const float len = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
		for (int i = 0; i < 3; ++i)
			g.look.sun[i] = g.hour_sun[i] = sun[i] / len;
		g.sun_from_game = read_game_sun(g.game_sun);
		if (g.sun_from_game && g.game_sun[1] < -0.05f) // (y is down)
			for (int i = 0; i < 3; ++i)
				g.look.sun[i] = g.game_sun[i];
		// (in the log at first and every 5 minutes, both ways: how the game's light moves with the hour)
		const ULONGLONG now = GetTickCount64();
		if (g.sun_logged_at == 0 || now - g.sun_logged_at > 300000)
		{
			g.sun_logged_at = now;
			const double h = vanadiel_hour();
			char text[200];
			if (g.sun_from_game)
				std::snprintf(text, sizeof(text), "FFXI World FX: Vana'diel %02d:%02d, toward the sun: the game's light %.3f %.3f %.3f, from the hour %.3f %.3f %.3f",
					int(h), int((h - int(h)) * 60.0), g.game_sun[0], g.game_sun[1], g.game_sun[2], g.hour_sun[0], g.hour_sun[1], g.hour_sun[2]);
			else
				std::snprintf(text, sizeof(text), "FFXI World FX: Vana'diel %02d:%02d, the game's light not found (%s): the sun from the hour",
					int(h), int((h - int(h)) * 60.0), g.env_global == 0 ? "no pattern match in FFXiMain.dll" : "no environment object");
			log_message(0, text);
		}
		g.hour_up = float(up);
		g.light_up = g.look.sun[1] < -0.05f;
		const float rise = float(std::clamp(up * 3.0, 0.0, 1.0));
		// By night the game's light is the moon (it uses the sun's slots): its shadows while it's up, at
		// moon_shadows of the sun's strength, faded out through twilight on both sides (the light swings from
		// one to the other then). Without the game's light, none at night as before.
		const bool moon = up < 0.0 && g.sun_from_game && g.game_sun[1] < -0.05f;
		const float moon_up = moon ? std::clamp(-g.game_sun[1] * 3.0f, 0.0f, 1.0f) * float(std::clamp(-up * 3.0, 0.0, 1.0)) : 0.0f;
		g.moon_now = moon_up * g.moon_shadows > 0.0f;
		g.look.sun_shadow = g.zone_sunlit ? g.sun_shadows * std::max(rise, g.moon_shadows * moon_up) : 0.0f;
		// How deep into the night (fireflies): from sunset, full a little before 19:00, gone again by sunrise; only
		// under open sky
		g.look.night = g.zone_sunlit ? float(std::clamp(-up / 0.2, 0.0, 1.0)) : 0.0f;
		// For the water: the sky it reflects when none is on screen (blue by day, warm at sunrise and sunset,
		// dark at night; a dim grey without open sky), and the sun's glints, warmer low down
		auto mix = [](const float *a, const float *b, double k, float *out) {
			const float f = float(std::clamp(k, 0.0, 1.0));
			for (int i = 0; i < 3; ++i)
				out[i] = a[i] + (b[i] - a[i]) * f;
		};
		static const float day[3] = { 0.42f, 0.56f, 0.72f }, dusk[3] = { 0.55f, 0.42f, 0.38f }, night[3] = { 0.035f, 0.045f, 0.08f };
		static const float low_sun[3] = { 1.0f, 0.6f, 0.35f }, high_sun[3] = { 1.0f, 0.95f, 0.85f };
		if (!g.zone_sunlit)
			g.look.sky[0] = g.look.sky[1] = g.look.sky[2] = 0.08f;
		else if (up >= 0.0)
			mix(dusk, day, up / 0.35, g.look.sky);
		else
			mix(dusk, night, -up / 0.2, g.look.sky);
		mix(low_sun, high_sun, up / 0.4, g.look.sun_color);
		g.look.glint = g.zone_sunlit ? rise : 0.0f;
		// (and the moon's glints on the water, pale and faint)
		if (moon_up > 0.0f && g.zone_sunlit)
		{
			static const float moon_light[3] = { 0.62f, 0.68f, 0.85f };
			for (int i = 0; i < 3; ++i)
				g.look.sun_color[i] = moon_light[i];
			g.look.glint = 0.35f * moon_up;
		}
	}

	// Draws grass on the list into the world (bound through rtv and dsv), then binds dgVoodoo's state again (mutex held)
	// The frame's look (mutex held): the clock, the sun and weather, those near, the grass's wear and cuts, ripples and
	// lamps; once a frame, where the first of its draws is (the blades at the zone's end, else the world's end)
	void frame_look(const grass::camera &cam)
	{
		g.look.time = static_cast<float>(GetTickCount64() % 3600000) / 1000.0f; // wraps hourly: floats stay precise
		// (actors not sent for half a second: the Ashita addon is gone, nobody pushes)
		const bool fresh = GetTickCount64() - g.actors_at < 500;
		g.look.grass = g.enabled;
		place_sun();
		apply_weather();
		{
			// (where the camera looks, as a bearing: the view's third column is toward the camera's back)
			float eye[3];
			grass::eye_from_view(cam.view, eye);
			g.cam_bearing = std::atan2(-cam.view[2], -cam.view[10]);
			apply_weather_fx(weather_fresh(), eye);
		}
		place_look();
		g.look.actors = g.actors;
		g.look.actor_count = fresh ? g.actor_count : 0;
		// The grass's wear: trodden down by those near (more where they fight) and cut by the weapon skills sent lately
		{
			const ULONGLONG ms = GetTickCount64();
			g.look.clock = ms / 1000.0;
			g.look.wear = g.grass_wear;
			g.look.fighting = fresh ? g.fighting : 0;
			uint32_t n = 0;
			for (uint32_t i = 0; g.grass_cuts && i < grass::kMaxCuts; ++i)
			{
				const auto &c = g.cuts[i];
				const double age = (ms - c.at) / 1000.0;
				if (c.at == 0 || age > grass::kCutKeep)
					continue;
				float *q = &g.cut_upload[n * 8];
				std::memcpy(q, c.swath, sizeof(c.swath));
				q[7] = float(age);
				n++;
			}
			g.look.cuts = g.cut_upload;
			g.look.cut_count = n;
		}
		g.look.ripples = g.ripple_upload;
		g.look.ripple_count = ripples_for_drawing(GetTickCount64());
		g.look.lamps = g.lamp_upload;
		g.look.lamp_count = pick_lamps(cam);
		// Lamp shadows need the characters near (the Ashita addon's): the march leaves them out, and without them it
		// laid a stepped copy of a character's silhouette away from the lamp (a Jeuno Mog House, 2026-10-21). So none
		// while the addon sends no one (log_people says so in ReShade.log)
		const bool people = fresh && g.actor_count > 0;
		g.look.lamp_shadows = people ? g.lamp_shadows : 0.0f;
		log_people(cam, people);
	}

	void inject(ID3D12GraphicsCommandList *list, list_data &d, uint64_t rtv, uint64_t dsv)
	{
		g.tried = true;
		g.passes_wanted++;
		g_has_drawn = true; // (the world found: World FX draws into it here, whatever is on this frame)
		if (!g.in_game)
		{
			g.skip = "not in the game (login, character select, zoning; or the worldfx Ashita addon isn't loaded)";
			return;
		}
		if (g.passes >= kMaxPasses)
		{
			g.skip = "the world ended more than twice this frame (the rest skipped)";
			return;
		}
		if (g.logging_moves && g.moves.size() < 1500)
		{
			// (with the world's states as this list knows them: the game may have changed them on another list)
			char item[64];
			std::snprintf(item, sizeof(item), " [GRASS rt 0x%X ds 0x%X]", static_cast<unsigned>(d.world_rt_state), static_cast<unsigned>(d.world_ds_state));
			g.moves += item;
		}
		g.skip = nullptr;
		const bool water_on = (g.look.water || g.look.water_debug) && g.renderer != nullptr && g.renderer->water_loaded();
		const bool rain_on = (g.rain_on || g.snow_on) && g.renderer != nullptr && g.renderer->rain_loaded();
		if (!g.enabled && !shading_on() && !water_on && g.look.fireflies <= 0.0f && !rain_on && !g.vis_on && !look_on())
		{
			g.skip = "off";
			return;
		}
		if (g.renderer == nullptr || !g.renderer->ready())
		{
			g.skip = "the renderer isn't ready";
			return;
		}
		if (!g.renderer->loaded() && !shading_on() && !water_on && !rain_on && !g.vis_on && !look_on())
		{
			g.skip = "no blades for this zone";
			return;
		}
		if (d.state.root == nullptr || d.state.pipeline == nullptr)
		{
			g.skip = "dgVoodoo's root signature or pipeline wasn't seen on this list";
			return;
		}
		grass::camera cam;
		if (const char *why = read_camera(cam))
		{
			g.skip = why;
			return;
		}
		g.passes++;

		grass::target t;
		t.rt = g.world_rt;
		t.ds = g.world_ds;
		t.rtv = { static_cast<SIZE_T>(rtv) };
		t.rtv_format = view_format(rtv, g.world_rt);
		t.dsv_format = view_format(dsv, g.world_ds);
		t.rt_state = d.world_rt_state;
		t.ds_state = d.world_ds_state;
		if (!g.logged_first)
			log_first(d, t);

		std::string error;
		// (the frame's look: worked out at the zone's end when the blades went in there, else here)
		const bool blades_went_in = g.blades_at_marker;
		g.blades_at_marker = false;
		if (!g.look_at_marker)
			frame_look(cam);
		g.look_at_marker = false;
		// The shading pass's safeguard (kShadeMaxMs)
		const ULONGLONG now = GetTickCount64();
		if (g.renderer->shading_ms() > (g.test_shade_max_ms > 0.0f ? g.test_shade_max_ms : kShadeMaxMs) && now >= g.shading_paused_until)
		{
			g.shading_paused_until = now + kShadePauseMs;
			char text[128];
			std::snprintf(text, sizeof(text), "FFXI World FX: the shading pass took %.1f ms of GPU time: paused 5 s", g.renderer->shading_ms());
			log_message(1, text);
			g.renderer->reset_timing();
		}
		g.shading_held = now < g.shading_paused_until ? "paused: it took too long on the GPU" : nullptr;
		grass::look l = g.look;
		// (the blades drawn at the zone's end this frame: the rest of it here, without them)
		l.phase = blades_went_in ? grass::look::phase_t::rest : grass::look::phase_t::all;
		g.last_at_marker = blades_went_in;
		// (the look once a frame: at a second world end before the present it would tone the world twice)
		if (g.passes > 1)
			l.post = grass::look::look_pass();
		if (g.shading_held != nullptr)
		{
			l.ao_strength = 0.0f;
			l.relief = 0.0f;
			l.sun_shadow = 0.0f;
			l.lamp_count = 0;
			l.wet = 0.0f;
			l.water = false; // (as costly: held with it)
		}
		// The game's visibility tests asked for since the last world: tested in this draw, answered a few presents on
		if (g.vis_on)
		{
			const uint32_t n = vistest::take(g.vis_objects, g.vis_boxes);
			std::string vis_error;
			if (n != 0 && !g.renderer->set_vis_boxes(g.vis_objects, g.vis_boxes, n, g.presents, vis_error))
				g.vis_skipped += n;
		}
		g.last = g.renderer->draw(list, t, cam, l, error);
		if (g.last.sun_map_cascades != 0)
			g.sun_map_redraws++;
		{
			addon_state::drawing &w = g.drawings[g.drawing_count++ % addon_state::kDrawings];
			w.at = now;
			w.frame = g.frames;
			w.blades = g.last.blades + (blades_went_in ? g.marker_blades.blades : 0);
			w.draws = g.last.draws + (blades_went_in ? g.marker_blades.draws : 0);
			w.shaded = g.last.shaded;
			w.held = g.shading_held != nullptr;
			w.bound = g.last.bound;
			w.rt_state = static_cast<uint32_t>(t.rt_state);
			w.ds_state = static_cast<uint32_t>(t.ds_state);
		}
		if (g.last.shaded && !g.shading_logged)
		{
			g.shading_logged = true;
			char text[128];
			std::snprintf(text, sizeof(text), "FFXI World FX: shading pass on, %.1f s after the game said a character is in", (now - g.in_game_since) / 1000.0);
			log_message(0, text);
		}
		// The game's pipeline, root signature and arguments, targets and viewport back, whenever the renderer
		// bound its own (the shading pass does too, with no grass near: left bound, the game's next draws ran
		// with them and hung the GPU)
		if (g.last.bound)
		{
			d.state.restore(list);
			gpu_crash::mark(list, L"worldfx: the game's state put back");
		}
		if (!error.empty())
		{
			g.error = error;
			g.enabled = false;
			g.skip = "the renderer failed (see above)";
			log_message(2, ("FFXI World FX: grass turned off: " + error).c_str());
			return;
		}
		const uint32_t draws = g.last.draws + (blades_went_in ? g.marker_blades.draws : 0);
		if (draws == 0)
			g.skip = "no blades near the camera";
		g.drew = draws != 0;
		g.at_vertices = g.world_vertices;
		g.of_vertices = g.world_vertices_last;
	}

	// Presents a zone marker keeps the world for: past them without one, the vertex counts decide again
	constexpr uint32_t kMarkerPresents = 8;

	// Whether the game side's zone markers name the world now (mutex held)
	bool marked()
	{
		return g.marker_rt != nullptr && g.markers != 0 && g.presents - g.marker_present <= kMarkerPresents;
	}

	bool same_size(ID3D12Resource *a, ID3D12Resource *b)
	{
		const D3D12_RESOURCE_DESC x = a->GetDesc(), y = b->GetDesc();
		return x.Width == y.Width && x.Height == y.Height && x.SampleDesc.Count == y.SampleDesc.Count;
	}

	void count_draw(list_data &d, uint64_t vertices)
	{
		if (d.rt0 == 0 || d.ds == 0)
			return;
		std::lock_guard<std::mutex> lock(g.mutex);
		bool found = false;
		for (target_use &use : g.uses)
			if (use.rt == d.rt0 && use.ds == d.ds)
			{
				use.vertices += vertices;
				found = true;
				break;
			}
		if (!found)
			g.uses.push_back({ d.rt0, d.ds, vertices });
		if (d.rt0 == g.world_rt && d.ds == g.world_ds)
			g.world_vertices += vertices;
	}

	void end_frame()
	{
		// In the game? (the Ashita addon's word: logged in, not zoning)
		{
			char text[8] = "";
			const DWORD n = GetEnvironmentVariableA(kInGameVariable, text, sizeof(text));
			const bool now_in = n == 1 && text[0] == '1';
			if (now_in && !g.in_game)
			{
				g.in_game_since = GetTickCount64();
				g.shading_logged = false;
				g.most_behind = 0;
			}
			if (now_in != g.in_game)
				log_message(0, now_in ? "FFXI World FX: the game says a character is in (drawing allowed)" :
					"FFXI World FX: the game says no character is in (login, character select or zoning: nothing drawn)");
			g.in_game = now_in;
		}
		// The safeguard: more world ends than allowed in this frame?
		{
			if (g.passes_wanted > kMaxPasses && g.passes_wanted > g.most_wanted)
			{
				g.most_wanted = g.passes_wanted;
				char text[128];
				std::snprintf(text, sizeof(text), "FFXI World FX: the world ended %u times between two presents (drawn at the first %u)", g.passes_wanted, kMaxPasses);
				log_message(1, text);
			}
			g.passes = 0;
			g.passes_wanted = 0;
		}
		const target_use *best = nullptr;
		for (const target_use &use : g.uses)
			if (best == nullptr || use.vertices > best->vertices)
				best = &use;
		if (marked())
		{
			// (the zone marker's world: its count still taken as the world's size where no clear said so)
			if (!g.world_cleared)
				for (const target_use &use : g.uses)
					if (use.rt == g.world_rt && use.ds == g.world_ds)
						learn_world_size(use.vertices);
		}
		else if (best != nullptr && best->vertices >= kMinWorldVertices)
		{
			if (best->rt != g.world_rt || best->ds != g.world_ds)
				g.recent_count = 0;
			g.world_rt = best->rt;
			g.world_ds = best->ds;
			if (!g.world_cleared)
				learn_world_size(best->vertices); // (no clear of the world's depth seen: present is the boundary)
		}
		if (!g.tried)
		{
			g.skip = g.world_rt == 0 ? "no world target yet" : "the world wasn't left after enough of it was drawn";
			g.last = {};
		}
		if (g.logging_moves)
		{
			// Three presents' worth, each line from one present to the next
			char head[160];
			std::snprintf(head, sizeof(head), "FFXI World FX: moves off the world and clears, present %u of 3 (share of %llu vertices -> new target; grass %s):",
				g.logged_frames + 1, static_cast<unsigned long long>(g.world_vertices_last), g.drew ? "drawn" : "not drawn");
			log_message(0, (head + g.moves).c_str());
			g.moves.clear();
			if (++g.logged_frames >= 3)
			{
				g.logging_moves = false;
				g.logged_moves = true;
			}
		}
		else if (!g.logged_moves && g.drew && ++g.frames_with_grass >= kLogMovesAfter)
		{
			g.logging_moves = true;
			g.moves.clear();
		}
		g.history[g.history_pos] = g.drew;
		g.history_pos = (g.history_pos + 1) % kHistory;
		if (g.history_count < kHistory)
			g.history_count++;
		g.uses.clear();
		if (!g.world_cleared)
		{
			g.world_vertices = 0;
			g.tried = false;
		}
		g.world_cleared = false;
		g.drew = false;
	}
}

// Events

static void log_gpu_crash(const char *line)
{
	log_message(2, line);
}

static std::vector<std::string> gpu_time_lines(const grass::renderer &r);

// For the crash report (on a thread pool thread): the add-on's state and its last drawings
static std::string crash_context()
{
	std::unique_lock<std::mutex> lock(g.mutex, std::defer_lock);
	for (int i = 0; i < 50 && !lock.try_lock(); ++i)
		Sleep(10);
	std::string out;
	char line[256];
	if (!lock.owns_lock())
		out += "  (busy: its state couldn't be read)\n";
	const ULONGLONG now = GetTickCount64();
	std::snprintf(line, sizeof(line), "  in the game: %s (for %.1f s); zone %u; frame %u\n", g.in_game ? "yes" : "no",
		g.in_game ? (now - g.in_game_since) / 1000.0 : 0.0, g.zone, g.frames);
	out += line;
	std::snprintf(line, sizeof(line), "  settings: grass %s (distance %.0f, tuft %d), ambient occlusion %.2f, relief %.2f, sun shadows %.2f, water %s (%s)\n",
		g.enabled ? "on" : "off", g.look.distance, g.look.tuft, g.look.ao_strength, g.look.relief, g.sun_shadows,
		g.look.water ? "on" : "off", g.water_status.c_str());
	out += line;
	std::snprintf(line, sizeof(line), "  sun shadow map: %s (%s), drawn again %u times\n", g.look.sun_map ? "on" : "off", g.shadow_status.c_str(), g.sun_map_redraws);
	out += line;
	std::snprintf(line, sizeof(line), "  sea: %s (%s), to %.0f yalms, %u corners last drawn\n", g.look.sea ? "on" : "off", g.sea_status.c_str(), g.look.sea_distance,
		g.last.sea_corners);
	out += line;
	std::snprintf(line, sizeof(line), "  lamps: glow %.2f, reach %.2f, shadows %.2f (%s); %u lit lights sent %.1f s ago, %u glowing in the last drawing\n",
		g.lamp_glow, g.lamp_reach, g.lamp_shadows, g.lamps_status.c_str(), static_cast<unsigned>(g.live_lights.size()),
		g.live_at != 0 ? (now - g.live_at) / 1000.0 : -1.0, g.last.lamps);
	out += line;
	std::snprintf(line, sizeof(line), "  glows: fixtures %.2f, effects %.2f; %u sent %.1f s ago, %u used, %u effects by lamps\n",
		g.fixture_glow, g.effect_glow, static_cast<unsigned>(g.glows.size()), g.glows_at != 0 ? (now - g.glows_at) / 1000.0 : -1.0,
		g.glows_used, g.glows_by_lamps);
	out += line;
	out += "  weather: fog " + std::string(g.weather_fog ? "on" : "off") + ", shadows by it " + (g.weather_shadows ? "on" : "off") + "; " +
		(g.weather_at != 0 ? weather_text() : "never sent") + "\n";
	std::snprintf(line, sizeof(line), "  wet ground: setting %.2f, wet now %.2f (%s)\n", g.wet_ground, g.wetness, g.wet_status.c_str());
	out += line;
	std::snprintf(line, sizeof(line), "  settled snow: setting %.2f, lying now %.2f%s; depth %.2f%s, trails %s%s\n", g.snow_cover_amount, g.snow_cover,
		g.last.snow_cover ? " (drawn)" : "", g.snow_depth, g.last.snow_layer ? " (drawn)" : "", g.snow_trails ? "on" : "off", g.last.snow_trails ? " (kept)" : "");
	out += line;
	std::snprintf(line, sizeof(line), "  rain %s (amount %.2f), raining now %.2f; snow %s (amount %.2f), snowing now %.2f; rain map: %s\n", g.rain_on ? "on" : "off",
		g.rain_amount, g.raining, g.snow_on ? "on" : "off", g.snow_amount, g.snowing, g.rain_status.c_str());
	out += line;
	out += "  sky of its own: " + sky_text() + "\n";
	out += "  weather of its own: " + weather_fx_text() + "\n";
	std::snprintf(line, sizeof(line), "  shading pass: %s\n", g.shading_held != nullptr ? g.shading_held : g.shading_logged ? "running" : "not run yet");
	out += line;
	if (g.renderer != nullptr)
	{
		std::snprintf(line, sizeof(line), "  GPU time a frame (smoothed): copies and shading %.2f ms, grass %.2f ms\n", g.renderer->shading_ms(), g.renderer->grass_ms());
		out += line;
		for (const std::string &l : gpu_time_lines(*g.renderer))
			out += "  " + l + "\n";
	}
	{
		const double h = vanadiel_hour();
		std::snprintf(line, sizeof(line), "  Vana'diel time %02d:%02d (sun %s); toward the sun %.2f %.2f %.2f (%s)\n", int(h), int((h - int(h)) * 60.0),
			h > 6.0 && h < 18.0 ? "up" : "down", g.look.sun[0], g.look.sun[1], g.look.sun[2], g.sun_from_game ? "the game's light read" : "the game's light not found");
		out += line;
		const moon_phase m = moon_now();
		std::snprintf(line, sizeof(line), "  the moon %.0f%% lit, %s (day %.1f of its 84; %s)\n", m.lit * 100.0f, m.waxing ? "waxing" : "waning", m.cycle * 84.0f,
			m.from_game ? "the game's clock" : "the PC's clock");
		out += line;
	}
	out += "  last drawings at the world's end (newest last):\n";
	const uint32_t n = std::min(g.drawing_count, addon_state::kDrawings);
	for (uint32_t i = g.drawing_count - n; i < g.drawing_count; ++i)
	{
		const addon_state::drawing &w = g.drawings[i % addon_state::kDrawings];
		std::snprintf(line, sizeof(line), "    %.3f s ago (frame %u): %u blades in %u draws, shading %s, state %s, world rt 0x%X ds 0x%X\n",
			(now - w.at) / 1000.0, w.frame, w.blades, w.draws, w.shaded ? "ran" : w.held ? "held" : "off", w.bound ? "put back" : "untouched", w.rt_state, w.ds_state);
		out += line;
	}
	if (n == 0)
		out += "    none\n";
	std::snprintf(line, sizeof(line), "  frame pacing (newest last; most presents the GPU was behind since in the game: %u):\n", g.most_behind);
	out += line;
	const uint32_t p = std::min(g.pace_count, addon_state::kPaces);
	for (uint32_t i = g.pace_count - p; i < g.pace_count; ++i)
	{
		const addon_state::pace &w = g.paces[i % addon_state::kPaces];
		std::snprintf(line, sizeof(line), "    %.3f s ago: present %llu, %.1f ms after the one before, the GPU %u presents behind\n",
			(now - w.at) / 1000.0, static_cast<unsigned long long>(w.present), w.ms, w.behind);
		out += line;
	}
	if (p == 0)
		out += "    none\n";
	return out;
}

// At each present (mutex held): how many presents the GPU hasn't finished, then our fence's next value
// signalled on the presenting queue after this frame's work (a signal, no GPU work)
static void pace_present(ID3D12CommandQueue *q)
{
	ID3D12Device *const device = g.dev;
	if (q == nullptr || device == nullptr)
		return;
	if (g.pace_device != device)
	{
		if (g.pace_fence != nullptr)
			g.pace_fence->Release();
		g.pace_fence = nullptr;
		g.pace_device = device;
		g.presents = 0;
		if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.pace_fence))))
			g.pace_fence = nullptr;
		else
			g.pace_fence->SetName(L"worldfx: frame pacing");
	}
	if (g.pace_fence == nullptr)
		return;
	const uint64_t done = g.pace_fence->GetCompletedValue();
	if (done == UINT64_MAX)
		return; // (the device is gone)
	LARGE_INTEGER now, frequency;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	addon_state::pace &w = g.paces[g.pace_count++ % addon_state::kPaces];
	w.at = GetTickCount64();
	w.present = g.presents;
	w.behind = static_cast<uint32_t>(std::min<uint64_t>(g.presents - std::min(done, g.presents), UINT32_MAX));
	w.ms = g.last_present.QuadPart != 0 ? float(double(now.QuadPart - g.last_present.QuadPart) * 1000.0 / double(frequency.QuadPart)) : 0.0f;
	g.last_present = now;
	if (g.in_game)
		g.most_behind = std::max(g.most_behind, w.behind);
	q->Signal(g.pace_fence, ++g.presents);
}

// Whether a clear's rectangles (none: all of it) cover the whole of res
static bool covers_all(ID3D12Resource *res, uint32_t rect_count, const D3D12_RECT *rects)
{
	if (rect_count == 0)
		return true;
	const D3D12_RESOURCE_DESC rd = res->GetDesc();
	for (uint32_t i = 0; i < rect_count; ++i)
		if (rects[i].left <= 0 && rects[i].top <= 0 && rects[i].right >= int32_t(rd.Width) && rects[i].bottom >= int32_t(rd.Height))
			return true;
	return false;
}

namespace worldfx
{
	void set_port(const port &p)
	{
		host = p;
	}

	ID3D12Device *attached()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		return g.dev;
	}

	// Before the device is created: Direct3D's record of what the GPU did, for a report if it stops (GpuCrashReports)
	void before_device(bool debug_layer_allowed)
	{
		if (g.crash_reports_tried)
			return;
		g.crash_reports_tried = true;
		bool on = true;
		config_value("GpuCrashReports", on);
		if (!on)
			g.crash_reports = "off (GpuCrashReports=0)";
		else
			gpu_crash::enable(g.crash_reports);
		log_message(0, ("FFXI World FX: GPU crash reports " + g.crash_reports).c_str());
		bool debug_layer = false;
		config_value("D3D12DebugLayer", debug_layer);
		if (debug_layer)
		{
			if (debug_layer_allowed)
				g.debug_layer = gpu_crash::enable_debug_layer(g.debug_layer_status);
			else
				g.debug_layer_status = "off (D3D12DebugLayer=1: not with this host, whose hooks need Direct3D 12's own classes)";
			log_message(0, ("FFXI World FX: Direct3D 12 debug layer " + g.debug_layer_status).c_str());
		}
	}

	void set_crash_reports_status(const std::string &status)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.crash_reports = status;
	}

	void set_casters(grass::renderer::caster_fn fn, void *context)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.casters = fn;
		g.casters_context = context;
		if (g.renderer != nullptr)
			g.renderer->set_casters(fn, context);
	}

	void set_zone_casters(grass::renderer::zone_bounds_fn bounds, grass::renderer::zone_draw_fn draw, void *context)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.zone_bounds = bounds;
		g.zone_draw = draw;
		g.zone_casters_context = context;
		if (g.renderer != nullptr)
			g.renderer->set_zone_casters(bounds, draw, context);
	}

	void attach(ID3D12Device *device)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (device == nullptr || g.dev == device)
			return;
		g.dev = device;
		g_has_drawn = false;
		g.renderer = std::make_unique<grass::renderer>();
		g.renderer->set_casters(g.casters, g.casters_context);
		g.renderer->set_zone_casters(g.zone_bounds, g.zone_draw, g.zone_casters_context);
		g.shader_stamp = 0; // (a new renderer starts from the built-in shaders: worldfx.hlsl is loaded again)
		g.shader_status = "built in";
		g.error.clear();
		std::string error;
		if (!g.renderer->init(device, error, shader_cache_folder()))
		{
			g.error = error;
			log_message(2, ("FFXI World FX: grass renderer: " + error).c_str());
		}
		else
		{
			char how[96];
			if (g.renderer->shaders_from_cache())
				std::snprintf(how, sizeof(how), "from the shader cache (%.2f s)", g.renderer->shader_seconds());
			else
				std::snprintf(how, sizeof(how), "compiled in %.1f s (kept in the shader cache)", g.renderer->shader_seconds());
			g.shader_status = std::string("built in, ") + how;
			log_message(0, (std::string("FFXI World FX: the built-in shaders, ") + how).c_str());
		}
		g.zone_tried = false;
		g.world_rt = nullptr;
		g.world_ds = nullptr;
		g.marker_rt = nullptr;
		g.marker_ds = nullptr;
		gpu_crash::watch(device, addon_folder() + L"worldfx-gpu-crash.txt", log_gpu_crash, crash_context);
		log_message(0, ("FFXI World FX: the GPU: " + gpu_crash::gpu_name(device) + "; video memory: " + gpu_crash::gpu_memory(device)).c_str());
	}

	void detach(ID3D12Device *device)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (device != g.dev)
			return;
		gpu_crash::unwatch();
		g_has_drawn = false;
		g.renderer.reset();
		if (g.pace_fence != nullptr)
			g.pace_fence->Release();
		g.pace_fence = nullptr;
		g.pace_device = nullptr;
		g.dev = nullptr;
		g.world_rt = nullptr;
		g.world_ds = nullptr;
		g.marker_rt = nullptr;
		g.marker_ds = nullptr;
		g.uses.clear();
	}

	void shutdown()
	{
		// At process exit the device may be gone already: its objects are left to the process's end, not released here
		std::lock_guard<std::mutex> lock(g.mutex);
		(void)g.renderer.release();
		g.dev = nullptr;
	}

	void on_reset(list_data &d)
	{
		d = list_data();
		// A list starts with the world in the states the last barrier on any list left it in: dgVoodoo may
		// change them on one list and draw on the next (the depth, before the resolve)
		std::lock_guard<std::mutex> lock(g.mutex);
		d.world_rt_state = g.last_rt_state;
		d.world_ds_state = g.last_ds_state;
	}

	void on_close(ID3D12GraphicsCommandList *list, list_data &d)
	{
		if (d.rt0 == nullptr)
			return;
		std::lock_guard<std::mutex> lock(g.mutex);
		if (due(d.rt0, d.ds))
			inject(list, d, uint64_t(d.state.rtvs[0].ptr), uint64_t(d.state.dsv.ptr));
	}

	void on_targets(ID3D12GraphicsCommandList *list, list_data &d, uint32_t count, const uint64_t *rtvs, uint64_t dsv)
	{
		ID3D12Resource *const old_rt = d.rt0, *const old_ds = d.ds;
		const uint64_t old_rtv = d.state.rtv_count != 0 ? uint64_t(d.state.rtvs[0].ptr) : 0;
		const uint64_t old_dsv = uint64_t(d.state.dsv.ptr);

		// The new binding (already made: this comes after the call)
		count = std::min<uint32_t>(count, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
		d.state.rtv_count = count;
		for (uint32_t i = 0; i < count; ++i)
			d.state.rtvs[i] = { static_cast<SIZE_T>(rtvs[i]) };
		d.state.dsv = { static_cast<SIZE_T>(dsv) };
		d.rt0 = count != 0 && rtvs[0] != 0 ? resource_of(rtvs[0]) : nullptr;
		d.ds = dsv != 0 ? resource_of(dsv) : nullptr;
		if (d.rt0 != nullptr && d.rt0 == g.world_rt && d.ds == g.world_ds)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.world_rtv = rtvs[0];
			g.world_dsv = dsv;
		}
		if (old_rt == nullptr || (old_rt == d.rt0 && old_ds == d.ds))
			return;

		std::lock_guard<std::mutex> lock(g.mutex);
		if (due(old_rt, old_ds, d.rt0))
			inject(list, d, old_rtv, old_dsv);
	}

	void on_pipeline(list_data &d, ID3D12PipelineState *pso)
	{
		d.state.pipeline = pso;
	}

	void on_topology(list_data &d, D3D12_PRIMITIVE_TOPOLOGY topology)
	{
		d.state.topology = topology;
	}

	void on_viewports(list_data &d, uint32_t first, uint32_t count, const D3D12_VIEWPORT *viewports)
	{
		const uint32_t total = std::min<uint32_t>(first + count, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
		for (uint32_t i = first; i < total; ++i)
			std::memcpy(&d.state.viewports[i], &viewports[i - first], sizeof(D3D12_VIEWPORT));
		d.state.viewport_count = count != 0 ? total : 0;
	}

	void on_scissors(list_data &d, uint32_t first, uint32_t count, const D3D12_RECT *rects)
	{
		const uint32_t total = std::min<uint32_t>(first + count, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
		for (uint32_t i = first; i < total; ++i)
			std::memcpy(&d.state.scissors[i], &rects[i - first], sizeof(D3D12_RECT));
		d.state.scissor_count = count != 0 ? total : 0;
	}

	void on_root_tables(list_data &d, ID3D12RootSignature *root, uint32_t first, uint32_t count, const uint64_t *tables)
	{
		// No tables: the root signature was set (or the list reset)
		d.state.set_root(root);
		for (uint32_t i = 0; i < count; ++i)
		{
			grass::root_arg &a = d.state.arg(first + i);
			a = grass::root_arg();
			a.kind = grass::root_arg::table;
			a.value = tables[i];
		}
	}

	void on_root_constants(list_data &d, ID3D12RootSignature *root, uint32_t param, uint32_t first, uint32_t count, const void *values)
	{
		d.state.set_root(root);
		d.state.set_constants(param, first, count, static_cast<const uint32_t *>(values));
	}

	void on_root_descriptor(list_data &d, ID3D12RootSignature *root, uint32_t param, grass::root_arg::kind_t kind, uint64_t address)
	{
		d.state.set_root(root);
		grass::root_arg &a = d.state.arg(param);
		a = grass::root_arg();
		if (kind == grass::root_arg::cbv || kind == grass::root_arg::srv || kind == grass::root_arg::uav)
		{
			a.kind = kind;
			a.value = address;
		}
	}

	void on_barrier(ID3D12GraphicsCommandList *list, list_data &d, uint32_t count, ID3D12Resource *const *resources, const D3D12_RESOURCE_STATES *after)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		for (uint32_t i = 0; i < count; ++i)
		{
			if (resources[i] == nullptr)
				continue;
			if (resources[i] == g.world_rt)
			{
				const D3D12_RESOURCE_STATES was = d.world_rt_state;
				g.last_rt_state = d.world_rt_state = after[i];
				// The world turned from a target into something read: its end (the resolve samples it next). Grass
				// goes in here, told by this list alone: no counts shared between frames, which dgVoodoo records
				// overlapping, the more so the higher the frame rate. (Earlier points left part of the ground out
				// of its copy, and blades read fog there.)
				const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				if ((d.world_rt_state & read) != 0 && (was & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0)
				{
					if (g.logging_moves && g.moves.size() < 1500)
						g.moves += " rt-read";
					if (!d.grass_done && g.world_rtv != 0 && g.world_dsv != 0)
					{
						d.grass_done = true;
						g.last_resolve_frame = g.frames;
						// The views were saved at the world's last bind: dgVoodoo may have put another target in those
						// descriptors since (while a zone loads), and a full-screen pass through them would then write
						// far outside it
						if (resource_of(g.world_rtv) == g.world_rt && resource_of(g.world_dsv) == g.world_ds)
							inject(list, d, g.world_rtv, g.world_dsv);
						else
						{
							g.skip = "the world's views hold something else now (skipped)";
							if (!g.logged_stale_views)
							{
								g.logged_stale_views = true;
								log_message(1, "FFXI World FX: the world's saved views held another target at its end (skipped; logged once)");
							}
						}
					}
				}
			}
			else if (resources[i] == g.world_ds)
				g.last_ds_state = d.world_ds_state = after[i];
		}
	}

	void on_clear_depth(ID3D12GraphicsCommandList *list, list_data &d, uint64_t dsv, bool depth, uint32_t rect_count, const D3D12_RECT *rects)
	{
		if (!depth)
			return;
		ID3D12Resource *const res = resource_of(dsv);
		if (res != nullptr && res == g.world_ds && !covers_all(res, rect_count, rects))
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			if (g.logging_moves && g.moves.size() < 1500)
				g.moves += " depth-clear(part)";
			return;
		}
		std::lock_guard<std::mutex> lock(g.mutex);
		if (res != g.world_ds)
			return;
		const double share = g.world_vertices_last != 0 ? double(g.world_vertices) / double(g.world_vertices_last) : 0.0;
		if (g.logging_moves && g.moves.size() < 1500)
		{
			char item[48];
			std::snprintf(item, sizeof(item), " %.0f%%:depth-clear", share * 100.0);
			g.moves += item;
		}
		// Whatever clears the world's depth (the frame's start, or a test mid-frame: the lens flare's and the
		// glows' visibility tests clear and switch surfaces), grass goes in before it while the ground is there
		if (!barrier_seen() && !g.tried && share >= kResolveShare && g.world_vertices >= kMinWorldVertices && d.rt0 == g.world_rt && d.ds == g.world_ds)
			inject(list, d, uint64_t(d.state.rtvs[0].ptr), dsv);
		if (!g.color_clears)
		{
			// (no clear of the world's colour seen: its depth clear is the frame boundary)
			learn_world_size(g.world_vertices);
			g.world_vertices = 0;
			g.tried = false;
			g.world_cleared = true;
		}
	}

	// The world's colour (the sky/fog clear) is cleared once, at the frame's start: the frame boundary for grass.
	// Not present: dgVoodoo records the next frame's lists on another thread while the last one presents, so a
	// present can land in the middle of a world, and counting from it put grass (and its copy) after a clear.
	void on_clear_target(list_data &, uint64_t rtv, const float color[4], uint32_t rect_count, const D3D12_RECT *rects)
	{
		ID3D12Resource *const res = resource_of(rtv);
		std::lock_guard<std::mutex> lock(g.mutex);
		if (res == nullptr || res != g.world_rt)
			return;
		if (!covers_all(res, rect_count, rects))
		{
			if (g.logging_moves && g.moves.size() < 1500)
				g.moves += " colour-clear(part)";
			return;
		}
		if (g.logging_moves && g.moves.size() < 1500)
			g.moves += " colour-clear";
		g.color_clears = true;
		// (the colour it's cleared to is the zone's fog: the water's sky along the horizon)
		if (color != nullptr)
		{
			for (int i = 0; i < 3; ++i)
				g.look.fog[i] = std::clamp(color[i], 0.0f, 1.0f);
			g.look.fog_known = true;
		}
		learn_world_size(g.world_vertices);
		g.world_vertices = 0;
		g.tried = false;
		g.world_cleared = true;
	}

	void on_world_marker(list_data &d, uint64_t dsv)
	{
		ID3D12Resource *const ds = resource_of(dsv);
		if (ds == nullptr)
			return;
		std::lock_guard<std::mutex> lock(g.mutex);
		g.blades_at_marker = false;
		g.look_at_marker = false;
		g.markers++;
		g.marker_present = g.presents;
		// The target: the one bound with this depth on this list (a list dgVoodoo has just reset has none bound: then
		// the one found with it before)
		ID3D12Resource *rt = nullptr;
		if (d.ds == ds && d.rt0 != nullptr && same_size(d.rt0, ds))
			rt = d.rt0;
		else if (g.marker_ds == ds)
			rt = g.marker_rt;
		else if (g.world_ds == ds && g.world_rt != nullptr && same_size(g.world_rt, ds))
			rt = g.world_rt;
		g.marker_ds = ds;
		g.marker_rt = rt;
		if (rt == nullptr || (rt == g.world_rt && ds == g.world_ds))
			return;
		g.world_rt = rt;
		g.world_ds = ds;
		g.recent_count = 0;
		// (its views: the ones bound now, else from its next bind)
		if (d.rt0 == rt && d.state.rtv_count != 0)
		{
			g.world_rtv = uint64_t(d.state.rtvs[0].ptr);
			g.world_dsv = dsv;
		}
		const D3D12_RESOURCE_DESC w = rt->GetDesc();
		char text[160];
		std::snprintf(text, sizeof(text), "FFXI World FX: the world is where the game's zone marker is: %llux%u%s", static_cast<unsigned long long>(w.Width), w.Height,
			g.marker_logged ? " (again: it had moved)" : "");
		log_message(g.marker_logged ? 1 : 0, text);
		g.marker_logged = true;
	}

	void on_zone_end(ID3D12GraphicsCommandList *list, list_data &d, uint64_t dsv, const D3D12_RESOURCE_STATES planes[2])
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.blades_at_marker = false;
		ID3D12Resource *const ds = resource_of(dsv);
		if (!g.in_game || !g.enabled || g.renderer == nullptr || !g.renderer->ready() || !g.renderer->loaded())
			return;
		// (the world the markers named, its views still its own, D24S8 as the blades' depth; dgVoodoo's state seen)
		if (ds == nullptr || ds != g.world_ds || g.world_rt == nullptr || g.world_rtv == 0 || resource_of(g.world_rtv) != g.world_rt ||
			view_format(dsv, ds) != DXGI_FORMAT_D24_UNORM_S8_UINT || d.state.root == nullptr || d.state.pipeline == nullptr)
			return;
		grass::camera cam;
		if (read_camera(cam) != nullptr)
			return;
		frame_look(cam);
		g.look_at_marker = true;
		grass::look l = g.look;
		l.phase = grass::look::phase_t::blades;
		grass::target t;
		t.rt = g.world_rt;
		t.ds = ds;
		t.rtv = { static_cast<SIZE_T>(g.world_rtv) };
		t.rtv_format = view_format(g.world_rtv, g.world_rt);
		t.dsv_format = view_format(dsv, ds);
		t.rt_state = d.world_rt_state;
		t.ds_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		t.blades_into_world_depth = true;
		// Both planes DEPTH_WRITE while the blades write the depth (and test it), then back as dgVoodoo had them
		D3D12_RESOURCE_BARRIER b[2] = {};
		UINT n = 0;
		for (UINT k = 0; k < 2; ++k)
			if (planes[k] != D3D12_RESOURCE_STATE_DEPTH_WRITE)
			{
				b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				b[n].Transition.pResource = ds;
				b[n].Transition.Subresource = k;
				b[n].Transition.StateBefore = planes[k];
				b[n].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
				++n;
			}
		if (n != 0)
			list->ResourceBarrier(n, b);
		std::string error;
		g.marker_blades = g.renderer->draw(list, t, cam, l, error);
		for (UINT k = 0; k < n; ++k)
			std::swap(b[k].Transition.StateBefore, b[k].Transition.StateAfter);
		if (n != 0)
			list->ResourceBarrier(n, b);
		if (g.marker_blades.bound)
		{
			d.state.restore(list);
			gpu_crash::mark(list, L"worldfx: the game's state put back (the blades at the zone's end)");
		}
		if (!error.empty())
		{
			g.error = error;
			g.enabled = false;
			log_message(2, ("FFXI World FX: grass turned off: " + error).c_str());
			return;
		}
		g.blades_at_marker = true;
		g.blades_at_markers++;
	}

	void on_draw(list_data &d, uint64_t vertices)
	{
		count_draw(d, vertices);
	}

	void on_present(ID3D12CommandQueue *queue)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (g.dev == nullptr || queue == nullptr)
			return;
		end_frame();
		pace_present(queue);
		// The visibility tests' counts from the frames the GPU has finished (their slots' presents + 2)
		if (g.vis_on && g.renderer != nullptr && g.pace_fence != nullptr)
		{
			const uint64_t done = g.pace_fence->GetCompletedValue();
			const uint64_t now = GetTickCount64();
			uint32_t n = 0;
			for (int i = 0; i < 4 && done != UINT64_MAX && (n = g.renderer->read_vis(done, g.vis_objects, g.vis_points)) != 0; ++i)
				vistest::answer(g.vis_objects, g.vis_points, n, now);
		}
		if (g.renderer != nullptr && g.renderer->ready())
		{
			static bool frequency_known = false;
			if (!frequency_known)
			{
				UINT64 ticks = 0;
				if (SUCCEEDED(queue->GetTimestampFrequency(&ticks)) && ticks != 0)
				{
					g.renderer->set_timestamp_frequency(ticks);
					frequency_known = true;
				}
			}
			g.renderer->release_old();
			if (g.debug_layer)
				gpu_crash::write_debug_messages(g.dev, addon_folder() + L"worldfx-d3d12-debug.txt");
			if (g.frames++ % kZoneCheckFrames == 0)
			{
				check_zone();
				check_shaders();
			}
		}
	}

	// ---- Weather debug: World FX's own weather forced (only its own drawing; nothing of the game's, the server's or
	// its packets). The game's weather is left out as before (worldfx_weather_drawn follows the weather drawn).
	namespace
	{
		const weather_choice kWeatherChoices[] = {
			{ "fine", "Fine" }, { "suny", "Sunshine" }, { "clod", "Clouds" }, { "mist", "Mist" }, { "dryw", "Hot spell" },
			{ "heat", "Heat wave" }, { "rain", "Rain" }, { "squl", "Squall" }, { "dust", "Dust storm" }, { "sand", "Sand storm" },
			{ "wind", "Wind" }, { "stom", "Gales" }, { "snow", "Snow" }, { "bliz", "Blizzard" }, { "thdr", "Thunder" },
			{ "bolt", "Thunderstorm" }, { "aura", "Auroras" }, { "ligt", "Stellar glare" }, { "fogd", "Gloom" }, { "dark", "Darkness" },
		};

		uint32_t name_word(const char *s)
		{
			uint32_t w = 0;
			for (int i = 0; i < 4 && s[i] != 0; ++i)
				w |= uint32_t(uint8_t(std::tolower(static_cast<unsigned char>(s[i])))) << (8 * i);
			return w;
		}

		const weather_choice *choice_of(uint32_t name)
		{
			for (const weather_choice &c : kWeatherChoices)
				if (name_word(c.name) == name)
					return &c;
			return nullptr;
		}
	}

	const weather_choice *weather_choices(size_t &count)
	{
		count = sizeof(kWeatherChoices) / sizeof(kWeatherChoices[0]);
		return kWeatherChoices;
	}

	bool force_weather(const char *name, float fade_seconds)
	{
		uint32_t want = 0;
		if (name != nullptr && name[0] != 0 && _stricmp(name, "off") != 0)
		{
			const weather_choice *c = nullptr;
			for (const weather_choice &k : kWeatherChoices)
				if (_stricmp(name, k.name) == 0 || _stricmp(name, k.label) == 0)
					c = &k;
			if (c == nullptr)
				return false;
			want = name_word(c->name);
		}
		std::lock_guard<std::mutex> lock(g.mutex);
		addon_state::forced_weather &f = g.forced;
		if (want == f.name)
			return true;
		// (from the weather drawn now, the larger share of it while it changes)
		const grass::weather now = weather_forced_over(g.weather_real);
		const uint32_t from = now.before != 0 && now.change < 0.5f ? now.before : now.name;
		f.name = want;
		f.from = from;
		f.since = GetTickCount64();
		f.fade_ms = ULONGLONG(std::clamp(fade_seconds, 0.0f, 600.0f) * 1000.0f);
		g.weather = weather_forced_over(g.weather_real);
		log_message(0, ("FFXI World FX: weather debug: " + (want != 0 ? "forced to " + weather_name_text(want) : std::string("back to the real weather"))).c_str());
		return true;
	}

	std::string forced_weather()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		return g.forced.name != 0 ? weather_name_text(g.forced.name) : std::string();
	}

	std::string forced_weather_text()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		const addon_state::forced_weather &f = g.forced;
		if (f.name == 0 && f.from == 0)
			return "World FX's own weather: the real weather (none forced).";
		const weather_choice *c = choice_of(f.name);
		std::string s = f.name != 0 ? "World FX's own weather forced to " + weather_name_text(f.name) + (c != nullptr ? std::string(" (") + c->label + ")" : std::string())
			: std::string("World FX's own weather going back to the real one");
		s += "; the real weather " + (g.weather_real.name != 0 ? weather_name_text(g.weather_real.name) : std::string("not read"));
		if (g.weather.before != 0 && g.weather.change < 1.0f)
		{
			char part[80];
			std::snprintf(part, sizeof(part), "; fading from %s, %.0f%% there", weather_name_text(g.weather.before).c_str(), g.weather.change * 100.0f);
			s += part;
		}
		s += ". Debug only, not saved; the zone's light, fog and sky colours still follow the real weather";
		if (!g.wx_on)
			s += "; World FX isn't drawing the zone's weather here now (only its rain and snow, where it has them)";
		return s + ".";
	}

	void flash_lightning()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		const ULONGLONG now = GetTickCount64();
		g.flashes.next = now;
		g.flash_until = now + 1200;
		g.flash_bolt = true;
	}

	void weather_look_now(grass::look::weather_fx &out, grass::weather *drawn)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.weather = weather_forced_over(g.weather_real);
		out = grass::look::weather_fx();
		grass::weather_look(g.weather, g.look.dome, out);
		if (drawn != nullptr)
			*drawn = g.weather;
	}
}

static void take_actors(const float *xyzr, const uint32_t *flags, uint32_t count)
{
	std::lock_guard<std::mutex> lock(g.mutex);
	count = xyzr != nullptr ? std::min<uint32_t>(count, grass::kMaxActors) : 0;
	uint32_t kept = 0, fighting = 0;
	for (uint32_t i = 0; i < count; ++i)
	{
		const float *a = xyzr + size_t(i) * 4;
		if (!std::isfinite(a[0]) || !std::isfinite(a[1]) || !std::isfinite(a[2]) || !(a[3] > 0.0f && a[3] < 10.0f))
			continue;
		std::memcpy(&g.actors[kept * 4], a, 16);
		if (flags != nullptr && (flags[i] & 1u) != 0)
			fighting |= 1u << kept;
		kept++;
	}
	g.actor_count = kept;
	g.fighting = fighting;
	g.actors_at = GetTickCount64();
	make_ripples(g.actors_at);
}

// From the worldfx Ashita addon, every frame: the characters and monsters near, xyzr[i * 4 ..] = x, y, z
// (their feet, the game's world: y is height, down is positive) and the radius they push grass within
extern "C" __declspec(dllexport) void __cdecl worldfx_grass_actors(const float *xyzr, uint32_t count)
{
	take_actors(xyzr, nullptr, count);
}

// For HiFi XI's game side: whether World FX draws into the game's world (it has found it since the game's device was
// attached): 1 or 0. Its game side replaces nothing of the game's without it (another output: DXVK, dgVoodoo's
// Direct3D 11; or started late, the world never found).
extern "C" __declspec(dllexport) uint32_t __cdecl worldfx_drawing()
{
	return g_has_drawn ? 1u : 0u;
}

// For HiFi XI's game side: whether World FX is casting characters' and monsters' shadows now (it's drawing, they cast,
// and the sun's or moon's shadows are on here, under open sky), so the game's own can be left out while they are:
// 1 or 0. Twilight, the weather fading the shadows: on from a share of 0.05, off again under 0.02 (no flicker).
extern "C" __declspec(dllexport) uint32_t __cdecl worldfx_casts_characters()
{
	static bool casting = false;
	std::lock_guard<std::mutex> lock(g.mutex);
	const float share = g.renderer != nullptr && g.look.sun_characters ? g.look.sun_shadow : 0.0f;
	casting = casting ? share >= 0.02f : share >= 0.05f;
	return casting ? 1u : 0u;
}

// The same with a word for each (addon 0.14 on): bit 0 set while they're fighting (the game's engaged status), which
// wears the grass under them down faster and wider
extern "C" __declspec(dllexport) void __cdecl worldfx_grass_actors2(const float *xyzr, const uint32_t *flags, uint32_t count)
{
	take_actors(xyzr, flags, count);
}

// From the worldfx Ashita addon when a weapon skill is used near: where it cuts the grass, 7 floats a swath: from x, y,
// z (the game's world, as the actors'), its half-width (yalms), to x, y, z. Kept for grass::kCutKeep seconds, the
// grass::kMaxCuts newest
extern "C" __declspec(dllexport) void __cdecl worldfx_grass_cut(const float *swaths, uint32_t count)
{
	std::lock_guard<std::mutex> lock(g.mutex);
	const ULONGLONG now = GetTickCount64();
	for (uint32_t i = 0; swaths != nullptr && i < std::min<uint32_t>(count, grass::kMaxCuts); ++i)
	{
		const float *s = swaths + size_t(i) * 7;
		bool ok = s[3] > 0.0f && s[3] <= 20.0f;
		for (int k = 0; k < 7 && ok; ++k)
			ok = std::isfinite(s[k]);
		if (!ok)
			continue;
		auto &c = g.cuts[g.cuts_next++ % grass::kMaxCuts];
		std::memcpy(c.swath, s, sizeof(c.swath));
		c.at = now;
		g.cuts_total++;
		g.last_cut_at = now;
	}
}

// From the worldfx Ashita addon every few frames: the lights lit in the game's global light table now (client-notes
// "Zone lamps": its entries' D3DLIGHT8), 10 words each, as the game keeps them: the light's id (its 4 characters),
// its type (1 a point light), its colour (r, g, b), where it is (x, y, z: the game's world, y down), how far it
// reaches and its brightness, the last eight floats as their bits (read as words: never as floats in Lua). Only
// point lights with sane numbers and some brightness are kept; a lamp not sent is off.
extern "C" __declspec(dllexport) void __cdecl worldfx_lamps(const uint32_t *words, uint32_t count)
{
	std::lock_guard<std::mutex> lock(g.mutex);
	count = words != nullptr ? std::min<uint32_t>(count, 256) : 0;
	g.live_lights.clear();
	for (uint32_t i = 0; i < count; ++i)
	{
		const uint32_t *w = words + size_t(i) * 10;
		float f[8];
		std::memcpy(f, w + 2, sizeof(f));
		bool ok = w[0] != 0 && w[1] == 1;
		for (int k = 0; k < 8 && ok; ++k)
			ok = std::isfinite(f[k]);
		if (!ok || !(f[7] > 0.0f) || !(f[6] > 0.0f) || std::fabs(f[3]) > 100000.0f || std::fabs(f[4]) > 100000.0f || std::fabs(f[5]) > 100000.0f)
			continue;
		g.live_lights.push_back({ w[0], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7] });
	}
	g.live_at = GetTickCount64();
	g.live_sends++;
}

// The lamps, for tests: out[0] the zone's baked lamps, [1] of them lit now, [2] lit lights not in the bake used,
// [3] lit lights left out as a whole area's light (too far reaching), [4] lamps whose glow the last drawing added
extern "C" __declspec(dllexport) void __cdecl worldfx_lamp_stats(uint32_t out[5])
{
	std::lock_guard<std::mutex> lock(g.mutex);
	out[0] = static_cast<uint32_t>(g.lamps.size());
	out[1] = g.lamps_lit;
	out[2] = g.lamps_unbaked_used;
	out[3] = g.lamps_area;
	out[4] = g.last.lamps;
}

// From the worldfx Ashita addon (0.10) every few frames: lights for glowing things the game lights nothing with, 10
// words each as worldfx_lamps takes them: an id (the addon's own), the kind (2 a glowing fixture, 3 a glowing effect),
// its colour (r, g, b), where it is (x, y, z: the game's world, y down), how far it reaches and how bright (as a
// lamp's: mostly 2 to 20), the last eight floats as their bits. Only sane ones are kept, at most 64; a glow not sent
// is gone.
extern "C" __declspec(dllexport) void __cdecl worldfx_glows(const uint32_t *words, uint32_t count)
{
	std::lock_guard<std::mutex> lock(g.mutex);
	count = words != nullptr ? std::min<uint32_t>(count, 64) : 0;
	g.glows.clear();
	for (uint32_t i = 0; i < count; ++i)
	{
		const uint32_t *w = words + size_t(i) * 10;
		float f[8];
		std::memcpy(f, w + 2, sizeof(f));
		bool ok = w[1] == 2 || w[1] == 3;
		for (int k = 0; k < 8 && ok; ++k)
			ok = std::isfinite(f[k]);
		if (!ok || !(f[7] > 0.0f) || !(f[6] > 0.0f) || f[6] > kLampMaxRange || std::fabs(f[3]) > 100000.0f || std::fabs(f[4]) > 100000.0f || std::fabs(f[5]) > 100000.0f)
			continue;
		g.glows.push_back({ w[0], w[1], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7] });
	}
	g.glows_at = GetTickCount64();
	g.glow_sends++;
}

// The glows, for tests: out[0] glows held (the last sent), [1] of them fixtures, [2] glows that went to the renderer
// last frame, [3] effects' glows left out last frame by a lit lamp, [4] lights (lamps and glows) the last drawing added
extern "C" __declspec(dllexport) void __cdecl worldfx_glow_stats(uint32_t out[5])
{
	std::lock_guard<std::mutex> lock(g.mutex);
	out[0] = static_cast<uint32_t>(g.glows.size());
	out[1] = static_cast<uint32_t>(std::count_if(g.glows.begin(), g.glows.end(), [](const addon_state::glow_light &l) { return l.kind == 2; }));
	out[2] = g.glows_used;
	out[3] = g.glows_by_lamps;
	out[4] = g.last.lamps;
}

// From the worldfx Ashita addon (0.7) every other frame: the game's weather, grass::kWeatherWords words as
// grass_render.hpp has them (the weather shown and the one it's changing from, the fog the world is drawn with this
// frame, the record the zone is lit by), as the game keeps them: floats as their bits (read as words: never as floats
// in Lua). What doesn't make sense is left out (decode_weather).
extern "C" __declspec(dllexport) void __cdecl worldfx_weather(const uint32_t *words, uint32_t count)
{
	std::lock_guard<std::mutex> lock(g.mutex);
	grass::decode_weather(words, words != nullptr ? std::min(count, grass::kWeatherWords) : 0, g.weather_real);
	g.weather = weather_forced_over(g.weather_real); // (debug: a forced weather over it, worldfx::force_weather)
	g.weather_at = GetTickCount64();
	g.weather_sends++;
}

// From HiFi XI's game side each frame: the game's Vana'diel clock (Earth seconds since 2002-01-01 JST, as FFXiMain
// 1039E0 works it out), for the moon's phase (moon_now)
extern "C" __declspec(dllexport) void __cdecl worldfx_moon(uint32_t clock)
{
	std::lock_guard<std::mutex> lock(g.mutex);
	g.moon_clock = clock;
	g.moon_clock_at = GetTickCount64();
}

// The weather as the last drawing used it, for tests: out[0] the game's fog on (1, else 0), [1..3] its colour, [4]
// where it starts, [5] where it ends, [6] the share of the sun's shadows the weather leaves (floats as their bits),
// [7] the weather's id, [8] its name (4 characters), [9] how many times the Ashita addon has sent it, [10] the wet
// look drawn (1, else 0), [11] how wet the ground is (float bits)
extern "C" __declspec(dllexport) void __cdecl worldfx_weather_stats(uint32_t out[12])
{
	std::lock_guard<std::mutex> lock(g.mutex);
	const float f[6] = { g.look.haze_color[0], g.look.haze_color[1], g.look.haze_color[2], g.look.haze_start, g.look.haze_end, g.look.weather_sun };
	out[0] = g.look.haze ? 1u : 0u;
	std::memcpy(out + 1, f, sizeof(f));
	out[7] = g.weather.id;
	out[8] = g.weather.name;
	out[9] = g.weather_sends;
	out[10] = g.last.wet ? 1u : 0u;
	std::memcpy(out + 11, &g.wetness, 4);
}

// The rain as the last drawing had it, for tests: out[0] its drops' places drawn (instances), [1] splash places, [2]
// how hard it rained (float bits: the look's), [3] the zone of the rain map loaded (0 none), [4] the map's window
// written again in that drawing (1, else 0), [5] how wet the ground is (float bits)
extern "C" __declspec(dllexport) void __cdecl worldfx_rain_stats(uint32_t out[6])
{
	std::lock_guard<std::mutex> lock(g.mutex);
	out[0] = g.last.rain_drops;
	out[1] = g.last.splashes;
	std::memcpy(out + 2, &g.look.rain, 4);
	out[3] = g.renderer != nullptr && g.renderer->rain_loaded() ? g.renderer->rain_zone() : 0u;
	out[4] = g.last.rain_map_moved ? 1u : 0u;
	std::memcpy(out + 5, &g.wetness, 4);
}

// The snow as the last drawing had it, for tests: out[0] its flakes' places drawn (instances), [1] how hard it snowed
// (float bits: the look's), [2] how far the wind slants it (yalms across for each yalm fallen, float bits), [3] how
// far its flakes sway (yalms, float bits)
extern "C" __declspec(dllexport) void __cdecl worldfx_snow_stats(uint32_t out[4])
{
	std::lock_guard<std::mutex> lock(g.mutex);
	out[0] = g.last.snow_flakes;
	std::memcpy(out + 1, &g.look.snow, 4);
	const float slant = std::sqrt(g.look.rain_slant[0] * g.look.rain_slant[0] + g.look.rain_slant[1] * g.look.rain_slant[1]);
	std::memcpy(out + 2, &slant, 4);
	std::memcpy(out + 3, &g.look.snow_sway, 4);
}

// The add-on's own sky as the last drawing had it, for tests: out[0] repainted in the shading pass (1, else 0), [1] the
// dome's segments it was drawn for (0 none), [2] the sun's (or moon's) strength, [3] the stars' (float bits), [4] 1 the moon
extern "C" __declspec(dllexport) void __cdecl worldfx_sky_stats(uint32_t out[5])
{
	std::lock_guard<std::mutex> lock(g.mutex);
	out[0] = g.last.sky ? 1u : 0u;
	out[1] = g.look.dome.on ? g.look.dome.segments : 0u;
	std::memcpy(out + 2, &g.look.dome.sun, 4);
	std::memcpy(out + 3, &g.look.dome.stars, 4);
	out[4] = g.look.dome.moon > 0.5f ? 1u : 0u;
}

// For HiFi XI's game side (src/game/game.hpp's worldfx_weather_drawn, whose kWeatherFx* bits these are, the same values):
// what of the weather World FX draws of its own now, so the game's own can be left out (gamerain).
//   - kWeatherFxAll while World FX draws all of the zone's weather (weather_all: drawing, WeatherOwn and its sky on, the
//     zone under open sky, the weather sent lately with a dome to repaint): the game's weather elements are all left out
//     then, its sky layers too.
//     Each of World FX's own parts can still be off (its setting): only World FX's goes then, the game's stays left out.
//   - kWeatherFxSky while its sky is drawn (the record's dome repainted).
//   - kWeatherFxRain, kWeatherFxSnow when it would draw them in this zone if that weather came, as the game side has always
//     hidden them: the zone's rain map loaded, the Rain (Snow) setting on and its amount over 0. Whatever the weather.
//   - the rest only with kWeatherFxAll, for what the weather now (or the one it's changing from) has of them and World FX
//     draws (its setting on): clouds (and mist's veil) with its sky; lightning in thunder and thunderstorms; dust and sand
//     storms; wind and gales; hot spells and heat waves; auroras with its sky; gloom and darkness.
namespace
{
	constexpr uint32_t kFxAll = 1u << 0, kFxSky = 1u << 1, kFxClouds = 1u << 2, kFxRain = 1u << 3, kFxSnow = 1u << 4, kFxLightning = 1u << 5,
		kFxDust = 1u << 6, kFxWind = 1u << 7, kFxHeat = 1u << 8, kFxAurora = 1u << 9, kFxGloom = 1u << 10;

	// (mutex held)
	uint32_t weather_bits()
	{
		if (!g_has_drawn || g.renderer == nullptr)
			return 0;
		uint32_t bits = 0;
		const bool map = g.zone_known && g.renderer->rain_loaded() && g.renderer->rain_zone() == g.zone;
		if (map && g.rain_on && g.rain_amount > 0.0f)
			bits |= kFxRain;
		if (map && g.snow_on && g.snow_amount > 0.0f)
			bits |= kFxSnow;
		if (g.look.dome.on)
			bits |= kFxSky;
		if (!weather_all())
			return bits;
		bits |= kFxAll;
		const grass::weather_kind_fx k = weather_fresh() ? grass::weather_fx_of(g.weather) : grass::weather_kind_fx();
		if (g.clouds_on && g.look.dome.on && (k.cover > 0.0f || k.veil > 0.0f))
			bits |= kFxClouds;
		if (g.lightning_on && k.flashes > 0.0f)
			bits |= kFxLightning;
		if (g.weather_dust > 0.0f && k.dust > 0.0f)
			bits |= kFxDust;
		if (g.weather_wind > 0.0f && k.windy > 0.0f)
			bits |= kFxWind;
		if (g.heat_shimmer > 0.0f && k.heat > 0.0f)
			bits |= kFxHeat;
		if (g.aurora_amount > 0.0f && k.aurora > 0.0f && g.look.dome.on)
			bits |= kFxAurora;
		if (g.gloom_fog > 0.0f && k.gloom > 0.0f)
			bits |= kFxGloom;
		return bits;
	}
}

extern "C" __declspec(dllexport) uint32_t __cdecl worldfx_weather_drawn()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	return weather_bits();
}

// The weather of its own as the last drawing had it (look::wx), for tests: out[0] all of the zone's weather World FX's own
// then (1, else 0), [1] the clouds' cover, [2] the lightning's flash, [3] its bolt, [4] the dust, [5] the wind, [6] the
// heat, [7] the aurora, [8] gloom's fog banks, [9] the stars' boost (float bits: the look's, settings in them), [10] the
// flashes since the add-on loaded, [11] ms until the next one comes (UINT32_MAX: none waited for), [12] what the last
// drawing drew of it (1 its parameters went to the passes, 2 the air pass ran, 4 heat shimmer, 8 leaves, 16 the clouds'
// own pass, 32 with the frame before's picture of them carried over), [13] the ground
// under the camera (the game's y, float bits), [14] the streaks, [15] the leaves (float bits)
extern "C" __declspec(dllexport) void __cdecl worldfx_weather_fx_stats(uint32_t out[16])
{
	std::lock_guard<std::mutex> lock(g.mutex);
	const grass::look::weather_fx &x = g.look.wx;
	out[0] = g.wx_on ? 1u : 0u;
	const float f[9] = { x.cloud_cover, x.flash, x.bolt, x.dust, x.wind_strength, x.heat, x.aurora, x.gloom, x.star_boost };
	std::memcpy(out + 1, f, sizeof(f));
	out[10] = g.flashes.count;
	const ULONGLONG now = GetTickCount64();
	out[11] = g.flashes.next == 0 ? UINT32_MAX : g.flashes.next > now ? uint32_t(std::min<ULONGLONG>(g.flashes.next - now, UINT32_MAX - 1)) : 0u;
	out[12] = (g.last.weather ? 1u : 0u) | (g.last.air ? 2u : 0u) | (g.last.shimmer ? 4u : 0u) | (g.last.leaves != 0 ? 8u : 0u) |
		(g.last.clouds ? 16u : 0u) | (g.last.clouds_carried ? 32u : 0u);
	std::memcpy(out + 13, &x.ground_y, 4);
	std::memcpy(out + 14, &x.streaks, 4);
	std::memcpy(out + 15, &x.leaves, 4);
}

// A setting changed as a host's panel or menu changes it (a flag 0 or 1, a number; worldfx::set_setting: kept to its range,
// used from the next frame on, saved): for tests, and hosts with no menu of their own. 1, or 0 when there's no such setting
// (or it's a colour).
extern "C" __declspec(dllexport) uint32_t __cdecl worldfx_set_setting(const char *key, float value)
{
	const worldfx::setting *s = worldfx::find_setting(key);
	if (s == nullptr || s->kind == worldfx::setting::colour)
		return 0;
	const float v[3] = { value, 0.0f, 0.0f };
	return worldfx::set_setting(key, v) ? 1u : 0u;
}

// The sea as the last drawing had it, for tests: out[0] its sheets' corners loaded (0 none), [1] drawn last frame, [2] the
// baked water's corners drawn, [3] how far out it reaches (yalms, float bits: look::sea_distance)
extern "C" __declspec(dllexport) void __cdecl worldfx_sea_stats(uint32_t out[4])
{
	std::lock_guard<std::mutex> lock(g.mutex);
	out[0] = g.renderer != nullptr ? g.renderer->sea_corners() : 0u;
	out[1] = g.last.sea_corners;
	out[2] = g.last.water_corners;
	std::memcpy(out + 3, &g.look.sea_distance, 4);
}

// The rings on the water, for tests: out[0] how many made since the add-on loaded, [1] how many spread now
extern "C" __declspec(dllexport) void __cdecl worldfx_ripple_stats(uint32_t out[2])
{
	std::lock_guard<std::mutex> lock(g.mutex);
	out[0] = g.ripples_made;
	out[1] = ripples_for_drawing(GetTickCount64());
}

// The last drawing at the world's end, for tests: out[0] blades, [1] draws, [2] the shading pass ran,
// [3] the game's state was put back after, [4] the sun's shadows were worked out
extern "C" __declspec(dllexport) void __cdecl worldfx_last_draw(uint32_t out[5])
{
	std::lock_guard<std::mutex> lock(g.mutex);
	out[0] = g.last.blades + (g.last_at_marker ? g.marker_blades.blades : 0);
	out[1] = g.last.draws + (g.last_at_marker ? g.marker_blades.draws : 0);
	out[2] = g.last.shaded ? 1u : 0u;
	out[3] = g.last.bound ? 1u : 0u;
	out[4] = g.last.sun ? 1u : 0u;
}

// For the tests: the world found (width, height; 0 0 none), whether the zone markers name it now (1), how many
// markers have come since the device was attached
extern "C" __declspec(dllexport) void __cdecl worldfx_world(uint32_t out[4])
{
	std::lock_guard<std::mutex> lock(g.mutex);
	const D3D12_RESOURCE_DESC w = g.world_rt != nullptr ? g.world_rt->GetDesc() : D3D12_RESOURCE_DESC {};
	out[0] = uint32_t(w.Width);
	out[1] = w.Height;
	out[2] = marked() ? 1u : 0u;
	out[3] = uint32_t(g.markers);
}

// For the end-to-end test: whether the last frame read the sun's shadow map, and how many times it was drawn
// again since the zone loaded
extern "C" __declspec(dllexport) void __cdecl worldfx_sun_map(uint32_t out[2])
{
	std::lock_guard<std::mutex> lock(g.mutex);
	out[0] = g.last.sun_map ? 1u : 0u;
	out[1] = g.sun_map_redraws;
}

// The game's visibility tests (vistest.hpp). The worldfx Ashita addon points the game's calls of its test (FFXiMain
// 06C280: thiscall on a test object, 9 arguments, ret 24h, 0..256 back) at these entries, one for each call site's
// kind. Each runs on the game's thread: it asks for the test (its box goes to the GPU at the world's end) and answers
// with that test object's newest answer, a few frames old. The game's own test runs instead only while no answers
// are coming in (the feature just turned on, zoning, the device gone); that waits on the GPU as before, for those
// frames only. A test object with no answer yet (new in view, or gone a minute) is answered vistest::kUnknown
// (hidden) until its first is in, a few frames on, instead of waiting for the game's test. A box with none
// of it on the screen is answered 0 here at once, as the game's test does without drawing. (The game's test leaves
// its own render states set when it draws, which an answer from here doesn't: the draws after it then see the states
// before it, as they do on every frame where nothing on the screen is tested.)
namespace
{
	// (a thiscall function seen as fastcall: this in ecx, edx unused, the rest on the stack, popped by the callee)
	using vis_test_fn = int32_t(__fastcall *)(void *, void *, const float *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
	std::atomic<uintptr_t> vis_original { 0 }; // the game's test
	std::atomic<bool> vis_wanted { false };
	std::atomic<uint32_t> vis_answered { 0 }, vis_by_game { 0 };

	template <uint32_t Kind>
	int32_t __fastcall vis_entry(void *object, void *edx, const float *rect, uint32_t depth, uint32_t width, uint32_t height,
		uint32_t a5, uint32_t a6, uint32_t a7, uint32_t use_depth, uint32_t a9)
	{
		const auto original = reinterpret_cast<vis_test_fn>(vis_original.load());
		if (vis_wanted.load() && rect != nullptr && object != nullptr)
		{
			vistest::box b;
			b.x0 = rect[0];
			b.y0 = rect[1];
			b.x1 = rect[2];
			b.y1 = rect[3];
			// (the game's test uses the depth asked for only with its 8th argument set, else just short of 1)
			const uint32_t z = use_depth != 0 ? depth : 0x3F7FFFFEu;
			std::memcpy(&b.depth, &z, 4);
			std::memcpy(&b.width, &width, 4);
			std::memcpy(&b.height, &height, 4);
			b.kind = Kind;
			if (std::isfinite(b.x0) && std::isfinite(b.y0) && std::isfinite(b.x1) && std::isfinite(b.y1) && std::isfinite(b.depth) &&
				b.width >= 1.0f && b.height >= 1.0f && b.width <= 65536.0f && b.height <= 65536.0f &&
				std::fabs(b.x0) < 1e9f && std::fabs(b.y0) < 1e9f && std::fabs(b.x1) < 1e9f && std::fabs(b.y1) < 1e9f)
			{
				// (none of it on the screen: the game's test answers 0 at once, drawing nothing; so does this)
				if (vistest::off_screen(b))
				{
					vis_answered++;
					return 0;
				}
				const uint64_t now = GetTickCount64();
				const int32_t known = vistest::ask(uint32_t(reinterpret_cast<uintptr_t>(object)), b, now);
				if (known >= 0 && vistest::live(now))
				{
					vis_answered++;
					return known;
				}
			}
		}
		vis_by_game++;
		return original != nullptr ? original(object, edx, rect, depth, width, height, a5, a6, a7, use_depth, a9) : 256;
	}
}

// The entry for a kind of call site (vistest::kind), or 0
extern "C" __declspec(dllexport) uintptr_t __cdecl worldfx_vis_entry(uint32_t kind)
{
	switch (kind)
	{
	case vistest::glow: return reinterpret_cast<uintptr_t>(&vis_entry<vistest::glow>);
	case vistest::glow_fade: return reinterpret_cast<uintptr_t>(&vis_entry<vistest::glow_fade>);
	case vistest::flare: return reinterpret_cast<uintptr_t>(&vis_entry<vistest::flare>);
	case vistest::door: return reinterpret_cast<uintptr_t>(&vis_entry<vistest::door>);
	case vistest::actor: return reinterpret_cast<uintptr_t>(&vis_entry<vistest::actor>);
	default: return 0;
	}
}

// From the Ashita addon: the game's test (where its calls went), and on or off. Off forgets every answer; the
// addon puts the calls back first.
extern "C" __declspec(dllexport) void __cdecl worldfx_vis_setup(uintptr_t original, uint32_t on)
{
	if (original != 0)
		vis_original = original;
	vis_wanted = on != 0 && vis_original.load() != 0;
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.vis_on = vis_wanted.load();
	}
	if (!vis_wanted.load())
		vistest::reset();
}

// The characters' and monsters' tests (vistest::actor: kept apart from worldfx_vis_stats, whose 12 words the
// worldfx Ashita addon reads too): out[0] asked for, out[1] answers kept for them
extern "C" __declspec(dllexport) void __cdecl worldfx_vis_actors(uint32_t out[2])
{
	const vistest::stats s = vistest::get_stats();
	out[0] = uint32_t(s.by_kind[vistest::actor]);
	out[1] = s.objects;
}

// For /worldfx vistest: out[0] tests answered from the depth copy, [1] run by the game, [2] boxes sent to the GPU,
// [3] answers read back, [4] test objects with an answer, [5] boxes dropped (over kMaxBoxes in a frame, or no slot
// free), [6] ms since the last answers came in (UINT32_MAX: none), [7..10] tests asked for by kind, [11] 1 while on
extern "C" __declspec(dllexport) void __cdecl worldfx_vis_stats(uint32_t out[12])
{
	const vistest::stats s = vistest::get_stats();
	const uint64_t now = GetTickCount64();
	out[0] = vis_answered.load();
	out[1] = vis_by_game.load();
	out[2] = uint32_t(s.taken);
	out[3] = uint32_t(s.answered);
	out[4] = s.objects;
	uint64_t skipped = 0;
	bool on = false;
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		skipped = g.vis_skipped;
		on = g.vis_on;
	}
	out[5] = uint32_t(s.dropped + skipped);
	out[6] = s.last_answer_ms != 0 ? uint32_t(std::min<uint64_t>(now - s.last_answer_ms, UINT32_MAX - 1)) : UINT32_MAX;
	for (uint32_t k = 0; k < 4; ++k) // (glow, fade, flare, door: the characters' are worldfx_vis_actors')
		out[7 + k] = uint32_t(s.by_kind[k]);
	out[11] = on ? 1u : 0u;
}

// The panel's text, formatted here and drawn through the host's UI
namespace
{
	template <typename... A>
	void text(const worldfx::ui &u, const char *format, A... args)
	{
		char s[2048];
		std::snprintf(s, sizeof(s), format, args...);
		u.text(s);
	}
	template <typename... A>
	void text_colored(const worldfx::ui &u, const float *rgba, const char *format, A... args)
	{
		char s[2048];
		std::snprintf(s, sizeof(s), format, args...);
		u.text_colored(rgba, s);
	}
	template <typename... A>
	void text_disabled(const worldfx::ui &u, const char *format, A... args)
	{
		char s[2048];
		std::snprintf(s, sizeof(s), format, args...);
		u.text_disabled(s);
	}
	template <typename... A>
	void text_wrapped(const worldfx::ui &u, const char *format, A... args)
	{
		char s[2048];
		std::snprintf(s, sizeof(s), format, args...);
		u.text_wrapped(s);
	}
}

// The panel's status lines (mutex held)
static void status_lines(const worldfx::ui &u)
{
	static const float good[4] = { 0.55f, 0.85f, 0.45f, 1.0f }, warn[4] = { 0.95f, 0.75f, 0.3f, 1.0f };
	if (g.world_rt != nullptr)
	{
		const D3D12_RESOURCE_DESC w = g.world_rt->GetDesc();
		text(u, "The world: %llux%u, found %s", static_cast<unsigned long long>(w.Width), w.Height,
			marked() ? "by the game's zone marker" : "by what's drawn most (no zone markers lately)");
	}
	else
		u.text_disabled(g.markers != 0 ? "The world: not found yet (zone markers seen, but not yet with its target bound)" : "The world: not found yet");
	{
		const double h = vanadiel_hour();
		text(u, "Vana'diel time %02d:%02d, sun %s%s; this zone: %s", int(h), int((h - int(h)) * 60.0),
			h > 6.0 && h < 18.0 ? "up" : "down", g.moon_now ? " (the moon's shadows)" : "", g.zone_sunlit ? "open sky" : "no sky (no sun shadows)");
		{
			const moon_phase m = moon_now();
			text(u, "The moon: %.0f%% lit, %s (day %.1f of its 84, from %s)", m.lit * 100.0f, m.waxing ? "waxing" : "waning", m.cycle * 84.0f,
				m.from_game ? "the game's clock" : "the PC's clock");
		}
		// (how high, and the compass bearing it's toward: x east, z north, y down)
		auto where = [](const float *s, char *out, size_t size) {
			std::snprintf(out, size, "%.0f deg up, bearing %.0f", std::asin(std::clamp(-s[1], -1.0f, 1.0f)) * 57.29578f,
				std::fmod(std::atan2(s[0], s[2]) * 57.29578f + 360.0f, 360.0f));
		};
		char game[64] = "not found (from the hour)", hour[64];
		if (g.sun_from_game)
			where(g.game_sun, game, sizeof(game));
		where(g.hour_sun, hour, sizeof(hour));
		text(u, "The game's light: %s; from the hour: %s", game, hour);
	}
	text(u, "Shadow map: %s; %s, drawn again %u times", g.shadow_status.c_str(), g.last.sun_map ? "in use" : "not in use", g.sun_map_redraws);
	if (weather_fresh())
		text_wrapped(u, "Weather: %s", weather_text().c_str());
	else
		u.text_disabled("Weather: not known (the worldfx Ashita addon sends it)");
	text(u, "Clear sky: %s", g.clear_sky_status.c_str());
	text(u, "Wet ground: %s; wet now %.0f%%%s", g.wet_status.c_str(), g.wetness * 100.0f, g.last.wet ? " (drawn)" : "");
	text(u, "Settled snow: %.0f%% lying%s", g.snow_cover * 100.0f, g.last.snow_cover ? " (drawn)" : "");
	text(u, "Rain map: %s; raining now %.0f%%%s", g.rain_status.c_str(), g.raining * 100.0f,
		g.last.rain_drops != 0 ? " (drawn)" : "");
	text(u, "Snowing now %.0f%%%s", g.snowing * 100.0f, g.last.snow_flakes != 0 ? " (drawn)" : "");
	text_wrapped(u, "Sky of its own: %s", sky_text().c_str());
	text_wrapped(u, "Weather of its own: %s", weather_fx_text().c_str());
	if (GetTickCount64() - g.live_at < kLiveLightsMs)
		text_wrapped(u, "Lamps: %s; lit in the game now: %u of them, %u other lights used, %u whole-area lights left out; glowing last frame: %u",
			g.lamps_status.c_str(), g.lamps_lit, g.lamps_unbaked_used, g.lamps_area, g.last.lamps);
	else
		text_wrapped(u, "Lamps: %s; which are lit isn't known (the worldfx Ashita addon sends that)", g.lamps_status.c_str());
	if (GetTickCount64() - g.glows_at < kLiveLightsMs)
	{
		const auto fixtures = std::count_if(g.glows.begin(), g.glows.end(), [](const addon_state::glow_light &l) { return l.kind == 2; });
		text(u, "Glows sent now: %u fixtures, %u effects; glowing last frame: %u (%u effects left to the lamp they're by)",
			static_cast<unsigned>(fixtures), static_cast<unsigned>(g.glows.size() - fixtures), g.glows_used, g.glows_by_lamps);
	}
	else
		u.text_disabled("Glows: none sent lately (the worldfx Ashita addon sends them)");
	text(u, "Water: %s", g.water_status.c_str());
	text(u, "Sea: %s%s", g.sea_status.c_str(), g.last.sea_corners != 0 ? " (drawn)" : "");
	text(u, "Rings from moving through the water: %u made, %u spreading now", g.ripples_made, g.look.ripple_count);
	if (g.look.fog_known)
		text(u, "Sky along the horizon (the zone's fog): %.2f %.2f %.2f", g.look.fog[0], g.look.fog[1], g.look.fog[2]);
	text(u, "Fireflies: night %.0f%%, %u homes near the camera last frame", g.look.night * 100.0f, g.last.fireflies);
	{
		const grass::look::look_pass &p = g.look.post;
		const float *k = g.grade_parts;
		text(u, "The look: tone mapping %s, bloom %.2f, grade %.2f%s; last frame %s (%u levels); grade now: twilight %.2f, night %.2f, "
			"rain %.2f, gloom %.2f, heat %.2f, dust %.2f, snow %.2f, mist %.2f (gain %.2f %.2f %.2f, saturation %.2f, contrast %.2f)",
			g.tonemap ? "on" : "off", g.bloom, g.colour_grade, g.zone_sunlit ? "" : " (none here: no open sky)", g.last.look ? "drawn" : "not drawn",
			g.last.look_levels, k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], p.gain[0], p.gain[1], p.gain[2], p.saturation, p.contrast);
	}
	if (GetTickCount64() - g.actors_at < 500)
	{
		uint32_t fighting = 0;
		for (uint32_t i = 0; i < g.actor_count; ++i)
			fighting += (g.fighting >> i) & 1u;
		text(u, "Pushing the grass: %u characters and monsters near, %u of them fighting", g.actor_count, fighting);
	}
	else
		u.text_disabled("Pushing the grass: nobody (the worldfx Ashita addon sends them)");
	if (g.cuts_total != 0)
		text(u, "Weapon skills cut the grass %u times, the last %.0f s ago%s", g.cuts_total, (GetTickCount64() - g.last_cut_at) / 1000.0,
			g.grass_cuts ? "" : " (cuts off)");
	else
		u.text_disabled("Weapon skills cut the grass: none yet (the worldfx Ashita addon 0.14 sends them)");
	if (g.dev != nullptr)
	{
		text(u, "Blades: %s", g.blades_status.c_str());
		uint32_t drew = 0;
		for (uint32_t i = 0; i < g.history_count; ++i)
			drew += g.history[i] ? 1 : 0;
		text_colored(u, drew != 0 && drew == g.history_count ? good : warn, "Drawn in %u of the last %u frames.", drew, g.history_count);
		const uint32_t last_blades = g.last.blades + (g.last_at_marker ? g.marker_blades.blades : 0);
		const uint32_t last_draws = g.last.draws + (g.last_at_marker ? g.marker_blades.draws : 0);
		if (last_draws != 0)
			text(u, "Last frame: %u blades in %u draws%s.", last_blades, last_draws,
				g.last_at_marker ? ", at the zone's end (before characters and effects, in the world's depth)" : "");
		else
			text(u, "Last frame: not drawn: %s.", g.skip != nullptr ? g.skip : "-");
		text(u, "Camera: %s", g.game_global != 0 ? "found" : g.game_scanned ? "not found in FFXiMain.dll" : "not looked for yet");
		text_wrapped(u, "Shaders: %s", g.shader_status.c_str());
		if (g.renderer != nullptr)
		{
			text(u, "GPU time a frame: copies and shading %.2f ms, grass %.2f ms", g.renderer->shading_ms(), g.renderer->grass_ms());
			for (const std::string &l : gpu_time_lines(*g.renderer))
				text(u, "%s", l.c_str());
		}
		if (g.pace_count != 0)
		{
			const addon_state::pace &w = g.paces[(g.pace_count - 1) % addon_state::kPaces];
			text(u, "Frame pacing: %.1f ms a frame, the GPU %u presents behind (most since in the game: %u)", w.ms, w.behind, g.most_behind);
		}
		text_wrapped(u, "GPU crash reports: %s%s", g.crash_reports.c_str(), g.crash_reports.rfind("on", 0) == 0 ? " (worldfx-gpu-crash.txt next to the add-on)" : "");
		text_wrapped(u, "Direct3D 12 debug layer: %s", g.debug_layer_status.c_str());
	}
}

void worldfx::draw_panel(const ui &u)
{
	std::lock_guard<std::mutex> lock(g.mutex);
	static const float good[4] = { 0.55f, 0.85f, 0.45f, 1.0f }, warn[4] = { 0.95f, 0.75f, 0.3f, 1.0f };
	// What stops it working goes above the settings; the rest of the status is under "Status"
	if (g.dev == nullptr)
		u.text_colored(warn, "Needs dgVoodoo's Direct3D 12 output (OutputAPI = d3d12_fl11_0).");
	else if (!g.error.empty())
		text_colored(u, warn, "Renderer: %s", g.error.c_str());
	if (g.shading_held != nullptr && shading_on())
		text_colored(u, warn, "World shading held: %s", g.shading_held);

	if (u.header("Grass", true))
	{
		if (u.checkbox("Draw grass", &g.enabled))
			set_config("Grass", g.enabled);
		if (u.slider_float("Grass distance (yalms)", &g.look.distance, 10.0f, 200.0f, "%.0f"))
			set_config("GrassDistance", g.look.distance);
		if (u.slider_float("Blade height (yalms)", &g.look.height, 0.1f, 1.5f, "%.2f"))
			set_config("GrassHeight", g.look.height);
		if (u.slider_int("Blades per tuft", &g.look.tuft, 1, 12))
			set_config("GrassTuft", g.look.tuft);
		if (u.slider_float("Fireflies at night (0 = none)", &g.look.fireflies, 0.0f, 2.0f, "%.2f"))
			set_config("Fireflies", g.look.fireflies);
		if (u.slider_float("Trodden down where people walk and fight (0 = off)", &g.grass_wear, 0.0f, 2.0f, "%.2f"))
			set_config("GrassWear", g.grass_wear);
		if (u.checkbox("Cut by weapon skills", &g.grass_cuts))
			set_config("GrassCuts", g.grass_cuts);
	}

	if (u.header("Light and shadow", true))
	{
		if (u.slider_float("Ambient occlusion (0 = off)", &g.look.ao_strength, 0.0f, 2.0f, "%.2f"))
			set_config("AOStrength", g.look.ao_strength);
		if (u.slider_float("Surface relief (0 = off)", &g.look.relief, 0.0f, 2.0f, "%.2f"))
			set_config("Relief", g.look.relief);
		if (u.slider_float("Sun shadows (0 = off)", &g.sun_shadows, 0.0f, 1.0f, "%.2f"))
			set_config("SunShadows", g.sun_shadows);
		if (u.slider_float("Moon shadows (share of the sun's, 0 = off)", &g.moon_shadows, 0.0f, 1.0f, "%.2f"))
			set_config("MoonShadows", g.moon_shadows);
		if (u.checkbox("Characters cast sun shadows too (the game draws its own)", &g.look.sun_characters))
			set_config("SunCharacters", g.look.sun_characters);
		if (u.slider_float("Lamp glow (0 = off)", &g.lamp_glow, 0.0f, 2.0f, "%.2f"))
			set_config("LampGlow", g.lamp_glow);
		if (u.slider_float("Lamp shadows (0 = off)", &g.lamp_shadows, 0.0f, 1.0f, "%.2f"))
			set_config("LampShadows", g.lamp_shadows);
		if (u.slider_float("Home Points, Survival Guides and such (0 = off)", &g.fixture_glow, 0.0f, 2.0f, "%.2f"))
			set_config("FixtureGlow", g.fixture_glow);
		if (u.slider_float("Glowing effects: spells, abilities, fires... (0 = off)", &g.effect_glow, 0.0f, 2.0f, "%.2f"))
			set_config("EffectGlow", g.effect_glow);
	}

	if (u.header("Weather", true))
	{
		if (u.checkbox("Fog the grass, fireflies and water as the game fogs the world", &g.weather_fog))
			set_config("WeatherFog", g.weather_fog);
		if (u.checkbox("Sun and moon shadows fade as the game dims its sun (cloud, rain...)", &g.weather_shadows))
			set_config("WeatherShadows", g.weather_shadows);
		if (u.slider_float("Wet path and stone in rain (0 = off)", &g.wet_ground, 0.0f, 1.0f, "%.2f"))
			set_config("WetGround", g.wet_ground);
		if (u.slider_float("Snow settling on the ground while it snows (0 = off; needs snow of its own)", &g.snow_cover_amount, 0.0f, 1.0f, "%.2f"))
			set_config("SnowCover", g.snow_cover_amount);
		if (u.slider_float("Its depth (0 = flat)", &g.snow_depth, 0.0f, 2.0f, "%.2f"))
			set_config("SnowDepth", g.snow_depth);
		if (u.checkbox("Trails trodden through it", &g.snow_trails))
			set_config("SnowTrails", g.snow_trails);
		if (u.checkbox("Rain of its own, in place of the game's", &g.rain_on))
			set_config("Rain", g.rain_on);
		if (u.slider_float("How hard it rains (times the weather's)", &g.rain_amount, 0.0f, 2.0f, "%.2f"))
			set_config("RainAmount", g.rain_amount);
		if (u.slider_float("Splashes (0 = none)", &g.rain_splashes, 0.0f, 2.0f, "%.2f"))
			set_config("RainSplashes", g.rain_splashes);
		if (u.slider_float("Rings on the water (0 = none)", &g.rain_rings, 0.0f, 2.0f, "%.2f"))
			set_config("RainRings", g.rain_rings);
		if (u.checkbox("Snow of its own, in place of the game's", &g.snow_on))
			set_config("Snow", g.snow_on);
		if (u.slider_float("How hard it snows (times the weather's)", &g.snow_amount, 0.0f, 2.0f, "%.2f"))
			set_config("SnowAmount", g.snow_amount);
		if (u.checkbox("Sky of its own: the game's gradient smooth, with a sun, a moon and stars", &g.sky_on))
			set_config("Sky", g.sky_on);
		if (u.slider_float("Its sun and moon (0 = none)", &g.sky_sun, 0.0f, 2.0f, "%.2f"))
			set_config("SkySun", g.sky_sun);
		if (u.slider_float("Its stars (0 = none)", &g.sky_stars, 0.0f, 2.0f, "%.2f"))
			set_config("SkyStars", g.sky_stars);
		if (u.checkbox("All of the weather its own (with its sky, under open sky: the game's weather left out, its sky layers too)", &g.weather_own))
			set_config("WeatherOwn", g.weather_own);
		u.text_disabled("  each part below off: only World FX's goes, the game's stays left out");
		if (u.checkbox("Clouds, as thick and dark as the weather has them, drifting with the wind", &g.clouds_on))
			set_config("Clouds", g.clouds_on);
		if (u.slider_int("Clouds' detail (0 low .. 2 high: steps through them)", &g.cloud_quality, 0, 2))
			set_config("CloudQuality", g.cloud_quality);
		if (u.slider_float("Sun shafts in the air (0 = none)", &g.vol_light, 0.0f, 2.0f, "%.2f"))
			set_config("VolLight", g.vol_light);
		if (u.slider_float("Fog lying low, thicker in mist, rain and snow (0 = clear air only)", &g.vol_fog, 0.0f, 2.0f, "%.2f"))
			set_config("VolFog", g.vol_fog);
		if (u.slider_int("Their detail (0 low .. 2 high: steps through the air)", &g.vol_quality, 0, 2))
			set_config("VolQuality", g.vol_quality);
		if (u.slider_int("Clouds' style (0 game-like: soft, in the sky's colours; 1 volumetric heaps)", &g.cloud_style, 0, 1))
			set_config("CloudStyle", g.cloud_style);
		if (u.checkbox("Lightning in thunder and thunderstorms: flashes and bolts", &g.lightning_on))
			set_config("Lightning", g.lightning_on);
		if (u.slider_float("Dust and sand storms (0 = none)", &g.weather_dust, 0.0f, 2.0f, "%.2f"))
			set_config("WeatherDust", g.weather_dust);
		if (u.slider_float("Wind and gales: grass bent, streaks and leaves (0 = none)", &g.weather_wind, 0.0f, 2.0f, "%.2f"))
			set_config("WeatherWind", g.weather_wind);
		if (u.slider_float("Heat shimmer in hot spells and heat waves (0 = none)", &g.heat_shimmer, 0.0f, 2.0f, "%.2f"))
			set_config("HeatShimmer", g.heat_shimmer);
		if (u.slider_float("Auroras (0 = none)", &g.aurora_amount, 0.0f, 2.0f, "%.2f"))
			set_config("Aurora", g.aurora_amount);
		if (u.slider_float("Gloom's fog banks (0 = none)", &g.gloom_fog, 0.0f, 2.0f, "%.2f"))
			set_config("GloomFog", g.gloom_fog);
	}

	if (u.header("Frame rate", false))
	{
		// (the worldfx Ashita addon reads the option from ReShade.ini every few seconds and points the game's calls)
		if (u.checkbox("Answer the game's glow, lamp, lens flare and door visibility tests without waiting on the GPU", &g.vis_option))
			set_config("VisTest", g.vis_option);
		const vistest::stats s = vistest::get_stats();
		const uint64_t now = GetTickCount64();
		if (!g.vis_on)
			u.text_disabled(g.vis_option ? "On, waiting for the worldfx Ashita addon (0.15 or later) to point the game's calls here" : "Off: the game's own tests (each waits for the GPU)");
		else if (s.last_answer_ms == 0 || now - s.last_answer_ms > vistest::kLiveMs)
			u.text_colored(warn, "On, no answers coming in yet: the game's own tests run meanwhile");
		else
			text(u, "On: %u objects answered from the depth copy, %llu boxes tested, %llu dropped", s.objects,
				static_cast<unsigned long long>(s.taken), static_cast<unsigned long long>(s.dropped + g.vis_skipped));
	}

	if (u.header("Water", true))
	{
		if (u.checkbox("Water", &g.look.water))
			set_config("Water", g.look.water);
		if (u.slider_float("Reflections (1 = as real water)", &g.look.water_reflect, 0.0f, 2.0f, "%.2f"))
			set_config("WaterReflections", g.look.water_reflect);
		if (u.slider_float("Ripples (0 = still)", &g.look.water_ripples, 0.0f, 2.0f, "%.2f"))
			set_config("WaterRipples", g.look.water_ripples);
		if (u.slider_float("Darker where deep (0 = off)", &g.look.water_tint, 0.0f, 1.0f, "%.2f"))
			set_config("WaterDepthTint", g.look.water_tint);
		if (u.slider_float("Rings from moving (0 = none)", &g.look.water_rings, 0.0f, 2.0f, "%.2f"))
			set_config("WaterRings", g.look.water_rings);
		if (u.checkbox("The sea to the horizon: the game's own past the zone's edge drawn as World FX water", &g.look.sea))
			set_config("WaterSea", g.look.sea);
	}

	if (u.header("Fine-tuning", false))
	{
		if (u.slider_float("Ambient occlusion radius (yalms)", &g.look.ao_radius, 0.3f, 3.0f, "%.2f"))
			set_config("AORadius", g.look.ao_radius);
		if (u.checkbox("Shadow map from the zone's meshes (what's off screen casts too)", &g.look.sun_map))
			set_config("SunMap", g.look.sun_map);
		if (u.slider_float("Lamp glow reach (times the lamp's own range)", &g.lamp_reach, 0.5f, 3.0f, "%.2f"))
			set_config("LampReach", g.lamp_reach);
		if (u.checkbox("Lamps: also lit lights the zone's lamp file doesn't list (from the game's light table)", &g.lamps_unbaked))
			set_config("LampsUnbaked", g.lamps_unbaked);
		if (u.slider_float("Firefly glow (yalms)", &g.look.firefly_size, 0.05f, 0.4f, "%.2f"))
			set_config("FireflySize", g.look.firefly_size);
		u.text("Grass colour, learned from the ground under each tuft:");
		if (u.checkbox("Only learn from the ground itself (not characters, trees, names, background)", &g.look.rule_in_front))
			set_config("GrassRuleInFront", g.look.rule_in_front);
		if (u.checkbox("Drop samples far brighter or darker than the rest", &g.look.rule_outliers))
			set_config("GrassRuleOutliers", g.look.rule_outliers);
		if (u.color_edit3("Grass hue (where the ground hasn't been seen)", g.look.color))
		{
			set_config("GrassHueR", g.look.color[0]);
			set_config("GrassHueG", g.look.color[1]);
			set_config("GrassHueB", g.look.color[2]);
		}
		if (u.button("Forget the colours learned") && g.renderer != nullptr)
		{
			std::string ignored;
			g.renderer->forget_colours(ignored);
		}
	}

	if (u.header("Status", false))
	{
		status_lines(u);
		if (u.checkbox("Show the zone's water collision (debug)", &g.look.water_debug))
			set_config("WaterDebug", g.look.water_debug);
		u.text_disabled("  blue deep, cyan shallow, yellow steep: at or behind what the game drew; red: something in front of it");
		if (u.checkbox("Show where the sky of its own repaints (debug)", &g.sky_debug))
			set_config("SkyDebug", g.sky_debug);
		u.text_disabled("  magenta: repainted; red: sky left as the game drew it (clouds, its sun and moon, effects over its dome)");
	}
}

// The settings by name (worldfx::setting): each one's kind, range and default, and where it lives. The defaults are
// what the state starts with, taken when the table is first made (load_settings, before anything is read).
namespace
{
	struct setting_slot
	{
		worldfx::setting info;
		bool *flag = nullptr;
		float *number = nullptr; // (a colour: three)
		int *whole = nullptr;
	};

	// Read before the device is made (before_device) and only kept here for the menu: they take effect at the next start
	bool g_crash_reports_setting = true, g_debug_layer_setting = false;
	// The game's own lens flare, water, rain and character shadows hidden: HiFi XI's game-side parts follow these
	// (worldfx_setting), the core doesn't (the World FX add-on's Ashita addon had its own commands for them)
	bool g_hide_flare = true, g_hide_game_water = true, g_hide_game_rain = true, g_hide_game_shadows = true;

	std::vector<setting_slot> &slots()
	{
		static std::vector<setting_slot> table = [] {
			std::vector<setting_slot> v;
			auto flag = [&v](const char *key, bool &value) {
				setting_slot s;
				s.info = { key, worldfx::setting::flag, 0.0f, 1.0f, { value ? 1.0f : 0.0f, 0.0f, 0.0f } };
				s.flag = &value;
				v.push_back(s);
			};
			auto number = [&v](const char *key, float &value, float low, float high) {
				setting_slot s;
				s.info = { key, worldfx::setting::number, low, high, { value, 0.0f, 0.0f } };
				s.number = &value;
				v.push_back(s);
			};
			auto whole = [&v](const char *key, int &value, int low, int high) {
				setting_slot s;
				s.info = { key, worldfx::setting::whole, float(low), float(high), { float(value), 0.0f, 0.0f } };
				s.whole = &value;
				v.push_back(s);
			};
			auto colour = [&v](const char *key, float *value) {
				setting_slot s;
				s.info = { key, worldfx::setting::colour, 1.0f, 0.0f, { value[0], value[1], value[2] } };
				s.number = value;
				v.push_back(s);
			};
			constexpr float none_low = 1.0f, none_high = 0.0f; // (no range)
			flag("Grass", g.enabled);
			number("GrassDistance", g.look.distance, 10.0f, 200.0f);
			number("GrassHeight", g.look.height, 0.1f, 1.5f);
			whole("GrassTuft", g.look.tuft, 1, 12);
			colour("GrassHue", g.look.color);
			number("Fireflies", g.look.fireflies, 0.0f, 2.0f);
			number("FireflySize", g.look.firefly_size, 0.05f, 0.4f);
			number("GrassWear", g.grass_wear, 0.0f, 2.0f);
			flag("GrassCuts", g.grass_cuts);
			number("AOStrength", g.look.ao_strength, 0.0f, 2.0f);
			number("AORadius", g.look.ao_radius, 0.3f, 3.0f);
			number("Relief", g.look.relief, 0.0f, 2.0f);
			number("SunShadows", g.sun_shadows, 0.0f, 1.0f);
			number("MoonShadows", g.moon_shadows, 0.0f, 1.0f);
			flag("SunCharacters", g.look.sun_characters);
			flag("SunMap", g.look.sun_map);
			number("LampGlow", g.lamp_glow, 0.0f, 2.0f);
			number("LampReach", g.lamp_reach, 0.5f, 3.0f);
			number("LampShadows", g.lamp_shadows, 0.0f, 1.0f);
			flag("LampsUnbaked", g.lamps_unbaked);
			number("FixtureGlow", g.fixture_glow, 0.0f, 2.0f);
			number("EffectGlow", g.effect_glow, 0.0f, 2.0f);
			flag("WeatherFog", g.weather_fog);
			flag("WeatherShadows", g.weather_shadows);
			number("WetGround", g.wet_ground, 0.0f, 1.0f);
			number("SnowCover", g.snow_cover_amount, 0.0f, 1.0f);
			number("SnowDepth", g.snow_depth, 0.0f, 2.0f);
			flag("SnowTrails", g.snow_trails);
			flag("Rain", g.rain_on);
			number("RainAmount", g.rain_amount, 0.0f, 2.0f);
			number("RainSplashes", g.rain_splashes, 0.0f, 2.0f);
			number("RainRings", g.rain_rings, 0.0f, 2.0f);
			flag("Snow", g.snow_on);
			number("SnowAmount", g.snow_amount, 0.0f, 2.0f);
			flag("Sky", g.sky_on);
			number("SkySun", g.sky_sun, 0.0f, 2.0f);
			number("SkyStars", g.sky_stars, 0.0f, 2.0f);
			flag("WeatherOwn", g.weather_own);
			flag("Clouds", g.clouds_on);
			whole("CloudQuality", g.cloud_quality, 0, 2);
			number("VolLight", g.vol_light, 0.0f, 2.0f);
			number("VolFog", g.vol_fog, 0.0f, 2.0f);
			whole("VolQuality", g.vol_quality, 0, 2);
			whole("CloudStyle", g.cloud_style, 0, 1);
			flag("Lightning", g.lightning_on);
			number("WeatherDust", g.weather_dust, 0.0f, 2.0f);
			number("WeatherWind", g.weather_wind, 0.0f, 2.0f);
			number("HeatShimmer", g.heat_shimmer, 0.0f, 2.0f);
			number("Aurora", g.aurora_amount, 0.0f, 2.0f);
			number("GloomFog", g.gloom_fog, 0.0f, 2.0f);
			flag("Tonemap", g.tonemap);
			number("Bloom", g.bloom, 0.0f, 2.0f);
			number("ColourGrade", g.colour_grade, 0.0f, 2.0f);
			flag("VisTest", g.vis_option);
			flag("GrassRuleInFront", g.look.rule_in_front);
			flag("GrassRuleOutliers", g.look.rule_outliers);
			flag("WaterDebug", g.look.water_debug);
			flag("SkyDebug", g.sky_debug);
			flag("Water", g.look.water);
			number("WaterReflections", g.look.water_reflect, 0.0f, 2.0f);
			number("WaterRipples", g.look.water_ripples, 0.0f, 2.0f);
			number("WaterDepthTint", g.look.water_tint, 0.0f, 1.0f);
			number("WaterRings", g.look.water_rings, 0.0f, 2.0f);
			flag("WaterSea", g.look.sea);
			flag("GpuCrashReports", g_crash_reports_setting);
			flag("D3D12DebugLayer", g_debug_layer_setting);
			flag("HideFlare", g_hide_flare);
			flag("HideGameWater", g_hide_game_water);
			flag("HideGameRain", g_hide_game_rain);
			flag("HideGameShadows", g_hide_game_shadows);
			number("TestVanadielHour", g.test_hour, none_low, none_high);
			number("TestMoon", g.test_moon, none_low, none_high);
			number("TestShadeMaxMs", g.test_shade_max_ms, none_low, none_high);
			return v;
		}();
		return table;
	}

	setting_slot *slot_of(const char *key)
	{
		if (key == nullptr)
			return nullptr;
		for (setting_slot &s : slots())
			if (std::strcmp(s.info.key, key) == 0)
				return &s;
		return nullptr;
	}

	// Kept to its range (a value that isn't a number: the default), as when read
	void keep_in_range(setting_slot &s)
	{
		const worldfx::setting &i = s.info;
		if (s.number != nullptr)
			for (int k = 0; k < (i.kind == worldfx::setting::colour ? 3 : 1); ++k)
			{
				float &v = s.number[k];
				if (!std::isfinite(v))
					v = i.fallback[k];
				else if (i.low <= i.high)
					v = std::clamp(v, i.low, i.high);
			}
		if (s.whole != nullptr && i.low <= i.high)
			*s.whole = std::clamp(*s.whole, int(i.low), int(i.high));
	}

	std::string channel_key(const char *key, int k)
	{
		return std::string(key) + "RGB"[k];
	}

	// (mutex held, or before anything else runs)
	void read_setting(setting_slot &s)
	{
		switch (s.info.kind)
		{
		case worldfx::setting::flag: config_value(s.info.key, *s.flag); break;
		case worldfx::setting::number: config_value(s.info.key, *s.number); break;
		case worldfx::setting::whole: config_value(s.info.key, *s.whole); break;
		case worldfx::setting::colour:
			for (int k = 0; k < 3; ++k)
				config_value(channel_key(s.info.key, k).c_str(), s.number[k]);
			break;
		}
		keep_in_range(s);
	}

	void save_setting(const setting_slot &s)
	{
		switch (s.info.kind)
		{
		case worldfx::setting::flag: set_config(s.info.key, *s.flag); break;
		case worldfx::setting::number: set_config(s.info.key, *s.number); break;
		case worldfx::setting::whole: set_config(s.info.key, *s.whole); break;
		case worldfx::setting::colour:
			for (int k = 0; k < 3; ++k)
				set_config(channel_key(s.info.key, k).c_str(), s.number[k]);
			break;
		}
	}
}

void worldfx::load_settings()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	for (setting_slot &s : slots())
		read_setting(s);
}

size_t worldfx::setting_count()
{
	return slots().size();
}

const worldfx::setting &worldfx::setting_at(size_t index)
{
	return slots()[index].info;
}

const worldfx::setting *worldfx::find_setting(const char *key)
{
	const setting_slot *s = slot_of(key);
	return s != nullptr ? &s->info : nullptr;
}

bool worldfx::get_setting(const char *key, float value[3])
{
	std::lock_guard<std::mutex> lock(g.mutex);
	setting_slot *s = slot_of(key);
	if (s == nullptr)
		return false;
	value[0] = value[1] = value[2] = 0.0f;
	if (s->flag != nullptr)
		value[0] = *s->flag ? 1.0f : 0.0f;
	else if (s->whole != nullptr)
		value[0] = float(*s->whole);
	else
		for (int k = 0; k < (s->info.kind == setting::colour ? 3 : 1); ++k)
			value[k] = s->number[k];
	return true;
}

bool worldfx::set_setting(const char *key, const float value[3])
{
	std::lock_guard<std::mutex> lock(g.mutex);
	setting_slot *s = slot_of(key);
	if (s == nullptr || value == nullptr)
		return false;
	if (s->flag != nullptr)
		*s->flag = value[0] >= 0.5f;
	else if (s->whole != nullptr)
		*s->whole = int(std::lround(std::isfinite(value[0]) ? value[0] : s->info.fallback[0]));
	else
		for (int k = 0; k < (s->info.kind == setting::colour ? 3 : 1); ++k)
			s->number[k] = value[k];
	keep_in_range(*s);
	save_setting(*s);
	return true;
}

void worldfx::reload_zone()
{
	g_reload_zone = true;
}

// The GPU time of World FX's sections (the renderer's markers), the costliest first: each one's smoothed time over the
// frames it ran in, and in how many of the frames timed it ran; then all of it a frame
static std::vector<std::string> gpu_time_lines(const grass::renderer &r)
{
	std::vector<std::string> out;
	const uint32_t frames = r.frames_timed();
	if (frames == 0)
	{
		out.push_back("GPU time by section: none timed yet");
		return out;
	}
	std::vector<const grass::renderer::gpu_section *> order;
	double total = 0.0;
	for (const auto &sec : r.gpu_sections())
	{
		order.push_back(&sec);
		total += double(sec.ms) * std::min(1.0, double(sec.frames) / double(frames));
	}
	std::sort(order.begin(), order.end(), [](const auto *a, const auto *b) { return a->ms > b->ms; });
	char line[256];
	std::snprintf(line, sizeof(line), "GPU time by section (over %u frames; about %.2f ms a frame in all):", frames, total);
	out.push_back(line);
	for (const auto *sec : order)
	{
		std::string name;
		for (wchar_t c : sec->name)
			name += c < 128 ? char(c) : '?';
		if (name.rfind("worldfx: ", 0) == 0)
			name.erase(0, 9);
		std::snprintf(line, sizeof(line), "  %-28s %6.2f ms  (in %3.0f%% of frames)", name.c_str(), sec->ms,
			100.0 * std::min(1.0, double(sec->frames) / double(frames)));
		out.push_back(line);
	}
	return out;
}

std::vector<std::string> worldfx::gpu_times()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	if (g.renderer == nullptr)
		return { "GPU time by section: World FX isn't drawing" };
	return gpu_time_lines(*g.renderer);
}

void worldfx::forget_colours()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	if (g.renderer != nullptr)
	{
		std::string ignored;
		g.renderer->forget_colours(ignored);
	}
}

void worldfx::draw_status(const ui &u)
{
	std::lock_guard<std::mutex> lock(g.mutex);
	status_lines(u);
}
