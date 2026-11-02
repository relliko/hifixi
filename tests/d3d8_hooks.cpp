/*
 * HiFi XI's hooks (src/host/hooks.cpp, as they are) over our d3d8.dll's Direct3D 12 stream (docs/own-frame/d3d8.md):
 * what HiFi XI relies on from dgVoodoo, checked on ours. The hooks go in first (on a WARP throwaway, as HiFi XI's
 * starter puts them in), then a frame shaped like FFXI's is drawn through d3d8.dll (D3D8_WARP=1: WARP, the debug layer
 * with D3D8_DEBUG=1):
 *   the world target A (1024x576 A8R8G8B8 + D24S8) cleared, the sky, the zone between HiFi XI's slot markers (1x1
 *   stencil-only clears at x = 16 kind + seq), a blended zone piece (Z writes off), the characters between theirs, a
 *   visibility test (a second target with A's depth, a 16x16 downsample, CopyRects into system memory, LockRect), the
 *   effects, the resolve onto the back buffer, the UI, Present; five frames.
 *
 *   build\d3d8\d3d8_hooks.exe <d3d8.dll>
 *
 * Checked, from the hooks' events (as backend.cpp gets them):
 *   - A's views: render target view of format 28 on a resource of 27, depth view 45 on 44 (made through the hooked
 *     device, so HiFi XI knows them);
 *   - each marker arrives as ClearDepthStencilView(A's view, stencil only, 1 rect = the marker's pixel) in order, its
 *     stencil plane put in DEPTH_WRITE just before (subresource 1), and slotmark::read reads it back;
 *   - when the zone begins, the target bound with A's depth on that list is A (world discovery by the marker);
 *   - viewports half a pixel in, depth 0..1; descriptor heaps set after every reset before the list draws;
 *   - the depth planes' barriers name subresource 0 or 1 only; plane 0 in DEPTH_WRITE for opaque draws, DEPTH_READ
 *     for the blended piece; A goes RENDER_TARGET -> PIXEL_SHADER_RESOURCE at the resolve, whose draw is next (4
 *     vertices into the bigger back buffer);
 *   - the list is closed and submitted between the visibility test's copy and its LockRect;
 *   - every list submitted is of the hooked class, none recorded unseen.
 * And HiFi XI drawing into the frame: at the zone's end marker, inside the hook (the thread quiet, as backend.cpp
 * draws), other heaps, root signature, targets, viewport and topology are set, then the state the hooks saw put back
 * (as grass::list_state does). The frame read back must be the same, pixel for pixel, as without it.
 * Then the direct channel (HifiFrameMark), the float world (HifiFrameSetWorldFormat) and the frame-rate speed-ups
 * (HifiFrameSetSpeedups): an interface-like run of quads the same, pixel for pixel, with dedupe and batch on, in fewer
 * draws.
 */

