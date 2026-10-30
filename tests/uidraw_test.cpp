/*
 * Offline test of the screen 2D's recorder (src/game/uidraw.cpp; docs/own-frame/2d.md): each FFXiMain dump mapped at its
 * own base (game_dump.hpp), the client device a stand-in of ours (every method the recorder touches, as Direct3D 8 has
 * them, its calls logged):
 *   - an image without the loop tail: refused, nothing written;
 *   - the three calls found (012779 call 00BB20, 0129EA call 00A820, 012AF5 call 009740) and their targets checked; only
 *     their rel32s changed;
 *   - the begin stub run as the loop tail calls it (ecx the wrapper; the game's empty 00BB20 run): the window open on
 *     the wrapper's +0xC device, ebx/esi/edi/ebp and eax kept; the end stub run (its jmp to the fade a stand-in taking
 *     the fade's two arguments): the interface's segment published and marked before the fade; the frame stub (into
 *     the game's own EndScene wrapper 009740, which calls the stand-in's EndScene): the window closed first;
 *   - through the device as the game calls it, shadow then native: every state call made on the device as well; strips,
 *     fans and lists unrolled with each triangle's first vertex first; the draws left out of the device only when
 *     native; a DrawIndexedPrimitive, a Clear and a SetRenderTarget each cut a segment, marked before the game's call;
 *     a render-target texture left to the game; ApplyStateBlock followed by reading the state again; the markers at
 *     x = 16 kind + seq on the stencil only; the device's vtable its own again after;
 *   - a texture taken once (LockRect read only, every level), held (one reference) while the store has it, taken again
 *     when its pixels change, let go by drop_textures();
 *   - no stencil on the bound depth: no window (the game draws all of it);
 *   - stop(): the dump again, byte for byte; refused while something else changed a call.
 *
 *   build\uidraw_test.exe
 */

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../src/game/slotmark.hpp"
#include "../src/game/uidraw.hpp"
#include "../src/ui2d/frame2d.hpp"
#include "../src/ui2d/gate.hpp"
#include "../src/ui2d/render2d.hpp"
#include "game_dump.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	namespace ud = hifi::game::uidraw;
	namespace u2 = hifi::ui2d;

	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	size_t differences(const std::string &path, uintptr_t base, size_t size)
	{
		FILE *f = nullptr;
		if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr)
			return size_t(-1);
		std::vector<uint8_t> file(size);
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

	// ---------------------------------------------------------------- the stand-in objects

	std::string calls; // what reached the device, in order: 'S' a state, 'D' DPUP, 'I' DIP, 'C' Clear (k + seq), 'T' SetRenderTarget, 'E' EndScene, 'F' the fade

	struct surface
	{
		const uint32_t *vt;
		uint32_t format, w, h;
	};
	long __stdcall surf_desc(surface *s, uint32_t d[8])
	{
		std::memset(d, 0, 32);
		d[0] = s->format;
		d[6] = s->w;
		d[7] = s->h;
		return 0;
	}
	unsigned long __stdcall surf_release(surface *) { return 0; }

	struct texture
	{
		const uint32_t *vt;
		uint32_t format, w, h, levels, usage, pool;
		long refs = 1;
		uint32_t locks = 0, readonly_locks = 0;
		std::vector<std::vector<uint8_t>> bits;
	};
	unsigned long __stdcall tex_addref(texture *t) { return ++t->refs; }
	unsigned long __stdcall tex_release(texture *t) { return --t->refs; }
	uint32_t __stdcall tex_type(texture *) { return 3; }
	uint32_t __stdcall tex_levels(texture *t) { return t->levels; }
	long __stdcall tex_desc(texture *t, uint32_t l, uint32_t d[8])
	{
		std::memset(d, 0, 32);
		d[0] = t->format, d[1] = 3, d[2] = t->usage, d[3] = t->pool, d[6] = std::max(1u, t->w >> l), d[7] = std::max(1u, t->h >> l);
		return 0;
	}
	long __stdcall tex_lock(texture *t, uint32_t l, int32_t locked[2], const void *, uint32_t flags)
	{
		t->locks++;
		t->readonly_locks += (flags & 0x10) != 0;
		locked[0] = int32_t(std::max(1u, t->w >> l) * 4);
		locked[1] = int32_t(reinterpret_cast<uintptr_t>(t->bits[l].data()));
		return 0;
	}
	long __stdcall tex_unlock(texture *, uint32_t) { return 0; }

	uint32_t g_surf_vt[16], g_tex_vt[20];

	texture *make_texture(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t pool, uint8_t fill)
	{
		texture *t = new texture { g_tex_vt, 21, w, h, levels, usage, pool };
		for (uint32_t l = 0; l < levels; ++l)
			t->bits.emplace_back(size_t(std::max(1u, w >> l)) * std::max(1u, h >> l) * 4, uint8_t(fill + l));
		return t;
	}

	struct device
	{
		const uint32_t *vt;
		uint32_t rs[256] = {};
		uint32_t tss[2][32] = {};
		uintptr_t tex[2] = {};
		uint32_t vp[6] = { 0, 0, 640, 480, 0, 0x3F800000 };
		uint32_t vs = 0x144, ps = 0;
		surface *rt = nullptr, *ds = nullptr;
		uint32_t dpup = 0, dpup_vertices = 0, blocks = 0;
		std::vector<int32_t> marks; // (x of each stencil-only one-pixel clear)
	};
	device *g_dev = nullptr;

	long __stdcall d_set_rs(device *d, uint32_t s, uint32_t v) { d->rs[s & 255] = v; calls += 'S'; return 0; }
	long __stdcall d_get_rs(device *d, uint32_t s, uint32_t *v) { *v = d->rs[s & 255]; return 0; }
	long __stdcall d_set_tss(device *d, uint32_t st, uint32_t t, uint32_t v) { d->tss[st & 1][t & 31] = v; calls += 'S'; return 0; }
	long __stdcall d_get_tss(device *d, uint32_t st, uint32_t t, uint32_t *v) { *v = d->tss[st & 1][t & 31]; return 0; }
	long __stdcall d_set_tex(device *d, uint32_t st, uintptr_t t) { d->tex[st & 1] = t; calls += 'S'; return 0; }
	long __stdcall d_get_tex(device *d, uint32_t st, uintptr_t *t)
	{
		*t = d->tex[st & 1];
		if (*t != 0)
			tex_addref(reinterpret_cast<texture *>(*t));
		return 0;
	}
	long __stdcall d_set_vp(device *d, const uint32_t *v) { std::memcpy(d->vp, v, 24); calls += 'S'; return 0; }
	long __stdcall d_get_vp(device *d, uint32_t *v) { std::memcpy(v, d->vp, 24); return 0; }
	long __stdcall d_set_vs(device *d, uint32_t h) { d->vs = h; calls += 'S'; return 0; }
	long __stdcall d_get_vs(device *d, uint32_t *h) { *h = d->vs; return 0; }
	long __stdcall d_set_ps(device *d, uint32_t h) { d->ps = h; calls += 'S'; return 0; }
	long __stdcall d_get_ps(device *d, uint32_t *h) { *h = d->ps; return 0; }
	long __stdcall d_get_rt(device *d, uintptr_t *s) { *s = reinterpret_cast<uintptr_t>(d->rt); return 0; }
	long __stdcall d_get_ds(device *d, uintptr_t *s) { *s = reinterpret_cast<uintptr_t>(d->ds); return d->ds != nullptr ? 0 : -1; }
	long __stdcall d_set_rt(device *d, uintptr_t rt, uintptr_t ds)
	{
		if (rt != 0)
			d->rt = reinterpret_cast<surface *>(rt);
		d->ds = reinterpret_cast<surface *>(ds);
		d->vp[0] = d->vp[1] = 0, d->vp[2] = d->rt->w, d->vp[3] = d->rt->h;
		calls += 'T';
		return 0;
	}
	long __stdcall d_apply(device *d, uint32_t)
	{
		// (a state block: the blend set to additive behind the recorder's back)
		d->rs[27] = 1, d->rs[19] = 2, d->rs[20] = 2;
		d->blocks++;
		calls += 'B';
		return 0;
	}
	long __stdcall d_clear(device *d, uint32_t count, const int32_t *rects, uint32_t flags, uint32_t, float, uint32_t)
	{
		if (count == 1 && rects != nullptr && flags == 4 && rects[1] == 0 && rects[3] == 1 && rects[2] == rects[0] + 1)
		{
			d->marks.push_back(rects[0]);
			char t[16];
			std::snprintf(t, sizeof(t), "C%u.", unsigned(rects[0] / 16));
			calls += t;
		}
		else
			calls += 'X';
		return 0;
	}
	long __stdcall d_dp(device *, uint32_t, uint32_t, uint32_t) { calls += 'P'; return 0; }
	long __stdcall d_dip(device *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { calls += 'I'; return 0; }
	long __stdcall d_dpup(device *d, uint32_t type, uint32_t count, const void *, uint32_t)
	{
		d->dpup++;
		d->dpup_vertices += type == 4 ? 3 * count : count + 2;
		calls += 'D';
		return 0;
	}
	long __stdcall d_dipup(device *, uint32_t, uint32_t, uint32_t, uint32_t, const void *, uint32_t, const void *, uint32_t) { calls += 'U'; return 0; }
	long __stdcall d_end_scene(device *) { calls += 'E'; return 0; }

	uint32_t g_dev_vt[1 + 97];

	void make_vtables()
	{
		const auto put = [](uint32_t *vt, uint32_t i, auto fn) { vt[i] = uint32_t(reinterpret_cast<uintptr_t>(fn)); };
		uint32_t *const dv = g_dev_vt + 1;
		put(dv, 50, &d_set_rs), put(dv, 51, &d_get_rs), put(dv, 63, &d_set_tss), put(dv, 62, &d_get_tss), put(dv, 61, &d_set_tex), put(dv, 60, &d_get_tex);
		put(dv, 40, &d_set_vp), put(dv, 41, &d_get_vp), put(dv, 76, &d_set_vs), put(dv, 77, &d_get_vs), put(dv, 88, &d_set_ps), put(dv, 89, &d_get_ps);
		put(dv, 32, &d_get_rt), put(dv, 33, &d_get_ds), put(dv, 31, &d_set_rt), put(dv, 54, &d_apply), put(dv, 36, &d_clear), put(dv, 70, &d_dp);
		put(dv, 71, &d_dip), put(dv, 72, &d_dpup), put(dv, 73, &d_dipup), put(dv, 35, &d_end_scene);
		put(g_surf_vt, 2, &surf_release), put(g_surf_vt, 8, &surf_desc);
		put(g_tex_vt, 1, &tex_addref), put(g_tex_vt, 2, &tex_release), put(g_tex_vt, 10, &tex_type), put(g_tex_vt, 13, &tex_levels);
		put(g_tex_vt, 14, &tex_desc), put(g_tex_vt, 16, &tex_lock), put(g_tex_vt, 17, &tex_unlock);
	}

	// The game's way of calling its device: through the object's vtable as it is now
	template <typename F>
	F method(device *d, uint32_t i)
	{
		return reinterpret_cast<F>(uintptr_t((*reinterpret_cast<uint32_t *const *>(d))[i]));
	}
	void rs(device *d, uint32_t s, uint32_t v) { method<long(__stdcall *)(device *, uint32_t, uint32_t)>(d, 50)(d, s, v); }
	void tss(device *d, uint32_t st, uint32_t t, uint32_t v) { method<long(__stdcall *)(device *, uint32_t, uint32_t, uint32_t)>(d, 63)(d, st, t, v); }
	void set_tex(device *d, texture *t) { method<long(__stdcall *)(device *, uint32_t, texture *)>(d, 61)(d, 0, t); }
	void dpup(device *d, uint32_t type, uint32_t count, const void *data, uint32_t stride)
	{
		method<long(__stdcall *)(device *, uint32_t, uint32_t, const void *, uint32_t)>(d, 72)(d, type, count, data, stride);
	}

	struct v28
	{
		float x, y, z, rhw;
		uint32_t c;
		float u, v;
	};

	// ---------------------------------------------------------------- running the stubs

	struct regs
	{
		uint32_t eax, ebx, esi, edi, ebp, esp_diff;
	};
	regs run_begin(uintptr_t stub, uintptr_t wrapper)
	{
		uint32_t a = 0, b = 0, s = 0, d = 0, p = 0, before = 0, after = 0;
		__asm {
			push ebx
			push esi
			push edi
			mov eax, 0x44444444
			mov ebx, 0x11111111
			mov esi, 0x22222222
			mov edi, 0x33333333
			mov ecx, wrapper
			mov before, esp
			call dword ptr [stub]
			mov after, esp
			mov a, eax
			mov b, ebx
			mov s, esi
			mov d, edi
			pop edi
			pop esi
			pop ebx
		}
		return { a, b, s, d, p, before - after };
	}

	void __cdecl fake_crossfade()
	{
		calls += 'x';
	}
	uint32_t fake_pop_ecx = 0;
	void __fastcall fake_pop(uint32_t ecx, uint32_t)
	{
		fake_pop_ecx = ecx;
		calls += 'p';
	}

	uint32_t fade_args[2] = {}, fade_ecx = 0;
	void __fastcall fake_fade(uint32_t ecx, uint32_t, uint32_t a, uint32_t b)
	{
		fade_ecx = ecx;
		fade_args[0] = a;
		fade_args[1] = b;
		calls += 'F';
	}
	// As 0129E2 calls the fade: push eax (the colour); push ebx (0); call
	regs run_end(uintptr_t stub, uintptr_t wrapper)
	{
		uint32_t a = 0, b = 0, s = 0, d = 0, p = 0, before = 0, after = 0;
		__asm {
			push ebx
			push esi
			push edi
			mov eax, 0x80808080
			mov ebx, 0
			mov esi, 0x22222222
			mov edi, 0x33333333
			mov ecx, wrapper
			mov before, esp
			push eax
			push ebx
			call dword ptr [stub]
			mov after, esp
			mov a, eax
			mov b, ebx
			mov s, esi
			mov d, edi
			pop edi
			pop esi
			pop ebx
		}
		return { a, b, s, d, p, before - after };
	}
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

// Both dumps' ranges kept free of WARP's memory: reserved from the start, let go just before one is mapped there, the rest
// reserved again (the dumps overlap)
struct ranges
{
	uintptr_t lo = 0, hi = 0;
	std::vector<void *> held;
	void reserve(uintptr_t from, uintptr_t to)
	{
		for (uintptr_t a = from; a < to; a += 0x10000)
			if (void *p = VirtualAlloc(reinterpret_cast<void *>(a), 0x10000, MEM_RESERVE, PAGE_NOACCESS))
				held.push_back(p);
	}
	void release()
	{
		for (void *p : held)
			VirtualFree(p, 0, MEM_RELEASE);
		held.clear();
	}
};

int main()
{
	make_vtables();
	ranges keep;
	keep.lo = 0x041C0000;
	keep.hi = 0x04490000 + 0x00C20000;
	keep.reserve(keep.lo, keep.hi);
	// The store (WARP), so textures can be taken
	ID3D12Device *d12 = nullptr;
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	std::string why;
	const bool store = SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
		SUCCEEDED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12))) && u2::render::make(d12, why);
	check(store, "the texture store on WARP" + (why.empty() ? "" : ": " + why));

	// The gate (src/ui2d/gate.hpp): off unless wanted, patched and made; shadow until the markers come through with 30
	// segments in a row that could be drawn and the target known; native until one can't be (then 2 s of shadow) or the
	// markers stop for a quarter second
	{
		using hifi::ui2d::gate_mode;
		hifi::ui2d::gate_in in;
		in.wanted = in.patched = in.renderer = in.target = true;
		in.now_ms = 100000;
		check(hifi::ui2d::next_mode(in) == gate_mode::shadow, "gate: no marker seen yet: shadow");
		in.ok_at = in.now_ms - 10;
		in.in_a_row = 29;
		check(hifi::ui2d::next_mode(in) == gate_mode::shadow, "gate: 29 segments in a row: shadow");
		in.in_a_row = 30;
		check(hifi::ui2d::next_mode(in) == gate_mode::native, "gate: 30 segments in a row, the target known: native");
		in.target = false;
		check(hifi::ui2d::next_mode(in) == gate_mode::shadow, "gate: the target not known: shadow");
		in.target = true;
		in.was = gate_mode::native;
		in.in_a_row = 3;
		check(hifi::ui2d::next_mode(in) == gate_mode::native, "gate: native stays native while segments draw");
		in.fail_at = in.now_ms - 1999;
		check(hifi::ui2d::next_mode(in) == gate_mode::shadow, "gate: a segment that couldn't be drawn: shadow");
		in.fail_at = in.now_ms - 2001;
		in.was = gate_mode::shadow;
		check(hifi::ui2d::next_mode(in) == gate_mode::shadow, "gate: 2 s later, 3 in a row: still shadow");
		in.in_a_row = 30;
		check(hifi::ui2d::next_mode(in) == gate_mode::native, "gate: then 30 in a row: native again");
		in.was = gate_mode::native;
		in.ok_at = in.now_ms - 251;
		check(hifi::ui2d::next_mode(in) == gate_mode::shadow, "gate: no marker for a quarter second: shadow");
		in.ok_at = in.now_ms;
		in.shadow_only = true;
		check(hifi::ui2d::next_mode(in) == gate_mode::shadow, "gate: [screen2d] shadow: never native");
		in.shadow_only = false;
		for (int k = 0; k < 3; ++k)
		{
			hifi::ui2d::gate_in o = in;
			(k == 0 ? o.wanted : k == 1 ? o.patched : o.renderer) = false;
			check(hifi::ui2d::next_mode(o) == gate_mode::off, std::string("gate: ") + (k == 0 ? "not wanted" : k == 1 ? "not patched" : "no renderer") + ": off");
		}
		static_assert(uint32_t(gate_mode::native) == uint32_t(ud::native) && uint32_t(gate_mode::shadow) == uint32_t(ud::shadow), "the gate's modes are uidraw's");
	}

	// An image without the game's code
	{
		std::vector<uint8_t> junk(0x10000, 0x90);
		hifi::game::set_image(reinterpret_cast<uintptr_t>(junk.data()), junk.size());
		const bool started = ud::start(why);
		check(!started && !why.empty() && !ud::in(), "an image without the loop tail: refused (" + why + ")");
	}

	surface bb { g_surf_vt, 22, 2560, 1440 }, bbz { g_surf_vt, 75, 2560, 1440 }, other { g_surf_vt, 21, 1280, 720 }, plain_z { g_surf_vt, 77, 1280, 720 };
	for (const char *path : dump::kDumps)
	{
		const std::string name = path;
		uintptr_t base = 0;
		keep.release();
		const size_t size = dump::map(name, base);
		keep.reserve(keep.lo, dump::base_of(name));
		keep.reserve((dump::base_of(name) + size + 0xFFFF) & ~uintptr_t(0xFFFF), keep.hi);
		if (size == 0)
		{
			std::printf("SKIP  %s (not mapped)\n", path);
			continue;
		}
		const std::string tag = name.substr(name.rfind('\\') + 1) + ": ";
		why.clear();
		const bool started = ud::start(why);
		check(started, tag + "the loop tail's calls through ours" + (why.empty() ? "" : ": " + why));
		const ud::sites w = ud::where();
		check(w.begin_call - base == 0x012779 && w.begin_target - base == 0x00BB20, tag + "the window's begin at 012779 (call 00BB20)");
		check(w.end_call - base == 0x0129EA && w.end_target - base == 0x00A820, tag + "the interface's end at 0129EA (call the fade 00A820)");
		check(w.frame_call - base == 0x012AF5 && w.frame_target - base == 0x009740, tag + "the frame's end at 012AF5 (call EndScene 009740)");
		const size_t changed = differences(name, base, size);
		check(w.screen_call - base == 0x012465 && w.screen_target - base == 0x012F40, tag + "the screen passes' window at 012465 (call the crossfade 012F40)");
		check(w.pop_call - base == 0x0126A1 && w.pop_target - base == 0x00A1D0, tag + "its end at 0126A1 (call A's pop 00A1D0)");
		check(w.movie - base == 0x456D1C, tag + "the movie player's global at [0x456D1C]");
		check(changed >= 5 && changed <= 20, tag + "only the five rel32s changed (" + std::to_string(changed) + " bytes)");

		// The wrapper and its device
		device dev;
		dev.vt = g_dev_vt + 1;
		dev.rt = &bb;
		dev.ds = &bbz;
		dev.vp[2] = 2560, dev.vp[3] = 1440;
		dev.tss[0][1] = 5, dev.tss[0][2] = 2, dev.tss[0][3] = 1, dev.tss[0][4] = 5, dev.tss[0][5] = 2, dev.tss[0][6] = 1;
		dev.tss[0][13] = dev.tss[0][14] = 1, dev.tss[0][16] = 1, dev.tss[0][17] = 2, dev.tss[1][1] = 1;
		dev.rs[8] = 3, dev.rs[9] = 2, dev.rs[22] = 1, dev.rs[168] = 0xF, dev.rs[171] = 1, dev.rs[19] = 5, dev.rs[20] = 6, dev.rs[27] = 1, dev.rs[25] = 5;
		uint32_t wrapper[8] = {};
		wrapper[3] = uint32_t(reinterpret_cast<uintptr_t>(&dev));
		const uint32_t own_vt = uint32_t(reinterpret_cast<uintptr_t>(dev.vt));

		texture *const tex = make_texture(32, 32, 2, 0, 1, 0x40), *const target_tex = make_texture(64, 64, 1, 1, 0, 0x10);
		const v28 quad[4] = { { 9.5f, 9.5f, 0, 1, 0x80808080, 0, 0 }, { 41.5f, 9.5f, 0, 1, 0x80808080, 1, 0 }, { 9.5f, 41.5f, 0, 1, 0x80808080, 0, 1 },
			{ 41.5f, 41.5f, 0, 1, 0x80808080, 1, 1 } };
		const v28 fan[5] = { { 100, 100, 0, 1, 0x11111111, 0, 0 }, { 120, 90, 0, 1, 0x22222222, 0, 0 }, { 130, 110, 0, 1, 0x33333333, 0, 0 },
			{ 110, 130, 0, 1, 0x44444444, 0, 0 }, { 90, 115, 0, 1, 0x55555555, 0, 0 } };
		// (the target as the Direct3D 12 side finds it at the markers: the pipelines are made for it while native)
		u2::render::note_target(DXGI_FORMAT_R8G8B8A8_UNORM, 1);
		for (const ud::mode m : { ud::shadow, ud::native })
		{
			const std::string mt = tag + (m == ud::shadow ? "shadow: " : "native: ");
			ud::set_mode(m);
			dev.rs[19] = 5, dev.rs[20] = 6; // (the state block's additive blend from the last round undone)
			calls.clear();
			dev.marks.clear();
			dev.dpup = 0;
			const uint32_t refs = uint32_t(tex->refs);
			const ud::counts c0 = ud::get_counts();
			const regs r = run_begin(w.code, reinterpret_cast<uintptr_t>(wrapper));
			check(r.esp_diff == 0 && r.eax == 0x44444444 && r.ebx == 0x11111111 && r.esi == 0x22222222 && r.edi == 0x33333333,
				mt + "the begin stub: the stack and eax, ebx, esi, edi as they were");
			const uint32_t seq = ud::get_counts().seq;
			check(ud::get_counts().windows == c0.windows + 1 && *reinterpret_cast<uint32_t *>(&dev) != own_vt, mt + "the window open on the wrapper's device");
			check(dev.marks.size() == 1 && dev.marks[0] == int32_t(hifi::slotmark::x_of(hifi::slotmark::ui_begin, seq)), mt + "its begin marked (kind 9)");
			// The game's calls: a textured quad, a fan, a list; then a DIP (cut); a render-target texture (the game's)
			set_tex(&dev, tex);
			dpup(&dev, 5, 2, quad, 28);
			rs(&dev, 9, 1); // (flat)
			dpup(&dev, 6, 3, fan, 28);
			rs(&dev, 9, 2);
			dpup(&dev, 4, 1, fan, 28);
			method<long(__stdcall *)(device *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>(&dev, 71)(&dev, 4, 0, 3, 0, 1);
			set_tex(&dev, target_tex);
			dpup(&dev, 5, 2, quad, 28);
			set_tex(&dev, tex);
			// A state block behind its back, then a draw recorded with the block's blend
			method<long(__stdcall *)(device *, uint32_t)>(&dev, 54)(&dev, 2);
			dpup(&dev, 5, 2, quad, 28);
			// The end stub: the fade called after the interface's segment is marked
			uint8_t saved_jmp[5];
			std::memcpy(saved_jmp, reinterpret_cast<const void *>(w.code + 0x20 + 0x11), 5);
			uint8_t to_fake[5] = { 0xE9 };
			hifi::game::put32(to_fake + 1, uint32_t(reinterpret_cast<uintptr_t>(&fake_fade) - (w.code + 0x20 + 0x11 + 5)));
			hifi::game::write_code(w.code + 0x20 + 0x11, to_fake, 5);
			const regs re = run_end(w.code + 0x20, reinterpret_cast<uintptr_t>(wrapper));
			hifi::game::write_code(w.code + 0x20 + 0x11, saved_jmp, 5);
			check(re.esp_diff == 0 && re.esi == 0x22222222 && re.edi == 0x33333333 && fade_args[0] == 0 && fade_args[1] == 0x80808080 &&
				fade_ecx == uint32_t(reinterpret_cast<uintptr_t>(wrapper)), mt + "the end stub: the fade called with its two arguments and the wrapper, the stack kept");
			// After the fade: a SetRenderTarget (cut) to a target whose depth has no stencil (left to the game), back
			dpup(&dev, 5, 2, quad, 28);
			method<long(__stdcall *)(device *, surface *, surface *)>(&dev, 31)(&dev, &other, &plain_z);
			dpup(&dev, 5, 2, quad, 28);
			method<long(__stdcall *)(device *, surface *, surface *)>(&dev, 31)(&dev, &bb, &bbz);
			dpup(&dev, 5, 2, quad, 28);
			// The frame stub: into the game's EndScene wrapper (009740), which calls the device's EndScene
			{
				const uintptr_t stub = w.code + 0x40, wr = reinterpret_cast<uintptr_t>(wrapper);
				__asm {
					mov ecx, wr
					call dword ptr [stub]
				}
			}
			check(*reinterpret_cast<uint32_t *>(&dev) == own_vt, mt + "the device's own vtable back at the frame's end");
			const u2::frame *f = u2::published(seq & 15);
			check(f != nullptr && f->seq.load() == seq, mt + "the window's frame published");
			if (f == nullptr)
				continue;
			const ud::counts c1 = ud::get_counts();
			// Recorded: quad, fan, list | (DIP) | (render target texture: the game's) | quad after the block | (fade) quad |
			// (SetRenderTarget) (no stencil: the game's) | (SetRenderTarget) quad
			check(f->draw_count == 6 && c1.recorded - c0.recorded == 6 && c1.passed - c0.passed == 3, mt + "6 draws recorded, 3 left to the game (" +
				std::to_string(f->draw_count) + ", " + std::to_string(c1.passed - c0.passed) + ")");
			check(f->segments.load() == 4, mt + "4 segments (" + std::to_string(f->segments.load()) + ")");
			const std::string kinds = [&] {
				std::string k;
				for (uint32_t i = 0; i < f->segments.load(); ++i)
					k += char('0' + f->seg[i].kind % 10) + std::to_string(f->seg[i].count) + ' ';
				return k;
			}();
			check(kinds == "03 01 01 11 ", mt + "segments: 3 cut by the DIP (the render-target texture's draw after it cuts nothing), 1 at the interface's end, 1 cut by the target change, 1 at the frame's end (" + kinds + ")");
			// The device's calls, in order: the markers before the game's own calls
			const size_t dip = calls.find('I'), cut_before_dip = calls.rfind("C10.", dip), fade = calls.find('F'), end = calls.find('E');
			check(dip != std::string::npos && cut_before_dip != std::string::npos && cut_before_dip < dip, mt + "the DIP's cut marked before the DIP");
			check(fade != std::string::npos && calls.rfind("C10.", fade) != std::string::npos && calls.rfind("C10.", fade) > calls.find('B'), mt + "the interface's segment marked before the fade");
			check(end != std::string::npos && calls.rfind("C11.", end) != std::string::npos && calls.rfind("C11.", end) > calls.rfind('T'), mt + "the frame's end marked before EndScene");
			const uint32_t marks_x = uint32_t(dev.marks.back());
			check(marks_x == hifi::slotmark::x_of(hifi::slotmark::frame_end, seq), mt + "markers at x = 16 kind + seq");
			uint32_t states = 0;
			for (const char ch : calls)
				states += ch == 'S';
			check(states == 5, mt + "every state call made on the device as well (" + std::to_string(states) + ")");
			// Left out of the device only when native: 8 DPUPs, 2 of them the game's
			check(dev.dpup == (m == ud::native ? 2u : 8u), mt + (m == ud::native ? "only the 2 DrawPrimitiveUPs left to the game reached the device" : "all 8 reached the device") +
				" (" + std::to_string(dev.dpup) + ")");
			// The vertices: the quad's strip as (0,1,2) (1,3,2); the fan's as (1,2,0) (2,3,0) (3,4,0); the list as it is
			const u2::vertex *v = f->vertices;
			const bool quad_ok = f->draws[0].count == 6 && v[0].x == 9.5f && v[1].x == 41.5f && v[2].y == 41.5f && v[3].x == 41.5f && v[3].y == 9.5f && v[4].y == 41.5f &&
				v[4].x == 41.5f && v[4].u == 1.0f && v[5].x == 9.5f && v[5].y == 41.5f && v[5].u == 0.0f && v[5].v == 1.0f && v[0].colour == 0x80808080;
			check(quad_ok, mt + "the strip unrolled: (0, 1, 2), (1, 3, 2): each triangle's first vertex first");
			const u2::vertex *fv = v + f->draws[1].first;
			check(f->draws[1].count == 9 && fv[0].colour == 0x22222222 && fv[1].colour == 0x33333333 && fv[2].colour == 0x11111111 && fv[3].colour == 0x33333333 &&
				fv[6].colour == 0x44444444 && fv[8].colour == 0x11111111 && f->draws[1].flat == 1, mt + "the fan unrolled: (1, 2, 0), (2, 3, 0), (3, 4, 0), flat");
			check(f->draws[2].count == 3 && v[f->draws[2].first].colour == 0x11111111 && f->draws[2].flat == 0, mt + "the list as it is");
			check(f->draws[0].texture != u2::kNoTexture && f->draws[0].texture == f->draws[1].texture && f->draws[0].texture == f->draws[3].texture,
				mt + "the texture's slot on every draw that samples it (" + std::to_string(f->draws[0].texture) + ", " + std::to_string(f->draws[3].texture) + ")");
			check(f->draws[3].blend.enable == 1 && f->draws[3].blend.src == 2 && f->draws[3].blend.dst == 2 && f->draws[0].blend.src == 5,
				mt + "the state block's blend read again after ApplyStateBlock (" + std::to_string(f->draws[3].blend.src) + "/" + std::to_string(f->draws[3].blend.dst) + ")");
			check(f->draws[0].clip[2] == 2560 && f->draws[0].target_w == 2560 && f->draws[0].mag_filter == 1 && f->draws[0].min_filter == 2 &&
				f->draws[0].colour.op == 5, mt + "the state as the device had it (viewport, target, filters, MODULATE2X)");
			bool natives = true;
			for (uint32_t i = 0; i < f->segments.load(); ++i)
				natives = natives && f->seg[i].native == (m == ud::native);
			check(natives, mt + "the segments say who drew them");
			check(tex->readonly_locks >= 2 && tex->refs == long(refs) + (m == ud::shadow ? 1 : 0), mt + "the texture read only and held once (" + std::to_string(tex->refs) + " refs)");
		}
		// New pixels: taken again
		{
			ud::set_mode(ud::native);
			const ud::counts c0 = ud::get_counts();
			tex->bits[0][5] ^= 0xFF;
			run_begin(w.code, reinterpret_cast<uintptr_t>(wrapper));
			set_tex(&dev, tex);
			dpup(&dev, 5, 2, quad, 28);
			ud::window_flush(hifi::slotmark::frame_end);
			check(ud::get_counts().texture_changes == c0.texture_changes + 1 && tex->refs == 2, tag + "a texture with new pixels taken again, still held once");
			ud::drop_textures();
			check(tex->refs == 1, tag + "drop_textures: let go");
		}
		// The screen passes' window on A (its stub reads the wrapper from the game's global; its jumps to the crossfade
		// and A's pop stand-ins): a wash quad recorded, a DIP cut (kind 8), the window closed and marked (kind 8) before
		// the pop, no begin marker
		{
			ud::set_mode(ud::native);
			const uintptr_t sc = hifi::game::find("8B0D????????508B91C001000052E8????????E8????????8BCEE8????????A0????????3C80");
			const uintptr_t global = hifi::game::word(sc + 2);
			const uint32_t old_wrapper = hifi::game::word(global);
			*reinterpret_cast<uint32_t *>(global) = uint32_t(reinterpret_cast<uintptr_t>(wrapper));
			uint8_t saved_screen[5], saved_pop[5];
			std::memcpy(saved_screen, reinterpret_cast<const void *>(w.code + 0x60 + 0x14), 5);
			std::memcpy(saved_pop, reinterpret_cast<const void *>(w.code + 0x80 + 0x14), 5);
			uint8_t j1[5] = { 0xE9 }, j2[5] = { 0xE9 };
			hifi::game::put32(j1 + 1, uint32_t(reinterpret_cast<uintptr_t>(&fake_crossfade) - (w.code + 0x60 + 0x14 + 5)));
			hifi::game::put32(j2 + 1, uint32_t(reinterpret_cast<uintptr_t>(&fake_pop) - (w.code + 0x80 + 0x14 + 5)));
			hifi::game::write_code(w.code + 0x60 + 0x14, j1, 5);
			hifi::game::write_code(w.code + 0x80 + 0x14, j2, 5);
			calls.clear();
			dev.marks.clear();
			dev.dpup = 0;
			const ud::counts c0 = ud::get_counts();
			const regs r = run_begin(w.code + 0x60, 0x12345678);
			check(r.esp_diff == 0 && r.ebx == 0x11111111 && r.esi == 0x22222222 && r.edi == 0x33333333 && calls.find('X') == std::string::npos,
				tag + "A: the screen stub: the crossfade called, the stack and registers kept");
			const uint32_t seq = ud::get_counts().seq;
			check(ud::get_counts().windows == c0.windows + 1 && dev.marks.empty() && calls.find('x') != std::string::npos,
				tag + "A: the window open (no begin marker), then the game's crossfade");
			const v28 wash[4] = { { -0.5f, -0.5f, 0, 1, 0x80FFFFFF, 0, 0 }, { 2559.5f, -0.5f, 0, 1, 0x80FFFFFF, 0, 0 }, { -0.5f, 1439.5f, 0, 1, 0x80FFFFFF, 0, 0 },
				{ 2559.5f, 1439.5f, 0, 1, 0x80FFFFFF, 0, 0 } };
			method<long(__stdcall *)(device *, uint32_t)>(&dev, 76)(&dev, 0x44);
			tss(&dev, 0, 1, 2), tss(&dev, 0, 2, 0), tss(&dev, 0, 4, 2), tss(&dev, 0, 5, 0);
			dpup(&dev, 5, 2, wash, 20);
			method<long(__stdcall *)(device *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>(&dev, 71)(&dev, 4, 0, 3, 0, 1);
			dpup(&dev, 5, 2, wash, 20);
			{
				const uintptr_t stub = w.code + 0x80, wr = reinterpret_cast<uintptr_t>(wrapper);
				__asm {
					mov ecx, wr
					call dword ptr [stub]
				}
			}
			const u2::frame *f = u2::published(seq & 15);
			check(f != nullptr && f->draw_count == 2 && f->segments.load() == 2 && f->seg[0].kind == hifi::slotmark::screen && f->seg[1].kind == hifi::slotmark::screen &&
				f->draws[0].colour.op == 2 && f->draws[0].target_w == 2560, tag + "A: the wash recorded twice, two segments of kind 8 (the DIP's cut, the pop)");
			check(dev.dpup == 0 && dev.marks.size() == 2 && uint32_t(dev.marks[1]) == hifi::slotmark::x_of(hifi::slotmark::screen, seq) &&
				calls.rfind("C8.") < calls.find('p') && *reinterpret_cast<uint32_t *>(&dev) == own_vt && fake_pop_ecx == uint32_t(reinterpret_cast<uintptr_t>(wrapper)),
				tag + "A: marked before the pop, the pop called with the wrapper, the device's own vtable back");
			hifi::game::write_code(w.code + 0x60 + 0x14, saved_screen, 5);
			hifi::game::write_code(w.code + 0x80 + 0x14, saved_pop, 5);
			*reinterpret_cast<uint32_t *>(global) = old_wrapper;
			method<long(__stdcall *)(device *, uint32_t)>(&dev, 76)(&dev, 0x144);
			tss(&dev, 0, 1, 5), tss(&dev, 0, 2, 2), tss(&dev, 0, 4, 5), tss(&dev, 0, 5, 2);
		}
		// The opening movie: no window while its player exists
		{
			const ud::counts c0 = ud::get_counts();
			*reinterpret_cast<uint32_t *>(w.movie) = 0x1234;
			run_begin(w.code, reinterpret_cast<uintptr_t>(wrapper));
			*reinterpret_cast<uint32_t *>(w.movie) = 0;
			check(ud::get_counts().movie == c0.movie + 1 && ud::get_counts().windows == c0.windows && *reinterpret_cast<uint32_t *>(&dev) == own_vt,
				tag + "the opening movie playing: no window, the game draws it all");
		}
		// No stencil on the bound depth: no window
		{
			const ud::counts c0 = ud::get_counts();
			dev.ds = &plain_z;
			run_begin(w.code, reinterpret_cast<uintptr_t>(wrapper));
			check(ud::get_counts().no_window == c0.no_window + 1 && *reinterpret_cast<uint32_t *>(&dev) == own_vt, tag + "no stencil to mark on: no window, the game draws it");
			dev.ds = &bbz;
			ud::set_mode(ud::off);
		}
		// stop(): refused while a call isn't ours, then the dump again
		{
			uint8_t saved[5];
			std::memcpy(saved, reinterpret_cast<const void *>(w.end_call), 5);
			const uint8_t other_call[5] = { 0xE8, 0, 0, 0, 0 };
			hifi::game::write_code(w.end_call, other_call, 5);
			const bool stopped = ud::stop(why);
		check(!stopped && ud::in(), tag + "stop() refused while something else changed a call (" + why + ")");
			hifi::game::write_code(w.end_call, saved, 5);
			check(ud::stop(why) && !ud::in(), tag + "stop()");
			check(dump::same_as_file(name, base, size), tag + "the dump again, byte for byte");
		}
		delete tex;
		delete target_tex;
		dump::unmap(base);
	}
	u2::render::release();
	std::printf("\n%d passed, %d failed\n", passes, failures);
	return failures == 0 ? 0 : 1;
}
