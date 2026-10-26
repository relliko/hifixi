#include <windows.h>
#include "runner.hpp"
#include "bake.hpp"
#include "dat.hpp"
#include "../render/worldfx_core.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <new>
#include <mutex>
#include <thread>

namespace hifi::bake
{
	namespace
	{
		// The zone ids "every zone" goes through (file ids 100..399: the game's zones and more; one that isn't a zone
		// has no collision, meshes or lamps: none of each, quickly)
		constexpr uint32_t kFirstZone = 0, kLastZone = 299;

		// The kinds, in the order they're made (rain and the sea after water: they read the zone's water). A kind added
		// later (the sea, 2026-10-22) is simply missing for every zone of an install made before it: made as zones are
		// entered, or by "every zone".
		enum class kind { grass, water, wet, rain, sea, shadow, lamps };
		const kind kKinds[] = { kind::grass, kind::water, kind::wet, kind::rain, kind::sea, kind::shadow, kind::lamps };
		const wchar_t *folder_of(kind k)
		{
			switch (k)
			{
			case kind::grass: return L"worldfx-grass";
			case kind::water: return L"worldfx-water";
			case kind::wet: return L"worldfx-wet";
			case kind::rain: return L"worldfx-rain";
			case kind::sea: return L"worldfx-sea";
			case kind::shadow: return L"worldfx-shadow";
			default: return L"worldfx-lamps";
			}
		}
		const char *name_of(kind k)
		{
			switch (k)
			{
			case kind::grass: return "grass";
			case kind::water: return "water";
			case kind::wet: return "wet ground";
			case kind::rain: return "rain";
			case kind::sea: return "sea";
			case kind::shadow: return "shadow map";
			default: return "lamps";
			}
		}

		struct state
		{
			std::mutex mutex;
			std::condition_variable wake;
			std::thread thread;
			std::wstring folder;
			std::deque<uint32_t> queue;
			bool quit = false;
			bool busy = false;
			uint32_t zone = 0, total = 0;
			uint32_t entered = 0;          // (the zone you're in, to have World FX read its new files)
			uint32_t made = 0, none = 0, failed = 0;
			std::vector<std::string> lines; // (the last few results)
		};
		state g;

		bool exists(const std::wstring &path)
		{
			const DWORD a = GetFileAttributesW(path.c_str());
			return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
		}

		// Whether a zone's lamps file is one from before the hour (LMP1, 2026-10-23: brightness without the lamp's
		// curve over the day): made again, as if missing
		bool older_lamps(kind k, const std::wstring &path)
		{
			if (k != kind::lamps)
				return false;
			char tag[4] = {};
			FILE *f = nullptr;
			if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr)
				return false;
			const bool read = fread(tag, 1, 4, f) == 4;
			fclose(f);
			return read && std::memcmp(tag, "LMP1", 4) == 0;
		}

		std::wstring file_of(const std::wstring &folder, kind k, uint32_t zone, const wchar_t *ext)
		{
			return folder + folder_of(k) + L"\\" + std::to_wstring(zone) + ext;
		}

		// Water made by the sea rule (2026-10-23: where the zone's collision water lies well under the game's sea sheets,
		// the water takes the sheets' height; it was 2-4.5 yalms under the sea in Port San d'Oria, Port Windurst,
		// Kazham, Lufaise, Misareaux, Qufim and 13 more) gets this mark beside it (an empty <zone>.r2). A water file
		// without it is from before: it and what's made from it (rain, sea; their none marks too) are made again.
		constexpr const wchar_t *kWaterRule = L".r2";

		// Whether the zone is one the sun lights (worldfx-sunlit.txt, shipped with HiFi XI: hand-made); every zone when
		// the list isn't there
		bool sunlit(const std::wstring &folder, uint32_t zone)
		{
			FILE *f = nullptr;
			if (_wfopen_s(&f, (folder + L"worldfx-sunlit.txt").c_str(), L"r") != 0 || f == nullptr)
				return true;
			bool found = false;
			char line[128];
			while (fgets(line, sizeof(line), f) != nullptr)
				if (line[0] >= '0' && line[0] <= '9' && std::strtoul(line, nullptr, 10) == zone)
					found = true;
			fclose(f);
			return found;
		}

