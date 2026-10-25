#pragma once

// HiFi XI's hold on dgVoodoo's Direct3D 12 output. No detours: the runtime's command list, queue and device classes
// keep one vtable each in D3D12Core.dll, shared by every object of the class, so swapping slots in those vtables sees
// every call dgVoodoo makes. (A command list gets a vtable of its own at its first Reset, a copy with the driver's
// functions in its draw and state slots: each is hooked in place when the list is first seen.) They're found by
// making a throwaway device, queue and list. Each slot's function is checked to live in D3D12's own runtime (or its
// debug layer, when that's on) first: behind another layer (ReShade) nothing is hooked.
//
// Best before dgVoodoo makes its device (early: HiFi XI's starter, bootloader\dxgi.dll, calls in when dgVoodoo first
// asks DXGI for a factory): every list is then seen from its creation, every view from its making. An Ashita plugin
// alone loads seconds after the device exists (late): lists are known from their next Reset, views made before
// aren't known at all.
//
// The hooks count every call (stats) and hand them to the backend (events.hpp), unless the thread is quiet (the
// backend's own recording on dgVoodoo's lists).

// (no d3d12.h here: the plugin includes Ashita's Direct3D 8 headers beside this; events.hpp has the events)
#include <cstdint>
#include <string>

namespace hifi::host
{
	// The hooked slots, in the order the counters are kept: the list's, the queue's, the device's
	enum slot : uint32_t
	{
		list_reset, list_close, list_barrier, list_om_targets, list_pipeline, list_topology, list_viewports, list_scissors,
		list_root_signature, list_root_table, list_root_constant, list_root_constants, list_root_cbv, list_root_srv, list_root_uav,
		list_clear_depth, list_clear_target, list_draw, list_draw_indexed,
		list_execute_indirect, list_execute_bundle, list_clear_state, list_barrier7,
		queue_execute,
		device_create_list, device_create_rtv, device_create_dsv, device_copy, device_copy_simple,
		slot_count,
	};
	constexpr bool is_list_slot(uint32_t s) { return s < queue_execute; }
	constexpr bool is_device_slot(uint32_t s) { return s > queue_execute && s < slot_count; }
	const char *slot_name(uint32_t s);

	// Swaps the slots in. False, with why, when the runtime's vtables can't be found or aren't D3D12's own (nothing
	// changed then). Safe to call again once installed (returns true). adapter: the IDXGIAdapter to make the throwaway
	// device on (nullptr: the default one; the starter passes WARP's, before the game's device is made).
	bool install(std::string &why, void *adapter = nullptr);
	// Puts back every slot that still holds ours (one some other hooker swapped over ours since is left alone)
	void uninstall();
	bool installed();

	// While one of these lives on a thread, the hooks there only call on: no counts, no events (the backend's own
	// recording on dgVoodoo's lists, its own views)
	struct quiet
	{
		quiet();
		~quiet();
		quiet(const quiet &) = delete;
		quiet &operator=(const quiet &) = delete;
	};
	bool is_quiet();

	// A tracer (the stream log, /hifi streamlog: src/backend/streamlog): while one is set, every hooked call is handed to
	// it before it's made, on the thread making it, with its object and its arguments in order (pointers and handles as
	// their values, floats as their bits, signed numbers sign-extended); quiet: made by HiFi XI itself (World FX drawing
	// into the frame). Not while the hooks are out.
	using tracer = void (*)(uint32_t s, const void *self, const uint64_t *args, uint32_t count, bool quiet);
	void set_tracer(tracer t);

	struct target_seen
	{
		const void *resource = nullptr; // (an ID3D12Resource, not referenced: a pointer to compare)
		uint64_t width = 0;
		uint32_t height = 0;
		uint32_t format = 0; // (DXGI_FORMAT)
		uint64_t ends = 0; // times it went from render target to read in a barrier (the world's end)
	};
	struct stats
	{
		uint64_t calls[slot_count] = {};
		uint64_t lists_executed = 0;       // lists submitted through ExecuteCommandLists
		uint64_t lists_foreign = 0;        // ... whose vtable couldn't be hooked (not the runtime's: a wrapper?)
		uint64_t vtables_hooked = 0;       // lists' own vtables hooked in place
		uint64_t vtables_refused = 0;      // ... not hooked, not being the runtime's copy
		uint64_t lists_seen = 0;           // distinct lists made or reset since the hooks went in
		uint64_t lists_unknown_state = 0;  // calls on a list not made or reset since the hooks went in
		target_seen world;                 // the largest 2D target seen turning from render target to read
		target_seen depth;                 // the largest depth buffer seen in a barrier
		uint32_t queues_seen = 0;
	};
	stats get_stats();
	void reset_stats();
}
