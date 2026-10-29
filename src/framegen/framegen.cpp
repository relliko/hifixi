/*
 * AMD FSR 3 frame generation's swap chain in dgVoodoo's place (framegen.hpp). Built with the FidelityFX SDK's code
 * (fg_names.h force-included, as there).
 *
 * DXGI's factory objects share one method table: CreateSwapChain (IDXGIFactory, slot 10) and CreateSwapChainForHwnd
 * (IDXGIFactory2, slot 15) in it are pointed at ours once, from a throwaway factory, before dgVoodoo makes its own. A swap
 * chain for a window at least 640 wide made on a Direct3D 12 command queue is AMD's frame interpolation swap chain
 * instead (ffxCreateFrameinterpolationSwapchain*DX12); AMD's code then makes the real one through the same factory call,
 * which a per-thread guard hands straight on to DXGI's. Anything else (dgVoodoo's own 1x1 probe, another device kind)
 * is DXGI's as before.
 */

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include "framegen.hpp"
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace hifi::framegen
{
	namespace
	{
		using create_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory *self, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **out);
		using create_hwnd_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory2 *self, IUnknown *device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc,
			const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen, IDXGIOutput *output, IDXGISwapChain1 **out);
		constexpr UINT kCreateSlot = 10, kCreateHwndSlot = 15;

		struct state
		{
			std::mutex mutex;
			bool installed = false;
			create_fn create = nullptr;
			create_hwnd_fn create_hwnd = nullptr;
			std::atomic<uint32_t> wrapped{ 0 }, passed{ 0 }, failed{ 0 };
			std::string last;   // (what happened to the last swap chain asked for)
			UINT width = 0, height = 0;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			UINT buffers = 0, flags = 0;
			DXGI_SWAP_EFFECT effect = DXGI_SWAP_EFFECT_DISCARD;
		};
		state g;
		thread_local int t_inside = 0; // (AMD's code making the real swap chain: straight to DXGI)

		bool patch(void **slot, void *to, void **was)
		{
			DWORD old = 0;
			if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
				return false;
			*was = *slot;
			*slot = to;
			VirtualProtect(slot, sizeof(void *), old, &old);
			return true;
		}

		void note(const char *what, UINT w, UINT h, DXGI_FORMAT f, UINT buffers, UINT flags, DXGI_SWAP_EFFECT effect)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.last = what;
			g.width = w;
			g.height = h;
			g.format = f;
			g.buffers = buffers;
			g.flags = flags;
			g.effect = effect;
		}

		// A queue to make AMD's swap chain on, when this is one and the window's big enough to be the game's
		ID3D12CommandQueue *queue_for(IUnknown *device, UINT width)
		{
			ID3D12CommandQueue *queue = nullptr;
			if (t_inside != 0 || device == nullptr || width < 640 || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))
				return nullptr;
			return queue;
		}

		HRESULT STDMETHODCALLTYPE create_for_hwnd(IDXGIFactory2 *self, IUnknown *device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc,
			const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen, IDXGIOutput *output, IDXGISwapChain1 **out)
		{
			ID3D12CommandQueue *const queue = desc != nullptr && out != nullptr ? queue_for(device, desc->Width) : nullptr;
			if (queue != nullptr)
			{
				t_inside++;
				FfxSwapchain made = nullptr;
				const FfxErrorCode e = ffxCreateFrameinterpolationSwapchainForHwndDX12(hwnd, desc, fullscreen, queue, self, made);
				t_inside--;
				queue->Release();
				IDXGISwapChain4 *const chain = e == FFX_OK ? ffxGetDX12SwapchainPtr(made) : nullptr;
				if (chain != nullptr)
				{
					*out = chain;
					g.wrapped++;
					note("wrapped (CreateSwapChainForHwnd)", desc->Width, desc->Height, desc->Format, desc->BufferCount, desc->Flags, desc->SwapEffect);
					return S_OK;
				}
				g.failed++;
				char text[96];
				std::snprintf(text, sizeof(text), "AMD's swap chain refused (error %d): DXGI's made instead", int(e));
				note(text, desc->Width, desc->Height, desc->Format, desc->BufferCount, desc->Flags, desc->SwapEffect);
			}
			else if (t_inside == 0)
				g.passed++;
			return g.create_hwnd(self, device, hwnd, desc, fullscreen, output, out);
		}

		HRESULT STDMETHODCALLTYPE create(IDXGIFactory *self, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **out)
		{
			ID3D12CommandQueue *const queue = desc != nullptr && out != nullptr ? queue_for(device, desc->BufferDesc.Width) : nullptr;
			if (queue != nullptr)
			{
				t_inside++;
				FfxSwapchain made = nullptr;
				const FfxErrorCode e = ffxCreateFrameinterpolationSwapchainDX12(desc, queue, self, made);
				t_inside--;
				queue->Release();
				IDXGISwapChain4 *const chain = e == FFX_OK ? ffxGetDX12SwapchainPtr(made) : nullptr;
				if (chain != nullptr)
				{
					*out = chain;
					g.wrapped++;
					note("wrapped (CreateSwapChain)", desc->BufferDesc.Width, desc->BufferDesc.Height, desc->BufferDesc.Format, desc->BufferCount, desc->Flags, desc->SwapEffect);
					return S_OK;
				}
				g.failed++;
				char text[96];
				std::snprintf(text, sizeof(text), "AMD's swap chain refused (error %d): DXGI's made instead", int(e));
				note(text, desc->BufferDesc.Width, desc->BufferDesc.Height, desc->BufferDesc.Format, desc->BufferCount, desc->Flags, desc->SwapEffect);
			}
			else if (t_inside == 0)
				g.passed++;
			return g.create(self, device, desc, out);
		}
	}

	bool wanted()
	{
		char v[8] = {};
		return GetEnvironmentVariableA("HIFIXI_FRAMEGEN", v, sizeof(v)) != 0 && v[0] == '1';
	}

	bool install(void *create_factory1, std::string &why)
	{
		if (!wanted() || g.installed)
			return true;
		using create_factory_fn = HRESULT(WINAPI *)(REFIID, void **);
		IDXGIFactory2 *factory = nullptr;
		if (create_factory1 == nullptr || FAILED(reinterpret_cast<create_factory_fn>(create_factory1)(IID_PPV_ARGS(&factory))) || factory == nullptr)
		{
			why = "no DXGI factory to find its methods by";
			return false;
		}
		void **const table = *reinterpret_cast<void ***>(factory);
		void *was_create = nullptr, *was_hwnd = nullptr;
		const bool ok = patch(&table[kCreateSlot], reinterpret_cast<void *>(&create), &was_create) &&
			patch(&table[kCreateHwndSlot], reinterpret_cast<void *>(&create_for_hwnd), &was_hwnd);
		factory->Release();
		if (!ok)
		{
			why = "the factory's methods couldn't be changed";
			return false;
		}
		g.create = reinterpret_cast<create_fn>(was_create);
		g.create_hwnd = reinterpret_cast<create_hwnd_fn>(was_hwnd);
		g.installed = true;
		return true;
	}

	std::vector<std::string> status()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		char line[320];
		if (!g.installed)
			std::snprintf(line, sizeof(line), "frame generation: not in (HIFIXI_FRAMEGEN=1 before the game starts)");
		else
			std::snprintf(line, sizeof(line), "frame generation: swap chains %u AMD's, %u DXGI's, %u refused; the last: %s %ux%u format %u, %u buffers, effect %u, flags 0x%X",
				g.wrapped.load(), g.passed.load(), g.failed.load(), g.last.empty() ? "none" : g.last.c_str(), g.width, g.height, unsigned(g.format),
				g.buffers, unsigned(g.effect), g.flags);
		return { line };
	}
}
