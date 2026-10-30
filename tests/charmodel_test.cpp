/*
 * Offline test of the character models recorded at the game's model draw (src/game/charmodel.cpp) and of their data
 * on the GPU (src/chars/cache.cpp, charframe.cpp), on each FFXiMain dump mapped at its own base (game_dump.hpp).
 *
 * A Mithra NPC (ROM2/0/52.DAT) is laid out in memory as the game holds it loaded (notes-characters.md "A live
 * actor", "Textures"): every chunk a resource (+8 the next's handle, +0x14 its own, +0x24 kind | size, +0x28 its
 * folder's handle, +0x30 the data), the Os2 meshes' section offsets made pointers, the first mesh's first texture
 * command drawn (0x8001, the texture object in its name's first dword); a model whose +0x44 instance names the Sk2
 * skeleton, a palette and the meshes. Then, through charslot's walk stub and see-through wrapper (stand-ins for the
 * game's walk and callback), the model draw's site is run as 02B6E0 reaches it (ebp the model, esi the actor, the
 * colour at [esp + 0xA8]):
 *   - the site found at 02B7E3, its early return 02B7B0, the lights table and AMBIENT; only the 5 bytes changed;
 *     refused while /hifi gamedraw chars has the site patched;
 *   - the frame: one opaque model published at the walk's end, one see-through model published before its marker
 *     (the markers B E S in order), the colour, actor, model, lights, ambient, the palette rows (the 4th column left
 *     out), the meshes named, the hide groups; the stub's registers and exits (the game's mov ecx kept);
 *   - with the game's drawing off: the early return taken;
 *   - the cache: every mesh and texture taken once (the texture of the drawn command through its object, the rest by
 *     name in the mesh's folder), decoded with no failures; then on WARP: all on the GPU, a mesh's vertices and a
 *     texture's level 0 read back equal to the decoders' own; the palettes' buffer there; everything let go once not
 *     named for a while;
 *   - stop(): the dump again, byte for byte.
 *
 *   build\charmodel_test.exe
 */

