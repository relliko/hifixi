#pragma once

// When the screen's 2D goes to HiFi XI (src/ui2d/ui2d.cpp, each present): never before its Direct3D 12 side has shown
// it can draw the game's segments, and back to the game at once when it can't. A pure function, so it's tested on its own
// (tests/uidraw_test.cpp).

#include <cstdint>

namespace hifi::ui2d
{
	enum class gate_mode : uint32_t { off = 0, shadow = 1, native = 2 };

	struct gate_in
	{
		bool wanted = false;        // [screen2d] native
		bool shadow_only = false;   // [screen2d] shadow: recorded and checked, never drawn by HiFi XI
		bool patched = false;       // the loop tail's calls are ours
		bool renderer = false;      // the 2D renderer is made on dgVoodoo's device
		bool target = false;        // the target the markers draw into is known (its format: the pipelines)
		uint64_t now_ms = 0;
		uint64_t ok_at = 0;         // when a marker last came through and its segment could be drawn (0: never)
		uint64_t fail_at = 0;       // when a segment last couldn't be (0: never)
		uint64_t in_a_row = 0;      // segments that could be drawn since the last that couldn't
		gate_mode was = gate_mode::off;
	};

	constexpr uint64_t kGateQuietMs = 250;    // the markers stopped coming: the game draws again
	constexpr uint64_t kGateFailHoldMs = 2000; // after a segment couldn't be drawn, the game draws for this long
	constexpr uint64_t kGateTrusted = 30;      // segments in a row that could have been drawn before HiFi XI draws them

	constexpr gate_mode next_mode(const gate_in &in)
	{
		if (!in.wanted || !in.patched || !in.renderer)
			return gate_mode::off;
		const bool fail_recent = in.fail_at != 0 && in.now_ms - in.fail_at < kGateFailHoldMs;
		const bool ok_recent = in.ok_at != 0 && in.now_ms - in.ok_at < kGateQuietMs;
		if (!in.shadow_only && !fail_recent && ok_recent && in.target && (in.was == gate_mode::native || in.in_a_row >= kGateTrusted))
			return gate_mode::native;
		return gate_mode::shadow;
	}
}
