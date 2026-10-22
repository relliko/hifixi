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

		// The kinds, in the order they're made (rain after water: it reads the zone's water)
		enum class kind { grass, water, wet, rain, shadow, lamps };
		const kind kKinds[] = { kind::grass, kind::water, kind::wet, kind::rain, kind::shadow, kind::lamps };
		const wchar_t *folder_of(kind k)
		{
			switch (k)
			{
			case kind::grass: return L"worldfx-grass";
			case kind::water: return L"worldfx-water";
			case kind::wet: return L"worldfx-wet";
			case kind::rain: return L"worldfx-rain";
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

		std::wstring file_of(const std::wstring &folder, kind k, uint32_t zone, const wchar_t *ext)
		{
			return folder + folder_of(k) + L"\\" + std::to_wstring(zone) + ext;
		}

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
			for (const kind k : kKinds)
			{
				const std::wstring out = file_of(folder, k, zone, L".bin"), none = file_of(folder, k, zone, L".none");
				if (exists(out) || exists(none))
					continue;
				if (k == kind::shadow && !sunlit(folder, zone))
					continue;
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
					case kind::wet: ok = wet(zone, out, r); break;
					case kind::rain: ok = rain(zone, out, file_of(folder, kind::water, zone, L".bin"), r); break;
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
