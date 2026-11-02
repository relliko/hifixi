#include <d3d12.h>
#include <dxgi1_4.h>
#include "backend.hpp"
#include "streamlog.hpp"
#include "../game/slotmark.hpp"
#include "../chars/marks.hpp" // (native characters)
#include "../chars/charinject.hpp" // (native characters)
#include "../zone/inject.hpp"
#include "../zone/frame.hpp"
#include "../zone/native.hpp"
#include "../zone/zone_gpu.hpp"
#include "../upscale/upscale.hpp"
#include "../upscale/masks.hpp"
#include "../framegen/framegen.hpp"
#include "../host/events.hpp"
#include "../host/passes.hpp"
#include "../host/frameapi.hpp"
#include "../host/shadercache.hpp"
#include "../host/retire.hpp"
#include "../render/worldfx_core.hpp"
#include "../render/gpu_crash.hpp"
#include <share.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <unordered_map>

// The core gets the hooks' calls as the World FX add-on got ReShade's events (worldfx_core.hpp says where each goes:
// before or after the call), for the lists of the game's device once it's attached (at the first present after its
// queue was seen). Everything the core records itself on the d3d8.dll's lists, and every view it makes, goes through the
// same hooks: the core is called with the thread quiet (host::quiet), so they let those through untouched.
//
// Which resource a view is of: the d3d8.dll's render target and depth views as they're made (CreateRenderTargetView,
// CreateDepthStencilView) and copied (CopyDescriptors): a map from the descriptor to the resource and the view's
// format. ReShade knew it from wrapping the device from the start; so does this, started early.
//
// Settings: HiFi XI's own file, config\hifixi\settings.ini beside the plugins folder ([worldfx]: World FX's, by the
// names the add-on had). The first time there's none, it's made from the add-on's settings (bootloader\ReShade.ini
// [FFXI_WORLDFX], read once and left as it was); ReShade.ini isn't read or written after that.

namespace hifi::backend
{
	namespace
	{
		std::wstring g_folder;
		std::wstring g_settings; // HiFi XI's settings file
		constexpr const wchar_t *kSection = L"worldfx";
		std::atomic<bool> g_started { false };
		bool g_early = false;
		std::atomic<ID3D12Device *> g_device { nullptr };     // the game's (its queue's)
		std::atomic<ID3D12CommandQueue *> g_queue { nullptr }; // its direct queue (the last lists were submitted on)
		// (our d3d8.dll's HifiFrameDevice: where the device and the game's queue come from; nullptr: an older one, they're
		// found through the hooked ExecuteCommandLists) and its channel's version (1: the marks without the world's fields)
		hifi_frame_device_fn g_frame_device = nullptr;
		uint32_t g_frame_version = 1;
		std::atomic<bool> g_attached { false };
		UINT g_rtv_increment = 0;
		std::atomic<void (*)(int, const char *)> g_log_sink { nullptr };
		std::atomic<uint64_t> g_presents { 0 };

		// The log: hifixi.log in the folder, started afresh each run, written as it comes (a crash keeps it); the last run's
		// kept as hifixi.previous.log (after a crash or a hang the game is started again before anyone sends the log)
		std::mutex log_mutex;
		FILE *log_file = nullptr;
		bool log_tried = false;

		void write_log(int level, const char *text)
		{
			{
				std::lock_guard<std::mutex> lock(log_mutex);
				if (!log_tried && !g_folder.empty())
				{
					log_tried = true;
					MoveFileExW((g_folder + L"hifixi.log").c_str(), (g_folder + L"hifixi.previous.log").c_str(), MOVEFILE_REPLACE_EXISTING);
					log_file = _wfsopen((g_folder + L"hifixi.log").c_str(), L"w", _SH_DENYWR);
				}
				if (log_file != nullptr)
				{
					SYSTEMTIME t;
					GetLocalTime(&t);
					std::fprintf(log_file, "%02u:%02u:%02u:%03u [%5lu] | %-5s | %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(),
						level == 2 ? "ERROR" : level == 1 ? "WARN" : "INFO", text);
					std::fflush(log_file);
				}
			}
			if (auto *const sink = g_log_sink.load())
				sink(level, text);
		}

		// Views: the resource and format behind each render target and depth descriptor made or copied since the start
		struct view_entry
		{
			ID3D12Resource *resource;
			DXGI_FORMAT format;
		};
		std::mutex views_mutex;
		std::unordered_map<uint64_t, view_entry> views;
		std::atomic<uint64_t> views_made { 0 }, views_copied { 0 }, views_unknown { 0 };

