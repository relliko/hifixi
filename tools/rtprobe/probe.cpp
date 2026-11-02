// rtprobe: can a 32-bit D3D12 process (FFXI's side) use hardware ray tracing done by a 64-bit helper on the same GPU?
// Built twice from this file: host32.exe (x86) and helper64.exe (x64). The host makes a shared texture, two shared
// fences (req: host -> helper, done: helper -> host) and a shared memory page, all inheritable NT handles, and starts
// the helper with their values on the command line. The helper traces rays (inline, RayQuery) into the texture.
//
// Phases: 1 correctness (the host reads the helper's picture), 2 round-trip latency, 3 GPU sharing cost
// (A host work alone, B + helper consumed a frame late, C + helper waited for in the same frame).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED %08lX: %s (line %d)\n", (unsigned long)hr_, #x, __LINE__); exit(1); } } while (0)

constexpr UINT kW = 1024, kH = 1024;
constexpr UINT kSlots = 4096;

struct shared_page
{
	volatile LONG quit;
	volatile UINT frame;      // the frame tag the helper writes into missed pixels
	volatile UINT spp;        // rays a pixel
	volatile LONG helper_ok;  // 1 once the helper is up, -1 if it failed (reason below)
	volatile LONG dxr_tier;
	volatile LONG gpu_wait;  // 1: the helper queues its work ahead and its GPU waits for req (no CPU in the loop)
	char why[256];
	double helper_gpu_ms[kSlots];     // the helper's dispatch on the GPU, by request number
	LONGLONG helper_wake_qpc[kSlots]; // when the helper's CPU saw request n
};

static double qpc_ms(LONGLONG a, LONGLONG b)
{
	static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
	return double(b - a) * 1000.0 / double(f.QuadPart);
}
static LONGLONG now() { LARGE_INTEGER x; QueryPerformanceCounter(&x); return x.QuadPart; }

static void wait_fence(ID3D12Fence *f, UINT64 v, HANDLE ev)
{
	if (f->GetCompletedValue() >= v)
		return;
	CHECK(f->SetEventOnCompletion(v, ev));
	WaitForSingleObject(ev, INFINITE);
}

static ComPtr<ID3D12Resource> buffer(ID3D12Device *d, D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_FLAGS flags,
	D3D12_RESOURCE_STATES state)
{
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = type;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = size;
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	rd.Flags = flags;
	ComPtr<ID3D12Resource> r;
	CHECK(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)));
	return r;
}

static double stats(std::vector<double> v, double *p50, double *p99)
{
	std::sort(v.begin(), v.end());
	double sum = 0;
	for (double x : v)
		sum += x;
	*p50 = v[v.size() / 2];
	*p99 = v[std::min(v.size() - 1, v.size() * 99 / 100)];
	return sum / double(v.size());
}

#ifdef _WIN64
// ---------------------------------------------------------------- helper (64-bit) ---------------------------------
static shared_page *page;

[[noreturn]] static void fail(const char *why)
{
	if (page)
	{
		strncpy_s(page->why, why, _TRUNCATE);
		page->helper_ok = -1;
	}
	printf("helper: %s\n", why);
	exit(1);
}

