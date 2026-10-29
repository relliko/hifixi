/*
 * The FidelityFX SDK's shaders for its DX12 backend (ffx/src/backends/shared/ffx_shader_blobs.h), in place of AMD's
 * generated tables: frame interpolation's, optical flow's and the FSR 3.1 upscaler's, every pass and permutation compiled with DXC when HiFi XI
 * is built (tools/ffx_sc: build/gen/fg_shaders.inc), each with its bindings. Never wave64 or 16-bit (the shaders are
 * the same whatever those option bits say).
 */

#include <FidelityFX/host/ffx_interface.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <ffx_shader_blobs.h>
#include <cstdint>
#include <cstring>

namespace
{
	struct shader_entry
	{
		FfxEffect effect;
		uint32_t pass;
		uint32_t options;
		FfxShaderBlob blob;
	};
#include "fg_shaders.inc"

	// The option bits each effect's shaders were made for (the rest select wave64 or 16-bit tables: none here)
	uint32_t options_of(FfxEffect effect, uint32_t options)
	{
		switch (effect)
		{
		case FFX_EFFECT_FRAMEINTERPOLATION:
			return options & 7u; // (low-res motion vectors 1, jittered motion vectors 2, inverted depth 4)
		case FFX_EFFECT_OPTICALFLOW:
			return options & 4u; // (HDR colour input 4)
		case FFX_EFFECT_FSR3UPSCALER:
			return options & 63u; // (Lanczos table 1, HDR 2, low-res motion vectors 4, jittered 8, inverted depth 16, sharpening 32)
		default:
			return options;
		}
	}
}

FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effectId, FfxPass passId, FfxBindStage, uint32_t permutationOptions, FfxShaderBlob *outBlob)
{
	const uint32_t options = options_of(effectId, permutationOptions);
	for (const shader_entry &e : kShaderEntries)
		if (e.effect == effectId && e.pass == uint32_t(passId) && e.options == options)
		{
			std::memcpy(outBlob, &e.blob, sizeof(FfxShaderBlob));
			return FFX_OK;
		}
	std::memset(outBlob, 0, sizeof(FfxShaderBlob));
	return FFX_ERROR_INVALID_ARGUMENT;
}

FfxErrorCode ffxIsWave64(FfxEffect, uint32_t, bool &isWave64)
{
	isWave64 = false;
	return FFX_OK;
}
