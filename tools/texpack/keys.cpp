/*
 * bake::key_tables for hifixi-texpack (keys.hpp): the tables found in FFXiMain.dll's bytes as a file, not in the
 * running game's image (dat.cpp's, which needs game::find and so the plugin's 32-bit code).
 */

#include "keys.hpp"
#include "../../src/bake/dat.hpp"
#include <cstring>
#include <vector>

namespace hifi::texpack
{
	namespace
	{
		std::vector<uint8_t> g_file;
		size_t g_at = 0;
		bool g_found = false;
		// (the first table's first 16 bytes; the second table is 0x328 bytes on, 256 bytes each)
		constexpr uint8_t kStart[16] = { 0xE2, 0xE5, 0x06, 0xA9, 0xED, 0x26, 0xF4, 0x42, 0x15, 0xF4, 0x81, 0x7F, 0xDE, 0x9A, 0xDE, 0xD0 };
		constexpr size_t kSecond = 0x328, kTable = 256;
	}

	bool load_key_tables(const std::wstring &path, std::string &why)
	{
		g_found = false;
		if (!bake::read_file(path, g_file))
		{
			why = "FFXiMain.dll can't be read (give the game's folder with --ffxi, or a dump with --ffximain)";
			return false;
		}
		for (size_t i = 0; i + kSecond + kTable <= g_file.size(); ++i)
			if (g_file[i] == kStart[0] && std::memcmp(g_file.data() + i, kStart, sizeof(kStart)) == 0)
			{
				g_at = i;
				g_found = true;
				return true;
			}
		why = "the game's key tables aren't in FFXiMain.dll's file (a client that packs it: give a dump of its image with --ffximain)";
		return false;
	}
}

namespace hifi::bake
{
	bool key_tables(const uint8_t *&first, const uint8_t *&second)
	{
		if (!texpack::g_found)
			return false;
		first = texpack::g_file.data() + texpack::g_at;
		second = first + texpack::kSecond;
		return true;
	}
}
