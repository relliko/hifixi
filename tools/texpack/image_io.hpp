#pragma once

// Pictures to and from PNG files for hifixi-texpack make (the batches the upscaler reads and writes) through Windows'
// own codecs (WIC), so the tool needs no image library and no Python. Also built into tests\fake_esrgan.cpp.

#include <cstdint>
#include <string>
#include <vector>

namespace hifi::texpack::image
{
	// RGBA8, rows top down
	struct picture
	{
		uint32_t w = 0, h = 0;
		std::vector<uint8_t> px;
	};

	// COM for WIC on this thread (multithreaded apartment) for as long as it lives
	struct com_scope
	{
		com_scope();
		~com_scope();
		com_scope(const com_scope &) = delete;
		com_scope &operator=(const com_scope &) = delete;
		bool ok = false;
	};

	// Any picture WIC reads (PNG, ...) as RGBA8; false (why) when it can't be
	bool read(const std::wstring &path, picture &out, std::string &why);
	// px is w x h with channels 3 (RGB) or 4 (RGBA), rows top down: written as a PNG of 24 or 32 bits a texel
	bool write_png(const std::wstring &path, const uint8_t *px, uint32_t w, uint32_t h, uint32_t channels, std::string &why);
}
