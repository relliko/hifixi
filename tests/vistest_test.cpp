/*
 * Offline test of the visibility tests' answers (src/render/vistest.cpp), the cache the game's calls are answered from
 * (worldfx_core's entries), with the clock given by the test:
 *   - a test object with no answer yet is answered kUnknown (hidden) at once, counted as unknown, and its box goes to
 *     the GPU at the next take;
 *   - an answer read back is what the next ask gets (4 a point, at most 256); a newer one replaces it;
 *   - an object not asked for a while (past 2 s, under a minute) keeps its last answer: asked again, it gets that one
 *     (counted stale) until a new one is in; past a minute unasked it's forgotten and is unknown again;
 *   - an answer for a forgotten object is dropped; reset() forgets everything and answers stop being live;
 *   - live(): not before any answer, then for kLiveMs (2 s) after the last; after a quiet while with nothing asked,
 *     live again from the next test asked, until it's been kLiveMs with nothing back;
 *   - over 8192 objects kept, those not asked for 2 s go at the next sweep, the ones asked lately stay;
 *   - take(): the newest box per object, in the order first asked, at most 256 (the rest dropped, asked again later).
 *
 *   build\vistest_test.exe     (built by build.cmd)
 */

#include "../src/render/vistest.hpp"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	vistest::box box_of(float x, uint32_t kind = vistest::glow)
	{
		return { x, 10.0f, x + 40.0f, 50.0f, 0.5f, 640.0f, 480.0f, kind };
	}

	// Takes what was asked and answers each object with its points
	void answer_all(uint32_t points, uint64_t now)
	{
		uint32_t objects[vistest::kMaxBoxes] = {};
		vistest::box boxes[vistest::kMaxBoxes] = {};
		const uint32_t n = vistest::take(objects, boxes);
		std::vector<uint32_t> p(n, points);
		vistest::answer(objects, p.data(), n, now);
	}
}