int main(int argc, char **argv)
{
	if (argc < 8)
		return 2;
	auto h = [&](int i) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(strtoull(argv[i], nullptr, 16))); };
	HANDLE h_req = h(1), h_done = h(2), h_tex = h(3), h_page = h(4);
	LUID luid;
	luid.LowPart = static_cast<DWORD>(strtoul(argv[5], nullptr, 16));
	luid.HighPart = static_cast<LONG>(strtol(argv[6], nullptr, 16));
	const char *cso_path = argv[7];

	page = static_cast<shared_page *>(MapViewOfFile(h_page, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(shared_page)));
	if (!page)
		return 3;

	ComPtr<IDXGIFactory4> factory;
	CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
	ComPtr<IDXGIAdapter1> adapter;
	if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
		fail("no adapter with the host's LUID");
	ComPtr<ID3D12Device5> dev;
	if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))))
		fail("D3D12CreateDevice (Device5) failed");
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
	dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
	page->dxr_tier = o5.RaytracingTier;
	if (o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
		fail("no DXR tier 1.1 in the helper");

	ComPtr<ID3D12Fence> req, done;
	ComPtr<ID3D12Resource> tex;
	if (FAILED(dev->OpenSharedHandle(h_req, IID_PPV_ARGS(&req))) || FAILED(dev->OpenSharedHandle(h_done, IID_PPV_ARGS(&done))))
		fail("OpenSharedHandle on the fences failed");
	if (FAILED(dev->OpenSharedHandle(h_tex, IID_PPV_ARGS(&tex))))
		fail("OpenSharedHandle on the texture failed");

	// shader
	FILE *f = nullptr;
	fopen_s(&f, cso_path, "rb");
	if (!f)
		fail("rt.cso not found");
	std::vector<char> cso;
	fseek(f, 0, SEEK_END);
	cso.resize(static_cast<size_t>(ftell(f)));
	fseek(f, 0, SEEK_SET);
	fread(cso.data(), 1, cso.size(), f);
	fclose(f);
	ComPtr<ID3D12RootSignature> rs;
	CHECK(dev->CreateRootSignature(0, cso.data(), cso.size(), IID_PPV_ARGS(&rs)));
	D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
	pd.pRootSignature = rs.Get();
	pd.CS = {cso.data(), cso.size()};
	ComPtr<ID3D12PipelineState> pso;
	CHECK(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));

	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	ComPtr<ID3D12CommandQueue> queue;
	CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
	ComPtr<ID3D12CommandAllocator> alloc;
	CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
	ComPtr<ID3D12GraphicsCommandList4> list;
	CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)));
	ComPtr<ID3D12Fence> own;
	CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&own)));
	HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	UINT64 own_v = 0;

	// a zone-sized scene: 320 x 320 quads of rolling ground = 204,800 triangles
	constexpr UINT N = 320;
	std::vector<float> verts;
	std::vector<UINT> idx;
	for (UINT z = 0; z <= N; ++z)
		for (UINT x = 0; x <= N; ++x)
		{
			float fx = -100.0f + 200.0f * x / N, fz = -100.0f + 200.0f * z / N;
			verts.push_back(fx);
			verts.push_back(3.0f * sinf(fx * 0.15f) * cosf(fz * 0.11f) + 1.5f * sinf(fx * 0.7f + fz * 0.5f));
			verts.push_back(fz);
		}
	for (UINT z = 0; z < N; ++z)
		for (UINT x = 0; x < N; ++x)
		{
			UINT a = z * (N + 1) + x, b = a + 1, c = a + N + 1, d = c + 1;
			UINT q[6] = {a, c, b, b, c, d};
			idx.insert(idx.end(), q, q + 6);
		}
	auto vb = buffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, verts.size() * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
	auto ib = buffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, idx.size() * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
	void *p;
	CHECK(vb->Map(0, nullptr, &p)); memcpy(p, verts.data(), verts.size() * 4); vb->Unmap(0, nullptr);
	CHECK(ib->Map(0, nullptr, &p)); memcpy(p, idx.data(), idx.size() * 4); ib->Unmap(0, nullptr);

	D3D12_RAYTRACING_GEOMETRY_DESC geo = {};
	geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
	geo.Triangles.VertexBuffer = {vb->GetGPUVirtualAddress(), 12};
	geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
	geo.Triangles.VertexCount = static_cast<UINT>(verts.size() / 3);
	geo.Triangles.IndexBuffer = ib->GetGPUVirtualAddress();
	geo.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
	geo.Triangles.IndexCount = static_cast<UINT>(idx.size());
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bin = {};
	bin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	bin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	bin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	bin.NumDescs = 1;
	bin.pGeometryDescs = &geo;
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bpre = {};
	dev->GetRaytracingAccelerationStructurePrebuildInfo(&bin, &bpre);

	D3D12_RAYTRACING_INSTANCE_DESC inst = {};
	inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1;
	inst.InstanceMask = 0xff;
	auto blas = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, bpre.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
	inst.AccelerationStructure = blas->GetGPUVirtualAddress();
	auto instb = buffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof inst, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
	CHECK(instb->Map(0, nullptr, &p)); memcpy(p, &inst, sizeof inst); instb->Unmap(0, nullptr);
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tin = {};
	tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	tin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	tin.NumDescs = 1;
	tin.InstanceDescs = instb->GetGPUVirtualAddress();
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tpre = {};
	dev->GetRaytracingAccelerationStructurePrebuildInfo(&tin, &tpre);
	auto tlas = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, tpre.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
	auto scratch = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, std::max(bpre.ScratchDataSizeInBytes, tpre.ScratchDataSizeInBytes), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
	bd.Inputs = bin;
	bd.DestAccelerationStructureData = blas->GetGPUVirtualAddress();
	bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
	list->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
	D3D12_RESOURCE_BARRIER ub = {};
	ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	list->ResourceBarrier(1, &ub);
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td = {};
	td.Inputs = tin;
	td.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
	td.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
	list->BuildRaytracingAccelerationStructure(&td, 0, nullptr);
	CHECK(list->Close());
	ID3D12CommandList *lists[] = {list.Get()};
	queue->ExecuteCommandLists(1, lists);
	CHECK(queue->Signal(own.Get(), ++own_v));
	wait_fence(own.Get(), own_v, ev);

	// UAV on the shared texture
	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = 1;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ComPtr<ID3D12DescriptorHeap> heap;
	CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
	uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	dev->CreateUnorderedAccessView(tex.Get(), nullptr, &uav, heap->GetCPUDescriptorHandleForHeapStart());

	D3D12_QUERY_HEAP_DESC qhd = {};
	qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	qhd.Count = 2;
	ComPtr<ID3D12QueryHeap> qh;
	CHECK(dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)));
	auto qrb = buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
	UINT64 freq = 0;
	CHECK(queue->GetTimestampFrequency(&freq));

	page->helper_ok = 1;
	printf("helper: up (64-bit, DXR tier %d, %u triangles, BLAS %llu KB)\n", o5.RaytracingTier,
		static_cast<unsigned>(idx.size() / 3), static_cast<unsigned long long>(bpre.ResultDataMaxSizeInBytes / 1024));

	for (UINT64 n = 1;; ++n)
	{
		const bool ahead = page->gpu_wait != 0;
		if (!ahead)
		{
			wait_fence(req.Get(), n, ev);
			page->helper_wake_qpc[n % kSlots] = now();
		}
		if (page->quit)
			break;
		if (ahead)
			CHECK(queue->Wait(req.Get(), n)); // queued before the host asks: the GPU starts the moment req reaches n
		UINT consts[4] = {page->frame, page->spp, kW, kH};
		CHECK(alloc->Reset());
		CHECK(list->Reset(alloc.Get(), pso.Get()));
		list->SetComputeRootSignature(rs.Get());
		ID3D12DescriptorHeap *heaps[] = {heap.Get()};
		list->SetDescriptorHeaps(1, heaps);
		list->SetComputeRoot32BitConstants(0, 4, consts, 0);
		list->SetComputeRootShaderResourceView(1, tlas->GetGPUVirtualAddress());
		list->SetComputeRootDescriptorTable(2, heap->GetGPUDescriptorHandleForHeapStart());
		list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
		list->Dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
		list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
		list->ResolveQueryData(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qrb.Get(), 0);
		CHECK(list->Close());
		queue->ExecuteCommandLists(1, lists);
		CHECK(queue->Signal(done.Get(), n));
		CHECK(queue->Signal(own.Get(), ++own_v));
		wait_fence(own.Get(), own_v, ev);
		UINT64 *ts;
		D3D12_RANGE rr = {0, 16};
		CHECK(qrb->Map(0, &rr, reinterpret_cast<void **>(&ts)));
		page->helper_gpu_ms[n % kSlots] = double(ts[1] - ts[0]) * 1000.0 / double(freq);
		qrb->Unmap(0, nullptr);
	}
	printf("helper: quit\n");
	return 0;
}

