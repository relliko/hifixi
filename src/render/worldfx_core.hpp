/*
 * World FX's core: what grass, the world shading, water, weather, lamps and the game's visibility tests do on each
 * of dgVoodoo's Direct3D 12 command lists and at each present, with the renderer (grass_render) doing the drawing.
 * It was the ReShade add-on's grass.cpp; since 2026-10-21 it's shared by two hosts that feed it the same events in
 * Direct3D 12's own terms:
 *   - the World FX ReShade add-on (..\reshade-worldfx\src\grass.cpp: ReShade's events and its ini, log and panel);
 *   - HiFi XI's plugin (src\backend: its own hooks on Direct3D 12, its own settings file).
 * The worldfx_* functions the worldfx Ashita addon calls are exported from here, so whichever DLL holds the core
 * answers them.
 */

#pragma once

#include "grass_render.hpp"
#include <d3d12.h>
#include <cstdint>
#include <string>
#include <vector>

namespace worldfx
{
	// What the host gives the core
	struct port
	{
		// A line for the host's log (level 0 info, 1 warning, 2 error)
		void (*log)(int level, const char *text) = nullptr;
		// A setting (false, value untouched, when it isn't set), and one changed in the panel: the add-on's are in
		// ReShade.ini [FFXI_WORLDFX], HiFi XI's in its own file
		bool (*get_bool)(const char *key, bool &value) = nullptr;
		bool (*get_float)(const char *key, float &value) = nullptr;
		bool (*get_int)(const char *key, int &value) = nullptr;
		void (*set_bool)(const char *key, bool value) = nullptr;
		void (*set_float)(const char *key, float value) = nullptr;
		void (*set_int)(const char *key, int value) = nullptr;
		// Where the zone files, worldfx.hlsl, the shader cache, worldfx-sunlit.txt and the crash report are (with a
		// slash at the end): the add-on's folder, which is the game's bootloader folder
		std::wstring folder;
	};
	void set_port(const port &p);
	// The settings, read once (the host calls it after set_port)
	void load_settings();

	// The settings by name, for a host's own menu: each one's kind, range (low > high: none) and default (what it is
	// when not set or not a number). A flag is 0 or 1; a colour is three floats, kept as <key>R, <key>G, <key>B.
	struct setting
	{
		const char *key;
		enum kind_t { flag, number, whole, colour } kind;
		float low, high;
		float fallback[3];
	};
	size_t setting_count();
	const setting &setting_at(size_t index);
	const setting *find_setting(const char *key);
	// A setting's value now; false when there's no such setting
	bool get_setting(const char *key, float value[3]);
	// A new value: kept to its range as when read, used from the next frame on, saved through the port
	bool set_setting(const char *key, const float value[3]);
	// The grass colours learned so far forgotten (learned again from the ground)
	void forget_colours();
	// The zone's files read again (from the next frame: the zone's own were just made, HiFi XI's bakers)
	void reload_zone();
	// The GPU time of each of World FX's sections, the costliest first (lines for /hifi gpu and the status)
	std::vector<std::string> gpu_times();

	// Weather debug (/hifi weather, the menu's "Weather (debug)"): World FX's own weather (its clouds, rain, snow,
	// lightning, dust, wind, heat, aurora, gloom, stars, wet ground) drawn as another weather's, whatever the zone's real
	// one, cross-faded over fade_seconds as a change of the game's. Only World FX's own drawing: nothing of the game's,
	// the server's or its packets; the zone's light, fog and sky colours (the game's record) stay the real weather's,
	// and the game's own weather stays left out as before. Not saved: none forced at every start.
	struct weather_choice
	{
		const char *name;  // the game's 4-letter weather name ("rain")
		const char *label; // what it is ("Rain")
	};
	// The weathers that can be forced, in the game's id order (fine .. dark)
	const weather_choice *weather_choices(size_t &count);
	// Forced to a weather (its 4-letter name or its label, any case), or back to the real one (nullptr, "" or "off");
	// false for a name it doesn't know
	bool force_weather(const char *name, float fade_seconds = 10.0f);
	// The weather forced now, its 4-letter name ("" none), and a line on it (for /hifi status and the menu)
	std::string forced_weather();
	std::string forced_weather_text();
	// Lightning flashes now, with its bolt, whatever the weather (while World FX draws the zone's weather)
	void flash_lightning();
	// For tests: the weather's own look now (grass::weather_look) from the weather World FX draws by (forced or real),
	// and that weather
	void weather_look_now(grass::look::weather_fx &out, grass::weather *drawn = nullptr);


