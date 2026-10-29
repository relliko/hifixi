/*
 * FSR 2's shaders for its DX12 backend (fsr2/dx12/shaders/ffx_fsr2_shaders_dx12.h), in place of AMD's generated tables:
 * each pass and permutation compiled when first asked for, from the HLSL in fsr2/shaders (built in: build/gen
 * fsr2_shaders.inc), with d3dcompiler_47 (cs_5_1), and its bindings read back with D3DReflect: the names and registers
 * of its SRVs, UAVs and constant buffers, as AMD's tool writes them.
 *
 * The defines are AMD's build's (src/ffx-fsr2-api/CMakeLists.txt): FFX_GPU, FFX_HLSL, the samplers' data halves, the
 * upsampler's Lanczos approximation (type 2), and the permutation's six options. Never 16-bit (FFX_HALF) or wave64:
 * shader model 5 has neither; SPD's wave operations (the luminance pyramid) are left out for its shared-memory path
 * (SPD_NO_WAVE_OPERATIONS). Kept for the process's life (a handful of passes, a few hundred kB).
 */

#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include "fsr2/dx12/shaders/ffx_fsr2_shaders_dx12.h"
#include "fsr2_shaders.hpp"
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	struct file
	{
		const char *name;
		const char *text;
	};
	const file kFiles[] = {
#include "fsr2_shaders.inc"
	};

	const char *source(const char *name)
	{
		const char *slash = std::strrchr(name, '/');
		const char *back = std::strrchr(name, '\\');
		const char *base = slash != nullptr && slash > (back != nullptr ? back : name) ? slash + 1 : back != nullptr ? back + 1 : name;
		for (const file &f : kFiles)
			if (std::strcmp(f.name, base) == 0)
				return f.text;
		return nullptr;
	}

	struct includes final : ID3DInclude
	{
		HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID *data, UINT *bytes) override
		{
			const char *text = source(name);
			if (text == nullptr)
				return E_FAIL;
			*data = text;
			*bytes = UINT(std::strlen(text));
			return S_OK;
		}
		HRESULT __stdcall Close(LPCVOID) override { return S_OK; }
	};

	// One pass's permutation, compiled: its bytecode and its bindings (SRVs, UAVs, constant buffers) as the blob points at them
	struct compiled
	{
		std::vector<uint8_t> code;
		std::vector<std::string> names[3];
		std::vector<const char *> name_ptrs[3];
		std::vector<uint32_t> slots[3];
	};

	std::mutex g_mutex;
	std::map<uint64_t, std::unique_ptr<compiled>> g_cache;
	std::string g_error;
	uint32_t g_compiled = 0;
	double g_ms = 0.0;

	const char *pass_file(FfxFsr2Pass pass)
	{
		switch (pass)
		{
		case FFX_FSR2_PASS_DEPTH_CLIP:
			return "ffx_fsr2_depth_clip_pass.hlsl";
		case FFX_FSR2_PASS_RECONSTRUCT_PREVIOUS_DEPTH:
			return "ffx_fsr2_reconstruct_previous_depth_pass.hlsl";
		case FFX_FSR2_PASS_LOCK:
			return "ffx_fsr2_lock_pass.hlsl";
		case FFX_FSR2_PASS_ACCUMULATE:
		case FFX_FSR2_PASS_ACCUMULATE_SHARPEN:
			return "ffx_fsr2_accumulate_pass.hlsl";
		case FFX_FSR2_PASS_RCAS:
			return "ffx_fsr2_rcas_pass.hlsl";
		case FFX_FSR2_PASS_COMPUTE_LUMINANCE_PYRAMID:
			return "ffx_fsr2_compute_luminance_pyramid_pass.hlsl";
		case FFX_FSR2_PASS_GENERATE_REACTIVE:
			return "ffx_fsr2_autogen_reactive_pass.hlsl";
		case FFX_FSR2_PASS_TCR_AUTOGENERATE:
			return "ffx_fsr2_tcr_autogen_pass.hlsl";
		default:
			return nullptr;
		}
	}

	compiled *compile(FfxFsr2Pass pass, uint32_t options)
	{
		const char *name = pass_file(pass);
		const char *text = name != nullptr ? source(name) : nullptr;
		if (text == nullptr)
		{
			g_error = "no shader for FSR 2's pass " + std::to_string(int(pass));
			return nullptr;
		}
		using compile_fn = decltype(&D3DCompile);
		using reflect_fn = decltype(&D3DReflect);
		static HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
		const auto compile_call = compiler != nullptr ? reinterpret_cast<compile_fn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
		const auto reflect_call = compiler != nullptr ? reinterpret_cast<reflect_fn>(GetProcAddress(compiler, "D3DReflect")) : nullptr;
		if (compile_call == nullptr || reflect_call == nullptr)
		{
			g_error = "d3dcompiler_47.dll isn't there";
			return nullptr;
		}
		auto flag = [&](uint32_t bit) { return (options & bit) != 0 ? "1" : "0"; };
		const D3D_SHADER_MACRO defines[] = {
			{ "FFX_GPU", "1" },
			{ "FFX_HLSL", "1" },
			{ "SPD_NO_WAVE_OPERATIONS", "1" },
			{ "FFX_FSR2_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF", "0" },
			{ "FFX_FSR2_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF", "0" },
			{ "FFX_FSR2_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF", "1" },
			{ "FFX_FSR2_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF", "0" },
			{ "FFX_FSR2_OPTION_UPSAMPLE_USE_LANCZOS_TYPE", "2" },
			{ "FFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE", flag(FSR2_SHADER_PERMUTATION_USE_LANCZOS_TYPE) },
			{ "FFX_FSR2_OPTION_HDR_COLOR_INPUT", flag(FSR2_SHADER_PERMUTATION_HDR_COLOR_INPUT) },
			{ "FFX_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS", flag(FSR2_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS) },
			{ "FFX_FSR2_OPTION_JITTERED_MOTION_VECTORS", flag(FSR2_SHADER_PERMUTATION_JITTER_MOTION_VECTORS) },
			{ "FFX_FSR2_OPTION_INVERTED_DEPTH", flag(FSR2_SHADER_PERMUTATION_DEPTH_INVERTED) },
			{ "FFX_FSR2_OPTION_APPLY_SHARPENING", flag(FSR2_SHADER_PERMUTATION_ENABLE_SHARPENING) },
			{ nullptr, nullptr },
		};
		includes inc;
		ID3DBlob *code = nullptr, *errors = nullptr;
		LARGE_INTEGER t0, t1, freq;
		QueryPerformanceCounter(&t0);
		const HRESULT hr = compile_call(text, std::strlen(text), name, defines, &inc, "CS", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		QueryPerformanceCounter(&t1);
		QueryPerformanceFrequency(&freq);
		g_ms += 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart);
		if (FAILED(hr))
		{
			g_error = std::string(name) + " didn't compile: " + (errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
			if (errors != nullptr)
				errors->Release();
			return nullptr;
		}
		if (errors != nullptr)
			errors->Release();
		auto c = std::make_unique<compiled>();
		const uint8_t *p = static_cast<const uint8_t *>(code->GetBufferPointer());
		c->code.assign(p, p + code->GetBufferSize());
		code->Release();
		ID3D11ShaderReflection *reflection = nullptr;
		if (FAILED(reflect_call(c->code.data(), c->code.size(), __uuidof(ID3D11ShaderReflection), reinterpret_cast<void **>(&reflection))) || reflection == nullptr)
		{
			g_error = std::string(name) + " couldn't be reflected";
			return nullptr;
		}
		D3D11_SHADER_DESC sd = {};
		reflection->GetDesc(&sd);
		for (UINT i = 0; i < sd.BoundResources; ++i)
		{
			D3D11_SHADER_INPUT_BIND_DESC b = {};
			if (FAILED(reflection->GetResourceBindingDesc(i, &b)))
				continue;
			int kind = -1; // 0 SRV, 1 UAV, 2 CBV (as the blob orders them below)
			switch (b.Type)
			{
			case D3D_SIT_TEXTURE:
			case D3D_SIT_STRUCTURED:
			case D3D_SIT_BYTEADDRESS:
			case D3D_SIT_TBUFFER:
				kind = 0;
				break;
			case D3D_SIT_UAV_RWTYPED:
			case D3D_SIT_UAV_RWSTRUCTURED:
			case D3D_SIT_UAV_RWBYTEADDRESS:
			case D3D_SIT_UAV_APPEND_STRUCTURED:
			case D3D_SIT_UAV_CONSUME_STRUCTURED:
			case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
				kind = 1;
				break;
			case D3D_SIT_CBUFFER:
				kind = 2;
				break;
			default:
				break; // (samplers: static in the backend's root signature)
			}
			if (kind < 0)
				continue;
			c->names[kind].push_back(b.Name);
			c->slots[kind].push_back(b.BindPoint);
		}
		reflection->Release();
		for (int k = 0; k < 3; ++k)
			for (const std::string &s : c->names[k])
				c->name_ptrs[k].push_back(s.c_str());
		g_compiled++;
		return c.release();
	}
}