#include "../src/chars/cache.hpp"
#include "../src/chars/charframe.hpp"
#include "../src/chars/os2.hpp"
#include "../src/chars/tex.hpp"
#include "../src/game/charmodel.hpp"
#include "../src/game/charslot.hpp"
#include "../src/game/slotmark.hpp"
#include "game_dump.hpp"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
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

	// ------------------------------------------------------------------------------------------ the Mithra, loaded
	struct loaded
	{
		std::vector<uint8_t *> res;    // each chunk's resource
		std::vector<uint32_t> kind;
		std::vector<uint32_t *> handle;
		uintptr_t sk2 = 0;
		std::vector<uintptr_t> meshes; // the Os2 resources
		std::vector<std::vector<uint8_t>> mesh_data; // (their data as loaded, for the decoder's own reading)
		uint32_t bones = 0;
		uint8_t *object = nullptr;     // the drawn command's texture object
		uintptr_t object_texture = 0;  // its resource
		std::set<uintptr_t> textures;  // every texture resource the meshes should name
	};

	uint8_t *alloc_aligned(size_t n)
	{
		// (top down: the dumps are mapped at their own bases, low in the address space)
		return static_cast<uint8_t *>(VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
	}

	bool load(const char *rel, loaded &L)
	{
		std::string path = std::string("C:\\Games\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI\\") + rel;
		std::ifstream f(path, std::ios::binary);
		std::vector<uint8_t> dat((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if (dat.empty())
			return false;
		std::vector<size_t> folders; // (open folders: their indices)
		size_t off = 0;
		while (off + 16 <= dat.size())
		{
			uint32_t w;
			std::memcpy(&w, dat.data() + off + 4, 4);
			const size_t size = size_t((w >> 7) & 0x7FFFF) * 16;
			if (size < 16 || off + size > dat.size())
				break;
			uint8_t *r = alloc_aligned(0x30 + size);
			std::memcpy(r + 0x20, dat.data() + off, 16);
			std::memcpy(r + 0x30, dat.data() + off + 16, size - 16);
			uint32_t *h = reinterpret_cast<uint32_t *>(alloc_aligned(16));
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
			off += size;
		}
		// The meshes as loaded: section offsets made pointers
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
			const uint32_t size = (((*reinterpret_cast<uint32_t *>(r + 0x24)) >> 7) & 0x7FFFF) * 16 - 16;
			ch::os2::sections s;
			ch::os2::sections_of_chunk(r + 0x30, size, s);
			for (int k = 0; k < 6; ++k)
			{
				const uint32_t p = uint32_t(reinterpret_cast<uintptr_t>(r + 0x30)) + s.at[k];
				std::memcpy(r + 0x30 + 6 + 6 * k, &p, 4);
			}
			L.meshes.push_back(reinterpret_cast<uintptr_t>(r));
		}
		// Each mesh's textures, found here by walking the DAT's chunks: the first texture of that name after the mesh's
		// folder starts, within that folder
		for (size_t mi = 0; mi < L.meshes.size(); ++mi)
		{
			uint8_t *r = reinterpret_cast<uint8_t *>(L.meshes[mi]);
			const uint32_t size = (((*reinterpret_cast<uint32_t *>(r + 0x24)) >> 7) & 0x7FFFF) * 16 - 16;
			ch::os2::sections s;
			ch::os2::sections_of_live(r + 0x30, size, uint32_t(reinterpret_cast<uintptr_t>(r + 0x30)), s);
			std::vector<ch::os2::texture_ref> refs;
			ch::os2::texture_commands(r + 0x30, size, s, refs);
			const uint32_t folder_handle = *reinterpret_cast<uint32_t *>(r + 0x28);
			size_t start = 0;
			for (size_t i = 0; i < L.res.size(); ++i)
				if (uint32_t(reinterpret_cast<uintptr_t>(L.handle[i])) == folder_handle)
					start = i;
			for (size_t ti = 0; ti < refs.size(); ++ti)
			{
				int depth = 0;
				for (size_t i = start + 1; i < L.res.size(); ++i)
				{
					if (L.kind[i] == 1)
						depth++;
					if (L.kind[i] == 0 && --depth < 0)
						break;
					if (L.kind[i] == 0x20 && std::memcmp(L.res[i] + 0x31, refs[ti].name, 16) == 0)
					{
						L.textures.insert(reinterpret_cast<uintptr_t>(L.res[i]));
						if (mi == 0 && ti == 0)
						{
							// (drawn: 0x8001, the object in the name's first dword)
							L.object = alloc_aligned(0x48);
							*reinterpret_cast<uint32_t *>(L.object + 8) = uint32_t(reinterpret_cast<uintptr_t>(L.res[i]));
							L.object_texture = reinterpret_cast<uintptr_t>(L.res[i]);
							r[0x30 + refs[ti].at] |= 1;
							const uint32_t o = uint32_t(reinterpret_cast<uintptr_t>(L.object));
							std::memcpy(r + 0x30 + refs[ti].at + 2, &o, 4);
						}
						break;
					}
				}
			}
			L.mesh_data.emplace_back(r + 0x30, r + 0x30 + size);
		}
		return L.sk2 != 0 && !L.meshes.empty();
	}

	// A model: +0x44 one instance (the skeleton, a palette, every mesh); its palette rows numbered so they can be told
	struct fake_model
	{
		uint8_t *model = nullptr, *node = nullptr;
		std::vector<float> palette;
		std::vector<uint8_t *> mesh_nodes;
	};
	void make_model(const loaded &L, fake_model &m, float tag)
	{
		m.model = alloc_aligned(0x200);
		m.node = alloc_aligned(0x40);
		m.palette.resize(size_t(L.bones) * 16);
		for (size_t i = 0; i < m.palette.size(); ++i)
			m.palette[i] = tag + float(i) * 0.25f;
		*reinterpret_cast<uint32_t *>(m.model + 0x44) = uint32_t(reinterpret_cast<uintptr_t>(m.node));
		*reinterpret_cast<uint32_t *>(m.node + 0xC) = *reinterpret_cast<const uint32_t *>(L.sk2 + 0x14);
		*reinterpret_cast<uint32_t *>(m.node + 0x14) = uint32_t(reinterpret_cast<uintptr_t>(m.palette.data()));
		uint8_t *prev = nullptr;
		for (uintptr_t r : L.meshes)
		{
			uint8_t *n = alloc_aligned(0x20);
			*reinterpret_cast<uint32_t *>(n + 8) = *reinterpret_cast<const uint32_t *>(r + 0x14);
			if (prev == nullptr)
				*reinterpret_cast<uint32_t *>(m.node + 0x20) = uint32_t(reinterpret_cast<uintptr_t>(n));
			else
				*reinterpret_cast<uint32_t *>(prev + 4) = uint32_t(reinterpret_cast<uintptr_t>(n));
			prev = n;
			m.mesh_nodes.push_back(n);
		}
	}

	// ------------------------------------------------------------------------------------------ running the site
	// The site as 02B6E0 reaches it: a frame of 0xB0 bytes with the actor at +0x90 and the colour at +0xA8, ebp the
	// model, esi the actor; the stub's exits come back here
	uint32_t g_stub = 0, g_model = 0, g_actor = 0, g_colour = 0, g_saved_esp = 0;
	uint32_t g_path = 0, g_ecx = 0, g_ebp = 0, g_esi = 0, g_edi = 0, g_ebx = 0;
	__declspec(naked) void land_go_on()
	{
		__asm {
			mov g_ecx, ecx
			mov g_ebp, ebp
			mov g_esi, esi
			mov g_edi, edi
			mov g_ebx, ebx
			mov g_path, 1
			mov esp, g_saved_esp
			ret
		}
	}
	__declspec(naked) void land_leave()
	{
		__asm {
			mov g_ebp, ebp
			mov g_esi, esi
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
			mov edi, 0x33333333
			mov ebx, 0x11111111
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

	// charslot's stand-ins: the walk draws the opaque model, the callback the see-through one
	fake_model *g_a = nullptr, *g_b = nullptr;
	std::string events;
	uint32_t see_through_published_at_marker = 0;
	void sink(uint32_t kind, uint32_t seq)
	{
		events += kind == hifi::slotmark::chars_begin ? 'B' : kind == hifi::slotmark::chars_end ? 'E' : kind == hifi::slotmark::see_through ? 'S' : '?';
		if (kind == hifi::slotmark::see_through)
			if (const ch::frame *f = ch::published(seq & 15))
				see_through_published_at_marker = f->see_through.load();
	}
	uint32_t g_colour_override = 0;
	// A stand-in IDirect3DDevice8 whose GetRenderState (51) answers the fog's states
	uint32_t fake_device_vt[64] = {}, fake_device[1] = {};
	long __stdcall fake_get_state(uintptr_t, uint32_t state, uint32_t *value)
	{
		const float start = 30.0f, end = 90.0f;
		if (state == 34)
			*value = 0xFF405060u;
		else if (state == 36)
			std::memcpy(value, &start, 4);
		else if (state == 37)
			std::memcpy(value, &end, 4);
		else
			*value = 0;
		return 0;
	}
	uint32_t __cdecl fake_walk()
	{
		run_site(hifi::game::charmodel::where().stub, uintptr_t(g_a->model), 0xAC70A000, g_colour_override != 0 ? g_colour_override : 0x80808080u);
		return 0;
	}
	uint32_t __cdecl fake_callback(uint32_t actor)
	{
		run_site(hifi::game::charmodel::where().stub, uintptr_t(g_b->model), actor, 0x40806040u);
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
	__declspec(naked) void call_wrapper(uint32_t, uint32_t)
	{
		__asm {
			pushad
			push dword ptr [esp + 0x28]
			call dword ptr [esp + 0x28]
			add esp, 4
			popad
			ret
		}
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

	// ------------------------------------------------------------------------------------------ WARP readback
	struct gpu
	{
		ID3D12Device *device = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		ID3D12CommandAllocator *allocator = nullptr;
		ID3D12GraphicsCommandList *list = nullptr;
		ID3D12Fence *fence = nullptr;
		uint64_t value = 0;
	};
	bool make_gpu(gpu &g)
	{
		IDXGIFactory4 *factory = nullptr;
		IDXGIAdapter *warp = nullptr;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
			FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device))))
			return false;
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		return SUCCEEDED(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue))) &&
			SUCCEEDED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator))) &&
			SUCCEEDED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator, nullptr, IID_PPV_ARGS(&g.list))) &&
			SUCCEEDED(g.list->Close()) && SUCCEEDED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));
	}
	ID3D12Resource *readback_buffer(gpu &g, uint64_t size)
	{
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC d = {};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = size;
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.SampleDesc.Count = 1;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *r = nullptr;
		g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r));
		return r;
	}
	void run(gpu &g)
	{
		g.list->Close();
		ID3D12CommandList *l[] = { g.list };
		g.queue->ExecuteCommandLists(1, l);
		g.queue->Signal(g.fence, ++g.value);
		HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		g.fence->SetEventOnCompletion(g.value, e);
		WaitForSingleObject(e, 10000);
		CloseHandle(e);
	}
	// A buffer's bytes
	std::vector<uint8_t> read_buffer(gpu &g, ID3D12Resource *b, uint64_t size)
	{
		ID3D12Resource *rb = readback_buffer(g, size);
		g.allocator->Reset();
		g.list->Reset(g.allocator, nullptr);
		g.list->CopyBufferRegion(rb, 0, b, 0, size);
		run(g);
		std::vector<uint8_t> out(static_cast<size_t>(size));
		void *p = nullptr;
		rb->Map(0, nullptr, &p);
		std::memcpy(out.data(), p, size_t(size));
		rb->Unmap(0, nullptr);
		rb->Release();
		return out;
	}
	// A texture's level 0 as RGBA rows
	std::vector<uint8_t> read_texture(gpu &g, ID3D12Resource *t)
	{
		const D3D12_RESOURCE_DESC d = t->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
		UINT rows = 0;
		UINT64 row_bytes = 0, total = 0;
		g.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &row_bytes, &total);
		ID3D12Resource *rb = readback_buffer(g, total);
		g.allocator->Reset();
		g.list->Reset(g.allocator, nullptr);
		D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
		dst.pResource = rb;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint = fp;
		src.pResource = t;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = 0;
		g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		run(g);
		std::vector<uint8_t> out(size_t(row_bytes * rows));
		uint8_t *p = nullptr;
		rb->Map(0, nullptr, reinterpret_cast<void **>(&p));
		for (UINT r = 0; r < rows; ++r)
			std::memcpy(out.data() + size_t(r) * row_bytes, p + fp.Offset + uint64_t(r) * fp.Footprint.RowPitch, size_t(row_bytes));
		rb->Unmap(0, nullptr);
		rb->Release();
		return out;
	}

	// The dumps' other addresses held round the one mapped (our threads and WARP allocate while it's tested)
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

	// ------------------------------------------------------------------------------------------ one dump
	void test_dump(const std::string &path, const loaded &L, gpu *warp)
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

		// Refused while /hifi gamedraw chars has the site patched
		uint8_t *site = reinterpret_cast<uint8_t *>(base + 0x02B7E3);
		const uint8_t kept[2] = { site[0], site[1] };
		site[0] = 0xEB;
		site[1] = 0xCB;
		why.clear();
		const bool refused = !cm::start(why) && !cm::in();
		check(refused, "refused while the site is the off switch's jmp: " + why);
		site[0] = kept[0];
		site[1] = kept[1];

		why.clear();
		const bool in = cs::start(why) && cm::start(why) && cm::in();
		check(in, "in" + (why.empty() ? "" : ": " + why));
		const cm::sites s = cm::where();
		check(s.site == base + 0x02B7E3 && s.early_return == base + 0x02B7B0, "  the site at 02B7E3, the early return 02B7B0");
		check(s.lights == base + 0x4572B8 && s.ambient == base + 0x4571D8, "  the lights table at +4572B8, AMBIENT at +4571D8");
		check(s.tfactor == base + 0x4574AC && s.renderer == base + 0x631F24, "  the colour factor at +4574AC, the renderer at +631F24");
		check(s.device == base + 0x45666C, "  the device wrapper at +45666C");
		check(s.aura_config == base + 0x193850 && s.aura_small == base + 0x47B958 && s.aura_second == base + 0x456D50 && s.aura_step == base + 0x014CF0,
			"  the aura ring's (038E10): the config getter 193850, the 256 x 256 target +47B958, the second target +456D50, the frame step 014CF0");
		check(site[0] == 0xE9 && s.site + 5 + *reinterpret_cast<const int32_t *>(site + 1) == s.stub, "  02B7E3: jmp the stub");
		check(differences(path, base, size) <= 13, "  nothing else changed (" + std::to_string(differences(path, base, size)) + " bytes: the site's 5, charslot's 8)");
		check(std::memcmp(reinterpret_cast<const uint8_t *>(s.stub + 0x15), kept, 2) == 0 && reinterpret_cast<const uint8_t *>(s.stub + 0x15)[0] == 0xB9,
			"  the stub keeps the game's mov ecx, imm32");
		const uint32_t imm = *reinterpret_cast<const uint32_t *>(s.stub + 0x16);

		// The lights and ambient as 0CB4E0 would have left them
		ch::light8 lights[4] = {};
		lights[0].type = 3;
		lights[0].diffuse[0] = 1.48f;
		lights[0].direction[1] = 0.95f;
		lights[1].type = 3;
		lights[1].diffuse[2] = 0.25f;
		std::memcpy(reinterpret_cast<void *>(s.lights), lights, sizeof(lights));
		*reinterpret_cast<uint32_t *>(s.ambient) = 0x80506070u;
		*reinterpret_cast<uint32_t *>(s.tfactor) = 0x80707070u;
		// The renderer as the zone pass leaves it: VIEW x PROJECTION, the fog
		uint8_t *renderer = alloc_aligned(0x40000);
		float view_proj[16];
		for (int k = 0; k < 16; ++k)
			view_proj[k] = float(k) * 0.5f - 3.0f;
		std::memcpy(renderer + 0x38F5C, view_proj, 64);
		*reinterpret_cast<uint32_t *>(renderer + 0x39254) = 0xFF336699u;
		*reinterpret_cast<float *>(renderer + 0x39258) = 400.0f;
		*reinterpret_cast<float *>(renderer + 0x3925C) = 120.0f;
		*reinterpret_cast<uint32_t *>(s.renderer) = uint32_t(reinterpret_cast<uintptr_t>(renderer));
		// The device as 0CB4E0 left it: the actor's fog (its wrapper's +0xC; GetRenderState slot 51)
		uint8_t *wrapper = alloc_aligned(0x40);
		*reinterpret_cast<uint32_t *>(wrapper + 0xC) = uint32_t(reinterpret_cast<uintptr_t>(fake_device));
		fake_device_vt[51] = uint32_t(reinterpret_cast<uintptr_t>(&fake_get_state));
		fake_device[0] = uint32_t(reinterpret_cast<uintptr_t>(fake_device_vt));
		*reinterpret_cast<uint32_t *>(s.device) = uint32_t(reinterpret_cast<uintptr_t>(wrapper));

		// The stubs' exits here; the walk and the callback stand-ins
		cm::set_exits(reinterpret_cast<uintptr_t>(&land_go_on), reinterpret_cast<uintptr_t>(&land_leave));
		const cs::sites ss = cs::where();
		*reinterpret_cast<int32_t *>(ss.stub + 0x0D) = int32_t(reinterpret_cast<uintptr_t>(&fake_walk) - (ss.stub + 0x11));
		*reinterpret_cast<int32_t *>(ss.wrapper + 0x11) = int32_t(reinterpret_cast<uintptr_t>(&fake_callback) - (ss.wrapper + 0x15));
		cs::set_sink(sink);
		cs::set_listener(cm::on_slot);
		cs::set_wanted(true);
		fake_model a, b;
		make_model(L, a, 1000.0f);
		make_model(L, b, 5000.0f);
		g_a = &a;
		g_b = &b;
		events.clear();
		see_through_published_at_marker = 0;
		call_walk(uint32_t(ss.stub));
		check(g_path == 1 && g_ecx == imm && g_ebp == uint32_t(uintptr_t(a.model)) && g_esi == 0xAC70A000u && g_edi == 0x33333333u && g_ebx == 0x11111111u,
			"  the walk's model: on to 02B7E8 with the game's ecx, ebp/esi/edi/ebx kept");
		call_wrapper(uint32_t(ss.wrapper), 0x5EE70001u);
		check(g_path == 1, "  the see-through model: on to 02B7E8");
		check(events == "BES", "  markers: begin, end, then the see-through one from the model draw (" + events + ")");
		check(see_through_published_at_marker == 1, "  the see-through model published before its marker");
		const uint32_t seq = cs::get_counts().seq;
		const ch::frame *f = ch::published(seq & 15);
		check(f != nullptr && f->opaque.load() == 1 && f->see_through.load() == 1 && f->models == 2, "  the frame: 1 opaque, 1 see-through");
		check(f != nullptr && f->camera && std::memcmp(f->view_proj, view_proj, 64) == 0 && std::abs(f->fog[0] - 0.2f) < 1e-3f && std::abs(f->fog[2] - 0.6f) < 1e-3f &&
				f->fog_end == 400.0f && f->fog_start == 120.0f,
			"  the camera and the fog read from the renderer at the walk's begin");
		check(f != nullptr && f->model[0].tfactor == 0x80707070u && (f->model[0].flags & ch::kNative) == 0, "  the colour factor copied; not HiFi XI's (not wanted)");
		check(f != nullptr && f->model[0].fog_read && f->model[0].fog_colour == 0xFF405060u && f->model[0].fog_start == 30.0f && f->model[0].fog_end == 90.0f,
			"  the actor's fog read back from the device (FOGCOLOR, FOGSTART, FOGEND)");
		if (f != nullptr && f->models == 2)
		{
			const ch::model_entry &m0 = f->model[0], &m1 = f->model[1];
			check(m0.actor == 0xAC70A000u && m0.model == uintptr_t(a.model) && m0.colour == 0x80808080u && m0.flags == 0, "  model 0: actor, model, colour, opaque");
			check(m1.actor == 0x5EE70001u && m1.model == uintptr_t(b.model) && m1.colour == 0x40806040u && m1.flags == ch::kFromFlush, "  model 1: from the flush, its colour");
			check(std::memcmp(m0.lights, lights, sizeof(lights)) == 0 && m0.ambient == 0x80506070u, "  the lights table and AMBIENT copied");
			const ch::instance_entry &i0 = f->instance[m0.first_instance], &i1 = f->instance[m1.first_instance];
			check(m0.instances == 1 && i0.bones == L.bones && i0.meshes == L.meshes.size() && m1.instances == 1 && i1.first_bone == L.bones,
				"  one instance each: " + std::to_string(L.bones) + " bones, " + std::to_string(L.meshes.size()) + " meshes");
			bool rows = true;
			for (uint32_t bn = 0; bn < L.bones && rows; ++bn)
				for (int r = 0; r < 4; ++r)
					for (int c = 0; c < 3; ++c)
						rows = rows && f->palette[i1.first_bone + bn].m[r * 3 + c] == b.palette[size_t(bn) * 16 + r * 4 + c];
			check(rows, "  the palette rows copied, the 4th column left out");
			bool named = true;
			for (uint32_t k = 0; k < i0.meshes; ++k)
				named = named && f->mesh[i0.first_mesh + k].mesh != ch::cache::kNone && f->mesh[i0.first_mesh + k].mesh == f->mesh[i1.first_mesh + k].mesh;
			check(named, "  every mesh named, the same ids for both models");
			uint32_t hidden = 0;
			for (const auto &d : L.mesh_data)
				hidden |= ch::os2::hidden_by(d[3]);
			check(m0.hidden == hidden, "  the hide groups from the meshes' byte 3: " + std::to_string(hidden));
		}
		const cm::counts n = cm::get_counts();
		check(n.new_meshes == L.meshes.size() && n.new_textures == L.textures.size() && n.textures_missing == 0 && n.faults == 0,
			"  taken once: " + std::to_string(n.new_meshes) + " meshes, " + std::to_string(n.new_textures) + " textures, none missing");

		// A model drawn outside the walk and the flush: not recorded; the game's drawing off: the early return
		run_site(s.stub, uintptr_t(a.model), 0xAC70A000, 0x80808080u);
		check(g_path == 1 && cm::get_counts().outside == 1 && f != nullptr && f->models == 2, "  drawn outside the world's passes: on to the game's draw, not recorded");
		cm::set_game_draws(false);
		run_site(s.stub, uintptr_t(a.model), 0xAC70A000, 0x80808080u);
		check(g_path == 2 && g_ebp == uint32_t(uintptr_t(a.model)), "  the game's drawing off: the early return");
		cm::set_game_draws(true);
		check(cm::status().find("2 recorded") != std::string::npos, "  status: " + cm::status());

		// Native wanted before the data is on the GPU: the game draws them
		cm::set_native(true);
		call_walk(uint32_t(ss.stub));
		check(g_path == 1 && cm::get_counts().not_ready >= 1 && cm::get_counts().native == 0, "  native wanted, data not on the GPU yet: the game draws it");
		cm::set_native(false);

		// The cache: decoded with no device, then on WARP
		ch::cache::frame_tick(nullptr, 1.0);
		check(ch::cache::settle(20000), "  the cache settled");
		ch::cache::counts c = ch::cache::get_counts();
		check(c.meshes == L.meshes.size() && c.textures == L.textures.size() && c.meshes_failed == 0 && c.textures_failed == 0,
			"  decoded: " + ch::cache::status()[0] + (c.last_failure.empty() ? "" : " / " + c.last_failure));
		if (warp != nullptr)
		{
			ch::cache::frame_tick(warp->device, 2.0);
			for (int i = 0; i < 600 && ch::cache::get_counts().meshes_ready + ch::cache::get_counts().textures_ready < L.meshes.size() + L.textures.size() &&
				ch::cache::get_counts().meshes_failed == 0; ++i)
				Sleep(50);
			check(ch::cache::settle(60000), "  uploaded on WARP" + (ch::cache::status().size() > 1 ? ": " + ch::cache::status()[1] : std::string()));
			c = ch::cache::get_counts();
			check(c.meshes_ready == c.meshes && c.textures_ready == c.textures && c.gpu_bytes > 0, "  all on the GPU: " + ch::cache::status()[0]);
			check(ch::cache::palettes(seq) != 0 && ch::cache::heap() != nullptr, "  the palettes' buffer and the descriptor heap made");
			// A mesh's vertices and corners, and the drawn command's texture, read back
			const uint32_t mid = f != nullptr ? f->mesh[f->instance[f->model[0].first_instance].first_mesh].mesh : 0;
			ch::cache::mesh_view mv;
			const bool have = ch::cache::mesh_view_of(mid, mv);
			ID3D12Resource *vb = nullptr, *cb = nullptr, *tx = nullptr;
			ch::cache::resources_of(mid, have && !mv.textures.empty() ? mv.textures[0] : ch::cache::kNone, vb, cb, tx);
			ch::os2::mesh dm;
			ch::os2::sections ls;
			std::string w2;
			ch::os2::sections_of_live(L.mesh_data[0].data(), L.mesh_data[0].size(), uint32_t(L.meshes[0] + 0x30), ls);
			ch::os2::decode(L.mesh_data[0].data(), L.mesh_data[0].size(), ls, dm, w2);
			bool same = have && vb != nullptr && cb != nullptr && mv.vertices == dm.vertices.size() && mv.corners == dm.corners.size() && mv.groups.size() == dm.groups.size();
			if (same)
			{
				const std::vector<uint8_t> vbytes = read_buffer(*warp, vb, uint64_t(mv.vertices) * sizeof(ch::os2::vertex));
				const std::vector<uint8_t> cbytes = read_buffer(*warp, cb, uint64_t(mv.corners) * sizeof(ch::os2::corner));
				same = std::memcmp(vbytes.data(), dm.vertices.data(), vbytes.size()) == 0 && std::memcmp(cbytes.data(), dm.corners.data(), cbytes.size()) == 0;
			}
			check(same, "  mesh 0's vertices (" + std::to_string(mv.vertices) + ") and corners (" + std::to_string(mv.corners) + ") read back as decoded");
			bool tsame = tx != nullptr;
			if (tsame)
			{
				const uint32_t hsize = (((*reinterpret_cast<const uint32_t *>(L.object_texture + 0x24)) >> 7) & 0x7FFFF) * 16 - 16;
				ch::tex::texture t;
				std::string w3;
				ch::tex::decode(reinterpret_cast<const uint8_t *>(L.object_texture + 0x30), hsize, t, w3);
				const std::vector<uint8_t> px = read_texture(*warp, tx);
				tsame = px == t.levels[0].rgba && tx->GetDesc().MipLevels == ch::tex::mip_count(t.w, t.h);
			}
			check(tsame, "  the drawn command's texture (through its object) read back as decoded, with its mips");
			D3D12_GPU_DESCRIPTOR_HANDLE th;
			check(have && mv.textures.size() > 0 && ch::cache::texture_view_of(mv.textures[0], th), "  its texture's view");

			// HiFi XI's draw wanted, every mesh on the GPU: both models left to it (the game's draw left out)
			cm::set_native(true);
			events.clear();
			call_walk(uint32_t(ss.stub));
			const bool opaque_left = g_path == 2;
			call_wrapper(uint32_t(ss.wrapper), 0x5EE70001u);
			const bool flush_left = g_path == 2;
			const ch::frame *f2 = ch::published(cs::get_counts().seq & 15);
			check(f2 != nullptr && f2->models == 2 && (f2->model[0].flags & ch::kNative) != 0 && (f2->model[1].flags & ch::kNative) != 0 && opaque_left && flush_left &&
					events == "BES",
				"  native wanted, data on the GPU: both marked HiFi XI's, the game's draw left (" + events + ")");
			// A model with colour alpha 0: the game's (it skips it itself)
			g_colour_override = 0x00808080u;
			call_walk(uint32_t(ss.stub));
			g_colour_override = 0;
			const ch::frame *f3 = ch::published(cs::get_counts().seq & 15);
			check(f3 != nullptr && (f3->model[0].flags & ch::kNative) == 0 && g_path == 1, "  colour alpha 0: left to the game");
			cm::set_native(false);
		}
		// Let go once not named for a while
		ch::cache::frame_tick(warp != nullptr ? warp->device : nullptr, 100.0);
		c = ch::cache::get_counts();
		check(c.meshes == 0 && c.textures == 0 && c.forgotten == L.meshes.size() + L.textures.size(), "  let go after " + std::to_string(int(ch::cache::kForgetSeconds)) + " s unnamed (" + ch::cache::status()[0] + ")");
		ch::cache::stop();

		// Back
		cs::set_wanted(false);
		cs::set_sink(nullptr);
		cs::set_listener(nullptr);
		*reinterpret_cast<int32_t *>(ss.stub + 0x0D) = int32_t(ss.walk - (ss.stub + 0x11));
		*reinterpret_cast<int32_t *>(ss.wrapper + 0x11) = int32_t(ss.callback - (ss.wrapper + 0x15));
		std::memset(reinterpret_cast<void *>(s.lights), 0, sizeof(lights));
		*reinterpret_cast<uint32_t *>(s.ambient) = 0;
		const uint8_t *orig_lights = nullptr;
		(void)orig_lights;
		cm::set_exits(0, 0);
		check(cm::stop(why) && cs::stop(why) && !cm::in(), "stop()" + (why.empty() ? "" : ": " + why));
		// (the lights and ambient written above were the dump's own bytes before: put back from the file)
		{
			FILE *fl = nullptr;
			if (fopen_s(&fl, path.c_str(), "rb") == 0 && fl != nullptr)
			{
				fseek(fl, long(s.lights - base), SEEK_SET);
				fread(reinterpret_cast<void *>(s.lights), 1, sizeof(lights), fl);
				fseek(fl, long(s.ambient - base), SEEK_SET);
				fread(reinterpret_cast<void *>(s.ambient), 1, 4, fl);
				fseek(fl, long(s.tfactor - base), SEEK_SET);
				fread(reinterpret_cast<void *>(s.tfactor), 1, 4, fl);
				fseek(fl, long(s.renderer - base), SEEK_SET);
				fread(reinterpret_cast<void *>(s.renderer), 1, 4, fl);
				fseek(fl, long(s.device - base), SEEK_SET);
				fread(reinterpret_cast<void *>(s.device), 1, 4, fl);
				fclose(fl);
			}
		}
		check(dump::same_as_file(path, base, size), "  the dump again, byte for byte");
		hold_round(0, 0);
		dump::unmap(base);
	}
}

