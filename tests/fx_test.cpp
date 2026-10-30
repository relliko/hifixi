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
#include "../src/game/gamewater.hpp"
#include "../src/game/slotmark.hpp"
#include "../src/zone/zone_gpu.hpp"
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
	fx::frame_slot(0); // (the frames' ring, some 25 MB, made here too, clear of the dumps' place)

	// (and kept free while each dump is tested: what's around it stays reserved, so the threads WARP and the decoders
	// start meanwhile don't put their stacks where the next dump goes)
	void *around[2] = {};
	const auto release_around = [&]() {
		for (void *&p : around)
			if (p != nullptr)
			{
				VirtualFree(p, 0, MEM_RELEASE);
				p = nullptr;
			}
	};
	for (const char *path : dump::kDumps)
	{
		if (kept_free != nullptr)
			VirtualFree(kept_free, 0, MEM_RELEASE);
		kept_free = nullptr;
		release_around();
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		const std::string name = std::string(path).substr(std::string(path).rfind('\\') + 1);
		if (size == 0)
		{
			check(false, "mapped " + name);
			continue;
		}
		const uintptr_t end = (base + size + 0xFFFF) & ~uintptr_t(0xFFFF);
		if (base > kDumpsFrom)
			around[0] = VirtualAlloc(reinterpret_cast<void *>(kDumpsFrom), base - kDumpsFrom, MEM_RESERVE, PAGE_NOACCESS);
		if (end < kDumpsTo)
			around[1] = VirtualAlloc(reinterpret_cast<void *>(end), kDumpsTo - end, MEM_RESERVE, PAGE_NOACCESS);
		fd::set_sink(&sink);
		const bool started = fd::start(why);
		check(started && fd::in(), name + ": start()" + (why.empty() ? "" : ": " + why));
		const fd::sites s = fd::where();
		check(s.setup_call == base + 0x04612E && s.setup == base + 0x0467E0, name + ": the setup's call at 04612E, to 0467E0");
		check(s.flush_call == base + 0x05562B && s.walk == base + 0x055520, name + ": the flush's call at 05562B, to the walk 055520");
		check(s.entry_site == base + 0x055539 && s.element_cb == base + 0x0555A0, name + ": the walk's callback call at 055539; the element callback 0555A0");
		check(s.ring_draw == base + 0x0565A0, name + ": the rings' draw 0565A0");
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
		uint8_t *scratch = block(64 * 32), *mem = block(0x40000);
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
			const uintptr_t e = take(0x240);
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

		// A ring element (0565A0): 4 segments, 2 rings of radius 1 and 2; TEXTUREFACTOR its own colour
		{
			const uintptr_t vt_ring = take(0x100);
			put<uint32_t>(vt_ring + 0x2C, uint32_t(s.element_draw));
			put<uint32_t>(vt_ring + 0x48, uint32_t(s.ring_draw));
			put<uint32_t>(vt_ring + 0x4C, uint32_t(address(reinterpret_cast<void *>(&pre_draw))));
			const uintptr_t er = element(vt_ring, 0, 0x48, 0, 0x40112233, -3.0f);
			put<float>(er + 0x138, 0.5f); // (faded: 046070's colour, not the ring's)
			put<int8_t>(er + 0x1B4, 4);
			put<int8_t>(er + 0x1B5, 2);
			put<float>(er + 0x194, 1.0f);
			put<float>(er + 0x198, 2.0f);
			put<uint32_t>(er + 0x1A4, 0xFF0000AAu); // (bytes AA 00 00 FF: red AA, alpha FF)
			put<uint32_t>(er + 0x1A8, 0x8000BB00u);
			const uintptr_t list3 = take(16), node3 = take(32);
			put<uint32_t>(node3 + 4, 0);
			put<uint32_t>(node3 + 8, uint32_t(s.element_cb));
			put<uint32_t>(node3 + 0xC, uint32_t(er));
			put<uint32_t>(list3 + 4, uint32_t(node3));
			put<uint32_t>(list3 + 8, 0);
			g_seen = seen();
			g_markers = 0;
			reinterpret_cast<flush_fn>(s.code)(list3, 0);
			const fx::frame *f3 = fx::published(g_last_seq);
			bool ok = g_markers == 1 && f3 != nullptr && f3->element_count == 1;
			if (ok)
			{
				const fx::element &r = f3->elements[0];
				const fx::draw &d = f3->draws[r.first_draw];
				const fx::vertex *v = f3->vertices + d.first;
				ok = r.cls == fx::class_ring && r.tfactor == 0x40112233u && d.count == 24 && d.stages == 0 && v[0].pos[1] == 1.0f && v[1].pos[1] == 2.0f &&
					v[0].colour == 0xFFAA0000u && v[1].colour == 0x8000BB00u && v[2].pos[0] == 0.0f && v[2].pos[1] == 1.0f && std::fabs(v[6 * 4 - 1].pos[1] - 2.0f) < 1e-6f;
			}
			check(ok, name + ": a ring: 4 segments x 2 triangles, inner then outer from angle 0, its colours made D3DCOLORs, its own TEXTUREFACTOR");
		}

		// An untextured kind-6 D3M whose resource has a root (file id 0: ROM/0/0.DAT, where 'coll' is): its first
		// vertex's y, written over by the vertex buffer object as the load does, comes back from the DAT
		{
			const uintptr_t r7 = resource(plain, 0x1F);
			const size_t names = geo::d3m_names_at(reinterpret_cast<const uint8_t *>(r7 + 0x30));
			put<uint16_t>(r7 + 0x32, uint16_t(get<uint16_t>(r7 + 0x32) | 0x8080));
			put<uint32_t>(r7 + 0x30 + names + 4, 0x1234); // (over the first vertex's y, as 03FEE0 leaves it)
			std::memcpy(reinterpret_cast<void *>(r7 + 0x20), "coll", 4);
			const uintptr_t root = take(0x40), root_handle = take(16);
			put<uint32_t>(root + 0x34, 0);
			put<uint32_t>(root_handle, uint32_t(root));
			put<uint32_t>(r7 + 0x28, uint32_t(root_handle));
			const uintptr_t e7 = element(vt_d3m, r7, 0x48, 0, 0x80808080, -3.5f);
			const uintptr_t list4 = take(16), node4 = take(32);
			put<uint32_t>(node4 + 4, 0);
			put<uint32_t>(node4 + 8, uint32_t(s.element_cb));
			put<uint32_t>(node4 + 0xC, uint32_t(e7));
			put<uint32_t>(list4 + 4, uint32_t(node4));
			put<uint32_t>(list4 + 8, 0);
			g_seen = seen();
			g_markers = 0;
			reinterpret_cast<flush_fn>(s.code)(list4, 0);
			const fx::frame *f4 = fx::published(g_last_seq);
			bool ok = g_markers == 1 && f4 != nullptr && f4->element_count == 1;
			if (ok)
			{
				const fx::draw &d = f4->draws[f4->elements[0].first_draw];
				ok = d.count == 36 && std::memcmp(f4->vertices + d.first, plain_m.vertices.data(), 36 * sizeof(fx::vertex)) == 0;
			}
			check(ok, name + ": an untextured kind-6 D3M drawn, its first vertex's y from its DAT (every vertex as the chunk has it)");
		}

		// MMB effect meshes (+0x179: 040C20 -> 183760): the zone's own model, drawn from the native zone renderer's data
		// (a made-up map's on WARP: one model of two pieces, the first textured); the mesh as the zone builds it (16FCD0:
		// its name +8, the 4-character one +0x18, FVF +0x20, stride +0x24, strips +0x2C, groups +0x60 / +0x44, the
		// offset +0xE0 added to the draw matrix's last row)
		{
			namespace zm = hifi::bake::zonemesh;
			zm::zone zd;
			zd.map = 1;
			for (int i = 0; i < 6; ++i)
			{
				zm::vertex v = {};
				v.pos[0] = float(i);
				v.colour = 0x80808080u;
				zd.vertices.push_back(v);
				zd.indices.push_back(uint32_t(i));
			}
			zm::piece p0, p1;
			p0.first_index = 0;
			p0.triangles = 1;
			p0.texture = 0;
			p1.first_index = 3;
			p1.triangles = 1;
			zd.pieces = { p0, p1 };
			zm::model mo;
			std::memcpy(mo.name, "fxtest_sheet    ", 16);
			mo.kind = 1;
			mo.first_piece = 0;
			mo.pieces = 2;
			mo.vertices = 6;
			mo.raw_pieces = 2;
			zd.models.push_back(mo);
			zm::texture tx;
			std::memcpy(tx.name, "fxtest  sheet   ", 16);
			tx.w = tx.h = 2;
			tx.format = zm::kBGRA8;
			tx.data.assign(16, 0x80);
			zd.textures.push_back(std::move(tx));
			hifi::zone::gpu_zone gz;
			const bool up = gz.upload(device, queue, std::move(zd), why) && gz.wait(10000);
			check(up, name + ": a made-up map's data on the GPU" + (why.empty() ? "" : ": " + why));
			fd::set_zones(&gz, nullptr);
			// (the live mesh: one group of two parts, the first with a texture object whose Direct3D texture is made)
			const auto mesh_of = [&](const char *full, const char *four) {
				const uintptr_t mesh = take(0x100), groups = take(0x24), parts = take(2 * 0xC0), object = take(0x50);
				std::memcpy(reinterpret_cast<void *>(mesh + 8), full, 16);
				std::memcpy(reinterpret_cast<void *>(mesh + 0x18), four, 4);
				put<uint32_t>(mesh + 0x20, 0x152);
				put<uint32_t>(mesh + 0x24, 0x24);
				put<uint32_t>(mesh + 0x2C, 5);
				put<int32_t>(mesh + 0x44, 1);
				put<uint32_t>(mesh + 0x60, uint32_t(groups));
				put<float>(mesh + 0xE0, 1.0f);
				put<float>(mesh + 0xE4, 2.0f);
				put<float>(mesh + 0xE8, 3.0f);
				put<int32_t>(groups, 2);
				put<uint32_t>(groups + 4, uint32_t(parts));
				put<uint32_t>(object + 0x40, 0x5678);
				put<uint32_t>(parts + 4, uint32_t(object));
				put<uint32_t>(parts + 0x28, 0x1234);
				put<uint32_t>(parts + 0xC0 + 0x28, 0x1234);
				return mesh;
			};
			const uintptr_t sheet = mesh_of("fxtest_sheet    ", "shet"), other = mesh_of("fxtest_other    ", "othr");
			const auto mmb = [&](uintptr_t mesh, uint16_t letter, uint32_t flags, float z) {
				const uintptr_t e = element(vt_d3m, 0, letter, flags, 0x80808080, z);
				put<uint32_t>(e + 0x104, uint32_t(mesh));
				put<uint8_t>(e + 0x179, 1);
				return e;
			};
			const uintptr_t m1 = mmb(sheet, 0x48, 0, -12.0f), m2 = mmb(sheet, 0x64 | 0x40 << 8, 0, -11.0f), m3 = mmb(other, 0x48, 0, -10.5f);
			const uintptr_t list5 = take(16), nodes5 = take(32 * 3);
			const uintptr_t order5[] = { m1, m2, m3 };
			for (size_t i = 0; i < 3; ++i)
			{
				const uintptr_t n = nodes5 + 32 * i;
				put<uint32_t>(n + 4, i + 1 < 3 ? uint32_t(n + 32) : 0);
				put<uint32_t>(n + 8, uint32_t(s.element_cb));
				put<uint32_t>(n + 0xC, uint32_t(order5[i]));
			}
			put<uint32_t>(list5 + 4, uint32_t(nodes5));
			put<uint32_t>(list5 + 8, 0);
			const fd::counts before = fd::get_counts();
			g_seen = seen();
			g_markers = 0;
			reinterpret_cast<flush_fn>(s.code)(list5, 0);
			const fx::frame *f5 = fx::published(g_last_seq);
			const fd::counts after = fd::get_counts();
			check(s.mmb_draw == base + 0x040C20, name + ": the MMB draw 040C20 (040BA0's jump)");
			check(g_markers == 1 && f5 != nullptr && f5->element_count == 2 && after.mesh == before.mesh + 1 && g_seen.setups == 1,
				name + ": two MMB meshes taken, one run; a mesh not in the zone's data the game's (" + order_seen() + ")");
			if (f5 != nullptr && f5->element_count == 2)
			{
				const fx::element &a = f5->elements[0], &b = f5->elements[1];
				const fx::draw *ad = f5->draws + a.first_draw, *bd = f5->draws + b.first_draw;
				check(a.cls == fx::class_mmb && a.zone == &gz && a.draws == 2 && ad[0].first == 0 && ad[0].count == 3 && ad[0].texture == 0 &&
						ad[0].stages == (fx::stage_zone_mesh | fx::stage_mmb_textured) && ad[1].first == 3 && ad[1].count == 3 && ad[1].texture == fx::kNone &&
						ad[1].stages == fx::stage_zone_mesh,
					name + ": the first: its two pieces from the zone's indices, the textured one by 183760's plain table, the other untextured");
				check(b.draws == 2 && bd[0].stages == (fx::stage_zone_mesh | fx::stage_mmb_textured_d) && b.blend == fx::blend_alpha && b.tfactor == 0x80808080,
					name + ": letter d: the textured mode, SRCALPHA / INVSRCALPHA, the colour's alpha the letter's high byte x 2");
				check(std::fabs(a.world[12] - 1.5f) < 1e-5f && std::fabs(a.world[13] - 1.75f) < 1e-5f && std::fabs(a.world[14] + 9.0f) < 1e-5f && a.world[15] == 1.0f,
					name + ": the draw matrix's last row plus the mesh's +0xE0 (1, 2, 3, 0)");
			}
			// World FX's gamewater list: a mesh on it is taken with nothing drawn (its routine routed on this dump only
			// where the dump has the game's own bytes there)
			std::string gw_why;
			if (hifi::game::gamewater::route(true, gw_why))
			{
				const uintptr_t gw = hifi::game::gamewater::where().memory;
				put<uint32_t>(gw + 0x10, hifi::game::name_value("shet"));
				put<uint32_t>(gw, 1);
				const fd::counts c5 = fd::get_counts();
				g_seen = seen();
				g_markers = 0;
				reinterpret_cast<flush_fn>(s.code)(list5, 0);
				const fd::counts c6 = fd::get_counts();
				check(g_markers == 0 && c6.taken[fx::class_mmb] == c5.taken[fx::class_mmb] && c6.water == c5.water + 2 && get<uint32_t>(gw + 4) >= 2,
					name + ": gamewater's list: both 'shet' meshes taken with nothing drawn, counted as its routine counts them");
				put<uint32_t>(gw, 0);
				check(hifi::game::gamewater::stop(gw_why), name + ": gamewater put back" + (gw_why.empty() ? "" : ": " + gw_why));
			}
			else
				std::printf("      (%s: gamewater not routed here: %s)\n", name.c_str(), gw_why.c_str());
			fd::set_zones(nullptr, nullptr);
			gz.release();
		}

		// A morph (03FBB0 -> 03F440: ROM3/9/111 'rng ', 2 targets of 80 positions, 120 triangles): its corners blended as
		// 03F8A0 blends them (the weights over their sum), the colours doubled; then with no weights at all the largest
		// of the last frame's (target 1) set to 1.0 in +0x194; +0x194 copied to +0x1A8 and +0x1BC cleared each time
		{
			std::vector<uint8_t> mp;
			geo::morph gm;
			check(chunk("ROM3\\9\\111.DAT", 0x25, "rng ", mp) && geo::parse_morph(mp.data(), mp.size(), gm, why) && gm.targets == 2 && gm.triangles == 120,
				name + ": the morph chunk read (2 targets, 120 triangles)");
			check(s.morph_draw == base + 0x03FBB0, name + ": the morphs' draw 03FBB0");
			const uintptr_t morph_res = resource(mp, 0x25);
			put<uint32_t>(morph_res + 0x30 + 0x10, uint32_t(tex_object));
			put<uint32_t>(tex_object + 0x40, 0x5678); // (its Direct3D texture made)
			const uintptr_t vt_morph = take(0x100);
			put<uint32_t>(vt_morph + 0x2C, uint32_t(s.element_draw));
			put<uint32_t>(vt_morph + 0x48, uint32_t(s.morph_draw));
			put<uint32_t>(vt_morph + 0x4C, uint32_t(address(reinterpret_cast<void *>(&pre_draw))));
			const uintptr_t em = element(vt_morph, morph_res, 0x48, 0, 0x80808080, -6.0f);
			const float wts[5] = { 1.0f, 3.0f, 0, 0, 0 };
			std::memcpy(reinterpret_cast<void *>(em + 0x194), wts, 20);
			const uintptr_t list7 = take(16), node7 = take(32);
			put<uint32_t>(node7 + 4, 0);
			put<uint32_t>(node7 + 8, uint32_t(s.element_cb));
			put<uint32_t>(node7 + 0xC, uint32_t(em));
			put<uint32_t>(list7 + 4, uint32_t(node7));
			put<uint32_t>(list7 + 8, 0);
			const auto morph_matches = [&](const float *weights, const fx::frame *fm) {
				if (fm == nullptr || fm->element_count != 1 || fm->elements[0].cls != fx::class_morph)
					return false;
				const fx::draw &d = fm->draws[fm->elements[0].first_draw];
				std::vector<fx::vertex> want;
				gm.blend(weights, want);
				bool same = d.count == want.size() && d.texture != fx::kNone && d.stages == fx::stage_texture;
				for (size_t i = 0; same && i < want.size(); ++i)
				{
					const fx::vertex &v = fm->vertices[d.first + i];
					for (int c = 0; c < 3; ++c)
						same = same && std::fabs(v.pos[c] - want[i].pos[c]) < 1e-4f && std::fabs(v.normal[c] - want[i].normal[c]) < 1e-4f;
					same = same && v.colour == want[i].colour && v.uv[0] == want[i].uv[0] && v.uv[1] == want[i].uv[1];
				}
				return same;
			};
			g_seen = seen();
			g_markers = 0;
			reinterpret_cast<flush_fn>(s.code)(list7, 0);
			check(g_markers == 1 && morph_matches(wts, fx::published(g_last_seq)) && get<float>(em + 0x1A8) == 1.0f && get<float>(em + 0x1AC) == 3.0f,
				name + ": a morph: its 360 corners blended 1:3 as the chunk's targets, the colours doubled; the weights kept as the last (+0x1A8)");
			const float none[5] = {}, last[5] = { 0.2f, 0.7f, 0, 0, 0 }, target1[5] = { 0.0f, 1.0f, 0, 0, 0 };
			std::memcpy(reinterpret_cast<void *>(em + 0x194), none, 20);
			std::memcpy(reinterpret_cast<void *>(em + 0x1A8), last, 20);
			g_seen = seen();
			g_markers = 0;
			reinterpret_cast<flush_fn>(s.code)(list7, 0);
			check(g_markers == 1 && morph_matches(target1, fx::published(g_last_seq)) && get<float>(em + 0x198) == 1.0f && get<float>(em + 0x1AC) == 1.0f &&
					get<uint32_t>(em + 0x1BC) == 0,
				name + ": no weights: the last frame's largest (target 1) set to 1.0 and drawn alone, as 03F8A0 does");
			put<uint32_t>(em + 0x1BC, 0x1234); // (a blend handed in ready: the game's)
			g_seen = seen();
			reinterpret_cast<flush_fn>(s.code)(list7, 0);
			check(g_seen.setups == 1, name + ": a morph with a blend handed in (+0x1BC) left to the game");
			put<uint32_t>(tex_object + 0x40, 0);
		}

		// A CMoVuAnmElem (03F130): Fire's 'fai0' frame 2 at each of 3 points, the game's own walk (063C80) and callback
		// (03EFC0) run: each point = record + t x its velocity + t^2 / 2 x (the walk's push + scale x its push) + the point
		// list's point (index 1), through VIEW (identity here), plus the draw matrix's translation; the frame's first quad's
		// corners (0, 1, 2, 5) at it. The game's strip (from vertex 1, 3 x 4 - 2 triangles) draws the first two points'
		// quads: 4 triangles. The walk's clock moved on by gfx +0xEB0; WORLD math +0x810; no z write, no fog.
		{
			std::vector<uint8_t> flip;
			geo::d3a fa;
			check(chunk("ROM\\11\\17.DAT", 0x21, "fai0", flip) && geo::parse_d3a(flip.data(), flip.size(), fa, why) && fa.frames == 16, name + ": the VuAnm's D3A read");
			check(s.vuanm_draw == base + 0x03F130 && s.vu_walk == base + 0x063C80 && s.vu_cursor == base + 0x47B9D4, name + ": VuAnm: 03F130, its walk 063C80, VA +47B9D4");
			const uintptr_t vres = resource(flip, 0x21);
			const uintptr_t vt_vu = take(0x100);
			put<uint32_t>(vt_vu + 0x2C, uint32_t(s.element_draw));
			put<uint32_t>(vt_vu + 0x48, uint32_t(s.vuanm_draw));
			put<uint32_t>(vt_vu + 0x4C, uint32_t(address(reinterpret_cast<void *>(&pre_draw))));
			const uintptr_t ev = element(vt_vu, vres, 0x48, 0, 0x80808080, -5.0f);
			put<uint32_t>(tex_object + 0x40, 0x5678);
			put<uint32_t>(ev + 0x140, uint32_t(tex_object));
			put<uint16_t>(ev + 0x170, 2);
			// (the corner table 03F130's first draw makes: per frame the first quad's vertices 0, 1, 2, 5)
			const uintptr_t corners = take(16 * 0x30);
			for (int fr = 0; fr < 16; ++fr)
				for (int j = 0; j < 4; ++j)
					std::memcpy(reinterpret_cast<void *>(corners + 0x30 * fr + 12 * j), fa.frame_list[size_t(fr)].vertices[size_t(j == 3 ? 5 : j)].pos, 12);
			put<uint32_t>(ev + 0x22D, uint32_t(corners));
			put<uint8_t>(ev + 0x1DC, 0); // (first: the game's)
			// The walk (+0x194): the point list handle, clock, index, scale, push, records, count
			const uintptr_t plist = take(0x30 + 16 + 32), plist_h = take(16), recs = take(3 * 0x24);
			put<uint32_t>(plist + 0x30, 2);
			const float pts[8] = { 10, 20, 30, 1, -1, -2, -3, 1 };
			std::memcpy(reinterpret_cast<void *>(plist + 0x40), pts, 32);
			put<uint32_t>(plist_h, uint32_t(plist));
			const float t = 2.0f, sc = 0.5f, push[3] = { 0.2f, -0.1f, 0.05f };
			float rec[3][9];
			for (int i = 0; i < 3; ++i)
				for (int c = 0; c < 3; ++c)
				{
					rec[i][c] = float((c + 1) * i) * (c == 2 ? -1.0f : 1.0f);
					rec[i][3 + c] = 0.1f * float(c + 1) * float(i + 1);
					rec[i][6 + c] = c == 0 ? 0.5f : c == 1 ? 0.0f : -0.5f;
				}
			std::memcpy(reinterpret_cast<void *>(recs), rec, sizeof(rec));
			put<uint32_t>(ev + 0x194, uint32_t(plist_h));
			put<float>(ev + 0x198, t);
			put<int32_t>(ev + 0x1A4, 7);
			put<int32_t>(ev + 0x1A8, 1);
			put<float>(ev + 0x1AC, sc);
			std::memcpy(reinterpret_cast<void *>(ev + 0x1BC), push, 12);
			put<uint32_t>(ev + 0x1D4, uint32_t(recs));
			put<int16_t>(ev + 0x1D8, 3);
			put<int32_t>(ev + 0x229, 18);
			put<float>(address(gfx) + 0xEB0, 0.25f);
			const uintptr_t list8 = take(16), node8 = take(32);
			put<uint32_t>(node8 + 4, 0);
			put<uint32_t>(node8 + 8, uint32_t(s.element_cb));
			put<uint32_t>(node8 + 0xC, uint32_t(ev));
			put<uint32_t>(list8 + 4, uint32_t(node8));
			put<uint32_t>(list8 + 8, 0);
			g_seen = seen();
			g_markers = 0;
			reinterpret_cast<flush_fn>(s.code)(list8, 0);
			check(g_seen.setups == 1 && g_markers == 0 && get<float>(ev + 0x198) == t, name + ": VuAnm's first draw (+0x1DC 0) left to the game (it makes the corner table)");
			put<uint8_t>(ev + 0x1DC, 1);
			const uint32_t cursor_before = get<uint32_t>(s.vu_cursor);
			g_seen = seen();
			g_markers = 0;
			reinterpret_cast<flush_fn>(s.code)(list8, 0);
			const fx::frame *f8 = fx::published(g_last_seq);
			bool ok = g_markers == 1 && g_seen.setups == 0 && f8 != nullptr && f8->element_count == 1 && f8->elements[0].cls == fx::class_vuanm;
			std::string what;
			if (ok)
			{
				const fx::element &v = f8->elements[0];
				const fx::draw &d = f8->draws[v.first_draw];
				ok = d.count == 12 && d.texture != fx::kNone && d.stages == fx::stage_texture && v.z_write == 0 && v.fog == 0 && v.light == fx::kNone &&
					std::memcmp(v.world, reinterpret_cast<const void *>(address(math) + 0x810), 64) == 0;
				const int quad[6] = { 0, 1, 2, 1, 2, 5 }; // (the corners of each triangle: c0 c1 c2, c1 c2 c3)
				float worst = 0.0f;
				for (int i = 0; ok && i < 2; ++i)
				{
					float pt[3];
					for (int c = 0; c < 3; ++c)
						pt[c] = rec[i][c] + t * rec[i][6 + c] + t * t * 0.5f * (push[c] + sc * rec[i][3 + c]) + pts[4 + c];
					pt[0] += 0.5f;
					pt[1] += -0.25f;
					pt[2] += -5.0f;
					for (int k = 0; k < 6; ++k)
					{
						const fx::vertex &gv = f8->vertices[d.first + 6 * i + k], &src = fa.frame_list[2].vertices[size_t(quad[k])];
						for (int c = 0; c < 3; ++c)
							worst = std::max(worst, std::fabs(gv.pos[c] - (src.pos[c] + pt[c])));
						ok = ok && gv.colour == src.colour && gv.uv[0] == src.uv[0] && gv.uv[1] == src.uv[1] && gv.normal[2] == -1.0f;
					}
				}
				ok = ok && worst < 1e-4f;
				what = " (worst " + std::to_string(worst) + ")";
			}
			check(ok, name + ": VuAnm: the game's walk placed frame 2's quad at the first two of 3 points (4 triangles of the strip), its UVs and colours" + what);
			check(std::fabs(get<float>(ev + 0x198) - (t + 0.25f)) < 1e-6f && get<uint32_t>(s.vu_cursor) == cursor_before && get<uint32_t>(base + 0x47BF34) == 0,
				name + ": the walk's clock moved on by gfx +0xEB0; the cursor put back, the attach slot cleared (as 063C80 leaves it)");
			put<uint32_t>(tex_object + 0x40, 0);
		}

		// A lit D3M (flags bit 16, no fog): its lights as 0467E0 works them out, the game's own calls (045C50, 0473F0,
		// the math object, 18B9A0, 18B9E0) on a made-up environment whose record isn't loaded ([env - 0x1C] not 1: 188CA0
		// gives a white light of 255, ambient 255, down +y; 188E00 AMBIENT 0xFFFFFFFF), the renderer's one-light path
		{
			const uintptr_t env_block = take(0x400), env = env_block + 0x200;
			put<uint32_t>(s.env, uint32_t(env));
			put<uint32_t>(address(renderer) + 0x3949C, 1);
			check(s.frame_of == base + 0x045C50 && s.light1 == base + 0x18B9A0 && s.lights2 == base + 0x18B960 && s.ambient == base + 0x18B9E0 &&
					s.material == base + 0x47BC28,
				name + ": the lighting's calls in 0467E0 (045C50, 18B9A0, 18B960, 18B9E0) and the material (VA +47BC28)");
			const uintptr_t el = element(vt_d3m, textured_res, 0x48, 1u << 16 | 1u << 25, 0x80808080, -6.5f);
			const uintptr_t list6 = take(16), node6 = take(32);
			put<uint32_t>(node6 + 4, 0);
			put<uint32_t>(node6 + 8, uint32_t(s.element_cb));
			put<uint32_t>(node6 + 0xC, uint32_t(el));
			put<uint32_t>(list6 + 4, uint32_t(node6));
			put<uint32_t>(list6 + 8, 0);
			const fd::counts c7 = fd::get_counts();
			g_seen = seen();
			g_markers = 0;
			reinterpret_cast<flush_fn>(s.code)(list6, 0);
			const fx::frame *f7 = fx::published(g_last_seq);
			const fd::counts c8 = fd::get_counts();
			bool ok = g_markers == 1 && f7 != nullptr && f7->element_count == 1 && c8.lit_taken == c7.lit_taken + 1 && f7->elements[0].light == 0 && f7->light_count == 1;
			std::string what;
			if (ok)
			{
				const fx::lighting &l = f7->lights[0];
				const float t0 = std::sqrt(l.toward[0][0] * l.toward[0][0] + l.toward[0][1] * l.toward[0][1] + l.toward[0][2] * l.toward[0][2]);
				char line[200];
				std::snprintf(line, sizeof(line), " (toward %.3f %.3f %.3f, diffuse %.0f, ambient %.0f, emissive %.3f)", l.toward[0][0], l.toward[0][1], l.toward[0][2],
					l.diffuse[0][0], l.ambient[0], l.emissive[0]);
				what = line;
				ok = std::fabs(t0 - 1.0f) < 1e-4f && l.diffuse[0][0] == 255.0f && l.diffuse[0][2] == 255.0f && l.diffuse[1][0] == 0.0f && l.ambient[0] == 256.0f &&
					l.ambient[2] == 256.0f && l.emissive[0] == get<float>(s.material + 0x30) && l.emissive[2] == get<float>(s.material + 0x38);
				// (WORLD the element's local matrix, its scale 1: the normals' rows its inverse transposed, nearly identity)
				for (int i = 0; i < 3; ++i)
					for (int j = 0; j < 3; ++j)
						ok = ok && std::fabs(l.normal[i][j] - (i == j ? 1.0f : 0.0f)) < 1e-4f;
			}
			check(ok, name + ": a lit D3M taken with its lights: one white light of 255, AMBIENT + its ambient, the material's emissive, normals by WORLD" + what);
			put<uint32_t>(s.env, 0);
			put<uint32_t>(address(renderer) + 0x3949C, 0);
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
	release_around();
	std::printf("\n%d passed, %d failed\n", passes, failures);
	return failures == 0 ? 0 : 1;
}
