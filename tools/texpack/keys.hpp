#pragma once

// The game's two key tables for hifixi-texpack (64-bit, so it can't load FFXiMain.dll, and has no game image to search
// as the plugin's dat.cpp does): read from FFXiMain.dll's file on disk, or from a dump of its image. The 16 bytes
// E2E506A9ED26F44215F4817FDE9ADED0 start the first table (found twice in the file, the first at file offset 0x4DEC0 in
// the November 2025 client; the first copy and the second table 0x328 on from it are byte-equal to the running game's).
// bake::key_tables (dat.hpp) then answers from these bytes; zonemesh needs them to unscramble a zone's MMBs.

#include <string>

namespace hifi::texpack
{
	// The tables read from path (FFXiMain.dll, or a dump of its image); false (why) when the file can't be read or
	// hasn't them (a client update that packs the DLL would do that: then give a dump with --ffximain)
	bool load_key_tables(const std::wstring &path, std::string &why);
}
