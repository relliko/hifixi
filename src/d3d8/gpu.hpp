#pragma once

// The Direct3D 12 side of the translator: the device, its one direct queue, the command list everything is recorded on
// (on the game's thread, as each Direct3D 8 call comes), the memory the frame's data is uploaded through, descriptor
// heaps, resource states and the root signature every draw uses.
//
// The list: one at a time. It's closed and submitted at Present, at a readback (the game waiting on the GPU: Lock of a
// surface a copy from a target went into), and every kSplitDraws draws (the GPU starts on the frame early, as
// dgVoodoo's lists do every ~256 draws in the game). After each submit the list is reset at once with the frame's
// allocator: nothing is bound on it then, and the device binds again what its next draw needs (`epoch` tells it).
//
// Frames in flight: kFrames sets of {allocator, upload pages, shader-visible descriptors}, each reused once the GPU has
// passed its fence.
//
// The root signature (every draw's):
//   0: root CBV b0, vertex shader constants        2: table t0-t7, the stages' textures (pixel shader)
//   1: root CBV b1, pixel shader constants         3: table s0-s7, their samplers (pixel shader)
// HiFi XI's hooks see these as they're set, and put them back after drawing into the frame: so the device keeps track of
// what it bound itself and only binds again what changed (no cache of the GPU's state outside the list: epoch).

#include "common.hpp"
#include <functional>
#include <unordered_map>

namespace hifi::d3d8
{
	constexpr uint32_t kFrames = 3;
	constexpr uint32_t kSplitDraws = 1024;
	constexpr uint32_t kMaxStages = 8;

	struct upload_alloc
	{
		uint8_t *cpu = nullptr;
		D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
		ID3D12Resource *resource = nullptr;
		UINT64 offset = 0;
	};

	// A GPU resource with the state the list leaves it in (in list order: one queue, recorded in order). Depth buffers
	// per plane (0 depth, 1 stencil), as dgVoodoo moves them and HiFi XI follows them.
	struct gpu_resource
	{
		ID3D12Resource *res = nullptr;
		D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES plane[2] = { D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_READ };
		bool depth = false;
		uint8_t planes = 1; // (depth: 2 with a stencil plane)
	};

	struct readback
	{
		ID3D12Resource *buffer = nullptr;
		UINT64 size = 0;
		UINT64 fence = 0; // done when the queue's fence has passed it
	};

	class gpu
	{
	public:
		bool init(IDXGIFactory2 *factory, IDXGIAdapter1 *adapter, bool debug, std::string &why);
		void shutdown();

		ID3D12Device *device = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		IDXGIFactory2 *factory = nullptr;
		ID3D12RootSignature *root = nullptr;
		ID3D12DescriptorHeap *srv_heap = nullptr, *sampler_heap = nullptr; // shader visible
		uint64_t epoch = 1;      // bumped at each Reset of the list: everything bound is gone
		uint32_t draws_in_list = 0;
		uint64_t frame_number = 0;
		uint64_t debug_messages = 0; // the debug layer's errors and warnings (D3D8_DEBUG=1), each logged once
		void drain_messages();

		// The list being recorded (open)
		ID3D12GraphicsCommandList *list() { return cmd; }
		// Closed, submitted and reset (wait: until the GPU has run it)
		void submit(bool wait);
		// The frame's end (after its last submit): its fence, and the next frame's set made free
		void end_frame();
		void wait_idle();
		void wait_for(UINT64 value);
		UINT64 last_submitted() const { return fence_value; }
		bool done(UINT64 value) const;

		// Frame memory (CPU-written, read by the GPU while this frame's set is in flight)
		upload_alloc upload(UINT64 size, UINT64 align = 256);

		// CPU-only descriptors for views (freed when the view's resource goes)
		D3D12_CPU_DESCRIPTOR_HANDLE new_rtv();
		D3D12_CPU_DESCRIPTOR_HANDLE new_dsv();
		D3D12_CPU_DESCRIPTOR_HANDLE new_srv();
		void free_rtv(D3D12_CPU_DESCRIPTOR_HANDLE h);
		void free_dsv(D3D12_CPU_DESCRIPTOR_HANDLE h);
		void free_srv(D3D12_CPU_DESCRIPTOR_HANDLE h);
		D3D12_CPU_DESCRIPTOR_HANDLE null_srv(D3D12_SRV_DIMENSION dim) const;

