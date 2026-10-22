/*
 * Offline test of HiFi XI's Direct3D 12 hooks (src/host/hooks.cpp), the way the plugin meets dgVoodoo: the device and
 * a list already in use when the hooks go in.
 *
 *   build\hook_test.exe            on WARP, no debug layer
 *   build\hook_test.exe --debug    the same with the debug layer on (its classes hooked: they're Direct3D 12's own)
 *   build\hook_test.exe --time     what a hook adds to each call: the same calls timed on a list the hooks see and
 *                                  on one they don't, rounds taken in turn
 *
 * 1. A WARP device, a queue, a "world" target (1024 x 576 R8G8B8A8) and its depth (D24S8), a small other target, a
 *    pipeline that draws a triangle. List A is reset and recorded before the hooks go in.
 * 2. The hooks go in mid-stream; list A goes on recording (those calls count as on a list in an unknown state), is
 *    closed and submitted.
 * 3. Two threads, each with its own allocator and list, record 40 frames the way dgVoodoo does: reset, targets, root
 *    signature and constants, draws, the world's end (its target to pixel-shader-resource), the depth to read, a
 *    clear of the small target, close; submitted on one queue, waited on with a fence.
 * 4. Checks: every call counted exactly; every list submitted of the hooked class; the lists reset since the hooks
 *    went in; the world's target and depth found from the barriers, its end once a frame; list A's calls before its
 *    first reset counted as unknown, none after; out again, nothing more is counted, and the slots are the runtime's
 *    own again.
 */

#include "../src/host/hooks.hpp"
#include "../src/host/events.hpp"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
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

	const char kShader[] = R"(
cbuffer c : register(b0) { float4 colour; };
float4 vs_main(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);
}
float4 ps_main() : SV_Target { return colour; }
)";

	void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = r;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		list->ResourceBarrier(1, &b);
	}
}

