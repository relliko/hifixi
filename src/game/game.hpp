#pragma once

// What HiFi XI's game-side parts share (src/game: what the worldfx Ashita addon did, in C++): FFXiMain.dll's code found
// by pattern, the game's memory read without crashing, its code changed and put back, memory of our own for a routine,
// and World FX's settings and exports (worldfx_core and the backend, in this same DLL).
//
// Everything here runs on the game's thread (Ashita's present and packet callbacks), as the addon did.

#include <cstddef>
#include <cstdint>
#include <string>

namespace hifi::game
{
	// FFXiMain.dll's image: where the patterns are looked for (the tests set a dump mapped at its base instead)
	void set_image(uintptr_t base, size_t size);
	bool image_known();
	// A pattern (hex bytes, ?? for any byte, as Ashita's FindPattern takes them) in the image: where it starts plus
	// offset, 0 when it isn't there
	uintptr_t find(const char *pattern, intptr_t offset = 0);

	// Reads that can't crash (ReadProcessMemory on this process: memory freed under us is a failed read): false when
	// any byte of it couldn't be read
	bool read(uintptr_t at, void *out, size_t size);
	template <typename T>
	bool read(uintptr_t at, T &out)
	{
		return read(at, &out, sizeof(T));
	}
	// A word, 0 when it can't be read
	uint32_t word(uintptr_t at);
	// A pointer worth following: in the process's space, aligned, not null
	inline bool pointer(uint32_t a) { return a >= 0x10000 && a < 0xFFFF0000u && a % 4 == 0; }
	// A float from its bits; false for a NaN or an infinity
	bool float_of(uint32_t bits, float &out);

	// The game's code written (unprotected, written, protection put back, the instruction cache told); false when it
	// couldn't be
	bool write_code(uintptr_t at, const void *bytes, size_t size);
	// Whether the code at `at` is these bytes
	bool code_is(uintptr_t at, const void *bytes, size_t size);
	// Memory of our own for routines (zeroed, executable). Freed only when nothing calls into it any more. (The tests
	// set where it goes: the routines hold its 32-bit address.)
	uintptr_t alloc_code(size_t size);
	void free_code(uintptr_t at);
	void set_alloc_at(uintptr_t at);

	// x86: the target of the rel32 call or jump whose opcode is at `at`; a little-endian word into bytes; a 4-character
	// name as the game keeps it in a word
	uint32_t rel_target(uintptr_t at);
	void put32(uint8_t *to, uint32_t value);
	uint32_t name_value(const char *four);
	std::string name_text(uint32_t value); // (its 4 characters, odd ones as '.', trailing spaces cut)

	// The game's entities (Ashita's IEntity in the plugin; a fake in the tests): positions are Ashita's (x east,
	// y north, z height down: the game's x, z, y)
	struct entities
	{
		virtual ~entities() = default;
		static constexpr uint32_t kCount = 2304;
		virtual const char *name(uint32_t i) const = 0; // (nullptr or "" when none)
		virtual bool position(uint32_t i, float &x, float &y, float &z) const = 0;
		virtual bool shown(uint32_t i) const = 0;        // (render flags 0, bit 0x200: drawn)
		virtual uint32_t server_id(uint32_t i) const = 0;
		virtual uint32_t status(uint32_t i) const = 0;   // (1: engaged, fighting)
		virtual float model_size(uint32_t i) const = 0;
	};

	// World FX's settings as HiFi XI keeps them: a flag (on unless 0 or false), a number; the fallback when not set
	bool setting_on(const char *key, bool fallback);
	float setting_number(const char *key, float fallback);
	// HiFi XI's own options, in a section of its settings file ([hifps], [drawcull]): a flag, a number, the text; the
	// fallback when not set. Set: written as text (0/1 for a flag, a whole number as one, else the shortest text that
	// reads back as the same float).
	bool option_on(const char *section, const char *key, bool fallback);
	float option_number(const char *section, const char *key, float fallback);
	std::string option_text(const char *section, const char *key, const char *fallback);
	void set_option_on(const char *section, const char *key, bool on);
	void set_option_number(const char *section, const char *key, float value);
	// The folder World FX's zone files are in (worldfx-water\, worldfx-rain\...), with a slash at the end
	std::string data_folder();
	// Whether World FX has a zone file of a kind (worldfx-water, worldfx-rain) for a zone
	bool has_zone_file(const char *kind, uint32_t zone);
}

// World FX's own functions (worldfx_core and the backend, in this DLL): what the addon called through GetProcAddress
extern "C"
{
	void __cdecl worldfx_grass_actors2(const float *xyzr, const uint32_t *flags, uint32_t count);
	void __cdecl worldfx_grass_cut(const float *swaths, uint32_t count);
	void __cdecl worldfx_lamps(const uint32_t *words, uint32_t count);
	void __cdecl worldfx_glows(const uint32_t *words, uint32_t count);
	void __cdecl worldfx_weather(const uint32_t *words, uint32_t count);
	uintptr_t __cdecl worldfx_vis_entry(uint32_t kind);
	void __cdecl worldfx_vis_setup(uintptr_t original, uint32_t on);
	void __cdecl worldfx_vis_stats(uint32_t out[12]);
	int __cdecl worldfx_setting(const char *key, char *out, uint32_t size);
	uint32_t __cdecl worldfx_casts_characters();
	const char *__cdecl worldfx_folder();
	// HiFi XI's own options (the backend; the tests stub them)
	int __cdecl hifixi_option(const char *section, const char *key, char *out, uint32_t size);
	void __cdecl hifixi_set_option(const char *section, const char *key, const char *value);
}
