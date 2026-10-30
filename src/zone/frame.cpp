/*
 * The zone passes' ring (frame.hpp): the game's thread fills a slot and publishes it with its seq (release);
 * dgVoodoo's thread finds it by the end marker's 4 bits (acquire). Static: 16 slots of up to 16,384 objects (20 MB);
 * the model actors' likewise (16 of up to 256).
 */

#include "frame.hpp"

namespace hifi::zone
{
	namespace
	{
		frame g_ring[kFrames];
		actor_frame g_actors[kFrames];
	}

	frame &frame_slot(uint32_t seq)
	{
		frame &f = g_ring[seq % kFrames];
		f.seq.store(0, std::memory_order_release); // (being filled: not to be read)
		return f;
	}

	const frame *published(uint32_t seq_low4)
	{
		const frame &f = g_ring[seq_low4 % kFrames];
		const uint32_t s = f.seq.load(std::memory_order_acquire);
		return s != 0 && (s % kFrames) == seq_low4 % kFrames ? &f : nullptr;
	}

	void publish(frame &f, uint32_t seq)
	{
		f.seq.store(seq, std::memory_order_release);
	}

	void clear_frames()
	{
		for (frame &f : g_ring)
			f.seq.store(0, std::memory_order_release);
	}

	actor_frame &actor_slot(uint32_t seq)
	{
		actor_frame &f = g_actors[seq % kFrames];
		f.seq.store(0, std::memory_order_release);
		return f;
	}

	const actor_frame *actor_published(uint32_t seq_low4)
	{
		const actor_frame &f = g_actors[seq_low4 % kFrames];
		const uint32_t s = f.seq.load(std::memory_order_acquire);
		return s != 0 && (s % kFrames) == seq_low4 % kFrames ? &f : nullptr;
	}

	void actor_publish(actor_frame &f, uint32_t seq)
	{
		f.seq.store(seq, std::memory_order_release);
	}

	void clear_actor_frames()
	{
		for (actor_frame &f : g_actors)
			f.seq.store(0, std::memory_order_release);
	}
}