		// What's bound on one of our d3d8.dll's lists, followed by the hooks, for FSR alone: it draws in the middle of
		// d3d8.dll's stream (at the resolve's draw) and binds all of it again after (list_state::restore), till a mark
		// places it (step 5 of docs' re-architecture). World FX takes nothing from here: it draws at the marks.
		struct list_capture
		{
			grass::list_state state;
			ID3D12Resource *rt0 = nullptr; // (the first target bound)
		};
		namespace capture
		{
			void targets(list_capture &d, uint32_t count, const uint64_t *rtvs, uint64_t dsv);
			void viewports(list_capture &d, uint32_t count, const D3D12_VIEWPORT *viewports)
			{
				const uint32_t total = std::min<uint32_t>(count, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
				for (uint32_t i = 0; i < total; ++i)
					d.state.viewports[i] = viewports[i];
				d.state.viewport_count = total;
			}
			void scissors(list_capture &d, uint32_t count, const D3D12_RECT *rects)
			{
				const uint32_t total = std::min<uint32_t>(count, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
				for (uint32_t i = 0; i < total; ++i)
					d.state.scissors[i] = rects[i];
				d.state.scissor_count = total;
			}
			void table(list_capture &d, uint32_t param, uint64_t table)
			{
				grass::root_arg &a = d.state.arg(param);
				a = grass::root_arg();
				a.kind = grass::root_arg::table;
				a.value = table;
			}
			void descriptor(list_capture &d, uint32_t param, grass::root_arg::kind_t kind, uint64_t address)
			{
				grass::root_arg &a = d.state.arg(param);
				a = grass::root_arg();
				a.kind = kind;
				a.value = address;
			}
		}

		// Each list's capture (made with the list, at its first reset since the start, or at its first mark; never freed:
		// a list at the same address later is made or reset before it records)
		std::mutex lists_mutex;
		std::unordered_map<void *, std::unique_ptr<list_capture>> lists;
		thread_local void *tl_list = nullptr;
		thread_local list_capture *tl_data = nullptr;

		list_capture *data_of(void *list, bool make)
		{
			if (list == tl_list && tl_data != nullptr)
				return tl_data;
			std::lock_guard<std::mutex> lock(lists_mutex);
			auto it = lists.find(list);
			if (it == lists.end())
			{
				if (!make)
					return nullptr;
				it = lists.emplace(list, std::make_unique<list_capture>()).first;
			}
			tl_list = list;
			tl_data = it->second.get();
			return tl_data;
		}
		// The thread our d3d8.dll records the game's frames on (the game's), from its first frame mark. Lists recorded on
		// any other thread (HiFi XI's own loaders' uploads, AMD's frame generation threads, DXGI's list in their Present)
		// are none of the core's business: their events go no further, so no lock of the passes is taken there. One was:
		// the characters' loader's upload reset a list inside the Direct3D debug layer's lock while the game's thread,
		// holding World FX's lock in its pass, waited on that one (a deadlock, 2026-11-02)
		std::atomic<DWORD> g_render_thread { 0 };
		bool elsewhere()
		{
			const DWORD t = g_render_thread.load(std::memory_order_relaxed);
			return t != 0 && t != GetCurrentThreadId();
		}

		// A list's data when the core should hear of it (attached, the list known, on the game's thread)
		list_capture *live(void *list)
		{
			return g_attached.load(std::memory_order_acquire) && !elsewhere() ? data_of(list, false) : nullptr;
		}

		// The core's port
		void port_log(int level, const char *text) { write_log(level, text); }

		bool ini_get(const char *key, std::wstring &out)
		{
			wchar_t wkey[64];
			size_t i = 0;
			for (; key[i] != 0 && i + 1 < 64; ++i)
				wkey[i] = wchar_t(key[i]);
			wkey[i] = 0;
			wchar_t buffer[256];
			const DWORD n = GetPrivateProfileStringW(kSection, wkey, L"\x1", buffer, 256, g_settings.c_str());
			if (n == 1 && buffer[0] == 1)
				return false;
			out.assign(buffer, n);
			return true;
		}
		void ini_set(const char *key, const wchar_t *value)
		{
			wchar_t wkey[64];
			size_t i = 0;
			for (; key[i] != 0 && i + 1 < 64; ++i)
				wkey[i] = wchar_t(key[i]);
			wkey[i] = 0;
			WritePrivateProfileStringW(kSection, wkey, value, g_settings.c_str());
		}
		// A safe start: the last session's GPU stopped (Direct3D removed the device: hung, reset; the core then sets
		// [worldfx] GpuStopped=1 with its report), so this one reads World FX's costliest parts as off. Only as read: the
		// file keeps their values (the next start has them back) and one turned on in the menu is saved as usual.
		bool g_safe_start = false;
		std::atomic<bool> g_own_d3d8 { false };
		const char *const kSafeOff[] = { "VolLight", "VolFog", "AOStrength", "WaterReflections", "Rain", "Snow", "CloudQuality", "LampShadows" };
		bool safe_off(const char *key, std::wstring &out)
		{
			if (!g_safe_start)
				return false;
			for (const char *k : kSafeOff)
				if (std::strcmp(k, key) == 0)
				{
					out = L"0";
					return true;
				}
			return false;
		}
		bool port_get_bool(const char *key, bool &value)
		{
			std::wstring s;
			if (!safe_off(key, s) && !ini_get(key, s))
				return false;
			value = s == L"1" || _wcsicmp(s.c_str(), L"true") == 0;
			return true;
		}
		bool port_get_float(const char *key, float &value)
		{
			std::wstring s;
			if (!safe_off(key, s) && !ini_get(key, s))
				return false;
			value = std::wcstof(s.c_str(), nullptr);
			return true;
		}
		bool port_get_int(const char *key, int &value)
		{
			std::wstring s;
			if (!safe_off(key, s) && !ini_get(key, s))
				return false;
			value = int(std::wcstol(s.c_str(), nullptr, 10));
			return true;
		}
		// (written as the add-on's were: 0 or 1, the shortest float that reads back the same)
		void port_set_bool(const char *key, bool value) { ini_set(key, value ? L"1" : L"0"); }
		void port_set_float(const char *key, float value)
		{
			char s[32];
			const auto r = std::to_chars(s, s + sizeof(s) - 1, value);
			*r.ptr = 0;
			wchar_t w[32];
			for (size_t i = 0; i <= size_t(r.ptr - s); ++i)
				w[i] = wchar_t(s[i]);
			ini_set(key, w);
		}
		void port_set_int(const char *key, int value) { ini_set(key, std::to_wstring(value).c_str()); }
		ID3D12Resource *port_resource_of_view(uint64_t view)
		{
			std::lock_guard<std::mutex> lock(views_mutex);
			const auto it = views.find(view);
			if (it == views.end())
			{
				views_unknown.fetch_add(1, std::memory_order_relaxed);
				return nullptr;
			}
			return it->second.resource;
		}
		DXGI_FORMAT port_format_of_view(uint64_t view)
		{
			std::lock_guard<std::mutex> lock(views_mutex);
			const auto it = views.find(view);
			return it != views.end() ? it->second.format : DXGI_FORMAT_UNKNOWN;
		}
		// (the stream log's: not counted as looked up)
		void *streamlog_view(uint64_t view)
		{
			std::lock_guard<std::mutex> lock(views_mutex);
			const auto it = views.find(view);
			return it != views.end() ? it->second.resource : nullptr;
		}

		// HiFi XI's settings file: HIFIXI_SETTINGS when set (the tests), else config\hifixi\settings.ini beside the
		// plugins folder this DLL is in (the game's)
		std::wstring settings_path()
		{
			wchar_t env[MAX_PATH * 2] = L"";
			const DWORD n = GetEnvironmentVariableW(L"HIFIXI_SETTINGS", env, MAX_PATH * 2);
			if (n > 0 && n < MAX_PATH * 2)
				return env;
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&settings_path), &self);
			wchar_t path[MAX_PATH] = L"";
			GetModuleFileNameW(self, path, MAX_PATH);
			std::wstring dir = path;
			dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
			wchar_t full[MAX_PATH * 2] = L"";
			if (GetFullPathNameW((dir + L"..\\config\\hifixi\\settings.ini").c_str(), MAX_PATH * 2, full, nullptr) == 0)
				return dir + L"hifixi-settings.ini";
			return full;
		}

