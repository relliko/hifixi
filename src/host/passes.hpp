#pragma once

// The own-frame passes (PLAN-own-frame.md): every part of the game's frame HiFi XI takes over after the zone and the
// characters registers here, so the plugin and the backend call it without knowing it by name.
//
// - The Direct3D 12 side: a handler for one slot-marker kind (src/game/slotmark.hpp, slotmark::first_pass on), called on
//   dgVoodoo's recording thread inside the hooks at that marker, with the list being recorded, the state dgVoodoo had
//   bound (to put back) and the world's target and depth as the markers found them.
// - The game side: a module the plugin calls each present (the game's thread), every half second to follow its options
//   and patches, for /hifi status lines, at Release (put the game's code back) and for /hifi <name> commands.
//
// Registration is static: a module's .cpp holds `static const hifi::passes::registrar r { ... };`. Every build that
// compiles such a .cpp compiles passes.cpp too.

#include <d3d12.h>
#include <cstdint>
#include <string>
#include <vector>

namespace grass
{
	struct list_state;
}

namespace hifi::passes
{
	// The world as the markers name it (as zone::inject::world and chars::inject::world)
	struct world
	{
		ID3D12Resource *rt = nullptr, *ds = nullptr;
		DXGI_FORMAT rt_format = DXGI_FORMAT_UNKNOWN, ds_format = DXGI_FORMAT_UNKNOWN;
		D3D12_RESOURCE_STATES rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		// The depth's two planes' states as dgVoodoo's barriers left them (the zone renderer follows them), when known
		bool planes_known = false;
		D3D12_RESOURCE_STATES ds_planes[2] = {};
		// What dgVoodoo last bound on the list (its OMSetRenderTargets: bound lazily, so it may be older than the marker),
		// the target's view's format; the back buffer for the kinds slotmark::on_back_buffer names
		ID3D12Resource *bound_rt = nullptr, *bound_ds = nullptr;
		DXGI_FORMAT bound_rt_format = DXGI_FORMAT_UNKNOWN;
		// The list's descriptor heaps as dgVoodoo last set them (heap_count 0: not known yet): bound again after a pass
		// binds its own, before grass::list_state::restore (a root table must be in the heaps bound when it's set)
		UINT heap_count = 0;
		ID3D12DescriptorHeap *heaps[2] = {};
	};

	using marker_fn = void (*)(ID3D12GraphicsCommandList *list, const grass::list_state &bound, const world &w, uint32_t seq_low4);

	struct module
	{
		const char *name = nullptr;                                   // (also its /hifi command)
		// Each present while the backend runs (in_game: logged in, in a zone)
		void (*present)(double now, bool in_game) = nullptr;
		// Every half second: patches in or out as its options say
		void (*update)(double now) = nullptr;
		std::vector<std::string> (*status)() = nullptr;
		// Release: the game's code put back; false and why when it couldn't be
		bool (*stop)(std::string &why) = nullptr;
		// /hifi <name> [args]: lines to print; false when it isn't a command of this module's
		bool (*command)(const std::vector<std::string> &args, std::vector<std::string> &out) = nullptr;
		// The Direct3D 12 device, once the backend has one (the game's thread); nullptr at Release
		void (*device)(ID3D12Device *device) = nullptr;
	};

	// Each kind has at most one handler (the last registered wins); kinds below slotmark::first_pass are the backend's own
	void on_marker(uint32_t kind, marker_fn fn);
	void add(const module &m);

	struct registrar
	{
		registrar(const module &m) { add(m); }
		registrar(uint32_t kind, marker_fn fn) { on_marker(kind, fn); }
	};

	// The backend and the plugin
	// (dgVoodoo's lists: reset, and each SetDescriptorHeaps, followed for world::heaps)
	void on_reset(ID3D12GraphicsCommandList *list);
	void on_heaps(ID3D12GraphicsCommandList *list, UINT count, ID3D12DescriptorHeap *const *heaps);
	bool dispatch(uint32_t kind, ID3D12GraphicsCommandList *list, const grass::list_state &bound, const world &w, uint32_t seq_low4);
	const std::vector<module> &modules();
}
