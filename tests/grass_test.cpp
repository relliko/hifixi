/*
 * Offline test of the grass renderer (HiFi XI's render engine, src\render\grass_render.cpp; from reshade-worldfx until 2026-10-27) on WARP, Direct3D 12's software device,
 * with the debug layer and GPU-based validation on. No game, no ReShade.
 *
 *   build\grass_test.exe [build\testdata\grass\100.bin]      (from this folder; build.cmd makes build\testdata: test_data.cpp)
 *
 * 1. Blades: a camera 2 yalms over the ground, 5 back from one blade, looking at it, with the game's
 *    kind of projection (right-handed, infinite far plane, near 0.1, y down). The world target starts
 *    in the pixel-shader-resource state (as after the game's own world pass), with alpha 0.5 and a
 *    "wall" at the near plane over its left quarter. Expected: blades at the projected point, none
 *    behind the wall, alpha untouched, the target back in its state.
 * 2. The game's state put back: a stand-in "game" binds a root signature with root constants, a root
 *    CBV, a descriptor table and a root SRV, a pipeline, a strip topology, an 8 x 8 viewport and its
 *    own target; grass is drawn and list_state::restore runs; the game's draw that follows, without
 *    binding anything again, must fill its whole target with (0.1, 0.2, 0.3, 0.4).
 * Any debug-layer error fails the test.
 */

#include "grass_render.hpp" // (HiFi XI's render engine: src\render)
#include <algorithm>
#include <chrono>
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

using namespace grass;

namespace
{
	int failures = 0;

	void check(bool ok, const char *what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
		if (!ok)
			failures++;
	}

	template <typename T>
	void release(T *&p)
	{
		if (p != nullptr)
			p->Release();
		p = nullptr;
	}

	struct vec3
	{
		float x, y, z;
	};
	vec3 sub(vec3 a, vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	vec3 cross(vec3 a, vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	vec3 normalize(vec3 a)
	{
		const float l = std::sqrt(dot(a, a));
		return { a.x / l, a.y / l, a.z / l };
	}

	// Right-handed look-at, row vectors (D3DXMatrixLookAtRH)
	void look_at(vec3 eye, vec3 at, vec3 up, float m[16])
	{
		const vec3 z = normalize(sub(eye, at));
		const vec3 x = normalize(cross(up, z));
		const vec3 y = cross(z, x);
		const float v[16] = {
			x.x, y.x, z.x, 0,
			x.y, y.y, z.y, 0,
			x.z, y.z, z.z, 0,
			-dot(x, eye), -dot(y, eye), -dot(z, eye), 1 };
		std::memcpy(m, v, sizeof(v));
	}

	// The game's projection: infinite far plane, near 0.1 (client-notes "ReShade setup")
	void projection(float aspect, float m[16])
	{
		const float b = 1.26f;
		const float p[16] = {
			b / aspect, 0, 0, 0,
			0, b, 0, 0,
			0, 0, -1, -1,
			0, 0, -0.1f, 0 };
		std::memcpy(m, p, sizeof(p));
	}

	// General 4x4 inverse (Gauss-Jordan); false when singular
	bool invert(const float m[16], float out[16])
	{
		double a[4][8];
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 8; ++c)
				a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
		for (int c = 0; c < 4; ++c)
		{
			int best = c;
			for (int r = c + 1; r < 4; ++r)
				if (std::fabs(a[r][c]) > std::fabs(a[best][c]))
					best = r;
			if (std::fabs(a[best][c]) < 1e-12)
				return false;
			for (int k = 0; k < 8; ++k)
				std::swap(a[c][k], a[best][k]);
			const double d = a[c][c];
			for (int k = 0; k < 8; ++k)
				a[c][k] /= d;
			for (int r = 0; r < 4; ++r)
				if (r != c)
				{
					const double f = a[r][c];
					for (int k = 0; k < 8; ++k)
						a[r][k] -= f * a[c][k];
				}
		}
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 4; ++c)
				out[r * 4 + c] = float(a[r][c + 4]);
		return true;
	}

	// The ground for the depth buffer: each pixel's view ray meets a flat ground at a height, and that's its
	// depth (the game's own ground is in its depth buffer when grass learns colours from it)
	const char kGroundShader[] = R"(
cbuffer c : register(b0) { row_major float4x4 vp; row_major float4x4 inv_vp; float4 info; }; // info: height, width, height in pixels
float4 vs_main(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float ps_main(float4 pos : SV_Position) : SV_Depth
{
	float2 ndc = float2(pos.x / info.y * 2.0 - 1.0, 1.0 - pos.y / info.z * 2.0);
	float4 a = mul(float4(ndc, 0.0, 1.0), inv_vp);
	float4 b = mul(float4(ndc, 0.5, 1.0), inv_vp);
	a /= a.w;
	b /= b.w;
	float3 dir = b.xyz - a.xyz;
	if (abs(dir.y) < 1e-6)
		return 1.0;
	float t = (info.x - a.y) / dir.y;
	if (t <= 0.0)
		return 1.0;
	// (info.w: bumps that high on it, 0.6 yalms across, as a rough surface's depth)
	float3 at = a.xyz + dir * t;
	t += info.w * sin(at.x * 10.472) * sin(at.z * 10.472) / dir.y;
	float4 c = mul(float4(a.xyz + dir * t, 1.0), vp);
	return saturate(c.z / c.w);
}
)";

	// A quad of one colour at one depth, depth-tested and not written (as the game's effects draw): rect in pixels, the
	// colour, info.x its depth, .y .z the target's size
	const char kQuadShader[] = R"(
cbuffer c : register(b0) { float4 rect; float4 colour; float4 info; };
float4 vs_main(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), info.x, 1.0);
}
float4 ps_main(float4 pos : SV_Position) : SV_Target
{
	if (pos.x < rect.x || pos.x >= rect.z || pos.y < rect.y || pos.y >= rect.w)
		discard;
	return colour;
}
)";

	pD3DCompile compile_fn = nullptr;

	std::vector<uint8_t> compile(const char *source, const char *entry, const char *target)
	{
		ID3DBlob *code = nullptr, *messages = nullptr;
		std::vector<uint8_t> out;
		if (SUCCEEDED(compile_fn(source, std::strlen(source), "test", nullptr, nullptr, entry, target, 0, 0, &code, &messages)))
			out.assign(static_cast<const uint8_t *>(code->GetBufferPointer()), static_cast<const uint8_t *>(code->GetBufferPointer()) + code->GetBufferSize());
		else
			std::printf("compile %s: %s\n", entry, messages != nullptr ? static_cast<const char *>(messages->GetBufferPointer()) : "?");
		release(code);
		release(messages);
		return out;
	}

	struct gpu
	{
		ID3D12Device *device = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		ID3D12CommandAllocator *allocator = nullptr;
		ID3D12GraphicsCommandList *list = nullptr;
		ID3D12Fence *fence = nullptr;
		UINT64 fence_value = 0;
		HANDLE event = nullptr;
		ID3D12InfoQueue *info = nullptr;

		void run()
		{
			list->Close();
			ID3D12CommandList *lists[] = { list };
			queue->ExecuteCommandLists(1, lists);
			queue->Signal(fence, ++fence_value);
			fence->SetEventOnCompletion(fence_value, event);
			WaitForSingleObject(event, INFINITE);
			allocator->Reset();
			list->Reset(allocator, nullptr);
		}

		ID3D12Resource *texture(DXGI_FORMAT format, UINT w, UINT h, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear)
		{
			D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = w;
			desc.Height = h;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			desc.Flags = flags;
			ID3D12Resource *res = nullptr;
			device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&res));
			return res;
		}

		ID3D12Resource *buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state)
		{
			D3D12_HEAP_PROPERTIES heap = { type };
			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = size;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ID3D12Resource *res = nullptr;
			device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
			return res;
		}

		// Copies a texture (in COPY_SOURCE) to memory: rows of w * bpp bytes
		std::vector<uint8_t> read(ID3D12Resource *tex, UINT bpp)
		{
			const D3D12_RESOURCE_DESC desc = tex->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
			UINT64 total = 0;
			device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
			ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
			D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
			dst.PlacedFootprint = fp;
			D3D12_TEXTURE_COPY_LOCATION src = { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
			src.SubresourceIndex = 0;
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			run();
			std::vector<uint8_t> out(size_t(desc.Width) * desc.Height * bpp);
			void *p = nullptr;
			rb->Map(0, nullptr, &p);
			for (UINT y = 0; y < desc.Height; ++y)
				std::memcpy(&out[size_t(y) * size_t(desc.Width) * bpp], static_cast<uint8_t *>(p) + size_t(y) * fp.Footprint.RowPitch, size_t(desc.Width) * bpp);
			rb->Unmap(0, nullptr);
			release(rb);
			return out;
		}

		int report_messages()
		{
			int errors = 0;
			if (info == nullptr)
				return 0;
			const UINT64 n = info->GetNumStoredMessages();
			for (UINT64 i = 0; i < n; ++i)
			{
				SIZE_T size = 0;
				info->GetMessage(i, nullptr, &size);
				std::vector<char> mem(size);
				auto *m = reinterpret_cast<D3D12_MESSAGE *>(mem.data());
				info->GetMessage(i, m, &size);
				if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
				{
					std::printf("debug layer: %s\n", m->pDescription);
					errors++;
				}
			}
			info->ClearStoredMessages();
			return errors;
		}
	};

	void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = res;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		list->ResourceBarrier(1, &b);
	}

	const char kGameShader[] = R"(
cbuffer rc : register(b0) { float4 rc; };
cbuffer cb : register(b1) { float4 cb; };
Texture2D<float4> tex : register(t0);
StructuredBuffer<float4> sb : register(t1);
float4 vs_main(uint id : SV_VertexID) : SV_Position
{
	// a strip of 4: only a strip covers the whole target
	return float4((id & 2) ? 1.0 : -1.0, (id & 1) ? 1.0 : -1.0, 0.5, 1.0);
}
float4 ps_main() : SV_Target
{
	return float4(rc.x, cb.y, tex.Load(int3(0, 0, 0)).z, sb[0].w);
}
)";
}

int main(int argc, char **argv)
{
	// ("clouds" as the argument: only the sky and its weather, 1x, the clouds' own sections with it: quick while working on
	// them; the zone's blades from the default file)
	const bool clouds_only = argc > 1 && std::strcmp(argv[1], "clouds") == 0;
	// ("look": only the look pass's own section, L below: quick while working on it)
	const bool look_only = argc > 1 && std::strcmp(argv[1], "look") == 0;
	// ("lamps": the blades' first sections, then only the lamps' own, 1p below)
	const bool lamps_only = argc > 1 && std::strcmp(argv[1], "lamps") == 0;
	const char *path = argc > 1 && !clouds_only && !look_only && !lamps_only ? argv[1] : "build\\testdata\\grass\\100.bin";
	std::setvbuf(stdout, nullptr, _IONBF, 0); // (each line out as it's printed: a crash shows where it was)

	compile_fn = reinterpret_cast<pD3DCompile>(GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile"));
	HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
	HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
	const auto get_debug = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(d3d12, "D3D12GetDebugInterface"));
	const auto create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
	using create_factory_fn = HRESULT(WINAPI *)(REFIID, void **);
	const auto create_factory = reinterpret_cast<create_factory_fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));

	ID3D12Debug *debug = nullptr;
	if (get_debug != nullptr && SUCCEEDED(get_debug(IID_PPV_ARGS(&debug))))
	{
		debug->EnableDebugLayer();
		ID3D12Debug1 *debug1 = nullptr;
		if (SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&debug1))))
		{
			debug1->SetEnableGPUBasedValidation(TRUE);
			release(debug1);
		}
	}
	check(debug != nullptr, "debug layer on");

	gpu g;
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	if (create_factory == nullptr || FAILED(create_factory(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
		FAILED(create_device(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device))))
	{
		std::printf("FAIL  no WARP device\n");
		return 1;
	}
	g.device->QueryInterface(IID_PPV_ARGS(&g.info));
	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));
	g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator));
	g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator, nullptr, IID_PPV_ARGS(&g.list));
	g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
	g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	// L. The look (look::post: tone mapping, bloom, the colour grade), over pictures put into a world target 320 x 180
	// (alpha 128, a name's pixel with alpha 1), nothing else of World FX on. All off: the world byte for byte as it went in
	// (and with the blades' phase, all on); the filmic curve on a grey ramp (exposure held at 1): mid-grey kept, the
	// shadows not lifted, white rolled off a little below white, the top of the ramp compressed, nothing going down; the
	// exposure: a bright noon picture darker, a dark one at night no brighter (the core's exposure_max 1 by night), one by
	// day hardly brighter, and moving slowly (a frame after a dark picture set at once, a bright one still comes out
	// brighter than it does set at once); bloom: a white square on dark grey lights the grey near it and nothing far off,
	// a picture with nothing bright is left as it was; the grade (colour_grade) on grey with a saturated patch: neutral at
	// a fine noon, warm at dusk, cool and no brighter at night, less saturated in rain and gloom, warm in heat waves and
	// dust storms, cool in snow, nothing under no open sky; names and the world's alpha left as they were.
	if (look_only)
	{
		renderer lr;
		std::string err;
		check(lr.init(g.device, err), "renderer init (look)");
		if (!err.empty())
			std::printf("  %s\n", err.c_str());
		const UINT LW = 320, LH = 180;
		D3D12_CLEAR_VALUE lds_clear = { DXGI_FORMAT_D24_UNORM_S8_UINT };
		lds_clear.DepthStencil.Depth = 1.0f;
		ID3D12Resource *lworld = g.texture(DXGI_FORMAT_R8G8B8A8_UNORM, LW, LH, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
		ID3D12Resource *ldepth = g.texture(DXGI_FORMAT_D24_UNORM_S8_UINT, LW, LH, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &lds_clear);
		ID3D12DescriptorHeap *lrtv_heap = nullptr, *ldsv_heap = nullptr;
		D3D12_DESCRIPTOR_HEAP_DESC lhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
		g.device->CreateDescriptorHeap(&lhd, IID_PPV_ARGS(&lrtv_heap));
		lhd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
		g.device->CreateDescriptorHeap(&lhd, IID_PPV_ARGS(&ldsv_heap));
		g.device->CreateRenderTargetView(lworld, nullptr, lrtv_heap->GetCPUDescriptorHandleForHeapStart());
		g.device->CreateDepthStencilView(ldepth, nullptr, ldsv_heap->GetCPUDescriptorHandleForHeapStart());
		g.list->ClearDepthStencilView(ldsv_heap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
		UINT64 total = 0;
		const D3D12_RESOURCE_DESC ldesc = lworld->GetDesc();
		g.device->GetCopyableFootprints(&ldesc, 0, 1, 0, &fp, nullptr, nullptr, &total);
		ID3D12Resource *up = g.buffer(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
		target lt;
		lt.rt = lworld;
		lt.ds = ldepth;
		lt.rtv = lrtv_heap->GetCPUDescriptorHandleForHeapStart();
		lt.rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
		lt.dsv_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		lt.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		lt.ds_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		camera lcam;
		look_at({ 0.0f, -2.0f, 0.0f }, { 0.0f, -2.0f, 10.0f }, { 0.0f, -1.0f, 0.0f }, lcam.view);
		projection(float(LW) / LH, lcam.proj);
		// A picture: rgb by pixel, alpha 128 but at the name's pixel (1)
		using picture = std::vector<uint8_t>;
		const UINT name_x = 5, name_y = 5;
		auto make = [&](auto rgb) {
			picture p(size_t(LW) * LH * 4);
			for (UINT y = 0; y < LH; ++y)
				for (UINT x = 0; x < LW; ++x)
				{
					uint8_t *q = &p[(size_t(y) * LW + x) * 4];
					rgb(x, y, q);
					q[3] = x == name_x && y == name_y ? 1 : 128;
				}
			return p;
		};
		// The look with nothing else of World FX's on
		auto bare = [] {
			look l;
			l.grass = false;
			l.ao_strength = l.relief = l.sun_shadow = 0.0f;
			l.water = false;
			l.sea = false;
			l.fireflies = 0.0f;
			return l;
		};
		// One frame: the picture in, the look drawn, the world back out
		uint32_t last_levels = 0;
		auto frame = [&](const picture &in, const look &l) {
			void *m = nullptr;
			up->Map(0, nullptr, &m);
			for (UINT y = 0; y < LH; ++y)
				std::memcpy(static_cast<uint8_t *>(m) + fp.Offset + size_t(y) * fp.Footprint.RowPitch, &in[size_t(y) * LW * 4], size_t(LW) * 4);
			up->Unmap(0, nullptr);
			barrier(g.list, lworld, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
			D3D12_TEXTURE_COPY_LOCATION dst = { lworld, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
			dst.SubresourceIndex = 0;
			D3D12_TEXTURE_COPY_LOCATION src = { up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
			src.PlacedFootprint = fp;
			g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			barrier(g.list, lworld, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
			std::string e;
			const draw_stats s = lr.draw(g.list, lt, lcam, l, e);
			if (!e.empty())
				std::printf("  draw: %s\n", e.c_str());
			last_levels = s.look ? s.look_levels : 0;
			barrier(g.list, lworld, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			picture out = g.read(lworld, 4);
			barrier(g.list, lworld, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.run();
			lr.release_old(2);
			return out;
		};
		auto px = [&](const picture &p, UINT x, UINT y, int c) { return int(p[(size_t(y) * LW + x) * 4 + c]); };
		// Mean of a channel (or of r, g, b: c = 3) over a rectangle
		auto mean = [&](const picture &p, UINT x0, UINT y0, UINT x1, UINT y1, int c) {
			double s = 0.0;
			for (UINT y = y0; y < y1; ++y)
				for (UINT x = x0; x < x1; ++x)
					s += c < 3 ? px(p, x, y, c) : (px(p, x, y, 0) + px(p, x, y, 1) + px(p, x, y, 2)) / 3.0;
			return s / (double(x1 - x0) * (y1 - y0));
		};
		auto save = [&](const picture &p, const char *file) {
			if (FILE *f = nullptr; fopen_s(&f, file, "wb") == 0 && f != nullptr)
			{
				std::fprintf(f, "P6 %u %u 255\n", LW, LH);
				for (size_t i = 0; i < size_t(LW) * LH; ++i)
					std::fwrite(&p[i * 4], 1, 3, f);
				fclose(f);
			}
		};
		// A picture's photo-like test scene: a grey ramp across (top half), colour bars (bottom half)
		const picture ramp = make([&](UINT x, UINT y, uint8_t *q) {
			const uint8_t v = uint8_t((x * 255 + (LW - 1) / 2) / (LW - 1));
			if (y < LH / 2)
				q[0] = q[1] = q[2] = v;
			else
			{
				q[0] = uint8_t(v * ((x / 40) % 2));
				q[1] = uint8_t(v * ((x / 80) % 2));
				q[2] = v;
			}
		});

		// All off: byte for byte; the blades' phase with all on: the same
		{
			look l = bare();
			const picture out = frame(ramp, l);
			check(out == ramp, "L: the look all off leaves the world byte for byte");
			l.post.tonemap = true;
			l.post.bloom = 1.0f;
			l.post.grade = true;
			l.post.gain[0] = 1.2f;
			l.phase = look::phase_t::blades;
			const picture out2 = frame(ramp, l);
			check(out2 == ramp, "L: never at the blades' phase (the zone's end)");
		}
		// The filmic curve, exposure held at 1
		{
			look l = bare();
			l.post.tonemap = true;
			l.post.exposure_min = l.post.exposure_max = 1.0f;
			l.post.snap = true;
			const picture out = frame(ramp, l);
			save(out, "build\\look_ramp.ppm");
			check(last_levels >= 3, "L: the look drawn with its levels");
			auto at = [&](int v) { const UINT x = UINT((v * (LW - 1) + 127) / 255); return px(out, x, LH / 4, 1); };
			std::printf("  ramp in -> out: 25 %d, 64 %d, 128 %d, 191 %d, 230 %d, 255 %d\n", at(25), at(64), at(128), at(191), at(230), at(255));
			check(std::abs(at(128) - 128) <= 8, "L: mid-grey kept (within 8)");
			check(at(25) <= 26 && at(64) <= 65, "L: the shadows not lifted");
			check(at(255) >= 238 && at(255) <= 252, "L: white rolled off a little below white (238..252)");
			check(at(255) - at(204) < 51, "L: the top of the ramp compressed (highlights rolled off)");
			int drops = 0;
			for (UINT x = 1; x < LW; ++x)
				if (px(out, x, LH / 4, 1) + 1 < px(out, x - 1, LH / 4, 1))
					drops++;
			check(drops == 0, "L: the curve never goes down");
			check(px(out, name_x, name_y, 0) == px(ramp, name_x, name_y, 0) && px(out, name_x, name_y, 3) == 1, "L: a name's pixel left as drawn");
			bool alpha = true;
			for (size_t i = 0; i < out.size(); i += 4)
				alpha = alpha && out[i + 3] == ramp[i + 3];
			check(alpha, "L: the world's alpha untouched");
		}
		// The exposure
		{
			auto flat = [&](uint8_t v) { return make([v](UINT, UINT, uint8_t *q) { q[0] = q[1] = q[2] = v; }); };
			const picture bright = flat(220), dark = flat(30);
			look l = bare();
			l.post.tonemap = true;
			l.post.snap = true;
			const double noon = mean(frame(bright, l), 10, 10, LW - 10, LH - 10, 3);
			const double day_dark = mean(frame(dark, l), 10, 10, LW - 10, LH - 10, 3);
			look n = l;
			n.post.exposure_max = 1.0f; // (the core's by night)
			const double night = mean(frame(dark, n), 10, 10, LW - 10, LH - 10, 3);
			std::printf("  exposure: bright 220 -> %.1f, dark 30 -> %.1f by day, %.1f at night\n", noon, day_dark, night);
			check(noon < 212.0, "L: a bright noon comes out darker (not blown out)");
			check(night <= 30.6, "L: a dark picture at night is no brighter");
			check(day_dark <= 33.5, "L: a dark picture by day hardly brighter");
			// Slowly: a dark picture set at once, then a bright one a sixtieth of a second later
			look a = l;
			a.clock = 100.0;
			frame(dark, a);
			a.post.snap = false;
			a.clock = 100.0 + 1.0 / 60.0;
			const double next = mean(frame(bright, a), 10, 10, LW - 10, LH - 10, 3);
			std::printf("  a frame after a dark picture: bright 220 -> %.1f (%.1f set at once)\n", next, noon);
			check(next > noon + 3.0, "L: the exposure moves slowly (a frame on, still the dark picture's)");
		}
		// Bloom
		{
			const picture spot = make([&](UINT x, UINT y, uint8_t *q) {
				const bool in = x >= LW / 2 - 4 && x < LW / 2 + 4 && y >= LH / 2 - 4 && y < LH / 2 + 4;
				q[0] = q[1] = q[2] = in ? 255 : 40;
			});
			look l = bare();
			l.post.bloom = 1.0f;
			const picture out = frame(spot, l);
			save(out, "build\\look_bloom.ppm");
			const double by_square = mean(out, LW / 2 + 8, LH / 2 - 2, LW / 2 + 16, LH / 2 + 2, 3);
			const double far_off = mean(out, 10, 10, 60, 40, 3);
			int far_most = 0;
			for (UINT y = 0; y < LH; ++y)
				for (UINT x = 0; x < LW; ++x)
					if (std::abs(int(x) - int(LW / 2)) > 70 || std::abs(int(y) - int(LH / 2)) > 70)
						far_most = std::max(far_most, std::abs(px(out, x, y, 1) - 40));
			std::printf("  bloom: grey 40 by the white square %.1f, far off %.1f (most off %d)\n", by_square, far_off, far_most);
			check(by_square >= 43.0, "L: bloom lights the grey near a bright square");
			check(far_most <= 1, "L: bloom leaves what's far from it (within the dither)");
			check(px(out, LW / 2, LH / 2, 1) >= 254, "L: the square itself stays white");
			const picture dull = make([](UINT x, UINT y, uint8_t *q) { q[0] = q[1] = q[2] = uint8_t(60 + (x + y) % 60); });
			const picture out2 = frame(dull, l);
			int most = 0;
			for (size_t i = 0; i < out2.size(); ++i)
				most = std::max(most, std::abs(int(out2[i]) - int(dull[i])));
			check(most <= 1, "L: nothing bright, no bloom (within the dither)");
			// A small bright dot (3 x 3) moved a pixel at a time across 8: the glow beside it changes in even steps, not in
			// one jump as the dot crosses from one of the first level's 4 x 4 blocks into the next (bright snow and stone
			// crawled at their edges as the camera moved, 2026-10-27)
			double glow[9] = {};
			for (int s = 0; s <= 8; ++s)
			{
				const UINT x0 = LW / 2 - 4 + UINT(s);
				const picture dot = make([&](UINT x, UINT y, uint8_t *q) {
					const bool in = x >= x0 && x < x0 + 3 && y >= LH / 2 && y < LH / 2 + 3;
					q[0] = q[1] = q[2] = in ? 255 : 40;
				});
				const picture o = frame(dot, l);
				glow[s] = mean(o, LW / 2 + 8, LH / 2 - 4, LW / 2 + 14, LH / 2 + 7, 3) - 40.0;
			}
			double biggest = 0.0;
			for (int s = 1; s <= 8; ++s)
				biggest = std::max(biggest, std::fabs(glow[s] - glow[s - 1]));
			const double range = std::fabs(glow[8] - glow[0]);
			std::printf("  bloom from a dot moved a pixel at a time: glow beside it %.2f .. %.2f, biggest step %.2f\n", glow[0], glow[8], biggest);
			check(range > 0.3 && biggest < 0.45 * range, "L: a small bright dot's glow moves smoothly as the dot does (no jumps between blocks)");
			// Round, not square: a dot's glow the same all the way round it, 20 and 36 pixels out (the coarse levels' texels
			// 16 and 32 pixels: drawn up between them one tap each, the glow came out in squares; Valkurm's sand at night,
			// 2026-10-27). The ring's spread (most - least) over its mean.
			{
				// (8 x 8, on black: the glow well over the dither 36 pixels out, read where the screen's curve is steep)
				const picture dot = make([&](UINT x, UINT y, uint8_t *q) {
					const bool in = x + 4 >= LW / 2 && x < LW / 2 + 4 && y + 4 >= LH / 2 && y < LH / 2 + 4;
					q[0] = q[1] = q[2] = in ? 255 : 0;
				});
				look lr2 = l;
				lr2.post.bloom = 8.0f;
				const picture o = frame(dot, lr2);
				save(o, "build\\look_bloom_dot.ppm");
				// (along the 4 diagonals against along the 4 axes, the same distances out, 3 x 3 means: a square glow reaches
				// further along its diagonals)
				const double cx = LW / 2 - 0.5, cy = LH / 2 - 0.5;
				auto at = [&](double x, double y) { return mean(o, UINT(std::lround(x)) - 1, UINT(std::lround(y)) - 1, UINT(std::lround(x)) + 2, UINT(std::lround(y)) + 2, 3); };
				for (const auto &band : { std::pair<double, double> { 12.0, 26.0 }, std::pair<double, double> { 26.0, 44.0 } })
				{
					double axes = 0.0, diagonals = 0.0;
					for (double r = band.first; r < band.second; r += 1.0)
					{
						const double d = r / std::sqrt(2.0);
						axes += at(cx + r, cy) + at(cx - r, cy) + at(cx, cy + r) + at(cx, cy - r);
						diagonals += at(cx + d, cy + d) + at(cx - d, cy + d) + at(cx + d, cy - d) + at(cx - d, cy - d);
					}
					const double n = 4.0 * (band.second - band.first);
					std::printf("  bloom round a dot %.0f..%.0f pixels out: along the axes %.2f, the diagonals %.2f (%.2f times)\n", band.first, band.second,
						axes / n, diagonals / n, diagonals / std::max(axes, 1e-6));
					// (one tap between the texels: 1.06 and 1.14 times; a tent of them: 1.04 and 1.04)
					check(axes / n > 1.0 && diagonals <= 1.08 * axes, band.first < 20.0 ? "L: a dot's glow round 12..26 pixels out (no squares)" : "L: a dot's glow round 26..44 pixels out (no squares)");
				}
			}
			// Steady: a bright ground near the threshold whose pixels change from frame to frame (sand and snow shimmering as
			// the camera moves), frames a 144th of a second apart: the glow it casts on the dark ground beside it hardly
			// changes from one frame to the next (each level of the bloom a share of its last frame's)
			{
				uint32_t seed = 12345;
				auto rnd = [&seed] {
					seed = seed * 1664525u + 1013904223u;
					return (seed >> 8) & 0xFFFF;
				};
				look lf = l;
				lf.post.bloom = 1.0f;
				lf.clock = 200.0;
				picture prev;
				double change = 0.0, glow_sum = 0.0;
				int counted = 0;
				for (int f = 0; f < 40; ++f)
				{
					const picture sand = make([&](UINT x, UINT y, uint8_t *q) {
						// (patches 6 pixels across, each its own brightness this frame: 150..255, around the knee)
						(void)x;
						q[0] = q[1] = q[2] = y < LH / 2 ? 40 : 0;
					});
					picture in = sand;
					for (UINT by = LH / 2; by < LH; by += 6)
						for (UINT bx = 0; bx < LW; bx += 6)
						{
							const uint8_t v = uint8_t(150 + rnd() % 106);
							for (UINT y = by; y < std::min(by + 6, LH); ++y)
								for (UINT x = bx; x < std::min(bx + 6, LW); ++x)
								{
									uint8_t *q = &in[(size_t(y) * LW + x) * 4];
									q[0] = q[1] = q[2] = v;
								}
						}
					const picture o = frame(in, lf);
					lf.clock += 1.0 / 144.0;
					if (f >= 20)
					{
						double d = 0.0, g2 = 0.0;
						for (UINT y = LH / 2 - 30; y < LH / 2 - 4; ++y)
							for (UINT x = 20; x < LW - 20; ++x)
							{
								d += std::abs(px(o, x, y, 1) - px(prev, x, y, 1));
								g2 += px(o, x, y, 1) - 40;
							}
						const double n = double(LW - 40) * 26.0;
						change += d / n;
						glow_sum += g2 / n;
						counted++;
					}
					prev = o;
				}
				change /= counted;
				glow_sum /= counted;
				std::printf("  bloom beside a shimmering bright ground, a 144th of a second apart: glow %.2f, changing %.3f a frame\n", glow_sum, change);
				// (the first level alone a fixed 0.45 of its last frame: 7.6 % of the glow a frame; each level by its time: 1.2 %)
				check(glow_sum > 1.0 && change < 0.03 * glow_sum, "L: the glow beside a shimmering bright ground stays steady from frame to frame");
				// ... and lets go soon: the ground dark, a quarter of a second later little of the glow is left
				const picture dark = make([](UINT, UINT y, uint8_t *q) { q[0] = q[1] = q[2] = y < LH / 2 ? 40 : 0; });
				picture o;
				for (int f = 0; f < 36; ++f)
				{
					o = frame(dark, lf);
					lf.clock += 1.0 / 144.0;
				}
				const double left = mean(o, 20, LH / 2 - 30, LW - 20, LH / 2 - 4, 1) - 40.0;
				std::printf("  a quarter of a second after the bright ground went dark: glow %.2f (of %.2f)\n", left, glow_sum);
				check(left < 0.15 * glow_sum, "L: the steadied glow lets go within a quarter of a second");
				// ... and a frame without bloom leaves nothing to go on: all bright with the bloom on (its levels kept bright), a
				// frame with the look on but the bloom off, then dark with the bloom on again: no glow from the bright frames
				const picture bright = make([](UINT, UINT, uint8_t *q) { q[0] = q[1] = q[2] = 250; });
				for (int f = 0; f < 3; ++f)
				{
					frame(bright, lf);
					lf.clock += 1.0 / 144.0;
				}
				look off = lf;
				off.post.bloom = 0.0f;
				off.post.tonemap = true;
				frame(bright, off);
				lf.clock += 1.0 / 144.0;
				const picture after = frame(dark, lf);
				int most = 0;
				for (size_t i = 0; i < after.size(); i += 4)
					most = std::max(most, std::abs(int(after[i + 1]) - int(dark[i + 1])));
				check(most <= 1, "L: no glow carried over from frames drawn without the bloom (within the dither)");
			}
		}
		// The grade's direction (no tone mapping, no bloom)
		{
			const picture grey = make([&](UINT x, UINT y, uint8_t *q) {
				const bool patch = x >= 200 && x < 260 && y >= 60 && y < 120;
				q[0] = patch ? 200 : 128;
				q[1] = patch ? 60 : 128;
				q[2] = patch ? 60 : 128;
			});
			struct scene
			{
				const char *what, *weather;
				float up, night;
				bool sunlit;
			};
			const scene scenes[] = { { "fine noon", "fine", 1.0f, 0.0f, true }, { "dusk", "fine", 0.05f, 0.0f, true },
				{ "night", "fine", -0.6f, 1.0f, true }, { "rain", "rain", 0.8f, 0.0f, true }, { "gloom", "dark", 0.8f, 0.0f, true },
				{ "heat wave", "heat", 1.0f, 0.0f, true }, { "dust storm", "dust", 0.9f, 0.0f, true }, { "snow", "snow", 0.8f, 0.0f, true },
				{ "no open sky", "rain", 0.05f, 0.0f, false } };
			for (const scene &s : scenes)
			{
				weather w;
				w.known = true;
				w.id = 0;
				w.name = uint32_t(uint8_t(s.weather[0])) | uint32_t(uint8_t(s.weather[1])) << 8 | uint32_t(uint8_t(s.weather[2])) << 16 | uint32_t(uint8_t(s.weather[3])) << 24;
				w.change = 1.0f;
				look l = bare();
				colour_grade(s.up, s.night, s.sunlit, &w, 1.0f, l.post);
				const picture out = frame(grey, l);
				const double r = mean(out, 20, 20, 120, 160, 0), gg = mean(out, 20, 20, 120, 160, 1), b = mean(out, 20, 20, 120, 160, 2);
				const double pr = mean(out, 210, 70, 250, 110, 0), pg = mean(out, 210, 70, 250, 110, 1);
				const double chroma = pr - pg; // (the patch's: 140 as it went in)
				std::printf("  grade %-11s grey -> %.1f %.1f %.1f, the red patch's chroma %.1f%s\n", s.what, r, gg, b, chroma, l.post.grade ? "" : " (no grade)");
				const std::string what = std::string("L: grade, ") + s.what + ": ";
				if (std::strcmp(s.what, "fine noon") == 0)
					check(std::fabs(r - b) <= 2.0 && std::fabs(chroma - 140.0) <= 6.0, (what + "about neutral").c_str());
				else if (std::strcmp(s.what, "dusk") == 0)
					check(r > b + 5.0, (what + "warm").c_str());
				else if (std::strcmp(s.what, "night") == 0)
					check(b > r + 4.0 && (r + gg + b) / 3.0 <= 128.5, (what + "cool, no brighter").c_str());
				else if (std::strcmp(s.what, "rain") == 0 || std::strcmp(s.what, "gloom") == 0)
					check(chroma < 140.0 * 0.85 && std::fabs(r - b) <= 6.0, (what + "less saturated, grey").c_str());
				else if (std::strcmp(s.what, "heat wave") == 0 || std::strcmp(s.what, "dust storm") == 0)
					check(r > b + 5.0, (what + "warm haze").c_str());
				else if (std::strcmp(s.what, "snow") == 0)
					check(b > r + 2.0, (what + "cool").c_str());
				else
					check(!l.post.grade, (what + "no grade").c_str());
			}
		}
		check(g.report_messages() == 0, "L: no debug-layer errors (the look)");
		release(up);
		release(lrtv_heap);
		release(ldsv_heap);
		release(lworld);
		release(ldepth);
		std::printf("%s (%d failed)\n", failures == 0 ? "all ok" : "FAILED", failures);
		return failures == 0 ? 0 : 1;
	}

	// The blades
	blade_file file;
	std::string error;
	check(read_blade_file(std::wstring(path, path + std::strlen(path)).c_str(), file, error), "read the blade file");
	if (file.blade_count() == 0)
	{
		std::printf("%s\n", error.c_str());
		return 1;
	}
	renderer r;
	check(r.init(g.device, error), "renderer init (shaders, root signature)");
	// (a fresh colour memory, the old one released at once: the tests forget only after a frame is read back, the GPU done
	// with it, and WARP runs out of memory keeping them all)
	auto fresh_memory = [&] {
		r.forget_colours(error);
		r.release_old(0);
	};
	if (!error.empty())
		std::printf("  %s\n", error.c_str());
	check(r.upload(file, error), "blades uploaded");

	// The camera: look at the first blade of the busiest cell, from 5 yalms away and 2 up (up is -y)
	const blade_file::cell *busiest = &file.cells[0];
	for (const blade_file::cell &c : file.cells)
		if (c.count > busiest->count)
			busiest = &c;
	const float *b = &file.blades[size_t(busiest->first) * 4];
	const vec3 at = { b[0], b[1], b[2] };
	const vec3 eye = { at.x + 4.0f, at.y - 2.0f, at.z + 3.0f };
	camera cam;
	look_at(eye, at, { 0.0f, -1.0f, 0.0f }, cam.view);
	const UINT W = 640, H = 360;
	projection(float(W) / H, cam.proj);
	float back[3];
	eye_from_view(cam.view, back);
	check(std::fabs(back[0] - eye.x) < 1e-3f && std::fabs(back[1] - eye.y) < 1e-3f && std::fabs(back[2] - eye.z) < 1e-3f, "camera position from the view matrix");
	std::printf("  blade at (%.2f, %.2f, %.2f), cell %d,%d with %u blades\n", at.x, at.y, at.z, busiest->cx, busiest->cz, busiest->count);

	// Where the blade's root should land (CPU), and its depth
	float vp[16];
	multiply(cam.view, cam.proj, vp);
	const float cx = at.x * vp[0] + at.y * vp[4] + at.z * vp[8] + vp[12];
	const float cy = at.x * vp[1] + at.y * vp[5] + at.z * vp[9] + vp[13];
	const float cz = at.x * vp[2] + at.y * vp[6] + at.z * vp[10] + vp[14];
	const float cw = at.x * vp[3] + at.y * vp[7] + at.z * vp[11] + vp[15];
	const float px = (cx / cw + 1.0f) * 0.5f * W, py = (1.0f - cy / cw) * 0.5f * H;
	std::printf("  projected to (%.1f, %.1f), depth %.5f\n", px, py, cz / cw);

	// 1. Blades into a world target in the pixel-shader-resource state, alpha 0.5, a wall over the left quarter
	// (a brown ground: blades take its brightness, turned green, so they're the pixels greener than red)
	D3D12_CLEAR_VALUE rt_clear = { DXGI_FORMAT_R8G8B8A8_UNORM, { 0.35f, 0.3f, 0.2f, 0.5f } };
	D3D12_CLEAR_VALUE ds_clear = { DXGI_FORMAT_D24_UNORM_S8_UINT };
	ds_clear.DepthStencil.Depth = 1.0f;
	ID3D12Resource *world = g.texture(DXGI_FORMAT_R8G8B8A8_UNORM, W, H, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &rt_clear);
	ID3D12Resource *depth = g.texture(DXGI_FORMAT_D24_UNORM_S8_UINT, W, H, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &ds_clear);
	ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
	g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
	g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap));
	const UINT rtv_step = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	const D3D12_CPU_DESCRIPTOR_HANDLE world_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	const D3D12_CPU_DESCRIPTOR_HANDLE screen_rtv = { world_rtv.ptr + rtv_step };
	const D3D12_CPU_DESCRIPTOR_HANDLE world_dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
	g.device->CreateRenderTargetView(world, nullptr, world_rtv);
	g.device->CreateDepthStencilView(depth, nullptr, world_dsv);

	// The ground pass: writes a flat ground at the looked-at blade's height into the world's depth
	ID3D12RootSignature *ground_root = nullptr;
	ID3D12PipelineState *ground_pso = nullptr;
	{
		D3D12_ROOT_PARAMETER gp = {};
		gp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		gp.Constants = { 0, 0, 36 };
		D3D12_ROOT_SIGNATURE_DESC grs = { 1, &gp };
		ID3DBlob *blob = nullptr, *err = nullptr;
		reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))(&grs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
		g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&ground_root));
		release(blob);
		release(err);
		const std::vector<uint8_t> vs = compile(kGroundShader, "vs_main", "vs_5_0"), ps = compile(kGroundShader, "ps_main", "ps_5_0");
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
		d.pRootSignature = ground_root;
		d.VS = { vs.data(), vs.size() };
		d.PS = { ps.data(), ps.size() };
		d.SampleMask = UINT_MAX;
		d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		d.DepthStencilState.DepthEnable = TRUE;
		d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		d.NumRenderTargets = 0;
		d.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
		d.SampleDesc.Count = 1;
		check(SUCCEEDED(g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&ground_pso))), "ground pass pipeline");
	}
	auto ground_depth = [&](const camera &c, float bumps = 0.0f) {
		float constants[36] = {};
		multiply(c.view, c.proj, constants);
		invert(constants, constants + 16);
		constants[32] = at.y;
		constants[33] = float(W);
		constants[34] = float(H);
		constants[35] = bumps;
		const D3D12_VIEWPORT gv = { 0.0f, 0.0f, float(W), float(H), 0.0f, 1.0f };
		const D3D12_RECT gs = { 0, 0, LONG(W), LONG(H) };
		g.list->SetGraphicsRootSignature(ground_root);
		g.list->SetPipelineState(ground_pso);
		g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		g.list->OMSetRenderTargets(0, nullptr, FALSE, &world_dsv);
		g.list->RSSetViewports(1, &gv);
		g.list->RSSetScissorRects(1, &gs);
		g.list->SetGraphicsRoot32BitConstants(0, 36, constants, 0);
		g.list->DrawInstanced(3, 1, 0, 0);
	};

	g.list->ClearRenderTargetView(world_rtv, rt_clear.Color, 0, nullptr);
	g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	const D3D12_RECT wall = { 0, 0, LONG(W / 4), LONG(H) };
	g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 1, &wall);
	barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	target t;
	t.rt = world;
	t.ds = depth;
	t.rtv = world_rtv;
	t.rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
	t.dsv_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	t.rt_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	t.ds_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	look l;
	const draw_stats stats = r.draw(g.list, t, cam, l, error);
	std::printf("  drew %u blades in %u draws%s%s\n", stats.blades, stats.draws, error.empty() ? "" : ": ", error.c_str());
	check(stats.draws > 0 && stats.blades > 0, "blades near the camera drawn");
	barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE); // fails validation if draw left it elsewhere
	const std::vector<uint8_t> pixels = g.read(world, 4);

	uint32_t green = 0, behind_wall = 0, near_point = 0, alpha_changed = 0;
	for (UINT y = 0; y < H; ++y)
		for (UINT x = 0; x < W; ++x)
		{
			const uint8_t *p = &pixels[(size_t(y) * W + x) * 4];
			if (p[3] != 128)
				alpha_changed++;
			if (p[1] <= p[0])
				continue;
			green++;
			if (x < W / 4)
				behind_wall++;
			if (std::fabs(x - px) < 6.0f && y < py + 3.0f && y > py - 30.0f)
				near_point++;
		}
	std::printf("  %u blade pixels, %u at the looked-at blade, %u behind the wall, %u with alpha changed\n", green, near_point, behind_wall, alpha_changed);
	check(green > 1000, "blades cover part of the picture");
	check(near_point > 5, "a blade stands where the CPU projection puts it");
	check(behind_wall == 0, "nothing drawn behind the near wall (depth test)");
	check(alpha_changed == 0, "the world's alpha is untouched");
	{
		const uint8_t *p = &pixels[(size_t(H / 2) * W + W / 2) * 4];
		std::printf("  colour at the centre: %u %u %u %u\n", p[0], p[1], p[2], p[3]);
	}

	// Writes a picture of it for a look: build\grass_test.ppm
	if (FILE *f = nullptr; fopen_s(&f, "build\\grass_test.ppm", "wb") == 0 && f != nullptr)
	{
		std::fprintf(f, "P6 %u %u 255\n", W, H);
		for (size_t i = 0; i < size_t(W) * H; ++i)
			std::fwrite(&pixels[i * 4], 1, 3, f);
		fclose(f);
	}
	check(g.report_messages() == 0, "no debug-layer errors (blades)");

	// (sections 1c to 1w skipped with "clouds": not indented again for it)
	if (!clouds_only)
	{
	// (with "lamps", on to 1p: every section between is a block of its own)
	if (lamps_only)
		goto lamps;
	// 1b2. The blades at the zone's end marker (look::phase_t::blades into the world's own depth: HiFi XI draws them
	// there, before the characters and effects): an effect drawn after them (depth-tested, no depth written, as the
	// game's) stays on top of them; a character standing just behind a blade is hidden by it where the blade is in
	// front, and seen beside it; the world's end then draws the rest (look::phase_t::rest) without blades again
	{
		ID3D12PipelineState *quad_pso = nullptr;
		ID3D12RootSignature *quad_root = nullptr;
		{
			D3D12_ROOT_PARAMETER qp = {};
			qp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			qp.Constants = { 0, 0, 12 };
			D3D12_ROOT_SIGNATURE_DESC qrs = { 1, &qp };
			ID3DBlob *blob = nullptr, *err = nullptr;
			reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))(&qrs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
			g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&quad_root));
			release(blob);
			release(err);
			const std::vector<uint8_t> vs = compile(kQuadShader, "vs_main", "vs_5_0"), ps = compile(kQuadShader, "ps_main", "ps_5_0");
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
			d.pRootSignature = quad_root;
			d.VS = { vs.data(), vs.size() };
			d.PS = { ps.data(), ps.size() };
			d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			d.SampleMask = UINT_MAX;
			d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			d.DepthStencilState.DepthEnable = TRUE;
			d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
			d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
			d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			d.NumRenderTargets = 1;
			d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
			d.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
			d.SampleDesc.Count = 1;
			check(SUCCEEDED(g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&quad_pso))), "zone's end: the quads' pipeline");
		}
		auto quad = [&](const float rect[4], const float colour[4], float z) {
			const float c[12] = { rect[0], rect[1], rect[2], rect[3], colour[0], colour[1], colour[2], colour[3], z, float(W), float(H), 0.0f };
			const D3D12_VIEWPORT qv = { 0.0f, 0.0f, float(W), float(H), 0.0f, 1.0f };
			const D3D12_RECT qs = { 0, 0, LONG(W), LONG(H) };
			g.list->SetGraphicsRootSignature(quad_root);
			g.list->SetPipelineState(quad_pso);
			g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			g.list->OMSetRenderTargets(1, &world_rtv, FALSE, &world_dsv);
			g.list->RSSetViewports(1, &qv);
			g.list->RSSetScissorRects(1, &qs);
			g.list->SetGraphicsRoot32BitConstants(0, 12, c, 0);
			g.list->DrawInstanced(3, 1, 0, 0);
		};
		fresh_memory();
		const float ground[4] = { 0.35f, 0.3f, 0.2f, 0.5f };
		barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		g.list->ClearRenderTargetView(world_rtv, ground, 0, nullptr);
		g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		ground_depth(cam);
		// (at the zone's end the world is a render target, its depth DEPTH_WRITE: as HiFi XI hands it over)
		target tz = t;
		tz.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		tz.ds_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		tz.blades_into_world_depth = true;
		look lz = l;
		lz.phase = look::phase_t::blades;
		lz.time = 0.0f;
		const draw_stats sz = r.draw(g.list, tz, cam, lz, error);
		check(sz.draws > 0 && sz.blades > 0 && error.empty(), ("zone's end: the blades drawn there (" + std::to_string(sz.blades) + ")" + (error.empty() ? "" : ": " + error)).c_str());
		check(!sz.shaded && sz.water_corners == 0 && sz.fireflies == 0, "  the blades alone: no shading, water or fireflies with them");
		// An effect over the middle (nearer than anything), and a character just behind the looked-at blade
		const float effect_rect[4] = { float(W) * 0.4f, float(H) * 0.6f, float(W) * 0.6f, float(H) * 0.9f };
		const float effect_colour[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
		const float char_rect[4] = { px - 12.0f, py - 40.0f, px + 12.0f, py };
		const float char_colour[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
		const float blade_z = cz / cw;
		quad(char_rect, char_colour, std::min(blade_z + 0.0005f * (1.0f - blade_z) * 4.0f, 0.99999f));
		quad(effect_rect, effect_colour, 0.0f);
		barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		// The world's end: the rest, without blades
		target tw = t;
		look lw = l;
		lw.phase = look::phase_t::rest;
		lw.time = 0.0f;
		const draw_stats sw = r.draw(g.list, tw, cam, lw, error);
		check(sw.blades == 0 && sw.draws == 0 && error.empty(), ("zone's end: the world's end draws no blades again (" + std::to_string(sw.blades) + ")").c_str());
		barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
		const std::vector<uint8_t> img = g.read(world, 4);
		r.release_old(0);
		uint32_t effect_px = 0, effect_kept = 0, effect_green = 0, char_seen = 0, char_hidden = 0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const uint8_t *q = &img[(size_t(y) * W + x) * 4];
				const float fx = float(x) + 0.5f, fy = float(y) + 0.5f;
				if (fx >= effect_rect[0] && fx < effect_rect[2] && fy >= effect_rect[1] && fy < effect_rect[3])
				{
					effect_px++;
					effect_kept += q[0] == 255 && q[1] == 0 && q[2] == 255;
					effect_green += q[1] > q[0]; // (a blade's green over it)
				}
				else if (fx >= char_rect[0] && fx < char_rect[2] && fy >= char_rect[1] && fy < char_rect[3])
				{
					if (q[0] == 0 && q[1] == 0 && q[2] == 255)
						char_seen++;
					else if (q[1] > q[0])
						char_hidden++; // (a blade's green in front of it)
				}
			}
		std::printf("  zone's end: effect %u of %u pixels as drawn (the rest shaded at the world's end), %u with a blade over them; character %u pixels seen, %u behind blades\n",
			effect_kept, effect_px, effect_green, char_seen, char_hidden);
		check(effect_px > 0 && effect_green == 0 && effect_kept > effect_px / 2, "zone's end: an effect drawn after the blades stays on top of them");
		check(char_seen > 0 && char_hidden > 0, "zone's end: a character just behind a blade is hidden by it, and seen beside it");
		check(g.report_messages() == 0, "no debug-layer errors (blades at the zone's end)");
		release(quad_pso);
		release(quad_root);

		// The look of it, as pictures (build\grass_test_zoneend_old.ppm, _new.ppm): the same frame drawn the old way (all
		// at the world's end: the ground shaded, then the blades) and the new (the blades at the zone's end, the shading
		// over them at the world's end), and how far apart they are
		auto picture = [&](bool at_marker) {
			fresh_memory();
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, ground, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			if (at_marker)
				r.draw(g.list, tz, cam, lz, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look la = l;
			la.time = 0.0f;
			la.phase = at_marker ? look::phase_t::rest : look::phase_t::all;
			r.draw(g.list, t, cam, la, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			std::vector<uint8_t> out = g.read(world, 4);
			r.release_old(0);
			return out;
		};
		const std::vector<uint8_t> old_way = picture(false), new_way = picture(true);
		uint64_t diff_sum = 0;
		uint32_t diff_px = 0;
		for (size_t i = 0; i < size_t(W) * H; ++i)
		{
			int d = 0;
			for (int c = 0; c < 3; ++c)
				d = std::max(d, std::abs(int(old_way[i * 4 + c]) - int(new_way[i * 4 + c])));
			diff_sum += d;
			diff_px += d > 8;
		}
		std::printf("  zone's end: the old way against the new: mean difference %.2f, %u pixels apart by more than 8\n", double(diff_sum) / (double(W) * H), diff_px);
		for (int k = 0; k < 2; ++k)
			if (FILE *f = nullptr; fopen_s(&f, k == 0 ? "build\\grass_test_zoneend_old.ppm" : "build\\grass_test_zoneend_new.ppm", "wb") == 0 && f != nullptr)
			{
				const std::vector<uint8_t> &px = k == 0 ? old_way : new_way;
				std::fprintf(f, "P6 %u %u 255\n", W, H);
				for (size_t i = 0; i < size_t(W) * H; ++i)
					std::fwrite(&px[i * 4], 1, 3, f);
				fclose(f);
			}
	}
	// 1c. Pushed aside: someone standing on the looked-at blade changes the picture there (the tufts around
	// bend away and flatten), and only around there
	{
		auto frame = [&](const float *who, uint32_t count) {
			fresh_memory();
			const float ground[4] = { 0.35f, 0.3f, 0.2f, 0.5f };
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, ground, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.actors = who;
			l2.actor_count = count;
			l2.time = 0.0f;
			r.draw(g.list, t, cam, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			std::vector<uint8_t> img = g.read(world, 4);
			r.release_old(0);
			return img;
		};
		const float someone[4] = { at.x, at.y, at.z, 1.0f };
		const std::vector<uint8_t> alone = frame(nullptr, 0), alone2 = frame(nullptr, 0), stood_on = frame(someone, 1);
		uint32_t changed = 0, same_twice = 0;
		double sx = 0.0, sy = 0.0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const size_t k = (size_t(y) * W + x) * 4;
				if (std::memcmp(&alone[k], &alone2[k], 3) != 0)
					same_twice++;
				if (std::memcmp(&alone[k], &stood_on[k], 3) != 0)
				{
					changed++;
					sx += x;
					sy += y;
				}
			}
		const double cxs = changed != 0 ? sx / changed : 0.0, cys = changed != 0 ? sy / changed : 0.0;
		std::printf("  someone standing on the looked-at blade: %u pixels change, centred at (%.0f, %.0f) (the blade at (%.0f, %.0f)); %u differ between two frames alone\n",
			changed, cxs, cys, px, py, same_twice);
		check(same_twice == 0, "the same frame twice is the same picture");
		check(changed > 100, "grass bends away from who stands in it"); // (a 1-yalm circle holds a few tufts at this size)
		check(std::fabs(cxs - px) < 60.0 && std::fabs(cys - py) < 60.0, "only around where they stand");
		check(g.report_messages() == 0, "no debug-layer errors (pushed aside)");
	}

	// 1c2. Worn and cut (the look's clock, its wear 1; a fresh memory for each part, kept from frame to frame within
	// it): the grass's green pixels in a box over the looked-at blade, and pictures compared away from it.
	// - Someone standing there 25 s (frames a quarter second apart: the most a frame counts) wears it down: fewer green
	//   pixels there once they've gone, the same away from it; 4 minutes later it's back.
	// - Fighting there (their bit in look::fighting) 5 s wears it down more than standing there 5 s.
	// - A weapon skill's cut (a circle 1.5 yalms round the blade, just made) leaves stubs: far fewer green pixels there from
	//   the next frame, the same away from it; growing back, more after 2 minutes, all after 4. A cut over kCutKeep old
	//   is left out; and with no wear and no cut, the picture is as without a clock at all.
	{
		auto frame = [&](double clock, const float *who, uint32_t count, uint32_t fighting, const float *cuts, uint32_t cut_count, float wear, draw_stats *st = nullptr) {
			const float ground[4] = { 0.35f, 0.3f, 0.2f, 0.5f };
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, ground, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.actors = who;
			l2.actor_count = count;
			l2.time = 0.0f;
			l2.clock = clock;
			l2.wear = wear;
			l2.fighting = fighting;
			l2.cuts = cuts;
			l2.cut_count = cut_count;
			const draw_stats s = r.draw(g.list, t, cam, l2, error);
			if (st != nullptr)
				*st = s;
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			std::vector<uint8_t> img = g.read(world, 4);
			r.release_old(0); // (the colour memories forgotten: the GPU is done with them once read, and WARP runs out)
			return img;
		};
		auto green_in = [&](const std::vector<uint8_t> &img, int x0, int y0, int x1, int y1) {
			uint32_t n = 0;
			for (int y = std::max(y0, 0); y < std::min(y1, int(H)); ++y)
				for (int x = std::max(x0, 0); x < std::min(x1, int(W)); ++x)
				{
					const uint8_t *p = &img[(size_t(y) * W + x) * 4];
					n += p[1] > p[0] ? 1u : 0u;
				}
			return n;
		};
		const int bx = int(px), by = int(py);
		auto at_blade = [&](const std::vector<uint8_t> &img) { return green_in(img, bx - 40, by - 50, bx + 40, by + 8); };
		// (away from it: the picture's far left and right thirds, outside 120 pixels of the blade)
		auto away_changed = [&](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
			uint32_t n = 0;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
					if (std::fabs(float(x) - px) > 120.0f && std::memcmp(&a[(size_t(y) * W + x) * 4], &b[(size_t(y) * W + x) * 4], 3) != 0)
						n++;
			return n;
		};

		// Stepping: 25 s stood on, then gone; 4 minutes later
		const float someone[4] = { at.x, at.y, at.z, 2.0f };
		fresh_memory();
		double clock = 100.0;
		std::vector<uint8_t> before;
		for (int i = 0; i < 8; ++i, clock += 0.25)
			before = frame(clock, nullptr, 0, 0, nullptr, 0, 1.0f); // (the colour memory settled first)
		for (int i = 0; i < 100; ++i, clock += 0.25)
			frame(clock, someone, 1, 0, nullptr, 0, 1.0f);
		const std::vector<uint8_t> trodden = frame(clock, nullptr, 0, 0, nullptr, 0, 1.0f);
		clock += 241.0;
		frame(clock, nullptr, 0, 0, nullptr, 0, 1.0f);
		const std::vector<uint8_t> recovered = frame(clock + 0.25, nullptr, 0, 0, nullptr, 0, 1.0f);
		const uint32_t g_before = at_blade(before), g_trodden = at_blade(trodden), g_recovered = at_blade(recovered);
		const uint32_t away_trodden = away_changed(before, trodden);
		std::printf("  worn: green pixels at the blade %u before, %u after 25 s stood on (%u changed away from it), %u four minutes later\n",
			g_before, g_trodden, away_trodden, g_recovered);
		check(g_trodden < g_before * 0.85, "stood on for a while, the grass stays trodden down after they've gone");
		check(away_trodden == 0, "only where they stood");
		check(g_recovered + 2 >= g_before * 0.97 && g_recovered <= g_before * 1.03 + 2, "four minutes later it's back up");

		// Fighting wears it down faster: 5 s each
		auto five_seconds = [&](uint32_t fighting) {
			fresh_memory();
			double c = 100.0;
			for (int i = 0; i < 8; ++i, c += 0.25)
				frame(c, nullptr, 0, 0, nullptr, 0, 1.0f);
			for (int i = 0; i < 20; ++i, c += 0.25)
				frame(c, someone, 1, fighting, nullptr, 0, 1.0f);
			return at_blade(frame(c, nullptr, 0, 0, nullptr, 0, 1.0f));
		};
		const uint32_t g_stood = five_seconds(0), g_fought = five_seconds(1);
		std::printf("  green pixels at the blade after 5 s there: %u stood, %u fighting\n", g_stood, g_fought);
		check(g_fought + 20 < g_stood, "fighting there wears it down faster than standing there");

		// A weapon skill's cut: a circle 1.5 yalms round the blade
		const float cut[8] = { at.x, at.y, at.z, 1.5f, at.x, at.y, at.z, 0.0f };
		fresh_memory();
		clock = 100.0;
		for (int i = 0; i < 8; ++i, clock += 0.25)
			before = frame(clock, nullptr, 0, 0, nullptr, 0, 1.0f);
		draw_stats cut_stats;
		frame(clock, nullptr, 0, 0, cut, 1, 1.0f, &cut_stats);
		const std::vector<uint8_t> just_cut = frame(clock + 0.25, nullptr, 0, 0, nullptr, 0, 1.0f);
		frame(clock + 120.0, nullptr, 0, 0, nullptr, 0, 1.0f);
		const std::vector<uint8_t> half_grown = frame(clock + 120.25, nullptr, 0, 0, nullptr, 0, 1.0f);
		frame(clock + 241.0, nullptr, 0, 0, nullptr, 0, 1.0f);
		const std::vector<uint8_t> grown = frame(clock + 241.25, nullptr, 0, 0, nullptr, 0, 1.0f);
		const uint32_t c_before = at_blade(before), c_cut = at_blade(just_cut), c_half = at_blade(half_grown), c_grown = at_blade(grown);
		const uint32_t away_cut = away_changed(before, just_cut);
		std::printf("  cut: green pixels at the blade %u before, %u just cut (%u changed away from it), %u after 2 minutes, %u after 4 (%u cut sent)\n",
			c_before, c_cut, away_cut, c_half, c_grown, cut_stats.cuts);
		check(cut_stats.wear && cut_stats.cuts == 1, "the cut went to the blades");
		check(c_cut < c_before * 0.5, "a weapon skill's cut leaves stubs");
		check(away_cut == 0, "only where it cut");
		check(c_half > c_cut + 10 && c_half < c_grown, "growing back: more after 2 minutes");
		check(c_grown + 2 >= c_before * 0.97 && c_grown <= c_before * 1.03 + 2, "all grown back after 4");

		// A cut too old is left out; no wear and no cut is as without a clock
		fresh_memory();
		clock = 100.0;
		for (int i = 0; i < 8; ++i, clock += 0.25)
			before = frame(clock, nullptr, 0, 0, nullptr, 0, 1.0f);
		float old_cut[8];
		std::memcpy(old_cut, cut, sizeof(old_cut));
		old_cut[7] = float(kCutKeep) + 1.0f;
		draw_stats old_stats;
		frame(clock, nullptr, 0, 0, old_cut, 1, 1.0f, &old_stats);
		const std::vector<uint8_t> after_old = frame(clock + 0.25, nullptr, 0, 0, nullptr, 0, 1.0f);
		check(old_stats.cuts == 0 && at_blade(after_old) == at_blade(before), "a cut over kCutKeep old is left out");
		fresh_memory();
		std::vector<uint8_t> no_clock, no_wear;
		draw_stats off_stats;
		for (int i = 0; i < 8; ++i)
			no_clock = frame(0.0, someone, 1, 0, nullptr, 0, 1.0f);
		fresh_memory();
		for (int i = 0; i < 8; ++i)
			no_wear = frame(100.0 + i * 0.25, someone, 1, 0, nullptr, 0, 0.0f, &off_stats);
		check(!off_stats.wear && no_clock == no_wear, "no wear and no cut: the picture is as without a clock");
		check(g.report_messages() == 0, "no debug-layer errors (worn and cut)");
	}

	// 1d. Fireflies at night: added light only (nothing darker than the same frame by day), somewhere over the
	// grass at some moment, none behind the near wall, the world's alpha untouched; with the grass off too
	{
		auto frame = [&](float night, float time, bool blades, draw_stats &st, float hours = 1.0f, float rain = 0.0f) {
			fresh_memory();
			const float ground[4] = { 0.1f, 0.12f, 0.08f, 0.5f };
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, ground, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			if (blades) // (without the grass, no ground either: nothing but the wall hides them, near ones included)
				ground_depth(cam);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 1, &wall);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.grass = blades;
			l2.night = night;
			l2.fireflies = 2.0f;
			l2.firefly_hours = hours;
			l2.rain = rain;
			l2.time = time;
			st = r.draw(g.list, t, cam, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			return g.read(world, 4);
		};
		uint32_t brighter = 0, darker = 0, behind = 0, alpha = 0, homes = 0, day_homes = 0, lone_brighter = 0;
		std::vector<uint8_t> brightest(size_t(W) * H * 4, 0); // (the grass off, the brightest of each pixel over the moments)
		for (int i = 0; i < 6; ++i)
		{
			const float time = 1.0f + i * 1.3f;
			draw_stats day_st, night_st, lone_st, lone_day_st;
			const std::vector<uint8_t> day = frame(0.0f, time, true, day_st), night = frame(1.0f, time, true, night_st);
			const std::vector<uint8_t> lone_day = frame(0.0f, time, false, lone_day_st), lone = frame(1.0f, time, false, lone_st);
			homes = std::max(homes, night_st.fireflies);
			day_homes += day_st.fireflies;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					const size_t k = (size_t(y) * W + x) * 4;
					for (int c = 0; c < 3; ++c)
						brightest[k + c] = std::max(brightest[k + c], lone[k + c]);
					bool up = false, lone_up = false;
					for (int c = 0; c < 3; ++c)
					{
						darker += night[k + c] < day[k + c] || lone[k + c] < lone_day[k + c] ? 1 : 0;
						up = up || night[k + c] > day[k + c];
						lone_up = lone_up || lone[k + c] > lone_day[k + c];
					}
					brighter += up ? 1 : 0;
					lone_brighter += lone_up ? 1 : 0;
					behind += (up || lone_up) && x < W / 4 ? 1 : 0;
					alpha += night[k + 3] != 128 || lone[k + 3] != 128 ? 1 : 0;
				}
		}
		// (a picture of them over all the moments, the grass off, for a look: build\fireflies_test.ppm)
		if (FILE *f = nullptr; fopen_s(&f, "build\\fireflies_test.ppm", "wb") == 0 && f != nullptr)
		{
			const std::vector<uint8_t> &shot = brightest;
			std::fprintf(f, "P6 %u %u 255\n", W, H);
			for (size_t i = 0; i < size_t(W) * H; ++i)
				std::fwrite(&shot[i * 4], 1, 3, f);
			fclose(f);
		}
		std::printf("  fireflies: %u homes near, %u pixels lit over 6 moments (%u with the grass off), %u darker, %u behind the wall, %u alpha changed\n",
			homes, brighter, lone_brighter, darker, behind, alpha);
		check(homes > 0 && day_homes == 0, "fireflies drawn at night, not by day");
		check(brighter > 20 && lone_brighter > 20, "fireflies light up a few spots, with or without the grass");
		check(darker == 0, "they only add light");
		check(behind == 0, "none behind the near wall (depth test)");
		check(alpha == 0, "the world's alpha is untouched (fireflies)");
		// Their hours and weather: half their hours (toward midnight) fewer, past them (after midnight) and in rain none
		{
			draw_stats half_st, late_st, rain_st;
			frame(1.0f, 1.0f, true, half_st, 0.5f);
			frame(1.0f, 1.0f, true, late_st, 0.0f);
			frame(1.0f, 1.0f, true, rain_st, 1.0f, 1.0f);
			std::printf("  fireflies' homes: %u in their hours, %u halfway out of them, %u past them, %u in rain\n", homes, half_st.fireflies,
				late_st.fireflies, rain_st.fireflies);
			check(half_st.fireflies > 0 && half_st.fireflies < homes && late_st.fireflies == 0 && rain_st.fireflies == 0,
				"fireflies fewer toward the end of their hours, none past them or in rain");
		}
		check(g.report_messages() == 0, "no debug-layer errors (fireflies)");
	}

	// 1d. What stands in front of a root isn't its ground: blades learn the ground, then someone reddish (as
	// bright, so it isn't a sudden change) stands over the root for 30 frames, nearer the camera; when they
	// step away, the blades there are the colour they were, not tinted red
	{
		const D3D12_RECT over = { LONG(px) - 40, LONG(py) - 50, LONG(px) + 40, LONG(py) + 20 };
		auto frame = [&](bool someone) {
			const float ground[4] = { 0.35f, 0.3f, 0.2f, 0.5f }, red[4] = { 0.6f, 0.15f, 0.15f, 0.5f };
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, ground, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			if (someone)
			{
				g.list->ClearRenderTargetView(world_rtv, red, 1, &over);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 1, &over);
			}
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.time = 0.0f;
			r.draw(g.list, t, cam, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
		};
		auto redness = [&]() {
			const std::vector<uint8_t> q = g.read(world, 4);
			double sum = 0.0;
			uint32_t n = 0;
			for (LONG y = over.top; y < over.bottom; ++y)
				for (LONG x = over.left; x < over.right; ++x)
				{
					if (x < 0 || y < 0 || x >= LONG(W) || y >= LONG(H))
						continue;
					const uint8_t *c = &q[(size_t(y) * W + x) * 4];
					if (std::abs(c[0] - 89) > 3 || std::abs(c[1] - 77) > 3 || std::abs(c[2] - 51) > 3) // (not the ground)
					{
						sum += double(c[0]) - c[1];
						n++;
					}
				}
			return n != 0 ? sum / n : 0.0;
		};
		fresh_memory();
		for (int i = 0; i < 5; ++i)
			frame(false);
		const double before = redness();
		for (int i = 0; i < 30; ++i)
			frame(true);
		frame(false);
		const double after = redness();
		std::printf("  blades over the root, red minus green: %.1f before someone stood there 30 frames, %.1f after\n", before, after);
		check(after - before < 3.0, "what stands in front of a root doesn't colour its grass");
		check(g.report_messages() == 0, "no debug-layer errors (in front)");
	}

	// 1e. Bright specks at the ground's own depth (a name's glyphs, drawn into the world): white stripes over a
	// half of the ground around the root don't make the blades there lighter than on plain ground
	{
		std::vector<D3D12_RECT> stripes;
		for (LONG x = LONG(px) - 60; x < LONG(px) + 60; x += 2)
			stripes.push_back({ x, LONG(py) - 60, x + 1, LONG(py) + 20 });
		auto light = [&](bool name) {
			fresh_memory();
			const float ground[4] = { 0.25f, 0.3f, 0.15f, 0.5f }, white[4] = { 1.0f, 1.0f, 1.0f, 0.5f };
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, ground, 0, nullptr);
			if (name)
				g.list->ClearRenderTargetView(world_rtv, white, UINT(stripes.size()), stripes.data());
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.time = 0.0f;
			r.draw(g.list, t, cam, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			const std::vector<uint8_t> q = g.read(world, 4);
			double sum = 0.0;
			uint32_t n = 0;
			for (LONG y = LONG(py) - 60; y < LONG(py) + 20; ++y)
				for (LONG x = LONG(px) - 60; x < LONG(px) + 60; ++x)
				{
					if (x < 0 || y < 0 || x >= LONG(W) || y >= LONG(H))
						continue;
					const uint8_t *c = &q[(size_t(y) * W + x) * 4];
					if (c[1] > c[0] + 8 && c[1] < 250) // (blades: green, not the white stripes)
					{
						sum += 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
						n++;
					}
				}
			return n != 0 ? sum / n : 0.0;
		};
		const double plain = light(false), named = light(true);
		std::printf("  blade brightness around the root: %.0f on plain ground, %.0f with white stripes over half of it\n", plain, named);
		check(named < plain * 1.12, "bright specks on the ground (a name) don't light up the grass");
		check(g.report_messages() == 0, "no debug-layer errors (specks)");
	}

	// 1g. A closer view wins: blades first seen from 20 yalms on light (fog-lit) ground, then from the usual 5
	// on dark ground, are dark within 20 frames (a fifth of a second), nearly as if they'd only ever been seen up close
	{
		const vec3 far_eye = { at.x + 16.0f, at.y - 6.0f, at.z + 12.0f };
		camera far_cam;
		look_at(far_eye, at, { 0.0f, -1.0f, 0.0f }, far_cam.view);
		std::memcpy(far_cam.proj, cam.proj, sizeof(cam.proj));
		auto frame = [&](const camera &c, const float *ground) {
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, ground, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(c);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.time = 0.0f;
			r.draw(g.list, t, c, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
		};
		const float light[4] = { 0.5f, 0.55f, 0.35f, 0.5f }, dark[4] = { 0.15f, 0.18f, 0.1f, 0.5f };
		auto brightness = [&]() {
			const std::vector<uint8_t> q = g.read(world, 4);
			double sum = 0.0;
			uint32_t n = 0;
			for (LONG y = LONG(py) - 30; y < LONG(py) + 10; ++y)
				for (LONG x = LONG(px) - 20; x < LONG(px) + 20; ++x)
				{
					const uint8_t *c = &q[(size_t(y) * W + x) * 4];
					if (std::abs(c[0] - 38) > 3 || std::abs(c[1] - 46) > 3 || std::abs(c[2] - 26) > 3) // (not the ground)
					{
						sum += 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
						n++;
					}
				}
			return n != 0 ? sum / n : 0.0;
		};
		fresh_memory();
		for (int i = 0; i < 10; ++i)
			frame(cam, dark);
		const double close_only = brightness();
		fresh_memory();
		for (int i = 0; i < 5; ++i)
			frame(far_cam, light);
		for (int i = 0; i < 20; ++i)
			frame(cam, dark);
		const double came_closer = brightness();
		std::printf("  blades on dark ground up close: brightness %.0f; first seen light from afar, then 20 frames up close: %.0f\n", close_only, came_closer);
		check(came_closer < close_only * 1.15 + 2.0, "a closer view takes over what was seen from afar");
		check(g.report_messages() == 0, "no debug-layer errors (closer)");
	}

	// 1h. The shading pass (grass off): flat plain ground comes out as it went in; ground just below
	// something standing on it is darker (ambient occlusion), ground away from it isn't; striped ground has
	// more contrast with surface relief on than off
	{
		const float plain[4] = { 0.45f, 0.42f, 0.36f, 0.5f }, dark_box[4] = { 0.2f, 0.2f, 0.2f, 0.5f };
		// Something standing on the ground behind the root on screen: a box of the depth a tenth of the way to the
		// camera (half a yalm in front of the ground there)
		const vec3 q = { at.x + (eye.x - at.x) * 0.1f, at.y + (eye.y - at.y) * 0.1f, at.z + (eye.z - at.z) * 0.1f };
		const float qz = q.x * vp[2] + q.y * vp[6] + q.z * vp[10] + vp[14], qw = q.x * vp[3] + q.y * vp[7] + q.z * vp[11] + vp[15];
		const D3D12_RECT box = { LONG(px) - 60, LONG(py) - 90, LONG(px) + 60, LONG(py) - 12 };
		std::vector<D3D12_RECT> stripes;
		for (LONG y = LONG(py) + 20; y < LONG(py) + 120; y += 6)
			stripes.push_back({ LONG(px) - 120, y, LONG(px) + 120, y + 3 });
		auto shaded = [&](bool with_box, bool striped, float ao, float relief) {
			const float stripe[4] = { 0.3f, 0.28f, 0.24f, 0.5f };
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
			if (striped)
				g.list->ClearRenderTargetView(world_rtv, stripe, UINT(stripes.size()), stripes.data());
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			if (with_box)
			{
				g.list->ClearRenderTargetView(world_rtv, dark_box, 1, &box);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, qz / qw, 0, 1, &box);
			}
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.grass = false;
			l2.ao_strength = ao;
			l2.ao_radius = 1.5f;
			l2.relief = relief;
			const draw_stats st = r.draw(g.list, t, cam, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			check(st.draws == 0 && st.shaded == (ao > 0.0f || relief > 0.0f), "the shading pass ran when on, without grass");
			return g.read(world, 4);
		};
		auto mean_luma = [&](const std::vector<uint8_t> &q, LONG x0, LONG y0, LONG x1, LONG y1, double *spread) {
			double sum = 0.0, sum2 = 0.0;
			uint32_t n = 0;
			for (LONG y = std::max(0L, y0); y < std::min(LONG(H), y1); ++y)
				for (LONG x = std::max(0L, x0); x < std::min(LONG(W), x1); ++x)
				{
					const uint8_t *c = &q[(size_t(y) * W + x) * 4];
					const double v = 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
					sum += v;
					sum2 += v * v;
					n++;
				}
			const double m = n != 0 ? sum / n : 0.0;
			if (spread != nullptr)
				*spread = n != 0 ? std::sqrt(std::max(0.0, sum2 / n - m * m)) : 0.0;
			return m;
		};
		const double plain_luma = 0.3 * 0.45 * 255 + 0.59 * 0.42 * 255 + 0.11 * 0.36 * 255;
		const std::vector<uint8_t> flat = shaded(false, false, 1.5f, 1.0f);
		const double flat_luma = mean_luma(flat, LONG(px) - 120, LONG(py) - 60, LONG(px) + 120, LONG(py) + 120, nullptr);
		const std::vector<uint8_t> boxed = shaded(true, false, 1.5f, 0.0f);
		const double below = mean_luma(boxed, LONG(px) - 50, LONG(py) - 10, LONG(px) + 50, LONG(py) + 2, nullptr);
		const double away = mean_luma(boxed, LONG(px) - 50, LONG(py) + 90, LONG(px) + 50, LONG(py) + 110, nullptr);
		double flat_spread = 0.0, relief_spread = 0.0;
		mean_luma(shaded(false, true, 0.0f, 0.0f), LONG(px) - 100, LONG(py) + 30, LONG(px) + 100, LONG(py) + 100, &flat_spread);
		mean_luma(shaded(false, true, 0.0f, 1.0f), LONG(px) - 100, LONG(py) + 30, LONG(px) + 100, LONG(py) + 100, &relief_spread);
		std::printf("  shading: flat plain ground %.1f (went in %.1f); by a box %.1f, away from it %.1f; stripes' spread %.1f, with relief %.1f\n",
			flat_luma, plain_luma, below, away, flat_spread, relief_spread);
		check(std::fabs(flat_luma - plain_luma) < 1.5, "flat plain ground comes out as it went in");
		check(below < away * 0.95 && std::fabs(away - plain_luma) < 2.0, "ground by something standing on it is darker, away from it not");
		check(relief_spread > flat_spread * 1.15, "surface relief brings out the ground's detail");
		check(g.report_messages() == 0, "no debug-layer errors (shading)");
	}

	// 1i. Shaders replaced while running (worldfx.hlsl next to the add-on): a source that doesn't compile is
	// refused and the ones in use stay; one that does is drawn with from the next draw (here a shading pass
	// painting the world magenta); then the built-in ones again
	{
		std::string source;
		if (FILE *f = nullptr; fopen_s(&f, "src\\render\\grass.hlsl", "rb") == 0 && f != nullptr)
		{
			char buffer[4096];
			size_t n;
			while ((n = fread(buffer, 1, sizeof(buffer), f)) != 0)
				source.append(buffer, n);
			fclose(f);
		}
		check(!source.empty(), "hifixi\\src\\render\\grass.hlsl read");
		auto magenta_share = [&]() {
			const float plain[4] = { 0.45f, 0.42f, 0.36f, 0.5f };
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.grass = false;
			l2.ao_strength = 0.8f; // (the shading pass on: it's off by default)
			r.draw(g.list, t, cam, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			const std::vector<uint8_t> q = g.read(world, 4);
			uint32_t m = 0;
			for (size_t i = 0; i < size_t(W) * H; ++i)
				if (q[i * 4] == 255 && q[i * 4 + 1] == 0 && q[i * 4 + 2] == 255)
					m++;
			return double(m) / (double(W) * H);
		};
		std::string why;
		check(!r.set_shader_source("this isn't HLSL", why) && !why.empty(), "a source that doesn't compile is refused, with the compiler's messages");
		const double before = magenta_share();
		const std::string marker = "return float4(haze + (col - haze) * (ao * relief * sunlit * wet_dark) + (glow + wet_add) * clear, 1.0);";
		std::string painted = source;
		const size_t at_marker = painted.find(marker);
		check(at_marker != std::string::npos, "the shading pass's return found in the source");
		if (at_marker != std::string::npos)
			painted.replace(at_marker, marker.size(), "return float4(1.0, 0.0, 1.0, 1.0);");
		why.clear();
		const bool swapped = r.set_shader_source(painted, why);
		const double after = magenta_share();
		r.set_shader_source(source, why);
		const double restored = magenta_share();
		std::printf("  shaders swapped while running: magenta %.0f%% before, %.0f%% with the changed source, %.0f%% after going back\n",
			before * 100.0, after * 100.0, restored * 100.0);
		check(swapped && before == 0.0 && after > 0.3 && restored == 0.0, "a changed source is drawn with from the next draw");
		check(g.report_messages() == 0, "no debug-layer errors (shaders swapped)");

		// The shader cache (compile_shaders with a folder: the add-on's worldfx-shader-cache). The built-in shaders are
		// hifixi's src\render\grass.hlsl byte for byte (the same key: the installed worldfx.hlsl beside them isn't compiled again). Small
		// sources with every entry point, each its own by a comment: compiled once, then read back from the folder the
		// same; a file with a byte changed or cut short isn't used (compiled again and written anew); a source that
		// doesn't compile leaves no file; only the kShaderCacheKeep written or used last are kept.
		{
			const uint64_t key = grass::shader_key(source);
			check(key != 0 && key == grass::shader_key(grass::built_in_shader_source()) && key != grass::shader_key(source + " "),
				"the built-in shaders have hifixi\\src\\render\\grass.hlsl's key, and a changed source another");
			const std::wstring folder = L"build\\test\\shader-cache";
			CreateDirectoryW(L"build\\test", nullptr);
			CreateDirectoryW(folder.c_str(), nullptr);
			WIN32_FIND_DATAW fd;
			if (HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd); h != INVALID_HANDLE_VALUE)
			{
				do
					if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
						DeleteFileW((folder + L"\\" + fd.cFileName).c_str());
				while (FindNextFileW(h, &fd));
				FindClose(h);
			}
			auto tiny = [](int n) {
				std::string s;
				for (const char *vs : { "vs_main", "shade_vs", "shadow_vs", "water_vs", "water_debug_vs", "firefly_vs", "rain_vs", "splash_vs", "snow_vs", "leaf_vs", "snow_layer_vs" })
					s += std::string("float4 ") + vs + "(uint id : SV_VertexID) : SV_Position { return float4(id, 0, 0, 1); }\n";
				for (const char *ps : { "ps_main", "shade_ps", "sun_ps", "shadow_ps", "water_ps", "water_debug_ps", "firefly_ps", "rain_ps", "splash_ps", "snow_ps", "vis_ps", "leaf_ps", "air_ps", "flash_ps", "cloud_ps", "cloud_shadow_ps", "look_first_ps",
					"look_down_ps", "look_exposure_ps", "look_up_ps", "look_ps", "trail_ps", "snow_layer_ps", "vol_ps", "ps_motion" })
					s += std::string("float4 ") + ps + "() : SV_Target { return float4(1, 0, 0, 1); }\n";
				return s + "// " + std::to_string(n) + "\n";
			};
			auto file_of = [&](const std::string &s) {
				char name[32];
				std::snprintf(name, sizeof(name), "\\%016llx.bin", static_cast<unsigned long long>(grass::shader_key(s)));
				return folder + std::wstring(name, name + std::strlen(name));
			};
			auto exists = [](const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
			auto bin_count = [&]() {
				uint32_t n = 0;
				if (HANDLE h = FindFirstFileW((folder + L"\\*.bin").c_str(), &fd); h != INVALID_HANDLE_VALUE)
				{
					do
						n++;
					while (FindNextFileW(h, &fd));
					FindClose(h);
				}
				return n;
			};
			grass::shader_code first, again, fresh;
			bool from_first = true, from_again = false, from_fresh = true;
			std::string cache_why;
			const bool compiled = grass::compile_shaders(tiny(0), folder, first, from_first, cache_why);
			const bool read_back = grass::compile_shaders(tiny(0), folder, again, from_again, cache_why);
			check(compiled && !from_first && exists(file_of(tiny(0))) && first.blobs.size() == 36, "a source compiled is written to the cache");
			check(read_back && from_again && again.blobs == first.blobs && again.key == first.key, "and read back from it the same");
			grass::compile_shaders(tiny(0), std::wstring(), fresh, from_fresh, cache_why);
			check(!from_fresh && fresh.blobs == first.blobs, "(the same as compiled without the cache)");
			// (a byte in the middle changed, then the file cut short)
			std::vector<uint8_t> bytes;
			if (FILE *f = nullptr; _wfopen_s(&f, file_of(tiny(0)).c_str(), L"rb") == 0 && f != nullptr)
			{
				uint8_t chunk[4096];
				size_t n;
				while ((n = fread(chunk, 1, sizeof(chunk), f)) != 0)
					bytes.insert(bytes.end(), chunk, chunk + n);
				fclose(f);
			}
			auto write_bytes = [&](const std::vector<uint8_t> &v) {
				if (FILE *f = nullptr; _wfopen_s(&f, file_of(tiny(0)).c_str(), L"wb") == 0 && f != nullptr)
				{
					fwrite(v.data(), 1, v.size(), f);
					fclose(f);
				}
			};
			bool changed_used = true, cut_used = true, rewritten = false;
			if (bytes.size() > 64)
			{
				std::vector<uint8_t> changed = bytes;
				changed[changed.size() / 2] ^= 0x10;
				write_bytes(changed);
				grass::shader_code c;
				grass::compile_shaders(tiny(0), folder, c, changed_used, cache_why);
				std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + bytes.size() / 2);
				write_bytes(cut);
				grass::compile_shaders(tiny(0), folder, c, cut_used, cache_why);
				grass::compile_shaders(tiny(0), folder, c, rewritten, cache_why);
				changed_used = changed_used || c.blobs != first.blobs;
			}
			check(!changed_used && !cut_used && rewritten, "a cache file with a byte changed, or cut short, isn't used: compiled again and written anew");
			grass::shader_code bad;
			bool from_bad = false;
			std::string bad_why;
			check(!grass::compile_shaders("this isn't HLSL", folder, bad, from_bad, bad_why) && !bad_why.empty() && !exists(file_of("this isn't HLSL")),
				"a source that doesn't compile leaves no file");
			// (six sources a moment apart, the second used again before the last: the four newest are kept, it among them)
			for (int n = 1; n <= 5; ++n)
			{
				Sleep(20);
				grass::shader_code c;
				bool from = false;
				grass::compile_shaders(tiny(n), folder, c, from, cache_why);
				if (n == 4)
				{
					Sleep(20);
					grass::compile_shaders(tiny(1), folder, c, from, cache_why);
				}
			}
			const uint32_t kept = bin_count();
			std::printf("  the shader cache: %u files kept of 6 sources\n", kept);
			check(kept == grass::kShaderCacheKeep && exists(file_of(tiny(1))) && exists(file_of(tiny(3))) && exists(file_of(tiny(4))) &&
				exists(file_of(tiny(5))) && !exists(file_of(tiny(0))) && !exists(file_of(tiny(2))),
				"only the four written or used last are kept (one used again stays)");
		}
	}

	// 1j. Sun shadows (grass off): flat ground in full sun comes out as it went in; with something standing
	// on the ground (the box of 1h) and the sun toward the camera and up, the ground just in front of it is
	// in its shadow, and ground further off isn't. With the sun to the right, the ground left of the box and
	// well behind it isn't shadowed either: the march passes behind the box there, not through it (a ghost
	// shadow on the far side of characters and trunks in game, from a 3-yalm thickness allowance)
	{
		const float plain[4] = { 0.45f, 0.42f, 0.36f, 0.5f }, dark_box[4] = { 0.2f, 0.2f, 0.2f, 0.5f };
		const vec3 q = { at.x + (eye.x - at.x) * 0.1f, at.y + (eye.y - at.y) * 0.1f, at.z + (eye.z - at.z) * 0.1f };
		const float qz = q.x * vp[2] + q.y * vp[6] + q.z * vp[10] + vp[14], qw = q.x * vp[3] + q.y * vp[7] + q.z * vp[11] + vp[15];
		const D3D12_RECT box = { LONG(px) - 60, LONG(py) - 90, LONG(px) + 60, LONG(py) - 12 };
		vec3 to_sun = normalize({ (eye.x - at.x) / 5.385f, (eye.y - at.y) / 5.385f - 1.0f, (eye.z - at.z) / 5.385f });
		D3D12_RECT pole = {};       // (a tall thin thing far off, at pole_depth: none while right is 0)
		float pole_depth = 0.0f;
		D3D12_RECT name = {};       // (pixels of a name facelift drew, marked alpha 1: none while right is 0)
		struct card
		{
			D3D12_RECT rect;
			float depth;
		};
		std::vector<card> cards; // (more things standing there, drawn in order after the box and the pole)
		const float *actors = nullptr; // (characters and monsters: the box as one, for the last check)
		uint32_t actor_count = 0;
		bool characters_cast = false;
		float weather_share = 1.0f;    // (the share of the sun's shadows the weather leaves)
		float haze_end = 0.0f;         // (the game's fog, in the ground's own colour, from 0 to this many yalms: none while 0)
		bool light_read = false;       // (the last frame read the host's light split: set_light_source)
		auto frame = [&](bool with_box, float shadows) {
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			if (with_box)
			{
				g.list->ClearRenderTargetView(world_rtv, dark_box, 1, &box);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, qz / qw, 0, 1, &box);
			}
			if (pole.right != 0)
			{
				g.list->ClearRenderTargetView(world_rtv, dark_box, 1, &pole);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, pole_depth, 0, 1, &pole);
			}
			for (const card &c : cards)
			{
				g.list->ClearRenderTargetView(world_rtv, dark_box, 1, &c.rect);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, c.depth, 0, 1, &c.rect);
			}
			if (name.right != 0)
			{
				const float marked[4] = { plain[0], plain[1], plain[2], 1.0f / 255.0f };
				g.list->ClearRenderTargetView(world_rtv, marked, 1, &name);
			}
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.grass = false;
			l2.ao_strength = 0.0f;
			l2.relief = 0.0f;
			l2.sun[0] = to_sun.x;
			l2.sun[1] = to_sun.y;
			l2.sun[2] = to_sun.z;
			l2.sun_shadow = shadows;
			l2.actors = actors;
			l2.actor_count = actor_count;
			l2.sun_characters = characters_cast;
			l2.weather_sun = weather_share;
			l2.haze = haze_end > 0.0f;
			std::memcpy(l2.haze_color, plain, sizeof(l2.haze_color));
			l2.haze_start = 0.0f;
			l2.haze_end = haze_end;
			const draw_stats st = r.draw(g.list, t, cam, l2, error);
			light_read = st.light_split;
			check(st.sun == (shadows * weather_share > 0.0f), "the sun's half-size pass ran when its shadows are on (and the weather leaves some)");
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			return g.read(world, 4);
		};
		auto mean = [&](const std::vector<uint8_t> &img, LONG x0, LONG y0, LONG x1, LONG y1) {
			double sum = 0.0;
			uint32_t n = 0;
			for (LONG y = std::max(0L, y0); y < std::min(LONG(H), y1); ++y)
				for (LONG x = std::max(0L, x0); x < std::min(LONG(W), x1); ++x)
				{
					const uint8_t *c = &img[(size_t(y) * W + x) * 4];
					sum += 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
					n++;
				}
			return n != 0 ? sum / n : 0.0;
		};
		const double plain_luma = 0.3 * 0.45 * 255 + 0.59 * 0.42 * 255 + 0.11 * 0.36 * 255;
		const double flat = mean(frame(false, 0.6f), LONG(px) - 120, LONG(py) - 60, LONG(px) + 120, LONG(py) + 120);
		const std::vector<uint8_t> lit = frame(true, 0.0f), shadowed = frame(true, 0.6f);
		const double near_lit = mean(lit, LONG(px) - 40, LONG(py) - 10, LONG(px) + 40, LONG(py) + 4);
		const double near_shadowed = mean(shadowed, LONG(px) - 40, LONG(py) - 10, LONG(px) + 40, LONG(py) + 4);
		const double far_lit = mean(lit, LONG(px) - 40, LONG(py) + 80, LONG(px) + 40, LONG(py) + 100);
		const double far_shadowed = mean(shadowed, LONG(px) - 40, LONG(py) + 80, LONG(px) + 40, LONG(py) + 100);
		std::printf("  sun shadows: flat ground %.1f (went in %.1f); in front of the box %.1f lit, %.1f with shadows; further off %.1f, %.1f\n",
			flat, plain_luma, near_lit, near_shadowed, far_lit, far_shadowed);
		check(std::fabs(flat - plain_luma) < 1.5, "flat ground in full sun comes out as it went in");
		check(near_shadowed < near_lit * 0.9, "the ground in front of something standing on it is in its shadow");
		check(std::fabs(far_shadowed - far_lit) < 1.0, "ground further off isn't");
		// (the camera's right in the world: the view's first column; and a little up, y being down)
		to_sun = normalize({ cam.view[0], cam.view[4] - 0.35f, cam.view[8] });
		const std::vector<uint8_t> side_lit = frame(true, 0.0f), side_shadowed = frame(true, 0.6f);
		const double behind_lit = mean(side_lit, LONG(px) - 110, LONG(py) - 70, LONG(px) - 66, LONG(py) - 30);
		const double behind_shadowed = mean(side_shadowed, LONG(px) - 110, LONG(py) - 70, LONG(px) - 66, LONG(py) - 30);
		std::printf("  sun to the right: ground left of the box and behind it %.1f lit, %.1f with shadows\n", behind_lit, behind_shadowed);
		check(std::fabs(behind_shadowed - behind_lit) < 1.0, "ground beside something and well behind it isn't shadowed by it");

		// The light split (set_light_source: HiFi XI's native zone and characters say each pixel's facing and how much of its
		// light is the sun's, src/zone/light_split.hpp): the ground facing up, its words made from the depth as drawn (the box
		// left unknown), the sun toward the camera and up again. A shadow takes the sun's share of the light as the word says,
		// scaled as sun_share's (1.39: the game's daylight share of a surface square to the sun, 0.72, takes all the slider's
		// strength): none from ground the word says the sun doesn't light (a surface lit by the ambient and lamps alone), half as
		// much at half the share; and where the word's depth isn't what's drawn (the box), the old way stands.
		{
			to_sun = normalize({ (eye.x - at.x) / 5.385f, (eye.y - at.y) / 5.385f - 1.0f, (eye.z - at.z) / 5.385f });
			frame(true, 0.0f);
			barrier(g.list, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			const std::vector<uint8_t> zb = g.read(depth, 4);
			barrier(g.list, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			ID3D12Resource *split = g.texture(DXGI_FORMAT_R32_UINT, W, H, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
			const D3D12_RESOURCE_DESC sd = split->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT sfp = {};
			UINT64 stotal = 0;
			g.device->GetCopyableFootprints(&sd, 0, 1, 0, &sfp, nullptr, nullptr, &stotal);
			ID3D12Resource *sup = g.buffer(D3D12_HEAP_TYPE_UPLOAD, stotal, D3D12_RESOURCE_STATE_GENERIC_READ);
			struct source
			{
				ID3D12Resource *texture;
			} src_ctx = { nullptr };
			r.set_light_source([](void *c) { return static_cast<source *>(c)->texture; }, &src_ctx);
			const float lx = cam.proj[11] != 0.0f ? cam.proj[10] / cam.proj[11] : cam.proj[10], ly = cam.proj[14];
			// (facing up, y down: octahedral (0, -1, 0) is (0, -1), each side 0..127)
			const uint32_t up_bits = uint32_t(std::lround(0.5 * 127.0)) << 11 | uint32_t(std::lround(0.0)) << 18;
			auto words = [&](float share) {
				uint32_t *w = nullptr;
				sup->Map(0, nullptr, reinterpret_cast<void **>(&w));
				for (UINT y = 0; y < H; ++y)
					for (UINT x = 0; x < W; ++x)
					{
						uint32_t v = 0;
						const uint32_t d24 = (zb[(size_t(y) * W + x) * 4] | zb[(size_t(y) * W + x) * 4 + 1] << 8 | zb[(size_t(y) * W + x) * 4 + 2] << 16);
						const bool in_box = LONG(x) >= box.left && LONG(x) < box.right && LONG(y) >= box.top && LONG(y) < box.bottom;
						if (d24 < 0xFFFFFF && !in_box)
						{
							const double vd = ly / (d24 / 16777215.0 - lx);
							const uint32_t code = uint32_t(std::clamp(std::lround((std::log2(std::max(vd, 1e-3)) + 3.0) * 128.0), 1L, 2047L));
							v = code | up_bits | uint32_t(std::lround(std::clamp(share, 0.0f, 1.0f) * 127.0f)) << 25;
						}
						w[size_t(y) * (sfp.Footprint.RowPitch / 4) + x] = v;
					}
				sup->Unmap(0, nullptr);
				if (src_ctx.texture != nullptr)
					barrier(g.list, split, kRead, D3D12_RESOURCE_STATE_COPY_DEST);
				D3D12_TEXTURE_COPY_LOCATION dst = { split, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, from = { sup, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
				from.PlacedFootprint = sfp;
				g.list->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
				barrier(g.list, split, D3D12_RESOURCE_STATE_COPY_DEST, kRead);
				src_ctx.texture = split;
			};
			words(0.0f);
			const std::vector<uint8_t> none_lit = frame(true, 0.0f), none_shadowed = frame(true, 0.6f);
			const bool read_none = light_read;
			words(0.36f);
			const std::vector<uint8_t> half_shadowed = frame(true, 0.6f);
			words(0.72f);
			const std::vector<uint8_t> full_lit = frame(true, 0.0f), full_shadowed = frame(true, 0.6f);
			const bool read_full = light_read;
			r.set_light_source(nullptr, nullptr);
			const std::vector<uint8_t> after = frame(true, 0.6f);
			const bool read_after = light_read;
			auto near_of = [&](const std::vector<uint8_t> &img) { return mean(img, LONG(px) - 40, LONG(py) - 10, LONG(px) + 40, LONG(py) + 4); };
			const double n_lit = near_of(none_lit), n0 = near_of(none_shadowed), n36 = near_of(half_shadowed), n72 = near_of(full_shadowed), f_lit = near_of(full_lit);
			const double box_split = mean(full_shadowed, box.left + 10, box.top + 10, box.right - 10, box.bottom - 10), box_old = mean(shadowed, box.left + 10, box.top + 10, box.right - 10, box.bottom - 10);
			std::printf("  the light split: read %d %d (after it's gone %d); in front of the box lit %.1f / %.1f, with shadows at a share of 0 %.1f, 0.36 %.1f, 0.72 %.1f "
				"(without the split %.1f); the box %.1f (without %.1f)\n", read_none, read_full, read_after, n_lit, f_lit, n0, n36, n72, near_shadowed, box_split, box_old);
			check(read_none && read_full && !read_after, "the shading pass read the light split while the host had one, and not after");
			check(std::fabs(n_lit - near_lit) < 1.0 && std::fabs(f_lit - near_lit) < 1.0, "lit ground is as it was, the split or not");
			check(std::fabs(n0 - n_lit) < 1.0, "no shadow on ground the sun doesn't light (its share 0)");
			check(n72 < n_lit * 0.9 && n36 < n_lit - 0.35 * (n_lit - n72) && n36 > n_lit - 0.65 * (n_lit - n72), "a shadow takes the sun's share: about half at half of it");
			check(std::fabs(box_split - box_old) < 1.0, "where the word isn't what's drawn (the box), as without the split");
			release(sup);
			release(split);
		}

		// A tall pole (a yalm wide, like a trunk) 20 yalms further off; the sun to the right, low (19 degrees up) and a little away
		// from the camera: the ground 10 to 13 yalms left of its foot is in its shadow (out of reach of the
		// 16-step march, 9 yalms along the ray: a tree's shadow stopped short of its height), the ground as far
		// right of it isn't
		{
			const vec3 right = { cam.view[0], cam.view[4], cam.view[8] }, ahead = { -cam.view[2], -cam.view[6], -cam.view[10] };
			const vec3 foot = { at.x + (at.x - eye.x) * 4.0f, at.y, at.z + (at.z - eye.z) * 4.0f };
			auto on_screen = [&](const vec3 &p, float *depth) {
				const float x = p.x * vp[0] + p.y * vp[4] + p.z * vp[8] + vp[12], y = p.x * vp[1] + p.y * vp[5] + p.z * vp[9] + vp[13];
				const float z = p.x * vp[2] + p.y * vp[6] + p.z * vp[10] + vp[14], w = p.x * vp[3] + p.y * vp[7] + p.z * vp[11] + vp[15];
				if (depth != nullptr)
					*depth = z / w;
				return std::make_pair((x / w + 1.0f) * 0.5f * W, (1.0f - y / w) * 0.5f * H);
			};
			auto along = [&](float d) { return vec3 { foot.x + right.x * d, foot.y + right.y * d, foot.z + right.z * d }; };
			const auto f = on_screen(foot, &pole_depth);
			pole = { LONG(f.first) - 5, 0, LONG(f.first) + 5, LONG(f.second) }; // (about a yalm wide)
			const auto l10 = on_screen(along(-10.0f), nullptr), l13 = on_screen(along(-13.0f), nullptr);
			const auto r10 = on_screen(along(10.0f), nullptr), r13 = on_screen(along(13.0f), nullptr);
			// (a little away from the camera: the march crosses the pole about half a yalm behind its face, through it,
			// as through a trunk; at a third of a yalm it was so near the near end of a hit's window that starting the
			// march two pixel widths off the ground lost it)
			to_sun = normalize({ right.x + ahead.x * 0.05f, right.y - 0.35f + ahead.y * 0.05f, right.z + ahead.z * 0.05f });
			const std::vector<uint8_t> pole_lit = frame(false, 0.0f), pole_shadowed = frame(false, 0.6f);
			// (a trunk's shadow is about as wide as the trunk: on ground 25 yalms off, seen this low, a band two or
			// three rows tall along the foot's row: its darkest row is measured)
			auto darkest_row = [&](const std::vector<uint8_t> &img, float x0, float x1, float y) {
				double least = 1e9;
				for (LONG row = LONG(y) - 5; row <= LONG(y) + 2; ++row)
					least = std::min(least, mean(img, LONG(x0), row, LONG(x1), row + 1));
				return least;
			};
			const float ly = (l10.second + l13.second) * 0.5f, ry = (r10.second + r13.second) * 0.5f;
			const double left_lit = darkest_row(pole_lit, l13.first, l10.first, ly);
			const double left_shadowed = darkest_row(pole_shadowed, l13.first, l10.first, ly);
			const double right_lit = darkest_row(pole_lit, r10.first, r13.first, ry);
			const double right_shadowed = darkest_row(pole_shadowed, r10.first, r13.first, ry);
			pole = {};
			std::printf("  a pole 25 yalms off (foot at %.0f, %.0f), sun low to the right: 10-13 yalms left of it (x %.0f..%.0f) %.1f lit, %.1f with shadows; as far right %.1f, %.1f\n",
				f.first, f.second, l13.first, l10.first, left_lit, left_shadowed, right_lit, right_shadowed);
			check(l13.first > 0.0f && r13.first < float(W), "the pole's ground in the picture");
			// (the sun 19 degrees up takes 64% of the strength from flat ground: the sun's share of its light)
			check(left_shadowed < left_lit * 0.97, "a tall thing's shadow reaches past 9 yalms (the 16-step march: not at all)");
			check(std::fabs(right_shadowed - right_lit) < 1.0, "not on the sun's side of it");
		}

		// The box as someone standing there (a character or monster sent by the Ashita addon): it casts no sun
		// shadow, the game drawing characters' own; with characters asked to cast, it does
		{
			to_sun = normalize({ (eye.x - at.x) / 5.385f, (eye.y - at.y) / 5.385f - 1.0f, (eye.z - at.z) / 5.385f });
			const float body[4] = { q.x, at.y, q.z, 2.0f };
			actors = body;
			actor_count = 1;
			characters_cast = false;
			const double skipped = mean(frame(true, 0.6f), LONG(px) - 40, LONG(py) - 10, LONG(px) + 40, LONG(py) + 4);
			characters_cast = true;
			const double cast = mean(frame(true, 0.6f), LONG(px) - 40, LONG(py) - 10, LONG(px) + 40, LONG(py) + 4);
			actors = nullptr;
			actor_count = 0;
			characters_cast = false;
			std::printf("  the box as a character: ground in front of it %.1f lit, %.1f with shadows, %.1f with characters casting\n", near_lit, skipped, cast);
			check(std::fabs(skipped - near_lit) < 1.5, "a character casts no sun shadow (the game draws its own)");
			check(cast < near_lit * 0.9, "unless characters are asked to cast");
		}

		// A character's own shadow (characters asked to cast) is worked out from them as they stand, not by the march
		// (the march found them only where a step happened to land inside them: whole at the feet, dotted and lighter
		// toward the head, and nothing of someone off screen).
		{
			const vec3 right = { cam.view[0], cam.view[4], cam.view[8] }; // (level: the camera doesn't roll)
			const vec3 right_h = normalize({ right.x, 0.0f, right.z }), ahead_h = normalize({ -cam.view[2], 0.0f, -cam.view[10] });
			const vec3 up_c = { cam.view[1], cam.view[5], cam.view[9] }, fwd = { -cam.view[2], -cam.view[6], -cam.view[10] };
			// (a point's pixel, and its view depth in z)
			auto project = [&](const vec3 &p) {
				const float x = p.x * vp[0] + p.y * vp[4] + p.z * vp[8] + vp[12], y = p.x * vp[1] + p.y * vp[5] + p.z * vp[9] + vp[13];
				const float w = p.x * vp[3] + p.y * vp[7] + p.z * vp[11] + vp[15];
				return vec3 { (x / w + 1.0f) * 0.5f * W, (1.0f - y / w) * 0.5f * H, w };
			};
			// (where the view ray through a pixel's middle meets the ground)
			auto ground_under = [&](float x, float y) {
				const float nx = (x + 0.5f) / W * 2.0f - 1.0f, ny = 1.0f - (y + 0.5f) / H * 2.0f;
				const float a = nx / cam.proj[0], b = ny / cam.proj[5];
				const vec3 d = { right.x * a + up_c.x * b + fwd.x, right.y * a + up_c.y * b + fwd.y, right.z * a + up_c.z * b + fwd.z };
				const float t = (at.y - eye.y) / d.y;
				return vec3 { eye.x + d.x * t, eye.y + d.y * t, eye.z + d.z * t };
			};
			auto depth_for = [](float w) { return 1.0f - 0.1f / w; }; // (the game's projection: depth from view depth)
			auto save = [&](const std::vector<uint8_t> &img, const char *path) { // (a picture for a look)
				if (FILE *f = nullptr; fopen_s(&f, path, "wb") == 0 && f != nullptr)
				{
					std::fprintf(f, "P6 %u %u 255\n", W, H);
					for (size_t i = 0; i < size_t(W) * H; ++i)
						std::fwrite(&img[i * 4], 1, 3, f);
					fclose(f);
				}
			};
			auto luma_at = [&](const std::vector<uint8_t> &img, LONG x, LONG y) {
				const uint8_t *c = &img[(size_t(y) * W + x) * 4];
				return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
			};
			// Dark texels standing alone in a rect: darker by over 3 (of 255) than both their neighbours across or both
			// up and down (a dot, or a line a pixel wide: the march's stipple and hatching; not a soft edge's steps)
			auto lone_dark = [&](const std::vector<uint8_t> &img, LONG x0, LONG y0, LONG x1, LONG y1) {
				uint32_t n = 0;
				for (LONG y = std::max(1L, y0); y < std::min(LONG(H) - 1, y1); ++y)
					for (LONG x = std::max(1L, x0); x < std::min(LONG(W) - 1, x1); ++x)
					{
						const double c = luma_at(img, x, y) + 3.0;
						const bool across = c < luma_at(img, x - 1, y) && c < luma_at(img, x + 1, y);
						const bool down = c < luma_at(img, x, y - 1) && c < luma_at(img, x, y + 1);
						n += across || down ? 1 : 0;
					}
				return n;
			};

			// Someone (size 2) ten yalms left of the looked-at point, off the screen's edge with all of their capsule, the
			// sun low (20 degrees up) from the left: their shadow runs along the ground across the left of the picture, as
			// their slim capsule (nothing of them on screen to take their shape from). It's as dark under their head as
			// under their knees, and its edge fades in row by row, with no dark texel standing alone in it.
			{
				const float size = 2.0f, e = 20.0f * 3.14159265f / 180.0f;
				const vec3 feet = { at.x - right_h.x * 10.0f, at.y, at.z - right_h.z * 10.0f };
				const float body[4] = { feet.x, feet.y, feet.z, size };
				to_sun = { -right_h.x * std::cos(e), -std::sin(e), -right_h.z * std::cos(e) };
				actors = body;
				actor_count = 1;
				const std::vector<uint8_t> no_cast = frame(false, 0.6f);
				characters_cast = true;
				const std::vector<uint8_t> cast = frame(false, 0.6f);
				actors = nullptr;
				actor_count = 0;
				characters_cast = false;
				// (the ground under a height of them, along the shadow's middle)
				auto under = [&](float h) {
					const float d = h / std::tan(e);
					return project({ feet.x + right_h.x * d, at.y, feet.z + right_h.z * d });
				};
				const vec3 knees = under(1.4f), head = under(3.0f), off_edge = project({ feet.x + right_h.x * 1.6f, at.y, feet.z + right_h.z * 1.6f });
				auto spot = [&](const std::vector<uint8_t> &img, const vec3 &s) { return mean(img, LONG(s.x) - 3, LONG(s.y) - 1, LONG(s.x) + 4, LONG(s.y) + 2); };
				const double knees_lit = spot(no_cast, knees), knees_cast = spot(cast, knees), head_lit = spot(no_cast, head), head_cast = spot(cast, head);
				// (down a column through the knees' shadow, from the lit ground 14 rows above its middle to near the
				// middle: each row as dark as the one above it or darker, but for a level of rounding)
				bool fades = true;
				for (LONG y = LONG(knees.y) - 14; y < LONG(knees.y) - 2; ++y)
					fades = fades && luma_at(cast, LONG(knees.x), y + 1) <= luma_at(cast, LONG(knees.x), y) + 1.0;
				const uint32_t alone = lone_dark(cast, 0, LONG(knees.y) - 24, LONG(head.x) + 40, LONG(knees.y) + 24);
				save(cast, "build\\actor_shadow_test.ppm");
				std::printf("  someone off screen to the left (their capsule's edge at x %.0f), sun low from the left: under their knees (%.0f, %.0f) %.1f lit, %.1f casting; under their head (%.0f, %.0f) %.1f, %.1f; lone dark texels %u\n",
					off_edge.x, knees.x, knees.y, knees_lit, knees_cast, head.x, head.y, head_lit, head_cast, alone);
				check(off_edge.x < 0.0f && knees.x > 8.0f && head.x < float(W) - 8.0f, "their capsule off screen, their shadow on it");
				check(knees_cast < knees_lit * 0.8, "someone off screen casts (the slim capsule)");
				check(std::fabs(head_cast - knees_cast) < 0.05 * knees_lit, "as dark under their head as under their knees");
				check(fades, "its edge fades in row by row");
				check(alone == 0, "no dark texel standing alone in it");
			}

			// Someone on screen: their shadow takes their shape from the depth. A shape drawn there as someone standing
			// legs apart (size 2: two legs from 0.6 to 2 yalms up, half a yalm wide and 0.4 apart, the body over them to
			// 3.2; all at the depth of their middle, 1.2 up), the sun beyond them, 30 degrees up: on the ground toward the
			// camera, where the ray toward the sun passes through a leg (by the shape: its stretch just behind the shape,
			// as deep as a body), it's dark, as dark under the hips as under the knees; where it passes through the gap,
			// it's lit (the slim capsule alone would cover the gap); no dark texel standing alone. With a wall drawn
			// behind them, 2 yalms further than the shape (seen through the gap and beside the legs), the same: nothing
			// but them casts their shadow.
			{
				const float size = 2.0f, e = 30.0f * 3.14159265f / 180.0f;
				const vec3 feet = at, middle = { at.x, at.y - 1.2f, at.z };
				const float w_shape = project(middle).z, thick = 0.35f * size + 0.15f; // (the shader's kThick)
				const float body[4] = { feet.x, feet.y, feet.z, size };
				to_sun = { ahead_h.x * std::cos(e), -std::sin(e), ahead_h.z * std::cos(e) };
				auto column = [&](float lateral) { return project({ middle.x + right.x * lateral, middle.y, middle.z + right.z * lateral }).x; };
				auto row = [&](float h) { return project({ feet.x, feet.y - h, feet.z }).y; };
				const float xl0 = column(-0.7f), xl1 = column(-0.2f), xr0 = column(0.2f), xr1 = column(0.7f);
				const float y_top = row(3.2f), y_hip = row(2.0f), y_foot = row(0.6f), y_ground = row(0.0f), y_knee = 0.5f * (y_hip + y_foot);
				const std::vector<card> shape = {
					{ { LONG(xl0), LONG(y_hip), LONG(xl1), LONG(y_foot) }, depth_for(w_shape) },
					{ { LONG(xr0), LONG(y_hip), LONG(xr1), LONG(y_foot) }, depth_for(w_shape) },
					{ { LONG(xl0), LONG(y_top), LONG(xr1), LONG(y_hip) }, depth_for(w_shape) } };
				const card behind = { { LONG(column(-1.4f)), LONG(row(2.2f)), LONG(column(1.4f)), LONG(y_ground) }, depth_for(w_shape + 2.0f) };
				actors = body;
				actor_count = 1;
				cards = shape;
				const std::vector<uint8_t> sunless = frame(false, 0.0f);
				characters_cast = true;
				const std::vector<uint8_t> shaped = frame(false, 0.6f);
				cards.insert(cards.begin(), behind);
				const std::vector<uint8_t> walled = frame(false, 0.6f);
				cards.clear();
				actors = nullptr;
				actor_count = 0;
				characters_cast = false;
				save(shaped, "build\\silhouette_test.ppm");
				save(walled, "build\\silhouette_wall_test.ppm");
				// Where each ground pixel's ray toward the sun is in its stretch just behind the shape (from 5 cm in to
				// 5 cm short of its far end), all of it: 1 a leg's lower half, 3 its upper half, 2 the gap, 0 elsewhere
				// or across an edge (5 pixels in from the edges: the shading pass's average spreads an edge 3 or 4)
				const vec3 depth_rate = { vp[3], vp[7], vp[11] };
				auto through = [&](LONG x, LONG y) {
					const vec3 G = ground_under(float(x), float(y));
					const float rate = dot(to_sun, depth_rate), wg = project(G).z;
					vec3 ends[2];
					for (int k = 0; k < 2; ++k)
					{
						const float t = (w_shape + (k == 0 ? 0.05f : thick - 0.05f) - wg) / rate;
						ends[k] = project({ G.x + to_sun.x * t, G.y + to_sun.y * t, G.z + to_sun.z * t });
					}
					auto within = [&](float lo_x, float hi_x, float lo_y, float hi_y) {
						const float m = 5.0f;
						return ends[0].x > lo_x + m && ends[0].x < hi_x - m && ends[1].x > lo_x + m && ends[1].x < hi_x - m &&
							ends[0].y > lo_y + m && ends[0].y < hi_y - m && ends[1].y > lo_y + m && ends[1].y < hi_y - m;
					};
					const bool lower = 0.5f * (ends[0].y + ends[1].y) > y_knee;
					return within(xl0, xl1, y_hip, y_foot) || within(xr0, xr1, y_hip, y_foot) ? (lower ? 1 : 3) : within(xl1, xr0, y_hip, y_foot) ? 2 : 0;
				};
				double sum[4][3] = {};
				uint32_t n[4] = {};
				for (LONG y = LONG(y_ground) + 2; y < LONG(H) - 1; ++y)
					for (LONG x = LONG(xl0) - 12; x < LONG(xr1) + 12; ++x)
					{
						const int c = through(x, y);
						sum[c][0] += luma_at(sunless, x, y);
						sum[c][1] += luma_at(shaped, x, y);
						sum[c][2] += luma_at(walled, x, y);
						n[c]++;
					}
				auto avg = [&](int c, int k) { return n[c] != 0 ? sum[c][k] / n[c] : 0.0; };
				const uint32_t alone = lone_dark(shaped, LONG(xl0) - 12, LONG(y_ground) + 2, LONG(xr1) + 12, LONG(H) - 1);
				std::printf("  someone legs apart (gap at x %.0f..%.0f), sun beyond them: ground in the legs' shadow under the knees (%u px) %.1f lit, %.1f casting, %.1f with a wall behind; under the hips (%u px) %.1f, %.1f, %.1f; the gap's (%u px) %.1f, %.1f, %.1f; lone dark texels %u\n",
					xl1, xr0, n[1], avg(1, 0), avg(1, 1), avg(1, 2), n[3], avg(3, 0), avg(3, 1), avg(3, 2), n[2], avg(2, 0), avg(2, 1), avg(2, 2), alone);
				check(n[1] > 30 && n[2] > 20 && n[3] > 30, "the legs' and the gap's shadows on the ground in the picture");
				check(avg(1, 1) < avg(1, 0) * 0.75, "the legs cast their shadow");
				check(std::fabs(avg(3, 1) - avg(1, 1)) < 0.05 * avg(1, 0), "as dark under the hips as under the knees");
				check(avg(2, 1) > avg(2, 0) * 0.95, "the gap between them is lit: the shadow has their shape, not a capsule's");
				check(alone == 0, "no dark texel standing alone in it");
				check(std::fabs(avg(1, 2) - avg(1, 1)) < 1.5 && std::fabs(avg(2, 2) - avg(2, 1)) < 1.5 && std::fabs(avg(3, 2) - avg(3, 1)) < 1.5,
					"with a wall behind them, the same: the wall doesn't cast");
			}

			// Something thin of theirs (an arm, a tail, a staff: an upright 0.12 yalms wide, 0.6 to 3 up, at the depth of
			// their middle), the sun from the right, 30 degrees up and turned a little away from the camera: the ray from
			// the ground crosses it sideways. Along the middle of its shadow (under the upright's middle at each height,
			// half a body's depth behind it), as dark everywhere, not dashed (the march's steps stepped over it for some
			// start phases)
			{
				const float size = 2.0f, e = 30.0f * 3.14159265f / 180.0f, turn = 25.0f * 3.14159265f / 180.0f;
				const vec3 feet = at, middle = { at.x, at.y - 1.2f, at.z };
				const float w_shape = project(middle).z, thick = 0.35f * size + 0.15f;
				const float body[4] = { feet.x, feet.y, feet.z, size };
				const vec3 across = normalize({ right_h.x * std::cos(turn) + ahead_h.x * std::sin(turn), 0.0f, right_h.z * std::cos(turn) + ahead_h.z * std::sin(turn) });
				to_sun = { across.x * std::cos(e), -std::sin(e), across.z * std::cos(e) };
				auto column = [&](float lateral) { return project({ middle.x + right.x * lateral, middle.y, middle.z + right.z * lateral }).x; };
				auto row = [&](float h) { return project({ feet.x, feet.y - h, feet.z }).y; };
				cards = { { { LONG(column(-0.06f)), LONG(row(3.0f)), LONG(column(0.06f)), LONG(row(0.6f)) }, depth_for(w_shape) } };
				actors = body;
				actor_count = 1;
				const std::vector<uint8_t> sunless = frame(false, 0.0f);
				characters_cast = true;
				const std::vector<uint8_t> thin = frame(false, 0.6f);
				cards.clear();
				actors = nullptr;
				actor_count = 0;
				characters_cast = false;
				save(thin, "build\\thin_part_test.ppm");
				// (the point half a body's depth behind the upright's middle at a height, and where its shadow falls)
				auto shadow_of = [&](float h) {
					const float nx = (column(0.0f) + 0.5f) / W * 2.0f - 1.0f, ny = 1.0f - (row(h) + 0.5f) / H * 2.0f;
					const float a = nx / cam.proj[0], b = ny / cam.proj[5], w = w_shape + 0.5f * thick;
					const vec3 X = { eye.x + (right.x * a + up_c.x * b + fwd.x) * w, eye.y + (right.y * a + up_c.y * b + fwd.y) * w, eye.z + (right.z * a + up_c.z * b + fwd.z) * w };
					const float t = (at.y - X.y) / -to_sun.y;
					return project({ X.x - to_sun.x * t, at.y, X.z - to_sun.z * t });
				};
				double darkest = 1e9, lightest = 0.0, lit_ground = 0.0;
				std::string along;
				for (int k = 0; k < 10; ++k)
				{
					const vec3 s = shadow_of(0.9f + 0.2f * k);
					const double v = mean(thin, LONG(s.x) - 1, LONG(s.y) - 1, LONG(s.x) + 2, LONG(s.y) + 2);
					darkest = std::min(darkest, v);
					lightest = std::max(lightest, v);
					lit_ground = std::max(lit_ground, mean(sunless, LONG(s.x) - 1, LONG(s.y) - 1, LONG(s.x) + 2, LONG(s.y) + 2));
					char part[32];
					std::snprintf(part, sizeof(part), " %.0f", v);
					along += part;
				}
				const vec3 first = shadow_of(0.9f), last = shadow_of(2.7f);
				std::printf("  something thin (0.12 yalms), sun from the right: along its shadow from (%.0f, %.0f) to (%.0f, %.0f):%s (lit %.1f)\n",
					first.x, first.y, last.x, last.y, along.c_str(), lit_ground);
				check(first.x > 2.0f && last.x > 2.0f && first.y < float(H) - 2.0f && last.y < float(H) - 2.0f, "its shadow in the picture");
				check(lightest < lit_ground * 0.75, "a thin part's shadow is dark all along");
				check(lightest - darkest < 0.06 * lit_ground, "and evenly: not dashed");
			}
		}

		// A name facelift drew into the world, in the box's shadow (its solid pixels marked with alpha 1 by
		// core/namedepth): left as drawn; the ground beside it shadowed as before
		{
			to_sun = normalize({ (eye.x - at.x) / 5.385f, (eye.y - at.y) / 5.385f - 1.0f, (eye.z - at.z) / 5.385f });
			name = { LONG(px) - 40, LONG(py) - 10, LONG(px), LONG(py) + 4 };
			const std::vector<uint8_t> named = frame(true, 0.6f);
			name = {};
			const double on_name = mean(named, LONG(px) - 36, LONG(py) - 8, LONG(px) - 4, LONG(py) + 2);
			const double beside = mean(named, LONG(px) + 4, LONG(py) - 8, LONG(px) + 36, LONG(py) + 2);
			std::printf("  a name in the box's shadow: %.1f (lit ground %.1f), the ground beside it %.1f\n", on_name, near_lit, beside);
			check(std::fabs(on_name - near_lit) < 1.5, "a name facelift drew into the world isn't shaded");
			check(beside < near_lit * 0.9, "the ground beside it still is");
		}

		// Someone in partial shade: a narrow thing a third of a yalm nearer the camera, just over the box's top
		// left, the sun toward the camera and up. As the world, the box's top left is in its shadow and its top
		// right isn't; as a character (sent as one), all of it takes the one shadow worked out from its chest:
		// the two sides alike (the facing built from the depth is a facet's on the game's models, and split
		// every limb in two)
		{
			to_sun = normalize({ (eye.x - at.x) / 5.385f, (eye.y - at.y) / 5.385f - 1.0f, (eye.z - at.z) / 5.385f });
			const float k = 0.3f / 4.85f; // (a third of a yalm toward the camera from the box)
			const vec3 nearer = { q.x + (eye.x - q.x) * k, q.y + (eye.y - q.y) * k, q.z + (eye.z - q.z) * k };
			pole = { LONG(px) - 60, LONG(py) - 130, LONG(px) - 20, LONG(py) - 92 };
			pole_depth = (nearer.x * vp[2] + nearer.y * vp[6] + nearer.z * vp[10] + vp[14]) / (nearer.x * vp[3] + nearer.y * vp[7] + nearer.z * vp[11] + vp[15]);
			const std::vector<uint8_t> as_world = frame(true, 0.6f);
			const float body[4] = { q.x, at.y, q.z, 2.0f };
			actors = body;
			actor_count = 1;
			const std::vector<uint8_t> as_someone = frame(true, 0.6f);
			actors = nullptr;
			actor_count = 0;
			pole = {};
			const double world_left = mean(as_world, LONG(px) - 56, LONG(py) - 88, LONG(px) - 24, LONG(py) - 70);
			const double world_right = mean(as_world, LONG(px) + 24, LONG(py) - 88, LONG(px) + 56, LONG(py) - 70);
			const double someone_left = mean(as_someone, LONG(px) - 56, LONG(py) - 88, LONG(px) - 24, LONG(py) - 70);
			const double someone_right = mean(as_someone, LONG(px) + 24, LONG(py) - 88, LONG(px) + 56, LONG(py) - 70);
			std::printf("  someone in partial shade: as the world, top left %.1f, top right %.1f; as a character %.1f, %.1f\n",
				world_left, world_right, someone_left, someone_right);
			check(world_left < world_right * 0.95, "the narrow thing shades the box's top left (as the world)");
			check(std::fabs(someone_left - someone_right) < 1.0, "a character takes one shadow all over, not each facet its own");
		}

		// The weather (as the worldfx Ashita addon sends it, decoded as the add-on does): how much of the sun's shadows it
		// leaves is the game's own sun light in its record (the sun colour's luminance times the zone's light intensity)
		// against the zone's clear sky's at that hour (a clear-sky file as tools/bake_clear_sky.py writes it, read back).
		// A record as bright as the clear sky leaves the box's shadow as it is; one at 0.89 of it (the game's cloud, at
		// the median over all zones) leaves 89% of it; 0.59 (rain) 59%, the sun pass still running; a change halfway
		// between sunshine and rain (blended in the game's record already) about 80%; brighter than the clear sky (snow)
		// all of it. No record (the game's stand-in, the first frames in a zone), or a clear sky too faint to compare
		// with (the darkest hours): as without weather. The game's fog (here in the ground's own colour, as its fog
		// would have left the ground, ending 8 yalms off: the box's ground, 5 off, 39% clear) takes about that share of
		// the shadow's darkening away: what's darkened is the scene under the fog, not the fog.
		{
			to_sun = normalize({ (eye.x - at.x) / 5.385f, (eye.y - at.y) / 5.385f - 1.0f, (eye.z - at.z) / 5.385f });
			// (a record: the sun colour 200 190 170 at this light intensity; the zone's light read)
			auto words_for = [](uint32_t name, uint32_t before, float change, float intensity) {
				std::vector<uint32_t> w(kWeatherWords, 0u);
				w[0] = 2;
				w[1] = name;
				w[2] = before;
				std::memcpy(&w[3], &change, 4);
				w[7] = intensity > 0.0f ? 1 | 4 | 8 : 4 | 8; // (the record, the weather object and the id read; no fog)
				uint32_t *r = &w[8];
				r[0x2C / 4] = 200u | 190u << 8 | 170u << 16 | 0x80u << 24;
				const float fog[2] = { 450.0f, 0.0f }, distance = 1000.0f;
				std::memcpy(&r[0x3C / 4], fog, 8);
				std::memcpy(&r[0x44 / 4], &intensity, 4);
				std::memcpy(&r[0x58 / 4], &distance, 4);
				return w;
			};
			// (the zone's clear sky: its sun light at 1.5 all day but for the dark hours, 0.03 from 0:00 to 2:00)
			const float clear_light = (0.3f * 200.0f + 0.59f * 190.0f + 0.11f * 170.0f) / 255.0f * 1.5f;
			std::vector<clear_sky> skies;
			std::string why;
			if (FILE *f = nullptr; fopen_s(&f, "build\\clearsky_test.txt", "wb") == 0 && f != nullptr)
			{
				std::fprintf(f, "# a test\n7");
				for (int h = 0; h < 24; ++h)
					std::fprintf(f, " %.6f", h <= 2 ? 0.03 : h == 13 ? 2.0 * clear_light : double(clear_light));
				std::fprintf(f, "\n");
				fclose(f);
			}
			const bool read_sky = read_clear_sky_file(L"build\\clearsky_test.txt", skies, why) && skies.size() == 8 && skies[7].known && !skies[0].known;
			const float at_noon = read_sky ? clear_sky_light(skies[7], 12.0) : 0.0f, at_half = read_sky ? clear_sky_light(skies[7], 12.5) : 0.0f;
			const float at_night = read_sky ? clear_sky_light(skies[7], 1.0) : 0.0f, round_midnight = read_sky ? clear_sky_light(skies[7], 23.5) : 0.0f;
			check(read_sky && std::fabs(at_noon - clear_light) < 1e-4f && std::fabs(at_half - 1.5f * clear_light) < 1e-4f && std::fabs(at_night - 0.03f) < 1e-6f &&
				std::fabs(round_midnight - 0.5f * (clear_light + 0.03f)) < 1e-4f, "the clear-sky file read back, its light blended between the hours and round midnight");
			auto share_of = [&](const std::vector<uint32_t> &w, weather &out, float sky) {
				decode_weather(w.data(), uint32_t(w.size()), out);
				return weather_sunlight(out, sky);
			};
			weather sunny, cloudy, rainy, changing, snowy, stand_in, dark;
			const float s_sunny = share_of(words_for(weather_name("suny"), 0, 1.0f, 1.5f), sunny, at_noon);
			const float s_cloudy = share_of(words_for(weather_name("clod"), 0, 1.0f, 1.5f * 0.89f), cloudy, at_noon);
			const float s_rainy = share_of(words_for(weather_name("rai1"), 0, 1.0f, 1.5f * 0.59f), rainy, at_noon);
			const float s_changing = share_of(words_for(weather_name("rain"), weather_name("suny"), 0.5f, 1.5f * 0.795f), changing, at_noon);
			const float s_snowy = share_of(words_for(weather_name("snow"), 0, 1.0f, 1.5f * 1.19f), snowy, at_noon);
			const float s_stand_in = share_of(words_for(weather_name("rain"), 0, 1.0f, 0.0f), stand_in, at_noon);
			const float s_dark = share_of(words_for(weather_name("rain"), 0, 1.0f, 1.5f * 0.59f), dark, at_night);
			check(sunny.known && sunny.record && sunny.name == weather_name("suny") && sunny.id == 2 && changing.before == weather_name("suny") &&
				std::fabs(changing.change - 0.5f) < 1e-6f && std::fabs(weather_light(sunny) - clear_light) < 1e-4f && !stand_in.record,
				"the weather decoded: the one shown, its id, the one it's changing from and how far, its record's sun light");
			auto darkening = [&](float share) {
				weather_share = share;
				const double shadowed = mean(frame(true, 0.6f), LONG(px) - 40, LONG(py) - 10, LONG(px) + 40, LONG(py) + 4);
				weather_share = 1.0f;
				return near_lit - shadowed;
			};
			const double d_clear = near_lit - near_shadowed, d_sunny = darkening(s_sunny), d_cloudy = darkening(s_cloudy), d_rainy = darkening(s_rainy),
				d_changing = darkening(s_changing);
			haze_end = 8.0f;
			const double d_fog = darkening(1.0f);
			haze_end = 0.0f;
			std::printf("  the weather's share of the sun's shadows, the game's sun light against the clear sky's: sunshine %.2f, cloud %.2f, rain %.2f, "
				"sunshine to rain halfway %.2f, snow %.2f, no record yet %.2f, rain in the darkest hours %.2f; the box's shadow darkens by %.1f with no weather, "
				"%.1f in sunshine, %.1f under cloud, %.1f in rain, %.1f halfway; %.1f in fog in the ground's colour (8 yalms)\n",
				s_sunny, s_cloudy, s_rainy, s_changing, s_snowy, s_stand_in, s_dark, d_clear, d_sunny, d_cloudy, d_rainy, d_changing, d_fog);
			check(std::fabs(s_sunny - 1.0f) < 1e-5f && std::fabs(d_sunny - d_clear) < 0.5, "a record as bright as the clear sky leaves the sun's shadows as they are");
			check(std::fabs(s_cloudy - 0.89f) < 1e-4f && d_cloudy > 0.82 * d_clear && d_cloudy < 0.95 * d_clear, "the game's cloud (0.89 of its clear sun) leaves 89% of them");
			check(std::fabs(s_rainy - 0.59f) < 1e-4f && d_rainy > 0.5 * d_clear && d_rainy < 0.68 * d_clear, "its rain (0.59) leaves 59% (the sun pass still runs)");
			check(std::fabs(s_changing - 0.795f) < 1e-4f && d_changing > 0.7 * d_clear && d_changing < 0.88 * d_clear, "a change halfway, blended in the record: about 80%");
			check(s_snowy == 1.0f && s_stand_in == 1.0f && s_dark == 1.0f, "brighter than the clear sky, no record yet, or the darkest hours: as without weather");
			check(d_fog > 0.25 * d_clear && d_fog < 0.55 * d_clear, "in fog a shadow's darkening fades with the scene under the fog");
		}
		check(g.report_messages() == 0, "no debug-layer errors (sun shadows)");
	}

	// 1o. The sun's shadow map, from a zone's meshes (here a stand-in made on the spot, as
	// tools/bake_shadow_geo.py lays them out): a ground sheet, a slab 6 yalms wide and 120 tall standing 12 yalms
	// beyond the looked-at point on the camera's side (7 behind the camera: off screen, so the march never sees
	// it), and a 8 x 8 card 30 yalms over the looked-at point (above the top of the screen), cut out to its left
	// half (its texture's alpha: x < 0.5 opaque). None of them is in the world's depth buffer.
	// - The sun behind the slab, 59 degrees up: the looked-at ground is in the slab's shadow with the map, lit
	//   without it; so is ground 55 yalms further out along the shadow (the far cascade), and ground beside the
	//   shadow isn't.
	// - The sun straight overhead: the card's shadow falls under its opaque half only.
	// - The sun in front of the camera, 59 and 15 degrees up: nothing casts on the ground in sight, and flat ground
	//   comes out as it went in (it is in the map itself: no shadow from a surface onto itself).
	// - The box of 1h (in the depth buffer, not in the map) still shades the ground just in front of it: the
	//   march goes on for contact shadows.
	// - The map is drawn again whenever its picture changes: not for the same sun and camera; with the sun moving,
	//   the near cascade every frame and the far one every other frame; the camera moving, the near one at once.
	//   With the sun turning a tenth of a degree a frame, the slab's shadow edge moves a little each frame.
	{
		const float plain[4] = { 0.45f, 0.42f, 0.36f, 0.5f }, dark_box[4] = { 0.2f, 0.2f, 0.2f, 0.5f };
		const vec3 dh = normalize({ eye.x - at.x, 0.0f, eye.z - at.z }), side = { -dh.z, 0.0f, dh.x };
		auto along = [&](float d, float s, float up) { return vec3 { at.x + dh.x * d + side.x * s, at.y - up, at.z + dh.z * d + side.z * s }; };
		shadow_file zone;
		zone.zone = 999;
		auto corner = [&](const vec3 &v, uint32_t w) {
			uint32_t bits[3];
			std::memcpy(bits, &v, 12);
			zone.triangles.insert(zone.triangles.end(), { bits[0], bits[1], bits[2], w });
		};
		auto quad = [&](const vec3 &a, const vec3 &b, const vec3 &c, const vec3 &d, bool card) {
			// (a b c d around; on the card, u runs a -> b and v a -> d, mask 1)
			const uint32_t wa = card ? (1u << 24) : 0, wb = card ? (4095u | (1u << 24)) : 0, wc = card ? (4095u | (4095u << 12) | (1u << 24)) : 0, wd = card ? ((4095u << 12) | (1u << 24)) : 0;
			corner(a, wa), corner(b, wb), corner(c, wc);
			corner(a, wa), corner(c, wc), corner(d, wd);
		};
		// (in cells as the bake makes them: first a square 3,000 yalms off, out of both cascades' reach, so every
		// draw of the map starts past the buffer's start)
		quad(along(3000, 3000, 0), along(3001, 3000, 0), along(3001, 3001, 0), along(3000, 3001, 0), false);
		zone.cells.push_back({ 90, 90, 0, zone.triangle_count() });
		quad(along(-100, -100, 0), along(100, -100, 0), along(100, 100, 0), along(-100, 100, 0), false); // the ground
		zone.cells.push_back({ 0, 0, 2, 2 });
		quad(along(12, -3, 0), along(12, 3, 0), along(12, 3, 120), along(12, -3, 120), false);         // the slab
		zone.cells.push_back({ 1, 0, 4, 2 });
		const vec3 c0 = { at.x - 4, at.y - 30, at.z - 4 }, c1 = { at.x + 4, at.y - 30, at.z - 4 }, c2 = { at.x + 4, at.y - 30, at.z + 4 }, c3 = { at.x - 4, at.y - 30, at.z + 4 };
		quad(c0, c1, c2, c3, true); // the card: u along x
		zone.cells.push_back({ 2, 0, 6, 2 });
		zone.masks = { 64, 64, 0, 2 };
		for (int row = 0; row < 64; ++row)
			zone.words.insert(zone.words.end(), { 0xFFFFFFFFu, 0u }); // (texels 0..31 of each row opaque)
		check(r.upload_shadow(zone, error) && r.shadow_loaded() && r.shadow_triangles() == 8, "a zone's meshes uploaded for the shadow map");

		bool with_box = false;
		bool characters = false; // (the characters' sun shadows asked for: rule bit 4)
		float bumps = 0.0f; // (the ground's depth made rough: bumps this high)
		float air = 0.0f; // (the light in the air: its density a yalm, 0 none)
		float air_light = 1.0f; // (its shafts' strength)
		bool air_dark = false; // (its own colour black: only the sun's light added)
		vec3 to_sun = {};
		draw_stats last = {};
		auto frame = [&](bool map, const camera &c) {
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(c, bumps);
			if (with_box)
			{
				const vec3 q = { at.x + (eye.x - at.x) * 0.1f, at.y + (eye.y - at.y) * 0.1f, at.z + (eye.z - at.z) * 0.1f };
				const float qz = q.x * vp[2] + q.y * vp[6] + q.z * vp[10] + vp[14], qw = q.x * vp[3] + q.y * vp[7] + q.z * vp[11] + vp[15];
				const D3D12_RECT box = { LONG(px) - 60, LONG(py) - 90, LONG(px) + 60, LONG(py) - 12 };
				g.list->ClearRenderTargetView(world_rtv, dark_box, 1, &box);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, qz / qw, 0, 1, &box);
			}
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.grass = false;
			l2.ao_strength = 0.0f;
			l2.relief = 0.0f;
			l2.water = false;
			l2.sun[0] = to_sun.x;
			l2.sun[1] = to_sun.y;
			l2.sun[2] = to_sun.z;
			l2.sun_shadow = 0.6f;
			l2.sun_map = map;
			l2.sun_characters = characters;
			l2.vol_density = air;
			l2.vol_light = air > 0.0f ? air_light : 0.0f;
			if (air_dark)
				l2.vol_air[0] = l2.vol_air[1] = l2.vol_air[2] = 0.0f;
			l2.vol_steps = 24;
			last = r.draw(g.list, t, c, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			return g.read(world, 4);
		};
		auto on_screen = [&](const vec3 &v) {
			const float x = v.x * vp[0] + v.y * vp[4] + v.z * vp[8] + vp[12], y = v.x * vp[1] + v.y * vp[5] + v.z * vp[9] + vp[13];
			const float w = v.x * vp[3] + v.y * vp[7] + v.z * vp[11] + vp[15];
			return std::make_pair((x / w + 1.0f) * 0.5f * W, (1.0f - y / w) * 0.5f * H);
		};
		// (the mean brightness in a small window around where a ground point is on screen; -1 off screen)
		auto at_point = [&](const std::vector<uint8_t> &img, const vec3 &v, int rx, int ry) {
			const auto sp = on_screen(v);
			if (sp.first < rx || sp.second < ry || sp.first >= W - rx || sp.second >= H - ry)
				return -1.0;
			double sum = 0.0;
			int n = 0;
			for (int y = int(sp.second) - ry; y <= int(sp.second) + ry; ++y)
				for (int x = int(sp.first) - rx; x <= int(sp.first) + rx; ++x)
				{
					const uint8_t *c = &img[(size_t(y) * W + x) * 4];
					sum += 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
					n++;
				}
			return sum / n;
		};
		const double plain_luma = 0.3 * 0.45 * 255 + 0.59 * 0.42 * 255 + 0.11 * 0.36 * 255;
		auto sun_from = [](const vec3 &horizontal, float degrees) {
			const float e = degrees * 3.14159265f / 180.0f;
			return normalize({ horizontal.x * std::cos(e), -std::sin(e), horizontal.z * std::cos(e) });
		};

		// The sun behind the slab
		to_sun = sun_from(dh, 59.0f);
		const std::vector<uint8_t> behind_off = frame(false, cam), behind_on = frame(true, cam);
		const draw_stats first = last;
		const vec3 far_point = along(-55, 0, 0), far_side = along(-55, 9, 0), near_side = along(0, 5.5f, 0);
		const double here_off = at_point(behind_off, at, 8, 4), here_on = at_point(behind_on, at, 8, 4);
		const double far_off = at_point(behind_off, far_point, 3, 1), far_on = at_point(behind_on, far_point, 3, 1);
		const double side_on = at_point(behind_on, near_side, 4, 4), far_side_on = at_point(behind_on, far_side, 3, 1);
		// (a picture of it for a look: build\shadow_map_test.ppm)
		if (FILE *f = nullptr; fopen_s(&f, "build\\shadow_map_test.ppm", "wb") == 0 && f != nullptr)
		{
			std::fprintf(f, "P6 %u %u 255\n", W, H);
			for (size_t i = 0; i < size_t(W) * H; ++i)
				std::fwrite(&behind_on[i * 4], 1, 3, f);
			std::fclose(f);
		}
		// (along the shadow, out to its tip 60 yalms off: each row of the ground there a yalm or more on from the last)
		std::printf("  along the slab's shadow with the map, yalms out: brightness");
		for (float d : { -20.0f, -30.0f, -40.0f, -45.0f, -50.0f, -55.0f, -65.0f })
			std::printf(" %.0f: %.1f", -d, at_point(behind_on, along(d, 0, 0), 3, 1));
		std::printf("\n");
		std::printf("  shadow map: the sun behind a slab off screen: looked-at ground %.1f without the map, %.1f with it; 55 yalms out %.1f, %.1f; beside the shadow %.1f, %.1f far out; drew cascades %u (%u triangles)\n",
			here_off, here_on, far_off, far_on, side_on, far_side_on, first.sun_map_cascades, first.sun_map_triangles);
		check(first.sun_map && first.sun_map_cascades == 3 && first.sun_map_triangles == 12, "the map drawn (both cascades, the far-off square left out) and read");
		// The light in the air (look::vol_*), the sun behind the slab: the air lit where the sun reaches it, not in the slab's
		// shadow. The fog alone (its air black, no light) dims what's behind it; the sun's light added on top of that is the
		// shafts: more over ground beside the shadow, seen through lit air, than far out along it, seen through shadowed air.
		// (Four frames: the far cascade's every-other-frame turn below stays as it was.)
		{
			to_sun = sun_from(dh, 59.0f);
			with_box = false;
			characters = false;
			bumps = 0.0f;
			const std::vector<uint8_t> clear_air = frame(true, cam);
			air = 0.02f;
			air_dark = true;
			air_light = 0.0f;
			const std::vector<uint8_t> dimmed = frame(true, cam);
			air_light = 1.0f;
			const std::vector<uint8_t> shafts = frame(true, cam);
			air_dark = false;
			const std::vector<uint8_t> hazy = frame(true, cam);
			const draw_stats vol_stats = last;
			air = 0.0f;
			const double here_shaft = at_point(shafts, far_point, 3, 1) - at_point(dimmed, far_point, 3, 1);
			const double side_shaft = at_point(shafts, far_side, 3, 1) - at_point(dimmed, far_side, 3, 1);
			uint32_t changed = 0;
			for (size_t i = 0; i < size_t(W) * H; ++i)
			{
				const int d = (hazy[i * 4] + hazy[i * 4 + 1] + hazy[i * 4 + 2]) - (clear_air[i * 4] + clear_air[i * 4 + 1] + clear_air[i * 4 + 2]);
				changed += d != 0 ? 1 : 0;
			}
			if (FILE *f = nullptr; fopen_s(&f, "build\\volumetric_test.ppm", "wb") == 0 && f != nullptr)
			{
				std::fprintf(f, "P6 %u %u 255\n", W, H);
				for (size_t i = 0; i < size_t(W) * H; ++i)
					std::fwrite(&hazy[i * 4], 1, 3, f);
				std::fclose(f);
			}
			std::printf("  light in the air: %u pixels changed; the sun's light it adds: %.1f far out along the slab's shadow, %.1f beside it\n",
				changed, here_shaft, side_shaft);
			check(vol_stats.volumetric && vol_stats.air && changed > W * H / 4, "the light in the air worked out and laid over the frame");
			check(side_shaft > here_shaft + 2.0, "  the air lit beside the slab's shadow more than in it (shafts)");
		}
		check(here_off > 0.0 && std::fabs(here_off - plain_luma) < 1.5, "without the map, nothing off screen casts (the march can't see it)");
		check(here_on > 0.0 && here_on < here_off * 0.6, "an off-screen tall caster shadows the ground (near cascade)");
		check(far_on > 0.0 && far_on < far_off * 0.75, "... and ground 55 yalms out along its shadow (far cascade)");
		check(side_on > 0.0 && std::fabs(side_on - plain_luma) < 1.5 && far_side_on > 0.0 && std::fabs(far_side_on - plain_luma) < 2.0, "ground beside its shadow isn't shadowed");

		// The sun overhead: the card's opaque half only
		to_sun = { 0.0f, -1.0f, 0.0f };
		const std::vector<uint8_t> overhead = frame(true, cam);
		const double under_opaque = at_point(overhead, { at.x - 2, at.y, at.z }, 3, 3), under_clear = at_point(overhead, { at.x + 2, at.y, at.z }, 3, 3);
		std::printf("  the sun overhead, a card 30 yalms up cut out to its left half: under the opaque half %.1f, under the clear half %.1f\n", under_opaque, under_clear);
		check(under_opaque > 0.0 && under_opaque < plain_luma * 0.6, "a cut-out card shades the ground under its opaque texels");
		check(under_clear > 0.0 && std::fabs(under_clear - plain_luma) < 1.5, "... and not under its clear ones");

		// The sun in front: flat ground as it went in, high and low
		to_sun = sun_from({ -dh.x, 0.0f, -dh.z }, 59.0f);
		const std::vector<uint8_t> front_high = frame(true, cam);
		to_sun = sun_from({ -dh.x, 0.0f, -dh.z }, 15.0f);
		const std::vector<uint8_t> front_low = frame(true, cam);
		double worst = 0.0;
		for (const std::vector<uint8_t> *img : { &front_high, &front_low })
			for (float d : { 3.0f, 0.0f, -4.0f, -10.0f, -25.0f })
				for (float sd : { -2.0f, 0.0f, 2.0f })
				{
					const double v = at_point(*img, along(d, sd, 0), 4, 1);
					if (v >= 0.0)
						worst = std::max(worst, std::fabs(v - plain_luma));
				}
		std::printf("  the sun in front, 59 and 15 degrees up: flat ground off by %.2f at most (in the map itself)\n", worst);
		check(worst < 1.5, "flat ground in the map comes out as it went in (no shadow from a surface onto itself)");

		// Contact shadows: the box of 1h, the sun toward the camera and up (as in 1j)
		to_sun = normalize({ (eye.x - at.x) / 5.385f, (eye.y - at.y) / 5.385f - 1.0f, (eye.z - at.z) / 5.385f });
		with_box = true;
		const std::vector<uint8_t> box_lit = frame(true, cam);
		with_box = false;
		double box_lit_mean = 0.0;
		{
			double sum = 0.0;
			int n = 0;
			for (LONG y = LONG(py) - 10; y < LONG(py) + 4; ++y)
				for (LONG x = LONG(px) - 40; x < LONG(px) + 40; ++x)
				{
					const uint8_t *c = &box_lit[(size_t(y) * W + x) * 4];
					sum += 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
					n++;
				}
			box_lit_mean = sum / n;
		}
		std::printf("  with the map, the ground in front of the box (in the depth buffer, not the map): %.1f (lit %.1f)\n", box_lit_mean, plain_luma);
		check(box_lit_mean < plain_luma * 0.9, "what's only on screen still casts contact shadows (the march's first steps)");

		// Drawn again as soon as the sun's turn shows: never for the same sun and camera, nor for the sun 0.002 degrees
		// on (a 100-yalm shadow 3.5 mm: under a quarter of a near texel); with the sun moving a tenth of a degree a
		// frame, the near cascade every frame and the far one every other frame; the camera 15 yalms across the rays
		// (out of the near cascade's middle half), the near one at once
		to_sun = sun_from(dh, 59.0f);
		frame(true, cam);
		frame(true, cam);
		const uint32_t same = last.sun_map_cascades;
		to_sun = sun_from(dh, 59.002f);
		frame(true, cam);
		const uint32_t hair = last.sun_map_cascades;
		to_sun = sun_from(dh, 59.1f);
		frame(true, cam);
		const uint32_t tenth_a = last.sun_map_cascades;
		to_sun = sun_from(dh, 59.2f);
		frame(true, cam);
		const uint32_t tenth_b = last.sun_map_cascades;
		camera moved = cam;
		{
			const vec3 e2 = { eye.x + side.x * 15.0f, eye.y, eye.z + side.z * 15.0f }, a2 = { at.x + side.x * 15.0f, at.y, at.z + side.z * 15.0f };
			look_at(e2, a2, { 0.0f, -1.0f, 0.0f }, moved.view);
		}
		frame(true, moved);
		const uint32_t walked = last.sun_map_cascades;
		std::printf("  cascades drawn again (bits: 1 near, 2 far): same sun and camera %u; the sun 0.002 degrees on %u; a tenth of a degree on, twice: %u, %u; the camera 15 yalms across %u\n",
			same, hair, tenth_a, tenth_b, walked);
		check(same == 0 && hair == 0, "the map isn't drawn again for the same sun and camera, nor a turn too small to show");
		check((tenth_a & 1) != 0 && (tenth_b & 1) != 0 && ((tenth_a ^ tenth_b) & 2) != 0, "with the sun moving, the near cascade is drawn again every frame, the far one every other frame");
		check((walked & 1) != 0, "the camera moving draws the near cascade again at once");

		// Smooth: the sun turning a tenth of a degree a frame (as the game eases its light) moves the slab's shadow a
		// little each frame, not in jumps (drawn only past a degree, it stood still for nine frames, then jumped).
		// Watched where the shadow's edge, 15 yalms from the slab, sweeps over the ground in those 20 frames.
		{
			const vec3 spot = along(-3.0f, 2.74f, 0.0f);
			std::vector<double> seen;
			for (int i = 0; i <= 20; ++i)
			{
				const float a = 0.1f * float(i) * 3.14159265f / 180.0f;
				to_sun = sun_from({ dh.x * std::cos(a) + side.x * std::sin(a), 0.0f, dh.z * std::cos(a) + side.z * std::sin(a) }, 59.0f);
				seen.push_back(at_point(frame(true, cam), spot, 3, 3));
			}
			double biggest = 0.0;
			int moved_frames = 0;
			for (size_t i = 1; i < seen.size(); ++i)
			{
				biggest = std::max(biggest, std::fabs(seen[i] - seen[i - 1]));
				moved_frames += std::fabs(seen[i] - seen[i - 1]) > 0.5 ? 1 : 0;
			}
			const double whole = std::fabs(seen.back() - seen.front());
			std::printf("  the sun turning 2 degrees a tenth at a time: the shadow's edge there went %.1f -> %.1f, in %d of 20 frames, at most %.1f in one\n",
				seen.front(), seen.back(), moved_frames, biggest);
			check(whole > 30.0 && biggest < 0.4 * whole && moved_frames >= 5, "the shadow moves with the sun a little each frame, not in jumps");
		}

		// Far past the zone's meshes, away from the sun (further from it than anything in the map): nothing shades
		// the ground there (the map's empty texels are never in the way)
		{
			camera away = cam;
			const vec3 e2 = { eye.x - dh.x * 5000.0f, eye.y, eye.z - dh.z * 5000.0f }, a2 = { at.x - dh.x * 5000.0f, at.y, at.z - dh.z * 5000.0f };
			look_at(e2, a2, { 0.0f, -1.0f, 0.0f }, away.view);
			const std::vector<uint8_t> img = frame(true, away);
			double sum = 0.0;
			int n = 0;
			for (LONG y = LONG(py) - 10; y < LONG(py) + 60; ++y)
				for (LONG x = LONG(px) - 80; x < LONG(px) + 80; ++x)
				{
					const uint8_t *c = &img[(size_t(y) * W + x) * 4];
					sum += 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
					n++;
				}
			std::printf("  5,000 yalms past the zone's meshes, away from the sun: ground %.1f (went in %.1f)\n", sum / n, plain_luma);
			check(last.sun_map && std::fabs(sum / n - plain_luma) < 1.5, "ground further from the sun than anything in the map isn't shaded by it");
		}

		// The sun 2 degrees up behind the slab: the flat ground in its shadow loses the sun's share of its light (small
		// at 2 degrees) alike near, seen face on, and far out, where it's seen nearly edge on (its facing can't be told
		// from the depth there: each row of pixels yalms further than the last), taken there at the share of a surface
		// facing up. (Taken whole, the far ground was twice as dark as the near at a low sun, along a line about 10
		// yalms ahead of a low camera that moved with it. Before that: the facing alone, under 0.05, left both
		// unshaded, as a ledge's steep face seen edge on under Jugner's trees stayed bright.)
		{
			to_sun = sun_from(dh, 2.0f);
			const std::vector<uint8_t> img = frame(true, cam);
			const double edge_on = at_point(img, along(-40, 0, 0), 3, 1), far_beside = at_point(img, along(-40, 9, 0), 3, 1);
			const double near_on = at_point(img, at, 8, 4);
			std::printf("  the sun 2 degrees up behind the slab: ground 45 yalms off (edge on) %.1f, beside the shadow %.1f; near (face on) %.1f (went in %.1f)\n",
				edge_on, far_beside, near_on, plain_luma);
			check(edge_on > 0.0 && edge_on < plain_luma - 2.0 && std::fabs(edge_on - near_on) < 3.0 && std::fabs(far_beside - plain_luma) < 2.0,
				"flat ground under something far above it is shaded alike seen edge on far out and face on near (no line where the view turns grazing)");
			check(near_on > plain_luma * 0.88 && near_on < plain_luma - 2.0, "a surface the sun only grazes loses only the sun's share of its light");
		}

		// The host's zone (set_zone_casters: HiFi XI's native zone data) in place of the uploaded meshes: a stand-in that
		// says its box is 200 yalms around the looked-at point and draws each cascade as a plate (a square 0.8 yalms across the
		// sun's rays: under the other sun below, its shadow falls beside the looked-at point) 1.2 yalms over the looked-at ground toward a sun 59 degrees up from the side. With it: the cascades
		// are the host's (both drawn by it), the looked-at ground in the plate's shadow; the slab's shadow, from the uploaded
		// meshes, isn't there (the sun behind the slab). A new number for its data draws both cascades again; with the host
		// gone, the meshes again.
		{
			struct zone_host
			{
				vec3 q, centre;
				uint64_t generation = 1;
				bool have = true;
				uint32_t draws = 0;
			} zh;
			to_sun = sun_from(side, 59.0f);
			const float lift = 1.2f / std::sin(59.0f * 3.14159265f / 180.0f);
			zh.q = { at.x + to_sun.x * lift, at.y + to_sun.y * lift, at.z + to_sun.z * lift };
			zh.centre = at;
			r.set_zone_casters(
				[](void *context, float lo[3], float hi[3], uint64_t *generation) {
					const zone_host &h = *static_cast<const zone_host *>(context);
					const float c[3] = { h.centre.x, h.centre.y, h.centre.z };
					for (int a = 0; a < 3; ++a)
					{
						lo[a] = c[a] - 200.0f;
						hi[a] = c[a] + 200.0f;
					}
					*generation = h.generation;
					return h.have;
				},
				[](void *context, ID3D12GraphicsCommandList *list, const float m[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size) -> uint32_t {
					zone_host &h = *static_cast<zone_host *>(context);
					h.draws++;
					const vec3 &q = h.q;
					const float x = q.x * m[0] + q.y * m[4] + q.z * m[8] + m[12], y = q.x * m[1] + q.y * m[5] + q.z * m[9] + m[13];
					const float z = q.x * m[2] + q.y * m[6] + q.z * m[10] + m[14];
					const float per_yalm = 0.5f * float(size) * std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
					const float cx = (x * 0.5f + 0.5f) * float(size), cy = (0.5f - y * 0.5f) * float(size);
					const D3D12_RECT rc = { LONG(cx - 0.4f * per_yalm), LONG(cy - 0.4f * per_yalm), LONG(cx + 0.4f * per_yalm) + 1, LONG(cy + 0.4f * per_yalm) + 1 };
					list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, z, 0, 1, &rc);
					return 2;
				},
				&zh);
			const std::vector<uint8_t> hosted = frame(true, cam);
			const draw_stats first_host = last;
			const uint32_t first_draws = zh.draws;
			frame(true, cam);
			const uint32_t still = last.sun_map_cascades;
			zh.generation = 2;
			frame(true, cam);
			const uint32_t new_data = last.sun_map_cascades;
			to_sun = sun_from(dh, 59.0f); // (the sun behind the slab: its shadow on the looked-at ground only from the meshes)
			const std::vector<uint8_t> slab_hosted = frame(true, cam);
			zh.have = false;
			const std::vector<uint8_t> slab_meshes = frame(true, cam);
			const draw_stats meshes_back = last;
			r.set_zone_casters(nullptr, nullptr, nullptr);
			const double here_hosted = at_point(hosted, at, 3, 3), beside_hosted = at_point(hosted, along(0, 4, 0), 3, 3);
			const double slab_with_host = at_point(slab_hosted, at, 8, 4), slab_with_meshes = at_point(slab_meshes, at, 8, 4);
			std::printf("  the host's zone: looked-at ground %.1f, 4 yalms beside %.1f (went in %.1f); cascades drawn by it %u (stats %u, host %d); then %u for the same data, %u for new data; the sun behind the slab: %.1f with the host, %.1f with the meshes (host %d)\n",
				here_hosted, beside_hosted, plain_luma, first_draws, first_host.sun_map_cascades, first_host.sun_map_host ? 1 : 0, still, new_data, slab_with_host, slab_with_meshes,
				meshes_back.sun_map_host ? 1 : 0);
			check(first_host.sun_map && first_host.sun_map_host && first_draws == 2 && first_host.sun_map_cascades == 3 && first_host.sun_map_triangles == 4, "with the host's zone, both cascades drawn by it");
			check(here_hosted > 0.0 && here_hosted < plain_luma * 0.8 && beside_hosted > 0.0 && std::fabs(beside_hosted - plain_luma) < 1.5, "what it draws shadows the ground behind it, not beside it");
			check(still == 0 && new_data == 3, "drawn again for new data (both cascades), not for the same");
			check(slab_with_host > 0.0 && std::fabs(slab_with_host - plain_luma) < 1.5 && slab_with_meshes < plain_luma * 0.6 && !meshes_back.sun_map_host,
				"the uploaded meshes aren't drawn while the host has the zone, and are again once it hasn't");
		}

		// The host's casters (set_casters: HiFi XI's characters and monsters drawn from their own meshes): a stand-in that
		// clears a rectangle of their map, a yalm wide and 1.6 tall across the sun's rays, to the depth of a point 1.2
		// yalms over the looked-at ground toward a sun 59 degrees up from the side (a plate facing the sun, as a body
		// would cast). With the characters' shadows asked for, the looked-at ground is in its shadow and ground 3 yalms
		// beside it isn't; not asked for (rule bit 4), or with no hook, nothing shades it (no actors here).
		{
			struct plate
			{
				vec3 q;
				uint32_t calls = 0;
			} pl;
			to_sun = sun_from(side, 59.0f);
			const float lift = 1.2f / std::sin(59.0f * 3.14159265f / 180.0f);
			pl.q = { at.x + to_sun.x * lift, at.y + to_sun.y * lift, at.z + to_sun.z * lift };
			r.set_casters([](void *context, ID3D12GraphicsCommandList *list, const float m[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size) {
				plate &p = *static_cast<plate *>(context);
				p.calls++;
				const vec3 &q = p.q;
				const float x = q.x * m[0] + q.y * m[4] + q.z * m[8] + m[12], y = q.x * m[1] + q.y * m[5] + q.z * m[9] + m[13];
				const float z = q.x * m[2] + q.y * m[6] + q.z * m[10] + m[14];
				const float per_yalm = 0.5f * float(size) * std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
				const float cx = (x * 0.5f + 0.5f) * float(size), cy = (0.5f - y * 0.5f) * float(size);
				const D3D12_RECT rc = { LONG(cx - 0.5f * per_yalm), LONG(cy - 0.8f * per_yalm), LONG(cx + 0.5f * per_yalm), LONG(cy + 0.8f * per_yalm) };
				list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, z, 0, 1, &rc);
				return true;
			}, &pl);
			characters = true;
			const std::vector<uint8_t> asked = frame(true, cam);
			const draw_stats with_casters = last;
			characters = false;
			const std::vector<uint8_t> not_asked = frame(true, cam);
			const draw_stats unasked = last;
			r.set_casters(nullptr, nullptr);
			characters = true;
			const std::vector<uint8_t> no_hook = frame(true, cam);
			characters = false;
			const vec3 beside = along(0, 3, 0);
			const double here_asked = at_point(asked, at, 3, 3), beside_asked = at_point(asked, beside, 3, 3);
			const double here_unasked = at_point(not_asked, at, 3, 3), here_no_hook = at_point(no_hook, at, 3, 3);
			std::printf("  the host's casters (a plate 1.2 yalms over the ground toward the sun): looked-at ground %.1f, 3 yalms beside %.1f; not asked for %.1f; no hook %.1f (went in %.1f); hook called %u times\n",
				here_asked, beside_asked, here_unasked, here_no_hook, plain_luma, pl.calls);
			check(with_casters.casters && !unasked.casters && pl.calls == 1, "the casters' hook called and its map read only with the characters' shadows asked for");
			check(here_asked > 0.0 && here_asked < plain_luma * 0.8, "what the host draws into the casters' map shadows the ground behind it");
			check(beside_asked > 0.0 && std::fabs(beside_asked - plain_luma) < 1.5, "... and not the ground beside its shadow");
			check(std::fabs(here_unasked - plain_luma) < 1.5 && std::fabs(here_no_hook - plain_luma) < 1.5, "not asked for, or with no hook, nothing shades it");
		}

		// A rough ground (bumps 0.08 yalms high, 0.6 across: their slopes under 40 degrees) in a sun 60 degrees up from
		// the right, nothing in the map near: nothing casts on it, so no shadow anywhere on it, near or far out where
		// it's seen nearly edge on (the march's first steps caught its own bumps: dark slabs that moved with the camera)
		{
			r.unload_shadow();
			bumps = 0.08f;
			to_sun = sun_from(side, 60.0f);
			const std::vector<uint8_t> img = frame(false, cam);
			// (against the same rough ground without sun shadows, over the lower two thirds of the picture)
			const std::vector<uint8_t> unshadowed = [&]() {
				// (the same frame with the sun's strength 0: what the rough ground is without shadows)
				barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
				g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
				ground_depth(cam, bumps);
				barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				look l2 = l;
				l2.grass = false;
				l2.water = false;
				l2.ao_strength = 0.0f;
				l2.relief = 0.0f;
				l2.sun_shadow = 0.0f;
				r.draw(g.list, t, cam, l2, error);
				barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
				return g.read(world, 4);
			}();
			bumps = 0.0f;
			double darker = 0.0, most = 0.0;
			uint32_t counted = 0;
			for (UINT y = H / 3; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					const uint8_t *with = &img[(size_t(y) * W + x) * 4], *without = &unshadowed[(size_t(y) * W + x) * 4];
					const double d = (0.3 * without[0] + 0.59 * without[1] + 0.11 * without[2]) - (0.3 * with[0] + 0.59 * with[1] + 0.11 * with[2]);
					darker += d;
					most = std::max(most, d);
					counted++;
				}
			darker /= std::max(counted, 1u);
			std::printf("  a rough ground in a high sun, nothing casting: %.2f darker on average with sun shadows, at most %.1f at a pixel\n", darker, most);
			check(darker < 0.15 && most < 15.0, "a rough surface doesn't shadow itself"); // (before: 0.34 on average, 22.9 at most)
		}
		r.unload_shadow();
		frame(true, cam);
		check(!last.sun_map && !r.shadow_loaded(), "without the zone's meshes, no map");
		check(g.report_messages() == 0, "no debug-layer errors (shadow map)");

	}

	// 1o2. Leaf shadows hold still while the map is drawn again: a card 8 yalms across, 6 over the looked-at point, its
	// mask 1,024 texels a side (0.8 cm: finer than the near cascade's 3.9 cm texels, as a leaf texture's are) with 70 %
	// of them opaque at random, over the ground sheet. The sun nearly overhead turns a hundredth of a degree a frame
	// (the near cascade drawn again each frame, each time on a slightly different grid, as the game's turning sun has
	// it about ten times a second). Under the card, the ground's brightness from frame to frame: with each map texel
	// tested on one mask bit (kSunMapCutout 0, the shaders before), the pattern changed each time (dapples flickering
	// in place, in game 2026-10-21); with the bits over its footprint counted (kSunMapCutout 0.5, kSunMapLeafArea), it
	// changes far less. A coarse cut-out (1o's, its texels coarser than the map's) is drawn as before.
	{
		const float plain[4] = { 0.45f, 0.42f, 0.36f, 0.5f };
		const vec3 dh = normalize({ eye.x - at.x, 0.0f, eye.z - at.z }), side = { -dh.z, 0.0f, dh.x };
		auto along = [&](float d, float s, float up) { return vec3 { at.x + dh.x * d + side.x * s, at.y - up, at.z + dh.z * d + side.z * s }; };
		shadow_file zone;
		zone.zone = 998;
		auto corner = [&](const vec3 &v, uint32_t w) {
			uint32_t bits[3];
			std::memcpy(bits, &v, 12);
			zone.triangles.insert(zone.triangles.end(), { bits[0], bits[1], bits[2], w });
		};
		auto quad = [&](const vec3 &a, const vec3 &b, const vec3 &c, const vec3 &d, bool card) {
			const uint32_t wa = card ? (1u << 24) : 0, wb = card ? (4095u | (1u << 24)) : 0, wc = card ? (4095u | (4095u << 12) | (1u << 24)) : 0, wd = card ? ((4095u << 12) | (1u << 24)) : 0;
			corner(a, wa), corner(b, wb), corner(c, wc);
			corner(a, wa), corner(c, wc), corner(d, wd);
		};
		quad(along(3000, 3000, 0), along(3001, 3000, 0), along(3001, 3001, 0), along(3000, 3001, 0), false);
		zone.cells.push_back({ 90, 90, 0, zone.triangle_count() });
		quad(along(-100, -100, 0), along(100, -100, 0), along(100, 100, 0), along(-100, 100, 0), false); // the ground
		zone.cells.push_back({ 0, 0, 2, 2 });
		quad({ at.x - 4, at.y - 6, at.z - 4 }, { at.x + 4, at.y - 6, at.z - 4 }, { at.x + 4, at.y - 6, at.z + 4 }, { at.x - 4, at.y - 6, at.z + 4 }, true);
		zone.cells.push_back({ 1, 0, 4, 2 });
		zone.masks = { 1024, 1024, 0, 32 };
		uint32_t seed = 12345u;
		for (int row = 0; row < 1024; ++row)
			for (int w = 0; w < 32; ++w)
			{
				uint32_t word = 0;
				for (int k = 0; k < 32; ++k)
				{
					seed = seed * 1664525u + 1013904223u;
					word |= ((seed >> 8) % 100u < 70u ? 1u : 0u) << k;
				}
				zone.words.push_back(word);
			}
		check(r.upload_shadow(zone, error), "a fine-leaved card's meshes uploaded");
		auto frame = [&](const vec3 &to_sun) {
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.grass = false;
			l2.ao_strength = 0.0f;
			l2.relief = 0.0f;
			l2.water = false;
			l2.sun[0] = to_sun.x;
			l2.sun[1] = to_sun.y;
			l2.sun[2] = to_sun.z;
			l2.sun_shadow = 0.6f;
			l2.sun_map = true;
			r.draw(g.list, t, cam, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			std::vector<uint8_t> img = g.read(world, 4);
			r.release_old(0);
			return img;
		};
		// (each frame's brightness over a box under the card's middle, and how much it changes frame to frame, per pixel)
		const int bx = int(px), by = int(py);
		auto flicker = [&](double &mean_luma) {
			std::vector<std::vector<double>> frames;
			for (int f = 0; f < 12; ++f)
			{
				const float a = (0.5f + 0.01f * f) * 3.14159265f / 180.0f; // (degrees from straight down)
				const std::vector<uint8_t> img = frame(normalize({ side.x * std::sin(a), -std::cos(a), side.z * std::sin(a) }));
				std::vector<double> box;
				for (int y = by - 8; y <= by + 8; ++y)
					for (int x = bx - 24; x <= bx + 24; ++x)
					{
						const uint8_t *c = &img[(size_t(y) * W + x) * 4];
						box.push_back(0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]);
					}
				frames.push_back(box);
			}
			double change = 0.0, sum = 0.0;
			size_t n = 0;
			for (size_t f = 2; f < frames.size(); ++f) // (from the third: the first draws both cascades)
				for (size_t i = 0; i < frames[f].size(); ++i)
				{
					change += std::fabs(frames[f][i] - frames[f - 1][i]);
					sum += frames[f][i];
					n++;
				}
			mean_luma = sum / std::max<size_t>(n, 1);
			return change / std::max<size_t>(n, 1);
		};
		std::string source;
		if (FILE *f = nullptr; fopen_s(&f, "src\\render\\grass.hlsl", "rb") == 0 && f != nullptr)
		{
			char buffer[4096];
			size_t got;
			while ((got = fread(buffer, 1, sizeof(buffer), f)) != 0)
				source.append(buffer, got);
			fclose(f);
		}
		const std::string marker = "static const float kSunMapCutout = 0.5;";
		std::string one_bit = source;
		const size_t at_marker = one_bit.find(marker);
		check(at_marker != std::string::npos, "kSunMapCutout found in the shaders' source");
		double luma_area = 0.0, luma_bit = 0.0, change_area = 0.0, change_bit = 0.0;
		if (at_marker != std::string::npos)
		{
			change_area = flicker(luma_area);
			one_bit.replace(at_marker, marker.size(), "static const float kSunMapCutout = 0.0;");
			grass::shader_code code;
			bool from_cache = false;
			std::string why;
			const bool compiled = grass::compile_shaders(one_bit, L"build\\test\\variant-cache", code, from_cache, why);
			check(compiled, "the shaders with one mask bit a map texel compile");
			if (compiled)
			{
				r.set_shader_code(std::move(code));
				change_bit = flicker(luma_bit);
			}
			grass::shader_code restored;
			if (grass::compile_shaders(source, L"build\\test\\variant-cache", restored, from_cache, why))
				r.set_shader_code(std::move(restored));
		}
		std::printf("  a fine-leaved card's shadow with the map drawn again each frame: brightness under it %.1f (one bit) / %.1f (its footprint), changing %.2f / %.2f a pixel a frame\n",
			luma_bit, luma_area, change_bit, change_area);
		check(change_bit > 1.0, "one mask bit a map texel: the dapples change each time the map is drawn again");
		check(change_area < 0.35 * change_bit, "the bits over its footprint: they hold still");
		const double plain_luma = 0.3 * 0.45 * 255 + 0.59 * 0.42 * 255 + 0.11 * 0.36 * 255;
		check(luma_area > 0.0 && luma_area < plain_luma * 0.75, "and it still shades the ground");
		r.unload_shadow();
		check(g.report_messages() == 0, "no debug-layer errors (leaf shadows)");
	}

	// 1o3. The map drawn about once a second and faded between drawings (the look's clock, 60 frames a second): a
	// card 8 yalms across 6 over the looked-at point, its mask 192 texels a side (4.2 cm: as coarse as the map's 3.9 cm
	// texels, as West Ronfaure's leaves are) with 70 % of them opaque at random, over the ground sheet; the sun 50
	// degrees up turning as the game's does, a tenth of a degree a second.
	// - Without the clock the near cascade is drawn again some 10 times a second (each time it has turned a quarter
	//   texel; 16 in game), and the ground under the card's shadow changes from frame to frame as its leaves' edge texels flip
	//   (the shimmer on the leaves' edges, in game 2026-10-21); with it, twice in two seconds, and the ground changes far
	//   less from frame to frame.
	// - After each drawing the fade starts at 0 and is halfway half a second on.
	// - The camera moved 15 yalms aside (out of the near cascade's middle half): drawn again at once.
	// - The sun held still: the fade ends on the picture a single drawing there gives.
	{
		const float plain[4] = { 0.45f, 0.42f, 0.36f, 0.5f };
		const vec3 dh = normalize({ eye.x - at.x, 0.0f, eye.z - at.z }), side = { -dh.z, 0.0f, dh.x };
		auto along = [&](float d, float s, float up) { return vec3 { at.x + dh.x * d + side.x * s, at.y - up, at.z + dh.z * d + side.z * s }; };
		shadow_file zone;
		zone.zone = 997;
		auto corner = [&](const vec3 &v, uint32_t w) {
			uint32_t bits[3];
			std::memcpy(bits, &v, 12);
			zone.triangles.insert(zone.triangles.end(), { bits[0], bits[1], bits[2], w });
		};
		auto quad = [&](const vec3 &a, const vec3 &b, const vec3 &c, const vec3 &d, bool card) {
			const uint32_t wa = card ? (1u << 24) : 0, wb = card ? (4095u | (1u << 24)) : 0, wc = card ? (4095u | (4095u << 12) | (1u << 24)) : 0, wd = card ? ((4095u << 12) | (1u << 24)) : 0;
			corner(a, wa), corner(b, wb), corner(c, wc);
			corner(a, wa), corner(c, wc), corner(d, wd);
		};
		quad(along(3000, 3000, 0), along(3001, 3000, 0), along(3001, 3001, 0), along(3000, 3001, 0), false);
		zone.cells.push_back({ 90, 90, 0, zone.triangle_count() });
		quad(along(-100, -100, 0), along(100, -100, 0), along(100, 100, 0), along(-100, 100, 0), false); // the ground
		zone.cells.push_back({ 0, 0, 2, 2 });
		quad({ at.x - 4, at.y - 6, at.z - 4 }, { at.x + 4, at.y - 6, at.z - 4 }, { at.x + 4, at.y - 6, at.z + 4 }, { at.x - 4, at.y - 6, at.z + 4 }, true);
		zone.cells.push_back({ 1, 0, 4, 2 });
		zone.masks = { 192, 192, 0, 6 };
		uint32_t seed = 777u;
		for (int row = 0; row < 192; ++row)
			for (int w = 0; w < 6; ++w)
			{
				uint32_t word = 0;
				for (int k = 0; k < 32; ++k)
				{
					seed = seed * 1664525u + 1013904223u;
					word |= ((seed >> 8) % 100u < 70u ? 1u : 0u) << k;
				}
				zone.words.push_back(word);
			}
		draw_stats st;
		auto frame = [&](const camera &c, const vec3 &to_sun, double clock) {
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(c);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.grass = false;
			l2.ao_strength = 0.0f;
			l2.relief = 0.0f;
			l2.water = false;
			l2.sun[0] = to_sun.x;
			l2.sun[1] = to_sun.y;
			l2.sun[2] = to_sun.z;
			l2.sun_shadow = 0.6f;
			l2.sun_map = true;
			l2.clock = clock;
			st = r.draw(g.list, t, c, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			std::vector<uint8_t> img = g.read(world, 4);
			r.release_old(0);
			return img;
		};
		// (the sun 50 degrees up, turned about the world's up by `turn` degrees from across the view)
		auto sun_at = [&](double turn) {
			const float a = float(turn * 3.14159265 / 180.0), e = 50.0f * 3.14159265f / 180.0f;
			const vec3 hz = { side.x * std::cos(a) + dh.x * std::sin(a), 0.0f, side.z * std::cos(a) + dh.z * std::sin(a) };
			return normalize({ hz.x * std::cos(e), -std::sin(e), hz.z * std::cos(e) });
		};
		// (where the card's middle shades the ground, on screen)
		const vec3 s0 = sun_at(0.0);
		const float down = 6.0f / -s0.y;
		const vec3 shade = { at.x - s0.x * down, at.y, at.z - s0.z * down };
		const float sx = shade.x * vp[0] + shade.y * vp[4] + shade.z * vp[8] + vp[12], sy = shade.x * vp[1] + shade.y * vp[5] + shade.z * vp[9] + vp[13];
		const float sw = shade.x * vp[3] + shade.y * vp[7] + shade.z * vp[11] + vp[15];
		const int bx = int((sx / sw + 1.0f) * 0.5f * W), by = int((1.0f - sy / sw) * 0.5f * H);
		auto box_of = [&](const std::vector<uint8_t> &img) {
			std::vector<double> v;
			for (int y = std::max(by - 10, 0); y <= std::min(by + 10, int(H) - 1); ++y)
				for (int x = std::max(bx - 30, 0); x <= std::min(bx + 30, int(W) - 1); ++x)
				{
					const uint8_t *c = &img[(size_t(y) * W + x) * 4];
					v.push_back(0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]);
				}
			return v;
		};
		// Two seconds of the sun turning, 60 frames a second, without and with the clock: the ground's change from frame
		// to frame under the card, and the drawings of each cascade
		auto run = [&](bool timed, uint32_t &near_draws, uint32_t &far_draws, std::vector<float> *fades) {
			check(r.upload_shadow(zone, error), "the coarse-leaved card's meshes uploaded");
			std::vector<double> before;
			double change = 0.0;
			size_t n = 0;
			near_draws = far_draws = 0;
			for (int f = 0; f < 132; ++f)
			{
				const double clock = 100.0 + f / 60.0;
				const std::vector<double> now = box_of(frame(cam, sun_at(0.1 * f / 60.0), timed ? clock : 0.0));
				if (f >= 12)
				{
					near_draws += st.sun_map_cascades & 1;
					far_draws += (st.sun_map_cascades >> 1) & 1;
					for (size_t i = 0; i < now.size() && i < before.size(); ++i)
						change += std::fabs(now[i] - before[i]);
					n += now.size();
				}
				if (fades != nullptr)
					fades->push_back(st.sun_map_cascades & 1 ? -1.0f : st.sun_map_fade[0]); // (-1: drawn again this frame)
				before = now;
			}
			return change / std::max<size_t>(n, 1);
		};
		uint32_t near_old = 0, far_old = 0, near_new = 0, far_new = 0;
		std::vector<float> fades;
		const double change_old = run(false, near_old, far_old, nullptr);
		const double change_new = run(true, near_new, far_new, &fades);
		std::printf("  the map in two seconds of the sun turning: near cascade drawn %u times, far %u, the ground under the leaves changing %.3f a pixel a frame; with the clock: %u, %u, %.3f\n",
			near_old, far_old, change_old, near_new, far_new, change_new);
		check(near_old >= 15, "without the clock: the near cascade drawn again each time the sun has turned a little (10 a second here)");
		check(near_new <= 3 && far_new <= 3 && near_new >= 1, "with it: about once a second");
		check(change_new < 0.35 * change_old, "and the leaves' edges change far less from frame to frame (faded, not flipped)");
		// (the fade after the first drawing with the clock past the start: 0 at it, halfway half a second on)
		size_t drawn = 0;
		for (size_t i = 20; i < fades.size() && drawn == 0; ++i)
			if (fades[i] < 0.0f)
				drawn = i;
		const float just_after = drawn + 1 < fades.size() ? fades[drawn + 1] : -2.0f, half = drawn + 30 < fades.size() ? fades[drawn + 30] : -2.0f;
		std::printf("  the fade after a drawing at frame %zu: %.3f the frame after, %.3f half a second on\n", drawn, just_after, half);
		check(drawn > 0 && just_after >= 0.0f && just_after < 0.05f && std::fabs(half - 0.5f) < 0.06f, "each drawing fades in over a second from 0");
		// (the camera 15 yalms aside: out of the near cascade's middle half)
		camera aside;
		look_at({ eye.x + side.x * 15.0f, eye.y, eye.z + side.z * 15.0f }, { at.x + side.x * 15.0f, at.y, at.z + side.z * 15.0f }, { 0.0f, -1.0f, 0.0f }, aside.view);
		std::memcpy(aside.proj, cam.proj, sizeof(cam.proj));
		frame(aside, sun_at(0.1 * 132 / 60.0), 100.0 + 132 / 60.0);
		check((st.sun_map_cascades & 1) != 0, "the camera out of the near cascade's middle: drawn again at once");
		// (the sun held still: 2.5 seconds on, the fade has ended on the drawing for it; against a single drawing there)
		const vec3 held = sun_at(0.3);
		std::vector<uint8_t> settled;
		for (int f = 0; f < 150; ++f)
			settled = frame(cam, held, 103.0 + f / 60.0);
		const float settled_fade = st.sun_map_fade[0];
		check(r.upload_shadow(zone, error), "the meshes uploaded again (no drawing yet)");
		const std::vector<uint8_t> single = frame(cam, held, 0.0);
		int most = 0;
		for (size_t i = 0; i < settled.size(); ++i)
			most = std::max(most, std::abs(int(settled[i]) - int(single[i])));
		std::printf("  the sun held still: the fade at %.2f, %d at most off a single drawing there\n", settled_fade, most);
		check(settled_fade == 1.0f && most <= 1, "the fade ends on the picture a single drawing gives");
		r.unload_shadow();
		check(g.report_messages() == 0, "no debug-layer errors (the map faded between drawings)");
	}

	// 1l. The water view: the zone's water collision over the world, each spot tinted by whether what's drawn
	// there is at it (blue: deep water) or in front of it (red). A patch of deep water lying on the ground
	// shows blue; the same patch a yalm under the ground shows red; with the view off, neither
	{
		const float plain[4] = { 0.45f, 0.42f, 0.36f, 0.5f };
		auto patch = [&](float y) {
			blade_file wf;
			wf.zone = 100;
			wf.cell_size = 32.0f;
			wf.stride = 8;
			const float x0 = at.x - 4.0f, x1 = at.x + 4.0f, z0 = at.z - 4.0f, z1 = at.z + 4.0f;
			const float corners[6][2] = { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z0 }, { x1, z1 }, { x0, z1 } };
			// (a cell 2,000 yalms off first, out of the water's reach: this one's draw starts past the buffer's
			// first corner, as a zone's do; SV_VertexID counts from 0 in every draw, whatever its start vertex)
			for (int k = 0; k < 6; ++k)
				wf.blades.insert(wf.blades.end(), { at.x + 2000.0f + float(k & 1), y, at.z + float(k >> 1), 9.0f, y, 5.0f, 0.0f, 0.0f });
			wf.cells.push_back({ int32_t(std::floor((at.x + 2000.0f) / 32.0f)), int32_t(std::floor(at.z / 32.0f)), 0, 6 });
			for (const auto &c : corners)
				wf.blades.insert(wf.blades.end(), { c[0], y, c[1], 9.0f, y, 5.0f, 0.0f, 0.0f });
			wf.cells.push_back({ int32_t(std::floor(at.x / 32.0f)), int32_t(std::floor(at.z / 32.0f)), 6, 6 });
			return wf;
		};
		auto frame = [&](bool on) {
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.grass = false;
			l2.ao_strength = 0.0f;
			l2.relief = 0.0f;
			l2.sun_shadow = 0.0f;
			l2.water = false;
			l2.water_debug = on;
			const draw_stats st = r.draw(g.list, t, cam, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			const std::vector<uint8_t> q = g.read(world, 4);
			uint32_t blue = 0, red = 0;
			for (size_t i = 0; i < size_t(W) * H; ++i)
			{
				const int cr = q[i * 4], cb = q[i * 4 + 2];
				blue += cb > cr + 60 ? 1u : 0u;
				red += cr > cb + 60 ? 1u : 0u;
			}
			return std::make_tuple(blue, red, st.water_corners);
		};
		std::string why;
		const bool up_on = r.upload_water(patch(at.y), why);
		const auto [on_blue, on_red, on_corners] = frame(true);
		const auto [off_blue, off_red, off_corners] = frame(false);
		const bool up_under = r.upload_water(patch(at.y + 1.0f), why);
		const auto [under_blue, under_red, under_corners] = frame(true);
		r.unload_water();
		std::printf("  water view: on the ground %u blue, %u red pixels (%u corners); a yalm under it %u blue, %u red; off %u blue, %u red\n",
			on_blue, on_red, on_corners, under_blue, under_red, off_blue, off_red);
		check(up_on && up_under && on_corners == 6 && under_corners == 6 && off_corners == 0, "the water patch loaded and drawn (only with the view on)");
		check(on_blue > 2000 && on_red < on_blue / 50, "water lying on the ground shows blue (at what's drawn)");
		check(under_red > 2000 && under_blue < under_red / 50, "water under the ground shows red (what's drawn is in front)");
		check(off_blue == 0 && off_red == 0, "with the view off, the world is left as it was");
		check(g.report_messages() == 0, "no debug-layer errors (water view)");
	}

	// 1m. The water: a patch of water half a yalm deep over the ground (its surface over the bed) changes the
	// world there, darker for the depth, and only there; the same patch with its surface a yalm under the
	// ground (the bank in front of it) leaves the world as drawn; and a film two hundredths of a yalm deep
	// (the water's line around a rock or someone's legs) changes it far less: it fades in over the last
	// few hundredths of a yalm, no hard edge
	{
		const float plain[4] = { 0.45f, 0.42f, 0.36f, 0.5f };
		auto pool = [&](float bed, float surface) {
			blade_file wf;
			wf.zone = 100;
			wf.cell_size = 32.0f;
			wf.stride = 8;
			const float x0 = at.x - 4.0f, x1 = at.x + 4.0f, z0 = at.z - 4.0f, z1 = at.z + 4.0f;
			const float corners[6][2] = { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z0 }, { x1, z1 }, { x0, z1 } };
			// (a cell 2,000 yalms off first, out of the water's reach: this one's draw starts past the buffer's
			// first corner, as a zone's do; SV_VertexID counts from 0 in every draw, whatever its start vertex)
			for (int k = 0; k < 6; ++k)
				wf.blades.insert(wf.blades.end(), { at.x + 2000.0f + float(k & 1), bed, at.z + float(k >> 1), 9.0f, surface, 5.0f, 0.0f, 0.0f });
			wf.cells.push_back({ int32_t(std::floor((at.x + 2000.0f) / 32.0f)), int32_t(std::floor(at.z / 32.0f)), 0, 6 });
			for (const auto &c : corners)
				wf.blades.insert(wf.blades.end(), { c[0], bed, c[1], 9.0f, surface, 5.0f, 0.3f, 0.0f });
			wf.cells.push_back({ int32_t(std::floor(at.x / 32.0f)), int32_t(std::floor(at.z / 32.0f)), 6, 6 });
			return wf;
		};
		uint32_t drawn_corners = 0;
		float fog[5] = {}; // (the game's fog: its colour, where it starts and ends; none while it ends at 0)
		bool bare = false;
		auto frame = [&](bool on, bool mirror = false, float ripples = 1.0f, const float *ring = nullptr) {
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.grass = false;
			l2.ao_strength = 0.0f;
			l2.relief = 0.0f;
			l2.sun_shadow = 0.0f;
			l2.water = on;
			l2.water_debug = false;
			l2.glint = 1.0f;
			l2.time = 3.0f;
			l2.water_ripples = ripples;
			l2.ripples = ring;
			l2.ripple_count = ring != nullptr ? 1 : 0;
			if (mirror)
			{
				l2.water_tint = 0.0f; // (the depth's darkening off and the reflections strong: the edge's fade alone shows)
				l2.water_reflect = 2.0f;
				l2.glint = 0.0f;
				l2.sky[0] = l2.sky[1] = l2.sky[2] = 1.0f; // (a white sky: the ground in the test is the clear's colour, as the sky would be)
			}
			l2.haze = fog[4] > 0.0f;
			std::memcpy(l2.haze_color, fog, sizeof(l2.haze_color));
			l2.haze_start = fog[3];
			l2.haze_end = fog[4];
			if (bare)
			{
				l2.water_tint = 0.0f; // (nothing but what's seen through it: no murk, reflections or glints)
				l2.water_reflect = 0.0f;
				l2.glint = 0.0f;
			}
			const draw_stats st = r.draw(g.list, t, cam, l2, error);
			drawn_corners = st.water_corners;
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			return g.read(world, 4);
		};
		auto compare = [&](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, double &lum_a, double &lum_b) {
			uint32_t changed = 0;
			lum_a = lum_b = 0.0;
			for (size_t i = 0; i < size_t(W) * H; ++i)
			{
				const uint8_t *p = &a[i * 4], *q = &b[i * 4];
				if (std::abs(p[0] - q[0]) + std::abs(p[1] - q[1]) + std::abs(p[2] - q[2]) > 6)
				{
					++changed;
					lum_a += 0.3 * p[0] + 0.59 * p[1] + 0.11 * p[2];
					lum_b += 0.3 * q[0] + 0.59 * q[1] + 0.11 * q[2];
				}
			}
			if (changed != 0)
			{
				lum_a /= changed;
				lum_b /= changed;
			}
			return changed;
		};
		std::string why;
		const bool up_over = r.upload_water(pool(at.y, at.y - 0.5f), why);
		const std::vector<uint8_t> off = frame(false);
		const std::vector<uint8_t> over = frame(true);
		const uint32_t over_corners = drawn_corners;
		const bool up_under = r.upload_water(pool(at.y + 1.5f, at.y + 1.0f), why);
		const std::vector<uint8_t> under = frame(true);
		r.upload_water(pool(at.y, at.y - 0.5f), why);
		const std::vector<uint8_t> over_mirror = frame(true, true);
		const bool up_film = r.upload_water(pool(at.y, at.y - 0.02f), why);
		const std::vector<uint8_t> film = frame(true, true);
		r.unload_water();
		double lum_off = 0.0, lum_over = 0.0, lum_ignored = 0.0;
		const uint32_t changed_over = compare(off, over, lum_off, lum_over);
		const uint32_t changed_under = compare(off, under, lum_ignored, lum_ignored);
		auto total = [&](const std::vector<uint8_t> &a) {
			double sum = 0.0;
			for (size_t i = 0; i < size_t(W) * H * 4; ++i)
				sum += (i & 3) == 3 ? 0.0 : std::abs(int(a[i]) - int(off[i]));
			return sum;
		};
		const double total_over = total(over_mirror), total_film = total(film);
		std::printf("  water: half a yalm deep changes %u pixels (brightness %.0f -> %.0f, %u corners); under the ground %u; a film %.0f%% as much as half a yalm (reflections only, %.2f a pixel)\n",
			changed_over, lum_off, lum_over, over_corners, changed_under, total_over > 0.0 ? 100.0 * total_film / total_over : 0.0, total_over / (double(W) * H));
		check(total_over > 0.02 * double(W) * H, "half a yalm of water mirrors the sky guessed when none is on screen");
		check(up_film && total_film < 0.25 * total_over, "a film of water (its line around a rock) changes the world far less than half a yalm of it");

		// Rings from someone moving through the water: half a second after one starts at the patch's middle (still
		// water otherwise, so only the ring changes it), the water around there changes; a ring started on water
		// two yalms higher (a pool above) leaves this water as it was
		const float ring_here[8] = { at.x, at.y - 0.5f, at.z, 1.0f, 0.5f, 0.4f, 0.0f, 0.0f };
		const float ring_above[8] = { at.x, at.y - 2.5f, at.z, 1.0f, 0.5f, 0.4f, 0.0f, 0.0f };
		r.upload_water(pool(at.y, at.y - 0.5f), why);
		const std::vector<uint8_t> still = frame(true, false, 0.0f), ringed = frame(true, false, 0.0f, ring_here),
			ringed_above = frame(true, false, 0.0f, ring_above);
		r.unload_water();
		double ignored = 0.0;
		const uint32_t ring_changed = compare(still, ringed, ignored, ignored), above_changed = compare(still, ringed_above, ignored, ignored);
		std::printf("  water: a ring changes %u pixels, one on water above %u\n", ring_changed, above_changed);
		if (FILE *f = nullptr; fopen_s(&f, "build\\water_test.ppm", "wb") == 0 && f != nullptr)
		{
			// (the ringed frame, to look at)
			std::fprintf(f, "P6\n%u %u\n255\n", W, H);
			for (size_t i = 0; i < size_t(W) * H; ++i)
				std::fwrite(&ringed[i * 4], 1, 3, f);
			std::fclose(f);
		}
		check(ring_changed > 300, "a ring from someone moving through the water shows on it");
		check(above_changed == 0, "a ring on water at another height leaves this water as it was");

		// The game's fog: the pool past where it ends (a yalm off: the nearest water on screen is nearly two), in the
		// colour the game drew the ground around it (as its fog would have left it): the water comes out as the fog, as
		// the ground does, reflections and all. In a blue fog, a far pool mirroring a white sky goes blue (its
		// reflections into the fog), and what's seen through it stays the game's own colour (fogged already: not twice)
		{
			r.upload_water(pool(at.y, at.y - 0.5f), why);
			const float lost_in[5] = { plain[0], plain[1], plain[2], 0.0f, 1.0f }, blue[5] = { 0.2f, 0.4f, 0.9f, 0.0f, 1.0f };
			std::memcpy(fog, lost_in, sizeof(fog));
			const std::vector<uint8_t> lost = frame(true), lost_mirror = frame(true, true);
			std::memcpy(fog, blue, sizeof(fog));
			const std::vector<uint8_t> blue_mirror = frame(true, true);
			bare = true;
			const std::vector<uint8_t> blue_through = frame(true, false, 0.0f);
			bare = false;
			std::memset(fog, 0, sizeof(fog));
			r.unload_water();
			double ignored_a = 0.0, ignored_b = 0.0;
			const uint32_t lost_changed = compare(off, lost, ignored_a, ignored_b), lost_mirror_changed = compare(off, lost_mirror, ignored_a, ignored_b);
			// (over the pool: where the white sky's mirror changed the world)
			double blue_minus_red = 0.0, white_blue_minus_red = 0.0;
			uint32_t pool_px = 0;
			for (size_t i = 0; i < size_t(W) * H; ++i)
			{
				const uint8_t *a = &off[i * 4], *m = &over_mirror[i * 4], *bm = &blue_mirror[i * 4];
				if (std::abs(a[0] - m[0]) + std::abs(a[1] - m[1]) + std::abs(a[2] - m[2]) <= 6)
					continue;
				pool_px++;
				blue_minus_red += double(bm[2]) - bm[0];
				white_blue_minus_red += double(m[2]) - m[0];
			}
			if (pool_px != 0)
			{
				blue_minus_red /= pool_px;
				white_blue_minus_red /= pool_px;
			}
			// (with no reflections, murk or glints, a still pool shows only the game's colour under it: fogged already)
			const uint32_t through_changed = compare(off, blue_through, ignored_a, ignored_b);
			std::printf("  water past the fog's end: %u pixels changed (%u with strong reflections) in fog the ground's colour; in a blue fog the "
				"mirror goes blue (blue - red %.0f, %.0f under a clear white sky) over %u pixels; seen only through, %u changed\n",
				lost_changed, lost_mirror_changed, blue_minus_red, white_blue_minus_red, pool_px, through_changed);
			check(lost_changed < 50 && lost_mirror_changed < 50, "water past the fog's end comes out as the fog, as the ground around it");
			check(pool_px > 1000 && blue_minus_red > white_blue_minus_red + 25.0, "a far pool's reflections fade into the fog's colour");
			check(through_changed < 50, "what's seen through the water isn't fogged twice (the game's own colour, fogged already)");
		}
		check(up_over && up_under && over_corners == 6, "the water patch loaded and drawn");
		check(changed_over > 2000, "water over the ground changes the world there");
		check(lum_over < lum_off, "water half a yalm deep is darker than the ground under it");
		check(changed_under == 0, "water whose surface is under what's drawn (a bank) leaves it as drawn");
		check(g.report_messages() == 0, "no debug-layer errors (water)");
	}

	// 1m2. The sea (the game's own sea sheets past the zone's water, read_sea_file / upload_sea): water half a yalm deep,
	// the camera 2 yalms over it looking out along it to the horizon. Baked water: a patch 16 yalms square 12 to 28 yalms
	// ahead, of 1-yalm quads, its edge distance 0 at its border (a straight edge, as where the collision ends) growing to 8
	// inside; and one 30 x 60 yalms 262 to 292 yalms ahead (where the water fades out without a sea: looked at from 150
	// yalms up). The sea: one sheet 4,000 yalms square at the same level, under both. Expected: the sea drawn where the
	// baked water isn't, out past 300 yalms; the near patch's inside exactly as without the sea (never under it or over it);
	// across its border the same look on both sides (its fading edge over the sea, not over the ground); the far patch drawn
	// whole, as the sea around it (no fade where the sea goes on); everywhere the same picture as the sea alone (no baked
	// water: one surface, no seam); WaterSea off (look::sea) or no sea loaded: as without it
	{
		const float plain[4] = { 0.45f, 0.42f, 0.36f, 0.5f };
		const float level = at.y - 0.5f; // (the water's surface: y is down)
		const vec3 low_eye = { at.x, at.y - 2.5f, at.z }, high_eye = { at.x, level - 150.0f, at.z };
		camera low_cam, high_cam;
		look_at(low_eye, { low_eye.x + 100.0f, low_eye.y + 2.0f, low_eye.z }, { 0.0f, -1.0f, 0.0f }, low_cam.view);
		look_at(high_eye, { at.x + 277.0f, level, at.z }, { 0.0f, -1.0f, 0.0f }, high_cam.view);
		std::memcpy(low_cam.proj, cam.proj, sizeof(low_cam.proj));
		std::memcpy(high_cam.proj, cam.proj, sizeof(high_cam.proj));
		// (a patch of 1-yalm quads from x0, z0, nx by nz, each corner's edge distance its distance to the border, at most 8)
		auto add_patch = [&](blade_file &wf, float x0, float z0, int nx, int nz) {
			const uint32_t first = wf.blade_count();
			auto edge = [&](float x, float z) { return std::min({ x - x0, x0 + nx - x, z - z0, z0 + nz - z, 8.0f }); };
			for (int j = 0; j < nz; ++j)
				for (int i = 0; i < nx; ++i)
				{
					const float xa = x0 + i, xb = xa + 1.0f, za = z0 + j, zb = za + 1.0f;
					const float q[6][2] = { { xa, za }, { xb, za }, { xb, zb }, { xa, za }, { xb, zb }, { xa, zb } };
					for (const auto &c : q)
						wf.blades.insert(wf.blades.end(), { c[0], at.y, c[1], 9.0f, level, edge(c[0], c[1]), 0.0f, 0.0f });
				}
			wf.cells.push_back({ int32_t(std::floor(x0 / 32.0f)), int32_t(std::floor(z0 / 32.0f)), first, wf.blade_count() - first });
		};
		blade_file baked;
		baked.zone = 100;
		baked.cell_size = 32.0f;
		baked.stride = 8;
		add_patch(baked, at.x + 12.0f, at.z - 8.0f, 16, 16);
		add_patch(baked, at.x + 262.0f, at.z - 30.0f, 30, 60);
		// (the sea file as HiFi XI's bake writes it: SEA1, the zone, its sheets, its corners)
		const std::wstring sea_path = L"build\\test\\sea_test.bin";
		CreateDirectoryW(L"build\\test", nullptr);
		if (FILE *f = nullptr; _wfopen_s(&f, sea_path.c_str(), L"wb") == 0 && f != nullptr)
		{
			const uint32_t head[4] = { 0x31414553 /* 'SEA1' */, 100, 1, 6 };
			fwrite(head, sizeof(head), 1, f);
			const float x0 = at.x - 2000.0f, x1 = at.x + 2000.0f, z0 = at.z - 2000.0f, z1 = at.z + 2000.0f;
			const float q[6][2] = { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z0 }, { x1, z1 }, { x0, z1 } };
			for (const auto &c : q)
			{
				const float rec[8] = { c[0], level + 30.0f, c[1], 200.0f, level, 8.0f, 0.0f, 0.0f };
				fwrite(rec, sizeof(rec), 1, f);
			}
			fclose(f);
		}
		blade_file sea, none;
		uint32_t sheets = 0, none_sheets = 0;
		std::string sea_why, none_why;
		const bool read_sea = read_sea_file(sea_path.c_str(), sea, sheets, sea_why);
		const bool read_none = read_sea_file(L"build\\test\\no_such_sea.bin", none, none_sheets, none_why);
		check(read_sea && sheets == 1 && sea.blade_count() == 6 && sea.stride == 8 && sea.blades[3] == 200.0f && !read_none && none_why == "no file for this zone",
			"the sea file read (SEA1: one sheet, six corners), none for a zone without one");
		draw_stats st = {};
		auto frame = [&](const camera &c, bool sea_on) {
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(c);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.grass = false;
			l2.ao_strength = l2.relief = 0.0f;
			l2.sun_shadow = 0.0f;
			l2.fireflies = 0.0f;
			l2.water_debug = false;
			l2.glint = 1.0f;
			l2.time = 3.0f;
			l2.sea = sea_on;
			l2.sea_distance = 1500.0f;
			st = r.draw(g.list, t, c, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			return g.read(world, 4);
		};
		// (where a pixel looks at the water: its world x, z (from the patches' middle line) and how far along it; false above
		// the horizon)
		auto on_water = [&](const camera &c, const vec3 &e, UINT x, UINT y, float &dx, float &dz, float &along) {
			const float nx = (x + 0.5f) / W * 2.0f - 1.0f, ny = 1.0f - (y + 0.5f) / H * 2.0f;
			const float vx = nx / c.proj[0], vy = ny / c.proj[5], vz = c.proj[11];
			const float *m = c.view;
			const vec3 d = { m[0] * vx + m[1] * vy + m[2] * vz, m[4] * vx + m[5] * vy + m[6] * vz, m[8] * vx + m[9] * vy + m[10] * vz };
			if (d.y <= 1e-5f)
				return false;
			const float s = (level - e.y) / d.y;
			dx = e.x + d.x * s - at.x;
			dz = e.z + d.z * s - at.z;
			along = std::sqrt((dx + at.x - e.x) * (dx + at.x - e.x) + (dz + at.z - e.z) * (dz + at.z - e.z));
			return true;
		};
		auto differs = [](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, size_t i) {
			return std::abs(a[i] - b[i]) + std::abs(a[i + 1] - b[i + 1]) + std::abs(a[i + 2] - b[i + 2]);
		};
		std::string up_why;
		r.unload_water();
		r.unload_sea();
		const std::vector<uint8_t> off = frame(low_cam, true), off_high = frame(high_cam, true);
		const bool up_baked = r.upload_water(baked, up_why);
		const std::vector<uint8_t> patches = frame(low_cam, true), patches_high = frame(high_cam, true);
		const draw_stats st_patches = st;
		const bool up_sea = r.upload_sea(sea, up_why);
		const std::vector<uint8_t> with_sea = frame(low_cam, true);
		const draw_stats st_sea = st;
		const std::vector<uint8_t> with_sea_high = frame(high_cam, true);
		const std::vector<uint8_t> sea_off = frame(low_cam, false);
		const draw_stats st_sea_off = st;
		r.unload_water();
		const std::vector<uint8_t> sea_alone = frame(low_cam, true), sea_alone_high = frame(high_cam, true);
		// (the near patch drawn twice over itself, as where the collision overlaps itself, and flowing (its ripples carried
		// downstream, unlike the sea's still ones): with the sea as without it, marked twice and still marked; the sea a
		// hundredth of a yalm over it here, so it's the stencil alone that keeps it off)
		blade_file sea_above = sea, twice;
		for (size_t k = 0; k < sea_above.blades.size(); k += 8)
			sea_above.blades[k + 4] -= 0.01f;
		r.upload_sea(sea_above, up_why);
		twice.zone = 100;
		twice.cell_size = 32.0f;
		twice.stride = 8;
		add_patch(twice, at.x + 12.0f, at.z - 8.0f, 16, 16);
		add_patch(twice, at.x + 12.0f, at.z - 8.0f, 16, 16);
		for (size_t k = 0; k < twice.blades.size(); k += 8)
			twice.blades[k + 6] = 0.8f;
		const bool up_twice = r.upload_water(twice, up_why);
		const std::vector<uint8_t> twice_sea = frame(low_cam, true);
		r.unload_sea();
		const std::vector<uint8_t> twice_plain = frame(low_cam, true);
		r.unload_water();
		r.release_old(0);
		uint32_t n_sea = 0, sea_drawn = 0, n_inside = 0, inside_changed = 0, n_far = 0, far_drawn = 0, far_without = 0, sea_off_changed = 0;
		uint32_t twice_changed = 0, flow_shows = 0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const size_t i = (size_t(y) * W + x) * 4;
				sea_off_changed += differs(sea_off, patches, i) != 0 ? 1 : 0;
				float dx = 0.0f, dz = 0.0f, along = 0.0f;
				if (!on_water(low_cam, low_eye, x, y, dx, dz, along))
					continue;
				const bool inside = dx > 14.0f && dx < 26.0f && std::fabs(dz) < 6.0f;
				const bool near_patch = dx > 11.0f && dx < 29.0f && std::fabs(dz) < 9.0f;
				if (inside)
				{
					n_inside++;
					inside_changed += differs(with_sea, patches, i) != 0 ? 1 : 0;
					twice_changed += differs(twice_sea, twice_plain, i) != 0 ? 1 : 0;
					flow_shows += differs(twice_plain, patches, i) > 6 ? 1 : 0;
				}
				else if (!near_patch && along < 250.0f)
				{
					n_sea++;
					sea_drawn += differs(with_sea, off, i) > 6 ? 1 : 0;
				}
			}
		// (from 150 yalms up: the water from 320 to 1,200 yalms off (along the water from 2 yalms up that far is a row or two at
		// the horizon, all the sky mirrored); the far patch's pixels against the sea alone there, with the sea and without it)
		double far_patch_vs_sea = 0.0, far_faded_vs_sea = 0.0;
		uint32_t n_far_patch = 0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				float dx = 0.0f, dz = 0.0f, along = 0.0f;
				if (!on_water(high_cam, high_eye, x, y, dx, dz, along))
					continue;
				const size_t i = (size_t(y) * W + x) * 4;
				if (along > 320.0f && along < 1200.0f)
				{
					n_far++;
					far_drawn += differs(with_sea_high, off_high, i) > 6 ? 1 : 0;
					far_without += differs(patches_high, off_high, i) != 0 ? 1 : 0;
				}
				if (!(dx > 264.0f && dx < 290.0f && std::fabs(dz) < 28.0f))
					continue;
				n_far_patch++;
				far_patch_vs_sea += differs(with_sea_high, sea_alone_high, i);
				far_faded_vs_sea += differs(patches_high, sea_alone_high, i);
			}
		far_patch_vs_sea /= std::max(1u, n_far_patch);
		far_faded_vs_sea /= std::max(1u, n_far_patch);
		// (the seam: the near patch's border, a yalm and a half either side of it, against the sea alone (no baked water) there,
		// with the sea and without it: its fading edge blended over the sea comes out as the sea, over the ground as half ground)
		double seam = 0.0, seam_plain = 0.0;
		uint32_t seam_n = 0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				float dx = 0.0f, dz = 0.0f, along = 0.0f;
				if (!on_water(low_cam, low_eye, x, y, dx, dz, along))
					continue;
				const float out_x = std::max(12.0f - dx, dx - 28.0f), out_z = std::fabs(dz) - 8.0f, border = std::max(out_x, out_z);
				if (std::fabs(border) > 1.5f)
					continue;
				const size_t i = (size_t(y) * W + x) * 4;
				seam += differs(with_sea, sea_alone, i);
				seam_plain += differs(patches, sea_alone, i);
				seam_n++;
			}
		seam /= std::max(1u, seam_n);
		seam_plain /= std::max(1u, seam_n);
		// (and anywhere on the water: with the sea it's the same as the sea alone, patches or not)
		double all_vs_alone = 0.0;
		uint32_t all_n = 0;
		for (size_t i = 0; i < size_t(W) * H * 4; i += 4)
			if (differs(sea_alone, off, i) > 6)
			{
				all_vs_alone += differs(with_sea, sea_alone, i);
				all_n++;
			}
		all_vs_alone /= std::max(1u, all_n);
		std::printf("  sea: %u of %u pixels of open water within 250 yalms drawn, %u of %u from 320 to 1200 (%u without it); inside the near patch %u of %u "
			"changed (drawn twice over itself, flowing: %u changed, the flow showing in %u); its border %.1f off the sea alone (%.1f without the sea, %u "
			"pixels); all the water %.2f off the sea alone (%u pixels); the far patch %.1f off the sea alone (%.1f without the sea, %u pixels); WaterSea "
			"off %u pixels changed; corners: sea %u, water %u (without the sea %u; WaterSea off: sea %u)\n", sea_drawn, n_sea, far_drawn, n_far,
			far_without, inside_changed, n_inside, twice_changed, flow_shows, seam, seam_plain, seam_n, all_vs_alone, all_n,
			far_patch_vs_sea, far_faded_vs_sea, n_far_patch, sea_off_changed, st_sea.sea_corners, st_sea.water_corners, st_patches.water_corners,
			st_sea_off.sea_corners);
		// (to look at: the baked water alone, with the sea, the sea alone; along the water (top) and from 150 yalms up)
		if (FILE *f = nullptr; fopen_s(&f, "build\\sea_test.ppm", "wb") == 0 && f != nullptr)
		{
			std::fprintf(f, "P6 %u %u 255\n", W * 3, H * 2);
			const std::vector<uint8_t> *const shown[2][3] = { { &patches, &with_sea, &sea_alone }, { &patches_high, &with_sea_high, &sea_alone_high } };
			for (UINT y = 0; y < H * 2; ++y)
				for (UINT x = 0; x < W * 3; ++x)
					std::fwrite(&(*shown[y / H][x / W])[(size_t(y % H) * W + x % W) * 4], 1, 3, f);
			fclose(f);
		}
		check(up_baked && up_sea && st_sea.sea_corners == 6 && st_patches.sea_corners == 0 && st_sea_off.sea_corners == 0,
			"the sea loaded and drawn (not with none loaded, nor with WaterSea off)");
		check(n_sea > 5000 && sea_drawn > n_sea * 95 / 100, "the sea drawn where the baked water isn't");
		check(n_inside > 500 && inside_changed == 0, "the baked water's inside exactly as without the sea (the sea never under it or over it)");
		check(up_twice && flow_shows > n_inside / 2 && twice_changed == 0, "baked water drawn twice over itself (flowing): with the sea as without it (still marked)");
		check(seam_n > 200 && seam < 3.0 && seam_plain > 5.0 * seam, "the same look across the baked water's border (its fading edge over the sea)");
		check(all_n > 5000 && all_vs_alone < 1.0, "the water with the sea the same as the sea alone, patches or not (no seam anywhere)");
		check(n_far > 2000 && far_drawn > n_far * 9 / 10 && far_without == 0, "the sea drawn past 300 yalms (nothing there without it)");
		check(n_far_patch > 300 && far_patch_vs_sea < 0.5 * far_faded_vs_sea, "baked water 262 to 292 yalms off drawn whole as the sea around it (not faded out)");
		check(sea_off_changed == 0, "WaterSea off: as without a sea");
		check(g.report_messages() == 0, "no debug-layer errors (sea)");
	}

	// 1k. GPU time: after a few frames the timer reports the shading pass's time (here on WARP, the software
	// device: only the ratio between effects means anything)
	{
		UINT64 ticks = 0;
		g.queue->GetTimestampFrequency(&ticks);
		r.set_timestamp_frequency(ticks);
		auto timed = [&](float ao, float sun) {
			for (int i = 0; i < 12; ++i)
			{
				const float plain[4] = { 0.45f, 0.42f, 0.36f, 0.5f };
				barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
				g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
				ground_depth(cam);
				barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				look l2 = l;
				l2.grass = false;
				l2.ao_strength = ao;
				l2.relief = 0.0f;
				l2.sun_shadow = sun;
				r.draw(g.list, t, cam, l2, error);
				barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
				g.run();
			}
			return r.shading_ms();
		};
		const float ao_ms = timed(0.8f, 0.0f), sun_ms = timed(0.0f, 0.6f);
		std::printf("  GPU time (WARP, 640 x 360): AO alone %.2f ms, sun shadows alone %.2f ms (smoothed)\n", ao_ms, sun_ms);
		check(ticks != 0 && ao_ms > 0.0f && sun_ms > 0.0f, "the timer reports the shading pass's GPU time");
		check(g.report_messages() == 0, "no debug-layer errors (timing)");
	}

	// 1b. The colour memory: blades seen on light ground, then a frame on dark ground (darkened that frame:
	// 1n), are as light as before back on light ground (a frame's sample moves the colour only a little), and
	// dark at once when the memory is forgotten
	{
		auto blade_light = [&](float r8, float g8, float b8, bool forget) {
			if (forget)
				fresh_memory();
			const float ground[4] = { r8 / 255.0f, g8 / 255.0f, b8 / 255.0f, 0.5f };
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, ground, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			r.draw(g.list, t, cam, l, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			const std::vector<uint8_t> pix = g.read(world, 4);
			double sum = 0.0;
			uint32_t n = 0;
			// (around the looked-at blade: the test's ground is flat at its height, the terrain further off isn't)
			for (size_t i = 0; i < size_t(W) * H; ++i)
			{
				const float x = float(i % W), y = float(i / W);
				if (std::fabs(x - px) > 60.0f || y < py - 60.0f || y > py + 20.0f)
					continue;
				if (std::abs(pix[i * 4] - r8) > 3 || std::abs(pix[i * 4 + 1] - g8) > 3 || std::abs(pix[i * 4 + 2] - b8) > 3)
				{
					sum += 0.3 * pix[i * 4] + 0.59 * pix[i * 4 + 1] + 0.11 * pix[i * 4 + 2];
					n++;
				}
			}
			return n != 0 ? sum / n : 0.0;
		};
		const double light = blade_light(150, 160, 90, true);
		blade_light(25, 30, 15, false);
		const double still = blade_light(150, 160, 90, false);
		const double dark = blade_light(25, 30, 15, true);
		std::printf("  blade brightness: %.0f on light ground, %.0f back on it after a frame on dark, %.0f on dark forgotten\n", light, still, dark);
		check(still > 0.95 * light && dark < 0.5 * light, "blades remember their ground's colour across frames");
		check(g.report_messages() == 0, "no debug-layer errors (colour memory)");
	}

	// 1n. A shadow passing over (someone's own, drawn by the game on the ground under the grass): blades
	// learned on light ground are dark from the frame after the ground goes dark, without learning it, so
	// they're as light as before as soon as it's gone
	{
		auto blades = [&](float r8, float g8, float b8, int frames) {
			const float ground[4] = { r8 / 255.0f, g8 / 255.0f, b8 / 255.0f, 0.5f };
			double sum = 0.0;
			for (int f = 0; f < frames; ++f)
			{
				barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
				g.list->ClearRenderTargetView(world_rtv, ground, 0, nullptr);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
				ground_depth(cam);
				barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				r.draw(g.list, t, cam, l, error);
				barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			}
			const std::vector<uint8_t> pix = g.read(world, 4);
			uint32_t n = 0;
			for (size_t i = 0; i < size_t(W) * H; ++i)
			{
				const float x = float(i % W), y = float(i / W);
				if (std::fabs(x - px) > 60.0f || y < py - 60.0f || y > py + 20.0f)
					continue;
				if (std::abs(pix[i * 4] - r8) > 3 || std::abs(pix[i * 4 + 1] - g8) > 3 || std::abs(pix[i * 4 + 2] - b8) > 3)
				{
					sum += 0.3 * pix[i * 4] + 0.59 * pix[i * 4 + 1] + 0.11 * pix[i * 4 + 2];
					n++;
				}
			}
			return n != 0 ? sum / n : 0.0;
		};
		fresh_memory();
		const double lit = blades(150, 160, 90, 3);
		const double shadowed = blades(75, 80, 45, 2);
		const double after = blades(150, 160, 90, 2);
		std::printf("  a shadow over the grass: blades %.0f on light ground, %.0f with the ground half as bright, %.0f when it's gone\n", lit, shadowed, after);
		check(shadowed < 0.7 * lit, "a shadow passing over darkens the grass at once");
		check(std::fabs(after - lit) < 0.05 * lit, "and isn't learned: gone with it");
		check(g.report_messages() == 0, "no debug-layer errors (shadow over the grass)");
	}

lamps:
	// 1p. Lamps (grass off, the shading pass on for them alone): a San d'Oria street lamp (its colour, range 4,
	// brightness 6, the glow at its default 0.5 and reach 1.5 times the range, as the add-on works them out) 3 yalms
	// over the looked-at point on dark flat ground. The ground at its foot gets brighter, and warmer (red more than
	// blue); ground 3 yalms off less so; ground 10 yalms off (past its reach) and the picture with the lamp at no
	// strength (the add-on's brightness 0 sends none) as they went in. Lamp shadows: a low lamp behind a wall standing
	// at the looked-at point (in the depth buffer as the game's walls are), the ground in front of the wall: darker
	// with lamp shadows on than off; the ground well beside the wall, whose way to the lamp is clear, the same.
	{
		const float night_ground[4] = { 0.12f, 0.11f, 0.1f, 0.5f };
		D3D12_RECT lamp_wall = {};
		float wall_depth = 0.0f;
		std::vector<std::pair<D3D12_RECT, float>> cards; // (more things standing in the depth buffer, drawn in order)
		const float *who = nullptr;                      // (characters and monsters, as the Ashita addon sends them)
		uint32_t who_count = 0;
		draw_stats lamp_st;                              // (the last frame's)
		auto frame = [&](const float *lamps, uint32_t count, float shadows) {
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, night_ground, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			if (lamp_wall.right != 0)
			{
				g.list->ClearRenderTargetView(world_rtv, night_ground, 1, &lamp_wall);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, wall_depth, 0, 1, &lamp_wall);
			}
			for (const auto &c : cards)
			{
				g.list->ClearRenderTargetView(world_rtv, night_ground, 1, &c.first);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, c.second, 0, 1, &c.first);
			}
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.grass = false;
			l2.ao_strength = 0.0f;
			l2.relief = 0.0f;
			l2.sun_shadow = 0.0f;
			l2.fireflies = 0.0f;
			l2.lamps = lamps;
			l2.lamp_count = count;
			l2.lamp_shadows = shadows;
			l2.actors = who;
			l2.actor_count = who_count;
			const draw_stats st = r.draw(g.list, t, cam, l2, error);
			lamp_st = st;
			check(st.shaded == (count != 0) && st.lamps == count, "the shading pass ran for the lamps (and only with some)");
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			return g.read(world, 4);
		};
		auto on_screen = [&](const vec3 &p, float *depth) {
			const float x = p.x * vp[0] + p.y * vp[4] + p.z * vp[8] + vp[12], y = p.x * vp[1] + p.y * vp[5] + p.z * vp[9] + vp[13];
			const float z = p.x * vp[2] + p.y * vp[6] + p.z * vp[10] + vp[14], w = p.x * vp[3] + p.y * vp[7] + p.z * vp[11] + vp[15];
			if (depth != nullptr)
				*depth = z / w;
			return std::make_pair((x / w + 1.0f) * 0.5f * W, (1.0f - y / w) * 0.5f * H);
		};
		// (mean red, green, blue and brightness over a box of 9 x 9 pixels around a point on screen)
		struct colour { double r, g, b, luma; };
		auto around = [&](const std::vector<uint8_t> &img, std::pair<float, float> at_px) {
			colour c = {};
			uint32_t n = 0;
			for (LONG y = LONG(at_px.second) - 4; y <= LONG(at_px.second) + 4; ++y)
				for (LONG x = LONG(at_px.first) - 4; x <= LONG(at_px.first) + 4; ++x)
					if (x >= 0 && y >= 0 && x < LONG(W) && y < LONG(H))
					{
						const uint8_t *p = &img[(size_t(y) * W + x) * 4];
						c.r += p[0], c.g += p[1], c.b += p[2];
						n++;
					}
			if (n != 0)
				c.r /= n, c.g /= n, c.b /= n;
			c.luma = 0.3 * c.r + 0.59 * c.g + 0.11 * c.b;
			return c;
		};
		// (the ground's horizontal way toward the camera, and across it)
		const vec3 toward = normalize({ eye.x - at.x, 0.0f, eye.z - at.z }), across = { toward.z, 0.0f, -toward.x };
		const float strength = 0.5f * (1.0f - std::exp(-6.0f / 4.0f));
		const float lamp[8] = { at.x, at.y - 3.0f, at.z, 4.0f * 1.5f, 1.469f * strength, 1.0f * strength, 0.219f * strength, 1.0f };
		const float dark_lamp[8] = { at.x, at.y - 3.0f, at.z, 4.0f * 1.5f, 0.0f, 0.0f, 0.0f, 1.0f };
		const auto foot = on_screen(at, nullptr);
		const auto three_off = on_screen({ at.x + across.x * 3.0f, at.y, at.z + across.z * 3.0f }, nullptr);
		const auto ten_off = on_screen({ at.x - toward.x * 10.0f, at.y, at.z - toward.z * 10.0f }, nullptr);
		const std::vector<uint8_t> unlit = frame(nullptr, 0, 0.0f), lit = frame(lamp, 1, 0.0f), dark = frame(dark_lamp, 1, 0.0f);
		const colour foot_unlit = around(unlit, foot), foot_lit = around(lit, foot);
		const colour three_unlit = around(unlit, three_off), three_lit = around(lit, three_off);
		const colour ten_unlit = around(unlit, ten_off), ten_lit = around(lit, ten_off);
		int dark_diff = 0;
		for (size_t i = 0; i < size_t(W) * H * 4; ++i)
			dark_diff = std::max(dark_diff, std::abs(int(dark[i]) - int(unlit[i])));
		std::printf("  a lamp 3 yalms over dark ground: at its foot %.1f -> %.1f (red +%.1f, blue +%.1f); 3 yalms off %.1f -> %.1f; 10 yalms off %.1f -> %.1f; at no strength, %d at most off\n",
			foot_unlit.luma, foot_lit.luma, foot_lit.r - foot_unlit.r, foot_lit.b - foot_unlit.b, three_unlit.luma, three_lit.luma, ten_unlit.luma, ten_lit.luma, dark_diff);
		check(foot_lit.luma > foot_unlit.luma * 1.3 + 4.0, "the ground under a lit lamp gets brighter");
		check(foot_lit.r - foot_unlit.r > 2.0 * (foot_lit.b - foot_unlit.b), "and warmer (a street lamp's colour)");
		check(three_lit.luma > three_unlit.luma + 1.0 && three_lit.luma < foot_lit.luma - 2.0, "less so 3 yalms off (within its reach)");
		check(std::fabs(ten_lit.luma - ten_unlit.luma) < 0.5, "ground past its reach as it went in");
		check(dark_diff <= 1, "a lamp at no strength (the game's brightness 0: none sent) changes nothing");
		{
			FILE *f = nullptr;
			if (fopen_s(&f, "build\\lamp_test.ppm", "wb") == 0 && f != nullptr)
			{
				std::fprintf(f, "P6 %u %u 255\n", W, H);
				for (size_t i = 0; i < size_t(W) * H; ++i)
					std::fwrite(&lit[i * 4], 1, 3, f);
				fclose(f);
			}
		}

		// The wall: as deep as the looked-at point, a yalm and a half tall over it on screen, about 0.6 yalms each way;
		// the lamp 2 yalms behind it, half a yalm up; the ground 1.5 yalms in front of it, and 3 yalms beside that
		const auto top = on_screen({ at.x, at.y - 1.5f, at.z }, nullptr);
		on_screen(at, &wall_depth);
		lamp_wall = { LONG(foot.first) - 25, LONG(top.second), LONG(foot.first) + 25, LONG(foot.second) + 1 };
		const float low_lamp[8] = { at.x - toward.x * 2.0f, at.y - 0.5f, at.z - toward.z * 2.0f, 4.0f * 1.5f, 1.469f * strength, 1.0f * strength, 0.219f * strength, 1.0f };
		const vec3 front = { at.x + toward.x * 1.5f, at.y, at.z + toward.z * 1.5f };
		const auto in_front = on_screen(front, nullptr);
		const auto beside = on_screen({ front.x + across.x * 3.0f, front.y, front.z + across.z * 3.0f }, nullptr);
		const std::vector<uint8_t> open = frame(low_lamp, 1, 0.0f), hidden = frame(low_lamp, 1, 1.0f), walled_unlit = frame(nullptr, 0, 0.0f);
		// (the same wall as someone standing there: a character or monster sent by the Ashita addon)
		const float body[4] = { at.x, at.y, at.z, 0.9f };
		who = body;
		who_count = 1;
		const std::vector<uint8_t> someone = frame(low_lamp, 1, 1.0f);
		who = nullptr;
		who_count = 0;
		// (the same light sent as a glow, a Home Point's or an effect's: it lights without a lamp shadow)
		float low_glow[8];
		std::memcpy(low_glow, low_lamp, sizeof(low_glow));
		low_glow[7] = 0.0f;
		const std::vector<uint8_t> glow_hidden = frame(low_glow, 1, 1.0f);
		lamp_wall = {};
		const colour front_glow = around(glow_hidden, in_front);
		const colour front_unlit = around(walled_unlit, in_front), front_open = around(open, in_front), front_hidden = around(hidden, in_front);
		const colour beside_open = around(open, beside), beside_hidden = around(hidden, beside);
		const colour front_someone = around(someone, in_front);
		std::printf("  a low lamp behind a wall: ground in front of the wall %.1f unlit, %.1f lit, %.1f with lamp shadows; beside it %.1f lit, %.1f with lamp shadows; the wall as someone standing there: %.1f\n",
			front_unlit.luma, front_open.luma, front_hidden.luma, beside_open.luma, beside_hidden.luma, front_someone.luma);
		check(front_open.luma > front_unlit.luma + 3.0, "the lamp's glow reaches the ground in front of the wall (no shadows)");
		check(front_hidden.luma < front_open.luma - 0.6 * (front_open.luma - front_unlit.luma), "with lamp shadows, the wall hides the lamp from it");
		check(beside_open.luma > front_unlit.luma + 0.5 && std::fabs(beside_hidden.luma - beside_open.luma) < 0.5, "ground whose way to the lamp is clear keeps its glow");
		check(std::fabs(front_someone.luma - front_open.luma) < 0.5, "a character or monster casts no lamp shadow (the game casts their own; the march laid a row of copies)");
		check(std::fabs(front_glow.luma - front_open.luma) < 0.5, "a glow (a Home Point's, an effect's) casts no lamp shadow: the wall leaves its light (its crystal laid a row of shadows)");

		// The lamp's shadow map from the host's zone (set_zone_casters: HiFi XI's native zone data; draw_lamp_maps): the same
		// low lamp, the wall not in the depth buffer at all (off screen, as far as the march can tell) but drawn by a
		// stand-in into the lamp's cube faces, as a plate 0.6 of the way from the light to the ground in front of the wall
		// (a yalm square across the face looking at it). The first frame only asks for room at the depth copy's end; then
		// one face a frame, the cube copied there with the sixth: the ground in front darker, as with the wall on screen, the
		// ground beside keeping its glow; a frame later nothing drawn again (kept); the host gone, marched again (nothing on
		// screen hides it: lit).
		{
			struct lamp_host
			{
				vec3 q;
				uint32_t faces = 0, hits = 0;
			} lh;
			const vec3 light = { low_lamp[0], low_lamp[1], low_lamp[2] };
			lh.q = { light.x + 0.6f * (front.x - light.x), light.y + 0.6f * (front.y - light.y), light.z + 0.6f * (front.z - light.z) };
			r.set_zone_casters(
				[](void *, float lo[3], float hi[3], uint64_t *generation) {
					for (int a = 0; a < 3; ++a)
					{
						lo[a] = -1000.0f;
						hi[a] = 1000.0f;
					}
					*generation = 7;
					return true;
				},
				[](void *context, ID3D12GraphicsCommandList *list, const float m[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size) -> uint32_t {
					lamp_host &h = *static_cast<lamp_host *>(context);
					h.faces++;
					const vec3 &q = h.q;
					const float x = q.x * m[0] + q.y * m[4] + q.z * m[8] + m[12], y = q.x * m[1] + q.y * m[5] + q.z * m[9] + m[13];
					const float z = q.x * m[2] + q.y * m[6] + q.z * m[10] + m[14], w = q.x * m[3] + q.y * m[7] + q.z * m[11] + m[15];
					if (!(w > 0.05f) || std::fabs(x) > w || std::fabs(y) > w)
						return 0;
					h.hits++;
					const float per_yalm = 0.5f * float(size) * std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]) / w;
					const float cx = (x / w * 0.5f + 0.5f) * float(size), cy = (0.5f - y / w * 0.5f) * float(size);
					const D3D12_RECT rc = { std::max(LONG(cx - 0.5f * per_yalm), 0L), std::max(LONG(cy - 0.5f * per_yalm), 0L),
						std::min(LONG(cx + 0.5f * per_yalm) + 1, LONG(size)), std::min(LONG(cy + 0.5f * per_yalm) + 1, LONG(size)) };
					list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, std::clamp(z / w, 0.0f, 1.0f), 0, 1, &rc);
					return 2;
				},
				&lh);
			frame(low_lamp, 1, 1.0f);
			const draw_stats asked = lamp_st;
			std::vector<uint8_t> mapped;
			uint32_t faces = 0, most_faces = 0, frames = 0;
			while (frames < 10 && (frames == 0 || lamp_st.lamp_maps == 0))
			{
				mapped = frame(low_lamp, 1, 1.0f);
				faces += lamp_st.lamp_map_faces;
				most_faces = std::max(most_faces, lamp_st.lamp_map_faces);
				frames++;
			}
			const draw_stats drawn = lamp_st;
			const uint32_t hits = lh.hits;
			const std::vector<uint8_t> kept = frame(low_lamp, 1, 1.0f);
			const draw_stats kept_st = lamp_st;
			r.set_zone_casters(nullptr, nullptr, nullptr);
			const std::vector<uint8_t> marched = frame(low_lamp, 1, 1.0f);
			const draw_stats marched_st = lamp_st;
			const colour front_mapped = around(mapped, in_front), front_kept = around(kept, in_front), beside_mapped = around(mapped, beside);
			const colour front_marched = around(marched, in_front);
			std::printf("  the lamp's shadow map (the wall only in it, a plate the host draws): ground in front %.1f, kept %.1f, beside %.1f; marched without it %.1f (lit %.1f, unlit %.1f); maps %u/%u/%u/%u; faces %u over %u frames (at most %u a frame), then %u (the plate in %u)\n",
				front_mapped.luma, front_kept.luma, beside_mapped.luma, front_marched.luma, front_open.luma, front_unlit.luma, asked.lamp_maps, drawn.lamp_maps,
				kept_st.lamp_maps, marched_st.lamp_maps, faces, frames, most_faces, kept_st.lamp_map_faces, hits);
			check(asked.lamp_maps == 0 && asked.lamp_map_faces == 0 && drawn.lamp_maps == 1 && faces == 6 && frames == 6 && most_faces == 1 && hits >= 1 &&
				kept_st.lamp_maps == 1 && kept_st.lamp_map_faces == 0 && marched_st.lamp_maps == 0,
				"the lamp's cube drawn a face a frame once it's asked for (six frames), then kept; none without the host");
			check(front_mapped.luma < front_open.luma - 0.6 * (front_open.luma - front_unlit.luma) && std::fabs(front_kept.luma - front_mapped.luma) < 0.5,
				"with its shadow map, what isn't on screen hides the lamp from the ground in front");
			check(std::fabs(beside_mapped.luma - beside_open.luma) < 0.5, "ground whose way to the lamp is clear keeps its glow (the map)");
			check(std::fabs(front_marched.luma - front_open.luma) < 0.5, "without the host's zone the march again (nothing on screen between: lit)");
			check(g.report_messages() == 0, "no debug-layer errors (the lamp's shadow map)");
		}

		// A lantern hanging 0.6 yalms in front of a wall facing the camera, the light inside it: the wall (as deep as a point
		// a yalm past the looked-at one) over the middle of the picture, and the lantern's front a quarter yalm in front of
		// the light, half a yalm across. With lamp shadows on, the wall around the lantern (0.4 to 0.9 yalms off the light,
		// beside it) keeps its glow as with them off: the lantern around the light doesn't hide it (marched to 85% of the
		// way, a dark disc there in a ring of glow)
		{
			const vec3 f = normalize(sub(at, eye));
			const vec3 wall_at = { at.x + f.x, at.y + f.y, at.z + f.z };
			const vec3 light = { wall_at.x - f.x * 0.6f, wall_at.y - f.y * 0.6f, wall_at.z - f.z * 0.6f };
			const vec3 lantern_front = { light.x - f.x * 0.25f, light.y - f.y * 0.25f, light.z - f.z * 0.25f };
			float wall_ndc = 0.0f, front_ndc = 0.0f;
			const auto mid = on_screen(wall_at, &wall_ndc);
			on_screen(lantern_front, &front_ndc);
			const vec3 right = { cam.view[0], cam.view[4], cam.view[8] };
			auto at_side = [&](const vec3 &p, float d) { return vec3 { p.x + right.x * d, p.y + right.y * d, p.z + right.z * d }; };
			const float lantern_px = on_screen(at_side(lantern_front, 0.25f), nullptr).first - mid.first;
			cards = { { { LONG(mid.first) - 200, LONG(mid.second) - 120, LONG(mid.first) + 200, LONG(mid.second) + 120 }, wall_ndc },
				{ { LONG(mid.first - lantern_px), LONG(mid.second - lantern_px), LONG(mid.first + lantern_px), LONG(mid.second + lantern_px) }, front_ndc } };
			const float lantern[8] = { light.x, light.y, light.z, 4.0f * 1.5f, 1.469f * strength, 1.0f * strength, 0.219f * strength, 1.0f };
			const std::vector<uint8_t> plain = frame(lantern, 1, 0.0f), shaded = frame(lantern, 1, 1.0f), dark_wall = frame(nullptr, 0, 0.0f);
			cards.clear();
			// (the wall 0.4 to 0.9 yalms beside the light, both sides, along its row)
			auto beside_lantern = [&](const std::vector<uint8_t> &img) {
				double sum = 0.0;
				uint32_t n = 0;
				for (float d = 0.4f; d <= 0.9f; d += 0.1f)
					for (float side : { -1.0f, 1.0f })
					{
						sum += around(img, on_screen(at_side(wall_at, d * side), nullptr)).luma;
						n++;
					}
				return sum / n;
			};
			const double wall_unlit = beside_lantern(dark_wall), wall_plain = beside_lantern(plain), wall_shaded = beside_lantern(shaded);
			std::printf("  a lantern 0.6 yalms in front of a wall (%.0f px across): the wall beside it %.1f unlit, %.1f lit, %.1f with lamp shadows\n",
				2.0f * lantern_px, wall_unlit, wall_plain, wall_shaded);
			check(wall_plain > wall_unlit + 5.0, "the wall behind a lantern glows");
			check(std::fabs(wall_shaded - wall_plain) < 0.5, "and keeps its glow with lamp shadows on (the lantern around the light doesn't hide it)");

			// The same lantern on its bracket: the wall over the whole picture, and behind the lantern's front a plate 1.6
			// yalms across as deep as the light (its bracket and back plate, or its glow drawn into the depth). The wall in
			// a ring 1.4 to 2.4 yalms off the light (beside, above and below it), just outside where lamp shadows start,
			// keeps its glow with them on: the steps toward the light ended behind the plate and found it, a dark ring
			// round the lantern (in game, 2026-10-21)
			float plate_ndc = 0.0f;
			on_screen(light, &plate_ndc);
			const float plate_px = on_screen(at_side(light, 0.8f), nullptr).first - mid.first;
			const vec3 above = { cam.view[1], cam.view[5], cam.view[9] };
			cards = { { { 0, 0, LONG(W), LONG(H) }, wall_ndc },
				{ { LONG(mid.first - plate_px), LONG(mid.second - plate_px), LONG(mid.first + plate_px), LONG(mid.second + plate_px) }, plate_ndc },
				{ { LONG(mid.first - lantern_px), LONG(mid.second - lantern_px), LONG(mid.first + lantern_px), LONG(mid.second + lantern_px) }, front_ndc } };
			const std::vector<uint8_t> bracket_plain = frame(lantern, 1, 0.0f), bracket_shaded = frame(lantern, 1, 1.0f), bracket_dark = frame(nullptr, 0, 0.0f);
			cards.clear();
			auto ring = [&](const std::vector<uint8_t> &img) {
				double sum = 0.0;
				uint32_t n = 0;
				for (float d = 1.4f; d <= 2.45f; d += 0.2f)
					for (float side : { -1.0f, 1.0f })
						for (const vec3 &way : { right, above })
						{
							sum += around(img, on_screen({ wall_at.x + way.x * d * side, wall_at.y + way.y * d * side, wall_at.z + way.z * d * side }, nullptr)).luma;
							n++;
						}
				return sum / n;
			};
			const double ring_unlit = ring(bracket_dark), ring_plain = ring(bracket_plain), ring_shaded = ring(bracket_shaded);
			std::printf("  the same lantern on a bracket plate (%.0f px across): the wall in a ring round it %.1f unlit, %.1f lit, %.1f with lamp shadows\n",
				2.0f * plate_px, ring_unlit, ring_plain, ring_shaded);
			check(ring_plain > ring_unlit + 3.0, "the wall round a lantern on its bracket glows");
			check(std::fabs(ring_shaded - ring_plain) < 0.5, "and keeps its glow with lamp shadows on: its bracket draws no dark ring round it");
		}

		// 1v. A Home Point crystal's glow (one of the worldfx addon's glows, as the add-on works it out: blue-cyan 0.35 0.75
		// 1, reach 7 x 1.5, brightness 6 at the fixture setting 1; lamp shadows at their default 0.5) 1.6 yalms over the
		// looked-at floor, a wall a yalm behind it (as deep as the point 1.6 yalms up there, from the top of the picture
		// down to the wall's foot): the wall beside the crystal and the floor at its foot get brighter and bluer (blue up
		// more than red); the same glow at no strength (its effect gone: the addon sends none) changes nothing.
		{
			const vec3 h = normalize({ at.x - eye.x, 0.0f, at.z - eye.z });
			const vec3 right = { cam.view[0], cam.view[4], cam.view[8] };
			const vec3 wall_foot = { at.x + h.x, at.y, at.z + h.z }, wall_mid = { wall_foot.x, wall_foot.y - 1.6f, wall_foot.z };
			float wall_ndc = 0.0f;
			const auto mid = on_screen(wall_mid, &wall_ndc);
			const auto foot_row = on_screen(wall_foot, nullptr);
			cards = { { { LONG(mid.first) - 220, 0, LONG(mid.first) + 220, LONG(foot_row.second) }, wall_ndc } };
			const float s = 1.0f * (1.0f - std::exp(-6.0f / 4.0f));
			const float crystal[8] = { at.x, at.y - 1.6f, at.z, 7.0f * 1.5f, 0.35f * s, 0.75f * s, 1.0f * s, 0.0f };
			const float gone[8] = { at.x, at.y - 1.6f, at.z, 7.0f * 1.5f, 0.0f, 0.0f, 0.0f, 0.0f };
			const std::vector<uint8_t> hp_unlit = frame(nullptr, 0, 0.5f), hp_lit = frame(crystal, 1, 0.5f), hp_dark = frame(gone, 1, 0.5f);
			cards.clear();
			auto on = [&](const vec3 &p) { return on_screen(p, nullptr); };
			const colour wall_l = around(hp_lit, on({ wall_mid.x - right.x * 0.7f, wall_mid.y, wall_mid.z - right.z * 0.7f }));
			const colour wall_r = around(hp_lit, on({ wall_mid.x + right.x * 0.7f, wall_mid.y, wall_mid.z + right.z * 0.7f }));
			const colour wall_l0 = around(hp_unlit, on({ wall_mid.x - right.x * 0.7f, wall_mid.y, wall_mid.z - right.z * 0.7f }));
			const colour wall_r0 = around(hp_unlit, on({ wall_mid.x + right.x * 0.7f, wall_mid.y, wall_mid.z + right.z * 0.7f }));
			const vec3 floor_at = { at.x - h.x * 0.5f, at.y, at.z - h.z * 0.5f };
			const colour floor_lit = around(hp_lit, on(floor_at)), floor_unlit = around(hp_unlit, on(floor_at));
			int hp_dark_diff = 0;
			for (size_t i = 0; i < size_t(W) * H * 4; ++i)
				hp_dark_diff = std::max(hp_dark_diff, std::abs(int(hp_dark[i]) - int(hp_unlit[i])));
			const double wall_rise = (wall_l.luma + wall_r.luma - wall_l0.luma - wall_r0.luma) / 2.0;
			const double wall_blue = (wall_l.b + wall_r.b - wall_l0.b - wall_r0.b) / 2.0, wall_red = (wall_l.r + wall_r.r - wall_l0.r - wall_r0.r) / 2.0;
			std::printf("  a Home Point's glow 1.6 yalms up, a wall a yalm behind: the wall beside it +%.1f (red +%.1f, blue +%.1f); the floor at its foot %.1f -> %.1f (red +%.1f, blue +%.1f); at no strength, %d at most off\n",
				wall_rise, wall_red, wall_blue, floor_unlit.luma, floor_lit.luma, floor_lit.r - floor_unlit.r, floor_lit.b - floor_unlit.b, hp_dark_diff);
			check(wall_rise > 5.0 && wall_blue > 2.0 * wall_red, "the wall beside a Home Point's glow gets brighter, and blue");
			check(floor_lit.luma > floor_unlit.luma + 5.0 && floor_lit.b - floor_unlit.b > 2.0 * (floor_lit.r - floor_unlit.r), "the floor at its foot too");
			check(hp_dark_diff <= 1, "its glow gone (none sent) changes nothing");
		}

		// 1p2. With the light split (HiFi XI's native zone and characters drew these pixels, lit by the zone's lamps
		// already: grass.hlsl lamp_light_known): the lantern 0.6 yalms in front of a wall facing the camera, the wall over
		// the whole picture and every pixel of it in the light split (its depth, facing the camera, no sun). The wall beside
		// the lantern comes out as it went in (the lamp's glow isn't added again; without the split it is); the same light
		// sent as an effect's glow still lights it; and with lamp shadows on, a pillar standing between the lantern and the
		// wall 2.6 yalms to its side darkens the wall there (the lamp's light the native pass drew there taken away), the
		// wall as far to the other side, whose way is clear, as it went in.
		{
			const vec3 f = normalize(sub(at, eye));
			const vec3 right = { cam.view[0], cam.view[4], cam.view[8] };
			auto at_side = [&](const vec3 &p, float d) { return vec3 { p.x + right.x * d, p.y + right.y * d, p.z + right.z * d }; };
			auto pushed = [&](const vec3 &p, float d) { return vec3 { p.x + f.x * d, p.y + f.y * d, p.z + f.z * d }; };
			auto view_depth = [&](const vec3 &p) { return p.x * vp[3] + p.y * vp[7] + p.z * vp[11] + vp[15]; };
			const vec3 wall_at = pushed(at, 1.0f), light = pushed(wall_at, -0.6f), pillar_at = pushed(wall_at, -0.45f);
			float wall_ndc = 0.0f, pillar_ndc = 0.0f;
			on_screen(wall_at, &wall_ndc);
			on_screen(pillar_at, &pillar_ndc);
			const LONG p0 = LONG(on_screen(at_side(pillar_at, 1.1f), nullptr).first), p1 = LONG(on_screen(at_side(pillar_at, 1.9f), nullptr).first);
			cards = { { { 0, 0, LONG(W), LONG(H) }, wall_ndc }, { { p0, 0, p1, LONG(H) }, pillar_ndc } };
			// The light split's words (light_split.hpp): each pixel the topmost card's view depth, facing the camera, no sun
			auto word = [](float depth, vec3 n) {
				const uint32_t code = uint32_t(std::clamp(std::round((std::log2(std::max(depth, 1e-3f)) + 3.0f) * 128.0f), 1.0f, 2047.0f));
				const float s = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
				n = { n.x / s, n.y / s, n.z / s };
				const float ox = n.z >= 0.0f ? n.x : (1.0f - std::fabs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
				const float oy = n.z >= 0.0f ? n.y : (1.0f - std::fabs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
				const uint32_t qx = uint32_t(std::round(std::clamp(ox * 0.5f + 0.5f, 0.0f, 1.0f) * 127.0f));
				const uint32_t qy = uint32_t(std::round(std::clamp(oy * 0.5f + 0.5f, 0.0f, 1.0f) * 127.0f));
				return code | qx << 11 | qy << 18;
			};
			const vec3 facing = { -f.x, -f.y, -f.z };
			const uint32_t wall_word = word(view_depth(wall_at), facing), pillar_word = word(view_depth(pillar_at), facing);
			ID3D12Resource *split = g.texture(DXGI_FORMAT_R32_UINT, W, H, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
			ID3D12Resource *split_up = g.buffer(D3D12_HEAP_TYPE_UPLOAD, UINT64(W) * 4 * H, D3D12_RESOURCE_STATE_GENERIC_READ);
			{
				uint32_t *words = nullptr;
				split_up->Map(0, nullptr, reinterpret_cast<void **>(&words));
				for (UINT y = 0; y < H; ++y)
					for (UINT x = 0; x < W; ++x)
						words[size_t(y) * W + x] = LONG(x) >= p0 && LONG(x) < p1 ? pillar_word : wall_word;
				split_up->Unmap(0, nullptr);
				D3D12_TEXTURE_COPY_LOCATION dst = { split, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, src = { split_up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
				dst.SubresourceIndex = 0;
				src.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, W, H, 1, W * 4 };
				g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
				barrier(g.list, split, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				g.run();
			}
			const float lantern[8] = { light.x, light.y, light.z, 4.0f * 1.5f, 1.469f * strength, 1.0f * strength, 0.219f * strength, 1.0f };
			float lantern_glow[8];
			std::memcpy(lantern_glow, lantern, sizeof(lantern_glow));
			lantern_glow[7] = 0.0f;
			const std::vector<uint8_t> unknown = frame(lantern, 1, 0.0f);
			r.set_light_source([](void *c) { return static_cast<ID3D12Resource *>(c); }, split);
			const std::vector<uint8_t> wall_unlit = frame(nullptr, 0, 0.0f), known = frame(lantern, 1, 0.0f), known_glow = frame(lantern_glow, 1, 0.0f),
				known_shadow = frame(lantern, 1, 1.0f);
			r.set_light_source(nullptr, nullptr);
			cards.clear();
			release(split);
			release(split_up);
			auto beside_lantern = [&](const std::vector<uint8_t> &img) {
				double sum = 0.0;
				uint32_t n = 0;
				for (float d = 0.4f; d <= 0.9f; d += 0.1f)
					for (float side : { -1.0f, 1.0f })
					{
						sum += around(img, on_screen(at_side(wall_at, d * side), nullptr)).luma;
						n++;
					}
				return sum / n;
			};
			const double near_dark = beside_lantern(wall_unlit), near_unknown = beside_lantern(unknown), near_known = beside_lantern(known),
				near_glow = beside_lantern(known_glow);
			const auto walled = on_screen(at_side(wall_at, 2.6f), nullptr), clear = on_screen(at_side(wall_at, -2.6f), nullptr);
			const double walled_lit = around(known, walled).luma, walled_shadow = around(known_shadow, walled).luma, walled_dark = around(wall_unlit, walled).luma;
			const double clear_lit = around(known, clear).luma, clear_shadow = around(known_shadow, clear).luma;
			std::printf("  with the light split: the wall beside a lantern %.1f unlit, %.1f lit without the split, %.1f with it, %.1f lit by a glow; behind a pillar %.1f -> %.1f with lamp shadows (%.1f unlit); clear the other side %.1f -> %.1f\n",
				near_dark, near_unknown, near_known, near_glow, walled_lit, walled_shadow, walled_dark, clear_lit, clear_shadow);
			check(near_unknown > near_dark + 5.0, "without the light split the lamp's glow is added as before");
			check(std::fabs(near_known - near_dark) < 0.5, "with it the zone's lamp isn't added again (the native passes lit it)");
			check(near_glow > near_dark + 5.0, "an effect's glow still lights what the light split knows");
			check(walled_shadow < walled_lit - 2.0 && walled_shadow >= 0.0, "a lamp shadow takes the lamp's light away where the light split knows the pixel");
			check(std::fabs(clear_shadow - clear_lit) < 0.5, "and leaves it where the way to the lamp is clear");
		}
		check(g.report_messages() == 0, "no debug-layer errors (lamps)");
	}
	if (lamps_only)
	{
		std::printf(failures == 0 ? "\nall passed (the lamps only)\n" : "\n%d failed (the lamps only)\n", failures);
		return failures == 0 ? 0 : 1;
	}

	// 1q. The game's fog (its weather's, linear by view depth from where it starts to where it ends, toward its colour):
	// with a blue fog ending 12 yalms off over the brown ground, the looked-at blade 5 yalms off comes out part way to the
	// fog's colour (about 45%); a fog too thin to see leaves the picture as without it (the fog taken out of the ground's
	// remembered colour and put back over the blade comes out the same); fireflies fade into it too, few seen with it
	// ending 3 yalms off. Then two patches of blades on the flat ground, one at the looked-at point and one 25 yalms
	// off along the view: in that fog every pixel of the far one is the fog's colour, the near one's part way.
	{
		const float brown[4] = { 0.35f, 0.3f, 0.2f, 0.5f };
		const float blue[3] = { 0.2f, 0.4f, 0.9f };
		auto frame = [&](float fog_end, float night = 0.0f, float time = 1.0f) {
			fresh_memory();
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, brown, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.time = time;
			l2.night = night;
			l2.fireflies = night > 0.0f ? 2.0f : 0.0f;
			l2.haze = fog_end > 0.0f;
			std::memcpy(l2.haze_color, blue, sizeof(blue));
			l2.haze_start = 0.0f;
			l2.haze_end = fog_end;
			r.draw(g.list, t, cam, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			std::vector<uint8_t> out = g.read(world, 4);
			r.release_old(0); // (the colour memories forgotten: the GPU is done with them once read, and WARP runs out)
			return out;
		};
		const std::vector<uint8_t> clear_frame = frame(0.0f), foggy = frame(12.0f), thin = frame(100000.0f);
		auto is_ground = [&](const uint8_t *p) {
			return std::abs(p[0] - int(brown[0] * 255.0f + 0.5f)) + std::abs(p[1] - int(brown[1] * 255.0f + 0.5f)) + std::abs(p[2] - int(brown[2] * 255.0f + 0.5f)) <= 6;
		};
		auto to_blue = [&](const uint8_t *p) {
			return std::sqrt(std::pow(p[0] - blue[0] * 255.0, 2) + std::pow(p[1] - blue[1] * 255.0, 2) + std::pow(p[2] - blue[2] * 255.0, 2));
		};
		uint32_t clear_blue = 0, near_n = 0, thin_diff = 0, thin_most = 0;
		double near_clear = 0.0, near_foggy = 0.0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const size_t k = (size_t(y) * W + x) * 4;
				const uint8_t *c = &clear_frame[k], *f = &foggy[k], *n = &thin[k];
				clear_blue += to_blue(c) < 12.0 ? 1 : 0;
				if (std::fabs(x - px) < 6.0f && y < py + 3.0f && y > py - 30.0f && !is_ground(c))
				{
					near_n++;
					near_clear += to_blue(c);
					near_foggy += to_blue(f);
				}
				const int most = std::max({ std::abs(c[0] - n[0]), std::abs(c[1] - n[1]), std::abs(c[2] - n[2]) });
				thin_diff += most > 2 ? 1 : 0;
				thin_most = std::max(thin_most, uint32_t(most));
			}
		if (near_n != 0)
		{
			near_clear /= near_n;
			near_foggy /= near_n;
		}
		std::printf("  the game's fog (blue, to 12 yalms): the looked-at blade %.0f from the fog's colour (%.0f without it, %u pixels; %u pixels "
			"of the fog's colour without the fog); a thin fog (to 100,000 yalms) changes %u pixels, by %u at most\n",
			near_foggy, near_clear, near_n, clear_blue, thin_diff, thin_most);
		if (FILE *f = nullptr; fopen_s(&f, "build\\fog_test.ppm", "wb") == 0 && f != nullptr)
		{
			// (the foggy frame, to look at)
			std::fprintf(f, "P6\n%u %u\n255\n", W, H);
			for (size_t i = 0; i < size_t(W) * H; ++i)
				std::fwrite(&foggy[i * 4], 1, 3, f);
			std::fclose(f);
		}
		check(clear_blue == 0 && near_n > 5 && near_foggy < 0.75 * near_clear && near_foggy > 0.3 * near_clear, "a near blade part of the way into the fog, as far into it as it is");
		check(thin_diff < 20 && thin_most <= 3, "a fog too thin to see leaves the grass as it was");

		// Fireflies: over six moments at night, the pixels they light (brighter than the same moment by day), with the fog
		// ending 3 yalms off and without
		uint32_t lit_clear = 0, lit_foggy = 0;
		for (int i = 0; i < 6; ++i)
		{
			const float time = 1.0f + i * 1.3f;
			const std::vector<uint8_t> day = frame(0.0f, 0.0f, time), night = frame(0.0f, 1.0f, time);
			const std::vector<uint8_t> foggy_day = frame(3.0f, 0.0f, time), foggy_night = frame(3.0f, 1.0f, time);
			for (size_t k = 0; k < size_t(W) * H * 4; k += 4)
			{
				lit_clear += night[k] > day[k] + 2 || night[k + 1] > day[k + 1] + 2 ? 1 : 0;
				lit_foggy += foggy_night[k] > foggy_day[k] + 2 || foggy_night[k + 1] > foggy_day[k + 1] + 2 ? 1 : 0;
			}
		}
		std::printf("  fireflies over six moments: %u pixels lit, %u in a fog ending 3 yalms off\n", lit_clear, lit_foggy);
		check(lit_clear > 20 && lit_foggy < 0.3 * lit_clear, "fireflies fade into the fog");

		// The two patches (25 points each, half a yalm apart, on the flat ground the depth holds): their own blade file
		// for now, the zone's put back after
		{
			const vec3 far_point = { eye.x + (at.x - eye.x) * 5.0f, at.y, eye.z + (at.z - eye.z) * 5.0f }; // (25 yalms across the ground)
			blade_file patches;
			patches.zone = 100;
			patches.cell_size = 16.0f;
			std::vector<std::pair<std::pair<int32_t, int32_t>, std::vector<float>>> cells; // ((cz, cx), points)
			for (const vec3 &c : { at, far_point })
				for (int i = 0; i < 25; ++i)
				{
					const float x = c.x + float(i % 5 - 2) * 0.5f, z = c.z + float(i / 5 - 2) * 0.5f;
					const std::pair<int32_t, int32_t> key = { int32_t(std::floor(z / 16.0f)), int32_t(std::floor(x / 16.0f)) };
					auto it = std::find_if(cells.begin(), cells.end(), [&](const auto &e) { return e.first == key; });
					if (it == cells.end())
						it = cells.insert(cells.end(), { key, {} });
					it->second.insert(it->second.end(), { x, c.y, z, float(i) * 0.137f });
				}
			std::sort(cells.begin(), cells.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
			for (const auto &e : cells)
			{
				patches.cells.push_back({ e.first.second, e.first.first, uint32_t(patches.blades.size() / 4), uint32_t(e.second.size() / 4) });
				patches.blades.insert(patches.blades.end(), e.second.begin(), e.second.end());
			}
			const bool swapped = r.upload(patches, error);
			const std::vector<uint8_t> patch_clear = frame(0.0f), patch_foggy = frame(12.0f);
			r.upload(file, error);
			r.release_old(0);
			const float fy = far_point.x * vp[1] + far_point.y * vp[5] + far_point.z * vp[9] + vp[13];
			const float fw = far_point.x * vp[3] + far_point.y * vp[7] + far_point.z * vp[11] + vp[15];
			const float far_row = (1.0f - fy / fw) * 0.5f * H;
			uint32_t far_n = 0, far_blue = 0, near_px = 0, other = 0;
			double near_patch_clear = 0.0, near_patch_foggy = 0.0;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					const size_t k = (size_t(y) * W + x) * 4;
					const uint8_t *c = &patch_clear[k], *f = &patch_foggy[k];
					if (is_ground(c) && is_ground(f))
						continue;
					if (float(y) < far_row + 3.0f && float(y) > far_row - 20.0f)
					{
						far_n++;
						far_blue += to_blue(f) < 8.0 ? 1 : 0;
					}
					else if (float(y) > py - 40.0f)
					{
						near_px++;
						near_patch_clear += to_blue(c);
						near_patch_foggy += to_blue(f);
					}
					else
						other++;
				}
			if (near_px != 0)
			{
				near_patch_clear /= near_px;
				near_patch_foggy /= near_px;
			}
			std::printf("  two patches of blades, the far one %.1f yalms deep (row %.0f): %u of its %u pixels the fog's colour; the near one %.0f from it "
				"(%.0f without the fog, %u pixels); %u pixels elsewhere\n", fw, far_row, far_blue, far_n, near_patch_foggy, near_patch_clear, near_px, other);
			check(swapped && far_n > 20 && far_blue == far_n && other == 0, "a patch of blades past where the fog ends comes out in its colour, every pixel");
			check(near_px > 50 && near_patch_foggy < 0.75 * near_patch_clear && near_patch_foggy > 0.3 * near_patch_clear, "the near patch part of the way");
		}
		check(g.report_messages() == 0, "no debug-layer errors (fog)");
	}

	// 1r. Wet ground (grass off, the shading pass on for it alone): a wet mask marking the flat ground as path or stone
	// where the world's x is less than the looked-at point's. Soaked, with no sheen or highlight, that side comes out
	// the 30% darker the wet look takes; with the sky's sheen it gets some light back (seen this low, a lot of the sky
	// is mirrored), still darker than dry. The other side is as drawn, and so is an upright wall standing on the wet
	// side (only what faces up gets wet); dry (0), or with no mask, nothing is drawn at all
	{
		const float plain[4] = { 0.45f, 0.42f, 0.36f, 0.5f }, dark_box[4] = { 0.2f, 0.2f, 0.2f, 0.5f };
		const vec3 q = { at.x + (eye.x - at.x) * 0.1f, at.y + (eye.y - at.y) * 0.1f, at.z + (eye.z - at.z) * 0.1f };
		const float qz = q.x * vp[2] + q.y * vp[6] + q.z * vp[10] + vp[14], qw = q.x * vp[3] + q.y * vp[7] + q.z * vp[11] + vp[15];
		const D3D12_RECT box = { LONG(px) - 60, LONG(py) - 90, LONG(px) + 60, LONG(py) - 12 };
		bool sheen = true;
		auto frame = [&](float wetness, bool with_box, draw_stats &st) {
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			if (with_box)
			{
				g.list->ClearRenderTargetView(world_rtv, dark_box, 1, &box);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, qz / qw, 0, 1, &box);
			}
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.grass = false;
			l2.ao_strength = 0.0f;
			l2.relief = 0.0f;
			l2.sun_shadow = 0.0f;
			l2.wet = wetness;
			if (!sheen)
				for (int i = 0; i < 3; ++i)
					l2.wet_sheen[i] = l2.wet_light[i] = 0.0f;
			st = r.draw(g.list, t, cam, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			return g.read(world, 4);
		};
		// (the mask: 0.5-yalm squares 30 yalms each way around the looked-at point, set left of it in the world's x)
		wet_file mask;
		mask.zone = 100;
		mask.square = 0.5f;
		mask.x0 = at.x - 30.0f;
		mask.z0 = at.z - 30.0f;
		mask.width = mask.height = 120;
		mask.words_a_row = (mask.width + 31) / 32;
		mask.words.assign(size_t(mask.height) * mask.words_a_row, 0u);
		for (uint32_t j = 0; j < mask.height; ++j)
			for (uint32_t i = 0; i < 60; ++i)
				mask.words[j * mask.words_a_row + (i >> 5)] |= 1u << (i & 31);
		mask.set = 60 * mask.height;
		draw_stats st_none, st_dry, st_wet, st_wall, st_bare;
		const std::vector<uint8_t> no_mask = frame(1.0f, false, st_none);
		const bool loaded = r.upload_wet(mask, error);
		const std::vector<uint8_t> dry = frame(0.0f, false, st_dry), soaked = frame(1.0f, false, st_wet), walled = frame(1.0f, true, st_wall);
		sheen = false;
		const std::vector<uint8_t> bare = frame(1.0f, false, st_bare);
		sheen = true;
		r.unload_wet();
		r.release_old(0);
		// (each pixel's ground point, x and z: its view ray down to the flat ground at the looked-at height)
		float inv[16];
		invert(vp, inv);
		auto ground_x = [&](UINT x, UINT y, float &gx, float &gz) {
			const float nx = (x + 0.5f) / W * 2.0f - 1.0f, ny = 1.0f - (y + 0.5f) / H * 2.0f;
			float p[2][3];
			for (int k = 0; k < 2; ++k)
			{
				const float c[4] = { nx, ny, k == 0 ? 0.5f : 0.9f, 1.0f };
				float o[4] = {};
				for (int col = 0; col < 4; ++col)
					o[col] = c[0] * inv[col] + c[1] * inv[4 + col] + c[2] * inv[8 + col] + c[3] * inv[12 + col];
				for (int a = 0; a < 3; ++a)
					p[k][a] = o[a] / o[3];
			}
			const float dy = p[1][1] - p[0][1];
			if (std::fabs(dy) < 1e-6f)
				return false;
			const float s = (at.y - p[0][1]) / dy;
			gx = p[0][0] + (p[1][0] - p[0][0]) * s;
			gz = p[0][2] + (p[1][2] - p[0][2]) * s;
			return s > -10.0f;
		};
		auto luma = [](const uint8_t *c) { return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]; };
		double wet_side = 0.0, dry_side = 0.0, bare_side = 0.0;
		uint32_t wet_n = 0, dry_n = 0, unchanged_dry = 0, wall_changed = 0, wall_n = 0, none_changed = 0, dry_changed = 0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const size_t k = (size_t(y) * W + x) * 4;
				const double plain_luma = (0.3 * 0.45 + 0.59 * 0.42 + 0.11 * 0.36) * 255.0;
				none_changed += std::abs(luma(&no_mask[k]) - plain_luma) > 1.0 ? 1 : 0;
				dry_changed += std::abs(luma(&dry[k]) - plain_luma) > 1.0 ? 1 : 0;
				if (LONG(x) >= box.left && LONG(x) < box.right && LONG(y) >= box.top && LONG(y) < box.bottom)
				{
					wall_n++;
					wall_changed += std::abs(luma(&walled[k]) - (0.2 * 255.0)) > 1.0 ? 1 : 0;
					continue;
				}
				float gx = 0.0f, gz = 0.0f;
				if (!ground_x(x, y, gx, gz))
					continue;
				if (gx < at.x - 1.0f && gx > at.x - 25.0f && std::fabs(gz - at.z) < 25.0f)
				{
					wet_n++;
					wet_side += luma(&soaked[k]);
					bare_side += luma(&bare[k]);
				}
				else if (gx > at.x + 1.0f)
				{
					dry_n++;
					dry_side += luma(&soaked[k]);
					unchanged_dry += std::abs(luma(&soaked[k]) - luma(&dry[k])) <= 1.0 ? 1 : 0;
				}
			}
		if (wet_n != 0)
		{
			wet_side /= wet_n;
			bare_side /= wet_n;
		}
		if (dry_n != 0)
			dry_side /= dry_n;
		std::printf("  wet ground: the masked side %.1f with the sky's sheen, %.1f without (%u pixels), the other %.1f (%u, %u as drawn); an upright wall "
			"on it: %u of %u pixels changed; shading pass with no mask %s, dry %s, soaked %s (wet look %s)\n", wet_side, bare_side, wet_n, dry_side, dry_n,
			unchanged_dry, wall_changed, wall_n, st_none.shaded ? "ran" : "didn't run", st_dry.shaded ? "ran" : "didn't run", st_wet.shaded ? "ran" : "didn't run",
			st_wet.wet ? "drawn" : "not drawn");
		check(loaded && st_wet.wet && st_bare.wet && wet_n > 2000 && dry_n > 2000, "the wet mask loaded and drawn with, both sides on screen");
		check(std::fabs(bare_side - 0.7 * dry_side) < 1.5, "path and stone in the wet mask 30% darker when soaked (no sheen)");
		check(wet_side > bare_side + 3.0 && wet_side < 0.95 * dry_side, "the sky's sheen gives some light back, still darker than dry");
		check(unchanged_dry == dry_n, "ground off the mask as drawn");
		check(wall_n > 0 && wall_changed == 0, "an upright wall on the wet side isn't (only what faces up gets wet)");
		check(!st_none.shaded && !st_dry.shaded && none_changed == 0 && dry_changed == 0, "no mask, or dry: no shading pass, the picture as drawn");
		check(g.report_messages() == 0, "no debug-layer errors (wet ground)");
	}

	// 1s. Rain (grass off): a rain map of flat ground at the looked-at height 60 yalms each way, the camera 2 yalms
	// over it. Raining as hard as it goes, drops show on the picture (against the same frame without rain) where the
	// view looks down onto the ground near the camera, and splashes add to that (against the same frame without
	// splashes). With a roof 3 yalms over the ground (1 over the camera) 20 yalms each way in the map, the same pixels
	// show no rain and no splash at all: drops end on it. At weather 'fine' (no rain) nothing is drawn. On the
	// add-on's water in the open, the rain's rings change the water (against the same rain without rings).
	{
		const float plain[4] = { 0.45f, 0.42f, 0.36f, 0.5f };
		auto map = [&](bool roof, bool pond) {
			rain_file m;
			m.zone = 104;
			m.square = 0.5f;
			m.x0 = at.x - 60.0f;
			m.z0 = at.z - 60.0f;
			m.width = m.height = 240;
			m.y0 = at.y - 10.0f;
			m.step = 1.0f / 16.0f;
			m.q.assign(size_t(m.width) * m.height, uint16_t(10.0f / m.step));
			m.kinds.assign(m.q.size(), kRainGround);
			for (uint32_t j = 0; j < m.height; ++j)
				for (uint32_t i = 0; i < m.width; ++i)
				{
					const float x = m.x0 + (i + 0.5f) * m.square, z = m.z0 + (j + 0.5f) * m.square;
					const size_t k = size_t(j) * m.width + i;
					if (roof && std::fabs(x - eye.x) < 20.0f && std::fabs(z - eye.z) < 20.0f)
						m.q[k] = uint16_t(7.0f / m.step); // (y 3 over the ground: y is down)
					if (pond && std::fabs(x - at.x) < 4.0f && std::fabs(z - at.z) < 4.0f)
					{
						m.q[k] = uint16_t(9.5f / m.step); // (the water's surface, half a yalm over the bed)
						m.kinds[k] = kRainWater;
					}
				}
			m.covered = m.width * m.height;
			return m;
		};
		blade_file pond;
		pond.zone = 104;
		pond.cell_size = 32.0f;
		pond.stride = 8;
		{
			const float x0 = at.x - 4.0f, x1 = at.x + 4.0f, z0 = at.z - 4.0f, z1 = at.z + 4.0f;
			const float corners[6][2] = { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z0 }, { x1, z1 }, { x0, z1 } };
			for (const auto &c : corners)
				pond.blades.insert(pond.blades.end(), { c[0], at.y, c[1], 9.0f, at.y - 0.5f, 5.0f, 0.0f, 0.0f });
			pond.cells.push_back({ int32_t(std::floor(at.x / 32.0f)), int32_t(std::floor(at.z / 32.0f)), 0, 6 });
		}
		auto frame = [&](float rain, float splashes, bool water, float rings, draw_stats &st) {
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.grass = false;
			l2.ao_strength = 0.0f;
			l2.relief = 0.0f;
			l2.sun_shadow = 0.0f;
			l2.fireflies = 0.0f;
			l2.water = water;
			l2.glint = 0.0f;
			l2.time = 100.0f;
			l2.rain = rain;
			l2.rain_slant[0] = 0.1f;
			l2.rain_slant[1] = 0.05f;
			l2.rain_light[0] = l2.rain_light[1] = l2.rain_light[2] = 0.9f;
			l2.rain_splashes = splashes;
			l2.rain_rings = rings;
			st = r.draw(g.list, t, cam, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			return g.read(world, 4);
		};
		// (the pixels that look down onto the ground within 15 yalms of the camera: all under the roof when it's there)
		float inv[16];
		invert(vp, inv);
		auto ground_at = [&](UINT x, UINT y, float &gx, float &gz) {
			const float nx = (x + 0.5f) / W * 2.0f - 1.0f, ny = 1.0f - (y + 0.5f) / H * 2.0f;
			float p[2][3];
			for (int k = 0; k < 2; ++k)
			{
				const float c[4] = { nx, ny, k == 0 ? 0.5f : 0.9f, 1.0f };
				float o[4] = {};
				for (int col = 0; col < 4; ++col)
					o[col] = c[0] * inv[col] + c[1] * inv[4 + col] + c[2] * inv[8 + col] + c[3] * inv[12 + col];
				for (int a = 0; a < 3; ++a)
					p[k][a] = o[a] / o[3];
			}
			const float dy = p[1][1] - p[0][1];
			if (std::fabs(dy) < 1e-6f)
				return false;
			const float s = (at.y - p[0][1]) / dy;
			gx = p[0][0] + (p[1][0] - p[0][0]) * s;
			gz = p[0][2] + (p[1][2] - p[0][2]) * s;
			return (gx - eye.x) * (p[1][0] - p[0][0]) + (gz - eye.z) * (p[1][2] - p[0][2]) > 0.0f; // (in front of the camera)
		};
		auto differ = [&](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, bool (*where)(float, float, float, float), uint32_t &n) {
			uint32_t changed = 0;
			n = 0;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					float gx = 0.0f, gz = 0.0f;
					if (!ground_at(x, y, gx, gz) || !where(gx - eye.x, gz - eye.z, gx - at.x, gz - at.z))
						continue;
					n++;
					const size_t k = (size_t(y) * W + x) * 4;
					if (std::abs(int(a[k]) - int(b[k])) > 2 || std::abs(int(a[k + 1]) - int(b[k + 1])) > 2 || std::abs(int(a[k + 2]) - int(b[k + 2])) > 2)
						changed++;
				}
			return changed;
		};
		auto near_ground = [](float ex, float ez, float, float) { return ex * ex + ez * ez < 15.0f * 15.0f; };
		auto on_pond = [](float, float, float px_, float pz_) { return std::fabs(px_) < 3.5f && std::fabs(pz_) < 3.5f; };
		draw_stats st_dry, st_rain, st_plain, st_roof_dry, st_roof, st_roof_plain, st_fine, st_w0, st_w1;
		std::string up_error;
		const bool up_open = r.upload_rain(map(false, false), up_error);
		float sy = 0.0f;
		uint8_t sk = 0;
		const bool looked = r.rain_surface(at.x, at.z, sy, sk) && std::fabs(sy - at.y) < 0.05f && sk == kRainGround;
		const std::vector<uint8_t> dry = frame(0.0f, 1.0f, false, 1.0f, st_dry), raining = frame(1.0f, 1.0f, false, 1.0f, st_rain),
			no_splash = frame(1.0f, 0.0f, false, 1.0f, st_plain), fine = frame(0.0f, 1.0f, false, 1.0f, st_fine);
		const bool up_roof = r.upload_rain(map(true, false), up_error);
		const bool roof_looked = r.rain_surface(eye.x, eye.z, sy, sk) && std::fabs(sy - (at.y - 3.0f)) < 0.05f;
		const std::vector<uint8_t> roof_dry = frame(0.0f, 1.0f, false, 1.0f, st_roof_dry), roofed = frame(1.0f, 1.0f, false, 1.0f, st_roof),
			roof_no_splash = frame(1.0f, 0.0f, false, 1.0f, st_roof_plain);
		const bool up_pond = r.upload_rain(map(false, true), up_error) && r.upload_water(pond, up_error);
		const std::vector<uint8_t> rings_off = frame(1.0f, 0.0f, true, 0.0f, st_w0), rings_on = frame(1.0f, 0.0f, true, 1.0f, st_w1);
		r.unload_rain();
		r.unload_water();
		r.release_old(0);
		uint32_t n_near = 0, n_pond = 0, n_tmp = 0;
		const uint32_t rain_px = differ(raining, dry, near_ground, n_near), splash_px = differ(raining, no_splash, near_ground, n_tmp);
		const uint32_t roof_rain_px = differ(roofed, roof_dry, near_ground, n_tmp), roof_splash_px = differ(roofed, roof_no_splash, near_ground, n_tmp);
		const uint32_t fine_px = differ(fine, dry, near_ground, n_tmp), ring_px = differ(rings_on, rings_off, on_pond, n_pond);
		std::printf("  rain: %u drop places and %u splash places drawn; of %u pixels looking at the ground within 15 yalms, %u changed by the rain, "
			"%u by its splashes; under a roof: %u and %u (drops %u); at 'fine': %u (drops %u); the pond's %u pixels: %u changed by the rings%s%s\n",
			st_rain.rain_drops, st_rain.splashes, n_near, rain_px, splash_px, roof_rain_px, roof_splash_px, st_roof.rain_drops, fine_px, st_fine.rain_drops,
			n_pond, ring_px, error.empty() ? "" : "; ", error.c_str());
		// (a picture of it: build\rain_test.ppm)
		if (FILE *f = nullptr; fopen_s(&f, "build\\rain_test.ppm", "wb") == 0 && f != nullptr)
		{
			std::fprintf(f, "P6 %u %u 255\n", W, H);
			for (size_t i = 0; i < size_t(W) * H; ++i)
				std::fwrite(&raining[i * 4], 1, 3, f);
			fclose(f);
		}
		check(up_open && up_roof && up_pond && looked && roof_looked, "rain maps loaded; the map read back where the ground and the roof are");
		check(st_rain.rain_drops > 0 && st_rain.splashes > 0 && n_near > 20000, "raining: drops and splashes drawn, the ground near the camera on screen");
		check(rain_px > 500, "drops show over the ground near the camera");
		check(splash_px > 30, "splashes on open ground near the camera");
		check(st_roof.rain_drops > 0 && roof_rain_px == 0, "under a roof: no drop seen at all (they end on it)");
		check(roof_splash_px == 0, "under a roof: no splash");
		check(st_fine.rain_drops == 0 && st_fine.splashes == 0 && fine_px == 0 && !st_fine.bound, "weather 'fine': no rain drawn, the picture as it was");
		check(st_w1.water_corners > 0 && n_pond > 2000 && ring_px > 100, "the rain's rings change the water");

		// Settled snow (look::snow_cover) on the same flat ground, with a roof 3 yalms over it on the far side of the
		// looked-at point (x past at.x + 1, all z) in the rain map: a whole layer whitens the open ground near the camera
		// and leaves the roofed ground as drawn; half a layer covers part of the open ground (patches); none, nothing
		// (no shading pass); and none without the rain map either.
		{
			auto snow_map = [&]() {
				rain_file m = map(false, false);
				for (uint32_t j = 0; j < m.height; ++j)
					for (uint32_t i = 0; i < m.width; ++i)
						if (m.x0 + (i + 0.5f) * m.square > at.x + 1.0f)
							m.q[size_t(j) * m.width + i] = uint16_t(7.0f / m.step);
				return m;
			};
			auto snow_frame = [&](float cover, draw_stats &st, const D3D12_RECT *dark = nullptr, float depth = 0.0f, const float *who = nullptr,
				bool trails = false, UINT dark_count = 1) {
				barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
				g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
				if (dark != nullptr) // (a dark patch drawn on the ground: a shadow)
				{
					static const float black[4] = { 0.02f, 0.02f, 0.02f, 1.0f };
					g.list->ClearRenderTargetView(world_rtv, black, dark_count, dark);
				}
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
				ground_depth(cam);
				barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				look l2 = l;
				l2.grass = false;
				l2.ao_strength = 0.0f;
				l2.relief = 0.0f;
				l2.sun_shadow = 0.0f;
				l2.fireflies = 0.0f;
				l2.water = false;
				l2.time = 100.0f;
				l2.snow_cover = cover;
				l2.snow_depth = depth;
				l2.snow_trails = trails;
				l2.snowfall = 1.0f;
				l2.actors = who;
				l2.actor_count = who != nullptr ? 1 : 0;
				st = r.draw(g.list, t, cam, l2, error);
				barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
				return g.read(world, 4);
			};
			draw_stats st_none, st_whole, st_half, st_nomap;
			const bool up_snow = r.upload_rain(snow_map(), up_error);
			// (the ground near the camera on each side of the roof's edge, a yalm clear of it; how many pixels whitened)
			static auto open_side = [](float ex, float ez, float px_, float) { return ex * ex + ez * ez < 15.0f * 15.0f && px_ < 0.0f; };
			const std::vector<uint8_t> bare = snow_frame(0.0f, st_none), whole = snow_frame(1.0f, st_whole), half = snow_frame(0.5f, st_half);
			// (a dark patch on the open ground near the camera, under a whole layer: none of it shows a little way off, where
			// the light's taps once reached and put copies of it on the snow around)
			// (in the middle of the open ground seen)
			D3D12_RECT dark = {};
			{
				double cx = 0.0, cy = 0.0;
				uint32_t n = 0;
				for (UINT y = 0; y < H; ++y)
					for (UINT x = 0; x < W; ++x)
					{
						float gx = 0.0f, gz = 0.0f;
						if (ground_at(x, y, gx, gz) && open_side(gx - eye.x, gz - eye.z, gx - at.x, gz - at.z))
							cx += x, cy += y, n++;
					}
				if (n != 0)
					dark = { LONG(cx / n) - 6, LONG(cy / n) - 6, LONG(cx / n) + 6, LONG(cy / n) + 6 };
			}
			draw_stats st_dark;
			const std::vector<uint8_t> shadowed = snow_frame(1.0f, st_dark, dark.right != 0 ? &dark : nullptr);
			uint32_t ghost_px = 0, ring_px = 0;
			for (LONG y = dark.top - 45; dark.right != 0 && y < dark.bottom + 45; ++y)
				for (LONG x = dark.left - 45; x < dark.right + 45; ++x)
				{
					if (x < 0 || y < 0 || x >= LONG(W) || y >= LONG(H) || (x >= dark.left - 8 && x < dark.right + 8 && y >= dark.top - 8 && y < dark.bottom + 8))
						continue;
					const size_t i = (size_t(y) * W + size_t(x)) * 4;
					const double d = (0.3 * whole[i] + 0.59 * whole[i + 1] + 0.11 * whole[i + 2]) - (0.3 * shadowed[i] + 0.59 * shadowed[i + 1] + 0.11 * shadowed[i + 2]);
					ghost_px += d > 12.0 ? 1 : 0;
					ring_px++;
				}
			std::printf("  settled snow over a dark patch at %ld,%ld: %u of %u pixels 8 to 45 out from it darker\n", dark.left, dark.top, ghost_px, ring_px);
			// (a floor's pattern under a whole layer: tile joints, dark lines 0.06 yalm wide on a 1-yalm grid across the open
			// ground near the camera, each a run of pixels; the snow over them within 8 yalms as bright as without them, not
			// the joints showing through as lines. Further off the grid's hard lines are a few pixels apart, where the game's
			// own floors are mip-filtered to a soft grey: counted apart, not checked)
			std::vector<D3D12_RECT> seams;
			std::vector<uint8_t> is_seam(size_t(W) * H, 0);
			uint32_t seam_px = 0, seam_dark_px = 0, far_px = 0, far_dark_px = 0;
			double seam_diff = 0.0;
			for (UINT y = 0; y < H; ++y)
			{
				LONG run = -1;
				for (UINT x = 0; x <= W; ++x)
				{
					float gx = 0.0f, gz = 0.0f;
					bool on = false;
					if (x < W && ground_at(x, y, gx, gz) && open_side(gx - eye.x, gz - eye.z, gx - at.x, gz - at.z))
					{
						const float fx = gx - std::floor(gx), fz = gz - std::floor(gz);
						on = fx < 0.06f || fz < 0.06f;
					}
					if (on)
					{
						const float ex = gx - eye.x, ez = gz - eye.z;
						is_seam[size_t(y) * W + x] = ex * ex + ez * ez < 8.0f * 8.0f ? 1 : 2;
						if (run < 0)
							run = LONG(x);
					}
					else if (run >= 0)
					{
						seams.push_back({ run, LONG(y), LONG(x), LONG(y) + 1 });
						run = -1;
					}
				}
			}
			if (!seams.empty())
			{
				draw_stats st_seams;
				const std::vector<uint8_t> seamed = snow_frame(1.0f, st_seams, seams.data(), 0.0f, nullptr, false, UINT(seams.size()));
				for (size_t k = 0; k < size_t(W) * H; ++k)
				{
					if (is_seam[k] == 0)
						continue;
					const size_t i = k * 4;
					const double d = (0.3 * whole[i] + 0.59 * whole[i + 1] + 0.11 * whole[i + 2]) - (0.3 * seamed[i] + 0.59 * seamed[i + 1] + 0.11 * seamed[i + 2]);
					if (is_seam[k] == 2)
					{
						far_dark_px += d > 12.0 ? 1 : 0;
						far_px++;
						continue;
					}
					seam_diff += std::fabs(d);
					seam_dark_px += d > 12.0 ? 1 : 0;
					seam_px++;
				}
				if (FILE *f = nullptr; fopen_s(&f, "build\\snow_seams_test.ppm", "wb") == 0 && f != nullptr)
				{
					std::fprintf(f, "P6 %u %u 255\n", W, H);
					for (size_t i = 0; i < size_t(W) * H; ++i)
						std::fwrite(&seamed[i * 4], 1, 3, f);
					fclose(f);
				}
			}
			std::printf("  settled snow over a floor's dark seams: within 8 yalms %u seam pixels, %u of them darker by over 12, %.1f darker on average; "
				"further off %u of %u\n", seam_px, seam_dark_px, seam_px != 0 ? seam_diff / seam_px : 0.0, far_dark_px, far_px);
			// The snow layer (look::snow_depth): half a layer with depth differs from the same flat on the open ground near
			// the camera (its drifts' sides lit as they face, raised over the ground) and not on the roofed side; a whole layer
			// with depth is about as bright as flat. Trails (look::snow_trails): someone standing on the open ground treads the
			// snow down where they stand (darker there than without them), the trail stays once they've gone, and a new zone's
			// map (the rain map loaded again) starts untrodden.
			static auto roofed_near = [](float ex, float ez, float px_, float) { return ex * ex + ez * ez < 15.0f * 15.0f && px_ > 2.0f; };
			auto open_mean_of = [&](const std::vector<uint8_t> &img) {
				double total = 0.0;
				uint32_t k = 0;
				for (UINT y = 0; y < H; ++y)
					for (UINT x = 0; x < W; ++x)
					{
						float gx = 0.0f, gz = 0.0f;
						if (!ground_at(x, y, gx, gz) || !open_side(gx - eye.x, gz - eye.z, gx - at.x, gz - at.z))
							continue;
						const uint8_t *c = &img[(size_t(y) * W + x) * 4];
						total += 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
						k++;
					}
				return k != 0 ? total / k : 0.0;
			};
			// (where someone stands: the open ground at the middle of the dark patch's place)
			float stand_x = 0.0f, stand_z = 0.0f;
			const bool stand_seen = dark.right != 0 && ground_at(UINT(dark.left + 6), UINT(dark.top + 6), stand_x, stand_z);
			// (someone walking past it, 3 yalms along x a yalm a frame: footprints a stride apart along the way)
			const float who_from[4] = { stand_x - 1.5f, at.y, stand_z, 1.0f }, who_a[4] = { stand_x - 0.5f, at.y, stand_z, 1.0f },
				who_b[4] = { stand_x + 0.5f, at.y, stand_z, 1.0f }, who[4] = { stand_x + 1.5f, at.y, stand_z, 1.0f };
			draw_stats st_from, st_a, st_b;
			auto mean_near = [&](const std::vector<uint8_t> &img, float reach, uint32_t &k) {
				double total = 0.0;
				k = 0;
				for (UINT y = 0; y < H; ++y)
					for (UINT x = 0; x < W; ++x)
					{
						float gx = 0.0f, gz = 0.0f;
						if (!ground_at(x, y, gx, gz) || (gx - stand_x) * (gx - stand_x) + (gz - stand_z) * (gz - stand_z) > reach * reach)
							continue;
						const uint8_t *c = &img[(size_t(y) * W + x) * 4];
						total += 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
						k++;
					}
				return k != 0 ? total / k : 0.0;
			};
			draw_stats st_layer_half, st_layer_whole, st_trod, st_gone, st_fresh;
			const std::vector<uint8_t> layer_half = snow_frame(0.5f, st_layer_half, nullptr, 1.0f);
			const std::vector<uint8_t> layer_whole = snow_frame(1.0f, st_layer_whole, nullptr, 1.0f, nullptr, true);
			snow_frame(1.0f, st_from, nullptr, 1.0f, who_from, true);
			snow_frame(1.0f, st_a, nullptr, 1.0f, who_a, true);
			snow_frame(1.0f, st_b, nullptr, 1.0f, who_b, true);
			const std::vector<uint8_t> trod = snow_frame(1.0f, st_trod, nullptr, 1.0f, who, true);
			const std::vector<uint8_t> gone = snow_frame(1.0f, st_gone, nullptr, 1.0f, nullptr, true);
			r.unload_rain();
			r.release_old(0);
			const bool up_again = r.upload_rain(snow_map(), up_error);
			const std::vector<uint8_t> fresh = snow_frame(1.0f, st_fresh, nullptr, 1.0f, nullptr, true);
			uint32_t n_layer = 0, n_layer_roofed = 0, n_stand = 0;
			const uint32_t layer_px = differ(layer_half, half, open_side, n_layer), layer_roofed_px = differ(layer_half, half, roofed_near, n_layer_roofed);
			const double flat_whole_mean = open_mean_of(whole), layer_whole_mean = open_mean_of(layer_whole);
			const double near_untrod = mean_near(layer_whole, 0.15f, n_stand), near_trod = mean_near(trod, 0.15f, n_stand),
				near_gone = mean_near(gone, 0.15f, n_stand), near_fresh = mean_near(fresh, 0.15f, n_stand);
			// (with depth, the trail's floor is seen only from above: from 2 yalms up its near wall hides it; the pixels around
			// the place within a yalm and a half that changed)
			auto changed_near = [&](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
				uint32_t k = 0;
				for (UINT y = 0; y < H; ++y)
					for (UINT x = 0; x < W; ++x)
					{
						float gx = 0.0f, gz = 0.0f;
						if (!ground_at(x, y, gx, gz) || (gx - stand_x) * (gx - stand_x) + (gz - stand_z) * (gz - stand_z) > 1.5f * 1.5f)
							continue;
						const uint8_t *c = &a[(size_t(y) * W + x) * 4], *d = &b[(size_t(y) * W + x) * 4];
						k += std::abs((0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]) - (0.3 * d[0] + 0.59 * d[1] + 0.11 * d[2])) > 8.0 ? 1 : 0;
					}
				return k;
			};
			const uint32_t trod_px = changed_near(trod, layer_whole), gone_px = changed_near(gone, layer_whole), fresh_px = changed_near(fresh, layer_whole);
			std::printf("  snow layer: half a layer differs from flat in %u of %u open pixels, %u of %u roofed; a whole layer %.1f (flat %.1f); "
				"where someone stands at %.1f,%.1f (%u pixels): %.1f untrodden, %.1f trodden, %.1f once gone, %.1f in a new zone; "
				"changed within 1.5 yalms: %u trodden, %u once gone, %u in a new zone\n",
				layer_px, n_layer, layer_roofed_px, n_layer_roofed, layer_whole_mean, flat_whole_mean, stand_x, stand_z, n_stand,
				near_untrod, near_trod, near_gone, near_fresh, trod_px, gone_px, fresh_px);
			if (FILE *f = nullptr; fopen_s(&f, "build\\snow_layer_test.ppm", "wb") == 0 && f != nullptr)
			{
				std::fprintf(f, "P6 %u %u 255\n", W, H);
				for (size_t i = 0; i < size_t(W) * H; ++i)
					std::fwrite(&trod[i * 4], 1, 3, f);
				fclose(f);
			}
			check(st_layer_half.snow_layer && st_layer_whole.snow_layer && !st_whole.snow_layer, "the snow layer drawn with its depth, not without");
			check(layer_px > n_layer / 20 && layer_roofed_px == 0, "  half a layer with depth differs from flat on the open ground, not under the roof");
			check(std::fabs(layer_whole_mean - flat_whole_mean) < 0.12 * flat_whole_mean, "  a whole layer with depth about as bright as flat");
			check(stand_seen && n_stand > 20 && st_trod.snow_trails && st_from.footprints == 0 && st_a.footprints + st_b.footprints + st_trod.footprints >= 3 && trod_px > 40,
				"  someone walking through it leaves footprints a stride apart (none where they were first seen)");
			check(gone_px == trod_px, "  the trail stays once they've gone");
			check(up_again && fresh_px == 0 && std::fabs(near_fresh - near_untrod) < 1.0, "  a new zone's snow starts untrodden");
			r.unload_rain();
			r.release_old(0);
			const std::vector<uint8_t> nomap = snow_frame(1.0f, st_nomap);
			static auto roofed_side = [](float ex, float ez, float px_, float) { return ex * ex + ez * ez < 15.0f * 15.0f && px_ > 2.0f; };
			uint32_t n_open = 0, n_roofed = 0;
			const uint32_t open_px = differ(whole, bare, open_side, n_open), roofed_px = differ(whole, bare, roofed_side, n_roofed);
			const uint32_t half_px = differ(half, bare, open_side, n_tmp), nomap_px = differ(nomap, bare, open_side, n_tmp);
			// (how white the open ground went: its mean brightness against the plain colour's)
			double sum = 0.0;
			uint32_t counted = 0;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					float gx = 0.0f, gz = 0.0f;
					if (!ground_at(x, y, gx, gz) || !open_side(gx - eye.x, gz - eye.z, gx - at.x, gz - at.z))
						continue;
					const uint8_t *c = &whole[(size_t(y) * W + x) * 4];
					sum += 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
					counted++;
				}
			const double open_mean = counted != 0 ? sum / counted : 0.0, plain_mean = 0.3 * 0.45 * 255 + 0.59 * 0.42 * 255 + 0.11 * 0.36 * 255;
			std::printf("  settled snow: open ground near the camera %u of %u pixels changed (brightness %.1f, went in %.1f), half a layer %u; "
				"under the roof %u of %u; none: shading pass %s; no rain map: %u\n",
				open_px, n_open, open_mean, plain_mean, half_px, roofed_px, n_roofed, st_none.shaded ? "ran" : "didn't run", nomap_px);
			if (FILE *f = nullptr; fopen_s(&f, "build\\snow_cover_test.ppm", "wb") == 0 && f != nullptr)
			{
				std::fprintf(f, "P6 %u %u 255\n", W, H);
				for (size_t i = 0; i < size_t(W) * H; ++i)
					std::fwrite(&whole[i * 4], 1, 3, f);
				fclose(f);
			}
			check(up_snow && st_whole.snow_cover && !st_none.snow_cover && !st_nomap.snow_cover, "settled snow drawn with its cover and the rain map, not without");
			check(n_open > 5000 && open_px > n_open * 9 / 10 && open_mean > plain_mean + 60.0, "a whole layer whitens the open ground (lit as the ground was)");
			check(n_roofed > 2000 && roofed_px == 0, "  and leaves the ground under a roof as drawn");
			check(half_px > n_open / 10 && half_px < open_px * 9 / 10, "  half a layer lies in patches over part of it");
			check(nomap_px == 0, "  none without the rain map");
			check(dark.right != 0 && st_dark.snow_cover && ring_px > 2000 && ghost_px == 0, "  a dark patch on it leaves no copies of itself on the snow around");
			// (the floor's light under the snow from a median: hard black joints a few pixels apart at a grazing view can still
			// fill most of its ring and leave a speck; the mean of a small ring before left every joint showing, 229 darker)
			check(seam_px > 300 && seam_dark_px * 20 < seam_px && seam_diff / std::max(1u, seam_px) < 15.0,
				"  a floor's thin dark seams don't show through a whole layer (the snow over them as bright as beside them)");
		}
		check(g.report_messages() == 0, "no debug-layer errors (rain)");
	}

	// 1t. A real rain map, packed as tools/bake_rain.py packs it (Windows' XPRESS): Jugner Forest's (build\testdata\rain\104.bin,
	// from tools/bake_all.py build\rain --what rain) unpacks whole, says what it covers, and has the lake where the
	// game's own rain ripples on it are placed (its rain folder's 'hamo' generators, at y 6.1 around (-97, 391))
	{
		rain_file jugner;
		std::string why;
		if (read_rain_file(L"build\\testdata\\rain\\104.bin", jugner, why))
		{
			uint32_t covered = 0;
			for (uint8_t k : jugner.kinds)
				covered += k != kRainNothing ? 1u : 0u;
			const uint32_t width = jugner.width, height = jugner.height, said = jugner.covered;
			const bool up = r.upload_rain(std::move(jugner), why);
			float ly = 0.0f;
			uint8_t lk = 0;
			const bool lake = r.rain_surface(-97.2f, 391.5f, ly, lk);
			std::printf("  Jugner's rain map: %u x %u squares, %u with a surface (the file says %u); at the lake: %s at y %.2f\n", width, height, covered, said,
				lk == kRainWater ? "water" : lk == kRainGround ? "ground" : lk == kRainStops ? "something stopping it" : "nothing", ly);
			check(up && covered == said && covered > 1000000, "a packed rain map read whole (Windows' XPRESS)");
			check(lake && lk == kRainWater && std::fabs(ly - 6.1f) < 1.5f, "Jugner's lake is water in it, where the game's rain ripples sit");
			r.unload_rain();
			r.release_old(0);
		}
		else
			std::printf("  (build\\testdata\\rain\\104.bin: %s: the packed rain map not checked)\n", why.c_str());
	}

	// 1w. The compact shadow file (SHD2: each model once and the zone's placements, packed), as the add-on reads it,
	// holds what the triangle soup (SHD1) of the same zone holds: the same cells in the same order, every triangle
	// with its cut-out word, at the same place (within a millimetre: the placements are kept as floats), the same
	// cut-out masks. Both are written by tests\t_bake_shadow.py (build\test\shd1_<zone>.bin, shd2_<zone>.bin and a
	// stored, unpacked shd2s_<zone>.bin); a damaged one is refused
	for (const wchar_t *zone : { L"100", L"104" })
	{
		const std::wstring dir = L"build\\test\\";
		shadow_file soup, compact, stored;
		std::string why1, why2, why3;
		const bool r1 = read_shadow_file((dir + L"shd1_" + zone + L".bin").c_str(), soup, why1);
		if (!r1)
		{
			std::printf("  (build\\test\\shd1_%ls.bin: %s: run tests\\t_bake_shadow.py; the compact shadow file not checked)\n", zone, why1.c_str());
			continue;
		}
		const auto t0 = std::chrono::steady_clock::now();
		const bool r2 = read_shadow_file((dir + L"shd2_" + zone + L".bin").c_str(), compact, why2);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		const bool r3 = read_shadow_file((dir + L"shd2s_" + zone + L".bin").c_str(), stored, why3);
		bool cells_same = r2 && soup.cells.size() == compact.cells.size();
		for (size_t c = 0; cells_same && c < soup.cells.size(); ++c)
			cells_same = soup.cells[c].cx == compact.cells[c].cx && soup.cells[c].cz == compact.cells[c].cz &&
				soup.cells[c].first == compact.cells[c].first && soup.cells[c].count == compact.cells[c].count;
		bool words_same = r2 && soup.triangles.size() == compact.triangles.size();
		float most = 0.0f;
		for (size_t i = 0; words_same && i < soup.triangles.size(); i += 4)
		{
			words_same = soup.triangles[i + 3] == compact.triangles[i + 3];
			for (int a = 0; a < 3; ++a)
			{
				float x, y;
				std::memcpy(&x, &soup.triangles[i + a], 4);
				std::memcpy(&y, &compact.triangles[i + a], 4);
				most = std::max(most, std::fabs(x - y));
			}
		}
		const bool masks_same = r2 && soup.masks == compact.masks && soup.words == compact.words;
		const bool stored_same = r3 && stored.triangles == compact.triangles && stored.cells.size() == compact.cells.size() && stored.masks == compact.masks;
		std::printf("  zone %ls: soup %u triangles in %zu cells; compact read and expanded in %.0f ms: %u triangles in %zu cells, at most %.5f yalms off\n",
			zone, soup.triangle_count(), soup.cells.size(), ms, compact.triangle_count(), compact.cells.size(), most);
		check(r2 && cells_same, "a compact shadow file expands into the soup's cells, in its order");
		check(words_same && most < 1e-3f, "every triangle at the same place with the same cut-out word");
		check(masks_same, "the same cut-out masks");
		check(stored_same, "an unpacked (stored) compact file reads the same");
		// The same file with one placement naming a model that isn't there: refused, nothing kept
		std::vector<uint8_t> bytes;
		if (FILE *f = nullptr; _wfopen_s(&f, (dir + L"shd2s_" + zone + L".bin").c_str(), L"rb") == 0 && f != nullptr)
		{
			fseek(f, 0, SEEK_END);
			bytes.resize(size_t(ftell(f)));
			fseek(f, 0, SEEK_SET);
			bytes.resize(fread(bytes.data(), 1, bytes.size(), f));
			fclose(f);
		}
		if (bytes.size() > 64)
		{
			uint32_t models = 0, model_tris = 0;
			std::memcpy(&models, bytes.data() + 12, 4);
			std::memcpy(&model_tris, bytes.data() + 16, 4);
			const size_t first_placement = 64 + size_t(models) * 8 + size_t(model_tris) * 48;
			const uint32_t bad = models + 5;
			std::memcpy(bytes.data() + first_placement, &bad, 4);
			const std::wstring broken = dir + L"shd2_broken.bin";
			if (FILE *f = nullptr; _wfopen_s(&f, broken.c_str(), L"wb") == 0 && f != nullptr)
			{
				fwrite(bytes.data(), 1, bytes.size(), f);
				fclose(f);
			}
			shadow_file refused;
			std::string why;
			check(!read_shadow_file(broken.c_str(), refused, why) && why == "the file is damaged" && refused.triangles.empty(),
				"a compact file naming a model it doesn't hold is refused");
		}
	}

	// 1u. Snow (grass off), on 1s's rain map of flat ground (the camera 2 yalms over it). By name, snow and blizzards
	// snow (0.6 and 1, the game's second names too), blended across a change; rain, thunder and fine weather don't,
	// and snow doesn't rain. Snowing, flakes show over the ground near the camera (against the same frame without
	// snow), as round soft dots (the changed pixels' blobs: most no longer than they're wide, as the rain's streaks
	// are); under a roof (1s's) none at all; with only rain or at 'fine', none drawn. In a blizzard blowing away from the
	// camera under an awning (a roof over the camera, ending 3 yalms ahead), none in its lee just past its edge (a
	// flake there came down through it), though they show there without the awning and further off with it.
	{
		weather changing;
		changing.known = true;
		changing.name = weather_name("bliz");
		changing.before = weather_name("snow");
		changing.change = 0.5f;
		check(weather_snowfall(weather_name("snow")) == 0.6f && weather_snowfall(weather_name("sno1")) == 0.6f &&
			weather_snowfall(weather_name("bliz")) == 1.0f && weather_snowfall(weather_name("bli1")) == 1.0f &&
			std::fabs(weather_snowfall(changing) - 0.8f) < 1e-6f, "snow by name: snow 0.6, blizzards 1 (and the second names), blended across a change");
		check(weather_snowfall(weather_name("rain")) == 0.0f && weather_snowfall(weather_name("thdr")) == 0.0f && weather_snowfall(weather_name("fine")) == 0.0f &&
			weather_downpour(weather_name("snow")) == 0.0f && weather_downpour(weather_name("bliz")) == 0.0f, "no snow in rain, thunder or fine weather; no rain in snow");
		check(weather_slant(weather_name("bliz")) >= 1.0f && weather_slant(weather_name("snow")) < 0.3f && weather_sway(weather_name("snow")) > weather_sway(weather_name("bliz")),
			"blizzards blown hard across, snow gently (its flakes sway more)");

		const float plain[4] = { 0.45f, 0.42f, 0.36f, 0.5f };
		// (the look direction across the ground, from the camera to the looked-at blade, and across it)
		const float fx = (at.x - eye.x) / 5.0f, fz = (at.z - eye.z) / 5.0f;
		enum roof_kind { in_the_open, under_roof, under_awning };
		auto map = [&](roof_kind kind) {
			rain_file m;
			m.zone = 111;
			m.square = 0.5f;
			m.x0 = at.x - 60.0f;
			m.z0 = at.z - 60.0f;
			m.width = m.height = 240;
			m.y0 = at.y - 10.0f;
			m.step = 1.0f / 16.0f;
			m.q.assign(size_t(m.width) * m.height, uint16_t(10.0f / m.step));
			m.kinds.assign(m.q.size(), kRainGround);
			for (uint32_t j = 0; j < m.height; ++j)
				for (uint32_t i = 0; i < m.width; ++i)
				{
					const float x = m.x0 + (i + 0.5f) * m.square - eye.x, z = m.z0 + (j + 0.5f) * m.square - eye.z;
					const float ahead = x * fx + z * fz, side = -x * fz + z * fx;
					const bool covered = kind == under_roof ? std::fabs(x) < 20.0f && std::fabs(z) < 20.0f : kind == under_awning && ahead < 3.0f && std::fabs(side) < 20.0f;
					if (covered)
						m.q[size_t(j) * m.width + i] = uint16_t(7.0f / m.step); // (y 3 over the ground: y is down)
				}
			m.covered = m.width * m.height;
			return m;
		};
		auto frame = [&](float rain, float snow, float slant, draw_stats &st) {
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.grass = false;
			l2.ao_strength = 0.0f;
			l2.relief = 0.0f;
			l2.sun_shadow = 0.0f;
			l2.fireflies = 0.0f;
			l2.water = false;
			l2.glint = 0.0f;
			l2.time = 100.0f;
			l2.rain = rain;
			l2.rain_splashes = 0.0f;
			l2.rain_light[0] = l2.rain_light[1] = l2.rain_light[2] = 0.9f;
			l2.rain_slant[0] = slant * fx; // (blowing the way the camera looks)
			l2.rain_slant[1] = slant * fz;
			l2.snow = snow;
			l2.snow_sway = 0.3f;
			l2.snow_light[0] = l2.snow_light[1] = l2.snow_light[2] = 0.95f;
			st = r.draw(g.list, t, cam, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			return g.read(world, 4);
		};
		// (the ground a pixel looks at, as 1s finds it: false above the horizon)
		float inv[16];
		invert(vp, inv);
		auto ground_at = [&](UINT x, UINT y, float &gx, float &gz) {
			const float nx = (x + 0.5f) / W * 2.0f - 1.0f, ny = 1.0f - (y + 0.5f) / H * 2.0f;
			float p[2][3];
			for (int k = 0; k < 2; ++k)
			{
				const float c[4] = { nx, ny, k == 0 ? 0.5f : 0.9f, 1.0f };
				float o[4] = {};
				for (int col = 0; col < 4; ++col)
					o[col] = c[0] * inv[col] + c[1] * inv[4 + col] + c[2] * inv[8 + col] + c[3] * inv[12 + col];
				for (int a = 0; a < 3; ++a)
					p[k][a] = o[a] / o[3];
			}
			const float dy = p[1][1] - p[0][1];
			if (std::fabs(dy) < 1e-6f)
				return false;
			const float s = (at.y - p[0][1]) / dy;
			gx = p[0][0] + (p[1][0] - p[0][0]) * s;
			gz = p[0][2] + (p[1][2] - p[0][2]) * s;
			return (gx - eye.x) * (p[1][0] - p[0][0]) + (gz - eye.z) * (p[1][2] - p[0][2]) > 0.0f;
		};
		auto changed_at = [&](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, size_t k) {
			return std::abs(int(a[k]) - int(b[k])) > 2 || std::abs(int(a[k + 1]) - int(b[k + 1])) > 2 || std::abs(int(a[k + 2]) - int(b[k + 2])) > 2;
		};
		// (pixels looking at the ground where(ahead, side) says, of the camera: how many, how many changed)
		auto differ = [&](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, bool (*where)(float, float), uint32_t &n) {
			uint32_t changed = 0;
			n = 0;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					float gx = 0.0f, gz = 0.0f;
					if (!ground_at(x, y, gx, gz))
						continue;
					const float dx = gx - eye.x, dz = gz - eye.z;
					if (!where(dx * fx + dz * fz, -dx * fz + dz * fx))
						continue;
					n++;
					changed += changed_at(a, b, (size_t(y) * W + x) * 4) ? 1 : 0;
				}
			return changed;
		};
		// (the changed pixels' blobs of 6 or more, 4-connected: how many, and the share of them that are round, their
		// longest spread at most 1.8 times their shortest: from the second moments, so a slanted streak isn't round)
		auto round_share = [&](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, uint32_t &blobs) {
			std::vector<uint8_t> mask(size_t(W) * H);
			for (size_t i = 0; i < mask.size(); ++i)
				mask[i] = changed_at(a, b, i * 4) ? 1 : 0;
			std::vector<uint32_t> todo;
			uint32_t round = 0;
			blobs = 0;
			for (size_t s = 0; s < mask.size(); ++s)
			{
				if (mask[s] != 1)
					continue;
				double n = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0;
				mask[s] = 2;
				todo.assign(1, uint32_t(s));
				while (!todo.empty())
				{
					const uint32_t k = todo.back();
					todo.pop_back();
					const double x = k % W, y = k / W;
					n += 1.0;
					sx += x;
					sy += y;
					sxx += x * x;
					syy += y * y;
					sxy += x * y;
					const uint32_t kx = k % W, ky = k / W;
					const uint32_t next[4] = { kx > 0 ? k - 1 : k, kx + 1 < W ? k + 1 : k, ky > 0 ? k - W : k, ky + 1 < H ? k + W : k };
					for (uint32_t q : next)
						if (mask[q] == 1)
						{
							mask[q] = 2;
							todo.push_back(q);
						}
				}
				if (n < 6.0)
					continue;
				blobs++;
				const double cxx = sxx / n - (sx / n) * (sx / n) + 1.0 / 12.0, cyy = syy / n - (sy / n) * (sy / n) + 1.0 / 12.0, cxy = sxy / n - (sx / n) * (sy / n);
				const double mid = 0.5 * (cxx + cyy), spread = std::sqrt(0.25 * (cxx - cyy) * (cxx - cyy) + cxy * cxy);
				round += mid + spread <= 1.8 * 1.8 * (mid - spread) ? 1 : 0;
			}
			return blobs != 0 ? float(round) / float(blobs) : 0.0f;
		};
		auto near_ground = [](float ahead, float side) { return ahead * ahead + side * side < 15.0f * 15.0f; };
		auto lee = [](float ahead, float side) { return ahead > 3.6f && ahead < 4.6f && std::fabs(side) < 6.0f; };
		auto past_lee = [](float ahead, float side) { return ahead > 8.0f && ahead < 14.0f && std::fabs(side) < 6.0f; };

		draw_stats st_dry, st_snow, st_rain, st_roof_dry, st_roof, st_rain_only, st_fine, st_bliz_open, st_bliz_awning, st_awning_dry;
		std::string up_error;
		const bool up_open = r.upload_rain(map(in_the_open), up_error);
		const std::vector<uint8_t> dry = frame(0.0f, 0.0f, 0.12f, st_dry), snowing = frame(0.0f, 1.0f, 0.12f, st_snow),
			raining = frame(1.0f, 0.0f, 0.12f, st_rain), fine = frame(0.0f, 0.0f, 0.0f, st_fine), bliz_open = frame(0.0f, 2.0f, 1.4f, st_bliz_open);
		const bool up_roof = r.upload_rain(map(under_roof), up_error);
		const std::vector<uint8_t> roof_dry = frame(0.0f, 0.0f, 0.12f, st_roof_dry), roofed = frame(0.0f, 1.0f, 0.12f, st_roof);
		const bool up_awning = r.upload_rain(map(under_awning), up_error);
		const std::vector<uint8_t> awning_dry = frame(0.0f, 0.0f, 1.4f, st_awning_dry), bliz_awning = frame(0.0f, 2.0f, 1.4f, st_bliz_awning);
		r.unload_rain();
		r.release_old(0);
		uint32_t n_near = 0, n_tmp = 0, n_lee = 0, n_past = 0, snow_blobs = 0, rain_blobs = 0;
		const uint32_t snow_px = differ(snowing, dry, near_ground, n_near), roof_px = differ(roofed, roof_dry, near_ground, n_tmp);
		const uint32_t rain_flakes = st_rain.snow_flakes, fine_px = differ(fine, dry, near_ground, n_tmp);
		const uint32_t lee_open_px = differ(bliz_open, dry, lee, n_lee), lee_px = differ(bliz_awning, awning_dry, lee, n_tmp);
		const uint32_t past_px = differ(bliz_awning, awning_dry, past_lee, n_past);
		const float snow_round = round_share(snowing, dry, snow_blobs), rain_round = round_share(raining, dry, rain_blobs);
		std::printf("  snow: %u flake places drawn; of %u pixels looking at the ground within 15 yalms, %u changed by it; blobs: %u, %.0f%% round "
			"(rain's streaks: %u, %.0f%% round); under a roof: %u changed (flakes %u); with only rain: %u flakes; at 'fine': %u changed (flakes %u)\n",
			st_snow.snow_flakes, n_near, snow_px, snow_blobs, snow_round * 100.0f, rain_blobs, rain_round * 100.0f, roof_px, st_roof.snow_flakes,
			rain_flakes, fine_px, st_fine.snow_flakes);
		std::printf("  blizzard blowing away from the camera: of the %u pixels of ground just past an awning's edge, %u changed in the open, %u with the "
			"awning (its lee); further off (%u pixels) %u%s%s\n", n_lee, lee_open_px, lee_px, n_past, past_px, error.empty() ? "" : "; ", error.c_str());
		// (a picture of it: build\snow_test.ppm)
		if (FILE *f = nullptr; fopen_s(&f, "build\\snow_test.ppm", "wb") == 0 && f != nullptr)
		{
			std::fprintf(f, "P6 %u %u 255\n", W, H);
			for (size_t i = 0; i < size_t(W) * H; ++i)
				std::fwrite(&snowing[i * 4], 1, 3, f);
			fclose(f);
		}
		check(up_open && up_roof && up_awning, "rain maps loaded for the snow");
		check(st_snow.snow_flakes > 0 && st_snow.rain_drops == 0 && st_snow.splashes == 0 && n_near > 20000, "snowing: flakes drawn (no drops, no splashes), the ground near the camera on screen");
		check(snow_px > 150, "flakes show over the ground near the camera");
		check(snow_blobs >= 10 && snow_round >= 0.75f && snow_round > rain_round + 0.3f, "round soft flakes, not streaks (most blobs round, the rain's not)");
		check(st_roof.snow_flakes > 0 && roof_px == 0, "under a roof: no flake seen at all (they end on it)");
		check(rain_flakes == 0 && st_rain.rain_drops > 0, "only rain: no snow drawn");
		check(st_fine.snow_flakes == 0 && st_fine.rain_drops == 0 && fine_px == 0 && !st_fine.bound, "weather 'fine': no snow drawn, the picture as it was");
		check(n_lee > 300 && lee_open_px > 0 && lee_px == 0 && past_px > 0, "a blizzard leaves the lee of an awning clear (flakes there in the open, and further off)");
		check(g.report_messages() == 0, "no debug-layer errors (snow)");

		// Leaves in gales (the weather of its own's, on the open map again): blown the way the camera looks, low over the
		// ground near it; none drawn without them
		{
			std::string leaf_why;
			const bool up_leaves = r.upload_rain(map(in_the_open), leaf_why);
			auto leaf_frame = [&](float leaves, draw_stats &st) {
				barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
				g.list->ClearRenderTargetView(world_rtv, plain, 0, nullptr);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
				ground_depth(cam);
				barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				look l2 = l;
				l2.grass = false;
				l2.ao_strength = l2.relief = 0.0f;
				l2.sun_shadow = 0.0f;
				l2.fireflies = 0.0f;
				l2.water = false;
				l2.time = 100.0f;
				l2.wx.wind[0] = fx;
				l2.wx.wind[1] = fz;
				l2.wx.wind_strength = 2.0f;
				l2.wx.ground_y = at.y;
				l2.wx.leaves = leaves;
				l2.wx.leaf_light[0] = l2.wx.leaf_light[1] = l2.wx.leaf_light[2] = 0.9f;
				st = r.draw(g.list, t, cam, l2, error);
				barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
				return g.read(world, 4);
			};
			draw_stats st_still, st_leaves;
			const std::vector<uint8_t> still = leaf_frame(0.0f, st_still), leafy = leaf_frame(1.0f, st_leaves);
			r.unload_rain();
			r.release_old(0);
			uint32_t n_leaf_ground = 0;
			const uint32_t leaf_px = differ(leafy, still, near_ground, n_leaf_ground);
			std::printf("  leaves in gales: %u leaf places drawn, %u of %u pixels of the ground within 15 yalms changed; without them %u places\n",
				st_leaves.leaves, leaf_px, n_leaf_ground, st_still.leaves);
			if (FILE *f = nullptr; fopen_s(&f, "build\\leaves_test.ppm", "wb") == 0 && f != nullptr)
			{
				std::fprintf(f, "P6 %u %u 255\n", W, H);
				for (size_t i = 0; i < size_t(W) * H; ++i)
					std::fwrite(&leafy[i * 4], 1, 3, f);
				fclose(f);
			}
			check(up_leaves && st_leaves.leaves > 0 && st_still.leaves == 0 && leaf_px > 30, "leaves blown along the ground in gales (none without them)");
			check(g.report_messages() == 0, "no debug-layer errors (leaves)");
		}
	}

	}

	// 1x. A sky of its own (grass off, the shading pass on for it alone). The game's dome drawn here as the game draws it
	// (client-notes "Sky", 03D640: West Ronfaure's sunshine record at noon, its 8 stops and 10 segments; strips of
	// vertex colours cut to whole bytes, Gouraud, no depth written), the camera 2 yalms over flat ground looking 20 degrees
	// up; an opaque cloud drawn over part of it, and a pole in front of it (its depth near, its colour the dome's).
	// Expected: the shader's rebuilt dome matches the strips drawn; the sky the zone left comes out as the add-on's smooth
	// gradient (within a byte of it, the stops' colours at their heights, rows following the curve to a third of a byte:
	// no steps of one), nothing else changes (the cloud, the pole, the ground); a sun disc where the light points, its glow
	// fading away from it, none over the cloud; stars by night (21:00's record, 16 segments), none with them off; no
	// dome in the record (segments 0) or the sky off: not drawn
	{
		const float noon_colours[8][3] = { { 154, 185, 218 }, { 103, 149, 218 }, { 85, 120, 191 }, { 61, 72, 173 }, { 45, 51, 125 }, { 45, 49, 125 }, { 45, 49, 125 }, { 45, 49, 125 } };
		const float noon_heights[8] = { 0.0f, 0.086f, 0.184f, 0.307f, 0.456f, 0.623f, 0.796f, 1.0f };
		const float night_colours[8][3] = { { 71, 66, 111 }, { 53, 41, 86 }, { 30, 29, 56 }, { 24, 28, 50 }, { 20, 22, 44 }, { 18, 19, 46 }, { 18, 19, 46 }, { 18, 19, 46 } };
		const float night_heights[8] = { 0.0f, 0.07f, 0.206f, 0.342f, 0.504f, 0.671f, 0.833f, 1.0f };
		auto dome_of = [](const float (&c)[8][3], const float (&h)[8], uint32_t segments) {
			look::sky_dome d;
			d.on = true;
			d.segments = segments;
			for (int i = 0; i < 8; ++i)
			{
				for (int k = 0; k < 3; ++k)
					d.stops[i][k] = c[i][k];
				d.stops[i][3] = h[i];
			}
			for (int k = 0; k < 3; ++k)
				d.clear[k] = c[0][k];
			return d;
		};

		// The game's dome pipeline: position and a byte colour a vertex (D3DCOLOR's), Gouraud, triangle strips, no depth
		const char kDomeShader[] = R"(
cbuffer c : register(b0) { row_major float4x4 vp; };
struct vin { float3 p : POSITION; float4 c : COLOR; };
struct vout { float4 pos : SV_Position; float4 c : COLOR; };
vout vs_main(vin i) { vout o; o.pos = mul(float4(i.p, 1.0), vp); o.c = i.c; return o; }
float4 ps_main(vout i) : SV_Target { return i.c; }
)";
		ID3D12RootSignature *dome_root = nullptr;
		ID3D12PipelineState *dome_pso = nullptr;
		{
			D3D12_ROOT_PARAMETER dp = {};
			dp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			dp.Constants = { 0, 0, 16 };
			D3D12_ROOT_SIGNATURE_DESC drs = { 1, &dp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
			ID3DBlob *blob = nullptr, *err = nullptr;
			reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))(&drs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
			g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&dome_root));
			release(blob);
			release(err);
			const std::vector<uint8_t> vs = compile(kDomeShader, "vs_main", "vs_5_0"), ps = compile(kDomeShader, "ps_main", "ps_5_0");
			const D3D12_INPUT_ELEMENT_DESC layout[2] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
			d.pRootSignature = dome_root;
			d.VS = { vs.data(), vs.size() };
			d.PS = { ps.data(), ps.size() };
			d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			d.SampleMask = UINT_MAX;
			d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			d.RasterizerState.DepthClipEnable = TRUE;
			d.InputLayout = { layout, 2 };
			d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			d.NumRenderTargets = 1;
			d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
			d.SampleDesc.Count = 1;
			check(SUCCEEDED(g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&dome_pso))), "the game's sky dome pipeline (for the test)");
		}
		// Its strips as 03D640 makes them: for each band, its strips (dome_strips), each a ring of 2 x segments + 2
		// vertices alternating the lower and upper edge, the first pair again at the end; radius 0.8 x (65535 - 0.1)
		struct dome_vertex
		{
			float x, y, z;
			uint8_t c[4];
		};
		ID3D12Resource *dome_vb = g.buffer(D3D12_HEAP_TYPE_UPLOAD, 16 * 64 * 40 * 34, D3D12_RESOURCE_STATE_GENERIC_READ);
		auto draw_dome = [&](const look::sky_dome &d, const camera &c) {
			std::vector<dome_vertex> v;
			std::vector<std::pair<UINT, UINT>> strips;
			const float radius = 0.8f * (65535.0f - 0.1f);
			for (int i = 0; i < 7; ++i)
			{
				const float h0 = d.stops[i][3], h1 = d.stops[i + 1][3];
				const uint32_t n = dome_strips(h0, h1);
				for (uint32_t m = 0; m < n; ++m)
				{
					const float f1 = float(m) / float(n), f2 = float(m + 1) / float(n);
					const float e1 = (f1 * h1 + (1.0f - f1) * h0) * 1.570796f, e2 = (f2 * h1 + (1.0f - f2) * h0) * 1.570796f;
					dome_vertex lo = {}, hi = {};
					for (int k = 0; k < 3; ++k)
					{
						lo.c[k] = uint8_t(std::floor(d.stops[i][k] * (1.0f - f1)) + std::floor(d.stops[i + 1][k] * f1));
						hi.c[k] = uint8_t(std::floor(d.stops[i][k] * (1.0f - f2)) + std::floor(d.stops[i + 1][k] * f2));
					}
					lo.c[3] = hi.c[3] = 128;
					const UINT first = UINT(v.size());
					for (uint32_t k = 0; k <= d.segments; ++k)
					{
						const float a = k == 0 || k == d.segments ? 0.0f : float(k) * 6.283f / float(d.segments);
						lo.x = radius * std::cos(e1) * std::sin(a), lo.y = -radius * std::sin(e1), lo.z = radius * std::cos(e1) * std::cos(a);
						hi.x = radius * std::cos(e2) * std::sin(a), hi.y = -radius * std::sin(e2), hi.z = radius * std::cos(e2) * std::cos(a);
						v.push_back(lo);
						v.push_back(hi);
					}
					strips.push_back({ first, UINT(v.size()) - first });
				}
			}
			void *p = nullptr;
			dome_vb->Map(0, nullptr, &p);
			std::memcpy(p, v.data(), v.size() * sizeof(dome_vertex));
			dome_vb->Unmap(0, nullptr);
			float view0[16];
			std::memcpy(view0, c.view, sizeof(view0));
			view0[12] = view0[13] = view0[14] = 0.0f; // (centred on the camera: the view's translation zeroed)
			float dvp[16];
			multiply(view0, c.proj, dvp);
			const D3D12_VERTEX_BUFFER_VIEW vbv = { dome_vb->GetGPUVirtualAddress(), UINT(v.size() * sizeof(dome_vertex)), UINT(sizeof(dome_vertex)) };
			const D3D12_VIEWPORT dv = { 0.0f, 0.0f, float(W), float(H), 0.0f, 1.0f };
			const D3D12_RECT ds = { 0, 0, LONG(W), LONG(H) };
			g.list->SetGraphicsRootSignature(dome_root);
			g.list->SetPipelineState(dome_pso);
			g.list->SetGraphicsRoot32BitConstants(0, 16, dvp, 0);
			g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			g.list->IASetVertexBuffers(0, 1, &vbv);
			g.list->OMSetRenderTargets(1, &world_rtv, FALSE, nullptr);
			g.list->RSSetViewports(1, &dv);
			g.list->RSSetScissorRects(1, &ds);
			for (const auto &s : strips)
				g.list->DrawInstanced(s.second, 1, s.first, 0);
		};

		// The camera: 2 yalms over the ground, looking 20 degrees up toward a bearing of 37 degrees (between the facets'
		// middles: the facets' shape matters)
		const vec3 sky_eye = { at.x, at.y - 2.0f, at.z };
		const float up20 = 20.0f * 3.14159265f / 180.0f;
		camera sky_cam;
		look_at(sky_eye, { sky_eye.x + 6.0f * std::cos(up20), sky_eye.y - 10.0f * std::sin(up20), sky_eye.z + 8.0f * std::cos(up20) }, { 0.0f, -1.0f, 0.0f }, sky_cam.view);
		std::memcpy(sky_cam.proj, cam.proj, sizeof(sky_cam.proj));
		// Each pixel's view ray in the world (y down), as the shader makes it
		auto ray_of = [&](float x, float y) {
			const float nx = x / W * 2.0f - 1.0f, ny = 1.0f - y / H * 2.0f;
			const float vx = nx / sky_cam.proj[0], vy = ny / sky_cam.proj[5], vz = sky_cam.proj[11];
			const float *m = sky_cam.view;
			return normalize({ m[0] * vx + m[1] * vy + m[2] * vz, m[4] * vx + m[5] * vy + m[6] * vz, m[8] * vx + m[9] * vy + m[10] * vz });
		};
		const D3D12_RECT cloud = { LONG(W * 0.18f), LONG(H * 0.12f), LONG(W * 0.38f), LONG(H * 0.3f) };
		const D3D12_RECT pole = { LONG(W * 0.5f) + 40, LONG(H * 0.05f), LONG(W * 0.5f) + 52, LONG(H * 0.5f) };
		const float cloud_colour[4] = { 0.9f, 0.91f, 0.93f, 0.5f };
		auto in = [](const D3D12_RECT &r, UINT x, UINT y) { return LONG(x) >= r.left && LONG(x) < r.right && LONG(y) >= r.top && LONG(y) < r.bottom; };
		// A frame: the game's dome (and its clear colour, the ground's depth, the cloud, the pole), its picture before
		// the add-on, then the add-on's shading pass with the sky on
		auto sky_frame = [&](const look::sky_dome &d, const look::sky_dome &painted, const float *sun, std::vector<uint8_t> &before, draw_stats &st, const D3D12_RECT &cloud_at) {
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			const float clear[4] = { d.clear[0] / 255.0f, d.clear[1] / 255.0f, d.clear[2] / 255.0f, 0.5f };
			g.list->ClearRenderTargetView(world_rtv, clear, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(sky_cam);
			draw_dome(d, sky_cam);
			g.list->ClearRenderTargetView(world_rtv, cloud_colour, 1, &cloud_at);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f - 0.1f / 20.0f, 0, 1, &pole);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			before = g.read(world, 4);
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look ls = l;
			ls.grass = false;
			ls.ao_strength = ls.relief = 0.0f;
			ls.sun_shadow = 0.0f;
			ls.fireflies = 0.0f;
			ls.water = ls.water_debug = false;
			ls.dome = painted;
			ls.time = 12.5f;
			if (sun != nullptr)
				for (int i = 0; i < 3; ++i)
					ls.sun[i] = sun[i];
			st = r.draw(g.list, t, sky_cam, ls, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			return g.read(world, 4);
		};

		// Noon, no sun disc: the gradient alone
		const look::sky_dome noon = dome_of(noon_colours, noon_heights, 10);
		std::vector<uint8_t> noon_before;
		draw_stats st_noon;
		const std::vector<uint8_t> noon_after = sky_frame(noon, noon, nullptr, noon_before, st_noon, cloud);
		double rebuilt_worst = 0.0, rebuilt_sum = 0.0, painted_worst = 0.0;
		uint32_t rebuilt_n = 0, rebuilt_over = 0, repainted = 0, changed_off_sky = 0, changed_cloud = 0, changed_pole = 0;
		double stop_sum[8][3] = {}, stop_n[8] = {};
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const vec3 w = ray_of(x + 0.5f, y + 0.5f);
				const float dir[3] = { w.x, w.y, w.z };
				const float el = std::asin(std::clamp(-w.y, -1.0f, 1.0f));
				const uint8_t *b0 = &noon_before[(size_t(y) * W + x) * 4], *a0 = &noon_after[(size_t(y) * W + x) * 4];
				const bool changed = std::memcmp(b0, a0, 4) != 0;
				if (in(pole, x, y))
				{
					changed_pole += changed ? 1 : 0;
					continue;
				}
				if (in(cloud, x, y))
				{
					changed_cloud += changed ? 1 : 0;
					continue;
				}
				if (el <= 0.0f)
				{
					changed_off_sky += changed ? 1 : 0;
					continue;
				}
				repainted += changed ? 1 : 0;
				float game[3], smooth[3];
				dome_colour(noon, dir, game);
				sky_gradient(noon, el / 1.5707963f, smooth);
				for (int k = 0; k < 3; ++k)
				{
					const double off = std::fabs(b0[k] - game[k]), painted_off = std::fabs(a0[k] - smooth[k]);
					rebuilt_worst = std::max(rebuilt_worst, off);
					rebuilt_sum += off;
					rebuilt_over += off > 3.0 ? 1 : 0;
					painted_worst = std::max(painted_worst, painted_off);
				}
				rebuilt_n += 3;
				// (the stops: pixels whose ray is within 0.06 degrees of a stop's height)
				for (int i = 1; i < 8; ++i)
					if (std::fabs(el / 1.5707963f - noon.stops[i][3]) < 0.06f / 90.0f)
					{
						for (int k = 0; k < 3; ++k)
							stop_sum[i][k] += a0[k];
						stop_n[i] += 1.0;
					}
			}
		std::printf("  sky: the game's dome drawn vs rebuilt by the shader's rule: within %.2f of a byte at worst (%.3f on average), %u values over 3\n",
			rebuilt_worst, rebuilt_sum / std::max(1u, rebuilt_n), rebuilt_over);
		std::printf("  sky: repainted %u pixels, within %.2f of a byte of the smooth gradient at worst; changed off the sky %u, in the cloud %u, on the pole %u\n",
			repainted, painted_worst, changed_off_sky, changed_cloud, changed_pole);
		double stop_worst = 0.0;
		int stops_seen = 0;
		for (int i = 1; i < 8; ++i)
			if (stop_n[i] >= 20.0)
			{
				stops_seen++;
				for (int k = 0; k < 3; ++k)
					stop_worst = std::max(stop_worst, std::fabs(stop_sum[i][k] / stop_n[i] - noon.stops[i][k]));
				std::printf("  sky: at stop %d's height (%.1f degrees): %.1f %.1f %.1f over %.0f pixels (the record's %.0f %.0f %.0f)\n", i, noon.stops[i][3] * 90.0f,
					stop_sum[i][0] / stop_n[i], stop_sum[i][1] / stop_n[i], stop_sum[i][2] / stop_n[i], stop_n[i], noon.stops[i][0], noon.stops[i][1], noon.stops[i][2]);
			}
		// Rows along the middle (32 columns each, away from the pole and the cloud): their mean against the smooth curve's
		double rows_worst = 0.0, game_rows_worst = 0.0;
		for (UINT y = 0; y < H; ++y)
		{
			double sum[3] = {}, want[3] = {}, game_sum[3] = {}, game_want[3] = {};
			int n = 0;
			for (UINT x = W / 2 - 40; x < W / 2 + 24; x += 2)
			{
				const vec3 w = ray_of(x + 0.5f, y + 0.5f);
				const float el = std::asin(std::clamp(-w.y, -1.0f, 1.0f));
				if (el < 0.01f || in(pole, x, y))
					continue;
				const float dir[3] = { w.x, w.y, w.z };
				float smooth[3], game[3];
				sky_gradient(noon, el / 1.5707963f, smooth);
				dome_colour(noon, dir, game);
				for (int k = 0; k < 3; ++k)
				{
					sum[k] += noon_after[(size_t(y) * W + x) * 4 + k];
					want[k] += smooth[k];
					game_sum[k] += noon_before[(size_t(y) * W + x) * 4 + k];
					game_want[k] += game[k];
				}
				n++;
			}
			if (n < 24)
				continue;
			for (int k = 0; k < 3; ++k)
			{
				rows_worst = std::max(rows_worst, std::fabs(sum[k] - want[k]) / n);
				game_rows_worst = std::max(game_rows_worst, std::fabs(game_sum[k] - game_want[k]) / n);
			}
		}
		std::printf("  sky: rows' means off the curve by %.2f of a byte at worst (the game's strips off their own exact colours: %.2f, whole bytes)\n",
			rows_worst, game_rows_worst);
		check(st_noon.shaded && st_noon.sky && st_noon.draws == 0, "the sky of its own drawn in the shading pass (alone)");
		check(rebuilt_worst <= 1.5 && rebuilt_over == 0, "the game's dome rebuilt per pixel matches its strips as drawn (within a byte and a half)");
		// (a byte: half of it the dither, half the rounding to whole bytes)
		check(repainted > W * H / 4 && painted_worst <= 1.05, "the sky the zone left comes out as the smooth gradient (within a byte)");
		check(stops_seen >= 4 && stop_worst < 0.75, "the record's colours at their stops' heights");
		check(rows_worst < 0.35, "smooth: rows follow the curve within a third of a byte (no steps of one)");
		check(changed_off_sky == 0 && changed_pole == 0, "nothing the zone drew changes (the ground, the pole in front of the sky)");
		check(changed_cloud == 0, "what the game drew over its dome (a cloud) stays as drawn");

		// The sun: toward a pixel up and to the right; the cloud moved beside it (its glow would reach it)
		const float sun_x = W * 0.72f, sun_y = H * 0.2f;
		const vec3 sw = ray_of(sun_x, sun_y);
		const float sun_dir[3] = { sw.x, sw.y, sw.z };
		look::sky_dome sunny = noon;
		sunny.sun = 1.0f;
		const D3D12_RECT cloud_by_sun = { LONG(sun_x) + 12, LONG(sun_y) - 20, LONG(sun_x) + 40, LONG(sun_y) + 20 };
		std::vector<uint8_t> sun_before;
		draw_stats st_sun;
		const std::vector<uint8_t> sun_after = sky_frame(noon, sunny, sun_dir, sun_before, st_sun, cloud_by_sun);
		auto lum_at = [&](const std::vector<uint8_t> &q, int x, int y) {
			const uint8_t *c = &q[(size_t(y) * W + x) * 4];
			return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
		};
		auto smooth_lum = [&](int x, int y) {
			const vec3 w = ray_of(x + 0.5f, y + 0.5f);
			float s[3];
			sky_gradient(noon, std::asin(std::clamp(-w.y, -1.0f, 1.0f)) / 1.5707963f, s);
			return 0.3 * s[0] + 0.59 * s[1] + 0.11 * s[2];
		};
		const int sx = int(sun_x), sy = int(sun_y);
		const double at_sun = lum_at(sun_after, sx, sy), near_sun = lum_at(sun_after, sx - 12, sy) - smooth_lum(sx - 12, sy);
		const double far_sun = lum_at(sun_after, int(W * 0.25f), int(H * 0.45f)) - smooth_lum(int(W * 0.25f), int(H * 0.45f));
		// (where the disc is: the middle of the pixels it turns white)
		double disc_x = 0.0, disc_y = 0.0;
		uint32_t disc_n = 0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
				if (!in(cloud_by_sun, x, y) && lum_at(sun_after, x, y) >= 250.0)
					disc_x += x + 0.5, disc_y += y + 0.5, disc_n++;
		disc_x /= std::max(1u, disc_n);
		disc_y /= std::max(1u, disc_n);
		uint32_t cloud_changed = 0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
				if (in(cloud_by_sun, x, y) && std::memcmp(&sun_before[(size_t(y) * W + x) * 4], &sun_after[(size_t(y) * W + x) * 4], 4) != 0)
					cloud_changed++;
		std::printf("  sun: at it %.1f (the sky there %.1f), 12 pixels off +%.1f, far off +%.1f; its disc %u pixels round %.1f,%.1f (the light toward %.1f,%.1f); %u cloud pixels changed\n",
			at_sun, smooth_lum(sx, sy), near_sun, far_sun, disc_n, disc_x, disc_y, sun_x, sun_y, cloud_changed);
		check(st_sun.sky && at_sun > 235.0, "a sun disc where the game's light points");
		check(disc_n >= 3 && disc_n < 200 && std::fabs(disc_x - sun_x) <= 1.5 && std::fabs(disc_y - sun_y) <= 1.5, "... a small one, round that point");
		check(near_sun > 8.0 && far_sun < near_sun * 0.25 && far_sun > -1.0, "its glow fading away from it");
		check(cloud_changed == 0, "no glow over the cloud beside it");
		if (FILE *f = nullptr; fopen_s(&f, "build\\sky_test.ppm", "wb") == 0 && f != nullptr)
		{
			std::fprintf(f, "P6 %u %u 255\n", W * 2, H);
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W * 2; ++x)
					std::fwrite(x < W ? &sun_before[(size_t(y) * W + x) * 4] : &sun_after[(size_t(y) * W + x - W) * 4], 1, 3, f);
			fclose(f);
		}

		// Night (21:00's record, 16 segments): stars over the sky, none with them off, none on the cloud or the ground
		const look::sky_dome night = dome_of(night_colours, night_heights, 16);
		look::sky_dome starry = night;
		starry.stars = 1.0f;
		starry.star_turn = 1.3f;
		std::vector<uint8_t> night_before, plain_before;
		draw_stats st_night, st_plain;
		const std::vector<uint8_t> night_after = sky_frame(night, starry, nullptr, night_before, st_night, cloud);
		const std::vector<uint8_t> plain_after = sky_frame(night, night, nullptr, plain_before, st_plain, cloud);
		double night_rebuilt = 0.0;
		uint32_t star_px = 0, plain_star_px = 0, night_wrong = 0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const vec3 w = ray_of(x + 0.5f, y + 0.5f);
				const float el = std::asin(std::clamp(-w.y, -1.0f, 1.0f));
				const size_t i = (size_t(y) * W + x) * 4;
				const bool off_sky = el <= 0.0f || in(pole, x, y) || in(cloud, x, y);
				if (off_sky)
				{
					night_wrong += std::memcmp(&night_before[i], &night_after[i], 4) != 0 ? 1 : 0;
					continue;
				}
				const float dir[3] = { w.x, w.y, w.z };
				float game[3], smooth[3];
				dome_colour(night, dir, game);
				sky_gradient(night, el / 1.5707963f, smooth);
				for (int k = 0; k < 3; ++k)
					night_rebuilt = std::max(night_rebuilt, std::fabs(night_before[i + k] - double(game[k])));
				const double s = 0.3 * smooth[0] + 0.59 * smooth[1] + 0.11 * smooth[2];
				star_px += 0.3 * night_after[i] + 0.59 * night_after[i + 1] + 0.11 * night_after[i + 2] - s > 12.0 ? 1 : 0;
				plain_star_px += 0.3 * plain_after[i] + 0.59 * plain_after[i + 1] + 0.11 * plain_after[i + 2] - s > 12.0 ? 1 : 0;
			}
		std::printf("  night (16 segments): the dome rebuilt within %.2f of a byte; %u star pixels (%u with the stars off), %u changed off the sky\n",
			night_rebuilt, star_px, plain_star_px, night_wrong);
		check(st_night.sky && night_rebuilt <= 1.5, "16 segments: the game's dome rebuilt as drawn too");
		check(star_px > 30 && plain_star_px == 0, "stars by night, none with them off");
		check(night_wrong == 0, "none on the cloud, the pole or the ground");
		if (FILE *f = nullptr; fopen_s(&f, "build\\sky_night_test.ppm", "wb") == 0 && f != nullptr)
		{
			std::fprintf(f, "P6 %u %u 255\n", W, H);
			for (size_t i = 0; i < size_t(W) * H; ++i)
				std::fwrite(&night_after[i * 4], 1, 3, f);
			fclose(f);
		}

		// No dome in the record (segments 0: dungeons, interiors) and the sky off: nothing drawn, the picture as it was
		look::sky_dome domeless = noon;
		domeless.segments = 0;
		look::sky_dome off = noon;
		off.on = false;
		std::vector<uint8_t> none_before, off_before;
		draw_stats st_none, st_off;
		const std::vector<uint8_t> none_after = sky_frame(noon, domeless, nullptr, none_before, st_none, cloud);
		const std::vector<uint8_t> off_after = sky_frame(noon, off, nullptr, off_before, st_off, cloud);
		check(!st_none.sky && !st_none.shaded && none_after == none_before && !st_off.sky && !st_off.shaded && off_after == off_before,
			"no dome in the record, or the sky off: nothing drawn, the picture as it was");
		// The debug view: what's repainted magenta (its green down), the cloud (sky kept as drawn) red (its blue down), the
		// ground and the pole as they were
		look::sky_dome shown = noon;
		shown.debug = true;
		std::vector<uint8_t> debug_before;
		draw_stats st_debug;
		const std::vector<uint8_t> debug_after = sky_frame(noon, shown, nullptr, debug_before, st_debug, cloud);
		uint32_t magenta = 0, red_cloud = 0, debug_off_sky = 0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const size_t i = (size_t(y) * W + x) * 4;
				const vec3 w = ray_of(x + 0.5f, y + 0.5f);
				if (in(pole, x, y) || -w.y <= 0.0f)
					debug_off_sky += std::memcmp(&debug_before[i], &debug_after[i], 4) != 0 ? 1 : 0;
				else if (in(cloud, x, y))
					red_cloud += debug_after[i + 2] + 30 < debug_before[i + 2] && debug_after[i] >= debug_before[i] ? 1 : 0;
				else
					magenta += debug_after[i + 1] + 20 < debug_before[i + 1] ? 1 : 0;
			}
		const uint32_t cloud_px = uint32_t((cloud.right - cloud.left) * (cloud.bottom - cloud.top));
		std::printf("  sky debug view: %u pixels magenta, %u of the cloud's %u red, %u off the sky changed\n", magenta, red_cloud, cloud_px, debug_off_sky);
		check(st_debug.sky && magenta > W * H / 4 && red_cloud == cloud_px && debug_off_sky == 0, "the debug view: repainted magenta, the cloud red, the rest as it was");
		check(g.report_messages() == 0, "no debug-layer errors (sky)");

		// 1y. The weather of its own (grass off), with the sky of its own: the game's dome drawn as above, the camera 2 yalms
		// over flat ground looking 20 degrees up (the ground far off along the bottom of the picture, striped across so a
		// shift shows), the pole in front of the sky 20 yalms off. By name, each weather's part (weather_fx_of), blended
		// across a change. Clouds: a few in fine weather, most of the sky in clouds, the ground as it was. Lightning: a frame
		// with the flash brighter than the one before, ground and sky, then as before; its bolt a bright line where it's aimed.
		// Dust: the ground tinted toward the dust's colour, far off more than near. Heat shimmer: the ground far off moved,
		// the ground near the camera, the pole and the sky not. An aurora: green light over the night sky, none by day.
		// Gloom: the ground below the camera fogged, the pole high over it hardly.
		{
			check(weather_fx_of(weather_name("fine")).cover > 0.0f && weather_fx_of(weather_name("suny")).cover > weather_fx_of(weather_name("fine")).cover &&
				weather_fx_of(weather_name("clod")).cover > weather_fx_of(weather_name("suny")).cover && weather_fx_of(weather_name("thdr")).cover == 1.0f,
				"clouds by name: a few in fine weather, scattered in sunshine, overcast in clouds and thunder");
			check(weather_fx_of(weather_name("mist")).veil > 0.5f && weather_fx_of(weather_name("mis1")).veil > 0.5f && weather_fx_of(weather_name("fine")).veil == 0.0f,
				"mist a veil over the sky (its second name too)");
			check(weather_fx_of(weather_name("thdr")).flashes > 0.0f && weather_fx_of(weather_name("bol1")).flashes == 1.0f &&
				weather_fx_of(weather_name("rain")).flashes == 0.0f && weather_fx_of(weather_name("fine")).flashes == 0.0f, "lightning in thunder and thunderstorms, not in rain");
			check(weather_fx_of(weather_name("dust")).dust > 0.0f && weather_fx_of(weather_name("san1")).sand == 1.0f && weather_fx_of(weather_name("dus1")).sand == 0.0f &&
				weather_fx_of(weather_name("win1")).windy == 1.0f && weather_fx_of(weather_name("stom")).wind > weather_fx_of(weather_name("wind")).wind &&
				weather_fx_of(weather_name("dry1")).heat > 0.0f && weather_fx_of(weather_name("heat")).heat > weather_fx_of(weather_name("dryw")).heat &&
				weather_fx_of(weather_name("aur1")).aurora > 0.0f && weather_fx_of(weather_name("fog1")).gloom > 0.0f &&
				weather_fx_of(weather_name("dark")).gloom > weather_fx_of(weather_name("fogd")).gloom && weather_fx_of(weather_name("ligt")).stars > 2.0f,
				"dust and sand, wind and gales, heat, aurora, gloom and stellar glare by name (and by second name)");
			weather fine_to_clouds;
			fine_to_clouds.known = true;
			fine_to_clouds.name = weather_name("clod");
			fine_to_clouds.before = weather_name("fine");
			fine_to_clouds.change = 0.5f;
			const float halfway = 0.5f * (weather_fx_of(weather_name("fine")).cover + weather_fx_of(weather_name("clod")).cover);
			check(std::fabs(weather_fx_of(fine_to_clouds).cover - halfway) < 1e-6f, "changing from fine weather to clouds halfway: the cover halfway between");
			// (the wind: the way the game's own clouds drift, research/ffxi-re/data/zone_wind.txt: zone 1's clouds 280.8 degrees,
			// its sunshine 192.8; a weather the zone's clouds don't say, between its others; a zone with none, west)
			const float to_deg = 57.29578f;
			auto deg = [&](float r) { return std::fmod(r * to_deg + 720.0f, 360.0f); };
			const float zone1_snow = deg(wind_bearing(1, weather_name("snow")));
			weather sunny_to_clouds = fine_to_clouds;
			sunny_to_clouds.before = weather_name("suny");
			const float zone1_halfway = deg(wind_bearing(1, sunny_to_clouds));
			std::printf("  wind: zone 1 clouds %.1f, clo1 %.1f, sunshine %.1f, snow (not in it) %.1f, sunshine to clouds halfway %.1f; zone 4000 %.1f degrees\n",
				deg(wind_bearing(1, weather_name("clod"))), deg(wind_bearing(1, weather_name("clo1"))), deg(wind_bearing(1, weather_name("suny"))), zone1_snow,
				zone1_halfway, deg(wind_bearing(4000, weather_name("fine"))));
			check(std::fabs(deg(wind_bearing(1, weather_name("clod"))) - 280.8f) < 0.05f && std::fabs(deg(wind_bearing(1, weather_name("clo1"))) - 280.8f) < 0.05f &&
				std::fabs(deg(wind_bearing(1, weather_name("suny"))) - 192.8f) < 0.05f && zone1_snow > 192.8f && zone1_snow < 290.0f &&
				zone1_halfway > 192.8f && zone1_halfway < 280.8f && std::fabs(deg(wind_bearing(4000, weather_name("fine"))) - 275.0f) < 0.05f,
				"the wind the way the game's own clouds drift (by either name; the zone's others' where it says none; west in a zone without)");

			// The weather's look as the core has it (weather_look: lit as a noon record lights the zone), the wind blowing
			// across the picture, the ground under the camera the flat ground's
			auto wx_for = [&](uint32_t name, const look::sky_dome &d) {
				weather w;
				w.known = true;
				w.name = name;
				w.record = true;
				w.sun_color[0] = w.sun_color[1] = w.sun_color[2] = 0.5f;
				w.intensity = 1.5f;
				w.record_fog_color[0] = 0.6f, w.record_fog_color[1] = 0.63f, w.record_fog_color[2] = 0.68f;
				look::weather_fx x;
				weather_look(w, d, x);
				x.wind[0] = sky_cam.view[0];
				x.wind[1] = sky_cam.view[8];
				x.ground_y = at.y;
				x.cloud_style = 1; // (the volumetric clouds for these; the game-like style's own checks below)
				return x;
			};
			std::vector<D3D12_RECT> stripes;
			for (LONG x = 0; x < LONG(W); x += 6)
				stripes.push_back({ x, 0, x + 3, LONG(H) });
			auto weather_frame = [&](const look::sky_dome &d, const look::weather_fx &wx, draw_stats &st) {
				barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
				const float clear[4] = { d.clear[0] / 255.0f, d.clear[1] / 255.0f, d.clear[2] / 255.0f, 0.5f }, stripe[4] = { 0.25f, 0.3f, 0.18f, 0.5f };
				g.list->ClearRenderTargetView(world_rtv, clear, 0, nullptr);
				g.list->ClearRenderTargetView(world_rtv, stripe, UINT(stripes.size()), stripes.data());
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
				ground_depth(sky_cam);
				draw_dome(d, sky_cam);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f - 0.1f / 20.0f, 0, 1, &pole);
				barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				look ls = l;
				ls.grass = false;
				ls.ao_strength = ls.relief = 0.0f;
				ls.sun_shadow = 0.0f;
				ls.fireflies = 0.0f;
				ls.water = ls.water_debug = false;
				ls.dome = d;
				ls.time = 12.5f;
				ls.wx = wx;
				r.forget_clouds(); // (each picture the clouds marched whole: the same weather, the same clouds)
				st = r.draw(g.list, t, sky_cam, ls, error);
				barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
				return g.read(world, 4);
			};
			// (each pixel: in the sky (above the horizon, not the pole), on the pole, or the ground and how far off it is)
			enum class at_kind { sky, pole, ground };
			auto kind_of = [&](UINT x, UINT y, float &yalms) {
				const vec3 w = ray_of(x + 0.5f, y + 0.5f);
				yalms = w.y > 1e-4f ? 2.0f / w.y : 1e9f; // (the camera 2 yalms over the ground: y is down)
				return in(pole, x, y) ? at_kind::pole : w.y < 0.0f ? at_kind::sky : at_kind::ground;
			};
			auto off_by = [](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, size_t i) {
				return std::max({ std::abs(int(a[i]) - int(b[i])), std::abs(int(a[i + 1]) - int(b[i + 1])), std::abs(int(a[i + 2]) - int(b[i + 2])) });
			};
			auto lum_of = [](const std::vector<uint8_t> &q, size_t i) { return 0.3 * q[i] + 0.59 * q[i + 1] + 0.11 * q[i + 2]; };
			const look::weather_fx calm;
			draw_stats st_calm, st_fine_c, st_clod;
			const std::vector<uint8_t> clear_sky = weather_frame(noon, calm, st_calm);
			const look::weather_fx fine_wx = wx_for(weather_name("fine"), noon), clod_wx = wx_for(weather_name("clod"), noon);
			const std::vector<uint8_t> fine_sky = weather_frame(noon, fine_wx, st_fine_c), clod_sky = weather_frame(noon, clod_wx, st_clod);
			uint32_t sky_n = 0, fine_cloudy = 0, clod_cloudy = 0, cloud_ground = 0;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					float yalms = 0.0f;
					const size_t i = (size_t(y) * W + x) * 4;
					if (kind_of(x, y, yalms) != at_kind::sky)
					{
						cloud_ground += off_by(clear_sky, fine_sky, i) != 0 || off_by(clear_sky, clod_sky, i) != 0 ? 1 : 0;
						continue;
					}
					sky_n++;
					fine_cloudy += off_by(clear_sky, fine_sky, i) > 3 ? 1 : 0;
					clod_cloudy += off_by(clear_sky, clod_sky, i) > 3 ? 1 : 0;
				}
			std::printf("  clouds: of %u sky pixels, %u cloudy in fine weather, %u in clouds; %u pixels off the sky changed\n", sky_n, fine_cloudy, clod_cloudy, cloud_ground);
			if (FILE *f = nullptr; fopen_s(&f, "build\\clouds_test.ppm", "wb") == 0 && f != nullptr)
			{
				std::fprintf(f, "P6 %u %u 255\n", W * 2, H);
				for (UINT y = 0; y < H; ++y)
					for (UINT x = 0; x < W * 2; ++x)
						std::fwrite(x < W ? &fine_sky[(size_t(y) * W + x) * 4] : &clod_sky[(size_t(y) * W + x - W) * 4], 1, 3, f);
				fclose(f);
			}
			check(st_calm.shaded && !st_calm.weather && st_fine_c.weather && st_clod.weather && !st_clod.air, "the weather's parameters go to the passes with clouds (no air pass for them)");
			check(fine_cloudy > sky_n / 50 && fine_cloudy < sky_n / 2, "clouds present in fine weather: a few");
			check(clod_cloudy > 2 * fine_cloudy && clod_cloudy > sky_n * 6 / 10, "cloudier in clouds than in fine weather: most of the sky");
			check(cloud_ground == 0, "the clouds only in the sky (the ground and the pole as they were)");

			// The clouds' own pass (cloud_ps): a slab of them marched at a quarter of the size, lit by the sun. A frame like
			// weather_frame's for any camera and sun, the clouds' last picture carried over or not (forget: every pixel of it
			// marched now, the same each time)
			auto cloud_frame = [&](const camera &c, const look::sky_dome &d, const look::weather_fx &wx, const float *sun, bool forget, draw_stats &st,
				float sun_shadow = 0.0f, float night = 0.0f) {
				barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
				const float clear[4] = { d.clear[0] / 255.0f, d.clear[1] / 255.0f, d.clear[2] / 255.0f, 0.5f }, stripe[4] = { 0.25f, 0.3f, 0.18f, 0.5f };
				g.list->ClearRenderTargetView(world_rtv, clear, 0, nullptr);
				g.list->ClearRenderTargetView(world_rtv, stripe, UINT(stripes.size()), stripes.data());
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
				ground_depth(c);
				draw_dome(d, c);
				g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f - 0.1f / 20.0f, 0, 1, &pole);
				barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				look ls = l;
				ls.grass = false;
				ls.ao_strength = ls.relief = 0.0f;
				ls.sun_shadow = sun_shadow;
				ls.fireflies = 0.0f;
				ls.night = night;
				ls.water = ls.water_debug = false;
				ls.dome = d;
				ls.time = 12.5f;
				ls.wx = wx;
				if (sun != nullptr)
					for (int i = 0; i < 3; ++i)
						ls.sun[i] = sun[i];
				if (forget)
					r.forget_clouds();
				st = r.draw(g.list, t, c, ls, error);
				barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
				return g.read(world, 4);
			};
			// (a pixel's view ray for a camera, y down; in the sky: above the horizon and not the pole)
			auto ray_for = [&](const camera &c, float x, float y) {
				const float nx = x / W * 2.0f - 1.0f, ny = 1.0f - y / H * 2.0f;
				const float vx = nx / c.proj[0], vy = ny / c.proj[5], vz = c.proj[11];
				const float *m = c.view;
				return normalize({ m[0] * vx + m[1] * vy + m[2] * vz, m[4] * vx + m[5] * vy + m[6] * vz, m[8] * vx + m[9] * vy + m[10] * vz });
			};
			auto sky_at = [&](const camera &c, UINT x, UINT y) { return !in(pole, x, y) && ray_for(c, x + 0.5f, y + 0.5f).y < -0.002f; };
			const float overhead[3] = { 0.0f, -1.0f, 0.0f };

			// Clear gaps in fine weather: much of its sky as it was
			uint32_t fine_clear = 0;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					float yalms = 0.0f;
					const size_t i = (size_t(y) * W + x) * 4;
					if (kind_of(x, y, yalms) == at_kind::sky && off_by(clear_sky, fine_sky, i) <= 1)
						fine_clear++;
				}
			std::printf("  fine weather: %u of %u sky pixels clear between its clouds\n", fine_clear, sky_n);
			check(st_fine_c.clouds && st_clod.clouds && !st_calm.clouds && fine_clear > sky_n * 4 / 10, "clear sky between fine weather's clouds (their own pass drawn)");

			// Lit tops, darker bases, the sun overhead: clouds seen from the side low over the horizon (2 to 16 degrees up: a
			// heap's top is higher up the screen there), each column's runs of cloud, their top quarter against their bottom
			{
				const look::weather_fx suny_wx = wx_for(weather_name("suny"), noon);
				draw_stats st_side, st_side_clear;
				const std::vector<uint8_t> side = cloud_frame(sky_cam, noon, suny_wx, overhead, true, st_side);
				const std::vector<uint8_t> side_clear = cloud_frame(sky_cam, noon, calm, overhead, true, st_side_clear);
				double top_sum = 0.0, bottom_sum = 0.0;
				uint32_t runs = 0, brighter = 0;
				for (UINT x = 0; x < W; ++x)
				{
					UINT y = 0;
					while (y < H)
					{
						auto cloudy_at = [&](UINT yy) {
							const vec3 w = ray_for(sky_cam, x + 0.5f, yy + 0.5f);
							const float up = std::asin(std::clamp(-w.y, -1.0f, 1.0f)) * 57.29578f;
							return sky_at(sky_cam, x, yy) && up > 2.0f && up < 16.0f && off_by(side_clear, side, (size_t(yy) * W + x) * 4) > 12;
						};
						if (!cloudy_at(y))
						{
							++y;
							continue;
						}
						UINT end = y;
						while (end < H && cloudy_at(end))
							++end;
						const UINT n = end - y;
						if (n >= 8)
						{
							double top = 0.0, bottom = 0.0;
							for (UINT k = 0; k < n / 4; ++k)
							{
								top += lum_of(side, (size_t(y + k) * W + x) * 4);
								bottom += lum_of(side, (size_t(end - 1 - k) * W + x) * 4);
							}
							top /= n / 4;
							bottom /= n / 4;
							top_sum += top;
							bottom_sum += bottom;
							runs++;
							brighter += top > bottom ? 1 : 0;
						}
						y = end;
					}
				}
				std::printf("  the sun overhead, clouds low over the horizon: %u runs of cloud down a column, their tops %.1f, bottoms %.1f on average; %u of them brighter on top\n",
					runs, runs != 0 ? top_sum / runs : 0.0, runs != 0 ? bottom_sum / runs : 0.0, brighter);
				check(st_side.clouds && runs > 20 && top_sum > bottom_sum + 8.0 * runs && brighter > runs * 6 / 10, "lit tops brighter than their bases with the sun overhead");
			}

			// Thin edges brighter toward the sun: the same clouds with the sun (no disc or glow of its own: only the clouds'
			// light) 25 degrees up where the camera looks, then behind it; cloud pixels next to clear sky within 40 degrees of
			// where the sun is in the first
			{
				const look::weather_fx fine_lit = wx_for(weather_name("suny"), noon);
				const vec3 ahead = normalize({ -sky_cam.view[2], 0.0f, -sky_cam.view[10] });
				const float e = 25.0f * 3.14159265f / 180.0f;
				const float toward[3] = { ahead.x * std::cos(e), -std::sin(e), ahead.z * std::cos(e) }, away[3] = { -toward[0], toward[1], -toward[2] };
				draw_stats st_t, st_a, st_c;
				const std::vector<uint8_t> lit_toward = cloud_frame(sky_cam, noon, fine_lit, toward, true, st_t);
				const std::vector<uint8_t> lit_away = cloud_frame(sky_cam, noon, fine_lit, away, true, st_a);
				const std::vector<uint8_t> bare = cloud_frame(sky_cam, noon, calm, toward, true, st_c);
				auto cloudy = [&](UINT x, UINT y) { return sky_at(sky_cam, x, y) && off_by(bare, lit_away, (size_t(y) * W + x) * 4) > 4; };
				double t_sum = 0.0, a_sum = 0.0;
				uint32_t edge_n = 0;
				for (UINT y = 2; y + 2 < H; ++y)
					for (UINT x = 2; x + 2 < W; ++x)
					{
						const vec3 w = ray_for(sky_cam, x + 0.5f, y + 0.5f);
						if (!cloudy(x, y) || w.x * toward[0] + w.y * toward[1] + w.z * toward[2] < std::cos(40.0f * 3.14159265f / 180.0f))
							continue;
						bool by_clear = false;
						for (int dy = -2; dy <= 2 && !by_clear; ++dy)
							for (int dx = -2; dx <= 2 && !by_clear; ++dx)
								by_clear = sky_at(sky_cam, x + dx, y + dy) && !cloudy(x + dx, y + dy);
						if (!by_clear)
							continue;
						const size_t i = (size_t(y) * W + x) * 4;
						t_sum += lum_of(lit_toward, i);
						a_sum += lum_of(lit_away, i);
						edge_n++;
					}
				std::printf("  thin edges near the sun: %u pixels, %.1f lit from behind them (toward the sun), %.1f lit from the camera's side\n", edge_n,
					edge_n != 0 ? t_sum / edge_n : 0.0, edge_n != 0 ? a_sum / edge_n : 0.0);
				check(edge_n > 50 && t_sum > a_sum * 1.1, "thin edges brighter toward the sun (a silver lining)");
			}

			// The picture carried over: a still camera, frame after frame (each marching a quarter of it, the rest carried
			// over): steady; then turning 1.5 degrees a frame for 8 frames, against the same view marched whole: no smear
			{
				draw_stats st0, st1;
				std::vector<uint8_t> prev = cloud_frame(sky_cam, noon, clod_wx, nullptr, true, st0);
				double still_mean = 0.0;
				int still_most = 0;
				uint32_t still_n = 0, carried = 0, still_jumps = 0;
				for (int f = 0; f < 6; ++f)
				{
					const std::vector<uint8_t> next = cloud_frame(sky_cam, noon, clod_wx, nullptr, false, st1);
					carried += st1.clouds_carried ? 1 : 0;
					if (f >= 2)
						for (UINT y = 0; y < H; ++y)
							for (UINT x = 0; x < W; ++x)
								if (sky_at(sky_cam, x, y))
								{
									const int d = off_by(prev, next, (size_t(y) * W + x) * 4);
									still_mean += d;
									still_most = std::max(still_most, d);
									still_jumps += d > 16 ? 1 : 0;
									still_n++;
								}
					prev = next;
				}
				still_mean /= std::max(1u, still_n);
				camera turned = sky_cam;
				const vec3 eye_at = { at.x, at.y - 2.0f, at.z };
				std::vector<uint8_t> turning;
				for (int f = 1; f <= 8; ++f)
				{
					const float b = (36.87f + 1.5f * f) * 3.14159265f / 180.0f;
					look_at(eye_at, { eye_at.x + 10.0f * std::sin(b) * std::cos(up20), eye_at.y - 10.0f * std::sin(up20), eye_at.z + 10.0f * std::cos(b) * std::cos(up20) },
						{ 0.0f, -1.0f, 0.0f }, turned.view);
					turning = cloud_frame(turned, noon, clod_wx, nullptr, false, st1);
					carried += st1.clouds_carried ? 1 : 0;
				}
				const std::vector<uint8_t> whole = cloud_frame(turned, noon, clod_wx, nullptr, true, st0);
				const std::vector<uint8_t> turned_clear = cloud_frame(turned, noon, calm, nullptr, true, st0);
				double turn_mean = 0.0, edge_mean = 0.0;
				uint32_t turn_n = 0, edge_n = 0;
				for (UINT y = 1; y + 1 < H; ++y)
					for (UINT x = 1; x + 1 < W; ++x)
						if (sky_at(turned, x, y))
						{
							const size_t i = (size_t(y) * W + x) * 4;
							const int d = off_by(whole, turning, i);
							turn_mean += d;
							turn_n++;
							// (where the clouds' edges are, marched whole: a smear shows most there)
							const bool cloud_here = off_by(turned_clear, whole, i) > 6;
							const bool cloud_right = off_by(turned_clear, whole, i + 4) > 6;
							if (cloud_here != cloud_right)
							{
								edge_mean += d;
								edge_n++;
							}
						}
				turn_mean /= std::max(1u, turn_n);
				edge_mean /= std::max(1u, edge_n);
				std::printf("  carried over: a still camera's frames %.2f apart on average (%d at most, %u of %u pixels by over 16); turning 1.5 degrees a frame for 8, against the view marched whole: "
					"%.2f apart, %.2f on the clouds' edges (%u); %u of 14 frames carried over\n", still_mean, still_most, still_jumps, still_n, turn_mean, edge_mean,
					edge_n, carried);
				check(carried == 14, "the clouds' last picture carried over frame to frame (still and turning)");
				// (a pixel on a cloud's edge, sharpened at the world's size, may step a little as its quarter-size pixel is marched
				// again: one in a thousand at most)
				check(still_mean < 1.0 && still_jumps * 1000 < still_n, "a still camera: the clouds steady frame to frame");
				check(edge_n > 100 && turn_mean < 3.0 && edge_mean < 8.0, "a turning camera: no smear (as the view marched whole)");
			}

			// The clouds' shadows on the ground: the sun 50 degrees up ahead of the camera, the sun's shadows at 0.6, sunshine's
			// clouds against no clouds: the ground 40 to 600 yalms off in patches, some of it darker by their shadows and
			// some not; with the sun's shadows off, none
			{
				const vec3 ahead = normalize({ -sky_cam.view[2], 0.0f, -sky_cam.view[10] });
				const float e = 50.0f * 3.14159265f / 180.0f;
				const float behind[3] = { ahead.x * std::cos(e), -std::sin(e), ahead.z * std::cos(e) };
				const look::weather_fx suny_wx = wx_for(weather_name("suny"), noon);
				draw_stats st_s, st_c, st_o;
				const std::vector<uint8_t> shaded = cloud_frame(sky_cam, noon, suny_wx, behind, true, st_s, 0.6f);
				const std::vector<uint8_t> unshaded = cloud_frame(sky_cam, noon, calm, behind, true, st_c, 0.6f);
				const std::vector<uint8_t> off_shadows = cloud_frame(sky_cam, noon, suny_wx, behind, true, st_o, 0.0f);
				const std::vector<uint8_t> off_clear = cloud_frame(sky_cam, noon, calm, behind, true, st_o, 0.0f);
				uint32_t ground_n = 0, darker = 0, off_changed = 0;
				for (UINT y = 0; y < H; ++y)
					for (UINT x = 0; x < W; ++x)
					{
						float yalms = 0.0f;
						const size_t i = (size_t(y) * W + x) * 4;
						if (kind_of(x, y, yalms) != at_kind::ground)
							continue;
						off_changed += off_by(off_shadows, off_clear, i) != 0 ? 1 : 0;
						if (yalms < 40.0f || yalms > 600.0f)
							continue;
						ground_n++;
						darker += lum_of(shaded, i) + 4.0 < lum_of(unshaded, i) ? 1 : 0;
					}
				std::printf("  the clouds' shadows: %u of %u ground pixels 40 to 600 yalms off darker under them; with the sun's shadows off %u changed\n", darker, ground_n, off_changed);
				check(st_s.cloud_shadows && !st_o.cloud_shadows && darker > ground_n / 20 && darker < ground_n * 19 / 20,
					"the clouds' shadows on the ground in patches (their own map made)");
				check(off_changed == 0, "no clouds' shadows with the sun's shadows off");

				// By night (the moon's shadows on): none either. The same clouds, the night at its deepest: the ground as without
				// clouds (a dust storm's unseen clouds left dark patches by moonlight, 2026-10-29)
				draw_stats st_n, st_nc;
				const std::vector<uint8_t> night_clouds = cloud_frame(sky_cam, noon, suny_wx, behind, true, st_n, 0.6f, 1.0f);
				const std::vector<uint8_t> night_clear = cloud_frame(sky_cam, noon, calm, behind, true, st_nc, 0.6f, 1.0f);
				uint32_t night_changed = 0;
				for (UINT y = 0; y < H; ++y)
					for (UINT x = 0; x < W; ++x)
					{
						float yalms = 0.0f;
						if (kind_of(x, y, yalms) == at_kind::ground && off_by(night_clouds, night_clear, (size_t(y) * W + x) * 4) != 0)
							night_changed++;
					}
				std::printf("  by night: %u ground pixels changed by the clouds\n", night_changed);
				check(!st_n.cloud_shadows && night_changed == 0, "no clouds' shadows by night");
			}

			// Clouds off (the setting: no cover, thickness or veil): nothing of their pass, the picture the same to the byte as
			// before they were marched (the shaders as they were, grass_before_clouds.hlsl: git HEAD's grass.hlsl from before
			// them, with a cloud_ps that does nothing; looked for in build\test, the check skipped without it)
			{
				look::weather_fx off_wx = fine_wx;
				off_wx.cloud_cover = off_wx.cloud_thick = off_wx.cloud_veil = 0.0f;
				look::weather_fx storm_off = wx_for(weather_name("bolt"), noon);
				storm_off.cloud_cover = storm_off.cloud_thick = storm_off.cloud_veil = 0.0f;
				storm_off.flash = 1.0f;
				draw_stats st_off, st_off2, st_b;
				const std::vector<uint8_t> off_now = cloud_frame(sky_cam, noon, off_wx, nullptr, true, st_off);
				const std::vector<uint8_t> storm_now = cloud_frame(sky_cam, noon, storm_off, nullptr, true, st_off2);
				const std::vector<uint8_t> calm_now = cloud_frame(sky_cam, noon, calm, nullptr, true, st_b);
				check(!st_off.clouds && !st_off2.clouds && st_off2.weather, "clouds off: their pass isn't drawn");
				std::string before_source;
				if (FILE *f = nullptr; fopen_s(&f, "build\\test\\grass_before_clouds.hlsl", "rb") == 0 && f != nullptr)
				{
					char buf[4096];
					for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) != 0;)
						before_source.append(buf, n);
					std::fclose(f);
				}
				std::string why;
				if (!before_source.empty() && r.set_shader_source(before_source, why))
				{
					const std::vector<uint8_t> off_before = cloud_frame(sky_cam, noon, off_wx, nullptr, true, st_b);
					const std::vector<uint8_t> storm_before = cloud_frame(sky_cam, noon, storm_off, nullptr, true, st_b);
					const std::vector<uint8_t> calm_before = cloud_frame(sky_cam, noon, calm, nullptr, true, st_b);
					r.set_shader_source(built_in_shader_source(), why);
					check(off_now == off_before && storm_now == storm_before && calm_now == calm_before,
						"clouds off: the picture byte for byte as before the clouds' march (fine weather, a thunderstorm's flash, no weather)");
				}
				else
					std::printf("  (clouds off against the shaders before them: build\\test\\grass_before_clouds.hlsl %s: not compared)\n",
						before_source.empty() ? "not found" : why.c_str());
			}

			// Pictures for a look (build\clouds_*.ppm): noon in fine weather, sunshine, clouds and rain, dusk in sunshine, each
			// carried over a few frames as in game
			{
				auto picture = [&](const char *file, const look::sky_dome &d, const look::weather_fx &wx, const float *sun) {
					draw_stats st;
					std::vector<uint8_t> q = cloud_frame(sky_cam, d, wx, sun, true, st);
					for (int f = 0; f < 7; ++f)
						q = cloud_frame(sky_cam, d, wx, sun, false, st);
					if (FILE *f = nullptr; fopen_s(&f, file, "wb") == 0 && f != nullptr)
					{
						std::fprintf(f, "P6 %u %u 255\n", W, H);
						for (size_t i = 0; i < size_t(W) * H; ++i)
							std::fwrite(&q[i * 4], 1, 3, f);
						fclose(f);
					}
				};
				const vec3 ahead = normalize({ -sky_cam.view[2], 0.0f, -sky_cam.view[10] });
				const float high = 60.0f * 3.14159265f / 180.0f, low = 4.0f * 3.14159265f / 180.0f;
				const float noon_sun[3] = { -ahead.z * std::cos(high), -std::sin(high), ahead.x * std::cos(high) };
				const float dusk_sun[3] = { (ahead.x * 0.9f + ahead.z * 0.44f) * std::cos(low), -std::sin(low), (ahead.z * 0.9f - ahead.x * 0.44f) * std::cos(low) };
				picture("build\\clouds_fine.ppm", noon, fine_wx, noon_sun);
				picture("build\\clouds_clod.ppm", noon, clod_wx, noon_sun);
				picture("build\\clouds_rain.ppm", noon, wx_for(weather_name("rain"), noon), noon_sun);
				// (dusk: West Ronfaure's noon gradient warmed toward the horizon, the sun orange and low ahead)
				look::sky_dome dusk = noon;
				const float dusk_colours[8][3] = { { 236, 152, 96 }, { 214, 136, 106 }, { 168, 118, 128 }, { 112, 100, 148 }, { 72, 76, 140 }, { 52, 58, 120 }, { 46, 50, 110 }, { 42, 46, 104 } };
				for (int i = 0; i < 8; ++i)
					for (int k = 0; k < 3; ++k)
						dusk.stops[i][k] = dusk_colours[i][k];
				for (int k = 0; k < 3; ++k)
					dusk.clear[k] = dusk_colours[0][k];
				dusk.sun = 1.0f;
				dusk.disc[0] = 1.0f, dusk.disc[1] = 0.62f, dusk.disc[2] = 0.38f;
				weather w;
				w.known = true;
				w.name = weather_name("suny");
				w.record = true;
				w.sun_color[0] = 0.9f, w.sun_color[1] = 0.5f, w.sun_color[2] = 0.28f;
				w.intensity = 1.5f;
				w.record_fog_color[0] = 0.7f, w.record_fog_color[1] = 0.52f, w.record_fog_color[2] = 0.46f;
				look::weather_fx dusk_wx;
				weather_look(w, dusk, dusk_wx);
				dusk_wx.wind[0] = sky_cam.view[0];
				dusk_wx.wind[1] = sky_cam.view[8];
				dusk_wx.ground_y = at.y;
				dusk_wx.cloud_light = std::clamp((std::sin(low) + 0.04f) / 0.12f, 0.0f, 1.0f);
				dusk_wx.cloud_style = 1;
				picture("build\\clouds_dusk.ppm", dusk, dusk_wx, dusk_sun);

				// The game-like style (CloudStyle 0, the default): at dusk in the sky's own colours (each cloudy pixel's colour,
				// its brightness aside, near the clear sky's there); by night, under a full moon 40 degrees up ahead, neither
				// style much brighter than the night sky round it (no glowing heaps)
				check(look::weather_fx().cloud_style == 0, "the game-like clouds by default");
				look::weather_fx dusk_game = dusk_wx;
				dusk_game.cloud_style = 0;
				picture("build\\clouds_dusk_gamelike.ppm", dusk, dusk_game, dusk_sun);
				draw_stats st_g;
				const std::vector<uint8_t> dusk_soft = cloud_frame(sky_cam, dusk, dusk_game, dusk_sun, true, st_g);
				const std::vector<uint8_t> dusk_bare = cloud_frame(sky_cam, dusk, calm, dusk_sun, true, st_g);
				look::weather_fx dusk_volume = dusk_wx;
				dusk_volume.cloud_style = 1;
				const std::vector<uint8_t> dusk_heaps = cloud_frame(sky_cam, dusk, dusk_volume, dusk_sun, true, st_g);
				auto hue_off = [&](const std::vector<uint8_t> &q, uint32_t &n) {
					double sum = 0.0;
					n = 0;
					for (UINT y = 0; y < H; ++y)
						for (UINT x = 0; x < W; ++x)
						{
							const size_t i = (size_t(y) * W + x) * 4;
							if (!sky_at(sky_cam, x, y) || off_by(dusk_bare, q, i) <= 4)
								continue;
							const double a = q[i] + q[i + 1] + q[i + 2] + 1.0, b = dusk_bare[i] + dusk_bare[i + 1] + dusk_bare[i + 2] + 1.0;
							for (int k = 0; k < 3; ++k)
								sum += std::fabs(q[i + k] / a - dusk_bare[i + k] / b);
							n++;
						}
					return n != 0 ? sum / n : 1.0;
				};
				uint32_t soft_n = 0, heaps_n = 0;
				const double soft_hue = hue_off(dusk_soft, soft_n), heaps_hue = hue_off(dusk_heaps, heaps_n);
				std::printf("  dusk: the game-like clouds' colour off the sky's %.3f (%u pixels), the volumetric's %.3f (%u)\n", soft_hue, soft_n, heaps_hue,
					heaps_n);
				check(soft_n > 2000 && soft_hue < 0.08 && soft_hue < heaps_hue, "game-like clouds at dusk in the sky's own colours");

				look::sky_dome moonlit = night;
				moonlit.sun = 0.6f;
				moonlit.moon = 1.0f;
				moonlit.moon_lit = 1.0f;
				moonlit.disc[0] = 0.8f, moonlit.disc[1] = 0.85f, moonlit.disc[2] = 1.0f;
				moonlit.stars = 1.0f;
				const float me = 40.0f * 3.14159265f / 180.0f;
				const float moon_dir[3] = { ahead.x * std::cos(me), -std::sin(me), ahead.z * std::cos(me) };
				weather wn;
				wn.known = true;
				wn.name = weather_name("suny");
				wn.record = true;
				wn.sun_color[0] = 0.25f, wn.sun_color[1] = 0.28f, wn.sun_color[2] = 0.38f;
				wn.intensity = 1.5f;
				wn.record_fog_color[0] = 0.12f, wn.record_fog_color[1] = 0.13f, wn.record_fog_color[2] = 0.2f;
				look::weather_fx night_wx;
				weather_look(wn, moonlit, night_wx);
				night_wx.wind[0] = sky_cam.view[0];
				night_wx.wind[1] = sky_cam.view[8];
				night_wx.ground_y = at.y;
				night_wx.cloud_light = 1.0f;
				look::weather_fx night_heaps = night_wx;
				night_heaps.cloud_style = 1;
				picture("build\\clouds_night_gamelike.ppm", moonlit, night_wx, moon_dir);
				picture("build\\clouds_night.ppm", moonlit, night_heaps, moon_dir);
				const std::vector<uint8_t> night_bare = cloud_frame(sky_cam, moonlit, calm, moon_dir, true, st_g);
				auto glow_of = [&](const std::vector<uint8_t> &q, double &mean) {
					double most = 0.0, sum = 0.0;
					uint32_t n = 0;
					for (UINT y = 0; y < H; ++y)
						for (UINT x = 0; x < W; ++x)
						{
							const size_t i = (size_t(y) * W + x) * 4;
							if (!sky_at(sky_cam, x, y) || off_by(night_bare, q, i) <= 3)
								continue;
							const double lift = lum_of(q, i) - lum_of(night_bare, i);
							if (lum_of(night_bare, i) > 60.0) // (the moon and its glow, and bright stars: what clouds pass over)
								continue;
							most = std::max(most, lift);
							sum += std::max(lift, 0.0);
							n++;
						}
					mean = n != 0 ? sum / n : 0.0;
					return most;
				};
				double soft_mean = 0.0, heaps_mean = 0.0;
				const double soft_most = glow_of(cloud_frame(sky_cam, moonlit, night_wx, moon_dir, true, st_g), soft_mean);
				const double heaps_most = glow_of(cloud_frame(sky_cam, moonlit, night_heaps, moon_dir, true, st_g), heaps_mean);
				std::printf("  night: the clouds over the night sky, game-like at most %.1f brighter (%.1f on average), volumetric %.1f (%.1f)\n", soft_most, soft_mean,
					heaps_most, heaps_mean);
				check(soft_most < 20.0 && soft_mean < 8.0 && heaps_most < 35.0 && heaps_mean < 10.0, "by night neither style's clouds glow");
			}
			check(g.report_messages() == 0, "no debug-layer errors (clouds)");

			// Lightning: the flash's frame against the one before (the same clouds, no flash) and the one after
			look::weather_fx storm = wx_for(weather_name("bolt"), noon);
			draw_stats st_before, st_flash, st_after, st_bolt;
			const std::vector<uint8_t> before_flash = weather_frame(noon, storm, st_before);
			storm.flash = 1.0f;
			const std::vector<uint8_t> flashing = weather_frame(noon, storm, st_flash);
			storm.flash = 0.0f;
			const std::vector<uint8_t> after_flash = weather_frame(noon, storm, st_after);
			double ground_before = 0.0, ground_flash = 0.0, sky_before = 0.0, sky_flash = 0.0;
			uint32_t ground_n = 0, flash_sky_n = 0;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					float yalms = 0.0f;
					const size_t i = (size_t(y) * W + x) * 4;
					const at_kind k = kind_of(x, y, yalms);
					if (k == at_kind::ground)
					{
						ground_before += lum_of(before_flash, i);
						ground_flash += lum_of(flashing, i);
						ground_n++;
					}
					else if (k == at_kind::sky)
					{
						sky_before += lum_of(before_flash, i);
						sky_flash += lum_of(flashing, i);
						flash_sky_n++;
					}
				}
			ground_before /= std::max(1u, ground_n);
			ground_flash /= std::max(1u, ground_n);
			sky_before /= std::max(1u, flash_sky_n);
			sky_flash /= std::max(1u, flash_sky_n);
			// (the bolt: aimed where the camera looks, coming out of the cloud 20 degrees up; how many sky pixels near white)
			storm.bolt = 2.5f;
			storm.bolt_bearing = std::atan2(6.0f, 8.0f);
			storm.bolt_top = 0.35f;
			storm.bolt_seed = 17.0f;
			const std::vector<uint8_t> bolted = weather_frame(noon, storm, st_bolt);
			uint32_t bolt_px = 0, white_before = 0;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					const size_t i = (size_t(y) * W + x) * 4;
					bolt_px += lum_of(bolted, i) > 235.0 ? 1 : 0;
					white_before += lum_of(before_flash, i) > 235.0 ? 1 : 0;
				}
			std::printf("  lightning: the ground %.1f before the flash, %.1f in it; the sky %.1f, %.1f; the frame after %s the one before; its bolt %u near-white pixels (%u before)\n",
				ground_before, ground_flash, sky_before, sky_flash, after_flash == before_flash ? "the same as" : "unlike", bolt_px, white_before);
			if (FILE *f = nullptr; fopen_s(&f, "build\\lightning_test.ppm", "wb") == 0 && f != nullptr)
			{
				std::fprintf(f, "P6 %u %u 255\n", W * 2, H);
				for (UINT y = 0; y < H; ++y)
					for (UINT x = 0; x < W * 2; ++x)
						std::fwrite(x < W ? &flashing[(size_t(y) * W + x) * 4] : &bolted[(size_t(y) * W + x - W) * 4], 1, 3, f);
				fclose(f);
			}
			check(st_flash.air && !st_before.air && ground_flash > ground_before * 1.3 && sky_flash > sky_before * 1.1, "a lightning flash: the frame brighter than the one before (the ground lit, the sky with it)");
			check(after_flash == before_flash, "... and back as it was after it");
			check(bolt_px > 15 && white_before == 0, "its bolt a bright line in the sky where it's aimed");

			// Dust: the ground tinted toward the dust's colour, the more the further off
			const look::weather_fx dust_wx = wx_for(weather_name("dust"), noon);
			draw_stats st_dust;
			const std::vector<uint8_t> dusty = weather_frame(noon, dust_wx, st_dust);
			double near_tint = 0.0, far_tint = 0.0;
			uint32_t near_n = 0, far_n = 0;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					float yalms = 0.0f;
					const size_t i = (size_t(y) * W + x) * 4;
					if (kind_of(x, y, yalms) != at_kind::ground || (yalms > 12.0f && yalms < 60.0f))
						continue;
					double was = 0.0, now = 0.0;
					for (int k = 0; k < 3; ++k)
					{
						was += std::fabs(clear_sky[i + k] - dust_wx.dust_colour[k] * 255.0);
						now += std::fabs(dusty[i + k] - dust_wx.dust_colour[k] * 255.0);
					}
					const double toward = was > 1.0 ? (was - now) / was : 0.0;
					(yalms <= 12.0f ? near_tint : far_tint) += toward;
					(yalms <= 12.0f ? near_n : far_n)++;
				}
			near_tint /= std::max(1u, near_n);
			far_tint /= std::max(1u, far_n);
			std::printf("  dust: the ground within 12 yalms %.0f%% of the way to the dust's colour (%u pixels), past 60 yalms %.0f%% (%u)\n", near_tint * 100.0, near_n, far_tint * 100.0, far_n);
			check(st_dust.air && near_n > 1000 && far_n > 1000 && near_tint > 0.02 && far_tint > 0.3 && far_tint > 2.0 * near_tint, "dust tints the low view: toward its colour, far off the most");

			// Heat shimmer: alone, against the same frame without it
			look::weather_fx heat_wx;
			heat_wx.heat = 1.0f;
			heat_wx.ground_y = at.y;
			draw_stats st_heat;
			const std::vector<uint8_t> shimmering = weather_frame(noon, heat_wx, st_heat);
			uint32_t far_moved = 0, far_ground = 0, near_moved = 0, sky_moved = 0, pole_moved = 0;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					float yalms = 0.0f;
					const size_t i = (size_t(y) * W + x) * 4;
					const bool moved = off_by(clear_sky, shimmering, i) != 0;
					switch (kind_of(x, y, yalms))
					{
					case at_kind::sky: sky_moved += moved ? 1 : 0; break;
					case at_kind::pole: pole_moved += moved ? 1 : 0; break;
					default:
						if (yalms > 110.0f)
						{
							far_ground++;
							far_moved += moved ? 1 : 0;
						}
						else if (yalms < 25.0f)
							near_moved += moved ? 1 : 0;
					}
				}
			std::printf("  heat shimmer: %u of %u pixels of ground past 110 yalms moved; within 25 yalms %u, the sky %u, the pole %u\n", far_moved, far_ground, near_moved,
				sky_moved, pole_moved);
			check(st_heat.shimmer && far_ground > 500 && far_moved > far_ground / 5, "heat shimmer moves the ground far off");
			check(near_moved == 0 && sky_moved == 0 && pole_moved == 0, "... and not the ground near the camera, the sky, or what stands in front of it");

			// An aurora: green over the night sky (no stars, so they don't count); by day, none
			look::sky_dome dark_sky = night;
			look::weather_fx aurora_wx;
			aurora_wx.aurora = 1.0f;
			aurora_wx.ground_y = at.y;
			draw_stats st_n0, st_n1, st_d1;
			const std::vector<uint8_t> night_plain = weather_frame(dark_sky, calm, st_n0), night_aurora = weather_frame(dark_sky, aurora_wx, st_n1);
			// (by day the core gives it 0.15 of its strength: see e2e)
			look::weather_fx day_wx = aurora_wx;
			day_wx.aurora = 0.15f;
			const std::vector<uint8_t> day_aurora = weather_frame(noon, day_wx, st_d1);
			uint32_t green_night = 0, green_day = 0, violet_night = 0;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					float yalms = 0.0f;
					const size_t i = (size_t(y) * W + x) * 4;
					if (kind_of(x, y, yalms) != at_kind::sky)
						continue;
					const int dr = night_aurora[i] - night_plain[i], dg = night_aurora[i + 1] - night_plain[i + 1], db = night_aurora[i + 2] - night_plain[i + 2];
					green_night += dg > 6 && dg > dr && dg > db ? 1 : 0;
					violet_night += db > 6 && dr > 3 && db > dg ? 1 : 0;
					const int ddg = day_aurora[i + 1] - clear_sky[i + 1];
					green_day += ddg > 6 ? 1 : 0;
				}
			std::printf("  aurora: %u sky pixels lit green by night (%u violet), %u by day\n", green_night, violet_night, green_day);
			if (FILE *f = nullptr; fopen_s(&f, "build\\aurora_test.ppm", "wb") == 0 && f != nullptr)
			{
				std::fprintf(f, "P6 %u %u 255\n", W, H);
				for (size_t i = 0; i < size_t(W) * H; ++i)
					std::fwrite(&night_aurora[i * 4], 1, 3, f);
				fclose(f);
			}
			check(st_n1.sky && green_night > 300 && violet_night > 20, "an aurora's colour at night: green curtains (violet higher)");
			check(green_day < green_night / 10, "... hardly any on a day's sky");

			// Gloom: fog banks along the ground, under the camera: the ground 15 to 30 yalms off fogged, the pole (20 yalms off,
			// all of it well over the camera) hardly
			look::weather_fx gloom_wx = wx_for(weather_name("fogd"), noon);
			gloom_wx.gloom = 1.0f;
			gloom_wx.cloud_cover = gloom_wx.cloud_veil = 0.0f;
			draw_stats st_gloom;
			const std::vector<uint8_t> gloomy = weather_frame(noon, gloom_wx, st_gloom);
			double ground_fog = 0.0, pole_fog = 0.0;
			uint32_t fog_ground_n = 0, fog_pole_n = 0;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					float yalms = 0.0f;
					const size_t i = (size_t(y) * W + x) * 4;
					const at_kind k = kind_of(x, y, yalms);
					if (k == at_kind::pole)
					{
						pole_fog += off_by(clear_sky, gloomy, i);
						fog_pole_n++;
					}
					else if (k == at_kind::ground && yalms > 15.0f && yalms < 30.0f)
					{
						ground_fog += off_by(clear_sky, gloomy, i);
						fog_ground_n++;
					}
				}
			ground_fog /= std::max(1u, fog_ground_n);
			pole_fog /= std::max(1u, fog_pole_n);
			std::printf("  gloom: the ground 15 to 30 yalms off changed by %.1f on average (%u pixels), the pole over the camera %.1f (%u)\n", ground_fog, fog_ground_n, pole_fog, fog_pole_n);
			check(st_gloom.air && fog_ground_n > 500 && ground_fog > 8.0 && ground_fog > 4.0 * pole_fog, "fog banks lower than the camera in gloom (the ground fogged, the pole over it hardly)");
			check(g.report_messages() == 0, "no debug-layer errors (the weather of its own)");
		}
		release(dome_vb);
		release(dome_pso);
		release(dome_root);
	}

	if (clouds_only)
	{
		std::printf(failures == 0 ? "\nall passed (the sky and its clouds only)\n" : "\n%d failed (the sky and its clouds only)\n", failures);
		return failures == 0 ? 0 : 1;
	}

	// 1z. The wind (grass on, nothing else of the weather): in gales blowing to the picture's right across the ground, the
	// blades lean that way: their pixels' middle moves right, against the same frame calm
	{
		auto frame = [&](float strength) {
			fresh_memory();
			const float ground[4] = { 0.35f, 0.3f, 0.2f, 0.5f };
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, ground, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.time = 0.0f;
			l2.wx.wind[0] = cam.view[0]; // (the camera's right, across the ground)
			l2.wx.wind[1] = cam.view[8];
			l2.wx.wind_strength = strength;
			l2.wx.ground_y = at.y;
			const draw_stats st = r.draw(g.list, t, cam, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			std::vector<uint8_t> img = g.read(world, 4);
			r.release_old(0);
			return std::make_pair(img, st);
		};
		auto middle = [&](const std::vector<uint8_t> &img, uint32_t &n) {
			double sx = 0.0;
			n = 0;
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					const uint8_t *p = &img[(size_t(y) * W + x) * 4];
					if (p[1] > p[0])
					{
						sx += x;
						n++;
					}
				}
			return n != 0 ? sx / n : 0.0;
		};
		const auto calm = frame(0.0f), gales = frame(2.0f);
		uint32_t calm_n = 0, gale_n = 0;
		const double calm_x = middle(calm.first, calm_n), gale_x = middle(gales.first, gale_n);
		std::printf("  wind: the blades' middle at x %.1f calm (%u pixels), %.1f in gales blowing right (%u)\n", calm_x, calm_n, gale_x, gale_n);
		check(!calm.second.weather && gales.second.weather && gales.second.blades == calm.second.blades && gale_x > calm_x + 2.0 && gale_n > 1000,
			"in gales the grass leans hard with the wind");
		check(g.report_messages() == 0, "no debug-layer errors (wind)");
	}

	// 1za. The blades' own motion (look::blade_motion: FSR 2's, the upscale's), in gales blowing to the picture's right, the
	// camera still: a frame at 1 s, then one at 1.1 s writes beside the blades where each was at 1 s (their sway in the
	// wind): w 1 on most blade pixels and a motion there, nothing (w 0) elsewhere, the view depth in z; the picture the same
	// to the byte as without it; and two frames at the same moment: no motion at all
	{
		auto half = [](uint16_t h) {
			const uint32_t sign = uint32_t(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
			uint32_t bits = 0;
			if (e == 0)
				bits = sign; // (zero and the tiny ones: none here)
			else if (e == 31)
				bits = sign | 0x7F800000u | (m << 13);
			else
				bits = sign | ((e + 112) << 23) | (m << 13);
			float f = 0.0f;
			std::memcpy(&f, &bits, 4);
			return f;
		};
		auto frame = [&](float time, bool motion, draw_stats &st, std::vector<uint8_t> *motion_px) {
			fresh_memory();
			const float ground[4] = { 0.35f, 0.3f, 0.2f, 0.5f };
			barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.list->ClearRenderTargetView(world_rtv, ground, 0, nullptr);
			g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			ground_depth(cam);
			barrier(g.list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			look l2 = l;
			l2.time = time;
			l2.wx.wind[0] = cam.view[0];
			l2.wx.wind[1] = cam.view[8];
			l2.wx.wind_strength = 2.0f;
			l2.wx.ground_y = at.y;
			l2.blade_motion = motion;
			st = r.draw(g.list, t, cam, l2, error);
			barrier(g.list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			std::vector<uint8_t> img = g.read(world, 4);
			if (motion_px != nullptr)
			{
				ID3D12Resource *tex = nullptr;
				uint64_t frames = 0;
				motion_px->clear();
				if (r.blade_motion(tex, frames))
				{
					const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
					barrier(g.list, tex, read, D3D12_RESOURCE_STATE_COPY_SOURCE);
					*motion_px = g.read(tex, 8);
					barrier(g.list, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, read);
					g.run();
				}
			}
			r.release_old(0);
			return img;
		};
		draw_stats st_prime, st_moving, st_plain, st_still;
		std::vector<uint8_t> moving_mv, still_mv;
		frame(1.0f, true, st_prime, nullptr);
		const std::vector<uint8_t> moving = frame(1.1f, true, st_moving, &moving_mv);
		const std::vector<uint8_t> plain = frame(1.1f, false, st_plain, nullptr);
		frame(1.1f, true, st_prime, nullptr);
		frame(1.1f, true, st_still, &still_mv);
		uint32_t blade_px = 0, moved = 0, other = 0, depth_ok = 0, still_moved = 0;
		double most = 0.0;
		for (size_t i = 0; i + 8 <= moving_mv.size() && moving_mv.size() == size_t(W) * H * 8; i += 8)
		{
			uint16_t h[4];
			std::memcpy(h, &moving_mv[i], 8);
			const float mx = half(h[0]), my = half(h[1]), z = half(h[2]), w = half(h[3]);
			if (w > 0.5f)
			{
				blade_px++;
				const double len = std::sqrt(double(mx) * mx + double(my) * my) * W; // (pixels)
				moved += len > 0.05 ? 1 : 0;
				most = std::max(most, len);
				depth_ok += z > 0.5f && z < 200.0f ? 1 : 0;
			}
			else
				other += (mx != 0.0f || my != 0.0f || z != 0.0f) ? 1 : 0;
		}
		for (size_t i = 0; i + 8 <= still_mv.size(); i += 8)
		{
			uint16_t h[4];
			std::memcpy(h, &still_mv[i], 8);
			if (half(h[3]) > 0.5f && std::sqrt(double(half(h[0])) * half(h[0]) + double(half(h[1])) * half(h[1])) * W > 0.01)
				still_moved++;
		}
		std::printf("  the blades' motion: %u blade pixels, %u of them moved (the most %.2f px), %u with their depth, %u others not empty; the same moment twice: %u moved\n",
			blade_px, moved, most, depth_ok, other, still_moved);
		check(st_moving.blade_motion && !st_plain.blade_motion && blade_px > 1000 && depth_ok == blade_px && other == 0,
			"written beside the blades (w 1, their view depth), nothing elsewhere");
		check(moved > blade_px / 2 && most > 0.2 && most < 40.0, "their sway in gales over a tenth of a second: a motion on most of them");
		check(moving == plain, "the picture the same to the byte with the motion as without");
		check(st_still.blade_motion && still_moved == 0, "two frames at the same moment, the camera still: no motion");
		check(g.report_messages() == 0, "no debug-layer errors (the blades' motion)");
	}

	// 2. The game's state put back
	const std::vector<uint8_t> gvs = compile(kGameShader, "vs_main", "vs_5_0"), gps = compile(kGameShader, "ps_main", "ps_5_0");
	D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
	D3D12_ROOT_PARAMETER params[4] = {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants = { 0, 0, 4 };
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].Descriptor = { 1, 0 };
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable = { 1, &range };
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
	params[3].Descriptor = { 1, 0 };
	D3D12_ROOT_SIGNATURE_DESC rsd = { 4, params };
	ID3DBlob *rs_blob = nullptr, *rs_err = nullptr;
	reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err);
	ID3D12RootSignature *game_root = nullptr;
	g.device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&game_root));
	release(rs_blob);
	release(rs_err);

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
	pd.pRootSignature = game_root;
	pd.VS = { gvs.data(), gvs.size() };
	pd.PS = { gps.data(), gps.size() };
	pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pd.SampleMask = UINT_MAX;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.RasterizerState.DepthClipEnable = TRUE;
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 1;
	pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
	pd.SampleDesc.Count = 1;
	ID3D12PipelineState *game_pso = nullptr;
	check(SUCCEEDED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&game_pso))), "stand-in game pipeline");

	// Its resources: a constant buffer, a 1 x 1 texture behind a descriptor table, a structured buffer
	ID3D12Resource *cb = g.buffer(D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
	ID3D12Resource *sb = g.buffer(D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
	float *mapped = nullptr;
	const float cb_value[4] = { 0.0f, 0.2f, 0.0f, 0.0f }, sb_value[4] = { 0.0f, 0.0f, 0.0f, 0.4f };
	cb->Map(0, nullptr, reinterpret_cast<void **>(&mapped));
	std::memcpy(mapped, cb_value, 16);
	cb->Unmap(0, nullptr);
	sb->Map(0, nullptr, reinterpret_cast<void **>(&mapped));
	std::memcpy(mapped, sb_value, 16);
	sb->Unmap(0, nullptr);
	ID3D12Resource *tex = g.texture(DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
	{
		ID3D12Resource *up = g.buffer(D3D12_HEAP_TYPE_UPLOAD, 512, D3D12_RESOURCE_STATE_GENERIC_READ);
		const float texel[4] = { 0.0f, 0.0f, 0.3f, 0.0f };
		up->Map(0, nullptr, reinterpret_cast<void **>(&mapped));
		std::memcpy(mapped, texel, 16);
		up->Unmap(0, nullptr);
		D3D12_TEXTURE_COPY_LOCATION dst = { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
		D3D12_TEXTURE_COPY_LOCATION src = { up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
		src.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1, 256 };
		g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		barrier(g.list, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		g.run();
		release(up);
	}
	ID3D12DescriptorHeap *srv_heap = nullptr;
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
	g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap));
	g.device->CreateShaderResourceView(tex, nullptr, srv_heap->GetCPUDescriptorHandleForHeapStart());

	ID3D12Resource *screen = g.texture(DXGI_FORMAT_R32G32B32A32_FLOAT, 8, 8, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
	g.device->CreateRenderTargetView(screen, nullptr, screen_rtv);
	const float black[4] = {};
	g.list->ClearRenderTargetView(screen_rtv, black, 0, nullptr);
	barrier(g.list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	// The game binds everything (and list_state notes it, as the add-on does from ReShade's events)
	list_state state;
	ID3D12DescriptorHeap *heaps[] = { srv_heap };
	g.list->SetDescriptorHeaps(1, heaps);
	g.list->SetGraphicsRootSignature(game_root);
	state.set_root(game_root);
	const uint32_t rc_values[2] = { 0x3DCCCCCD, 0x12345678 }; // 0.1f, then junk the shader doesn't read
	g.list->SetGraphicsRoot32BitConstants(0, 2, rc_values, 0);
	state.set_constants(0, 0, 2, rc_values);
	g.list->SetGraphicsRoot32BitConstant(0, 0x40000000, 3);
	state.set_constants(0, 3, 1, std::vector<uint32_t> { 0x40000000 }.data());
	g.list->SetGraphicsRootConstantBufferView(1, cb->GetGPUVirtualAddress());
	state.arg(1).kind = root_arg::cbv;
	state.arg(1).value = cb->GetGPUVirtualAddress();
	g.list->SetGraphicsRootDescriptorTable(2, srv_heap->GetGPUDescriptorHandleForHeapStart());
	state.arg(2).kind = root_arg::table;
	state.arg(2).value = srv_heap->GetGPUDescriptorHandleForHeapStart().ptr;
	g.list->SetGraphicsRootShaderResourceView(3, sb->GetGPUVirtualAddress());
	state.arg(3).kind = root_arg::srv;
	state.arg(3).value = sb->GetGPUVirtualAddress();
	g.list->SetPipelineState(game_pso);
	state.pipeline = game_pso;
	g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	state.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
	const D3D12_VIEWPORT game_viewport = { 0, 0, 8, 8, 0, 1 };
	const D3D12_RECT game_scissor = { 0, 0, 8, 8 };
	g.list->RSSetViewports(1, &game_viewport);
	g.list->RSSetScissorRects(1, &game_scissor);
	state.viewports[0] = game_viewport;
	state.viewport_count = 1;
	state.scissors[0] = game_scissor;
	state.scissor_count = 1;
	g.list->OMSetRenderTargets(1, &screen_rtv, FALSE, nullptr);
	state.rtvs[0] = screen_rtv;
	state.rtv_count = 1;

	// Grass in between, then the state put back
	error.clear();
	const draw_stats stats2 = r.draw(g.list, t, cam, l, error);
	check(stats2.draws > 0, "grass drawn between the game's binds and its draw");
	state.restore(g.list);

	// The game's draw, nothing bound again
	g.list->DrawInstanced(4, 1, 0, 0);
	barrier(g.list, screen, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
	const std::vector<uint8_t> out = g.read(screen, 16);
	uint32_t right = 0;
	for (size_t i = 0; i < 64; ++i)
	{
		const float *p = reinterpret_cast<const float *>(&out[i * 16]);
		if (std::fabs(p[0] - 0.1f) < 1e-4f && std::fabs(p[1] - 0.2f) < 1e-4f && std::fabs(p[2] - 0.3f) < 1e-4f && std::fabs(p[3] - 0.4f) < 1e-4f)
			right++;
	}
	const float *p0 = reinterpret_cast<const float *>(&out[0]);
	std::printf("  game draw after grass: %u of 64 pixels right, first (%.3f, %.3f, %.3f, %.3f)\n", right, p0[0], p0[1], p0[2], p0[3]);
	check(right == 64, "the game's draw after grass sees all its state (root args, pipeline, strip, viewport, target)");
	check(g.report_messages() == 0, "no debug-layer errors (state put back)");

	// 3. The game's visibility tests (vistest): the world's depth 1 (nothing drawn) with a wall at depth 0.4 over its
	// left half (0.4: no rounding tie in D24, as 0.5 has); boxes on either side, across the edge, partly off the screen,
	// larger than the screen and its 256 round it, at the wall's depth exactly; each box's points read back from the
	// slot once its stamp + 2 is done, against the model of the game's count below (research/ffxi-re/data/vistest-re.md
	// section 0: 06C300's two clamps, 06C530's xs x ys share of the 16 x 16 and its point-sampled UVs, 06C8B0's every
	// other texel).
	{
		const float kWall = 0.4f;
		auto model = [&](const vistest::box &b, float wall_x) {
			const int w = int(b.width), h = int(b.height);
			const int ax = int(b.x0), ay = int(b.y0), cx2 = int(b.x1), cy2 = int(b.y1); // (_ftol: toward zero)
			const int x0 = std::min(ax, cx2), y0 = std::min(ay, cy2), x1 = std::max(ax, cx2), y1 = std::max(ay, cy2);
			const int px0 = std::max(x0, -256), py0 = std::max(y0, -256), px1 = std::min(x1, w + 256), py1 = std::min(y1, h + 256);
			const int sx0 = std::max(x0, 0), sy0 = std::max(y0, 0), sx1 = std::min(x1, w), sy1 = std::min(y1, h);
			if (sx1 - sx0 <= 0 || sy1 - sy0 <= 0 || px1 - px0 <= 0 || py1 - py0 <= 0)
				return 0u;
			const int xs = 16 * (sx1 - sx0) / (px1 - px0), ys = 16 * (sy1 - sy0) / (py1 - py0);
			uint32_t n = 0;
			for (int bb = 0; bb < 8; ++bb)
				for (int a = 0; a < 8; ++a)
				{
					if (2 * a >= xs || 2 * bb >= ys)
						continue;
					const int x = std::clamp(int(std::floor(px0 + 1 + double(px1 - px0 - 1) * (2 * a) / xs)), 0, w - 1);
					const int y = std::clamp(int(std::floor(py0 + 1 + double(py1 - py0 - 1) * (2 * bb) / ys)), 0, h - 1);
					(void)y;
					const float scene = x < int(wall_x) ? kWall : 1.0f;
					if (uint32_t(b.depth * 16777215.0f + 0.5f) <= uint32_t(scene * 16777215.0f + 0.5f))
						n++;
				}
			return n;
		};
		const float wall_x = W / 2.0f;
		const D3D12_RECT left = { 0, 0, LONG(wall_x), LONG(H) };
		g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		g.list->ClearDepthStencilView(world_dsv, D3D12_CLEAR_FLAG_DEPTH, kWall, 0, 1, &left);
		const float fw = float(W), fh = float(H);
		const vistest::box boxes[] = {
			{ 400.0f, 100.0f, 480.0f, 180.0f, 0.9f, fw, fh, vistest::glow },     // right half, nothing in front: all
			{ 40.0f, 100.0f, 120.0f, 180.0f, 0.9f, fw, fh, vistest::glow_fade }, // left half, behind the wall: none
			{ 40.0f, 100.0f, 120.0f, 180.0f, 0.3f, fw, fh, vistest::glow },      // left half, in front of it: all
			{ 280.0f, 50.0f, 360.0f, 130.0f, 0.9f, fw, fh, vistest::flare },     // across the wall's edge: half
			{ 600.0f, 320.0f, 680.0f, 400.0f, 0.9f, fw, fh, vistest::glow },     // a quarter on the screen
			{ 40.0f, 200.0f, 120.0f, 280.0f, kWall, fw, fh, vistest::glow },    // at the wall's depth (LESSEQUAL): all
			{ 120.0f, 180.0f, 40.0f, 100.0f, 0.9f, fw, fh, vistest::glow },      // corners the other way round: none
			{ 100.0f, 100.0f, 100.0f, 180.0f, 0.3f, fw, fh, vistest::glow },     // no width: none
			{ -1000.0f, -1000.0f, 2000.0f, 2000.0f, 0.9f, fw, fh, vistest::door }, // past the screen and its 256: 4 x 3 cells
			{ 700.0f, 100.0f, 800.0f, 180.0f, 0.3f, fw, fh, vistest::glow },     // off the screen: none
			{ -0.9f, 100.9f, 79.9f, 180.9f, 0.9f, fw, fh, vistest::glow },       // corners truncated toward zero: as 0..79 x 100..180
		};
		const uint32_t n = uint32_t(sizeof(boxes) / sizeof(boxes[0]));
		uint32_t objects[vistest::kMaxBoxes] = {}, points[vistest::kMaxBoxes] = {};
		for (uint32_t i = 0; i < n; ++i)
			objects[i] = 0x1000 + i;
		error.clear();
		check(r.set_vis_boxes(objects, boxes, n, 7, error), "visibility test boxes set");
		check(r.read_vis(100, objects, points) == 0, "no slot ready before it's drawn");
		look lv = l;
		lv.grass = false;
		const draw_stats vs = r.draw(g.list, t, cam, lv, error);
		if (!error.empty())
			std::printf("  %s\n", error.c_str());
		check(vs.vis_boxes == n && vs.bound, "the boxes tested in the draw (and the state marked to put back)");
		g.run();
		check(r.read_vis(8, objects, points) == 0, "the slot not read while its stamp + 2 isn't done");
		const uint32_t got = r.read_vis(9, objects, points);
		check(got == n, "the slot read once its stamp + 2 is done");
		bool all_match = got == n;
		for (uint32_t i = 0; i < got; ++i)
		{
			const uint32_t want = model(boxes[i], wall_x);
			std::printf("  box %u (object %X): %u of 64 points passed, the model %u\n", i, objects[i], points[i], want);
			all_match = all_match && objects[i] == 0x1000 + i && points[i] == want;
		}
		check(all_match, "each box's count is the model's");
		check(got == n && points[0] == 64 && points[1] == 0 && points[2] == 64 && points[5] == 64 && points[6] == 0 && points[7] == 0,
			"all behind nothing, none behind the wall, all in front of it, all at its depth, none with no width");
		check(got == n && points[3] == 32 && points[4] == 16, "half across the wall's edge, a quarter partly off the screen");
		check(got == n && points[8] == 6 && points[9] == 0, "a box past the screen and its 256: 12 cells counted, half of them behind the wall; off the screen: none");
		bool off_ok = true;
		for (uint32_t i = 0; i < n; ++i)
			off_ok = off_ok && vistest::off_screen(boxes[i]) == (i == 7 || i == 9);
		check(off_ok, "vistest::off_screen says which boxes the game answers 0 without drawing (no width, off the screen)");
		check(r.read_vis(100, objects, points) == 0, "each slot read once");
		// Four frames of boxes in a row with the GPU never done: the fifth has no slot
		bool all_set = true;
		for (int f = 0; f < 4; ++f)
		{
			all_set = all_set && r.set_vis_boxes(objects, boxes, n, 20 + f, error);
			r.draw(g.list, t, cam, lv, error);
		}
		check(all_set && !r.set_vis_boxes(objects, boxes, n, 24, error), "no slot free while the GPU has all four: the boxes are dropped");
		g.run();
		uint32_t read = 0;
		for (int f = 0; f < 4; ++f)
			read += r.read_vis(26, objects, points) != 0 ? 1u : 0u;
		check(read == 4 && r.set_vis_boxes(objects, boxes, n, 30, error), "each read once done, then free again");
		r.draw(g.list, t, cam, lv, error);
		g.run();
		check(r.read_vis(32, objects, points) == n, "(and the last one read)");
		check(g.report_messages() == 0, "no debug-layer errors (visibility tests)");
	}

	r.unload();
	r.release_old(0);
	release(game_pso);
	release(game_root);
	release(cb);
	release(sb);
	release(tex);
	release(srv_heap);
	release(screen);
	release(world);
	release(depth);
	release(rtv_heap);
	release(dsv_heap);

	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures == 0 ? 0 : 1;
}
