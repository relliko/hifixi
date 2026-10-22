#pragma once

// hifps's sites.lua and sigs.lua in C++: finds the frame step's client offsets (step.cpp's site tables, written
// against the client in FFXiMain_04490000.bin) in the running client, so a client update that moves code doesn't stop
// the frame step. Each offset has a byte signature (sigs.lua, from hifps's tools/gen_sigs.py; copied in by
// tests/hifps_sigs_cpp.py): whole instructions around it, with '??' for what moves when code or data moves (rel32
// branch targets, 32-bit immediates and displacements that point into the image) and for the bytes the frame step
// rewrites (a site's call operand, a smoothing loop's immediate float), so a client already patched still matches.
//
// An offset is looked for where it was, then shifted like the nearest offset found so far, then (a call site) among
// all calls to the getters, then (anything else) everywhere in .text.
//
// (Only the frame step uses this: hifps's experimental toggles and graphics settings find their code by patterns of
// their own.)

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace hifi::game::hifps::sites
{
	// What a signature is for: a call to a step getter, a smoothing loop's immediate float, a loop's float operand
	enum class kind : uint8_t
	{
		site,
		imm,
		op
	};
	struct signature
	{
		uint32_t offset;     // in the analysed client
		kind what;
		uint32_t at;         // where the offset is in the pattern
		const char *pattern; // hex bytes, ?? for any
	};
	// sigs.lua's signatures, by offset
	const signature *signatures(size_t &count);

	struct found
	{
		std::map<uint32_t, uint32_t> map; // offset in the analysed client -> offset in this one (only those found)
		std::vector<uint32_t> extra;      // calls to a getter that no table names (new in this client), ascending
		uint32_t callers = 0;             // calls to the getters in all
	};

	// .text's offset range in an image at mem (from its PE header); false when it has none
	bool text(const uint8_t *mem, size_t size, uint32_t &lo, uint32_t &hi);
	// Offsets of every 'call rel32' in [lo, hi) whose target is one of the getters (offsets)
	std::vector<uint32_t> callers(const uint8_t *mem, uint32_t lo, uint32_t hi, uint32_t getter1, uint32_t getter2);
	// The tables' offsets in the image at mem (FFXiMain.dll's base; the tests pass a dump), given the two step getters'
	// offsets
	found resolve(const uint8_t *mem, size_t size, uint32_t getter1, uint32_t getter2);

	// FFXiMain.dll's image that `inside` is in (the module in game, a dump mapped at its base in the tests): its base and
	// size (SizeOfImage), false when it isn't a PE image
	bool image_of(uintptr_t inside, uintptr_t &base, size_t &size);
	// Every place (up to max) a pattern (hex, ?? for any byte) starts in [base, base + size), in order
	std::vector<uintptr_t> find_all(uintptr_t base, size_t size, const char *pattern, size_t max);
}
