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
		// The resource a render target or depth view (a CPU descriptor handle) is of now, nullptr when not known;
		// and the view's format (DXGI_FORMAT_UNKNOWN when not known: the resource's is used)
		ID3D12Resource *(*resource_of_view)(uint64_t view) = nullptr;
		DXGI_FORMAT (*format_of_view)(uint64_t view) = nullptr;
		// A root signature's parameters, for the log: how many (0: not known) and each one's kind ("constants",
		// "table", "root descriptor")
		size_t (*root_param_count)(ID3D12RootSignature *root) = nullptr;
		const char *(*root_param)(ID3D12RootSignature *root, size_t index) = nullptr;
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

	// What dgVoodoo has bound on one command list: the host keeps one for each list, reset with the list
	struct list_data
	{
		grass::list_state state;
		ID3D12Resource *rt0 = nullptr;
		ID3D12Resource *ds = nullptr;
		// The world's states on this list, from its barriers (what they are while it's being drawn into, until one says otherwise)
		D3D12_RESOURCE_STATES world_rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		D3D12_RESOURCE_STATES world_ds_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		bool grass_done = false; // grass went in on this list at the world's end
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
	// At process exit: the device may be gone, its objects are left to the process's end
	void shutdown();

	// The events, for lists of the attached device, each in the place ReShade raises its own: "after" once the call
	// is made, "before" ahead of it. The core may record on the list in on_close, on_targets, on_barrier and
	// on_clear_depth: the host mustn't see those calls as dgVoodoo's.
	void on_reset(list_data &d);                                                       // after Reset
	void on_close(ID3D12GraphicsCommandList *list, list_data &d);                      // before Close
	void on_targets(ID3D12GraphicsCommandList *list, list_data &d, uint32_t count, const uint64_t *rtvs, uint64_t dsv); // after
	void on_pipeline(list_data &d, ID3D12PipelineState *pso);
	void on_topology(list_data &d, D3D12_PRIMITIVE_TOPOLOGY topology);
	void on_viewports(list_data &d, uint32_t first, uint32_t count, const D3D12_VIEWPORT *viewports);
	void on_scissors(list_data &d, uint32_t first, uint32_t count, const D3D12_RECT *rects);
	// Root arguments, each with the root signature they're for (no tables: the signature was set, or the list reset)
	void on_root_tables(list_data &d, ID3D12RootSignature *root, uint32_t first, uint32_t count, const uint64_t *tables);
	void on_root_constants(list_data &d, ID3D12RootSignature *root, uint32_t param, uint32_t first, uint32_t count, const void *values);
	void on_root_descriptor(list_data &d, ID3D12RootSignature *root, uint32_t param, grass::root_arg::kind_t kind, uint64_t address);
	// Transitions, each resource and the state it's moved to (after)
	void on_barrier(ID3D12GraphicsCommandList *list, list_data &d, uint32_t count, ID3D12Resource *const *resources, const D3D12_RESOURCE_STATES *after);
	void on_clear_depth(ID3D12GraphicsCommandList *list, list_data &d, uint64_t dsv, bool depth, uint32_t rect_count, const D3D12_RECT *rects); // before
	void on_clear_target(list_data &d, uint64_t rtv, const float color[4], uint32_t rect_count, const D3D12_RECT *rects);                      // before
	void on_draw(list_data &d, uint64_t vertices); // before (vertices or indices times instances)
	// The game side's zone marker (src/game/slotmark.hpp), cleared on this depth view: the world is this depth and the
	// target bound with it. While markers come they decide the world, not the vertex counts (the game's zone may be off).
	void on_world_marker(list_data &d, uint64_t dsv);
	// The game side's zone end marker (the world's depth through dsv; its planes' states as dgVoodoo left them, depth
	// and stencil): the blades drawn there, into the world with its depth written, before the characters and effects
	// (the world's end then draws the rest without them)
	void on_zone_end(ID3D12GraphicsCommandList *list, list_data &d, uint64_t dsv, const D3D12_RESOURCE_STATES planes[2]);
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
