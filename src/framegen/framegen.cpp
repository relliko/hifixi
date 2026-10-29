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
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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
			void (*log)(int, const char *) = nullptr;
			std::atomic<bool> wanted{ false }; // (the setting at the start)
			std::atomic<bool> on{ false };     // (and now)
			std::atomic<bool> automatic{ true }; // (only while the game runs below half the refresh rate)
			std::atomic<uint32_t> refresh_override{ 0 }; // (tests: the screen's refresh rate; 0 its monitor's)
		};
		state g;
		thread_local int t_inside = 0; // (AMD's code making the real swap chain: straight to DXGI)

		// Generation: AMD's frame interpolation and optical flow, made for A's size and the screen's, their shared
		// resources (dilated depth and motion, the last frame's depth, the flow and its scene-change data) in one
		// backend context of their own, as AMD's FSR 3 makes them. One set (a kit) for each pair of sizes; a kit made for
		// sizes gone is retired, not destroyed: destroyed once the game's queue is past a fence signalled at its retiring.
		//
		// Locks: prepare() runs inside HiFi XI's list hooks with upscale's lock held, and DXGI's own present work (on its
		// threads, its swap chain's lock held) goes through those hooks too (its lists' Reset). So nothing here calls the
		// swap chain or waits for the GPU or a present with gen.mutex or a hook's lock held: the swap chain's size comes from
		// its making and each present (generate()), and generation is turned on and off from the game thread's present
		// (present()), outside every lock.
		struct kit
		{
			uint32_t rw = 0, rh = 0, dw = 0, dh = 0;
			std::vector<uint8_t> scratch;
			FfxInterface iface = {};
			FfxUInt32 shared_id = 0;
			FfxOpticalflowContext of = {};
			FfxFrameInterpolationContext fi = {};
			enum { kDilatedDepth, kDilatedMotion, kPrevDepth, kFlow, kSceneChange, kShared };
			FfxResourceInternal shared[kShared] = {};
			int made = 0; // (how far its making got: 1 backend, 2 + optical flow, 3 + frame interpolation, 4 + resources)
			uint64_t retired_at = 0;
		};

		void destroy(kit &k)
		{
			if (k.made >= 4)
				for (FfxResourceInternal &r : k.shared)
					k.iface.fpDestroyResource(&k.iface, r, k.shared_id);
			if (k.made >= 3)
				ffxFrameInterpolationContextDestroy(&k.fi);
			if (k.made >= 2)
				ffxOpticalflowContextDestroy(&k.of);
			if (k.made >= 1)
				k.iface.fpDestroyBackendContext(&k.iface, k.shared_id);
			k.made = 0;
		}

		struct generation
		{
			std::mutex mutex;
			FfxSwapchain swapchain = nullptr;  // (the game window's, AMD's)
			ID3D12CommandQueue *queue = nullptr;
			uint32_t sw = 0, sh = 0;           // (its size: at its making, then each present's)
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			std::unique_ptr<kit> now;
			std::vector<std::unique_ptr<kit>> retired;
			ID3D12Fence *fence = nullptr;     // (on the game's queue: kits retired before its value are done with)
			uint64_t fence_value = 0;
			FfxFrameInterpolationPrepareDescription prepared = {};
			FfxResource picture = {};
			bool fresh = false, reset = false;
			bool apart = true; // (FSR 2's picture the swap chain's size: the world without the UI)
			uint64_t dispatch_id = 0;
			uint64_t prepared_at = 0;  // (GetTickCount64 of the last prepare())
			bool enabled = false;      // (as last told to AMD's swap chain, from present())
			// Automatic (present()): the game's frame rate (its presents, smoothed), the screen's refresh rate (its
			// window's monitor, read every 5 s), whether generation is wanted for it, since when what would change that
			// has held, and when it last changed
			HWND hwnd = nullptr;
			uint32_t refresh = 0;
			uint64_t refresh_at = 0;
			LARGE_INTEGER last_present = {};
			double fps = 0.0;
			bool auto_wants = true;
			uint64_t auto_since = 0, auto_switched = 0;
			uint32_t prepares = 0, generated = 0, repeated = 0, failed = 0, switches = 0;
			ID3D12Resource *last_output = nullptr;
			std::string why;
		};
		generation gen;

		void keep_swapchain(FfxSwapchain made, ID3D12CommandQueue *queue, UINT width, UINT height, DXGI_FORMAT format, HWND hwnd)
		{
			std::lock_guard<std::mutex> lock(gen.mutex);
			// (a swap chain made again: the kits made for the last are left, not destroyed under it)
			gen.now.release();
			gen.retired.clear();
			if (gen.fence != nullptr)
				gen.fence->Release();
			gen.fence = nullptr;
			gen.fresh = false;
			gen.enabled = false;
			gen.swapchain = made;
			if (gen.queue != nullptr)
				gen.queue->Release();
			gen.queue = queue;
			queue->AddRef();
			gen.sw = width;
			gen.sh = height;
			gen.format = format;
			gen.hwnd = hwnd;
			gen.refresh_at = 0;
		}

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
			if (g.log != nullptr)
			{
				char text[200];
				std::snprintf(text, sizeof(text), "HiFi XI: frame generation: a swap chain %s, %ux%u format %u, %u buffers, effect %u, flags 0x%X", what, w, h,
					unsigned(f), buffers, unsigned(effect), flags);
				g.log(std::strncmp(what, "wrapped", 7) == 0 ? 0 : 1, text);
			}
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
				IDXGISwapChain4 *const chain = e == FFX_OK ? ffxGetDX12SwapchainPtr(made) : nullptr;
				if (chain != nullptr)
					keep_swapchain(made, queue, desc->Width, desc->Height, desc->Format, hwnd);
				queue->Release();
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
				IDXGISwapChain4 *const chain = e == FFX_OK ? ffxGetDX12SwapchainPtr(made) : nullptr;
				if (chain != nullptr)
					keep_swapchain(made, queue, desc->BufferDesc.Width, desc->BufferDesc.Height, desc->BufferDesc.Format, desc->OutputWindow);
				queue->Release();
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

		// (gen.mutex held) The retired kits the game's queue is past: destroyed. Never waits.
		void sweep()
		{
			if (gen.fence == nullptr)
				return;
			const uint64_t done = gen.fence->GetCompletedValue();
			if (done == UINT64_MAX) // (the device removed: left as they are)
				return;
			for (auto it = gen.retired.begin(); it != gen.retired.end();)
				if ((*it)->retired_at <= done)
				{
					destroy(**it);
					it = gen.retired.erase(it);
				}
				else
					++it;
		}

		// (gen.mutex held) The kit for A's size and the screen's; the one before retired behind the fence
		bool make_kit(ID3D12Device *device, uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh, std::string &why)
		{
			if (gen.fence == nullptr && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gen.fence))))
			{
				gen.fence = nullptr;
				why = "no fence for frame generation's retired contexts";
				return false;
			}
			if (gen.now != nullptr)
			{
				// (the game's queue has every list that used it already: the fence passes once they're done)
				gen.now->retired_at = ++gen.fence_value;
				gen.queue->Signal(gen.fence, gen.fence_value);
				gen.retired.push_back(std::move(gen.now));
			}
			gen.fresh = false;
			auto k = std::make_unique<kit>();
			constexpr size_t kContexts = FFX_OPTICALFLOW_CONTEXT_COUNT + FFX_FRAMEINTERPOLATION_CONTEXT_COUNT + 1;
			k->scratch.assign(ffxGetScratchMemorySizeDX12(kContexts), 0);
			FfxErrorCode e = ffxGetInterfaceDX12(&k->iface, ffxGetDeviceDX12(device), k->scratch.data(), k->scratch.size(), kContexts);
			if (e == FFX_OK && (e = k->iface.fpCreateBackendContext(&k->iface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &k->shared_id)) == FFX_OK)
				k->made = 1;
			if (e != FFX_OK)
			{
				why = "AMD's DX12 backend refused (error " + std::to_string(int(e)) + ")";
				return false;
			}
			FfxOpticalflowContextDescription od = {};
			od.backendInterface = k->iface;
			od.resolution = { dw, dh };
			if ((e = ffxOpticalflowContextCreate(&k->of, &od)) != FFX_OK)
			{
				destroy(*k);
				why = "optical flow's context refused (error " + std::to_string(int(e)) + ")";
				return false;
			}
			k->made = 2;
			FfxFrameInterpolationContextDescription fd = {};
			fd.backendInterface = k->iface;
			fd.flags = FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE;
			fd.maxRenderSize = { rw, rh };
			fd.displaySize = { dw, dh };
			fd.backBufferFormat = ffxGetSurfaceFormatDX12(gen.format);
			fd.previousInterpolationSourceFormat = fd.backBufferFormat;
			if ((e = ffxFrameInterpolationContextCreate(&k->fi, &fd)) != FFX_OK)
			{
				destroy(*k);
				why = "frame interpolation's context refused (error " + std::to_string(int(e)) + ")";
				return false;
			}
			k->made = 3;
			FfxFrameInterpolationSharedResourceDescriptions fs = {};
			FfxOpticalflowSharedResourceDescriptions os = {};
			ffxFrameInterpolationGetSharedResourceDescriptions(&k->fi, &fs);
			ffxOpticalflowGetSharedResourceDescriptions(&k->of, &os);
			const FfxCreateResourceDescription *descs[kit::kShared] = { &fs.dilatedDepth, &fs.dilatedMotionVectors, &fs.reconstructedPrevNearestDepth,
				&os.opticalFlowVector, &os.opticalFlowSCD };
			int i = 0;
			for (; i < kit::kShared && e == FFX_OK; ++i)
				e = k->iface.fpCreateResource(&k->iface, descs[i], k->shared_id, &k->shared[i]);
			if (e != FFX_OK)
			{
				for (int j = 0; j < i - 1; ++j)
					k->iface.fpDestroyResource(&k->iface, k->shared[j], k->shared_id);
				destroy(*k);
				why = "frame generation's resources couldn't be made (error " + std::to_string(int(e)) + ")";
				return false;
			}
			k->made = 4;
			k->rw = rw;
			k->rh = rh;
			k->dw = dw;
			k->dh = dh;
			gen.now = std::move(k);
			return true;
		}

		// The frame between the last and this one, asked for by AMD's swap chain at dgVoodoo's Present (its list on the
		// game's queue, after the frame's own): optical flow over FSR 2's pictures, then frame interpolation. Without a
		// prepare() this frame, or made for another size of the screen, this frame again.
		FfxErrorCode generate(const FfxFrameGenerationDispatchDescription *d, void *)
		{
			std::lock_guard<std::mutex> lock(gen.mutex);
			ID3D12GraphicsCommandList *const list = static_cast<ID3D12GraphicsCommandList *>(d->commandList);
			const uint32_t pw = d->presentColor.description.width, ph = d->presentColor.description.height;
			gen.sw = pw;
			gen.sh = ph;
			kit *const k = gen.now.get();
			if (k == nullptr || !gen.fresh || k->dw != pw || k->dh != ph)
			{
				gen.fresh = false;
				ID3D12Resource *const from = static_cast<ID3D12Resource *>(d->presentColor.resource), *const to = static_cast<ID3D12Resource *>(d->outputs[0].resource);
				D3D12_RESOURCE_BARRIER b[2] = {};
				b[0].Transition = { from, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE };
				b[1].Transition = { to, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST };
				list->ResourceBarrier(2, b);
				list->CopyResource(to, from);
				std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
				std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
				list->ResourceBarrier(2, b);
				gen.repeated++;
				return FFX_OK;
			}
			gen.fresh = false;
			const bool reset = d->reset || gen.reset;
			const FfxResource flow = k->iface.fpGetResource(&k->iface, k->shared[kit::kFlow]);
			const FfxResource scene_change = k->iface.fpGetResource(&k->iface, k->shared[kit::kSceneChange]);

			FfxOpticalflowDispatchDescription o = {};
			o.commandList = d->commandList;
			o.color = gen.apart ? gen.picture : d->presentColor;
			o.reset = reset;
			o.backbufferTransferFunction = d->backBufferTransferFunction;
			o.minMaxLuminance = { d->minMaxLuminance[0], d->minMaxLuminance[1] };
			o.opticalFlowVector = flow;
			o.opticalFlowSCD = scene_change;
			FfxErrorCode e = ffxOpticalflowContextDispatch(&k->of, &o);

			const FfxFrameInterpolationPrepareDescription &p = gen.prepared;
			FfxFrameInterpolationDispatchDescription f = {};
			f.commandList = d->commandList;
			f.displaySize = { pw, ph };
			f.currentBackBuffer = d->presentColor;
			if (gen.apart)
				f.currentBackBuffer_HUDLess = gen.picture;
			f.renderSize = p.renderSize;
			f.output = d->outputs[0];
			f.opticalFlowVector = flow;
			f.opticalFlowSceneChangeDetection = scene_change;
			f.opticalFlowBlockSize = 8;
			f.opticalFlowScale = { 1.0f / float(pw), 1.0f / float(ph) };
			f.frameTimeDelta = p.frameTimeDelta;
			f.reset = reset;
			f.cameraNear = p.cameraNear;
			f.cameraFar = p.cameraFar;
			f.viewSpaceToMetersFactor = p.viewSpaceToMetersFactor;
			f.cameraFovAngleVertical = p.cameraFovAngleVertical;
			f.interpolationRect = { 0, 0, int(pw), int(ph) }; // (the whole screen: the swap chain's own rect is from when generation was turned on)
			f.frameID = ++gen.dispatch_id; // (one more each frame made: the swap chain's id is the game thread's)
			f.dilatedDepth = p.dilatedDepth;
			f.dilatedMotionVectors = p.dilatedMotionVectors;
			f.reconstructedPrevDepth = p.reconstructedPrevDepth;
			f.backBufferTransferFunction = d->backBufferTransferFunction;
			f.minMaxLuminance[0] = d->minMaxLuminance[0];
			f.minMaxLuminance[1] = d->minMaxLuminance[1];
			e |= ffxFrameInterpolationDispatch(&k->fi, &f);
			if (e != FFX_OK)
			{
				gen.failed++;
				gen.why = "frame interpolation's dispatch failed (error " + std::to_string(int(e)) + ")";
				return e;
			}
			gen.generated++;
			gen.last_output = static_cast<ID3D12Resource *>(d->outputs[0].resource);
			return FFX_OK;
		}
	}

	bool prepare(ID3D12GraphicsCommandList *list, const frame &in, std::string &why)
	{
		std::lock_guard<std::mutex> lock(gen.mutex);
		if (!g.installed || gen.swapchain == nullptr)
		{
			why = !g.installed ? "not wanted" : "no AMD swap chain";
			return false;
		}
		if (!g.on.load())
		{
			why = "turned off";
			return false;
		}
		if (list == nullptr || in.depth == nullptr || in.motion == nullptr || in.picture == nullptr)
		{
			why = "FSR 2's depth, motion or picture missing";
			return false;
		}
		if (gen.sw == 0 || gen.sh == 0)
		{
			why = "AMD's swap chain's size isn't known";
			return false;
		}
		sweep();
		// (made for the swap chain's size; FSR 2's picture kept apart from the UI only when it's that size too, else the
		// back buffer as it is, its UI moved with the world)
		const D3D12_RESOURCE_DESC dd = in.depth->GetDesc(), pd = in.picture->GetDesc();
		const uint32_t rw = uint32_t(dd.Width), rh = dd.Height, dw = gen.sw, dh = gen.sh;
		gen.apart = pd.Width == dw && pd.Height == dh;
		kit *k = gen.now.get();
		if (k == nullptr || rw != k->rw || rh != k->rh || dw != k->dw || dh != k->dh)
		{
			ID3D12Device *device = nullptr;
			const bool ok = SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&device))) && make_kit(device, rw, rh, dw, dh, why);
			if (device != nullptr)
				device->Release();
			if (!ok)
			{
				gen.failed++;
				gen.why = why;
				return false;
			}
			k = gen.now.get();
		}
		FfxFrameInterpolationPrepareDescription p = {};
		p.commandList = ffxGetCommandListDX12(list);
		p.renderSize = { rw, rh };
		p.jitterOffset = { in.jitter[0], in.jitter[1] };
		p.motionVectorScale = { float(rw), float(rh) };
		p.frameTimeDelta = std::clamp(in.dt_ms, 1.0f, 100.0f);
		p.cameraNear = 0.1f;
		p.cameraFar = 65535.0f;
		p.viewSpaceToMetersFactor = 1.0f;
		p.cameraFovAngleVertical = in.fov;
		p.depth = ffxGetResourceDX12(in.depth, ffxGetResourceDescriptionDX12(in.depth), L"hifixi: FSR 2's depth", FFX_RESOURCE_STATE_COMPUTE_READ);
		p.motionVectors = ffxGetResourceDX12(in.motion, ffxGetResourceDescriptionDX12(in.motion), L"hifixi: FSR 2's motion", FFX_RESOURCE_STATE_COMPUTE_READ);
		p.frameID = gen.dispatch_id + 1;
		p.dilatedDepth = k->iface.fpGetResource(&k->iface, k->shared[kit::kDilatedDepth]);
		p.dilatedMotionVectors = k->iface.fpGetResource(&k->iface, k->shared[kit::kDilatedMotion]);
		p.reconstructedPrevDepth = k->iface.fpGetResource(&k->iface, k->shared[kit::kPrevDepth]);
		const FfxErrorCode e = ffxFrameInterpolationPrepare(&k->fi, &p);
		if (e != FFX_OK)
		{
			gen.failed++;
			why = gen.why = "frame interpolation's prepare failed (error " + std::to_string(int(e)) + ")";
			return false;
		}
		gen.prepared = p;
		gen.picture = ffxGetResourceDX12(in.picture, ffxGetResourceDescriptionDX12(in.picture), L"hifixi: FSR 2's picture", FFX_RESOURCE_STATE_PIXEL_READ);
		gen.fresh = true;
		gen.reset = in.reset;
		gen.prepares++;
		gen.prepared_at = GetTickCount64();
		gen.why.clear();
		return true;
	}

	namespace
	{
		// (gen.mutex held) The screen's refresh rate: the game window's monitor's, read again every 5 s; 0 unknown
		uint32_t refresh_rate(uint64_t now)
		{
			if (const uint32_t forced = g.refresh_override.load())
				return gen.refresh = forced;
			if (gen.refresh_at != 0 && now - gen.refresh_at < 5000)
				return gen.refresh;
			gen.refresh_at = now;
			gen.refresh = 0;
			MONITORINFOEXW mi = {};
			mi.cbSize = sizeof(mi);
			DEVMODEW dm = {};
			dm.dmSize = sizeof(dm);
			if (gen.hwnd != nullptr && GetMonitorInfoW(MonitorFromWindow(gen.hwnd, MONITOR_DEFAULTTONEAREST), &mi) &&
				EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
				gen.refresh = dm.dmDisplayFrequency;
			return gen.refresh;
		}

		// (gen.mutex held) Automatic: generation wanted while the game runs below half the refresh rate. With it on, the
		// game's held at about half (each of its frames shown twice): at 96% of half or more for 3 s, off, to see what it
		// runs at alone; with it off, below 90% of half for 2 s, on. Not changed again within 5 s of the last change.
		bool automatic_wants(uint64_t now)
		{
			const uint32_t hz = refresh_rate(now);
			if (hz == 0 || gen.fps <= 0.0)
				return gen.auto_wants;
			const double half = hz * 0.5;
			const bool change = gen.auto_wants ? gen.fps >= 0.96 * half : gen.fps < 0.90 * half;
			if (!change)
			{
				gen.auto_since = 0;
				return gen.auto_wants;
			}
			if (gen.auto_since == 0)
				gen.auto_since = now;
			if (now - gen.auto_since >= (gen.auto_wants ? 3000u : 2000u) && now - gen.auto_switched >= 5000)
			{
				gen.auto_wants = !gen.auto_wants;
				gen.auto_switched = now;
				gen.auto_since = 0;
			}
			return gen.auto_wants;
		}
	}

	void present()
	{
		FfxFrameGenerationConfig c = {};
		{
			std::lock_guard<std::mutex> lock(gen.mutex);
			if (!g.installed || gen.swapchain == nullptr)
				return;
			// (the game's frame rate from its presents, smoothed over about half a second)
			LARGE_INTEGER t, f;
			QueryPerformanceCounter(&t);
			QueryPerformanceFrequency(&f);
			if (gen.last_present.QuadPart != 0)
			{
				const double dt = double(t.QuadPart - gen.last_present.QuadPart) / double(f.QuadPart);
				if (dt > 0.0 && dt < 0.5)
					gen.fps = gen.fps <= 0.0 ? 1.0 / dt : gen.fps + (1.0 / dt - gen.fps) * std::min(1.0, dt / 0.5);
			}
			gen.last_present = t;
			const uint64_t now = GetTickCount64();
			// (on while FSR 2 hands its frames over and the setting's on, automatic: while the game runs below half the
			// refresh rate; off a second after FSR 2's last frame, else each frame is shown twice)
			const bool fsr2 = g.on.load() && gen.now != nullptr && now - gen.prepared_at < 1000;
			const bool want = fsr2 && (!g.automatic.load() || automatic_wants(now));
			if (want == gen.enabled)
				return;
			gen.enabled = want;
			gen.switches++;
			c.swapChain = gen.swapchain;
			c.frameGenerationCallback = generate;
			c.frameGenerationEnabled = want;
			c.interpolationRect = { 0, 0, int(gen.sw), int(gen.sh) };
			c.frameID = gen.dispatch_id;
		}
		// (outside every lock: turning it on or off waits for the presents in flight, whose DXGI work goes through
		// HiFi XI's hooks and generate())
		const ULONGLONG asked = GetTickCount64();
		const FfxErrorCode e = ffxSetFrameGenerationConfigToSwapchainDX12(&c);
		// (in HiFi XI's log each time, with how long the game's thread waited on it: a hang to find)
		if (g.log != nullptr)
		{
			char text[160];
			std::snprintf(text, sizeof(text), "HiFi XI: frame generation turned %s (the game at %.0f fps, the screen %u Hz; the game's thread waited %llu ms)",
				c.frameGenerationEnabled ? "on" : "off", gen.fps, gen.refresh, GetTickCount64() - asked);
			g.log(GetTickCount64() - asked > 250 ? 1 : 0, text);
		}
		if (e != FFX_OK)
		{
			std::lock_guard<std::mutex> lock(gen.mutex);
			gen.enabled = !c.frameGenerationEnabled;
			gen.failed++;
			gen.why = "AMD's swap chain refused to turn generation " + std::string(c.frameGenerationEnabled ? "on" : "off") + " (error " + std::to_string(int(e)) + ")";
		}
	}

	uint64_t made_between()
	{
		std::lock_guard<std::mutex> lock(gen.mutex);
		return gen.generated;
	}

	ID3D12Resource *last_made()
	{
		std::lock_guard<std::mutex> lock(gen.mutex);
		return gen.last_output;
	}

	void set_wanted(bool on)
	{
		g.wanted = on;
		g.on = on;
	}

	bool wanted()
	{
		return g.wanted.load();
	}

	void set_on(bool on)
	{
		g.on = on;
	}

	void set_automatic(bool on)
	{
		g.automatic = on;
	}

	void set_refresh_for_tests(uint32_t hz)
	{
		g.refresh_override = hz;
	}

	bool install(void *create_factory1, std::string &why, void (*log)(int, const char *))
	{
		g.log = log;
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
			std::snprintf(line, sizeof(line), g.on.load() ? "frame generation: on from the next start (AMD's swap chain is put in only as the game starts)" :
				"frame generation: off (the Upscaling tab)");
		else
			std::snprintf(line, sizeof(line), "frame generation: swap chains %u AMD's, %u DXGI's, %u refused; the last: %s %ux%u format %u, %u buffers, effect %u, flags 0x%X",
				g.wrapped.load(), g.passed.load(), g.failed.load(), g.last.empty() ? "none" : g.last.c_str(), g.width, g.height, unsigned(g.format),
				g.buffers, unsigned(g.effect), g.flags);
		std::vector<std::string> lines = { line };
		if (g.installed)
		{
			std::lock_guard<std::mutex> glock(gen.mutex);
			char state[160];
			if (!g.on.load())
				std::snprintf(state, sizeof(state), "off (turned off: on again at once from the menu)");
			else if (gen.now == nullptr)
				std::snprintf(state, sizeof(state), "contexts not made");
			else if (!gen.enabled && GetTickCount64() - gen.prepared_at >= 1000)
				std::snprintf(state, sizeof(state), "off (no FSR 2 frames)");
			else if (!gen.enabled)
				std::snprintf(state, sizeof(state), "off (automatic: the game runs %.0f fps, at or above half the screen's %u Hz)", gen.fps, gen.refresh);
			else
				std::snprintf(state, sizeof(state), "on%s, %s", g.automatic.load() ? " (automatic: the game below half the screen's refresh rate)" : "",
					gen.apart ? "the UI kept apart" : "the UI moved with the world (FSR 2's picture isn't the screen's size)");
			std::snprintf(line, sizeof(line), "frame generation: %s; %u frames prepared, %u made between, %u shown twice, %u failed, turned on/off %u times%s%s",
				state, gen.prepares, gen.generated, gen.repeated, gen.failed, gen.switches, gen.why.empty() ? "" : ": ", gen.why.c_str());
			lines.push_back(line);
		}
		return lines;
	}
}
