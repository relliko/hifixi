/*
 * Offline test of the cloth (type-1) meshes HiFi XI draws in its own character models (src/game/charmodel.cpp,
 * src/chars/os2.cpp; research/ffxi-re/notes-actor-extras.md 1), on each FFXiMain dump mapped at its own base
 * (game_dump.hpp), against tests/cloth_oracle.py (the game's step in Python, made first: build.cmd runs it):
 *   - the cloth's sites found: 0224D0's call of 023290 and its two globals, 025030 (the cloth node's draw) calling
 *     0224D0, 023290's palette pointer;
 *   - each oracle DAT (the San d'Orian banner ROM/7/95; a Gigas ROM/203/121 with two cloths, one- and two-bone
 *     vertices) laid out as the game holds it loaded: its resources' handles in a stand-in for the resource manager's
 *     table (the step checks the skeleton's handle there for its spheres), each cloth resource's eight section offsets
 *     made pointers (022470), a model whose instance names the skeleton, a palette and every mesh, the cloth meshes in
 *     nodes as 029F70 makes them (vtable +0x1C = 025030, +0x3C reset, +0x40 a work buffer, +0x44 n + 1);
 *   - the environment's wind and the step's scratch allocations (the game's pools: not in the dump) stood in for;
 *   - the models drawn through charslot's walk and the model stub as 02B6E0 reaches it, the cache uploading to WARP:
 *     once the meshes are on the GPU, each model HiFi XI's (the early return), its cloth an instance of its own per
 *     mesh, and frame by frame (the skeleton carried along a path, the wind gusting then stopped, the actor's no-wind
 *     flag, frames that aren't pose frames) every cloth vertex handed over (the rows' translation and row 0) as the
 *     oracle steps it: the dump's own 023290 run by the stub, the spheres left out (their counts zeroed);
 *   - then with the spheres: the pinned vertices still the oracle's, the rest finite and near, unit normals;
 *   - a cloth whose node isn't the game's cloth node: the model left to the game;
 *   - the actor's extra passes (notes-actor-extras.md 2) as 0D6D30 leaves them for 02B6E0: their globals found in
 *     02B6E0's code; the overlay (its flag, offsets, factor, its texture through the handle, the kind-10 reference
 *     and the texture object to the texture's resource) and the cloak (offsets, factor, PROJECTION read at the walk's
 *     begin) recorded with the model; an overlay whose factor's alpha is 0 not kept; one whose texture isn't on the GPU
 *     yet, or a cloak that adds something on a model of the opaque walk: the model the game's;
 *   - stop(): the dump again, byte for byte.
 *
 *   build\cloth_test.exe build\test\cloth_oracle.bin
 */

