/*
 * HiFi XI's shader tool for AMD's frame generation (src/framegen): every pass and permutation of the FidelityFX SDK's
 * frame interpolation and optical flow compiled with Microsoft's DXC (the Windows SDK's dxcompiler.dll, signed; its
 * dxil.dll beside it signs the output), as AMD's own tool (FidelityFX_SC, unsigned: never run) would: cs_6_2, entry CS,
 * FFX_GPU, FFX_HLSL, FFX_HLSL_SM 62, each component's base defines, every combination of its permutation options, wave32,
 * no 16-bit types. Each one's bindings (constant buffers, SRV and UAV textures and buffers, samplers: names, registers,
 * counts, spaces) read back through DXC's reflection. Written as C++ for src/framegen/ffx_shader_blobs.cpp: the DXIL
 * bytes and an FfxShaderBlob for each (effect, pass, options).
 *
 *   ffx_sc.exe <dxcompiler.dll> <src/framegen/ffx> <out.inc>
 */

#include <windows.h>
#include <dxcapi.h>
#include <d3d12shader.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
	struct job
	{
		const char *effect, *pass, *file;
		const wchar_t *component; // (its folder under include/FidelityFX/gpu and src/backends/dx12/shaders)
		std::vector<const wchar_t *> base;   // its base defines
		std::vector<std::pair<const wchar_t *, uint32_t>> options; // its permutation defines and their option bits
	};

	const std::vector<const wchar_t *> kFiBase = { L"FFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
		L"FFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0", L"FFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
		L"FFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0", L"FFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2" };
	// (FRAMEINTERPOLATION_SHADER_PERMUTATION_*: low-res motion vectors 1, jittered motion vectors 2, inverted depth 4)
	const std::vector<std::pair<const wchar_t *, uint32_t>> kFiOptions = { { L"FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS", 1u },
		{ L"FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS", 2u }, { L"FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH", 4u } };
	// (OPTICALFLOW_HDR_COLOR_INPUT 4)
	const std::vector<std::pair<const wchar_t *, uint32_t>> kOfOptions = { { L"FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT", 4u } };

	std::vector<job> jobs()
	{
		std::vector<job> j;
		auto fi = [&](const char *pass, const char *file) { j.push_back({ "FFX_EFFECT_FRAMEINTERPOLATION", pass, file, L"frameinterpolation", kFiBase, kFiOptions }); };
		auto of = [&](const char *pass, const char *file) { j.push_back({ "FFX_EFFECT_OPTICALFLOW", pass, file, L"opticalflow", {}, kOfOptions }); };
		fi("FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_AND_DILATE", "ffx_frameinterpolation_reconstruct_and_dilate_pass.hlsl");
		fi("FFX_FRAMEINTERPOLATION_PASS_SETUP", "ffx_frameinterpolation_setup_pass.hlsl");
		fi("FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_PREV_DEPTH", "ffx_frameinterpolation_reconstruct_previous_depth_pass.hlsl");
		fi("FFX_FRAMEINTERPOLATION_PASS_GAME_MOTION_VECTOR_FIELD", "ffx_frameinterpolation_game_motion_vector_field_pass.hlsl");
		fi("FFX_FRAMEINTERPOLATION_PASS_OPTICAL_FLOW_VECTOR_FIELD", "ffx_frameinterpolation_optical_flow_vector_field_pass.hlsl");
		fi("FFX_FRAMEINTERPOLATION_PASS_DISOCCLUSION_MASK", "ffx_frameinterpolation_disocclusion_mask_pass.hlsl");
		fi("FFX_FRAMEINTERPOLATION_PASS_INTERPOLATION", "ffx_frameinterpolation_pass.hlsl");
		fi("FFX_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID", "ffx_frameinterpolation_compute_inpainting_pyramid_pass.hlsl");
		fi("FFX_FRAMEINTERPOLATION_PASS_INPAINTING", "ffx_frameinterpolation_inpainting_pass.hlsl");
		fi("FFX_FRAMEINTERPOLATION_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID", "ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass.hlsl");
		fi("FFX_FRAMEINTERPOLATION_PASS_DEBUG_VIEW", "ffx_frameinterpolation_debug_view_pass.hlsl");
		of("FFX_OPTICALFLOW_PASS_PREPARE_LUMA", "ffx_opticalflow_prepare_luma_pass.hlsl");
		of("FFX_OPTICALFLOW_PASS_GENERATE_OPTICAL_FLOW_INPUT_PYRAMID", "ffx_opticalflow_compute_luminance_pyramid_pass.hlsl");
		of("FFX_OPTICALFLOW_PASS_GENERATE_SCD_HISTOGRAM", "ffx_opticalflow_generate_scd_histogram_pass.hlsl");
		of("FFX_OPTICALFLOW_PASS_COMPUTE_SCD_DIVERGENCE", "ffx_opticalflow_compute_scd_divergence_pass.hlsl");
		of("FFX_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5", "ffx_opticalflow_compute_optical_flow_advanced_pass_v5.hlsl");
		of("FFX_OPTICALFLOW_PASS_FILTER_OPTICAL_FLOW_V5", "ffx_opticalflow_filter_optical_flow_pass_v5.hlsl");
		of("FFX_OPTICALFLOW_PASS_SCALE_OPTICAL_FLOW_ADVANCED_V5", "ffx_opticalflow_scale_optical_flow_advanced_pass_v5.hlsl");
		return j;
	}

	std::wstring wide(const std::string &s)
	{
		return std::wstring(s.begin(), s.end());
	}

	// The FfxShaderBlob's seven kinds of binding, in its order
	enum kind { kCbv, kSrvTex, kUavTex, kSrvBuf, kUavBuf, kSampler, kAccel, kKinds };
	const char *const kKindNames[kKinds] = { "cb", "srvt", "uavt", "srvb", "uavb", "smp", "rt" };

	struct binding
	{
		std::string name;
		uint32_t reg, count, space;
	};

	int kind_of(const D3D12_SHADER_INPUT_BIND_DESC &b)
	{
		switch (b.Type)
		{
		case D3D_SIT_CBUFFER:
			return kCbv;
		case D3D_SIT_TBUFFER:
		case D3D_SIT_STRUCTURED:
		case D3D_SIT_BYTEADDRESS:
			return kSrvBuf;
		case D3D_SIT_TEXTURE:
			return b.Dimension == D3D_SRV_DIMENSION_BUFFER ? kSrvBuf : kSrvTex;
		case D3D_SIT_UAV_RWTYPED:
			return b.Dimension == D3D_SRV_DIMENSION_BUFFER ? kUavBuf : kUavTex;
		case D3D_SIT_UAV_RWSTRUCTURED:
		case D3D_SIT_UAV_RWBYTEADDRESS:
		case D3D_SIT_UAV_APPEND_STRUCTURED:
		case D3D_SIT_UAV_CONSUME_STRUCTURED:
		case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
			return kUavBuf;
		case D3D_SIT_SAMPLER:
			return kSampler;
		case D3D_SIT_RTACCELERATIONSTRUCTURE:
			return kAccel;
		default:
			return -1;
		}
	}
}

