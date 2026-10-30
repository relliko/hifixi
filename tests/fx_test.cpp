/*
 * Offline test of the effects' game side (src/game/fxdraw.cpp) on both FFXiMain dumps (game_dump.hpp):
 *   - start(): the flush's call of the walk (05562B -> 055520), the walk's callback call (055539) and the element
 *     setup's call (04612E -> 0467E0) go through our stubs; every function and global it reads found where the game
 *     has them (046070, 0555A0, 040BA0, 045D40, 18BA20; VA 0490BC0C, the gfx, renderer, camera, environment and math
 *     objects); stop(): the dump again, byte for byte;
 *   - the game's own code run through the stubs: the flush stub on a made-up sorted list, the game's walk (055520), the
 *     element callback (0555A0) and element draw (046070), with made-up gfx, device, math and element objects (the math
 *     object's vtable the game's own: 045D40 builds the draw matrix with the game's matrix code): elements recorded into
 *     the flush's frame (the D3M's vertices, the draw matrix, TEXTUREFACTOR, blend, z write, runs), an fx_run marker
 *     for each run of elements before anything that isn't one (a see-through character, a lens flare's own draw, an
 *     element left to the game), none at the end with nothing pending, the game's setup called only for what's left;
 *   - textures: an element whose texture isn't on the GPU yet is the game's; once fxtextures has copied it in (WARP),
 *     it's recorded with the texture's id;
 *   - the gate: with native drawing off nothing is taken and no marker sent; the probe sends an empty run.
 */

