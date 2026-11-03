/*
 * The characters' meshes and textures on the GPU (cache.hpp).
 *
 * The game's thread adds entries with the bytes it copied and names known ones each frame; a thread of ours decodes
 * what's queued (os2::decode, tex::decode + make_mips) and, once there's a device, uploads a batch: default-heap
 * resources made in COMMON, one upload buffer, one copy list on our own copy queue, then waits for its fence and marks
 * the batch's entries ready. Everything shared is under one mutex; decoding and uploading run outside it.
 *
 * Textures are decoded only as far as the next batch (kBatchBytes in all, decoded and not yet on the GPU): a crowd
 * queued at once, or before there's a device, isn't all held decoded in the game's 32-bit memory. A batch counts its
 * entries' real upload bytes; one entry bigger than a batch goes alone (and, once held back, before any other), one
 * over kEntryCap isn't decoded at all, and one its detail map would take over kEntryCap is decoded without one.
 *
 * No device call is made while the mutex is held, Release included (resources let go wait in `dead` until it's left):
 * with the debug layer a device call takes the runtime's own locks, and the game's thread asks the cache for views
 * while it records (docs/d3d12/threads-pso.md). A pair of descriptors let go goes back to `free_slots` only once the
 * GPU is past every list recorded with it (retire::then): lists in flight still read the views it holds.
 *
 * The texture pack's part (docs/texture-pack/characters.md): the key hashed on the worker from the bytes the game held,
 * before they're freed; everything else on the game's thread, under the mutex (charpack.cpp makes the streamer's calls
 * and the device calls with it left). A pack pair is taken only for a ticket that is ready, only over kPackSlotFloor,
 * written once before the state says ready, and let go like a game pair (retire::then, the heap's generation). Its
 * ticket goes to the drop hook from bury(), outside the mutex: the texture is let go behind the same fence.
 */