		// The first time there's no settings file: one made from the add-on's settings ([FFXI_WORLDFX] in ReShade.ini in
		// the game's bootloader folder), which are left as they were
		void import_settings()
		{
			if (GetFileAttributesW(g_settings.c_str()) != INVALID_FILE_ATTRIBUTES)
				return;
			// (its folder, and the folders above it as far as they're missing)
			for (size_t at = g_settings.find_first_of(L"\\/", 3); at != std::wstring::npos; at = g_settings.find_first_of(L"\\/", at + 1))
				CreateDirectoryW(g_settings.substr(0, at).c_str(), nullptr);
			const std::wstring reshade_ini = g_folder + L"ReShade.ini";
			static wchar_t section[32768];
			const DWORD n = GetFileAttributesW(reshade_ini.c_str()) != INVALID_FILE_ATTRIBUTES ? GetPrivateProfileSectionW(L"FFXI_WORLDFX", section, 32768, reshade_ini.c_str()) : 0;
			FILE *f = _wfsopen(g_settings.c_str(), L"w", _SH_DENYWR);
			if (f == nullptr)
			{
				write_log(2, "HiFi XI: its settings file couldn't be made: World FX's settings are the defaults, and changes aren't kept");
				return;
			}
			SYSTEMTIME t;
			GetLocalTime(&t);
			std::fprintf(f, "; HiFi XI's settings.\n");
			if (n != 0)
				std::fprintf(f, "; [worldfx] was made on %04u-%02u-%02u %02u:%02u from World FX's settings in the ReShade add-on (bootloader\\ReShade.ini,\n"
					"; [FFXI_WORLDFX]), which are left as they were: HiFi XI doesn't read or write ReShade.ini.\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
			std::vector<std::string> lines;
			for (const wchar_t *p = section; n != 0 && *p != 0; p += wcslen(p) + 1)
			{
				std::string line;
				for (const wchar_t *c = p; *c != 0; ++c)
					line += *c < 128 ? char(*c) : '?';
				if (line.find('=') != std::string::npos)
					lines.push_back(line);
			}
			// (none of the presets: custom)
			std::fprintf(f, "\n[hifixi]\npreset=custom\n");
			std::fprintf(f, "\n[worldfx]\n");
			for (const std::string &line : lines)
				std::fprintf(f, "%s\n", line.c_str());
			std::fclose(f);
			const uint32_t values = uint32_t(lines.size());
			char text[160];
			std::snprintf(text, sizeof(text), "HiFi XI: its settings file made, %u of World FX's settings taken from ReShade.ini (left as it was)", values);
			write_log(0, text);
		}

		// A file from before the presets were these (HiFi XI 0.10 and before kept a copy of the old setup's settings,
		// [mine], and a preset of that name): the copy taken out, the preset custom
		void tidy_settings()
		{
			wchar_t probe[8] = L"";
			if (GetFileAttributesW(g_settings.c_str()) == INVALID_FILE_ATTRIBUTES)
				return;
			if (GetPrivateProfileSectionW(L"mine", probe, 8, g_settings.c_str()) != 0)
				WritePrivateProfileStringW(L"mine", nullptr, nullptr, g_settings.c_str());
			wchar_t preset[32] = L"";
			GetPrivateProfileStringW(L"hifixi", L"preset", L"", preset, 32, g_settings.c_str());
			if (preset[0] == 0 || _wcsicmp(preset, L"mine") == 0)
				WritePrivateProfileStringW(L"hifixi", L"preset", L"custom", g_settings.c_str());
		}

		// The addons HiFi XI took over, their settings taken once into their sections, when there are any and HiFi XI
		// hasn't taken them before ([hifixi] addons=1): hifps's
		// (config\addons\hifps\settings.lua: a returned table, its exp = {...} inside as exp_<name>; its gfx = {...} left out),
		// drawcull's (config\addons\drawcull\<character>\settings.lua: settings["key"] = value lines; the character's
		// whose file was saved last). Literal tables only: no Lua is run.
		void import_addon_options()
		{
			wchar_t done[8] = L"";
			if (GetFileAttributesW(g_settings.c_str()) == INVALID_FILE_ATTRIBUTES ||
				GetPrivateProfileStringW(L"hifixi", L"addons", L"", done, 8, g_settings.c_str()) != 0)
				return;
			// (Ashita's folder: the bootloader's parent)
			std::wstring root = g_folder;
			while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
				root.pop_back();
			root = root.substr(0, root.find_last_of(L"\\/") + 1);
			auto put = [](const wchar_t *section, const std::string &key, const std::string &value) {
				const std::wstring k(key.begin(), key.end()), v(value.begin(), value.end());
				WritePrivateProfileStringW(section, k.c_str(), v.c_str(), g_settings.c_str());
			};
			auto trim = [](std::string s) {
				const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n,;");
				return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
			};
			auto value_of = [](const std::string &v) { return v == "true" ? std::string("1") : v == "false" ? std::string("0") : v; };
			uint32_t taken = 0;
			// hifps
			if (FILE *f = nullptr; _wfopen_s(&f, (root + L"config\\addons\\hifps\\settings.lua").c_str(), L"r") == 0 && f != nullptr)
			{
				std::string prefix;
				char raw[512];
				while (std::fgets(raw, sizeof(raw), f) != nullptr)
				{
					std::string line = raw;
					line = line.substr(0, line.find("--"));
					line = trim(line);
					if (line.empty() || line.rfind("return", 0) == 0)
						continue;
					if (line[0] == '}')
					{
						prefix.clear();
						continue;
					}
					const size_t eq = line.find('=');
					if (eq == std::string::npos)
						continue;
					const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
					if (value == "{")
					{
						prefix = key;
						continue;
					}
					// (its graphics settings, gfx, the game's own lights, and its old glows toggle aren't HiFi XI's)
					if (key.empty() || value.empty() || key == "glows" || prefix == "gfx")
						continue;
					put(L"hifps", prefix.empty() ? key : prefix + "_" + key, value_of(value));
					taken++;
				}
				std::fclose(f);
			}
			// drawcull: the character's whose settings were saved last
			{
				WIN32_FIND_DATAW d;
				const std::wstring base = root + L"config\\addons\\drawcull\\";
				std::wstring newest;
				FILETIME newest_at = {};
				HANDLE h = FindFirstFileW((base + L"*").c_str(), &d);
				if (h != INVALID_HANDLE_VALUE)
				{
					do
					{
						if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || d.cFileName[0] == L'.' || _wcsicmp(d.cFileName, L"defaults") == 0)
							continue;
						WIN32_FILE_ATTRIBUTE_DATA a;
						const std::wstring file = base + d.cFileName + L"\\settings.lua";
						if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &newest_at) > 0)
						{
							newest_at = a.ftLastWriteTime;
							newest = file;
						}
					} while (FindNextFileW(h, &d));
					FindClose(h);
				}
				if (FILE *f = nullptr; !newest.empty() && _wfopen_s(&f, newest.c_str(), L"r") == 0 && f != nullptr)
				{
					char raw[512];
					while (std::fgets(raw, sizeof(raw), f) != nullptr)
					{
						// settings["key"] = value;
						const std::string line = raw;
						const size_t open = line.find("[\""), close = line.find("\"]"), eq = line.find('=');
						if (open == std::string::npos || close == std::string::npos || eq == std::string::npos || close < open)
							continue;
						put(L"drawcull", line.substr(open + 2, close - open - 2), value_of(trim(line.substr(eq + 1))));
						taken++;
					}
					std::fclose(f);
				}
			}
			WritePrivateProfileStringW(L"hifixi", L"addons", L"1", g_settings.c_str());
			char text[160];
			std::snprintf(text, sizeof(text), "HiFi XI: %u settings taken from the hifps and drawcull addons (left as they were)", taken);
			write_log(0, text);
		}

		// The frame rate's six speed-ups (src/game/hifps/exp.hpp) were on in every preset until 0.64, off by default in
		// 0.65, and out of the menu since 0.66 (the game sends ~115 draws a frame through the d3d8.dll now: nothing for
		// them to save, and occlusion brought the GPU readbacks back). The ones a settings file has on are turned off once
		// more ([hifixi] speedups_off 2: whoever turned one on in 0.65 has no menu to turn it off).
		void speedups_off_once()
		{
			wchar_t done[8] = L"";
			if (GetFileAttributesW(g_settings.c_str()) == INVALID_FILE_ATTRIBUTES ||
				(GetPrivateProfileStringW(L"hifixi", L"speedups_off", L"", done, 8, g_settings.c_str()) != 0 && std::wcscmp(done, L"2") == 0))
				return;
			unsigned turned = 0;
			for (const wchar_t *key : { L"exp_ashita", L"exp_occlusion", L"exp_walls", L"exp_sse", L"exp_batch", L"exp_dedupe" })
			{
				wchar_t v[8] = L"";
				GetPrivateProfileStringW(L"hifps", key, L"", v, 8, g_settings.c_str());
				if (v[0] != L'\0' && std::wcscmp(v, L"0") != 0)
				{
					WritePrivateProfileStringW(L"hifps", key, L"0", g_settings.c_str());
					turned++;
				}
			}
			WritePrivateProfileStringW(L"hifixi", L"speedups_off", L"2", g_settings.c_str());
			if (turned != 0)
			{
				char text[200];
				std::snprintf(text, sizeof(text), "HiFi XI: the frame rate's %u speed-ups that were on turned off (out of the menu since 0.66: "
					"nothing left for them to save)", turned);
				write_log(0, text);
			}
		}

		void setup_port(const wchar_t *folder = nullptr)
		{
			wchar_t path[MAX_PATH] = L"";
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			g_folder = folder != nullptr ? std::wstring(folder) : std::wstring(path);
			const size_t slash = g_folder.find_last_of(L"\\/");
			if (folder == nullptr)
				g_folder = slash == std::wstring::npos ? std::wstring() : g_folder.substr(0, slash + 1);
			else if (!g_folder.empty() && g_folder.back() != L'\\' && g_folder.back() != L'/')
				g_folder += L'\\';
			// (HiFi XI's own shaders kept compiled beside the log: src/host/shadercache.hpp)
			shadercache::set_folder(g_folder + L"hifixi-shader-cache\\");
			g_settings = settings_path();
			import_settings();
			tidy_settings();
			import_addon_options();
			speedups_off_once();
			{
				std::string where;
				for (wchar_t c : g_settings)
					where += c < 128 ? char(c) : '?';
				write_log(0, ("HiFi XI: settings from " + where).c_str());
			}
			{
				g_safe_start = false;
				std::wstring stopped;
				if (ini_get("GpuStopped", stopped) && stopped == L"1")
				{
					g_safe_start = true;
					ini_set("GpuStopped", nullptr); // (one safe start: if it stops again, it's set again)
					write_log(1, "HiFi XI: the GPU stopped last session (worldfx-gpu-crash.txt): a safe start, light in the air, "
						"ambient occlusion, water reflections, rain, snow, cloud quality and lamp shadows off for this session");
				}
			}
			worldfx::port p;
			p.log = port_log;
			p.get_bool = port_get_bool;
			p.get_float = port_get_float;
			p.get_int = port_get_int;
			p.set_bool = port_set_bool;
			p.set_float = port_set_float;
			p.set_int = port_set_int;
			p.folder = g_folder;
			worldfx::set_port(p);
			worldfx::load_settings();
			streamlog::set_views(streamlog_view);
			streamlog::set_folder(g_folder);
		}

		// (the targets bound now: the call is made)
		void capture::targets(list_capture &d, uint32_t count, const uint64_t *rtvs, uint64_t dsv)
		{
			count = std::min<uint32_t>(count, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
			d.state.rtv_count = count;
			for (uint32_t i = 0; i < count; ++i)
				d.state.rtvs[i] = { static_cast<SIZE_T>(rtvs[i]) };
			d.state.dsv = { static_cast<SIZE_T>(dsv) };
			d.rt0 = count != 0 && rtvs[0] != 0 ? port_resource_of_view(rtvs[0]) : nullptr;
		}

		// The hooks' events
		void ev_created(ID3D12GraphicsCommandList *list)
		{
			*data_of(list, true) = list_capture();
		}
		void ev_reset(ID3D12GraphicsCommandList *list)
		{
			if (elsewhere())
				return;
			upscale::on_reset(list);
			*data_of(list, true) = list_capture();
		}
		void ev_close(ID3D12GraphicsCommandList *list)
		{
			if (list_capture *const d = live(list))
			{
				host::quiet q;
				upscale::before_other(list, d->state); // (the screen upscaled before the list closes, when it's waiting)
			}
		}
		void ev_targets(ID3D12GraphicsCommandList *list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs, BOOL single_range, const D3D12_CPU_DESCRIPTOR_HANDLE *dsv)
		{
			list_capture *const d = live(list);
			if (d == nullptr)
				return;
			const uint32_t n = rtvs != nullptr ? std::min<uint32_t>(count, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) : 0;
			uint64_t v[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
			for (uint32_t i = 0; i < n; ++i)
				v[i] = single_range ? uint64_t(rtvs[0].ptr) + uint64_t(i) * g_rtv_increment : uint64_t(rtvs[i].ptr);
			capture::targets(*d, n, v, dsv != nullptr ? uint64_t(dsv->ptr) : 0);
		}
		void ev_pipeline(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso)
		{
			if (list_capture *const d = live(list))
				d->state.pipeline = pso;
		}
		void ev_topology(ID3D12GraphicsCommandList *list, D3D12_PRIMITIVE_TOPOLOGY topology)
		{
			if (list_capture *const d = live(list))
				d->state.topology = topology;
		}
		void ev_viewports(ID3D12GraphicsCommandList *list, UINT count, const D3D12_VIEWPORT *viewports)
		{
			if (list_capture *const d = live(list))
				capture::viewports(*d, viewports != nullptr ? count : 0, viewports);
		}
		void ev_scissors(ID3D12GraphicsCommandList *list, UINT count, const D3D12_RECT *rects)
		{
			if (list_capture *const d = live(list))
				capture::scissors(*d, rects != nullptr ? count : 0, rects);
		}
		void ev_root_signature(ID3D12GraphicsCommandList *list, ID3D12RootSignature *root)
		{
			if (list_capture *const d = live(list))
				d->state.set_root(root);
		}
		void ev_root_table(ID3D12GraphicsCommandList *list, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE table)
		{
			if (list_capture *const d = live(list))
				capture::table(*d, param, table.ptr);
		}
		void ev_root_constants(ID3D12GraphicsCommandList *list, UINT param, UINT count, const void *values, UINT first)
		{
			if (list_capture *const d = live(list))
				d->state.set_constants(param, first, count, static_cast<const uint32_t *>(values));
		}
		void ev_root_cbv(ID3D12GraphicsCommandList *list, UINT param, D3D12_GPU_VIRTUAL_ADDRESS address)
		{
			if (list_capture *const d = live(list))
				capture::descriptor(*d, param, grass::root_arg::cbv, address);
		}
		void ev_root_srv(ID3D12GraphicsCommandList *list, UINT param, D3D12_GPU_VIRTUAL_ADDRESS address)
		{
			if (list_capture *const d = live(list))
				capture::descriptor(*d, param, grass::root_arg::srv, address);
		}
		void ev_root_uav(ID3D12GraphicsCommandList *list, UINT param, D3D12_GPU_VIRTUAL_ADDRESS address)
		{
			if (list_capture *const d = live(list))
				capture::descriptor(*d, param, grass::root_arg::uav, address);
		}
		void ev_barrier(ID3D12GraphicsCommandList *list, UINT count, const D3D12_RESOURCE_BARRIER *barriers)
		{
			list_capture *const d = live(list);
			if (d == nullptr)
				return;
			// (FSR follows the world and its target from the resolve on; World FX takes its states from the marks)
			host::quiet q;
			upscale::after_barrier(list, count, barriers, d->state);
		}
		// The game side's slot markers (src/game/slotmark.hpp) as our d3d8.dll hands them on: how many of each kind, how
		// many came out of order (each zone begin, then its end with the same seq, then the next seq's begin), the last
		std::atomic<uint64_t> marks_seen[slotmark::kinds] = {};
		std::atomic<uint64_t> marks_out_of_order { 0 };
		std::atomic<uint32_t> last_mark { 0xFFFFFFFFu }; // (kind << 8 | seq)
		// The world's target as the zone's begin found it, and its view's format
		ID3D12Resource *zone_rt = nullptr;
		DXGI_FORMAT zone_rt_format = DXGI_FORMAT_UNKNOWN;

		// The world as World FX takes it at a mark: our d3d8.dll's word (channel 3), the views and formats when it's bound
		worldfx::world_mark world_of(const hifi_frame_mark &fm)
		{
			worldfx::world_mark w;
			w.list = fm.list;
			w.rt = fm.world_rt;
			w.ds = fm.world_ds;
			w.rt_state = fm.world_rt_state;
			w.ds_planes[0] = fm.world_ds_planes[0];
			w.ds_planes[1] = fm.world_ds_plane_count > 1 ? fm.world_ds_planes[1] : fm.world_ds_planes[0];
			if (fm.rt != nullptr && fm.rt == fm.world_rt && fm.ds != nullptr && fm.ds == fm.world_ds)
			{
				w.rtv = uint64_t(fm.rtv.ptr);
				w.dsv = uint64_t(fm.dsv.ptr);
				w.rt_format = fm.rt_format;
				w.ds_format = fm.ds_format;
				w.width = fm.width;
				w.height = fm.height;
			}
			w.fog_known = fm.world_clear_known != 0;
			for (int i = 0; i < 3; ++i)
				w.fog[i] = fm.world_clear[i];
			return w;
		}

		// fm: our d3d8.dll's word at the mark (src/host/frameapi.hpp): what's bound and the states, exactly
		void on_mark(ID3D12GraphicsCommandList *list, list_capture &d, const slotmark::mark &m, const hifi_frame_mark &fm)
		{
			ID3D12Resource *const depth = fm.ds;
			// (the depth's planes, after the trackers have taken the depth in)
			const auto exact_planes = [&]() {
				if (depth == nullptr)
					return;
				const D3D12_RESOURCE_STATES planes[2] = { fm.ds_planes[0], fm.ds_plane_count > 1 ? fm.ds_planes[1] : fm.ds_planes[0] };
				zone::inject::set_depth_planes(depth, planes);
				chars::inject::set_depth_planes(depth, planes);
			};
			// (the world's target's state: our d3d8.dll's, bound or not (version 2); an older one's only when it's bound, else
			// as the barriers left it)
			// (the world's target's state: our d3d8.dll's, bound or not)
			const auto world_rt_state = [&]() {
				return fm.world_rt != nullptr && fm.world_rt == zone_rt ? fm.world_rt_state : D3D12_RESOURCE_STATE_RENDER_TARGET;
			};
			marks_seen[m.kind].fetch_add(1, std::memory_order_relaxed);
			// (the own-frame passes: their module draws at its marker, src/host/passes.hpp)
			if (m.kind >= slotmark::first_pass)
			{
				// (the back buffer's own markers leave the world's depth tracking alone)
				if (!slotmark::on_back_buffer(m.kind))
				{
					zone::inject::on_marker(depth);
					chars::inject::on_marker(depth);
					exact_planes();
				}
				// (and the upscaled world, when it waits for the next call after the resolve, goes in before what they
				// draw: a marker isn't a clear to it)
				else
					upscale::before_other(list, d.state);
				if (depth == nullptr)
					return;
				passes::world w;
				w.rt = zone_rt;
				w.ds = depth;
				w.rt_format = zone_rt_format;
				w.ds_format = fm.ds_format;
				w.rt_state = world_rt_state();
				w.planes_known = zone::inject::depth_planes(depth, w.ds_planes);
				w.bound_rt = fm.rt;
				w.bound_ds = fm.ds;
				w.bound_rt_format = fm.rt_format;
				passes::dispatch(m.kind, list, w, m.seq);
				return;
			}
			// (native characters: their markers are checked on their own, and leave the zone's order check alone)
			if (m.kind >= slotmark::chars_begin)
				chars::marks::seen(m.kind, m.seq);
			const uint32_t was = m.kind >= slotmark::chars_begin ? 0xFFFFFFFFu : last_mark.exchange(m.kind << 8 | m.seq);
			if (was != 0xFFFFFFFFu && (m.kind == slotmark::zone_begin || m.kind == slotmark::zone_end))
			{
				const uint32_t wk = was >> 8, ws = was & 0xFF;
				const bool in_order = m.kind == slotmark::zone_end ? (wk == slotmark::zone_begin && ws == m.seq) : (wk == slotmark::zone_end && ((ws + 1) & 15) == m.seq);
				if (!in_order)
					marks_out_of_order.fetch_add(1, std::memory_order_relaxed);
			}
			zone::inject::on_marker(depth);
			chars::inject::on_marker(depth); // (native characters)
			exact_planes();
			// The world's target: the one bound with the depth at the zone's begin
			if (m.kind == slotmark::zone_begin && depth != nullptr && fm.rt != nullptr)
			{
				zone_rt = fm.rt;
				zone_rt_format = fm.rt_format;
				worldfx::on_world_marker(world_of(fm));
			}
			if (m.kind == slotmark::zone_end && depth != nullptr)
			{
				zone::inject::world w;
				w.rt = zone_rt;
				w.ds = depth;
				w.rt_format = zone_rt_format;
				w.ds_format = fm.ds_format;
				w.rt_state = world_rt_state();
				zone::inject::on_zone_end(list, w, m.seq);
				// (then World FX's blades, written into the world's depth before the characters and effects)
				worldfx::on_zone_end(world_of(fm));
			}
			// (native characters: the opaque models at their end marker, a see-through one at its own)
			if ((m.kind == slotmark::chars_end || m.kind == slotmark::see_through) && depth != nullptr)
			{
				chars::inject::world w;
				w.rt = zone_rt;
				w.ds = depth;
				w.rt_format = zone_rt_format;
				w.ds_format = fm.ds_format;
				w.rt_state = world_rt_state();
				if (m.kind == slotmark::chars_end)
				{
					// (the model actors the walk left to HiFi XI: ships, furniture, plants, drawn by the zone renderer)
					zone::inject::world zw;
					zw.rt = w.rt;
					zw.ds = w.ds;
					zw.rt_format = w.rt_format;
					zw.ds_format = w.ds_format;
					zw.rt_state = w.rt_state;
					zone::inject::on_actors_end(list, zw, m.seq);
					chars::inject::on_chars_end(list, w, m.seq);
					// (the world with its opaque characters: FSR's reactive mask from here)
					upscale::masks::snapshot(list, w.rt, w.rt_state, upscale::masks::opaque);
				}
				else
					chars::inject::on_see_through(list, w, m.seq);
			}
		}

		void ev_clear_depth(ID3D12GraphicsCommandList *list, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS, UINT, const D3D12_RECT *)
		{
			list_capture *const d = live(list);
			if (d == nullptr || port_resource_of_view(dsv.ptr) == nullptr)
				return;
			// (the upscaled world, when it waits for the next call after the resolve, goes in before the clear)
			host::quiet q;
			upscale::before_other(list, d->state);
		}
		void ev_clear_target(ID3D12GraphicsCommandList *list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT count, const D3D12_RECT *rects)
		{
			if (list_capture *const d = live(list))
			{
				host::quiet q;
				upscale::before_other(list, d->state);
			}
			(void)rtv;
			(void)color;
			(void)count;
			(void)rects;
		}
		void ev_heaps(ID3D12GraphicsCommandList *list, UINT count, ID3D12DescriptorHeap *const *heaps)
		{
			if (live(list) != nullptr)
			{
				upscale::on_heaps(list, count, heaps);
			}
		}
		void ev_draw(ID3D12GraphicsCommandList *list, uint64_t vertices)
		{
			if (list_capture *const d = live(list))
			{
				host::quiet q;
				upscale::before_draw(list, vertices, d->rt0, uint64_t(d->state.rtvs[0].ptr), port_format_of_view, d->state);
			}
		}
		void ev_execute(ID3D12CommandQueue *queue, UINT, ID3D12CommandList *const *)
		{
			// (our d3d8.dll names its device and queue itself, backend::present: only an older one's are learnt here)
			if (g_frame_device != nullptr || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
				return;
			g_queue.store(queue, std::memory_order_release);
			if (g_device.load(std::memory_order_acquire) == nullptr)
			{
				ID3D12Device *device = nullptr;
				if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr)
				{
					device->Release(); // (the d3d8.dll holds it for as long as it draws)
					g_device.store(device, std::memory_order_release);
				}
			}
		}
		void ev_view(ID3D12Device *, D3D12_DESCRIPTOR_HEAP_TYPE, D3D12_CPU_DESCRIPTOR_HANDLE at, ID3D12Resource *resource, DXGI_FORMAT format, uint32_t)
		{
			std::lock_guard<std::mutex> lock(views_mutex);
			views[uint64_t(at.ptr)] = { resource, format };
			views_made.fetch_add(1, std::memory_order_relaxed);
		}
		void ev_copy(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT dst_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE *dst_starts, const UINT *dst_sizes,
			UINT src_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE *src_starts, const UINT *src_sizes)
		{
			if (dst_starts == nullptr || src_starts == nullptr)
				return;
			const uint64_t step = device->GetDescriptorHandleIncrementSize(type);
			// (the source ranges, one after another, go into the destination ranges one after another; a range without a
			// size is one descriptor)
			std::lock_guard<std::mutex> lock(views_mutex);
			UINT di = 0, si = 0, dk = 0, sk = 0;
			while (di < dst_ranges && si < src_ranges)
			{
				const UINT dn = dst_sizes != nullptr ? dst_sizes[di] : 1, sn = src_sizes != nullptr ? src_sizes[si] : 1;
				if (dk >= dn)
				{
					di++;
					dk = 0;
					continue;
				}
				if (sk >= sn)
				{
					si++;
					sk = 0;
					continue;
				}
				const uint64_t dst = uint64_t(dst_starts[di].ptr) + dk * step, src = uint64_t(src_starts[si].ptr) + sk * step;
				const auto it = views.find(src);
				if (it != views.end())
				{
					const view_entry e = it->second;
					views[dst] = e;
				}
				else
					views.erase(dst);
				views_copied.fetch_add(1, std::memory_order_relaxed);
				dk++;
				sk++;
			}
		}

		const host::events &events_table()
		{
			static const host::events e = [] {
				host::events x;
				x.created = ev_created;
				x.reset = ev_reset;
				x.close = ev_close;
				x.targets = ev_targets;
				x.pipeline = ev_pipeline;
				x.topology = ev_topology;
				x.viewports = ev_viewports;
				x.scissors = ev_scissors;
				x.root_signature = ev_root_signature;
				x.root_table = ev_root_table;
				x.root_constants = ev_root_constants;
				x.root_cbv = ev_root_cbv;
				x.root_srv = ev_root_srv;
				x.root_uav = ev_root_uav;
				x.barrier = ev_barrier;
				x.clear_depth = ev_clear_depth;
				x.clear_target = ev_clear_target;
				x.draw = ev_draw;
				x.heaps = ev_heaps;
				x.execute = ev_execute;
				x.view = ev_view;
				x.copy = ev_copy;
				return x;
			}();
			return e;
		}
	}

	namespace
	{
		// The direct channel's marks (src/host/frameapi.hpp): our d3d8.dll calls this on its list where the game side
		// marks one of its passes
		// (our d3d8.dll's HifiFrameSetWorldFormat; nullptr: an older one, the world stays 8-bit)
		hifi_frame_set_world_format_fn g_set_world_format = nullptr;
		// The world's depth planes, exactly as our d3d8.dll has them at this mark (version 2: at every mark, the world's
		// end and the back buffer's too), for the renderers that draw on it: nothing follows barriers for them
		void world_planes(const hifi_frame_mark &fm)
		{
			if (g_frame_version < 2 || fm.world_ds == nullptr)
				return;
			const D3D12_RESOURCE_STATES planes[2] = { fm.world_ds_planes[0], fm.world_ds_plane_count > 1 ? fm.world_ds_planes[1] : fm.world_ds_planes[0] };
			zone::inject::set_depth_planes(fm.world_ds, planes);
			chars::inject::set_depth_planes(fm.world_ds, planes);
		}

		void __cdecl on_frame_mark(void *, uint32_t kind, uint32_t seq, const hifi_frame_mark *fm)
		{
			g_render_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
			// (the device removed: none of HiFi XI's passes recorded again; the game's own lists go on as d3d8.dll has them)
			if (gpu_crash::lost())
				return;
			// (the world's format as the setting has it: d3d8.dll changes it at the next zone's begin mark)
			if (g_set_world_format != nullptr)
				g_set_world_format(worldfx::float_world() ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM);
			if (fm == nullptr || fm->list == nullptr || kind >= slotmark::kinds || !g_attached.load(std::memory_order_acquire))
				return;
			host::quiet q;
			world_planes(*fm); // (at every mark, one with no depth bound too)
			// (the world's end comes with the resolve's targets bound, often no depth; every other mark is on a depth)
			if (fm->dsv.ptr == 0 && kind != slotmark::world_end)
				return;
			// (the list's data made here when the hooks haven't seen it: the mark is the game's thread, its list d3d8.dll's)
			list_capture *const d = data_of(fm->list, true);
			// (the world's end: World FX draws into it there, before the resolve reads it)
			if (kind == slotmark::world_end)
			{
				worldfx::on_world_end(world_of(*fm));
				// (and FSR's resolve is the next draw out of it: armed here, its state followed from the barriers on)
				if (fm->world_rt != nullptr)
					upscale::on_world_end(fm->list, fm->world_rt, fm->world_rt_state);
				return;
			}
			slotmark::mark m;
			m.kind = kind;
			m.seq = seq & 15;
			on_mark(fm->list, *d, m, *fm);
		}

		// Our d3d8.dll told where to call (nullptr: let go, at stop); false when it isn't the one loaded, or is an older
		// HiFi XI's (channel 1: its marks don't carry the world's states, and nothing follows barriers for them any more,
		// so its passes would read stale states: they stay off, as with another d3d8.dll)
		const char *g_channel_refused = nullptr;
		bool direct_channel(bool on)
		{
			HMODULE d3d8 = GetModuleHandleW(L"d3d8.dll");
			const auto set = d3d8 != nullptr ? reinterpret_cast<hifi_frame_set_handler_fn>(GetProcAddress(d3d8, kHifiFrameSetHandler)) : nullptr;
			if (set == nullptr)
				return false;
			const auto version = reinterpret_cast<hifi_frame_version_fn>(GetProcAddress(d3d8, kHifiFrameVersion));
			g_frame_version = version != nullptr ? version() : 1;
			if (on && g_frame_version < kHifiFrameChannel)
			{
				g_channel_refused = "the d3d8.dll in bootloader is an older HiFi XI's (its marks don't carry the world's states): its passes (native zone and "
					"characters, World FX, FSR) stay off; put this release's d3d8.dll (and d3d8-shader-cache) in bootloader";
				return false;
			}
			g_set_world_format = on ? reinterpret_cast<hifi_frame_set_world_format_fn>(GetProcAddress(d3d8, kHifiFrameSetWorldFormat)) : nullptr;
			g_frame_device = on ? reinterpret_cast<hifi_frame_device_fn>(GetProcAddress(d3d8, kHifiFrameDevice)) : nullptr;
			set(on ? &on_frame_mark : nullptr, nullptr);
			return true;
		}
	}

	bool early_start(void *create_factory1)
	{
		if (g_started.load())
			return true;
		setup_port();
		write_log(0, "HiFi XI: starting before the game's Direct3D 12 device (the starter, bootloader\\dxgi.dll)");
		worldfx::before_device(true);
		// The throwaway objects the hooks are found by: on WARP, so the game's adapter isn't touched before the d3d8.dll
		IDXGIFactory4 *factory = nullptr;
		IDXGIAdapter *warp = nullptr;
		using create_factory_fn = HRESULT(WINAPI *)(REFIID, void **);
		if (create_factory1 != nullptr && SUCCEEDED(reinterpret_cast<create_factory_fn>(create_factory1)(IID_PPV_ARGS(&factory))) && factory != nullptr)
			factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
		std::string why;
		const bool ok = host::install(why, warp);
		if (warp != nullptr)
			warp->Release();
		if (factory != nullptr)
			factory->Release();
		if (!ok)
		{
			write_log(2, ("HiFi XI: Direct3D 12 hooks not in: " + why).c_str());
			return false;
		}
		host::set_events(&events_table());
		g_early = true;
		g_started = true;
		write_log(0, "HiFi XI: Direct3D 12 hooks in, before the game's device: its lists and views followed from their making");
		const bool direct = direct_channel(true);
		g_own_d3d8 = direct;
		if (g_channel_refused != nullptr)
			write_log(2, (std::string("HiFi XI: ") + g_channel_refused).c_str());
		else
			write_log(direct ? 0 : 2, direct ? "HiFi XI: HiFi XI's d3d8.dll calls it at the game's passes (the direct channel)" :
				"HiFi XI: the d3d8.dll loaded isn't HiFi XI's own: its passes (native zone and characters, World FX, FSR) can't be placed and stay off; "
				"HiFi XI needs its own d3d8.dll in bootloader");
		// (frame generation: the d3d8.dll's swap chain made as AMD's, when the setting's on as the game starts, and FSR too:
		// generation runs only on FSR's frames, so without it AMD's swap chain would only stand in the way of each present)
		{
			const auto on = [](const char *key, bool by_default) {
				std::string v;
				return option("upscale", key, v) ? v != "0" && _stricmp(v.c_str(), "false") != 0 : by_default;
			};
			framegen::set_wanted(on("framegen", false) && on("fsr", true));
		}
		if (framegen::wanted())
		{
			// (AMD's own threads quiet: DXGI's list in their Present mustn't wait on World FX's lock)
			framegen::set_thread_start(host::quiet_thread);
			if (framegen::install(create_factory1, why, [](int level, const char *text) { write_log(level, text); }))
				write_log(0, "HiFi XI: frame generation's swap chain in (upscale.framegen on): the game window's swap chain will be AMD's");
			else
				write_log(2, ("HiFi XI: frame generation's swap chain not in: " + why).c_str());
		}
		return true;
	}

	bool late_start(std::string &why)
	{
		if (g_started.load())
			return true;
		setup_port();
		worldfx::set_crash_reports_status("off (Direct3D 12's device was made before HiFi XI loaded: HiFi XI's starter, bootloader\\dxgi.dll, makes them possible)");
		if (!host::install(why))
		{
			write_log(2, ("HiFi XI: Direct3D 12 hooks not in: " + why).c_str());
			return false;
		}
		host::set_events(&events_table());
		g_early = false;
		g_started = true;
		direct_channel(true);
		write_log(1, "HiFi XI: Direct3D 12 hooks in after the game's device was made (no starter, bootloader\\dxgi.dll): the views it made before aren't known, so its world isn't found");
		return true;
	}

	bool started() { return g_started.load(); }
	bool early() { return g_early; }

	void stop()
	{
		direct_channel(false);
		host::uninstall();
	}

	bool resume(std::string &why)
	{
		return host::install(why);
	}

	void present()
	{
		if (!g_started.load() || !host::installed())
			return;
		g_presents.fetch_add(1, std::memory_order_relaxed);
		// (the device and the game's queue: our d3d8.dll says them, nothing to learn from its submits)
		if (g_frame_device != nullptr)
		{
			ID3D12Device *device = nullptr;
			ID3D12CommandQueue *queue = nullptr;
			if (g_frame_device(&device, &queue) != 0)
			{
				if (g_device.load(std::memory_order_acquire) == nullptr)
					g_device.store(device, std::memory_order_release);
				g_queue.store(queue, std::memory_order_release);
			}
		}
		if (!g_attached.load(std::memory_order_acquire))
		{
			ID3D12Device *const device = g_device.load(std::memory_order_acquire);
			if (device == nullptr)
				return;
			g_rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			{
				host::quiet q;
				worldfx::attach(device);
			}
			g_attached.store(true, std::memory_order_release);
			write_log(0, "HiFi XI: World FX attached to the game's device");
		}
		ID3D12CommandQueue *const queue = g_queue.load(std::memory_order_acquire);
		if (queue == nullptr || gpu_crash::lost()) // (the device removed: World FX's own uploads and lists not sent)
			return;
		host::quiet q;
		worldfx::on_present(queue);
		retire::on_present(queue); // (the zone's, effects' and characters' GPU objects let go behind the GPU)
	}

	const std::wstring &folder() { return g_folder; }

	const std::wstring &settings_file() { return g_settings; }

	bool safe_start() { return g_safe_start; }

	bool own_d3d8() { return g_own_d3d8.load(); }

	bool setting(const char *key, std::string &value)
	{
		std::wstring w;
		if (g_settings.empty() || !ini_get(key, w))
			return false;
		value.clear();
		for (wchar_t c : w)
			value += c < 128 ? char(c) : '?';
		return true;
	}

	namespace
	{
		bool section_get(const wchar_t *section, const char *key, std::string &value)
		{
			if (g_settings.empty())
				return false;
			wchar_t wkey[64];
			size_t i = 0;
			for (; key[i] != 0 && i + 1 < 64; ++i)
				wkey[i] = wchar_t(key[i]);
			wkey[i] = 0;
			wchar_t buffer[256];
			const DWORD n = GetPrivateProfileStringW(section, wkey, L"\x1", buffer, 256, g_settings.c_str());
			if (n == 1 && buffer[0] == 1)
				return false;
			value.clear();
			for (DWORD k = 0; k < n; ++k)
				value += buffer[k] < 128 ? char(buffer[k]) : '?';
			return true;
		}
	}

	std::string preset()
	{
		std::string p;
		return section_get(L"hifixi", "preset", p) && !p.empty() ? p : std::string("custom");
	}

	void set_preset(const char *name)
	{
		if (g_settings.empty() || name == nullptr)
			return;
		std::wstring w;
		for (const char *c = name; *c != 0; ++c)
			w += wchar_t(*c);
		WritePrivateProfileStringW(L"hifixi", L"preset", w.c_str(), g_settings.c_str());
	}

	bool option(const char *section, const char *key, std::string &value)
	{
		if (section == nullptr || key == nullptr)
			return false;
		std::wstring w;
		for (const char *c = section; *c != 0; ++c)
			w += wchar_t(*c);
		return section_get(w.c_str(), key, value);
	}

	void set_option(const char *section, const char *key, const char *value)
	{
		if (g_settings.empty() || section == nullptr || key == nullptr)
			return;
		std::wstring s, k, v;
		for (const char *c = section; *c != 0; ++c)
			s += wchar_t(*c);
		for (const char *c = key; *c != 0; ++c)
			k += wchar_t(*c);
		for (const char *c = value != nullptr ? value : ""; *c != 0; ++c)
			v += wchar_t(*c);
		WritePrivateProfileStringW(s.c_str(), k.c_str(), value != nullptr ? v.c_str() : nullptr, g_settings.c_str());
	}

	bool start_settings_only(const wchar_t *settings_file, const wchar_t *folder)
	{
		if (g_started.load())
			return false;
		if (settings_file != nullptr)
			SetEnvironmentVariableW(L"HIFIXI_SETTINGS", settings_file);
		setup_port(folder);
		return true;
	}

	void log(int level, const char *text) { write_log(level, text); }

	uint64_t zone_ends_seen() { return marks_seen[slotmark::zone_end].load(std::memory_order_relaxed); }

	void set_log_sink(void (*sink)(int level, const char *text)) { g_log_sink.store(sink); }

	std::vector<std::string> status()
	{
		std::vector<std::string> out;
		char line[512];
		{
			std::string where;
			for (wchar_t c : g_settings)
				where += c < 128 ? char(c) : '?';
			out.push_back("settings: " + (where.empty() ? std::string("not read yet") : where));
		}
		if (g_safe_start)
			out.push_back("safe start: the GPU stopped last session (worldfx-gpu-crash.txt); light in the air, ambient occlusion, water "
				"reflections, rain, snow, cloud quality and lamp shadows off until the next start (or turned on again in the menu)");
		std::snprintf(line, sizeof(line), "started %s; World FX %s; %llu presents", !g_started.load() ? "not yet" : g_early ? "early (the starter)" : "late (no starter)",
			g_attached.load() ? "attached to the game's device" : "not attached yet", static_cast<unsigned long long>(g_presents.load()));
		out.push_back(line);
		for (const std::string &l : worldfx::frame_counts())
			out.push_back(l);
		out.push_back(shadercache::status());
		size_t known = 0;
		{
			std::lock_guard<std::mutex> lock(views_mutex);
			known = views.size();
		}
		std::snprintf(line, sizeof(line), "views: %zu known, %llu made and %llu copied since the start, %llu looked up not known", known,
			static_cast<unsigned long long>(views_made.load()), static_cast<unsigned long long>(views_copied.load()), static_cast<unsigned long long>(views_unknown.load()));
		out.push_back(line);
		size_t list_count = 0;
		{
			std::lock_guard<std::mutex> lock(lists_mutex);
			list_count = lists.size();
		}
		std::snprintf(line, sizeof(line), "lists followed: %zu", list_count);
		out.push_back(line);
		std::snprintf(line, sizeof(line), "slot markers seen in Direct3D 12: zone begin %llu, zone end %llu; out of order %llu",
			static_cast<unsigned long long>(marks_seen[slotmark::zone_begin].load()), static_cast<unsigned long long>(marks_seen[slotmark::zone_end].load()),
			static_cast<unsigned long long>(marks_out_of_order.load()));
		out.push_back(line);
		out.push_back(chars::marks::status()); // (native characters)
		return out;
	}

}

