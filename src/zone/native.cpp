/*
 * The native zone renderer's per-zone data in the game (native.hpp). One loader for each of the renderer's two map
 * slots (the zone, and a second map such as a building's interior), each one load at a time: a thread reads the map's
 * DATs (zonemesh::load: 3-35 ms offline), then the game's thread uploads them at its next present (gpu_zone::upload on
 * our copy queue) and polls the copy's fence at the presents after. A map left is kept for kRetirePresents more
 * presents before its resources are let go.
 */

#include <windows.h>
#include "native.hpp"
#include "zone_gpu.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace hifi::zone::native
{
	namespace
	{
		namespace zm = bake::zonemesh;
		constexpr uint32_t kRetirePresents = 240;

		struct retired
		{
			std::unique_ptr<gpu_zone> z;
			uint64_t at = 0;
		};

		// One map slot's data
		struct slot
		{
			uint32_t asked = 0; // (the map the last present was for)
			// The load in flight: its map, its thread, what it read
			uint32_t loading = 0;
			std::thread thread;
			std::atomic<bool> loaded { false };
			zm::zone data;
			bake::result result;
			bool read_ok = false;
			double read_ms = 0.0;
			double current_read_ms = 0.0; // (the uploaded map's)
			// The map uploaded (or uploading), and why the last one failed
			uint32_t map = 0;
			std::unique_ptr<gpu_zone> current;
			std::string why;
			uint32_t failed_map = 0;
		};

		struct state
		{
			ID3D12Device *device = nullptr;
			ID3D12CommandQueue *copy = nullptr;
			uint64_t presents = 0;
			slot slots[2];
			std::vector<retired> old;
		};
		state g;

		void join(slot &s)
		{
			if (s.thread.joinable())
				s.thread.join();
		}

		void retire(slot &s)
		{
			if (s.current != nullptr)
				g.old.push_back({ std::move(s.current), g.presents });
			s.map = 0;
		}

		void forget_device()
		{
			for (slot &s : g.slots)
			{
				join(s);
				s.loading = 0;
				s.loaded = false;
				s.current.reset();
				s.map = 0;
			}
			g.old.clear();
			if (g.copy != nullptr)
				g.copy->Release();
			g.copy = nullptr;
			g.device = nullptr;
		}

		void follow(slot &s, ID3D12Device *device, uint32_t map)
		{
			s.asked = map;
			if (map == 0)
			{
				if (s.loading == 0)
					retire(s);
				return;
			}
			if (s.current != nullptr)
				s.current->ready();

			// A load done: uploaded now (unless the map has changed since: then it's dropped)
			if (s.loading != 0 && s.loaded.load(std::memory_order_acquire))
			{
				join(s);
				const uint32_t m = s.loading;
				s.loading = 0;
				s.loaded = false;
				if (m == map)
				{
					if (!s.read_ok)
					{
						s.why = s.result.none ? "no meshes (" + s.result.why + ")" : s.result.why;
						s.failed_map = m;
					}
					else
					{
						retire(s);
						auto next = std::make_unique<gpu_zone>();
						std::string why;
						if (next->upload(device, g.copy, std::move(s.data), why))
						{
							s.current = std::move(next);
							s.map = m;
							s.current_read_ms = s.read_ms;
							s.why.clear();
							s.failed_map = 0;
						}
						else
						{
							s.why = why;
							s.failed_map = m;
						}
					}
				}
				s.data = zm::zone();
			}

			// A new map: its load started (one at a time; not again for a map that just failed)
			if (map != s.map && s.loading == 0 && map != s.failed_map)
			{
				s.loading = map;
				s.loaded = false;
				slot *const p = &s;
				s.thread = std::thread([p, map] {
					const auto t0 = std::chrono::steady_clock::now();
					p->read_ok = zm::load(map, p->data, p->result);
					p->read_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
					p->loaded.store(true, std::memory_order_release);
				});
			}
		}
	}

	void frame(ID3D12Device *device, uint32_t zone, uint32_t second, bool wanted)
	{
		g.presents++;
		for (size_t i = 0; i < g.old.size();)
			if (g.presents - g.old[i].at > kRetirePresents)
				g.old.erase(g.old.begin() + ptrdiff_t(i));
			else
				++i;
		if (device != g.device)
			forget_device();
		if (!wanted || device == nullptr)
		{
			for (slot &s : g.slots)
				follow(s, nullptr, 0);
			return;
		}
		g.device = device;
		if (g.copy == nullptr)
		{
			D3D12_COMMAND_QUEUE_DESC qd = {};
			qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
			if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.copy))))
			{
				g.copy = nullptr;
				g.slots[0].why = "no copy queue could be made";
				return;
			}
		}
		follow(g.slots[0], device, zone);
		// (the second map only while there's a zone: it's a part of one)
		follow(g.slots[1], device, zone != 0 ? second : 0);
	}

	gpu_zone *ready_zone(uint32_t i)
	{
		// (only the map asked for: not the last one while the next is read, or when it couldn't be)
		if (i >= 2)
			return nullptr;
		const slot &s = g.slots[i];
		return s.current != nullptr && s.map == s.asked && s.current->ready() ? s.current.get() : nullptr;
	}

	std::vector<std::string> status()
	{
		std::vector<std::string> lines;
		char line[320];
		for (uint32_t i = 0; i < 2; ++i)
		{
			const slot &s = g.slots[i];
			const char *const what = i == 0 ? "zone" : "second map";
			if (s.current != nullptr)
			{
				const zm::zone &z = s.current->data();
				if (s.current->ready())
					std::snprintf(line, sizeof(line), "native zone data: %s %u on the GPU: %zu models, %zu pieces, %zu textures, %zu normal maps, %zu placements; %.1f MB; read in %.0f ms, copied in %.0f ms",
						what, s.map, z.models.size(), z.pieces.size(), z.textures.size(), z.normal_maps.size(), z.placements.size(), double(s.current->bytes()) / 1048576.0,
						s.current_read_ms, s.current->upload_ms());
				else
					std::snprintf(line, sizeof(line), "native zone data: %s %u being copied to the GPU (%.1f MB)", what, s.map, double(s.current->bytes()) / 1048576.0);
				lines.push_back(line);
			}
			else if (i == 0 && g.device == nullptr)
				lines.push_back("native zone data: not loaded (off: /hifi zonedata on reads each zone's meshes, textures and normal maps onto the GPU)");
			if (s.loading != 0)
				lines.push_back(std::string("native zone data: reading ") + what + " " + std::to_string(s.loading));
			if (!s.why.empty())
				lines.push_back(std::string("native zone data: ") + what + " " + std::to_string(s.failed_map) + " not loaded: " + s.why);
		}
		if (!g.old.empty())
			lines.push_back("native zone data: " + std::to_string(g.old.size()) + " earlier map(s) let go shortly");
		return lines;
	}

	void stop()
	{
		forget_device();
		for (slot &s : g.slots)
		{
			s.why.clear();
			s.failed_map = 0;
		}
	}
}
