/*
 * Offline test of the model actors handed to the zone renderer (src/game/modelactor.cpp, src/zone/actordata, the actor
 * frames in src/zone/frame, zone_renderer's object-list draw), on both FFXiMain dumps (game_dump.hpp) and WARP:
 *   - start(): 0C4CC8's call goes to on_draw, the game's draw it went to is 1844C0, the Mog House object and the light
 *     table found where 1844C0 and 178760 read them; stop(): the dump again, byte for byte;
 *   - a model DAT (the first of the model actors' file ids, 0x791C + n, that has a mesh with all its textures) read by
 *     zonemesh::load_file and put on the GPU;
 *   - on_draw, with a made-up renderer, actor, resource chain and mesh: left to the game (the stand-in draw called, with
 *     the block and the alpha flag) while native drawing is off, outside the walk, while the DAT's data isn't up, and for
 *     a mesh the DAT hasn't; recorded otherwise (the game's draw not called): the matrix as the zone's objects have it,
 *     the model, the alpha test, its DAT's index (one for two actors of one DAT), the camera and the default
 *     environment's light from the renderer; in a Mog House the light table's l_01 as lamp 0 of each;
 *   - the walk's end publishes the frame under its seq;
 *   - the frame drawn by the zone renderer's object-list record on WARP with the debug layer: the model's pixels in the
 *     target, nothing said by the debug layer.
 */

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include "../src/bake/zonemesh.hpp"
#include "../src/game/modelactor.hpp"
#include "../src/zone/actordata.hpp"
#include "../src/zone/draw.hpp"
#include "../src/zone/frame.hpp"
#include "../src/zone/zone_gpu.hpp"
#include "game_dump.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	namespace zm = hifi::bake::zonemesh;
	namespace ma = hifi::game::modelactor;
	namespace zn = hifi::zone;

	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	template <typename T>
	void put(std::vector<uint8_t> &b, size_t at, T v)
	{
		std::memcpy(b.data() + at, &v, sizeof(T));
	}

	// The game's draw's stand-in: what it was called with
	struct drawn
	{
		int calls = 0;
		uintptr_t renderer = 0, block = 0;
		uint32_t alpha = 0;
	};
	drawn g_drawn;
	void __fastcall game_draw(uintptr_t renderer, uintptr_t, uintptr_t block, uint32_t alpha)
	{
		g_drawn.calls++;
		g_drawn.renderer = renderer;
		g_drawn.block = block;
		g_drawn.alpha = alpha;
	}

	// The DAT's data, as actordata would hand it over
	uint32_t g_file = 0;
	zn::gpu_zone *g_data = nullptr;
	zn::gpu_zone *find_data(uint32_t file)
	{
		return file == g_file ? g_data : nullptr;
	}
	zn::gpu_zone *find_none(uint32_t)
	{
		return nullptr;
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

	bool close_to(float a, float b)
	{
		return std::fabs(a - b) <= 1e-4f * std::max(1.0f, std::fabs(b));
	}
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0); // (a test that dies says how far it got)
	std::string why;
	// The sites on both dumps; the call and the dump put back
	for (const char *path : dump::kDumps)
	{
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		if (size == 0)
		{
			check(false, std::string("mapped ") + path);
			continue;
		}
		const std::string name = std::string(path).substr(std::string(path).rfind('\\') + 1);
		const bool started = ma::start(why);
		check(started && ma::in(), name + ": start()" + (why.empty() ? "" : ": " + why));
		const ma::sites s = ma::where();
		check(s.call == base + 0x0C4CC8, name + ": the call at 0C4CC8");
		check(s.original == base + 0x1844C0, name + ": it went to 1844C0");
		check(s.mog_house == base + 0x6631D4 && s.lights == base + 0x632040, name + ": the Mog House object (VA +6631D4) and the light table (+632040)");
		uint8_t call[5] = {};
		std::memcpy(call, reinterpret_cast<const void *>(s.call), 5);
		int32_t rel = 0;
		std::memcpy(&rel, call + 1, 4);
		check(call[0] == 0xE8 && s.call + 5 + intptr_t(rel) == reinterpret_cast<uintptr_t>(&ma::on_draw), name + ": the call now goes to on_draw");
		check(ma::stop(why) && !ma::in() && differing(path, base, size) == 0, name + ": stop(): the dump again, byte for byte" + why);
		dump::unmap(base);
	}

	// (modelactor_test scan [count]: the model actors' DATs from 0x791C on, how many have meshes and how many of those have
	// all their textures in the DAT: the ones HiFi XI can draw)
	if (argc > 1 && std::strcmp(argv[1], "scan") == 0)
	{
		uintptr_t b = 0;
		if (dump::map(dump::kDumps[0], b) == 0)
			return 1;
		const uint32_t n = argc > 2 ? uint32_t(std::atoi(argv[2])) : 3000;
		uint32_t files = 0, meshes = 0, whole = 0;
		for (uint32_t id = 0x791C; id < 0x791C + n; ++id)
		{
			zm::zone z;
			hifi::bake::result r;
			if (!zm::load_file(id, z, r))
				continue;
			files++;
			uint32_t ok = 0;
			for (const zm::model &m : z.models)
				ok += m.missing == 0 ? 1 : 0;
			meshes += uint32_t(z.models.size());
			whole += ok;
			std::printf("%u (model %u): %zu meshes, %u with all their textures; %zu textures\n", id, id - 0x791C, z.models.size(), ok, z.textures.size());
		}
		std::printf("%u DATs with meshes, %u meshes, %u with all their textures\n", files, meshes, whole);
		return 0;
	}

	// The first dump for the rest (the key tables, the sites)
	uintptr_t base = 0;
	const size_t size = dump::map(dump::kDumps[0], base);
	if (size == 0)
	{
		check(false, "the FFXiMain dump mapped");
		return 1;
	}

	// A model DAT: the first of the model actors' file ids with a mesh whose textures are all in it
	zm::zone z;
	hifi::bake::result r;
	uint32_t model = zm::kNone, tried = 0;
	for (uint32_t id = 0x791C + 1; id < 0x791C + 4000 && model == zm::kNone; ++id)
	{
		if (!zm::load_file(id, z, r))
			continue;
		tried++;
		for (uint32_t m = 0; m < z.models.size(); ++m)
			if (z.models[m].missing == 0 && z.models[m].pieces != 0 && z.models[m].kind == 1)
			{
				model = m;
				g_file = id;
				break;
			}
	}
	check(model != zm::kNone && z.map == g_file, "a model actor's DAT read by load_file: file " + std::to_string(g_file) + " (" + std::to_string(tried) + " read), " +
		std::to_string(z.models.size()) + " models, " + std::to_string(z.textures.size()) + " textures, no placements, no normal maps");
	if (model == zm::kNone)
		return 1;
	check(z.placements.empty() && z.normal_maps.empty(), "no placements and no normal maps from a model DAT");
	char model_name[16];
	std::memcpy(model_name, z.models[model].name, 16);
	const zm::model mo = z.models[model];

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
	ID3D12CommandQueue *copy = nullptr, *direct = nullptr;
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_COPY };
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&copy));
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&direct));
	zn::gpu_zone gz;
	check(gz.upload(device, copy, std::move(z), why) && gz.wait(10000), "the model DAT on the GPU" + why);
	g_data = &gz;

	// actordata: asked for, read on its thread, uploaded at a later present
	{
		check(zn::actordata::find(g_file) == nullptr, "actordata: nothing while not wanted");
		zn::actordata::frame(device, true);
		check(zn::actordata::find(g_file) == nullptr, "actordata: asked for the first time: not there yet");
		zn::gpu_zone *got = nullptr;
		for (int i = 0; i < 2000 && got == nullptr; ++i)
		{
			zn::actordata::frame(device, true);
			got = zn::actordata::find(g_file);
			if (got == nullptr)
				Sleep(2);
		}
		check(got != nullptr && got->data().map == g_file && got->data().find(model_name) == model, "actordata: read, uploaded and handed over a few presents later");
		zn::actordata::stop();
		check(zn::actordata::find(g_file) == nullptr, "actordata: nothing after stop()");
	}

	const bool started = ma::start(why);
	check(started, "start() for the recording" + (why.empty() ? "" : ": " + why));
	ma::set_draw(reinterpret_cast<uintptr_t>(&game_draw));
	ma::set_data(find_data);

	// A renderer (the camera, the default environment's light), an actor with its block, its resource up to a DAT's root,
	// its mesh
	std::vector<uint8_t> ren(0x40000), actor(0x800), resource(0x40), root(0x40), mesh(0x80), other(0x80);
	uint32_t handle = uint32_t(uintptr_t(root.data()));
	for (int i = 0; i < 16; ++i)
		put<float>(ren, 0x38F5C + 4 * i, float(i + 1));
	put<float>(ren, 0x390F0, 10.0f);
	put<float>(ren, 0x390F4, 20.0f);
	put<float>(ren, 0x390F8, 30.0f);
	put<float>(ren, 0x391C0, 0.0f); // (the sun's Direction: straight down)
	put<float>(ren, 0x391C4, -2.0f);
	put<float>(ren, 0x391C8, 0.0f);
	put<float>(ren, 0x39184, 0.75f);
	put<float>(ren, 0x39188, 0.5f);
	put<float>(ren, 0x3918C, 0.25f);
	put<uint32_t>(ren, 0x39250, 0x00402010);
	put<uint32_t>(ren, 0x39254, 0x00808080);
	put<float>(ren, 0x39258, 300.0f);
	put<float>(ren, 0x3925C, 50.0f);
	put<float>(ren, 0x3954C, 1.0f);
	put<uint32_t>(resource, 0x28, uint32_t(uintptr_t(&handle)));
	put<uint32_t>(root, 0x28, 0);
	put<uint32_t>(root, 0x34, g_file);
	std::memcpy(mesh.data() + 8, model_name, 16);
	std::memcpy(other.data() + 8, "model   nosuchmo", 16);
	put<uint32_t>(actor, 0x5C4, uint32_t(uintptr_t(resource.data())));
	const uintptr_t block = uintptr_t(actor.data()) + 0x5C8;
	put<uint32_t>(actor, 0x5C8 + 0x18, uint32_t(uintptr_t(mesh.data())));
	// (the world matrix, row vectors: a turn about y and a place)
	const float c = std::cos(0.5f), sn = std::sin(0.5f);
	const float world[16] = { c, 0, -sn, 0, 0, 1, 0, 0, sn, 0, c, 0, 100.0f, -5.0f, 200.0f, 1 };
	std::memcpy(actor.data() + 0x5C8 + 0x40, world, 64);
	const uintptr_t R = uintptr_t(ren.data());
	const zn::actor_frame *pub = nullptr;

	auto draws_left = [&](const char *what, uint64_t ma::counts::*counter) {
		const int before = g_drawn.calls;
		const uint64_t was = counter != nullptr ? ma::get_counts().*counter : 0;
		g_drawn = drawn { before };
		ma::on_draw(R, 0, block, 1);
		check(g_drawn.calls == before + 1 && g_drawn.renderer == R && g_drawn.block == block && g_drawn.alpha == 1 &&
			(counter == nullptr || ma::get_counts().*counter == was + 1), std::string("left to the game (its draw called with the block and the flag): ") + what);
	};

	ma::set_native(true);
	draws_left("outside the walk", &ma::counts::game);
	ma::on_slot(2, 5, 0);
	ma::set_native(false);
	draws_left("native drawing off", &ma::counts::game);
	ma::set_native(true);
	ma::set_data(find_none);
	draws_left("the DAT's data not up", &ma::counts::not_ready);
	ma::set_data(find_data);
	put<uint32_t>(actor, 0x5C8 + 0x18, uint32_t(uintptr_t(other.data())));
	draws_left("a mesh the DAT hasn't", &ma::counts::unknown);
	put<uint32_t>(actor, 0x5C8 + 0x18, uint32_t(uintptr_t(mesh.data())));

	// Recorded: twice, one with the alpha test
	{
		const int before = g_drawn.calls;
		ma::on_draw(R, 0, block, 1);
		ma::on_draw(R, 0, block, 0);
		check(g_drawn.calls == before && ma::get_counts().drawn == 2, "recorded: the game's draw not called");
		ma::on_slot(3, 5, 0);
		pub = zn::actor_published(5);
		check(pub != nullptr && pub->count == 2 && ma::get_counts().last == 2, "the walk's end: published under its seq, two objects");
		if (pub != nullptr && pub->count == 2)
		{
			const zn::frame_object &o = pub->objects[0];
			bool m_ok = true;
			for (int k = 0; k < 3; ++k)
				for (int j = 0; j < 4; ++j)
					m_ok = m_ok && o.world[k * 4 + j] == world[j * 4 + k];
			check(m_ok && o.world[3] == 100.0f && o.world[7] == -5.0f && o.world[11] == 200.0f, "the matrix as the zone's objects have it (column k of the row-vector matrix)");
			check(o.model == model && o.zone == 0 && pub->data_count == 1 && pub->data[0] == &gz && pub->objects[1].zone == 0, "the model, its DAT's data (one for both)");
			check(o.flags == (zm::kAlphaTested | zn::kDoor) && pub->objects[1].flags == zn::kDoor, "the alpha test from +0x770, fixed-function with Z writes (kDoor)");
			check(pub->lamp_count == 0 && o.lamps[0] == 0xFFFFFFFFu, "no Mog House: no lamp");
			check(pub->view_proj[0] == 1.0f && pub->view_proj[15] == 16.0f && pub->camera[0] == 10.0f && pub->camera[2] == 30.0f, "the camera from the renderer");
			const zn::frame_env &e = pub->env;
			check(close_to(e.sun_dir[1], 1.0f) && close_to(e.sun[0], 0.75f) && close_to(e.ambient[0], 64.0f / 255.0f) && close_to(e.ambient[2], 16.0f / 255.0f) &&
				close_to(e.fog[1], 128.0f / 255.0f) && e.fog_end == 300.0f && e.fog_start == 50.0f && e.fog_scale == 1.0f, "the default environment's sun, ambient and fog");
		}
	}

	// A Mog House: the light table's l_01, lamp 0 of each
	{
		const ma::sites s = ma::where();
		uint32_t was_mh = 0;
		std::memcpy(&was_mh, reinterpret_cast<const void *>(s.mog_house), 4);
		const uintptr_t entry = s.lights + 3 * 0x70;
		uint8_t was_entry[0x70];
		std::memcpy(was_entry, reinterpret_cast<const void *>(entry), 0x70);
		const uint32_t one = 1, name = 0x31305F6C;
		std::memcpy(reinterpret_cast<void *>(s.mog_house), &one, 4);
		uint8_t e[0x70] = {};
		std::memcpy(e, &name, 4);
		const float diffuse[3] = { 0.9f, 0.8f, 0.6f }, pos[3] = { 1.0f, 2.0f, 3.0f }, range = 12.0f, att2 = 0.25f;
		std::memcpy(e + 8 + 4, diffuse, 12);
		std::memcpy(e + 8 + 0x34, pos, 12);
		std::memcpy(e + 8 + 0x4C, &range, 4);
		std::memcpy(e + 8 + 0x5C, &att2, 4);
		std::memcpy(reinterpret_cast<void *>(entry), e, 0x70);
		ma::on_slot(2, 6, 0);
		ma::on_draw(R, 0, block, 0);
		ma::on_slot(3, 6, 0);
		const zn::actor_frame *mh = zn::actor_published(6);
		check(mh != nullptr && mh->count == 1 && mh->lamp_count == 1 && mh->objects[0].lamps[0] == 0 && mh->lamps[0].pos[1] == 2.0f &&
			mh->lamps[0].range == 12.0f && mh->lamps[0].attenuation2 == 0.25f && mh->lamps[0].colour[0] == 0.9f, "a Mog House: l_01 of the light table, lamp 0 of each object");
		check(zn::actor_published(5) == pub, "the earlier walk's frame still published under its own seq");
		std::memcpy(reinterpret_cast<void *>(s.mog_house), &was_mh, 4);
		std::memcpy(reinterpret_cast<void *>(entry), was_entry, 0x70);
	}

	// The frame drawn by the zone renderer, its camera looking at the model's box (an orthographic one: the box into
	// the middle of the target)
	{
		const UINT W = 256, H = 256;
		zn::actor_frame &f = zn::actor_slot(7);
		f.count = 1;
		f.data_count = 1;
		f.data[0] = &gz;
		f.lamp_count = 0;
		zn::frame_object &o = f.objects[0];
		o = zn::frame_object();
		const float ident[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
		std::memcpy(o.world, ident, sizeof(ident));
		o.model = model;
		o.flags = zn::kDoor;
		float centre[3], span = 1e-3f;
		for (int k = 0; k < 3; ++k)
		{
			centre[k] = 0.5f * (mo.lo[k] + mo.hi[k]);
			span = std::max(span, mo.hi[k] - mo.lo[k]);
		}
		const float s = 1.6f / span;
		const float vp[16] = { s, 0, 0, 0, 0, s, 0, 0, 0, 0, 0.5f * s, 0, -centre[0] * s, -centre[1] * s, 0.5f - 0.5f * s * centre[2], 1 };
		std::memcpy(f.view_proj, vp, 64);
		f.camera[0] = centre[0];
		f.camera[1] = centre[1];
		f.camera[2] = centre[2] - span;
		f.env = zn::frame_env();
		const float up[3] = { 0, 1, 0 }, white[3] = { 1, 1, 1 }, half[3] = { 0.5f, 0.5f, 0.5f };
		std::memcpy(f.env.sun_dir, up, 12);
		std::memcpy(f.env.sun, half, 12);
		std::memcpy(f.env.ambient, white, 12);
		f.env.fog_start = 1e6f;
		f.env.fog_end = 2e6f;
		zn::actor_publish(f, 7);

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
		td.Format = DXGI_FORMAT_D32_FLOAT;
		td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		D3D12_CLEAR_VALUE cv = {};
		cv.Format = DXGI_FORMAT_D32_FLOAT;
		cv.DepthStencil.Depth = 1.0f;
		device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&ds));
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
		device->CreateDepthStencilView(ds, nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());

		zn::zone_renderer renderer;
		check(renderer.init(device, why), "the zone renderer made" + why);
		ID3D12CommandAllocator *alloc = nullptr;
		ID3D12GraphicsCommandList *list = nullptr;
		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
		device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
		const float black[4] = { 0, 0, 0, 0 };
		list->ClearRenderTargetView(rtv_heap->GetCPUDescriptorHandleForHeapStart(), black, 0, nullptr);
		list->ClearDepthStencilView(dsv_heap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		zn::draw_target t;
		t.rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
		t.dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
		t.rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
		t.dsv_format = DXGI_FORMAT_D32_FLOAT;
		t.width = float(W);
		t.height = float(H);
		t.offset = 0.0f;
		const zn::actor_frame *p = zn::actor_published(7);
		zn::draw_pass pass;
		pass.view_proj = p->view_proj;
		pass.camera = p->camera;
		pass.envs = &p->env;
		pass.env_count = 1;
		pass.zones = p->data;
		pass.zone_count = p->data_count;
		zn::draw_options opt;
		// (both windings: which way round the box lands depends on the model)
		zn::frame_object both[2] = { p->objects[0], p->objects[0] };
		both[1].flags |= zm::kMirrored;
		const zn::draw_stats st = renderer.record(list, t, pass, both, 2, opt, 1);
		check(st.objects == 2 && st.draws == 2 * mo.pieces && st.skipped == 0, "recorded: both objects, each piece once (" + std::to_string(st.draws) + " draws, " +
			std::to_string(st.triangles) + " triangles)");
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = rt;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		list->ResourceBarrier(1, &b);
		D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
		src.pResource = rt;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.pResource = readback;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, W * 4 };
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		list->Close();
		ID3D12CommandList *lists[] = { list };
		direct->ExecuteCommandLists(1, lists);
		ID3D12Fence *fence = nullptr;
		device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
		direct->Signal(fence, 1);
		HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		fence->SetEventOnCompletion(1, ev);
		WaitForSingleObject(ev, 30000);
		uint8_t *px = nullptr;
		const D3D12_RANGE all = { 0, size_t(W) * H * 4 };
		readback->Map(0, &all, reinterpret_cast<void **>(&px));
		uint32_t lit = 0;
		for (size_t i = 0; px != nullptr && i < size_t(W) * H; ++i)
			lit += (px[i * 4] | px[i * 4 + 1] | px[i * 4 + 2]) != 0 ? 1 : 0;
		readback->Unmap(0, nullptr);
		check(lit > 200, "the model in the target: " + std::to_string(lit) + " pixels");
		UINT64 messages = info != nullptr ? info->GetNumStoredMessages() : 0;
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
		check(bad == 0, "the debug layer said nothing");
		renderer.release();
		CloseHandle(ev);
		fence->Release();
		list->Release();
		alloc->Release();
		rtv_heap->Release();
		dsv_heap->Release();
		readback->Release();
		ds->Release();
		rt->Release();
	}

	check(ma::stop(why) && differing(dump::kDumps[0], base, size) == 0, "stop(): the dump again, byte for byte" + why);
	gz.release();
	dump::unmap(base);
	std::printf(failures == 0 ? "\nall passed (%d)\n" : "\n%d failed\n", failures == 0 ? passes : failures);
	return failures == 0 ? 0 : 1;
}
