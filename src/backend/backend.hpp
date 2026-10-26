#pragma once

// HiFi XI's backend: World FX's core (src/render/worldfx_core) fed from HiFi XI's own Direct3D 12 hooks
// (src/host), in place of the World FX ReShade add-on. Its settings are HiFi XI's own (config\hifixi\settings.ini,
// [worldfx]: made once from the add-on's in ReShade.ini, which isn't read or written after), its files the add-on's
// (worldfx-grass\ and the rest in the game's bootloader folder, which is the process's own), its log hifixi.log there.
//
// Two ways in:
//   early: HiFi XI's starter (bootloader\dxgi.dll, src/starter) calls hifixi_early_start the first time dgVoodoo asks
//          DXGI for a factory, before it makes its device: every list and view is followed from its making, and the
//          GPU crash record (and the debug layer, with D3D12DebugLayer=1) can be turned on;
//   late:  the plugin alone, seconds after the device exists: views made before aren't known, so the world isn't
//          found (the status says so).

// (no d3d12.h here: the plugin includes Ashita's Direct3D 8 headers beside this)
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::backend
{
	bool early_start(void *create_factory1);
	// From the plugin, when the starter didn't start it
	bool late_start(std::string &why);
	bool started();
	bool early();
	// The hooks out (the plugin unloaded) and back in
	void stop();
	bool resume(std::string &why);

	// Each present, before it (Ashita's Direct3DPresent; the native end-to-end test's stand-in): the core attached once
	// the game's device is known, then its present
	void present();

	// Where World FX's zone files are (the process's folder, with a slash at the end)
	const std::wstring &folder();
	// HiFi XI's settings file, and one of World FX's settings in it as text (false: not set)
	const std::wstring &settings_file();
	bool setting(const char *key, std::string &value);
	// The preset the settings were last set from ("off", "balanced", "high", "ultra"; "custom" once one was changed by
	// hand, and for settings taken from elsewhere), kept in the settings file
	std::string preset();
	void set_preset(const char *name);
	// HiFi XI's own options (not World FX's), each in its section of the settings file ([hifps], [drawcull]); false:
	// not set. set_option with value nullptr takes it out (its default again).
	bool option(const char *section, const char *key, std::string &value);
	void set_option(const char *section, const char *key, const char *value);
	// For the menu's test: the settings alone (the core's port, this file), no hooks; folder: where hifixi.log goes
	bool start_settings_only(const wchar_t *settings_file, const wchar_t *folder);
	// A line into hifixi.log (and the plugin's sink, when set: level 0 info, 1 warning, 2 error)
	void log(int level, const char *text);
	void set_log_sink(void (*sink)(int level, const char *text));

	// What the backend knows, for /hifi status
	std::vector<std::string> status();
	// The zone's end markers seen in Direct3D 12 since the start (src/game/slotmark.hpp)
	uint64_t zone_ends_seen();
}
