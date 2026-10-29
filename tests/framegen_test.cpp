/*
 * Frame generation's swap chain (src/framegen), offline on WARP with the debug layer, as dgVoodoo would ask for its own:
 *   - with HIFIXI_FRAMEGEN unset, nothing is changed: a swap chain is DXGI's;
 *   - with it set, a window's 1280x720 FLIP_DISCARD swap chain made on a Direct3D 12 queue comes back as AMD's frame
 *     interpolation swap chain (and a 1x1 one, dgVoodoo's probe, as DXGI's);
 *   - through it, as dgVoodoo draws: each back buffer cleared to its own colour and presented, 60 times; resized to
 *     1600x900 and presented 30 more (frame generation off: the game's frames as they are);
 *   - generation on: each frame FSR 2's depth, motion and picture (stand-ins) handed to prepare() before Present, a UI
 *     box drawn over the back buffer: a frame made between each two, the real swap chain shown twice as many;
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
	{
		std::string not_yet;
		const hifi::framegen::frame none;
		const bool refused = !hifi::framegen::prepare(list, none, not_yet);
		check(refused && !not_yet.empty(), "prepare() without FSR 2's frame: refused (" + not_yet + ")");
	}

	// Generation on: FSR 2's frame as upscale's pass hands it over (A 800x450: depth, motion; its picture 1600x900), the
	// back buffer that picture with a UI box over it; each frame prepared, then presented
	auto texture = [&](UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_STATES state) {
		D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		rd.Width = w;
		rd.Height = h;
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.Format = format;
		rd.SampleDesc.Count = 1;
		rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		ID3D12Resource *t = nullptr;
		device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&t));
		return t;
	};
	ID3D12Resource *depth = texture(800, 450, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	ID3D12Resource *motion = texture(800, 450, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	ID3D12Resource *picture = texture(1600, 900, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	check(depth != nullptr && motion != nullptr && picture != nullptr, "FSR 2's depth, motion and picture stand-ins");
	ID3D12DescriptorHeap *rtvs = nullptr;
	const D3D12_DESCRIPTOR_HEAP_DESC rh4 = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4 };
	device->CreateDescriptorHeap(&rh4, IID_PPV_ARGS(&rtvs));
	const UINT rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto rtv = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{ rtvs->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * rtv_step }; };
	device->CreateRenderTargetView(depth, nullptr, rtv(0));
	device->CreateRenderTargetView(motion, nullptr, rtv(1));
	device->CreateRenderTargetView(picture, nullptr, rtv(2));
	auto barrier = [&](ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
		D3D12_RESOURCE_BARRIER b = {};
		b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
		list->ResourceBarrier(1, &b);
	};
	std::string prepare_why;
	// count frames as the game draws them at the swap chain's size w x h: FSR 2's frame prepared (unless without_fsr2),
	// generation turned on or off as the game thread's present does, then presented; the frames prepared and presented
	auto run = [&](UINT count, UINT w, UINT h, bool without_fsr2, uint32_t &prepared, uint32_t &presented) {
		prepared = presented = 0;
		for (UINT f = 0; f < count; ++f)
		{
			const UINT index = chain->GetCurrentBackBufferIndex();
			ID3D12Resource *buffer = nullptr;
			if (FAILED(chain->GetBuffer(index, IID_PPV_ARGS(&buffer))))
				continue;
			device->CreateRenderTargetView(buffer, nullptr, rtv(3));
			alloc->Reset();
			list->Reset(alloc, nullptr);
			// (the world: grey, a bright bar 64 wide moving 16 pixels right each frame; its motion, in A's pixels, back to where it was)
			const float world[4] = { 0.2f, 0.2f, 0.2f, 1.0f }, bright[4] = { 0.8f, 0.8f, 0.8f, 1.0f }, far_depth[4] = { 0.5f }, still[4] = {};
			const float moved[4] = { -16.0f / float(w), 0.0f, 0.0f, 0.0f };
			const LONG bar_x = LONG(400 + 16 * f);
			const D3D12_RECT bar = { bar_x, 0, bar_x + 64, LONG(h) }, bar_in_a = { LONG(bar_x * 800 / w), 0, LONG((bar_x + 64) * 800 / w), 450 };
			// (FSR 2's pass: its depth and motion written, its picture drawn, then onto the back buffer; the UI over that)
			barrier(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			barrier(motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			barrier(picture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			list->ClearRenderTargetView(rtv(0), far_depth, 0, nullptr);
			list->ClearRenderTargetView(rtv(1), still, 0, nullptr);
			list->ClearRenderTargetView(rtv(1), moved, 1, &bar_in_a);
			list->ClearRenderTargetView(rtv(2), world, 0, nullptr);
			list->ClearRenderTargetView(rtv(2), bright, 1, &bar);
			barrier(depth, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			barrier(motion, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			barrier(picture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			barrier(buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
			list->ClearRenderTargetView(rtv(3), world, 0, nullptr);
			list->ClearRenderTargetView(rtv(3), bright, 1, &bar);
			const float ui[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			const D3D12_RECT box = { 40, 40, 400, 120 };
			list->ClearRenderTargetView(rtv(3), ui, 1, &box);
			barrier(buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
			hifi::framegen::frame in;
			in.depth = depth;
			in.motion = motion;
			in.picture = picture;
			in.jitter[0] = (f % 2) ? 0.25f : -0.25f;
			in.fov = 0.8f;
			in.dt_ms = 16.7f;
			in.reset = f == 0;
			if (!without_fsr2 && hifi::framegen::prepare(list, in, prepare_why))
				prepared++;
			list->Close();
			ID3D12CommandList *l = list;
			queue->ExecuteCommandLists(1, &l);
			hifi::framegen::present();
			presented += SUCCEEDED(chain->Present(0, 0)) ? 1 : 0;
			wait();
			buffer->Release();
		}
		wait();
		Sleep(200); // (AMD's presenting thread: its last frames out)
	};
	// the numbers in the status's generation line
	struct counts
	{
		unsigned prepared = 0, made = 0, twice = 0, failed = 99, switches = 0;
		std::string line;
	};
	auto counted = [&]() {
		counts c;
		const std::vector<std::string> st = hifi::framegen::status();
		c.line = st.size() > 1 ? st[1] : std::string();
		const size_t at = c.line.find("; ");
		if (at != std::string::npos)
			sscanf_s(c.line.c_str() + at, "; %u frames prepared, %u made between, %u shown twice, %u failed, turned on/off %u times", &c.prepared, &c.made, &c.twice,
				&c.failed, &c.switches);
		std::printf("  %s\n", c.line.c_str());
		return c;
	};
	auto real_presents = [&]() {
		UINT n = 0;
		chain->GetLastPresentCount(&n);
		return n;
	};

	uint32_t prepared = 0, presented = 0;
	UINT presents_before = real_presents();
	run(60, 1600, 900, false, prepared, presented);
	const counts first = counted();
	check(prepared == 60 && presented == 60, "60 frames prepared and presented with generation on (" + std::to_string(prepared) + ", " + std::to_string(presented) +
		(prepare_why.empty() ? std::string(")") : "; " + prepare_why + ")"));
	check(first.made >= 58 && first.failed == 0 && first.switches == 1, "a frame made between each two, generation turned on once (" + std::to_string(first.made) +
		" made, " + std::to_string(first.failed) + " failed, " + std::to_string(first.switches) + " turned)");
	const UINT doubled = real_presents() - presents_before;
	check(doubled >= 116, "the real swap chain shown twice as many frames as the game presented (" + std::to_string(doubled) + " for 60)");
	wait();

	// The last frame made between (frames 58 and 59: the bar from x 1328, then 1344): the bar halfway, the UI box as drawn
	if (ID3D12Resource *between = hifi::framegen::last_made())
	{
		const D3D12_RESOURCE_DESC md = between->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
		UINT64 bytes = 0;
		device->GetCopyableFootprints(&md, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
		D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = bytes;
		bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *readback = nullptr;
		device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
		alloc->Reset();
		list->Reset(alloc, nullptr);
		barrier(between, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
		src.pResource = between;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.pResource = readback;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint = fp;
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		barrier(between, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		list->Close();
		ID3D12CommandList *l = list;
		queue->ExecuteCommandLists(1, &l);
		wait();
		const uint8_t *px = nullptr;
		readback->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint8_t **>(&px)));
		auto at = [&](UINT x, UINT y) { return px + fp.Offset + SIZE_T(y) * fp.Footprint.RowPitch + SIZE_T(x) * 4; };
		const uint8_t *ui_px = at(200, 80);
		int left = -1, right = -1;
		for (UINT x = 0; x < 1600; ++x)
			if (at(x, 450)[0] > 128)
			{
				if (left < 0)
					left = int(x);
				right = int(x);
			}
		std::printf("  the frame made between: the bar from x %d to %d, the UI box %u %u %u (format %u)\n", left, right, ui_px[0], ui_px[1], ui_px[2], unsigned(md.Format));
		check(left >= 1333 && left <= 1339 && right >= 1396 && right <= 1402, "  the bar in it halfway: from x 1336 (the frames' 1328 and 1344)");
		check(ui_px[0] >= 250 && ui_px[1] >= 250 && ui_px[2] >= 250, "  the UI box in it white, as drawn");
		readback->Unmap(0, nullptr);
		readback->Release();
	}
	else
		check(false, "the last frame made between: none");

	// Resized with generation on (1280x720, FSR 2's picture that size too): the contexts made again, the old ones
	// retired and destroyed once the GPU is past them, frames made between at the new size
	{
		wait();
		const HRESULT rs = chain->ResizeBuffers(2, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
		picture->Release();
		picture = texture(1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		device->CreateRenderTargetView(picture, nullptr, rtv(2));
		run(30, 1280, 720, false, prepared, presented);
		const counts c = counted();
		check(SUCCEEDED(rs) && prepared == 30 && c.failed == 0 && c.made - first.made >= 27 && c.line.find("the UI kept apart") != std::string::npos,
			"resized to 1280x720 with generation on: 30 frames, " + std::to_string(c.made - first.made) + " made between, the UI still kept apart");
	}
	// FSR 2 stopped (a menu, a load): generation off a second later, each frame shown once
	{
		Sleep(1100);
		const UINT before = real_presents();
		run(20, 1280, 720, true, prepared, presented);
		const counts c = counted();
		const UINT shown = real_presents() - before;
		check(c.line.find("off (no FSR 2 frames)") != std::string::npos && c.switches == 2 && shown >= 19 && shown <= 21,
			"FSR 2 stopped: generation turned off, the real swap chain shown each frame once (" + std::to_string(shown) + " for 20)");
		run(20, 1280, 720, false, prepared, presented);
		const counts back = counted();
		check(back.switches == 3 && back.made - c.made >= 18 && back.failed == 0, "FSR 2 back: generation on again (" + std::to_string(back.made - c.made) + " made between)");
	}

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
