#include "vistest.hpp"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vistest
{
	namespace
	{
		struct pending
		{
			box b;
			uint32_t order; // (where in the asking order it came first)
		};
		struct known
		{
			int32_t value = -1;
			uint64_t asked_ms = 0;  // last asked for (answers not asked for kRememberMs are forgotten)
		};
		constexpr uint64_t kForgetMs = 2000; // (unasked longer: its answer is stale, kept for when it comes back)

		std::mutex mutex;
		std::unordered_map<uint32_t, pending> asked;
		std::vector<uint32_t> asked_order; // the objects in asked, in the order first asked
		std::unordered_map<uint32_t, known> answers;
		uint64_t last_forget_ms = 0;
		uint64_t waiting_since_ms = 0; // (the first test asked since the last answer came in; 0 none)
		stats counts;
	}

	int32_t ask(uint32_t object, const box &b, uint64_t now_ms)
	{
		std::lock_guard<std::mutex> lock(mutex);
		counts.asked++;
		if (waiting_since_ms == 0)
			waiting_since_ms = now_ms;
		if (b.kind < kinds)
			counts.by_kind[b.kind]++;
		auto p = asked.find(object);
		if (p != asked.end())
			p->second.b = b;
		else
		{
			asked.emplace(object, pending { b, uint32_t(asked_order.size()) });
			asked_order.push_back(object);
		}
		known &k = answers[object];
		if (k.value >= 0 && now_ms - k.asked_ms > kForgetMs)
			counts.stale++;
		k.asked_ms = now_ms;
		if (k.value < 0)
		{
			counts.unknown++;
			return kUnknown;
		}
		return k.value;
	}

	bool live(uint64_t now_ms)
	{
		std::lock_guard<std::mutex> lock(mutex);
		return (counts.last_answer_ms != 0 && now_ms - counts.last_answer_ms <= kLiveMs) ||
			(waiting_since_ms != 0 && now_ms - waiting_since_ms <= kLiveMs);
	}

	uint64_t overdue_ms(uint64_t now_ms)
	{
		std::lock_guard<std::mutex> lock(mutex);
		const uint64_t since = std::max(counts.last_answer_ms, waiting_since_ms);
		return since != 0 && now_ms > since ? now_ms - since : 0;
	}

	uint32_t take(uint32_t objects[kMaxBoxes], box boxes[kMaxBoxes])
	{
		std::lock_guard<std::mutex> lock(mutex);
		uint32_t n = 0;
		for (uint32_t object : asked_order)
		{
			if (n == kMaxBoxes)
			{
				counts.dropped++;
				continue;
			}
			objects[n] = object;
			boxes[n] = asked[object].b;
			n++;
		}
		asked.clear();
		asked_order.clear();
		counts.taken += n;
		return n;
	}

	void answer(const uint32_t *objects, const uint32_t *points, uint32_t n, uint64_t now_ms)
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (uint32_t i = 0; i < n; ++i)
		{
			auto a = answers.find(objects[i]);
			if (a == answers.end())
				continue; // (forgotten since: not asked for a while)
			a->second.value = int32_t(points[i] < kPoints ? points[i] : kPoints) * 4;
			counts.answered++;
		}
		if (n != 0)
		{
			counts.last_answer_ms = now_ms;
			waiting_since_ms = 0;
		}
		if (now_ms - last_forget_ms > 1000)
		{
			last_forget_ms = now_ms;
			// (kept kRememberMs since last asked; past kMaxRemembered, those not asked for kForgetMs go too)
			const bool full = answers.size() > kMaxRemembered;
			for (auto a = answers.begin(); a != answers.end();)
			{
				const uint64_t idle = now_ms - a->second.asked_ms;
				a = idle > kRememberMs || (full && idle > kForgetMs) ? answers.erase(a) : std::next(a);
			}
		}
	}

	bool off_screen(const box &b)
	{
		// (the game's: _ftol truncates toward zero; sorted; kept within 0 .. the viewport's width and height)
		const double x0 = std::trunc(double(b.x0)), y0 = std::trunc(double(b.y0)), x1 = std::trunc(double(b.x1)), y1 = std::trunc(double(b.y1));
		const double lx = std::max(std::min(x0, x1), 0.0), hx = std::min(std::max(x0, x1), std::trunc(double(b.width)));
		const double ly = std::max(std::min(y0, y1), 0.0), hy = std::min(std::max(y0, y1), std::trunc(double(b.height)));
		return hx - lx <= 0.0 || hy - ly <= 0.0;
	}

	void reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		asked.clear();
		asked_order.clear();
		answers.clear();
		counts.last_answer_ms = 0;
		waiting_since_ms = 0;
	}

	stats get_stats()
	{
		std::lock_guard<std::mutex> lock(mutex);
		stats s = counts;
		s.objects = uint32_t(answers.size());
		return s;
	}
}
