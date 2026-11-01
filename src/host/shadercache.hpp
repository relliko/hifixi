#pragma once

// HiFi XI's own shaders kept compiled on disk (bootloader\hifixi-shader-cache): the zone, characters, effects, world,
// 2D, FSR's motion and the upscale passes compile on the game's thread when their renderer is made, which froze the
// title screen for seconds at every start. compile() takes D3DCompile's arguments: a shader met before (the same source,
// entry, target, flags and defines, from this very build of HiFi XI) is read back from its file instead; one compiled
// is written for the next start. Without a folder (set_folder: the offline tests) it's D3DCompile as it is.
//
// The key holds the build's own stamp (its image's time stamp) because an include handler's files aren't hashed: a new
// build compiles everything once. Files are '<key>.bin': 'HXSC', the key, the code's size, the code.

#include <windows.h>
#include <d3dcompiler.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace hifi::shadercache
{
	using compile_fn = decltype(&D3DCompile);

	struct counts
	{
		std::atomic<uint32_t> compiled { 0 }, read { 0 }, failed { 0 };
		std::atomic<uint64_t> compile_us { 0 }, read_us { 0 };
	};
	inline counts &stats()
	{
		static counts c;
		return c;
	}
	inline std::wstring &folder_ref()
	{
		static std::wstring f;
		return f;
	}
	inline std::mutex &folder_mutex()
	{
		static std::mutex m;
		return m;
	}

	// Where the files go (with a trailing slash; empty: no cache). Made when it isn't there.
	inline void set_folder(const std::wstring &folder)
	{
		std::lock_guard<std::mutex> lock(folder_mutex());
		folder_ref() = folder;
		if (!folder.empty())
			CreateDirectoryW(folder.c_str(), nullptr);
	}

	namespace detail
	{
		inline uint64_t fnv(uint64_t h, const void *data, size_t size)
		{
			const uint8_t *p = static_cast<const uint8_t *>(data);
			for (size_t i = 0; i < size; ++i)
				h = (h ^ p[i]) * 1099511628211ull;
			return h;
		}
		inline uint64_t fnv(uint64_t h, const char *s)
		{
			return s != nullptr ? fnv(h, s, std::strlen(s) + 1) : fnv(h, "\x01", 1);
		}
		// This build's stamp: the time stamp of the image this code is in
		inline uint32_t build_stamp()
		{
			static const uint32_t stamp = [] {
				HMODULE self = nullptr;
				if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCWSTR>(&build_stamp), &self) || self == nullptr)
					return 0u;
				const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(self);
				const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const uint8_t *>(self) + dos->e_lfanew);
				return uint32_t(nt->FileHeader.TimeDateStamp);
			}();
			return stamp;
		}
		inline double now_us()
		{
			LARGE_INTEGER f, c;
			QueryPerformanceFrequency(&f);
			QueryPerformanceCounter(&c);
			return double(c.QuadPart) * 1e6 / double(f.QuadPart);
		}
		using create_blob_fn = HRESULT(WINAPI *)(SIZE_T, ID3DBlob **);
		inline create_blob_fn create_blob()
		{
			static const create_blob_fn fn = [] {
				HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
				return compiler != nullptr ? reinterpret_cast<create_blob_fn>(GetProcAddress(compiler, "D3DCreateBlob")) : nullptr;
			}();
			return fn;
		}
		constexpr uint32_t kMagic = 0x43535848; // 'HXSC'
	}

	// D3DCompile through the cache: the same arguments, the same answers (errors only when it was compiled now and failed)
	inline HRESULT compile(compile_fn fn, const void *source, size_t size, const char *name, const D3D_SHADER_MACRO *defines, ID3DInclude *include,
		const char *entry, const char *target, UINT flags1, UINT flags2, ID3DBlob **code, ID3DBlob **errors)
	{
		std::wstring folder;
		{
			std::lock_guard<std::mutex> lock(folder_mutex());
			folder = folder_ref();
		}
		using namespace detail;
		uint64_t key = 1469598103934665603ull;
		const uint32_t stamp = build_stamp();
		key = fnv(key, &stamp, sizeof(stamp));
		key = fnv(key, source, size);
		key = fnv(key, entry);
		key = fnv(key, target);
		key = fnv(key, &flags1, sizeof(flags1));
		key = fnv(key, &flags2, sizeof(flags2));
		for (const D3D_SHADER_MACRO *d = defines; d != nullptr && d->Name != nullptr; ++d)
		{
			key = fnv(key, d->Name);
			key = fnv(key, d->Definition);
		}
		wchar_t file_name[32];
		swprintf_s(file_name, L"%016llx.bin", static_cast<unsigned long long>(key));
		const std::wstring path = folder + file_name;
		const double t0 = now_us();
		if (!folder.empty() && create_blob() != nullptr)
		{
			FILE *f = nullptr;
			if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f != nullptr)
			{
				uint32_t head[2] = {};
				uint64_t got_key = 0;
				bool ok = std::fread(head, 4, 1, f) == 1 && head[0] == kMagic && std::fread(&got_key, 8, 1, f) == 1 && got_key == key &&
					std::fread(&head[1], 4, 1, f) == 1 && head[1] != 0 && head[1] < (64u << 20);
				ID3DBlob *blob = nullptr;
				ok = ok && SUCCEEDED(create_blob()(head[1], &blob)) && blob != nullptr && std::fread(blob->GetBufferPointer(), 1, head[1], f) == head[1];
				std::fclose(f);
				if (ok)
				{
					*code = blob;
					if (errors != nullptr)
						*errors = nullptr;
					stats().read++;
					stats().read_us += uint64_t(now_us() - t0);
					return S_OK;
				}
				if (blob != nullptr)
					blob->Release();
			}
		}
		const HRESULT hr = fn(source, size, name, defines, include, entry, target, flags1, flags2, code, errors);
		stats().compile_us += uint64_t(now_us() - t0);
		if (FAILED(hr) || *code == nullptr)
		{
			stats().failed++;
			return hr;
		}
		stats().compiled++;
		if (!folder.empty())
		{
			// (written whole under another name, then renamed: a half-written file is never read)
			const std::wstring part = path + L".part";
			FILE *f = nullptr;
			if (_wfopen_s(&f, part.c_str(), L"wb") == 0 && f != nullptr)
			{
				const uint32_t magic = kMagic, n = uint32_t((*code)->GetBufferSize());
				const bool ok = std::fwrite(&magic, 4, 1, f) == 1 && std::fwrite(&key, 8, 1, f) == 1 && std::fwrite(&n, 4, 1, f) == 1 &&
					std::fwrite((*code)->GetBufferPointer(), 1, n, f) == n;
				std::fclose(f);
				if (!ok || !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
					DeleteFileW(part.c_str());
			}
		}
		return hr;
	}

	// (status) "N compiled in X ms, M read back from the cache in Y ms"
	inline std::string status()
	{
		char line[200];
		std::snprintf(line, sizeof(line), "shaders: %u compiled (%.0f ms), %u read from the cache (%.0f ms)%s", stats().compiled.load(),
			stats().compile_us.load() / 1000.0, stats().read.load(), stats().read_us.load() / 1000.0,
			stats().failed.load() != 0 ? (", " + std::to_string(stats().failed.load()) + " failed").c_str() : "");
		return line;
	}
}
