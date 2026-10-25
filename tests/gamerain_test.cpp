/*
 * Offline test of the game's weather left out (src/game/gamerain.cpp, begun as the worldfx addon's /worldfx gamerain in
 * C++): each FFXiMain dump mapped at its own base (game_dump.hpp), the routine's memory from 0x30000000, what World FX
 * draws of the weather faked (worldfx_weather_drawn answers from a word here); World FX's settings and folder faked too,
 * only to show they're no longer looked at.
 *   - the pattern unique in the dump; 046070's "mov al, [ebp+17Ah]" (04609D) becoming a call to the routine and a nop
 *     and nothing else around it; the routine byte for byte as written out here for its memory at 0x30000000;
 *   - its all word and list in each case: no zone; World FX drawing its rain and snow (the whole list), one of them
 *     (the other's alone), neither, other parts of the weather alone (nothing), all of it (the all word, an empty
 *     list), all with every other bit, back from all to the list mode; World FX's own Rain/Snow settings and rain maps
 *     not looked at; the route out and in again (the same memory); the status and seen lines in both modes;
 *   - stop(): not while something else changed the code (its memory kept), then the image the dump again, byte for
 *     byte, the memory freed;
 *   - the routine's memory (the whole list filled, the all word 0) written to build\test\gamerain_cave.bin and run as
 *     the game would by tests/gamerain_cave.c (build\test\gamerain_cave.exe: build.cmd), which runs both modes on it.
 *
 *   build\gamerain_test.exe     (built by build.cmd)
 */

