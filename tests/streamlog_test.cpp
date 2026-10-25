/*
 * Offline test of the stream log (src/backend/streamlog.cpp, /hifi streamlog) on HiFi XI's hooks, on WARP.
 *
 *   build\streamlog_test.exe
 *
 * The hooks go in; a "world" (1024 x 576 target and its D24S8 depth) and a small target, their views known to the log the
 * way the backend's view map knows dgVoodoo's. Frames are recorded the way dgVoodoo records them (a Reset, the
 * world's barriers, its colour clear then its depth clear over all of it, draws, the world's end), each with draws of its
 * own vertex count so the file shows which frames went in:
 *   frame 0 (9 vertices) before the present that starts the log: not in it;
 *   frame 1 (3 vertices, plus a stencil-only one-pixel clear, a barrier on the stencil plane, one of HiFi XI's own
 *   draws made quiet, a list submitted): the log's one frame, its colour clear and barriers taken from before its depth
 *   clear;
 *   frame 2 (7 vertices): its start ends the log; nothing of it in.
 * The game's draws (as Ashita hands them) between the presents: in the file with no FFXiMain callers (not loaded here).
 * Then: the totals by target, the tracer off once written (more calls change nothing), a log with no world frame
 * written after its wait, the caller scan on a made-up image (calls of each kind found, other words not).
 */

#include "../src/backend/streamlog.hpp"
#include "../src/host/hooks.hpp"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
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

	std::map<uint64_t, ID3D12Resource *> g_views;
	void *view_of(uint64_t v)
	{
		const auto it = g_views.find(v);
		return it != g_views.end() ? it->second : nullptr;
	}

	const char kShader[] = R"(
