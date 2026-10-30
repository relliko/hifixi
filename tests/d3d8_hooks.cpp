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
 */

#include "../src/host/hooks.hpp"
#include "../src/host/events.hpp"
#include "../src/game/slotmark.hpp"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3d8.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
	int failures = 0;
	void check(bool ok, const char *what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
		if (!ok)
			failures++;
	}

	// ---- What the hooks hand on ----
	struct view_info { ID3D12Resource *res; DXGI_FORMAT format; };
	std::map<uint64_t, view_info> views;
	struct mark_seen { uint32_t kind, seq; bool stencil_plane_write; bool rect_ok; uint64_t dsv; ID3D12Resource *bound_rt; ID3D12Resource *bound_ds; };
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

	// HiFi XI drawing into the frame: other state set on the list, then what was bound put back
	void draw_into(ID3D12GraphicsCommandList *l)
	{
		hifi::host::quiet q;
		const list_state s = lists[l];
		l->SetDescriptorHeaps(1, &own_heap);
		l->SetGraphicsRootSignature(own_root);
		l->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
		const D3D12_VIEWPORT vp = { 3.0f, 7.0f, 11.0f, 13.0f, 0.2f, 0.4f };
		l->RSSetViewports(1, &vp);
		l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
		// (put back as grass::list_state does: heaps first, then targets, viewports, scissors, pipeline, topology, root)
		if (s.heap_count != 0)
			l->SetDescriptorHeaps(s.heap_count, s.heaps);
		if (s.rtv_count != 0 || s.has_dsv)
			l->OMSetRenderTargets(s.rtv_count, s.rtv_count != 0 ? &s.rtv : nullptr, FALSE, s.has_dsv ? &s.dsv : nullptr);
		l->RSSetViewports(1, &s.vp);
		l->RSSetScissorRects(1, &s.sc);
		if (s.pso != nullptr)
			l->SetPipelineState(s.pso);
		if (s.topo != D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)
			l->IASetPrimitiveTopology(s.topo);
		if (s.root != nullptr)
		{
			l->SetGraphicsRootSignature(s.root);
			for (UINT p = 0; p < 2; ++p)
				if (s.cbv[p] != 0)
					l->SetGraphicsRootConstantBufferView(p, s.cbv[p]);
			for (UINT p = 2; p < 4; ++p)
				if (s.table[p].ptr != 0)
					l->SetGraphicsRootDescriptorTable(p, s.table[p]);
		}
		injected++;
	}

	void ev_clear_depth(ID3D12GraphicsCommandList *l, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS f, UINT n, const D3D12_RECT *r)
	{
		hifi::slotmark::mark m;
		if ((f & D3D12_CLEAR_FLAG_DEPTH) != 0 || r == nullptr || n == 0 ||
			!hifi::slotmark::read(false, (f & D3D12_CLEAR_FLAG_STENCIL) != 0, n, r[0].left, r[0].top, r[0].right, r[0].bottom, m))
			return;
		mark_seen s;
		s.kind = m.kind;
		s.seq = m.seq;
		s.dsv = uint64_t(dsv.ptr);
		s.rect_ok = n == 1 && r[0].right - r[0].left == 1 && r[0].bottom - r[0].top == 1;
		ID3D12Resource *ds = resource_of(dsv.ptr);
		s.stencil_plane_write = ds != nullptr && plane_state[1][ds] == D3D12_RESOURCE_STATE_DEPTH_WRITE;
		const list_state &ls = lists[l];
		s.bound_rt = ls.rtv_count != 0 ? resource_of(ls.rtv.ptr) : nullptr;
		s.bound_ds = ls.has_dsv ? resource_of(ls.dsv.ptr) : nullptr;
		marks.push_back(s);
		if (inject && m.kind == hifi::slotmark::zone_end)
			draw_into(l);
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
			x.clear_depth = ev_clear_depth;
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

	void marker(uint32_t kind)
	{
		const LONG x = LONG(hifi::slotmark::x_of(kind, seq));
		const D3DRECT r = { x, 0, x + 1, 1 };
		dev->Clear(1, &r, D3DCLEAR_STENCIL, 0, 1.0f, 0);
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

	// Frames: three plain, the fourth read back; then HiFi XI drawing in at the zone's end, the fifth read back
	std::vector<uint32_t> plain, injected_px;
	for (int f = 0; f < 4; ++f)
		plain = frame(f == 3);
	const size_t marks_plain = marks.size();
	inject = true;
	injected_px = frame(true);
	inject = false;

	// Markers
	bool order = marks_plain == 16, rects = true, planes = true, bound = true, views_known = true;
	for (size_t i = 0; i < marks.size(); ++i)
	{
		const mark_seen &s = marks[i];
		order = order && s.kind == uint32_t(i % 4) && s.seq == uint32_t((i / 4 + 1) & 15);
		rects = rects && s.rect_ok;
		planes = planes && s.stencil_plane_write;
		views_known = views_known && resource_of(s.dsv) == world_ds;
		if (s.kind == hifi::slotmark::zone_begin)
			bound = bound && s.bound_rt == world_rt && s.bound_ds == world_ds;
	}
	char line[200];
	std::snprintf(line, sizeof(line), "the slot markers came in order, as stencil-only clears of one pixel (%zu of them)", marks.size());
	check(order && rects, line);
	check(views_known, "each marker's view is A's depth (known from its making)");
	check(planes, "each marker's stencil plane in DEPTH_WRITE just before it (subresource 1)");
	check(bound, "at the zone's begin, A and its depth bound on the list (the world found by the marker)");
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
	check(injected == 1, "HiFi XI drew in at the zone's end marker");
	size_t same = 0;
	for (size_t i = 0; i < plain.size() && i < injected_px.size(); ++i)
		same += plain[i] == injected_px[i] ? 1 : 0;
	std::snprintf(line, sizeof(line), "the frame drawn after HiFi XI's state was put back is the same, pixel for pixel (%zu of %zu)", same, plain.size());
	check(!plain.empty() && same == plain.size(), line);
	std::printf("%s (%d failed)\n", failures == 0 ? "all passed" : "FAILED", failures);
	std::fflush(stdout);
	ExitProcess(failures == 0 ? 0 : 1);
}