#include "../src/chars/cache.hpp"
#include "../src/chars/charframe.hpp"
#include "../src/chars/os2.hpp"
#include "../src/game/charmodel.hpp"
#include "../src/game/charslot.hpp"
#include "../src/game/slotmark.hpp"
#include "game_dump.hpp"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	namespace ch = hifi::chars;
	const char *kGame = "C:\\Games\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI\\";

	uint8_t *alloc_aligned(size_t n)
	{
		// (top down: the dumps are mapped at their own bases, low in the address space)
		return static_cast<uint8_t *>(VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
	}

	// ------------------------------------------------------------------------------------------ the oracle
	struct oracle_mesh
	{
		uint32_t ordinal = 0, n = 0;
	};
	struct oracle_frame
	{
		std::vector<float> palette; // bones x 16
		float move[3] = {}, strength = 0.0f;
		uint32_t no_wind = 0, pose = 1;
		std::vector<std::vector<float>> pos, nrm; // per mesh, 3 a vertex
	};
	struct oracle_dat
	{
		std::string rel;
		uint32_t bones = 0;
		std::vector<oracle_mesh> meshes;
		std::vector<oracle_frame> frames;
	};
	bool read_oracle(const char *path, std::vector<oracle_dat> &out)
	{
		std::ifstream f(path, std::ios::binary);
		std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		size_t o = 0;
		auto u32 = [&]() {
			uint32_t v = 0;
			if (o + 4 <= d.size())
				std::memcpy(&v, d.data() + o, 4);
			o += 4;
			return v;
		};
		auto floats = [&](std::vector<float> &v, size_t n) {
			v.resize(n);
			if (o + 4 * n <= d.size())
				std::memcpy(v.data(), d.data() + o, 4 * n);
			o += 4 * n;
		};
		const uint32_t dats = u32();
		for (uint32_t k = 0; k < dats && o < d.size(); ++k)
		{
			oracle_dat od;
			const uint32_t len = u32();
			od.rel.assign(reinterpret_cast<const char *>(d.data() + o), len);
			o += len;
			od.bones = u32();
			const uint32_t meshes = u32(), frames = u32();
			for (uint32_t m = 0; m < meshes; ++m)
			{
				oracle_mesh om;
				om.ordinal = u32();
				om.n = u32();
				od.meshes.push_back(om);
			}
			for (uint32_t fr = 0; fr < frames; ++fr)
			{
				oracle_frame of;
				floats(of.palette, size_t(od.bones) * 16);
				std::vector<float> mv;
				floats(mv, 4);
				std::memcpy(of.move, mv.data(), 12);
				of.strength = mv[3];
				of.no_wind = u32();
				of.pose = u32();
				of.pos.resize(meshes);
				of.nrm.resize(meshes);
				for (uint32_t m = 0; m < meshes; ++m)
				{
					floats(of.pos[m], size_t(od.meshes[m].n) * 3);
					floats(of.nrm[m], size_t(od.meshes[m].n) * 3);
				}
				od.frames.push_back(std::move(of));
			}
			out.push_back(std::move(od));
		}
		return o == d.size() && !out.empty();
	}

	// ------------------------------------------------------------------------------------------ a DAT, loaded
	// The resource manager's table as the step's sphere lookup checks a handle against it (0741E0: a handle in
	// [table + 8, table + 0x40004], what it names outside it): every handle a slot of it
	struct loaded
	{
		std::vector<uint8_t *> res;
		std::vector<uint32_t> kind;
		std::vector<uint32_t *> handle;
		uintptr_t sk2 = 0;
		uint32_t bones = 0;
		std::vector<uintptr_t> meshes;     // the Os2 resources in file order
		std::vector<bool> cloth;
		uint32_t textures = 0;
	};
	uint8_t *g_table = nullptr;
	uint32_t g_next_slot = 0;

	bool load(const std::string &rel, loaded &L)
	{
		std::string path = kGame + rel;
		for (char &c : path)
			if (c == '/')
				c = '\\';
		std::ifstream f(path, std::ios::binary);
		std::vector<uint8_t> dat((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if (dat.empty())
			return false;
		std::vector<size_t> folders;
		size_t off = 0;
		while (off + 16 <= dat.size())
		{
			uint32_t w;
			std::memcpy(&w, dat.data() + off + 4, 4);
			const size_t size = size_t((w >> 7) & 0x7FFFF) * 16;
			if (size < 16 || off + size > dat.size())
				break;
			uint8_t *r = alloc_aligned(0x30 + size + 16);
			std::memcpy(r + 0x20, dat.data() + off, 16);
			std::memcpy(r + 0x30, dat.data() + off + 16, size - 16);
			uint32_t *h = reinterpret_cast<uint32_t *>(g_table + 8 + 4 * size_t(g_next_slot++));
			*h = uint32_t(reinterpret_cast<uintptr_t>(r));
			*reinterpret_cast<uint32_t *>(r + 0x14) = uint32_t(reinterpret_cast<uintptr_t>(h));
			const uint32_t kind = w & 0x7F;
			if (!L.res.empty())
				*reinterpret_cast<uint32_t *>(L.res.back() + 8) = uint32_t(reinterpret_cast<uintptr_t>(h));
			if (kind == 1)
			{
				*reinterpret_cast<uint32_t *>(r + 0x28) = uint32_t(reinterpret_cast<uintptr_t>(h));
				folders.push_back(L.res.size());
			}
			else
			{
				if (!folders.empty())
					*reinterpret_cast<uint32_t *>(r + 0x28) = uint32_t(reinterpret_cast<uintptr_t>(L.handle[folders.back()]));
				if (kind == 0 && !folders.empty())
					folders.pop_back();
			}
			L.res.push_back(r);
			L.kind.push_back(kind);
			L.handle.push_back(h);
			L.textures += kind == 0x20;
			off += size;
		}
		for (size_t i = 0; i < L.res.size(); ++i)
		{
			uint8_t *r = L.res[i];
			if (L.kind[i] == 0x29 && L.sk2 == 0)
			{
				L.sk2 = reinterpret_cast<uintptr_t>(r);
				L.bones = *reinterpret_cast<uint16_t *>(r + 0x32);
			}
			if (L.kind[i] != 0x2A)
				continue;
			// The section offsets made pointers (02D8F0: the six; 022470 for a cloth: the six, the constraints, the
			// spheres)
			const bool cloth = (r[0x32] & 0x7F) == 1;
			const uint32_t base = uint32_t(reinterpret_cast<uintptr_t>(r + 0x30));
			const uint32_t six[6] = { 6, 0xC, 0x12, 0x18, 0x1E, 0x24 };
			for (uint32_t at : six)
			{
				uint32_t words;
				std::memcpy(&words, r + 0x30 + at, 4);
				const uint32_t p = base + words * 2;
				std::memcpy(r + 0x30 + at, &p, 4);
			}
			if (cloth)
				for (uint32_t at : { 0x2Eu, 0x4Cu })
				{
					uint32_t words;
					std::memcpy(&words, r + 0x30 + at, 4);
					const uint32_t p = base + words * 2;
					std::memcpy(r + 0x30 + at, &p, 4);
				}
			L.meshes.push_back(reinterpret_cast<uintptr_t>(r));
			L.cloth.push_back(cloth);
		}
		return L.sk2 != 0 && !L.meshes.empty();
	}
	uint16_t sphere_count(uintptr_t res) { return *reinterpret_cast<const uint16_t *>(res + 0x30 + 0x50); }
	void set_sphere_count(uintptr_t res, uint16_t n) { *reinterpret_cast<uint16_t *>(res + 0x30 + 0x50) = n; }
	uint32_t cloth_n(uintptr_t res)
	{
		const uint16_t *counts = reinterpret_cast<const uint16_t *>(uintptr_t(*reinterpret_cast<const uint32_t *>(res + 0x30 + 0x12)));
		return uint32_t(counts[0]) + counts[1];
	}

	// A model: +0x30 posed once (bit 4), +0x34 its move, +0x44 one instance (+0xC the skeleton's handle, +0x14 the palette,
	// +0x20 the meshes); a cloth mesh's node as 029F70 makes it; the actor (+0x17C: no wind)
	uint32_t g_cloth_vtable[16] = {};
	struct fake_model
	{
		uint8_t *model = nullptr, *instance = nullptr, *actor = nullptr;
		std::vector<float> palette;
		std::vector<uint8_t *> nodes;
		std::vector<uint8_t *> cloth_nodes; // (in mesh order)
	};
	void make_model(const loaded &L, fake_model &m)
	{
		m.model = alloc_aligned(0x200);
		m.instance = alloc_aligned(0x40);
		m.actor = alloc_aligned(0x800);
		m.palette.assign(size_t(L.bones) * 16, 0.0f);
		*reinterpret_cast<uint32_t *>(m.model + 0x30) = 0x10;
		*reinterpret_cast<uint32_t *>(m.model + 0x44) = uint32_t(reinterpret_cast<uintptr_t>(m.instance));
		*reinterpret_cast<uint32_t *>(m.instance + 0xC) = *reinterpret_cast<const uint32_t *>(L.sk2 + 0x14);
		*reinterpret_cast<uint32_t *>(m.instance + 0x14) = uint32_t(reinterpret_cast<uintptr_t>(m.palette.data()));
		uint8_t *prev = nullptr;
		for (size_t i = 0; i < L.meshes.size(); ++i)
		{
			const uintptr_t r = L.meshes[i];
			uint8_t *n = alloc_aligned(L.cloth[i] ? 0x1C8 : 0x20);
			*reinterpret_cast<uint32_t *>(n + 8) = *reinterpret_cast<const uint32_t *>(r + 0x14);
			if (L.cloth[i])
			{
				const uint32_t count = cloth_n(r);
				*reinterpret_cast<uint32_t *>(n) = uint32_t(reinterpret_cast<uintptr_t>(g_cloth_vtable));
				n[0x3C] = 1;
				// (the pool's 4-byte alignment: the step aligns it to 16)
				*reinterpret_cast<uint32_t *>(n + 0x40) = uint32_t(reinterpret_cast<uintptr_t>(alloc_aligned((count + 1) * 60 + 16) + 4));
				*reinterpret_cast<uint32_t *>(n + 0x44) = count + 1;
				m.cloth_nodes.push_back(n);
			}
			if (prev == nullptr)
				*reinterpret_cast<uint32_t *>(m.instance + 0x20) = uint32_t(reinterpret_cast<uintptr_t>(n));
			else
				*reinterpret_cast<uint32_t *>(prev + 4) = uint32_t(reinterpret_cast<uintptr_t>(n));
			prev = n;
			m.nodes.push_back(n);
		}
	}

	// ------------------------------------------------------------------------------------------ the step's stand-ins
	// 028640 (the game's pool, cdecl (count, kind)) and 028700 (its free), called by 023700 for its scratch
	uint32_t g_allocs = 0, g_frees = 0;
	void *__cdecl fake_alloc(int n, int)
	{
		g_allocs++;
		return std::calloc(size_t(n < 1 ? 1 : n) + 16, 1);
	}
	void __cdecl fake_free(void *p)
	{
		g_frees++;
		std::free(p);
	}
	void point_call(uintptr_t at, const void *to)
	{
		*reinterpret_cast<int32_t *>(at + 1) = int32_t(reinterpret_cast<uintptr_t>(to) - (at + 5));
	}

	// ------------------------------------------------------------------------------------------ running the site
	uint32_t g_stub = 0, g_model = 0, g_actor = 0, g_colour = 0, g_saved_esp = 0, g_path = 0;
	__declspec(naked) void land_go_on()
	{
		__asm {
			mov g_path, 1
			mov esp, g_saved_esp
			ret
		}
	}
	__declspec(naked) void land_leave()
	{
		__asm {
			mov g_path, 2
			mov esp, g_saved_esp
			ret
		}
	}
	__declspec(naked) void enter_site()
	{
		__asm {
			mov g_saved_esp, esp
			sub esp, 0xB0
			mov eax, g_colour
			mov [esp + 0xA8], eax
			mov eax, g_actor
			mov [esp + 0x90], eax
			mov ebp, g_model
			mov esi, g_actor
			jmp g_stub
		}
	}
	void run_site(uintptr_t stub, uintptr_t model, uintptr_t actor, uint32_t colour)
	{
		g_stub = uint32_t(stub);
		g_model = uint32_t(model);
		g_actor = uint32_t(actor);
		g_colour = colour;
		g_path = 0;
		__asm {
			pushad
			call enter_site
			popad
		}
	}
	fake_model *g_m = nullptr;
	uint32_t __cdecl fake_walk()
	{
		run_site(hifi::game::charmodel::where().stub, uintptr_t(g_m->model), uintptr_t(g_m->actor), 0x80808080u);
		return 0;
	}
	__declspec(naked) void call_walk(uint32_t)
	{
		__asm {
			pushad
			call dword ptr [esp + 0x24]
			popad
			ret
		}
	}
	void sink(uint32_t, uint32_t) {}

	// ------------------------------------------------------------------------------------------ WARP
	ID3D12Device *make_warp()
	{
		IDXGIFactory4 *factory = nullptr;
		IDXGIAdapter *warp = nullptr;
		ID3D12Device *d = nullptr;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
			FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d))))
			return nullptr;
		return d;
	}

	void *g_round[2] = {};
	void hold_round(uintptr_t base, size_t size)
	{
		constexpr uintptr_t from = 0x041C0000, to = 0x05200000;
		for (void *&p : g_round)
			if (p != nullptr)
			{
				VirtualFree(p, 0, MEM_RELEASE);
				p = nullptr;
			}
		if (size == 0)
			return;
		if (base > from)
			g_round[0] = VirtualAlloc(reinterpret_cast<void *>(from), base - from, MEM_RESERVE, PAGE_NOACCESS);
		const uintptr_t end = (base + size + 0xFFFF) & ~uintptr_t(0xFFFF);
		if (end < to)
			g_round[1] = VirtualAlloc(reinterpret_cast<void *>(end), to - end, MEM_RESERVE, PAGE_NOACCESS);
	}

	// The image's words put back from the file
	void restore(const std::string &path, uintptr_t base, uintptr_t at, size_t n)
	{
		FILE *fl = nullptr;
		if (fopen_s(&fl, path.c_str(), "rb") == 0 && fl != nullptr)
		{
			fseek(fl, long(at - base), SEEK_SET);
			fread(reinterpret_cast<void *>(at), 1, n, fl);
			fclose(fl);
		}
	}

	// ------------------------------------------------------------------------------------------ one DAT on one dump
	struct globals
	{
		uintptr_t env = 0, flag = 0;
		float *wind = nullptr; // (the stand-in environment: +0x124 direction, +0x134 strength)
	};

	// The frame's model 0 and its cloth instances' rows: positions and normals per cloth (empty when not drawn)
	bool frame_cloth(const loaded &L, std::vector<std::vector<float>> &pos, std::vector<std::vector<float>> &nrm, bool &native, uint32_t &instances)
	{
		const ch::frame *f = ch::published(hifi::game::charslot::get_counts().seq & 15);
		if (f == nullptr || f->models < 1)
			return false;
		const ch::model_entry &m = f->model[0];
		native = (m.flags & ch::kNative) != 0;
		instances = m.instances;
		pos.clear();
		nrm.clear();
		uint32_t cloths = 0;
		for (bool c : L.cloth)
			cloths += c;
		if (m.instances != 1 + cloths)
			return false;
		for (uint32_t c = 0; c < cloths; ++c)
		{
			const ch::instance_entry &ie = f->instance[m.first_instance + 1 + c];
			std::vector<float> p, n;
			for (uint32_t b = 0; b < ie.bones; ++b)
			{
				const float *row = f->palette[ie.first_bone + b].m;
				p.insert(p.end(), { row[9], row[10], row[11] });
				n.insert(n.end(), { row[0], row[1], row[2] });
			}
			pos.push_back(std::move(p));
			nrm.push_back(std::move(n));
		}
		return true;
	}

	void set_frame(fake_model &m, const globals &G, const oracle_frame &of)
	{
		std::memcpy(m.palette.data(), of.palette.data(), m.palette.size() * 4);
		std::memcpy(m.model + 0x34, of.move, 12);
		G.wind[0x134 / 4] = of.strength;
		m.actor[0x17C] = uint8_t(of.no_wind);
		*reinterpret_cast<uint8_t *>(G.flag) = uint8_t(of.pose);
	}

	void test_dat(const oracle_dat &od, const globals &G, ID3D12Device *warp, uintptr_t walk_stub, bool passes_test)
	{
		namespace cm = hifi::game::charmodel;
		std::printf("  %s\n", od.rel.c_str());
		loaded L;
		const bool ok = load(od.rel, L);
		uint32_t cloths = 0;
		for (bool c : L.cloth)
			cloths += c;
		check(ok && L.bones == od.bones && cloths == od.meshes.size(), "    laid out as loaded: " + std::to_string(L.meshes.size()) + " meshes (" + std::to_string(cloths) +
			" cloth), " + std::to_string(L.bones) + " bones, the handles in the manager's table");
		if (!ok || cloths != od.meshes.size())
			return;
		std::vector<uint16_t> spheres;
		for (size_t i = 0; i < L.meshes.size(); ++i)
			if (L.cloth[i])
			{
				spheres.push_back(sphere_count(L.meshes[i]));
				set_sphere_count(L.meshes[i], 0);
			}
		fake_model m;
		make_model(L, m);
		g_m = &m;
		set_frame(m, G, od.frames[0]);

		// Native wanted: the first walks name the meshes (the game draws them meanwhile), the cache uploads them
		ch::cache::frame_tick(warp, 1.0);
		cm::set_native(true);
		call_walk(uint32_t(walk_stub));
		check(g_path == 1 && m.cloth_nodes[0][0x3C] == 1, "    data not on the GPU yet: the game's (on to its draw), the cloth not stepped");
		ch::cache::frame_tick(warp, 2.0);
		for (int i = 0; i < 600 && (ch::cache::get_counts().meshes_ready < L.meshes.size() || ch::cache::get_counts().textures_ready < ch::cache::get_counts().textures) &&
			ch::cache::get_counts().meshes_failed == 0; ++i)
			Sleep(50);
		const ch::cache::counts cc = ch::cache::get_counts();
		check(cc.meshes_ready == L.meshes.size() && cc.meshes_failed == 0 && cc.textures_ready == cc.textures, "    on WARP: " + ch::cache::status()[0] +
			(cc.last_failure.empty() ? "" : " / " + cc.last_failure));
		// (the cloth meshes as the cache decoded them: one vertex a work vertex)
		{
			const ch::frame *f = ch::published(hifi::game::charslot::get_counts().seq & 15);
			bool decoded = f != nullptr;
			uint32_t c = 0;
			for (size_t i = 0; decoded && i < L.meshes.size(); ++i)
				if (L.cloth[i])
				{
					ch::cache::mesh_view mv;
					const uint32_t id = f->instance[f->model[0].first_instance + 1 + c].meshes == 1 ? f->mesh[f->instance[f->model[0].first_instance + 1 + c].first_mesh].mesh : ch::cache::kNone;
					decoded = ch::cache::mesh_view_of(id, mv) && mv.ready && mv.vertices == od.meshes[c].n && mv.corners > 0;
					c++;
				}
			check(decoded, "    each cloth in an instance of its own, its mesh on the GPU with one vertex a work vertex");
		}

		// Frame by frame against the oracle (no spheres)
		uint64_t steps_before = cm::get_counts().cloth_steps;
		double worst_p = 0.0, worst_n = 0.0;
		int bad_frames = 0, not_native = 0;
		std::string first_bad;
		for (size_t fr = 0; fr < od.frames.size(); ++fr)
		{
			const oracle_frame &of = od.frames[fr];
			set_frame(m, G, of);
			call_walk(uint32_t(walk_stub));
			std::vector<std::vector<float>> pos, nrm;
			bool native = false;
			uint32_t inst = 0;
			const bool got = frame_cloth(L, pos, nrm, native, inst);
			not_native += !(got && native && g_path == 2);
			bool same = got;
			for (size_t c = 0; same && c < od.meshes.size(); ++c)
			{
				same = pos[c].size() == of.pos[c].size();
				for (size_t k = 0; same && k < pos[c].size(); ++k)
				{
					const double dp = std::fabs(double(pos[c][k]) - of.pos[c][k]), dn = std::fabs(double(nrm[c][k]) - of.nrm[c][k]);
					worst_p = std::max(worst_p, dp);
					worst_n = std::max(worst_n, dn);
					same = dp < 2e-4 && dn < 2e-3;
				}
			}
			if (!same)
			{
				if (bad_frames == 0)
					first_bad = std::to_string(fr);
				bad_frames++;
			}
		}
		uint32_t pose_frames = 0;
		for (const oracle_frame &of : od.frames)
			pose_frames += of.pose;
		const uint64_t steps = cm::get_counts().cloth_steps - steps_before;
		check(not_native == 0, "    every frame HiFi XI's: the early return taken, the model marked native (" + std::to_string(not_native) + " not)");
		check(steps == uint64_t(pose_frames) * od.meshes.size(), "    the game's step run once a pose frame per cloth: " + std::to_string(steps) + " (" +
			std::to_string(pose_frames) + " pose frames of " + std::to_string(od.frames.size()) + ")");
		char line[200];
		std::snprintf(line, sizeof(line), "    every cloth vertex as the oracle's, %zu frames (worst %.2g yalms, normals %.2g)%s", od.frames.size(), worst_p, worst_n,
			bad_frames ? (" FIRST BAD FRAME " + first_bad).c_str() : "");
		check(bad_frames == 0, line);
		check(g_allocs == g_frees && g_allocs > 0, "    the step's scratch taken and given back (" + std::to_string(g_allocs) + ")");

		// With the spheres: the pinned vertices still skinned as the oracle's, the rest finite and near, normals unit
		{
			size_t c = 0;
			for (size_t i = 0; i < L.meshes.size(); ++i)
				if (L.cloth[i])
				{
					set_sphere_count(L.meshes[i], spheres[c]);
					m.cloth_nodes[c][0x3C] = 1;
					c++;
				}
			bool sane = true, pinned = true;
			size_t moved = 0;
			for (size_t fr = 0; fr < od.frames.size(); ++fr)
			{
				const oracle_frame &of = od.frames[fr];
				set_frame(m, G, of);
				call_walk(uint32_t(walk_stub));
				std::vector<std::vector<float>> pos, nrm;
				bool native = false;
				uint32_t inst = 0;
				sane = sane && frame_cloth(L, pos, nrm, native, inst) && native;
				for (size_t k = 0; sane && k < od.meshes.size(); ++k)
				{
					const uintptr_t res = L.meshes[od.meshes[k].ordinal];
					const uint16_t p1 = *reinterpret_cast<const uint16_t *>(res + 0x30 + 0x2A), p2 = *reinterpret_cast<const uint16_t *>(res + 0x30 + 0x2C);
					const uint16_t *counts = reinterpret_cast<const uint16_t *>(uintptr_t(*reinterpret_cast<const uint32_t *>(res + 0x30 + 0x12)));
					const uint32_t n1 = counts[0];
					for (uint32_t v = 0; sane && v < od.meshes[k].n; ++v)
					{
						const bool is_pinned = v < p1 || (v >= n1 && v < n1 + p2);
						float l2 = 0.0f, far2 = 0.0f;
						for (int a = 0; a < 3; ++a)
						{
							const float p = pos[k][3 * v + a], q = of.pos[k][3 * v + a];
							sane = sane && std::isfinite(p) && std::isfinite(nrm[k][3 * v + a]);
							l2 += nrm[k][3 * v + a] * nrm[k][3 * v + a];
							far2 += (p - q) * (p - q);
							if (is_pinned)
								pinned = pinned && std::fabs(p - q) < 1e-4f;
						}
						sane = sane && std::fabs(l2 - 1.0f) < 1e-3f && far2 < 4.0f;
						moved += far2 > 1e-6f;
					}
				}
			}
			check(sane, "    with the spheres: every vertex finite, within 2 yalms of the sphereless one, unit normals");
			check(pinned, "    with the spheres: the pinned vertices skinned as the oracle's");
			std::printf("    (with the spheres %zu vertex-frames moved off the sphereless path)\n", moved);
		}

		// The extra passes, as 0D6D30 sets them for the actor's draw (the banner's texture standing for the overlay's)
		if (passes_test)
		{
			const cm::sites ps = cm::where();
			uintptr_t texture_res = 0;
			for (size_t i = 0; i < L.res.size() && texture_res == 0; ++i)
				if (L.kind[i] == 0x20)
					texture_res = reinterpret_cast<uintptr_t>(L.res[i]);
			// (the handle -> its kind-10 reference -> +0x30 the texture object -> +8 its resource)
			uint8_t *object = alloc_aligned(0x48), *ref = alloc_aligned(0x40), *handle = alloc_aligned(16);
			*reinterpret_cast<uint32_t *>(object + 8) = uint32_t(texture_res);
			*reinterpret_cast<uint32_t *>(ref + 0x30) = uint32_t(reinterpret_cast<uintptr_t>(object));
			*reinterpret_cast<uint32_t *>(handle) = uint32_t(reinterpret_cast<uintptr_t>(ref));
			*reinterpret_cast<uint8_t *>(ps.overlay_flag) = 1;
			*reinterpret_cast<float *>(ps.overlay_u) = 0.25f;
			*reinterpret_cast<float *>(ps.overlay_v) = -0.5f;
			*reinterpret_cast<uint32_t *>(ps.overlay_handle) = uint32_t(reinterpret_cast<uintptr_t>(handle));
			*reinterpret_cast<uint32_t *>(ps.overlay_colour) = 0x80404040u;
			float projection[16] = {};
			for (int k = 0; k < 16; ++k)
				projection[k] = float(k + 1) * 0.125f;
			std::memcpy(reinterpret_cast<void *>(ps.cloak_projection), projection, 64);
			const uint64_t overlays = cm::get_counts().overlays;
			call_walk(uint32_t(walk_stub));
			const ch::frame *f = ch::published(hifi::game::charslot::get_counts().seq & 15);
			const ch::model_entry *me = f != nullptr && f->models == 1 ? &f->model[0] : nullptr;
			check(me != nullptr && (me->passes & ch::kPassOverlay) != 0 && (me->passes & ch::kPassCloak) == 0 && me->overlay_texture != ch::cache::kNone &&
					me->overlay_offset[0] == 0.25f && me->overlay_offset[1] == -0.5f && me->overlay_colour == 0x80404040u && (me->flags & ch::kNative) != 0 && g_path == 2 &&
					cm::get_counts().overlays == overlays + 1,
				"    the overlay recorded: its texture (through the handle, the reference, the object), offsets, factor; the model HiFi XI's");
			check(f != nullptr && f->projection_read && std::memcmp(f->projection, projection, 64) == 0, "    PROJECTION read at the walk's begin");
			// (its factor's alpha 0: adds nothing, not kept)
			*reinterpret_cast<uint32_t *>(ps.overlay_colour) = 0x00404040u;
			call_walk(uint32_t(walk_stub));
			f = ch::published(hifi::game::charslot::get_counts().seq & 15);
			check(f != nullptr && f->models == 1 && f->model[0].passes == 0 && (f->model[0].flags & ch::kNative) != 0, "    an overlay whose factor's alpha is 0: not kept");
			// (its texture not on the GPU yet: a texture the cache hasn't seen)
			*reinterpret_cast<uint32_t *>(ps.overlay_colour) = 0x80404040u;
			uint8_t *other = alloc_aligned(0x30 + 0x200);
			std::memcpy(other, reinterpret_cast<const void *>(texture_res), 0x30 + 0x100);
			*reinterpret_cast<uint32_t *>(other + 0x14) = 0x7EADBEEFu;
			*reinterpret_cast<uint32_t *>(object + 8) = uint32_t(reinterpret_cast<uintptr_t>(other));
			const uint64_t refused = cm::get_counts().passes_refused;
			call_walk(uint32_t(walk_stub));
			f = ch::published(hifi::game::charslot::get_counts().seq & 15);
			check(f != nullptr && f->models == 1 && (f->model[0].flags & ch::kNative) == 0 && g_path == 1 && cm::get_counts().passes_refused == refused + 1,
				"    an overlay whose texture isn't on the GPU yet: the model the game's");
			*reinterpret_cast<uint8_t *>(ps.overlay_flag) = 0;
			// The cloak: on an actor fading (factor alpha 0x40) in the opaque walk: the game's; at alpha 0 (an opaque actor) not kept
			*reinterpret_cast<uint8_t *>(ps.cloak_flag) = 1;
			*reinterpret_cast<float *>(ps.cloak_a) = 0.1f;
			*reinterpret_cast<float *>(ps.cloak_b) = 0.2f;
			*reinterpret_cast<uint32_t *>(ps.cloak_factor) = 0x40808080u;
			call_walk(uint32_t(walk_stub));
			f = ch::published(hifi::game::charslot::get_counts().seq & 15);
			check(f != nullptr && f->models == 1 && (f->model[0].passes & ch::kPassCloak) != 0 && f->model[0].cloak_offset[0] == 0.1f && f->model[0].cloak_offset[1] == 0.2f &&
					f->model[0].cloak_factor == 0x40808080u && (f->model[0].flags & ch::kNative) == 0 && g_path == 1,
				"    the cloak recorded (offsets, factor); in the opaque walk (no scene's copy there): the model the game's");
			*reinterpret_cast<uint32_t *>(ps.cloak_factor) = 0x00808080u;
			call_walk(uint32_t(walk_stub));
			f = ch::published(hifi::game::charslot::get_counts().seq & 15);
			check(f != nullptr && f->models == 1 && f->model[0].passes == 0 && (f->model[0].flags & ch::kNative) != 0 && g_path == 2,
				"    a cloak on an opaque actor (factor alpha 0: adds nothing): not kept, the model HiFi XI's");
			*reinterpret_cast<uint8_t *>(ps.cloak_flag) = 0;
		}

		// A cloth node that isn't the game's cloth node (its vtable's +0x1C elsewhere): the model the game's
		{
			const uint32_t keep = g_cloth_vtable[7];
			g_cloth_vtable[7] = 0x1234;
			const uint64_t refused = cm::get_counts().cloth_refused;
			call_walk(uint32_t(walk_stub));
			std::vector<std::vector<float>> pos, nrm;
			bool native = true;
			uint32_t inst = 0;
			frame_cloth(L, pos, nrm, native, inst);
			check(g_path == 1 && !native && cm::get_counts().cloth_refused == refused + 1, "    a node not the game's cloth node: the model the game's, whole");
			g_cloth_vtable[7] = keep;
		}
		cm::set_native(false);
		ch::cache::frame_tick(warp, 100.0);
		ch::cache::stop();
	}

	void test_dump(const std::string &path, const std::vector<oracle_dat> &oracle, ID3D12Device *warp)
	{
		namespace cm = hifi::game::charmodel;
		namespace cs = hifi::game::charslot;
		std::printf("\n%s\n", path.c_str());
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		check(size != 0, "mapped at its base");
		if (size == 0)
			return;
		hold_round(base, size);
		std::string why;
		const bool in = cs::start(why) && cm::start(why) && cm::in();
		check(in, "in" + (why.empty() ? "" : ": " + why));
		const cm::sites s = cm::where();
		check(s.cloth_step == base + 0x023290 && s.cloth_node == base + 0x025030, "  the cloth's step 023290, its node's draw 025030");
		check(s.cloth_buffer == base + 0x456E40 && s.cloth_flag == base + 0x457460 && s.cloth_palette == base + 0x45EC20,
			"  the globals: the aligned buffer +456E40, the pose frame's flag +457460, the palette pointer +45EC20");
		check(s.overlay_flag == base + 0x4570FC && s.overlay_u == base + 0x457100 && s.overlay_v == base + 0x4570F8 && s.overlay_handle == base + 0x457268 &&
				s.overlay_colour == base + 0x4574B0,
			"  the overlay's globals: flag +4570FC, offsets +457100 / +4570F8, the texture's handle +457268, factor +4574B0");
		check(s.cloak_flag == base + 0x4572B0 && s.cloak_projection == base + 0x457468 && s.cloak_a == base + 0x457458 && s.cloak_b == base + 0x45726C &&
				s.cloak_factor == base + 0x4574B4,
			"  the cloak's: flag +4572B0, PROJECTION +457468, offsets +457458 / +45726C, factor +4574B4");

		// The stand-ins: the environment with its wind ([65CB04]: +0x124 direction, +0x134 strength), the resource
		// manager's table ([47D128]), the step's scratch (023700's calls of 028640 / 028700)
		globals G;
		G.flag = s.cloth_flag;
		G.wind = reinterpret_cast<float *>(alloc_aligned(0x400));
		G.wind[0x124 / 4] = -1.0f;
		G.env = *reinterpret_cast<const uint32_t *>(base + 0x023728);
		const uintptr_t table_global = *reinterpret_cast<const uint32_t *>(base + 0x071065);
		const uintptr_t alloc_call = base + 0x023974, free_call = base + 0x023BB8;
		const bool layout = G.env == base + 0x65CB04 && table_global == base + 0x47D128 && *reinterpret_cast<const uint8_t *>(alloc_call) == 0xE8 &&
			*reinterpret_cast<const uint8_t *>(free_call) == 0xE8 && hifi::game::rel_target(alloc_call) == base + 0x028640 && hifi::game::rel_target(free_call) == base + 0x028700;
		check(layout, "  the step's environment [65CB04], the manager's table [47D128], its scratch calls 028640 / 028700 where expected");
		if (!layout)
			return;
		*reinterpret_cast<uint32_t *>(G.env) = uint32_t(reinterpret_cast<uintptr_t>(G.wind));
		*reinterpret_cast<uint32_t *>(table_global) = uint32_t(reinterpret_cast<uintptr_t>(g_table));
		point_call(alloc_call, reinterpret_cast<const void *>(&fake_alloc));
		point_call(free_call, reinterpret_cast<const void *>(&fake_free));
		g_cloth_vtable[7] = uint32_t(s.cloth_node);

		// The stub's exits here; charslot's walk calls ours
		cm::set_exits(reinterpret_cast<uintptr_t>(&land_go_on), reinterpret_cast<uintptr_t>(&land_leave));
		const cs::sites ss = cs::where();
		*reinterpret_cast<int32_t *>(ss.stub + 0x0D) = int32_t(reinterpret_cast<uintptr_t>(&fake_walk) - (ss.stub + 0x11));
		cs::set_sink(sink);
		cs::set_listener(cm::on_slot);
		cs::set_wanted(true);
		// (the renderer as the zone pass leaves it: a camera, so a model can be HiFi XI's)
		uint8_t *renderer = alloc_aligned(0x40000);
		*reinterpret_cast<float *>(renderer + 0x38F5C) = 1.0f;
		*reinterpret_cast<uint32_t *>(s.renderer) = uint32_t(reinterpret_cast<uintptr_t>(renderer));

		for (size_t k = 0; k < oracle.size(); ++k)
			test_dat(oracle[k], G, warp, ss.stub, k == 0);

		check(cm::status().find("cloth:") != std::string::npos, "  status: " + cm::status());

		// Back
		cs::set_wanted(false);
		cs::set_sink(nullptr);
		cs::set_listener(nullptr);
		*reinterpret_cast<int32_t *>(ss.stub + 0x0D) = int32_t(ss.walk - (ss.stub + 0x11));
		cm::set_exits(0, 0);
		check(cm::stop(why) && cs::stop(why) && !cm::in(), "stop()" + (why.empty() ? "" : ": " + why));
		restore(path, base, alloc_call, 5);
		restore(path, base, free_call, 5);
		restore(path, base, G.env, 4);
		restore(path, base, table_global, 4);
		restore(path, base, G.flag, 4);
		restore(path, base, s.cloth_buffer, 4);
		restore(path, base, s.cloth_palette, 4);
		restore(path, base, s.renderer, 4);
		for (uintptr_t at : { s.overlay_flag, s.overlay_u, s.overlay_v, s.overlay_handle, s.overlay_colour, s.cloak_flag, s.cloak_a, s.cloak_b, s.cloak_factor })
			restore(path, base, at, 4);
		restore(path, base, s.cloak_projection, 64);
		// (the step's own scratch globals, written as the game's step writes them: [456E38] its pointer, [456E3C] a flag)
		restore(path, base, base + 0x456E38, 8);
		{
			std::string where;
			FILE *fl = nullptr;
			if (fopen_s(&fl, path.c_str(), "rb") == 0 && fl != nullptr)
			{
				std::vector<uint8_t> file(size);
				fread(file.data(), 1, size, fl);
				fclose(fl);
				int shown = 0;
				for (size_t i = 0; i < size && shown < 8; ++i)
					if (file[i] != reinterpret_cast<const uint8_t *>(base)[i])
					{
						char at[16];
						std::snprintf(at, sizeof(at), " %06zX", i);
						where += at;
						shown++;
					}
			}
			check(dump::same_as_file(path, base, size), "  the dump again, byte for byte" + (where.empty() ? std::string() : " (differs at" + where + ")"));
		}
		hold_round(0, 0);
		dump::unmap(base);
	}
}

