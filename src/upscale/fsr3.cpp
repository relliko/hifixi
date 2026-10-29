/*
 * AMD FSR 3.1's upscaler for temporal.cpp (fsr3.hpp). As AMD's FSR 3 makes it (src/framegen/ffx: ffx_fsr3.cpp, not
 * built here): one DX12 backend interface for the upscaler and a shared-resources context holding the dilated depth,
 * dilated motion and the last frame's nearest depth it writes each frame (shared with frame generation in AMD's FSR 3;
 * HiFi XI's frame generation makes its own). Auto exposure, depth not inverted with its far plane at infinity (near 0.1:
 * the game's), motion vectors at the render size without the jitter (the permutations tools/ffx_sc compiles).
 */

#include <windows.h>
#include <d3d12.h>
#include "fsr3.hpp"
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <algorithm>
#include <vector>

namespace hifi::upscale::fsr3
{
	struct upscaler
	{
		std::vector<uint8_t> scratch;
		FfxInterface iface = {};
		FfxUInt32 shared_id = 0;
		FfxFsr3UpscalerContext ctx = {};
		enum { kPrevDepth, kDilatedDepth, kDilatedMotion, kShared };
		FfxResourceInternal shared[kShared] = {};
		int made = 0; // (how far its making got: 1 backend, 2 + shared resources' context, 3 + upscaler, 4 + resources)
		uint32_t rw = 0, rh = 0, dw = 0, dh = 0;
	};

	void destroy(upscaler *u)
	{
		if (u == nullptr)
			return;
		if (u->made >= 4)
			for (FfxResourceInternal &r : u->shared)
				u->iface.fpDestroyResource(&u->iface, r, u->shared_id);
		if (u->made >= 3)
			ffxFsr3UpscalerContextDestroy(&u->ctx);
		if (u->made >= 2)
			u->iface.fpDestroyBackendContext(&u->iface, u->shared_id);
		delete u;
	}

	upscaler *make(ID3D12Device *device, uint32_t render_w, uint32_t render_h, uint32_t display_w, uint32_t display_h, std::string &why)
	{
		upscaler *u = new upscaler();
		u->rw = render_w;
		u->rh = render_h;
		u->dw = display_w;
		u->dh = display_h;
		constexpr size_t kContexts = FFX_FSR3UPSCALER_CONTEXT_COUNT + 1;
		u->scratch.assign(ffxGetScratchMemorySizeDX12(kContexts), 0);
		FfxErrorCode e = ffxGetInterfaceDX12(&u->iface, ffxGetDeviceDX12(device), u->scratch.data(), u->scratch.size(), kContexts);
		if (e == FFX_OK)
			u->made = 1;
		if (e == FFX_OK && (e = u->iface.fpCreateBackendContext(&u->iface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &u->shared_id)) == FFX_OK)
			u->made = 2;
		if (e != FFX_OK)
		{
			why = "FSR 3.1's DX12 backend couldn't be set up (error " + std::to_string(int(e)) + ")";
			destroy(u);
			return nullptr;
		}
		FfxFsr3UpscalerContextDescription d = {};
		d.flags = FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE | FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE;
		d.maxRenderSize = { render_w, render_h };
		d.maxUpscaleSize = { display_w, display_h };
		d.backendInterface = u->iface;
		if ((e = ffxFsr3UpscalerContextCreate(&u->ctx, &d)) != FFX_OK)
		{
			why = "FSR 3.1's context couldn't be made (error " + std::to_string(int(e)) + ")";
			destroy(u);
			return nullptr;
		}
		u->made = 3;
		FfxFsr3UpscalerSharedResourceDescriptions s = {};
		ffxFsr3UpscalerGetSharedResourceDescriptions(&u->ctx, &s);
		const FfxCreateResourceDescription *descs[upscaler::kShared] = { &s.reconstructedPrevNearestDepth, &s.dilatedDepth, &s.dilatedMotionVectors };
		int i = 0;
		for (; i < upscaler::kShared && e == FFX_OK; ++i)
			e = u->iface.fpCreateResource(&u->iface, descs[i], u->shared_id, &u->shared[i]);
		if (e != FFX_OK)
		{
			for (int j = 0; j < i - 1; ++j)
				u->iface.fpDestroyResource(&u->iface, u->shared[j], u->shared_id);
			why = "FSR 3.1's shared resources couldn't be made (error " + std::to_string(int(e)) + ")";
			destroy(u);
			return nullptr;
		}
		u->made = 4;
		return u;
	}

