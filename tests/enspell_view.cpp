/*
 * The en-spell effects offline (src/chars/chardraw.cpp record_enspells), through the native character renderer's own
 * path as the game feeds it: DATs into the characters' cache, a frame published (charframe.hpp), drawn by charinject at
 * the characters' end marker into a world target and depth made as dgVoodoo makes them. Frame after frame with the
 * effects' clock held at each frame's time (inject::set_clock), each picture written out as a BMP, for a look (and a
 * GIF: tools/enspell_gif.py).
 *
 *   build\enspell_view.exe <poses.bin> <out folder> <frames> <dat> [dat...]      (built by build.cmd)
 *
 * poses.bin: u32 poses, u32 bones, then each pose's palette (16 float32 a bone, chars.py's: row vectors, translation
 * in the last row): tests/enspell_poses.py writes one. The pose for frame i is i % poses; time = i / 30 s.
 * ENSPELL=n the element (charframe.hpp kEn*: 1 fire (the default) .. 8 dark; 0 none); ENSPELL_W / ENSPELL_H the picture's size (960 x 720); ENSPELL_YAW, ENSPELL_DIST,
 * ENSPELL_LIFT the camera round the blade (degrees, yalms, yalms up); ENSPELL_WARP=1 on WARP instead of the GPU;
 * ENSPELL_DEBUG=1 with the debug layer (its errors and warnings counted at the end: exit code 3 when any).
 */

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include "../src/chars/cache.hpp"
#include "../src/chars/charframe.hpp"
#include "../src/chars/charinject.hpp"
#include "../src/chars/os2.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{
	namespace ch = hifi::chars;

	struct dat_chunk
	{
		char name[4];
		uint32_t kind;
		size_t folder;
		std::vector<uint8_t> data;
	};
	std::vector<dat_chunk> read_dat(const std::string &rel)
	{
		std::string path = "C:\\Games\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI\\" + rel;
		std::replace(path.begin(), path.end(), '/', '\\');
		std::ifstream f(path, std::ios::binary);
		std::vector<uint8_t> dat((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		std::vector<dat_chunk> out;
		std::vector<size_t> folders;
		for (size_t off = 0; off + 16 <= dat.size();)
		{
			uint32_t w;
			std::memcpy(&w, dat.data() + off + 4, 4);
			const size_t size = size_t((w >> 7) & 0x7FFFF) * 16;
			if (size < 16 || off + size > dat.size())
				break;
			dat_chunk c;
			std::memcpy(c.name, dat.data() + off, 4);
			c.kind = w & 0x7F;
			c.folder = folders.empty() ? 0 : folders.back();
			c.data.assign(dat.begin() + off + 16, dat.begin() + off + size);
			if (c.kind == 1)
				folders.push_back(out.size());
			else if (c.kind == 0 && !folders.empty())
				folders.pop_back();
			out.push_back(std::move(c));
			off += size;
		}
		return out;
	}

	// A DAT's meshes into the cache with the textures their commands name (the first of that name in the mesh's folder)
	uint32_t g_next_key = 0x60000000u;
	void load_into_cache(const std::vector<dat_chunk> &dat, std::vector<uint32_t> &meshes, std::vector<ch::os2::mesh> &decoded, std::vector<int> &slots)
	{
		std::map<size_t, uint32_t> texture_of_chunk;
		for (size_t i = 0; i < dat.size(); ++i)
		{
			if (dat[i].kind != 0x2A)
				continue;
			ch::os2::sections s;
			ch::os2::sections_of_chunk(dat[i].data.data(), dat[i].data.size(), s);
			std::vector<ch::os2::texture_ref> refs;
			ch::os2::texture_commands(dat[i].data.data(), dat[i].data.size(), s, refs);
			std::vector<uint32_t> ids;
			for (const auto &r : refs)
			{
				uint32_t id = ch::cache::kNone;
				int depth = 0;
				for (size_t j = dat[i].folder + 1; j < dat.size(); ++j)
				{
					if (dat[j].kind == 1)
						depth++;
					if (dat[j].kind == 0 && --depth < 0)
						break;
					if (dat[j].kind == 0x20 && dat[j].data.size() > 17 && std::memcmp(dat[j].data.data() + 1, r.name, 16) == 0)
					{
						auto known = texture_of_chunk.find(j);
						if (known == texture_of_chunk.end())
						{
							ch::cache::key k;
							k.resource = g_next_key += 16;
							std::vector<uint8_t> bytes = dat[j].data;
							known = texture_of_chunk.emplace(j, ch::cache::add_texture(k, std::move(bytes))).first;
						}
						id = known->second;
						break;
					}
				}
				ids.push_back(id);
			}
			ch::cache::key k;
			k.resource = g_next_key += 16;
			std::vector<uint8_t> bytes = dat[i].data;
			meshes.push_back(ch::cache::add_mesh(k, std::move(bytes), 0, std::move(ids), dat[i].name));
			ch::os2::mesh m;
			std::string why;
			ch::os2::decode(dat[i].data.data(), dat[i].data.size(), s, m, why);
			decoded.push_back(std::move(m));
			slots.push_back(ch::os2::weapon_slot(dat[i].name));
		}
	}

	struct mat4
	{
		float m[16];
	};
	mat4 mul(const mat4 &a, const mat4 &b)
	{
		mat4 o = {};
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				for (int k = 0; k < 4; ++k)
					o.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
		return o;
	}
	// The game's camera: y down, looking from eye to at
	mat4 camera(const float eye[3], const float at[3], float aspect)
	{
		float z[3] = { eye[0] - at[0], eye[1] - at[1], eye[2] - at[2] }, up[3] = { 0, -1, 0 }, x[3], y[3];
		auto norm = [](float *v) {
			const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			for (int k = 0; k < 3; ++k)
				v[k] /= l;
		};
		auto cross = [](const float *a, const float *b, float *o) {
			o[0] = a[1] * b[2] - a[2] * b[1];
			o[1] = a[2] * b[0] - a[0] * b[2];
			o[2] = a[0] * b[1] - a[1] * b[0];
		};
		auto dot = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
		norm(z);
		cross(up, z, x);
		norm(x);
		cross(z, x, y);
		const mat4 v = { { x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1 } };
		const float ys = 1.0f / std::tan(0.4f), xs = ys / aspect, zn = 0.1f, zf = 500.0f;
		const mat4 p = { { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zn - zf), -1, 0, 0, zn * zf / (zn - zf), 0 } };
		return mul(v, p);
	}

	float env_float(const char *name, float fallback)
	{
		const char *v = getenv(name);
		return v != nullptr ? float(atof(v)) : fallback;
	}

	void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Transition.pResource = r;
		b.Transition.Subresource = sub;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		list->ResourceBarrier(1, &b);
		ch::inject::on_barrier(1, &b);
	}

	bool write_bmp(const std::string &path, const uint8_t *rgba, UINT w, UINT h)
	{
		FILE *fo = nullptr;
		if (fopen_s(&fo, path.c_str(), "wb") != 0 || fo == nullptr)
			return false;
		uint8_t head[54] = { 'B', 'M' };
		auto put = [&](int at, uint32_t v) { std::memcpy(head + at, &v, 4); };
		put(2, 54 + w * h * 4);
		put(10, 54);
		put(14, 40);
		put(18, w);
		put(22, uint32_t(-int32_t(h)));
		head[26] = 1;
		head[28] = 32;
		fwrite(head, 1, 54, fo);
		for (size_t i = 0; i < size_t(w) * h; ++i)
		{
			const uint8_t bgra[4] = { rgba[i * 4 + 2], rgba[i * 4 + 1], rgba[i * 4 + 0], 255 };
			fwrite(bgra, 1, 4, fo);
		}
		fclose(fo);
		return true;
	}
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }
namespace hifi::backend
{
	void log(int, const char *) {}
}

