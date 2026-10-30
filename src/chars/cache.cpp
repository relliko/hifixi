/*
 * The characters' meshes and textures on the GPU (cache.hpp).
 *
 * The game's thread adds entries with the bytes it copied and names known ones each frame; a thread of ours decodes
 * what's queued (os2::decode, tex::decode + make_mips) and, once there's a device, uploads a batch: default-heap
 * resources made in COMMON, one upload buffer, one copy list on our own copy queue, then waits for its fence and marks
 * the batch's entries ready. Everything shared is under one mutex; decoding and uploading run outside it.
 *
 * No device call is made while the mutex is held, Release included (resources let go wait in `dead` until it's left):
 * dgVoodoo's recording thread asks the cache for views inside our hooks, which with the debug layer run inside its
 * device lock, so a device call made under our mutex could wait on that lock while the recording thread waits on us.
 */

#include <windows.h>
#include "cache.hpp"
#include "charframe.hpp"
#include "tex.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace hifi::chars::cache
{
	namespace
	{
		template <typename T>
		void drop(T *&p)
		{
			if (p != nullptr)
				p->Release();
			p = nullptr;
		}

		double seconds()
		{
			LARGE_INTEGER t, f;
			QueryPerformanceCounter(&t);
			QueryPerformanceFrequency(&f);
			return double(t.QuadPart) / double(f.QuadPart);
		}

		enum class state
		{
			queued,    // bytes in, not decoded yet
			decoded,   // decoded, waiting for a device
			uploading, // in a batch whose copies run
			ready,
			failed,
		};

		constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
		constexpr uint64_t kBatchBytes = 48ull << 20; // (upload bytes a batch at most)

		struct mesh_entry
		{
			key k;
			std::vector<uint8_t> raw;
			uint32_t base = 0;
			std::vector<uint32_t> textures;
			uint8_t hides = 0;
			state s = state::queued;
			os2::mesh decoded;  // (vertices and corners dropped once uploaded; groups kept)
			int weapon_slot = -1; // (the weapon slot its name gives: os2::weapon_slot)
			os2::blade blade;   // (found from the vertices while they're here)
			uint32_t vertices = 0, corners = 0;
			ID3D12Resource *vb = nullptr, *cb = nullptr;
			uint32_t slot = kNoSlot;
			double seen = 0.0;
			uint64_t gpu_bytes = 0;
		};

		struct texture_entry
		{
			key k;
			std::vector<uint8_t> raw;
			state s = state::queued;
			tex::texture decoded; // (dropped once uploaded)
			ID3D12Resource *t = nullptr;
			uint32_t slot = kNoSlot;
			double seen = 0.0;
			uint64_t gpu_bytes = 0;
		};

		struct state_all
		{
			std::mutex mutex;
			std::condition_variable wake, idle;
			std::thread worker;
			bool quit = false;
			bool busy = false; // the worker is decoding or uploading outside the lock

			std::vector<std::unique_ptr<mesh_entry>> meshes;
			std::vector<std::unique_ptr<texture_entry>> textures;
			std::vector<uint32_t> free_meshes, free_textures;
			std::unordered_map<uintptr_t, uint32_t> mesh_of, texture_of;
			double now = 0.0;

			// The GPU (made by the worker once there's a device; the palettes' buffer by the game's thread)
			ID3D12Device *device = nullptr;
			ID3D12CommandQueue *queue = nullptr;
			ID3D12CommandAllocator *allocator = nullptr;
			ID3D12GraphicsCommandList *list = nullptr;
			ID3D12Fence *fence = nullptr;
			uint64_t fence_value = 0;
			ID3D12DescriptorHeap *heap = nullptr;
			UINT step = 0;
			std::vector<uint32_t> free_slots; // (pairs of descriptors: slot s = descriptors 2s, 2s + 1)
			ID3D12Resource *palette_buffer = nullptr;
			uint8_t *palette_mapped = nullptr;
			std::string gpu_failure;

			counts n;
			std::vector<IUnknown *> dead; // (to be released once the mutex is left)
		};
		state_all g;

		// (the mutex held) What's been let go, to release after it's left
		std::vector<IUnknown *> take_dead()
		{
			std::vector<IUnknown *> d;
			d.swap(g.dead);
			return d;
		}
		void bury(std::vector<IUnknown *> &&d)
		{
			for (IUnknown *p : d)
				if (p != nullptr)
					p->Release();
		}
		template <typename T>
		void later(T *&p)
		{
			if (p != nullptr)
				g.dead.push_back(p);
			p = nullptr;
		}

		// (under the lock) An entry's GPU things let go
		void release_mesh(mesh_entry &e)
		{
			later(e.vb);
			later(e.cb);
			if (e.slot != kNoSlot)
				g.free_slots.push_back(e.slot);
			e.slot = kNoSlot;
		}
		void release_texture(texture_entry &e)
		{
			later(e.t);
			if (e.slot != kNoSlot)
				g.free_slots.push_back(e.slot);
			e.slot = kNoSlot;
		}

		ID3D12Resource *make_buffer(ID3D12Device *device, D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_STATES st)
		{
			D3D12_HEAP_PROPERTIES hp = {};
			hp.Type = heap;
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			d.Width = std::max<uint64_t>(size, 256);
			d.Height = 1;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.SampleDesc.Count = 1;
			d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ID3D12Resource *r = nullptr;
			if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r))))
				return nullptr;
			return r;
		}

		D3D12_RESOURCE_DESC texture_desc(const tex::texture &t)
		{
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			d.Width = t.w;
			d.Height = t.h;
			d.DepthOrArraySize = 1;
			d.MipLevels = UINT16(t.levels.size());
			d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			d.SampleDesc.Count = 1;
			return d;
		}

		// (the worker, the mutex NOT held) The queue, list, fence and heap, once; stored under the lock by the caller
		struct gpu_parts
		{
			ID3D12CommandQueue *queue = nullptr;
			ID3D12CommandAllocator *allocator = nullptr;
			ID3D12GraphicsCommandList *list = nullptr;
			ID3D12Fence *fence = nullptr;
			ID3D12DescriptorHeap *heap = nullptr;
			UINT step = 0;
		};
		bool make_gpu_parts(ID3D12Device *device, gpu_parts &p)
		{
			D3D12_COMMAND_QUEUE_DESC qd = {};
			qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
			D3D12_DESCRIPTOR_HEAP_DESC hd = {};
			hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			hd.NumDescriptors = kDescriptors;
			hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&p.queue))) ||
				FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&p.allocator))) ||
				FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, p.allocator, nullptr, IID_PPV_ARGS(&p.list))) ||
				FAILED(p.list->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.fence))) ||
				FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.heap))))
			{
				drop(p.heap);
				drop(p.fence);
				drop(p.list);
				drop(p.allocator);
				drop(p.queue);
				return false;
			}
			p.step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			return true;
		}

		// (the worker, the lock held by `lock`) The GPU parts made once there's a device (the lock left meanwhile)
		bool make_gpu(std::unique_lock<std::mutex> &lock)
		{
			if (g.queue != nullptr)
				return true;
			if (g.device == nullptr || !g.gpu_failure.empty())
				return false;
			ID3D12Device *const device = g.device;
			lock.unlock();
			gpu_parts p;
			const bool ok = make_gpu_parts(device, p);
			lock.lock();
			if (!ok)
			{
				g.gpu_failure = "the characters' copy queue or descriptor heap couldn't be made";
				return false;
			}
			g.queue = p.queue;
			g.allocator = p.allocator;
			g.list = p.list;
			g.fence = p.fence;
			g.heap = p.heap;
			g.step = p.step;
			g.free_slots.clear();
			// (the last pairs are the renderer's own: reserved())
			for (uint32_t s = kDescriptors / 2 - kReserved / 2; s-- > 0;)
				g.free_slots.push_back(s);
			return true;
		}

		D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor(uint32_t index)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE h = g.heap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += SIZE_T(index) * g.step;
			return h;
		}

		// (the worker, outside the lock) Decode one entry
		void decode_mesh(mesh_entry &e, double &ms)
		{
			const double t0 = seconds();
			os2::sections s;
			std::string why;
			const bool ok = (e.base != 0 ? os2::sections_of_live(e.raw.data(), e.raw.size(), e.base, s) : os2::sections_of_chunk(e.raw.data(), e.raw.size(), s)) &&
				os2::decode(e.raw.data(), e.raw.size(), s, e.decoded, why);
			e.s = ok ? state::decoded : state::failed;
			if (!ok)
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				g.n.last_failure = "a mesh: " + (why.empty() ? std::string("its header") : why);
			}
			e.vertices = uint32_t(e.decoded.vertices.size());
			e.corners = uint32_t(e.decoded.corners.size());
			if (ok)
				e.blade = os2::blade_of(e.decoded, e.weapon_slot);
			std::vector<uint8_t>().swap(e.raw);
			ms += (seconds() - t0) * 1000.0;
		}
		void decode_texture(texture_entry &e, double &ms)
		{
			const double t0 = seconds();
			std::string why;
			const bool ok = tex::decode(e.raw.data(), e.raw.size(), e.decoded, why);
			if (ok)
				tex::make_mips(e.decoded);
			e.s = ok ? state::decoded : state::failed;
			if (!ok)
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				g.n.last_failure = "a texture: " + why;
			}
			std::vector<uint8_t>().swap(e.raw);
			ms += (seconds() - t0) * 1000.0;
		}

		// (the worker, outside the lock; the entries are its own until marked) One batch's copies, waited for
		struct batch
		{
			std::vector<mesh_entry *> meshes;
			std::vector<texture_entry *> textures;
		};
		bool upload(batch &b, double &ms, std::string &why)
		{
			const double t0 = seconds();
			ID3D12Device *const device = g.device;
			// Where each goes in the upload buffer
			uint64_t at = 0;
			auto align = [](uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); };
			std::vector<uint64_t> mesh_at;
			for (mesh_entry *e : b.meshes)
			{
				mesh_at.push_back(at);
				at = align(at + uint64_t(e->vertices) * sizeof(os2::vertex), 16);
				at = align(at + uint64_t(e->corners) * sizeof(os2::corner), 16);
			}
			at = align(at, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
			std::vector<std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>> spots(b.textures.size());
			std::vector<std::vector<UINT>> rows(b.textures.size());
			std::vector<std::vector<UINT64>> row_bytes(b.textures.size());
			for (size_t i = 0; i < b.textures.size(); ++i)
			{
				const D3D12_RESOURCE_DESC d = texture_desc(b.textures[i]->decoded);
				const UINT levels = d.MipLevels;
				spots[i].resize(levels);
				rows[i].resize(levels);
				row_bytes[i].resize(levels);
				UINT64 size = 0;
				device->GetCopyableFootprints(&d, 0, levels, at, spots[i].data(), rows[i].data(), row_bytes[i].data(), &size);
				at = align(at + size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
			}
			ID3D12Resource *up = make_buffer(device, D3D12_HEAP_TYPE_UPLOAD, at, D3D12_RESOURCE_STATE_GENERIC_READ);
			if (up == nullptr)
			{
				why = "an upload buffer couldn't be made";
				return false;
			}
			bool ok = true;
			for (mesh_entry *e : b.meshes)
			{
				e->vb = make_buffer(device, D3D12_HEAP_TYPE_DEFAULT, uint64_t(e->vertices) * sizeof(os2::vertex), D3D12_RESOURCE_STATE_COMMON);
				e->cb = make_buffer(device, D3D12_HEAP_TYPE_DEFAULT, uint64_t(e->corners) * sizeof(os2::corner), D3D12_RESOURCE_STATE_COMMON);
				ok = ok && e->vb != nullptr && e->cb != nullptr;
				e->gpu_bytes = uint64_t(e->vertices) * sizeof(os2::vertex) + uint64_t(e->corners) * sizeof(os2::corner);
			}
			for (texture_entry *e : b.textures)
			{
				D3D12_HEAP_PROPERTIES hp = {};
				hp.Type = D3D12_HEAP_TYPE_DEFAULT;
				const D3D12_RESOURCE_DESC d = texture_desc(e->decoded);
				ok = ok && SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&e->t)));
				e->gpu_bytes = device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
			}
			uint8_t *mapped = nullptr;
			const D3D12_RANGE none = { 0, 0 };
			if (!ok || FAILED(up->Map(0, &none, reinterpret_cast<void **>(&mapped))))
			{
				up->Release();
				why = "the characters' resources couldn't be made (video memory?)";
				return false;
			}
			for (size_t i = 0; i < b.meshes.size(); ++i)
			{
				const mesh_entry &e = *b.meshes[i];
				const uint64_t vbytes = uint64_t(e.vertices) * sizeof(os2::vertex);
				std::memcpy(mapped + mesh_at[i], e.decoded.vertices.data(), size_t(vbytes));
				std::memcpy(mapped + align(mesh_at[i] + vbytes, 16), e.decoded.corners.data(), size_t(e.corners) * sizeof(os2::corner));
			}
			for (size_t i = 0; i < b.textures.size(); ++i)
			{
				const tex::texture &t = b.textures[i]->decoded;
				for (size_t l = 0; l < t.levels.size(); ++l)
				{
					const tex::level &lv = t.levels[l];
					for (UINT r = 0; r < rows[i][l]; ++r)
						std::memcpy(mapped + spots[i][l].Offset + uint64_t(r) * spots[i][l].Footprint.RowPitch, lv.rgba.data() + size_t(r) * lv.w * 4, size_t(lv.w) * 4);
				}
			}
			up->Unmap(0, nullptr);

			// The copies, on our queue, waited for
			if (FAILED(g.allocator->Reset()) || FAILED(g.list->Reset(g.allocator, nullptr)))
			{
				up->Release();
				why = "the copy list couldn't be reset";
				return false;
			}
			for (size_t i = 0; i < b.meshes.size(); ++i)
			{
				const mesh_entry &e = *b.meshes[i];
				const uint64_t vbytes = uint64_t(e.vertices) * sizeof(os2::vertex);
				if (vbytes != 0)
					g.list->CopyBufferRegion(e.vb, 0, up, mesh_at[i], vbytes);
				if (e.corners != 0)
					g.list->CopyBufferRegion(e.cb, 0, up, align(mesh_at[i] + vbytes, 16), uint64_t(e.corners) * sizeof(os2::corner));
			}
			for (size_t i = 0; i < b.textures.size(); ++i)
				for (UINT l = 0; l < UINT(spots[i].size()); ++l)
				{
					D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
					dst.pResource = b.textures[i]->t;
					dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					dst.SubresourceIndex = l;
					src.pResource = up;
					src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					src.PlacedFootprint = spots[i][l];
					g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
				}
			if (FAILED(g.list->Close()))
			{
				up->Release();
				why = "the copy list couldn't be closed";
				return false;
			}
			ID3D12CommandList *lists[] = { g.list };
			g.queue->ExecuteCommandLists(1, lists);
			const uint64_t value = ++g.fence_value;
			g.queue->Signal(g.fence, value);
			if (g.fence->GetCompletedValue() < value)
			{
				HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
				g.fence->SetEventOnCompletion(value, ev);
				WaitForSingleObject(ev, 10000);
				CloseHandle(ev);
			}
			up->Release();
			if (g.fence->GetCompletedValue() < value)
			{
				why = "the copies didn't finish in 10 s";
				return false;
			}
			ms += (seconds() - t0) * 1000.0;
			return true;
		}

		void worker()
		{
			std::unique_lock<std::mutex> lock(g.mutex);
			for (;;)
			{
				// What there is to do: entries to decode, decoded ones to upload (with a device)
				std::vector<mesh_entry *> decode_m;
				std::vector<texture_entry *> decode_t;
				batch b;
				uint64_t batch_bytes = 0;
				const bool gpu = make_gpu(lock);
				for (auto &e : g.meshes)
					if (e)
					{
						if (e->s == state::queued)
							decode_m.push_back(e.get());
						else if (e->s == state::decoded && gpu && batch_bytes < kBatchBytes && g.free_slots.size() > 0)
						{
							e->s = state::uploading;
							e->slot = g.free_slots.back();
							g.free_slots.pop_back();
							b.meshes.push_back(e.get());
							batch_bytes += uint64_t(e->vertices) * sizeof(os2::vertex) + uint64_t(e->corners) * sizeof(os2::corner);
						}
					}
				for (auto &e : g.textures)
					if (e)
					{
						if (e->s == state::queued)
							decode_t.push_back(e.get());
						else if (e->s == state::decoded && gpu && batch_bytes < kBatchBytes && g.free_slots.size() > 0)
						{
							e->s = state::uploading;
							e->slot = g.free_slots.back();
							g.free_slots.pop_back();
							b.textures.push_back(e.get());
							batch_bytes += uint64_t(e->decoded.w) * e->decoded.h * 6;
						}
					}
				if (decode_m.empty() && decode_t.empty() && b.meshes.empty() && b.textures.empty())
				{
					g.busy = false;
					g.idle.notify_all();
					if (g.quit)
						return;
					{
						std::vector<IUnknown *> d = take_dead();
						lock.unlock();
						bury(std::move(d));
						lock.lock();
					}
					g.wake.wait(lock);
					continue;
				}
				g.busy = true;
				// (the entries picked are the worker's until their state changes: the game's thread doesn't touch an
				// entry's decoded data or resources, and forgets only entries that are ready, failed or decoded)
				for (mesh_entry *e : decode_m)
					e->s = state::uploading; // (held while decoding: not forgotten under us)
				for (texture_entry *e : decode_t)
					e->s = state::uploading;
				lock.unlock();
				double decode_ms = 0.0, upload_ms = 0.0;
				for (mesh_entry *e : decode_m)
					decode_mesh(*e, decode_ms);
				for (texture_entry *e : decode_t)
					decode_texture(*e, decode_ms);
				std::string why;
				const bool uploaded = (b.meshes.empty() && b.textures.empty()) || upload(b, upload_ms, why);
				// The views, still outside the lock (the entries are the worker's until they're marked)
				if (uploaded)
				{
					for (mesh_entry *e : b.meshes)
					{
						D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
						v.Format = DXGI_FORMAT_UNKNOWN;
						v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
						v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
						v.Buffer.NumElements = std::max(e->vertices, 1u);
						v.Buffer.StructureByteStride = sizeof(os2::vertex);
						g.device->CreateShaderResourceView(e->vb, &v, cpu_descriptor(2 * e->slot));
						v.Buffer.NumElements = std::max(e->corners, 1u);
						v.Buffer.StructureByteStride = sizeof(os2::corner);
						g.device->CreateShaderResourceView(e->cb, &v, cpu_descriptor(2 * e->slot + 1));
					}
					for (texture_entry *e : b.textures)
					{
						D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
						v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
						v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
						v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
						v.Texture2D.MipLevels = UINT(e->decoded.levels.size());
						g.device->CreateShaderResourceView(e->t, &v, cpu_descriptor(2 * e->slot));
					}
				}
				lock.lock();
				g.n.decode_ms += decode_ms;
				g.n.upload_ms += upload_ms;
				// (decode_mesh/_texture set decoded or failed; they took the lock only for the failure text)
				for (mesh_entry *e : b.meshes)
				{
					if (uploaded)
					{
						std::vector<os2::vertex>().swap(e->decoded.vertices);
						std::vector<os2::corner>().swap(e->decoded.corners);
						e->s = state::ready;
						g.n.gpu_bytes += e->gpu_bytes;
					}
					else
					{
						release_mesh(*e);
						e->s = state::failed;
						g.n.last_failure = why;
					}
				}
				for (texture_entry *e : b.textures)
				{
					if (uploaded)
					{
						e->decoded.levels.clear();
						e->decoded.levels.shrink_to_fit();
						e->s = state::ready;
						g.n.gpu_bytes += e->gpu_bytes;
					}
					else
					{
						release_texture(*e);
						e->s = state::failed;
						g.n.last_failure = why;
					}
				}
			}
		}

		void start_worker()
		{
			if (!g.worker.joinable())
			{
				g.quit = false;
				g.worker = std::thread(worker);
			}
		}

		template <typename E>
		uint32_t add(std::vector<std::unique_ptr<E>> &list, std::vector<uint32_t> &free, std::unordered_map<uintptr_t, uint32_t> &by, std::unique_ptr<E> e)
		{
			uint32_t id;
			if (!free.empty())
			{
				id = free.back();
				free.pop_back();
				list[id] = std::move(e);
			}
			else
			{
				id = uint32_t(list.size());
				list.push_back(std::move(e));
			}
			by[list[id]->k.resource] = id;
			return id;
		}

		// (under the lock) An entry let go: its GPU things, its id freed
		void forget_mesh(uint32_t id)
		{
			mesh_entry &e = *g.meshes[id];
			release_mesh(e);
			if (e.s == state::ready || e.s == state::uploading)
				g.n.gpu_bytes -= std::min(g.n.gpu_bytes, e.gpu_bytes);
			auto it = g.mesh_of.find(e.k.resource);
			if (it != g.mesh_of.end() && it->second == id)
				g.mesh_of.erase(it);
			g.meshes[id].reset();
			g.free_meshes.push_back(id);
			g.n.forgotten++;
		}
		void forget_texture(uint32_t id)
		{
			texture_entry &e = *g.textures[id];
			release_texture(e);
			if (e.s == state::ready)
				g.n.gpu_bytes -= std::min(g.n.gpu_bytes, e.gpu_bytes);
			auto it = g.texture_of.find(e.k.resource);
			if (it != g.texture_of.end() && it->second == id)
				g.texture_of.erase(it);
			g.textures[id].reset();
			g.free_textures.push_back(id);
			g.n.forgotten++;
		}
		// (an entry the worker holds isn't forgotten: queued, decoded with the worker, or uploading)
		bool forgettable(state s)
		{
			return s == state::ready || s == state::failed || s == state::decoded;
		}
	}

	uint32_t mesh(const key &k)
	{
		struct buried
		{
			std::vector<IUnknown *> d;
			~buried() { bury(std::move(d)); }
		} graves;
		std::vector<IUnknown *> &dead = graves.d;
		std::lock_guard<std::mutex> lock(g.mutex);
		struct at_exit
		{
			std::vector<IUnknown *> &d;
			~at_exit() { d = take_dead(); }
		} keep { dead };
		auto it = g.mesh_of.find(k.resource);
		if (it == g.mesh_of.end())
			return kNone;
		mesh_entry &e = *g.meshes[it->second];
		if (!(e.k == k))
		{
			// (another resource at the same address: the old entry goes, once the worker isn't holding it)
			if (forgettable(e.s))
				forget_mesh(it->second);
			else
				g.mesh_of.erase(it);
			return kNone;
		}
		e.seen = g.now;
		for (uint32_t t : e.textures)
			if (t != kNone && t < g.textures.size() && g.textures[t])
				g.textures[t]->seen = g.now;
		return it->second;
	}

	uint32_t texture(const key &k)
	{
		struct buried
		{
			std::vector<IUnknown *> d;
			~buried() { bury(std::move(d)); }
		} graves;
		std::lock_guard<std::mutex> lock(g.mutex);
		struct at_exit
		{
			std::vector<IUnknown *> &d;
			~at_exit() { d = take_dead(); }
		} keep { graves.d };
		auto it = g.texture_of.find(k.resource);
		if (it == g.texture_of.end())
			return kNone;
		texture_entry &e = *g.textures[it->second];
		if (!(e.k == k))
		{
			if (forgettable(e.s))
				forget_texture(it->second);
			else
				g.texture_of.erase(it);
			return kNone;
		}
		e.seen = g.now;
		return it->second;
	}

	uint32_t add_mesh(const key &k, std::vector<uint8_t> &&data, uint32_t live_base, std::vector<uint32_t> &&textures, const char *name)
	{
		auto e = std::make_unique<mesh_entry>();
		e->k = k;
		e->weapon_slot = os2::weapon_slot(name);
		e->hides = data.size() > 3 ? data[3] : 0;
		e->raw = std::move(data);
		e->base = live_base;
		e->textures = std::move(textures);
		std::lock_guard<std::mutex> lock(g.mutex);
		e->seen = g.now;
		g.n.bytes_in += e->raw.size();
		const uint32_t id = add(g.meshes, g.free_meshes, g.mesh_of, std::move(e));
		start_worker();
		g.wake.notify_one();
		return id;
	}

	uint32_t add_texture(const key &k, std::vector<uint8_t> &&data)
	{
		auto e = std::make_unique<texture_entry>();
		e->k = k;
		e->raw = std::move(data);
		std::lock_guard<std::mutex> lock(g.mutex);
		e->seen = g.now;
		g.n.bytes_in += e->raw.size();
		const uint32_t id = add(g.textures, g.free_textures, g.texture_of, std::move(e));
		start_worker();
		g.wake.notify_one();
		return id;
	}

	uint8_t mesh_hides(uint32_t id)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		return id < g.meshes.size() && g.meshes[id] ? g.meshes[id]->hides : 0;
	}

	void frame_tick(ID3D12Device *device, double now)
	{
		// The palettes' upload buffer (kFrames slots of kMaxBones rows, mapped for good), made once outside the lock
		bool want_buffer = false;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			want_buffer = device != nullptr && g.device == nullptr;
		}
		ID3D12Resource *buffer = nullptr;
		uint8_t *mapped = nullptr;
		if (want_buffer)
		{
			buffer = make_buffer(device, D3D12_HEAP_TYPE_UPLOAD, uint64_t(kFrames) * kMaxBones * sizeof(bone), D3D12_RESOURCE_STATE_GENERIC_READ);
			const D3D12_RANGE none = { 0, 0 };
			if (buffer != nullptr && FAILED(buffer->Map(0, &none, reinterpret_cast<void **>(&mapped))))
			{
				buffer->Release();
				buffer = nullptr;
			}
		}
		std::vector<IUnknown *> dead;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.now = now;
			if (want_buffer && g.device == nullptr)
			{
				g.device = device;
				g.palette_buffer = buffer;
				g.palette_mapped = mapped;
				buffer = nullptr;
				if (g.palette_buffer == nullptr)
					g.gpu_failure = "the palettes' upload buffer couldn't be made";
				start_worker();
				g.wake.notify_one();
			}
			// What hasn't been named for a while
			for (uint32_t i = 0; i < g.meshes.size(); ++i)
				if (g.meshes[i] && now - g.meshes[i]->seen > kForgetSeconds && forgettable(g.meshes[i]->s))
					forget_mesh(i);
			for (uint32_t i = 0; i < g.textures.size(); ++i)
				if (g.textures[i] && now - g.textures[i]->seen > kForgetSeconds && forgettable(g.textures[i]->s))
					forget_texture(i);
			dead = take_dead();
		}
		if (buffer != nullptr)
			buffer->Release(); // (made twice at once: not ours)
		bury(std::move(dead));
	}

	void publish_palettes(const frame &f, uint32_t from, uint32_t to)
	{
		if (g.palette_mapped == nullptr || from >= to || to > kMaxBones)
			return;
		const uint32_t slot = f.seq.load(std::memory_order_relaxed) % kFrames;
		std::memcpy(g.palette_mapped + (uint64_t(slot) * kMaxBones + from) * sizeof(bone), &f.palette[from], size_t(to - from) * sizeof(bone));
	}

	D3D12_GPU_VIRTUAL_ADDRESS palettes(uint32_t seq)
	{
		if (g.palette_buffer == nullptr)
			return 0;
		return g.palette_buffer->GetGPUVirtualAddress() + uint64_t(seq % kFrames) * kMaxBones * sizeof(bone);
	}

	bool mesh_view_of(uint32_t id, mesh_view &out)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (id >= g.meshes.size() || !g.meshes[id] || g.meshes[id]->s != state::ready)
			return false;
		const mesh_entry &e = *g.meshes[id];
		out.ready = true;
		out.hides = e.hides;
		out.vertices = e.vertices;
		out.corners = e.corners;
		out.groups = e.decoded.groups;
		out.textures = e.textures;
		out.blade = e.blade;
		out.views = g.heap->GetGPUDescriptorHandleForHeapStart();
		out.views.ptr += uint64_t(2 * e.slot) * g.step;
		return true;
	}

	bool mesh_ready(uint32_t id)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (id >= g.meshes.size() || !g.meshes[id] || g.meshes[id]->s != state::ready)
			return false;
		for (uint32_t t : g.meshes[id]->textures)
			if (t != kNone && (t >= g.textures.size() || !g.textures[t] || (g.textures[t]->s != state::ready && g.textures[t]->s != state::failed)))
				return false;
		return true;
	}

	bool texture_view_of(uint32_t id, D3D12_GPU_DESCRIPTOR_HANDLE &out)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (id >= g.textures.size() || !g.textures[id] || g.textures[id]->s != state::ready)
			return false;
		out = g.heap->GetGPUDescriptorHandleForHeapStart();
		out.ptr += uint64_t(2 * g.textures[id]->slot) * g.step;
		return true;
	}

	void resources_of(uint32_t mesh, uint32_t texture, ID3D12Resource *&vertices, ID3D12Resource *&corners, ID3D12Resource *&tex)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		vertices = corners = tex = nullptr;
		if (mesh < g.meshes.size() && g.meshes[mesh] && g.meshes[mesh]->s == state::ready)
		{
			vertices = g.meshes[mesh]->vb;
			corners = g.meshes[mesh]->cb;
		}
		if (texture < g.textures.size() && g.textures[texture] && g.textures[texture]->s == state::ready)
			tex = g.textures[texture]->t;
	}

	ID3D12DescriptorHeap *heap()
	{
		return g.heap;
	}

	bool reserved(uint32_t k, D3D12_CPU_DESCRIPTOR_HANDLE &cpu, D3D12_GPU_DESCRIPTOR_HANDLE &gpu)
	{
		if (g.heap == nullptr || k >= kReserved)
			return false;
		const uint32_t index = kDescriptors - kReserved + k;
		cpu = cpu_descriptor(index);
		gpu = g.heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += uint64_t(index) * g.step;
		return true;
	}

	counts get_counts()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		counts c = g.n;
		for (auto &e : g.meshes)
			if (e)
			{
				c.meshes++;
				c.meshes_ready += e->s == state::ready;
				c.meshes_failed += e->s == state::failed;
			}
		for (auto &e : g.textures)
			if (e)
			{
				c.textures++;
				c.textures_ready += e->s == state::ready;
				c.textures_failed += e->s == state::failed;
			}
		if (c.last_failure.empty())
			c.last_failure = g.gpu_failure;
		return c;
	}

	std::vector<std::string> status()
	{
		const counts c = get_counts();
		char line[400];
		std::snprintf(line, sizeof(line), "character data: %u meshes (%u on the GPU, %u couldn't be read), %u textures (%u on the GPU, %u couldn't be read); %.1f MB on the GPU; "
			"%.1f MB taken from the game, decoding %.0f ms, uploading %.0f ms in all; %u let go",
			c.meshes, c.meshes_ready, c.meshes_failed, c.textures, c.textures_ready, c.textures_failed, double(c.gpu_bytes) / 1048576.0, double(c.bytes_in) / 1048576.0,
			c.decode_ms, c.upload_ms, c.forgotten);
		std::vector<std::string> out { line };
		if (!c.last_failure.empty())
			out.push_back("character data: the last that couldn't be: " + c.last_failure);
		return out;
	}

	bool settle(uint32_t ms)
	{
		std::unique_lock<std::mutex> lock(g.mutex);
		return g.idle.wait_for(lock, std::chrono::milliseconds(ms), [] {
			if (g.busy)
				return false;
			// (decoded with a device: to be uploaded, unless the GPU side couldn't be made)
			const bool uploads = g.device != nullptr && g.gpu_failure.empty();
			for (auto &e : g.meshes)
				if (e && (e->s == state::queued || (e->s == state::decoded && uploads)))
					return false;
			for (auto &e : g.textures)
				if (e && (e->s == state::queued || (e->s == state::decoded && uploads)))
					return false;
			return true;
		});
	}

	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.quit = true;
			g.wake.notify_all();
		}
		if (g.worker.joinable())
			g.worker.join();
		std::vector<IUnknown *> dead;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			for (auto &e : g.meshes)
				if (e)
					release_mesh(*e);
			for (auto &e : g.textures)
				if (e)
					release_texture(*e);
			g.meshes.clear();
			g.textures.clear();
			g.free_meshes.clear();
			g.free_textures.clear();
			g.mesh_of.clear();
			g.texture_of.clear();
			if (g.palette_buffer != nullptr && g.palette_mapped != nullptr)
				g.palette_buffer->Unmap(0, nullptr);
			g.palette_mapped = nullptr;
			later(g.palette_buffer);
			later(g.heap);
			later(g.fence);
			later(g.list);
			later(g.allocator);
			later(g.queue);
			g.device = nullptr;
			g.fence_value = 0;
			g.free_slots.clear();
			g.busy = false;
			g.quit = false;
			g.now = 0.0;
			g.n = counts();
			g.gpu_failure.clear();
			dead = take_dead();
		}
		bury(std::move(dead));
	}
}
