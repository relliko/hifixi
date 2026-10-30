#pragma once

// The game's sky dome as 03D640 builds it (research/ffxi-re/client-notes.md "Sky", "The dome: 03D640"), on the CPU, so
// the native dome is the game's to the byte: between each pair of the 8 stops a band of max(1, ftol((h1 - h0) x
// 12.048)) strips; strip j of n from f1 = j / n to f2 = (j + 1) / n (floats), at elevations ((1 - f) h0 + f h1) x
// 1.570796; its edges' colours 03DFE0(c0, 1 - f) + 03DFE0(c1, f), each channel of each truncated to a whole byte
// (03DFE0: byte x the float, ftol, 0 when not above 0), the bytes added (03E1D0); a ring of segments facets, the vertices
// at azimuth k x 6.283 / segments (x r cos e sin a, y -r sin e, z r cos e cos a: y down), lower then upper edge, the
// first two again at the end; one triangle strip each.

#include <cstdint>
#include <vector>

namespace hifi::world::dome
{
	struct vertex
	{
		float x, y, z;
		uint32_t colour; // D3DCOLOR 0xAARRGGBB
	};

	// One strip: where its vertices start (2 x (segments + 1) of them), its edges
	struct strip
	{
		uint32_t first = 0;
		float e_lo = 0.0f, e_hi = 0.0f;    // elevations (radians)
		uint32_t c_lo = 0, c_hi = 0;       // the edges' colours
		uint32_t band = 0;
	};

	// 03DFE0: a D3DCOLOR's four bytes each times f, truncated (0 unless above 0)
	uint32_t scale(uint32_t colour, float f);
	// The strips in a band (at least 1; none for a falling one)
	uint32_t band_strips(float h0, float h1);

	// The dome's strips and vertices for 03D640's arguments (radius, 8 stops, segments around, the colours as D3DCOLORs,
	// the heights); nothing for segments under 3 (03DB60 draws no dome then) or more than 1024
	void build(float radius, uint32_t segments, const uint32_t colours[8], const float heights[8], std::vector<strip> &strips, std::vector<vertex> &vertices);
}
