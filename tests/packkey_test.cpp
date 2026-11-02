/*
 * The texture pack's key (src/pack/pack.hpp) on the game's own DATs, against research\ffxi-re\tools\packkey_check.py
 * (an independent reading in Python, which also matched it to textures copied from the running game):
 *   - mt_h81_2 (a Mithra head, ROM\309\74 at 0x9E810) and hf_cro1_ (ROM\31\115): the keys the Python check made;
 *   - hf_shie1 in ROM\55\43 and ROM\31\43: the same name, two textures, two keys;
 *   - hifi-textures' copy of ROM\309\74 (when the overlay is there): mt_h81_2 bigger, its key another.
 *
 *   build\packkey_test.exe
 *
 * Needs the game's files (C:\Games\PhoenixXI). Exit code: the number of failures.
 */

#include "../src/pack/pack.hpp"
#include "../src/bake/dat.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	std::string text(const hifi::pack::key &k)
	{
		char s[64];
		std::snprintf(s, sizeof(s), "%016llX:%u:%ux%u", static_cast<unsigned long long>(k.hash), k.bytes, k.w, k.h);
		return s;
	}

	// The keys of a DAT's textures named `name` (the last 8 of the 16 bytes at body+1), in file order
	std::vector<hifi::pack::key> keys(const std::wstring &path, const char *name)
	{
		std::vector<hifi::pack::key> out;
		std::vector<uint8_t> dat;
		if (!hifi::bake::read_file(path, dat))
			return out;
		for (const hifi::bake::chunk &c : hifi::bake::chunks(dat))
			if (c.type == 0x20 && c.size > 17 && std::memcmp(c.body + 9, name, 8) == 0)
				out.push_back(hifi::pack::key_of(c.body, c.size));
		return out;
	}
}

// (HiFi XI's options and World FX's settings: not looked at)
extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main()
{
	const std::wstring game = hifi::bake::ffxi_folder();
	struct want
	{
		const wchar_t *dat;
		const char *name, *key;
	};
	const want wants[] = {
		{ L"ROM\\309\\74.DAT", "mt_h81_2", "E34BB46396730081:16464:128x128" },
		{ L"ROM\\31\\115.DAT", "hf_cro1_", "4C28957BDBD371FA:4176:64x64" },
		{ L"ROM\\55\\43.DAT", "hf_shie1", "3996E0ECB6AE7D68:8272:128x64" },
		{ L"ROM\\31\\43.DAT", "hf_shie1", "07EC892EB76421F2:8272:128x64" },
	};
	for (const want &w : wants)
	{
		const std::vector<hifi::pack::key> got = keys(game + L"\\" + w.dat, w.name);
		check(!got.empty() && text(got[0]) == w.key, std::string(w.name) + ": " + (got.empty() ? "not found" : text(got[0])) + " (Python: " + w.key + ")");
	}
	check(hifi::pack::key_of(nullptr, 0).bytes == 0 && hifi::pack::key_of(reinterpret_cast<const uint8_t *>("short"), 5).bytes == 0, "too small to be a texture: no key");

	// (the overlay's copy, when it's there: what the game holds in memory while it's listed)
	const std::wstring overlay = L"C:\\Games\\PhoenixXI\\polplugins\\DATs\\hifi-textures\\ROM\\309\\74.DAT";
	const std::vector<hifi::pack::key> own = keys(game + L"\\ROM\\309\\74.DAT", "mt_h81_2"), big = keys(overlay, "mt_h81_2");
	if (big.empty())
		std::printf("--    hifi-textures' ROM\\309\\74.DAT isn't there: its check skipped\n");
	else
		check(!own.empty() && big[0].w == own[0].w * 4 && !(big[0] == own[0]), "the overlay's mt_h81_2: " + text(big[0]) + ", not the game's key");

	std::printf(failures == 0 ? "all passed (%d)\n" : "%d failed\n", failures == 0 ? passes : failures);
	return failures;
}