#include "../src/host/hooks.hpp"
#include "../src/host/events.hpp"
#include "../src/game/slotmark.hpp"
#include "../src/host/frameapi.hpp"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3d8.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
	int failures = 0;
	// (a half's value: normal ones and zero are all this test reads)
	float half_f(uint16_t h)
	{
		const int e = (h >> 10) & 31, m = h & 1023;
		const float v = e == 0 ? std::ldexp(float(m), -24) : std::ldexp(float(m | 1024), e - 25);
		return (h & 0x8000) != 0 ? -v : v;
	}
	void check(bool ok, const char *what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
		if (!ok)
			failures++;
	}

	// ---- What the hooks hand on ----
	struct view_info { ID3D12Resource *res; DXGI_FORMAT format; };
	std::map<uint64_t, view_info> views;
	struct mark_seen { uint32_t kind, seq; ID3D12Resource *bound_rt, *bound_ds; bool list_known; };
	std::vector<mark_seen> marks;
	struct list_state
	{
		bool heaps_set = false;
		uint32_t draws_before_heaps = 0;
		ID3D12DescriptorHeap *heaps[2] = {};
		UINT heap_count = 0;
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = {}, dsv = {};
		UINT rtv_count = 0;
		bool has_dsv = false;
		D3D12_VIEWPORT vp = {};
		D3D12_RECT sc = {};
		ID3D12PipelineState *pso = nullptr;
		D3D12_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D12RootSignature *root = nullptr;
		D3D12_GPU_VIRTUAL_ADDRESS cbv[2] = {};
		D3D12_GPU_DESCRIPTOR_HANDLE table[4] = {};
	};
	std::map<void *, list_state> lists;
	std::map<ID3D12Resource *, D3D12_RESOURCE_STATES> plane_state[2];
	ID3D12Resource *world_rt = nullptr, *world_ds = nullptr, *back = nullptr;
	uint32_t bad_subresources = 0, viewport_bad = 0, viewports = 0, draws = 0, closes = 0, executes = 0;
	uint32_t opaque_draws_plane_ok = 0, opaque_draws_plane_bad = 0, blended_read = 0;
	bool expect_blended = false, expect_opaque = false;
	bool world_ended = false, resolve_seen = false;
	int resolve_draw_vertices = -1;
	ID3D12Resource *resolve_target = nullptr;
	uint32_t readback_phase = 0; // 1: the copy made, waiting for the close; 2: closed and submitted
	bool inject = false;
	uint32_t injected = 0;

	ID3D12Device *dev12 = nullptr;
	ID3D12DescriptorHeap *own_heap = nullptr;
	ID3D12RootSignature *own_root = nullptr;

	ID3D12Resource *resource_of(uint64_t v)
	{
		const auto it = views.find(v);
		return it != views.end() ? it->second.res : nullptr;
	}

	void ev_view(ID3D12Device *d, D3D12_DESCRIPTOR_HEAP_TYPE, D3D12_CPU_DESCRIPTOR_HANDLE at, ID3D12Resource *r, DXGI_FORMAT f, uint32_t)
	{
		views[uint64_t(at.ptr)] = { r, f };
		if (dev12 == nullptr)
			dev12 = d;
	}
	void ev_reset(ID3D12GraphicsCommandList *l) { lists[l] = list_state(); }
	void ev_created(ID3D12GraphicsCommandList *l) { lists[l] = list_state(); }
	void ev_close(ID3D12GraphicsCommandList *) { closes++; }
	void ev_execute(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *)
	{
		executes++;
		if (readback_phase == 1)
			readback_phase = 2;
	}
	void ev_heaps(ID3D12GraphicsCommandList *l, UINT n, ID3D12DescriptorHeap *const *h)
	{
		list_state &s = lists[l];
		s.heaps_set = true;
		s.heap_count = n;
		for (UINT i = 0; i < n && i < 2; ++i)
			s.heaps[i] = h[i];
	}
	void ev_targets(ID3D12GraphicsCommandList *l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE *dsv)
	{
		list_state &s = lists[l];
		s.rtv_count = n;
		s.rtv = n != 0 ? rtvs[0] : D3D12_CPU_DESCRIPTOR_HANDLE {};
		s.has_dsv = dsv != nullptr;
		s.dsv = dsv != nullptr ? *dsv : D3D12_CPU_DESCRIPTOR_HANDLE {};
	}
	void ev_viewports(ID3D12GraphicsCommandList *l, UINT n, const D3D12_VIEWPORT *v)
	{
		viewports++;
		if (n != 1 || v[0].TopLeftX - float(int(v[0].TopLeftX)) != 0.5f || v[0].TopLeftY - float(int(v[0].TopLeftY)) != 0.5f || v[0].MinDepth != 0.0f || v[0].MaxDepth != 1.0f)
			viewport_bad++;
		lists[l].vp = v[0];
	}
	void ev_scissors(ID3D12GraphicsCommandList *l, UINT, const D3D12_RECT *r) { lists[l].sc = r[0]; }
	void ev_pipeline(ID3D12GraphicsCommandList *l, ID3D12PipelineState *p) { lists[l].pso = p; }
	void ev_topology(ID3D12GraphicsCommandList *l, D3D12_PRIMITIVE_TOPOLOGY t) { lists[l].topo = t; }
	void ev_root_signature(ID3D12GraphicsCommandList *l, ID3D12RootSignature *r)
	{
		list_state &s = lists[l];
		s.root = r;
		std::memset(s.cbv, 0, sizeof(s.cbv));
		std::memset(s.table, 0, sizeof(s.table));
	}
	void ev_root_cbv(ID3D12GraphicsCommandList *l, UINT p, D3D12_GPU_VIRTUAL_ADDRESS a)
	{
		if (p < 2)
			lists[l].cbv[p] = a;
	}
	void ev_root_table(ID3D12GraphicsCommandList *l, UINT p, D3D12_GPU_DESCRIPTOR_HANDLE h)
	{
		if (p < 4)
			lists[l].table[p] = h;
	}
	void ev_barrier(ID3D12GraphicsCommandList *, UINT n, const D3D12_RESOURCE_BARRIER *b)
	{
		for (UINT i = 0; i < n; ++i)
		{
			if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
				continue;
			const D3D12_RESOURCE_TRANSITION_BARRIER &t = b[i].Transition;
			if (t.pResource == world_ds)
			{
				if (t.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
					plane_state[0][t.pResource] = plane_state[1][t.pResource] = t.StateAfter;
				else if (t.Subresource < 2)
					plane_state[t.Subresource][t.pResource] = t.StateAfter;
				else
					bad_subresources++;
			}
			if (t.pResource == world_rt && (t.StateBefore & D3D12_RESOURCE_STATE_RENDER_TARGET) && (t.StateAfter & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
				world_ended = true;
		}
	}
	void ev_draw(ID3D12GraphicsCommandList *l, uint64_t vertices)
	{
		draws++;
		list_state &s = lists[l];
		if (!s.heaps_set)
			s.draws_before_heaps++;
		if (world_ended && !resolve_seen && resolve_draw_vertices < 0)
		{
			resolve_seen = true;
			resolve_draw_vertices = int(vertices);
			resolve_target = resource_of(s.rtv.ptr);
		}
		if (expect_opaque || expect_blended)
		{
			const D3D12_RESOURCE_STATES p0 = plane_state[0][world_ds];
			if (expect_opaque)
				(p0 == D3D12_RESOURCE_STATE_DEPTH_WRITE ? opaque_draws_plane_ok : opaque_draws_plane_bad)++;
			if (expect_blended && p0 == D3D12_RESOURCE_STATE_DEPTH_READ)
				blended_read++;
		}
	}

	// HiFi XI drawing into the frame: other state set on the list, nothing put back (the d3d8.dll binds its own again)
	void draw_into(ID3D12GraphicsCommandList *l)
	{
		hifi::host::quiet q;
		l->SetDescriptorHeaps(1, &own_heap);
		l->SetGraphicsRootSignature(own_root);
		l->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
		const D3D12_VIEWPORT vp = { 3.0f, 7.0f, 11.0f, 13.0f, 0.2f, 0.4f };
		l->RSSetViewports(1, &vp);
		l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
		injected++;
	}

	const hifi::host::events &events()
	{
		static hifi::host::events e = [] {
			hifi::host::events x;
			x.created = ev_created;
			x.reset = ev_reset;
			x.close = ev_close;
			x.targets = ev_targets;
			x.pipeline = ev_pipeline;
			x.topology = ev_topology;
			x.viewports = ev_viewports;
			x.scissors = ev_scissors;
			x.root_signature = ev_root_signature;
			x.root_table = ev_root_table;
			x.root_cbv = ev_root_cbv;
			x.barrier = ev_barrier;
			x.draw = ev_draw;
			x.heaps = ev_heaps;
			x.execute = ev_execute;
			x.view = ev_view;
			return x;
		}();
		return e;
	}

	// ---- The Direct3D 8 frame ----
	IDirect3DDevice8 *dev = nullptr;
	constexpr UINT AW = 1024, AH = 576;
	IDirect3DTexture8 *A_tex = nullptr, *test_tex = nullptr, *tiny_tex = nullptr, *tex = nullptr;
	IDirect3DSurface8 *A = nullptr, *Az = nullptr, *bb = nullptr, *test = nullptr, *tiny = nullptr, *sys = nullptr, *out = nullptr;
	struct V { float x, y, z; DWORD c; float u, v; };
	struct VR { float x, y, z, w; DWORD c; float u, v; };
	uint32_t seq = 0, readbacks_ok = 0;

	// The game side's marks, as src/game/directmark.hpp sends them (HifiFrameMark), and the handler HiFi XI registers
	// for the frames: what each was handed; at the zone's end, while inject is on, it draws in
	hifi_frame_mark_fn g_mark = nullptr;
	void marker(uint32_t kind)
	{
		if (g_mark != nullptr)
			g_mark(kind, seq);
	}
	void __cdecl frame_handler(void *, uint32_t kind, uint32_t s, const hifi_frame_mark *m)
	{
		if (kind == hifi::slotmark::world_end)
			return;
		mark_seen x;
		x.kind = kind;
		x.seq = s;
		x.bound_rt = m->rt;
		x.bound_ds = m->ds;
		x.list_known = lists.count(m->list) != 0;
		marks.push_back(x);
		if (inject && kind == hifi::slotmark::zone_end)
			draw_into(m->list);
	}

	// The direct channel's handler (src/host/frameapi.hpp): what it was handed; it clears an 8x8 corner of the target
	// white, then binds nothing and a 1x1 viewport (the draw after must bind the game's again)
	struct direct_seen
	{
		uint32_t calls = 0, kinds[4] = {}, seqs[4] = {};
		hifi_frame_mark m = {};   // (the first call's)
		hifi_frame_mark end = {}; // (the second's: the world's end)
	} g_direct;
	void __cdecl direct_handler(void *, uint32_t kind, uint32_t seq, const hifi_frame_mark *m)
	{
		if (g_direct.calls < 4)
		{
			g_direct.kinds[g_direct.calls] = kind;
			g_direct.seqs[g_direct.calls] = seq;
		}
		if (g_direct.calls == 0)
			g_direct.m = *m;
		else if (g_direct.calls == 1)
			g_direct.end = *m;
		g_direct.calls++;
		if (m->rt != nullptr && m->rt_state == D3D12_RESOURCE_STATE_RENDER_TARGET)
		{
			const float white[4] = { 1, 1, 1, 1 };
			const D3D12_RECT corner = { 0, 0, 8, 8 };
			m->list->ClearRenderTargetView(m->rtv, white, 1, &corner);
		}
		m->list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
		const D3D12_VIEWPORT one = { 0, 0, 1, 1, 0, 1 };
		m->list->RSSetViewports(1, &one);
	}

	// The float world's handler: what the zone's begin handed; at the world's end, A copied into a readback buffer of its
	// own (read once the GPU is past it), so what the game drew is read as the GPU holds it, past white and all
	struct float_seen
	{
		hifi_frame_mark begin = {}, end = {};
		bool begun = false, ended = false;
		ID3D12Resource *rb = nullptr;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
	} g_float;
	void __cdecl float_handler(void *, uint32_t kind, uint32_t, const hifi_frame_mark *m)
	{
		if (kind == hifi::slotmark::zone_begin)
		{
			g_float.begin = *m;
			g_float.begun = true;
			return;
		}
		if (kind != hifi::slotmark::world_end || m->rt == nullptr || dev12 == nullptr || g_float.ended)
			return;
		g_float.end = *m;
		g_float.ended = true;
		const D3D12_RESOURCE_DESC d = m->rt->GetDesc();
		UINT64 total = 0;
		dev12->GetCopyableFootprints(&d, 0, 1, 0, &g_float.fp, nullptr, nullptr, &total);
		D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = total;
		bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		hifi::host::quiet q;
		if (FAILED(dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_float.rb))))
			return;
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = m->rt;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = m->rt_state;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		m->list->ResourceBarrier(1, &b);
		D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
		to.pResource = g_float.rb;
		to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		to.PlacedFootprint = g_float.fp;
		from.pResource = m->rt;
		from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		m->list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
		std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
		m->list->ResourceBarrier(1, &b);
	}

	void tri_strip_band(float y0, float y1, float z, DWORD c, int n)
	{
		std::vector<V> v;
		for (int i = 0; i <= n; ++i)
		{
			const float x = -1.0f + 2.0f * i / n;
			v.push_back({ x, y0, z, c, x, 0 });
			v.push_back({ x, y1, z, c, x, 1 });
		}
		dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
		dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, UINT(v.size() - 2), v.data(), sizeof(V));
	}

	void quad_rhw(float x0, float y0, float x1, float y1, DWORD c)
	{
		const VR v[4] = { { x0, y0, 0, 1, c, 0, 0 }, { x1, y0, 0, 1, c, 1, 0 }, { x0, y1, 0, 1, c, 0, 1 }, { x1, y1, 0, 1, c, 1, 1 } };
		dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
		dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(VR));
	}

	std::vector<uint32_t> frame(bool read_world)
	{
		seq++;
		dev->BeginScene();
		dev->SetRenderTarget(A, Az);
		D3DVIEWPORT8 vp = { 0, 0, AW, AH, 0.0f, 1.0f };
		dev->SetViewport(&vp);
		dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x80B2C6C7, 1.0f, 0);
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		// Sky: Z writes off
		dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		tri_strip_band(0.4f, 1.0f, 0.999f, 0xFF6080C0, 10);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		// Zone, between its markers: opaque ground, a blended piece
		marker(hifi::slotmark::zone_begin);
		dev->SetTexture(0, tex);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		expect_opaque = true;
		tri_strip_band(-1.0f, 0.5f, 0.6f, 0xFF808080, 20);
		expect_opaque = false;
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		dev->SetRenderState(D3DRS_ZBIAS, 8);
		expect_blended = true;
		tri_strip_band(-0.5f, 0.0f, 0.55f, 0x8040A060, 6);
		expect_blended = false;
		dev->SetRenderState(D3DRS_ZBIAS, 0);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		marker(hifi::slotmark::zone_end);
		// Characters, between theirs
		marker(hifi::slotmark::chars_begin);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		expect_opaque = true;
		tri_strip_band(-0.2f, 0.3f, 0.4f, 0xFFC08040, 4);
		expect_opaque = false;
		marker(hifi::slotmark::chars_end);
		// A visibility test: a quad into the test target with A's depth, downsampled to 16x16, read back
		dev->SetRenderTarget(test, Az);
		dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		tri_strip_band(-0.1f, 0.1f, 0.5f, 0xFFFFFFFF, 2);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		dev->SetRenderTarget(tiny, nullptr);
		dev->SetTexture(0, test_tex);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		quad_rhw(-0.5f, -0.5f, 15.5f, 15.5f, 0xFFFFFFFF);
		const RECT r16 = { 0, 0, 16, 16 };
		const POINT p0 = { 0, 0 };
		dev->CopyRects(tiny, &r16, 1, sys, &p0);
		readback_phase = 1;
		D3DLOCKED_RECT lr;
		sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
		readbacks_ok += readback_phase == 2 ? 1 : 0;
		sys->UnlockRect();
		readback_phase = 0;
		// Effects in A (Z writes off), then the resolve and the UI on the back buffer
		dev->SetRenderTarget(A, Az);
		dev->SetViewport(&vp);
		dev->SetTexture(0, nullptr);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
		tri_strip_band(0.1f, 0.2f, 0.3f, 0x80FF2020, 3);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		std::vector<uint32_t> px;
		if (read_world)
		{
			const RECT whole = { 0, 0, LONG(AW), LONG(AH) };
			dev->CopyRects(A, &whole, 1, out, &p0);
			D3DLOCKED_RECT o;
			out->LockRect(&o, nullptr, D3DLOCK_READONLY);
			px.resize(size_t(AW) * AH);
			for (UINT y = 0; y < AH; ++y)
				std::memcpy(px.data() + size_t(y) * AW, static_cast<const uint8_t *>(o.pBits) + size_t(y) * o.Pitch, AW * 4);
			out->UnlockRect();
		}
		world_ended = resolve_seen = false;
		dev->SetRenderTarget(bb, nullptr);
		D3DVIEWPORT8 sv = { 0, 0, 1280, 720, 0.0f, 1.0f };
		dev->SetViewport(&sv);
		dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		dev->SetTexture(0, A_tex);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
		quad_rhw(-0.5f, -0.5f, 1279.5f, 719.5f, 0xFFFFFFFF);
		dev->SetTexture(0, nullptr);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		for (int i = 0; i < 20; ++i)
			quad_rhw(20.0f + 30 * i, 600, 45.0f + 30 * i, 680, 0xC0202838);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		dev->EndScene();
		dev->Present(nullptr, nullptr, nullptr, nullptr);
		return px;
	}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 2)
	{
		std::printf("d3d8_hooks.exe <d3d8.dll>\n");
		return 1;
	}
	// (D3D8_DEBUG=1: the debug layer on before anything is made, so the hooks go into its classes as they do in
	// hook_test --debug; the DLL logs what it says)
	if (std::getenv("D3D8_DEBUG") != nullptr)
	{
		ID3D12Debug *debug = nullptr;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
		{
			debug->EnableDebugLayer();
			debug->Release();
		}
	}
	// The hooks first, on a WARP throwaway (as HiFi XI's starter puts them in)
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
	std::string why;
	check(hifi::host::install(why, warp), ("HiFi XI's hooks in (" + why + ")").c_str());
	hifi::host::set_events(&events());

	SetEnvironmentVariableA("D3D8_WARP", "1");
	WNDCLASSA wc = {};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(nullptr);
	wc.lpszClassName = "d3d8_hooks";
	RegisterClassA(&wc);
	HWND window = CreateWindowA("d3d8_hooks", "d3d8_hooks", WS_OVERLAPPEDWINDOW, 40, 40, 1296, 759, nullptr, nullptr, wc.hInstance, nullptr);
	HMODULE m = LoadLibraryA(argv[1]);
	using create_fn = IDirect3D8 *(WINAPI *)(UINT);
	const auto create = m != nullptr ? reinterpret_cast<create_fn>(GetProcAddress(m, "Direct3DCreate8")) : nullptr;
	IDirect3D8 *d3d = create != nullptr ? create(D3D_SDK_VERSION) : nullptr;
	if (d3d == nullptr)
	{
		std::printf("FAIL  no d3d8 (%s)\n", argv[1]);
		return 1;
	}
	D3DPRESENT_PARAMETERS pp = {};
	pp.BackBufferWidth = 1280;
	pp.BackBufferHeight = 720;
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.BackBufferCount = 1;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.hDeviceWindow = window;
	pp.Windowed = TRUE;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
	{
		std::printf("FAIL  CreateDevice\n");
		return 1;
	}
	dev->CreateTexture(AW, AH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &A_tex);
	A_tex->GetSurfaceLevel(0, &A);
	dev->CreateDepthStencilSurface(AW, AH, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, &Az);
	dev->CreateTexture(AW, AH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &test_tex);
	test_tex->GetSurfaceLevel(0, &test);
	dev->CreateTexture(16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tiny_tex);
	tiny_tex->GetSurfaceLevel(0, &tiny);
	dev->CreateImageSurface(16, 16, D3DFMT_A8R8G8B8, &sys);
	dev->CreateImageSurface(AW, AH, D3DFMT_A8R8G8B8, &out);
	dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	dev->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex);
	D3DLOCKED_RECT lr;
	tex->LockRect(0, &lr, nullptr, 0);
	for (int y = 0; y < 32; ++y)
		for (int x = 0; x < 32; ++x)
			static_cast<DWORD *>(lr.pBits)[y * lr.Pitch / 4 + x] = ((x ^ y) & 4) ? 0xFF907050 : 0xFF506070;
	tex->UnlockRect(0);

	// HiFi XI's own heap and root signature (made on the game's device, the thread quiet)
	if (dev12 != nullptr)
	{
		hifi::host::quiet q;
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = 16;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		dev12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&own_heap));
		D3D12_ROOT_SIGNATURE_DESC rd = {};
		ID3DBlob *blob = nullptr, *err = nullptr;
		if (SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
			dev12->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&own_root));
	}
	check(own_heap != nullptr && own_root != nullptr, "HiFi XI's own heap and root signature made");

	// A's views as HiFi XI sees them made
	for (const auto &v : views)
	{
		const D3D12_RESOURCE_DESC d = v.second.res != nullptr ? v.second.res->GetDesc() : D3D12_RESOURCE_DESC {};
		if (d.Width == AW && d.Height == AH && v.second.format == DXGI_FORMAT_R8G8B8A8_UNORM && world_rt == nullptr && d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
			world_rt = v.second.res;
		if (d.Width == AW && d.Height == AH && v.second.format == DXGI_FORMAT_D24_UNORM_S8_UINT && d.Format == DXGI_FORMAT_R24G8_TYPELESS)
			world_ds = v.second.res;
	}
	check(world_rt != nullptr, "A's render target view: format 28 on a resource of 27, made through the hooked device");
	check(world_ds != nullptr, "A's depth view: format 45 on a resource of 44");
	// (World FX reads A's depth in its shaders, as dgVoodoo lets it: an SRV of R24_UNORM_X8_TYPELESS, the resource in
	// DEPTH_READ | PIXEL_SHADER_RESOURCE; a depth made with DENY_SHADER_RESOURCE removed the device in game, 2026-10-30)
	check(world_ds != nullptr && (world_ds->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0,
		"A's depth readable by shaders (no DENY_SHADER_RESOURCE): World FX's SRV of it");

	// Frames: three plain, the fourth read back; then HiFi XI drawing in at the zone's end, the fifth read back (the
	// marks through the direct channel)
	const auto set_handler = reinterpret_cast<hifi_frame_set_handler_fn>(GetProcAddress(m, kHifiFrameSetHandler));
	g_mark = reinterpret_cast<hifi_frame_mark_fn>(GetProcAddress(m, kHifiFrameMark));
	if (set_handler != nullptr)
		set_handler(&frame_handler, nullptr);
	std::vector<uint32_t> plain, injected_px;
	for (int f = 0; f < 4; ++f)
		plain = frame(f == 3);
	const size_t marks_plain = marks.size();
	inject = true;
	injected_px = frame(true);
	inject = false;
	if (set_handler != nullptr)
		set_handler(nullptr, nullptr);

	// Markers
	bool order = marks_plain == 16, bound = true, depth = true, known = true;
	for (size_t i = 0; i < marks.size(); ++i)
	{
		const mark_seen &s = marks[i];
		order = order && s.kind == uint32_t(i % 4) && s.seq == uint32_t((i / 4 + 1) & 15);
		depth = depth && s.bound_ds == world_ds;
		known = known && s.list_known;
		if (s.kind == hifi::slotmark::zone_begin)
			bound = bound && s.bound_rt == world_rt;
	}
	char line[200];
	std::snprintf(line, sizeof(line), "the marks came in order, each through the direct channel (%zu of them)", marks.size());
	check(order, line);
	check(depth, "each mark handed A's depth");
	check(known, "each mark on a list the hooks have seen made");
	check(bound, "at the zone's begin, A bound with it (the world found by the mark)");
	check(bad_subresources == 0, "A's depth's barriers name plane 0 or 1 (or all) only");
	std::snprintf(line, sizeof(line), "the depth plane in DEPTH_WRITE for opaque draws (%u), DEPTH_READ for the blended piece (%u)", opaque_draws_plane_ok, blended_read);
	check(opaque_draws_plane_bad == 0 && opaque_draws_plane_ok != 0 && blended_read != 0, line);
	std::snprintf(line, sizeof(line), "viewports half a pixel in, depth 0..1 (%u of %u not)", viewport_bad, viewports);
	check(viewport_bad == 0 && viewports != 0, line);
	bool heaps = true;
	for (const auto &l : lists)
		heaps = heaps && l.second.draws_before_heaps == 0;
	check(heaps, "descriptor heaps set after every reset before the list draws");
	if (std::getenv("D3D8_ASYNCREADBACK") != nullptr)
	{
		// (asynchronous readbacks: the test's answer is an earlier frame's, nothing submitted or waited on for it)
		std::snprintf(line, sizeof(line), "asynchronous readbacks: no list closed for the visibility tests (%u of %u closed)", readbacks_ok, seq);
		check(readbacks_ok == 0, line);
	}
	else
	{
		std::snprintf(line, sizeof(line), "the list closed and submitted between each visibility test's copy and its LockRect (%u of %u)", readbacks_ok, seq);
		check(readbacks_ok == seq, line);
	}
	std::snprintf(line, sizeof(line), "the resolve: A turned readable, the next draw of %d vertices into the bigger back buffer", resolve_draw_vertices);
	check(resolve_draw_vertices == 4 && resolve_target != nullptr && resolve_target != world_rt && resolve_target->GetDesc().Width > AW, line);
	const hifi::host::stats st = hifi::host::get_stats();
	std::snprintf(line, sizeof(line), "every list submitted of the hooked class, none recorded unseen (%llu lists, %llu foreign, %llu calls unseen)",
		static_cast<unsigned long long>(st.lists_executed), static_cast<unsigned long long>(st.lists_foreign), static_cast<unsigned long long>(st.lists_unknown_state));
	check(st.lists_executed != 0 && st.lists_foreign == 0 && st.lists_unknown_state == 0, line);
	// (the hooks' own statistic: the largest target seen turning readable, the first of equal size; the visibility test's
	// target is A's size, as in the game)
	check(st.world.width == AW && st.world.height == AH, "the hooks' own count finds a world-sized target turning readable");
	check(injected == 1, "HiFi XI drew in at the zone's end mark");
	size_t same = 0;
	for (size_t i = 0; i < plain.size() && i < injected_px.size(); ++i)
		same += plain[i] == injected_px[i] ? 1 : 0;
	std::snprintf(line, sizeof(line), "the frame drawn after HiFi XI's own state is the same, pixel for pixel: the d3d8.dll bound its own again (%zu of %zu)", same, plain.size());
	check(!plain.empty() && same == plain.size(), line);
	// The direct channel: HifiFrameMark answers 0 without a handler; with one, it's called on the list with A and its
	// depth in their states (A a target, the depth's planes as drawn), and the game's next draw lands where it should
	{
		const auto set = reinterpret_cast<hifi_frame_set_handler_fn>(GetProcAddress(m, kHifiFrameSetHandler));
		const auto mark = reinterpret_cast<hifi_frame_mark_fn>(GetProcAddress(m, kHifiFrameMark));
		check(set != nullptr && mark != nullptr, "the d3d8.dll exports HifiFrameSetHandler and HifiFrameMark");
		if (set != nullptr && mark != nullptr)
		{
			check(mark(hifi::slotmark::zone_end, 1) == 0, "HifiFrameMark without a handler: 0 (the game side counts it not sent)");
			set(&direct_handler, nullptr);
			dev->BeginScene();
			dev->SetRenderTarget(A, Az);
			D3DVIEWPORT8 vp = { 0, 0, AW, AH, 0.0f, 1.0f };
			dev->SetViewport(&vp);
			dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
			dev->SetTexture(0, nullptr);
			dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			dev->SetRenderState(D3DRS_ZENABLE, TRUE);
			dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
			tri_strip_band(0.5f, 0.75f, 0.5f, 0xFF404040, 4);
			const int answered = mark(hifi::slotmark::zone_begin, 7);
			tri_strip_band(-0.5f, 0.0f, 0.5f, 0xFF808080, 4);
			// (A read into a small target, as a distortion grabs it mid-frame: not the world's end)
			dev->SetRenderTarget(tiny, nullptr);
			dev->SetTexture(0, A_tex);
			dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			quad_rhw(-0.5f, -0.5f, 15.5f, 15.5f, 0xFFFFFFFF);
			dev->SetTexture(0, nullptr);
			const uint32_t calls_after_grab = g_direct.calls;
			dev->SetRenderTarget(A, Az);
			const RECT whole = { 0, 0, LONG(AW), LONG(AH) };
			const POINT p0 = { 0, 0 };
			dev->CopyRects(A, &whole, 1, out, &p0);
			D3DLOCKED_RECT o;
			out->LockRect(&o, nullptr, D3DLOCK_READONLY);
			const auto at = [&](UINT x, UINT y) { return *reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(o.pBits) + size_t(y) * o.Pitch + x * 4) & 0xFFFFFFu; };
			const uint32_t corner = at(2, 2), band = at(AW / 2, AH * 5 / 8), past = at(AW / 2, AH * 7 / 8);
			out->UnlockRect();
			dev->EndScene();
			dev->Present(nullptr, nullptr, nullptr, nullptr);
			set(nullptr, nullptr);
			const hifi_frame_mark &fm = g_direct.m;
			check(answered == 1 && g_direct.kinds[0] == hifi::slotmark::zone_begin && g_direct.seqs[0] == 7,
				"HifiFrameMark: the handler called with the kind and seq");
			std::snprintf(line, sizeof(line), "... and the world's end sent once, by itself, at the first copy reading A (%u calls; the second kind %u, seq %u)",
				g_direct.calls, g_direct.kinds[1], g_direct.seqs[1]);
			check(g_direct.calls == 2 && g_direct.kinds[1] == hifi::slotmark::world_end && g_direct.seqs[1] == 7, line);
			check(calls_after_grab == 1, "... and not at a draw reading A into a 16 x 16 target (a distortion's grab, mid-frame)");
			check(fm.list != nullptr && fm.rt == world_rt && fm.ds == world_ds && fm.width == AW && fm.height == AH && fm.rt_state == D3D12_RESOURCE_STATE_RENDER_TARGET &&
					fm.rt_format == DXGI_FORMAT_R8G8B8A8_UNORM && fm.ds_format == DXGI_FORMAT_D24_UNORM_S8_UINT && fm.ds_plane_count == 2 &&
					fm.ds_planes[0] == D3D12_RESOURCE_STATE_DEPTH_WRITE,
				"... handed A and its depth, their views' formats, A a target and the depth's plane 0 in DEPTH_WRITE");
			std::snprintf(line, sizeof(line), "... what it drew stays (corner %06X), and the game's next draw binds A, its viewport and pipeline again "
				"(band %06X, below it %06X)", corner, band, past);
			check(corner == 0xFFFFFF && band == 0x808080 && past == 0, line);
			// The channel's version 2: the world (A and its depth, named at the zone's begin) at every mark, bound or not
			const auto version = reinterpret_cast<hifi_frame_version_fn>(GetProcAddress(m, kHifiFrameVersion));
			check(version != nullptr && version() == kHifiFrameChannel, "HifiFrameVersion: 2 (the marks carry the world's states)");
			check(fm.world_rt == world_rt && fm.world_ds == world_ds && fm.world_rt_state == fm.rt_state && fm.world_ds_plane_count == 2 &&
					fm.world_ds_planes[0] == fm.ds_planes[0] && fm.world_ds_planes[1] == fm.ds_planes[1],
				"... at the zone's begin, the world is what's bound: the same resources and states");
			const hifi_frame_mark &fe = g_direct.end;
			check(fe.world_rt == world_rt && fe.world_ds == world_ds && fe.world_ds_plane_count == 2,
				"... at the world's end, A and its depth named as the world");
			// (a mark with only a small target bound, no depth: the world's depth and planes handed all the same, as the
			// back buffer's marks need them)
			g_direct = direct_seen();
			set(&direct_handler, nullptr);
			dev->BeginScene();
			dev->SetRenderTarget(A, Az);
			dev->SetViewport(&vp);
			tri_strip_band(0.5f, 0.75f, 0.5f, 0xFF404040, 4);
			mark(hifi::slotmark::zone_begin, 8);
			dev->SetRenderTarget(tiny, nullptr);
			mark(hifi::slotmark::ui_begin, 8);
			dev->SetRenderTarget(A, Az);
			dev->EndScene();
			dev->Present(nullptr, nullptr, nullptr, nullptr);
			set(nullptr, nullptr);
			const hifi_frame_mark &zb = g_direct.m, &ui = g_direct.end;
			std::snprintf(line, sizeof(line), "... at a mark with no depth bound, the world's depth and its planes all the same (planes %u, %u)",
				unsigned(ui.world_ds_planes[0]), unsigned(ui.world_ds_planes[1]));
			check(g_direct.calls == 2 && ui.ds == nullptr && ui.rt != world_rt && ui.world_ds == world_ds && ui.world_rt == world_rt &&
					ui.world_ds_planes[0] == zb.ds_planes[0] && ui.world_ds_planes[1] == zb.ds_planes[1], line);
			// HifiFrameDevice: the device the world was made on, and a direct queue
			const auto device_of = reinterpret_cast<hifi_frame_device_fn>(GetProcAddress(m, kHifiFrameDevice));
			ID3D12Device *d12 = nullptr, *made_on = nullptr;
			ID3D12CommandQueue *queue = nullptr;
			const bool named = device_of != nullptr && device_of(&d12, &queue) != 0;
			if (world_rt != nullptr)
				world_rt->GetDevice(IID_PPV_ARGS(&made_on));
			check(named && d12 != nullptr && d12 == made_on && queue != nullptr && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT,
				"HifiFrameDevice: the world's device and a direct queue (no submit has to be watched for them)");
			if (made_on != nullptr)
				made_on->Release();
		}
	}
	// The float world (HifiFrameSetWorldFormat): asked for, A is made again in RGBA16F at the zone's begin mark, before
	// the handler sees it, through the same view (the hooks see the new texture in it); the game's draws land in it and
	// light added past white stays there (a white band with half white added over it: 1.5 on the GPU); a copy of it into
	// memory comes back clamped, as an 8-bit target's; the resolve reads it; asked back to 8 bits, the next zone's begin
	// makes it RGBA8 again
	{
		const auto set = reinterpret_cast<hifi_frame_set_handler_fn>(GetProcAddress(m, kHifiFrameSetHandler));
		const auto mark = reinterpret_cast<hifi_frame_mark_fn>(GetProcAddress(m, kHifiFrameMark));
		const auto set_format = reinterpret_cast<hifi_frame_set_world_format_fn>(GetProcAddress(m, kHifiFrameSetWorldFormat));
		check(set_format != nullptr, "the d3d8.dll exports HifiFrameSetWorldFormat");
		if (set != nullptr && mark != nullptr && set_format != nullptr)
		{
			check(set_format(DXGI_FORMAT_R32_FLOAT) == 0 && set_format(DXGI_FORMAT_R16G16B16A16_FLOAT) == 1, "... turning down a format it doesn't make, taking RGBA16F");
			set(&float_handler, nullptr);
			// (one frame, the band drawn twice: white, then half white added)
			auto frame_float = [&](uint32_t s) {
				dev->BeginScene();
				dev->SetRenderTarget(A, Az);
				D3DVIEWPORT8 vp = { 0, 0, AW, AH, 0.0f, 1.0f };
				dev->SetViewport(&vp);
				dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
				mark(hifi::slotmark::zone_begin, s);
				dev->SetTexture(0, nullptr);
				dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
				dev->SetRenderState(D3DRS_ZENABLE, FALSE);
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
				tri_strip_band(-0.5f, 0.0f, 0.5f, 0xFFFFFFFF, 4);
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
				dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
				tri_strip_band(-0.5f, 0.0f, 0.5f, 0xFF808080, 4);
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
				dev->SetRenderState(D3DRS_ZENABLE, TRUE);
				const RECT whole = { 0, 0, LONG(AW), LONG(AH) };
				const POINT p0 = { 0, 0 };
				dev->CopyRects(A, &whole, 1, out, &p0); // (the world's end: the handler copies A first)
				D3DLOCKED_RECT o;
				out->LockRect(&o, nullptr, D3DLOCK_READONLY); // (waits for the GPU)
				const uint32_t band = *reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(o.pBits) + size_t(AH * 5 / 8) * o.Pitch + (AW / 2) * 4);
				out->UnlockRect();
				// The resolve: A read into the back buffer
				dev->SetRenderTarget(bb, nullptr);
				D3DVIEWPORT8 sv = { 0, 0, 1280, 720, 0.0f, 1.0f };
				dev->SetViewport(&sv);
				dev->SetRenderState(D3DRS_ZENABLE, FALSE);
				dev->SetTexture(0, A_tex);
				dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
				quad_rhw(-0.5f, -0.5f, 1279.5f, 719.5f, 0xFFFFFFFF);
				dev->SetTexture(0, nullptr);
				dev->SetRenderState(D3DRS_ZENABLE, TRUE);
				dev->EndScene();
				dev->Present(nullptr, nullptr, nullptr, nullptr);
				return band;
			};
			const uint32_t band = frame_float(9);
			float on_gpu[4] = {};
			if (g_float.rb != nullptr)
			{
				uint8_t *p = nullptr;
				const D3D12_RANGE r = { 0, SIZE_T(g_float.fp.Footprint.RowPitch) * g_float.fp.Footprint.Height };
				if (SUCCEEDED(g_float.rb->Map(0, &r, reinterpret_cast<void **>(&p))))
				{
					const uint16_t *h = reinterpret_cast<const uint16_t *>(p + size_t(AH * 5 / 8) * g_float.fp.Footprint.RowPitch + (AW / 2) * 8);
					for (int i = 0; i < 4; ++i)
						on_gpu[i] = half_f(h[i]);
					const D3D12_RANGE none = { 0, 0 };
					g_float.rb->Unmap(0, &none);
				}
				g_float.rb->Release();
				g_float.rb = nullptr;
			}
			const hifi_frame_mark &b = g_float.begin;
			const auto v = views.find(uint64_t(b.rtv.ptr));
			check(g_float.begun && b.rt != nullptr && b.rt != world_rt && b.rt_format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
					b.rt->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_TYPELESS && b.width == AW && b.height == AH && b.rt_state == D3D12_RESOURCE_STATE_RENDER_TARGET,
				"... at the zone's begin, A made again: RGBA16F (a typeless resource, float views), the same size, a target");
			check(v != views.end() && v->second.res == b.rt && v->second.format == DXGI_FORMAT_R16G16B16A16_FLOAT,
				"... in the same view, which the hooks saw made again with the new texture");
			char line[200];
			std::snprintf(line, sizeof(line), "... the band on the GPU past white: %.3f %.3f %.3f (1.5 wanted), alpha %.3f", on_gpu[0], on_gpu[1], on_gpu[2], on_gpu[3]);
			check(g_float.ended && std::fabs(on_gpu[0] - 1.502f) < 0.01f && std::fabs(on_gpu[1] - 1.502f) < 0.01f && std::fabs(on_gpu[2] - 1.502f) < 0.01f, line);
			std::snprintf(line, sizeof(line), "... copied into memory, clamped to white as an 8-bit target's (%08X)", band);
			check((band & 0xFFFFFF) == 0xFFFFFF, line);
			// Back to 8 bits
			set_format(DXGI_FORMAT_R8G8B8A8_UNORM);
			g_float = float_seen();
			frame_float(10);
			if (g_float.rb != nullptr)
				g_float.rb->Release();
			check(g_float.begun && g_float.begin.rt_format == DXGI_FORMAT_R8G8B8A8_UNORM && g_float.begin.rt->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_TYPELESS,
				"... asked back to 8 bits: A RGBA8 again at the next zone's begin");
			set(nullptr, nullptr);
		}
	}
	// The frame-rate speed-ups (HifiFrameSetSpeedups): an interface-like run of quads drawn with them off, with dedupe
	// and batch, and with batch alone must read back the same, pixel for pixel; batched, it takes fewer draws on the list.
	// In the run: quads that overlap (blended: order shows), the same shader and texture set again before each (dropped
	// by dedupe), a stage op changed mid-run, the texture locked and rewritten mid-run (the quads before keep its old
	// texels), flat shading (not batched), a fan (not a quad), a stencil-only marker clear, two quads reading a render
	// target as the resolve does (not batched)
	{
		const auto set_speedups = reinterpret_cast<hifi_frame_set_speedups_fn>(GetProcAddress(m, kHifiFrameSetSpeedups));
		const auto speedup_stats = reinterpret_cast<hifi_frame_speedup_stats_fn>(GetProcAddress(m, kHifiFrameSpeedupStats));
		check(set_speedups != nullptr && speedup_stats != nullptr, "the d3d8.dll exports HifiFrameSetSpeedups and HifiFrameSpeedupStats");
		IDirect3DTexture8 *ui = nullptr;
		dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &ui);
		auto fill_ui = [&](uint32_t c) {
			D3DLOCKED_RECT lr;
			if (ui != nullptr && SUCCEEDED(ui->LockRect(0, &lr, nullptr, 0)))
			{
				for (int y = 0; y < 16; ++y)
					for (int x = 0; x < 16; ++x)
						reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch)[x] = (x + y) % 3 == 0 ? 0x40FFFFFFu : c;
				ui->UnlockRect(0);
			}
		};
		auto glyphs = [&](int n, float y, uint32_t seed) {
			for (int i = 0; i < n; ++i)
			{
				const DWORD c = 0x80000000u | ((seed * 2654435761u + uint32_t(i) * 40503u) & 0xFFFFFFu);
				dev->SetTexture(0, ui);
				quad_rhw(30.0f + 23.0f * i, y, 70.0f + 23.0f * i, y + 50.0f, c); // (40 wide, 23 apart: each over the last)
			}
		};
		auto run = [&](uint32_t flags, uint32_t &d3d12_draws, uint32_t stats_out[4]) {
			if (set_speedups != nullptr)
				set_speedups(flags);
			uint32_t s0[4] = {};
			if (speedup_stats != nullptr)
				speedup_stats(s0);
			fill_ui(0xC040C080);
			const uint32_t d0 = uint32_t(draws);
			dev->BeginScene();
			dev->SetRenderTarget(A, Az);
			D3DVIEWPORT8 vp = { 0, 0, AW, AH, 0.0f, 1.0f };
			dev->SetViewport(&vp);
			dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF101820, 1.0f, 0);
			dev->SetRenderState(D3DRS_ZENABLE, FALSE);
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
			dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
			dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
			dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
			glyphs(30, 40, 1);
			dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
			glyphs(10, 100, 2);
			dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			glyphs(10, 160, 3);
			fill_ui(0xC0E04020); // (mid-run: the ten before keep the old texels)
			glyphs(10, 160, 4);
			dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_FLAT);
			glyphs(5, 220, 5);
			dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
			glyphs(5, 280, 6);
			const VR fan[5] = { { 400, 400, 0, 1, 0xC0FF8000, 0.5f, 0.5f }, { 340, 340, 0, 1, 0xC000FF80, 0, 0 }, { 460, 340, 0, 1, 0xC08000FF, 1, 0 },
				{ 460, 460, 0, 1, 0xC0FFFFFF, 1, 1 }, { 340, 460, 0, 1, 0xC0404040, 0, 1 } };
			dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 3, fan, sizeof(VR));
			glyphs(8, 340, 7);
			const D3DRECT pixel = { 1000, 10, 1001, 11 };
			dev->Clear(1, &pixel, D3DCLEAR_STENCIL, 0, 1.0f, 3); // (a slot marker's clear: the quads before it drawn first)
			glyphs(8, 400, 8);
			// (two quads reading a render target, as the resolve does: drawn as they are, never batched)
			dev->SetTexture(0, test_tex);
			quad_rhw(600, 300, 700, 380, 0xC0FFFFFF);
			quad_rhw(650, 320, 750, 400, 0xC0FFFFFF);
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			dev->SetRenderState(D3DRS_ZENABLE, TRUE);
			dev->SetTexture(0, nullptr);
			dev->EndScene();
			const RECT whole = { 0, 0, LONG(AW), LONG(AH) };
			const POINT p0 = { 0, 0 };
			dev->CopyRects(A, &whole, 1, out, &p0);
			std::vector<uint32_t> px(size_t(AW) * AH);
			D3DLOCKED_RECT o;
			if (SUCCEEDED(out->LockRect(&o, nullptr, D3DLOCK_READONLY)))
			{
				for (UINT y = 0; y < AH; ++y)
					std::memcpy(px.data() + size_t(y) * AW, static_cast<const uint8_t *>(o.pBits) + size_t(y) * o.Pitch, AW * 4);
				out->UnlockRect();
			}
			d3d12_draws = uint32_t(draws) - d0;
			dev->Present(nullptr, nullptr, nullptr, nullptr);
			if (speedup_stats != nullptr)
			{
				speedup_stats(stats_out);
				for (int i = 0; i < 4; ++i)
					stats_out[i] -= s0[i];
			}
			return px;
		};
		if (set_speedups != nullptr && speedup_stats != nullptr && ui != nullptr)
		{
			check(set_speedups(kHifiSpeedupsAsk) == 0, "... off until asked");
			uint32_t n_off = 0, n_both = 0, n_batch = 0, s_off[4] = {}, s_both[4] = {}, s_batch[4] = {};
			const std::vector<uint32_t> off = run(0, n_off, s_off);
			const std::vector<uint32_t> both = run(kHifiDedupe | kHifiBatch, n_both, s_both);
			const std::vector<uint32_t> batch = run(kHifiBatch, n_batch, s_batch);
			size_t same_both = 0, same_batch = 0;
			for (size_t i = 0; i < off.size(); ++i)
			{
				same_both += off[i] == both[i];
				same_batch += off[i] == batch[i];
			}
			char said[240];
			std::snprintf(said, sizeof(said), "... the quads read back the same with dedupe and batch (%zu of %zu pixels) and with batch alone (%zu)", same_both, off.size(), same_batch);
			check(same_both == off.size() && same_batch == off.size(), said);
			std::snprintf(said, sizeof(said), "... batched: %u draws on the list against %u (quads batched %u, in %u draws; flat-shaded ones, the fan and "
				"the render target's drawn as they are)", n_both, n_off, s_both[1], s_both[2]);
			// (88 quads, 5 of them flat and 2 reading a render target: 81 batched in 7 draws, cut by the two op changes, the
			// lock, flat shading, the fan, the marker and the render target's texture)
			check(s_off[1] == 0 && s_both[1] == 81 && s_both[2] == 7 && n_both + 74 == n_off && n_batch == n_both, said);
			std::snprintf(said, sizeof(said), "... dedupe dropped the shader and texture set again (%u calls; %u without it)", s_both[0], s_batch[0]);
			check(s_both[0] >= 2 * 86 - 10 && s_batch[0] == 0 && s_off[0] == 0, said);
			// Batch turned off with quads waiting: they're drawn there and then
			set_speedups(kHifiBatch);
			dev->BeginScene();
			dev->SetRenderTarget(A, Az);
			dev->SetTexture(0, ui);
			const uint32_t before = uint32_t(draws);
			uint32_t stats_before[4] = {};
			speedup_stats(stats_before);
			glyphs(3, 40, 9);
			const uint32_t waiting = uint32_t(draws) - before;
			set_speedups(0);
			const uint32_t after = uint32_t(draws) - before;
			uint32_t stats_after[4] = {};
			speedup_stats(stats_after);
			dev->SetTexture(0, nullptr);
			dev->EndScene();
			dev->Present(nullptr, nullptr, nullptr, nullptr);
			std::snprintf(said, sizeof(said), "... turned off with 3 quads waiting: drawn at once (%u draws before, %u after; %u batches)", waiting, after, stats_after[2] - stats_before[2]);
			check(waiting == 0 && after == 1 && stats_after[2] - stats_before[2] == 1, said);
			ui->Release();
		}
	}
	std::printf("%s (%d failed)\n", failures == 0 ? "all passed" : "FAILED", failures);
	std::fflush(stdout);
	ExitProcess(failures == 0 ? 0 : 1);
}