#include <windows.h>
#include "cache.hpp"
#include "charframe.hpp"
#include "charoptions.hpp"
#include "tex.hpp"
#include "../host/retire.hpp"
#include <algorithm>
#include <atomic>
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

		uint64_t align_up(uint64_t v, uint64_t a)
		{
			return (v + a - 1) & ~(a - 1);
		}

		// What a texture of w x h decodes to (RGBA8 with its mips; twice that with a detail map): what the worker plans
		// its decoding by, before it has the levels
		uint64_t planned_bytes(uint32_t w, uint32_t h, bool detail)
		{
			const uint64_t colour = uint64_t(w) * h * 4 * 4 / 3;
			return detail ? 2 * colour : colour;
		}

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
			uint64_t upload_bytes = 0; // (its share of a batch's upload buffer, known once decoded)
		};

		// A texture's pack part (cache.hpp; docs/texture-pack/characters.md "The upgrade state machine")
		enum class pack_state : uint8_t
		{
			none,    // (a key, nothing asked; or no key)
			asked,   // (a ticket on its way: "upgrading")
			writing, // (its ticket ready, a pair taken: the views being written, the mutex left)
			ready,   // (the pair written: texture_view_of hands it out)
			spent,   // (missed or gone: the game's until the pack part is forgotten)
			evicted, // (let go under the streamer's pressure: the game's until pack_unevict)
		};

		struct texture_entry
		{
			key k;
			std::vector<uint8_t> raw;
			state s = state::queued;
			uint32_t id = kNone, serial = 0; // (its id, and which texture held it: ids are reused)
			// The texture pack's part: the key (from raw, by the worker before it's freed; bytes 0: none), its state, the
			// ticket and the pack's pair, the epoch it asked in, the view's bytes, when the pair was last handed out, why
			// it's spent
			hifi::pack::key pack;
			pack_state ps = pack_state::none;
			hifi::pack::stream::ticket ticket;
			uint32_t pack_slot = kNoSlot;
			uint32_t pack_epoch = 0;
			uint64_t pack_bytes = 0;
			double pack_drawn = 0.0;
			pack_miss spent_why = pack_miss::not_in_pack;
			bool with_detail = false; // (whether its decode makes a detail map: [chars] detail when it was picked)
			tex::texture decoded; // (dropped once uploaded)
			tex::texture detail;  // its detail map (tex::make_detail; dropped once uploaded)
			ID3D12Resource *t = nullptr;
			ID3D12Resource *d = nullptr; // (the detail map: the slot's second descriptor)
			uint32_t slot = kNoSlot;
			double seen = 0.0;
			uint64_t gpu_bytes = 0;
			uint64_t decoded_bytes = 0; // (its levels' bytes while decoded or uploading: in g.decoded_bytes)
			uint64_t upload_bytes = 0;  // (its share of a batch's upload buffer, colour and detail map)
		};

		struct state_all
		{
			std::mutex mutex;
			std::condition_variable wake, idle;
			std::thread worker;
			bool quit = false;
			bool busy = false; // the worker is decoding or uploading outside the lock
			bool held = false; // it waits with textures queued that the batch holds back (no GPU to upload the decoded ones to)
			uint64_t decoded_bytes = 0; // (the textures' decoded bytes not yet on the GPU: decoded or uploading)
			uint32_t waiting = kNone;   // (the texture the batch held back first: no other is decoded until it is)

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
			uint64_t heap_generation = 0;     // (free_slots made again for a new heap: a pair let go from the last isn't given back)
			ID3D12Resource *palette_buffer = nullptr;
			uint8_t *palette_mapped = nullptr;
			std::string gpu_failure;

			counts n;
			std::vector<IUnknown *> dead; // (to be released once the mutex is left)
			std::vector<uint32_t> slots_dead; // (pairs let go: given back behind the GPU once the mutex is left)
			std::vector<hifi::pack::stream::ticket> tickets_dead; // (pack tickets let go: to the drop hook once it's left)

			// The texture pack's part (the game's thread; the worker only pushes onto the asks' list)
			uint32_t next_serial = 0;
			bool pack_on = false;                              // (set_pack_on)
			std::vector<std::pair<uint32_t, uint32_t>> pack_asks; // (id, serial: textures to ask the pack for)
			std::unordered_map<uint32_t, uint32_t> pack_by_ticket; // (a held ticket's slot -> its texture)
			double pack_now = 0.0;
			std::vector<uint32_t> held_slots;                  // (test::hold_slots)
		};
		state_all g;
		std::atomic<bool> g_detail { options::kDetailByDefault };
		std::atomic<bool> g_pack_compare { false };
		std::atomic<pack_drop_fn> g_pack_drop { nullptr };

		// What's been let go under the lock, handed over once it's left
		struct dead_things
		{
			std::vector<IUnknown *> objects;
			std::vector<uint32_t> slots;
			std::vector<hifi::pack::stream::ticket> tickets;
			uint64_t generation = 0;
		};
		// (the mutex held)
		dead_things take_dead()
		{
			dead_things d;
			d.objects.swap(g.dead);
			d.slots.swap(g.slots_dead);
			d.tickets.swap(g.tickets_dead);
			d.generation = g.heap_generation;
			return d;
		}
		// (lists recorded this frame or before may still draw them, or read the views in a pair: let go behind the GPU,
		// not at once; a pair back to free_slots only then, and only into the heap it came from. A pack ticket to the drop
		// hook: the streamer hands its texture to retire::later at the last drop, the same fence and +2 as the pair)
		void bury(dead_things &&d)
		{
			if (!d.tickets.empty())
				if (const pack_drop_fn drop = g_pack_drop.load())
					for (const hifi::pack::stream::ticket &t : d.tickets)
						drop(t);
			for (IUnknown *p : d.objects)
				retire::later(p);
			if (!d.slots.empty())
				retire::then([slots = std::move(d.slots), generation = d.generation]() {
					std::lock_guard<std::mutex> lock(g.mutex);
					if (g.heap_generation != generation)
						return;
					g.free_slots.insert(g.free_slots.end(), slots.begin(), slots.end());
					g.n.slots_retiring -= std::min(g.n.slots_retiring, uint32_t(slots.size()));
					g.wake.notify_one(); // (decoded entries may be waiting for a pair)
				});
		}
		template <typename T>
		void later(T *&p)
		{
			if (p != nullptr)
				g.dead.push_back(p);
			p = nullptr;
		}

		// (under the lock) An entry's GPU things let go, its pair of descriptors with them
		void release_slot(uint32_t &slot)
		{
			if (slot != kNoSlot)
			{
				g.slots_dead.push_back(slot);
				g.n.slots_retiring++;
			}
			slot = kNoSlot;
		}
		void release_mesh(mesh_entry &e)
		{
			later(e.vb);
			later(e.cb);
			release_slot(e.slot);
		}
		// (under the lock) A texture's pack part let go: its pair behind the GPU, its ticket to the drop hook (both once the
		// mutex is left), none again. Whatever its state: an asked ticket is dropped wherever the streamer has it (drop never
		// blocks; what only the copy queue saw the worker lets go behind its own fence)
		void release_pack(texture_entry &e)
		{
			if (e.pack_slot != kNoSlot)
				g.n.pack_pairs -= std::min(g.n.pack_pairs, 1u);
			release_slot(e.pack_slot);
			if (e.ticket.generation != 0)
			{
				const auto it = g.pack_by_ticket.find(e.ticket.slot);
				if (it != g.pack_by_ticket.end() && it->second == e.id)
					g.pack_by_ticket.erase(it);
				g.tickets_dead.push_back(e.ticket);
				g.n.pack_dropped++;
			}
			e.ticket = hifi::pack::stream::ticket {};
			e.ps = pack_state::none;
			e.pack_bytes = 0;
		}
		void release_texture(texture_entry &e)
		{
			later(e.t);
			later(e.d);
			release_slot(e.slot);
			release_pack(e);
		}
		// (under the lock) A texture on the GPU with a key and no pack part onto the asks' list, while pack parts are wanted
		void pack_ask_later(const texture_entry &e)
		{
			if (g.pack_on && e.s == state::ready && e.pack.bytes != 0 && e.ps == pack_state::none)
				g.pack_asks.emplace_back(e.id, e.serial);
		}
		// (under the lock) The texture a ticket of the cache's is held by (nullptr: none; a ticket let go isn't anyone's)
		texture_entry *pack_holder(hifi::pack::stream::ticket t)
		{
			const auto it = g.pack_by_ticket.find(t.slot);
			if (it == g.pack_by_ticket.end() || it->second >= g.textures.size() || !g.textures[it->second])
				return nullptr;
			texture_entry &e = *g.textures[it->second];
			return e.ticket == t ? &e : nullptr;
		}
		// (under the lock) A texture's decoded bytes no longer held (on the GPU, failed or let go)
		void drop_decoded(texture_entry &e)
		{
			g.decoded_bytes -= std::min(g.decoded_bytes, e.decoded_bytes);
			e.decoded_bytes = 0;
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
			g.heap_generation++;
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

		// (the worker, outside the lock) Decode one entry: whether it could be (its state is set under the lock after:
		// until then the entry is the worker's)
		bool decode_mesh(mesh_entry &e, double &ms)
		{
			const double t0 = seconds();
			os2::sections s;
			std::string why;
			const bool ok = (e.base != 0 ? os2::sections_of_live(e.raw.data(), e.raw.size(), e.base, s) : os2::sections_of_chunk(e.raw.data(), e.raw.size(), s)) &&
				os2::decode(e.raw.data(), e.raw.size(), s, e.decoded, why);
			if (!ok)
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				g.n.last_failure = "a mesh: " + (why.empty() ? std::string("its header") : why);
			}
			e.vertices = uint32_t(e.decoded.vertices.size());
			e.corners = uint32_t(e.decoded.corners.size());
			// (as upload() lays it out: the vertices, then the corners, each 16-aligned)
			e.upload_bytes = align_up(uint64_t(e.vertices) * sizeof(os2::vertex), 16) + align_up(uint64_t(e.corners) * sizeof(os2::corner), 16);
			if (ok)
				e.blade = os2::blade_of(e.decoded, e.weapon_slot);
			std::vector<uint8_t>().swap(e.raw);
			ms += (seconds() - t0) * 1000.0;
			return ok;
		}
		bool decode_texture(texture_entry &e, double &ms)
		{
			const double t0 = seconds();
			std::string why;
			const bool ok = tex::decode(e.raw.data(), e.raw.size(), e.decoded, why);
			if (ok)
			{
				// (the texture pack's key: the chunk's bytes as the game holds them, before they're freed. About 20 us a
				// texture; read by the game's thread only once the entry is marked, under the mutex)
				e.pack = hifi::pack::key_of(e.raw.data(), e.raw.size());
				tex::make_mips(e.decoded);
				if (e.with_detail)
					tex::make_detail(e.decoded, e.detail);
			}
			if (!ok)
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				g.n.last_failure = "a texture: " + why;
			}
			// Its bytes now, and its share of an upload buffer as GetCopyableFootprints lays it out (rows 256-aligned, each
			// level 512-aligned): counted on the CPU, no device call under the lock when batches are picked
			e.decoded_bytes = e.upload_bytes = 0;
			for (const tex::texture *t : { &e.decoded, &e.detail })
				for (const tex::level &lv : t->levels)
				{
					e.decoded_bytes += lv.rgba.size();
					e.upload_bytes += align_up(align_up(uint64_t(lv.w) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) * lv.h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
				}
			std::vector<uint8_t>().swap(e.raw);
			ms += (seconds() - t0) * 1000.0;
			return ok;
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
			// (each texture's colour, then its detail map: pictures 2i and 2i + 1)
			std::vector<std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>> spots(2 * b.textures.size());
			std::vector<std::vector<UINT>> rows(2 * b.textures.size());
			std::vector<std::vector<UINT64>> row_bytes(2 * b.textures.size());
			auto picture = [&](size_t i) -> const tex::texture & { return i % 2 == 0 ? b.textures[i / 2]->decoded : b.textures[i / 2]->detail; };
			for (size_t i = 0; i < spots.size(); ++i)
			{
				if (picture(i).levels.empty())
					continue;
				const D3D12_RESOURCE_DESC d = texture_desc(picture(i));
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
				if (!e->detail.levels.empty())
				{
					const D3D12_RESOURCE_DESC dd = texture_desc(e->detail);
					ok = ok && SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&e->d)));
					e->gpu_bytes += device->GetResourceAllocationInfo(0, 1, &dd).SizeInBytes;
				}
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
			for (size_t i = 0; i < spots.size(); ++i)
			{
				const tex::texture &t = picture(i);
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
			for (size_t i = 0; i < spots.size(); ++i)
				for (UINT l = 0; l < UINT(spots[i].size()); ++l)
				{
					D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
					dst.pResource = i % 2 == 0 ? b.textures[i / 2]->t : b.textures[i / 2]->d;
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
				uint64_t batch_bytes = 0, planned = 0;
				bool full = false, held = false;
				const bool gpu = make_gpu(lock);
				const bool detail = g_detail.load();
				// (a decoded entry into the batch by its upload bytes: one that doesn't fit waits for the next, one bigger
				// than a whole batch goes alone)
				const auto take = [&](uint64_t bytes) {
					const bool empty = b.meshes.empty() && b.textures.empty();
					if (!gpu || full || g.free_slots.empty() || (!empty && batch_bytes + bytes > kBatchBytes))
						return false;
					batch_bytes += bytes;
					full = batch_bytes >= kBatchBytes;
					return true;
				};
				// (textures first: while a big one is held decoded nothing else is, so it's the only one left the next time
				// round, however many meshes come meanwhile. One held back waits for nothing else decoded and then goes
				// alone, before any other: textures arriving meanwhile, decoded beside the batches being uploaded, would
				// otherwise keep it back for as long as they come)
				if (g.waiting != kNone && (g.waiting >= g.textures.size() || !g.textures[g.waiting] || g.textures[g.waiting]->s != state::queued))
					g.waiting = kNone;
				for (uint32_t i = 0; i < g.textures.size(); ++i)
					if (const auto &e = g.textures[i])
					{
						if (e->s == state::queued)
						{
							// Decoded only as far as the next batch: what's decoded and not on the GPU yet, and what's planned
							// now, within kBatchBytes (one at least when there's none); one over kEntryCap never (a guard: the
							// colour alone is 85 MB at most, tex::dimensions taking 4096 x 4096 at most). With its detail map,
							// make_detail's own buffers count too while it runs: over the cap with them, it goes without one
							// (better than its models left to the game)
							uint32_t w = 0, h = 0;
							const bool known = tex::dimensions(e->raw.data(), e->raw.size(), w, h);
							const bool with_detail = detail && known && planned_bytes(w, h, true) + tex::detail_work_bytes(w, h) <= kEntryCap;
							const uint64_t bytes = known ? planned_bytes(w, h, with_detail) : 0;
							if (bytes > kEntryCap)
							{
								std::vector<uint8_t>().swap(e->raw);
								e->s = state::failed;
								g.n.too_big++;
								g.n.last_failure = "a texture: too big (" + std::to_string(w) + " x " + std::to_string(h) + ")";
								if (g.waiting == i)
									g.waiting = kNone;
							}
							else if (g.waiting != kNone && g.waiting != i)
								held = true;
							else if (g.decoded_bytes + planned + bytes <= kBatchBytes || (g.decoded_bytes == 0 && decode_t.empty()))
							{
								e->with_detail = with_detail;
								g.n.detail_skipped += detail && known && !with_detail;
								decode_t.push_back(e.get());
								planned += bytes;
								if (g.waiting == i)
									g.waiting = kNone;
							}
							else
							{
								held = true;
								if (g.waiting == kNone)
									g.waiting = i;
							}
						}
						else if (e->s == state::decoded && take(e->upload_bytes))
						{
							e->s = state::uploading;
							e->slot = g.free_slots.back();
							g.free_slots.pop_back();
							b.textures.push_back(e.get());
						}
					}
				// (meshes are small: all decoded as they come)
				for (auto &e : g.meshes)
					if (e)
					{
						if (e->s == state::queued)
							decode_m.push_back(e.get());
						else if (e->s == state::decoded && take(e->upload_bytes))
						{
							e->s = state::uploading;
							e->slot = g.free_slots.back();
							g.free_slots.pop_back();
							b.meshes.push_back(e.get());
						}
					}
				if (decode_m.empty() && decode_t.empty() && b.meshes.empty() && b.textures.empty())
				{
					g.busy = false;
					g.held = held;
					g.idle.notify_all();
					if (g.quit)
						return;
					if (dead_things d = take_dead(); !d.objects.empty() || !d.slots.empty())
					{
						lock.unlock();
						bury(std::move(d));
						lock.lock();
						continue; // (the lock was left: a wake meanwhile would be missed, so look again first)
					}
					g.wake.wait(lock);
					continue;
				}
				g.busy = true;
				// (the entries picked are the worker's until their state changes: the game's thread doesn't touch an
				// entry's decoded data or resources, and forgets only entries that are ready, failed, decoded or still queued)
				for (mesh_entry *e : decode_m)
					e->s = state::uploading; // (held while decoding: not forgotten under us)
				for (texture_entry *e : decode_t)
					e->s = state::uploading;
				lock.unlock();
				double decode_ms = 0.0, upload_ms = 0.0;
				// (anything thrown, out of memory in the 32-bit game: that entry failed, the thread goes on; an exception
				// leaving a thread ends the game)
				const auto thrown = [](const char *what) {
					std::lock_guard<std::mutex> failure(g.mutex);
					g.n.last_failure = std::string(what) + ": out of memory (or the decoder threw)";
				};
				std::vector<char> mesh_ok(decode_m.size(), 0), texture_ok(decode_t.size(), 0);
				for (size_t i = 0; i < decode_m.size(); ++i)
					try
					{
						mesh_ok[i] = decode_mesh(*decode_m[i], decode_ms);
					}
					catch (...)
					{
						mesh_entry *e = decode_m[i];
						e->decoded = os2::mesh();
						std::vector<uint8_t>().swap(e->raw);
						e->vertices = e->corners = 0;
						thrown("a mesh");
					}
				for (size_t i = 0; i < decode_t.size(); ++i)
					try
					{
						texture_ok[i] = decode_texture(*decode_t[i], decode_ms);
					}
					catch (...)
					{
						texture_entry *e = decode_t[i];
						e->decoded = tex::texture();
						e->detail = tex::texture();
						e->pack = hifi::pack::key {};
						std::vector<uint8_t>().swap(e->raw);
						thrown("a texture");
					}
				std::string why;
				bool uploaded = b.meshes.empty() && b.textures.empty();
				if (!uploaded)
					try
					{
						uploaded = upload(b, upload_ms, why);
					}
					catch (...)
					{
						why = "an upload: out of memory (or it threw)";
					}
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
						// (the detail map beside it, read through the same table; a flat one's view (nullptr) when none)
						v.Texture2D.MipLevels = e->d != nullptr ? UINT(e->detail.levels.size()) : 1;
						g.device->CreateShaderResourceView(e->d, &v, cpu_descriptor(2 * e->slot + 1));
					}
				}
				lock.lock();
				g.n.decode_ms += decode_ms;
				g.n.upload_ms += upload_ms;
				// (what was decoded is decoded or failed only now, under the lock: the game's thread forgets decoded entries)
				for (size_t i = 0; i < decode_m.size(); ++i)
					decode_m[i]->s = mesh_ok[i] ? state::decoded : state::failed;
				for (size_t i = 0; i < decode_t.size(); ++i)
				{
					texture_entry *e = decode_t[i];
					e->s = texture_ok[i] ? state::decoded : state::failed;
					if (texture_ok[i])
					{
						g.decoded_bytes += e->decoded_bytes;
						g.n.decoded_peak = std::max(g.n.decoded_peak, g.decoded_bytes);
					}
					else
						e->decoded_bytes = 0;
				}
				if (uploaded && !(b.meshes.empty() && b.textures.empty()))
				{
					g.n.batches++;
					g.n.batch_bytes_max = std::max(g.n.batch_bytes_max, batch_bytes);
				}
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
					// (its levels let go either way: on the GPU now, or failed for good)
					drop_decoded(*e);
					e->decoded.levels.clear();
					e->decoded.levels.shrink_to_fit();
					e->detail.levels.clear();
					e->detail.levels.shrink_to_fit();
					if (uploaded)
					{
						e->s = state::ready;
						g.n.gpu_bytes += e->gpu_bytes;
						pack_ask_later(*e);
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
			drop_decoded(e);
			if (e.s == state::ready)
				g.n.gpu_bytes -= std::min(g.n.gpu_bytes, e.gpu_bytes);
			auto it = g.texture_of.find(e.k.resource);
			if (it != g.texture_of.end() && it->second == id)
				g.texture_of.erase(it);
			g.textures[id].reset();
			g.free_textures.push_back(id);
			g.n.forgotten++;
		}
		// (an entry the worker holds isn't forgotten: decoding or uploading, both marked uploading while it holds them; one
		// still queued is the cache's alone: the batch may hold it back a while, and it goes if it's no longer named)
		bool forgettable(state s)
		{
			return s == state::ready || s == state::failed || s == state::decoded || s == state::queued;
		}
	}

	uint32_t mesh(const key &k)
	{
		struct buried
		{
			dead_things d;
			~buried() { bury(std::move(d)); }
		} graves;
		dead_things &dead = graves.d;
		std::lock_guard<std::mutex> lock(g.mutex);
		struct at_exit
		{
			dead_things &d;
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
			dead_things d;
			~buried() { bury(std::move(d)); }
		} graves;
		std::lock_guard<std::mutex> lock(g.mutex);
		struct at_exit
		{
			dead_things &d;
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
		g.held = false;
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
		e->serial = ++g.next_serial;
		g.n.bytes_in += e->raw.size();
		const uint32_t id = add(g.textures, g.free_textures, g.texture_of, std::move(e));
		g.textures[id]->id = id;
		start_worker();
		g.held = false; // (settle: the worker looks again before it counts as idle)
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
		dead_things dead;
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
				g.held = false;
				g.wake.notify_one();
			}
			// What hasn't been named for a while
			const uint32_t forgotten = g.n.forgotten;
			for (uint32_t i = 0; i < g.meshes.size(); ++i)
				if (g.meshes[i] && now - g.meshes[i]->seen > kForgetSeconds && forgettable(g.meshes[i]->s))
					forget_mesh(i);
			for (uint32_t i = 0; i < g.textures.size(); ++i)
				if (g.textures[i] && now - g.textures[i]->seen > kForgetSeconds && forgettable(g.textures[i]->s))
					forget_texture(i);
			if (g.n.forgotten != forgotten)
			{
				g.held = false; // (decoded ones let go make room in the batch for those held back)
				g.wake.notify_one();
			}
			dead = take_dead();
		}
		if (buffer != nullptr)
			buffer->Release(); // (made twice at once: not ours)
		bury(std::move(dead));
	}

	void set_detail(bool on)
	{
		g_detail.store(on);
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
		// (a texture that couldn't be read leaves the model to the game, which draws it as it can: never ready; one not
		// found at all, kNone, is drawn without)
		for (uint32_t t : g.meshes[id]->textures)
			if (t != kNone && (t >= g.textures.size() || !g.textures[t] || g.textures[t]->s != state::ready))
				return false;
		return true;
	}

	bool texture_view_of(uint32_t id, D3D12_GPU_DESCRIPTOR_HANDLE &out, bool allow_pack, bool *is_pack)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (is_pack != nullptr)
			*is_pack = false;
		if (id >= g.textures.size() || !g.textures[id] || g.textures[id]->s != state::ready)
			return false;
		texture_entry &e = *g.textures[id];
		// (the pack's pair only once it's written and its ticket held ready: nothing of the streamer's is read here)
		const bool pack = allow_pack && e.ps == pack_state::ready && e.pack_slot != kNoSlot && !g_pack_compare.load(std::memory_order_relaxed);
		out = g.heap->GetGPUDescriptorHandleForHeapStart();
		out.ptr += uint64_t(2 * (pack ? e.pack_slot : e.slot)) * g.step;
		if (pack)
		{
			e.pack_drawn = g.pack_now;
			if (is_pack != nullptr)
				*is_pack = true;
		}
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
				bool left = false;
				for (uint32_t t : e->textures)
					left = left || (t != kNone && t < g.textures.size() && g.textures[t] && g.textures[t]->s == state::failed);
				c.meshes_left_failed += left;
			}
		for (auto &e : g.textures)
			if (e)
			{
				c.textures++;
				c.textures_ready += e->s == state::ready;
				c.textures_failed += e->s == state::failed;
				c.detail_textures += e->s == state::ready && e->d != nullptr;
				if (e->s != state::ready)
					continue;
				// (the pack's part: a texture with no key counts as not in it)
				c.pack_keyed += e->pack.bytes != 0;
				c.pack_ready += e->ps == pack_state::ready;
				c.pack_bytes += e->ps == pack_state::ready ? e->pack_bytes : 0;
				c.pack_asked += e->ps == pack_state::asked || e->ps == pack_state::writing;
				c.pack_not_in += e->pack.bytes == 0 || (e->ps == pack_state::spent && e->spent_why == pack_miss::not_in_pack);
				c.pack_failed += e->ps == pack_state::spent && e->spent_why == pack_miss::failed;
				c.pack_gone += e->ps == pack_state::spent && e->spent_why == pack_miss::gone;
				c.pack_evicted += e->ps == pack_state::evicted;
			}
		c.decoded_bytes = g.decoded_bytes;
		c.slots_free = uint32_t(g.free_slots.size());
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
		std::snprintf(line, sizeof(line), "character data: textures decoded now %.1f MB (peak %.1f), %u batches (largest %.1f MB), %u too big, %u with a detail map; "
			"%u decoded without one (too big for it); %u descriptor pairs free (%u waiting for the GPU); %u meshes left to the game (a texture couldn't be read)",
			double(c.decoded_bytes) / 1048576.0, double(c.decoded_peak) / 1048576.0, c.batches, double(c.batch_bytes_max) / 1048576.0, c.too_big, c.detail_textures,
			c.detail_skipped, c.slots_free, c.slots_retiring, c.meshes_left_failed);
		out.push_back(line);
		std::snprintf(line, sizeof(line), "character data: texture pack: %u ready (%.1f MB), %u asked, %u not in it, %u failed, %u gone, %u evicted (%llu in all, the video "
			"memory over 95%%); %llu skipped for want of descriptors (%u waiting); %u pairs for pack views; %llu tickets let go",
			c.pack_ready, double(c.pack_bytes) / 1048576.0, c.pack_asked, c.pack_not_in, c.pack_failed, c.pack_gone, c.pack_evicted,
			static_cast<unsigned long long>(c.pack_evictions), static_cast<unsigned long long>(c.pack_skipped_slots), c.pack_waiting_slots, c.pack_pairs,
			static_cast<unsigned long long>(c.pack_dropped));
		out.push_back(line);
		return out;
	}

	// ---- The texture pack's part (cache.hpp; src/chars/charpack.cpp drives it on the game's thread)

	namespace
	{
		// (each call that may let go of a pack part: what it let go buried once the mutex is left, as mesh() and texture()
		// do; constructed before the lock, so destroyed after it)
		struct pack_graves
		{
			dead_things d;
			~pack_graves() { bury(std::move(d)); }
		};
	}

	void set_pack_drop(pack_drop_fn fn)
	{
		g_pack_drop.store(fn);
	}

	void set_pack_on(bool on)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (on == g.pack_on)
			return;
		g.pack_on = on;
		g.pack_asks.clear();
		for (const auto &e : g.textures)
			if (e)
				pack_ask_later(*e);
	}

	void forget_pack_textures()
	{
		pack_graves dead;
		std::lock_guard<std::mutex> lock(g.mutex);
		g.pack_asks.clear();
		for (const auto &e : g.textures)
			if (e && (e->ps != pack_state::none || e->ticket.generation != 0 || e->pack_slot != kNoSlot))
				release_pack(*e);
		// (none again: asked again in the next epoch while wanted)
		for (const auto &e : g.textures)
			if (e)
				pack_ask_later(*e);
		dead.d = take_dead();
	}

	void set_pack_compare(bool on)
	{
		g_pack_compare.store(on);
	}

	void pack_tick(double now)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.pack_now = now;
	}

	ID3D12Device *device()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		return g.device;
	}

	void take_pack_asks(uint32_t max, std::vector<pack_ask> &out)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.n.pack_waiting_slots = 0;
		if (g.heap == nullptr || g.free_slots.size() <= kPackSlotFloor)
		{
			// (held: the game's own textures and the meshes keep the pairs left)
			if (g.heap != nullptr)
				g.n.pack_waiting_slots = uint32_t(g.pack_asks.size());
			return;
		}
		// (no more than there are pairs to spare over the floor, less the ones already on their way: each ready ticket takes
		// one, and one past them would be read, uploaded and let go for nothing in pack_begin. A walk only while asks wait)
		if (g.pack_asks.empty())
			return;
		size_t coming = 0;
		for (const auto &e : g.textures)
			coming += e && (e->ps == pack_state::asked || e->ps == pack_state::writing);
		const size_t spare = g.free_slots.size() - kPackSlotFloor;
		if (coming >= spare)
		{
			g.n.pack_waiting_slots = uint32_t(g.pack_asks.size());
			return;
		}
		max = uint32_t(std::min<size_t>(max, spare - coming));
		const size_t first = out.size();
		size_t k = 0;
		for (; k < g.pack_asks.size() && out.size() - first < max; ++k)
		{
			const auto [id, serial] = g.pack_asks[k];
			if (id >= g.textures.size() || !g.textures[id])
				continue;
			const texture_entry &e = *g.textures[id];
			if (e.serial != serial || e.s != state::ready || e.ps != pack_state::none || e.pack.bytes == 0)
				continue;
			bool twice = false;
			for (size_t j = first; j < out.size() && !twice; ++j)
				twice = out[j].id == id;
			if (twice)
				continue;
			pack_ask a;
			a.id = id;
			a.serial = serial;
			a.k = e.pack;
			// (the game's own smallest level: 6 levels, or 1 with a side under 32. A 4x record then has its levels down to
			// that size, at most its stored chain; one the game draws with one level, down to the game's own size)
			const uint32_t n = tex::mip_count(e.pack.w, e.pack.h);
			a.down_to_w = e.pack.w >> (n - 1);
			a.down_to_h = e.pack.h >> (n - 1);
			out.push_back(a);
		}
		g.pack_asks.erase(g.pack_asks.begin(), g.pack_asks.begin() + ptrdiff_t(k));
	}

	bool pack_asked(uint32_t id, uint32_t serial, hifi::pack::stream::ticket t, uint32_t epoch)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (t.generation == 0 || id >= g.textures.size() || !g.textures[id])
			return false;
		texture_entry &e = *g.textures[id];
		if (e.serial != serial || e.s != state::ready || e.ps != pack_state::none)
			return false;
		e.ps = pack_state::asked;
		e.ticket = t;
		e.pack_epoch = epoch;
		g.pack_by_ticket[t.slot] = id;
		return true;
	}

	void pack_refused(uint32_t id, uint32_t serial)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (id >= g.textures.size() || !g.textures[id])
			return;
		texture_entry &e = *g.textures[id];
		if (e.serial == serial && e.ps == pack_state::none)
		{
			e.ps = pack_state::spent;
			e.spent_why = pack_miss::failed;
		}
	}

	bool pack_begin(hifi::pack::stream::ticket t, pack_writing &out)
	{
		pack_graves dead;
		std::lock_guard<std::mutex> lock(g.mutex);
		bool ok = false;
		texture_entry *const e = pack_holder(t);
		if (e != nullptr && e->ps == pack_state::asked && e->s == state::ready && g.heap != nullptr)
		{
			if (g.free_slots.size() <= kPackSlotFloor)
			{
				// (no pair to spare: the ticket let go, the texture none again, asked again once there are)
				release_pack(*e);
				g.n.pack_skipped_slots++;
				pack_ask_later(*e);
			}
			else
			{
				// (taken now, written once by the caller with the mutex left, before the state says ready: no list can be
				// reading it before then, and none is given it again until it's let go behind the GPU)
				e->pack_slot = g.free_slots.back();
				g.free_slots.pop_back();
				g.n.pack_pairs++;
				e->ps = pack_state::writing;
				out = pack_writing {};
				out.id = e->id;
				out.serial = e->serial;
				out.slot = e->pack_slot;
				out.t = t;
				out.colour = cpu_descriptor(2 * e->pack_slot);
				out.detail = cpu_descriptor(2 * e->pack_slot + 1);
				out.detail_map = e->d;
				if (out.detail_map != nullptr)
					out.detail_map->AddRef(); // (an interlocked count, no lock of the runtime's: the view is made after the mutex is left)
				ok = true;
			}
		}
		dead.d = take_dead();
		return ok;
	}

	void write_pack_detail(pack_writing &w)
	{
		ID3D12Device *device = nullptr;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			device = g.device;
		}
		if (device != nullptr)
		{
			// (a second view of the game texture's own detail map, its levels from the resource (the decoded ones are freed
			// after the upload); or the null view the game's pair has. Never CopyDescriptors: the heap is shader visible,
			// and a copy's source must be a CPU-only heap)
			D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
			v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			v.Texture2D.MipLevels = w.detail_map != nullptr ? UINT(w.detail_map->GetDesc().MipLevels) : 1;
			device->CreateShaderResourceView(w.detail_map, &v, w.detail);
		}
		if (w.detail_map != nullptr)
			w.detail_map->Release();
		w.detail_map = nullptr;
	}

	void pack_end(const pack_writing &w, bool ok, uint64_t bytes)
	{
		pack_graves dead;
		std::lock_guard<std::mutex> lock(g.mutex);
		if (w.id < g.textures.size() && g.textures[w.id])
		{
			texture_entry &e = *g.textures[w.id];
			if (e.serial == w.serial && e.ps == pack_state::writing && e.ticket == w.t && e.pack_slot == w.slot)
			{
				if (ok)
				{
					e.ps = pack_state::ready;
					e.pack_bytes = bytes;
					e.pack_drawn = g.pack_now;
				}
				else
				{
					release_pack(e);
					e.ps = pack_state::spent;
					e.spent_why = pack_miss::failed;
				}
			}
		}
		dead.d = take_dead();
	}

	void pack_spent(hifi::pack::stream::ticket t, pack_miss why)
	{
		pack_graves dead;
		std::lock_guard<std::mutex> lock(g.mutex);
		// (only one still on its way: a ready texture's ticket turned gone keeps its texture till it's let go, zones.md
		// decision 1, and the module forgets every pack part before it ends an epoch)
		if (texture_entry *const e = pack_holder(t); e != nullptr && e->ps == pack_state::asked)
		{
			release_pack(*e);
			e->ps = pack_state::spent;
			e->spent_why = why;
		}
		dead.d = take_dead();
	}

	uint32_t pack_evict(uint64_t max_bytes, uint64_t &bytes)
	{
		pack_graves dead;
		std::lock_guard<std::mutex> lock(g.mutex);
		bytes = 0;
		// (the ready ones, the longest since a model drew them first: texture_view_of stamps pack_drawn as it hands the
		// pair out. A walk every half second while the card is nearly full: a few thousand entries at most)
		std::vector<std::pair<double, uint32_t>> ready;
		for (const auto &e : g.textures)
			if (e && e->ps == pack_state::ready)
				ready.emplace_back(e->pack_drawn, e->id);
		std::sort(ready.begin(), ready.end());
		uint32_t n = 0;
		for (const auto &[drawn, id] : ready)
		{
			if (n != 0 && bytes >= max_bytes)
				break;
			texture_entry &e = *g.textures[id];
			const uint64_t view_bytes = e.pack_bytes;
			bytes += view_bytes;
			// (the pair behind the GPU and the ticket to the drop hook, as any let go: the lists recorded with it are behind
			// the same fence; the streamer's texture goes at its last ticket, a texture shared with a zone stays. Its bytes
			// kept, for pack_unevict's pace)
			release_pack(e);
			e.ps = pack_state::evicted;
			e.pack_bytes = view_bytes;
			n++;
		}
		g.n.pack_evictions += n;
		dead.d = take_dead();
		return n;
	}

	uint32_t pack_unevict(uint64_t max_bytes, uint32_t &left)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		// (the ones a model drew last first: the likeliest to be on screen still)
		std::vector<std::pair<double, uint32_t>> evicted;
		for (const auto &e : g.textures)
			if (e && e->ps == pack_state::evicted)
				evicted.emplace_back(e->pack_drawn, e->id);
		std::sort(evicted.begin(), evicted.end(), [](const auto &a, const auto &b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
		uint32_t n = 0;
		uint64_t bytes = 0;
		for (const auto &[drawn, id] : evicted)
		{
			if (n != 0 && bytes >= max_bytes)
				break;
			texture_entry &e = *g.textures[id];
			bytes += e.pack_bytes;
			e.pack_bytes = 0;
			e.ps = pack_state::none;
			pack_ask_later(e);
			n++;
		}
		left = uint32_t(evicted.size()) - n;
		return n;
	}

	namespace test
	{
		hifi::pack::key pack_key_of(uint32_t id)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			// (only once the worker has let go of it: it writes the key outside the lock)
			if (id >= g.textures.size() || !g.textures[id] || g.textures[id]->s == state::queued || g.textures[id]->s == state::uploading)
				return hifi::pack::key {};
			return g.textures[id]->pack;
		}

		const char *pack_state_of(uint32_t id)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			if (id >= g.textures.size() || !g.textures[id])
				return "";
			static const char *const names[] = { "none", "asked", "writing", "ready", "spent", "evicted" };
			return names[size_t(g.textures[id]->ps)];
		}

		hifi::pack::stream::ticket pack_ticket_of(uint32_t id)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			return id < g.textures.size() && g.textures[id] ? g.textures[id]->ticket : hifi::pack::stream::ticket {};
		}

		uint32_t pack_slot_of(uint32_t id)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			return id < g.textures.size() && g.textures[id] && g.textures[id]->pack_slot != kNoSlot ? g.textures[id]->pack_slot : kNone;
		}

		uint32_t game_slot_of(uint32_t id)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			return id < g.textures.size() && g.textures[id] && g.textures[id]->slot != kNoSlot ? g.textures[id]->slot : kNone;
		}

		void hold_slots(uint32_t leave_free)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			while (g.free_slots.size() > leave_free)
			{
				g.held_slots.push_back(g.free_slots.back());
				g.free_slots.pop_back();
			}
		}

		void give_back_slots()
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.free_slots.insert(g.free_slots.end(), g.held_slots.begin(), g.held_slots.end());
			g.held_slots.clear();
			g.wake.notify_one();
		}

		bool slot_free(uint32_t slot)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			return std::find(g.free_slots.begin(), g.free_slots.end(), slot) != g.free_slots.end();
		}

		bool slots_consistent(std::string &why)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			std::vector<uint8_t> uses(kDescriptors / 2, 0);
			bool ok = true;
			auto use = [&](uint32_t s, const char *what) {
				if (s == kNoSlot)
					return;
				if (s >= uses.size() || ++uses[s] > 1)
				{
					if (ok)
						why = std::string("pair ") + std::to_string(s) + " twice (" + what + ")";
					ok = false;
				}
			};
			for (uint32_t s : g.free_slots)
				use(s, "free");
			for (uint32_t s : g.held_slots)
				use(s, "held by the test");
			for (const auto &e : g.meshes)
				if (e)
					use(e->slot, "a mesh's");
			for (const auto &e : g.textures)
				if (e)
				{
					use(e->slot, "a texture's");
					use(e->pack_slot, "a texture's pack pair");
				}
			return ok;
		}
	}

	bool settle(uint32_t ms)
	{
		std::unique_lock<std::mutex> lock(g.mutex);
		return g.idle.wait_for(lock, std::chrono::milliseconds(ms), [] {
			if (g.busy)
				return false;
			// (decoded with a device: to be uploaded, unless the GPU side couldn't be made; queued: to be decoded, unless the
			// worker waits with them held back by a batch it has nowhere to upload)
			const bool uploads = g.device != nullptr && g.gpu_failure.empty();
			for (auto &e : g.meshes)
				if (e && (e->s == state::queued || (e->s == state::decoded && uploads)))
					return false;
			for (auto &e : g.textures)
				if (e && ((e->s == state::queued && !g.held) || (e->s == state::decoded && uploads)))
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
		dead_things dead;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			for (auto &e : g.meshes)
				if (e)
					release_mesh(*e);
			for (auto &e : g.textures)
				if (e)
					release_texture(*e);
			g.slots_dead.clear(); // (the heap goes too; pairs let go before are left out by the generation)
			g.heap_generation++;
			// (the pack's part: every ticket went to tickets_dead above, for the drop hook; the module's clear_all has
			// dropped them all before this in the plugin's Release, so the stopped streamer frees nothing twice)
			g.pack_on = false;
			g.pack_asks.clear();
			g.pack_by_ticket.clear();
			g.held_slots.clear();
			g.pack_now = 0.0;
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
			g.held = false;
			g.decoded_bytes = 0;
			g.waiting = kNone;
			g.quit = false;
			g.now = 0.0;
			g.n = counts();
			g.gpu_failure.clear();
			dead = take_dead();
		}
		bury(std::move(dead));
	}
}
