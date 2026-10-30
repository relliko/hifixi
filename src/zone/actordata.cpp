/*
 * The model actors' data on the GPU (actordata.hpp). One DAT read at a time on a thread (zonemesh::load_file: a ship's or
 * a furniture DAT is small), uploaded at the next present on our copy queue, its fence polled at the presents after.
 * The DATs asked for wait in a short queue (a Mog House full of furniture asks for a dozen at once).
 */

#include <windows.h>
#include "actordata.hpp"
#include "zone_gpu.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>

namespace hifi::zone::actordata
{
	namespace
	{
		namespace zm = bake::zonemesh;
		constexpr uint64_t kRetirePresents = 240;
		constexpr uint64_t kUnusedPresents = 3600; // (half a minute to a minute: a Mog House left and come back to keeps its furniture)
		constexpr size_t kMaxFiles = 96;           // (DATs held at once; asked for past that: the game draws those)
		constexpr size_t kMaxQueued = 64;

		struct file
		{
			std::unique_ptr<gpu_zone> z;
			uint64_t used_at = 0; // (the present it was last asked for)
			std::string why;      // (why it couldn't be read: then never asked again this session)
			double read_ms = 0.0;
		};

		struct retired
		{
			std::unique_ptr<gpu_zone> z;
			uint64_t at = 0;
		};

		struct state
		{
			ID3D12Device *device = nullptr;
			ID3D12CommandQueue *copy = nullptr;
			bool wanted = false;
			uint64_t presents = 0;
			std::unordered_map<uint32_t, file> files;
			std::deque<uint32_t> queue;
			// The load in flight: its file, its thread, what it read
			uint32_t loading = 0;
			std::thread thread;
			std::atomic<bool> loaded { false };
			zm::zone data;
			bake::result result;
			bool read_ok = false;
			double read_ms = 0.0;
			std::vector<retired> old;
			uint64_t asked = 0, uploaded = 0, failed = 0, let_go = 0;
		};
		state g;

		void join()
		{
			if (g.thread.joinable())
				g.thread.join();
		}

		void forget_all()
		{
			join();
			g.loading = 0;
			g.loaded = false;
			g.data = zm::zone();
			g.files.clear();
			g.queue.clear();
			g.old.clear();
		}

		void forget_device()
		{
			forget_all();
			if (g.copy != nullptr)
				g.copy->Release();
			g.copy = nullptr;
			g.device = nullptr;
		}
	}

	void frame(ID3D12Device *device, bool wanted)
	{
		g.presents++;
		for (size_t i = 0; i < g.old.size();)
			if (g.presents - g.old[i].at > kRetirePresents)
				g.old.erase(g.old.begin() + ptrdiff_t(i));
			else
				++i;
		if (device != g.device)
			forget_device();
		g.wanted = wanted && device != nullptr;
		if (!g.wanted)
		{
			if (!g.files.empty() || g.loading != 0)
			{
				join();
				g.loading = 0;
				g.loaded = false;
				for (auto &f : g.files)
					if (f.second.z != nullptr)
						g.old.push_back({ std::move(f.second.z), g.presents });
				g.files.clear();
				g.queue.clear();
			}
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
				return;
			}
		}

		// DATs no model actor has drawn from lately let go
		for (auto i = g.files.begin(); i != g.files.end();)
			if (i->second.z != nullptr && g.presents - i->second.used_at > kUnusedPresents)
			{
				g.old.push_back({ std::move(i->second.z), g.presents });
				g.let_go++;
				i = g.files.erase(i);
			}
			else
				++i;

		// A load done: uploaded now
		if (g.loading != 0 && g.loaded.load(std::memory_order_acquire))
		{
			join();
			const uint32_t id = g.loading;
			g.loading = 0;
			g.loaded = false;
			file &f = g.files[id];
			f.read_ms = g.read_ms;
			if (!g.read_ok)
			{
				f.why = g.result.none ? "no meshes (" + g.result.why + ")" : g.result.why;
				g.failed++;
			}
			else
			{
				auto next = std::make_unique<gpu_zone>();
				std::string why;
				if (next->upload(device, g.copy, std::move(g.data), why))
				{
					f.z = std::move(next);
					g.uploaded++;
				}
				else
				{
					f.why = why;
					g.failed++;
				}
			}
			g.data = zm::zone();
		}
		for (auto &f : g.files)
			if (f.second.z != nullptr)
				f.second.z->ready();

		// The next one asked for
		if (g.loading == 0 && !g.queue.empty())
		{
			const uint32_t id = g.queue.front();
			g.queue.pop_front();
			g.loading = id;
			g.loaded = false;
			g.thread = std::thread([id] {
				const auto t0 = std::chrono::steady_clock::now();
				g.read_ok = zm::load_file(id, g.data, g.result);
				g.read_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				g.loaded.store(true, std::memory_order_release);
			});
		}
	}

	gpu_zone *find(uint32_t id)
	{
		if (!g.wanted || id == 0)
			return nullptr;
		const auto known = g.files.find(id);
		if (known != g.files.end())
		{
			file &f = known->second;
			if (f.z == nullptr)
				return nullptr; // (being read, or couldn't be)
			f.used_at = g.presents;
			return f.z->ready() ? f.z.get() : nullptr;
		}
		// (asked for: read at a later present; not while too many are held or waiting)
		if (g.files.size() >= kMaxFiles || g.queue.size() >= kMaxQueued || id == g.loading ||
			std::find(g.queue.begin(), g.queue.end(), id) != g.queue.end())
			return nullptr;
		g.queue.push_back(id);
		g.asked++;
		file &f = g.files[id];
		f.used_at = g.presents;
		return nullptr;
	}

	std::vector<std::string> status()
	{
		std::vector<std::string> lines;
		char line[320];
		size_t up = 0, bytes = 0, bad = 0;
		for (const auto &f : g.files)
		{
			if (f.second.z != nullptr)
			{
				up++;
				bytes += size_t(f.second.z->bytes());
			}
			else if (!f.second.why.empty())
				bad++;
		}
		std::snprintf(line, sizeof(line), "model actors' data: %zu DATs on the GPU (%.1f MB), %zu waiting, %zu couldn't be read; %llu asked for, %llu uploaded, %llu let go since the start",
			up, double(bytes) / 1048576.0, g.queue.size() + (g.loading != 0 ? 1 : 0), bad, static_cast<unsigned long long>(g.asked),
			static_cast<unsigned long long>(g.uploaded), static_cast<unsigned long long>(g.let_go));
		lines.push_back(line);
		for (const auto &f : g.files)
			if (f.second.z == nullptr && !f.second.why.empty())
			{
				lines.push_back("model actors' data: DAT " + std::to_string(f.first) + " not loaded: " + f.second.why);
				if (lines.size() > 4)
					break;
			}
		return lines;
	}

	void stop()
	{
		forget_device();
		g.wanted = false;
	}
}