int main()
{
	using namespace vistest;
	uint64_t t = 100000;

	check(!live(t), "no answers yet: not live (the game's own test runs)");
	check(ask(1, box_of(0), t) == kUnknown && kUnknown == 0, "a new object: answered kUnknown (hidden) at once, not -1");
	check(get_stats().unknown == 1 && get_stats().asked == 1, "  counted as unknown");
	answer_all(64, t + 30);
	check(live(t + 30) && live(t + 30 + kLiveMs) && !live(t + 31 + kLiveMs), "live for 250 ms after the last answer");
	check(ask(1, box_of(0), t + 40) == 256, "its answer read back: all 64 points, 256");
	answer_all(10, t + 60);
	check(ask(1, box_of(0), t + 70) == 40, "a newer answer replaces it (10 points: 40)");
	answer_all(99, t + 90);
	check(ask(1, box_of(0), t + 100) == 256, "points over 64 counted as 64");
	check(get_stats().unknown == 1 && get_stats().stale == 0, "  nothing unknown or stale since");


	// Unasked for 5 s: kept (the sweeps run at each answer, once a second)
	answer_all(64, t + 100); // (object 1, asked just above, answered as before: 256)
	ask(2, box_of(100), t + 2000);
	answer_all(16, t + 2100);
	for (uint64_t s = 1; s <= 5; ++s)
	{
		ask(2, box_of(100), t + 2100 + s * 1000);
		answer_all(16, t + 2100 + s * 1000 + 50);
	}
	check(ask(1, box_of(0), t + 7300) == 256, "back in view after 7 s unasked: its last answer, not unknown");
	check(get_stats().stale == 1 && get_stats().unknown == 2, "  counted stale (object 2 was unknown once)");
	answer_all(32, t + 7350);
	check(ask(1, box_of(0), t + 7400) == 128, "  until the new one is in");

	// Unasked past a minute: forgotten
	for (uint64_t s = 1; s <= 62; ++s)
	{
		ask(2, box_of(100), t + 7400 + s * 1000);
		answer_all(16, t + 7400 + s * 1000 + 50);
	}
	const uint64_t unknown_before = get_stats().unknown;
	check(ask(1, box_of(0), t + 70000) == kUnknown && get_stats().unknown == unknown_before + 1,
		"unasked over a minute: forgotten, unknown again (answered hidden, no waiting)");
	check(ask(2, box_of(100), t + 70010) == 64, "  object 2, asked all along: still known (16 points: 64)");

	// An answer for an object forgotten since it was taken: dropped
	{
		uint64_t now = t + 80000;
		answer_all(16, now - 10); // (what was asked before answered first: only object 3 is taken below)
		ask(3, box_of(200), now);
		uint32_t objects[kMaxBoxes] = {};
		box boxes[kMaxBoxes] = {};
		const uint32_t n = take(objects, boxes);
		for (uint64_t s = 1; s <= 62; ++s) // (object 3 not asked again for 62 s: swept)
		{
			ask(2, box_of(100), now + s * 1000);
			answer_all(16, now + s * 1000 + 50);
		}
		std::vector<uint32_t> p(n, 64);
		answer(objects, p.data(), n, now + 63000);
		check(n == 1 && ask(3, box_of(200), now + 63100) == kUnknown, "an answer for an object forgotten since: dropped, still unknown");
	}

	// take(): the newest box per object, in the order first asked, at most 256
	{
		reset();
		check(!live(t + 200000) && ask(1, box_of(0), t + 200000) == kUnknown, "reset(): everything forgotten, not live");
		uint64_t now = t + 200000;
		ask(7, box_of(1), now);
		ask(5, box_of(2), now);
		ask(7, box_of(3), now);
		for (uint32_t i = 0; i < 300; ++i)
			ask(1000 + i, box_of(float(i)), now);
		uint32_t objects[kMaxBoxes] = {};
		box boxes[kMaxBoxes] = {};
		const uint64_t dropped = get_stats().dropped;
		const uint32_t n = take(objects, boxes);
		check(n == kMaxBoxes && objects[0] == 1 && objects[1] == 7 && boxes[1].x0 == 3.0f && objects[2] == 5,
			"take(): in the order first asked, the newest box for each");
		check(get_stats().dropped == dropped + (3 + 300 - kMaxBoxes), "  over 256 dropped (asked again next frame)");
	}

	// Over 8192 kept: those not asked for 2 s go first
	{
		reset();
		uint64_t now = t + 300000;
		for (uint32_t i = 0; i < kMaxRemembered + 100; ++i)
			ask(50000 + i, box_of(0), now);
		uint32_t objects[kMaxBoxes] = {};
		box boxes[kMaxBoxes] = {};
		take(objects, boxes); // (empties the asked list: only the kept answers matter here)
		ask(7, box_of(0), now + 4000);
		uint32_t seven = 7, points = 64;
		answer(&seven, &points, 1, now + 4050);
		check(get_stats().objects == 1 && ask(7, box_of(0), now + 4100) == 256,
			"over 8192 kept: those unasked for 2 s swept, the one asked lately kept");
	}

	// A quiet while (nothing asked, so nothing answered): the next test asked is live, its last answer (or kUnknown),
	// not the game's waiting test (the stutters in game, 2026-10-27); asked on with nothing back for kLiveMs, not live
	{
		reset();
		const uint64_t q = t + 400000;
		ask(9, box_of(0), q);
		answer_all(64, q + 20);
		const uint64_t later = q + 20 + kLiveMs + 5000; // (long after that answer, nothing asked between)
		check(!live(later), "a quiet while with nothing asked: not live before anything is");
		check(ask(9, box_of(0), later) == 256 && live(later) && live(later + kLiveMs), "  the next test asked: live (its last answer) for kLiveMs with nothing back");
		check(!live(later + kLiveMs + 1), "  still nothing back after that: not live (the game's own test runs)");
		answer_all(64, later + kLiveMs + 10);
		check(live(later + kLiveMs + 10), "  answers back: live again");
	}

	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
