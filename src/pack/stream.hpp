#pragma once

// The texture pack's streamer (docs/texture-pack/streamer.md, the design; docs/texture-pack-plan.md M3): pack records
// read on a thread of ours straight into an upload ring and copied into textures of their own on a copy queue of ours,
// for owners that draw them (zones and model actors in M4, characters in M5).
//
//   - One worker thread (below normal; raised for the join), one COPY queue, one fence only it signals, four allocators
//     and lists (at most 4 copies in flight). A ticket is ready only once the worker has seen the copy fence pass its
//     copy (a CPU check in place of a GPU Wait: docs/d3d12/sync.md), stored with memory_order_release.
//   - The ring: one buffer in a CUSTOM heap (CPU write-back, MEMORY_POOL_L0), made in COMMON, mapped once, kept until
//     stop() or device loss. The reader reads each record's levels into it and checks their CRCs there (an UPLOAD
//     mapping is write-combined: read back at 57 MB/s on the 4070 Ti). A record's span never straddles its end.
//   - Textures: committed, DEFAULT heap, made in COMMON, written once on the copy queue, read by implicit promotion;
//     nobody ever puts a barrier on one (docs/d3d12/barriers.md). Shared by every owner asking the same key and levels.
//   - The streamer owns no shader-visible heap: an owner makes its own view of a ready ticket with write_view, on its
//     own thread, into a slot it alone decides is free.
//   - One mutex, a leaf: never held across ReadFile, a D3D12 or DXGI call, a fence wait, pack::load or a call into
//     retire; no callbacks. An owner may call in with its own lock held (owner lock, then the streamer's).
//   - Textures that were ready go through retire::later (the game queue's fence, the +2 rule) at the last drop of the
//     tickets holding them, gone ones too: an open, close or reload turns a ready ticket gone but leaves it its texture
//     (held) till it's dropped (docs/texture-pack/zones.md decision 1); stop, a new device or a loss let go at once.
//     What only the copy queue ever saw (never ready, the ring, queue, lists, fence) is released by the worker once its
//     own fence passed, or leaked on purpose when that doesn't happen within the wait.
//
// Started by the texture pack's module (src/pack/pack_module.cpp) at the first enable with a device, kept for the
// session (paused with kOff while the pack is off); the zones and model actors ask through src/zone/zonepack;
// packstream_test drives it alone.

