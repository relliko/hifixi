/*
 * The world upscaled to the screen (src/upscale) on a list recorded as dgVoodoo records its own, on WARP with the debug
 * layer. dgVoodoo's stand-in: the world A (320x180, typeless, a light and a dark half split along a slanted edge, a red
 * band along the bottom), the screen's back buffer (640x360), its own root signature, shader-visible heap, table and
 * pipeline bound once; at A's end the resolve (one 6-vertex quad stretching A over the screen), then the "UI" (the same
 * quad into a 64x64 corner, with nothing bound again, as dgVoodoo binds lazily).
 *   - the first frame: the module asks for its screen-size texture and the back buffer's pipeline, the game's stretch
 *     stays (the picture as with FSR off); once prepared, FSR's picture: close to the stretch overall, its edge no wider,
 *     not the same picture;
 *   - the UI drawn on top as dgVoodoo meant it, the same with FSR as without (its pipeline, root signature, heap and
 *     table put back);
 *   - ours goes in at the next draw, at the list's close, and after a barrier that moves the back buffer on (PRESENT);
 *   - none with FSR off, for a draw of more than 6 vertices, or with the world as big as the screen;
 *   - FSR 2 (temporal): the world's depth (typeless D24S8) followed as the native zone follows it, each frame's camera
 *     published as the zone pass's and taken at its end marker; the first frame asks for FSR 2's context, prepare makes it
 *     (FSR 2's shaders compiled with d3dcompiler_47 and reflected), then each frame FSR 2's picture: at twice the world's
 *     size close to the stretch, its colours kept, the UI on top as meant; with the world as big as the screen (FSR 2's
 *     anti-aliasing alone) close to the world itself; temporal_running while it draws; a jittered run stays finite;
 *   - FSR 2's motion pass, with World FX's blades' motion: taken where a blade is still what's seen (its view depth the
 *     world's), the camera's where something nearer was drawn over it and where there's no blade;
 *   - the debug layer says nothing.
 *
 *   build\upscale_test.exe [out.bmp]
 */

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include "../src/upscale/upscale.hpp"
#include "../src/upscale/temporal.hpp"
#include <DirectXPackedVector.h>
#include "../src/zone/frame.hpp"
#include "../src/zone/inject.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

	constexpr UINT IW = 320, IH = 180, OW = 640, OH = 360, UI = 64;

	// dgVoodoo's stand-in: a textured quad (6 vertices from SV_VertexID) through a root table
	const char *kGame = R"(
