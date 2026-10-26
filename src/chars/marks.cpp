// The character markers read and checked (marks.hpp)

#include "marks.hpp"
#include "../game/slotmark.hpp"
#include <cstdio>
#include <mutex>

namespace hifi::chars::marks
{
	namespace
	{
		std::mutex mutex;
		counts n;
		uint32_t last_kind = 0xFFFFFFFFu, last_seq = 0, frame_see_through = 0;
	}

	bool seen(uint32_t kind, uint32_t seq)
	{
		std::lock_guard<std::mutex> lock(mutex);
		bool in_order = true;
		const bool first = last_kind == 0xFFFFFFFFu;
		switch (kind)
		{
		case slotmark::chars_begin:
			n.begin++;
			// (after the last frame's end or see-through markers, with the next seq)
			in_order = first || ((last_kind == slotmark::chars_end || last_kind == slotmark::see_through) && ((last_seq + 1) & 15) == seq);
			frame_see_through = 0;
			break;
		case slotmark::chars_end:
			n.end++;
			in_order = first || (last_kind == slotmark::chars_begin && last_seq == seq);
			break;
		case slotmark::see_through:
			n.see_through++;
			in_order = first || ((last_kind == slotmark::chars_end || last_kind == slotmark::see_through) && last_seq == seq);
			if (++frame_see_through > n.most_see_through)
				n.most_see_through = frame_see_through;
			break;
		default:
			return false;
		}
		if (!in_order)
			n.out_of_order++;
		last_kind = kind;
		last_seq = seq & 15;
		return in_order;
	}

	counts get_counts()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return n;
	}

	std::string status()
	{
		const counts c = get_counts();
		char line[200];
		std::snprintf(line, sizeof(line), "character markers seen in Direct3D 12: begin %llu, end %llu, see-through %llu (most in one frame %u); out of order %llu",
			static_cast<unsigned long long>(c.begin), static_cast<unsigned long long>(c.end), static_cast<unsigned long long>(c.see_through),
			c.most_see_through, static_cast<unsigned long long>(c.out_of_order));
		return line;
	}

	void reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		n = counts();
		last_kind = 0xFFFFFFFFu;
		last_seq = 0;
		frame_see_through = 0;
	}
}

// For the offline probe: the character markers' status line
extern "C" __declspec(dllexport) void __cdecl hifixi_chars_status(char *out, uint32_t size)
{
	if (out != nullptr && size != 0)
		std::snprintf(out, size, "%s", hifi::chars::marks::status().c_str());
}
