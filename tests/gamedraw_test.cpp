/*
 * Offline test of the game's own drawing switched off (src/game/gamedraw.cpp; research/ffxi-re/notes-offswitch.md):
 * each FFXiMain dump mapped at its own base (game_dump.hpp).
 *   - an image without the game's code: refused and said why, nothing written;
 *   - the sites found where the notes have them (17C9C0, 17ADE0, 02B7E3 and its early return 02B7B0);
 *   - zone off: 17C9C0 and 17ADE0 begin with ret 0Ch and ret 4, nothing else in the image changed; back on: the dump
 *     again, byte for byte;
 *   - chars off: 02B7E3 is "jmp short 02B7B0" and 02B7B0 is the model draw's own early return (model +0xB4 from the
 *     triangle count, pop edi/esi/ebp/ebx, add esp 7Ch, ret 1Ch); back on: the dump again;
 *   - both off, then one site changed by something else: switching that part back refused and left as it is, the
 *     other part still put back; stop() refused too while the change stands; once the game's byte is back, stop()
 *     puts everything back and the image is the dump again.
 *
 *   build\gamedraw_test.exe     (built by build.cmd)
 */

#include "../src/game/gamedraw.hpp"
#include "game_dump.hpp"
#include <cstdint>
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

	bool bytes_are(uintptr_t at, const std::vector<uint8_t> &b)
	{
		return std::memcmp(reinterpret_cast<const void *>(at), b.data(), b.size()) == 0;
	}

	// How many bytes of the image differ from the file
	size_t differences(const std::string &path, uintptr_t base, size_t size)
	{
		FILE *f = nullptr;
		if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr)
			return size_t(-1);
		std::vector<uint8_t> file(size);
		const size_t got = fread(file.data(), 1, size, f);
		fclose(f);
		if (got != size)
			return size_t(-1);
		size_t n = 0;
		const uint8_t *image = reinterpret_cast<const uint8_t *>(base);
		for (size_t i = 0; i < size; ++i)
			n += image[i] != file[i];
		return n;
	}

	void test_dump(const std::string &path)
	{
		namespace gamedraw = hifi::game::gamedraw;
		using gamedraw::part;
		std::printf("\n%s\n", path.c_str());
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		check(size != 0, "mapped at its base");
		if (size == 0)
			return;
		std::string why;

		check(gamedraw::set(part::zone, true, why) && gamedraw::is_off(part::zone), "zone off" + (why.empty() ? "" : ": " + why));
		const gamedraw::sites s = gamedraw::where();
		check(s.zone_draw == base + 0x17C9C0 && s.zone_lit == base + 0x17ADE0 && s.chars_jump == base + 0x02B7E3 && s.chars_return == base + 0x02B7B0,
			"  the sites found at 17C9C0, 17ADE0, 02B7E3, 02B7B0");
		check(bytes_are(base + 0x17C9C0, { 0xC2, 0x0C, 0x00, 0x0C }) && bytes_are(base + 0x17ADE0, { 0xC2, 0x04, 0x00, 0x56 }),
			"  17C9C0 begins ret 0Ch, 17ADE0 ret 4");
		check(differences(path, base, size) == 6, "  nothing else changed (6 bytes)");
		check(gamedraw::status().find("zone OFF, characters on") != std::string::npos, "  status: " + gamedraw::status());
		check(gamedraw::set(part::zone, true, why), "  zone off again: nothing to do");
		check(gamedraw::set(part::zone, false, why) && !gamedraw::is_off(part::zone) && dump::same_as_file(path, base, size),
			"zone back on: the dump again, byte for byte");

		check(gamedraw::set(part::chars, true, why) && gamedraw::is_off(part::chars), "chars off" + (why.empty() ? "" : ": " + why));
		const uint8_t *j = reinterpret_cast<const uint8_t *>(base + 0x02B7E3);
		check(j[0] == 0xEB && base + 0x02B7E5 + int8_t(j[1]) == base + 0x02B7B0, "  02B7E3: jmp short 02B7B0");
		// (02B7B0: mov eax, [triangles]; pop edi; mov [ebp+0B4h], eax; pop esi; pop ebp; pop ebx; add esp, 7Ch; ret 1Ch)
		const uint8_t *r = reinterpret_cast<const uint8_t *>(base + 0x02B7B0);
		check(r[0] == 0xA1 && bytes_are(base + 0x02B7B5, { 0x5F, 0x89, 0x85, 0xB4, 0x00, 0x00, 0x00, 0x5E, 0x5D, 0x5B, 0x83, 0xC4, 0x7C, 0xC2, 0x1C, 0x00 }),
			"  02B7B0 is the model draw's own early return");
		check(differences(path, base, size) == 2, "  nothing else changed (2 bytes)");
		check(gamedraw::set(part::chars, false, why) && dump::same_as_file(path, base, size), "chars back on: the dump again, byte for byte");

		// Both off, then something else changes the zone's first site
		check(gamedraw::set(part::zone, true, why) && gamedraw::set(part::chars, true, why), "both off");
		uint8_t *zone = reinterpret_cast<uint8_t *>(base + 0x17C9C0);
		zone[1] = 0x99;
		why.clear();
		check(!gamedraw::set(part::zone, false, why) && gamedraw::is_off(part::zone) && zone[0] == 0xC2 && zone[1] == 0x99,
			"  zone back on refused while something else changed it, left as it is: " + why);
		why.clear();
		check(!gamedraw::stop(why) && !gamedraw::is_off(part::chars) && gamedraw::is_off(part::zone), "  stop(): refused for the zone, chars put back: " + why);
		zone[1] = 0x0C;
		check(gamedraw::stop(why) && !gamedraw::is_off(part::zone) && dump::same_as_file(path, base, size),
			"  once the byte is back, stop(): the dump again, byte for byte");
		check(gamedraw::status().find("all of it") != std::string::npos, "  status: " + gamedraw::status());
		dump::unmap(base);
	}
}

// (HiFi XI's options and World FX's settings: not looked at)
extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main()
{
	namespace gamedraw = hifi::game::gamedraw;
	std::vector<uint8_t> empty(0x10000, 0);
	hifi::game::set_image(reinterpret_cast<uintptr_t>(empty.data()), empty.size());
	std::string why;
	check(!gamedraw::set(gamedraw::part::zone, true, why) && !gamedraw::is_off(gamedraw::part::zone) && !why.empty(),
		"an image without the game's code: refused, " + why);
	check(!gamedraw::set(gamedraw::part::chars, true, why), "  chars refused too");
	check(gamedraw::stop(why), "  stop(): nothing to put back");

	for (const char *path : dump::kDumps)
	{
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		{
			check(false, std::string(path) + " is there");
			continue;
		}
		test_dump(path);
	}
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
