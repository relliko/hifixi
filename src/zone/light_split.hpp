#pragma once
// The light split: what HiFi XI's native zone and characters know of each pixel's light, for World FX's shading pass
// (grass.hlsl light_at), so it needn't guess it from the depth. A second target beside the world's, the world's size,
// one 32-bit word a pixel, written by the opaque and alpha-tested pieces (blended ones leave it):
//
//   bits  0-10  the view depth: round((log2(depth) + 3) x 128), 1..2047 (0: nothing drawn here this frame); the
//               shading pass takes the word only where the world's own depth comes to the same code (+-2.5), so what
//               the game drew over it afterwards (an effect's mesh, a model left to the game) is left to the old way
//   bits 11-24  the surface's facing (world), octahedral, 7 bits a side; the mesh's own (its normal map left out),
//               turned toward the camera
//   bits 25-31  the sun's share of the light drawn there, 0..127: how much of it a full shadow takes away (the sun's
//               term as the game lights it, against everything else: the ambient, the lamps; after the 0..1 clamp)
//
// Cleared to 0 by the first native pass after World FX last took it (take): one frame's at a time, as dgVoodoo
// records one frame after another on its one thread. Written only while World FX asks for it (take in the last second)
// and the world has one sample. Header only: the zone's and the characters' injections and main.cpp share it.
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace hifi::light
{
	constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R32_UINT;
	// (between passes: what World FX's copy reads it from)
	constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	constexpr ULONGLONG kWantedMs = 1000;

	// The word in HLSL, put before the native passes' shaders (grass.hlsl light_at reads it)
	inline const char *kHlsl = R"(
// (light_split.hpp) The light split's word: the view depth, the facing (world) and the sun's share of the light drawn
float2 light_oct(float3 n)
{
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	return n.z >= 0 ? n.xy : (1 - abs(n.yx)) * float2(n.x >= 0 ? 1 : -1, n.y >= 0 ? 1 : -1);
}
uint light_word(float depth, float3 n, float share)
{
	const uint code = (uint)clamp(round((log2(max(depth, 1e-3)) + 3.0) * 128.0), 1.0, 2047.0);
	const float l = length(n);
	const uint2 q = (uint2)round(saturate((l > 1e-6 ? light_oct(n / l) : float2(0, 0)) * 0.5 + 0.5) * 127.0);
	return code | q.x << 11 | q.y << 18 | (uint)round(saturate(share) * 127.0) << 25;
}
// The sun's share of a lit colour (after its 0..1 clamp): how much of it goes without the sun's term
float light_share(float3 lit, float3 without_sun)
{
	const float3 k = float3(0.2126, 0.7152, 0.0722);
	const float all = dot(lit, k);
	return all > 1e-4 ? saturate(dot(lit - without_sun, k) / all) : 0;
}
)";

	namespace detail
	{
		struct state
		{
			std::mutex mutex;
			ID3D12Device *device = nullptr;
			ID3D12Resource *texture = nullptr;
			ID3D12DescriptorHeap *heap = nullptr;
			D3D12_RESOURCE_STATES now = D3D12_RESOURCE_STATE_COMMON; // (its state)
			bool written = false;   // a pass drew into it since World FX last took it
			ULONGLONG asked_at = 0; // when World FX last asked (take)
			uint64_t frames = 0;    // takes so far (old textures are let go 16 on: past the GPU)
			std::vector<std::pair<ID3D12Resource *, uint64_t>> old;
			uint64_t taken = 0, cleared = 0; // (status)
		};
		inline state g;

		inline void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
		{
			if (from == to)
				return;
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = r;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = from;
			b.Transition.StateAfter = to;
			list->ResourceBarrier(1, &b);
		}

		// (under the lock) The texture the world's size on this device, made (again) when it isn't
		inline bool texture_for(ID3D12Device *device, UINT64 width, UINT height)
		{
			if (g.device != device)
			{
				for (auto &o : g.old)
					o.first->Release();
				g.old.clear();
				if (g.texture != nullptr)
					g.texture->Release();
				if (g.heap != nullptr)
					g.heap->Release();
				g.texture = nullptr;
				g.heap = nullptr;
				g.device = device;
			}
			if (g.texture != nullptr)
			{
				const D3D12_RESOURCE_DESC d = g.texture->GetDesc();
				if (d.Width == width && d.Height == height)
					return true;
				g.old.push_back({ g.texture, g.frames });
				g.texture = nullptr;
			}
			if (g.heap == nullptr)
			{
				const D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
				if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap))))
				{
					g.heap = nullptr;
					return false;
				}
			}
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			d.Width = width;
			d.Height = height;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = kFormat;
			d.SampleDesc.Count = 1;
			d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			const D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
			D3D12_CLEAR_VALUE cv = {};
			cv.Format = kFormat;
			if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, &cv, IID_PPV_ARGS(&g.texture))))
			{
				g.texture = nullptr;
				return false;
			}
			g.texture->SetName(L"hifixi: the light split");
			g.now = D3D12_RESOURCE_STATE_COMMON;
			g.written = false;
			device->CreateRenderTargetView(g.texture, nullptr, g.heap->GetCPUDescriptorHandleForHeapStart());
			return true;
		}
	}

	// A native pass about to draw into the world (world_rt: its target, one sample): the light split's view, in
	// RENDER_TARGET, cleared when it's the first pass since World FX took it. False: not wanted, or not to be had.
	inline bool begin(ID3D12GraphicsCommandList *list, ID3D12Device *device, ID3D12Resource *world_rt, D3D12_CPU_DESCRIPTOR_HANDLE &rtv)
	{
		using namespace detail;
		if (list == nullptr || device == nullptr || world_rt == nullptr)
			return false;
		const D3D12_RESOURCE_DESC rd = world_rt->GetDesc();
		std::lock_guard<std::mutex> lock(g.mutex);
		if (g.asked_at == 0 || GetTickCount64() - g.asked_at > kWantedMs || rd.SampleDesc.Count != 1 || !texture_for(device, rd.Width, rd.Height))
			return false;
		transition(list, g.texture, g.now, D3D12_RESOURCE_STATE_RENDER_TARGET);
		g.now = D3D12_RESOURCE_STATE_RENDER_TARGET;
		rtv = g.heap->GetCPUDescriptorHandleForHeapStart();
		if (!g.written)
		{
			const float none[4] = {};
			list->ClearRenderTargetView(rtv, none, 0, nullptr);
			g.written = true;
			g.cleared++;
		}
		return true;
	}

	// ... and drawn: back to where World FX reads it
	inline void end(ID3D12GraphicsCommandList *list)
	{
		using namespace detail;
		std::lock_guard<std::mutex> lock(g.mutex);
		if (g.texture == nullptr)
			return;
		transition(list, g.texture, g.now, kRead);
		g.now = kRead;
	}

	// World FX's shading pass, once a frame: the light split drawn since it last asked (in kRead; nullptr: none). Asking
	// is what keeps the native passes writing it.
	inline ID3D12Resource *take()
	{
		using namespace detail;
		std::lock_guard<std::mutex> lock(g.mutex);
		g.asked_at = GetTickCount64();
		g.frames++;
		for (auto it = g.old.begin(); it != g.old.end();)
			if (g.frames - it->second > 16)
			{
				it->first->Release();
				it = g.old.erase(it);
			}
			else
				++it;
		const bool fresh = g.written && g.texture != nullptr && g.now == kRead;
		g.written = false;
		if (!fresh)
			return nullptr;
		g.taken++;
		return g.texture;
	}

	// (status: how many frames World FX took it, how many it was cleared)
	inline void counts(uint64_t &taken, uint64_t &cleared)
	{
		using namespace detail;
		std::lock_guard<std::mutex> lock(g.mutex);
		taken = g.taken;
		cleared = g.cleared;
	}

	inline void release()
	{
		using namespace detail;
		std::lock_guard<std::mutex> lock(g.mutex);
		for (auto &o : g.old)
			o.first->Release();
		g.old.clear();
		if (g.texture != nullptr)
			g.texture->Release();
		if (g.heap != nullptr)
			g.heap->Release();
		g.texture = nullptr;
		g.heap = nullptr;
		g.device = nullptr;
		g.now = D3D12_RESOURCE_STATE_COMMON;
		g.written = false;
	}
}