#else
// ---------------------------------------------------------------- host (32-bit, FFXI's side) ----------------------
int main()
{
	printf("host: %d-bit process\n", int(sizeof(void *) * 8));
	ComPtr<ID3D12Device> dev;
	CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)));
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
	dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
	printf("host: DXR tier in this process: %d\n", o5.RaytracingTier);
	LUID luid = dev->GetAdapterLuid();

	SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};

	// the shared picture: what a GI probe atlas / RT result would be
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = kW;
	td.Height = kH;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
	ComPtr<ID3D12Resource> tex;
	CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex)));
	ComPtr<ID3D12Fence> req, done, own;
	CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&req)));
	CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&done)));
	CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&own)));
	HANDLE h_req, h_done, h_tex;
	CHECK(dev->CreateSharedHandle(req.Get(), &sa, GENERIC_ALL, nullptr, &h_req));
	CHECK(dev->CreateSharedHandle(done.Get(), &sa, GENERIC_ALL, nullptr, &h_done));
	CHECK(dev->CreateSharedHandle(tex.Get(), &sa, GENERIC_ALL, nullptr, &h_tex));
	HANDLE h_page = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(shared_page), nullptr);
	auto *page = static_cast<shared_page *>(MapViewOfFile(h_page, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(shared_page)));
	if (!page)
		return 1;

	char dir[MAX_PATH];
	GetModuleFileNameA(nullptr, dir, MAX_PATH);
	*strrchr(dir, '\\') = 0;
	char cmd[1024];
	sprintf_s(cmd, "\"%s\\helper64.exe\" %p %p %p %p %lx %lx \"%s\\rt.cso\"", dir, h_req, h_done, h_tex, h_page,
		luid.LowPart, static_cast<unsigned long>(luid.HighPart), dir);
	STARTUPINFOA si = {sizeof si};
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
	{
		printf("host: CreateProcess failed %lu\n", GetLastError());
		return 1;
	}
	LONGLONG t_start = now();
	while (page->helper_ok == 0)
	{
		if (WaitForSingleObject(pi.hProcess, 1) == WAIT_OBJECT_0)
		{
			printf("host: the helper exited: %s\n", page->why);
			return 1;
		}
	}
	if (page->helper_ok < 0)
	{
		printf("host: helper failed: %s\n", page->why);
		return 1;
	}
	printf("host: helper up after %.0f ms\n", qpc_ms(t_start, now()));

	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	ComPtr<ID3D12CommandQueue> queue;
	CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
	ComPtr<ID3D12CommandAllocator> alloc;
	CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
	ComPtr<ID3D12GraphicsCommandList> list;
	CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)));
	CHECK(list->Close());
	ID3D12CommandList *lists[] = {list.Get()};
	HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	UINT64 own_v = 0, n = 0;
	auto finish = [&] {
		CHECK(queue->Signal(own.Get(), ++own_v));
		wait_fence(own.Get(), own_v, ev);
	};

	// ---- phase 1: the helper's picture, read here
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
	UINT64 rb_size = 0;
	dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &rb_size);
	auto rb = buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, rb_size, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
	page->spp = 1;
	page->frame = 0x5A;
	++n;
	CHECK(queue->Signal(req.Get(), n));
	CHECK(queue->Wait(done.Get(), n)); // the GPU waits for the helper's GPU, no CPU in between
	CHECK(alloc->Reset());
	CHECK(list->Reset(alloc.Get(), nullptr));
	D3D12_TEXTURE_COPY_LOCATION src = {tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
	D3D12_TEXTURE_COPY_LOCATION dst = {rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
	dst.PlacedFootprint = fp;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	CHECK(list->Close());
	queue->ExecuteCommandLists(1, lists);
	finish();
	unsigned char *px;
	CHECK(rb->Map(0, nullptr, reinterpret_cast<void **>(&px)));
	UINT hit = 0, miss_ok = 0, bad = 0;
	for (UINT y = 0; y < kH; ++y)
		for (UINT x = 0; x < kW; ++x)
		{
			const unsigned char *c = px + y * fp.Footprint.RowPitch + x * 4;
			if (c[3] != 255)
				++bad;
			else if (c[1] > 0)
				++hit;
			else if (c[0] == 0 && c[2] == 0x5A)
				++miss_ok;
			else
				++bad;
		}
	rb->Unmap(0, nullptr);
	bool ok = bad == 0 && hit > kW * kH / 10 && miss_ok > kW * kH / 10;
	printf("phase 1 (picture made by the 64-bit helper, read by the 32-bit host): %s  hit %u, sky with this frame's tag %u, wrong %u\n",
		ok ? "PASS" : "FAIL", hit, miss_ok, bad);
	if (!ok)
		return 1;

	// ---- phase 2: round trip, host CPU signals -> helper CPU wakes, records, submits -> its GPU done -> host sees it
	std::vector<double> rt, wake, hgpu;
	for (int i = 0; i < 500; ++i)
	{
		page->frame = UINT(n + 1);
		LONGLONG t0 = now();
		++n;
		CHECK(queue->Signal(req.Get(), n));
		wait_fence(done.Get(), n, ev);
		LONGLONG t1 = now();
		rt.push_back(qpc_ms(t0, t1));
		wake.push_back(qpc_ms(t0, page->helper_wake_qpc[n % kSlots]));
	}
	Sleep(20);
	for (UINT64 k = n - 499; k <= n; ++k)
		hgpu.push_back(page->helper_gpu_ms[k % kSlots]);
	double a50, a99, w50, w99, g50, g99;
	double am = stats(rt, &a50, &a99), wm = stats(wake, &w50, &w99), gm = stats(hgpu, &g50, &g99);
	printf("phase 2 (500 round trips, 1 ray/px at %ux%u):\n", kW, kH);
	printf("  host signal -> host sees helper done: mean %.3f ms, p50 %.3f, p99 %.3f\n", am, a50, a99);
	printf("  host signal -> helper CPU awake:       mean %.3f ms, p50 %.3f, p99 %.3f\n", wm, w50, w99);
	printf("  helper's dispatch on the GPU:          mean %.3f ms, p50 %.3f, p99 %.3f\n", gm, g50, g99);

	// ---- phase 3: GPU sharing. Host "frame" = copies of a 4096^2 texture (stand-in for the game's own GPU work).
	td.Width = td.Height = 4096;
	td.Flags = D3D12_RESOURCE_FLAG_NONE;
	ComPtr<ID3D12Resource> big_a, big_b;
	CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&big_a)));
	CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&big_b)));
	D3D12_QUERY_HEAP_DESC qhd = {};
	qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	qhd.Count = 2;
	ComPtr<ID3D12QueryHeap> qh;
	CHECK(dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)));
	auto qrb = buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
	UINT64 freq = 0;
	CHECK(queue->GetTimestampFrequency(&freq));
	const int copies = atoi(getenv("RTP_COPIES") ? getenv("RTP_COPIES") : "12");
	const UINT heavy = UINT(atoi(getenv("RTP_SPP") ? getenv("RTP_SPP") : "16"));

	struct result { double frame, host_gpu, helper_gpu; };
	auto run = [&](int mode) -> result // 0 alone, 1 helper consumed a frame late, 2 helper waited for in the frame
	{
		std::vector<double> fr, hg, hh;
		UINT64 first = n + 1;
		for (int i = 0; i < 300; ++i)
		{
			LONGLONG t0 = now();
			CHECK(alloc->Reset());
			CHECK(list->Reset(alloc.Get(), nullptr));
			list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
			for (int c = 0; c < copies; ++c)
				list->CopyResource(big_b.Get(), big_a.Get());
			list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
			list->ResolveQueryData(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qrb.Get(), 0);
			CHECK(list->Close());
			if (mode == 1 && n >= first)
				CHECK(queue->Wait(done.Get(), n)); // last frame's answer before this frame's use of it
			queue->ExecuteCommandLists(1, lists);
			if (mode != 0)
			{
				page->frame = UINT(n + 1);
				++n;
				CHECK(queue->Signal(req.Get(), n));
				if (mode == 2)
					CHECK(queue->Wait(done.Get(), n));
			}
			finish();
			fr.push_back(qpc_ms(t0, now()));
			UINT64 *ts;
			CHECK(qrb->Map(0, nullptr, reinterpret_cast<void **>(&ts)));
			hg.push_back(double(ts[1] - ts[0]) * 1000.0 / double(freq));
			qrb->Unmap(0, nullptr);
		}
		if (mode != 0)
		{
			wait_fence(done.Get(), n, ev);
			Sleep(20);
			for (UINT64 k = first; k <= n; ++k)
				hh.push_back(page->helper_gpu_ms[k % kSlots]);
		}
		double p50, p99;
		result r = {};
		r.frame = stats(fr, &p50, &p99);
		r.host_gpu = stats(hg, &p50, &p99);
		r.helper_gpu = hh.empty() ? 0 : stats(hh, &p50, &p99);
		return r;
	};
	page->spp = heavy;
	run(0); // warm up
	result a = run(0);
	printf("phase 3 (host frame = %d copies of 4096^2; helper %u rays/px at %ux%u = %.1f M rays):\n", copies, heavy, kW, kH, kW * kH * heavy / 1e6);
	printf("  A host alone:                   frame %.3f ms (host GPU work %.3f)\n", a.frame, a.host_gpu);
	for (int ahead = 0; ahead < 2; ++ahead)
	{
		page->gpu_wait = ahead;
		run(1); // settle into the mode
		result b = run(1), c = run(2);
		printf(" helper %s:\n", ahead ? "queued ahead, its GPU waits (the real design)" : "woken on the CPU each frame");
		printf("  B + helper, used a frame late:  frame %.3f ms (host GPU work %.3f, helper GPU %.3f)  overhead vs A + helper: %+.3f ms\n",
			b.frame, b.host_gpu, b.helper_gpu, b.frame - (a.frame + b.helper_gpu));
		printf("  C + helper, waited in-frame:    frame %.3f ms (host GPU work %.3f, helper GPU %.3f)  overhead vs A + helper: %+.3f ms\n",
			c.frame, c.host_gpu, c.helper_gpu, c.frame - (a.frame + c.helper_gpu));
	}

	page->quit = 1;
	CHECK(req->Signal(n + 1));
	WaitForSingleObject(pi.hProcess, 5000);
	return 0;
}
#endif
