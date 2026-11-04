#pragma once

// The texture pack's height maps (plan M9; docs/texture-pack/format.md "Height records"): what texpack make --heights
// and texpack heights store as a BC4_UNORM record (kind 2) beside an entry's colour. Pure CPU, no device, no game
// code: the callers read the game's files and hand the texels in.
//
// What a height is: H = (s - 0.5) * span, s the record's texel (0..1, 0.5 the map's mean), span the record's float
// (record::param). H is in the game's own bump units, measured on the ORIGINAL texture's grid: the game makes its zone
// normal maps as normalize(sobel(bytes / 255), 1) (zonemesh::sobel, 01E0A0), so a map of the original's size gives the
// game's tilt with normalize(sobel(H), 1); a map n times as wide (the pack's colour is 2x or 4x) takes its own Sobel
// times n. Height changes only how light falls on the surface: the painted colour is never touched (no AO or cavity
// multiplied into it).
//
// Where it comes from, in this order (the plan: game heights first, so painted shading isn't taken for relief):
//   - from_game: the zone's bump DAT height map (chunk 0x5D, matched by the texture name's last 8 characters, as the
//     game matches it) resampled to the map's size, high-passed;
//   - from_colour: a port of research\ffxi-re\tools\nmmockup.py nmap_multi on the colour the texture is drawn with
//     (the pack's, else the original): brightness, the painted broad light and shade taken out with a high-pass at
//     kBroad original texels, then detail at three scales (0.6, 1.2, 2.5 map texels, weights 1, 0.7, 0.4) times
//     kRelief, scaled to original texels.
// Both are coverage-aware: texels an alpha test throws away (cut-out) or that are mostly see-through (blended) count
// for nothing in the blurs, the mean and the mips, so a leaf's edge doesn't take the height of the hole beside it.

#include <cstdint>
#include <string>
#include <vector>

namespace hifi::pack::height
{
	// nmap_multi's k, and its broad high-pass (Gaussian sigma, in the ORIGINAL texture's texels)
	constexpr float kRelief = 2.2f;
	constexpr float kBroad = 6.0f;

	// The map's resolution (open question 11; --height-scale): full = the colour it's drawn with (the pack's colour
	// record, or the original), half = half that but never under the original, game = the original's size
	enum class scale : uint8_t
	{
		half,
		full,
		game,
	};
	const char *name(scale s);
	bool parse(const std::string &text, scale &out);
	// The map's size for a colour of cw x ch made from a w x h original; false when it can't be stored (a side not a
	// multiple of 4)
	bool size_for(scale s, uint32_t cw, uint32_t ch, uint32_t w, uint32_t h, uint32_t &out_w, uint32_t &out_h);

	// A height field on the map's grid: H as above (float), and each texel's weight (0..1: how much of it the game
	// shows)
	struct field
	{
		uint32_t w = 0, h = 0;
		std::vector<float> v, wt;
	};

	// Each texel's weight from the colour's alpha (the game's raw alpha, 0x80 opaque): cut-out and mixed (pack.hpp
	// kAlphaCut, kAlphaMixed) 1 when alpha passes the test (above cut_threshold, bc2::zone_cut_threshold or
	// kCharsCutThreshold) else 0; blended alpha / 0x80 (at most 1); none and mask 1
	float weight_of(uint8_t alpha, uint8_t alpha_kind, float cut_threshold);

	// From the colour (RGBA8, cw x ch, rows top first) of a texture whose original is orig_w wide, onto a w x h map
	// (cw and ch whole multiples of w and h: the colour box-filtered down by its weights). false (why) on sizes that
	// don't fit
	bool from_colour(const uint8_t *rgba, uint32_t cw, uint32_t ch, uint32_t orig_w, uint32_t w, uint32_t h, uint8_t alpha_kind, float cut_threshold,
		field &out, std::string &why);
	// From the game's height bytes (gw x gh, rows top first, as the bump DAT's chunk 0x5D holds them from +0x20),
	// resampled (bilinear, wrapping: zone textures tile) onto a w x h map, high-passed at kBroad original texels; the
	// weights from the colour as from_colour's
	bool from_game(const uint8_t *bytes, uint32_t gw, uint32_t gh, const uint8_t *rgba, uint32_t cw, uint32_t ch, uint32_t orig_w, uint32_t w, uint32_t h,
		uint8_t alpha_kind, float cut_threshold, field &out, std::string &why);

	// A field made into the record's levels: centred on its weighted mean (0.5), span 2x the 99.9th percentile of |H|
	// over the texels that count (the rare ones past it clamped), coverage-aware 2x2 mips down to 4x4
	// (bc2::chain_levels), each level BC4. span 0: a flat map (all 0.5)
	struct made
	{
		std::vector<std::vector<uint8_t>> levels; // tight BC4 blocks, largest first
		float span = 0;
		uint32_t w = 0, h = 0;
	};
	made encode(const field &f);
	// Level 0's 8-bit texels before BC4 (the tests compare the decoded record with them)
	std::vector<uint8_t> texels(const field &f, float &span);

	// The game's normal for heights (bytes / 255 on the original's grid; zonemesh::sobel without the bytes): the
	// tests' measure of how far a map's tilt is from the game's. h is w x h; out gets 3 floats a texel
	void normals(const float *heights, uint32_t w, uint32_t h, float gain, std::vector<float> &out);
}