// The starter's call (src/starter): the first time the game asks DXGI for a factory, with DXGI's own
// CreateDXGIFactory1 (not the starter's)
extern "C" __declspec(dllexport) int __cdecl hifixi_early_start(void *create_factory1)
{
	return hifi::backend::early_start(create_factory1) ? 1 : 0;
}

// For the native end-to-end test, which has no Ashita: what the plugin does at each present
extern "C" __declspec(dllexport) void __cdecl hifixi_present()
{
	hifi::backend::present();
}

// For the offline probe (research/ffxi-re/tools/dgvprobe, native mode), which has no game: a zone pass as the game side
// would publish it, for HiFi XI to draw at the zone's end marker on the d3d8.dll's own list. `image`: an FFXiMain dump's bytes
// (the key tables); the map's data loaded and put on the GPU over the first calls (0 until it's there); then every
// placement of its MZB, as seen from view_proj (row vectors), a day's light. 1 when the pass is published under seq.
namespace hifi::game
{
	void set_image(uintptr_t base, size_t size); // (src/game/game.hpp, whose export declarations this file defines)
}

extern "C" __declspec(dllexport) int __cdecl hifixi_zone_test(const void *image, uint32_t image_size, uint32_t map, uint32_t seq, const float *view_proj)
{
	namespace zm = hifi::bake::zonemesh;
	ID3D12Device *const device = worldfx::attached();
	if (device == nullptr || image == nullptr)
		return 0;
	static bool image_set = false;
	if (!image_set)
	{
		hifi::game::set_image(reinterpret_cast<uintptr_t>(image), image_size);
		image_set = true;
	}
	hifi::zone::native::frame(device, map, 0, true);
	hifi::zone::gpu_zone *const gz = hifi::zone::native::ready_zone();
	std::string why;
	if (gz == nullptr || !hifi::zone::inject::prepare(device, why))
		return 0;
	// (the sun's shadow cascades from it, as the plugin hands them to World FX)
	static bool zone_casters_set = false;
	if (!zone_casters_set)
	{
		worldfx::set_zone_casters(hifi::zone::inject::zone_bounds_hook(), hifi::zone::inject::zone_draw_hook(), nullptr);
		worldfx::set_floor_source(hifi::zone::inject::floor_hook(), nullptr);
		zone_casters_set = true;
	}
	const zm::zone &d = gz->data();
	hifi::zone::frame &f = hifi::zone::frame_slot(seq);
	f.native = true;
	f.zone = gz;
	f.map = map;
	f.second = nullptr;
	f.env_count = 1;
	f.no_bump = GetEnvironmentVariableA("LOOK_NOBUMP", nullptr, 0) != 0; // (dgvprobe's look test: as its D3D8 replay draws)
	std::memcpy(f.view_proj, view_proj, 64);
	{
		// The camera: where clip x, y and w are 0 ((p, 1) x the matrix's columns 0, 1 and 3; row vectors), by Cramer's rule
		const float *m = view_proj;
		const float a[3][3] = { { m[0], m[4], m[8] }, { m[1], m[5], m[9] }, { m[3], m[7], m[11] } }, b[3] = { -m[12], -m[13], -m[15] };
		auto det = [](const float q[3][3]) {
			return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) + q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
		};
		const float whole = det(a);
		for (int k = 0; k < 3; ++k)
		{
			float q[3][3];
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
					q[r][c] = c == k ? b[r] : a[r][c];
			f.camera[k] = whole != 0.0f ? det(q) / whole : 0.0f;
		}
	}
	const float sd[3] = { 0.36f, -0.83f, 0.42f }, sun[3] = { 1.26f, 1.25f, 1.1f }, amb[3] = { 0.49f, 0.47f, 0.49f }, fog[3] = { 0.55f, 0.65f, 0.78f };
	std::memcpy(f.sun_dir, sd, 12);
	std::memcpy(f.sun, sun, 12);
	std::memcpy(f.sun_bump, sun, 12);
	std::memcpy(f.ambient, amb, 12);
	std::memcpy(f.ambient_bump, amb, 12);
	std::memcpy(f.fog, fog, 12);
	f.fog_start = 150.0f;
	f.fog_end = 600.0f;
	f.fog_scale = 1.0f;
	f.count = 0;
	for (const zm::placement &p : d.placements)
	{
		if (f.count >= hifi::zone::kMaxObjects)
			break;
		hifi::zone::frame_object &o = f.objects[f.count++];
		for (int k = 0; k < 3; ++k)
		{
			o.world[k * 4 + 0] = p.m[k];
			o.world[k * 4 + 1] = p.m[3 + k];
			o.world[k * 4 + 2] = p.m[6 + k];
			o.world[k * 4 + 3] = p.t[k];
		}
		o.model = p.model;
		o.flags = p.flags;
		o.sway = 0.0f;
		o.env = 0;
		o.door_env = 0;
		o.zone = 0;
		for (uint32_t &l : o.lamps)
			l = 0xFFFFFFFFu;
	}
	f.lamp_count = 0;
	hifi::zone::publish(f, seq);
	return 1;
}

