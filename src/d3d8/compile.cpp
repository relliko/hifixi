// HLSL compiled with Windows' own d3dcompiler_47.dll (loaded when first needed), each text once: kept in memory by its
// hash, and in the cache folder as <hash>.<target>.bin so the next start doesn't compile again (a few milliseconds each:
// the fixed function makes a few hundred over a session).
//
// Ahead of time: every shader key a draw meets (the fixed function's keys, a vs.1.1 / ps.1.x program with what shapes its
// HLSL) is written to keys.bin in the cache folder. At the device's making, worker threads read it and compile every key
// whose blob isn't in the folder, at below-normal priority: a DLL whose HLSL changed (an update) compiles its shaders there
// instead of in play, and a keys file shipped with the DLL (tests\d3d8_warm.cmd makes one from the scenes and the d3dlog
// replays) does the same for the first session. A draw that needs a shader a worker hasn't got to compiles it itself.

#include "shaders.hpp"
#include <d3dcommon.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>
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
		std::mutex mutex; // (the cache, the stats, the keys; never held while compiling)
		std::unordered_map<uint64_t, std::vector<uint8_t>> cache;
		compile_stats stats;
		std::set<std::vector<uint8_t>> keys_known;
		bool keys_loaded = false;
		constexpr char kKeysMagic[8] = { 'D', '3', 'D', '8', 'K', 'E', 'Y', '1' };

		std::wstring file_of(uint64_t h, const char *target)
		{
			wchar_t name[64];
			swprintf_s(name, L"%016llx.%hs.bin", static_cast<unsigned long long>(h), target);
			return folder + name;
		}

		bool read_file(const std::wstring &path, std::vector<uint8_t> &out)
		{
			FILE *f = nullptr;
			if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr)
				return false;
			std::fseek(f, 0, SEEK_END);
			const long n = std::ftell(f);
			std::fseek(f, 0, SEEK_SET);
			out.resize(n > 0 ? size_t(n) : 0);
			const bool ok = n > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
			std::fclose(f);
			return ok;
		}

		void make_folder()
		{
			if (!folder_made && !folder.empty())
			{
				CreateDirectoryW(folder.c_str(), nullptr);
				folder_made = true;
			}
		}

		// keys.bin: the magic, then records of [kind u8][length u16][bytes]
		void load_keys()
		{
			if (keys_loaded)
				return;
			keys_loaded = true;
			std::vector<uint8_t> b;
			if (folder.empty() || !read_file(folder + L"keys.bin", b) || b.size() < 8 || std::memcmp(b.data(), kKeysMagic, 8) != 0)
				return;
			for (size_t at = 8; at + 3 <= b.size();)
			{
				const size_t n = size_t(b[at + 1]) | size_t(b[at + 2]) << 8;
				if (at + 3 + n > b.size())
					break;
				keys_known.insert(std::vector<uint8_t>(b.begin() + ptrdiff_t(at), b.begin() + ptrdiff_t(at + 3 + n)));
				at += 3 + n;
			}
		}

		// The HLSL and target a key record stands for (false: not one this DLL reads)
		bool hlsl_of(const std::vector<uint8_t> &rec, std::string &hlsl, const char *&target)
		{
			const uint8_t kind = rec[0];
			const uint8_t *p = rec.data() + 3;
			const size_t n = rec.size() - 3;
			switch (kind)
			{
			case key_ff_vs:
			{
				if (n != sizeof(ff_vs_key))
					return false;
				ff_vs_key k;
				std::memcpy(&k, p, sizeof(k));
				hlsl = ff_vs_hlsl(k);
				target = "vs_5_0";
				return true;
			}
			case key_ff_ps:
			{
				if (n != sizeof(ff_ps_key))
					return false;
				ff_ps_key k;
				std::memcpy(&k, p, sizeof(k));
				hlsl = ff_ps_hlsl(k);
				target = "ps_5_0";
				return true;
			}
			case key_sm1_vs:
			{
				// inputs[16], clip planes, the tokens
				if (n < 17 + 8 || (n - 17) % 4 != 0)
					return false;
				sm1_shader s;
				std::vector<DWORD> tokens((n - 17) / 4);
				std::memcpy(tokens.data(), p + 17, n - 17);
				if (!read_sm1(tokens.data(), s) || s.pixel)
					return false;
				hlsl = sm1_vs_hlsl(s, p, p[16]);
				target = "vs_5_0";
				return !hlsl.empty();
			}
			case key_sm1_ps:
			{
				// tex[8], projected, tail (4), the tokens
				if (n < 13 + 8 || (n - 13) % 4 != 0)
					return false;
				sm1_shader s;
				std::vector<DWORD> tokens((n - 13) / 4);
				std::memcpy(tokens.data(), p + 13, n - 13);
				if (!read_sm1(tokens.data(), s) || !s.pixel)
					return false;
				ps_tail tail;
				std::memcpy(&tail, p + 9, sizeof(tail));
				hlsl = sm1_ps_hlsl(s, p, p[8], tail);
				target = "ps_5_0";
				return !hlsl.empty();
			}
			default:
				return false;
			}
		}
	}

	void set_shader_cache_folder(const std::wstring &f)
	{
		std::lock_guard<std::mutex> lock(mutex);
		folder = f;
		if (!folder.empty() && folder.back() != L'\\' && folder.back() != L'/')
			folder += L'\\';
		folder_made = false;
		keys_loaded = false;
		keys_known.clear();
	}

	compile_stats shader_stats()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return stats;
	}

	const std::vector<uint8_t> &compile(const std::string &hlsl, const char *target, std::string &why, bool ahead)
	{
		static const std::vector<uint8_t> none;
		const uint64_t h = hash_bytes(target, std::strlen(target), hash_bytes(hlsl.data(), hlsl.size()));
		std::wstring path;
		{
			std::lock_guard<std::mutex> lock(mutex);
			const auto it = cache.find(h);
			if (it != cache.end())
				return it->second;
			if (!folder.empty())
				path = file_of(h, target);
		}

		// From the folder
		std::vector<uint8_t> blob;
		if (!path.empty() && read_file(path, blob))
		{
			std::lock_guard<std::mutex> lock(mutex);
			stats.from_disk++;
			return cache.emplace(h, std::move(blob)).first->second;
		}

		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!tried)
			{
				tried = true;
				if (HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll"))
					d3dcompile = reinterpret_cast<compile_fn>(GetProcAddress(m, "D3DCompile"));
				if (d3dcompile == nullptr)
					log(2, "d3dcompiler_47.dll isn't there: no shaders can be made");
			}
		}
		if (d3dcompile == nullptr)
		{
			why = "no d3dcompiler_47.dll";
			return none;
		}
		const auto t0 = std::chrono::steady_clock::now();
		ID3DBlob *code = nullptr, *errors = nullptr;
		const HRESULT hr = d3dcompile(hlsl.data(), hlsl.size(), "d3d8", nullptr, nullptr, "main", target, (1u << 15) /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, &code, &errors);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if (FAILED(hr) || code == nullptr)
		{
			why = errors != nullptr ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()) : "D3DCompile failed";
			release(code);
			release(errors);
			std::lock_guard<std::mutex> lock(mutex);
			stats.failed++;
			return none;
		}
		release(errors);
		blob.assign(static_cast<const uint8_t *>(code->GetBufferPointer()), static_cast<const uint8_t *>(code->GetBufferPointer()) + code->GetBufferSize());
		code->Release();
		std::lock_guard<std::mutex> lock(mutex);
		if (ahead)
		{
			stats.ahead++;
			stats.ahead_ms += ms;
		}
		else
		{
			stats.compiled++;
			stats.ms += ms;
		}
		const auto it = cache.find(h);
		if (it != cache.end())
			return it->second; // (a worker and a draw both made it: the first kept)
		if (!folder.empty())
		{
			make_folder();
			FILE *f = nullptr;
			if (_wfopen_s(&f, file_of(h, target).c_str(), L"wb") == 0 && f != nullptr)
			{
				std::fwrite(blob.data(), 1, blob.size(), f);
				std::fclose(f);
			}
		}
		return cache.emplace(h, std::move(blob)).first->second;
	}

	void note_shader_key(uint8_t kind, const void *bytes, size_t n, const void *more, size_t more_n)
	{
		std::vector<uint8_t> rec(3 + n + more_n);
		rec[0] = kind;
		rec[1] = uint8_t((n + more_n) & 255);
		rec[2] = uint8_t((n + more_n) >> 8);
		std::memcpy(rec.data() + 3, bytes, n);
		if (more_n != 0)
			std::memcpy(rec.data() + 3 + n, more, more_n);
		std::lock_guard<std::mutex> lock(mutex);
		if (folder.empty() || n + more_n > 0xFFFF)
			return;
		load_keys();
		if (!keys_known.insert(rec).second)
			return;
		make_folder();
		const std::wstring path = folder + L"keys.bin";
		const bool fresh = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
		FILE *f = nullptr;
		if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || f == nullptr)
			return;
		if (fresh)
			std::fwrite(kKeysMagic, 1, 8, f);
		std::fwrite(rec.data(), 1, rec.size(), f);
		std::fclose(f);
	}

	uint32_t compile_keys_now()
	{
		std::vector<std::vector<uint8_t>> todo;
		{
			std::lock_guard<std::mutex> lock(mutex);
			load_keys();
			todo.assign(keys_known.begin(), keys_known.end());
		}
		uint32_t made = 0;
		for (const std::vector<uint8_t> &k : todo)
		{
			std::string hlsl, why;
			const char *target = nullptr;
			if (hlsl_of(k, hlsl, target) && !compile(hlsl, target, why, true).empty())
				made++;
		}
		return made;
	}

	void compile_ahead()
	{
		std::vector<std::vector<uint8_t>> todo;
		{
			std::lock_guard<std::mutex> lock(mutex);
			load_keys();
			todo.assign(keys_known.begin(), keys_known.end());
		}
		if (todo.empty())
			return;
		// Workers, below normal priority, each taking the next key; they finish on their own (nothing waits for them)
		struct work
		{
			std::vector<std::vector<uint8_t>> keys;
			std::atomic<size_t> next { 0 };
			std::atomic<uint32_t> left { 0 };
			std::chrono::steady_clock::time_point start;
		};
		auto *w = new work();
		w->keys = std::move(todo);
		w->start = std::chrono::steady_clock::now();
		const uint32_t threads = std::clamp<uint32_t>(std::thread::hardware_concurrency() / 2, 1, 4);
		w->left = threads;
		log(0, "compiling ahead: %zu shader keys known (keys.bin), on %u worker threads", w->keys.size(), threads);
		for (uint32_t t = 0; t < threads; ++t)
			std::thread([w] {
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
				for (size_t i = w->next++; i < w->keys.size(); i = w->next++)
				{
					std::string hlsl, why;
					const char *target = nullptr;
					if (hlsl_of(w->keys[i], hlsl, target))
						compile(hlsl, target, why, true);
				}
				if (--w->left == 0)
				{
					const compile_stats s = shader_stats();
					log(0, "compiled ahead: %u shaders in %.0f ms of compiling (%.0f ms after the device was made); %u read from the folder",
						s.ahead, s.ahead_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w->start).count(), s.from_disk);
					delete w;
				}
			}).detach();
	}
}
