#pragma once

// HiFi XI's own d3d8.dll (docs/own-frame/d3d8.md): the game's Direct3D 8 turned into Direct3D 12 on the game's own thread,
// in place of dgVoodoo. What all of it shares: the headers (Ashita's Direct3D 8 SDK headers beside Windows' Direct3D 12),
// the log, small helpers and a COM base.

#include <windows.h>
#include <d3d8.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

namespace hifi::d3d8
{
	// The log: d3d8.log beside this DLL (or D3D8_LOG), started afresh each run. level 0 info, 1 warning, 2 error.
	void log(int level, const char *format, ...);
	// Once per distinct text (unsupported features: said once, not every frame)
	void log_once(int level, const char *format, ...);

	inline uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
	inline uint64_t align_up64(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

	inline float as_float(DWORD v)
	{
		float f;
		std::memcpy(&f, &v, 4);
		return f;
	}

	// FNV-1a over bytes (keys of shaders and pipelines)
	inline uint64_t hash_bytes(const void *data, size_t n, uint64_t h = 1469598103934665603ull)
	{
		const uint8_t *p = static_cast<const uint8_t *>(data);
		for (size_t i = 0; i < n; ++i)
			h = (h ^ p[i]) * 1099511628211ull;
		return h;
	}

	template <typename T>
	void release(T *&p)
	{
		if (p != nullptr)
		{
			p->Release();
			p = nullptr;
		}
	}

	// A plain reference count for the COM objects handed to the game
	struct refcount
	{
		std::atomic<ULONG> refs { 1 };
		ULONG add() { return ++refs; }
		ULONG sub() { return --refs; }
	};

	class device;
	class direct3d;
	// (d3d8.cpp: the IDirect3D8 object's parts the device uses)
	IDXGIFactory2 *factory_of(direct3d *d);
	IDXGIAdapter1 *adapter_of(direct3d *d, UINT adapter);
	bool debug_of(direct3d *d);
	IDirect3D8 *interface_of(direct3d *d);
	std::wstring dll_folder();
	std::wstring env_value(const wchar_t *name);
	void fill_caps(D3DCAPS8 &c, UINT adapter);

	class base_texture;
	class texture;
	class cube_texture;
	class volume_texture;
	class surface;
	class volume;
	class vertex_buffer;
	class index_buffer;
	class swap_chain;
}
