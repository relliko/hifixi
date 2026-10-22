#pragma once

// For the game-side tests: an FFXiMain dump (FFXiMain_<base hex>.bin, flat: file offset = image offset) mapped at its
// own base in this 32-bit process, as the image the patterns are looked for in; our routines' memory handed out from
// 0x30000000 up (as the worldfx addon's Python tests did: the routines hold 32-bit addresses), and the dumps' paths.
// FFXiMain_041C0000.bin was dumped with the worldfx addon's patches in (its flare 054AF1, its water 040DC4, 0D5365 and
// 0D64FA): a test of those sites puts the game's own bytes back in the mapped copy first.

#include "../src/game/game.hpp"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace dump
{
	inline const char *const kDumps[] = { "C:\\Games\\PhoenixXI\\FFXiMain_04490000.bin", "C:\\Games\\PhoenixXI\\FFXiMain_041C0000.bin" };
	constexpr uintptr_t kAllocAt = 0x30000000;

	// The base in the file's name (FFXiMain_<hex>.bin), 0 when it has none
	inline uintptr_t base_of(const std::string &path)
	{
		const size_t at = path.rfind('_');
		return at == std::string::npos ? 0 : uintptr_t(std::strtoul(path.c_str() + at + 1, nullptr, 16));
	}

	// The dump mapped at its base (read, write, execute: patched and run as the game's would be); its size, 0 when it
	// couldn't be. Unmap it before mapping another. (Its place taken first, then the file read straight into it: the
	// dumps' ranges overlap, and a copy of one on the heap could sit where the next must go.)
	inline size_t map(const std::string &path, uintptr_t &base)
	{
		base = base_of(path);
		WIN32_FILE_ATTRIBUTE_DATA a = {};
		if (base == 0 || !GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a) || a.nFileSizeHigh != 0 || a.nFileSizeLow == 0)
			return 0;
		const size_t size = a.nFileSizeLow;
		void *p = VirtualAlloc(reinterpret_cast<void *>(base), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if (p != reinterpret_cast<void *>(base))
		{
			if (p != nullptr)
				VirtualFree(p, 0, MEM_RELEASE);
			return 0;
		}
		FILE *f = nullptr;
		size_t got = 0;
		if (fopen_s(&f, path.c_str(), "rb") == 0 && f != nullptr)
		{
			got = fread(p, 1, size, f);
			fclose(f);
		}
		if (got != size)
		{
			VirtualFree(p, 0, MEM_RELEASE);
			return 0;
		}
		hifi::game::set_image(base, size);
		hifi::game::set_alloc_at(kAllocAt);
		return size;
	}

	inline void unmap(uintptr_t base)
	{
		VirtualFree(reinterpret_cast<void *>(base), 0, MEM_RELEASE);
	}

	// The dump's bytes as in the file (to check the image is the dump again after everything is put back)
	inline bool same_as_file(const std::string &path, uintptr_t base, size_t size)
	{
		FILE *f = nullptr;
		if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr)
			return false;
		std::vector<char> bytes(size);
		const size_t got = fread(bytes.data(), 1, size, f);
		fclose(f);
		return got == size && std::memcmp(bytes.data(), reinterpret_cast<const void *>(base), size) == 0;
	}
}
