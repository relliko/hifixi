/*
 * The effects' textures (textures.hpp). Three threads touch an entry: the game's (the map from keys to ids, last use,
 * the chunk's bytes handed over), the decoder's (the bytes to RGBA) and dgVoodoo's recording thread (the GPU copy, the
 * view, the release). Each entry's state says whose it is:
 *   free -> queued (the game's add) -> decoded (the decoder) -> on_gpu (the copy recorded) or failed
 *   on_gpu / failed -> forgotten (the game's tick, unused for kForgetSeconds) -> free (dgVoodoo's thread, the texture
 *   kept two more seconds for the lists already recorded)
 */

#include <windows.h>
#include "fxtextures.hpp"
#include "../chars/tex.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace hifi::fx::textures
{
	namespace
	{
		enum : uint32_t
		{
			st_free = 0,
			st_queued = 1,
			st_decoded = 2,
			st_on_gpu = 3,
			st_failed = 4,
			st_forgotten = 5,
		};
		constexpr ULONGLONG kRetireMs = 2000;

		struct entry
		{
			std::atomic<uint32_t> state { st_free };
			key k;
			double used = 0.0;
			std::vector<uint8_t> chunk; // (the game's thread hands it to the decoder)
			uint32_t w = 0, h = 0;
			std::vector<uint8_t> rgba;  // (the decoder's, for the copy)
			ID3D12Resource *tex = nullptr;
		};

		struct key_hash
		{
			size_t operator()(const key &k) const { return std::hash<uint64_t>()((uint64_t(k.resource) << 32) ^ k.handle ^ (uint64_t(k.data) << 16) ^ k.header); }
		};

		struct state
		{
			std::unique_ptr<entry[]> entries { new entry[kMax] };
			// The game's thread
			std::unordered_map<key, uint32_t, key_hash> ids;
			uint32_t forgotten = 0;
			// The decoder
			std::mutex queue_mutex;
			std::condition_variable wake;
			std::deque<uint32_t> queue;
			std::thread decoder;
			bool stopping = false;
			uint32_t busy = 0;
			// dgVoodoo's thread
			std::mutex gpu_mutex;
			ID3D12Device *device = nullptr;
			ID3D12DescriptorHeap *heap = nullptr;
			UINT step = 0;
			ID3D12Resource *white = nullptr;
			bool white_copied = false;
			std::vector<std::pair<ID3D12Resource *, ULONGLONG>> retired; // (let go at that tick count)
			uint64_t gpu_bytes = 0;
			std::atomic<uint32_t> waiting { 0 };
		};
		state g;

		void decoder_loop()
		{
			for (;;)
			{
				uint32_t id = kNone;
				{
					std::unique_lock<std::mutex> lock(g.queue_mutex);
					g.wake.wait(lock, [] { return g.stopping || !g.queue.empty(); });
					if (g.stopping)
						return;
					id = g.queue.front();
					g.queue.pop_front();
					g.busy++;
				}
				entry &e = g.entries[id];
				chars::tex::texture t;
				std::string why;
				const bool ok = chars::tex::decode(e.chunk.data(), e.chunk.size(), t, why) && !t.levels.empty();
				e.chunk.clear();
				e.chunk.shrink_to_fit();
				if (ok)
				{
					e.w = t.levels[0].w;
					e.h = t.levels[0].h;
					e.rgba = std::move(t.levels[0].rgba);
					e.state.store(st_decoded, std::memory_order_release);
					g.waiting.fetch_add(1, std::memory_order_relaxed);
				}
				else
					e.state.store(st_failed, std::memory_order_release);
				std::lock_guard<std::mutex> lock(g.queue_mutex);
				g.busy--;
			}
		}

		template <typename T>
		void drop(T *&p)
		{
			if (p != nullptr)
				p->Release();
			p = nullptr;
		}

		D3D12_CPU_DESCRIPTOR_HANDLE cpu_at(UINT i)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE h = g.heap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += SIZE_T(i) * g.step;
			return h;
		}

		// (under the GPU lock) A texture made, its pixels copied in on the list through an upload buffer of ours, its
		// view at a slot; nullptr when it can't be
		ID3D12Resource *upload_one(ID3D12GraphicsCommandList *list, uint32_t w, uint32_t h, const uint8_t *rgba, UINT slot)
		{
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			d.Width = w;
			d.Height = h;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			d.SampleDesc.Count = 1;
			const D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT };
			ID3D12Resource *tex = nullptr;
			if (FAILED(g.device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))))
				return nullptr;
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT spot = {};
			UINT rows = 0;
			UINT64 row_bytes = 0, total = 0;
			g.device->GetCopyableFootprints(&d, 0, 1, 0, &spot, &rows, &row_bytes, &total);
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = total;
			bd.Height = 1;
			bd.DepthOrArraySize = 1;
			bd.MipLevels = 1;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			const D3D12_HEAP_PROPERTIES uh = { D3D12_HEAP_TYPE_UPLOAD };
			ID3D12Resource *up = nullptr;
			uint8_t *mapped = nullptr;
			const D3D12_RANGE none = { 0, 0 };
			if (FAILED(g.device->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up))) ||
				FAILED(up->Map(0, &none, reinterpret_cast<void **>(&mapped))))
			{
				drop(up);
				tex->Release();
				return nullptr;
			}
			for (uint32_t y = 0; y < h; ++y)
				std::memcpy(mapped + spot.Offset + size_t(y) * spot.Footprint.RowPitch, rgba + size_t(y) * w * 4, size_t(w) * 4);
			up->Unmap(0, nullptr);
			D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
			dst.pResource = tex;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.pResource = up;
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint = spot;
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = tex;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			list->ResourceBarrier(1, &b);
			g.retired.push_back({ up, GetTickCount64() + kRetireMs });
			D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
			sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			sv.Texture2D.MipLevels = 1;
			g.device->CreateShaderResourceView(tex, &sv, cpu_at(slot));
			g.gpu_bytes += uint64_t(w) * h * 4;
			return tex;
		}
	}

	uint32_t find(const key &k, double now)
	{
		const auto it = g.ids.find(k);
		if (it == g.ids.end())
			return kNone;
		g.entries[it->second].used = now;
		return it->second;
	}

	uint32_t add(const key &k, std::vector<uint8_t> &&chunk, double now)
	{
		if (const uint32_t known = find(k, now); known != kNone)
			return known;
		for (uint32_t i = 0; i < kMax; ++i)
		{
			entry &e = g.entries[i];
			if (e.state.load(std::memory_order_acquire) != st_free)
				continue;
			e.k = k;
			e.used = now;
			e.chunk = std::move(chunk);
			e.rgba.clear();
			e.tex = nullptr;
			e.state.store(st_queued, std::memory_order_release);
			g.ids[k] = i;
			{
				std::lock_guard<std::mutex> lock(g.queue_mutex);
				g.stopping = false;
				if (!g.decoder.joinable())
					g.decoder = std::thread(decoder_loop);
				g.queue.push_back(i);
			}
			g.wake.notify_one();
			return i;
		}
		return kNone;
	}

	bool ready(uint32_t id)
	{
		return id < kMax && g.entries[id].state.load(std::memory_order_acquire) == st_on_gpu;
	}

	bool failed(uint32_t id)
	{
		return id < kMax && g.entries[id].state.load(std::memory_order_acquire) == st_failed;
	}

	void tick(double now)
	{
		for (auto it = g.ids.begin(); it != g.ids.end();)
		{
			entry &e = g.entries[it->second];
			const uint32_t s = e.state.load(std::memory_order_acquire);
			if ((s == st_on_gpu || s == st_failed) && now - e.used > kForgetSeconds)
			{
				e.state.store(st_forgotten, std::memory_order_release);
				g.forgotten++;
				it = g.ids.erase(it);
			}
			else
				++it;
		}
	}

	uint32_t waiting()
	{
		return g.waiting.load(std::memory_order_relaxed);
	}

	bool prepare(ID3D12Device *device, std::string &why)
	{
		std::lock_guard<std::mutex> lock(g.gpu_mutex);
		if (g.device == device && g.heap != nullptr)
			return true;
		if (device == nullptr)
			return false;
		ID3D12DescriptorHeap *heap = nullptr;
		const D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kReserved + kMax, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))))
		{
			why = "the effects' descriptor heap couldn't be made";
			return false;
		}
		// (another device: every texture made again on it)
		for (uint32_t i = 0; i < kMax; ++i)
		{
			entry &e = g.entries[i];
			drop(e.tex);
			uint32_t s = st_on_gpu;
			if (e.state.compare_exchange_strong(s, st_decoded))
				g.waiting.fetch_add(1, std::memory_order_relaxed);
		}
		drop(g.heap);
		drop(g.white);
		for (auto &r : g.retired)
			r.first->Release();
		g.retired.clear();
		g.heap = heap;
		g.device = device;
		g.step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		g.white_copied = false;
		g.gpu_bytes = 0;
		return true;
	}

	void release()
	{
		std::lock_guard<std::mutex> lock(g.gpu_mutex);
		for (uint32_t i = 0; i < kMax; ++i)
		{
			entry &e = g.entries[i];
			drop(e.tex);
			uint32_t s = st_on_gpu;
			if (e.state.compare_exchange_strong(s, st_decoded))
				g.waiting.fetch_add(1, std::memory_order_relaxed);
		}
		drop(g.heap);
		drop(g.white);
		for (auto &r : g.retired)
			r.first->Release();
		g.retired.clear();
		g.device = nullptr;
		g.gpu_bytes = 0;
	}

	uint32_t upload(ID3D12GraphicsCommandList *list, uint64_t budget)
	{
		std::lock_guard<std::mutex> lock(g.gpu_mutex);
		if (g.device == nullptr || g.heap == nullptr)
			return 0;
		const ULONGLONG now = GetTickCount64();
		for (auto it = g.retired.begin(); it != g.retired.end();)
			if (now >= it->second)
			{
				it->first->Release();
				it = g.retired.erase(it);
			}
			else
				++it;
		if (!g.white_copied)
		{
			const uint8_t white[4] = { 255, 255, 255, 255 };
			g.white = upload_one(list, 1, 1, white, 0);
			g.white_copied = g.white != nullptr;
		}
		uint32_t went = 0;
		uint64_t bytes = 0;
		for (uint32_t i = 0; i < kMax; ++i)
		{
			entry &e = g.entries[i];
			const uint32_t s = e.state.load(std::memory_order_acquire);
			if (s == st_forgotten)
			{
				if (e.tex != nullptr)
				{
					g.gpu_bytes -= std::min<uint64_t>(g.gpu_bytes, uint64_t(e.w) * e.h * 4);
					g.retired.push_back({ e.tex, now + kRetireMs });
					e.tex = nullptr;
				}
				e.rgba.clear();
				e.rgba.shrink_to_fit();
				e.state.store(st_free, std::memory_order_release);
				continue;
			}
			if (s != st_decoded || bytes > budget)
				continue;
			e.tex = upload_one(list, e.w, e.h, e.rgba.data(), kReserved + i);
			g.waiting.fetch_sub(1, std::memory_order_relaxed);
			if (e.tex == nullptr)
			{
				e.state.store(st_failed, std::memory_order_release);
				continue;
			}
			// (its pixels kept: on another device it's copied again)
			bytes += uint64_t(e.w) * e.h * 4;
			e.state.store(st_on_gpu, std::memory_order_release);
			went++;
		}
		return went;
	}

	ID3D12DescriptorHeap *heap()
	{
		return g.heap;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE view(uint32_t id)
	{
		D3D12_GPU_DESCRIPTOR_HANDLE h = g.heap->GetGPUDescriptorHandleForHeapStart();
		h.ptr += UINT64(id < kMax ? kReserved + id : 0) * g.step;
		return h;
	}

	bool reserved(uint32_t k, D3D12_CPU_DESCRIPTOR_HANDLE &cpu, D3D12_GPU_DESCRIPTOR_HANDLE &gpu)
	{
		if (g.heap == nullptr || k == 0 || k >= kReserved)
			return false;
		cpu = cpu_at(k);
		gpu = g.heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += UINT64(k) * g.step;
		return true;
	}

	counts get_counts()
	{
		counts c;
		for (uint32_t i = 0; i < kMax; ++i)
		{
			const uint32_t s = g.entries[i].state.load(std::memory_order_relaxed);
			c.entries += s != st_free && s != st_forgotten;
			c.on_gpu += s == st_on_gpu;
			c.failed += s == st_failed;
		}
		c.forgotten = g.forgotten;
		c.gpu_bytes = g.gpu_bytes;
		return c;
	}

	std::string status()
	{
		const counts c = get_counts();
		char line[200];
		std::snprintf(line, sizeof(line), "effect textures: %u known, %u on the GPU (%.1f MB), %u couldn't be read, %u let go after %.0f s unused", c.entries, c.on_gpu,
			double(c.gpu_bytes) / 1048576.0, c.failed, c.forgotten, kForgetSeconds);
		return line;
	}

	bool settle(uint32_t ms)
	{
		const ULONGLONG until = GetTickCount64() + ms;
		for (;;)
		{
			{
				std::lock_guard<std::mutex> lock(g.queue_mutex);
				if (g.queue.empty() && g.busy == 0)
					return true;
			}
			if (GetTickCount64() > until)
				return false;
			Sleep(1);
		}
	}

	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(g.queue_mutex);
			g.stopping = true;
			g.queue.clear();
		}
		g.wake.notify_all();
		if (g.decoder.joinable())
			g.decoder.join();
		release();
		g.ids.clear();
		for (uint32_t i = 0; i < kMax; ++i)
		{
			entry &e = g.entries[i];
			e.chunk.clear();
			e.rgba.clear();
			e.state.store(st_free, std::memory_order_release);
		}
		g.waiting.store(0);
	}
}
