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
#include "../render/worldfx_core.hpp"
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
// queue was seen). Everything the core records itself on dgVoodoo's lists, and every view it makes, goes through the
// same hooks: the core is called with the thread quiet (host::quiet), so they let those through untouched.
//
// Which resource a view is of: dgVoodoo's render target and depth views as they're made (CreateRenderTargetView,
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
		std::atomic<bool> g_attached { false };
		UINT g_rtv_increment = 0;
		std::atomic<void (*)(int, const char *)> g_log_sink { nullptr };
		std::atomic<uint64_t> g_presents { 0 };

		// The log: hifixi.log in the folder, started afresh each run, written as it comes (a crash keeps it)
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

		// Each list's data for the core (made with the list or at its first reset since the start; never freed: a list
		// at the same address later is made or reset before it records)
		std::mutex lists_mutex;
		std::unordered_map<void *, std::unique_ptr<worldfx::list_data>> lists;
		thread_local void *tl_list = nullptr;
		thread_local worldfx::list_data *tl_data = nullptr;

		worldfx::list_data *data_of(void *list, bool make)
		{
			if (list == tl_list && tl_data != nullptr)
				return tl_data;
			std::lock_guard<std::mutex> lock(lists_mutex);
			auto it = lists.find(list);
			if (it == lists.end())
			{
				if (!make)
					return nullptr;
				it = lists.emplace(list, std::make_unique<worldfx::list_data>()).first;
			}
			tl_list = list;
			tl_data = it->second.get();
			return tl_data;
		}
		// A list's data when the core should hear of it (attached, the list known)
		worldfx::list_data *live(void *list)
		{
			return g_attached.load(std::memory_order_acquire) ? data_of(list, false) : nullptr;
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
		bool port_get_bool(const char *key, bool &value)
		{
			std::wstring s;
			if (!ini_get(key, s))
				return false;
			value = s == L"1" || _wcsicmp(s.c_str(), L"true") == 0;
			return true;
		}
		bool port_get_float(const char *key, float &value)
		{
			std::wstring s;
			if (!ini_get(key, s))
				return false;
			value = std::wcstof(s.c_str(), nullptr);
			return true;
		}
		bool port_get_int(const char *key, int &value)
		{
			std::wstring s;
			if (!ini_get(key, s))
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
			g_settings = settings_path();
			import_settings();
			tidy_settings();
			import_addon_options();
			{
				std::string where;
				for (wchar_t c : g_settings)
					where += c < 128 ? char(c) : '?';
				write_log(0, ("HiFi XI: settings from " + where).c_str());
			}
			worldfx::port p;
			p.log = port_log;
			p.get_bool = port_get_bool;
			p.get_float = port_get_float;
			p.get_int = port_get_int;
			p.set_bool = port_set_bool;
			p.set_float = port_set_float;
			p.set_int = port_set_int;
			p.resource_of_view = port_resource_of_view;
			p.format_of_view = port_format_of_view;
			p.folder = g_folder;
			worldfx::set_port(p);
			worldfx::load_settings();
			streamlog::set_views(streamlog_view);
			streamlog::set_folder(g_folder);
		}

		// The hooks' events
		void ev_created(ID3D12GraphicsCommandList *list)
		{
			*data_of(list, true) = worldfx::list_data();
		}
		void ev_reset(ID3D12GraphicsCommandList *list)
		{
			zone::inject::on_reset(list);
			chars::inject::on_reset(list); // (native characters)
			upscale::on_reset(list);
			worldfx::list_data *const d = data_of(list, true);
			if (!g_attached.load(std::memory_order_acquire))
			{
				*d = worldfx::list_data();
				return;
			}
			host::quiet q;
			worldfx::on_reset(*d);
		}
		void ev_close(ID3D12GraphicsCommandList *list)
		{
			if (worldfx::list_data *const d = live(list))
			{
				host::quiet q;
				upscale::before_other(list, d->state); // (the screen upscaled before the list closes, when it's waiting)
				worldfx::on_close(list, *d);
			}
		}
		void ev_targets(ID3D12GraphicsCommandList *list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs, BOOL single_range, const D3D12_CPU_DESCRIPTOR_HANDLE *dsv)
		{
			worldfx::list_data *const d = live(list);
			if (d == nullptr)
				return;
			const uint32_t n = rtvs != nullptr ? std::min<uint32_t>(count, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) : 0;
			uint64_t v[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
			for (uint32_t i = 0; i < n; ++i)
				v[i] = single_range ? uint64_t(rtvs[0].ptr) + uint64_t(i) * g_rtv_increment : uint64_t(rtvs[i].ptr);
			host::quiet q;
			worldfx::on_targets(list, *d, n, v, dsv != nullptr ? uint64_t(dsv->ptr) : 0);
		}
		void ev_pipeline(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso)
		{
			if (worldfx::list_data *const d = live(list))
				worldfx::on_pipeline(*d, pso);
		}
		void ev_topology(ID3D12GraphicsCommandList *list, D3D12_PRIMITIVE_TOPOLOGY topology)
		{
			if (worldfx::list_data *const d = live(list))
				worldfx::on_topology(*d, topology);
		}
		void ev_viewports(ID3D12GraphicsCommandList *list, UINT count, const D3D12_VIEWPORT *viewports)
		{
			if (worldfx::list_data *const d = live(list))
				worldfx::on_viewports(*d, 0, viewports != nullptr ? count : 0, viewports);
		}
		void ev_scissors(ID3D12GraphicsCommandList *list, UINT count, const D3D12_RECT *rects)
		{
			if (worldfx::list_data *const d = live(list))
				worldfx::on_scissors(*d, 0, rects != nullptr ? count : 0, rects);
		}
		void ev_root_signature(ID3D12GraphicsCommandList *list, ID3D12RootSignature *root)
		{
			if (worldfx::list_data *const d = live(list))
				worldfx::on_root_tables(*d, root, 0, 0, nullptr);
		}
		void ev_root_table(ID3D12GraphicsCommandList *list, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE table)
		{
			if (worldfx::list_data *const d = live(list))
			{
				const uint64_t t = table.ptr;
				worldfx::on_root_tables(*d, d->state.root, param, 1, &t);
			}
		}
		void ev_root_constants(ID3D12GraphicsCommandList *list, UINT param, UINT count, const void *values, UINT first)
		{
			if (worldfx::list_data *const d = live(list))
				worldfx::on_root_constants(*d, d->state.root, param, first, count, values);
		}
		void ev_root_cbv(ID3D12GraphicsCommandList *list, UINT param, D3D12_GPU_VIRTUAL_ADDRESS address)
		{
			if (worldfx::list_data *const d = live(list))
				worldfx::on_root_descriptor(*d, d->state.root, param, grass::root_arg::cbv, address);
		}
		void ev_root_srv(ID3D12GraphicsCommandList *list, UINT param, D3D12_GPU_VIRTUAL_ADDRESS address)
		{
			if (worldfx::list_data *const d = live(list))
				worldfx::on_root_descriptor(*d, d->state.root, param, grass::root_arg::srv, address);
		}
		void ev_root_uav(ID3D12GraphicsCommandList *list, UINT param, D3D12_GPU_VIRTUAL_ADDRESS address)
		{
			if (worldfx::list_data *const d = live(list))
				worldfx::on_root_descriptor(*d, d->state.root, param, grass::root_arg::uav, address);
		}
		void ev_barrier(ID3D12GraphicsCommandList *list, UINT count, const D3D12_RESOURCE_BARRIER *barriers)
		{
			worldfx::list_data *const d = live(list);
			if (d == nullptr)
				return;
			// (the native zone renderer follows the world depth's planes, each of them)
			zone::inject::on_barrier(count, barriers);
			chars::inject::on_barrier(count, barriers); // (native characters: the same)
			// (transitions only, as ReShade reports them: the resource and where it went. Of a subresource other than the
			// first only when it's all of them: the core follows the world's depth plane, and the slot markers' stencil
			// clears move the stencil plane (subresource 1) on its own, DEPTH_READ to DEPTH_WRITE and back at the next draw,
			// which would otherwise read as the depth's state)
			host::quiet q;
			for (UINT done = 0; done < count;)
			{
				ID3D12Resource *res[32];
				D3D12_RESOURCE_STATES after[32];
				uint32_t n = 0;
				for (; done < count && n < 32; ++done)
					if (barriers[done].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
						(barriers[done].Transition.Subresource == 0 || barriers[done].Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
					{
						res[n] = barriers[done].Transition.pResource;
						after[n] = barriers[done].Transition.StateAfter;
						n++;
					}
				if (n != 0)
					worldfx::on_barrier(list, *d, n, res, after);
			}
			// (the world's end on this list: the upscale's resolve is the next draw out of it)
			if (d->world_ended != nullptr)
			{
				upscale::on_world_end(list, d->world_ended, d->world_rt_state);
				d->world_ended = nullptr;
			}
			upscale::after_barrier(list, count, barriers, d->state);
		}
		// The game side's slot markers (src/game/slotmark.hpp) as dgVoodoo hands them on: how many of each kind, how many
		// came out of order (each zone begin, then its end with the same seq, then the next seq's begin), the last
		std::atomic<uint64_t> marks_seen[slotmark::kinds] = {};
		std::atomic<uint64_t> marks_out_of_order { 0 };
		std::atomic<uint32_t> last_mark { 0xFFFFFFFFu }; // (kind << 8 | seq)
		// The world's target as the markers find it (dgVoodoo's recording thread), and its view's format
		ID3D12Resource *zone_rt = nullptr;
		DXGI_FORMAT zone_rt_format = DXGI_FORMAT_UNKNOWN;

		void on_mark(ID3D12GraphicsCommandList *list, worldfx::list_data &d, uint64_t dsv, const slotmark::mark &m)
		{
			marks_seen[m.kind].fetch_add(1, std::memory_order_relaxed);
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
			ID3D12Resource *const depth = port_resource_of_view(dsv);
			zone::inject::on_marker(depth);
			chars::inject::on_marker(depth); // (native characters)
			if (m.kind == slotmark::zone_begin)
				worldfx::on_world_marker(d, dsv);
			// The world's target: the one bound with this depth on this list, else the one found with it before
			if (depth != nullptr && d.ds == depth && d.rt0 != nullptr && d.state.rtv_count != 0)
			{
				zone_rt = d.rt0;
				zone_rt_format = port_format_of_view(uint64_t(d.state.rtvs[0].ptr));
			}
			if (m.kind == slotmark::zone_end && depth != nullptr)
			{
				zone::inject::world w;
				w.rt = zone_rt;
				w.ds = depth;
				w.rt_format = zone_rt_format;
				w.ds_format = port_format_of_view(dsv);
				w.rt_state = d.world_rt_state;
				zone::inject::on_zone_end(list, d.state, w, m.seq);
				// (then World FX's blades, written into the world's depth before the characters and effects)
				D3D12_RESOURCE_STATES planes[2];
				if (zone::inject::depth_planes(depth, planes))
					worldfx::on_zone_end(list, d, dsv, planes);
			}
			// (native characters: the opaque models at their end marker, a see-through one at its own)
			if ((m.kind == slotmark::chars_end || m.kind == slotmark::see_through) && depth != nullptr)
			{
				chars::inject::world w;
				w.rt = zone_rt;
				w.ds = depth;
				w.rt_format = zone_rt_format;
				w.ds_format = port_format_of_view(dsv);
				w.rt_state = d.world_rt_state;
				if (m.kind == slotmark::chars_end)
				{
					chars::inject::on_chars_end(list, d.state, w, m.seq);
					// (the world with its opaque characters: FSR's reactive mask from here)
					upscale::masks::snapshot(list, w.rt, w.rt_state, upscale::masks::opaque);
				}
				else
					chars::inject::on_see_through(list, d.state, w, m.seq);
			}
		}

		void ev_clear_depth(ID3D12GraphicsCommandList *list, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, UINT count, const D3D12_RECT *rects)
		{
			worldfx::list_data *const d = live(list);
			if (d == nullptr)
				return;
			if ((flags & D3D12_CLEAR_FLAG_DEPTH) == 0)
			{
				// (a stencil-only clear: one of the game side's markers, when it's shaped like one on a view that's known)
				slotmark::mark m;
				if (rects != nullptr && count != 0 && slotmark::read(false, (flags & D3D12_CLEAR_FLAG_STENCIL) != 0, count, rects[0].left, rects[0].top, rects[0].right, rects[0].bottom, m) &&
					port_resource_of_view(dsv.ptr) != nullptr)
				{
					host::quiet q;
					on_mark(list, *d, dsv.ptr, m);
				}
				return;
			}
			// (a view not known isn't the world's: ReShade always knew which resource it was)
			if (port_resource_of_view(dsv.ptr) == nullptr)
				return;
			host::quiet q;
			upscale::before_other(list, d->state);
			worldfx::on_clear_depth(list, *d, dsv.ptr, true, rects != nullptr ? count : 0, rects);
		}
		void ev_clear_target(ID3D12GraphicsCommandList *list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT count, const D3D12_RECT *rects)
		{
			if (worldfx::list_data *const d = live(list))
			{
				host::quiet q;
				upscale::before_other(list, d->state);
				worldfx::on_clear_target(*d, rtv.ptr, color, rects != nullptr ? count : 0, rects);
			}
		}
		void ev_heaps(ID3D12GraphicsCommandList *list, UINT count, ID3D12DescriptorHeap *const *heaps)
		{
			if (live(list) != nullptr)
			{
				zone::inject::on_heaps(list, count, heaps);
				chars::inject::on_heaps(list, count, heaps); // (native characters)
				upscale::on_heaps(list, count, heaps);
			}
		}
		void ev_draw(ID3D12GraphicsCommandList *list, uint64_t vertices)
		{
			if (worldfx::list_data *const d = live(list))
			{
				{
					host::quiet q;
					upscale::before_draw(list, vertices, d->rt0, uint64_t(d->state.rtvs[0].ptr), port_format_of_view, d->state);
				}
				worldfx::on_draw(*d, vertices);
			}
		}
		void ev_execute(ID3D12CommandQueue *queue, UINT, ID3D12CommandList *const *)
		{
			if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
				return;
			g_queue.store(queue, std::memory_order_release);
			if (g_device.load(std::memory_order_acquire) == nullptr)
			{
				ID3D12Device *device = nullptr;
				if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr)
				{
					device->Release(); // (dgVoodoo holds it for as long as it draws)
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

	bool early_start(void *create_factory1)
	{
		if (g_started.load())
			return true;
		setup_port();
		write_log(0, "HiFi XI: starting before the game's Direct3D 12 device (the starter, bootloader\\dxgi.dll)");
		worldfx::before_device(true);
		// The throwaway objects the hooks are found by: on WARP, so the game's adapter isn't touched before dgVoodoo
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
		// (frame generation: dgVoodoo's swap chain made as AMD's, when the setting's on as the game starts)
		{
			std::string on;
			framegen::set_wanted(option("upscale", "framegen", on) && on != "0" && _stricmp(on.c_str(), "false") != 0);
		}
		if (framegen::wanted())
		{
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
		write_log(1, "HiFi XI: Direct3D 12 hooks in after the game's device was made (no starter, bootloader\\dxgi.dll): the views it made before aren't known, so its world isn't found");
		return true;
	}

	bool started() { return g_started.load(); }
	bool early() { return g_early; }

	void stop()
	{
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
		if (queue == nullptr)
			return;
		host::quiet q;
		worldfx::on_present(queue);
	}

	const std::wstring &folder() { return g_folder; }

	const std::wstring &settings_file() { return g_settings; }

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
		std::snprintf(line, sizeof(line), "started %s; World FX %s; %llu presents", !g_started.load() ? "not yet" : g_early ? "early (the starter)" : "late (no starter)",
			g_attached.load() ? "attached to the game's device" : "not attached yet", static_cast<unsigned long long>(g_presents.load()));
		out.push_back(line);
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
// would publish it, for HiFi XI to draw at the zone's end marker on dgVoodoo's own list. `image`: an FFXiMain dump's bytes
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
