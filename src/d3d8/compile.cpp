// HLSL compiled with Windows' own d3dcompiler_47.dll (loaded when first needed), each text once: kept in memory by its
// hash, and in the cache folder as <hash>.<target>.bin so the next start doesn't compile again (a few milliseconds each:
// the fixed function makes a few hundred over a session).

#include "shaders.hpp"
#include <d3dcommon.h>
#include <chrono>
#include <cstdio>
#include <unordered_map>

namespace hifi::d3d8
{
	namespace
	{
		using compile_fn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
		compile_fn d3dcompile = nullptr;
		bool tried = false;
		std::wstring folder;
		bool folder_made = false;
		std::unordered_map<uint64_t, std::vector<uint8_t>> cache;
		compile_stats stats;

		std::wstring file_of(uint64_t h, const char *target)
		{
			wchar_t name[64];
			swprintf_s(name, L"%016llx.%hs.bin", static_cast<unsigned long long>(h), target);
			return folder + name;
		}
	}

	void set_shader_cache_folder(const std::wstring &f)
	{
		folder = f;
		if (!folder.empty() && folder.back() != L'\\' && folder.back() != L'/')
			folder += L'\\';
		folder_made = false;
	}

	compile_stats shader_stats() { return stats; }

	const std::vector<uint8_t> &compile(const std::string &hlsl, const char *target, std::string &why)
	{
		static const std::vector<uint8_t> none;
		const uint64_t h = hash_bytes(target, std::strlen(target), hash_bytes(hlsl.data(), hlsl.size()));
		const auto it = cache.find(h);
		if (it != cache.end())
			return it->second;

		// From the folder
		if (!folder.empty())
		{
			FILE *f = nullptr;
			if (_wfopen_s(&f, file_of(h, target).c_str(), L"rb") == 0 && f != nullptr)
			{
				std::vector<uint8_t> blob;
				std::fseek(f, 0, SEEK_END);
				const long n = std::ftell(f);
				std::fseek(f, 0, SEEK_SET);
				if (n > 0)
				{
					blob.resize(size_t(n));
					if (std::fread(blob.data(), 1, blob.size(), f) != blob.size())
						blob.clear();
				}
				std::fclose(f);
				if (!blob.empty())
				{
					stats.from_disk++;
					return cache[h] = std::move(blob);
				}
			}
		}

		if (!tried)
		{
			tried = true;
			if (HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll"))
				d3dcompile = reinterpret_cast<compile_fn>(GetProcAddress(m, "D3DCompile"));
			if (d3dcompile == nullptr)
				log(2, "d3dcompiler_47.dll isn't there: no shaders can be made");
		}
		if (d3dcompile == nullptr)
		{
			why = "no d3dcompiler_47.dll";
			return none;
		}
		const auto t0 = std::chrono::steady_clock::now();
		ID3DBlob *code = nullptr, *errors = nullptr;
		const HRESULT hr = d3dcompile(hlsl.data(), hlsl.size(), "d3d8", nullptr, nullptr, "main", target, (1u << 15) /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, &code, &errors);
		stats.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if (FAILED(hr) || code == nullptr)
		{
			why = errors != nullptr ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()) : "D3DCompile failed";
			release(code);
			release(errors);
			stats.failed++;
			return none;
		}
		release(errors);
		std::vector<uint8_t> blob(static_cast<const uint8_t *>(code->GetBufferPointer()), static_cast<const uint8_t *>(code->GetBufferPointer()) + code->GetBufferSize());
		code->Release();
		stats.compiled++;
		if (!folder.empty())
		{
			if (!folder_made)
			{
				CreateDirectoryW(folder.c_str(), nullptr);
				folder_made = true;
			}
			FILE *f = nullptr;
			if (_wfopen_s(&f, file_of(h, target).c_str(), L"wb") == 0 && f != nullptr)
			{
				std::fwrite(blob.data(), 1, blob.size(), f);
				std::fclose(f);
			}
		}
		return cache[h] = std::move(blob);
	}
}
