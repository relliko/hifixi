#include "fxframe.hpp"
#include <cstring>
#include <memory>

namespace hifi::fx
{
	namespace
	{
		frame *ring()
		{
			static std::unique_ptr<frame[]> slots(new frame[kFrames]);
			return slots.get();
		}
	}

	frame &frame_slot(uint32_t seq)
	{
		return ring()[seq % kFrames];
	}

	const frame *published(uint32_t seq_low4)
	{
		const frame &f = ring()[seq_low4 % kFrames];
		const uint32_t seq = f.seq.load(std::memory_order_acquire);
		return seq != 0 && (seq & 15u) == (seq_low4 & 15u) ? &f : nullptr;
	}

	void clear_frames()
	{
		frame *const r = ring();
		for (uint32_t i = 0; i < kFrames; ++i)
		{
			r[i].seq.store(0, std::memory_order_release);
			r[i].runs.store(0, std::memory_order_release);
		}
	}

	bool letter_blend(uint16_t letter, uint32_t &colour, uint8_t &blend)
	{
		const uint32_t l = letter & 0xFF, hi = uint32_t(letter) >> 8;
		switch (l)
		{
		case 0x00: blend = blend_one_zero; return true;
		case 0x49: blend = blend_add; return true;                // I
		case 0x46: case 0x47: blend = blend_darken; return true;  // F, G
		case 0x64: case 0x68:                                     // d, h
		{
			const uint32_t a = (uint32_t(letter) >> 7) & ~1u;
			colour = (colour & 0x00FFFFFFu) | (a < 255 ? a : 255) << 24;
			blend = l == 0x64 ? blend_alpha : blend_add;
			return true;
		}
		case 0x62: colour = (colour & 0x00FFFFFFu) | hi << 24; blend = blend_subtract; return true; // b
		case 0x41: case 0x42: case 0x43: blend = blend_subtract; return true; // A, B, C
		case 0x48: blend = blend_add; return true;                // H
		case 0x03: case 0x44: blend = blend_alpha; return true;   // D
		default: return false;
		}
	}

	void element_projection(const frame &f, const element &e, float out[16])
	{
		std::memcpy(out, f.proj, sizeof(f.proj));
		if (e.projection == proj_camera)
			return;
		if (e.projection == proj_screen)
		{
			out[0] = e.scale_x;
			out[5] = e.scale_y;
		}
		const float n = e.near_z, fz = e.far_z;
		if (n != fz)
		{
			out[10] = fz / (n - fz);
			out[14] = n * fz / (n - fz);
		}
	}
}