		void note(const std::string &line)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.lines.push_back(line);
			if (g.lines.size() > 6)
				g.lines.erase(g.lines.begin());
		}

		// One zone's missing files; whether any was made
		bool make_zone(const std::wstring &folder, uint32_t zone)
		{
			bool any = false;
			const bool old_water = exists(file_of(folder, kind::water, zone, L".bin")) && !exists(file_of(folder, kind::water, zone, kWaterRule));
			for (const kind k : kKinds)
			{
				const std::wstring out = file_of(folder, k, zone, L".bin"), none = file_of(folder, k, zone, L".none");
				// (rain and wet ground for every zone the sun lights, whatever its own weather, so a forced weather (/hifi
				// weather) rains there too: a none mark made before that rule has no <zone>.aw beside it and is made again)
				const bool any_weather = (k == kind::rain || k == kind::wet) && sunlit(folder, zone);
				const std::wstring aw = file_of(folder, k, zone, L".aw");
				const bool old_none = any_weather && exists(none) && !exists(aw);
				const bool redo = older_lamps(k, out) || (old_water && (k == kind::water || k == kind::rain || k == kind::sea)) || old_none;
				if (!redo && (exists(out) || exists(none)))
					continue;
				if (redo)
				{
					// (made again from nothing: a none mark or an old file left would stand for it if this one fails)
					DeleteFileW(none.c_str());
					if (k != kind::water && k != kind::lamps)
						DeleteFileW(out.c_str());
				}
				if (k == kind::shadow && !sunlit(folder, zone))
					continue;
				// (the sea reads the zone's water: none when the zone has none; when its water wasn't made (it failed),
				// not tried, so not marked none either: tried again with the water next time)
				if (k == kind::sea && !exists(file_of(folder, kind::water, zone, L".bin")))
				{
					if (exists(file_of(folder, kind::water, zone, L".none")))
					{
						std::lock_guard<std::mutex> lock(g.mutex);
						g.none++;
						write_file(none, "", 0);
					}
					continue;
				}
				result r;
				const auto t0 = std::chrono::steady_clock::now();
				bool ok = false;
				// (a baker that throws, out of memory or anything else, fails that file alone: the thread goes on)
				try
				{
					switch (k)
					{
					case kind::grass: ok = grass(zone, out, r); break;
					case kind::water: ok = water(zone, out, r); break;
					case kind::wet: ok = wet(zone, out, r, any_weather); break;
					case kind::rain: ok = rain(zone, out, file_of(folder, kind::water, zone, L".bin"), r, any_weather); break;
					case kind::sea: ok = sea(zone, out, file_of(folder, kind::water, zone, L".bin"), r); break;
					case kind::shadow: ok = shadow(zone, out, r); break;
					case kind::lamps: ok = lamps(zone, out, r); break;
					}
				}
				catch (const std::bad_alloc &)
				{
					ok = false;
					r = result();
					r.why = "out of memory";
				}
				catch (const std::exception &e)
				{
					ok = false;
					r = result();
					r.why = e.what();
				}
				const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
				char line[256];
				if (ok && k == kind::water)
					write_file(file_of(folder, kind::water, zone, kWaterRule), "", 0);
				std::lock_guard<std::mutex> lock(g.mutex);
				if (ok)
				{
					g.made++;
					any = true;
					std::snprintf(line, sizeof(line), "zone %u: its %s made (%.1f s)", zone, name_of(k), s);
					g.lines.push_back(line);
				}
				else if (r.none)
				{
					g.none++;
					write_file(none, "", 0);
					if (any_weather)
						write_file(aw, "", 0);
				}
				else
				{
					g.failed++;
					std::snprintf(line, sizeof(line), "zone %u: its %s couldn't be made: %s", zone, name_of(k), r.why.c_str());
					g.lines.push_back(line);
				}
				if (g.lines.size() > 6)
					g.lines.erase(g.lines.begin());
			}
			return any;
		}

		void run()
		{
			for (;;)
			{
				uint32_t zone = 0;
				std::wstring folder;
				{
					std::unique_lock<std::mutex> lock(g.mutex);
					g.wake.wait(lock, [] { return g.quit || !g.queue.empty(); });
					if (g.quit)
						return;
					zone = g.queue.front();
					g.queue.pop_front();
					g.busy = true;
					g.zone = zone;
					folder = g.folder;
				}
				const bool any = make_zone(folder, zone);
				std::lock_guard<std::mutex> lock(g.mutex);
				g.busy = false;
				if (g.queue.empty())
					g.total = 0;
				// (the zone you're in has new files: World FX reads them again)
				if (any && zone == g.entered)
					worldfx::reload_zone();
			}
		}
	}

	namespace runner
	{
		void start(const std::wstring &folder)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.folder = folder;
			if (g.thread.joinable())
				return;
			g.quit = false;
			g.thread = std::thread(run);
			// (below the game's threads: making files never costs a frame)
			SetThreadPriority(g.thread.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
		}

		void zone_entered(uint32_t zone)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.entered = zone;
			if (zone == 0 || zone >= 1000) // (none yet; a Mog House: nothing of its own to make)
				return;
			for (auto it = g.queue.begin(); it != g.queue.end(); ++it)
				if (*it == zone)
				{
					g.queue.erase(it);
					break;
				}
			g.queue.push_front(zone);
			g.total = std::max<uint32_t>(g.total, uint32_t(g.queue.size()));
			g.wake.notify_one();
		}

		void build_all()
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			for (uint32_t zone = kFirstZone; zone <= kLastZone; ++zone)
				if (std::find(g.queue.begin(), g.queue.end(), zone) == g.queue.end())
					g.queue.push_back(zone);
			g.total = uint32_t(g.queue.size());
			g.wake.notify_one();
		}

		void cancel()
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.queue.clear();
			g.total = 0;
		}

		runner_status status()
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			runner_status s;
			s.busy = g.busy || !g.queue.empty();
			s.zone = g.zone;
			s.left = uint32_t(g.queue.size()) + (g.busy ? 1u : 0u);
			s.total = g.total;
			char line[160];
			std::snprintf(line, sizeof(line), "Made so far: %u files (%u kinds a zone has none of, %u that couldn't be made)", g.made, g.none, g.failed);
			s.lines.push_back(line);
			for (const std::string &l : g.lines)
				s.lines.push_back(l);
			return s;
		}

		void stop()
		{
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				g.quit = true;
				g.queue.clear();
				g.wake.notify_one();
			}
			if (g.thread.joinable())
				g.thread.join();
		}
	}
}
