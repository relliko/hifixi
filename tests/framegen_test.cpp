/*
 * Frame generation's swap chain (src/framegen), offline on WARP with the debug layer, as dgVoodoo would ask for its own:
 *   - with HIFIXI_FRAMEGEN unset, nothing is changed: a swap chain is DXGI's;
 *   - with it set, a window's 1280x720 FLIP_DISCARD swap chain made on a Direct3D 12 queue comes back as AMD's frame
 *     interpolation swap chain (and a 1x1 one, dgVoodoo's probe, as DXGI's);
 *   - through it, as dgVoodoo draws: each back buffer cleared to its own colour and presented, 60 times; resized to
 *     1600x900 and presented 30 more (frame generation off: the game's frames as they are);
 *   - the debug layer says nothing.
 *
 *   build\framegen_test.exe
 */

#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include "../src/framegen/framegen.hpp"
#include <cstdio>
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
			// (AMD's swap chain waits on its fences' value 0 once at its start and after a resize: a warning, nothing wrong)
			if (m->ID == D3D12_MESSAGE_ID(1424) && m->Severity == D3D12_MESSAGE_SEVERITY_WARNING)
				continue;
			if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING && m->ID != D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE)
			{
				std::printf("  debug layer (%s, id %d): %s\n", m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR ? "error" : "warning", int(m->ID), m->pDescription);
				if (errors++ == 0)
					first = m->pDescription;
			}
		}
		q->ClearStoredMessages();
		return errors;
	}

	HWND make_window(int w, int h)
	{
		WNDCLASSW wc = {};
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"hifixi_framegen_test";
		RegisterClassW(&wc);
		return CreateWindowExW(0, wc.lpszClassName, L"framegen_test", WS_OVERLAPPEDWINDOW, 0, 0, w, h, nullptr, nullptr, wc.hInstance, nullptr);
	}
}

int main()
{
	using create_factory_fn = HRESULT(WINAPI *)(REFIID, void **);
	const auto create_factory1 = reinterpret_cast<create_factory_fn>(GetProcAddress(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1"));

	// Not wanted: nothing changed
	SetEnvironmentVariableA("HIFIXI_FRAMEGEN", nullptr);
	std::string why;
	check(!hifi::framegen::wanted() && hifi::framegen::install(reinterpret_cast<void *>(create_factory1), why), "HIFIXI_FRAMEGEN unset: nothing put in");

	ID3D12Debug *debug = nullptr;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
		debug->EnableDebugLayer();
	IDXGIFactory4 *factory = nullptr;
	create_factory1(IID_PPV_ARGS(&factory));
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
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	ID3D12CommandQueue *queue = nullptr;
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
	ID3D12CommandAllocator *alloc = nullptr;
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	ID3D12GraphicsCommandList *list = nullptr;
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
	list->Close();
	ID3D12Fence *fence = nullptr;
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	UINT64 fence_value = 0;
	auto wait = [&]() {
		queue->Signal(fence, ++fence_value);
		while (fence->GetCompletedValue() < fence_value)
			Sleep(1);
	};

	HWND hwnd = make_window(1280, 720);
	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = 1280;
	sd.Height = 720;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	{
		IDXGISwapChain1 *plain = nullptr;
		factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &plain);
		check(plain != nullptr && hifi::framegen::status()[0].find("not in") != std::string::npos, "unset: the swap chain is DXGI's");
		if (plain != nullptr)
			plain->Release();
	}

	// Wanted: the factory's calls through ours
	SetEnvironmentVariableA("HIFIXI_FRAMEGEN", "1");
	check(hifi::framegen::wanted() && hifi::framegen::install(reinterpret_cast<void *>(create_factory1), why), "HIFIXI_FRAMEGEN=1: the factory's swap chain calls through ours" + (why.empty() ? "" : ": " + why));
	{
		HWND tiny = make_window(64, 64);
		DXGI_SWAP_CHAIN_DESC1 td = sd;
		td.Width = td.Height = 1;
		IDXGISwapChain1 *probe = nullptr;
		factory->CreateSwapChainForHwnd(queue, tiny, &td, nullptr, nullptr, &probe);
		const std::string st = hifi::framegen::status()[0];
		check(probe != nullptr && st.find("0 AMD's, 1 DXGI's") != std::string::npos, "a 1x1 swap chain (dgVoodoo's probe): DXGI's (" + st + ")");
		if (probe != nullptr)
			probe->Release();
		DestroyWindow(tiny);
	}
	IDXGISwapChain1 *chain1 = nullptr;
	const HRESULT hr = factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &chain1);
	const std::string made = hifi::framegen::status()[0];
	std::printf("  %s\n", made.c_str());
	check(SUCCEEDED(hr) && chain1 != nullptr && made.find("1 AMD's") != std::string::npos, "the game window's 1280x720 swap chain: AMD's frame interpolation swap chain");
	if (chain1 == nullptr)
		return 1;
	IDXGISwapChain4 *chain = nullptr;
	check(SUCCEEDED(chain1->QueryInterface(IID_PPV_ARGS(&chain))) && chain != nullptr, "  it answers for IDXGISwapChain4");

	// As dgVoodoo draws: each back buffer cleared and presented
	ID3D12DescriptorHeap *rtv_heap = nullptr;
	const D3D12_DESCRIPTOR_HEAP_DESC rh = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
	device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtv_heap));
	auto frames = [&](UINT count, float base) {
		uint32_t ok = 0;
		float last[4] = {};
		for (UINT f = 0; f < count; ++f)
		{
			const UINT index = chain->GetCurrentBackBufferIndex();
			ID3D12Resource *buffer = nullptr;
			if (FAILED(chain->GetBuffer(index, IID_PPV_ARGS(&buffer))))
				continue;
			device->CreateRenderTargetView(buffer, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
			alloc->Reset();
			list->Reset(alloc, nullptr);
			D3D12_RESOURCE_BARRIER b = {};
			b.Transition.pResource = buffer;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			list->ResourceBarrier(1, &b);
			const float colour[4] = { base, float(f % 8) / 8.0f, 0.25f, 1.0f };
			std::memcpy(last, colour, sizeof(last));
			list->ClearRenderTargetView(rtv_heap->GetCPUDescriptorHandleForHeapStart(), colour, 0, nullptr);
			std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
			list->ResourceBarrier(1, &b);
			list->Close();
			ID3D12CommandList *l = list;
			queue->ExecuteCommandLists(1, &l);
			ok += SUCCEEDED(chain->Present(0, 0)) ? 1 : 0;
			wait();
			buffer->Release();
		}
		return ok;
	};
	check(frames(60, 0.5f) == 60, "60 frames cleared and presented through it");
	{
		DXGI_SWAP_CHAIN_DESC1 now = {};
		chain->GetDesc1(&now);
		wait();
		const HRESULT rs = chain->ResizeBuffers(2, 1600, 900, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
		DXGI_SWAP_CHAIN_DESC1 after = {};
		chain->GetDesc1(&after);
		check(SUCCEEDED(rs) && after.Width == 1600 && after.Height == 900, "resized to 1600x900 (from " + std::to_string(now.Width) + "x" + std::to_string(now.Height) + ")");
	}
	check(frames(30, 0.75f) == 30, "30 more frames at the new size");
	wait();

	const uint32_t errors = debug_messages(info, why);
	check(errors == 0, "no debug-layer errors" + (errors != 0 ? " (" + std::to_string(errors) + ", the first: " + why + ")" : std::string()));
	chain->Release();
	chain1->Release();
	DestroyWindow(hwnd);
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures == 0 ? 0 : 1;
}
