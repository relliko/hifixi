#include "frame2d.hpp"

namespace hifi::ui2d
{
	namespace
	{
		frame *ring()
		{
			static frame *const all = new frame[kFrames]; // (about 11 MB: kept off the image's own data)
			return all;
		}
	}

	frame &begin(uint32_t seq)
	{
		frame &f = ring()[seq % kFrames];
		f.seq.store(0, std::memory_order_release);
		f.segments.store(0, std::memory_order_release);
		f.draw_count = 0;
		f.vertex_count = 0;
		f.seq.store(seq, std::memory_order_release);
		return f;
	}

	const frame *published(uint32_t seq_low4)
	{
		const frame *f = &ring()[seq_low4 % kFrames];
		const uint32_t seq = f->seq.load(std::memory_order_acquire);
		return seq != 0 && (seq & 15u) == (seq_low4 & 15u) ? f : nullptr;
	}

	bool publish(frame &f, const segment &s)
	{
		const uint32_t n = f.segments.load(std::memory_order_relaxed);
		if (n >= kMaxSegments)
			return false;
		f.seg[n] = s;
		f.segments.store(n + 1, std::memory_order_release);
		return true;
	}

	void clear()
	{
		for (uint32_t i = 0; i < kFrames; ++i)
		{
			ring()[i].seq.store(0, std::memory_order_release);
			ring()[i].segments.store(0, std::memory_order_release);
		}
	}
}
