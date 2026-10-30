#pragma once

// The game side's half of the direct channel (src/host/frameapi.hpp; no d3d12.h here: the game side's files include
// Ashita's Direct3D 8 headers): a pass's mark handed to HiFi XI's own d3d8.dll as a call. False when the d3d8.dll
// loaded isn't ours (or has no handler yet): the caller sends its stencil marker as before.

#include <windows.h>
#include <cstdint>

namespace hifi::game
{
	inline bool direct_mark(uint32_t kind, uint32_t seq)
	{
		using mark_fn = int(__cdecl *)(uint32_t, uint32_t);
		static mark_fn fn = nullptr;
		static bool looked = false;
		if (!looked)
		{
			looked = true;
			if (HMODULE d3d8 = GetModuleHandleW(L"d3d8.dll"))
				fn = reinterpret_cast<mark_fn>(GetProcAddress(d3d8, "HifiFrameMark"));
		}
		return fn != nullptr && fn(kind, seq) != 0;
	}
}
