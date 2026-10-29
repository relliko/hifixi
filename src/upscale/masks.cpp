/*
 * The world's copies for FSR's masks (masks.hpp). Textures of A's own (typeless) format, made again when A's size or
 * format changes; the ones they replace let go 16 takes on (past the GPU). Its own lock: World FX's renderer calls in
 * holding World FX's, temporal.cpp's pass holding the upscale's; this one is taken last by both.
 */

#include <windows.h>
#include "masks.hpp"
#include "upscale.hpp"
#include "../render/grass_render.hpp"
#include <atomic>
#include <mutex>
#include <vector>

namespace hifi::upscale::masks
{
	namespace
	{
		struct state
		{
			std::mutex mutex;
			std::atomic<bool> on{ true };
			ID3D12Resource *copy[kinds] = {};
			D3D12_RESOURCE_STATES copy_state[kinds] = {};
			bool fresh[kinds] = {};
			uint64_t width = 0;
			uint32_t height = 0;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			std::vector<std::pair<ID3D12Resource *, uint64_t>> old; // (with the take they were let go at)
			uint64_t takes = 0;
			std::atomic<uint64_t> frames[kinds] = {};
		};
		state g;

		void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
		{
			if (from == to)
				return;
			D3D12_RESOURCE_BARRIER b = {};
			b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
			list->ResourceBarrier(1, &b);
		}

		// (g.mutex held) The copies for A's size and format
		bool fit(ID3D12Resource *world)
		{
			const D3D12_RESOURCE_DESC wd = world->GetDesc();
			if (wd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || wd.SampleDesc.Count != 1)
				return false;
			if (g.copy[0] != nullptr && wd.Width == g.width && wd.Height == g.height && wd.Format == g.format)
				return true;
			for (ID3D12Resource *&c : g.copy)
				if (c != nullptr)
				{
					g.old.push_back({ c, g.takes });
					c = nullptr;
				}
			ID3D12Device *device = nullptr;
			if (FAILED(world->GetDevice(IID_PPV_ARGS(&device))))
				return false;
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			d.Width = wd.Width;
			d.Height = wd.Height;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = wd.Format;
			d.SampleDesc.Count = 1;
			const D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
			bool ok = true;
			for (int k = 0; k < kinds && ok; ++k)
			{
				ok = SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.copy[k])));
				g.copy_state[k] = D3D12_RESOURCE_STATE_COPY_DEST;
				g.fresh[k] = false;
			}
			device->Release();
			if (!ok)
			{
				for (ID3D12Resource *&c : g.copy)
					if (c != nullptr)
					{
						c->Release();
						c = nullptr;
					}
				return false;
			}
			g.width = wd.Width;
			g.height = wd.Height;
			g.format = wd.Format;
			return true;
		}
	}

	void set_on(bool on)
	{
		g.on = on;
		// (World FX's renderer's three points: its hook while on)
		grass::set_snapshot_hook(on ? &from_world_fx : nullptr);
	}

	bool on()
	{
		return g.on.load();
	}

	void snapshot(ID3D12GraphicsCommandList *list, ID3D12Resource *world, D3D12_RESOURCE_STATES state, when w)
	{
		if (!g.on.load(std::memory_order_relaxed) || list == nullptr || world == nullptr || w < 0 || w >= kinds || !temporal_running())
			return;
		std::lock_guard<std::mutex> lock(g.mutex);
		if (!fit(world))
			return;
		ID3D12Resource *const c = g.copy[w];
		barrier(list, world, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier(list, c, g.copy_state[w], D3D12_RESOURCE_STATE_COPY_DEST);
		list->CopyResource(c, world);
		barrier(list, c, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		barrier(list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
		g.copy_state[w] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		g.fresh[w] = true;
	}

	void from_world_fx(ID3D12GraphicsCommandList *list, ID3D12Resource *world, D3D12_RESOURCE_STATES state, int when)
	{
		if (when >= 0 && when <= 2)
			snapshot(list, world, state, masks::when(when + 1));
	}

	taken take()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		taken t;
		t.format = g.format;
		for (int k = 0; k < kinds; ++k)
		{
			t.copy[k] = g.copy[k];
			t.fresh[k] = g.fresh[k] && g.copy_state[k] == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			if (t.fresh[k])
				g.frames[k]++;
			g.fresh[k] = false;
		}
		g.takes++;
		for (auto it = g.old.begin(); it != g.old.end();)
			if (g.takes - it->second > 16)
			{
				it->first->Release();
				it = g.old.erase(it);
			}
			else
				++it;
		return t;
	}

	uint64_t frames_with(when w)
	{
		return w >= 0 && w < kinds ? g.frames[w].load() : 0;
	}
}
