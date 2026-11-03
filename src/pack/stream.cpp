/*
 * The texture pack's streamer (stream.hpp; docs/texture-pack/streamer.md is the design, its measurements and why).
 *
 * The callers' side (request, drop, poll, get, take_done, the owners, limits and pause bits) is a table of tickets and a
 * map of resident textures under one mutex, `g.mutex`, a leaf: nothing in this file calls Direct3D 12, DXGI, ReadFile,
 * pack::load or retire while holding it (but write_view's AddRef, an interlocked count), and nothing calls back out of
 * it (docs/d3d12/threads-pso.md). What's let go under it is collected and handed to retire::later once it's left (the
 * characters' dead_things pattern).
 *
 * The worker's side (worker() and what it calls: the copy queue, its fence, the allocators and lists, the ring, the
 * reader) belongs to the worker thread alone, in a `gpu` it keeps on its stack: those objects are touched by no other
 * thread, so they need no lock. It takes the lock only to pick a ticket and to publish what it did.
 *
 * Each ticket's state is one atomic word, generation << 8 | state, in chunks that are never freed: poll() reads it with
 * no lock (an acquire load), and the worker stores ready (a release store, under the lock) only once it has read the
 * copy fence past that texture's copy.
 */

#include <windows.h>
#include "stream.hpp"
#include "stream_crumbs.hpp"
#include "validate.hpp"
#include "../host/retire.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace hifi::pack::stream
{
	namespace
	{
		namespace sc = stream_crumbs;

		// The ticket table: slots in chunks of kChunk words, at most kChunks chunks (4 M tickets alive or freed and not
		// yet reused); a chunk, once made, is never freed (poll reads it with no lock)
		constexpr uint32_t kChunk = 1024, kChunks = 4096;
		constexpr uint32_t kGenerationMask = 0xFFFFFF;
		// Freed slots are reused only once this many are waiting: a dropped ticket reads "dropped" for a while, not
		// "gone" at once
		constexpr size_t kReuseAfter = 4096;
		constexpr uint64_t kAlign = kLevelAlign;
		constexpr DWORD kWaitMs = 1000, kStopWaitMs = 50, kDeviceWaitMs = 1000;

		std::atomic<std::atomic<uint32_t> *> g_words[kChunks];
		std::atomic<uint64_t> g_presents { 0 };
		// (set by stop(): an open loading pack.idx on the worker gives up between pieces; cleared by start)
		std::atomic<bool> g_cancel_open { false };
		// (tests only: test::before_pick)
		std::atomic<void (*)()> g_before_pick { nullptr };
		// (tests only: test::after_load)
		std::atomic<void (*)()> g_after_load { nullptr };
		// (tests only: test::fail_parts)
		std::atomic<const char *> g_fail_parts { nullptr };

		uint64_t round_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
		unsigned long long megabytes(uint64_t bytes) { return static_cast<unsigned long long>((bytes + 524288) / 1048576); }

		// What a resident texture is known by: the key, the kind and the levels asked (count as asked once "all the rest"
		// is known from the record)
		struct rkey
		{
			uint64_t hash = 0;
			uint32_t bytes = 0, w = 0, h = 0;
			uint8_t kind = 0, first = 0, count = 0;
			bool operator==(const rkey &) const = default;
		};
		struct rkey_hash
		{
			size_t operator()(const rkey &k) const
			{
				return size_t(k.hash ^ (k.hash >> 32) ^ (uint64_t(k.first) << 8) ^ (uint64_t(k.count) << 16) ^ (uint64_t(k.kind) << 24));
			}
		};

		// A texture: being made and copied (one job of the worker's holds it), or ready and shared by the tickets that
		// hold it. Taken out of the map (in_map false) by an epoch change while its job still runs: the job then lets go
		// of it alone. A ready one taken out by an open, close or reload is held (g.held) for the tickets that held it,
		// gone now, until the last of them is dropped (zones.md decision 1)
		struct resident
		{
			rkey k;
			enum phase_t : uint8_t
			{
				making,
				copying,
				ready,
			} phase = making;
			bool in_map = true;
			bool held = false; // (out of the map, kept for gone tickets: g.held)
			ID3D12Resource *res = nullptr;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			uint32_t w = 0, h = 0, levels = 0, first = 0;
			uint64_t bytes = 0;
			uint32_t refs = 0;             // (ready tickets holding it)
			std::vector<uint32_t> waiters; // (tickets waiting for it while it's made and copied)
			uint64_t asked_at = 0;         // (the present it was last asked for)
		};

		struct slot_data
		{
			bool live = false;
			uint32_t gen = 0;
			owner o = 0;
			state st = state::none;
			miss why = miss::none;
			want w;      // (count as asked once "all the rest" is known)
			record rec;  // (the record found at the request)
			char name[17] = {};
			uint64_t span = 0, seq = 0;
			resident *r = nullptr; // (the texture it waits for or holds)
		};

		struct owner_rec
		{
			std::string name;
			std::unordered_set<uint32_t> slots;
			// (its live slots that became ready, missed or gone since its last take_done: a set, so a ticket is handed over
			// once however often it changed, and a slot given back leaves it: never more than the slots it holds, even for an
			// owner that only polls)
			std::unordered_set<uint32_t> done;
		};

		// One texture's way through the worker: made, its ring space, read, recorded, submitted
		struct job
		{
			resident *r = nullptr;
			record rec;
			char name[17] = {};
			std::shared_ptr<const index> snap; // (its level CRCs)
			std::shared_ptr<reader> rd;
			uint32_t first = 0, count = 0;
			uint64_t span = 0;
			uint64_t at = 0, end = 0; // (ring space [at, end): the span rounded to 512, or to the ring's end when the next
			                          // span wrapped to 0: those bytes go with this copy)
			uint64_t fence = 0;       // (0 until submitted)
			uint32_t list = 0;
			limits lim;
			uint64_t made_since = 0;
			ID3D12Fence *gate = nullptr; // (tests only: test::gate_copies)
			uint64_t gate_value = 0;
		};

		struct state_all
		{
			std::mutex mutex;
			std::thread thread;
			HANDLE wake = nullptr, stop_event = nullptr; // (made at the first start, never closed: any thread may set them)
			bool started = false, quit = false;
			ID3D12Device *device = nullptr;              // (AddRef'd: start and set_device)
			std::vector<ID3D12Device *> old_devices;     // (released by the worker once its copies on them are done)
			bool device_change = false;
			uint64_t ring_size = 0;
			std::string failure;
			// The pack
			std::shared_ptr<const index> snap;
			std::shared_ptr<reader> rd;
			int post = 0; // (1 open a folder, 2 open an index, 3 close: the worker does it)
			uint64_t posted = 0, post_done = 0; // (the opens and closes asked; the last of them the worker has done)
			std::wstring post_folder;
			std::shared_ptr<const index> post_index;
			std::string open_status;
			uint32_t epoch = 1;
			// Tickets, owners, textures
			std::vector<slot_data> slots;
			std::deque<uint32_t> free_slots;
			std::set<std::tuple<int64_t, uint64_t, uint32_t>> queue; // (-priority, order asked, slot)
			uint64_t seq = 0;
			std::unordered_map<owner, owner_rec> owners;
			owner next_owner = 1;
			std::unordered_map<rkey, resident *, rkey_hash> map;
			// (ready textures an open, close or reload took out of the map while gone tickets still hold them: each let go
			// at its last drop, or all at once by stop, a device change or loss)
			std::unordered_set<resident *> held;
			limits lim;
			uint64_t made_since = 0; // (allocation bytes made since the last set_limits: the usage lags)
			uint64_t refused_bytes = 0; // (the allocation the 95% stop last refused: set_limits keeps kVideoMemory while it won't fit)
			uint64_t presents_at_start = 0;
			ID3D12Fence *test_gate = nullptr; // (tests only: test::gate_copies; the test keeps it alive)
			uint64_t test_gate_value = 0;
			uint32_t pause = 0;
			counts n;
			std::deque<std::string> log; // (take_log's lines, at most kLogLines; how many more were left out)
			uint64_t log_dropped = 0;
			std::vector<IUnknown *> to_retire; // (let go under the lock: to retire::later once it's left)
			// (left running at exit only when stop() wasn't called: let go rather than end the process)
			~state_all()
			{
				if (thread.joinable())
					thread.detach();
			}
		};
		state_all g;

		// ---- Crumbs and words

		void crumb(uint32_t s)
		{
			sc::the.state.store(s);
			sc::the.progress_tick.store(GetTickCount64());
		}

		// (the mutex held) The counts the crash section shows
		void publish_locked()
		{
			sc::the.pause.store(g.pause);
			sc::the.epoch.store(g.epoch);
			sc::the.queued.store(uint32_t(g.queue.size()));
			sc::the.in_flight.store(uint32_t(g.n.in_flight));
			sc::the.resident.store(uint32_t(g.n.resident));
			sc::the.resident_bytes.store(g.n.resident_bytes);
			uint64_t refused = 0;
			for (uint64_t r : g.n.refused)
				refused += r;
			sc::the.refused.store(uint32_t(refused));
		}

		// (the mutex held) A line for take_log
		void log_locked(std::string line)
		{
			if (g.log.size() >= kLogLines)
			{
				g.log_dropped++;
				return;
			}
			g.log.push_back(std::move(line));
		}

		std::atomic<uint32_t> *word_of(uint32_t slot)
		{
			std::atomic<uint32_t> *chunk = g_words[slot / kChunk].load(std::memory_order_acquire);
			return chunk != nullptr ? chunk + slot % kChunk : nullptr;
		}

		// (the mutex held) A slot's state, its word stored with release (ready published after the fence was seen)
		void set_state(uint32_t s, state st)
		{
			slot_data &d = g.slots[s];
			d.st = st;
			word_of(s)->store((d.gen << 8) | uint32_t(st), std::memory_order_release);
		}

		// (the mutex held) A ticket now ready, missed or gone: its owner's take_done hands it over (once)
		void tell_owner(uint32_t s)
		{
			slot_data &d = g.slots[s];
			auto it = g.owners.find(d.o);
			if (it != g.owners.end())
				it->second.done.insert(s);
		}

		// (the mutex held) keep: a ready ticket turned gone by an open, close or reload keeps its texture (d.r) till it's
		// dropped
		void finish(uint32_t s, state st, miss why = miss::none, bool keep = false)
		{
			slot_data &d = g.slots[s];
			d.why = why;
			if (!keep)
				d.r = nullptr;
			set_state(s, st);
			if (st == state::missed)
				g.n.refused[size_t(why)]++;
			tell_owner(s);
		}

		// (the mutex held) A new slot for owner o; UINT32_MAX when the table is full
		uint32_t new_slot(owner o)
		{
			uint32_t s;
			if (g.free_slots.size() > kReuseAfter)
			{
				s = g.free_slots.front();
				g.free_slots.pop_front();
			}
			else
			{
				s = uint32_t(g.slots.size());
				if (s == 0)
					s = 1, g.slots.emplace_back(); // (slot 0 unused: a zero ticket is none)
				if (s >= kChunk * kChunks)
					return UINT32_MAX;
				if (g_words[s / kChunk].load(std::memory_order_relaxed) == nullptr)
				{
					auto *chunk = new std::atomic<uint32_t>[kChunk];
					for (uint32_t i = 0; i < kChunk; ++i)
						chunk[i].store(0, std::memory_order_relaxed);
					g_words[s / kChunk].store(chunk, std::memory_order_release);
				}
				g.slots.emplace_back();
			}
			slot_data &d = g.slots[s];
			const uint32_t gen = d.gen % kGenerationMask + 1;
			d = slot_data {};
			d.gen = gen;
			d.live = true;
			d.o = o;
			g.owners[o].slots.insert(s);
			return s;
		}

		// (the mutex held) A slot given back: it reads `st` (dropped, or gone at stop) until it's reused
		void free_slot(uint32_t s, state st = state::dropped)
		{
			slot_data &d = g.slots[s];
			if (!d.live)
				return;
			d.live = false;
			d.r = nullptr;
			set_state(s, st);
			auto it = g.owners.find(d.o);
			if (it != g.owners.end())
			{
				it->second.slots.erase(s);
				it->second.done.erase(s);
			}
			g.free_slots.push_back(s);
		}

		rkey key_of(const slot_data &d)
		{
			return rkey { d.w.k.hash, d.w.k.bytes, d.w.k.w, d.w.k.h, d.w.kind, d.w.first, d.w.count };
		}

		// (the mutex held) A ready texture's last reference gone: out of the map (or the held list), to the retire list
		void let_go_locked(resident *r)
		{
			if (r->in_map)
				g.map.erase(r->k);
			g.to_retire.push_back(r->res);
			g.n.retired++;
			if (r->held)
			{
				g.held.erase(r);
				g.n.held--;
				g.n.held_bytes -= r->bytes;
			}
			else
			{
				g.n.resident--;
				g.n.resident_bytes -= r->bytes;
			}
			delete r;
		}

		// (the mutex held) A ticket dropped, in any state
		void drop_locked(uint32_t s)
		{
			slot_data &d = g.slots[s];
			if (!d.live)
				return;
			switch (d.st)
			{
			case state::queued:
				g.queue.erase(std::make_tuple(-int64_t(d.w.priority), d.seq, s));
				break;
			case state::reading:
			case state::copying:
				if (d.r != nullptr)
				{
					auto &w = d.r->waiters;
					w.erase(std::remove(w.begin(), w.end(), s), w.end());
				}
				break;
			case state::ready:
				if (d.r != nullptr)
				{
					if (--d.r->refs == 0)
						let_go_locked(d.r);
					else
						g.n.shared_bytes -= d.r->bytes; // (one owner fewer sharing it)
				}
				break;
			case state::gone:
				// (a ticket an open, close or reload turned gone while it was ready: its texture held till now)
				if (d.r != nullptr && --d.r->refs == 0)
					let_go_locked(d.r);
				break;
			default:
				break;
			}
			free_slot(s);
		}

		// (the mutex held) A new epoch: the pack opened, closed or reloaded, a new device, the device lost, stop. Every
		// ticket alive reads gone (lost: the ones queued or on their way read missed); one that already missed stays
		// missed, with its reason (it holds nothing); those on their way are left to their jobs (out of the map, nobody
		// waiting); nothing stays queued. Ready textures, as `keep` says (zones.md decision 1):
		//   - keep (an open, close or reload: done on the worker at a time of its own, while owners may still record views
		//     of them): out of the map (a new request streams the key again) but held, each by the tickets that held it,
		//     gone now and keeping it, until the last of them is dropped; then to the retire list as any drop. So a view's
		//     texture lives exactly while its ticket is held, whatever the timing;
		//   - let go (stop, a new device, the device lost): to the retire list at once, with every one held before, and
		//     the gone tickets' hold on them forgotten (stop ends everything, release_all follows; retire frees a device's
		//     objects at once when it sees the next device; on a lost device nothing runs)
		void end_epoch_locked(bool lost, bool keep)
		{
			g.epoch++;
			for (uint32_t s = 1; s < g.slots.size(); ++s)
			{
				slot_data &d = g.slots[s];
				if (!d.live || d.st == state::missed)
					continue;
				if (d.st == state::gone)
				{
					if (!keep)
						d.r = nullptr; // (its texture let go below)
					continue;
				}
				const bool on_way = d.st == state::queued || d.st == state::reading || d.st == state::copying;
				finish(s, lost && on_way ? state::missed : state::gone, lost && on_way ? miss::lost : miss::none, keep && d.st == state::ready);
			}
			for (auto &[k, r] : g.map)
			{
				r->in_map = false;
				if (r->phase == resident::ready)
				{
					if (keep)
					{
						r->held = true;
						g.held.insert(r);
						g.n.held++;
						g.n.held_bytes += r->bytes;
					}
					else
						let_go_locked(r);
				}
				else
					r->waiters.clear();
			}
			if (!keep)
			{
				const std::vector<resident *> held(g.held.begin(), g.held.end());
				for (resident *r : held)
					let_go_locked(r);
			}
			g.map.clear();
			g.queue.clear();
			g.n.resident = 0;
			g.n.resident_bytes = 0;
			g.n.shared_bytes = 0;
			publish_locked();
		}

		void hand_to_retire(std::vector<IUnknown *> &v)
		{
			for (IUnknown *p : v)
				retire::later(p);
			v.clear();
		}
		std::vector<IUnknown *> take_retire_locked()
		{
			std::vector<IUnknown *> v;
			v.swap(g.to_retire);
			return v;
		}

		void release(IUnknown *p)
		{
			if (p != nullptr)
				p->Release();
		}

		// ---- The worker's GPU parts (the worker's alone)

		struct gpu
		{
			ID3D12Device *device = nullptr; // (g.device when the parts were made; g holds it)
			ID3D12CommandQueue *queue = nullptr;
			ID3D12CommandAllocator *allocator[kInFlight] = {};
			ID3D12GraphicsCommandList *list[kInFlight] = {};
			uint64_t list_value[kInFlight] = {}; // (the fence value after each one's last list)
			ID3D12Fence *fence = nullptr;
			HANDLE fence_event = nullptr;
			uint64_t value = 0, completed = 0;
			ID3D12Resource *ring = nullptr;
			uint8_t *mapped = nullptr;
			uint64_t ring_size = 0, head = 0;
			std::deque<job *> flight; // (jobs holding ring space, oldest first: all submitted but perhaps the last)
			bool lost = false, failed = false;
		};

		uint32_t submitted(const gpu &w)
		{
			uint32_t n = 0;
			for (const job *j : w.flight)
				n += j->fence != 0;
			return n;
		}

		void ring_crumbs(const gpu &w)
		{
			sc::the.ring_head.store(w.head);
			sc::the.ring_tail.store(w.flight.empty() ? w.head : w.flight.front()->at);
		}

		// The ring, queue, lists and fence let go: released when nothing of theirs can run any more, else left
		void drop_parts(gpu &w, bool can_release)
		{
			if (can_release && w.ring != nullptr && w.mapped != nullptr)
				w.ring->Unmap(0, nullptr);
			IUnknown *parts[] = { w.ring, w.queue, w.fence, w.allocator[0], w.allocator[1], w.allocator[2], w.allocator[3], w.list[0], w.list[1], w.list[2],
				w.list[3] };
			uint64_t left = 0;
			for (IUnknown *p : parts)
				if (p != nullptr)
				{
					if (can_release)
						p->Release();
					else
						left++;
				}
			if (left != 0)
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				g.n.leaked += left;
			}
			HANDLE e = w.fence_event;
			w = gpu {};
			w.fence_event = e;
		}

		// (no lock) The queue, lists, fence and ring for the device; false: the pack stays off (g.failure says why)
		bool make_parts(gpu &w, ID3D12Device *device, uint64_t ring_size)
		{
			crumb(sc::kMaking);
			w.device = device;
			const char *why = nullptr;
			D3D12_COMMAND_QUEUE_DESC qd = {};
			qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
			if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&w.queue))))
				why = "the copy queue couldn't be made";
			for (uint32_t k = 0; why == nullptr && k < kInFlight; ++k)
				if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&w.allocator[k]))) ||
					FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, w.allocator[k], nullptr, IID_PPV_ARGS(&w.list[k]))) || FAILED(w.list[k]->Close()))
					why = "the copy lists couldn't be made";
			if (why == nullptr && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&w.fence))))
				why = "the copy fence couldn't be made";
			if (why == nullptr)
				why = g_fail_parts.load(); // (tests only: the ring refused, as a card short of it would)
			if (why == nullptr)
			{
				// (CPU write-back in L0: the reader checks each level's CRC over the bytes it just read into it, and an UPLOAD
				// heap's write-combined mapping reads back at 57 MB/s on the 4070 Ti. Made in COMMON: a buffer promotes to
				// COPY_SOURCE on the copy queue and decays after each ExecuteCommandLists)
				D3D12_HEAP_PROPERTIES hp = {};
				hp.Type = D3D12_HEAP_TYPE_CUSTOM;
				hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
				hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
				D3D12_RESOURCE_DESC bd = {};
				bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				bd.Width = ring_size;
				bd.Height = 1;
				bd.DepthOrArraySize = 1;
				bd.MipLevels = 1;
				bd.SampleDesc.Count = 1;
				bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				void *p = nullptr;
				if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&w.ring))) ||
					FAILED(w.ring->Map(0, nullptr, &p)) || p == nullptr)
					why = "the upload ring couldn't be made (a CUSTOM write-back heap)";
				else
				{
					w.mapped = static_cast<uint8_t *>(p);
					w.ring_size = ring_size;
				}
			}
			if (why != nullptr)
			{
				drop_parts(w, true);
				w.failed = true;
				crumb(sc::kFailed);
				std::lock_guard<std::mutex> lock(g.mutex);
				g.failure = why;
				g.n.failure = why;
				log_locked(std::string("texture pack: the pack is off: ") + why);
				// (what's queued now never comes)
				for (const auto &q : g.queue)
					finish(std::get<2>(q), state::missed, miss::no_ring);
				g.queue.clear();
				publish_locked();
				return false;
			}
			w.queue->SetName(L"hifixi: pack copy queue");
			w.fence->SetName(L"hifixi: pack copies");
			w.ring->SetName(L"hifixi: pack ring");
			for (uint32_t k = 0; k < kInFlight; ++k)
			{
				wchar_t name[48];
				std::swprintf(name, 48, L"hifixi: pack copy list %u", k);
				w.allocator[k]->SetName(name);
				w.list[k]->SetName(name);
			}
			D3D12_HEAP_PROPERTIES hp = {};
			D3D12_HEAP_FLAGS hf = D3D12_HEAP_FLAG_NONE;
			w.ring->GetHeapProperties(&hp, &hf);
			sc::the.ring_size.store(ring_size);
			ring_crumbs(w);
			std::lock_guard<std::mutex> lock(g.mutex);
			g.n.ring_bytes = ring_size;
			g.n.ring_heap = hp.Type;
			g.n.ring_page = hp.CPUPageProperty;
			g.n.ring_pool = hp.MemoryPoolPreference;
			return true;
		}

		// (the mutex held) The jobs in flight let go (a device change, the device lost, stop): their textures collected
		// into `made` when no copy can still use them, else left (counted). An entry still in the map (none should be:
		// the epoch ends first, and pick_locked starts nothing while a device change waits) is taken out and its tickets
		// read gone, so no map entry outlives its job
		void let_flight_go_locked(gpu &w, bool can_release, std::vector<IUnknown *> &made)
		{
			for (job *j : w.flight)
			{
				resident *r = j->r;
				if (can_release)
					made.push_back(r->res), g.n.released++;
				else
					g.n.leaked++;
				if (r->in_map)
				{
					for (uint32_t s : r->waiters)
						finish(s, state::gone);
					g.map.erase(r->k);
				}
				delete r;
				delete j;
			}
			g.n.in_flight = 0;
		}

		// (no lock held) Every copy submitted waited for, at most `ms` (and no longer once the stop event is set, when
		// `stop_too`). true: all seen done, what they use may be released. It loops on the fence's value, not on its
		// event: the event is auto-reset, and an older registration (the idle wait's for the oldest copy, left behind when a
		// request or the stop event woke it) sets it as soon as that copy is done, before the last one is. A removed
		// device's fence reads UINT64_MAX: then only what was seen done before the loss counts (a removed device may run a
		// submitted copy on: lose_parts)
		bool wait_copies(gpu &w, DWORD ms, bool stop_too)
		{
			if (w.fence == nullptr || submitted(w) == 0)
				return true;
			const ULONGLONG end = GetTickCount64() + ms;
			bool last = false;
			for (;;)
			{
				const uint64_t v = w.fence->GetCompletedValue();
				if (v == UINT64_MAX)
					return w.completed >= w.value;
				w.completed = v;
				if (v >= w.value)
					return true;
				const ULONGLONG now = GetTickCount64();
				if (last || now >= end || FAILED(w.fence->SetEventOnCompletion(w.value, w.fence_event)))
					return false;
				HANDLE hs[2] = { w.fence_event, g.stop_event };
				if (WaitForMultipleObjects(stop_too ? 2 : 1, hs, FALSE, DWORD(end - now)) == WAIT_OBJECT_0 + 1)
					last = true; // (stopping: one last look)
			}
		}

		// ---- Losing the device

		// (no lock) The device removed: what the worker holds let go at once, or left (below). It waits for no fence
		// again, and makes nothing until a new device
		void lose_parts(gpu &w)
		{
			crumb(sc::kLost);
			// (a copy submitted and not seen done before the loss may still run: a removed device needn't stop at once,
			// and WARP's RemoveDevice doesn't (packstream_test crashed releasing a ring and texture a copy still used). So
			// unless every copy was seen done, what they use is left, not released: their textures, the ring, the
			// queue and lists. A lost device ends the game's session anyway)
			const bool idle = w.completed >= w.value;
			std::vector<IUnknown *> made;
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				let_flight_go_locked(w, idle, made);
				publish_locked();
			}
			w.flight.clear();
			for (IUnknown *p : made)
				release(p);
			drop_parts(w, idle);
			w.lost = true;
		}

		// (the mutex held) The device lost: kLost, a new epoch with the tickets on their way missed
		void lose_locked()
		{
			if ((g.pause & kLost) != 0)
				return;
			g.pause |= kLost;
			end_epoch_locked(true, false);
		}

		// ---- Ring space

		bool ring_take(gpu &w, job *j, uint64_t n)
		{
			if (n > w.ring_size)
				return false;
			if (w.flight.empty())
				j->at = 0;
			else
			{
				const uint64_t tail = w.flight.front()->at;
				if (w.head > tail)
				{
					// (in use: [tail, head))
					if (w.ring_size - w.head >= n)
						j->at = w.head;
					else if (tail >= n)
					{
						// (the bytes to the end skipped: freed with the last copy before the wrap)
						std::lock_guard<std::mutex> lock(g.mutex);
						g.n.ring_wraps++;
						g.n.ring_skipped += w.ring_size - w.head;
						w.flight.back()->end = w.ring_size;
						j->at = 0;
					}
					else
						return false;
				}
				else if (tail - w.head >= n) // (in use: [tail, end) and [0, head))
					j->at = w.head;
				else
					return false;
			}
			j->end = j->at + n;
			w.head = j->end;
			w.flight.push_back(j);
			ring_crumbs(w);
			return true;
		}

		// The last job's space given back (its read failed: nothing was submitted)
		void ring_untake(gpu &w)
		{
			w.flight.pop_back();
			w.head = w.flight.empty() ? 0 : w.flight.back()->end;
			ring_crumbs(w);
		}

		// ---- Copies done

		bool lose_by_fence(gpu &w)
		{
			std::vector<IUnknown *> rel;
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				lose_locked();
				rel = take_retire_locked();
			}
			hand_to_retire(rel);
			lose_parts(w);
			return false;
		}

		// (no lock held) The copies the fence has passed: their textures ready for every ticket still waiting, or let go
		// when none is (only the copy queue saw them); their ring space and lists free. false: the device is lost
		bool complete(gpu &w)
		{
			if (w.lost || w.fence == nullptr)
				return !w.lost;
			const uint64_t v = w.fence->GetCompletedValue();
			sc::the.completed.store(v);
			if (v == UINT64_MAX)
				return lose_by_fence(w);
			w.completed = v;
			std::vector<job *> done;
			while (!w.flight.empty() && w.flight.front()->fence != 0 && w.flight.front()->fence <= v)
			{
				done.push_back(w.flight.front());
				w.flight.pop_front();
			}
			if (w.flight.empty())
				w.head = 0;
			ring_crumbs(w);
			if (done.empty())
				return true;
			std::vector<IUnknown *> rel;
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				for (job *j : done)
				{
					resident *r = j->r;
					if (r->in_map && !r->waiters.empty())
					{
						r->phase = resident::ready;
						r->refs = uint32_t(r->waiters.size());
						for (uint32_t s : r->waiters)
						{
							g.slots[s].r = r;
							set_state(s, state::ready); // (release: the copy is done, the CPU saw it)
							tell_owner(s);
						}
						r->waiters.clear();
						g.n.resident++;
						g.n.resident_bytes += r->bytes;
						g.n.shared_bytes += uint64_t(r->refs - 1) * r->bytes;
						g.n.resident_peak = std::max(g.n.resident_peak, g.n.resident);
					}
					else
					{
						if (r->in_map)
							g.map.erase(r->k);
						rel.push_back(r->res);
						g.n.released++;
						delete r;
					}
					delete j;
				}
				g.n.in_flight -= std::min<uint64_t>(g.n.in_flight, done.size());
				publish_locked();
			}
			for (IUnknown *p : rel)
				release(p);
			return true;
		}

		// (no lock held) Waits for the oldest copy, at most kWaitMs, or the stop event. false: stopping or lost
		bool wait_oldest(gpu &w)
		{
			if (w.lost || w.flight.empty() || w.flight.front()->fence == 0)
				return !w.lost;
			crumb(sc::kRingWait);
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				g.n.ring_waits++;
			}
			if (w.fence->GetCompletedValue() < w.flight.front()->fence && SUCCEEDED(w.fence->SetEventOnCompletion(w.flight.front()->fence, w.fence_event)))
			{
				HANDLE hs[2] = { g.stop_event, w.fence_event };
				if (WaitForMultipleObjects(2, hs, FALSE, kWaitMs) == WAIT_OBJECT_0)
					return false;
			}
			return complete(w);
		}

		// ---- One texture

		D3D12_RESOURCE_DESC desc_of(const record &r, uint32_t first, uint32_t count)
		{
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			d.Width = std::max(1u, uint32_t(r.w) >> first);
			d.Height = std::max(1u, uint32_t(r.h) >> first);
			d.DepthOrArraySize = 1;
			d.MipLevels = uint16_t(count);
			d.Format = DXGI_FORMAT(r.format);
			d.SampleDesc.Count = 1;
			d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			return d;
		}

		bool sides_by_4(const record &r, uint32_t level)
		{
			const uint32_t w = std::max(1u, uint32_t(r.w) >> level), h = std::max(1u, uint32_t(r.h) >> level);
			return w % 4 == 0 && h % 4 == 0;
		}

		// (no lock held) A job that won't be copied: its tickets missed (unless it's gone meanwhile), its texture
		// released at once (no queue ever ran on it), its entry let go. A failure of the record's or the read's, with
		// tickets still asking for it, is a line for take_log (plan §3: a miss plus a log line), with `detail` (the
		// reader's or the rule's why)
		void fail_job(job *j, miss why, const std::string &detail = std::string())
		{
			ID3D12Resource *res = j->r->res;
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				resident *r = j->r;
				if (!r->waiters.empty() && why != miss::lost)
				{
					char line[200];
					std::snprintf(line, sizeof(line), "texture pack: %.16s (key %016llx, pack-%03u.bin at %llu, levels %u to %u) left to the game: %s", j->name,
						static_cast<unsigned long long>(r->k.hash), j->rec.file, static_cast<unsigned long long>(j->rec.offset), j->first,
						j->first + (j->count != 0 ? j->count - 1 : 0), text(why));
					log_locked(detail.empty() ? std::string(line) : std::string(line) + " (" + detail + ")");
				}
				for (uint32_t s : r->waiters)
					finish(s, state::missed, why);
				if (r->in_map)
					g.map.erase(r->k);
				if (res != nullptr)
					g.n.released++;
				delete r;
				publish_locked();
			}
			release(res);
			delete j;
		}

		// The video memory budget: the lower of the two; 0 none
		uint64_t budget_of(const limits &l)
		{
			uint64_t budget = l.vram_budget;
			if (l.budget_cap != 0 && (budget == 0 || l.budget_cap < budget))
				budget = l.budget_cap;
			return budget;
		}

		// (the mutex held) A job's tickets back in the queue in the order they were asked, its entry let go (a texture made
		// for it is the caller's to release)
		void requeue_locked(resident *r)
		{
			for (uint32_t s : r->waiters)
			{
				slot_data &d = g.slots[s];
				d.r = nullptr;
				set_state(s, state::queued);
				g.queue.emplace(-int64_t(d.w.priority), d.seq, s);
			}
			if (r->in_map)
				g.map.erase(r->k);
			delete r;
		}

		// (no lock held) A read cancelled by an open, close or stop the worker hasn't got to yet: its tickets back in the
		// queue, where the epoch that open or stop ends finds them (gone), not missed as "a read failed" (a reload during a
		// read used to leave them missed, counted as a failed read); its texture released at once (no queue ran on it).
		// false: no such call is pending (the read failed for another reason)
		bool requeue_cancelled(job *j)
		{
			ID3D12Resource *res = j->r->res;
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				if (g.post == 0 && !g.quit)
					return false;
				requeue_locked(j->r);
				if (res != nullptr)
					g.n.released++;
				publish_locked();
			}
			release(res);
			delete j;
			return true;
		}

		// (no lock held) Over 95% of the video memory budget with this texture's `bytes`: the tickets back in the queue,
		// nothing new started till limits come under which it fits (kVideoMemory, set_limits)
		void requeue_job(job *j, uint64_t bytes)
		{
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				requeue_locked(j->r);
				g.pause |= kVideoMemory;
				g.refused_bytes = bytes;
				g.n.requeued++;
				publish_locked();
			}
			delete j;
		}

		// (no lock held) Make, read, record and submit one texture. false: stopping or lost (the job is finished either way)
		bool run(gpu &w, job *j)
		{
			ID3D12Device *const device = w.device;
			crumb(sc::kMaking);
			if (const char *bad = check(j->rec, device))
			{
				fail_job(j, miss::rule, bad);
				return true;
			}
			if (j->first >= j->rec.levels || j->count == 0 || j->count > j->rec.levels - j->first)
			{
				fail_job(j, miss::rule, "no such levels");
				return true;
			}
			D3D12_RESOURCE_DESC d = desc_of(j->rec, j->first, j->count);
			D3D12_RESOURCE_ALLOCATION_INFO ai = device->GetResourceAllocationInfo(0, 1, &d);
			// The video memory budget: the usage published (it lags) plus what was made since, plus this
			const uint64_t budget = budget_of(j->lim);
			bool skipped = false;
			if (budget != 0)
			{
				const uint64_t used = j->lim.vram_usage + j->made_since;
				if ((used + ai.SizeInBytes) * 100 > budget * kSkipPercent && j->count > 1 && sides_by_4(j->rec, j->first + 1))
				{
					j->first++;
					j->count--;
					d = desc_of(j->rec, j->first, j->count);
					ai = device->GetResourceAllocationInfo(0, 1, &d);
					skipped = true;
				}
				if ((used + ai.SizeInBytes) * 100 > budget * kStopPercent)
				{
					requeue_job(j, ai.SizeInBytes);
					return true;
				}
			}
			j->span = reader::levels_span(j->rec, j->first, j->count);
			if (j->span == 0 || round_up(j->span, kAlign) > w.ring_size)
			{
				fail_job(j, j->span == 0 ? miss::rule : miss::too_big);
				return true;
			}
			D3D12_HEAP_PROPERTIES hp = {};
			hp.Type = D3D12_HEAP_TYPE_DEFAULT;
			ID3D12Resource *res = nullptr;
			// (COMMON: the copy queue promotes it to COPY_DEST; the direct queue's reads promote it to a read state; it
			// decays after each ExecuteCommandLists, so no barrier is ever put on it)
			if (const HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res)); FAILED(hr))
			{
				char e[48];
				std::snprintf(e, sizeof(e), "CreateCommittedResource 0x%08lx", static_cast<unsigned long>(hr));
				fail_job(j, miss::create, e);
				return true;
			}
			{
				wchar_t name[40] = L"hifixi: pack ";
				for (size_t i = 0; i < 16 && j->name[i] != 0; ++i)
					name[13 + i] = wchar_t(uint8_t(j->name[i]));
				res->SetName(name);
			}
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				j->r->res = res;
				j->r->format = d.Format;
				j->r->w = uint32_t(d.Width);
				j->r->h = d.Height;
				j->r->levels = j->count;
				j->r->first = j->first;
				j->r->bytes = ai.SizeInBytes;
				g.n.made++;
				g.n.made_bytes += ai.SizeInBytes;
				g.made_since += ai.SizeInBytes;
				g.n.skipped_top += skipped;
			}

			// Ring space: the oldest copies waited for until the span fits
			const uint64_t need = round_up(j->span, kAlign);
			while (!ring_take(w, j, need))
				if (!wait_oldest(w))
				{
					fail_job(j, w.lost ? miss::lost : miss::read);
					return false;
				}

			// The levels read straight into the ring, each level's CRC checked there
			crumb(sc::kReading);
			{
				layout l;
				footprint(j->rec.format, j->rec.w, j->rec.h, j->rec.levels, l);
				sc::the.last_hash.store(j->r->k.hash);
				sc::the.last_file.store(j->rec.file);
				sc::the.last_offset.store(uint64_t(j->rec.offset) + l.level[j->first].offset);
				sc::the.last_bytes.store(j->span);
				sc::the.last_first.store(j->first);
				sc::the.last_count.store(j->count);
			}
			LARGE_INTEGER t0, t1, f;
			QueryPerformanceCounter(&t0);
			std::string why;
			const bool read = j->rd != nullptr && j->rd->read_levels(j->rec, j->first, j->count, w.mapped + j->at, why);
			QueryPerformanceCounter(&t1);
			QueryPerformanceFrequency(&f);
			bool abandoned;
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				g.n.reads++;
				if (read)
					g.n.bytes_read += j->span;
				g.n.read_ms += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
				// (nobody waits for it any more: dropped while it was read, or an epoch change)
				abandoned = !j->r->in_map || j->r->waiters.empty();
			}
			if (!read || abandoned)
			{
				ring_untake(w);
				if (!read && !abandoned && why == "cancelled" && requeue_cancelled(j))
					return true;
				fail_job(j, why.rfind("CRC", 0) == 0 ? miss::crc : miss::read, why);
				return true;
			}

			// One list: each level from its place in the ring (GetCopyableFootprints of the texture made, from the ring's
			// offset; the record's stored layout is the same shifted, checked)
			crumb(sc::kRecording);
			uint32_t k = 0;
			while (k < kInFlight && w.list_value[k] > w.completed)
				++k;
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[kMaxLevels] = {};
			UINT rows[kMaxLevels] = {};
			UINT64 row_bytes[kMaxLevels] = {}, total = 0;
			device->GetCopyableFootprints(&d, 0, j->count, j->at, fp, rows, row_bytes, &total);
			layout l;
			footprint(j->rec.format, j->rec.w, j->rec.h, j->rec.levels, l);
			bool same = k < kInFlight && total <= j->span;
			for (uint32_t i = 0; same && i < j->count; ++i)
				same = fp[i].Offset - j->at == l.level[j->first + i].offset - l.level[j->first].offset && fp[i].Footprint.RowPitch == l.level[j->first + i].row_pitch;
			if (!same || FAILED(w.allocator[k]->Reset()) || FAILED(w.list[k]->Reset(w.allocator[k], nullptr)))
			{
				ring_untake(w);
				fail_job(j, miss::rule,
					k >= kInFlight ? "no copy list free"
					: !same        ? "the texture's copy footprints aren't the record's stored layout"
								   : "a copy list couldn't be reset");
				return true;
			}
			for (uint32_t i = 0; i < j->count; ++i)
			{
				D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
				dst.pResource = res;
				dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				dst.SubresourceIndex = i;
				src.pResource = w.ring;
				src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				src.PlacedFootprint = fp[i];
				w.list[k]->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			}
			if (FAILED(w.list[k]->Close()))
			{
				ring_untake(w);
				fail_job(j, miss::rule, "the copy list couldn't be closed");
				return true;
			}
			ID3D12CommandList *lists[] = { w.list[k] };
			// (tests only: the copy held back on the GPU until the test's fence reaches the value)
			if (j->gate != nullptr)
				w.queue->Wait(j->gate, j->gate_value);
			w.queue->ExecuteCommandLists(1, lists);
			w.queue->Signal(w.fence, ++w.value);
			sc::the.signalled.store(w.value);
			j->fence = w.value;
			j->list = k;
			w.list_value[k] = w.value;
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				j->r->phase = resident::copying;
				for (uint32_t s : j->r->waiters)
					set_state(s, state::copying);
				g.n.in_flight++;
				g.n.copies++;
				publish_locked();
			}
			return true;
		}

		// ---- Picking

		struct pacing
		{
			uint64_t seen = 0, bytes = 0;
			uint32_t records = 0;
		};

		// (the mutex held) The next texture to make, or nullptr: paused, 4 in flight, this present's bytes used up, or
		// nothing queued; or something the loop's top handles first (a device change, an open or close, stop). Tickets
		// whose texture is resident or on its way are joined to it on the way
		job *pick_locked(gpu &w, pacing &p)
		{
			// (the address-space floor: the worker's bit, from the module's numbers; 0 not measured)
			const bool low = (g.lim.avail_virtual != 0 && g.lim.avail_virtual < kFloorVirtual) || (g.lim.largest_free != 0 && g.lim.largest_free < kFloorBlock);
			g.pause = low ? g.pause | kAddressSpace : g.pause & ~uint32_t(kAddressSpace);
			if (g.pause != 0 || w.lost || w.failed || w.queue == nullptr || w.flight.size() >= kInFlight)
				return nullptr;
			// (set_device, open or stop since the loop's top read them: a ticket of the new epoch must not start on the old
			// device's queue and ring, whose parts the device change then lets go under it)
			if (g.device_change || g.post != 0 || g.quit || w.device != g.device)
				return nullptr;
			const uint64_t presents = g_presents.load();
			if (presents != p.seen)
			{
				p.seen = presents;
				p.bytes = 0;
				p.records = 0;
			}
			// (no cap before the first present since start: the tests, the first moments)
			const uint64_t cap = presents != g.presents_at_start ? g.lim.per_present : 0;
			while (!g.queue.empty())
			{
				const uint32_t s = std::get<2>(*g.queue.begin());
				slot_data &d = g.slots[s];
				const rkey k = key_of(d);
				auto it = g.map.find(k);
				if (it != g.map.end())
				{
					// (asked again since it was queued: shared)
					g.queue.erase(g.queue.begin());
					resident *r = it->second;
					r->asked_at = presents;
					d.r = r;
					g.n.shared++;
					if (r->phase == resident::ready)
					{
						r->refs++;
						g.n.shared_bytes += r->bytes;
						set_state(s, state::ready);
						tell_owner(s);
					}
					else
					{
						r->waiters.push_back(s);
						set_state(s, r->phase == resident::copying ? state::copying : state::reading);
					}
					continue;
				}
				if (cap != 0 && p.bytes != 0 && p.bytes + d.span > cap)
					return nullptr; // (the next present)
				g.queue.erase(g.queue.begin());
				auto *r = new resident;
				r->k = k;
				r->asked_at = presents;
				r->waiters.push_back(s);
				g.map.emplace(k, r);
				d.r = r;
				set_state(s, state::reading);
				job *j = new job;
				j->r = r;
				j->rec = d.rec;
				std::memcpy(j->name, d.name, sizeof(j->name));
				j->snap = g.snap;
				j->rd = g.rd;
				j->first = d.w.first;
				j->count = d.w.count;
				j->lim = g.lim;
				j->made_since = g.made_since;
				j->gate = g.test_gate;
				j->gate_value = g.test_gate_value;
				p.bytes += d.span;
				p.records++;
				if (cap != 0)
				{
					// (the pacing's own record, for the status and the tests: presents something started in, the most
					// bytes one of them started, a record over the cap alone, and over it with others (never))
					g.n.present_max_bytes = std::max(g.n.present_max_bytes, p.bytes);
					if (p.bytes > cap)
						(p.records == 1 ? g.n.present_alone : g.n.present_breaks)++;
					if (p.records == 1)
						g.n.presents++;
				}
				publish_locked();
				return j;
			}
			return nullptr;
		}

		// ---- Opening, device change, stop

		void do_open(int post, uint64_t number, const std::wstring &folder, std::shared_ptr<const index> given)
		{
			crumb(sc::kOpening);
			std::string why;
			std::shared_ptr<const index> idx;
			if (post == 1)
			{
				// (stop() cancels it between pieces of pack.idx: stopping, nothing changes)
				idx = pack::load(folder, nullptr, why, &g_cancel_open);
				if (!idx && g_cancel_open.load())
					return;
			}
			else if (post == 2)
				idx = std::move(given);
			if (void (*fn)() = g_after_load.load())
				fn();
			std::shared_ptr<reader> rd = idx ? std::make_shared<reader>(idx) : nullptr;
			std::shared_ptr<reader> old;
			std::vector<IUnknown *> rel;
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				old = std::move(g.rd);
				g.snap = idx;
				g.rd = rd;
				g.open_status = post == 1 && !idx ? "pack can't be used: " + why : "";
				end_epoch_locked(false, true);
				// (with its epoch: an open or close asked while this one loaded is done next, and raises it again)
				g.post_done = number;
				rel = take_retire_locked();
			}
			// (the worker is the reading thread: no read of the old reader's runs now. Its files closed, the folder can be
			// renamed)
			if (old)
				old->close();
			hand_to_retire(rel);
		}

		// (no lock held) A new device (or none): the old one's copies waited for at most kDeviceWaitMs, then everything
		// made on it released, or left when they didn't finish (or the old device was removed with copies not seen done)
		void switch_device(gpu &w)
		{
			crumb(sc::kDeviceChange);
			const bool done = w.lost || wait_copies(w, kDeviceWaitMs, true);
			std::vector<IUnknown *> made;
			std::vector<ID3D12Device *> devices;
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				let_flight_go_locked(w, done, made);
				devices.swap(g.old_devices);
				publish_locked();
			}
			w.flight.clear();
			for (IUnknown *p : made)
				release(p);
			drop_parts(w, done);
			for (ID3D12Device *d : devices)
				d->Release();
		}

		// (no lock held) quit: the copies waited for at most kStopWaitMs, the ready textures to the retire list, the rest
		// released or left, the reader closed
		void shut_down(gpu &w)
		{
			crumb(sc::kStopping);
			const bool done = w.lost || wait_copies(w, kStopWaitMs, false);
			std::vector<IUnknown *> rel, made;
			std::vector<ID3D12Device *> devices;
			std::shared_ptr<reader> rd;
			{
				std::lock_guard<std::mutex> lock(g.mutex);
				end_epoch_locked(false, false);
				// (every ticket now reads gone; slots given back)
				for (uint32_t s = 1; s < g.slots.size(); ++s)
					free_slot(s, state::gone);
				g.owners.clear();
				let_flight_go_locked(w, done, made);
				rel = take_retire_locked();
				devices.swap(g.old_devices);
				rd = std::move(g.rd);
				g.snap = nullptr;
				publish_locked();
			}
			w.flight.clear();
			hand_to_retire(rel);
			for (IUnknown *p : made)
				release(p);
			drop_parts(w, done);
			if (rd)
				rd->close();
			for (ID3D12Device *d : devices)
				d->Release();
			crumb(sc::kStopped);
		}

		void worker()
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			gpu w;
			w.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			pacing p;
			for (;;)
			{
				// What's asked of it
				int post = 0;
				uint64_t number = 0;
				std::wstring folder;
				std::shared_ptr<const index> given;
				bool change = false, lost = false;
				ID3D12Device *device = nullptr;
				uint64_t ring_size = 0;
				{
					std::lock_guard<std::mutex> lock(g.mutex);
					if (g.quit)
						break;
					change = g.device_change;
					g.device_change = false;
					lost = (g.pause & kLost) != 0;
					device = g.device;
					ring_size = g.ring_size;
					// (an open or close is taken only when it's what this pass does: one posted with a device change or a
					// loss waits for the next pass. Taken with them, it was dropped: the module's set_device then open,
					// both pending at the loop's top on the 4070 Ti, left the pack unopened for good, M4)
					if (!change && !(lost && !w.lost))
					{
						post = g.post;
						number = g.posted; // (the last asked: a later one overwrote any before it)
						g.post = 0;
						folder.swap(g.post_folder);
						given = std::move(g.post_index);
					}
				}
				if (change)
				{
					switch_device(w);
					continue;
				}
				if (lost && !w.lost)
				{
					lose_parts(w);
					continue;
				}
				if (post != 0)
				{
					do_open(post, number, folder, std::move(given));
					continue;
				}
				if (!w.lost && !w.failed && w.queue == nullptr && device != nullptr)
					make_parts(w, device, ring_size);
				if (!w.flight.empty() && !complete(w))
					continue;

				// One texture to start
				if (void (*fn)() = g_before_pick.load())
					fn(); // (tests only: what another thread may do between the loop's top and the pick)
				job *j;
				uint32_t pause;
				{
					std::lock_guard<std::mutex> lock(g.mutex);
					j = pick_locked(w, p);
					pause = g.pause;
				}
				if (j != nullptr)
				{
					run(w, j);
					continue;
				}

				// Nothing to start: a copy, a request, a present, a resume or stop
				crumb(w.lost ? sc::kLost : pause != 0 ? sc::kPaused : sc::kFenceWait);
				HANDLE hs[3] = { g.stop_event, g.wake, w.fence_event };
				DWORD n = 2;
				if (!w.lost && w.fence != nullptr && submitted(w) != 0 && SUCCEEDED(w.fence->SetEventOnCompletion(w.flight.front()->fence, w.fence_event)))
					n = 3;
				WaitForMultipleObjects(n, hs, FALSE, kWaitMs);
			}
			shut_down(w);
			if (w.fence_event != nullptr)
				CloseHandle(w.fence_event);
		}

		void wake() { SetEvent(g.wake); }
		std::shared_ptr<reader> reader_now()
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			return g.rd;
		}
	}

	const char *text(state s)
	{
		static const char *const names[] = { "none", "queued", "reading", "copying", "ready", "missed", "dropped", "gone" };
		return size_t(s) < sizeof(names) / sizeof(names[0]) ? names[size_t(s)] : "?";
	}

	const char *text(miss m)
	{
		static const char *const names[] = { "none", "no pack open", "not in the pack", "the pack says draw the game's", "a broken rule",
			"a first level that isn't a multiple of 4", "bigger than the upload ring", "a read failed", "a CRC mismatch", "the texture couldn't be made",
			"the device was lost", "no upload ring" };
		return size_t(m) < sizeof(names) / sizeof(names[0]) ? names[size_t(m)] : "?";
	}

	bool start(ID3D12Device *device, uint32_t ring_mb)
	{
		if (device != nullptr)
			device->AddRef();
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			if (!g.started)
			{
				if (g.wake == nullptr)
				{
					g.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
					g.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
				}
				ResetEvent(g.stop_event);
				g.started = true;
				g.quit = false;
				g.device = device;
				g.device_change = false;
				g.ring_size = uint64_t(std::clamp(ring_mb, kRingMinMb, kRingMaxMb)) << 20;
				g.failure.clear();
				g.open_status.clear();
				g.pause = 0;
				g.lim = limits {};
				g.made_since = 0;
				g.refused_bytes = 0;
				g_cancel_open = false;
				g.presents_at_start = g_presents.load();
				g.n = counts {};
				g.n.started = true;
				sc::the.started.store(1);
				sc::the.ring_size.store(g.ring_size);
				crumb(sc::kIdle);
				publish_locked();
				g.thread = std::thread(worker);
				return true;
			}
		}
		if (device != nullptr)
			device->Release();
		return false;
	}

	void set_device(ID3D12Device *device)
	{
		if (device != nullptr)
			device->AddRef();
		std::vector<IUnknown *> rel;
		bool started;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			started = g.started;
			if (started)
			{
				// (the epoch first: every old ticket reads gone before anything of the new device's is made; the old
				// device kept till the worker has let go of what it made on it)
				end_epoch_locked(false, false);
				if (g.device != nullptr)
					g.old_devices.push_back(g.device);
				g.device = device;
				g.device_change = true;
				g.pause &= ~uint32_t(kLost | kVideoMemory);
				g.failure.clear();
				g.n.failure.clear();
				publish_locked();
				rel = take_retire_locked();
			}
		}
		if (!started)
		{
			if (device != nullptr)
				device->Release();
			return;
		}
		// (the old device's resident textures: retire lets them go at once when it sees the new device)
		hand_to_retire(rel);
		// (a reader cancelled by a device loss reads again)
		if (std::shared_ptr<reader> rd = reader_now())
			rd->resume();
		wake();
	}

	void device_lost()
	{
		std::vector<IUnknown *> rel;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			if (!g.started)
				return;
			lose_locked();
			rel = take_retire_locked();
		}
		hand_to_retire(rel);
		// (a read in flight ends now: the worker makes nothing more)
		if (std::shared_ptr<reader> rd = reader_now())
			rd->cancel();
		wake();
	}

	void stop()
	{
		std::shared_ptr<reader> rd;
		std::thread t;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			if (!g.started)
				return;
			g.quit = true;
			rd = g.rd;
			t = std::move(g.thread);
		}
		// (a read in flight ends within milliseconds: CancelIoEx, and the reader's own check after its ReadFile; an open
		// loading pack.idx at its next piece)
		g_cancel_open = true;
		if (rd)
			rd->cancel();
		SetEvent(g.stop_event);
		if (t.joinable())
		{
			// (below normal, it may wait for a core while the CPU is busy, the caller the game's thread: raised for its
			// last steps, as memwatch's)
			SetThreadPriority(t.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
			t.join();
		}
		ID3D12Device *device = nullptr;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			device = g.device;
			g.device = nullptr;
			g.started = false;
			g.n.started = false;
			g.post = 0;
			g.post_folder.clear();
			g.post_index = nullptr;
			sc::the.started.store(0);
		}
		if (device != nullptr)
			device->Release();
	}

	bool running()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		return g.started;
	}

	uint64_t open(const std::wstring &folder)
	{
		std::shared_ptr<reader> rd;
		uint64_t number = 0;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.post = 1;
			number = ++g.posted;
			g.post_folder = folder;
			g.post_index = nullptr;
			rd = g.rd;
		}
		if (rd)
			rd->cancel(); // (the old snapshot's read in flight ends now)
		wake();
		return number;
	}

	uint64_t open(std::shared_ptr<const index> pack)
	{
		std::shared_ptr<reader> rd;
		uint64_t number = 0;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.post = 2;
			number = ++g.posted;
			g.post_folder.clear();
			g.post_index = std::move(pack);
			rd = g.rd;
		}
		if (rd)
			rd->cancel();
		wake();
		return number;
	}

	uint64_t close()
	{
		std::shared_ptr<reader> rd;
		uint64_t number = 0;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.post = 3;
			number = ++g.posted;
			g.post_folder.clear();
			g.post_index = nullptr;
			rd = g.rd;
		}
		if (rd)
			rd->cancel();
		wake();
		return number;
	}

	std::shared_ptr<const index> current()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		return g.snap;
	}

	uint32_t epoch()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		return g.epoch;
	}

	uint64_t posts_done()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		return g.post_done;
	}

	std::string open_status()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		return g.open_status;
	}

	owner add_owner(const char *name)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		const owner o = g.next_owner++;
		g.owners[o].name = name != nullptr ? name : "";
		return o;
	}

	void drop_owner(owner o)
	{
		std::vector<IUnknown *> rel;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			auto it = g.owners.find(o);
			if (it == g.owners.end())
				return;
			const std::vector<uint32_t> slots(it->second.slots.begin(), it->second.slots.end());
			for (uint32_t s : slots)
				drop_locked(s);
			g.owners.erase(o);
			publish_locked();
			rel = take_retire_locked();
		}
		hand_to_retire(rel);
	}

	ticket request(owner o, const want &w)
	{
		ticket t;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			if (g.owners.find(o) == g.owners.end())
				return t;
			const uint32_t s = new_slot(o);
			if (s == UINT32_MAX)
				return t;
			slot_data &d = g.slots[s];
			t = ticket { s, d.gen };
			d.w = w;
			d.seq = ++g.seq;
			g.n.asked++;
			auto missed = [&](miss why) {
				finish(s, state::missed, why);
				publish_locked();
				return t;
			};
			if (!g.started || (w.epoch != 0 && w.epoch != g.epoch))
			{
				finish(s, state::gone);
				return t;
			}
			if ((g.pause & kLost) != 0)
				return missed(miss::lost);
			if (!g.failure.empty())
				return missed(miss::no_ring);
			if (!g.snap)
				return missed(miss::no_pack);
			const entry *e = g.snap->find(w.k);
			if (e == nullptr)
				return missed(miss::not_in_pack);
			if ((e->flags & kNoPack) != 0)
				return missed(miss::no_pack_flag);
			const record *r = e->map(w.kind);
			if (r == nullptr)
				return missed(miss::not_in_pack);
			if (check(*r) != nullptr || w.first >= r->levels)
				return missed(miss::rule);
			// (count 0, or past the chain: all the rest; count 0 with a size to stop at: down to the first level whose sides
			// are at most it, the game's own size for a zone's texture: zones.md "The zone's levels")
			d.w.count = uint8_t(w.count == 0 || w.count > r->levels - w.first ? r->levels - w.first : w.count);
			if (w.count == 0 && w.down_to_w != 0 && w.down_to_h != 0)
			{
				uint32_t n = 1;
				while (n < d.w.count && ((uint32_t(r->w) >> (w.first + n - 1)) > w.down_to_w || (uint32_t(r->h) >> (w.first + n - 1)) > w.down_to_h))
					++n;
				d.w.count = uint8_t(n);
			}
			if (w.first > 0 && !sides_by_4(*r, w.first))
				return missed(miss::first_level);
			d.span = reader::levels_span(*r, d.w.first, d.w.count);
			if (d.span == 0)
				return missed(miss::rule);
			if (round_up(d.span, kAlign) > g.ring_size)
				return missed(miss::too_big);
			d.rec = *r;
			std::memcpy(d.name, e->name, 16);
			d.name[16] = 0;
			// Resident: a reference, ready at once. On its way: joined. Else queued
			auto it = g.map.find(key_of(d));
			if (it != g.map.end())
			{
				resident *res = it->second;
				res->asked_at = g_presents.load();
				d.r = res;
				g.n.shared++;
				if (res->phase == resident::ready)
				{
					res->refs++;
					g.n.shared_bytes += res->bytes;
					set_state(s, state::ready);
					tell_owner(s);
				}
				else
				{
					res->waiters.push_back(s);
					set_state(s, res->phase == resident::copying ? state::copying : state::reading);
				}
			}
			else
			{
				g.queue.emplace(-int64_t(w.priority), d.seq, s);
				set_state(s, state::queued);
			}
			publish_locked();
		}
		wake();
		return t;
	}

	state poll(ticket t)
	{
		if (t.generation == 0)
			return state::none;
		std::atomic<uint32_t> *wd = t.slot < kChunk * kChunks ? word_of(t.slot) : nullptr;
		if (wd == nullptr)
			return state::gone;
		const uint32_t v = wd->load(std::memory_order_acquire);
		return (v >> 8) == t.generation ? state(v & 0xFF) : state::gone;
	}

	miss reason(ticket t)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (t.slot >= g.slots.size() || g.slots[t.slot].gen != t.generation)
			return miss::none;
		return g.slots[t.slot].why;
	}

	void take_done(owner o, std::vector<std::pair<ticket, state>> &out)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		auto it = g.owners.find(o);
		if (it == g.owners.end())
			return;
		// (a slot given back has left the set: every one here is live and the owner's)
		for (uint32_t s : it->second.done)
			out.emplace_back(ticket { s, g.slots[s].gen }, g.slots[s].st);
		it->second.done.clear();
	}

	namespace
	{
		// (the mutex held)
		bool get_locked(ticket t, view &out)
		{
			if (t.slot == 0 || t.slot >= g.slots.size())
				return false;
			const slot_data &d = g.slots[t.slot];
			if (!d.live || d.gen != t.generation || d.st != state::ready || d.r == nullptr)
				return false;
			const resident &r = *d.r;
			out.resource = r.res;
			out.format = r.format;
			out.w = r.w;
			out.h = r.h;
			out.levels = r.levels;
			out.first = r.first;
			out.bytes = r.bytes;
			return true;
		}
	}

	bool get(ticket t, view &out)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		return get_locked(t, out);
	}

	void drop(ticket t)
	{
		std::vector<IUnknown *> rel;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			if (t.slot == 0 || t.slot >= g.slots.size() || g.slots[t.slot].gen != t.generation)
				return;
			drop_locked(t.slot);
			publish_locked();
			rel = take_retire_locked();
		}
		hand_to_retire(rel);
	}

	uint32_t in_flight(owner o)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		auto it = g.owners.find(o);
		if (it == g.owners.end())
			return 0;
		uint32_t n = 0;
		for (uint32_t s : it->second.slots)
		{
			const state st = g.slots[s].st;
			n += st == state::queued || st == state::reading || st == state::copying;
		}
		return n;
	}

	bool write_view(ticket t, D3D12_CPU_DESCRIPTOR_HANDLE slot)
	{
		view v;
		ID3D12Device *device = nullptr;
		{
			// (the ticket, the texture and the device under one lock, each AddRef'd: another thread's set_device, device_lost
			// or stop may end the epoch meanwhile, and retire may let the texture go at once on a new device. AddRef only:
			// an interlocked count; their Release comes after the lock is left)
			std::lock_guard<std::mutex> lock(g.mutex);
			if (g.device == nullptr || !get_locked(t, v))
				return false;
			device = g.device;
			device->AddRef();
			v.resource->AddRef();
		}
		D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
		d.Format = v.format;
		d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		d.Texture2D.MostDetailedMip = 0;
		d.Texture2D.MipLevels = v.levels;
		device->CreateShaderResourceView(v.resource, &d, slot);
		v.resource->Release();
		device->Release();
		return true;
	}

	void on_present()
	{
		g_presents.fetch_add(1);
		if (g.wake != nullptr)
			SetEvent(g.wake);
	}

	void set_limits(const limits &l)
	{
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.lim = l;
			g.made_since = 0;
			// (kVideoMemory, what pressure() reads, from the new numbers: set over 95% of the budget, and kept while the
			// texture last refused still wouldn't fit; else cleared, and the worker tries the 95% stop again with them. So
			// it's the level, not whether a ticket happened to be refused since the last sample)
			const uint64_t budget = budget_of(l);
			const uint64_t need = (g.pause & kVideoMemory) != 0 ? g.refused_bytes : 0;
			if (budget != 0 && (l.vram_usage + need) * 100 > budget * kStopPercent)
				g.pause |= kVideoMemory;
			else
			{
				g.pause &= ~uint32_t(kVideoMemory);
				g.refused_bytes = 0;
			}
			publish_locked();
		}
		if (g.wake != nullptr)
			wake();
	}

	void pause(uint32_t bits)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.pause |= bits & kCallerBits;
		publish_locked();
	}

	void resume(uint32_t bits)
	{
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.pause &= ~(bits & kCallerBits);
			publish_locked();
		}
		if (g.wake != nullptr)
			wake();
	}

	uint32_t paused()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		return g.pause;
	}

	bool pressure()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		return (g.pause & kVideoMemory) != 0;
	}

	counts get_counts()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		counts c = g.n;
		c.epoch = g.epoch;
		c.pause = g.pause;
		c.owners = uint32_t(g.owners.size());
		c.queued = g.queue.size();
		return c;
	}

	std::vector<std::string> status()
	{
		const counts c = get_counts();
		std::vector<std::string> out;
		char line[400];
		if (!c.started)
		{
			out.push_back("streamer: not started");
			return out;
		}
		std::snprintf(line, sizeof(line),
			"streamer: %llu asked (%llu shared), %llu queued, %llu copies in flight, %llu textures resident (%llu MB, at most %llu at once; %llu MB not taken twice by sharing)",
			static_cast<unsigned long long>(c.asked), static_cast<unsigned long long>(c.shared), static_cast<unsigned long long>(c.queued),
			static_cast<unsigned long long>(c.in_flight), static_cast<unsigned long long>(c.resident), megabytes(c.resident_bytes),
			static_cast<unsigned long long>(c.resident_peak), megabytes(c.shared_bytes));
		out.push_back(line);
		if (c.held != 0)
		{
			std::snprintf(line, sizeof(line), "  %llu textures (%llu MB) kept for owners that haven't let go of them since the pack was opened, closed or reloaded",
				static_cast<unsigned long long>(c.held), megabytes(c.held_bytes));
			out.push_back(line);
		}
		std::snprintf(line, sizeof(line), "  read %llu MB in %.0f ms; made %llu textures (%llu without their top level), %llu let go behind the copies, %llu to the retire list, %llu objects left",
			megabytes(c.bytes_read), c.read_ms, static_cast<unsigned long long>(c.made), static_cast<unsigned long long>(c.skipped_top),
			static_cast<unsigned long long>(c.released), static_cast<unsigned long long>(c.retired), static_cast<unsigned long long>(c.leaked));
		out.push_back(line);
		std::snprintf(line, sizeof(line), "  upload ring %llu MB (%s), wrapped %llu times; at most %llu MB started in one present; paused 0x%x",
			megabytes(c.ring_bytes), c.ring_page == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK ? "write-back" : "not write-back", static_cast<unsigned long long>(c.ring_wraps),
			megabytes(c.present_max_bytes), c.pause);
		out.push_back(line);
		std::string refused = "  refused:";
		for (size_t m = 1; m < size_t(miss::count_); ++m)
			if (c.refused[m] != 0)
				refused += std::string(" ") + text(miss(m)) + " " + std::to_string(c.refused[m]) + ";";
		if (refused.back() == ':')
			refused += " none";
		out.push_back(refused);
		if (!c.failure.empty())
			out.push_back("  the pack is off: " + c.failure);
		return out;
	}

	void take_log(std::vector<std::string> &out)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		for (std::string &l : g.log)
			out.push_back(std::move(l));
		g.log.clear();
		if (g.log_dropped != 0)
			out.push_back("texture pack: " + std::to_string(g.log_dropped) + " more lines left out");
		g.log_dropped = 0;
	}

	namespace test
	{
		std::unique_lock<std::mutex> hold()
		{
			return std::unique_lock<std::mutex>(g.mutex);
		}

		void gate_copies(ID3D12Fence *fence, uint64_t value)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.test_gate = fence;
			g.test_gate_value = value;
		}

		void before_pick(void (*fn)())
		{
			g_before_pick = fn;
		}

		void after_load(void (*fn)())
		{
			g_after_load = fn;
		}

		void fail_parts(const char *why)
		{
			g_fail_parts = why;
		}

		size_t done_waiting(owner o)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			auto it = g.owners.find(o);
			return it != g.owners.end() ? it->second.done.size() : 0;
		}
	}
}
