/*
 * The game's sky dome (dome.hpp), 03D640's arithmetic: the x87 code's floats where it stores them (f = j / n, 1 - f,
 * the elevations, the radii and heights of the edges), its products in double (exact where the x87's are: a byte times
 * a float).
 */

#include "dome.hpp"
#include <cmath>

namespace hifi::world::dome
{
	namespace
	{
		constexpr float kStrips = 12.048192977905273f;  // [0x32ABA8]
		constexpr float kQuarter = 1.570796012878418f;  // [0x32AB98]
		constexpr float kTurn = 6.2829999923706055f;    // [0x329D2C]

		uint32_t channel(uint32_t byte, float f)
		{
			const double v = double(byte) * double(f);
			return v > 0.0 ? uint32_t(int64_t(v)) & 0xFF : 0; // (_ftol truncates; 0x32 95D8 is 0.0)
		}
	}

	uint32_t scale(uint32_t c, float f)
	{
		return channel(c & 0xFF, f) | channel(c >> 8 & 0xFF, f) << 8 | channel(c >> 16 & 0xFF, f) << 16 | channel(c >> 24, f) << 24;
	}

	uint32_t band_strips(float h0, float h1)
	{
		const double d = (double(h1) - double(h0)) * double(kStrips);
		const int64_t n = std::isfinite(d) ? int64_t(d) : 0; // (_ftol)
		// (03D681: 0 -> 1; a falling band's negative count draws nothing (03D69D jle): never in the records)
		return n == 0 ? 1u : n < 0 ? 0u : uint32_t(n > 1024 ? 1024 : n);
	}

	void build(float radius, uint32_t segments, const uint32_t colours[8], const float heights[8], std::vector<strip> &strips, std::vector<vertex> &vertices)
	{
		strips.clear();
		vertices.clear();
		if (segments < 3 || segments > 1024 || !std::isfinite(radius))
			return;
		for (uint32_t b = 0; b + 1 < 8; ++b)
		{
			const float h0 = heights[b], h1 = heights[b + 1];
			const uint32_t n = band_strips(h0, h1);
			const float nf = float(n);
			for (uint32_t j = 0; j < n; ++j)
			{
				const float f1 = float(double(j) / double(nf)), f2 = float(double(j + 1) / double(nf));
				const float a1 = float(1.0 - double(f1)), a2 = float(1.0 - double(f2));
				strip s;
				s.band = b;
				s.e_lo = float(((1.0 - double(f1)) * double(h0) + double(f1) * double(h1)) * double(kQuarter));
				s.e_hi = float(((1.0 - double(f2)) * double(h0) + double(f2) * double(h1)) * double(kQuarter));
				// (03E1D0 adds the bytes as they are: the two shares never pass 255 together)
				const uint32_t lo0 = scale(colours[b], a1), lo1 = scale(colours[b + 1], f1);
				const uint32_t hi0 = scale(colours[b], a2), hi1 = scale(colours[b + 1], f2);
				s.c_lo = 0;
				s.c_hi = 0;
				for (int k = 0; k < 32; k += 8)
				{
					s.c_lo |= ((lo0 >> k & 0xFF) + (lo1 >> k & 0xFF) & 0xFF) << k;
					s.c_hi |= ((hi0 >> k & 0xFF) + (hi1 >> k & 0xFF) & 0xFF) << k;
				}
				const float r_lo = float(std::cos(double(s.e_lo)) * double(radius)), y_lo = -float(std::sin(double(s.e_lo)) * double(radius));
				const float r_hi = float(std::cos(double(s.e_hi)) * double(radius)), y_hi = -float(std::sin(double(s.e_hi)) * double(radius));
				s.first = uint32_t(vertices.size());
				vertices.push_back({ 0.0f, y_lo, r_lo, s.c_lo });
				vertices.push_back({ 0.0f, y_hi, r_hi, s.c_hi });
				for (uint32_t k = 1; k < segments; ++k)
				{
					const double a = double(int32_t(k)) * double(kTurn) / double(int32_t(segments));
					const double sa = std::sin(a), ca = std::cos(a);
					vertices.push_back({ float(sa * double(r_lo)), y_lo, float(ca * double(r_lo)), s.c_lo });
					vertices.push_back({ float(sa * double(r_hi)), y_hi, float(ca * double(r_hi)), s.c_hi });
				}
				vertices.push_back({ 0.0f, y_lo, r_lo, s.c_lo });
				vertices.push_back({ 0.0f, y_hi, r_hi, s.c_hi });
				strips.push_back(s);
			}
		}
	}
}
