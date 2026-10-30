/*
 * Offline test of the native effect renderer (src/fx: fxinject at an fx_run marker, fxrender, fxtextures, fxgeo) on
 * WARP with the debug layer, against research/ffxi-re/tools/effects_play.py through tests/fx_oracle.py play: an
 * action's effect run by the player to one frame, its elements (far to near) with their models' and textures' chunks
 * and the picture the player's rasteriser makes of them on black.
 *
 * Each oracle's elements become a flush's frame as src/game/fxdraw records one (WORLD from the player's view-space
 * matrix, TEXTUREFACTOR as it computes it, the blend by the letter, z write by flags bit 12, the class's stages and
 * geometry from the chunks through fxgeo), one run; the textures go through fxtextures (decoded on its thread, copied
 * in on the list at the marker). An MMB effect mesh is drawn from its DAT's data on the GPU as the native zone
 * renderer has it (zonemesh::load_file, gpu_zone uploaded on the test's queue): its pieces by the model's name. The marker is handled on a list recorded as dgVoodoo records (its own root signature,
 * heap and targets bound), which must be bound again after. The picture must match the oracle's: the mean difference
 * per channel and the share of pixels off by more than 24 (of 255) small (8-bit blending and the rasteriser's own
 * rounding differ, never the shapes, colours or order).
 *
 *   build\fx_render_test.exe <oracle.bin> [...]   (writes <oracle>.ours.bmp and <oracle>.ref.bmp beside each)
 */

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include "../src/fx/fxframe.hpp"
#include "../src/fx/fxgeo.hpp"
#include "../src/fx/fxinject.hpp"
#include "../src/fx/fxtextures.hpp"
#include "../src/chars/tex.hpp"
#include "../src/zone/zone_gpu.hpp"
#include "game_dump.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
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

	void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = r;
		b.Transition.Subresource = sub;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		list->ResourceBarrier(1, &b);
	}

	void write_bmp(const std::string &path, const uint8_t *rgb, uint32_t w, uint32_t h, size_t stride, size_t pitch)
	{
		FILE *f = nullptr;
		if (fopen_s(&f, path.c_str(), "wb") != 0 || f == nullptr)
			return;
		uint8_t head[54] = { 'B', 'M' };
		auto put = [&](int at, uint32_t v) { std::memcpy(head + at, &v, 4); };
		put(2, 54 + w * h * 4);
		put(10, 54);
		put(14, 40);
		put(18, w);
		put(22, uint32_t(-int32_t(h)));
		head[26] = 1;
		head[28] = 32;
		fwrite(head, 1, 54, f);
		for (uint32_t y = 0; y < h; ++y)
			for (uint32_t x = 0; x < w; ++x)
			{
				const uint8_t *q = rgb + y * pitch + x * stride;
				const uint8_t o[4] = { q[2], q[1], q[0], 255 };
				fwrite(o, 1, 4, f);
			}
		fclose(f);
	}

	// An oracle: its elements and chunks
	struct oracle_element
	{
		uint32_t cls = 0, tf = 0, letter = 0, flags = 0, frame = 0;
		float uv[2] = {}, m[9] = {}, t[3] = {};
		uint32_t model = 0xFFFFFFFFu;
		std::vector<uint32_t> textures;
		float weights[5] = {};
		uint32_t segments = 0, rings = 0;
		float radii[4] = {};
		uint32_t colours[4] = {};
	};
	struct oracle
	{
		uint32_t w = 0, h = 0;
		float focal = 0.0f;
		std::vector<std::vector<uint8_t>> blobs;
		std::vector<oracle_element> elements;
		std::vector<uint8_t> picture; // w x h x 3
	};

	bool load(const std::string &path, oracle &o)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f)
			return false;
		std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		size_t p = 0;
		auto u32 = [&]() { uint32_t v = 0; if (p + 4 <= b.size()) std::memcpy(&v, &b[p], 4); p += 4; return v; };
		auto f32 = [&]() { float v = 0; if (p + 4 <= b.size()) std::memcpy(&v, &b[p], 4); p += 4; return v; };
		if (b.size() < 24 || std::memcmp(b.data(), "FXO1", 4) != 0)
			return false;
		p = 4;
		o.w = u32();
		o.h = u32();
		o.focal = f32();
		const uint32_t count = u32(), blobs = u32();
		for (uint32_t i = 0; i < blobs; ++i)
		{
			const uint32_t n = u32();
			if (p + n > b.size())
				return false;
			o.blobs.emplace_back(b.begin() + p, b.begin() + p + n);
			p += n;
		}
		for (uint32_t i = 0; i < count; ++i)
		{
			oracle_element e;
			e.cls = u32();
			e.tf = u32();
			e.letter = u32();
			e.flags = u32();
			e.uv[0] = f32();
			e.uv[1] = f32();
			e.frame = u32();
			for (float &x : e.m)
				x = f32();
			for (float &x : e.t)
				x = f32();
			e.model = u32();
			const uint32_t nt = u32();
			for (uint32_t k = 0; k < nt; ++k)
				e.textures.push_back(u32());
			for (float &x : e.weights)
				x = f32();
			e.segments = u32();
			e.rings = u32();
			for (float &x : e.radii)
				x = f32();
			for (uint32_t &x : e.colours)
				x = u32();
			o.elements.push_back(e);
		}
		const size_t px = size_t(o.w) * o.h * 3;
		if (p + px != b.size())
			return false;
		o.picture.assign(b.begin() + p, b.end());
		return true;
	}

	// The test's texture ids by blob (each blob a texture chunk's data), and their sizes
	struct tex_entry
	{
		uint32_t id = fx::kNone, w = 0, h = 0;
	};
	std::map<std::pair<const oracle *, uint32_t>, tex_entry> g_tex;
	uint32_t g_next_resource = 0x10000;

	// The DATs' data on the GPU (an MMB mesh's), by file id
	ID3D12Device *g_device = nullptr;
	ID3D12CommandQueue *g_queue = nullptr;
	std::map<uint32_t, std::unique_ptr<hifi::zone::gpu_zone>> g_zones;

	hifi::zone::gpu_zone *zone_data(uint32_t file)
	{
		auto it = g_zones.find(file);
		if (it != g_zones.end())
			return it->second.get();
		auto z = std::make_unique<hifi::zone::gpu_zone>();
		hifi::bake::zonemesh::zone d;
		hifi::bake::result r;
		std::string why;
		if (!hifi::bake::zonemesh::load_file(file, d, r) || !z->upload(g_device, g_queue, std::move(d), why) || !z->wait(60000))
		{
			check(false, "DAT " + std::to_string(file) + "'s data: " + r.why + why);
			z.reset();
		}
		return (g_zones[file] = std::move(z)).get();
	}

	tex_entry texture(const oracle &o, uint32_t blob)
	{
		if (blob >= o.blobs.size())
			return tex_entry();
		const auto k = std::make_pair(&o, blob);
		auto it = g_tex.find(k);
		if (it != g_tex.end())
			return it->second;
		tex_entry t;
		hifi::chars::tex::texture dec;
		std::string why;
		if (hifi::chars::tex::decode(o.blobs[blob].data(), o.blobs[blob].size(), dec, why))
		{
			t.w = dec.w;
			t.h = dec.h;
			fx::textures::key key;
			key.resource = g_next_resource;
			g_next_resource += 0x100;
			key.handle = blob;
			key.header = 0x20;
			t.id = fx::textures::add(key, std::vector<uint8_t>(o.blobs[blob]), 1.0);
		}
		return g_tex[k] = t;
	}

	// The oracle's elements as a flush's frame (one run), as the game side records them
	uint32_t build(const oracle &o, fx::frame &f, uint32_t seq, uint32_t &skipped)
	{
		f.seq.store(0);
		f.runs.store(0);
		f.element_count = f.draw_count = f.vertex_count = 0;
		// PROJECTION: RH, the player's focal length; near 0.05 (its rasteriser drops triangles nearer)
		const float n = 0.05f, fz = 1000.0f;
		const float p[16] = { 2.0f * o.focal / float(o.w), 0, 0, 0, 0, 2.0f * o.focal / float(o.h), 0, 0, 0, 0, fz / (n - fz), -1, 0, 0, n * fz / (n - fz), 0 };
		std::memcpy(f.proj, p, 64);
		skipped = 0;
		for (const oracle_element &oe : o.elements)
		{
			fx::element e;
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 3; ++c)
					e.world[r * 4 + c] = oe.m[r * 3 + c];
				e.world[r * 4 + 3] = 0.0f;
			}
			e.world[12] = oe.t[0];
			e.world[13] = oe.t[1];
			e.world[14] = oe.t[2];
			e.world[15] = 1.0f;
			e.tfactor = oe.tf;
			uint32_t ignored = 0;
			uint8_t blend = fx::blend_add;
			if (!fx::letter_blend(uint16_t(oe.letter), ignored, blend))
			{
				skipped++;
				continue;
			}
			e.blend = blend;
			e.z_write = (oe.flags >> 12) & 1;
			e.uv[0] = oe.uv[0];
			e.uv[1] = oe.uv[1];
			std::vector<fx::vertex> verts;
			std::vector<fx::draw> draws;
			std::string why;
			const bool bit28 = (oe.flags & (1u << 28)) != 0;
			if (oe.cls == 1 && oe.model < o.blobs.size() && o.blobs[oe.model].size() == 24 && std::memcmp(o.blobs[oe.model].data(), "MMB1", 4) == 0)
			{
				// (as fxdraw's record_mmb: the model's pieces in file order, untextured or by 183760's textured mode)
				uint32_t file = 0;
				std::memcpy(&file, o.blobs[oe.model].data() + 4, 4);
				hifi::zone::gpu_zone *const z = zone_data(file);
				const char *name = reinterpret_cast<const char *>(o.blobs[oe.model].data() + 8);
				const uint32_t model = z != nullptr ? z->data().find(name) : hifi::bake::zonemesh::kNone;
				if (model == hifi::bake::zonemesh::kNone)
				{
					skipped++;
					continue;
				}
				const auto &d = z->data();
				const auto &m = d.models[model];
				const bool mode = (oe.letter & 0xFF) == 0x64 || bit28;
				for (uint32_t i = m.first_piece; i < m.first_piece + m.pieces; ++i)
				{
					const auto &pc = d.pieces[i];
					fx::draw dr;
					dr.first = pc.first_index;
					dr.count = pc.triangles * 3;
					dr.texture = pc.texture == hifi::bake::zonemesh::kNone ? fx::kNone : pc.texture;
					dr.stages = fx::stage_zone_mesh | (pc.texture == hifi::bake::zonemesh::kNone ? 0u : mode ? fx::stage_mmb_textured_d : fx::stage_mmb_textured);
					draws.push_back(dr);
				}
				e.zone = z;
				e.cls = fx::class_mmb;
				if (f.draw_count + draws.size() > fx::kMaxDraws)
				{
					skipped++;
					continue;
				}
				e.first_draw = f.draw_count;
				e.draws = uint32_t(draws.size());
				for (const fx::draw &d0 : draws)
					f.draws[f.draw_count++] = d0;
				f.elements[f.element_count++] = e;
				continue;
			}
			if (oe.cls == 0 && oe.model < o.blobs.size())
			{
				geo::d3m m;
				if (!geo::parse_d3m(o.blobs[oe.model].data(), o.blobs[oe.model].size(), m, why))
				{
					skipped++;
					continue;
				}
				verts = m.vertices;
				bool ok = true;
				for (const geo::d3m_run &r : m.runs)
				{
					fx::draw d;
					d.first = r.first;
					d.count = r.count;
					if (r.texture >= 0)
					{
						const tex_entry t = size_t(r.texture) < oe.textures.size() ? texture(o, oe.textures[size_t(r.texture)]) : tex_entry();
						if (t.id == fx::kNone)
							ok = false;
						d.texture = t.id;
						d.stages = fx::stage_texture | (bit28 ? fx::stage_alpha_diffuse : 0);
					}
					else
						d.stages = m.textures == 0 && (oe.letter & 0xFF) == 0x44 ? fx::stage_alpha_2x : 0;
					draws.push_back(d);
				}
				if (!ok)
				{
					skipped++;
					continue;
				}
				e.cls = fx::class_d3m;
			}
			else if (oe.cls == 2 && oe.model < o.blobs.size())
			{
				geo::d3a a;
				if (!geo::parse_d3a(o.blobs[oe.model].data(), o.blobs[oe.model].size(), a, why) || a.frame_list.empty() || oe.textures.empty())
				{
					skipped++;
					continue;
				}
				const tex_entry t = texture(o, oe.textures[0]);
				if (t.id == fx::kNone)
				{
					skipped++;
					continue;
				}
				verts = a.frame_list[oe.frame % a.frame_list.size()].vertices;
				// (03E6B0: UVs in texels when +0 > 0 and +7 is 0)
				if (a.units > 0 && a.uvs_unit == 0)
					for (fx::vertex &v : verts)
					{
						v.uv[0] /= float(t.w);
						v.uv[1] /= float(t.h);
					}
				for (fx::vertex &v : verts)
					for (float &u : v.uv)
						if (!std::isfinite(u))
							u = 0.0f;
				fx::draw d;
				d.count = uint32_t(verts.size());
				d.texture = t.id;
				d.stages = fx::stage_texture | (bit28 ? fx::stage_alpha_diffuse : 0) | ((oe.letter & 0xFF) == 0x40 ? fx::stage_alpha_tfactor : 0);
				draws.push_back(d);
				e.z_write = 0;
				e.cls = fx::class_d3a;
			}
			else if (oe.cls == 4 && oe.model < o.blobs.size())
			{
				geo::morph m;
				if (!geo::parse_morph(o.blobs[oe.model].data(), o.blobs[oe.model].size(), m, why) || oe.textures.empty())
				{
					skipped++;
					continue;
				}
				const tex_entry t = texture(o, oe.textures[0]);
				m.blend(oe.weights, verts);
				fx::draw d;
				d.count = uint32_t(verts.size());
				d.texture = t.id;
				d.stages = t.id != fx::kNone ? fx::stage_texture : 0;
				draws.push_back(d);
				e.cls = fx::class_morph;
			}
			else if (oe.cls == 5 && oe.segments > 0 && oe.rings >= 2)
			{
				// (0565A0: strips in the XY plane, angles 0, 0, d .. (n-2)d, 0; untextured, 353810)
				std::vector<float> angles = { 0.0f };
				for (uint32_t k = 0; k + 1 < oe.segments; ++k)
					angles.push_back(float(k) * 6.283184f / float(oe.segments));
				angles.push_back(0.0f);
				for (uint32_t band = 0; band + 1 < oe.rings && band < 3; ++band)
				{
					std::vector<fx::vertex> strip;
					for (const float a : angles)
						for (uint32_t side = 0; side < 2; ++side)
						{
							const float r = oe.radii[band + side];
							const uint32_t c = oe.colours[band + side];
							fx::vertex v = {};
							v.pos[0] = std::sin(a) * r;
							v.pos[1] = std::cos(a) * r;
							v.colour = (c & 0xFF000000u) | (c & 0xFF) << 16 | (c & 0xFF00) | ((c >> 16) & 0xFF);
							strip.push_back(v);
						}
					for (size_t k = 0; k + 2 < strip.size(); ++k)
						for (size_t j = 0; j < 3; ++j)
							verts.push_back(strip[k + j]);
				}
				fx::draw d;
				d.count = uint32_t(verts.size());
				draws.push_back(d);
				e.cls = fx::class_ring;
			}
			else
			{
				skipped++;
				continue;
			}
			if (f.vertex_count + verts.size() > fx::kMaxVertices || f.draw_count + draws.size() > fx::kMaxDraws)
			{
				skipped++;
				continue;
			}
			std::memcpy(f.vertices + f.vertex_count, verts.data(), verts.size() * sizeof(fx::vertex));
			e.first_draw = f.draw_count;
			e.draws = uint32_t(draws.size());
			for (fx::draw d : draws)
			{
				d.first += f.vertex_count;
				f.draws[f.draw_count++] = d;
			}
			f.vertex_count += uint32_t(verts.size());
			f.elements[f.element_count++] = e;
		}
		f.run_list[0].first = 0;
		f.run_list[0].count = f.element_count;
		f.runs.store(1);
		f.seq.store(seq);
		return f.element_count;
	}
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	// (the dump mapped first, before Direct3D's DLLs could take its place: zonemesh unscrambles MMBs with the game's
	// key tables)
	uintptr_t dump_base = 0;
	const bool mapped = dump::map(dump::kDumps[0], dump_base) != 0;
	check(mapped, "the dump mapped (the MMBs' key tables)");
	ID3D12Debug *debug = nullptr;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
		debug->EnableDebugLayer();
	IDXGIFactory4 *factory = nullptr;
	CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	IDXGIAdapter *warp = nullptr;
	factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
	ID3D12Device *device = nullptr;
	if (FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
	{
		check(false, "a WARP device");
		return 1;
	}
	ID3D12InfoQueue *info = nullptr;
	device->QueryInterface(IID_PPV_ARGS(&info));
	std::string why;
	check(fx::inject::prepare(device, why), "the renderer made" + (why.empty() ? std::string() : ": " + why));
	ID3D12CommandQueue *queue = nullptr;
	const D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
	g_device = device;
	g_queue = queue;
	ID3D12Fence *fence = nullptr;
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	uint64_t fence_value = 0;
	const D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, rb = { D3D12_HEAP_TYPE_READBACK };

	uint32_t seq = 100;
	for (int a = 1; a < argc; ++a)
	{
		const std::string path = argv[a];
		const std::string name = path.substr(path.find_last_of("\\/") + 1);
		oracle o;
		if (!load(path, o))
		{
			check(false, name + ": the oracle read");
			continue;
		}
		const UINT W = o.w, H = o.h;
		// The world: A (RGBA8), its depth (D24S8), a readback
		D3D12_RESOURCE_DESC td = {};
		td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		td.Width = W;
		td.Height = H;
		td.DepthOrArraySize = 1;
		td.MipLevels = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		ID3D12Resource *A = nullptr, *Az = nullptr, *back = nullptr;
		D3D12_CLEAR_VALUE cv = {};
		cv.Format = td.Format;
		device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&A));
		D3D12_RESOURCE_DESC dd = td;
		dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		D3D12_CLEAR_VALUE dv = {};
		dv.Format = dd.Format;
		dv.DepthStencil.Depth = 1.0f;
		device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dv, IID_PPV_ARGS(&Az));
		const UINT pitch = (W * 4 + 255) & ~255u;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = UINT64(pitch) * H;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&back));
		ID3D12DescriptorHeap *rtvh = nullptr, *dsvh = nullptr, *game_heap = nullptr;
		D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
		device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh));
		hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
		device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvh));
		hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&game_heap));
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvh->GetCPUDescriptorHandleForHeapStart(), dsv = dsvh->GetCPUDescriptorHandleForHeapStart();
		device->CreateRenderTargetView(A, nullptr, rtv);
		device->CreateDepthStencilView(Az, nullptr, dsv);

		// The frame, its textures decoded (ids by blob: this oracle's own)
		g_tex.clear();
		fx::frame &f = fx::frame_slot(++seq);
		uint32_t skipped = 0;
		const uint32_t built = build(o, f, seq, skipped);
		check(fx::textures::settle(20000), name + ": its textures decoded");
		check(built != 0 && skipped == 0, name + ": " + std::to_string(built) + " of " + std::to_string(o.elements.size()) + " elements made into a frame (" +
			std::to_string(skipped) + " not)");

		// The list as dgVoodoo records it: cleared, its own heap and targets, then the marker
		ID3D12CommandAllocator *alloc = nullptr;
		ID3D12GraphicsCommandList *list = nullptr;
		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
		device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
		const float black[4] = { 0, 0, 0, 0 };
		list->ClearRenderTargetView(rtv, black, 0, nullptr);
		list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		list->SetDescriptorHeaps(1, &game_heap);
		list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
		const D3D12_VIEWPORT vp = { 0, 0, float(W), float(H), 0, 1 };
		const D3D12_RECT sc = { 0, 0, LONG(W), LONG(H) };
		list->RSSetViewports(1, &vp);
		list->RSSetScissorRects(1, &sc);
		grass::list_state bound;
		bound.rtvs[0] = rtv;
		bound.rtv_count = 1;
		bound.dsv = dsv;
		bound.viewports[0] = vp;
		bound.viewport_count = 1;
		bound.scissors[0] = sc;
		bound.scissor_count = 1;
		hifi::passes::world w;
		w.rt = A;
		w.ds = Az;
		w.rt_format = DXGI_FORMAT_R8G8B8A8_UNORM;
		w.ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		w.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		w.planes_known = true;
		w.ds_planes[0] = w.ds_planes[1] = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		w.heap_count = 1;
		w.heaps[0] = game_heap;
		const fx::inject::counts before = fx::inject::get_counts();
		fx::inject::on_run(list, bound, w, seq & 15);
		const fx::inject::counts after = fx::inject::get_counts();
		// (the marker's own clear, as dgVoodoo hands it on: the state must be the game's again)
		const D3D12_RECT pixel = { LONG(16 * 5 + (seq & 15)), 0, LONG(16 * 5 + (seq & 15) + 1), 1 };
		list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 1, &pixel);
		barrier(list, A, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
		dst.pResource = back;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, pitch };
		src.pResource = A;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		check(SUCCEEDED(list->Close()), name + ": the list closes");
		ID3D12CommandList *lists[] = { list };
		queue->ExecuteCommandLists(1, lists);
		queue->Signal(fence, ++fence_value);
		HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		fence->SetEventOnCompletion(fence_value, ev);
		WaitForSingleObject(ev, 60000);
		CloseHandle(ev);
		check(after.runs == before.runs + 1 && after.elements - before.elements == built && after.failed == before.failed,
			name + ": the marker drew its run (" + std::to_string(after.elements - before.elements) + " elements, " + std::to_string(after.draws - before.draws) + " draws, " +
			std::to_string(after.textures - before.textures) + " textures copied in)");
		std::string first;
		const uint32_t messages = debug_messages(info, first);
		check(messages == 0, name + ": the debug layer: nothing (" + std::to_string(messages) + (messages != 0 ? ": " + first : std::string()) + ")");
		check(device->GetDeviceRemovedReason() == S_OK, name + ": the device still there");

		// The picture against the oracle's
		uint8_t *px = nullptr;
		back->Map(0, nullptr, reinterpret_cast<void **>(&px));
		double total = 0.0;
		uint64_t off = 0, lit_ref = 0, lit_ours = 0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const uint8_t *q = px + size_t(y) * pitch + size_t(x) * 4;
				const uint8_t *r = o.picture.data() + (size_t(y) * W + x) * 3;
				int worst = 0;
				for (int c = 0; c < 3; ++c)
				{
					const int d = std::abs(int(q[c]) - int(r[c]));
					total += d;
					worst = std::max(worst, d);
				}
				off += worst > 24;
				lit_ref += r[0] + r[1] + r[2] > 24;
				lit_ours += q[0] + q[1] + q[2] > 24;
			}
		const double mean = total / (double(W) * H * 3.0), share = double(off) / (double(W) * H);
		const std::string base = path.substr(0, path.find_last_of('.'));
		write_bmp(base + ".ours.bmp", px, W, H, 4, pitch);
		write_bmp(base + ".ref.bmp", o.picture.data(), W, H, 3, size_t(W) * 3);
		back->Unmap(0, nullptr);
		char line[300];
		std::snprintf(line, sizeof(line), "%s: the picture as effects_play's (mean difference %.2f of 255, %.2f%% of pixels off by more than 24; lit pixels %llu, the oracle's %llu)",
			name.c_str(), mean, share * 100.0, static_cast<unsigned long long>(lit_ours), static_cast<unsigned long long>(lit_ref));
		check(lit_ref > 100 && mean < 1.5 && share < 0.01, line);
		alloc->Release();
		list->Release();
		A->Release();
		Az->Release();
		back->Release();
		rtvh->Release();
		dsvh->Release();
		game_heap->Release();
	}
	// The blend table and the trails' stage on their own: three screen-filling quads (WORLD and PROJECTION identity) on
	// black: a weapon trail's (vertex colour only, SRCALPHA / ONE: 200 100 50), a subtractive letter's over it (B:
	// REVSUBTRACT, the untextured stage: 0x20 x 0x80 x 2 = 0x20 each, alpha 0xFF x 0x80 x 4 = full), a darkening one's
	// (F: ZERO / INVSRCALPHA, alpha 0x40 x 0x40 x 4 = 0x40): (200 - 32) x (1 - 0x40 / 255), ...
	{
		const UINT W = 64, H = 64;
		D3D12_RESOURCE_DESC td = {};
		td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		td.Width = W;
		td.Height = H;
		td.DepthOrArraySize = 1;
		td.MipLevels = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		ID3D12Resource *A = nullptr, *Az = nullptr, *back = nullptr;
		D3D12_CLEAR_VALUE cv = {};
		cv.Format = td.Format;
		device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&A));
		D3D12_RESOURCE_DESC dd = td;
		dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		D3D12_CLEAR_VALUE dv = {};
		dv.Format = dd.Format;
		dv.DepthStencil.Depth = 1.0f;
		device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dv, IID_PPV_ARGS(&Az));
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = 256 * H;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&back));
		ID3D12DescriptorHeap *rtvh = nullptr, *dsvh = nullptr, *game_heap = nullptr;
		D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
		device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh));
		hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
		device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvh));
		hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&game_heap));
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvh->GetCPUDescriptorHandleForHeapStart(), dsv = dsvh->GetCPUDescriptorHandleForHeapStart();
		device->CreateRenderTargetView(A, nullptr, rtv);
		device->CreateDepthStencilView(Az, nullptr, dsv);
		fx::frame &f = fx::frame_slot(++seq);
		f.seq.store(0);
		f.element_count = f.draw_count = f.vertex_count = 0;
		const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		std::memcpy(f.proj, id, 64);
		struct quad
		{
			uint32_t colour, tfactor, stages;
			uint8_t blend;
		};
		const quad quads[] = { { 0xFFC86432u, 0, fx::stage_diffuse, fx::blend_add }, { 0xFF202020u, 0x80808080u, 0, fx::blend_subtract },
			{ 0x40000000u, 0x40808080u, 0, fx::blend_darken } };
		for (const quad &q : quads)
		{
			const float c[6][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { -1, 1 }, { 1, -1 }, { 1, 1 } };
			fx::element e;
			std::memcpy(e.world, id, 64);
			e.tfactor = q.tfactor;
			e.blend = q.blend;
			e.first_draw = f.draw_count;
			e.draws = 1;
			f.draws[f.draw_count++] = { f.vertex_count, 6, fx::kNone, q.stages };
			for (const auto &p : c)
			{
				fx::vertex v = {};
				v.pos[0] = p[0];
				v.pos[1] = p[1];
				v.pos[2] = 0.5f;
				v.colour = q.colour;
				f.vertices[f.vertex_count++] = v;
			}
			f.elements[f.element_count++] = e;
		}
		f.run_list[0] = { 0, f.element_count };
		f.runs.store(1);
		f.seq.store(seq);
		ID3D12CommandAllocator *alloc = nullptr;
		ID3D12GraphicsCommandList *list = nullptr;
		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
		device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
		const float black[4] = { 0, 0, 0, 0 };
		list->ClearRenderTargetView(rtv, black, 0, nullptr);
		list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		list->SetDescriptorHeaps(1, &game_heap);
		grass::list_state bound;
		hifi::passes::world w;
		w.rt = A;
		w.ds = Az;
		w.rt_format = DXGI_FORMAT_R8G8B8A8_UNORM;
		w.ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		w.planes_known = true;
		w.ds_planes[0] = w.ds_planes[1] = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		w.heap_count = 1;
		w.heaps[0] = game_heap;
		fx::inject::on_run(list, bound, w, seq & 15);
		barrier(list, A, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
		dst.pResource = back;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
		src.pResource = A;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		list->Close();
		ID3D12CommandList *lists[] = { list };
		queue->ExecuteCommandLists(1, lists);
		queue->Signal(fence, ++fence_value);
		HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		fence->SetEventOnCompletion(fence_value, ev);
		WaitForSingleObject(ev, 60000);
		CloseHandle(ev);
		uint8_t *px = nullptr;
		back->Map(0, nullptr, reinterpret_cast<void **>(&px));
		const uint8_t *m = px + 32 * 256 + 32 * 4;
		const double keep = 1.0 - 64.0 / 255.0;
		const int want[3] = { int((200 - 32) * keep + 0.5), int((100 - 32) * keep + 0.5), int((50 - 32) * keep + 0.5) };
		char line[200];
		std::snprintf(line, sizeof(line), "the blend table: a trail's colour added, B subtracted, F darkening: %d %d %d (want %d %d %d, within 2)", m[0], m[1], m[2], want[0], want[1],
			want[2]);
		check(std::abs(m[0] - want[0]) <= 2 && std::abs(m[1] - want[1]) <= 2 && std::abs(m[2] - want[2]) <= 2, line);
		back->Unmap(0, nullptr);
		std::string first;
		const uint32_t messages = debug_messages(info, first);
		check(messages == 0, "the blend table: the debug layer: nothing (" + std::to_string(messages) + (messages != 0 ? ": " + first : std::string()) + ")");

		// A distortion quad by value (0426F0): the world red left of x 36, blue right; a CMoDist diamond of half the screen
		// (WORLD scale 0.5, PROJECTION identity) whose grab is the unshifted diamond's (its screen points: centre (32, 32),
		// 1 (16, 32), 2 (32, 48), 3 (48, 32), 4 (32, 16); box 16 .. 48, so moved by 16, scale 1; UVs / 64), drawn shifted
		// right by 0.25 (8 pixels): at (40, 32) the fan shows the world from (32, 32): red where the world is blue; alpha
		// there the centre's (0xFF x 0x80 x 4: full) and the colour the world's x 0x80 x 2; outside the fan the world.
		{
			fx::frame &fd = fx::frame_slot(++seq);
			fd.seq.store(0);
			fd.element_count = fd.draw_count = fd.vertex_count = fd.light_count = fd.grab_count = 0;
			std::memcpy(fd.proj, id, 64);
			fx::element e;
			const float wm[16] = { 0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0.25f, 0, 0.5f, 1 };
			std::memcpy(e.world, wm, 64);
			e.tfactor = 0x80808080u;
			e.blend = fx::blend_alpha;
			e.cls = fx::class_dist;
			e.first_draw = 0;
			e.draws = 1;
			const float moved[5][2] = { { 16, 16 }, { 0, 16 }, { 16, 32 }, { 32, 16 }, { 16, 0 } };
			const float screen[5][2] = { { 32, 32 }, { 16, 32 }, { 32, 48 }, { 48, 32 }, { 32, 16 } };
			const float dia[5][2] = { { 0, 0 }, { -1, 0 }, { 0, -1 }, { 1, 0 }, { 0, 1 } };
			const int strip[4] = { 1, 2, 4, 3 };
			fx::grab &gr = fd.grabs[fd.grab_count++];
			for (int k = 0; k < 4; ++k)
			{
				gr.pos[k][0] = moved[strip[k]][0];
				gr.pos[k][1] = moved[strip[k]][1];
				gr.uv[k][0] = screen[strip[k]][0] / 64.0f;
				gr.uv[k][1] = screen[strip[k]][1] / 64.0f;
			}
			for (int k = 1; k <= 4; ++k)
				for (const int p : { 0, k, k == 4 ? 1 : k + 1 })
				{
					fx::vertex v = {};
					v.pos[0] = dia[p][0];
					v.pos[1] = dia[p][1];
					v.colour = p == 0 ? 0xFF808080u : 0x00808080u;
					v.uv[0] = moved[p][0] / 255.0f;
					v.uv[1] = moved[p][1] / 255.0f;
					fd.vertices[fd.vertex_count++] = v;
				}
			fx::draw dr;
			dr.first = 0;
			dr.count = 12;
			dr.texture = fx::kDistTexture;
			dr.stages = fx::stage_dist;
			dr.grab = 0;
			fd.draws[fd.draw_count++] = dr;
			fd.elements[fd.element_count++] = e;
			fd.run_list[0] = { 0, 1 };
			fd.runs.store(1);
			fd.seq.store(seq);
			ID3D12CommandAllocator *da = nullptr;
			ID3D12GraphicsCommandList *dl = nullptr;
			device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&da));
			device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, da, nullptr, IID_PPV_ARGS(&dl));
			barrier(dl, A, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET); // (as the blend table left it; left so again)
			const float red[4] = { 1, 0, 0, 1 }, blue[4] = { 0, 0, 1, 1 };
			const D3D12_RECT left = { 0, 0, 36, LONG(H) }, right = { 36, 0, LONG(W), LONG(H) };
			dl->ClearRenderTargetView(rtv, red, 1, &left);
			dl->ClearRenderTargetView(rtv, blue, 1, &right);
			dl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			dl->SetDescriptorHeaps(1, &game_heap);
			grass::list_state dbound;
			hifi::passes::world dw;
			dw.rt = A;
			dw.ds = Az;
			dw.rt_format = DXGI_FORMAT_R8G8B8A8_UNORM;
			dw.ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			dw.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
			dw.planes_known = true;
			dw.ds_planes[0] = dw.ds_planes[1] = D3D12_RESOURCE_STATE_DEPTH_WRITE;
			dw.heap_count = 1;
			dw.heaps[0] = game_heap;
			fx::inject::on_run(dl, dbound, dw, seq & 15);
			barrier(dl, A, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			D3D12_TEXTURE_COPY_LOCATION ddst = {}, dsrc = {};
			ddst.pResource = back;
			ddst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			ddst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
			dsrc.pResource = A;
			dsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dl->CopyTextureRegion(&ddst, 0, 0, 0, &dsrc, nullptr);
			dl->Close();
			ID3D12CommandList *dls[] = { dl };
			queue->ExecuteCommandLists(1, dls);
			queue->Signal(fence, ++fence_value);
			HANDLE dev_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			fence->SetEventOnCompletion(fence_value, dev_);
			WaitForSingleObject(dev_, 60000);
			CloseHandle(dev_);
			uint8_t *dp = nullptr;
			back->Map(0, nullptr, reinterpret_cast<void **>(&dp));
			const uint8_t *at40 = dp + 32 * 256 + 40 * 4, *at60 = dp + 32 * 256 + 60 * 4, *at2 = dp + 2 * 256 + 2 * 4;
			char dline[240];
			std::snprintf(dline, sizeof(dline), "a distortion quad: at (40, 32) the world from (32, 32): %d %d %d (want red over blue); outside: %d %d %d, %d %d %d", at40[0], at40[1], at40[2],
				at60[0], at60[1], at60[2], at2[0], at2[1], at2[2]);
			check(at40[0] > 220 && at40[2] < 40 && at60[2] == 255 && at60[0] == 0 && at2[0] == 255 && at2[2] == 0, dline);
			back->Unmap(0, nullptr);
			const fx::inject::counts dc = fx::inject::get_counts();
			std::string dfirst;
			const uint32_t dmessages = debug_messages(info, dfirst);
			check(dmessages == 0 && dc.failed == 0, "a distortion quad: the debug layer: nothing (" + std::to_string(dmessages) + (dmessages != 0 ? ": " + dfirst : std::string()) + ")");
			dl->Release();
			da->Release();
		}

		// The 042D00 distortions by value: the world red left of x 36, blue right, green above y 16; each element grabs the
		// whole world flipped (as 042D00: (0, 0) .. (256, 256) from (-0.5/64, 63.5/64) .. (63.5/64, -0.5/64)) and draws a
		// band of the screen whose UVs are 042D00's ((x + 0.5) / 64, (63.5 - y) / 64) of a point 16 pixels to the left (and,
		// for the mesh, 16 up). The mesh (rows 16-40, 352740: colour the target's, alpha CURRENT x TFACTOR x 2 x the target's
		// 0x80 x 4: full; WRAP): at (40, 24) green (from (24, 8)), at (40, 36) red, at (60, 36) blue, at (8, 36) blue (from
		// x -8, wrapped); the ring (rows 40-64, 353378: colour vertex 0x80 x 2 x the target's, alpha 0x80 x 0x80 x 4: full;
		// MIRROR): at (40, 50) red, at (8, 50) red (x -8 mirrored), at (60, 50) blue
		{
			fx::frame &fd = fx::frame_slot(++seq);
			fd.seq.store(0);
			fd.element_count = fd.draw_count = fd.vertex_count = fd.light_count = fd.grab_count = 0;
			std::memcpy(fd.proj, id, 64);
			const float rows[2][2] = { { 16, 40 }, { 40, 64 } };
			for (int k = 0; k < 2; ++k)
			{
				fx::element e;
				std::memcpy(e.world, id, 64);
				e.tfactor = 0x80FFFFFFu;
				e.blend = fx::blend_alpha;
				e.cls = k == 0 ? fx::class_dist_model : fx::class_dist_ring;
				e.first_draw = fd.draw_count;
				e.draws = 1;
				fx::grab &gr = fd.grabs[fd.grab_count];
				const float pos[4][2] = { { 0, 0 }, { 256, 0 }, { 0, 256 }, { 256, 256 } };
				const float uv[4][2] = { { -0.5f / 64, 63.5f / 64 }, { 63.5f / 64, 63.5f / 64 }, { -0.5f / 64, -0.5f / 64 }, { 63.5f / 64, -0.5f / 64 } };
				std::memcpy(gr.pos, pos, sizeof(pos));
				std::memcpy(gr.uv, uv, sizeof(uv));
				fx::draw dr;
				dr.first = fd.vertex_count;
				dr.count = 6;
				dr.texture = fx::kDistTexture;
				dr.stages = k == 0 ? fx::stage_dist_model : fx::stage_dist_ring;
				dr.grab = fd.grab_count++;
				const float corner[6][2] = { { 0, rows[k][0] }, { 64, rows[k][0] }, { 0, rows[k][1] }, { 0, rows[k][1] }, { 64, rows[k][0] }, { 64, rows[k][1] } };
				for (const auto &c : corner)
				{
					fx::vertex v = {};
					v.pos[0] = c[0] / 32.0f - 1.0f;
					v.pos[1] = 1.0f - c[1] / 32.0f;
					v.pos[2] = 0.5f;
					v.colour = k == 0 ? 0xFFFFFFFFu : 0x80808080u;
					v.uv[0] = (c[0] - 16.0f + 0.5f) / 64.0f;
					v.uv[1] = (63.5f - (c[1] - (k == 0 ? 16.0f : 0.0f))) / 64.0f;
					fd.vertices[fd.vertex_count++] = v;
				}
				fd.draws[fd.draw_count++] = dr;
				fd.elements[fd.element_count++] = e;
			}
			fd.run_list[0] = { 0, 2 };
			fd.runs.store(1);
			fd.seq.store(seq);
			ID3D12CommandAllocator *da = nullptr;
			ID3D12GraphicsCommandList *dl = nullptr;
			device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&da));
			device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, da, nullptr, IID_PPV_ARGS(&dl));
			barrier(dl, A, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			const float red[4] = { 1, 0, 0, 1 }, blue[4] = { 0, 0, 1, 1 }, green[4] = { 0, 1, 0, 1 };
			const D3D12_RECT left = { 0, 0, 36, LONG(H) }, right = { 36, 0, LONG(W), LONG(H) }, top = { 0, 0, LONG(W), 16 };
			dl->ClearRenderTargetView(rtv, red, 1, &left);
			dl->ClearRenderTargetView(rtv, blue, 1, &right);
			dl->ClearRenderTargetView(rtv, green, 1, &top);
			dl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			dl->SetDescriptorHeaps(1, &game_heap);
			grass::list_state dbound;
			hifi::passes::world dw;
			dw.rt = A;
			dw.ds = Az;
			dw.rt_format = DXGI_FORMAT_R8G8B8A8_UNORM;
			dw.ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			dw.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
			dw.planes_known = true;
			dw.ds_planes[0] = dw.ds_planes[1] = D3D12_RESOURCE_STATE_DEPTH_WRITE;
			dw.heap_count = 1;
			dw.heaps[0] = game_heap;
			fx::inject::on_run(dl, dbound, dw, seq & 15);
			barrier(dl, A, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			D3D12_TEXTURE_COPY_LOCATION ddst = {}, dsrc = {};
			ddst.pResource = back;
			ddst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			ddst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
			dsrc.pResource = A;
			dsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dl->CopyTextureRegion(&ddst, 0, 0, 0, &dsrc, nullptr);
			dl->Close();
			ID3D12CommandList *dls[] = { dl };
			queue->ExecuteCommandLists(1, dls);
			queue->Signal(fence, ++fence_value);
			HANDLE dev_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			fence->SetEventOnCompletion(fence_value, dev_);
			WaitForSingleObject(dev_, 60000);
			CloseHandle(dev_);
			uint8_t *dp = nullptr;
			back->Map(0, nullptr, reinterpret_cast<void **>(&dp));
			const auto at_px = [&](int x, int y) { return dp + y * 256 + x * 4; };
			const auto is = [&](int x, int y, int r, int g, int b) {
				const uint8_t *p = at_px(x, y);
				return std::abs(p[0] - r) <= 8 && std::abs(p[1] - g) <= 8 && std::abs(p[2] - b) <= 8;
			};
			const bool mesh_ok = is(40, 24, 0, 255, 0) && is(40, 36, 255, 0, 0) && is(60, 36, 0, 0, 255) && is(8, 36, 0, 0, 255);
			const bool ring_ok = is(40, 50, 255, 0, 0) && is(8, 50, 255, 0, 0) && is(60, 50, 0, 0, 255);
			char dline[300];
			std::snprintf(dline, sizeof(dline),
				"the 042D00 distortions: the mesh (40, 24) %d %d %d (want green), (8, 36) %d %d %d (want blue: WRAP); the ring (40, 50) %d %d %d (want red), (8, 50) %d %d %d (want red: MIRROR)",
				at_px(40, 24)[0], at_px(40, 24)[1], at_px(40, 24)[2], at_px(8, 36)[0], at_px(8, 36)[1], at_px(8, 36)[2], at_px(40, 50)[0], at_px(40, 50)[1], at_px(40, 50)[2], at_px(8, 50)[0], at_px(8, 50)[1], at_px(8, 50)[2]);
			check(mesh_ok && ring_ok, dline);
			back->Unmap(0, nullptr);
			const fx::inject::counts dc = fx::inject::get_counts();
			std::string dfirst;
			const uint32_t dmessages = debug_messages(info, dfirst);
			check(dmessages == 0 && dc.failed == 0, "the 042D00 distortions: the debug layer: nothing (" + std::to_string(dmessages) + (dmessages != 0 ? ": " + dfirst : std::string()) + ")");
			dl->Release();
			da->Release();
		}

		// A lens flare's pieces by value (3520A8, pre-transformed): a quad over the left half in the game's pixels (WORLD
		// pixels to clip space, PROJECTION identity), untextured (white), vertex colour 0x40804020, TFACTOR alpha 0x80,
		// SRCALPHA / ONE on black: colour 0x80 0x40 0x20 x 2, alpha 0x40 x 2 x 0x80 x 2 (0.504): 128 64 32; the right half
		// untouched
		{
			fx::frame &ff = fx::frame_slot(++seq);
			ff.seq.store(0);
			ff.element_count = ff.draw_count = ff.vertex_count = ff.light_count = ff.grab_count = 0;
			fx::element e;
			const float pw[16] = { 2.0f / 64, 0, 0, 0, 0, -2.0f / 64, 0, 0, 0, 0, 1, 0, 1.0f / 64 - 1.0f, 1.0f - 1.0f / 64, 0, 1 };
			std::memcpy(e.world, pw, 64);
			e.projection = fx::proj_identity;
			e.tfactor = 0x80FFFFFFu;
			e.blend = fx::blend_add;
			e.cls = fx::class_flare;
			e.first_draw = 0;
			e.draws = 1;
			const float c[6][2] = { { -0.5f, -0.5f }, { 31.5f, -0.5f }, { -0.5f, 63.5f }, { -0.5f, 63.5f }, { 31.5f, -0.5f }, { 31.5f, 63.5f } };
			for (const auto &p : c)
			{
				fx::vertex v = {};
				v.pos[0] = p[0];
				v.pos[1] = p[1];
				v.colour = 0x40804020u;
				ff.vertices[ff.vertex_count++] = v;
			}
			ff.draws[ff.draw_count++] = { 0, 6, fx::kNone, fx::stage_flare };
			ff.elements[ff.element_count++] = e;
			ff.run_list[0] = { 0, 1 };
			ff.runs.store(1);
			ff.seq.store(seq);
			ID3D12CommandAllocator *fa = nullptr;
			ID3D12GraphicsCommandList *fl = nullptr;
			device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&fa));
			device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, fa, nullptr, IID_PPV_ARGS(&fl));
			barrier(fl, A, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			const float none[4] = { 0, 0, 0, 0 };
			fl->ClearRenderTargetView(rtv, none, 0, nullptr);
			fl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			fl->SetDescriptorHeaps(1, &game_heap);
			grass::list_state fbound;
			hifi::passes::world fw;
			fw.rt = A;
			fw.ds = Az;
			fw.rt_format = DXGI_FORMAT_R8G8B8A8_UNORM;
			fw.ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			fw.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
			fw.planes_known = true;
			fw.ds_planes[0] = fw.ds_planes[1] = D3D12_RESOURCE_STATE_DEPTH_WRITE;
			fw.heap_count = 1;
			fw.heaps[0] = game_heap;
			fx::inject::on_run(fl, fbound, fw, seq & 15);
			barrier(fl, A, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			D3D12_TEXTURE_COPY_LOCATION fdst = {}, fsrc = {};
			fdst.pResource = back;
			fdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			fdst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
			fsrc.pResource = A;
			fsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			fl->CopyTextureRegion(&fdst, 0, 0, 0, &fsrc, nullptr);
			fl->Close();
			ID3D12CommandList *fls[] = { fl };
			queue->ExecuteCommandLists(1, fls);
			queue->Signal(fence, ++fence_value);
			HANDLE fev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			fence->SetEventOnCompletion(fence_value, fev);
			WaitForSingleObject(fev, 60000);
			CloseHandle(fev);
			uint8_t *fp = nullptr;
			back->Map(0, nullptr, reinterpret_cast<void **>(&fp));
			const uint8_t *lft = fp + 32 * 256 + 10 * 4, *edge = fp + 32 * 256 + 31 * 4, *rgt = fp + 32 * 256 + 32 * 4;
			char fline[200];
			std::snprintf(fline, sizeof(fline), "a lens flare's stage, pre-transformed: %d %d %d at x 10 (want 128 64 32, within 2), x 31 %d, x 32 %d (the half's edge)", lft[0], lft[1], lft[2],
				edge[0], rgt[0]);
			check(std::abs(lft[0] - 128) <= 2 && std::abs(lft[1] - 64) <= 2 && std::abs(lft[2] - 32) <= 2 && edge[0] > 100 && rgt[0] == 0, fline);
			back->Unmap(0, nullptr);
			std::string ffirst;
			const uint32_t fmessages = debug_messages(info, ffirst);
			check(fmessages == 0, "a lens flare's stage: the debug layer: nothing (" + std::to_string(fmessages) + (fmessages != 0 ? ": " + ffirst : std::string()) + ")");
			fl->Release();
			fa->Release();
		}

		// 183760's tables by value: an MMB mesh of three screen thirds (a made-up map's data on the GPU, its texture
		// 2x2 of one texel), SRCALPHA / INVSRCALPHA on black: the left third textured (the plain table), the middle with
		// the textured mode (no texture alpha), the right untextured
		{
			namespace zm = hifi::bake::zonemesh;
			zm::zone zd;
			zd.map = 2;
			for (int third = 0; third < 3; ++third)
			{
				const float x0 = -1.0f + 2.0f * third / 3.0f, x1 = -1.0f + 2.0f * (third + 1) / 3.0f;
				const float c[6][2] = { { x0, -1 }, { x1, -1 }, { x0, 1 }, { x0, 1 }, { x1, -1 }, { x1, 1 } };
				zm::piece pc;
				pc.first_index = uint32_t(zd.indices.size());
				pc.triangles = 2;
				pc.texture = third < 2 ? 0 : zm::kNone;
				for (const auto &p : c)
				{
					zm::vertex v = {};
					v.pos[0] = p[0];
					v.pos[1] = p[1];
					v.pos[2] = 0.5f;
					v.colour = 0x40806020u;
					v.uv[0] = (p[0] + 1.0f) * 0.5f;
					v.uv[1] = (p[1] + 1.0f) * 0.5f;
					zd.indices.push_back(uint32_t(zd.vertices.size()));
					zd.vertices.push_back(v);
				}
				zd.pieces.push_back(pc);
			}
			zm::model mo;
			std::memcpy(mo.name, "fxtest_thirds   ", 16);
			mo.pieces = mo.raw_pieces = 3;
			mo.vertices = uint32_t(zd.vertices.size());
			zd.models.push_back(mo);
			zm::texture tx;
			std::memcpy(tx.name, "fxtest  thirds  ", 16);
			tx.w = tx.h = 2;
			tx.format = zm::kBGRA8;
			for (int k = 0; k < 4; ++k)
				tx.data.insert(tx.data.end(), { 0x40, 0x80, 0xC0, 0xFF });
			zd.textures.push_back(std::move(tx));
			hifi::zone::gpu_zone gz;
			check(gz.upload(device, queue, std::move(zd), why) && gz.wait(10000), "183760's tables: the made-up map's data on the GPU");
			fx::frame &g3 = fx::frame_slot(++seq);
			g3.seq.store(0);
			g3.element_count = g3.draw_count = g3.vertex_count = 0;
			std::memcpy(g3.proj, id, 64);
			const uint32_t stages[3] = { fx::stage_zone_mesh | fx::stage_mmb_textured, fx::stage_zone_mesh | fx::stage_mmb_textured_d, fx::stage_zone_mesh };
			for (uint32_t third = 0; third < 3; ++third)
			{
				fx::element e;
				std::memcpy(e.world, id, 64);
				e.tfactor = 0x80808080u;
				e.blend = fx::blend_alpha;
				e.zone = &gz;
				e.cls = fx::class_mmb;
				e.first_draw = g3.draw_count;
				e.draws = 1;
				g3.draws[g3.draw_count++] = { 6 * third, 6, third < 2 ? 0u : fx::kNone, stages[third] };
				g3.elements[g3.element_count++] = e;
			}
			g3.run_list[0] = { 0, g3.element_count };
			g3.runs.store(1);
			g3.seq.store(seq);
			alloc->Reset();
			list->Reset(alloc, nullptr);
			barrier(list, A, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			list->ClearRenderTargetView(rtv, black, 0, nullptr);
			list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			list->SetDescriptorHeaps(1, &game_heap);
			fx::inject::on_run(list, bound, w, seq & 15);
			barrier(list, A, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			list->Close();
			queue->ExecuteCommandLists(1, lists);
			queue->Signal(fence, ++fence_value);
			HANDLE ev3 = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			fence->SetEventOnCompletion(fence_value, ev3);
			WaitForSingleObject(ev3, 60000);
			CloseHandle(ev3);
			back->Map(0, nullptr, reinterpret_cast<void **>(&px));
			const auto sat = [](double v) { return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; };
			const double D[4] = { 0x80 / 255.0, 0x60 / 255.0, 0x20 / 255.0, 0x40 / 255.0 }, T[4] = { 0xC0 / 255.0, 0x80 / 255.0, 0x40 / 255.0, 1.0 }, F = 0x80 / 255.0;
			bool ok = true;
			std::string got;
			for (int third = 0; third < 3; ++third)
			{
				const double a = third == 0 ? sat(sat(T[3] * F * 2.0) * D[3] * 4.0) : sat(D[3] * F * 4.0);
				const uint8_t *q = px + 32 * 256 + size_t(10 + 21 * third) * 4;
				for (int k = 0; k < 3; ++k)
				{
					const double c = third == 0 ? sat(sat(T[k] * F) * D[k] * 4.0) : third == 1 ? sat(sat(T[k] * D[k]) * F * 4.0) : sat(D[k] * F * 2.0);
					const int want3 = int(c * a * 255.0 + 0.5);
					ok = ok && std::abs(int(q[k]) - want3) <= 2;
					got += (got.empty() ? "" : " ") + std::to_string(q[k]) + "/" + std::to_string(want3);
				}
			}
			back->Unmap(0, nullptr);
			check(ok, "183760's tables: plain, the textured mode, untextured (got/want: " + got + ", within 2)");
			const uint32_t messages3 = debug_messages(info, first);
			check(messages3 == 0, "183760's tables: the debug layer: nothing (" + std::to_string(messages3) + (messages3 != 0 ? ": " + first : std::string()) + ")");
		}

		// A lit element by value (D3D8's lighting as 0467E0 sets it): a screen-filling untextured quad, its normals
		// (0, 0, 2) (not normalized: NORMALIZENORMALS 0), one light toward +z of diffuse 0.5, ambient 0.25, emissive blue
		// 0.1, ONE / ZERO: colour = vertex x (0.25 + 0.5 x 2) + emissive, then x TFACTOR x 2
		{
			fx::frame &g4 = fx::frame_slot(++seq);
			g4.seq.store(0);
			g4.element_count = g4.draw_count = g4.vertex_count = g4.light_count = 0;
			std::memcpy(g4.proj, id, 64);
			fx::element e;
			std::memcpy(e.world, id, 64);
			e.tfactor = 0x80808080u;
			e.blend = fx::blend_one_zero;
			e.first_draw = 0;
			e.draws = 1;
			fx::lighting &l = g4.lights[g4.light_count];
			l = fx::lighting();
			for (int i = 0; i < 3; ++i)
				l.normal[i][i] = 1.0f;
			l.toward[0][2] = 1.0f;
			l.diffuse[0][0] = l.diffuse[0][1] = l.diffuse[0][2] = 0.5f;
			l.ambient[0] = l.ambient[1] = l.ambient[2] = 0.25f;
			l.emissive[2] = 0.1f;
			e.light = g4.light_count++;
			g4.draws[g4.draw_count++] = { 0, 6, fx::kNone, 0 };
			const float c[6][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { -1, 1 }, { 1, -1 }, { 1, 1 } };
			for (const auto &p : c)
			{
				fx::vertex v = {};
				v.pos[0] = p[0];
				v.pos[1] = p[1];
				v.pos[2] = 0.5f;
				v.normal[2] = 2.0f;
				v.colour = 0xFF806020u;
				g4.vertices[g4.vertex_count++] = v;
			}
			g4.elements[g4.element_count++] = e;
			g4.run_list[0] = { 0, 1 };
			g4.runs.store(1);
			g4.seq.store(seq);
			alloc->Reset();
			list->Reset(alloc, nullptr);
			barrier(list, A, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			list->ClearRenderTargetView(rtv, black, 0, nullptr);
			list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			list->SetDescriptorHeaps(1, &game_heap);
			fx::inject::on_run(list, bound, w, seq & 15);
			barrier(list, A, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			list->Close();
			queue->ExecuteCommandLists(1, lists);
			queue->Signal(fence, ++fence_value);
			HANDLE ev4 = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			fence->SetEventOnCompletion(fence_value, ev4);
			WaitForSingleObject(ev4, 60000);
			CloseHandle(ev4);
			back->Map(0, nullptr, reinterpret_cast<void **>(&px));
			const uint8_t *q = px + 32 * 256 + 32 * 4;
			const double vc[3] = { 0x80 / 255.0, 0x60 / 255.0, 0x20 / 255.0 }, emi[3] = { 0.0, 0.0, 0.1 }, tf = 2.0 * 0x80 / 255.0;
			bool ok = true;
			std::string got;
			for (int k = 0; k < 3; ++k)
			{
				const double lit = std::min(1.0, vc[k] * 1.25 + emi[k]);
				const int want4 = int(std::min(1.0, lit * tf) * 255.0 + 0.5);
				ok = ok && std::abs(int(q[k]) - want4) <= 2;
				got += (got.empty() ? "" : " ") + std::to_string(q[k]) + "/" + std::to_string(want4);
			}
			back->Unmap(0, nullptr);
			check(ok, "a lit element: vertex x (ambient + diffuse x N.L, N not normalized) + emissive (got/want: " + got + ", within 2)");
			const uint32_t messages4 = debug_messages(info, first);
			check(messages4 == 0, "a lit element: the debug layer: nothing (" + std::to_string(messages4) + (messages4 != 0 ? ": " + first : std::string()) + ")");
		}
		list->Release();
		alloc->Release();
		A->Release();
		Az->Release();
		back->Release();
		rtvh->Release();
		dsvh->Release();
		game_heap->Release();
	}
	fx::inject::release();
	fx::textures::stop();
	g_zones.clear();
	if (mapped)
		dump::unmap(dump_base);
	std::printf("\n%d passed, %d failed\n", passes, failures);
	return failures == 0 ? 0 : 1;
}