		// A table of n shader views for this draw, copied from CPU descriptors (null: the null view of dims[i]); the
		// last one made again when the views are the same and its descriptors still this frame's
		D3D12_GPU_DESCRIPTOR_HANDLE srv_table(const D3D12_CPU_DESCRIPTOR_HANDLE *views, uint32_t n);
		// kMaxStages samplers' table (the same descriptions give the same table)
		D3D12_GPU_DESCRIPTOR_HANDLE sampler_table(const D3D12_SAMPLER_DESC *descs);

		// Resource states: barriers queued, recorded together before the next draw, clear or copy
		void transition(gpu_resource &r, D3D12_RESOURCE_STATES to);
		void transition_plane(gpu_resource &r, uint32_t plane, D3D12_RESOURCE_STATES to);
		void flush_barriers();

		// A COM object let go once the GPU is past everything submitted so far
		void defer_release(IUnknown *object);
		// A function run then (descriptors freed with their resource)
		void defer(std::function<void()> fn);

		// Readbacks: a buffer the list copies into; read after wait_for(r.fence)
		readback new_readback(UINT64 size);
		void free_readback(readback &r);

		// Called before each submit (the device: nothing to do yet) and after each reset (the device marks its bindings gone)
		std::function<void()> on_reset;

		// (the long-frame log's: this frame's time waiting for the GPU on the calling thread, and how many waits)
		double wait_ms = 0;
		uint32_t waits = 0;

	private:
		struct page
		{
			ID3D12Resource *buffer = nullptr;
			uint8_t *cpu = nullptr;
			UINT64 size = 0, used = 0;
		};
		struct frame_set
		{
			ID3D12CommandAllocator *allocator = nullptr;
			std::vector<page> pages;
			UINT64 fence = 0;
			uint32_t srv_begin = 0, srv_end = 0; // its share of the shader-visible heap
		};
		struct pool
		{
			ID3D12DescriptorHeap *heap = nullptr;
			UINT step = 0;
			uint32_t next = 0, size = 0;
			std::vector<uint32_t> free_list;
			D3D12_CPU_DESCRIPTOR_HANDLE take();
			void give(D3D12_CPU_DESCRIPTOR_HANDLE h);
		};
		struct deferred
		{
			UINT64 fence;
			IUnknown *object;
			std::function<void()> fn;
		};

		void open_list();
		void run_deferred();
		page new_page(UINT64 size);

		ID3D12GraphicsCommandList *cmd = nullptr;
		ID3D12Fence *fence = nullptr;
		ID3D12InfoQueue *info = nullptr;
		HANDLE fence_event = nullptr;
		UINT64 fence_value = 0;
		frame_set frames[kFrames];
		uint32_t current = 0;
		std::vector<page> spare_pages;
		pool rtv_pool, dsv_pool, srv_pool;
		D3D12_CPU_DESCRIPTOR_HANDLE null_views[4] = {}; // 2D, cube, 3D, (spare)
		ID3D12Resource *white[3] = {};
		UINT srv_step = 0, sampler_step = 0;
		uint32_t srv_next = 0;
		D3D12_CPU_DESCRIPTOR_HANDLE last_views[kMaxStages] = {};
		uint32_t last_count = 0;
		D3D12_GPU_DESCRIPTOR_HANDLE last_table = {};
		std::unordered_map<uint64_t, D3D12_GPU_DESCRIPTOR_HANDLE> sampler_tables;
		std::vector<std::vector<uint8_t>> sampler_keys;
		uint32_t sampler_next = 0;
		std::vector<D3D12_RESOURCE_BARRIER> pending;
		std::vector<deferred> deferred_list;
		std::vector<readback> spare_readbacks;
	};
}
