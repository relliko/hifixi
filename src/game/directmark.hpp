#pragma once

// The game side's half of the direct channel (src/host/frameapi.hpp; no d3d12.h here: the game side's files include
// Ashita's Direct3D 8 headers): a pass's mark handed to HiFi XI's own d3d8.dll as a call. The only way a mark goes out:
// with another d3d8.dll loaded (or ours with no handler yet) nothing is marked and the game draws those passes itself.

#include <windows.h>
#include <cstdint>

namespace hifi::game
{
	using direct_mark_fn = int(__cdecl *)(uint32_t kind, uint32_t seq);

	// Where marks go: our d3d8.dll's HifiFrameMark, looked up once; the offline tests put their own in
	inline direct_mark_fn &direct_mark_target()
	{
		static direct_mark_fn fn = nullptr;
		static bool looked = false;
		if (!looked)
		{
			looked = true;
			if (fn == nullptr)
				if (HMODULE d3d8 = GetModuleHandleW(L"d3d8.dll"))
					fn = reinterpret_cast<direct_mark_fn>(GetProcAddress(d3d8, "HifiFrameMark"));
		}
		return fn;
	}

	// Whether marks can go out at all (our d3d8.dll is the one loaded)
	inline bool direct_channel()
	{
		return direct_mark_target() != nullptr;
	}

	// The mark sent; false when it couldn't be (no channel, or nothing on the other end yet)
	inline bool direct_mark(uint32_t kind, uint32_t seq)
	{
		const direct_mark_fn fn = direct_mark_target();
		return fn != nullptr && fn(kind, seq) != 0;
	}

	// (the offline tests: marks to fn instead; nullptr: none)
	inline void set_direct_mark(direct_mark_fn fn)
	{
		direct_mark_target();
		direct_mark_target() = fn;
	}
}