	// The world as our d3d8.dll hands it at a mark (src/host/frameapi.hpp, channel 3): its target and depth (named at the
	// zone's begin) and the states they're in now, bound or not; their views, formats and size when they're the ones bound
	// (the zone's begin and end); the colour the target was last cleared to, the whole of it (the zone's fog)
	struct world_mark
	{
		ID3D12GraphicsCommandList *list = nullptr;
		ID3D12Resource *rt = nullptr, *ds = nullptr;
		D3D12_RESOURCE_STATES rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		D3D12_RESOURCE_STATES ds_planes[2] = { D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE }; // (depth, stencil)
		uint64_t rtv = 0, dsv = 0; // (0: not bound at this mark)
		DXGI_FORMAT rt_format = DXGI_FORMAT_UNKNOWN, ds_format = DXGI_FORMAT_UNKNOWN; // (the views')
		uint32_t width = 0, height = 0;
		bool fog_known = false;
		float fog[3] = {};
	};

	// Before the game's Direct3D 12 device is made: the GPU crash record (GpuCrashReports) and, where the host can
	// have it, the debug layer (D3D12DebugLayer). Once.
	void before_device(bool debug_layer_allowed);
	// Why the crash reports are off, when the host couldn't call before_device in time
	void set_crash_reports_status(const std::string &status);
	// The game's device (its window's): the renderer made for it. Again for another device moves everything there.
	void attach(ID3D12Device *device);
	void detach(ID3D12Device *device);
	ID3D12Device *attached();
	// Shadow casters the host draws itself into the sun's shadow map (grass::renderer::set_casters), for this renderer and
	// any made later. Called under the core's mutex: it mustn't call back into the core.
	void set_casters(grass::renderer::caster_fn fn, void *context);
	// The zone's casters the host draws itself (grass::renderer::set_zone_casters), likewise
	void set_zone_casters(grass::renderer::zone_bounds_fn bounds, grass::renderer::zone_draw_fn draw, void *context);
	// The ground's colour map the host draws for the grass (grass::renderer::set_floor_source), likewise
	void set_floor_source(grass::renderer::floor_fn fn, void *context);
	// The light split the host draws (grass::renderer::set_light_source), likewise
	void set_light_source(grass::renderer::light_fn fn, void *context);
	// The camera of the frame being finished (VIEW and PROJECTION, row vectors), from the host: the zone pass's
	// recorded on the game's thread, when it's fresh (false: the game's camera read now). Called under the core's mutex.
	using camera_source_fn = bool (*)(void *context, float view[16], float proj[16]);
	void set_camera_source(camera_source_fn source, void *context);
	// (status) Frames in the game World FX's pass drew, and the ones it didn't by reason
	std::vector<std::string> frame_counts();
	// The debug overlay's lines (sun shadows: the setting, open sky or not, the map's use): none while the render thread
	// holds World FX's lock (never waited for)
	std::vector<std::string> overlay_lines();
	// The blades' own motion written beside them (FSR 2's: the host's upscale), and its target (grass::renderer::
	// blade_motion: the world's size, in a shader-readable state; frames counts the frames that wrote it)
	void set_blade_motion(bool on);
	bool blade_motion(ID3D12Resource *&texture, uint64_t &frames);
	// At process exit: the device may be gone, its objects are left to the process's end
	void shutdown();

	// The marks, on the game's thread (the core records on the mark's list only there; our d3d8.dll binds all of its own
	// again after). Our d3d8.dll's world end (slotmark::world_end): the first draw or copy reading the world's target,
	// before anything is bound for it; World FX draws into the world here, once after each zone's begin.
	void on_world_end(const world_mark &m);
	// Whether the world is wanted as floats (the FloatWorld setting: light past white, our d3d8.dll asked for it at each mark)
	bool float_world();
	// The game side's zone marker (src/game/slotmark.hpp) through our d3d8.dll: the world is the target and depth bound
	// there, through those views
	void on_world_marker(const world_mark &m);
	// The game side's zone end marker (the world bound, its planes' states as our d3d8.dll has them): the blades drawn
	// there, into the world with its depth written, before the characters and effects (the world's end then draws the
	// rest without them)
	void on_zone_end(const world_mark &m);
	// Each present, on the queue it's presented from (before it)
	void on_present(ID3D12CommandQueue *queue);

	// The settings panel, drawn through the host's UI (ImGui's, ReShade's or Ashita's): a widget returns true when it
	// was changed or clicked
	struct ui
	{
		bool (*checkbox)(const char *label, bool *value) = nullptr;
		bool (*slider_float)(const char *label, float *value, float low, float high, const char *format) = nullptr;
		bool (*slider_int)(const char *label, int *value, int low, int high) = nullptr;
		bool (*color_edit3)(const char *label, float rgb[3]) = nullptr; // (no inputs: the picker alone)
		bool (*button)(const char *label) = nullptr;
		bool (*header)(const char *label, bool default_open) = nullptr; // (a collapsing header: true while open)
		void (*text)(const char *text) = nullptr;
		void (*text_colored)(const float rgba[4], const char *text) = nullptr;
		void (*text_disabled)(const char *text) = nullptr;
		void (*text_wrapped)(const char *text) = nullptr;
	};
	void draw_panel(const ui &u);
	// Its status lines alone (what's drawn, the zone's files, the weather, GPU time, pacing, crash reports)
	void draw_status(const ui &u);
}