int wmain(int argc, wchar_t **argv)
{
	if (argc < 4)
	{
		std::printf("ffx_sc <dxcompiler.dll> <src/framegen/ffx> <out.inc>\n");
		return 2;
	}
	HMODULE dxc = LoadLibraryW(argv[1]);
	const auto create = dxc != nullptr ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxc, "DxcCreateInstance")) : nullptr;
	if (create == nullptr)
	{
		std::printf("ffx_sc: %ls couldn't be loaded\n", argv[1]);
		return 1;
	}
	IDxcCompiler3 *compiler = nullptr;
	IDxcUtils *utils = nullptr;
	if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) || FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))))
	{
		std::printf("ffx_sc: DXC's compiler couldn't be made\n");
		return 1;
	}
	IDxcIncludeHandler *includes = nullptr;
	utils->CreateDefaultIncludeHandler(&includes);
	const std::wstring root = argv[2];
	const std::wstring gpu = root + L"/include/FidelityFX/gpu";
	FILE *out = nullptr;
	if (_wfopen_s(&out, argv[3], L"wb") != 0 || out == nullptr)
	{
		std::printf("ffx_sc: %ls couldn't be written\n", argv[3]);
		return 1;
	}
	std::fprintf(out, "// Made by tools/ffx_sc from src/framegen/ffx's shaders with DXC: do not edit\n");
	std::string table;
	uint32_t n = 0, failed = 0;
	size_t bytes = 0;
	for (const job &jb : jobs())
	{
		const std::wstring file = root + L"/src/backends/dx12/shaders/" + jb.component + L"/" + wide(jb.file);
		IDxcBlobEncoding *source = nullptr;
		if (FAILED(utils->LoadFile(file.c_str(), nullptr, &source)))
		{
			std::printf("ffx_sc: %ls missing\n", file.c_str());
			return 1;
		}
		const uint32_t combos = 1u << jb.options.size();
		for (uint32_t c = 0; c < combos; ++c)
		{
			uint32_t options = 0;
			std::vector<std::wstring> defs = { L"FFX_GPU=1", L"FFX_HLSL=1", L"FFX_HLSL_SM=62" };
			for (const wchar_t *d : jb.base)
				defs.push_back(d);
			for (size_t k = 0; k < jb.options.size(); ++k)
			{
				const bool on = (c >> k) & 1u;
				defs.push_back(std::wstring(jb.options[k].first) + (on ? L"=1" : L"=0"));
				options |= on ? jb.options[k].second : 0u;
			}
			const std::wstring inc_component = gpu + L"/" + jb.component;
			std::vector<std::wstring> args = { file, L"-T", L"cs_6_2", L"-E", L"CS", L"-Wno-for-redefinition", L"-Wno-ambig-lit-shift",
				L"-O3", L"-Qstrip_debug", L"-Qstrip_reflect", L"-I", gpu, L"-I", inc_component };
			for (const std::wstring &d : defs)
			{
				args.push_back(L"-D");
				args.push_back(d);
			}
			std::vector<LPCWSTR> argp;
			for (const std::wstring &a : args)
				argp.push_back(a.c_str());
			const DxcBuffer buf = { source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_ACP };
			IDxcResult *result = nullptr;
			HRESULT status = E_FAIL;
			if (FAILED(compiler->Compile(&buf, argp.data(), UINT32(argp.size()), includes, IID_PPV_ARGS(&result))) || FAILED(result->GetStatus(&status)) || FAILED(status))
			{
				IDxcBlobUtf8 *errors = nullptr;
				if (result != nullptr)
					result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
				std::printf("ffx_sc: %s (options %u) didn't compile:\n%s\n", jb.file, options, errors != nullptr ? errors->GetStringPointer() : "?");
				failed++;
				continue;
			}
			IDxcBlob *code = nullptr, *refl = nullptr;
			result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr);
			result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&refl), nullptr);
			ID3D12ShaderReflection *reflection = nullptr;
			if (refl != nullptr)
			{
				const DxcBuffer rb = { refl->GetBufferPointer(), refl->GetBufferSize(), 0 };
				utils->CreateReflection(&rb, IID_PPV_ARGS(&reflection));
			}
			if (code == nullptr || reflection == nullptr)
			{
				std::printf("ffx_sc: %s (options %u): no code or reflection\n", jb.file, options);
				failed++;
				continue;
			}
			std::vector<binding> b[kKinds];
			D3D12_SHADER_DESC sd = {};
			reflection->GetDesc(&sd);
			for (UINT i = 0; i < sd.BoundResources; ++i)
			{
				D3D12_SHADER_INPUT_BIND_DESC d = {};
				if (FAILED(reflection->GetResourceBindingDesc(i, &d)))
					continue;
				const int k = kind_of(d);
				if (k >= 0)
					b[k].push_back({ d.Name, d.BindPoint, d.BindCount, d.Space });
			}
			// The bytes, then each kind's names, registers, counts and spaces
			std::fprintf(out, "static const uint8_t g_dxil_%u[] = {", n);
			const uint8_t *p = static_cast<const uint8_t *>(code->GetBufferPointer());
			for (size_t i = 0; i < code->GetBufferSize(); ++i)
				std::fprintf(out, "%s%u", i == 0 ? "\n" : i % 40 == 0 ? ",\n" : ",", p[i]);
			std::fprintf(out, " };\n");
			bytes += code->GetBufferSize();
			for (int k = 0; k < kKinds; ++k)
			{
				if (b[k].empty())
					continue;
				std::fprintf(out, "static const char *g_%s_names_%u[] = {", kKindNames[k], n);
				for (const binding &x : b[k])
					std::fprintf(out, " \"%s\",", x.name.c_str());
				std::fprintf(out, " };\n");
				const char *const field[3] = { "reg", "count", "space" };
				for (int f = 0; f < 3; ++f)
				{
					std::fprintf(out, "static const uint32_t g_%s_%s_%u[] = {", kKindNames[k], field[f], n);
					for (const binding &x : b[k])
						std::fprintf(out, " %u,", f == 0 ? x.reg : f == 1 ? x.count : x.space);
					std::fprintf(out, " };\n");
				}
			}
			char head[200];
			std::snprintf(head, sizeof(head), "\t{ %s, %s, %uu, { g_dxil_%u, uint32_t(sizeof(g_dxil_%u))", jb.effect, jb.pass, options, n, n);
			table += head;
			for (int k = 0; k < kKinds; ++k)
				table += ", " + std::to_string(b[k].size()) + "u";
			for (int k = 0; k < kKinds; ++k)
			{
				const std::string s = std::to_string(n);
				if (b[k].empty())
					table += ", nullptr, nullptr, nullptr, nullptr";
				else
					table += std::string(", g_") + kKindNames[k] + "_names_" + s + ", g_" + kKindNames[k] + "_reg_" + s + ", g_" + kKindNames[k] + "_count_" + s +
						", g_" + kKindNames[k] + "_space_" + s;
			}
			table += " } },\n";
			n++;
			reflection->Release();
			refl->Release();
			code->Release();
			result->Release();
		}
		source->Release();
	}
	std::fprintf(out, "static const shader_entry kShaderEntries[] = {\n%s};\n", table.c_str());
	std::fclose(out);
	std::printf("ffx_sc: %u shaders (%zu kB of DXIL), %u failed\n", n, bytes / 1024, failed);
	return failed == 0 ? 0 : 1;
}