#include "../src/game/gamerain.hpp"
#include "game_dump.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
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

	// What World FX draws of the weather (worldfx_weather_drawn), and its settings and folder: set here only to show
	// they're no longer looked at
	uint32_t drawn = 0;
	std::map<std::string, std::string> settings;
	std::string folder;

	constexpr uint32_t kRain = hifi::game::kWeatherFxRain, kSnow = hifi::game::kWeatherFxSnow, kAll = hifi::game::kWeatherFxAll;

	constexpr uintptr_t kSite = 0x04609D;   // mov al, [ebp+17Ah] in 046070, every effect element's draw
	constexpr const char *kPattern = "8D8DF8000000D91C2450E8????????8B00A3????????8A857A01000084C00F85";
	constexpr uint32_t kSize = 0x270, kCode = 0x190, kCodeSize = 0xE0;
	constexpr uint32_t kAllWord = 0x0C;     // the routine's all word (not 0: every weather element skipped)

	// The routine for its memory at 0x30000000, written out (capstone's reading of it in the comments; the addon's was
	// this less the all word: its 30-3B here are new, and its count of skipped draws moved to the end, C7-D2)
	constexpr const char *kRoutine =
		"8A857A010000"     // 00        mov  al, [ebp+17Ah]
		"84C0" "7401" "C3" // 06        test al, al; jz go; ret
		"60" "31DB"        // 0B go:    pushad; xor ebx, ebx
		"8BB5FC000000"     // 0E        mov  esi, [ebp+0FCh]
		"85F6" "7466"      // 14        test esi, esi; jz out (7E)
		"8B36" "85F6"      // 18        mov  esi, [esi]; test esi, esi
		"7460"             // 1C        jz   out
		"8A4624" "247F"    // 1E        mov  al, [esi+24h]; and al, 7Fh
		"3C05" "7557"      // 23        cmp  al, 5; jne out
		"F686DE00000080"   // 27        test byte [esi+0DEh], 80h
		"744E"             // 2E        jz   out
		"833D0C00003000"   // 30        cmp  dword [3000000Ch], 0       the all word
		"0F95C3"           // 37        setne bl
		"750C"             // 3A        jne  model (48)
		"F7850C01000000004000" // 3C    test dword [ebp+10Ch], 400000h
		"7536"             // 46        jnz  out
		"31FF"             // 48 model: xor  edi, edi
		"8B86E4000000"     // 4A        mov  eax, [esi+0E4h]
		"85C0" "7408"      // 50        test eax, eax; jz nomodel (5C)
		"803801" "7503"    // 54        cmp  byte [eax], 1; jne nomodel
		"8B780C"           // 59        mov  edi, [eax+0Ch]
		"B904000000"       // 5C        mov  ecx, 4
		"8B4628"           // 61 up:    mov  eax, [esi+28h]
		"85C0" "7416"      // 64        test eax, eax; jz out
		"8B30" "85F6"      // 68        mov  esi, [eax]; test esi, esi
		"7410"             // 6C        jz   out
		"8A4624" "247F"    // 6E        mov  al, [esi+24h]; and al, 7Fh
		"3C01" "7409"      // 73        cmp  al, 1; je found (80)
		"3C05" "7503"      // 77        cmp  al, 5; jne out
		"49" "75E3"        // 7B        dec  ecx; jnz up (61)
		"EB49"             // 7E out:   jmp  out2 (C9)
		"8B5620"           // 80 found: mov  edx, [esi+20h]
		"8B0D04000030"     // 83        mov  ecx, [30000004h]           seen_at
		"83E11F"           // 89        and  ecx, 1Fh
		"8914CD90000030"   // 8C        mov  [ecx*8+30000090h], edx     seen
		"893CCD94000030"   // 93        mov  [ecx*8+30000094h], edi
		"FF0504000030"     // 9A        inc  dword [30000004h]
		"BE10000030"       // A0        mov  esi, 30000010h             the list
		"8B06" "85C0"      // A5 scan:  mov  eax, [esi]; test eax, eax
		"741E"             // A9        jz   out2
		"39D0" "750B"      // AB        cmp  eax, edx; jne next (BA)
		"8B4604"           // AF        mov  eax, [esi+4]
		"85C0" "7411"      // B2        test eax, eax; jz hit (C7)
		"39F8" "740D"      // B6        cmp  eax, edi; je hit
		"83C608"           // BA next:  add  esi, 8
		"81FE90000030"     // BD        cmp  esi, 30000090h             the list's end
		"72E0"             // C3        jb   scan (A5)
		"EB02"             // C5        jmp  out2
		"B301"             // C7 hit:   mov  bl, 1
		"84DB" "7406"      // C9 out2:  test bl, bl; jz done (D3)
		"FF0500000030"     // CD        inc  dword [30000000h]          hidden
		"881D08000030"     // D3 done:  mov  [30000008h], bl            result
		"61"               // D9        popad
		"A008000030"       // DA        mov  al, [30000008h]
		"C3";              // DF        ret

	using pair_list = std::vector<std::pair<std::string, std::string>>; // (weather, model; "" for any)
	const pair_list kRainList = { { "rain", "" }, { "squl", "" }, { "rai1", "" }, { "squ1", "" },
		{ "bolt", "rain" }, { "bolt", "smok" }, { "bolt", "asi1" }, { "bolt", "hamo" },
		{ "bol1", "rain" }, { "bol1", "smok" }, { "bol1", "asi1" }, { "bol1", "hamo" } };
	const pair_list kSnowList = { { "snow", "" }, { "bliz", "" }, { "sno1", "" }, { "bli1", "" } };

	pair_list full_list()
	{
		pair_list all = kRainList;
		all.insert(all.end(), kSnowList.begin(), kSnowList.end());
		return all; // (16 pairs: the routine's whole list, no end marker)
	}

	std::string from_hex(const char *hex)
	{
		std::string out;
		for (size_t i = 0; hex[i] != 0 && hex[i + 1] != 0; i += 2)
		{
			const char two[3] = { hex[i], hex[i + 1], 0 };
			out += char(std::strtoul(two, nullptr, 16));
		}
		return out;
	}

	std::string bytes_at(uintptr_t at, size_t n)
	{
		return std::string(reinterpret_cast<const char *>(at), n);
	}

	uint32_t word_at(uintptr_t at)
	{
		uint32_t v = 0;
		std::memcpy(&v, reinterpret_cast<const void *>(at), 4);
		return v;
	}

	std::string name(uint32_t v)
	{
		return v != 0 ? std::string(reinterpret_cast<const char *>(&v), 4) : std::string();
	}

	// Where a pattern (hex, ?? for any byte) is in the image, every place
	std::vector<uintptr_t> find_all(uintptr_t base, size_t size, const char *pattern)
	{
		std::vector<uint8_t> want;
		std::vector<bool> known;
		for (size_t i = 0; pattern[i] != 0 && pattern[i + 1] != 0; i += 2)
		{
			const char two[3] = { pattern[i], pattern[i + 1], 0 };
			known.push_back(two[0] != '?');
			want.push_back(two[0] != '?' ? uint8_t(std::strtoul(two, nullptr, 16)) : uint8_t(0));
		}
		std::vector<uintptr_t> out;
		const uint8_t *image = reinterpret_cast<const uint8_t *>(base);
		for (size_t at = 0; at + want.size() <= size; ++at)
		{
			size_t i = 0;
			while (i < want.size() && (!known[i] || image[at + i] == want[i]))
				i++;
			if (i == want.size())
				out.push_back(base + at);
		}
		return out;
	}

	// The routine's list as it reads it: pairs up to the first 0 name
	pair_list pairs(uintptr_t mem)
	{
		pair_list out;
		for (uint32_t i = 0; i < 16; ++i)
		{
			const uint32_t w = word_at(mem + 0x10 + i * 8), m = word_at(mem + 0x10 + i * 8 + 4);
			if (w == 0)
				break;
			out.push_back({ name(w), name(m) });
		}
		return out;
	}

	// Every word of the list past its pairs 0 (nothing left over from a longer list)
	bool rest_empty(uintptr_t mem)
	{
		const size_t n = pairs(mem).size();
		for (size_t i = n * 2; i < 32; ++i)
			if (word_at(mem + 0x10 + i * 4) != 0)
				return false;
		return true;
	}

	uint32_t all_word(uintptr_t mem)
	{
		return word_at(mem + kAllWord);
	}

	bool memory_free(uintptr_t at)
	{
		MEMORY_BASIC_INFORMATION mbi = {};
		return VirtualQuery(reinterpret_cast<void *>(at), &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_FREE;
	}

	// A few of the dump's bytes as in its file (not the whole of it: a dump-sized buffer can land where the next dump
	// is to be mapped)
	std::string file_bytes(const std::string &path, size_t at, size_t n)
	{
		std::string out(n, '\0');
		FILE *f = nullptr;
		if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr)
			return std::string();
		const bool ok = fseek(f, long(at), SEEK_SET) == 0 && fread(out.data(), 1, n, f) == n;
		fclose(f);
		return ok ? out : std::string();
	}

	bool write_file(const std::string &path, const std::string &bytes)
	{
		FILE *f = nullptr;
		if (fopen_s(&f, path.c_str(), "wb") != 0 || f == nullptr)
			return false;
		const bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
		fclose(f);
		return ok;
	}

	bool contains(const std::string &s, const char *part)
	{
		return s.find(part) != std::string::npos;
	}

	// HiFi XI's folder (this exe is in its build folder)
	std::string root_folder()
	{
		char path[MAX_PATH] = "";
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		std::string s(path);
		for (int up = 0; up < 2; ++up)
		{
			const size_t at = s.find_last_of("\\/");
			s = at != std::string::npos ? s.substr(0, at) : std::string(".");
		}
		return s + "\\";
	}

	// The cave run on the image file: its lines printed, its exit code (-1 when it couldn't be run)
	int run_cave(const std::string &exe, const std::string &image)
	{
		const std::string command = "\"\"" + exe + "\" \"" + image + "\"\""; // (cmd /c takes the outer quotes off)
		FILE *p = _popen(command.c_str(), "r");
		if (p == nullptr)
			return -1;
		char line[512];
		while (fgets(line, sizeof(line), p) != nullptr)
		{
			std::string s(line);
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
				s.pop_back();
			std::printf("  32-bit: %s\n", s.c_str());
		}
		return _pclose(p);
	}

	// One dump: everything t_gamerain.py checks of the addon, on the port
	void test_dump(const std::string &path, const std::string &cave_image)
	{
		namespace gamerain = hifi::game::gamerain;
		const std::string file = path.substr(path.find_last_of('\\') + 1);
		std::printf("\n%s\n", file.c_str());
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		char line[256];
		std::snprintf(line, sizeof(line), "mapped at its base, 0x%08X", unsigned(base));
		check(size != 0, line);
		if (size == 0)
			return;

		const std::vector<uintptr_t> hits = find_all(base, size, kPattern);
		std::snprintf(line, sizeof(line), "the pattern is unique in %s, at 0x%06X (the mov at +0x16)", file.c_str(), unsigned(kSite - 0x16));
		check(hits.size() == 1 && hits[0] == base + kSite - 0x16, line);
		const uintptr_t site = base + kSite;
		const std::string original = bytes_at(site, 6);
		check(original == from_hex("8A857A010000") && bytes_at(site + 6, 4) == from_hex("84C00F85"),
			"the dump's 04609D: mov al, [ebp+17Ah]; test al, al; jne");
		check(gamerain::status() == "not hidden yet" && gamerain::where().memory == 0, "before the route: not hidden yet, no memory");

		// The route in
		std::string why;
		const bool in = gamerain::route(true, why);
		check(in && why.empty(), "route(true): in" + (why.empty() ? std::string() : " (" + why + ")"));
		const uintptr_t mem = gamerain::where().memory;
		check(mem == dump::kAllocAt && gamerain::where().site == site, "its memory at 0x30000000, the site found at 04609D");
		const std::string call = bytes_at(site, 6);
		const uint32_t target = uint32_t(site + 5 + word_at(site + 1));
		check(uint8_t(call[0]) == 0xE8 && target == mem + kCode && uint8_t(call[5]) == 0x90, "04609D calls the routine (then a nop)");
		check(bytes_at(site + 6, 32) == file_bytes(path, kSite + 6, 32) && bytes_at(site - 32, 32) == file_bytes(path, kSite - 32, 32),
			"only those 6 bytes changed around it");
		check(bytes_at(mem + kCode, kCodeSize) == from_hex(kRoutine), "the routine is as written out here, byte for byte (224 bytes)");
		// (its page past it too: nothing written beyond the routine's memory)
		check(bytes_at(mem + kCode + kCodeSize, 0x1000 - kCode - kCodeSize) == std::string(0x1000 - kCode - kCodeSize, '\0'),
			"  and nothing past it (to the end of its page)");
		check(all_word(mem) == 0 && pairs(mem).empty(), "  its all word 0, its list empty");

		// The all word and the list in each case, as World FX says (asked at every update)
		drawn = kRain | kSnow;
		gamerain::update(0);
		check(pairs(mem).empty() && all_word(mem) == 0 && gamerain::status() == "drawn: no zone yet",
			"no zone yet: empty list, all word 0 (" + gamerain::status() + ")");

		gamerain::update(104);
		check(pairs(mem) == full_list() && all_word(mem) == 0,
			"World FX drawing its rain and snow: the list is the rain's and the snow's (" + std::to_string(pairs(mem).size()) + " pairs), all word 0");
		check(gamerain::status() == "hidden while World FX draws its own here: rain and snow (0.0 of its effects skipped a frame)",
			"  status: " + gamerain::status());

		drawn = kSnow;
		gamerain::update(104);
		check(pairs(mem) == kSnowList && rest_empty(mem) && all_word(mem) == 0, "World FX drawing its snow alone: only the snow's entries (the rest of the list empty)");
		check(contains(gamerain::status(), "snow (World FX's Rain setting is off: rain drawn)"), "  status: " + gamerain::status());
		drawn = kRain;
		gamerain::update(104);
		check(pairs(mem) == kRainList && rest_empty(mem) && all_word(mem) == 0, "World FX drawing its rain alone: only the rain's entries");
		check(contains(gamerain::status(), "rain (World FX's Snow setting is off: snow drawn)"), "  status: " + gamerain::status());
		drawn = 0;
		gamerain::update(104);
		check(pairs(mem).empty() && rest_empty(mem) && all_word(mem) == 0, "World FX drawing none of the weather: empty list, all word 0");
		check(gamerain::status() == "drawn: World FX draws none of the weather here", "  status: " + gamerain::status());
		drawn = hifi::game::kWeatherFxSky | hifi::game::kWeatherFxClouds | hifi::game::kWeatherFxLightning | hifi::game::kWeatherFxDust |
			hifi::game::kWeatherFxWind | hifi::game::kWeatherFxHeat | hifi::game::kWeatherFxAurora | hifi::game::kWeatherFxGloom;
		gamerain::update(104);
		check(pairs(mem).empty() && all_word(mem) == 0 && gamerain::status() == "drawn: World FX draws none of the weather here",
			"every other part of the weather but not all of it (sky, clouds, lightning...): nothing left out (only kWeatherFxAll takes the sky)");
		drawn = kRain | kSnow;
		gamerain::update(104);
		check(pairs(mem) == full_list() && all_word(mem) == 0, "rain and snow again: the whole list is back");

		// World FX decides now: its own Rain and Snow settings and its rain maps aren't looked at any more
		settings["Rain"] = "0";
		settings["Snow"] = "0";
		settings["RainAmount"] = "0.000000";
		gamerain::update(104);
		check(pairs(mem) == full_list(), "World FX's Rain=0, Snow=0, RainAmount=0 but its export says rain and snow: the whole list");
		gamerain::update(100);
		check(pairs(mem) == full_list(), "zone 100 (no rain map in World FX's folder, which isn't known either): the whole list");
		settings.clear();

		// All mode: the all word set, the list emptied; status and the rate
		drawn = kAll;
		gamerain::update(104);
		check(all_word(mem) == 1 && pairs(mem).empty() && rest_empty(mem), "World FX drawing all of the weather: all word 1, the list empty");
		check(gamerain::status() == "all of the game's weather left out while World FX draws its own here (0.0 of its effects skipped a frame)",
			"  status: " + gamerain::status());
		drawn = 0xFFFFFFFFu;
		gamerain::update(104);
		check(all_word(mem) == 1 && pairs(mem).empty() && contains(gamerain::status(), "all of the game's weather left out"),
			"every bit set (all, rain, snow...): all mode, the list empty");
		drawn = kAll | kRain;
		gamerain::update(104);
		*reinterpret_cast<volatile uint32_t *>(mem) += 30;
		for (int i = 0; i < 10; ++i)
			gamerain::count_frame();
		gamerain::update(104);
		check(contains(gamerain::status(), "all of the game's weather left out while World FX draws its own here (3.0 of its effects skipped a frame)"),
			"  30 skipped over 10 frames: " + gamerain::status());
		drawn = kRain | kSnow;
		gamerain::update(104);
		check(all_word(mem) == 0 && pairs(mem) == full_list(), "all back to rain and snow: all word 0, the whole list");
		drawn = kAll;
		gamerain::update(104);
		check(all_word(mem) == 1 && pairs(mem).empty(), "and all again");
		drawn = 0;
		gamerain::update(104);
		check(all_word(mem) == 0 && pairs(mem).empty() && gamerain::status() == "drawn: World FX draws none of the weather here",
			"all to nothing: all word 0, empty list (" + gamerain::status() + ")");
		drawn = kAll;
		gamerain::update(0);
		check(all_word(mem) == 0 && gamerain::status() == "drawn: no zone yet", "all with no zone yet: all word 0");
		drawn = kRain | kSnow;
		gamerain::update(104);

		// Elements skipped a frame: 30 over 10 frames
		*reinterpret_cast<volatile uint32_t *>(mem) += 30;
		for (int i = 0; i < 10; ++i)
			gamerain::count_frame();
		gamerain::update(104);
		check(contains(gamerain::status(), "(3.0 of its effects skipped a frame)"), "30 skipped over 10 frames: " + gamerain::status());
		gamerain::update(104);
		check(contains(gamerain::status(), "(3.0 of its effects"), "  an update with no frame since: the rate kept");
		for (int i = 0; i < 5; ++i)
			gamerain::count_frame();
		gamerain::update(104);
		check(contains(gamerain::status(), "(0.0 of its effects"), "  5 frames with none skipped: 0.0");

		// The route out and in again, in all mode
		drawn = kAll;
		gamerain::update(104);
		check(gamerain::route(false, why) && why.empty(), "route(false) in all mode: out");
		check(bytes_at(site, 6) == original, "  the game's 6 bytes are back");
		gamerain::update(104);
		check(pairs(mem).empty() && rest_empty(mem) && all_word(mem) == 0, "  the all word cleared, the list empty");
		check(gamerain::status() == "drawn (not left out: the \"The game's own\" tab of HiFi XI's menu)", "  status: " + gamerain::status());
		check(gamerain::seen() == "nothing seen: the game's weather isn't being sorted (not left out: the \"The game's own\" tab of HiFi XI's menu)",
			"  seen: " + gamerain::seen());
		check(gamerain::route(true, why) && why.empty(), "route(true): in again");
		gamerain::update(104);
		check(bytes_at(site, 6) == call && gamerain::where().memory == mem && all_word(mem) == 1 && pairs(mem).empty(),
			"  the call again, the same memory, the all word set");
		check(bytes_at(mem + kCode, kCodeSize) == from_hex(kRoutine), "  the routine as written out");

		// ... and in the list mode
		drawn = kRain | kSnow;
		gamerain::update(104);
		check(gamerain::route(false, why) && why.empty() && bytes_at(site, 6) == original, "route(false) in the list mode: out, the game's bytes back");
		gamerain::update(104);
		check(pairs(mem).empty() && rest_empty(mem) && all_word(mem) == 0, "  the list emptied");
		check(gamerain::route(true, why) && why.empty(), "route(true): in again");
		gamerain::update(104);
		check(bytes_at(site, 6) == call && gamerain::where().memory == mem && pairs(mem) == full_list() && all_word(mem) == 0,
			"  the call again, the same memory, the list filled");
		check(memory_free(dump::kAllocAt + 0x10000), "  no second allocation");

		// The routine's memory as the game would run it (the list filled, the all word 0: the cave sets it itself), for
		// the 32-bit run
		char head[8];
		const uint32_t at = uint32_t(mem), n = kSize;
		std::memcpy(head, &at, 4);
		std::memcpy(head + 4, &n, 4);
		check(write_file(cave_image, std::string(head, 8) + bytes_at(mem, kSize)), "the routine's memory written to " + cave_image);

		check(gamerain::seen() == "none yet", "seen with nothing drawn: " + gamerain::seen());
		const std::string ring = std::string("rainsmokboltligh") + std::string("rain\0\0\0\0", 8) + "snowhit3";
		std::memcpy(reinterpret_cast<void *>(mem + 0x90), ring.data(), 32);
		*reinterpret_cast<volatile uint32_t *>(mem + 4) = 4;
		check(gamerain::seen() == "bolt/ligh, rain/- (hidden), rain/smok (hidden), snow/hit3 (hidden)", "  after four: " + gamerain::seen());
		drawn = kAll;
		gamerain::update(104);
		check(gamerain::seen() == "bolt/ligh (hidden), rain/- (hidden), rain/smok (hidden), snow/hit3 (hidden)",
			"  in all mode, every one hidden: " + gamerain::seen());
		drawn = kRain | kSnow;
		gamerain::update(104);

		// Put back: not over something else's change
		const uint8_t nops[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		hifi::game::write_code(site, nops, sizeof(nops));
		const bool stopped = gamerain::stop(why);
		check(!stopped && why == "something else has changed the game's code since: left as it is", "stop() with the code changed since: " + why);
		check(bytes_at(site, 6) == std::string(6, '\x90') && gamerain::where().memory == mem && !memory_free(mem), "  the code left as it is, the memory kept");
		hifi::game::write_code(site, call.data(), call.size());
		check(gamerain::stop(why), "stop(): put back");
		check(dump::same_as_file(path, base, size), "  the image is the dump again, byte for byte");
		check(memory_free(mem) && gamerain::where().memory == 0 && gamerain::where().site == 0, "  the routine's memory freed");
		check(gamerain::status() == "not hidden yet", "  status: " + gamerain::status());
		dump::unmap(base);
	}
}

