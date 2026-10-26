// The vtable slot of each hooked method, from the SDK's C interface (its vtable structs), not counted by hand. Its own
// translation unit: CINTERFACE turns d3d12.h's interfaces into C structs here only.
#define CINTERFACE
#define COBJMACROS
#include <d3d12.h>
#include <cstddef>
#include "slots.hpp"

namespace hifi::host
{
#define HIFI_SLOT(vtbl, method) uint32_t(offsetof(vtbl, method) / sizeof(void *))

	void slot_indices(uint32_t out[slot_count])
	{
		out[list_reset] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, Reset);
		out[list_close] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, Close);
		out[list_barrier] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, ResourceBarrier);
		out[list_om_targets] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, OMSetRenderTargets);
		out[list_pipeline] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, SetPipelineState);
		out[list_topology] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, IASetPrimitiveTopology);
		out[list_viewports] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, RSSetViewports);
		out[list_scissors] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, RSSetScissorRects);
		out[list_root_signature] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, SetGraphicsRootSignature);
		out[list_root_table] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, SetGraphicsRootDescriptorTable);
		out[list_root_constant] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, SetGraphicsRoot32BitConstant);
		out[list_root_constants] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, SetGraphicsRoot32BitConstants);
		out[list_root_cbv] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, SetGraphicsRootConstantBufferView);
		out[list_root_srv] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, SetGraphicsRootShaderResourceView);
		out[list_root_uav] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, SetGraphicsRootUnorderedAccessView);
		out[list_clear_depth] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, ClearDepthStencilView);
		out[list_clear_target] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, ClearRenderTargetView);
		out[list_draw] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, DrawInstanced);
		out[list_draw_indexed] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, DrawIndexedInstanced);
		out[list_execute_indirect] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, ExecuteIndirect);
		out[list_execute_bundle] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, ExecuteBundle);
		out[list_clear_state] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, ClearState);
		out[list_barrier7] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, Barrier);
		out[list_heaps] = HIFI_SLOT(ID3D12GraphicsCommandList7Vtbl, SetDescriptorHeaps);
		out[queue_execute] = HIFI_SLOT(ID3D12CommandQueueVtbl, ExecuteCommandLists);
		out[device_create_list] = HIFI_SLOT(ID3D12DeviceVtbl, CreateCommandList);
		out[device_create_rtv] = HIFI_SLOT(ID3D12DeviceVtbl, CreateRenderTargetView);
		out[device_create_dsv] = HIFI_SLOT(ID3D12DeviceVtbl, CreateDepthStencilView);
		out[device_copy] = HIFI_SLOT(ID3D12DeviceVtbl, CopyDescriptors);
		out[device_copy_simple] = HIFI_SLOT(ID3D12DeviceVtbl, CopyDescriptorsSimple);
	}

	uint32_t list_vtable_slots()
	{
		return uint32_t(sizeof(ID3D12GraphicsCommandList7Vtbl) / sizeof(void *));
	}
}