int main(int argc, char **argv)
{
	const bool debug_layer = argc > 1 && std::strcmp(argv[1], "--debug") == 0;
	const bool time_mode = argc > 1 && std::strcmp(argv[1], "--time") == 0;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
	const auto create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
	if (debug_layer)
	{
		// (the debug layer's classes are Direct3D 12's own too: everything below runs through them)
		ID3D12Debug *debug = nullptr;
		const auto get_debug = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(d3d12, "D3D12GetDebugInterface"));
		check(get_debug != nullptr && SUCCEEDED(get_debug(IID_PPV_ARGS(&debug))), "the debug layer");
		if (debug != nullptr)
		{
			debug->EnableDebugLayer();
			debug->Release();
		}
	}

	// 1. The device and everything on it, as a game would have them before the hooks
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	ID3D12Device *device = nullptr;
	using create_factory_fn = HRESULT(WINAPI *)(REFIID, void **);
	const auto create_factory = reinterpret_cast<create_factory_fn>(GetProcAddress(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1"));
	if (create_factory == nullptr || FAILED(create_factory(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
		FAILED(create_device(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
	{
		std::printf("FAIL  no WARP device\n");
		return 1;
	}
	ID3D12CommandQueue *queue = nullptr;
	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
	ID3D12Fence *fence = nullptr;
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	UINT64 fence_value = 0;
	auto wait_idle = [&] {
		queue->Signal(fence, ++fence_value);
		fence->SetEventOnCompletion(fence_value, event);
		WaitForSingleObject(event, INFINITE);
	};

	const UINT W = 1024, H = 576;
	D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
	auto texture = [&](DXGI_FORMAT f, UINT w, UINT h, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear) {
		D3D12_RESOURCE_DESC d = {};
		d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		d.Width = w;
		d.Height = h;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = f;
		d.SampleDesc.Count = 1;
		d.Flags = flags;
		ID3D12Resource *r = nullptr;
		device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, clear, IID_PPV_ARGS(&r));
		return r;
	};
	D3D12_CLEAR_VALUE ds_clear = { DXGI_FORMAT_D24_UNORM_S8_UINT };
	ds_clear.DepthStencil.Depth = 1.0f;
	ID3D12Resource *world = texture(DXGI_FORMAT_R8G8B8A8_UNORM, W, H, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
	ID3D12Resource *depth = texture(DXGI_FORMAT_D24_UNORM_S8_UINT, W, H, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &ds_clear);
	ID3D12Resource *other_target = texture(DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
	ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap));
	const D3D12_CPU_DESCRIPTOR_HANDLE world_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	const D3D12_CPU_DESCRIPTOR_HANDLE other_rtv = { world_rtv.ptr + device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) };
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
	device->CreateRenderTargetView(world, nullptr, world_rtv);
	device->CreateRenderTargetView(other_target, nullptr, other_rtv);
	device->CreateDepthStencilView(depth, nullptr, dsv);

	// The pipeline: a triangle over the target, its colour a root constant
	const auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile"));
	ID3DBlob *vs = nullptr, *ps = nullptr, *sig_blob = nullptr;
	compile(kShader, sizeof(kShader), "shader", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vs, nullptr);
	compile(kShader, sizeof(kShader), "shader", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &ps, nullptr);
	D3D12_ROOT_PARAMETER rp = {};
	rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	rp.Constants = { 0, 0, 4 };
	rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
	reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, nullptr);
	ID3D12RootSignature *root = nullptr;
	device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(), IID_PPV_ARGS(&root));
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
	pd.pRootSignature = root;
	pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pd.SampleMask = UINT_MAX;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.DepthStencilState.DepthEnable = TRUE;
	pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 1;
	pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	pd.SampleDesc.Count = 1;
	ID3D12PipelineState *pso = nullptr;
	check(SUCCEEDED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))), "the stand-in game's pipeline");

	// --time: list P is reset before the hooks go in and never submitted, so its own vtable stays the runtime's; list
	// H is made after, so its calls go through the hooks. The same calls (the kinds dgVoodoo makes most) are recorded on
	// each in turn; the difference of the medians over the calls is what a hook costs.
	if (time_mode)
	{
		D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
		D3D12_ROOT_PARAMETER params[3] = { rp };
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[1].Descriptor = { 1, 0 };
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[2].DescriptorTable = { 1, &range };
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_ROOT_SIGNATURE_DESC trsd = { 3, params };
		ID3DBlob *tsig_blob = nullptr;
		reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))(&trsd, D3D_ROOT_SIGNATURE_VERSION_1, &tsig_blob, nullptr);
		ID3D12RootSignature *troot = nullptr;
		device->CreateRootSignature(0, tsig_blob->GetBufferPointer(), tsig_blob->GetBufferSize(), IID_PPV_ARGS(&troot));
		pd.pRootSignature = troot;
		ID3D12PipelineState *tpso = nullptr;
		check(SUCCEEDED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&tpso))), "a pipeline with a constant buffer and a table");
		D3D12_HEAP_PROPERTIES upload = { D3D12_HEAP_TYPE_UPLOAD };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = 256;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *cb = nullptr;
		device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cb));
		D3D12_DESCRIPTOR_HEAP_DESC sd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		ID3D12DescriptorHeap *srv_heap = nullptr;
		device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&srv_heap));
		D3D12_SHADER_RESOURCE_VIEW_DESC null_srv = {};
		null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		null_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		null_srv.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(nullptr, &null_srv, srv_heap->GetCPUDescriptorHandleForHeapStart());
		const D3D12_GPU_DESCRIPTOR_HANDLE table = srv_heap->GetGPUDescriptorHandleForHeapStart();
		const D3D12_GPU_VIRTUAL_ADDRESS cb_at = cb->GetGPUVirtualAddress();

		// A round: a list's worth of the calls, timed from its reset to its close
		const int kRounds = 300, kSteps = 1000;
		uint32_t calls = 0;
		auto round = [&](ID3D12CommandAllocator *a, ID3D12GraphicsCommandList *l) {
			LARGE_INTEGER t0, t1;
			a->Reset();
			QueryPerformanceCounter(&t0);
			l->Reset(a, nullptr);
			l->SetDescriptorHeaps(1, &srv_heap); // (not hooked)
			l->SetGraphicsRootSignature(troot);
			l->OMSetRenderTargets(1, &world_rtv, FALSE, &dsv);
			uint32_t n = 3;
			for (int i = 0; i < kSteps; ++i)
			{
				l->SetPipelineState(tpso);
				l->SetGraphicsRootDescriptorTable(2, table);
				l->SetGraphicsRootConstantBufferView(1, cb_at);
				l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				l->DrawInstanced(3, 1, 0, 0);
				l->DrawInstanced(3, 1, 0, 0);
				l->DrawInstanced(3, 1, 0, 0);
				n += 7;
				if (i % 50 == 49)
				{
					transition(l, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
					transition(l, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
					n += 2;
				}
			}
			l->Close();
			QueryPerformanceCounter(&t1);
			calls = n + 1;
			return double(t1.QuadPart - t0.QuadPart);
		};
		ID3D12CommandAllocator *alloc_p = nullptr, *alloc_h = nullptr;
		ID3D12GraphicsCommandList *list_p = nullptr, *list_h = nullptr;
		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_p));
		device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_p, nullptr, IID_PPV_ARGS(&list_p));
		list_p->Close();
		round(alloc_p, list_p); // (its first reset: its own vtable, the runtime's functions in it)
		std::string why;
		check(hifi::host::install(why), "the hooks in");
		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_h));
		device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_h, nullptr, IID_PPV_ARGS(&list_h));
		list_h->Close();
		for (int i = 0; i < 20; ++i) // (warm-up)
		{
			round(alloc_p, list_p);
			round(alloc_h, list_h);
		}
		std::vector<double> plain, hooked;
		uint64_t counted_on_p = 0;
		for (int i = 0; i < kRounds; ++i)
		{
			hifi::host::reset_stats();
			plain.push_back(round(alloc_p, list_p));
			const hifi::host::stats s = hifi::host::get_stats();
			for (uint64_t c : s.calls)
				counted_on_p += c;
			hooked.push_back(round(alloc_h, list_h));
		}
		const hifi::host::stats s = hifi::host::get_stats();
		check(counted_on_p == 0, "list P's calls didn't go through the hooks (a clean baseline)");
		check(s.calls[hifi::host::list_draw] == 3ull * kSteps, "list H's did");
		std::sort(plain.begin(), plain.end());
		std::sort(hooked.begin(), hooked.end());
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		const double us = 1e6 / double(f.QuadPart);
		const double p = plain[kRounds / 2] * us, h = hooked[kRounds / 2] * us;
		const double per_call = (h - p) * 1000.0 / calls;
		std::printf("  a round of %u calls: %.1f us without the hooks, %.1f us through them (medians of %d); %.1f ns a call\n", calls, p, h, kRounds, per_call);
		std::printf("  (quartiles without %.1f..%.1f us, through %.1f..%.1f us)\n", plain[kRounds / 4] * us, plain[3 * kRounds / 4] * us,
			hooked[kRounds / 4] * us, hooked[3 * kRounds / 4] * us);
		// (3,760 calls a frame through the hooked slots: /hifi in game 2026-10-21, at 5120x2880)
		std::printf("  at the 3,760 calls a frame counted in game: %.3f ms a frame\n", per_call * 3760.0 / 1e6);
		hifi::host::uninstall();
		std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
		return failures == 0 ? 0 : 1;
	}

	// A frame of the world on a list (already reset): what's counted, for the checks
	struct counted
	{
		uint64_t barriers = 0, targets = 0, pipelines = 0, topologies = 0, viewports = 0, scissors = 0, signatures = 0, constants = 0,
			constant = 0, depth_clears = 0, target_clears = 0, draws = 0;
	};
	auto record_world = [&](ID3D12GraphicsCommandList *l, counted &c, bool end) {
		const D3D12_VIEWPORT vp = { 0, 0, float(W), float(H), 0, 1 };
		const D3D12_RECT sc = { 0, 0, LONG(W), LONG(H) };
		l->OMSetRenderTargets(1, &world_rtv, FALSE, &dsv);
		c.targets++;
		l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		c.depth_clears++;
		l->SetGraphicsRootSignature(root);
		c.signatures++;
		l->SetPipelineState(pso);
		c.pipelines++;
		l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		c.topologies++;
		l->RSSetViewports(1, &vp);
		c.viewports++;
		l->RSSetScissorRects(1, &sc);
		c.scissors++;
		for (int i = 0; i < 5; ++i)
		{
			const float colour[4] = { 0.1f * i, 0.2f, 0.3f, 1.0f };
			l->SetGraphicsRoot32BitConstants(0, 4, colour, 0);
			c.constants++;
			l->DrawInstanced(3, 1, 0, 0);
			c.draws++;
		}
		l->SetGraphicsRoot32BitConstant(0, 0, 0);
		c.constant++;
		if (end)
		{
			// The world's end: its target to be read, the depth to read; the other_target target cleared; then back
			transition(l, world, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			transition(l, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
			const float black[4] = {};
			l->OMSetRenderTargets(1, &other_rtv, FALSE, nullptr);
			c.targets++;
			l->ClearRenderTargetView(other_rtv, black, 0, nullptr);
			c.target_clears++;
			transition(l, world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			transition(l, depth, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			c.barriers += 4;
		}
	};

	// List A: in use before the hooks go in
	ID3D12CommandAllocator *alloc_a = nullptr;
	ID3D12GraphicsCommandList *list_a = nullptr;
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_a));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_a, nullptr, IID_PPV_ARGS(&list_a));
	counted before;
	record_world(list_a, before, false);
	// List B: reset and submitted before the hooks, as dgVoodoo's lists are (a reset gave it a vtable of its own, with
	// the driver's functions in many slots), then reset again and left open
	ID3D12CommandAllocator *alloc_b = nullptr;
	ID3D12GraphicsCommandList *list_b = nullptr;
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_b));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_b, nullptr, IID_PPV_ARGS(&list_b));
	list_b->Close();
	for (int f = 0; f < 3; ++f)
	{
		alloc_b->Reset();
		list_b->Reset(alloc_b, nullptr);
		counted pre;
		record_world(list_b, pre, true);
		list_b->Close();
		ID3D12CommandList *b[] = { list_b };
		queue->ExecuteCommandLists(1, b);
		wait_idle();
	}
	alloc_b->Reset();
	list_b->Reset(alloc_b, nullptr);
	void **const b_vtable_before = *reinterpret_cast<void ***>(list_b);

	// 2. The hooks go in, list A and list B still open
	std::string why;
	const bool in = hifi::host::install(why);
	if (!in)
		std::printf("  install: %s\n", why.c_str());
	check(in, "the hooks go in while a list is open");
	if (!in)
		return 1;
	hifi::host::reset_stats();
	counted a_after;
	record_world(list_a, a_after, true);
	list_a->Close();
	ID3D12CommandList *one[] = { list_a };
	queue->ExecuteCommandLists(1, one);
	wait_idle();
	hifi::host::stats s = hifi::host::get_stats();
	const uint64_t a_calls = a_after.barriers / 1 + a_after.targets + a_after.pipelines + a_after.topologies + a_after.viewports + a_after.scissors +
		a_after.signatures + a_after.constants + a_after.constant + a_after.depth_clears + a_after.target_clears + a_after.draws + 1; // (+ the Close)
	std::printf("  list A after the hooks: %llu calls counted as on a list in an unknown state (recorded %llu, barriers %llu)\n",
		static_cast<unsigned long long>(s.lists_unknown_state), static_cast<unsigned long long>(a_calls), static_cast<unsigned long long>(a_after.barriers));
	// (barriers are counted once per ResourceBarrier call: record_world makes them one at a time)
	check(s.lists_unknown_state == a_calls, "every call on the list opened before the hooks counted as in an unknown state");
	check(s.calls[hifi::host::list_draw] == a_after.draws && s.calls[hifi::host::list_barrier] == a_after.barriers, "its draws and barriers counted");
	check(s.lists_executed == 1 && s.lists_foreign == 0, "it was submitted, of the hooked class");
	check(s.world.width == W && s.world.height == H && s.world.format == DXGI_FORMAT_R8G8B8A8_UNORM && s.world.ends == 1,
		"the world's target found from its barrier at the world's end (not the small one)");
	check(s.depth.width == W && s.depth.height == H && s.depth.format == DXGI_FORMAT_D24_UNORM_S8_UINT, "its depth found");

	// List B, on a vtable of its own made before the hooks: unseen until it's submitted, its vtable hooked then, and
	// every call counted from its next reset on
	hifi::host::reset_stats();
	counted b_unseen;
	record_world(list_b, b_unseen, true);
	list_b->Close();
	ID3D12CommandList *b_one[] = { list_b };
	queue->ExecuteCommandLists(1, b_one);
	wait_idle();
	s = hifi::host::get_stats();
	std::printf("  list B's frame before it was seen: %llu draws counted (of %llu), its vtable %s, %llu list vtables hooked\n",
		static_cast<unsigned long long>(s.calls[hifi::host::list_draw]), static_cast<unsigned long long>(b_unseen.draws),
		*reinterpret_cast<void ***>(list_b) == b_vtable_before ? "the same" : "another", static_cast<unsigned long long>(s.vtables_hooked));
	// (the debug layer's lists keep their class's vtable, list A's: list B's calls are seen from the start then)
	const bool own_vtable = b_vtable_before != *reinterpret_cast<void ***>(list_a);
	if (own_vtable)
	{
		check(s.calls[hifi::host::list_draw] == 0 && s.calls[hifi::host::queue_execute] == 1 && s.lists_executed == 1 && s.lists_foreign == 0,
			"its frame before it was submitted went unseen (its own vtable not hooked yet); the submission seen, the list hooked at it");
		check(s.vtables_hooked >= 1 && s.vtables_refused == 0, "its own vtable hooked in place, none refused");
	}
	else
		check(s.calls[hifi::host::list_draw] == b_unseen.draws && s.lists_executed == 1 && s.lists_foreign == 0 && s.vtables_refused == 0,
			"on its class's vtable (the debug layer's): its frame seen from the start, submitted, nothing refused");
	hifi::host::reset_stats();
	alloc_b->Reset();
	list_b->Reset(alloc_b, nullptr);
	counted b_seen;
	record_world(list_b, b_seen, true);
	list_b->Close();
	queue->ExecuteCommandLists(1, b_one);
	wait_idle();
	s = hifi::host::get_stats();
	check(s.calls[hifi::host::list_reset] == 1 && s.calls[hifi::host::list_draw] == b_seen.draws && s.calls[hifi::host::list_barrier] == b_seen.barriers &&
		s.calls[hifi::host::list_root_constants] == b_seen.constants && s.calls[hifi::host::list_pipeline] == b_seen.pipelines &&
		s.calls[hifi::host::list_viewports] == b_seen.viewports && s.calls[hifi::host::list_om_targets] == b_seen.targets,
		"its next frame: the reset, draws, barriers, constants, pipeline, viewports and targets all counted (the driver's slots too)");
	check(s.lists_unknown_state == 0 && s.world.ends == 1, "known from its reset on; the world's end seen");

	// 3. Two threads, 40 frames each half: each records on its own list, then the main thread submits them
	hifi::host::reset_stats();
	const int kFrames = 40;
	ID3D12CommandAllocator *alloc[2] = {};
	ID3D12GraphicsCommandList *list[2] = {};
	for (int t = 0; t < 2; ++t)
	{
		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[t]));
		device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[t], nullptr, IID_PPV_ARGS(&list[t]));
		list[t]->Close(); // (made after the hooks: closed, then reset each frame)
	}
	counted per[2];
	uint64_t resets = 0, closes = 2; // (the two Closes above)
	for (int f = 0; f < kFrames; ++f)
	{
		std::thread th[2];
		for (int t = 0; t < 2; ++t)
			th[t] = std::thread([&, t] {
				alloc[t]->Reset();
				list[t]->Reset(alloc[t], nullptr);
				record_world(list[t], per[t], t == 1); // (the second list ends the world)
				list[t]->Close();
			});
		for (auto &x : th)
			x.join();
		resets += 2;
		closes += 2;
		ID3D12CommandList *two[] = { list[0], list[1] };
		queue->ExecuteCommandLists(2, two);
		wait_idle();
	}
	s = hifi::host::get_stats();
	const uint64_t draws = per[0].draws + per[1].draws, barriers = per[0].barriers + per[1].barriers;
	std::printf("  %d frames on two threads: %llu draws, %llu barriers, %llu resets, %llu closes, %llu lists submitted; counted %llu, %llu, %llu, %llu, %llu\n",
		kFrames, static_cast<unsigned long long>(draws), static_cast<unsigned long long>(barriers), static_cast<unsigned long long>(resets),
		static_cast<unsigned long long>(closes), static_cast<unsigned long long>(2ull * kFrames),
		static_cast<unsigned long long>(s.calls[hifi::host::list_draw]), static_cast<unsigned long long>(s.calls[hifi::host::list_barrier]),
		static_cast<unsigned long long>(s.calls[hifi::host::list_reset]), static_cast<unsigned long long>(s.calls[hifi::host::list_close]),
		static_cast<unsigned long long>(s.lists_executed));
	check(s.calls[hifi::host::list_draw] == draws && s.calls[hifi::host::list_barrier] == barriers && s.calls[hifi::host::list_reset] == resets &&
		s.calls[hifi::host::list_close] == closes, "draws, barriers, resets and closes counted exactly");
	check(s.calls[hifi::host::list_root_constants] == per[0].constants + per[1].constants && s.calls[hifi::host::list_root_constant] == per[0].constant + per[1].constant &&
		s.calls[hifi::host::list_om_targets] == per[0].targets + per[1].targets && s.calls[hifi::host::list_pipeline] == per[0].pipelines + per[1].pipelines &&
		s.calls[hifi::host::list_viewports] == per[0].viewports + per[1].viewports && s.calls[hifi::host::list_scissors] == per[0].scissors + per[1].scissors &&
		s.calls[hifi::host::list_root_signature] == per[0].signatures + per[1].signatures && s.calls[hifi::host::list_topology] == per[0].topologies + per[1].topologies &&
		s.calls[hifi::host::list_clear_depth] == per[0].depth_clears + per[1].depth_clears && s.calls[hifi::host::list_clear_target] == per[0].target_clears + per[1].target_clears,
		"targets, pipelines, topology, viewports, scissors, root signature and constants, clears counted exactly");
	check(s.calls[hifi::host::queue_execute] == uint64_t(kFrames) && s.lists_executed == 2ull * kFrames && s.lists_foreign == 0, "every submission counted, every list of the hooked class");
	check(s.lists_unknown_state == 0, "lists made after the hooks went in known from their making (their Closes before a reset too)");
	check(s.lists_seen >= 2 && s.world.ends == uint64_t(kFrames), "both lists known after their reset; the world's end once a frame");
	check(s.calls[hifi::host::list_execute_indirect] == 0 && s.calls[hifi::host::list_execute_bundle] == 0 && s.calls[hifi::host::list_barrier7] == 0,
		"nothing counted that wasn't called");

	// The events the backend gets: a list made, views made and copied (with their resources and formats), a draw and
	// a barrier; none on a quiet thread
	{
		static int made = 0, views = 0, copies = 0, e_draws = 0, e_barriers = 0;
		static ID3D12Resource *view_resource = nullptr;
		static DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
		static UINT copied = 0;
		static hifi::host::events e;
		e.created = [](ID3D12GraphicsCommandList *) { made++; };
		e.view = [](ID3D12Device *, D3D12_DESCRIPTOR_HEAP_TYPE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource *res, DXGI_FORMAT f, uint32_t) {
			views++;
			view_resource = res;
			view_format = f;
		};
		e.copy = [](ID3D12Device *, D3D12_DESCRIPTOR_HEAP_TYPE, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *sizes) {
			copies++;
			copied = sizes != nullptr ? sizes[0] : 1;
		};
		e.draw = [](ID3D12GraphicsCommandList *, uint64_t) { e_draws++; };
		e.barrier = [](ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *) { e_barriers++; };
		hifi::host::set_events(&e);
		ID3D12CommandAllocator *alloc_e = nullptr;
		ID3D12GraphicsCommandList *list_e = nullptr;
		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_e));
		device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_e, nullptr, IID_PPV_ARGS(&list_e));
		D3D12_RENDER_TARGET_VIEW_DESC vd = {};
		vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		device->CreateRenderTargetView(world, &vd, other_rtv);
		device->CopyDescriptorsSimple(1, other_rtv, world_rtv, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		counted ec;
		record_world(list_e, ec, true);
		{
			hifi::host::quiet q;
			list_e->DrawInstanced(3, 1, 0, 0);
			device->CreateRenderTargetView(other_target, nullptr, other_rtv);
		}
		list_e->Close();
		queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList *const *>(&list_e));
		wait_idle();
		hifi::host::set_events(nullptr);
		device->CreateRenderTargetView(other_target, nullptr, other_rtv); // (the descriptor as it was)
		check(made == 1, "a list made: the backend told");
		check(views == 1 && view_resource == world && view_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "a view made: its resource and format told (the quiet one not)");
		check(copies == 1 && copied == 1, "a view copied: told");
		check(e_draws == int(ec.draws) && e_barriers == int(ec.barriers), "draws and barriers told, the quiet draw not");
		list_e->Release();
		alloc_e->Release();
	}

	// List A again, reset now: known from here on
	hifi::host::reset_stats();
	alloc_a->Reset();
	list_a->Reset(alloc_a, nullptr);
	counted a_reset;
	record_world(list_a, a_reset, false);
	list_a->Close();
	queue->ExecuteCommandLists(1, one);
	wait_idle();
	s = hifi::host::get_stats();
	check(s.lists_unknown_state == 0, "list A after a reset: none of its calls unknown");

	// 4. Out again: nothing more counted, and the runtime's own functions back in the slots
	hifi::host::uninstall();
	check(!hifi::host::installed(), "the hooks out");
	hifi::host::reset_stats();
	alloc[0]->Reset();
	list[0]->Reset(alloc[0], nullptr);
	counted after;
	record_world(list[0], after, true);
	list[0]->Close();
	ID3D12CommandList *last[] = { list[0] };
	queue->ExecuteCommandLists(1, last);
	wait_idle();
	s = hifi::host::get_stats();
	uint64_t total = 0;
	for (uint64_t c : s.calls)
		total += c;
	check(total == 0 && s.lists_executed == 0, "out: nothing counted");
	check(hifi::host::install(why), "in again");
	hifi::host::uninstall();

	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures == 0 ? 0 : 1;
}