// What World FX draws of the weather now: as the test sets it
extern "C" uint32_t __cdecl worldfx_weather_drawn()
{
	return drawn;
}

extern "C" int __cdecl worldfx_setting(const char *key, char *out, uint32_t size)
{
	const auto v = settings.find(key != nullptr ? key : "");
	if (v == settings.end() || out == nullptr || size == 0)
		return 0;
	const size_t n = v->second.size() < size - 1 ? v->second.size() : size - 1;
	std::memcpy(out, v->second.data(), n);
	out[n] = 0;
	return 1;
}

// (HiFi XI's own options: none set here, each its default)
extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}

extern "C" const char *__cdecl worldfx_folder()
{
	return folder.c_str();
}

int main()
{
	namespace gamerain = hifi::game::gamerain;
	const std::string root = root_folder();
	const std::string cave_exe = root + "build\\test\\gamerain_cave.exe", cave_image = root + "build\\test\\gamerain_cave.bin";

	// Not found: said why, nothing taken (in either mode)
	std::vector<uint8_t> empty(0x10000, 0);
	hifi::game::set_image(reinterpret_cast<uintptr_t>(empty.data()), empty.size());
	std::string why;
	const bool routed = gamerain::route(true, why);
	check(!routed && why == "the game's effect draw wasn't found", "an image without the draw: " + why);
	drawn = kRain | kSnow;
	gamerain::update(104);
	check(gamerain::status() == "drawn: the game's effect draw wasn't found" && gamerain::where().memory == 0,
		"  status: " + gamerain::status() + " (no memory taken)");
	drawn = kAll;
	gamerain::update(104);
	check(gamerain::status() == "drawn: the game's effect draw wasn't found" && gamerain::where().memory == 0,
		"  World FX drawing all of the weather: " + gamerain::status());
	check(gamerain::stop(why), "  stop(): nothing to put back");

	for (const char *path : dump::kDumps)
	{
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		{
			check(false, std::string(path) + " is there");
			continue;
		}
		const int ok_before = passes, failed_before = failures;
		test_dump(path, cave_image);

		// The routine itself, 32-bit, as the game would run it
		if (GetFileAttributesA(cave_exe.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			check(false, cave_exe + " built (build.cmd)");
			continue;
		}
		const int code = run_cave(cave_exe, cave_image);
		check(code == 0, "the routine ran as the game would (32-bit): " + std::string(code == 0 ? "all passed" : "failures above"));
		std::printf("(%s: %d ok, %d failed)\n", path, passes - ok_before, failures - failed_before);
	}

	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures;
}
