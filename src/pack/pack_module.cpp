/*
 * The texture pack as an own-frame module (src/host/passes.hpp), "textures". For now (M1, docs/texture-pack-plan.md)
 * it only measures and reports: the game's address space and its largest free block (memwatch, on its own thread),
 * the video memory, the characters' and the zone's bytes on the GPU, each zone's lowest free memory (logged once as the
 * next zone comes), and whether a texture bake runs. No pack is read yet, no setting; /hifi textures [status].
 */

#include "memwatch.hpp"
#include "../backend/backend.hpp"
#include "../chars/cache.hpp"
#include "../game/feed.hpp"
#include "../host/passes.hpp"
#include "../render/gpu_crash.hpp"
#include "../zone/native.hpp"
#include "../zone/zone_gpu.hpp"
#include <algorithm>
#include <cstdio>

namespace hifi::pack
{
	namespace
	{
		namespace mw = memwatch;

		struct state
		{
			ID3D12Device *device = nullptr;
			bool started = false;
			uint32_t zone = 0;
			uint64_t vram_peak = 0; // (this zone's most video memory used, for its log line)
			uint8_t bake_logged = mw::kBakeNotChecked;
			std::vector<std::string> lines;
		};
		state g;

		unsigned long long megabytes(uint64_t bytes)
		{
			return static_cast<unsigned long long>((bytes + 524288) / 1048576);
		}

		void present(double, bool)
		{
			// A zone change: the last zone's lowest free memory into the log, once, then the marks started again
			const uint32_t zone = game::feed::zone();
			if (zone == g.zone)
				return;
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

		void update(double)
		{
			// (the probes on their own thread from the first update on: nothing of them on the game's)
			if (!g.started)
			{
				mw::start();
				g.started = true;
			}
			uint64_t vram = 0, budget = 0;
			gpu_crash::video_memory(g.device, vram, budget); // (its adapter kept: cheap every half second)
			g.vram_peak = std::max(g.vram_peak, vram);
			const uint64_t chars = chars::cache::get_counts().gpu_bytes;
			const zone::gpu_zone *const z0 = zone::native::ready_zone(0), *const z1 = zone::native::ready_zone(1);
			const uint64_t zone0 = z0 != nullptr ? z0->bytes() : 0, zone1 = z1 != nullptr ? z1->bytes() : 0;
			mw::set_gpu(vram, budget, chars, zone0 + zone1);
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
			// The status lines, made here: status() only hands them out
			std::vector<std::string> lines;
			lines.push_back("textures: no texture pack read yet (this build measures the game's memory only)");
			char line[400];
			if (v.status_at != 0)
			{
				std::snprintf(line, sizeof(line), "textures: address space %llu MB of %llu MB free, largest free block %llu MB (walked in %.1f ms); this zone's lowest %llu MB free, %llu MB block",
					megabytes(v.avail_virtual), megabytes(v.total_virtual), megabytes(v.largest_free), v.walk_ms, megabytes(v.low_avail_virtual), megabytes(v.low_largest_free));
				lines.push_back(line);
			}
			std::snprintf(line, sizeof(line), "textures: video memory %llu MB of %llu MB (this zone up to %llu MB); characters %llu MB, this zone %llu MB",
				megabytes(vram), megabytes(budget), megabytes(g.vram_peak), megabytes(chars), megabytes(zone0));
			lines.push_back(line);
			if (zone1 != 0)
				lines.back() += " and its second map " + std::to_string(megabytes(zone1)) + " MB";
			lines.push_back(std::string("textures: a texture bake running (bakeall.py, realesrgan-ncnn-vulkan): ") +
				(v.bake == mw::kBakeYes ? "yes" : v.bake == mw::kBakeNo ? "no" : v.bake == mw::kBakeUnknown ? "can't tell" : "not checked yet"));
			g.lines = std::move(lines);
		}

		std::vector<std::string> status()
		{
			return g.lines;
		}

		bool stop(std::string &)
		{
			mw::stop();
			g.started = false;
			return true;
		}

		bool command(const std::vector<std::string> &args, std::vector<std::string> &out)
		{
			if (args.size() >= 2 && args[1] != "status")
				return false;
			out = status();
			if (out.empty())
				out.push_back("textures: not measured yet");
			return true;
		}

		void device(ID3D12Device *device)
		{
			g.device = device;
		}

		const passes::registrar module_entry(passes::module { "textures", &present, &update, &status, &stop, &command, &device });
	}
}
