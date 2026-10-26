// The characters' frame ring (charframe.hpp)

#include "charframe.hpp"
#include <memory>

namespace hifi::chars
{
	namespace
	{
		std::unique_ptr<frame[]> g_frames;
		frame *frames()
		{
			if (!g_frames)
				g_frames.reset(new frame[kFrames]);
			return g_frames.get();
		}
	}

	frame &frame_slot(uint32_t seq)
	{
		return frames()[seq % kFrames];
	}

	const frame *published(uint32_t seq_low4)
	{
		if (!g_frames)
			return nullptr;
		const frame &f = g_frames[seq_low4 % kFrames];
		const uint32_t seq = f.seq.load(std::memory_order_acquire);
		return seq != 0 && (seq & 15) == (seq_low4 & 15) ? &f : nullptr;
	}

	frame &begin_frame(uint32_t seq)
	{
		frame &f = frame_slot(seq);
		f.seq.store(0, std::memory_order_release);
		f.opaque.store(0, std::memory_order_relaxed);
		f.see_through.store(0, std::memory_order_relaxed);
		f.models = f.instances = f.meshes = f.bones = f.dropped = 0;
		f.camera = false;
		f.seq.store(seq, std::memory_order_release);
		return f;
	}

	void publish_opaque(frame &f)
	{
		f.opaque.store(f.models, std::memory_order_release);
	}

	void publish_see_through(frame &f)
	{
		f.see_through.store(f.models - f.opaque.load(std::memory_order_relaxed), std::memory_order_release);
	}

	void clear_frames()
	{
		if (!g_frames)
			return;
		for (uint32_t i = 0; i < kFrames; ++i)
		{
			g_frames[i].seq.store(0, std::memory_order_release);
			g_frames[i].opaque.store(0, std::memory_order_relaxed);
			g_frames[i].see_through.store(0, std::memory_order_relaxed);
		}
	}
}
