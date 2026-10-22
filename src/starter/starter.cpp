/*
 * HiFi XI's starter: bootloader\dxgi.dll. dgVoodoo loads dxgi.dll from the game's folder first (ReShade's dxgi.dll got
 * in the same way). This one hands every call on to Windows' own dxgi.dll, and the first time a DXGI factory is asked
 * for (dgVoodoo does before it makes its Direct3D 12 device) it loads HiFi XI and starts it: hifixi_early_start, with
 * Windows' CreateDXGIFactory1. HiFi XI then holds Direct3D 12 from before the game's device is made, so it follows
 * every command list and view from its making, and can turn on the GPU crash record. The HiFi XI it loads is
 * plugins\hifixi.dll (or hifixi.dll next to this, for the tests): the very module Ashita loads as a plugin later, so
 * the plugin finds itself started.
 *
 * x86 only (as the game). Each export is a thunk jumping to Windows' function of the same name, so the arguments and
 * the calling convention pass through untouched.
 */

#include <windows.h>
#include <atomic>

extern "C"
{
	// Windows' dxgi.dll functions, in the order of the names below (the .def's ordinals)
	FARPROC starter_procs[20] = {};
	void __cdecl starter_resolve();
	void __cdecl starter_resolve_start();
}

namespace
{
	const char *const kNames[20] = {
		"ApplyCompatResolutionQuirking", "CompatString", "CompatValue", "DXGIDumpJournal", "PIXBeginCapture", "PIXEndCapture",
		"PIXGetCaptureState", "SetAppCompatStringPointer", "UpdateHMDEmulationStatus", "CreateDXGIFactory1", "CreateDXGIFactory2",
		"CreateDXGIFactory", "DXGID3D10CreateDevice", "DXGID3D10CreateLayeredDevice", "DXGID3D10GetLayeredDeviceSize",
		"DXGID3D10RegisterLayers", "DXGIDeclareAdapterRemovalSupport", "DXGIDisableVBlankVirtualization", "DXGIGetDebugInterface1",
		"DXGIReportAdapterConfiguration",
	};
	constexpr int kCreateFactory1 = 9;

	SRWLOCK resolve_lock = SRWLOCK_INIT;
	std::atomic<bool> resolved { false };
	// 0: not started; 1: starting (on start_thread); 2: done (started or not)
	std::atomic<int> start_state { 0 };
	std::atomic<DWORD> start_thread { 0 };

	bool exists(const wchar_t *path)
	{
		const DWORD a = GetFileAttributesW(path);
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	// HiFi XI: next to this (the tests), else the game's plugins folder beside the bootloader folder this is in
	HMODULE load_hifixi()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&load_hifixi), &self);
		wchar_t dir[MAX_PATH] = L"";
		const DWORD n = GetModuleFileNameW(self, dir, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
			return nullptr;
		wchar_t *slash = wcsrchr(dir, L'\\');
		if (slash == nullptr)
			return nullptr;
		slash[1] = 0;
		const wchar_t *const tails[] = { L"hifixi.dll", L"..\\plugins\\hifixi.dll" };
		for (const wchar_t *tail : tails)
		{
			wchar_t joined[MAX_PATH * 2], full[MAX_PATH * 2];
			wcscpy_s(joined, dir);
			wcscat_s(joined, tail);
			if (GetFullPathNameW(joined, MAX_PATH * 2, full, nullptr) == 0 || !exists(full))
				continue;
			if (HMODULE m = LoadLibraryW(full))
				return m;
		}
		return nullptr;
	}
}

// Windows' dxgi.dll and its functions, once (System32: SysWOW64 for this 32-bit process)
extern "C" void __cdecl starter_resolve()
{
	if (resolved.load(std::memory_order_acquire))
		return;
	AcquireSRWLockExclusive(&resolve_lock);
	if (!resolved.load(std::memory_order_relaxed))
	{
		wchar_t path[MAX_PATH] = L"";
		const UINT n = GetSystemDirectoryW(path, MAX_PATH);
		if (n != 0 && n < MAX_PATH - 12)
		{
			wcscat_s(path, L"\\dxgi.dll");
			if (HMODULE real = LoadLibraryW(path))
				for (int i = 0; i < 20; ++i)
					starter_procs[i] = GetProcAddress(real, kNames[i]);
		}
		resolved.store(true, std::memory_order_release);
	}
	ReleaseSRWLockExclusive(&resolve_lock);
}

// The factories also start HiFi XI, once: the first caller does, others wait for it (a call from inside the start
// itself, Direct3D 12 asking DXGI for one, goes straight on)
extern "C" void __cdecl starter_resolve_start()
{
	starter_resolve();
	int expected = 0;
	if (start_state.compare_exchange_strong(expected, 1))
	{
		start_thread.store(GetCurrentThreadId());
		if (HMODULE hifixi = load_hifixi())
		{
			using start_fn = int(__cdecl *)(void *);
			if (const auto start = reinterpret_cast<start_fn>(GetProcAddress(hifixi, "hifixi_early_start")))
				start(reinterpret_cast<void *>(starter_procs[kCreateFactory1]));
		}
		start_state.store(2);
		return;
	}
	while (start_state.load() == 1 && start_thread.load() != GetCurrentThreadId())
		Sleep(1);
}

// The exports: each resolves Windows' function and jumps to it (the arguments untouched on the stack)
#define STARTER_THUNK(n, name, first) \
	extern "C" __declspec(naked) void thunk_##name() \
	{ \
		__asm { call first } \
		__asm { jmp dword ptr [starter_procs + 4 * n] } \
	}

STARTER_THUNK(0, ApplyCompatResolutionQuirking, starter_resolve)
STARTER_THUNK(1, CompatString, starter_resolve)
STARTER_THUNK(2, CompatValue, starter_resolve)
STARTER_THUNK(3, DXGIDumpJournal, starter_resolve)
STARTER_THUNK(4, PIXBeginCapture, starter_resolve)
STARTER_THUNK(5, PIXEndCapture, starter_resolve)
STARTER_THUNK(6, PIXGetCaptureState, starter_resolve)
STARTER_THUNK(7, SetAppCompatStringPointer, starter_resolve)
STARTER_THUNK(8, UpdateHMDEmulationStatus, starter_resolve)
STARTER_THUNK(9, CreateDXGIFactory1, starter_resolve_start)
STARTER_THUNK(10, CreateDXGIFactory2, starter_resolve_start)
STARTER_THUNK(11, CreateDXGIFactory, starter_resolve_start)
STARTER_THUNK(12, DXGID3D10CreateDevice, starter_resolve)
STARTER_THUNK(13, DXGID3D10CreateLayeredDevice, starter_resolve)
STARTER_THUNK(14, DXGID3D10GetLayeredDeviceSize, starter_resolve)
STARTER_THUNK(15, DXGID3D10RegisterLayers, starter_resolve)
STARTER_THUNK(16, DXGIDeclareAdapterRemovalSupport, starter_resolve)
STARTER_THUNK(17, DXGIDisableVBlankVirtualization, starter_resolve)
STARTER_THUNK(18, DXGIGetDebugInterface1, starter_resolve)
STARTER_THUNK(19, DXGIReportAdapterConfiguration, starter_resolve)
