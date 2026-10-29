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
		};
		state g;
		thread_local int t_inside = 0; // (AMD's code making the real swap chain: straight to DXGI)

		// Generation: AMD's frame interpolation and optical flow, made for A's size and the screen's, their shared
		// resources (dilated depth and motion, the last frame's depth, the flow and its scene-change data) in one
		// backend context of their own, as AMD's FSR 3 makes them
		struct generation
		{
			std::mutex mutex;
			FfxSwapchain swapchain = nullptr;  // (the game window's, AMD's)
			ID3D12CommandQueue *queue = nullptr;
			bool made = false;
			uint32_t rw = 0, rh = 0, dw = 0, dh = 0;
			std::vector<uint8_t> scratch;
			FfxInterface iface = {};
			FfxUInt32 shared_id = 0;
			FfxOpticalflowContext of = {};
			FfxFrameInterpolationContext fi = {};
			enum { kDilatedDepth, kDilatedMotion, kPrevDepth, kFlow, kSceneChange, kShared };
			FfxResourceInternal shared[kShared] = {};
			FfxFrameInterpolationPrepareDescription prepared = {};
			FfxResource picture = {};
			bool fresh = false, reset = false;
			bool apart = true; // (FSR 2's picture the swap chain's size: the world without the UI)
			uint64_t frame_id = 0;
			uint32_t prepares = 0, generated = 0, repeated = 0, failed = 0;
			ID3D12Resource *last_output = nullptr;
			std::string why;
		};
		generation gen;

		void keep_swapchain(FfxSwapchain made, ID3D12CommandQueue *queue)
		{
			std::lock_guard<std::mutex> lock(gen.mutex);
			// (a swap chain made again: the contexts made for the last are left, not destroyed under it)
			gen.made = false;
			gen.swapchain = made;
			if (gen.queue != nullptr)
				gen.queue->Release();
			gen.queue = queue;
			queue->AddRef();
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
					keep_swapchain(made, queue);
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
					keep_swapchain(made, queue);
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

		// (gen.mutex held) Everything made for the last sizes destroyed, the GPU done with it first
		void release_generation()
		{
			if (!gen.made)
				return;
			ffxWaitForPresents(gen.swapchain);
			ID3D12Device *device = nullptr;
			ID3D12Fence *fence = nullptr;
			if (gen.queue != nullptr && SUCCEEDED(gen.queue->GetDevice(IID_PPV_ARGS(&device))) &&
				SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
			{
				HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
				gen.queue->Signal(fence, 1);
				fence->SetEventOnCompletion(1, done);
				WaitForSingleObject(done, 5000);
				CloseHandle(done);
			}
			if (fence != nullptr)
				fence->Release();
			if (device != nullptr)
				device->Release();
			ffxFrameInterpolationContextDestroy(&gen.fi);
			ffxOpticalflowContextDestroy(&gen.of);
			for (FfxResourceInternal &r : gen.shared)
				gen.iface.fpDestroyResource(&gen.iface, r, gen.shared_id);
			gen.iface.fpDestroyBackendContext(&gen.iface, gen.shared_id);
			gen.made = false;
		}

		// (gen.mutex held) The contexts and shared resources for A's size and the screen's
		bool make_generation(ID3D12Device *device, uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh, DXGI_FORMAT format, std::string &why)
		{
			release_generation();
			constexpr size_t kContexts = FFX_OPTICALFLOW_CONTEXT_COUNT + FFX_FRAMEINTERPOLATION_CONTEXT_COUNT + 1;
			gen.scratch.assign(ffxGetScratchMemorySizeDX12(kContexts), 0);
			FfxErrorCode e = ffxGetInterfaceDX12(&gen.iface, ffxGetDeviceDX12(device), gen.scratch.data(), gen.scratch.size(), kContexts);
			if (e == FFX_OK)
				e = gen.iface.fpCreateBackendContext(&gen.iface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &gen.shared_id);
			if (e != FFX_OK)
			{
				why = "AMD's DX12 backend refused (error " + std::to_string(int(e)) + ")";
				return false;
			}
			FfxOpticalflowContextDescription od = {};
			od.backendInterface = gen.iface;
			od.resolution = { dw, dh };
			if ((e = ffxOpticalflowContextCreate(&gen.of, &od)) != FFX_OK)
			{
				gen.iface.fpDestroyBackendContext(&gen.iface, gen.shared_id);
				why = "optical flow's context refused (error " + std::to_string(int(e)) + ")";
				return false;
			}
			FfxFrameInterpolationContextDescription fd = {};
			fd.backendInterface = gen.iface;
			fd.flags = FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE;
			fd.maxRenderSize = { rw, rh };
			fd.displaySize = { dw, dh };
			fd.backBufferFormat = ffxGetSurfaceFormatDX12(format);
			fd.previousInterpolationSourceFormat = fd.backBufferFormat;
			if ((e = ffxFrameInterpolationContextCreate(&gen.fi, &fd)) != FFX_OK)
			{
				ffxOpticalflowContextDestroy(&gen.of);
				gen.iface.fpDestroyBackendContext(&gen.iface, gen.shared_id);
				why = "frame interpolation's context refused (error " + std::to_string(int(e)) + ")";
				return false;
			}
			FfxFrameInterpolationSharedResourceDescriptions fs = {};
			FfxOpticalflowSharedResourceDescriptions os = {};
			ffxFrameInterpolationGetSharedResourceDescriptions(&gen.fi, &fs);
			ffxOpticalflowGetSharedResourceDescriptions(&gen.of, &os);
			const FfxCreateResourceDescription *descs[generation::kShared] = { &fs.dilatedDepth, &fs.dilatedMotionVectors, &fs.reconstructedPrevNearestDepth,
				&os.opticalFlowVector, &os.opticalFlowSCD };
			for (int i = 0; i < generation::kShared && e == FFX_OK; ++i)
				e = gen.iface.fpCreateResource(&gen.iface, descs[i], gen.shared_id, &gen.shared[i]);
			gen.made = true;
			gen.rw = rw;
			gen.rh = rh;
			gen.dw = dw;
			gen.dh = dh;
			gen.fresh = false;
			if (e != FFX_OK)
			{
				release_generation();
				why = "frame generation's resources couldn't be made (error " + std::to_string(int(e)) + ")";
				return false;
			}
			return true;
		}

		// The frame between the last and this one, asked for by AMD's swap chain at dgVoodoo's Present (its list on the
		// game's queue, after the frame's own): optical flow over FSR 2's pictures, then frame interpolation. Without a
		// prepare() this frame, this frame again.
		FfxErrorCode generate(const FfxFrameGenerationDispatchDescription *d, void *)
		{
			std::lock_guard<std::mutex> lock(gen.mutex);
			ID3D12GraphicsCommandList *const list = static_cast<ID3D12GraphicsCommandList *>(d->commandList);
			if (!gen.made || !gen.fresh)
			{
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
			const FfxResource flow = gen.iface.fpGetResource(&gen.iface, gen.shared[generation::kFlow]);
			const FfxResource scene_change = gen.iface.fpGetResource(&gen.iface, gen.shared[generation::kSceneChange]);

			FfxOpticalflowDispatchDescription o = {};
			o.commandList = d->commandList;
			o.color = gen.apart ? gen.picture : d->presentColor;
			o.reset = reset;
			o.backbufferTransferFunction = d->backBufferTransferFunction;
			o.minMaxLuminance = { d->minMaxLuminance[0], d->minMaxLuminance[1] };
			o.opticalFlowVector = flow;
			o.opticalFlowSCD = scene_change;
			FfxErrorCode e = ffxOpticalflowContextDispatch(&gen.of, &o);

			const FfxFrameInterpolationPrepareDescription &p = gen.prepared;
			FfxFrameInterpolationDispatchDescription f = {};
			f.commandList = d->commandList;
			f.displaySize = { d->presentColor.description.width, d->presentColor.description.height };
			f.currentBackBuffer = d->presentColor;
			if (gen.apart)
				f.currentBackBuffer_HUDLess = gen.picture;
			f.renderSize = p.renderSize;
			f.output = d->outputs[0];
			f.opticalFlowVector = flow;
			f.opticalFlowSceneChangeDetection = scene_change;
			f.opticalFlowBlockSize = 8;
			f.opticalFlowScale = { 1.0f / float(f.displaySize.width), 1.0f / float(f.displaySize.height) };
			f.frameTimeDelta = p.frameTimeDelta;
			f.reset = reset;
			f.cameraNear = p.cameraNear;
			f.cameraFar = p.cameraFar;
			f.viewSpaceToMetersFactor = p.viewSpaceToMetersFactor;
			f.cameraFovAngleVertical = p.cameraFovAngleVertical;
			f.interpolationRect = d->interpolationRect;
			f.frameID = d->frameID;
			f.dilatedDepth = p.dilatedDepth;
			f.dilatedMotionVectors = p.dilatedMotionVectors;
			f.reconstructedPrevDepth = p.reconstructedPrevDepth;
			f.backBufferTransferFunction = d->backBufferTransferFunction;
			f.minMaxLuminance[0] = d->minMaxLuminance[0];
			f.minMaxLuminance[1] = d->minMaxLuminance[1];
			e |= ffxFrameInterpolationDispatch(&gen.fi, &f);
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
		if (list == nullptr || in.depth == nullptr || in.motion == nullptr || in.picture == nullptr)
		{
			why = "FSR 2's depth, motion or picture missing";
			return false;
		}
		// (made for the swap chain's size; FSR 2's picture kept apart from the UI only when it's that size too, else the
		// back buffer as it is, its UI moved with the world)
		const D3D12_RESOURCE_DESC dd = in.depth->GetDesc(), pd = in.picture->GetDesc();
		DXGI_SWAP_CHAIN_DESC1 sd = {};
		IDXGISwapChain4 *const chain = ffxGetDX12SwapchainPtr(gen.swapchain);
		if (chain == nullptr || FAILED(chain->GetDesc1(&sd)) || sd.Width == 0 || sd.Height == 0)
		{
			why = "AMD's swap chain's size isn't known";
			return false;
		}
		const uint32_t rw = uint32_t(dd.Width), rh = dd.Height, dw = sd.Width, dh = sd.Height;
		gen.apart = pd.Width == dw && pd.Height == dh;
		if (!gen.made || rw != gen.rw || rh != gen.rh || dw != gen.dw || dh != gen.dh)
		{
			ID3D12Device *device = nullptr;
			const bool ok = SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&device))) && make_generation(device, rw, rh, dw, dh, sd.Format, why);
			if (device != nullptr)
				device->Release();
			if (!ok)
			{
				gen.failed++;
				gen.why = why;
				return false;
			}
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
		p.frameID = ++gen.frame_id;
		p.dilatedDepth = gen.iface.fpGetResource(&gen.iface, gen.shared[generation::kDilatedDepth]);
		p.dilatedMotionVectors = gen.iface.fpGetResource(&gen.iface, gen.shared[generation::kDilatedMotion]);
		p.reconstructedPrevDepth = gen.iface.fpGetResource(&gen.iface, gen.shared[generation::kPrevDepth]);
		FfxErrorCode e = ffxFrameInterpolationPrepare(&gen.fi, &p);
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

		// AMD's swap chain told, each frame: generation on through generate(), this frame's id (one more each frame)
		FfxFrameGenerationConfig c = {};
		c.swapChain = gen.swapchain;
		c.frameGenerationCallback = generate;
		c.frameGenerationEnabled = true;
		if (gen.apart)
			c.HUDLessColor = gen.picture;
		c.interpolationRect = { 0, 0, int(dw), int(dh) };
		c.frameID = gen.frame_id;
		if ((e = ffxSetFrameGenerationConfigToSwapchainDX12(&c)) != FFX_OK)
		{
			gen.failed++;
			why = gen.why = "AMD's swap chain refused the configuration (error " + std::to_string(int(e)) + ")";
			return false;
		}
		gen.why.clear();
		return true;
	}

	ID3D12Resource *last_made()
	{
		std::lock_guard<std::mutex> lock(gen.mutex);
		return gen.last_output;
	}

	bool wanted()
	{
		char v[8] = {};
		return GetEnvironmentVariableA("HIFIXI_FRAMEGEN", v, sizeof(v)) != 0 && v[0] == '1';
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
			std::snprintf(line, sizeof(line), "frame generation: not in (HIFIXI_FRAMEGEN=1 before the game starts)");
		else
			std::snprintf(line, sizeof(line), "frame generation: swap chains %u AMD's, %u DXGI's, %u refused; the last: %s %ux%u format %u, %u buffers, effect %u, flags 0x%X",
				g.wrapped.load(), g.passed.load(), g.failed.load(), g.last.empty() ? "none" : g.last.c_str(), g.width, g.height, unsigned(g.format),
				g.buffers, unsigned(g.effect), g.flags);
		std::vector<std::string> lines = { line };
		if (g.installed)
		{
			std::lock_guard<std::mutex> glock(gen.mutex);
			std::snprintf(line, sizeof(line), "frame generation: %s; %u frames prepared, %u made between, %u shown twice, %u failed%s%s",
				!gen.made ? "contexts not made" : gen.apart ? "on, the UI kept apart" : "on, the UI moved with the world (FSR 2's picture isn't the screen's size)", gen.prepares, gen.generated, gen.repeated, gen.failed, gen.why.empty() ? "" : ": ", gen.why.c_str());
			lines.push_back(line);
		}
		return lines;
	}
}
