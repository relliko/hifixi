/*
 * Offline test of the world part's modules as the plugin and the backend drive them (src/world/world_module.cpp through
 * src/host/passes): on the first FFXiMain dump and WARP with the debug layer,
 *   - the modules registered ("sky", "names", "grid"), the device handed over (the renderer made, the font read);
 *   - their update putting each pass's game side in, and the gate: each pass left to the game while its markers aren't
 *     coming through, HiFi XI's once they are (the markers sent through worldmark's sink straight to the handlers, as
 *     dgVoodoo hands them to the backend, which calls zone::inject::on_marker first);
 *   - the handlers drawing the native frames into the world (the sky without depth, the names and a depth-tested grid
 *     outline with the world's depth planes taken from zone/inject's tracking and put back), dgVoodoo's state bound
 *     again; the pixels in the world's target; nothing said by the debug layer;
 *   - /hifi sky|names|grid status and on/off; stop() leaving the dump as it was.
 */

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include "../src/game/griddraw.hpp"
#include "../src/game/namedraw.hpp"
#include "../src/game/skydraw.hpp"
#include "../src/game/slotmark.hpp"
#include "../src/game/worldmark.hpp"
#include "../src/host/passes.hpp"
#include "../src/render/grass_render.hpp"
#include "../src/world/frames.hpp"
#include "../src/zone/inject.hpp"
#include "game_dump.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	namespace sd = hifi::game::skydraw;
	namespace nd = hifi::game::namedraw;
	namespace gd = hifi::game::griddraw;

	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	uint32_t word_at(uintptr_t a)
	{
		uint32_t v;
		std::memcpy(&v, reinterpret_cast<const void *>(a), 4);
		return v;
	}
	void put_word(uintptr_t a, uint32_t v)
	{
		std::memcpy(reinterpret_cast<void *>(a), &v, 4);
	}

	// What dgVoodoo's recording thread would be in: the list, its bound state, the world
	ID3D12GraphicsCommandList *g_list = nullptr;
	grass::list_state g_bound;
	hifi::passes::world g_world;
	uint32_t g_dispatched[16] = {};
	void sink(uint32_t kind, uint32_t seq)
	{
		// (as the backend: the depth to zone/inject first, then the pass's handler)
		hifi::zone::inject::on_marker(g_world.ds);
		if (hifi::passes::dispatch(kind, g_list, g_bound, g_world, seq & 15))
			g_dispatched[kind]++;
	}

	// The game's callees, standing in
	void __cdecl game_dome(float, uint32_t, uint32_t, const uint32_t *, const float *) {}
	uint8_t __fastcall game_page(uintptr_t, uintptr_t, uint32_t, uint32_t *, uint32_t *) { return 1; }
	long __fastcall game_glyph(uintptr_t, uintptr_t, uint32_t, uint32_t, const void *, uint32_t) { return 0; }
	void __cdecl game_stars(const float *, uint32_t, uint32_t) {}
	std::vector<uint8_t> g_font(0x40);
	// 086210 as far as the patched calls go: the letter 'A' of 'font fontshp' (u 211, v 131, 19 x 23) at 40, 30, z 0.5
	void __cdecl game_name(const float *, const char *, float, uint32_t colour, uint32_t ls)
	{
		uint32_t c = colour, l = ls;
		nd::on_page(uintptr_t(g_font.data()), 0, 'A', &c, &l);
		hifi::world::name_vertex v[4];
		for (int k = 0; k < 4; ++k)
		{
			v[k].x = 40.0f - 0.5f + (k & 1 ? 19.0f : 0.0f);
			v[k].y = 30.0f - 0.5f + (k & 2 ? 23.0f : 0.0f);
			v[k].z = 0.5f;
			v[k].rhw = 1.0f;
			v[k].colour = c;
			v[k].u = float(211 + (k & 1 ? 19 : 0)) / 256.0f;
			v[k].v = float(131 + (k & 2 ? 23 : 0)) / 256.0f;
		}
		nd::on_glyph(0x1234, 0, 5, 2, v, 28);
	}
	uint8_t __fastcall game_pass(uintptr_t, uintptr_t)
	{
		const float pos[4] = { 0.0f, 0.0f, 5.0f, 1.0f };
		nd::on_name(pos, "A", 1.0f, 0xC0808080, 0x80808080);
		return 0;
	}
	void __cdecl game_outline(const float *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
	uint32_t __fastcall game_grid(uintptr_t, uintptr_t, uint32_t)
	{
		float c[16] = {};
		const float a[4][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
		for (int k = 0; k < 4; ++k)
		{
			c[k * 4 + 0] = a[k][0];
			c[k * 4 + 2] = a[k][1];
		}
		gd::on_outline(c, 0xFF, 0x60, 0x60, 0x80, 1);
		return 0;
	}

	const hifi::passes::module *module_named(const char *name)
	{
		for (const hifi::passes::module &m : hifi::passes::modules())
			if (m.name != nullptr && std::strcmp(m.name, name) == 0)
				return &m;
		return nullptr;
	}
	bool says(const std::vector<std::string> &lines, const char *text)
	{
		for (const std::string &l : lines)
			if (l.find(text) != std::string::npos)
				return true;
		return false;
	}
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	uintptr_t base = 0;
	const size_t size = dump::map(dump::kDumps[0], base);
	if (size == 0)
	{
		MEMORY_BASIC_INFORMATION mi = {};
		VirtualQuery(reinterpret_cast<void *>(dump::base_of(dump::kDumps[0])), &mi, sizeof(mi));
		std::printf("     (at the dump's base: state %lX, allocation %p, region %p + %zX)\n", mi.State, mi.AllocationBase, mi.BaseAddress, mi.RegionSize);
		check(false, "the FFXiMain dump mapped");
		return 1;
	}
	const hifi::passes::module *sky = module_named("sky"), *names = module_named("names"), *grid = module_named("grid");
	check(sky != nullptr && names != nullptr && grid != nullptr && sky->update != nullptr && sky->device != nullptr && names->status != nullptr && grid->command != nullptr,
		"the three modules registered (sky with the update and device for all)");
	if (sky == nullptr || names == nullptr || grid == nullptr)
		return 1;

	ID3D12Debug *debug = nullptr;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
		debug->EnableDebugLayer();
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
	ID3D12Device *device = nullptr;
	D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
	if (device == nullptr)
	{
		check(false, "a WARP device");
		return 1;
	}
	ID3D12InfoQueue *info = nullptr;
	device->QueryInterface(IID_PPV_ARGS(&info));
	ID3D12CommandQueue *queue = nullptr;
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

	// The world: A (R8G8B8A8) and its depth (D24S8), dgVoodoo's own views of them
	const UINT W = 160, H = 96;
	D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, rb = { D3D12_HEAP_TYPE_READBACK };
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = W;
	td.Height = H;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	ID3D12Resource *rt = nullptr, *ds = nullptr, *readback = nullptr;
	D3D12_CLEAR_VALUE black_value = {};
	black_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &black_value, IID_PPV_ARGS(&rt));
	td.Format = DXGI_FORMAT_R24G8_TYPELESS;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	D3D12_CLEAR_VALUE far_value = {};
	far_value.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	far_value.DepthStencil.Depth = 1.0f;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &far_value, IID_PPV_ARGS(&ds));
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = UINT64(W) * H * 4;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
	ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap));
	device->CreateRenderTargetView(rt, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
	D3D12_DEPTH_STENCIL_VIEW_DESC dsd = {};
	dsd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dsd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	device->CreateDepthStencilView(ds, &dsd, dsv_heap->GetCPUDescriptorHandleForHeapStart());
	g_world.rt = rt;
	g_world.ds = ds;
	g_world.rt_format = DXGI_FORMAT_R8G8B8A8_UNORM;
	g_world.ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	g_world.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
	// (dgVoodoo's state on the list: its target and depth, a viewport and scissor; restored after each pass)
	g_bound.rtvs[0] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	g_bound.rtv_count = 1;
	g_bound.dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
	g_bound.viewports[0] = { 0.5f, 0.5f, float(W), float(H), 0.0f, 1.0f };
	g_bound.viewport_count = 1;
	g_bound.scissors[0] = { 0, 0, LONG(W), LONG(H) };
	g_bound.scissor_count = 1;
	g_bound.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	ID3D12CommandAllocator *alloc = nullptr;
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&g_list));
	const float black[4] = { 0, 0, 0, 0 };
	g_list->ClearRenderTargetView(g_bound.rtvs[0], black, 0, nullptr);
	g_list->ClearDepthStencilView(g_bound.dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	// (zone/inject follows the depth's planes from dgVoodoo's barriers: one that leaves both in DEPTH_WRITE)
	hifi::zone::inject::on_marker(ds);
	D3D12_RESOURCE_BARRIER seen = {};
	seen.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	seen.Transition.pResource = ds;
	seen.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	seen.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_READ;
	seen.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	hifi::zone::inject::on_barrier(1, &seen);

	// The device: the renderer made, the font read
	sky->device(device);
	hifi::game::worldmark::set_sink(sink);
	// The first update: the game sides in; nothing native yet (no marker seen)
	sky->update(0.0);
	check(sd::in() && nd::in() && gd::in(), "update(): the three game sides in");
	sd::set_draw(reinterpret_cast<uintptr_t>(&game_dome));
	nd::set_callees(reinterpret_cast<uintptr_t>(&game_pass), reinterpret_cast<uintptr_t>(&game_name), reinterpret_cast<uintptr_t>(&game_stars),
		reinterpret_cast<uintptr_t>(&game_page), reinterpret_cast<uintptr_t>(&game_glyph));
	gd::set_callees(reinterpret_cast<uintptr_t>(&game_grid), reinterpret_cast<uintptr_t>(&game_outline));
	check(says(sky->status(), "the game draws it") && says(names->status(), "the game draws them"), "before any marker: the game draws them");

	// A frame: the dome (West Ronfaure's cloudy midnight), the names, the grid; the wrapper's cache a camera looking north,
	// a little up; the field's phase; the font object's first codes
	std::vector<uint8_t> wrapper(0xA00);
	const float yaw = 0.0f, pitch = 0.3f;
	const float f[3] = { std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
	float r[3] = { f[2], 0.0f, -f[0] };
	const float rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
	r[0] /= rl;
	r[2] /= rl;
	const float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
	float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }, view[16] = {}, proj[16] = {};
	for (int i = 0; i < 3; ++i)
	{
		view[i * 4 + 0] = r[i];
		view[i * 4 + 1] = u[i];
		view[i * 4 + 2] = -f[i];
	}
	view[15] = 1.0f;
	const float ys = 1.0f / std::tan(0.6f);
	proj[0] = ys / (float(W) / float(H));
	proj[5] = ys;
	proj[10] = 65535.0f / (0.1f - 65535.0f);
	proj[11] = -1.0f;
	proj[14] = 0.1f * 65535.0f / (0.1f - 65535.0f);
	std::memcpy(wrapper.data() + 0x93C, ident, 64);
	std::memcpy(wrapper.data() + 0x97C, view, 64);
	std::memcpy(wrapper.data() + 0x9BC, proj, 64);
	const uintptr_t wrapper_global = sd::where().wrapper, counter = sd::where().counter, phase = nd::where().phase;
	const uint32_t wrapper_was = word_at(wrapper_global), counter_was = word_at(counter), phase_was = word_at(phase);
	put_word(wrapper_global, uint32_t(uintptr_t(wrapper.data())));
	put_word(counter, 21); // (past the session's first strips)
	uint32_t field = 0x60;
	put_word(phase, uint32_t(uintptr_t(&field)));
	const uint32_t firsts[2] = { 0x20, 0x8E };
	std::memcpy(g_font.data() + 0x24, firsts, 8);
	const uint32_t cols[8] = { 0x80414454, 0x80373A48, 0x80323441, 0x80323441, 0x80292C36, 0x80252630, 0x8015161E, 0x800E0E14 };
	const float heights[8] = { 0.0f, 0.165f, 0.273f, 0.381f, 0.49f, 0.66f, 0.828f, 1.0f };
	auto frame = [&]() {
		sd::on_dome(52427.92f, 8, 10, cols, heights);
		nd::on_pass(0, 0);
		gd::on_grid(0, 0, 0);
	};
	// A frame the game draws, its markers coming through; then the gate opens
	frame();
	sky->update(0.0);
	check(g_dispatched[hifi::slotmark::sky_end] == 1 && g_dispatched[hifi::slotmark::names_end] == 1 && g_dispatched[12] == 1, "each pass's marker reaches its handler");
	check(says(sky->status(), "sky dome: HiFi XI draws it") && says(names->status(), "names: HiFi XI draws them") && says(grid->status(), "layout grid: HiFi XI draws it"),
		"the markers coming through, HiFi XI's side up: the gate open for all three");
	// The native frame: drawn by the handlers into the world
	frame();
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = rt;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	g_list->ResourceBarrier(1, &b);
	D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
	src.pResource = rt;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.pResource = readback;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, W * 4 };
	g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	g_list->Close();
	ID3D12CommandList *lists[] = { g_list };
	queue->ExecuteCommandLists(1, lists);
	ID3D12Fence *fence = nullptr;
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	queue->Signal(fence, 1);
	HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	fence->SetEventOnCompletion(1, ev);
	WaitForSingleObject(ev, 60000);
	std::vector<uint8_t> px(size_t(W) * H * 4);
	uint8_t *p = nullptr;
	const D3D12_RANGE all = { 0, px.size() };
	if (SUCCEEDED(readback->Map(0, &all, reinterpret_cast<void **>(&p))))
	{
		std::memcpy(px.data(), p, px.size());
		readback->Unmap(0, nullptr);
	}
	auto at = [&](UINT x, UINT y) { return px.data() + (size_t(y) * W + x) * 4; };
	// (the top row is sky: the dome's alpha 0x80; the letter at 40..58, 30..52 over it; the grid's outline drawn with its
	// depth flag, so through the world's depth)
	const uint8_t *top = at(W - 2, 1);
	check(top[3] == 0x80 && (top[0] | top[1] | top[2]) != 0, "the sky's handler drew the dome into the world (its alpha 0x80)");
	uint32_t white = 0;
	for (UINT y = 30; y < 53; ++y)
		for (UINT x = 40; x < 59; ++x)
			white += at(x, y)[0] > 0xC0 && at(x, y)[1] > 0xC0 && at(x, y)[2] > 0xC0 ? 1 : 0;
	check(white > 40, "the names' handler drew the letter into the world with its depth (" + std::to_string(white) + " light pixels)");
	uint32_t red = 0;
	for (size_t i = 0; i < size_t(W) * H; ++i)
		red += px[i * 4] == 0xFF && px[i * 4 + 1] == 0x60 && px[i * 4 + 2] == 0x60 ? 1 : 0;
	check(red > 20, "the grid's handler drew the depth-tested outline (" + std::to_string(red) + " pixels)");
	const UINT64 messages = info != nullptr ? info->GetNumStoredMessages() : 0;
	uint32_t bad = 0;
	for (UINT64 i = 0; i < messages; ++i)
	{
		SIZE_T n = 0;
		info->GetMessage(i, nullptr, &n);
		std::vector<uint8_t> m(n);
		D3D12_MESSAGE *msg = reinterpret_cast<D3D12_MESSAGE *>(m.data());
		info->GetMessage(i, msg, &n);
		if (msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
		{
			bad++;
			std::printf("     debug layer: %s\n", msg->pDescription);
		}
	}
	check(bad == 0, "the debug layer said nothing (the depth's planes and the target taken and put back, dgVoodoo's state bound again)");
	for (const hifi::passes::module *m : { sky, names, grid })
		for (const std::string &l : m->status())
			std::printf("     %s\n", l.c_str());
	check(says(sky->status(), "drawn 1, failed 0") && says(names->status(), "drawn 1, failed 0") && says(grid->status(), "drawn 1, failed 0"), "each handler: one frame drawn, none failed");

	// /hifi names off: the game's again (its game side out); on: back in at the next update
	std::vector<std::string> out;
	check(names->command({ "names", "off" }, out) && !nd::in(), "/hifi names off: the names' game side out");
	out.clear();
	check(names->command({ "names", "on" }, out), "/hifi names on");
	sky->update(0.0);
	check(nd::in(), "update(): the names' game side in again");
	out.clear();
	check(sky->command({ "sky" }, out) && says(out, "/hifi sky on|off"), "/hifi sky: its status");

	// Out: the dump as it was
	put_word(wrapper_global, wrapper_was);
	put_word(counter, counter_was);
	put_word(phase, phase_was);
	hifi::game::worldmark::set_sink(nullptr);
	std::string why;
	const bool stopped = sky->stop(why) && names->stop(why) && grid->stop(why);
	check(stopped && !sd::in() && !nd::in() && !gd::in() && dump::same_as_file(dump::kDumps[0], base, size), "stop(): the dump again, byte for byte" + why);
	sky->device(nullptr);
	dump::unmap(base);
	CloseHandle(ev);
	std::printf(failures == 0 ? "\nall passed (%d)\n" : "\n%d failed\n", failures == 0 ? passes : failures);
	return failures == 0 ? 0 : 1;
}
