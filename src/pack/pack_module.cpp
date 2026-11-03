/*
 * The texture pack as an own-frame module (src/host/passes.hpp), "textures" (pack_module.hpp; the design:
 * docs/texture-pack/zones.md "The module's lifecycle"). All of it runs on the game's thread: update every half second,
 * present each present, device on a change, the commands, stop first in the plugin's Release.
 *
 *   - [textures] pack (off by default) and a pack folder (<bootloader>\hifixi-pack, pack.idx in it) turn it on: the
 *     streamer (src/pack/stream) started once a session with the game's device, the pack opened on its worker (never
 *     read here), owners told the epoch they ask in once it's open. Turned off: every owner dropped first, the loaders
 *     out of pack mode, the pack closed on the worker (its files let go: the folder can be deleted), the streamer
 *     paused (kOff) but kept (its ring's address space isn't given back: streamer.md "As built").
 *   - The order at every close, open (on, reload) and stop is the consumer's clear_all first, in the same call (zones.md
 *     decision 2): no owner holds a ticket of the epoch the call ends.
 *   - States: off, opening, on, failed (the open refused the pack: why in the status), lost (the device removed: nothing
 *     more for the session). After a GPU stop (backend::safe_start) the pack stays off for the session unless /hifi
 *     textures on says otherwise.
 *   - Each update: the streamer's limits (video memory from gpu_crash::video_memory, [textures] budget_mb, the address
 *     space from memwatch, [textures] per_present_mb), its log lines into hifixi.log, the status and menu strings
 *     (status() and menu_lines() only hand them out). Each present: stream::on_present, the quiet window (kQuiet), then
 *     the consumer's frame.
 *   - As in M1, the game's memory measured and reported: the address space and its largest free block (memwatch, on its
 *     own thread), the video memory, the characters' and the zone's bytes on the GPU, each zone's lowest free memory
 *     (logged once as the next zone comes), and whether a texture bake runs.
 */

