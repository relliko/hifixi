#pragma once

// What HiFi XI's zone bakers (src/bake: reshade-worldfx's tools/bake_*.py, in C++, run on the player's own PC from
// their own game files) share: a zone's DAT found and read (zone_collision.py's zone_dat_path), its chunks, the game's
// two decryption key tables (found in FFXiMain.dll's image: nothing of Square Enix's is in HiFi XI), and Windows' XPRESS
// packing as the Python bakers call it (ntdll), so a file baked here is the same bytes as one baked there.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::bake
{
	// FINAL FANTASY XI's folder (with VTABLE.DAT, FTABLE.DAT and the ROM folders): the game's own, found from the
	// running game (or the tests' default, C:\Games\PhoenixXI\SquareEnix\FINAL FANTASY XI); set to override
	const std::wstring &ffxi_folder();
	void set_ffxi_folder(const std::wstring &folder);

	// The DAT of file id `id` (zone_dat_path: VTABLE.DAT's byte 1, FTABLE.DAT; else ROMn's VTABLEn / FTABLEn); empty
	// with why when no ROM has it
	std::wstring dat_path(uint32_t id, std::string &why);
	// A zone's DAT (file id 100 + zone)
	inline std::wstring zone_dat_path(uint32_t zone, std::string &why) { return dat_path(100 + zone, why); }
	// A whole file; false when it can't be read
	bool read_file(const std::wstring &path, std::vector<uint8_t> &out);

	// One chunk of a DAT: its type (bits 0-6 of the header's second word), its 4-character name, its body (after the
	// 16-byte header; size from bits 7-25, in 16-byte units)
	struct chunk
	{
		uint32_t type;
		char name[4];
		const uint8_t *body;
		size_t size;
	};
	std::vector<chunk> chunks(const std::vector<uint8_t> &dat);
	// The first chunk of a type, copied (bodies are decrypted in place); false when there's none
	bool find_chunk(const std::vector<uint8_t> &dat, uint32_t type, std::vector<uint8_t> &out);

	// The game's key tables (256 bytes each, in FFXiMain.dll: the first, the MZB's and the MMB's first, where its
	// first bytes are; the second, the MMB's swaps, 0x328 bytes on), found in the game's image (hifi::game::set_image
	// for the tests); false when the image isn't known or they aren't there
	bool key_tables(const uint8_t *&first, const uint8_t *&second);

	// Windows' XPRESS + Huffman (RtlCompressBuffer, format 4 | COMPRESSION_ENGINE_MAXIMUM, chunk 4096), as
	// bake_rain.py's and bake_shadow_geo.py's xpress(): empty when it fails
	std::vector<uint8_t> xpress(const uint8_t *data, size_t size);
	bool unxpress(const uint8_t *data, size_t size, size_t unpacked, std::vector<uint8_t> &out);

	// A file written whole (to a temporary name first, then moved into place: a reader never sees half of one), its
	// folder made when missing; false when it couldn't be
	bool write_file(const std::wstring &path, const void *data, size_t size);
}