int main(int argc, char **argv)
{
	namespace inj = ch::inject;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 5)
	{
		std::printf("usage: enspell_view <poses.bin> <out folder> <frames> <dat> [dat...]\n");
		return 2;
	}
	const UINT W = UINT(env_float("ENSPELL_W", 960)), H = UINT(env_float("ENSPELL_H", 720));
	const int frames = atoi(argv[3]);
	const uint8_t effect = getenv("ENSPELL") != nullptr ? uint8_t(std::clamp(atoi(getenv("ENSPELL")), 0, int(ch::kEnDark))) : ch::kEnFire;

	// The poses
	std::vector<std::vector<ch::bone>> poses;
	{
		std::ifstream f(argv[1], std::ios::binary);
		uint32_t n = 0, bones = 0;
		f.read(reinterpret_cast<char *>(&n), 4);
		f.read(reinterpret_cast<char *>(&bones), 4);
		for (uint32_t p = 0; p < n && f; ++p)
		{
			std::vector<ch::bone> pose;
			for (uint32_t i = 0; i < bones && f; ++i)
			{
				float m[16];
				f.read(reinterpret_cast<char *>(m), 64);
				ch::bone b;
				for (int r = 0; r < 4; ++r)
					for (int c = 0; c < 3; ++c)
						b.m[r * 3 + c] = m[r * 4 + c];
				pose.push_back(b);
			}
			poses.push_back(std::move(pose));
		}
	}
	if (poses.empty() || poses[0].empty())
	{
		std::printf("no poses in %s\n", argv[1]);
		return 1;
	}

	// The device
	const bool debug_layer = getenv("ENSPELL_DEBUG") != nullptr;
	if (debug_layer)
	{
		ID3D12Debug *debug = nullptr;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
			debug->EnableDebugLayer();
	}
	IDXGIFactory4 *factory = nullptr;
	CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	IDXGIAdapter *adapter = nullptr;
	if (getenv("ENSPELL_WARP") != nullptr)
		factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
	ID3D12Device *device = nullptr;
	if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
	{
		std::printf("no Direct3D 12 device\n");
		return 1;
	}
	ID3D12CommandQueue *direct = nullptr;
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&direct));

	// The character's pieces into the cache, on the GPU
	std::vector<uint32_t> meshes;
	std::vector<ch::os2::mesh> decoded;
	std::vector<int> slots;
	for (int a = 4; a < argc; ++a)
		load_into_cache(read_dat(argv[a]), meshes, decoded, slots);
	ch::cache::frame_tick(device, 1.0);
	for (int i = 0; i < 400 && ch::cache::get_counts().meshes_ready < meshes.size(); ++i)
	{
		ch::cache::frame_tick(device, 1.0);
		Sleep(50);
	}
	ch::cache::settle(60000);
	uint32_t blades = 0;
	for (size_t k = 0; k < meshes.size(); ++k)
	{
		const ch::os2::blade b = ch::os2::blade_of(decoded[k], slots[k]);
		if (b.valid)
		{
			blades++;
			std::printf("blade: bone %u, base %.3f %.3f %.3f, tip %.3f %.3f %.3f, half width %.3f\n", b.bone, b.base[0], b.base[1], b.base[2], b.tip[0], b.tip[1], b.tip[2],
				b.half_width);
		}
	}
	std::printf("%zu meshes (%u ready), %u blade(s): %s\n", meshes.size(), ch::cache::get_counts().meshes_ready, blades, ch::cache::status()[0].c_str());
	std::string why;
	if (!inj::prepare(device, why))
	{
		std::printf("the renderer: %s\n", why.c_str());
		return 1;
	}
	inj::set_options(true);

	// The camera: on the blade's middle in the first pose, from the side the character faces
	float at[3] = { 0, -1.0f, 0 };
	for (size_t k = 0; k < meshes.size(); ++k)
	{
		const ch::os2::blade b = ch::os2::blade_of(decoded[k], slots[k]);
		if (!b.valid)
			continue;
		const float *m = poses[0][std::min<size_t>(b.bone, poses[0].size() - 1)].m;
		const float mid[3] = { 0.5f * (b.base[0] + b.tip[0]), 0.5f * (b.base[1] + b.tip[1]), 0.5f * (b.base[2] + b.tip[2]) };
		for (int c = 0; c < 3; ++c)
			at[c] = mid[0] * m[c] + mid[1] * m[3 + c] + mid[2] * m[6 + c] + m[9 + c];
	}
	// (between the blade and the hips, bone 2)
	{
		const float *hips = poses[0][std::min<size_t>(2, poses[0].size() - 1)].m;
		const float k = env_float("ENSPELL_AIM", 0.5f);
		for (int c = 0; c < 3; ++c)
			at[c] += (hips[9 + c] - at[c]) * k;
	}
	const float yaw = env_float("ENSPELL_YAW", 25.0f) * 3.14159265f / 180.0f, dist = env_float("ENSPELL_DIST", 3.0f), lift = env_float("ENSPELL_LIFT", 0.35f);
	const float eye[3] = { at[0] + dist * std::sin(yaw), at[1] - lift, at[2] + dist * std::cos(yaw) };
	const mat4 vp = camera(eye, at, float(W) / H);

	// dgVoodoo's world: typeless resources, UNORM / D24S8 views
	D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, rb = { D3D12_HEAP_TYPE_READBACK };
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = W;
	td.Height = H;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.SampleDesc.Count = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	ID3D12Resource *A = nullptr, *Az = nullptr;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&A));
	td.Format = DXGI_FORMAT_R24G8_TYPELESS;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&Az));
	ID3D12DescriptorHeap *rtvh = nullptr, *dsvh = nullptr, *srvh = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh));
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvh));
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvh));
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvh->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvh->GetCPUDescriptorHandleForHeapStart(), dsv_clear = dsv;
	dsv_clear.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	D3D12_RENDER_TARGET_VIEW_DESC rvd = {};
	rvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	device->CreateRenderTargetView(A, &rvd, rtv);
	D3D12_DEPTH_STENCIL_VIEW_DESC dvd = {};
	dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dvd.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL;
	device->CreateDepthStencilView(Az, &dvd, dsv);
	dvd.Flags = D3D12_DSV_FLAG_NONE;
	device->CreateDepthStencilView(Az, &dvd, dsv_clear);
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = UINT64(W) * 4 * H;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *readback = nullptr;
	device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
	ID3D12CommandAllocator *alloc = nullptr;
	ID3D12GraphicsCommandList *list = nullptr;
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
	list->Close();
	ID3D12Fence *fence = nullptr;
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	const float kNight[4] = { 0.07f, 0.08f, 0.12f, 1.0f };

	for (int i = 0; i < frames; ++i)
	{
		const std::vector<ch::bone> &pose = poses[size_t(i) % poses.size()];
		const double t = double(i) / 30.0;
		ch::cache::frame_tick(device, 2.0 + t);
		inj::set_clock(10.0 + t);
		// The frame, as the game side publishes it: the character, opaque, HiFi XI's, with the en-spell on
		const uint32_t seq = uint32_t(i) + 1;
		ch::frame &f = ch::begin_frame(seq);
		std::memcpy(f.view_proj, vp.m, 64);
		f.camera = true;
		const float fog[3] = { kNight[0], kNight[1], kNight[2] };
		std::memcpy(f.fog, fog, 12);
		f.fog_start = 150.0f;
		f.fog_end = 600.0f;
		ch::model_entry &m = f.model[f.models];
		m = ch::model_entry();
		m.actor = 0xAC700000u;
		m.colour = 0x80808080u;
		m.flags = ch::kNative;
		m.enspell = effect;
		ch::light8 moon = {};
		moon.type = 3;
		moon.diffuse[0] = 0.75f;
		moon.diffuse[1] = 0.8f;
		moon.diffuse[2] = 0.95f;
		moon.direction[0] = 0.31f;
		moon.direction[1] = 0.951f;
		m.lights[0] = moon;
		if (effect != ch::kEnNone)
		{
			// (the element's own light on the character from the blade's side: fire and thunder flicker)
			static const float kLight[9][3] = { { 0, 0, 0 }, { 1.25f, 0.55f, 0.18f }, { 0.45f, 0.75f, 1.2f }, { 0.5f, 1.0f, 0.6f }, { 0.9f, 0.6f, 0.25f },
				{ 0.8f, 0.65f, 1.4f }, { 0.3f, 0.6f, 1.2f }, { 1.3f, 1.15f, 0.8f }, { 0.45f, 0.15f, 0.7f } };
			ch::light8 glow = {};
			glow.type = 3;
			float flicker = 1.0f;
			if (effect == ch::kEnFire)
				flicker = 0.85f + 0.15f * std::sin(float(t) * 17.0f) * std::sin(float(t) * 7.3f + 1.0f);
			else if (effect == ch::kEnThunder)
				flicker = (i % 5) < 2 ? 1.25f : 0.7f;
			for (int k = 0; k < 3; ++k)
				glow.diffuse[k] = kLight[effect][k] * flicker;
			glow.direction[0] = -0.6f;
			glow.direction[1] = 0.3f;
			glow.direction[2] = 0.75f;
			m.lights[1] = glow;
		}
		m.ambient = 0xFF404858u;
		m.tfactor = 0x80808080u;
		m.first_instance = f.instances;
		m.instances = 1;
		ch::instance_entry &ie = f.instance[f.instances++];
		ie.first_bone = f.bones;
		ie.bones = uint32_t(pose.size());
		for (size_t b = 0; b < pose.size(); ++b)
			f.palette[f.bones + b] = pose[b];
		f.bones += ie.bones;
		ie.first_mesh = f.meshes;
		ie.meshes = uint32_t(meshes.size());
		uint32_t hidden = 0;
		for (size_t k = 0; k < meshes.size(); ++k)
		{
			f.mesh[f.meshes + k].mesh = meshes[k];
			hidden |= ch::os2::hidden_by(ch::cache::mesh_hides(meshes[k]));
		}
		m.hidden = hidden;
		f.meshes += ie.meshes;
		f.models++;
		ch::publish_opaque(f);
		ch::cache::publish_palettes(f, 0, f.bones);

		// The list, as dgVoodoo records the world round the characters' end marker
		alloc->Reset();
		list->Reset(alloc, nullptr);
		inj::on_reset(list);
		inj::on_marker(Az);
		list->ClearRenderTargetView(rtv, kNight, 0, nullptr);
		list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
		list->SetDescriptorHeaps(1, &srvh);
		inj::on_heaps(list, 1, &srvh);
		grass::list_state bound;
		bound.rtvs[0] = rtv;
		bound.rtv_count = 1;
		bound.dsv = dsv;
		inj::world w;
		w.rt = A;
		w.ds = Az;
		w.rt_format = DXGI_FORMAT_R8G8B8A8_UNORM;
		w.ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		// (the planes as dgVoodoo leaves them at the marker: depth read, stencil written)
		barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
		barrier(list, Az, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
		barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		inj::on_chars_end(list, bound, w, seq & 15);
		barrier(list, Az, 0, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
		dst.pResource = readback;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_TYPELESS, W, H, 1, W * 4 };
		src.pResource = A;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		list->Close();
		ID3D12CommandList *lists[] = { list };
		direct->ExecuteCommandLists(1, lists);
		direct->Signal(fence, UINT64(i) + 1);
		fence->SetEventOnCompletion(UINT64(i) + 1, done);
		WaitForSingleObject(done, 60000);
		if (device->GetDeviceRemovedReason() != S_OK)
		{
			std::printf("the device was removed at frame %d\n", i);
			return 1;
		}
		uint8_t *px = nullptr;
		readback->Map(0, nullptr, reinterpret_cast<void **>(&px));
		char name[64];
		std::snprintf(name, sizeof(name), "\\frame%03d.bmp", i);
		write_bmp(std::string(argv[2]) + name, px, W, H);
		readback->Unmap(0, nullptr);
	}
	const inj::counts n = inj::get_counts();
	std::printf("%d frames: %llu drawn, %llu models, %llu with an en-spell; %s\n", frames, n.frames, n.models, n.enspell_models, inj::status()[0].c_str());
	inj::release();
	ch::cache::stop();
	if (debug_layer)
	{
		ID3D12InfoQueue *q = nullptr;
		uint32_t bad = 0;
		if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&q))))
			for (UINT64 i = 0, count = q->GetNumStoredMessages(); i < count; ++i)
			{
				SIZE_T size = 0;
				q->GetMessage(i, nullptr, &size);
				std::vector<uint8_t> buf(size);
				D3D12_MESSAGE *msg = reinterpret_cast<D3D12_MESSAGE *>(buf.data());
				q->GetMessage(i, msg, &size);
				if (msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING && msg->ID != D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE &&
					msg->ID != D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE && bad++ < 5)
					std::printf("debug layer: %s\n", msg->pDescription);
			}
		std::printf("the debug layer: %u errors or warnings\n", bad);
		return bad != 0 ? 3 : 0;
	}
	return 0;
}