#include <d3d12.h>
#include <dxgi1_4.h>
#include "../src/fx/fxframe.hpp"
#include "../src/fx/fxgeo.hpp"
#include "../src/fx/fxtextures.hpp"
#include "../src/game/fxdraw.hpp"
#include "../src/game/slotmark.hpp"
#include "game_dump.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	namespace fd = hifi::game::fxdraw;
	namespace fx = hifi::fx;
	namespace geo = hifi::fx::geo;

	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	size_t differing(const char *path, uintptr_t base, size_t size)
	{
		std::vector<uint8_t> file(size);
		FILE *f = nullptr;
		if (fopen_s(&f, path, "rb") != 0 || f == nullptr)
			return size_t(-1);
		const size_t got = fread(file.data(), 1, size, f);
		fclose(f);
		if (got != size)
			return size_t(-1);
		size_t n = 0;
		const uint8_t *image = reinterpret_cast<const uint8_t *>(base);
		for (size_t i = 0; i < size; ++i)
			n += image[i] != file[i];
		return n;
	}

	// A DAT's chunk data (after its 16-byte header) by type and name
	bool chunk(const char *rel, uint32_t type, const char *name, std::vector<uint8_t> &out)
	{
		std::ifstream f(std::string("C:\\Games\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI\\") + rel, std::ios::binary);
		if (!f)
			return false;
		std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		for (size_t off = 0; off + 16 <= d.size();)
		{
			uint32_t w = 0;
			std::memcpy(&w, &d[off + 4], 4);
			const size_t size = ((w >> 7) & 0x7FFFF) * 16;
			if (size < 16 || off + size > d.size())
				break;
			if ((w & 0x7F) == type && std::memcmp(&d[off], name, 4) == 0)
			{
				out.assign(d.begin() + off + 16, d.begin() + off + size);
				return true;
			}
			off += size;
		}
		return false;
	}

	// What the stand-ins saw
	struct seen
	{
		std::vector<std::string> order; // markers and callbacks as they came
		int setups = 0, render_states = 0, pre_draws = 0;
	};
	seen g_seen;
	uint32_t g_runs_at_marker[64] = {};
	uint32_t g_markers = 0, g_last_seq = 0;
	void sink(uint32_t kind, uint32_t seq)
	{
		g_last_seq = seq & 15;
		g_seen.order.push_back("marker " + std::to_string(kind) + " " + std::to_string(seq & 15));
		const fx::frame *f = fx::published(seq & 15);
		if (g_markers < 64)
			g_runs_at_marker[g_markers] = f != nullptr ? f->runs.load() : 0;
		g_markers++;
	}
	uint32_t __cdecl actor_callback(uintptr_t arg)
	{
		g_seen.order.push_back("actor " + std::to_string(arg));
		return 0;
	}
	uint32_t __cdecl flare_draw(uintptr_t arg)
	{
		g_seen.order.push_back("flare " + std::to_string(arg & 0xFFFF));
		return 0;
	}
	uint32_t __fastcall game_setup(uintptr_t element, uintptr_t, uint32_t arg)
	{
		g_seen.setups++;
		g_seen.order.push_back("game " + std::to_string(element & 0xFFFF) + (arg == 0xFFFF ? "" : " ?"));
		return 1; // (as if the game skipped it: its class draw isn't run here)
	}
	void __fastcall pre_draw(uintptr_t, uintptr_t)
	{
		g_seen.pre_draws++;
	}
	long __stdcall set_render_state(uintptr_t, uint32_t, uint32_t)
	{
		g_seen.render_states++;
		return 0;
	}

	// Memory of the test's own for the made-up objects (below 2 GB, 32-bit addresses)
	uint8_t *block(size_t size)
	{
		void *p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		std::memset(p, 0, size);
		return static_cast<uint8_t *>(p);
	}
	template <typename T>
	void put(uintptr_t at, T v)
	{
		std::memcpy(reinterpret_cast<void *>(at), &v, sizeof(T));
	}
	template <typename T>
	T get(uintptr_t at)
	{
		T v;
		std::memcpy(&v, reinterpret_cast<const void *>(at), sizeof(T));
		return v;
	}
	void identity(uintptr_t at)
	{
		const float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		std::memcpy(reinterpret_cast<void *>(at), m, 64);
	}
	uintptr_t address(const void *p)
	{
		return reinterpret_cast<uintptr_t>(p);
	}
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::string why;
	// The chunks the made-up elements draw: an untextured D3M (ROM/0/0 'coll', 12 triangles), a textured one (Fire's
	// 'fa04', 2 triangles, texture 'fai01   fai04   ') and its texture
	std::vector<uint8_t> plain, textured, texture;
	check(chunk("ROM\\0\\0.DAT", 0x1F, "coll", plain) && chunk("ROM\\11\\17.DAT", 0x1F, "fa04", textured), "the D3M chunks read");
	{
		// (the texture: the first 0x20 chunk named fai04 in Fire's DAT)
		std::ifstream f("C:\\Games\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI\\ROM\\11\\17.DAT", std::ios::binary);
		std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		for (size_t off = 0; off + 16 <= d.size();)
		{
			uint32_t w = 0;
			std::memcpy(&w, &d[off + 4], 4);
			const size_t size = ((w >> 7) & 0x7FFFF) * 16;
			if (size < 16)
				break;
			if ((w & 0x7F) == 0x20 && off + 33 <= d.size() && std::memcmp(&d[off + 17], "fai01   fai04   ", 16) == 0)
			{
				texture.assign(d.begin() + off + 16, d.begin() + off + size);
				break;
			}
			off += size;
		}
	}
	check(!texture.empty(), "its texture read");
	geo::d3m plain_m, textured_m;
	check(geo::parse_d3m(plain.data(), plain.size(), plain_m, why, false) && plain_m.textures == 0 && plain_m.triangles == 12, "the untextured D3M: 12 triangles");
	check(geo::parse_d3m(textured.data(), textured.size(), textured_m, why, false) && textured_m.textures == 1, "the textured D3M: one texture");

	// WARP for the texture's copy (the dumps' place kept free of its DLLs meanwhile: both ranges, 041C0000 on)
	constexpr uintptr_t kDumpsFrom = 0x041C0000, kDumpsTo = 0x05200000;
	void *kept_free = VirtualAlloc(reinterpret_cast<void *>(kDumpsFrom), kDumpsTo - kDumpsFrom, MEM_RESERVE, PAGE_NOACCESS);
	IDXGIFactory4 *factory = nullptr;
	CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	IDXGIAdapter *warp = nullptr;
	factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
	ID3D12Device *device = nullptr;
	check(SUCCEEDED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "a WARP device");
	ID3D12CommandQueue *queue = nullptr;
	const D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

	if (kept_free != nullptr)
		VirtualFree(kept_free, 0, MEM_RELEASE);
	for (const char *path : dump::kDumps)
	{
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		const std::string name = std::string(path).substr(std::string(path).rfind('\\') + 1);
		if (size == 0)
		{
			check(false, "mapped " + name);
			continue;
		}
		fd::set_sink(&sink);
		const bool started = fd::start(why);
		check(started && fd::in(), name + ": start()" + (why.empty() ? "" : ": " + why));
		const fd::sites s = fd::where();
		check(s.setup_call == base + 0x04612E && s.setup == base + 0x0467E0, name + ": the setup's call at 04612E, to 0467E0");
		check(s.flush_call == base + 0x05562B && s.walk == base + 0x055520, name + ": the flush's call at 05562B, to the walk 055520");
		check(s.entry_site == base + 0x055539 && s.element_cb == base + 0x0555A0, name + ": the walk's callback call at 055539; the element callback 0555A0");
		check(s.d3a_draw == base + 0x03EB30 && s.trail_cb == base + 0x01C930, name + ": the D3A draw 03EB30, the weapon trails' callback 01C930");
		check(s.element_draw == base + 0x046070 && s.d3m_draw == base + 0x040BA0 && s.draw_matrix == base + 0x045D40 && s.fog_env == base + 0x18BA20,
			name + ": 046070, 040BA0, 045D40, 18BA20");
		check(s.colour == base + 0x47BC0C && s.gfx == base + 0x47BFA8 && s.renderer == base + 0x631F24 && s.camera == base + 0x4568FC && s.env == base + 0x65CB04 &&
				s.math == base + 0x47CF7C,
			name + ": the colour (VA +47BC0C), gfx (+47BFA8), renderer (+631F24), camera (+4568FC), environment (+65CB04), math (+47CF7C)");
		const auto calls_to = [](uintptr_t at, uintptr_t to) { return get<uint8_t>(at) == 0xE8 && at + 5 + uintptr_t(get<int32_t>(at + 1)) == to; };
		check(calls_to(s.setup_call, address(reinterpret_cast<void *>(&fd::on_setup))), name + ": the setup's call goes to on_setup");
		check(calls_to(s.flush_call, s.code) && calls_to(s.entry_site, s.code + 0x20) && get<uint16_t>(s.entry_site + 5) == 0x9090, name + ": the flush and the walk go through our stubs");

		// The game's globals pointed at made-up objects (put back before the dump is compared)
		const uint32_t kept[6] = { get<uint32_t>(s.gfx), get<uint32_t>(s.renderer), get<uint32_t>(s.camera), get<uint32_t>(s.env), get<uint32_t>(s.math), get<uint32_t>(s.colour) };
		uint8_t *gfx = block(0x1000), *dev = block(0x400), *wrapper = block(0x1000), *renderer = block(0x40000), *camera = block(0x400), *math = block(0x1000);
		uint8_t *scratch = block(64 * 32), *mem = block(0x10000);
		uintptr_t *dev_vt = reinterpret_cast<uintptr_t *>(dev + 0x100);
		dev_vt[0xC8 / 4] = address(reinterpret_cast<void *>(&set_render_state));
		put<uint32_t>(address(dev), uint32_t(address(dev_vt)));
		put<uint32_t>(address(gfx) + 0xEBC, uint32_t(address(dev)));
		put<uint32_t>(address(gfx) + 0xDC8, uint32_t(address(wrapper)));
		put<uint32_t>(address(gfx) + 0xDEC, 0x800);
		for (const uintptr_t m : { 0xB50u, 0xB90u, 0xD10u, 0xD50u, 0xC10u })
			identity(address(gfx) + m);
		put<uint8_t>(address(wrapper) + 0x92D, 1);
		const float proj[16] = { 1.5f, 0, 0, 0, 0, 2.0f, 0, 0, 0, 0, -1.0f, -1, 0, 0, -0.1f, 0 };
		std::memcpy(renderer + 0x38DDC, proj, 64);
		put<uint8_t>(address(renderer) + 0x39598, 1);
		put<float>(address(renderer) + 0x39594, 0.0003f);
		put<float>(address(camera) + 0x2DC, 0.1f);
		put<float>(address(camera) + 0x2E0, 65535.0f);
		put<float>(address(camera) + 0x2E8, 1.0f);
		put<float>(address(camera) + 0x2F0, 0.5625f);
		put<float>(address(camera) + 0x2F4, 350.0f);
		put<uint32_t>(address(math), uint32_t(base + 0x32C368)); // (the game's matrix class)
		identity(address(math) + 0x810);
		for (int i = 0; i < 32; ++i)
			put<uint32_t>(address(math) + 0xA10 + 4 * i, uint32_t(address(scratch) + 64 * i));
		put<uint32_t>(s.gfx, uint32_t(address(gfx)));
		put<uint32_t>(s.renderer, uint32_t(address(renderer)));
		put<uint32_t>(s.camera, uint32_t(address(camera)));
		put<uint32_t>(s.env, 0); // (no environment: no fog asked for)
		put<uint32_t>(s.math, uint32_t(address(math)));

		// The elements: a vtable each kind (+0x2C the draw, +0x48 the class draw, +0x4C the pre-draw), a generator
		uintptr_t at = address(mem);
		auto take = [&](size_t n) { const uintptr_t a = at; at += (n + 15) & ~size_t(15); return a; };
		const uintptr_t vt_d3m = take(0x100), vt_flare = take(0x100);
		put<uint32_t>(vt_d3m + 0x2C, uint32_t(s.element_draw));
		put<uint32_t>(vt_d3m + 0x48, uint32_t(s.d3m_draw));
		put<uint32_t>(vt_d3m + 0x4C, uint32_t(address(reinterpret_cast<void *>(&pre_draw))));
		put<uint32_t>(vt_flare + 0x2C, uint32_t(address(reinterpret_cast<void *>(&flare_draw))));
		const uintptr_t gen = take(0x100), gen_handle = take(16);
		put<uint32_t>(gen_handle, uint32_t(gen));
		// (a resource: +0x14 its handle, +0x24 the header copy, +0x30 the data)
		auto resource = [&](const std::vector<uint8_t> &data, uint32_t type) {
			const uintptr_t r = take(0x30 + data.size() + 16);
			put<uint32_t>(r + 0x14, uint32_t(r + 0x10));
			put<uint32_t>(r + 0x24, type | uint32_t((data.size() + 16) / 16) << 7);
			std::memcpy(reinterpret_cast<void *>(r + 0x30), data.data(), data.size());
			return r;
		};
		const uintptr_t plain_res = resource(plain, 0x1F), textured_res = resource(textured, 0x1F), tex_res = resource(texture, 0x20);
		const uintptr_t tex_object = take(0x50);
		put<uint32_t>(tex_object + 8, uint32_t(tex_res));
		for (const uintptr_t r : { plain_res, textured_res })
		{
			const size_t names = geo::d3m_names_at(reinterpret_cast<const uint8_t *>(r + 0x30));
			put<uint16_t>(r + 0x32, uint16_t(get<uint16_t>(r + 0x32) | 0x8080)); // (made)
			put<uint32_t>(r + 0x30 + names + 4, 0x1234); // (a vertex buffer object)
			if (r == textured_res)
				put<uint32_t>(r + 0x30 + names, uint32_t(tex_object));
		}
		auto element = [&](uintptr_t vt, uintptr_t res, uint16_t letter, uint32_t flags, uint32_t colour, float z) {
			const uintptr_t e = take(0x200);
			put<uint32_t>(e, uint32_t(vt));
			put<float>(e + 0x138, 1.0f);
			put<float>(e + 0x134, 1.0f);
			put<uint32_t>(e + 0xF8, colour);
			put<uint32_t>(e + 0xFC, uint32_t(gen_handle));
			put<uint32_t>(e + 0x10C, flags);
			put<uint16_t>(e + 0x16C, letter);
			for (int k = 0; k < 3; ++k)
				put<float>(e + 0x60 + 20 * k, 1.0f); // (the local matrix: scale 1, +0x60's diagonal)
			put<float>(e + 0x60 + 60, 1.0f);
			put<float>(e + 0x188, 0.5f);
			put<float>(e + 0x18C, -0.25f);
			put<float>(e + 0x190, z);
			if (res != 0)
			{
				const uintptr_t h = take(16);
				put<uint32_t>(h, uint32_t(res));
				put<uint32_t>(e + 0x104, uint32_t(h));
			}
			return e;
		};
		const uintptr_t e1 = element(vt_d3m, textured_res, 0x48, 0x1000, 0x80406080, -10.0f); // H, z write
		const uintptr_t e2 = element(vt_d3m, textured_res, 0x64 | 0x30 << 8, 0, 0x80808080, -8.0f); // d: alpha 0x60
		const uintptr_t e3 = element(vt_d3m, textured_res, 0x44, 1u << 16, 0x80808080, -7.0f); // lit: the game's
		const uintptr_t e4 = element(vt_d3m, plain_res, 0x44, 0, 0xFF808080, -6.0f);         // untextured kind 6: the game's
		const uintptr_t e5 = element(vt_flare, 0, 0x48, 0, 0x80808080, -5.0f);               // a lens flare's own draw
		const uintptr_t e6 = element(vt_d3m, textured_res, 0x44, 0, 0xFF808080, -4.0f);      // D
		// The sorted list: 055520 starts at [list+4] + [list+8] x 32, each node +4 next, +8 callback, +0xC argument
		const uintptr_t nodes = take(32 * 16), list = take(16);
		const uintptr_t order[][2] = { { s.element_cb, e1 }, { s.element_cb, e2 }, { address(reinterpret_cast<void *>(&actor_callback)), 77 },
			{ s.element_cb, e3 }, { s.element_cb, e4 }, { s.element_cb, e5 }, { s.element_cb, e6 } };
		const size_t count = sizeof(order) / sizeof(order[0]);
		auto make_list = [&]() {
			for (size_t i = 0; i < count; ++i)
			{
				const uintptr_t n = nodes + 32 * i;
				put<uint32_t>(n + 4, i + 1 < count ? uint32_t(n + 32) : 0);
				put<uint32_t>(n + 8, uint32_t(order[i][0]));
				put<uint32_t>(n + 0xC, uint32_t(order[i][1]));
			}
			put<uint32_t>(list + 4, uint32_t(nodes));
			put<uint32_t>(list + 8, 0);
		};
		using flush_fn = void(__fastcall *)(uintptr_t ecx, uintptr_t edx);
		const auto flush = [&]() {
			make_list();
			g_seen = seen();
			g_markers = 0;
			reinterpret_cast<flush_fn>(s.code)(list, 0);
		};
		const auto order_seen = []() {
			std::string got;
			for (const std::string &o : g_seen.order)
				got += (got.empty() ? "" : ", ") + o;
			return got;
		};
		fd::set_setup(address(reinterpret_cast<void *>(&game_setup)));
		const auto low = [](uintptr_t e) { return std::to_string(e & 0xFFFF); };

		// Native drawing off: nothing taken, no marker
		fd::set_wanted(true);
		fd::set_native(false);
		fd::set_probe(false);
		const fd::counts c0 = fd::get_counts();
		flush();
		check(g_markers == 0 && g_seen.setups == 5 && fd::get_counts().flushes == c0.flushes + 1, name + ": native off: every element the game's, no marker");
		// The probe: an empty run at the end
		fd::set_probe(true);
		flush();
		const fx::frame *pf = fx::published(g_last_seq);
		check(g_markers == 1 && pf != nullptr && pf->runs.load() == 1 && pf->run_list[0].count == 0, name + ": the probe: one marker, an empty run");
		fd::set_probe(false);

		// Native drawing on, the texture not on the GPU yet: every element the game's, the texture queued
		fd::set_native(true);
		flush();
		check(g_markers == 0 && g_seen.setups == 5 && fd::get_counts().texture >= 3, name + ": the texture not up yet: the game's, no marker (" + order_seen() + ")");

		// The texture decoded and copied in on a list (as at a marker)
		fx::textures::settle(10000);
		check(fx::textures::prepare(device, why), name + ": the textures' heap");
		{
			ID3D12CommandAllocator *alloc = nullptr;
			ID3D12GraphicsCommandList *cl = nullptr;
			device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
			device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl));
			const uint32_t went = fx::textures::upload(cl, 1u << 24);
			cl->Close();
			ID3D12CommandList *lists[] = { cl };
			queue->ExecuteCommandLists(1, lists);
			ID3D12Fence *fence = nullptr;
			device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
			queue->Signal(fence, 1);
			HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			fence->SetEventOnCompletion(1, ev);
			WaitForSingleObject(ev, 10000);
			CloseHandle(ev);
			fence->Release();
			cl->Release();
			alloc->Release();
			check(went >= 1, name + ": the texture copied in");
		}

		// Now: e1, e2 a run before the actor; e3 lit and e4 untextured (its first vertex overwritten): the game's, with
		// nothing pending before them no marker; e5 not an element of 046070's: nothing pending; e6 the last run at the
		// end, pulled nearer by a depth bias
		put<float>(e6 + 0x128, 10.0f);
		const fd::counts c1 = fd::get_counts();
		flush();
		const std::string got = order_seen();
		const std::string m = "marker 5 " + std::to_string(g_last_seq);
		check(got == m + ", actor 77, game " + low(e3) + ", game " + low(e4) + ", flare " + low(e5) + ", " + m, name + ": the order: " + got);
		const fx::frame *f = fx::published(g_last_seq);
		check(f != nullptr && f->runs.load() == 2 && f->run_list[0].first == 0 && f->run_list[0].count == 2 && f->run_list[1].first == 2 && f->run_list[1].count == 1,
			name + ": two runs, {e1, e2} and {e6}");
		check(g_seen.pre_draws == 5 && g_seen.render_states == 5, name + ": 046070 ran for each of its elements (the pre-draw, BLENDOP) and stopped at the setup");
		const fd::counts c2 = fd::get_counts();
		check(c2.lit == c1.lit + 1 && c2.overwritten == c1.overwritten + 1, name + ": left to the game: lit (1), untextured with its first vertex overwritten (1)");
		if (f != nullptr && f->element_count == 3)
		{
			const fx::element &a = f->elements[0], &b = f->elements[1], &p = f->elements[2];
			const fx::draw &ad = f->draws[a.first_draw];
			check(a.draws == 1 && ad.count == 6 && ad.texture != fx::kNone && ad.stages == fx::stage_texture, name + ": e1: one textured draw of its 6 vertices");
			bool same = true;
			for (int i = 0; i < 6; ++i)
			{
				const fx::vertex &v = f->vertices[ad.first + i];
				same = same && std::memcmp(&v, &textured_m.vertices[size_t(i)], sizeof(fx::vertex)) == 0;
			}
			check(same, name + ": e1's vertices as the chunk has them");
			check(a.blend == fx::blend_add && a.z_write == 1 && a.tfactor == 0x80406080 && a.fog == 0 && a.fog_colour == 0, name + ": e1: H, z write, its colour, no fog (no environment)");
			check(std::fabs(a.world[12] - 0.5f) < 1e-6f && std::fabs(a.world[13] + 0.25f) < 1e-6f && std::fabs(a.world[14] + 10.0f) < 1e-6f &&
					std::fabs(a.world[0] - 1.0f) < 1e-5f && std::fabs(a.world[5] - 1.0f) < 1e-5f && std::fabs(a.world[10] - 1.0f) < 1e-5f,
				name + ": e1's draw matrix (045D40): the local matrix, at its view position");
			check(b.blend == fx::blend_alpha && b.tfactor == 0x60808080 && b.z_write == 0 && b.projection == fx::proj_camera,
				name + ": e2: d, the colour's alpha the letter's high byte x 2; no depth pull without a bias");
			check(p.tfactor == 0xFF808080 && p.blend == fx::blend_alpha, name + ": e6: D (alpha from 0x7F up full)");
			const float k = 1.0f - 4.0f / 32.0f, shift = k * k * k * 0.03f * 10.0f;
			check(p.projection == fx::proj_near_far && std::fabs(p.near_z - (0.1f - shift + 0.0003f)) < 1e-5f && std::fabs(p.far_z - (65535.0f - shift + 0.0003f)) < 1e-2f,
				name + ": e6's depth pull: near and far less (1 - 4/32)^3 x 0.03 x 10, plus the renderer's shift");
		}
		else
			check(false, name + ": three elements recorded (" + std::to_string(f != nullptr ? f->element_count : 0) + ")");

		// A D3A (Fire's 'fai0': 16 frames of one quad) and a weapon trail, then e1: one run of three
		{
			std::vector<uint8_t> flip;
			check(chunk("ROM\\11\\17.DAT", 0x21, "fai0", flip), name + ": the D3A chunk read");
			geo::d3a fa;
			check(geo::parse_d3a(flip.data(), flip.size(), fa, why) && fa.frames == 16 && !fa.frame_list.empty(), name + ": 'fai0': 16 frames");
			const uintptr_t d3a_res = resource(flip, 0x21);
			const uintptr_t table = take(4 * 16);
			uint32_t vfirst = 0;
			for (int i = 0; i < fa.frames; ++i)
			{
				const uint16_t q = uint16_t(fa.frame_list[size_t(i)].vertices.size() / 6);
				put<uint16_t>(table + 4 * i, q);
				put<uint16_t>(table + 4 * i + 2, uint16_t(vfirst));
				vfirst += q * 6u;
			}
			put<uint32_t>(d3a_res + 0x30 + 8, uint32_t(tex_object));
			put<uint32_t>(d3a_res + 0x30 + 0xC, 0x1234);
			put<uint32_t>(d3a_res + 0x30 + 0x14, uint32_t(table));
			put<uint16_t>(tex_object + 0x24, get<uint16_t>(tex_res + 0x30 + 21));
			put<uint16_t>(tex_object + 0x26, get<uint16_t>(tex_res + 0x30 + 25));
			const uintptr_t vt_d3a = take(0x100);
			put<uint32_t>(vt_d3a + 0x2C, uint32_t(s.element_draw));
			put<uint32_t>(vt_d3a + 0x48, uint32_t(s.d3a_draw));
			put<uint32_t>(vt_d3a + 0x4C, uint32_t(address(reinterpret_cast<void *>(&pre_draw))));
			const uintptr_t ea = element(vt_d3a, d3a_res, 0x48, 1u << 16 | 0x1000, 0x80808080, -9.0f); // (lit and z write: the D3A sets neither)
			put<uint16_t>(ea + 0x170, 17); // (past the frames: back to 0, written back)
			// The trail: 3 samples, the newest at index 1 (head 2), one strip
			const uintptr_t trail = take(0x1550);
			put<int32_t>(trail + 0x14A8, 2);
			put<int32_t>(trail + 0x14AC, 3);
			for (int i = 0; i < 3; ++i)
			{
				const int at_ = (2 - 1 - i + 80) % 80;
				const float tip[4] = { float(i), 1.0f, 0.0f, 1.0f }, bse[4] = { float(i), 0.0f, 0.0f, 1.0f };
				const float ca[4] = { 255.0f, 128.0f, 64.0f, 200.0f - 50.0f * i }, cb[4] = { 10.0f, 20.0f, 30.0f, 0.0f };
				std::memcpy(reinterpret_cast<void *>(trail + 0x40 + 16 * at_), tip, 16);
				std::memcpy(reinterpret_cast<void *>(trail + 0x540 + 16 * at_), bse, 16);
				std::memcpy(reinterpret_cast<void *>(trail + 0xA40 + 16 * at_), ca, 16);
				std::memcpy(reinterpret_cast<void *>(trail + 0xF40 + 16 * at_), cb, 16);
			}
			const uintptr_t list2 = take(16), nodes2 = take(32 * 4);
			const uintptr_t order2[][2] = { { s.element_cb, ea }, { s.trail_cb, trail }, { s.element_cb, e1 } };
			for (size_t i = 0; i < 3; ++i)
			{
				const uintptr_t n = nodes2 + 32 * i;
				put<uint32_t>(n + 4, i + 1 < 3 ? uint32_t(n + 32) : 0);
				put<uint32_t>(n + 8, uint32_t(order2[i][0]));
				put<uint32_t>(n + 0xC, uint32_t(order2[i][1]));
			}
			put<uint32_t>(list2 + 4, uint32_t(nodes2));
			put<uint32_t>(list2 + 8, 0);
			g_seen = seen();
			g_markers = 0;
			reinterpret_cast<flush_fn>(s.code)(list2, 0);
			const fx::frame *f2 = fx::published(g_last_seq);
			check(g_markers == 1 && f2 != nullptr && f2->runs.load() == 1 && f2->run_list[0].count == 3, name + ": the D3A, the trail and e1: one run, one marker (" + order_seen() + ")");
			if (f2 != nullptr && f2->element_count == 3)
			{
				const fx::element &a = f2->elements[0], &tr = f2->elements[1];
				const fx::draw &ad = f2->draws[a.first_draw], &td = f2->draws[tr.first_draw];
				check(a.cls == fx::class_d3a && ad.count == 6 && ad.texture != fx::kNone && ad.stages == fx::stage_texture && a.z_write == 0 && a.fog == 0,
					name + ": the D3A: one quad, textured, no z write, no fog");
				check(get<uint16_t>(ea + 0x170) == 0, name + ": its frame past the count wrapped to 0 in the element, as 03E910 does");
				const float su = 1.0f / float(get<uint16_t>(tex_object + 0x24)), sv = 1.0f / float(get<uint16_t>(tex_object + 0x26));
				bool same = true;
				for (int k = 0; k < 6; ++k)
				{
					const fx::vertex &v = f2->vertices[ad.first + k], &w0 = fa.frame_list[0].vertices[size_t(k)];
					const float wu = fa.units >= 1 && fa.uvs_unit == 0 ? w0.uv[0] * su : w0.uv[0], wv = fa.units >= 1 && fa.uvs_unit == 0 ? w0.uv[1] * sv : w0.uv[1];
					same = same && std::memcmp(v.pos, w0.pos, 12) == 0 && v.colour == w0.colour && std::fabs(v.uv[0] - wu) < 1e-6f && std::fabs(v.uv[1] - wv) < 1e-6f;
				}
				check(same, name + ": the D3A's frame 0 vertices, the UVs as the load scales them");
				bool trail_ok = tr.cls == fx::class_trail && td.count == 12 && td.stages == fx::stage_diffuse && tr.blend == fx::blend_add && tr.z_write == 0;
				// (newest first: sample 0 at x 0, the tip with colour A (alpha 200), then the base with B)
				const fx::vertex &t0 = f2->vertices[td.first], &t1 = f2->vertices[td.first + 1];
				trail_ok = trail_ok && t0.pos[0] == 0.0f && t0.pos[1] == 1.0f && t0.colour == 0xC8FF8040u && t1.pos[1] == 0.0f && t1.colour == 0x000A141Eu;
				check(trail_ok, name + ": the trail: one strip of 3 pairs as 4 triangles, the newest sample first, vertex colour only, additive");
				check(std::memcmp(tr.world, reinterpret_cast<const void *>(address(gfx) + 0xB50), 64) == 0, name + ": the trail through the camera's VIEW (gfx +0xB50)");
			}
			else
				check(false, name + ": the D3A, the trail and e1 recorded (" + std::to_string(f2 != nullptr ? f2->element_count : 0) + ")");
		}

		// Put back
		fd::set_native(false);
		fd::set_wanted(false);
		put<uint32_t>(s.gfx, kept[0]);
		put<uint32_t>(s.renderer, kept[1]);
		put<uint32_t>(s.camera, kept[2]);
		put<uint32_t>(s.env, kept[3]);
		put<uint32_t>(s.math, kept[4]);
		put<uint32_t>(s.colour, kept[5]);
		check(fd::stop(why) && !fd::in(), name + ": stop()" + (why.empty() ? "" : ": " + why));
		const size_t diff = differing(path, base, size);
		check(diff == 0, name + ": the dump again, byte for byte (" + std::to_string(diff) + " bytes differ)");
		fx::textures::stop();
		dump::unmap(base);
	}
	std::printf("\n%d passed, %d failed\n", passes, failures);
	return failures == 0 ? 0 : 1;
}