// For the offline probe: the native zone renderer's status line (what it drew, why it couldn't)
extern "C" __declspec(dllexport) void __cdecl hifixi_zone_status(char *out, uint32_t size)
{
	std::string all;
	for (const auto &lines : { hifi::zone::inject::status(), hifi::zone::native::status() })
		for (const std::string &line : lines)
			all += (all.empty() ? "" : "; ") + line;
	std::snprintf(out, size, "%s", all.c_str());
}

// For the worldfx Ashita addon (0.16): one of World FX's settings ([worldfx] in HiFi XI's settings file), as its text;
// 0 when it isn't set (the default then)
extern "C" __declspec(dllexport) int __cdecl worldfx_setting(const char *key, char *out, uint32_t size)
{
	std::string value;
	if (key == nullptr || out == nullptr || size == 0 || !hifi::backend::setting(key, value))
		return 0;
	const size_t n = std::min<size_t>(value.size(), size - 1);
	std::memcpy(out, value.data(), n);
	out[n] = 0;
	return 1;
}

// For HiFi XI's game side (src/game: hifps, drawcull): one of its options, in its own section of HiFi XI's settings
// file ([hifps], [drawcull]), as its text; 0 when it isn't set (the default then)
extern "C" __declspec(dllexport) int __cdecl hifixi_option(const char *section, const char *key, char *out, uint32_t size)
{
	std::string value;
	if (out == nullptr || size == 0 || !hifi::backend::option(section, key, value))
		return 0;
	const size_t n = std::min<size_t>(value.size(), size - 1);
	std::memcpy(out, value.data(), n);
	out[n] = 0;
	return 1;
}

// And one set (value nullptr: taken out, its default again)
extern "C" __declspec(dllexport) void __cdecl hifixi_set_option(const char *section, const char *key, const char *value)
{
	hifi::backend::set_option(section, key, value);
}

// For the worldfx Ashita addon (0.16): the folder World FX's zone files are in (the add-on's was its own)
extern "C" __declspec(dllexport) const char *__cdecl worldfx_folder()
{
	static std::string narrow;
	if (narrow.empty())
	{
		const std::wstring &w = hifi::backend::folder();
		const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
		narrow.assign(size_t(n), '\0');
		if (n > 0)
			WideCharToMultiByte(CP_ACP, 0, w.c_str(), int(w.size()), narrow.data(), n, nullptr, nullptr);
	}
	return narrow.c_str();
}