#include "pack.hpp"
#include <d3d12.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace hifi::pack::stream
{
	using owner = uint32_t;
	struct ticket
	{
		uint32_t slot = 0, generation = 0; // (generation 0: no ticket)
		bool operator==(const ticket &) const = default;
	};
	// none: no ticket. queued: waiting for the worker. reading, copying: the worker has it (alone, or joined to another
	// ticket's texture). ready: get() and write_view() answer. missed: never (reason()). dropped: dropped, not yet
	// reused. gone: an older epoch's (pack opened, closed or reloaded, new device, device lost, stop), or reused
	enum class state : uint8_t
	{
		none,
		queued,
		reading,
		copying,
		ready,
		missed,
		dropped,
		gone,
	};
	const char *text(state s);
	// Why a ticket missed (counted by reason)
	enum class miss : uint8_t
	{
		none,
		no_pack,       // (no pack open)
		not_in_pack,   // (no entry with the key, or no record of the kind)
		no_pack_flag,  // (the entry's flag: the game's texture is drawn)
		rule,          // (a record breaking format.md's rules, or a level range past its levels)
		first_level,   // (first > 0 and that level's sides aren't multiples of 4: a BC texture's top level must be)
		too_big,       // (the levels' span bigger than the upload ring)
		read,          // (a read failed or was cut short or cancelled)
		crc,           // (a level's CRC didn't match)
		create,        // (the texture couldn't be made)
		lost,          // (the device was lost with it queued or in flight)
		no_ring,       // (the ring, queue, lists or fence couldn't be made: the pack is off)
		count_,
	};
	const char *text(miss m);

	struct want
	{
		key k;
		uint8_t kind = kColour;
		uint8_t first = 0, count = 0; // levels: the top `first` never wanted; count 0: all the rest
		int32_t priority = 0;         // (higher first; then the order asked)
		uint32_t epoch = 0;           // (the snapshot's the key was found in; 0: the current one)
		// (count 0 and these set: the levels from `first` down to the first whose sides are at most these, the game's own
		// size (zones: the key's w x h), worked out from the record under the streamer's lock; 0: all the rest)
		uint32_t down_to_w = 0, down_to_h = 0;
	};
	struct view
	{
		// (valid while the ticket is held, and until the retire fence after its drop: an open, close or reload turns a
		// ready ticket gone but leaves it its texture till it's dropped (zones.md decision 1). Stop, a new device and the
		// device lost let go at once: owners are cleared before stop, a device's objects are freed when retire sees the
		// next, and nothing runs on a lost one. get() and write_view() answer only for a ready ticket)
		ID3D12Resource *resource = nullptr;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		uint32_t w = 0, h = 0, levels = 0, first = 0; // (w x h: level `first` of the record's, the texture's level 0)
		uint64_t bytes = 0;                           // (its allocation: GetResourceAllocationInfo)
	};
	struct limits
	{
		uint64_t vram_usage = 0, vram_budget = 0, budget_cap = 0; // (0: unknown or none; the budget the lower of the two)
		uint64_t avail_virtual = 0, largest_free = 0;            // (0: not measured, no limit)
		uint64_t per_present = 64ull << 20;                      // (record bytes started in one present; 0: no cap)
	};
	// Pause bits: set by callers (kOff the option, kSafeStart backend::safe_start, kQuiet the module's zone-in window,
	// kTest the tests); set by the worker (kVideoMemory over 95% of the budget, kAddressSpace below the floor, kLost)
	enum : uint32_t
	{
		kOff = 1,
		kSafeStart = 2,
		kQuiet = 4,
		kTest = 8,
		kCallerBits = kOff | kSafeStart | kQuiet | kTest,
		kVideoMemory = 0x100,
		kAddressSpace = 0x200,
		kLost = 0x400,
	};
	// The pacing and the floors (streamer.md "Pacing", "Budget, address-space floor, quiet window")
	constexpr uint32_t kInFlight = 4;
	constexpr uint64_t kFloorVirtual = 512ull << 20, kFloorBlock = 64ull << 20;
	constexpr uint32_t kSkipPercent = 85, kStopPercent = 95;
	constexpr uint32_t kRingMinMb = 8, kRingMaxMb = 64, kRingDefaultMb = 32;

	// (the module, game thread) The worker started with the device (AddRef'd: nullptr, none yet) and the ring's size
	// (clamped to 8-64 MB); false when it already runs. set_device on a new device or nullptr (the epoch raised first,
	// every ticket gone); device_lost when gpu_crash::lost() says so; stop() joins (under 100 ms)
	bool start(ID3D12Device *device, uint32_t ring_mb = kRingDefaultMb);
	void set_device(ID3D12Device *device);
	void device_lost();
	void stop();
	bool running();
	// The pack: loaded and checked on the worker (pack::load); the index now (nullptr: none) and its epoch. Each open
	// and close cancels the read in flight and raises the epoch once the worker has done it. Each returns its number,
	// and posts_done() the number of the last the worker has done: an open still loading isn't cancelled by a newer
	// open or close, so it raises the epoch too, before the newer one does. Whoever waits for its own post waits for
	// posts_done() to reach it, not for the epoch to move (the module's check_open)
	uint64_t open(const std::wstring &folder);
	uint64_t open(std::shared_ptr<const index> pack); // (tests: an index they built)
	uint64_t close();
	uint64_t posts_done();
	std::shared_ptr<const index> current();
	uint32_t epoch();
	// The last open's failure ("" when it loaded, or none was asked)
	std::string open_status();

	// Owners and their textures (any thread; no device call, no I/O, no wait)
	owner add_owner(const char *name);
	void drop_owner(owner o); // (every ticket it holds dropped; the owner may be destroyed right after)
	ticket request(owner o, const want &w);
	state poll(ticket t); // (no lock: an acquire load)
	miss reason(ticket t);
	// The owner's tickets that became ready, missed or gone since its last call, each once, with their state now (in no
	// particular order; a dropped ticket isn't among them). An owner that never calls it costs nothing more
	void take_done(owner o, std::vector<std::pair<ticket, state>> &out);
	bool get(ticket t, view &out);
	void drop(ticket t); // (any state; never blocks)
	uint32_t in_flight(owner o); // (the owner's tickets queued, reading or copying)
	// (the owner's thread, none of its locks held, the ticket held) The ticket's SRV into the owner's slot: its format,
	// the levels uploaded, most detailed 0. false when it isn't ready. Safe against an epoch change on another thread
	// (the texture and device are held for the call), but a view made just before one is the owner's to drop
	bool write_view(ticket t, D3D12_CPU_DESCRIPTOR_HANDLE slot);

	// Pacing, limits, pausing
	void on_present(); // (the module's present: a new present's bytes)
	void set_limits(const limits &l);
	void pause(uint32_t bits);  // (caller bits only)
	void resume(uint32_t bits);
	uint32_t paused();          // (all bits, the worker's too)
	// (over 95% of the video memory budget: owners drop what they can) kVideoMemory: set by set_limits whose numbers are
	// over, or by a texture refused for its own size, and kept by each set_limits under which it still wouldn't fit
	bool pressure();

	// Status
	struct counts
	{
		bool started = false;
		uint32_t epoch = 0, pause = 0, owners = 0;
		uint64_t asked = 0, shared = 0;          // (requests; of those, ready at once or joined to a texture on its way)
		uint64_t shared_bytes = 0;               // (video memory not taken twice now: each resident texture's bytes times
		                                         // its references past the first)
		uint64_t queued = 0, in_flight = 0;      // (tickets waiting; copies submitted and not yet seen done)
		uint64_t resident = 0, resident_bytes = 0, resident_peak = 0;
		uint64_t held = 0, held_bytes = 0;       // (textures an open, close or reload took out while gone tickets still
		                                         // hold them: let go at each one's last drop)
		uint64_t made = 0, made_bytes = 0;       // (textures made)
		uint64_t released = 0;                   // (released by the worker: never ready, behind its own fence)
		uint64_t retired = 0;                    // (handed to retire::later: were ready)
		uint64_t leaked = 0;                     // (objects left on purpose: copies not seen done in time)
		uint64_t skipped_top = 0, requeued = 0;  // (made without the top level; put back by the 95% stop)
		uint64_t refused[size_t(miss::count_)] = {};
		uint64_t bytes_read = 0, reads = 0;
		double read_ms = 0.0;
		uint64_t ring_bytes = 0, ring_wraps = 0, ring_skipped = 0, ring_waits = 0;
		D3D12_HEAP_TYPE ring_heap = D3D12_HEAP_TYPE(0);  // (GetHeapProperties of the ring, read on the worker)
		D3D12_CPU_PAGE_PROPERTY ring_page = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		D3D12_MEMORY_POOL ring_pool = D3D12_MEMORY_POOL_UNKNOWN;
		uint64_t presents = 0, present_max_bytes = 0, present_alone = 0, present_breaks = 0;
		uint64_t copies = 0;
		std::string failure; // (the worker's GPU parts couldn't be made: why)
	};
	counts get_counts();
	std::vector<std::string> status();
	// Lines for the log since the last call, each once: a texture the worker couldn't make for a ticket that asked it (a
	// broken rule, a failed or cut-short read, a CRC mismatch, the texture not made: its name, key, data file and why)
	// and the GPU parts not made. The streamer logs nothing itself (no callbacks); the module writes these from update().
	// At most kLogLines are kept between calls, then a line saying how many more were left out
	constexpr size_t kLogLines = 64;
	void take_log(std::vector<std::string> &out);

	namespace test
	{
		// (tests only) The streamer's mutex held while the result lives: the crash section must not wait on it
		std::unique_lock<std::mutex> hold();
		// (tests only) Every copy submitted from now on held on the GPU until fence reaches value (its queue Waits on it
		// first); nullptr: none. The test keeps the fence alive and signals it. So a test sees that nothing reads ready and
		// no ring space is used again while a copy hasn't run
		void gate_copies(ID3D12Fence *fence, uint64_t value);
		// (tests only) Called on the worker between its loop's top and the pick of the next texture, no lock held
		// (nullptr: none): a test calls set_device and request there, the race of a device change landing in that window
		void before_pick(void (*fn)());
		// (tests only) Called on the worker in each open, after pack.idx is loaded and before it's put in use, no lock
		// held (nullptr: none): a test holds an open there while it asks for another, or changes the device
		void after_load(void (*fn)());
		// (tests only) How many tickets the owner's next take_done would hand over
		size_t done_waiting(owner o);
	}
}