// (HiFi XI's options and World FX's settings: not looked at)
extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

void *g_hold = nullptr;
constexpr uintptr_t kHoldFrom = 0x041C0000, kHoldTo = 0x05200000;
void hold(bool on)
{
	if (on && g_hold == nullptr)
		g_hold = VirtualAlloc(reinterpret_cast<void *>(kHoldFrom), kHoldTo - kHoldFrom, MEM_RESERVE, PAGE_NOACCESS);
	else if (!on && g_hold != nullptr)
	{
		VirtualFree(g_hold, 0, MEM_RELEASE);
		g_hold = nullptr;
	}
}

int main(int argc, char **argv)
{
	hold(true);
	check(g_hold != nullptr, "the dumps' addresses held");
	std::vector<oracle_dat> oracle;
	const bool have = argc > 1 && read_oracle(argv[1], oracle);
	check(have, std::string("the oracle read: ") + (argc > 1 ? argv[1] : "(none given)"));
	if (!have)
		return 1;
	g_table = alloc_aligned(0x40010);
	ID3D12Device *warp = make_warp();
	check(warp != nullptr, "a WARP device");
	if (warp == nullptr)
		return 1;
	for (const char *path : dump::kDumps)
	{
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		{
			check(false, std::string(path) + " is there");
			continue;
		}
		hold(false);
		test_dump(path, oracle, warp);
		hold(true);
	}
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