Texture2D t : register(t0);
SamplerState s : register(s0);
struct o { float4 p : SV_Position; float2 uv : TEXCOORD; };
o vs(uint id : SV_VertexID)
{
	static const float2 c[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
	o r;
	r.uv = c[id];
	r.p = float4(c[id] * float2(2, -2) + float2(-1, 1), 0, 1);
	return r;
}
float4 ps(o i) : SV_Target { return t.Sample(s, i.uv); }
)";

	ID3DBlob *compile(const char *src, const char *entry, const char *target)
	{
		using compile_fn = decltype(&D3DCompile);
		static const auto c = reinterpret_cast<compile_fn>(GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile"));
		ID3DBlob *code = nullptr, *errors = nullptr;
		if (c == nullptr || FAILED(c(src, std::strlen(src), "game", nullptr, nullptr, entry, target, 0, 0, &code, &errors)))
			return nullptr;
		return code;
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
			if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING && m->ID != D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE)
			{
				if (errors++ == 0)
					first = m->pDescription;
			}
		}
		q->ClearStoredMessages();
		return errors;
	}

	DXGI_FORMAT screen_format(uint64_t) { return DXGI_FORMAT_R8G8B8A8_UNORM; }

	// The world's picture: light right of a slanted edge, dark left of it, a red band along the bottom
	uint32_t world_texel(UINT x, UINT y)
	{
		if (y >= 150)
			return 0xFF1E1EC8u; // (R 200, G 30, B 30; RGBA bytes little-endian)
		const uint32_t v = float(x) + 0.5f * float(y) > 200.0f ? 220u : 40u;
		return 0xFF000000u | v << 16 | v << 8 | v;
	}

	struct gpu
	{
		ID3D12Device *device = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		ID3D12CommandAllocator *alloc = nullptr;
		ID3D12GraphicsCommandList *list = nullptr;
		ID3D12Fence *fence = nullptr;
		UINT64 value = 0;
		void run()
		{
			list->Close();
			ID3D12CommandList *l = list;
			queue->ExecuteCommandLists(1, &l);
			queue->Signal(fence, ++value);
			while (fence->GetCompletedValue() < value)
				Sleep(1);
			alloc->Reset();
			list->Reset(alloc, nullptr);
		}
	};

	void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to, D3D12_RESOURCE_BARRIER *out = nullptr)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Transition.pResource = r;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		list->ResourceBarrier(1, &b);
		if (out != nullptr)
			*out = b;
	}

	float luma(const std::vector<uint8_t> &px, UINT x, UINT y)
	{
		const uint8_t *p = &px[(size_t(y) * OW + x) * 4];
		return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
	}
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main(int argc, char **argv)
{
	namespace up = hifi::upscale;
	ID3D12Debug *debug = nullptr;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
		debug->EnableDebugLayer();
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
	gpu g;
	if (FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device))))
	{
		check(false, "a WARP device");
		return 1;
	}
	ID3D12Device *const device = g.device;
	ID3D12InfoQueue *info = nullptr;
	device->QueryInterface(IID_PPV_ARGS(&info));
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr, IID_PPV_ARGS(&g.list));
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
	ID3D12GraphicsCommandList *const list = g.list;

	// dgVoodoo's world (typeless, a UNORM view), one as big as the screen, and the back buffer
	D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, upload = { D3D12_HEAP_TYPE_UPLOAD }, rb = { D3D12_HEAP_TYPE_READBACK };
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.SampleDesc.Count = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	td.Width = IW;
	td.Height = IH;
	ID3D12Resource *A = nullptr, *big = nullptr, *B = nullptr;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&A));
	td.Width = OW;
	td.Height = OH;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&big));
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&B));
	// (the world's picture uploaded, into both: the big one at the same picture doubled)
	{
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = UINT64(OW) * 4 * OH * 2;
		bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *buf = nullptr;
		device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf));
		uint32_t *p = nullptr;
		buf->Map(0, nullptr, reinterpret_cast<void **>(&p));
		for (UINT y = 0; y < IH; ++y)
			for (UINT x = 0; x < IW; ++x)
				p[y * IW + x] = world_texel(x, y);
		uint32_t *q = p + IW * IH;
		for (UINT y = 0; y < OH; ++y)
			for (UINT x = 0; x < OW; ++x)
				q[y * OW + x] = world_texel(x / 2, y / 2);
		buf->Unmap(0, nullptr);
		D3D12_TEXTURE_COPY_LOCATION src = { buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, dst = { A, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
		src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_TYPELESS, IW, IH, 1, IW * 4 };
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		src.PlacedFootprint.Offset = UINT64(IW) * IH * 4;
		src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_TYPELESS, OW, OH, 1, OW * 4 };
		dst.pResource = big;
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		barrier(list, A, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
		barrier(list, big, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
		g.run();
		buf->Release();
	}

	// dgVoodoo's own: a root signature (a table of one view, a static sampler), a shader-visible heap with the worlds'
	// views, its pipeline, an RTV heap
	ID3D12RootSignature *game_root = nullptr;
	ID3D12PipelineState *game_pso = nullptr;
	{
		D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0 };
		D3D12_ROOT_PARAMETER p = {};
		p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		p.DescriptorTable.NumDescriptorRanges = 1;
		p.DescriptorTable.pDescriptorRanges = &range;
		D3D12_STATIC_SAMPLER_DESC s = {};
		s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		const D3D12_ROOT_SIGNATURE_DESC rd = { 1, &p, 1, &s, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		ID3DBlob *blob = nullptr, *errors = nullptr;
		D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
		device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&game_root));
		ID3DBlob *vs = compile(kGame, "vs", "vs_5_0"), *ps = compile(kGame, "ps", "ps_5_0");
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
		d.pRootSignature = game_root;
		d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		d.NumRenderTargets = 1;
		d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		d.SampleDesc.Count = 1;
		d.SampleMask = UINT_MAX;
		d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&game_pso));
	}
	ID3D12DescriptorHeap *game_heap = nullptr, *rtv_heap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&game_heap));
	const D3D12_DESCRIPTOR_HEAP_DESC rh = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
	device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtv_heap));
	const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sd.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(A, &sd, game_heap->GetCPUDescriptorHandleForHeapStart());
	device->CreateShaderResourceView(big, &sd, { game_heap->GetCPUDescriptorHandleForHeapStart().ptr + step });
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	device->CreateRenderTargetView(B, nullptr, rtv);

	// Readback of the back buffer
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = UINT64(OW) * 4 * OH;
	bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *readback = nullptr;
	device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));

	enum class after_resolve { draw, close, present_barrier };
	// One frame as dgVoodoo records it: the world's end, the resolve (vertices), then the UI. Returns the back buffer.
	auto frame = [&](ID3D12Resource *world, UINT world_slot, uint64_t resolve_vertices, after_resolve how) {
		up::on_reset(list);
		ID3D12DescriptorHeap *heaps[1] = { game_heap };
		list->SetDescriptorHeaps(1, heaps);
		up::on_heaps(list, 1, heaps);
		const float clear[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
		list->ClearRenderTargetView(rtv, clear, 0, nullptr);
		grass::list_state st;
		st.rtvs[0] = rtv;
		st.rtv_count = 1;
		st.pipeline = game_pso;
		st.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		st.viewports[0] = { 0.0f, 0.0f, float(OW), float(OH), 0.0f, 1.0f };
		st.viewport_count = 1;
		st.scissors[0] = { 0, 0, LONG(OW), LONG(OH) };
		st.scissor_count = 1;
		st.set_root(game_root);
		const D3D12_GPU_DESCRIPTOR_HANDLE table = { game_heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(world_slot) * step };
		st.arg(0).kind = grass::root_arg::table;
		st.arg(0).value = table.ptr;
		list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		list->SetPipelineState(game_pso);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->RSSetViewports(1, st.viewports);
		list->RSSetScissorRects(1, st.scissors);
		list->SetGraphicsRootSignature(game_root);
		list->SetGraphicsRootDescriptorTable(0, table);
		// (the world's end: it turns readable)
		D3D12_RESOURCE_BARRIER b = {};
		barrier(list, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &b);
		up::on_world_end(list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		up::after_barrier(list, 1, &b, st);
		// The resolve
		up::before_draw(list, resolve_vertices, B, rtv.ptr, screen_format, st);
		list->DrawInstanced(6, 1, 0, 0);
		D3D12_RESOURCE_STATES b_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		if (how == after_resolve::close)
		{
			up::before_other(list, st);
			g.run();
			// (the UI on the next list: dgVoodoo binds it all again there)
			list->SetDescriptorHeaps(1, heaps);
			list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			list->SetPipelineState(game_pso);
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			list->SetGraphicsRootSignature(game_root);
			list->SetGraphicsRootDescriptorTable(0, table);
		}
		else if (how == after_resolve::present_barrier)
		{
			barrier(list, B, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT, &b);
			up::after_barrier(list, 1, &b, st);
			barrier(list, B, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET, &b);
			up::after_barrier(list, 1, &b, st);
		}
		// The UI: the same quad into the corner, nothing bound again but the viewport
		const D3D12_VIEWPORT corner = { 0.0f, 0.0f, float(UI), float(UI), 0.0f, 1.0f };
		const D3D12_RECT corner_rect = { 0, 0, LONG(UI), LONG(UI) };
		if (how != after_resolve::close)
			up::before_draw(list, 6, B, rtv.ptr, screen_format, st);
		list->RSSetViewports(1, &corner);
		list->RSSetScissorRects(1, &corner_rect);
		list->DrawInstanced(6, 1, 0, 0);
		barrier(list, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		barrier(list, B, b_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		D3D12_TEXTURE_COPY_LOCATION dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, src = { B, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
		dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, OW, OH, 1, OW * 4 };
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		barrier(list, B, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		g.run();
		std::vector<uint8_t> px(size_t(OW) * OH * 4);
		uint8_t *p = nullptr;
		readback->Map(0, nullptr, reinterpret_cast<void **>(&p));
		std::memcpy(px.data(), p, px.size());
		readback->Unmap(0, nullptr);
		return px;
	};
	auto drawn = [&]() {
		const std::vector<std::string> s = up::status();
		unsigned long long n = 0;
		// (the count before " frames drawn", on or off)
		const size_t at = s.empty() ? std::string::npos : s[0].find(" frames drawn");
		if (at != std::string::npos)
		{
			size_t from = at;
			while (from > 0 && s[0][from - 1] >= '0' && s[0][from - 1] <= '9')
				--from;
			n = std::strtoull(s[0].c_str() + from, nullptr, 10);
		}
		return n;
	};
	auto same = [&](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, bool corner_only) {
		uint32_t differ = 0;
		for (UINT y = 0; y < OH; ++y)
			for (UINT x = 0; x < OW; ++x)
				if ((x < UI && y < UI) == corner_only)
					for (int c = 0; c < 3; ++c)
						differ += a[(size_t(y) * OW + x) * 4 + c] != b[(size_t(y) * OW + x) * 4 + c] ? 1 : 0;
		return differ;
	};
	// (pixels along a row from dark to light past the edge: between a tenth and nine tenths of the way)
	auto edge_width = [&](const std::vector<uint8_t> &px, UINT y) {
		uint32_t w = 0;
		for (UINT x = UI; x < OW; ++x)
		{
			const float l = luma(px, x, y);
			w += l > 40.0f + 18.0f && l < 220.0f - 18.0f ? 1 : 0;
		}
		return w;
	};
	std::string why, first;

	// The game's stretch: FSR off
	up::set_options(false, false, 0.8f);
	const std::vector<uint8_t> stretch = frame(A, 0, 6, after_resolve::draw);
	check(up::prepare(device, why), "the passes made (shaders through AMD's headers, root signature, EASU's pipeline)" + (why.empty() ? "" : ": " + why));
	check(drawn() == 0, "FSR off: nothing drawn");

	// FSR on: the first frame asks, prepare makes, then it's drawn
	up::set_options(true, false, 0.8f);
	const std::vector<uint8_t> asked = frame(A, 0, 6, after_resolve::draw);
	check(drawn() == 0 && same(asked, stretch, false) == 0, "the first frame: the game's stretch while the screen-size texture is asked for");
	check(up::prepare(device, why), "... made off dgVoodoo's thread");
	const std::vector<uint8_t> fsr = frame(A, 0, 6, after_resolve::draw);
	check(drawn() == 1, "then FSR drawn after the resolve, at the UI's draw");
	{
		double diff = 0.0;
		for (UINT y = UI; y < OH; ++y)
			for (UINT x = 0; x < OW; ++x)
				diff += std::fabs(luma(fsr, x, y) - luma(stretch, x, y));
		diff /= double(OH - UI) * OW;
		const uint32_t ws = edge_width(stretch, 100), wf = edge_width(fsr, 100);
		const uint8_t *red = &fsr[(size_t(340) * OW + 400) * 4];
		std::printf("  FSR against the stretch: %.2f levels apart on average; the edge %u pixels wide (the stretch's %u); the red band %u %u %u\n",
			diff, wf, ws, red[0], red[1], red[2]);
		check(same(fsr, stretch, false) > 1000 && diff < 6.0, "FSR's picture: not the stretch's, close to it overall");
		check(wf <= ws && wf >= 1, "its edge no wider than the stretch's");
		check(std::abs(int(red[0]) - 200) <= 3 && std::abs(int(red[1]) - 30) <= 3 && std::abs(int(red[2]) - 30) <= 3, "colours kept (the red band)");
		check(same(fsr, stretch, true) == 0, "the UI drawn on top as dgVoodoo meant it (its state put back)");
		if (argc > 1)
			if (FILE *f = nullptr; fopen_s(&f, argv[1], "wb") == 0 && f != nullptr)
			{
				uint8_t head[54] = { 'B', 'M' };
				auto put = [&](int at, uint32_t v) { std::memcpy(head + at, &v, 4); };
				put(2, 54 + OW * OH * 4);
				put(10, 54);
				put(14, 40);
				put(18, OW);
				put(22, uint32_t(-int32_t(OH)));
				head[26] = 1;
				head[28] = 32;
				fwrite(head, 1, 54, f);
				for (size_t i = 0; i < size_t(OW) * OH; ++i)
				{
					const uint8_t q[4] = { fsr[i * 4 + 2], fsr[i * 4 + 1], fsr[i * 4], 255 };
					fwrite(q, 1, 4, f);
				}
				fclose(f);
			}
	}

	// At the list's close, and after a barrier that moves the back buffer on
	const std::vector<uint8_t> at_close = frame(A, 0, 6, after_resolve::close);
	check(drawn() == 2 && same(at_close, fsr, false) == 0 && same(at_close, stretch, true) == 0, "at the list's close: the same picture, the UI as meant");
	const std::vector<uint8_t> at_barrier = frame(A, 0, 6, after_resolve::present_barrier);
	check(drawn() == 3 && same(at_barrier, fsr, false) == 0, "after a barrier moving the back buffer to PRESENT: the same picture");

	// The game's tint (its resolve's: a cutscene fade) on FSR's picture too, the UI left as drawn
	{
		up::set_tint(0x404000);
		const std::vector<uint8_t> half = frame(A, 0, 6, after_resolve::draw);
		up::set_tint(0x000000);
		const std::vector<uint8_t> black = frame(A, 0, 6, after_resolve::draw);
		up::set_tint(0x808080);
		const std::vector<uint8_t> back = frame(A, 0, 6, after_resolve::draw);
		const uint8_t *was = &fsr[(size_t(340) * OW + 400) * 4], *now = &half[(size_t(340) * OW + 400) * 4];
		std::printf("  the red band with the tint 404000: %u %u %u (without: %u %u %u)\n", now[0], now[1], now[2], was[0], was[1], was[2]);
		check(std::abs(int(now[0]) - int(was[0]) / 2) <= 1 && std::abs(int(now[1]) - int(was[1]) / 2) <= 1 && now[2] == 0 && same(half, fsr, true) == 0,
			"the tint 404000: the world's red and green halved, its blue gone, the UI as drawn");
		uint32_t lit = 0;
		for (UINT y = UI; y < OH; ++y)
			for (UINT x = 0; x < OW; ++x)
				lit += luma(black, x, y) > 0.0f ? 1 : 0;
		check(lit == 0 && same(black, fsr, true) == 0, "the tint 000000: the world black, the UI as drawn");
		check(same(back, fsr, false) == 0, "the tint 808080 again: the picture as before");
		check(drawn() == 6, "drawn each of those frames");
	}

	// Not the resolve: more than 6 vertices; the world as big as the screen; FSR off again
	const std::vector<uint8_t> many = frame(A, 0, 300, after_resolve::draw);
	check(drawn() == 6 && same(many, stretch, false) == 0, "a first draw out of the world of 300 vertices: not taken for the resolve");
	frame(big, 1, 6, after_resolve::draw);
	check(drawn() == 6, "the world as big as the screen: the game's stretch");
	up::set_options(false, false, 0.8f);
	frame(A, 0, 6, after_resolve::draw);
	check(drawn() == 6, "FSR off again: nothing");

	// FSR 2: A's depth and the big one's (as dgVoodoo's: typeless D24S8), each frame's camera as the zone pass's
	{
		namespace zi = hifi::zone::inject;
		auto make_depth = [&](UINT w, UINT h) {
			D3D12_RESOURCE_DESC dd = {};
			dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			dd.Width = w;
			dd.Height = h;
			dd.DepthOrArraySize = 1;
			dd.MipLevels = 1;
			dd.SampleDesc.Count = 1;
			dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
			dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
			D3D12_CLEAR_VALUE cv = {};
			cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			cv.DepthStencil.Depth = 0.995f;
			ID3D12Resource *z = nullptr;
			device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&z));
			ID3D12DescriptorHeap *dh = nullptr;
			const D3D12_DESCRIPTOR_HEAP_DESC dhd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
			device->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&dh));
			D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
			dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			device->CreateDepthStencilView(z, &dv, dh->GetCPUDescriptorHandleForHeapStart());
			list->ClearDepthStencilView(dh->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.995f, 0, 0, nullptr);
			g.run();
			dh->Release();
			return z;
		};
		// (the native zone's tracking: the marker names the depth, dgVoodoo's barriers its planes' states)
		auto follow_depth = [&](ID3D12Resource *z) {
			zi::on_marker(z);
			D3D12_RESOURCE_BARRIER b[2] = {};
			for (UINT k = 0; k < 2; ++k)
			{
				b[k].Transition.pResource = z;
				b[k].Transition.Subresource = k;
				b[k].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
				b[k].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_READ;
			}
			list->ResourceBarrier(2, b);
			zi::on_barrier(2, b);
			g.run();
		};
		ID3D12Resource *Az = make_depth(IW, IH), *bigz = make_depth(OW, OH);
		// The game's camera: looking along -z, its projection at F 242, 16:9 (row vectors)
		float view[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		float proj[16] = {};
		proj[5] = 242.0f / 192.0f;
		proj[0] = proj[5] / (0.9482f * 16.0f / 9.0f);
		proj[10] = -1.0000015f;
		proj[11] = -1.0f;
		proj[14] = -0.10000015f;
		uint32_t seq = 1;
		auto publish = [&](float jx, float jy, UINT w, UINT h) {
			hifi::zone::frame &f = hifi::zone::frame_slot(seq);
			f.native = false;
			std::memcpy(f.view, view, 64);
			std::memcpy(f.proj, proj, 64);
			f.proj[8] -= 2.0f * jx / float(w);
			f.proj[9] += 2.0f * jy / float(h);
			f.jitter[0] = jx;
			f.jitter[1] = jy;
			hifi::zone::publish(f, seq);
			zi::on_zone_end(list, grass::list_state(), zi::world(), seq % 16);
			seq++;
		};
		auto fsr2_drawn = [&]() {
			const std::vector<std::string> st = up::status();
			unsigned long long n = 0;
			for (const std::string &line : st)
				if (line.rfind("upscale: FSR 2", 0) == 0 || line.rfind("upscale: FSR 3.1", 0) == 0)
				{
					const size_t at = line.find(" frames drawn");
					size_t from = at;
					while (from > 0 && line[from - 1] >= '0' && line[from - 1] <= '9')
						--from;
					n = at != std::string::npos ? std::strtoull(line.c_str() + from, nullptr, 10) : 0;
				}
			return n;
		};

		// Twice the world's size (FSR 2 here: FSR 3.1's upscaler after)
		follow_depth(Az);
		up::set_options(true, true, 0.8f, false);
		publish(0.0f, 0.0f, IW, IH);
		frame(A, 0, 6, after_resolve::draw);
		const auto t0 = std::chrono::steady_clock::now();
		check(up::prepare(device, why), "FSR 2's context made off dgVoodoo's thread" + (why.empty() ? "" : ": " + why));
		const double make_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		std::vector<uint8_t> fsr2;
		for (int k = 0; k < 8; ++k)
		{
			publish(0.0f, 0.0f, IW, IH);
			fsr2 = frame(A, 0, 6, after_resolve::draw);
		}
		for (const std::string &line : up::status())
			std::printf("  %s\n", line.c_str());
		check(fsr2_drawn() == 8, "FSR 2 drawn each frame (8) once made, in " + std::to_string(make_s) + " s");
		{
			double diff = 0.0;
			for (UINT y = UI; y < OH; ++y)
				for (UINT x = 0; x < OW; ++x)
					diff += std::fabs(luma(fsr2, x, y) - luma(stretch, x, y));
			diff /= double(OH - UI) * OW;
			const uint8_t *red = &fsr2[(size_t(340) * OW + 400) * 4];
			std::printf("  FSR 2 at twice the size against the stretch: %.2f levels apart on average; the red band %u %u %u\n", diff, red[0], red[1], red[2]);
			check(diff < 8.0 && same(fsr2, stretch, false) > 1000, "FSR 2's picture: its own, close to the stretch overall");
			check(std::abs(int(red[0]) - 200) <= 8 && std::abs(int(red[1]) - 30) <= 8 && std::abs(int(red[2]) - 30) <= 8, "colours kept (the red band)");
			check(same(fsr2, stretch, true) == 0, "the UI drawn on top as dgVoodoo meant it (its state put back after FSR 2)");
			check(up::temporal_running() && up::jitter_phases() >= 8, "running (the jitter goes with it), " + std::to_string(up::jitter_phases()) + " jitter phases");
		}
		// Jittered frames: still a picture (nothing lost or blown out)
		{
			bool finite = true;
			std::vector<uint8_t> px;
			for (uint32_t k = 0; k < 8; ++k)
			{
				const float jx = (k % 2 ? 0.25f : -0.25f), jy = (k % 4 < 2 ? 0.25f : -0.25f);
				publish(jx, jy, IW, IH);
				px = frame(A, 0, 6, after_resolve::draw);
			}
			double mean = 0.0;
			for (UINT y = UI; y < OH; ++y)
				for (UINT x = 0; x < OW; ++x)
					mean += luma(px, x, y);
			mean /= double(OH - UI) * OW;
			double want = 0.0;
			for (UINT y = UI; y < OH; ++y)
				for (UINT x = 0; x < OW; ++x)
					want += luma(stretch, x, y);
			want /= double(OH - UI) * OW;
			finite = std::fabs(mean - want) < 6.0;
			check(finite && fsr2_drawn() == 16, "jittered frames: drawn, the picture as bright as the world (" + std::to_string(mean) + " against " + std::to_string(want) + ")");
		}
		// The game's tint on FSR 2's picture (its copy onto the back buffer), the UI as drawn
		{
			publish(0.0f, 0.0f, IW, IH);
			up::set_tint(0x404000);
			const std::vector<uint8_t> half = frame(A, 0, 6, after_resolve::draw);
			up::set_tint(0x808080);
			const uint8_t *now = &half[(size_t(340) * OW + 400) * 4];
			std::printf("  FSR 2's red band with the tint 404000: %u %u %u\n", now[0], now[1], now[2]);
			check(fsr2_drawn() == 17 && std::abs(int(now[0]) - 100) <= 5 && std::abs(int(now[1]) - 15) <= 5 && now[2] == 0 && same(half, stretch, true) == 0,
				"FSR 2 with the tint 404000: the world's red and green halved, its blue gone, the UI as drawn");
		}
		// The world as big as the screen: FSR 2's anti-aliasing alone (a new context for that size)
		follow_depth(bigz);
		publish(0.0f, 0.0f, OW, OH);
		frame(big, 1, 6, after_resolve::draw);
		check(up::prepare(device, why), "a context for the world as big as the screen" + (why.empty() ? "" : ": " + why));
		std::vector<uint8_t> native;
		for (int k = 0; k < 6; ++k)
		{
			publish(0.0f, 0.0f, OW, OH);
			native = frame(big, 1, 6, after_resolve::draw);
		}
		{
			double diff = 0.0;
			for (UINT y = UI; y < OH; ++y)
				for (UINT x = 0; x < OW; ++x)
				{
					const uint32_t t = world_texel(x / 2, y / 2);
					const float l = 0.299f * float(t & 0xFF) + 0.587f * float((t >> 8) & 0xFF) + 0.114f * float((t >> 16) & 0xFF);
					diff += std::fabs(luma(native, x, y) - l);
				}
			diff /= double(OH - UI) * OW;
			std::printf("  FSR 2 at the world's own size against the world: %.2f levels apart on average\n", diff);
			check(fsr2_drawn() == 23 && diff < 4.0, "the world as big as the screen: FSR 2 drawn, close to the world itself");
		}
		// FSR 3.1's upscaler in FSR 2's place (upscale.fsr3): a context of its own for twice the world's size, its shaders
		// built in (DXC); the same picture as FSR 2's would be, the UI on top as drawn
		{
			// (A's depth back to a target first: follow_depth takes it from one, and the first part left it read)
			D3D12_RESOURCE_BARRIER back[2] = {};
			for (UINT k = 0; k < 2; ++k)
				back[k].Transition = { Az, k, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE };
			list->ResourceBarrier(2, back);
			g.run();
			follow_depth(Az);
			up::set_options(true, true, 0.8f, true);
			publish(0.0f, 0.0f, IW, IH);
			frame(A, 0, 6, after_resolve::draw);
			const auto t3 = std::chrono::steady_clock::now();
			check(up::prepare(device, why), "FSR 3.1's upscaler made off dgVoodoo's thread" + (why.empty() ? "" : ": " + why));
			const double make3 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t3).count();
			const unsigned long long before = fsr2_drawn();
			std::vector<uint8_t> fsr3;
			for (int k = 0; k < 8; ++k)
			{
				publish(0.0f, 0.0f, IW, IH);
				fsr3 = frame(A, 0, 6, after_resolve::draw);
			}
			bool named = false;
			for (const std::string &line : up::status())
			{
				std::printf("  %s\n", line.c_str());
				named |= line.rfind("upscale: FSR 3.1 made", 0) == 0;
			}
			check(named && fsr2_drawn() - before == 8, "FSR 3.1's upscaler drawn each frame (8) once made, in " + std::to_string(make3) + " s");
			double diff = 0.0;
			for (UINT y = UI; y < OH; ++y)
				for (UINT x = 0; x < OW; ++x)
					diff += std::fabs(luma(fsr3, x, y) - luma(stretch, x, y));
			diff /= double(OH - UI) * OW;
			const uint8_t *red = &fsr3[(size_t(340) * OW + 400) * 4];
			std::printf("  FSR 3.1 at twice the size against the stretch: %.2f levels apart on average; the red band %u %u %u\n", diff, red[0], red[1], red[2]);
			check(diff < 8.0 && same(fsr3, stretch, false) > 1000, "FSR 3.1's picture: its own, close to the stretch overall");
			check(std::abs(int(red[0]) - 200) <= 8 && std::abs(int(red[1]) - 30) <= 8 && std::abs(int(red[2]) - 30) <= 8, "  colours kept (the red band)");
			check(same(fsr3, stretch, true) == 0, "  the UI drawn on top as dgVoodoo meant it");
		}
		up::set_options(false, false, 0.8f);

		// FSR 2's motion pass with the blades' motion (World FX's target: R16G16B16A16_FLOAT, xy the motion, z the view
		// depth, w 1 for a blade): the left third blades seen (at the world's depth there, 20 yalms), the middle third blades
		// with something nearer drawn over them since (their depth 5 yalms), the right third none. The camera still.
		{
			namespace tp = hifi::upscale::temporal;
			tp::context tc;
			check(tc.make(device, IW, IH, OW, OH, false, why), "FSR 2's context on its own" + (why.empty() ? "" : ": " + why));
			D3D12_RESOURCE_DESC md = {};
			md.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			md.Width = IW;
			md.Height = IH;
			md.DepthOrArraySize = 1;
			md.MipLevels = 1;
			md.SampleDesc.Count = 1;
			md.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			md.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			ID3D12Resource *blades = nullptr;
			device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &md, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&blades));
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
			UINT64 total = 0;
			device->GetCopyableFootprints(&md, 0, 1, 0, &fp, nullptr, nullptr, &total);
			D3D12_RESOURCE_DESC ud = {};
			ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			ud.Width = total;
			ud.Height = ud.DepthOrArraySize = ud.MipLevels = 1;
			ud.SampleDesc.Count = 1;
			ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ID3D12Resource *ub = nullptr;
			device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ub));
			uint8_t *m = nullptr;
			ub->Map(0, nullptr, reinterpret_cast<void **>(&m));
			using DirectX::PackedVector::XMConvertFloatToHalf;
			for (UINT y = 0; y < IH; ++y)
				for (UINT x = 0; x < IW; ++x)
				{
					const UINT band = x * 3 / IW;
					const float v[4] = { band == 2 ? 0.0f : 0.01f, band == 2 ? 0.0f : -0.02f, band == 0 ? 20.0f : band == 1 ? 5.0f : 0.0f, band == 2 ? 0.0f : 1.0f };
					uint16_t *q = reinterpret_cast<uint16_t *>(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch + size_t(x) * 8);
					for (int k = 0; k < 4; ++k)
						q[k] = XMConvertFloatToHalf(v[k]);
				}
			ub->Unmap(0, nullptr);
			D3D12_TEXTURE_COPY_LOCATION src = { ub, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, dst = { blades, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
			src.PlacedFootprint = fp;
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barrier(list, blades, D3D12_RESOURCE_STATE_COPY_DEST, read);
			tp::camera cam;
			std::memcpy(cam.view, view, 64);
			std::memcpy(cam.proj, proj, 64);
			cam.jitter[0] = cam.jitter[1] = 0.0f;
			std::string fail;
			// (A's depth: DEPTH_READ, as follow_depth left it; A a target, as frame() leaves it)
			const bool recorded = tc.record(list, A, D3D12_RESOURCE_STATE_RENDER_TARGET, Az, D3D12_RESOURCE_STATE_DEPTH_READ, blades, cam, 0.5f, 16.0f, true, fail);
			// (its motion read back)
			ID3D12Resource *mv = tc.motion();
			barrier(list, mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			D3D12_RESOURCE_DESC vd = mv->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT vfp = {};
			UINT64 vtotal = 0;
			device->GetCopyableFootprints(&vd, 0, 1, 0, &vfp, nullptr, nullptr, &vtotal);
			D3D12_RESOURCE_DESC rd = ud;
			rd.Width = vtotal;
			ID3D12Resource *rbuf = nullptr;
			device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbuf));
			D3D12_TEXTURE_COPY_LOCATION rdst = { rbuf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, rsrc = { mv, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
			rdst.PlacedFootprint = vfp;
			list->CopyTextureRegion(&rdst, 0, 0, 0, &rsrc, nullptr);
			barrier(list, mv, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			g.run();
			uint8_t *r = nullptr;
			rbuf->Map(0, nullptr, reinterpret_cast<void **>(&r));
			using DirectX::PackedVector::XMConvertHalfToFloat;
			uint32_t taken = 0, camera_mid = 0, camera_right = 0, band_px[3] = {};
			for (UINT y = 0; y < IH; ++y)
				for (UINT x = 0; x < IW; ++x)
				{
					const uint16_t *q = reinterpret_cast<const uint16_t *>(r + size_t(y) * vfp.Footprint.RowPitch + size_t(x) * 4);
					const float mx = XMConvertHalfToFloat(q[0]), my = XMConvertHalfToFloat(q[1]);
					const UINT band = x * 3 / IW;
					band_px[band]++;
					const bool blade = std::fabs(mx - 0.01f) < 1e-4f && std::fabs(my + 0.02f) < 1e-4f;
					const bool none = std::fabs(mx) < 1e-5f && std::fabs(my) < 1e-5f;
					taken += band == 0 && blade ? 1 : 0;
					camera_mid += band == 1 && none ? 1 : 0;
					camera_right += band == 2 && none ? 1 : 0;
				}
			rbuf->Unmap(0, nullptr);
			std::printf("  the motion pass with the blades' motion: %u of %u seen blades' pixels took it; %u of %u covered and %u of %u without blades the camera's\n",
				taken, band_px[0], camera_mid, band_px[1], camera_right, band_px[2]);
			check(recorded && taken == band_px[0] && camera_mid == band_px[1] && camera_right == band_px[2],
				"a blade's own motion where it's still seen; the camera's where something was drawn over it, and where there's none" + (fail.empty() ? "" : " (" + fail + ")"));
			check(tc.blade_frames() == 1, "  counted as a frame with the grass's own motion");
			rbuf->Release();

			// FSR's masks (masks.hpp) from the world's copies: the opaque world grey all over; with the game's effects (pre_fx,
			// and shaded the same) a bright box in its middle; with World FX's own (pre_air) another at the left as well. The
			// reactive mask in the first box only, the transparency mask in the second only.
			{
				ID3D12DescriptorHeap *rtvs = nullptr;
				const D3D12_DESCRIPTOR_HEAP_DESC rh = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
				device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvs));
				const D3D12_RECT box_fx = { LONG(IW / 2 - 20), LONG(IH / 2 - 20), LONG(IW / 2 + 20), LONG(IH / 2 + 20) };
				const D3D12_RECT box_wfx = { 10, LONG(IH / 2 - 20), 50, LONG(IH / 2 + 20) };
				auto copy_of = [&](bool fx, bool wfx) {
					D3D12_RESOURCE_DESC cd = {};
					cd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
					cd.Width = IW;
					cd.Height = IH;
					cd.DepthOrArraySize = 1;
					cd.MipLevels = 1;
					cd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
					cd.SampleDesc.Count = 1;
					cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
					ID3D12Resource *c = nullptr;
					device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&c));
					device->CreateRenderTargetView(c, nullptr, rtvs->GetCPUDescriptorHandleForHeapStart());
					const float grey[4] = { 0.3f, 0.3f, 0.3f, 1.0f }, white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
					list->ClearRenderTargetView(rtvs->GetCPUDescriptorHandleForHeapStart(), grey, 0, nullptr);
					if (fx)
						list->ClearRenderTargetView(rtvs->GetCPUDescriptorHandleForHeapStart(), white, 1, &box_fx);
					if (wfx)
						list->ClearRenderTargetView(rtvs->GetCPUDescriptorHandleForHeapStart(), white, 1, &box_wfx);
					barrier(list, c, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					g.run();
					return c;
				};
				hifi::upscale::masks::taken mt;
				mt.format = DXGI_FORMAT_R8G8B8A8_UNORM;
				mt.copy[hifi::upscale::masks::opaque] = copy_of(false, false);
				mt.copy[hifi::upscale::masks::pre_fx] = copy_of(true, false);
				mt.copy[hifi::upscale::masks::shaded] = copy_of(true, false);
				mt.copy[hifi::upscale::masks::pre_air] = copy_of(true, true);
				for (bool &f : mt.fresh)
					f = true;
				std::string mfail;
				const bool mrecorded = tc.record(list, A, D3D12_RESOURCE_STATE_RENDER_TARGET, Az, D3D12_RESOURCE_STATE_DEPTH_READ, nullptr, cam, 0.5f, 16.0f, true, mfail, &mt);
				g.run();
				auto read_r8 = [&](ID3D12Resource *t) {
					barrier(list, t, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
					const D3D12_RESOURCE_DESC td = t->GetDesc();
					D3D12_PLACED_SUBRESOURCE_FOOTPRINT tfp = {};
					UINT64 ttotal = 0;
					device->GetCopyableFootprints(&td, 0, 1, 0, &tfp, nullptr, nullptr, &ttotal);
					D3D12_RESOURCE_DESC bd2 = ud;
					bd2.Width = ttotal;
					ID3D12Resource *b2 = nullptr;
					device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd2, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&b2));
					D3D12_TEXTURE_COPY_LOCATION d2 = { b2, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, s2 = { t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
					d2.PlacedFootprint = tfp;
					list->CopyTextureRegion(&d2, 0, 0, 0, &s2, nullptr);
					barrier(list, t, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					g.run();
					std::vector<uint8_t> out(size_t(IW) * IH);
					uint8_t *m = nullptr;
					b2->Map(0, nullptr, reinterpret_cast<void **>(&m));
					for (UINT y = 0; y < IH; ++y)
						std::memcpy(&out[size_t(y) * IW], m + size_t(y) * tfp.Footprint.RowPitch, IW);
					b2->Unmap(0, nullptr);
					b2->Release();
					return out;
				};
				const std::vector<uint8_t> reactive = read_r8(tc.reactive()), comp = read_r8(tc.composition());
				auto inside = [](const D3D12_RECT &b, UINT x, UINT y) { return LONG(x) >= b.left && LONG(x) < b.right && LONG(y) >= b.top && LONG(y) < b.bottom; };
				uint32_t r_in = 0, r_out = 0, t_in = 0, t_out = 0, n_fx = 0, n_wfx = 0;
				for (UINT y = 0; y < IH; ++y)
					for (UINT x = 0; x < IW; ++x)
					{
						const uint8_t rv = reactive[size_t(y) * IW + x], tv = comp[size_t(y) * IW + x];
						const bool in_fx = inside(box_fx, x, y), in_wfx = inside(box_wfx, x, y);
						n_fx += in_fx ? 1 : 0;
						n_wfx += in_wfx ? 1 : 0;
						r_in += in_fx && rv > 150 ? 1 : 0;
						r_out += !in_fx && rv != 0 ? 1 : 0;
						t_in += in_wfx && tv > 120 ? 1 : 0;
						t_out += !in_wfx && tv != 0 ? 1 : 0;
					}
				std::printf("  the masks: reactive in %u of %u of the effects' box, %u outside; transparency in %u of %u of World FX's box, %u outside\n",
					r_in, n_fx, r_out, t_in, n_wfx, t_out);
				check(mrecorded && r_in == n_fx && r_out == 0, "the reactive mask where the game's effects are, nowhere else" + (mfail.empty() ? "" : " (" + mfail + ")"));
				check(t_in == n_wfx && t_out == 0 && tc.mask_frames() == 1, "  the transparency mask where World FX's own see-through things are, nowhere else");
				for (ID3D12Resource *c : mt.copy)
					c->Release();
				rtvs->Release();
			}

			// The characters' own motion (chars::inject's texture: xyz a character's move since the last frame in the world,
			// w its view depth): the left third a character nearer than what's seen there (5 yalms: something drawn over it),
			// the middle third one seen (20 yalms) that moved half a yalm along the view's x, the right third none. Its
			// motion where it's seen: back half a yalm, through the camera; the camera's (none: it's still) elsewhere.
			{
				ID3D12Resource *chars = nullptr, *cub = nullptr;
				device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &md, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&chars));
				device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cub));
				// (half a yalm along the view's x, in the world: VIEW's first column)
				const float move[3] = { 0.5f * view[0], 0.5f * view[4], 0.5f * view[8] };
				uint8_t *cm = nullptr;
				cub->Map(0, nullptr, reinterpret_cast<void **>(&cm));
				for (UINT y = 0; y < IH; ++y)
					for (UINT x = 0; x < IW; ++x)
					{
						const UINT band = x * 3 / IW;
						const float v[4] = { band == 2 ? 0.0f : move[0], band == 2 ? 0.0f : move[1], band == 2 ? 0.0f : move[2], band == 0 ? 5.0f : band == 1 ? 20.0f : 0.0f };
						uint16_t *q = reinterpret_cast<uint16_t *>(cm + fp.Offset + size_t(y) * fp.Footprint.RowPitch + size_t(x) * 8);
						for (int k = 0; k < 4; ++k)
							q[k] = XMConvertFloatToHalf(v[k]);
					}
				cub->Unmap(0, nullptr);
				D3D12_TEXTURE_COPY_LOCATION csrc = { cub, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, cdst = { chars, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
				csrc.PlacedFootprint = fp;
				list->CopyTextureRegion(&cdst, 0, 0, 0, &csrc, nullptr);
				barrier(list, chars, D3D12_RESOURCE_STATE_COPY_DEST, read);
				std::string cfail;
				const bool crecorded = tc.record(list, A, D3D12_RESOURCE_STATE_RENDER_TARGET, Az, D3D12_RESOURCE_STATE_DEPTH_READ, nullptr, cam, 0.5f, 16.0f, true, cfail,
					nullptr, chars);
				ID3D12Resource *cmv = tc.motion();
				barrier(list, cmv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
				ID3D12Resource *crb = nullptr;
				D3D12_RESOURCE_DESC crd = ud;
				crd.Width = vtotal;
				device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &crd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&crb));
				D3D12_TEXTURE_COPY_LOCATION cr_dst = { crb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, cr_src = { cmv, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
				cr_dst.PlacedFootprint = vfp;
				list->CopyTextureRegion(&cr_dst, 0, 0, 0, &cr_src, nullptr);
				barrier(list, cmv, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				g.run();
				uint8_t *cr = nullptr;
				crb->Map(0, nullptr, reinterpret_cast<void **>(&cr));
				const float want_x = -0.25f * proj[0] / 20.0f; // (back half a yalm at 20 yalms: UV units)
				uint32_t moved = 0, still_left = 0, still_right = 0, n_band[3] = {};
				for (UINT y = 0; y < IH; ++y)
					for (UINT x = 0; x < IW; ++x)
					{
						const uint16_t *q = reinterpret_cast<const uint16_t *>(cr + size_t(y) * vfp.Footprint.RowPitch + size_t(x) * 4);
						const float mx = XMConvertHalfToFloat(q[0]), my = XMConvertHalfToFloat(q[1]);
						const UINT band = x * 3 / IW;
						n_band[band]++;
						const bool none = std::fabs(mx) < 1e-4f && std::fabs(my) < 1e-4f;
						moved += band == 1 && std::fabs(mx - want_x) < 0.02f * std::fabs(want_x) + 2e-4f && std::fabs(my) < 2e-4f ? 1 : 0;
						still_left += band == 0 && none ? 1 : 0;
						still_right += band == 2 && none ? 1 : 0;
					}
				crb->Unmap(0, nullptr);
				std::printf("  the characters' motion: %u of %u seen characters' pixels moved back %.4f (UV), %u of %u covered and %u of %u without a character the camera's\n",
					moved, n_band[1], want_x, still_left, n_band[0], still_right, n_band[2]);
				check(crecorded && moved == n_band[1] && still_left == n_band[0] && still_right == n_band[2],
					"a character's own motion where it's seen; the camera's where something was drawn over it, and where there's none" + (cfail.empty() ? "" : " (" + cfail + ")"));
				check(tc.char_frames() == 1, "  counted as a frame with the characters' own motion");
				crb->Release();
				cub->Release();
				chars->Release();
			}
			ub->Release();
			blades->Release();
			tc.release();
		}
		Az->Release();
		bigz->Release();
	}

	const uint32_t errors = debug_messages(info, first);
	check(errors == 0, "no debug-layer errors" + (errors != 0 ? " (" + std::to_string(errors) + ", the first: " + first + ")" : std::string()));
	for (const std::string &line : up::status())
		std::printf("  %s\n", line.c_str());
	up::release();
	std::printf("%d ok, %d failed\n", passes, failures);
	return failures == 0 ? 0 : 1;
}
