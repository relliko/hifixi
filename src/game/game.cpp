#include "../../deps/ashita/plugins/sdk/Ashita.h" // (its Memory.h needs the rest)
#include "game.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace hifi::game
{
	namespace
	{
		uintptr_t g_base = 0;
		size_t g_size = 0;
		bool g_image_set = false;
		uintptr_t g_alloc_at = 0;
		// gGame's global, from the camera reader (mov eax, [gGame]; add eax, 0x94; ret), looked for once in an image
		constexpr const char *kGamePat = "A1????????0594000000C3";
		uintptr_t g_game_at = 0;
		bool g_game_looked = false;

		std::mutex g_image_mutex; // (the zone bakers' thread looks for the game's key tables too)

		// FFXiMain.dll's image, as loaded
		void find_image()
		{
			std::lock_guard<std::mutex> lock(g_image_mutex);
			if (g_image_set)
				return;
			const auto base = reinterpret_cast<const uint8_t *>(GetModuleHandleW(L"FFXiMain.dll"));
			if (base == nullptr)
				return;
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_lfanew);
			g_base = reinterpret_cast<uintptr_t>(base);
			g_size = nt->OptionalHeader.SizeOfImage;
			g_image_set = true;
		}
	}

	void set_image(uintptr_t base, size_t size)
	{
		std::lock_guard<std::mutex> lock(g_image_mutex);
		g_base = base;
		g_size = size;
		g_image_set = true;
		g_game_looked = false;
	}

	bool image_known()
	{
		find_image();
		return g_base != 0;
	}

	uintptr_t find(const char *pattern, intptr_t offset)
	{
		find_image();
		if (g_base == 0)
			return 0;
		return Ashita::Memory::FindPattern(g_base, g_size, pattern, offset, 0);
	}

	bool read(uintptr_t at, void *out, size_t size)
	{
		SIZE_T got = 0;
		return at >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(at), out, size, &got) != 0 && got == size;
	}

	uint32_t word(uintptr_t at)
	{
		uint32_t v = 0;
		return read(at, v) ? v : 0;
	}

	bool float_of(uint32_t bits, float &out)
	{
		if ((bits & 0x7F800000u) == 0x7F800000u)
			return false;
		std::memcpy(&out, &bits, 4);
		return true;
	}

	uint32_t game_object()
	{
		if (!g_game_looked && image_known())
		{
			g_game_looked = true;
			const uintptr_t p = find(kGamePat);
			const uint32_t global = p != 0 ? word(p + 1) : 0;
			g_game_at = pointer(global) ? global : 0;
		}
		const uint32_t game = g_game_at != 0 ? word(g_game_at) : 0;
		return pointer(game) ? game : 0;
	}

	uint32_t resolve_tint()
	{
		const uint32_t game = game_object();
		uint32_t tint = 0;
		return game != 0 && read(game + 0x320, tint) ? tint & 0xFFFFFFu : 0x808080u;
	}

	bool write_code(uintptr_t at, const void *bytes, size_t size)
	{
		DWORD old = 0;
		if (!VirtualProtect(reinterpret_cast<void *>(at), size, PAGE_EXECUTE_READWRITE, &old))
			return false;
		std::memcpy(reinterpret_cast<void *>(at), bytes, size);
		VirtualProtect(reinterpret_cast<void *>(at), size, old, &old);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(at), size);
		return true;
	}

	bool code_is(uintptr_t at, const void *bytes, size_t size)
	{
		std::string here(size, '\0');
		return read(at, here.data(), size) && std::memcmp(here.data(), bytes, size) == 0;
	}

	uintptr_t alloc_code(size_t size)
	{
		void *p = VirtualAlloc(g_alloc_at != 0 ? reinterpret_cast<void *>(g_alloc_at) : nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if (p != nullptr && g_alloc_at != 0)
			g_alloc_at += (size + 0xFFFF) & ~size_t(0xFFFF); // (the next one past it, on the allocation granularity)
		return reinterpret_cast<uintptr_t>(p);
	}

	void free_code(uintptr_t at)
	{
		if (at != 0)
			VirtualFree(reinterpret_cast<void *>(at), 0, MEM_RELEASE);
	}

	void set_alloc_at(uintptr_t at)
	{
		g_alloc_at = at;
	}

	uint32_t rel_target(uintptr_t at)
	{
		int32_t rel = 0;
		if (!read(at + 1, rel))
			return 0;
		return uint32_t(at + 5 + uint32_t(rel));
	}

	void put32(uint8_t *to, uint32_t value)
	{
		std::memcpy(to, &value, 4);
	}

	uint32_t name_value(const char *four)
	{
		uint32_t v = 0;
		std::memcpy(&v, four, 4);
		return v;
	}

	std::string name_text(uint32_t value)
	{
		std::string s;
		for (int i = 0; i < 4; ++i)
		{
			const char c = char((value >> (8 * i)) & 0xFF);
			s += (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || c == '~' || c == ' ' ? c : '.';
		}
		while (!s.empty() && s.back() == ' ')
			s.pop_back();
		return s;
	}

	bool setting_on(const char *key, bool fallback)
	{
		char text[64] = "";
		if (worldfx_setting(key, text, sizeof(text)) != 1)
			return fallback;
		return std::strcmp(text, "0") != 0 && _stricmp(text, "false") != 0;
	}

	float setting_number(const char *key, float fallback)
	{
		char text[64] = "";
		if (worldfx_setting(key, text, sizeof(text)) != 1)
			return fallback;
		char *end = nullptr;
		const float v = std::strtof(text, &end);
		return end != text && std::isfinite(v) ? v : fallback;
	}

	bool option_on(const char *section, const char *key, bool fallback)
	{
		char text[64] = "";
		if (hifixi_option(section, key, text, sizeof(text)) != 1)
			return fallback;
		return std::strcmp(text, "0") != 0 && _stricmp(text, "false") != 0;
	}

	float option_number(const char *section, const char *key, float fallback)
	{
		char text[64] = "";
		if (hifixi_option(section, key, text, sizeof(text)) != 1)
			return fallback;
		char *end = nullptr;
		const float v = std::strtof(text, &end);
		return end != text && std::isfinite(v) ? v : fallback;
	}

	std::string option_text(const char *section, const char *key, const char *fallback)
	{
		char text[256] = "";
		if (hifixi_option(section, key, text, sizeof(text)) != 1)
			return fallback != nullptr ? fallback : "";
		return text;
	}

	void set_option_on(const char *section, const char *key, bool on)
	{
		hifixi_set_option(section, key, on ? "1" : "0");
	}

	void set_option_number(const char *section, const char *key, float value)
	{
		// (a whole number as one; else the shortest text that reads back as the same float)
		char text[32];
		if (value == std::floor(value) && std::fabs(value) < 1e7f)
			std::snprintf(text, sizeof(text), "%ld", std::lround(value));
		else
			for (int digits = 1; digits <= 9; ++digits)
			{
				std::snprintf(text, sizeof(text), "%.*g", digits, double(value));
				if (std::strtof(text, nullptr) == value)
					break;
			}
		hifixi_set_option(section, key, text);
	}

	std::string data_folder()
	{
		const char *f = worldfx_folder();
		return f != nullptr ? std::string(f) : std::string();
	}

	bool has_zone_file(const char *kind, uint32_t zone)
	{
		const std::string folder = data_folder();
		if (folder.empty())
			return false;
		const std::string path = folder + kind + "\\" + std::to_string(zone) + ".bin";
		const DWORD a = GetFileAttributesA(path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}
}
