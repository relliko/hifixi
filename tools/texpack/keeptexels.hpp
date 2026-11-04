#pragma once

// keeptexels.py (research\ffxi-re\tools\upscale) in C++ for hifixi-texpack make: the upscale corrected until shrinking
// it back gives the original again (iterative back-projection, per GRAIN x GRAIN original texels), the correction faded
// out at the upscale's strong edges (exp(-(grad/EDGE)^2), grad from its luma shrunk to the original's size and
// enlarged again bilinearly, wrapping). Doubles throughout and numpy's round half to even at the end, as the script:
// tests\texpack_test.cpp holds it within 1 level of the script's output on 20 pictures (tests\data\keeptexels).

#include <cstdint>
#include <vector>

namespace hifi::texpack::keeptexels
{
	constexpr int kRounds = 8;
	constexpr double kEdge = 24.0;
	constexpr uint32_t kGrain = 2;

	// orig: the original, RGBA8 w x h (its alpha unused). up: the upscale, RGBA8 (w*s) x (h*s), s a whole number (2 or
	// 4); its colour is replaced, its alpha kept. false when the sizes aren't a whole multiple
	bool apply(const uint8_t *orig, uint32_t w, uint32_t h, uint8_t *up, uint32_t W, uint32_t H, int rounds = kRounds, double edge = kEdge,
		uint32_t grain = kGrain);
}