	bool dispatch(upscaler *u, const frame &f, std::string &why)
	{
		if (u == nullptr || f.list == nullptr || f.color == nullptr || f.depth == nullptr || f.motion == nullptr || f.output == nullptr)
		{
			why = "FSR 3.1's inputs missing";
			return false;
		}
		auto res = [](ID3D12Resource *r, const wchar_t *name, FfxResourceStates state) {
			return r != nullptr ? ffxGetResourceDX12(r, ffxGetResourceDescriptionDX12(r), name, state) : ffxGetResourceDX12(nullptr, FfxResourceDescription{}, name, state);
		};
		FfxFsr3UpscalerDispatchDescription d = {};
		d.commandList = ffxGetCommandListDX12(f.list);
		d.color = res(f.color, L"hifixi: the world", FFX_RESOURCE_STATE_COMPUTE_READ);
		d.depth = res(f.depth, L"hifixi: FSR's depth", FFX_RESOURCE_STATE_COMPUTE_READ);
		d.motionVectors = res(f.motion, L"hifixi: FSR's motion", FFX_RESOURCE_STATE_COMPUTE_READ);
		d.exposure = res(nullptr, L"hifixi: no exposure", FFX_RESOURCE_STATE_COMPUTE_READ);
		d.reactive = res(f.reactive, L"hifixi: FSR's reactive mask", FFX_RESOURCE_STATE_COMPUTE_READ);
		d.transparencyAndComposition = res(f.composition, L"hifixi: FSR's composition mask", FFX_RESOURCE_STATE_COMPUTE_READ);
		d.reconstructedPrevNearestDepth = u->iface.fpGetResource(&u->iface, u->shared[upscaler::kPrevDepth]);
		d.dilatedDepth = u->iface.fpGetResource(&u->iface, u->shared[upscaler::kDilatedDepth]);
		d.dilatedMotionVectors = u->iface.fpGetResource(&u->iface, u->shared[upscaler::kDilatedMotion]);
		d.output = res(f.output, L"hifixi: FSR's picture", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
		d.jitterOffset = { f.jitter[0], f.jitter[1] };
		d.motionVectorScale = { float(u->rw), float(u->rh) };
		d.renderSize = { u->rw, u->rh };
		d.upscaleSize = { u->dw, u->dh };
		d.enableSharpening = f.sharpen > 0.0f;
		d.sharpness = std::clamp(f.sharpen, 0.0f, 1.0f);
		d.frameTimeDelta = std::clamp(f.dt_ms, 1.0f, 100.0f);
		d.preExposure = 1.0f;
		d.reset = f.reset;
		d.cameraNear = 0.1f;
		d.cameraFar = 65535.0f;
		d.cameraFovAngleVertical = f.fov;
		d.viewSpaceToMetersFactor = 1.0f;
		const FfxErrorCode e = ffxFsr3UpscalerContextDispatch(&u->ctx, &d);
		if (e != FFX_OK)
		{
			why = "FSR 3.1's dispatch failed (error " + std::to_string(int(e)) + ")";
			return false;
		}
		return true;
	}

	int32_t jitter_phases(uint32_t render_w, uint32_t display_w)
	{
		return ffxFsr3UpscalerGetJitterPhaseCount(int32_t(render_w), int32_t(display_w));
	}
}
