#pragma once

// BC7 colour for hifixi-texpack make (docs/texture-pack-plan.md M8; docs/texture-pack/upscaler.md "BC7"): our own mip
// chain as 8-bit RGBA (the coverage-correct cut-outs of bc2::full_chain, without BC2's 4-bit alpha), written as
// R8G8B8A8_UNORM DDS files with their mips, compressed to BC7_UNORM by Microsoft's texconv.exe (DirectXTex) as a child
// process on a batch (texconv keeps a DDS's own mips when no -m is given: checked by texpack_test), read back, and a
// BC7 decoder for the soul test, verify and the tests. The partition tables are the ones texconv's own decoder uses
// (derived from it 2026-11-04 and checked against it on random blocks of every mode by texpack_test).

#include "upscale.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hifi::texpack::bc7
{
	// One 16-byte block to 16 RGBA8 texels (row by row); a reserved mode (byte 0 zero) gives zeros, as D3D's
	void decode_block(const uint8_t *block, uint8_t out[16][4]);
	// Tight BC7 blocks (w and h multiples of 4) to RGBA8
	std::vector<uint8_t> decode(const uint8_t *blocks, uint32_t w, uint32_t h);
	// A level of a pack format (BC2 or BC7) to RGBA8; empty for another format
	std::vector<uint8_t> decode_level(uint32_t format, const uint8_t *blocks, uint32_t w, uint32_t h);

	// Our mip chain as RGBA8, bc2::chain_levels(w, h) levels, level 0 first: level 0's alpha held to [alpha_lo,
	// alpha_hi] (the original's range: Lanczos rings past it, and the game's alpha is raw 0..0x80), each next level the
	// last averaged 2x2 (bc2::full_chain's box), a cut-out's (pack kAlphaCut) alpha scaled per level so the fraction of
	// texels above cut_threshold stays level 0's (8-bit alpha here: BC7 keeps it), never past alpha_hi
	std::vector<std::vector<uint8_t>> rgba_chain(std::vector<uint8_t> level0, uint32_t w, uint32_t h, uint8_t alpha_kind, float cut_threshold,
		uint8_t alpha_lo, uint8_t alpha_hi);

	// A DDS (DX10 header, R8G8B8A8_UNORM, these mips) written; false (why)
	bool write_rgba_dds(const std::wstring &path, const std::vector<std::vector<uint8_t>> &levels, uint32_t w, uint32_t h, std::string &why);
	// A block-compressed DDS read back: its format must be format (DXGI), its size w x h and its mip count levels; out:
	// each level's tight blocks. false (why) otherwise
	bool read_bc_dds(const std::wstring &path, uint32_t format, uint32_t w, uint32_t h, uint32_t levels, std::vector<std::vector<uint8_t>> &out,
		std::string &why);

	// texconv's options for BC7 (chosen by measuring speed against PSNR: docs/texture-pack/upscaler.md "BC7")
	extern const wchar_t *kTexconvOptions;

	struct compress_options
	{
		std::wstring texconv;    // texconv.exe
		std::wstring list;       // a text file of the input DDS files, one a line
		std::wstring cwd;        // texconv's working folder (empty: texconv's own). texconv reads the list byte by byte as
		                         // the classic locale (no UTF-8), so make writes names relative to this, plain ASCII
		std::wstring out;        // where the BC7 files go (same names)
		std::wstring log;        // its console output, appended
		std::wstring extra;      // (tests and measuring: more options, e.g. -nogpu or -bc x)
		double poll_s = 3.0, stall_s = 900.0;
		std::function<std::string()> should_end; // as upscale::run_options'
	};
	// texconv -nologo -y -f BC7_UNORM -dx10 <kTexconvOptions> -o out -flist list, in a job object at below-normal
	// priority (upscale::run_program): as upscale::run
	upscale::end compress(const compress_options &o, std::string &why);
}
