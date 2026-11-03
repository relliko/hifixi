#pragma once

// The streamer's breadcrumbs (docs/texture-pack/streamer.md "The crash and hang section"): what the worker last did,
// as lock-free atomics (its state, last record, fence and ring written by the worker and start/stop; the counts, epoch
// and pause bits by whoever holds the streamer's mutex, publish_locked), read by pack::crash_section (memwatch.cpp) from
// the GPU crash report's thread pool thread or the hang report's, with the game's thread perhaps stuck, maybe inside
// the streamer's mutex. Header only: memwatch.cpp includes it without linking stream.cpp, so a plugin that doesn't
// start the streamer (M3) reads "streamer: not started".

#include <atomic>
#include <cstdint>

namespace hifi::pack::stream_crumbs
{
	// (64-bit atomics in this 32-bit process: lock-free on x86, so a report's thread never waits on one)
	static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free);

	// What the worker is doing
	enum : uint32_t
	{
		kNotStarted = 0,
		kIdle,
		kOpening,     // (pack::load)
		kMaking,      // (the ring, queue and lists, or a texture: device calls)
		kReading,     // (ReadFile into the ring)
		kRecording,   // (the copy list recorded and submitted)
		kRingWait,    // (waiting for ring space or a free list: the oldest copy's fence)
		kFenceWait,   // (nothing to start: waiting for a copy, a request or a present)
		kPaused,
		kLost,
		kStopping,
		kStopped,
		kFailed,      // (the GPU parts couldn't be made)
		kDeviceChange,
	};
	inline const char *state_text(uint32_t s)
	{
		static const char *const names[] = { "not started", "idle", "opening the pack", "making GPU objects", "reading", "recording a copy",
			"waiting for ring space", "waiting for a copy, a request or a present", "paused", "device lost", "stopping", "stopped",
			"failed (no ring, queue or lists)", "changing device" };
		return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
	}

	struct crumbs
	{
		std::atomic<uint32_t> started { 0 }, state { kNotStarted }, pause { 0 }, epoch { 0 };
		std::atomic<uint32_t> queued { 0 }, in_flight { 0 }, resident { 0 };
		std::atomic<uint64_t> resident_bytes { 0 };
		std::atomic<uint32_t> refused { 0 }; // (all reasons)
		// The last record the worker took: its key's hash, data file, offset and bytes (the levels' span), levels
		std::atomic<uint64_t> last_hash { 0 }, last_offset { 0 }, last_bytes { 0 };
		std::atomic<uint32_t> last_file { 0 }, last_first { 0 }, last_count { 0 };
		// The copy fence: the last value signalled, the last completed value the worker read
		std::atomic<uint64_t> signalled { 0 }, completed { 0 };
		std::atomic<uint64_t> ring_size { 0 }, ring_head { 0 }, ring_tail { 0 };
		std::atomic<uint64_t> progress_tick { 0 }; // (GetTickCount64 at the last state change)
	};
	// (constant-initialized: no guard, no lock on first use)
	inline constinit crumbs the;
}
