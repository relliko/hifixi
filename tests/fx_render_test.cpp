/*
 * Offline test of the native effect renderer (src/fx: fxinject at an fx_run marker, fxrender, fxtextures, fxgeo) on
 * WARP with the debug layer, against research/ffxi-re/tools/effects_play.py through tests/fx_oracle.py play: an
 * action's effect run by the player to one frame, its elements (far to near) with their models' and textures' chunks
 * and the picture the player's rasteriser makes of them on black.
 *
 * Each oracle's elements become a flush's frame as src/game/fxdraw records one (WORLD from the player's view-space
 * matrix, TEXTUREFACTOR as it computes it, the blend by the letter, z write by flags bit 12, the class's stages and
 * geometry from the chunks through fxgeo), one run; the textures go through fxtextures (decoded on its thread, copied
 * in on the list at the marker). The marker is handled on a list recorded as dgVoodoo records (its own root signature,
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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
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

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
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
	fx::inject::release();
	fx::textures::stop();
	std::printf("\n%d passed, %d failed\n", passes, failures);
	return failures == 0 ? 0 : 1;
}
