/*
 * The actors' part of owning the frame drawn (src/chars/chardraw.cpp, charinject.cpp; docs/own-frame/actors.md), on
 * WARP with the debug layer:
 *   - cloth: the San d'Orian banner (ROM/7/95.DAT: its pole and its cloth 'hf_f') as the cache has it from the DAT, its
 *     skeleton and cloth vertices as tests/cloth_oracle.py steps them (its last two frames): every cloth vertex on a
 *     drawn pixel, the cloth's pixels textured; its motion (FSR) on the cloth's pixels the move of its vertices between
 *     the two frames, none of its own on the pole;
 *   - the overlay pass (025C40): a uniform overlay texture added on the banner's pixels as the stages make it
 *     (MODULATE2X twice: 2 x 2 x texel x factor, alpha alike), nothing beside it;
 *   - the cloak pass (025330) on a see-through model drawn into T: the scene's copy (a picture of two halves) added with
 *     the weight 0x80 less the actor's alpha makes (8 x weight squared on alpha, 1.004 x the copy on colour), sampled
 *     at each pixel's own place shifted by the offsets through PROJECTION: half the screen's height here, so each
 *     pixel takes the other half's colour;
 *   - the same model through the see-through marker (charinject: the world's target copied, then T, then the
 *     composite), the debug layer saying nothing; the counts.
 *
 *   build\actors_draw_test.exe build\test\cloth_oracle.bin
 */

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include "../src/chars/cache.hpp"
#include "../src/chars/chardraw.hpp"
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
	namespace inj = hifi::chars::inject;

	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	constexpr UINT W = 480, H = 360;

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
	// The game's camera (y down, looking from eye to at): VIEW and PROJECTION
	void camera(const float eye[3], const float at[3], mat4 &view, mat4 &proj)
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
		view = { { x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1 } };
		const float ys = 1.0f / std::tan(0.4f), xs = ys / (float(W) / H), zn = 0.1f, zf = 500.0f;
		proj = { { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zn - zf), -1, 0, 0, zn * zf / (zn - zf), 0 } };
	}
	bool project(const mat4 &vp, const float p[3], float &sx, float &sy, float &w)
	{
		float o[4] = {};
		const float q[4] = { p[0], p[1], p[2], 1 };
		for (int j = 0; j < 4; ++j)
			for (int k = 0; k < 4; ++k)
				o[j] += q[k] * vp.m[k * 4 + j];
		if (o[3] <= 0)
			return false;
		sx = (o[0] / o[3] * 0.5f + 0.5f) * W;
		sy = (0.5f - o[1] / o[3] * 0.5f) * H;
		w = o[3];
		return sx >= 0 && sy >= 0 && sx < W && sy < H;
	}

	// ------------------------------------------------------------------------------------------ the oracle's banner
	struct oracle_frame
	{
		std::vector<float> palette, pos, nrm;
	};
	// The first DAT's (the banner's) last two frames: its palette, its one cloth's positions and normals
	bool read_banner(const char *path, uint32_t &bones, uint32_t &n, oracle_frame &before, oracle_frame &now)
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
		auto floats = [&](std::vector<float> &v, size_t k) {
			v.resize(k);
			if (o + 4 * k <= d.size())
				std::memcpy(v.data(), d.data() + o, 4 * k);
			o += 4 * k;
		};
		if (u32() < 1)
			return false;
		o += u32();
		bones = u32();
		const uint32_t meshes = u32(), frames = u32();
		if (meshes != 1 || frames < 2)
			return false;
		u32();
		n = u32();
		for (uint32_t fr = 0; fr < frames; ++fr)
		{
			oracle_frame of;
			floats(of.palette, size_t(bones) * 16);
			o += 4 * 4 + 8;
			floats(of.pos, size_t(n) * 3);
			floats(of.nrm, size_t(n) * 3);
			if (fr + 2 == frames)
				before = of;
			else if (fr + 1 == frames)
				now = of;
		}
		return o <= d.size();
	}

	struct dat_chunk
	{
		uint32_t kind;
		size_t folder;
		uint8_t name[4];
		std::vector<uint8_t> data;
	};
	std::vector<dat_chunk> read_dat(const char *rel)
	{
		std::string path = std::string("C:\\Games\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI\\") + rel;
		std::ifstream f(path, std::ios::binary);
		std::vector<uint8_t> dat((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		std::vector<dat_chunk> out;
		std::vector<size_t> folders;
		size_t off = 0;
		while (off + 16 <= dat.size())
		{
			uint32_t w;
			std::memcpy(&w, dat.data() + off + 4, 4);
			const size_t size = size_t((w >> 7) & 0x7FFFF) * 16;
			if (size < 16 || off + size > dat.size())
				break;
			dat_chunk c;
			c.kind = w & 0x7F;
			c.folder = folders.empty() ? 0 : folders.back();
			std::memcpy(c.name, dat.data() + off, 4);
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
	// Each mesh into the cache with the textures its commands name (the first of that name in its folder)
	struct loaded
	{
		uint32_t pole = ch::cache::kNone, cloth = ch::cache::kNone;
		std::vector<ch::os2::mesh> decoded;
	};
	loaded load_banner(const std::vector<dat_chunk> &dat)
	{
		loaded L;
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
							k.resource = 0x70000000u + uint32_t(j) * 16;
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
			k.resource = 0x60000000u + uint32_t(i) * 16;
			std::vector<uint8_t> bytes = dat[i].data;
			const uint32_t id = ch::cache::add_mesh(k, std::move(bytes), 0, std::move(ids));
			const bool cloth = (dat[i].data[2] & 0x7F) == 1;
			(cloth ? L.cloth : L.pole) = id;
		}
		return L;
	}
	// A plain 32-bit texture chunk of one colour (BGRA bytes), 4 x 4
	std::vector<uint8_t> uniform_texture(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
	{
		std::vector<uint8_t> d(57 + 16 * 4, 0);
		d[0] = 0x10;
		std::memcpy(d.data() + 1, "test    overlay ", 16);
		d[21] = 4;
		d[25] = 4;
		d[31] = 32;
		for (int i = 0; i < 16; ++i)
		{
			d[57 + 4 * i] = b;
			d[58 + 4 * i] = g;
			d[59 + 4 * i] = r;
			d[60 + 4 * i] = a;
		}
		return d;
	}

	// ------------------------------------------------------------------------------------------ the GPU
	struct gpu
	{
		ID3D12Device *device = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		ID3D12CommandAllocator *allocator = nullptr;
		ID3D12GraphicsCommandList *list = nullptr;
		ID3D12Fence *fence = nullptr;
		ID3D12InfoQueue *info = nullptr;
		uint64_t value = 0;
	};
	void begin_list(gpu &g)
	{
		g.allocator->Reset();
		g.list->Reset(g.allocator, nullptr);
	}
	void run(gpu &g)
	{
		g.list->Close();
		ID3D12CommandList *l[] = { g.list };
		g.queue->ExecuteCommandLists(1, l);
		g.queue->Signal(g.fence, ++g.value);
		HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		g.fence->SetEventOnCompletion(g.value, e);
		WaitForSingleObject(e, 30000);
		CloseHandle(e);
	}
	uint32_t debug_messages(ID3D12InfoQueue *q, std::string &first)
	{
		if (q == nullptr)
			return 0;
		const UINT64 n = q->GetNumStoredMessages();
		uint32_t errors = 0;
		for (UINT64 i = 0; i < n; ++i)
		{
			SIZE_T size = 0;
			q->GetMessage(i, nullptr, &size);
			std::vector<uint8_t> buf(size);
			D3D12_MESSAGE *m = reinterpret_cast<D3D12_MESSAGE *>(buf.data());
			q->GetMessage(i, m, &size);
			if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING && m->ID != D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE &&
				m->ID != D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE)
			{
				if (errors++ == 0)
					first = m->pDescription;
			}
		}
		q->ClearStoredMessages();
		return errors;
	}
	ID3D12Resource *texture(gpu &g, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear)
	{
		D3D12_RESOURCE_DESC d = {};
		d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		d.Width = W;
		d.Height = H;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = format;
		d.SampleDesc.Count = 1;
		d.Flags = flags;
		const D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
		ID3D12Resource *r = nullptr;
		g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, clear, IID_PPV_ARGS(&r));
		return r;
	}
	void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Transition.pResource = r;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		list->ResourceBarrier(1, &b);
	}
	// A texture's bytes (rows packed), read back from `state` and left there
	std::vector<uint8_t> read_back(gpu &g, ID3D12Resource *t, D3D12_RESOURCE_STATES state, uint32_t bytes_per_pixel)
	{
		const D3D12_RESOURCE_DESC d = t->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
		UINT rows = 0;
		UINT64 row_bytes = 0, total = 0;
		g.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &row_bytes, &total);
		D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = total;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *rb = nullptr;
		g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
		begin_list(g);
		barrier(g.list, t, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
		dst.pResource = rb;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint = fp;
		src.pResource = t;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		barrier(g.list, t, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
		run(g);
		std::vector<uint8_t> out(size_t(W) * H * bytes_per_pixel);
		uint8_t *p = nullptr;
		rb->Map(0, nullptr, reinterpret_cast<void **>(&p));
		for (UINT r = 0; r < rows; ++r)
			std::memcpy(out.data() + size_t(r) * W * bytes_per_pixel, p + fp.Offset + uint64_t(r) * fp.Footprint.RowPitch, size_t(W) * bytes_per_pixel);
		rb->Unmap(0, nullptr);
		rb->Release();
		return out;
	}
	float half_to_float(uint16_t h)
	{
		const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
		float v = e == 0 ? std::ldexp(float(m), -24) : std::ldexp(float(m | 1024), int(e) - 25);
		return s ? -v : v;
	}
}

int main(int argc, char **argv)
{
	uint32_t bones = 0, n = 0;
	oracle_frame before, now;
	const bool have = argc > 1 && read_banner(argv[1], bones, n, before, now);
	check(have && bones == 4 && n == 105, "the banner's last two oracle frames: " + std::to_string(bones) + " bones, " + std::to_string(n) + " cloth vertices");
	if (!have)
		return 1;

	gpu g;
	ID3D12Debug *debug = nullptr;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
		debug->EnableDebugLayer();
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
	if (FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device))))
	{
		check(false, "a WARP device");
		return 1;
	}
	g.device->QueryInterface(IID_PPV_ARGS(&g.info));
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));
	g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator));
	g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator, nullptr, IID_PPV_ARGS(&g.list));
	g.list->Close();
	g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));

	// The banner and an overlay texture into the cache, on the GPU
	const std::vector<dat_chunk> dat = read_dat("ROM\\7\\95.DAT");
	const loaded L = load_banner(dat);
	ch::cache::key ok_key;
	ok_key.resource = 0x7F000000u;
	const uint32_t overlay = ch::cache::add_texture(ok_key, uniform_texture(0x40, 0x20, 0x10, 0x80));
	ch::cache::frame_tick(g.device, 1.0);
	for (int i = 0; i < 400 && !(ch::cache::mesh_ready(L.pole) && ch::cache::mesh_ready(L.cloth)); ++i)
		Sleep(50);
	ch::cache::settle(60000);
	D3D12_GPU_DESCRIPTOR_HANDLE oh;
	check(ch::cache::mesh_ready(L.pole) && ch::cache::mesh_ready(L.cloth) && ch::cache::texture_view_of(overlay, oh), "the banner's pole and cloth, an overlay texture on the GPU: " +
		ch::cache::status()[0]);
	ch::cache::mesh_view cv;
	const bool viewed = ch::cache::mesh_view_of(L.cloth, cv);
	check(viewed && cv.vertices == n, "  the cloth: one vertex a work vertex (" + std::to_string(cv.vertices) + ")");

	ch::char_renderer renderer;
	std::string why;
	check(renderer.init(g.device, why), "the renderer made" + (why.empty() ? "" : ": " + why));

	// The camera on the cloth
	float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
	for (uint32_t i = 0; i < n; ++i)
		for (int k = 0; k < 3; ++k)
		{
			lo[k] = std::min(lo[k], now.pos[3 * i + k]);
			hi[k] = std::max(hi[k], now.pos[3 * i + k]);
		}
	const float centre[3] = { (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2 };
	const float eye[3] = { centre[0] + 3.2f, centre[1] - 0.2f, centre[2] - 1.0f };
	mat4 view, proj;
	camera(eye, centre, view, proj);
	const mat4 vp = mul(view, proj);

	// A frame: the banner, its skeleton (instance 0, the pole) and its cloth (instance 1: the vertices as rows)
	auto rows_of = [&](const oracle_frame &of, std::vector<ch::bone> &skeleton, std::vector<ch::bone> &cloth) {
		skeleton.resize(bones);
		for (uint32_t b = 0; b < bones; ++b)
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 3; ++c)
					skeleton[b].m[r * 3 + c] = of.palette[size_t(b) * 16 + r * 4 + c];
		cloth.assign(n, ch::bone());
		for (uint32_t i = 0; i < n; ++i)
		{
			float *m = cloth[i].m;
			std::memset(m, 0, sizeof(cloth[i].m));
			m[0] = of.nrm[3 * i];
			m[1] = of.nrm[3 * i + 1];
			m[2] = of.nrm[3 * i + 2];
			m[9] = of.pos[3 * i];
			m[10] = of.pos[3 * i + 1];
			m[11] = of.pos[3 * i + 2];
		}
	};
	auto make_frame = [&](uint32_t seq, const oracle_frame &of, uint32_t colour, uint32_t flags, uint8_t extra) -> ch::frame & {
		ch::frame &f = ch::begin_frame(seq);
		std::memcpy(f.view_proj, vp.m, 64);
		f.camera = true;
		std::memcpy(f.projection, proj.m, 64);
		f.projection_read = true;
		ch::model_entry &m = f.model[0];
		m = ch::model_entry();
		m.actor = 0xBA770000u;
		m.model = 0xBA771000u;
		m.colour = colour;
		m.flags = flags;
		ch::light8 sun = {};
		sun.type = 3;
		sun.diffuse[0] = sun.diffuse[1] = sun.diffuse[2] = 1.2f;
		sun.direction[0] = 0.31f;
		sun.direction[1] = 0.951f;
		m.lights[0] = sun;
		m.ambient = 0xFF606060u;
		m.tfactor = 0x80808080u;
		m.passes = extra;
		m.overlay_texture = overlay;
		m.overlay_offset[0] = 0.25f;
		m.overlay_colour = 0x80404040u;
		std::vector<ch::bone> skeleton, cloth;
		rows_of(of, skeleton, cloth);
		m.first_instance = 0;
		m.instances = 2;
		f.instance[0] = { 0, bones, 0, 1 };
		f.instance[1] = { bones, n, 1, 1 };
		std::copy(skeleton.begin(), skeleton.end(), f.palette);
		std::copy(cloth.begin(), cloth.end(), f.palette + bones);
		f.bones = bones + n;
		f.mesh[0].mesh = L.pole;
		f.mesh[1].mesh = L.cloth;
		f.meshes = 2;
		f.instances = 2;
		f.models = 1;
		if (flags & ch::kFromFlush)
			ch::publish_see_through(f);
		else
			ch::publish_opaque(f);
		ch::cache::publish_palettes(f, 0, f.bones);
		return f;
	};

	// Targets: the world's (RGBA8, cleared to a sky), its depth, the motion
	const float kSky[4] = { 0.30f, 0.35f, 0.45f, 1.0f };
	D3D12_CLEAR_VALUE rc = {}, dc = {}, mc = {};
	rc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	std::memcpy(rc.Color, kSky, 16);
	dc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dc.DepthStencil.Depth = 1.0f;
	mc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	ID3D12Resource *rt = texture(g, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &rc);
	ID3D12Resource *ds = texture(g, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc);
	ID3D12Resource *mt = texture(g, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &mc);
	ID3D12Resource *tt = texture(g, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
	ID3D12DescriptorHeap *rtvs = nullptr, *dsvs = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 };
	g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs));
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
	g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvs));
	const UINT step = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), mrtv = rtv, trtv = rtv;
	mrtv.ptr += step;
	trtv.ptr += 2 * step;
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
	g.device->CreateRenderTargetView(rt, nullptr, rtv);
	g.device->CreateRenderTargetView(mt, nullptr, mrtv);
	g.device->CreateRenderTargetView(tt, nullptr, trtv);
	g.device->CreateDepthStencilView(ds, nullptr, dsv);
	ch::char_target t;
	t.rtv = rtv;
	t.dsv = dsv;
	t.rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
	t.dsv_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	t.width = float(W);
	t.height = float(H);
	ch::char_options o;

	// Draws the opaque frame into a cleared world (with the motion target when asked): the world's pixels, the motion
	auto draw_opaque = [&](const ch::frame &f, bool motion, std::vector<uint8_t> &px, std::vector<uint8_t> &mv, ch::char_stats &st) {
		begin_list(g);
		g.list->ClearRenderTargetView(rtv, kSky, 0, nullptr);
		g.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		ch::char_target tm = t;
		if (motion)
		{
			const float none[4] = {};
			g.list->ClearRenderTargetView(mrtv, none, 0, nullptr);
			tm.motion_rtv = mrtv;
			tm.motion_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		}
		st = renderer.record_opaque(g.list, tm, f, o);
		run(g);
		px = read_back(g, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, 4);
		if (motion)
			mv = read_back(g, mt, D3D12_RESOURCE_STATE_RENDER_TARGET, 8);
	};
	auto is_sky = [&](const uint8_t *p) {
		return std::abs(p[0] - int(kSky[0] * 255 + 0.5f)) <= 1 && std::abs(p[1] - int(kSky[1] * 255 + 0.5f)) <= 1 && std::abs(p[2] - int(kSky[2] * 255 + 0.5f)) <= 1;
	};

	// ---- Cloth: drawn where its vertices are, textured; its motion the oracle's move
	{
		make_frame(40, before, 0x80808080u, ch::kNative, 0);
		const ch::frame &f = make_frame(41, now, 0x80808080u, ch::kNative, 0);
		std::vector<uint8_t> px, mv;
		ch::char_stats st;
		draw_opaque(f, true, px, mv, st);
		std::string first;
		const uint32_t messages = debug_messages(g.info, first);
		check(messages == 0, "the debug layer: nothing (" + std::to_string(messages) + (messages ? ": " + first : "") + ")");
		uint32_t on = 0, total = 0;
		float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
		for (uint32_t i = 0; i < n; ++i)
		{
			float sx, sy, w;
			if (!project(vp, &now.pos[3 * i], sx, sy, w))
				continue;
			total++;
			x0 = std::min(x0, sx), x1 = std::max(x1, sx), y0 = std::min(y0, sy), y1 = std::max(y1, sy);
			bool drawn = false;
			for (int dy = -1; dy <= 1 && !drawn; ++dy)
				for (int dx = -1; dx <= 1 && !drawn; ++dx)
				{
					const int x = int(sx) + dx, y = int(sy) + dy;
					drawn = x >= 0 && y >= 0 && x < int(W) && y < int(H) && !is_sky(&px[(size_t(y) * W + x) * 4]);
				}
			on += drawn;
		}
		check(total > n * 9 / 10 && on >= total * 9 / 10, "cloth: its vertices on drawn pixels: " + std::to_string(on) + " of " + std::to_string(total) + " in view; " +
			std::to_string(st.draws) + " draws, " + std::to_string(st.triangles) + " triangles");
		// (inside the cloth's screen box: mostly drawn, in more than one colour: its texture)
		uint32_t inside = 0, drawn = 0, colours = 0;
		uint8_t first_colour[3] = {};
		for (int y = int(y0) + 2; y < int(y1) - 2; ++y)
			for (int x = int(x0) + 2; x < int(x1) - 2; ++x)
			{
				const uint8_t *p = &px[(size_t(y) * W + x) * 4];
				inside++;
				if (is_sky(p))
					continue;
				if (drawn++ == 0)
					std::memcpy(first_colour, p, 3);
				else if (std::abs(p[0] - first_colour[0]) + std::abs(p[1] - first_colour[1]) + std::abs(p[2] - first_colour[2]) > 24)
					colours++;
			}
		check(inside > 500 && drawn > inside / 3 && colours > drawn / 20, "  its screen box drawn (" + std::to_string(drawn) + " of " + std::to_string(inside) + "), textured (" +
			std::to_string(colours) + " pixels off the first one's colour)");
		// The motion: each cloth vertex's pixel moved by the oracle's move (within a pixel's slack of its neighbours'), the
		// pole's none
		float largest = 0.0f;
		for (uint32_t i = 0; i < n; ++i)
			largest = std::max(largest, std::sqrt(std::pow(now.pos[3 * i] - before.pos[3 * i], 2.0f) + std::pow(now.pos[3 * i + 1] - before.pos[3 * i + 1], 2.0f) +
															  std::pow(now.pos[3 * i + 2] - before.pos[3 * i + 2], 2.0f)));
		// (each vertex's pixel: the move it made, within 40% of the largest of a neighbour's (the pixel's motion is its
		// triangles' blend); the pinned ones, still, none)
		uint32_t right = 0, checked = 0, still = 0;
		for (uint32_t i = 0; i < n; ++i)
		{
			float sx, sy, w;
			if (!project(vp, &now.pos[3 * i], sx, sy, w))
				continue;
			const uint16_t *q = reinterpret_cast<const uint16_t *>(&mv[(size_t(int(sy)) * W + int(sx)) * 8]);
			const float m[4] = { half_to_float(q[0]), half_to_float(q[1]), half_to_float(q[2]), half_to_float(q[3]) };
			if (m[3] <= 0.0f)
				continue;
			checked++;
			float d2 = 0.0f;
			for (int k = 0; k < 3; ++k)
				d2 += std::pow(m[k] - (now.pos[3 * i + k] - before.pos[3 * i + k]), 2.0f);
			right += std::sqrt(d2) <= 0.4f * largest + 1e-4f;
			still += std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]) < 1e-4f;
		}
		char line[240];
		std::snprintf(line, sizeof(line), "cloth: its motion (FSR) from the last frame's rows: %u of %u vertex pixels the oracle's move (the largest %.4f yalms; %u still)",
			right, checked, largest, still);
		check(checked > n / 2 && right >= checked * 9 / 10 && still < checked, line);
	}

	// ---- The overlay: (64, 32, 16) added on the banner's pixels, as the stages make it from the uniform texture
	{
		const ch::frame &f0 = make_frame(42, now, 0x80808080u, ch::kNative, 0);
		std::vector<uint8_t> plain, mv;
		ch::char_stats st;
		draw_opaque(f0, false, plain, mv, st);
		const ch::frame &f1 = make_frame(43, now, 0x80808080u, ch::kNative, ch::kPassOverlay);
		std::vector<uint8_t> lit;
		draw_opaque(f1, false, lit, mv, st);
		std::string first;
		const uint32_t messages = debug_messages(g.info, first);
		// texel (0x40, 0x20, 0x10, 0x80): stage 0 2t = (0.502, 0.251, 0.125), alpha 1; stage 1 x 2 x 0x40 = (0.252, 0.126,
		// 0.063), alpha x 2 x 0x80: 1. Added x alpha (1): (64.3, 32.1, 16.1)
		const double want[3] = { 2 * (2 * 0x40 / 255.0) * (0x40 / 255.0) * 255, 2 * (2 * 0x20 / 255.0) * (0x40 / 255.0) * 255, 2 * (2 * 0x10 / 255.0) * (0x40 / 255.0) * 255 };
		// (every pixel it changed: the old one plus that; the mesh's alpha-tested holes too, as the game's pass has no
		// alpha test; none outside the banner's drawn box)
		int bx0 = int(W), by0 = int(H), bx1 = -1, by1 = -1;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
				if (!is_sky(&plain[(size_t(y) * W + x) * 4]))
				{
					bx0 = std::min(bx0, int(x)), bx1 = std::max(bx1, int(x));
					by0 = std::min(by0, int(y)), by1 = std::max(by1, int(y));
				}
		uint32_t model = 0, changed = 0, right = 0, outside = 0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const uint8_t *a = &plain[(size_t(y) * W + x) * 4], *b = &lit[(size_t(y) * W + x) * 4];
				model += !is_sky(a);
				if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2])
					continue;
				changed++;
				if (int(x) < bx0 - 3 || int(x) > bx1 + 3 || int(y) < by0 - 3 || int(y) > by1 + 3)
					outside++;
				bool ok = true;
				for (int k = 0; k < 3; ++k)
					ok = ok && std::fabs(b[k] - std::min(255.0, a[k] + want[k])) <= 2.0;
				right += ok;
			}
		check(messages == 0 && st.overlays == 1, "overlay: drawn (" + std::to_string(st.overlays) + "), the debug layer nothing (" + std::to_string(messages) + (messages ? ": " + first : "") + ")");
		check(model > 1000 && changed >= model * 9 / 10 && right >= changed * 97 / 100 && outside == 0, "overlay: (64, 32, 16) added on " + std::to_string(right) + " of the " +
			std::to_string(changed) + " pixels it changed (the banner's: " + std::to_string(model) + "; MODULATE2X twice of the texel and its factor), none beside it (" +
			std::to_string(outside) + ")");
	}

	// ---- The cloak, into T: the scene's copy (top half one colour, bottom the other) with the weight 0x80 less the
	// actor's alpha (0x40) makes, shifted half the screen down through PROJECTION
	{
		// (the scene's copy: rows above H/2 (0.2, 0, 0), below (0, 0.2, 0); its view in the cache's reserved slot 1)
		ID3D12Resource *scene = texture(g, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
		{
			D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = UINT64(W) * 4 * H;
			bd.Height = 1;
			bd.DepthOrArraySize = 1;
			bd.MipLevels = 1;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ID3D12Resource *up = nullptr;
			g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up));
			uint8_t *p = nullptr;
			up->Map(0, nullptr, reinterpret_cast<void **>(&p));
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					uint8_t *q = p + (size_t(y) * W + x) * 4;
					q[0] = y < H / 2 ? 51 : 0;
					q[1] = y < H / 2 ? 0 : 51;
					q[2] = 0;
					q[3] = 255;
				}
			up->Unmap(0, nullptr);
			begin_list(g);
			D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
			dst.pResource = scene;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.pResource = up;
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, W * 4 };
			g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			barrier(g.list, scene, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			run(g);
			up->Release();
		}
		D3D12_CPU_DESCRIPTOR_HANDLE scpu;
		D3D12_GPU_DESCRIPTOR_HANDLE sgpu;
		check(ch::cache::reserved(1, scpu, sgpu), "cloak: the cache's reserved descriptor 1 for the scene's copy");
		D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		sv.Texture2D.MipLevels = 1;
		g.device->CreateShaderResourceView(scene, &sv, scpu);
		// (the shift: half the screen's height in v at the cloth's distance: b = 0.5 w / _22, w the eye's distance)
		float cw = 0.0f, sx, sy;
		project(vp, centre, sx, sy, cw);
		const float b = 0.5f * cw / proj.m[5];
		auto draw_t = [&](const ch::frame &f, D3D12_GPU_DESCRIPTOR_HANDLE with, std::vector<uint8_t> &px, ch::char_stats &st) {
			begin_list(g);
			const float zero[4] = {};
			g.list->ClearRenderTargetView(trtv, zero, 0, nullptr);
			g.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ch::char_target ts = t;
			ts.rtv = trtv;
			st = renderer.record_model(g.list, ts, f, 0, o, with);
			run(g);
			px = read_back(g, tt, D3D12_RESOURCE_STATE_RENDER_TARGET, 4);
		};
		ch::frame &f0 = make_frame(44, now, 0x40808080u, ch::kNative | ch::kFromFlush, 0);
		std::vector<uint8_t> plain, cloaked;
		ch::char_stats st;
		draw_t(f0, sgpu, plain, st);
		ch::frame &f1 = make_frame(45, now, 0x40808080u, ch::kNative | ch::kFromFlush, ch::kPassCloak);
		f1.model[0].cloak_offset[0] = 0.0f;
		f1.model[0].cloak_offset[1] = b;
		f1.model[0].cloak_factor = (0x80u - 0x40u) << 24 | 0x808080u;
		draw_t(f1, sgpu, cloaked, st);
		std::string first;
		const uint32_t messages = debug_messages(g.info, first);
		check(messages == 0 && st.cloaks == 1, "cloak: drawn (" + std::to_string(st.cloaks) + "), the debug layer nothing (" + std::to_string(messages) + (messages ? ": " + first : "") + ")");
		// weight: alpha 0x40 / 255 = a; stage 0 sat(2a), stage 1 sat(4 x that x a): 0.504; colour 2 x 0.2 x 0x80/255;
		// T's alpha + weight^2
		const double a = 0x40 / 255.0, wgt = std::min(1.0, 4 * std::min(1.0, 2 * a) * a), add = 2 * 51 * (0x80 / 255.0) * wgt, add_a = wgt * wgt * 255;
		uint32_t covered = 0, top_right = 0, top = 0, bottom_right = 0, bottom = 0, alpha_right = 0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const uint8_t *p = &plain[(size_t(y) * W + x) * 4], *q = &cloaked[(size_t(y) * W + x) * 4];
				const int dr = q[0] - p[0], dg = q[1] - p[1];
				if (dr == 0 && dg == 0 && q[3] == p[3])
					continue;
				covered++;
				alpha_right += std::fabs(q[3] - std::min(255.0, p[3] + add_a)) <= 2.0;
				// (each pixel shifted half the screen down, wrapping: above the middle takes the bottom's green, below it
				// the top's red; a band round the middle left out, the shift being half the screen at the centre's depth)
				if (y < H / 2 - H / 10)
				{
					top++;
					top_right += std::abs(dg - add) <= 3.0 && dr == 0;
				}
				else if (y > H / 2 + H / 10)
				{
					bottom++;
					bottom_right += std::abs(dr - add) <= 3.0 && dg == 0;
				}
			}
		char line[300];
		std::snprintf(line, sizeof(line), "cloak: the scene's copy added with weight %.3f (%.1f on colour, %.1f on T's alpha) on %u pixels: above the middle the bottom's "
			"green %u of %u, below it the top's red %u of %u; alpha %u", wgt, add, add_a, covered, top_right, top, bottom_right, bottom, alpha_right);
		check(covered > 1000 && top > 100 && bottom > 100 && top_right >= top * 95 / 100 && bottom_right >= bottom * 95 / 100 && alpha_right >= covered * 95 / 100, line);
		// An opaque actor's cloak (weight 0) and no scene's copy: nothing drawn
		ch::frame &f2 = make_frame(46, now, 0x40808080u, ch::kNative | ch::kFromFlush, ch::kPassCloak);
		f2.model[0].cloak_factor = 0x808080u;
		std::vector<uint8_t> none;
		draw_t(f2, {}, none, st);
		check(st.cloaks == 0 && none == plain, "cloak: none without the scene's copy");
		scene->Release();
	}

	// ---- Through the see-through marker (charinject): the world's target copied, T, the composite
	{
		check(inj::prepare(g.device, why), "charinject: the renderer made" + (why.empty() ? "" : ": " + why));
		ID3D12DescriptorHeap *heap = ch::cache::heap();
		begin_list(g);
		inj::on_reset(g.list);
		inj::on_heaps(g.list, 1, &heap);
		inj::on_marker(ds);
		D3D12_RESOURCE_BARRIER planes[1] = {};
		planes[0].Transition.pResource = ds;
		planes[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		planes[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		planes[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		inj::on_barrier(1, planes);
		g.list->ClearRenderTargetView(rtv, kSky, 0, nullptr);
		g.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		ch::frame &f = make_frame(47, now, 0x40808080u, ch::kNative | ch::kFromFlush, ch::kPassCloak | ch::kPassOverlay);
		f.model[0].cloak_offset[1] = 0.05f;
		f.model[0].cloak_factor = (0x80u - 0x40u) << 24 | 0x808080u;
		inj::world w;
		w.rt = rt;
		w.ds = ds;
		w.rt_format = DXGI_FORMAT_R8G8B8A8_UNORM;
		w.ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		w.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		grass::list_state bound;
		inj::on_see_through(g.list, bound, w, 47 & 15);
		run(g);
		const std::vector<uint8_t> px = read_back(g, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, 4);
		std::string first;
		const uint32_t messages = debug_messages(g.info, first);
		uint32_t drawn = 0;
		for (size_t i = 0; i < size_t(W) * H; ++i)
			drawn += !is_sky(&px[i * 4]);
		const inj::counts c = inj::get_counts();
		check(messages == 0 && g.device->GetDeviceRemovedReason() == S_OK, "charinject: the debug layer nothing (" + std::to_string(messages) + (messages ? ": " + first : "") + ")");
		check(c.see_through == 1 && c.cloaks == 1 && c.overlays == 1 && c.cloak_failed == 0 && drawn > 1000, "charinject: the see-through model with its overlay and cloak (the world's "
			"target copied first) laid over the world: " + std::to_string(drawn) + " pixels; " + inj::status().back());
		inj::release();
	}

	// ---- The aura ring (038A80 / 038E10, model_entry::aura) through the see-through marker: three copies, two moved 20
	// pixels right and left in red (0x40 alpha: x 4, full where T is), the last the solid one: beside the model where it
	// alone leaves the sky, the world turned red (its green and blue gone, partly on the copy's soft edge)
	{
		const auto draw_see_through = [&](uint32_t seq, uint8_t aura, std::vector<uint8_t> &px) {
			std::string why_;
			check(inj::prepare(g.device, why_), "aura: the renderer made" + (why_.empty() ? "" : ": " + why_));
			ID3D12DescriptorHeap *heap = ch::cache::heap();
			begin_list(g);
			inj::on_reset(g.list);
			inj::on_heaps(g.list, 1, &heap);
			inj::on_marker(ds);
			D3D12_RESOURCE_BARRIER planes[1] = {};
			planes[0].Transition.pResource = ds;
			planes[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			planes[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
			planes[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
			inj::on_barrier(1, planes);
			g.list->ClearRenderTargetView(rtv, kSky, 0, nullptr);
			g.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ch::frame &f = make_frame(seq, now, 0x80808080u, ch::kNative | ch::kFromFlush, 0);
			ch::model_entry &m = f.model[0];
			m.aura = aura;
			const float offsets[3][2] = { { 20.0f, 0.0f }, { -20.0f, 0.0f }, { 0.0f, 0.0f } };
			std::memcpy(m.aura_offset, offsets, sizeof(offsets));
			m.aura_colour[0] = m.aura_colour[1] = 0x40FF0000u;
			inj::world w;
			w.rt = rt;
			w.ds = ds;
			w.rt_format = DXGI_FORMAT_R8G8B8A8_UNORM;
			w.ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			w.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
			grass::list_state bound;
			inj::on_see_through(g.list, bound, w, seq & 15);
			run(g);
			px = read_back(g, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, 4);
		};
		std::vector<uint8_t> plain, ringed;
		draw_see_through(48, 0, plain);
		draw_see_through(49, 3, ringed);
		std::string first;
		const uint32_t messages = debug_messages(g.info, first);
		const inj::counts c = inj::get_counts();
		uint32_t halo = 0, red = 0, full = 0, model = 0;
		for (size_t i = 0; i < size_t(W) * H; ++i)
		{
			const uint8_t *a = &plain[i * 4], *b = &ringed[i * 4];
			model += !is_sky(a);
			if (!is_sky(a) || is_sky(b))
				continue;
			halo++;
			// (its green and blue going: the copy's red over the sky, whole where T is whole, less on its softened edge)
			red += b[1] < 69 && b[2] < 95 && b[0] >= b[1] ? 1 : 0;
			full += b[1] < 8 && b[2] < 8 ? 1 : 0;
		}
		check(messages == 0 && g.device->GetDeviceRemovedReason() == S_OK, "aura: the debug layer nothing (" + std::to_string(messages) + (messages ? ": " + first : "") + ")");
		check(c.auras == 1 && c.aura_failed == 0 && model > 1000 && halo > model / 10 && red >= halo * 9 / 10 && full >= halo / 2,
			"aura: the ring beside the model (" + std::to_string(halo) + " pixels where it alone leaves the sky, " + std::to_string(red) + " of them reddened, " + std::to_string(full) + " all red; the model " +
				std::to_string(model) + ")");
		inj::release();
	}

	renderer.release();
	ch::cache::stop();
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