#include <windows.h>
#include "pack_module.hpp"
#include "memwatch.hpp"
#include "pack.hpp"
#include "packoptions.hpp"
#include "stream.hpp"
#include "../backend/backend.hpp"
#include "../bake/dat.hpp"
#include "../chars/cache.hpp"
#include "../game/feed.hpp"
#include "../game/game.hpp"
#include "../host/passes.hpp"
#include "../render/gpu_crash.hpp"
#include "../zone/native.hpp"
#include "../zone/zone_gpu.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace hifi::pack
{
	namespace
	{
		namespace mw = memwatch;
		namespace opt = options;

		enum class mode : uint8_t
		{
			off,
			opening,
			on,
			failed,
			lost,
		};

		struct state
		{
			ID3D12Device *device = nullptr;
			bool started = false;         // (memwatch's thread, from the first update on)
			bool looked = false;          // (the folder looked for: at the first update)
			std::wstring folder;          // (with its slash)
			uint64_t folder_bytes = 0;    // (the files in it, at the last look)
			bool overlay_listed = false;  // (pivot.ini lists hifi-textures, at the last look)
			bool stream_started = false;  // (once a session: kept till stop)
			mode m = mode::off;
			uint64_t asked = 0;           // (opening: the open's number, stream::open's; done once posts_done reaches it)
			uint32_t epoch = 0;           // (on: the epoch owners ask in)
			std::string why;              // (failed: what the open said)
			bool session_on = false;      // (/hifi textures on: a safe start doesn't keep it off this session)
			bool lost_seen = false;
			bool zones = false, actors = false; // (the loaders' pack mode as last set)
			// (on: the index's summary)
			uint64_t entries = 0, scenery = 0, pack_bytes = 0;
			// (the quiet window: the zone it's for, when the character came in, native's slot 0 and how long it's been ready)
			uint32_t q_zone = UINT32_MAX;
			double q_in_since = -1.0;
			const void *q_ready = nullptr;
			uint32_t q_presents = 0;
			bool quiet_set = false;       // (kQuiet as the streamer has it from us)
			double delay = opt::kZoneDelayS;
			uint64_t vram = 0, budget = 0; // (the last update's)
			// (M1's reports)
			uint32_t zone = 0;
			uint64_t vram_peak = 0;        // (this zone's most video memory used, for its log line)
			uint8_t bake_logged = mw::kBakeNotChecked;
		};
		state g;

		// What owners and the menu read, any time (the game's thread; atomics so a mark moved to another thread stays right)
		std::atomic<bool> g_folder_exists { false }, g_in_use { false }, g_compare { false }, g_quiet { false }, g_fsr { false };
		std::atomic<uint32_t> g_epoch { 0 }, g_starts { 0 };
		std::atomic<uint64_t> g_io { 0 };
		std::atomic<int> g_force_quiet { -1 };
		consumer g_consumer; // (constant-initialized: set_consumer may run in another file's static init)
		// The strings status() and menu_lines() hand out, made in update (a leaf: held only to copy them)
		std::mutex g_text_mutex;
		std::vector<std::string> g_status, g_menu;
		std::string g_folder_text;

		// (each file, DXGI and streamer call: the tests' check that the menu makes none)
		void io() { g_io.fetch_add(1, std::memory_order_relaxed); }

		unsigned long long megabytes(uint64_t bytes)
		{
			return static_cast<unsigned long long>((bytes + 524288) / 1048576);
		}

		double gigabytes(uint64_t bytes)
		{
			return double(bytes) / 1073741824.0;
		}

		// 23617 as "23,617"
		std::string thousands(uint64_t n)
		{
			std::string s = std::to_string(n);
			for (int at = int(s.size()) - 3; at > 0; at -= 3)
				s.insert(size_t(at), ",");
			return s;
		}

		std::string narrow(const std::wstring &w)
		{
			std::string s;
			for (wchar_t c : w)
				s += c < 128 ? char(c) : '?';
			return s;
		}

		void log(int level, const std::string &line)
		{
			backend::log(level, ("HiFi XI: " + line).c_str());
		}

		// The consumer's calls (none set: nothing to call)
		void clear_all()
		{
			if (g_consumer.clear_all != nullptr)
				g_consumer.clear_all();
		}

		void set_mode(bool zones, bool actors)
		{
			g.zones = zones;
			g.actors = actors;
			if (g_consumer.set_mode != nullptr)
				g_consumer.set_mode(zones, actors);
		}

		void set_compare(bool on)
		{
			g_compare.store(on);
			if (g_consumer.set_compare != nullptr)
				g_consumer.set_compare(on);
		}

		void publish()
		{
			g_epoch.store(g.m == mode::on ? g.epoch : 0);
			g_in_use.store(g.m == mode::on);
			if (g.m != mode::on)
				g_fsr.store(false);
		}

		// The folder looked for (pack.idx in it), the bytes of its files, and whether pivot.ini still lists the
		// hifi-textures overlay: at the first update, /hifi textures on and reload; never per frame
		void look()
		{
			io();
			g.folder = folder_in(backend::folder());
			const bool exists = GetFileAttributesW((g.folder + L"pack.idx").c_str()) != INVALID_FILE_ATTRIBUTES;
			g.folder_bytes = 0;
			if (exists)
			{
				WIN32_FIND_DATAW f;
				const HANDLE h = FindFirstFileW((g.folder + L"*").c_str(), &f);
				if (h != INVALID_HANDLE_VALUE)
				{
					do
						if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
							g.folder_bytes += (uint64_t(f.nFileSizeHigh) << 32) | f.nFileSizeLow;
					while (FindNextFileW(h, &f));
					FindClose(h);
				}
			}
			g.overlay_listed = false;
			for (const std::wstring &name : bake::overlays())
				g.overlay_listed |= CompareStringOrdinal(name.c_str(), -1, L"hifi-textures", -1, TRUE) == CSTR_EQUAL;
			g_folder_exists.store(exists);
			std::lock_guard<std::mutex> lock(g_text_mutex);
			g_folder_text = narrow(g.folder);
		}

		bool pack_option() { return game::option_on("textures", "pack", opt::kPackByDefault); }
		bool safe_held() { return backend::safe_start() && !g.session_on; }

		// Whether the pack should be in use now, and if not why (for the log)
		bool want(const char *&why)
		{
			why = !pack_option() ? "turned off" : !g_folder_exists.load() ? "no pack folder" : safe_held() ? "the graphics card stopped last session" :
				g.device == nullptr ? "no graphics device" : gpu_crash::lost() ? "the graphics card stopped" : nullptr;
			return why == nullptr;
		}

		// The loaders' pack mode from the hidden keys (while in use)
		void follow_mode()
		{
			const bool zones = game::option_on("textures", "zone", opt::kZoneByDefault), actors = game::option_on("textures", "actors", opt::kActorsByDefault);
			if (zones != g.zones || actors != g.actors)
				set_mode(zones, actors);
		}

		// off -> opening: the streamer started (once a session), the pack posted to its worker
		void begin_open()
		{
			if (!g.stream_started)
			{
				const uint32_t ring = uint32_t(std::clamp(game::option_number("textures", "ring_mb", opt::kRingMb), float(stream::kRingMinMb), float(stream::kRingMaxMb)));
				io();
				if (!stream::start(g.device, ring))
				{
					// (already running: a start this module didn't make; kept, on this device)
					io();
					stream::set_device(g.device);
				}
				g.stream_started = true;
				g.quiet_set = false; // (a start clears the pause bits)
				g_starts.fetch_add(1);
				log(0, "texture pack: the streamer started (upload ring " + std::to_string(ring) + " MB, kept for the session)");
			}
			// (nothing should hold a ticket here; after a failure or a device change, belt and braces)
			clear_all();
			set_mode(false, false);
			io();
			g.asked = stream::open(g.folder);
			io();
			stream::resume(stream::kOff);
			if (g_quiet.load() && !g.quiet_set)
			{
				io();
				stream::pause(stream::kQuiet);
				g.quiet_set = true;
			}
			g.m = mode::opening;
			publish();
		}

		// on, opening or failed -> off: every owner dropped, the loaders out of pack mode, the pack closed, the streamer
		// paused and kept
		void turn_off(const char *why)
		{
			const mode was = g.m;
			clear_all();
			set_mode(false, false);
			set_compare(false);
			if (g.stream_started)
			{
				io();
				stream::close();
				io();
				stream::pause(stream::kOff);
			}
			g.m = mode::off;
			publish();
			if (was != mode::failed)
				log(0, std::string("texture pack: off (") + why + ")");
		}

		// opening -> on or failed, once the worker has done this open: its own post, not any epoch raised (an open still
		// loading when another was asked, by a reload or off and on, raises the epoch first; on in that epoch, every
		// request after the newer open's read gone)
		void check_open()
		{
			io();
			if (stream::posts_done() < g.asked)
				return;
			io();
			const uint32_t e = stream::epoch();
			io();
			const std::shared_ptr<const index> idx = stream::current();
			if (!idx)
			{
				io();
				g.why = stream::open_status();
				const std::string prefix = "pack can't be used: ";
				if (g.why.rfind(prefix, 0) == 0)
					g.why = g.why.substr(prefix.size());
				if (g.why.empty())
					g.why = "it didn't open";
				g.m = mode::failed;
				io();
				stream::pause(stream::kOff);
				set_mode(false, false);
				publish();
				log(1, "texture pack: " + narrow(g.folder) + " can't be used: " + g.why);
				return;
			}
			g.epoch = e;
			g.entries = idx->entries.size();
			g.scenery = idx->by_source[kSourceZone];
			g.pack_bytes = 0;
			for (uint64_t b : idx->file_bytes)
				g.pack_bytes += b;
			g.m = mode::on;
			follow_mode();
			set_compare(g_compare.load());
			publish();
			char line[400];
			std::snprintf(line, sizeof(line), "texture pack: %s: %s textures (scenery %s, the rest %s), %.1f GB, opened (epoch %u; zones %s, model actors %s)",
				narrow(g.folder).c_str(), thousands(g.entries).c_str(), thousands(g.scenery).c_str(), thousands(g.entries - g.scenery).c_str(),
				gigabytes(g.pack_bytes), g.epoch, g.zones ? "on" : "off", g.actors ? "on" : "off");
			log(0, line);
			if (!idx->status.empty())
				log(1, "texture pack: " + idx->status);
		}

		// The states moved as the option, the folder, safe start and the device say
		void step()
		{
			const char *why = nullptr;
			const bool w = want(why);
			switch (g.m)
			{
			case mode::off:
				if (w)
					begin_open();
				break;
			case mode::opening:
				if (!w)
					turn_off(why);
				else
					check_open();
				break;
			case mode::on:
				if (!w)
					turn_off(why);
				else
					follow_mode();
				break;
			case mode::failed:
				if (!w)
					turn_off(why); // (its files closed; turned on again, it's opened again)
				break;
			case mode::lost:
				break;
			}
		}

		// /hifi textures reload: the folder looked for again, every owner dropped, the pack opened anew (the zone in view
		// keeps its game textures, its slots spent, until the next zone)
		void reload()
		{
			look();
			if (!g.stream_started || g.m == mode::lost || g.m == mode::off)
			{
				step();
				return;
			}
			const char *why = nullptr;
			if (!want(why))
			{
				turn_off(why);
				return;
			}
			log(0, "texture pack: reloaded");
			begin_open();
		}

		// The device removed (present): the streamer told, every owner dropped; nothing more this session
		void follow_lost()
		{
			if (g.lost_seen || !gpu_crash::lost())
				return;
			g.lost_seen = true;
			if (g.stream_started)
			{
				io();
				stream::device_lost();
			}
			clear_all();
			set_mode(false, false);
			set_compare(false);
			const bool was_off = g.m == mode::off;
			g.m = mode::lost;
			publish();
			if (!was_off || g.stream_started)
				log(1, "texture pack: stopped, the graphics card stopped (device removed)");
		}

		// The quiet window's inputs this present, kQuiet as it says (only when it changes)
		void follow_quiet(double now)
		{
			const uint32_t zone = game::feed::zone();
			if (zone != g.q_zone)
			{
				g.q_zone = zone;
				g.q_in_since = -1.0;
				g.q_ready = nullptr;
				g.q_presents = 0;
			}
			if (!game::feed::in())
				g.q_in_since = -1.0;
			else if (g.q_in_since < 0.0)
				g.q_in_since = now;
			const void *const ready = zone::native::ready_zone(0);
			if (ready == nullptr || ready != g.q_ready)
			{
				g.q_ready = ready;
				g.q_presents = ready != nullptr ? 1 : 0;
			}
			else if (g.q_presents < 1000000)
				g.q_presents++;
			quiet_inputs q;
			q.now = now;
			q.in_since = g.q_in_since;
			q.delay = g.delay;
			q.ready_presents = g.q_presents;
			q.zone_wanted = zone != 0;
			q.vram_usage = g.vram;
			q.vram_budget = g.budget;
			const int forced = g_force_quiet.load();
			const bool quiet = forced >= 0 ? forced != 0 : quiet_window(q);
			g_quiet.store(quiet);
			if (g.stream_started && quiet != g.quiet_set)
			{
				io();
				if (quiet)
					stream::pause(stream::kQuiet);
				else
					stream::resume(stream::kQuiet);
				g.quiet_set = quiet;
			}
		}

		void present(double now, bool)
		{
			// A zone change: the last zone's lowest free memory into the log, once, then the marks started again
			if (const uint32_t zone = game::feed::zone(); zone != g.zone)
			{
				const mw::values v = mw::read();
				if (g.zone != 0 && v.status_at != 0)
				{
					char line[300];
					std::snprintf(line, sizeof(line), "HiFi XI: memory in zone %u: at the lowest %llu MB of address space free, largest free block %llu MB; video memory up to %llu MB of %llu MB",
						g.zone, megabytes(v.low_avail_virtual), megabytes(v.low_largest_free), megabytes(g.vram_peak), megabytes(v.vram_budget));
					backend::log(0, line);
				}
				g.zone = zone;
				g.vram_peak = 0;
				mw::reset_low_water();
			}
			follow_lost();
			if (g.m == mode::lost)
				return;
			follow_quiet(now);
			if (!g.stream_started)
				return;
			// (a new present's bytes for the streamer's pacing, then the owners: requests, ready tickets read, views written)
			io();
			stream::on_present();
			if (g.m == mode::on && g_consumer.frame != nullptr)
				g_consumer.frame(now, g.epoch);
		}

		// The status and the menu's lines, from what the module knows now (the streamer's counts asked once)
		void make_lines(const mw::values &v, uint64_t chars, uint64_t zone0, uint64_t zone1)
		{
			std::vector<std::string> status, menu, area;
			if (g.m == mode::on && g_consumer.lines != nullptr)
				g_consumer.lines(area);
			stream::counts c;
			if (g.stream_started)
			{
				io();
				c = stream::get_counts();
			}
			char line[500];
			// (what it is doing)
			const std::string folder = narrow(g.folder);
			std::string found;
			if (g.m == mode::on)
			{
				std::snprintf(line, sizeof(line), "Found: %s textures (scenery %s, the rest %s), %.1f GB", thousands(g.entries).c_str(),
					thousands(g.scenery).c_str(), thousands(g.entries - g.scenery).c_str(), gigabytes(g.pack_bytes));
				found = line;
			}
			switch (g.m)
			{
			case mode::on:
				status.push_back("textures: on, " + folder + ": " + found.substr(7) + " (epoch " + std::to_string(g.epoch) + "; zones " +
					(g.zones ? "on" : "off") + ", model actors " + (g.actors ? "on" : "off") + ")");
				menu.push_back(found);
				break;
			case mode::opening:
				status.push_back("textures: opening " + folder + "...");
				menu.push_back("Opening the texture pack...");
				break;
			case mode::failed:
				status.push_back("textures: " + folder + " can't be used: " + g.why + " (/hifi textures reload once it's put right)");
				menu.push_back("Texture pack can't be used: " + g.why);
				break;
			case mode::lost:
				status.push_back("textures: stopped, the graphics card stopped (restart the game)");
				menu.push_back("Stopped: the graphics card stopped. Restart the game to use it again.");
				break;
			case mode::off:
				if (!g_folder_exists.load())
				{
					status.push_back("textures: no texture pack on this PC (" + folder + ")");
					break;
				}
				if (!pack_option())
				{
					status.push_back("textures: off (" + folder + "; /hifi textures on)");
					menu.push_back("Off: turn it on above.");
				}
				else if (safe_held())
				{
					status.push_back("textures: paused after a graphics-card stop (/hifi textures on to use it this session)");
					menu.push_back("Paused after a graphics-card stop: /hifi textures on uses it this session.");
				}
				else
				{
					status.push_back("textures: on, waiting for the game's graphics device");
					menu.push_back("Waiting for the game's graphics device.");
				}
				break;
			}
			if (g.m == mode::on)
			{
				for (const std::string &l : area)
				{
					status.push_back("textures: " + l);
					menu.push_back(l);
				}
				if (g_compare.load())
				{
					status.push_back("textures: comparing: the game's own textures drawn (/hifi textures compare again)");
					menu.push_back("Comparing: the game's own textures are drawn (/hifi textures compare again).");
				}
				if (c.pause & stream::kVideoMemory)
					menu.push_back("Paused: the graphics card is nearly full.");
				if (c.pause & stream::kAddressSpace)
					menu.push_back("Paused: the game is low on memory.");
				if (g_quiet.load())
					menu.push_back("Waiting for the area to settle.");
				std::snprintf(line, sizeof(line), "On the graphics card: %llu MB for the pack", megabytes(c.resident_bytes));
				std::string card = line;
				if (c.held != 0)
				{
					// (textures gone tickets still hold since a reload: a drop forgotten costs video memory, never a crash)
					std::snprintf(line, sizeof(line), " (and %llu MB kept for areas that haven't let go of them)", megabytes(c.held_bytes));
					card += line;
				}
				if (g.budget != 0)
				{
					std::snprintf(line, sizeof(line), "; %s of %s MB in use", thousands(megabytes(g.vram)).c_str(), thousands(megabytes(g.budget)).c_str());
					card += line;
				}
				menu.push_back(card + ".");
			}
			if (g.stream_started)
			{
				io();
				for (const std::string &l : stream::status())
					status.push_back("textures: " + l);
			}
			if (v.status_at != 0)
			{
				std::snprintf(line, sizeof(line), "Game memory: %.1f of %.1f GB in use", gigabytes(v.total_virtual - std::min(v.total_virtual, v.avail_virtual)),
					gigabytes(v.total_virtual));
				std::string game_memory = line;
				if (v.walk_at != 0)
				{
					std::snprintf(line, sizeof(line), " (largest free block %llu MB)", megabytes(v.largest_free));
					game_memory += line;
				}
				menu.push_back(game_memory + ".");
			}
			if (g.overlay_listed)
				menu.push_back("The hifi-textures XIPivot overlay is still installed, so the game also loads its bigger files. Remove it to save the "
					"game's memory.");
			std::snprintf(line, sizeof(line), "To remove the texture pack: turn it off, then delete the hifixi-pack folder (%.1f GB).", gigabytes(g.folder_bytes));
			menu.push_back(line);
			// (M1's: the game's memory as the pack needs it)
			if (v.status_at != 0)
			{
				std::snprintf(line, sizeof(line), "textures: address space %llu MB of %llu MB free, largest free block %llu MB (walked in %.1f ms); this zone's lowest %llu MB free, %llu MB block",
					megabytes(v.avail_virtual), megabytes(v.total_virtual), megabytes(v.largest_free), v.walk_ms, megabytes(v.low_avail_virtual), megabytes(v.low_largest_free));
				status.push_back(line);
			}
			std::snprintf(line, sizeof(line), "textures: video memory %llu MB of %llu MB (this zone up to %llu MB); characters %llu MB, this zone %llu MB",
				megabytes(g.vram), megabytes(g.budget), megabytes(g.vram_peak), megabytes(chars), megabytes(zone0));
			status.push_back(line);
			if (zone1 != 0)
				status.back() += " and its second map " + std::to_string(megabytes(zone1)) + " MB";
			status.push_back(std::string("textures: a texture bake running (bakeall.py, realesrgan-ncnn-vulkan): ") +
				(v.bake == mw::kBakeYes ? "yes" : v.bake == mw::kBakeNo ? "no" : v.bake == mw::kBakeUnknown ? "can't tell" : "not checked yet"));
			std::lock_guard<std::mutex> lock(g_text_mutex);
			g_status = std::move(status);
			g_menu = std::move(menu);
		}

		void update(double)
		{
			// (the probes on their own thread from the first update on: nothing of them on the game's)
			if (!g.started)
			{
				mw::start();
				g.started = true;
			}
			if (!g.looked)
			{
				look();
				g.looked = true;
			}
			io();
			gpu_crash::video_memory(g.device, g.vram, g.budget); // (its adapter kept: cheap every half second)
			g.vram_peak = std::max(g.vram_peak, g.vram);
			const uint64_t chars = chars::cache::get_counts().gpu_bytes;
			const zone::gpu_zone *const z0 = zone::native::ready_zone(0), *const z1 = zone::native::ready_zone(1);
			const uint64_t zone0 = z0 != nullptr ? z0->bytes() : 0, zone1 = z1 != nullptr ? z1->bytes() : 0;
			mw::set_gpu(g.vram, g.budget, chars, zone0 + zone1);
			const mw::values v = mw::read();
			// (the bake's state in the log as it changes)
			if (v.bake != g.bake_logged && v.bake != mw::kBakeNotChecked)
			{
				if (v.bake == mw::kBakeYes)
					backend::log(1, "HiFi XI: a texture bake is running (bakeall.py or realesrgan-ncnn-vulkan): crash and hang reports say so");
				else if (v.bake == mw::kBakeNo && g.bake_logged != mw::kBakeNotChecked)
					backend::log(0, "HiFi XI: the texture bake has ended");
				else if (v.bake == mw::kBakeUnknown)
					backend::log(0, "HiFi XI: whether a texture bake is running can't be told (a python.exe's command line couldn't be read)");
				g.bake_logged = v.bake;
			}
			g.delay = std::max(0.0, double(game::option_number("textures", "zone_delay_s", opt::kZoneDelayS)));
			step();
			if (g.stream_started && g.m != mode::lost)
			{
				// (the streamer's limits: one input, made here; the worker never asks DXGI or memwatch itself)
				stream::limits l;
				l.vram_usage = g.vram;
				l.vram_budget = g.budget;
				l.budget_cap = uint64_t(std::max(0.0f, game::option_number("textures", "budget_mb", opt::kBudgetMb))) << 20;
				l.avail_virtual = v.status_at != 0 ? v.avail_virtual : 0;
				l.largest_free = v.walk_at != 0 ? v.largest_free : 0;
				l.per_present = uint64_t(std::max(0.0f, game::option_number("textures", "per_present_mb", opt::kPerPresentMb))) << 20;
				io();
				stream::set_limits(l);
				// (what the worker couldn't make, each once: the streamer logs nothing itself)
				std::vector<std::string> lines;
				io();
				stream::take_log(lines);
				for (const std::string &line : lines)
					log(1, line);
			}
			g_fsr.store(g.m == mode::on && game::option_on("textures", "fsr_bias", opt::kFsrBiasByDefault));
			make_lines(v, chars, zone0, zone1);
		}

		std::vector<std::string> status()
		{
			std::lock_guard<std::mutex> lock(g_text_mutex);
			return g_status;
		}

		bool stop(std::string &)
		{
			// (every owner dropped before the streamer goes: then stop() joins its worker, under 100 ms, the resident textures
			// to the retire list, which the plugin's retire::release_all deals with last)
			clear_all();
			set_mode(false, false);
			set_compare(false);
			if (g.stream_started)
			{
				io();
				stream::stop();
			}
			mw::stop();
			// (the plugin loaded again finds the DLL as it left it: everything from the start, the streamer started again at
			// the next enable, one more ring's address space, once)
			g = state {};
			publish();
			g_quiet.store(false);
			g_folder_exists.store(false);
			{
				std::lock_guard<std::mutex> lock(g_text_mutex);
				g_status.clear();
				g_menu.clear();
				g_folder_text.clear();
			}
			return true;
		}

		bool command(const std::vector<std::string> &args, std::vector<std::string> &out)
		{
			const std::string what = args.size() >= 2 ? args[1] : "status";
			if (what == "on")
			{
				game::set_option_on("textures", "pack", true);
				const bool lifted = safe_held();
				g.session_on = true; // (a safe start doesn't keep it off this session)
				look();
				step();
				if (!g_folder_exists.load())
					out.push_back("textures: no texture pack on this PC (" + narrow(g.folder) + ")");
				else
				{
					out.push_back(std::string("textures: on (saved)") + (lifted ? "; used this session despite the graphics card's stop last session" : "") +
						": scenery, ships and furniture switch at the next area you enter");
					if (lifted)
						log(0, "texture pack: on for this session despite last session's GPU stop (/hifi textures on)");
				}
				return true;
			}
			if (what == "off")
			{
				game::set_option_on("textures", "pack", false);
				step();
				out.push_back("textures: off (saved): the game's own textures are drawn again");
				return true;
			}
			if (what == "compare")
			{
				set_compare(!g_compare.load());
				out.push_back(g_compare.load() ? "textures: comparing: the game's own textures are drawn where the pack's are ready (/hifi textures compare again)" :
					"textures: the pack's textures drawn again");
				return true;
			}
			if (what == "reload")
			{
				reload();
				out.push_back(g.m == mode::opening ? "textures: opening the pack again (the area in view keeps the game's own textures until the next)" :
					g_folder_exists.load() ? "textures: looked for again (off: nothing opened)" : "textures: no texture pack on this PC (" + narrow(g.folder) + ")");
				return true;
			}
			if (what != "status")
			{
				out.push_back("textures: /hifi textures [status|on|off|compare|reload]");
				return true;
			}
			out = status();
			if (out.empty())
				out.push_back("textures: not looked at yet");
			return true;
		}

		void device(ID3D12Device *device)
		{
			if (device == g.device)
				return;
			g.device = device;
			// (native and actordata let go of the old device's zones in their frame, before this: their owners dropped by
			// the hook. Then whatever's left dropped, and the streamer moved: its epoch raised at once)
			if (!g.stream_started || g.m == mode::lost)
				return;
			clear_all();
			io();
			stream::set_device(device);
			// (opening: nothing asked again. The open posted is done before or after the change, and check_open reads the
			// epoch once it is, past the one the change raised)
			if (g.m == mode::on)
			{
				io();
				g.epoch = stream::epoch();
				publish();
			}
		}

		const passes::registrar module_entry(passes::module { "textures", &present, &update, &status, &stop, &command, &device });
	}

	bool folder_exists()
	{
		return g_folder_exists.load();
	}

	std::vector<std::string> menu_lines()
	{
		std::lock_guard<std::mutex> lock(g_text_mutex);
		return g_menu;
	}

	std::string folder_text()
	{
		std::lock_guard<std::mutex> lock(g_text_mutex);
		return g_folder_text;
	}

	bool in_use() { return g_in_use.load(); }
	uint32_t epoch_in_use() { return g_epoch.load(); }
	bool comparing() { return g_compare.load(); }
	bool quiet() { return g_quiet.load(); }
	bool fsr_bias_on() { return g_fsr.load(); }

	float fsr_bias(float ratio)
	{
		if (!(ratio > 0.0f) || ratio >= 1.0f)
			return 0.0f;
		return std::clamp(std::log2(ratio), -1.0f, 0.0f);
	}

	void set_consumer(const consumer &c)
	{
		g_consumer = c;
	}

	bool quiet_window(const quiet_inputs &q)
	{
		if (q.in_since < 0.0 || q.now - q.in_since < q.delay)
			return true;
		if (!q.zone_wanted || q.ready_presents >= kQuietPresents)
			return false;
		return q.vram_budget != 0 && q.vram_usage * 100 >= q.vram_budget * kQuietPercent;
	}

	namespace test
	{
		uint64_t io_calls() { return g_io.load(); }
		uint32_t stream_starts() { return g_starts.load(); }
		void force_quiet(int v) { g_force_quiet.store(v); }
	}
}
