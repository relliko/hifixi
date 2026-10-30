/*
 * The world part's rings (frames.hpp), as zone/frame.cpp's: static, 16 slots each (the names' about 2.5 MB in all).
 */

#include "frames.hpp"

namespace hifi::world
{
	namespace
	{
		sky_frame g_sky[kSlots];
		names_frame g_names[kSlots];
		grid_frame g_grid[kSlots];

		template <typename T>
		T &slot(T (&ring)[kSlots], uint32_t seq)
		{
			T &f = ring[seq % kSlots];
			f.seq.store(0, std::memory_order_release); // (being filled: not to be read)
			return f;
		}

		template <typename T>
		const T *published(const T (&ring)[kSlots], uint32_t seq_low4)
		{
			const T &f = ring[seq_low4 % kSlots];
			const uint32_t s = f.seq.load(std::memory_order_acquire);
			return s != 0 && (s % kSlots) == seq_low4 % kSlots ? &f : nullptr;
		}
	}

	sky_frame &sky_slot(uint32_t seq) { return slot(g_sky, seq); }
	const sky_frame *sky_published(uint32_t seq_low4) { return published(g_sky, seq_low4); }
	void sky_publish(sky_frame &f, uint32_t seq) { f.seq.store(seq, std::memory_order_release); }

	names_frame &names_slot(uint32_t seq) { return slot(g_names, seq); }
	const names_frame *names_published(uint32_t seq_low4) { return published(g_names, seq_low4); }
	void names_publish(names_frame &f, uint32_t seq) { f.seq.store(seq, std::memory_order_release); }

	grid_frame &grid_slot(uint32_t seq) { return slot(g_grid, seq); }
	const grid_frame *grid_published(uint32_t seq_low4) { return published(g_grid, seq_low4); }
	void grid_publish(grid_frame &f, uint32_t seq) { f.seq.store(seq, std::memory_order_release); }

	void clear_frames()
	{
		for (sky_frame &f : g_sky)
			f.seq.store(0, std::memory_order_release);
		for (names_frame &f : g_names)
			f.seq.store(0, std::memory_order_release);
		for (grid_frame &f : g_grid)
			f.seq.store(0, std::memory_order_release);
	}
}
