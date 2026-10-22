#pragma once

// What the hooks hand the backend (set_events): each call dgVoodoo makes on the hooked slots, in the place ReShade
// raises its own event (before Close, clears and draws; after the rest), on the thread that made it. Any may be null.
#include <d3d12.h>
#include "hooks.hpp"

namespace hifi::host
{
	struct events
	{
		// Lists
		void (*created)(ID3D12GraphicsCommandList *list) = nullptr; // a direct list made (open, nothing bound)
		void (*reset)(ID3D12GraphicsCommandList *list) = nullptr;
		void (*close)(ID3D12GraphicsCommandList *list) = nullptr;
		void (*targets)(ID3D12GraphicsCommandList *list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs, BOOL single_range, const D3D12_CPU_DESCRIPTOR_HANDLE *dsv) = nullptr;
		void (*pipeline)(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso) = nullptr;
		void (*topology)(ID3D12GraphicsCommandList *list, D3D12_PRIMITIVE_TOPOLOGY topology) = nullptr;
		void (*viewports)(ID3D12GraphicsCommandList *list, UINT count, const D3D12_VIEWPORT *viewports) = nullptr;
		void (*scissors)(ID3D12GraphicsCommandList *list, UINT count, const D3D12_RECT *rects) = nullptr;
		void (*root_signature)(ID3D12GraphicsCommandList *list, ID3D12RootSignature *root) = nullptr;
		void (*root_table)(ID3D12GraphicsCommandList *list, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE table) = nullptr;
		void (*root_constants)(ID3D12GraphicsCommandList *list, UINT param, UINT count, const void *values, UINT first) = nullptr;
		void (*root_cbv)(ID3D12GraphicsCommandList *list, UINT param, D3D12_GPU_VIRTUAL_ADDRESS address) = nullptr;
		void (*root_srv)(ID3D12GraphicsCommandList *list, UINT param, D3D12_GPU_VIRTUAL_ADDRESS address) = nullptr;
		void (*root_uav)(ID3D12GraphicsCommandList *list, UINT param, D3D12_GPU_VIRTUAL_ADDRESS address) = nullptr;
		void (*barrier)(ID3D12GraphicsCommandList *list, UINT count, const D3D12_RESOURCE_BARRIER *barriers) = nullptr;
		void (*clear_depth)(ID3D12GraphicsCommandList *list, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, UINT count, const D3D12_RECT *rects) = nullptr;
		void (*clear_target)(ID3D12GraphicsCommandList *list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT count, const D3D12_RECT *rects) = nullptr;
		void (*draw)(ID3D12GraphicsCommandList *list, uint64_t vertices) = nullptr;
		// The queue: lists submitted (after)
		void (*execute)(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists) = nullptr;
		// The device: a render target or depth view made (resource null: a null view), and views copied
		void (*view)(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, D3D12_CPU_DESCRIPTOR_HANDLE at, ID3D12Resource *resource, DXGI_FORMAT format, uint32_t dsv_flags) = nullptr;
		void (*copy)(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT dst_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE *dst_starts, const UINT *dst_sizes,
			UINT src_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE *src_starts, const UINT *src_sizes) = nullptr;
	};
	// The table must outlive the hooks (a static one)
	void set_events(const events *e);
}