Fsr2ShaderBlobDX12 fsr2GetPermutationBlobByIndexDX12(FfxFsr2Pass passId, uint32_t permutationOptions)
{
	// (16-bit and wave64 never: the same shader whatever those bits say)
	const uint32_t options = permutationOptions & ~uint32_t(FSR2_SHADER_PERMUTATION_FORCE_WAVE64 | FSR2_SHADER_PERMUTATION_ALLOW_FP16);
	const FfxFsr2Pass file_pass = passId == FFX_FSR2_PASS_ACCUMULATE_SHARPEN ? FFX_FSR2_PASS_ACCUMULATE : passId;
	const uint64_t key = (uint64_t(file_pass) << 32) | options;
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_cache.find(key);
	if (it == g_cache.end())
	{
		compiled *c = compile(file_pass, options);
		if (c == nullptr)
			return Fsr2ShaderBlobDX12 {};
		it = g_cache.emplace(key, std::unique_ptr<compiled>(c)).first;
	}
	const compiled &c = *it->second;
	Fsr2ShaderBlobDX12 b = {};
	b.data = c.code.data();
	b.size = uint32_t(c.code.size());
	b.srvCount = uint32_t(c.slots[0].size());
	b.uavCount = uint32_t(c.slots[1].size());
	b.cbvCount = uint32_t(c.slots[2].size());
	b.boundSRVResourceNames = const_cast<const char **>(c.name_ptrs[0].data());
	b.boundSRVResources = c.slots[0].data();
	b.boundUAVResourceNames = const_cast<const char **>(c.name_ptrs[1].data());
	b.boundUAVResources = c.slots[1].data();
	b.boundCBVResourceNames = const_cast<const char **>(c.name_ptrs[2].data());
	b.boundCBVResources = c.slots[2].data();
	return b;
}

namespace hifi::upscale::fsr2_shaders
{
	std::string last_error()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_error;
	}

	void stats(uint32_t &compiled_count, double &ms)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		compiled_count = g_compiled;
		ms = g_ms;
	}
}
