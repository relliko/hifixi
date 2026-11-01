/*
 * The world part's slot markers (worldmark.hpp). The marker: a call into HiFi XI's own d3d8.dll (directmark.hpp), as
 * charslot.cpp sends it.
 */

#include "worldmark.hpp"
#include "slotmark.hpp"
#include "directmark.hpp"

namespace hifi::game::worldmark
{
	namespace
	{
		struct state
		{
			void (*sink)(uint32_t, uint32_t) = nullptr;
			volatile bool wanted = false;
			counts n;
		};
		state g;

	}

	void set_wanted(bool wanted)
	{
		g.wanted = wanted;
	}

	bool wanted()
	{
		return g.wanted || g.sink != nullptr;
	}

	bool send(uint32_t kind, uint32_t seq)
	{
		if (kind >= slotmark::kinds)
			return false;
		if (g.sink != nullptr)
		{
			g.sink(kind, seq);
			g.n.sent[kind]++;
			return true;
		}
		if (!g.wanted)
			return false;
		if (!hifi::game::direct_mark(kind, seq))
		{
			g.n.not_sent++;
			return false;
		}
		g.n.sent[kind]++;
		return true;
	}

	counts get_counts()
	{
		return g.n;
	}

	void set_sink(void (*sink)(uint32_t, uint32_t))
	{
		g.sink = sink;
	}
}