float4 vs_main(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);
}
float4 ps_main() : SV_Target { return float4(1, 0, 0, 1); }
)";

	void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = r;
		b.Transition.Subresource = sub;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		list->ResourceBarrier(1, &b);
	}

	// The tap the plugin gives the log (hifps::exp::draw_tap's place): the taps kept, what they call on fakes that
	// count and keep their arguments
	const void *g_taps[4] = {};
	int g_tap_ons = 0, g_tap_offs = 0;
	bool g_tap_fails = false;
	uint32_t g_forwarded[4] = {};
	uint32_t g_args[9] = {};
	void *const kDevice = reinterpret_cast<void *>(uintptr_t(0x5EED0000));
	void keep(std::initializer_list<uint32_t> a)
	{
		uint32_t i = 0;
		for (uint32_t v : a)
			g_args[i++] = v;
	}
	long __stdcall fake_dp(void *self, uint32_t t, uint32_t start, uint32_t n)
	{
		g_forwarded[0]++;
		keep({ uint32_t(uintptr_t(self)), t, start, n });
		return 0x11;
	}
	long __stdcall fake_dip(void *self, uint32_t t, uint32_t min_index, uint32_t vertices, uint32_t start, uint32_t n)
	{
		g_forwarded[1]++;
		keep({ uint32_t(uintptr_t(self)), t, min_index, vertices, start, n });
		return 0x22;
	}
	long __stdcall fake_dpup(void *self, uint32_t t, uint32_t n, const void *data, uint32_t stride)
	{
		g_forwarded[2]++;
		keep({ uint32_t(uintptr_t(self)), t, n, uint32_t(uintptr_t(data)), stride });
		return 0x33;
	}
	long __stdcall fake_dipup(void *self, uint32_t t, uint32_t min_index, uint32_t vertices, uint32_t n, const void *indices, uint32_t format,
		const void *data, uint32_t stride)
	{
		g_forwarded[3]++;
		keep({ uint32_t(uintptr_t(self)), t, min_index, vertices, n, uint32_t(uintptr_t(indices)), format, uint32_t(uintptr_t(data)), stride });
		return 0x44;
	}
	bool tap_on(const void *const taps[4], void *originals[4], std::string &why)
	{
		g_tap_ons++;
		if (g_tap_fails)
		{
			why = "the client device was not found";
			return false;
		}
		for (int i = 0; i < 4; ++i)
			g_taps[i] = taps[i];
		originals[0] = reinterpret_cast<void *>(&fake_dp);
		originals[1] = reinterpret_cast<void *>(&fake_dip);
		originals[2] = reinterpret_cast<void *>(&fake_dpup);
		originals[3] = reinterpret_cast<void *>(&fake_dipup);
		return true;
	}
	void tap_off() { g_tap_offs++; }
	// The game's draws as its device calls them, through the taps
	long game_dp(uint32_t t, uint32_t n)
	{
		return reinterpret_cast<long(__stdcall *)(void *, uint32_t, uint32_t, uint32_t)>(g_taps[0])(kDevice, t, 7, n);
	}
	long game_dip(uint32_t t, uint32_t n, uint32_t vertices)
	{
		return reinterpret_cast<long(__stdcall *)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>(g_taps[1])(kDevice, t, 3, vertices, 9, n);
	}
	long game_dpup(uint32_t t, uint32_t n, uint32_t stride)
	{
		static const float data[64] = {};
		return reinterpret_cast<long(__stdcall *)(void *, uint32_t, uint32_t, const void *, uint32_t)>(g_taps[2])(kDevice, t, n, data, stride);
	}
	long game_dipup(uint32_t t, uint32_t n, uint32_t vertices)
	{
		static const uint16_t indices[8] = {};
		static const float data[64] = {};
		return reinterpret_cast<long(__stdcall *)(void *, uint32_t, uint32_t, uint32_t, uint32_t, const void *, uint32_t, const void *, uint32_t)>(g_taps[3])(
			kDevice, t, 1, vertices, n, indices, 101, data, 20);
	}

	std::string read_file(const std::string &path)
	{
		std::ifstream f(path, std::ios::binary);
		std::stringstream s;
		s << f.rdbuf();
		return s.str();
	}

	// The file the line names ("Stream log written: <path> (...)")
	std::string written_path(const std::string &line)
	{
		const std::string head = "Stream log written: ";
		if (line.rfind(head, 0) != 0)
			return std::string();
		const size_t end = line.find(" (", head.size());
		return line.substr(head.size(), end == std::string::npos ? std::string::npos : end - head.size());
	}

	size_t count_of(const std::string &s, const std::string &what)
	{
		size_t n = 0;
		for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1))
			n++;
		return n;
	}

	std::string section(const std::string &s, const char *from, const char *to)
	{
		const size_t a = s.find(from);
		if (a == std::string::npos)
			return std::string();
		const size_t b = to != nullptr ? s.find(to, a) : std::string::npos;
		return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
	}
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	// The caller scan first (no device needed): a made-up module image at a made-up address
	{
		std::vector<uint8_t> image(0x8000, 0x90);
		const uintptr_t base = 0x10000000;
		auto put = [&](uint32_t at, std::initializer_list<uint8_t> bytes) {
			uint32_t i = 0;
			for (uint8_t b : bytes)
				image[at + i++] = b;
		};
		put(0x2000 - 5, { 0xE8, 1, 2, 3, 4 });                   // call rel32
		put(0x3000 - 6, { 0xFF, 0x91, 0x18, 0x01, 0x00, 0x00 }); // call [ecx+118h] (the game's device calls)
		put(0x3100 - 2, { 0xFF, 0xD0 });                         // call eax
		put(0x3200 - 3, { 0xFF, 0x50, 0x10 });                   // call [eax+10h]
		put(0x4000 - 5, { 0x00, 0x00, 0x00, 0x00, 0x00 });       // after no call
		const uint32_t stack[] = { 0x1234, uint32_t(base + 0x4000), uint32_t(base + 0x2000), uint32_t(base + 0x9000), uint32_t(base + 0x3000),
			uint32_t(base + 0x3100), uint32_t(base + 0x3200), uint32_t(base + 0x2000) };
		uint32_t out[8] = {};
		const uint32_t n = hifi::backend::streamlog::callers(image.data(), base, 0x1000, 0x8000, stack, 8, out, 4);
		check(n == 4 && out[0] == 0x2000 && out[1] == 0x3000 && out[2] == 0x3100 && out[3] == 0x3200,
			"the caller scan: return addresses after a call, innermost first, others and words outside the code left out");
		const uint32_t m = hifi::backend::streamlog::callers(image.data(), base, 0x1000, 0x8000, stack, 8, out, 2);
		check(m == 2, "... as many as asked for at most");
	}

	HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
	const auto create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
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
	std::string why;
	check(hifi::host::install(why, warp), "the hooks in");

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
	ID3D12Resource *tiny = texture(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
	ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap));
	const D3D12_CPU_DESCRIPTOR_HANDLE world_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	const D3D12_CPU_DESCRIPTOR_HANDLE tiny_rtv = { world_rtv.ptr + device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) };
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
	device->CreateRenderTargetView(world, nullptr, world_rtv);
	device->CreateRenderTargetView(tiny, nullptr, tiny_rtv);
	device->CreateDepthStencilView(depth, nullptr, dsv);
	g_views[world_rtv.ptr] = world;
	g_views[tiny_rtv.ptr] = tiny;
	g_views[dsv.ptr] = depth;

	const auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile"));
	ID3DBlob *vs = nullptr, *ps = nullptr, *sig_blob = nullptr;
	compile(kShader, sizeof(kShader), "shader", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vs, nullptr);
	compile(kShader, sizeof(kShader), "shader", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &ps, nullptr);
	D3D12_ROOT_SIGNATURE_DESC rsd = {};
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
	check(SUCCEEDED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))), "the stand-in pipeline");

	ID3D12CommandAllocator *alloc = nullptr;
	ID3D12GraphicsCommandList *list = nullptr;
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
	list->Close();

	// A frame as dgVoodoo records one: draws of `vertices` each; extra: frame 1's stencil-only clear, stencil-plane
	// barrier and HiFi XI's own draw
	auto frame = [&](UINT vertices, bool extra) {
		alloc->Reset();
		list->Reset(alloc, nullptr);
		transition(list, world, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		const float fog[4] = { 0.698f, 0.776f, 0.780f, 0.5f };
		list->ClearRenderTargetView(world_rtv, fog, 0, nullptr);
		list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		const D3D12_VIEWPORT vp = { 0.5f, 0.5f, float(W), float(H), 0, 1 };
		const D3D12_RECT sc = { 0, 0, LONG(W), LONG(H) };
		list->OMSetRenderTargets(1, &world_rtv, FALSE, &dsv);
		list->SetGraphicsRootSignature(root);
		list->SetPipelineState(pso);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->RSSetViewports(1, &vp);
		list->RSSetScissorRects(1, &sc);
		for (int i = 0; i < 5; ++i)
			list->DrawInstanced(vertices, 1, 0, 0);
		if (extra)
		{
			transition(list, depth, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
			transition(list, depth, 1, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			const D3D12_RECT pixel = { 2, 0, 3, 1 };
			list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 1, &pixel);
			{
				hifi::host::quiet q;
				list->DrawInstanced(6, 1, 0, 0);
			}
			const float black[4] = {};
			list->ClearRenderTargetView(tiny_rtv, black, 0, nullptr);
		}
		transition(list, world, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		list->Close();
		ID3D12CommandList *const lists[] = { list };
		queue->ExecuteCommandLists(1, lists);
		wait_idle();
	};
	// (the world starts readable, as at the end of a frame)
	alloc->Reset();
	list->Reset(alloc, nullptr);
	transition(list, world, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	list->Close();
	{
		ID3D12CommandList *const lists[] = { list };
		queue->ExecuteCommandLists(1, lists);
		wait_idle();
	}

	namespace sl = hifi::backend::streamlog;
	wchar_t here[MAX_PATH];
	GetCurrentDirectoryW(MAX_PATH, here);
	std::wstring folder = std::wstring(here) + L"\\build\\test_streamlog\\";
	CreateDirectoryW((std::wstring(here) + L"\\build").c_str(), nullptr);
	CreateDirectoryW(folder.c_str(), nullptr);
	sl::set_folder(folder);
	sl::set_views(view_of);
	sl::set_d3d8_tap(tap_on, tap_off);

	check(sl::present().empty() && !sl::busy() && !sl::d3d8_on(), "nothing asked for: idle, the game's draws not looked at");
	check(sl::request(1, "zone off!", "test context line\n"), "a log asked for");
	check(!sl::request(1, "again", ""), "... not a second while it's under way");
	frame(9, false); // (before the present that starts it)
	check(g_tap_ons == 0, "no tap before the log starts");
	check(sl::present().empty() && sl::d3d8_on() && g_tap_ons == 1 && g_tap_offs == 0, "the present after the request starts it: the tap on");
	frame(3, true);
	const long r1 = game_dip(4, 30, 52);
	const bool dip_on = g_forwarded[1] == 1 && r1 == 0x22 && g_args[0] == 0x5EED0000 && g_args[1] == 4 && g_args[2] == 3 && g_args[3] == 52 && g_args[4] == 9 && g_args[5] == 30;
	const long r2 = game_dpup(5, 2, 24);
	const bool dpup_on = g_forwarded[2] == 1 && r2 == 0x33 && g_args[1] == 5 && g_args[2] == 2 && g_args[4] == 24;
	const long r3 = game_dp(4, 99);
	const bool dp_on = g_forwarded[0] == 1 && r3 == 0x11 && g_args[2] == 7 && g_args[3] == 99;
	const long r4 = game_dipup(6, 3, 5);
	const bool dipup_on = g_forwarded[3] == 1 && r4 == 0x44 && g_args[3] == 5 && g_args[4] == 3 && g_args[6] == 101 && g_args[8] == 20;
	check(dip_on && dpup_on && dp_on && dipup_on, "each tap calls on with the game's arguments, and hands back its result");
	check(sl::present().empty() && !sl::d3d8_on() && g_tap_offs == 1, "the next present ends the game's frame (the tap off); the world frame isn't over yet");
	frame(7, false); // (its start ends the log)
	const std::string said = sl::present();
	std::printf("  %s\n", said.c_str());
	const std::string path = written_path(said);
	check(!path.empty() && !sl::busy(), "written at the present after the next world frame's start");
	check(path.find("hifixi-stream-") != std::string::npos && path.find("-zoneoff.txt") != std::string::npos, "the file's name has the time and the label (kept characters only)");
	const std::string log = read_file(path);
	const std::string stream = section(log, "==== Direct3D 12", "==== The game's");
	const std::string game = section(log, "==== The game's", "==== Totals");
	const std::string totals = section(log, "==== Totals", nullptr);
	check(log.find("test context line") != std::string::npos && log.find("World frames asked for: 1; logged: 1. Ended: at the next world frame's start.") != std::string::npos,
		"the header: the context, the frames, how it ended");
	check(count_of(stream, "---- world frame") == 1, "one world frame");
	check(count_of(stream, "DrawInstanced 3 x1") == 5 && count_of(stream, "DrawInstanced 9 x1") == 0 && count_of(stream, "DrawInstanced 7 x1") == 0,
		"its draws and only its: not the frame before the log, nor the one after");
	const size_t colour = stream.find("ClearRenderTargetView R1(1024x576 f28) colour 0.6980"), depth_clear = stream.find("ClearDepthStencilView R2(1024x576 f45) depth, depth 1");
	const size_t barrier = stream.find("ResourceBarrier 1: [R1(1024x576 f28) all 0x80->0x4]"), reset = stream.find("Reset (pipeline none)");
	check(reset != std::string::npos && barrier != std::string::npos && colour != std::string::npos && depth_clear != std::string::npos &&
		reset < barrier && barrier < colour && colour < depth_clear, "the frame's reset, barrier and colour clear from before its depth clear, in order");
	check(stream.find("OMSetRenderTargets R1(1024x576 f28) ds R2(1024x576 f45)") != std::string::npos, "the targets bound, by their resources");
	check(stream.find("RSSetViewports 1: 0.5 0.5 1024.0 x 576.0, z 0..1") != std::string::npos && stream.find("SetPipelineState P1") != std::string::npos &&
		stream.find("SetGraphicsRootSignature S1") != std::string::npos && stream.find("IASetPrimitiveTopology 4 (triangles)") != std::string::npos,
		"state calls with their values");
	check(stream.find("[R2(1024x576 f45) sub 1 0x10->0x20]") != std::string::npos, "a barrier on the depth's stencil plane");
	check(stream.find("ClearDepthStencilView R2(1024x576 f45) stencil, depth 1 stencil 0 rects 1, first 2,0-3,1") != std::string::npos, "the stencil-only one-pixel clear");
	check(stream.find("game  hifi L1   DrawInstanced 6 x1") != std::string::npos, "HiFi XI's own draw marked 'hifi'");
	check(stream.find("game  Q1   ExecuteCommandLists L1") != std::string::npos, "the list submitted");
	check(stream.find("ClearRenderTargetView R3(16x16 f28)") != std::string::npos, "the small target's clear");
	check(count_of(stream, "ClearRenderTargetView R1(1024x576 f28)") == 1 && count_of(stream, "Reset (pipeline") == 1,
		"the next frame's own calls before its depth clear left out");
	check(totals.find("calls: 22; Reset 1; Close 1; ResourceBarrier 4;") != std::string::npos, "totals by kind of call (its colour clear's calls in)");
	check(game.find("D3D8 DIP TRIANGLELIST prims 30 verts 52 []") != std::string::npos && game.find("D3D8 DPUP TRIANGLESTRIP prims 2 stride 24 []") != std::string::npos &&
		game.find("D3D8 DP TRIANGLELIST prims 99 []") != std::string::npos && game.find("D3D8 DIPUP TRIANGLEFAN prims 3 verts 5 []") != std::string::npos &&
		game.find("not tapped") == std::string::npos, "the game's draws of that frame, through the taps");
	check(totals.find("R1(1024x576 f28) + R2(1024x576 f45): 5 draws (0 indexed), 15 vertices") != std::string::npos, "totals by target and depth (HiFi XI's own left out)");
	check(totals.find("4  (not in FFXiMain)") != std::string::npos, "the game's draws by where they're made");

	// Written: the tracer off, nothing more taken in
	frame(5, false);
	check(sl::present().empty() && !sl::busy(), "after: idle, nothing written");

	// Two frames: the second marked before its own calls, the game's frames two too
	check(sl::request(2, "two", ""), "a two-frame log asked for");
	check(sl::present().empty(), "... started");
	frame(4, false);
	game_dp(5, 20);
	check(sl::present().empty() && sl::d3d8_on(), "... the game's first frame ended, its second going on");
	frame(5, false);
	game_dp(5, 21);
	check(sl::present().empty() && !sl::d3d8_on(), "... and ended");
	frame(8, false);
	const std::string said3 = sl::present();
	std::printf("  %s\n", said3.c_str());
	const std::string log3 = read_file(written_path(said3));
	const std::string stream3 = section(log3, "==== Direct3D 12", "==== The game's");
	const size_t second = stream3.find("---- world frame 2");
	const size_t after = second != std::string::npos ? stream3.find('\n', second) + 1 : std::string::npos;
	const std::string next_line = after != std::string::npos ? stream3.substr(after, stream3.find('\n', after) - after) : std::string();
	check(count_of(stream3, "---- world frame") == 2 && next_line.find("Reset (pipeline") != std::string::npos,
		"two world frames, the second's marker right before its reset");
	check(count_of(stream3, "DrawInstanced 4 x1") == 5 && count_of(stream3, "DrawInstanced 5 x1") == 5 && count_of(stream3, "DrawInstanced 8 x1") == 0 &&
		count_of(stream3, "ClearRenderTargetView R1(1024x576 f28)") == 2, "both frames' calls, not the third's");
	check(log3.find("---- game frame 2") != std::string::npos && log3.find("prims 20") != std::string::npos && log3.find("prims 21") != std::string::npos,
		"the game's two frames of draws");

	// No world frame in time: written with what there was
	check(g_tap_ons == 2 && g_tap_offs == 2, "  its tap on for both game frames, off after");
	sl::set_wait_ms(50);
	g_tap_fails = true;
	check(sl::request(2, "", ""), "a second log asked for");
	check(sl::present().empty(), "... started");
	Sleep(80);
	check(sl::present().empty(), "... its game frame 2 still to end");
	Sleep(10);
	const std::string said2 = sl::present();
	std::printf("  %s\n", said2.c_str());
	const std::string log2 = read_file(written_path(said2));
	check(!log2.empty() && log2.find("logged: 0. Ended: no world frame started within 50 ms") != std::string::npos, "no world frame: written after its wait, saying so");
	check(log2.find("(the game's draws not tapped: the client device was not found)") != std::string::npos, "a tap that couldn't go on: the file says why");

	hifi::host::uninstall();
	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures == 0 ? 0 : 1;
}