// (HiFi XI's options and World FX's settings: not looked at)
extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

// The dumps' addresses held while they aren't mapped (the heap, WARP and our own threads allocate meanwhile)
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

int main()
{
	hold(true);
	check(g_hold != nullptr, "the dumps' addresses held");
	loaded L;
	const bool loaded_ok = load("ROM2\\0\\52.DAT", L);
	check(loaded_ok, "the Mithra laid out as loaded: " + std::to_string(L.res.size()) + " resources, " + std::to_string(L.meshes.size()) + " meshes, " +
		std::to_string(L.bones) + " bones, " + std::to_string(L.textures.size()) + " textures named");
	gpu warp;
	const bool have_warp = make_gpu(warp);
	check(have_warp, "a WARP device");
	std::vector<uint8_t> empty(0x10000, 0);
	hifi::game::set_image(reinterpret_cast<uintptr_t>(empty.data()), empty.size());
	std::string why;
	const bool refused = !hifi::game::charmodel::start(why) && !why.empty();
	check(refused, "an image without the game's code: refused, " + why);
	for (const char *path : dump::kDumps)
	{
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		{
			check(false, std::string(path) + " is there");
			continue;
		}
		hold(false);
		test_dump(path, L, have_warp ? &warp : nullptr);
		hold(true);
	}
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
