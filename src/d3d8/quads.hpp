#pragma once

// The device's quad batch (device::kBatch, docs/frame-rate.md): which DrawPrimitiveUP calls it takes, and how a quad goes
// into the list it draws them as. Kept apart from the device so d3d8_unit checks them without one.

#include <d3d8.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hifi::d3d8
{
	// A quad as the interface and the text draw them: a two-triangle strip of 12 to 64 bytes a vertex, whole dwords. Flat
	// shading is left as it is: a list names each triangle's first vertex differently from a strip.
	inline bool batchable_quad(D3DPRIMITIVETYPE type, UINT count, UINT stride, DWORD shade_mode)
	{
		return type == D3DPT_TRIANGLESTRIP && count == 2 && stride >= 12 && stride <= 64 && stride % 4 == 0 && shade_mode != D3DSHADE_FLAT;
	}

	// The strip v0 v1 v2 v3 onto the list as v0 v1 v2, v2 v1 v3: the same two triangles, windings and order (as hifps's
	// proxy made them)
	inline void append_quad(std::vector<uint8_t> &list, const void *strip, UINT stride)
	{
		const uint8_t *v = static_cast<const uint8_t *>(strip);
		for (const int i : { 0, 1, 2, 2, 1, 3 })
			list.insert(list.end(), v + size_t(i) * stride, v + size_t(i + 1) * stride);
	}
}
