#include "passes.hpp"

namespace hifi::passes
{
	namespace
	{
		constexpr uint32_t kKinds = 16; // (slotmark::kinds)
		marker_fn *handlers()
		{
			static marker_fn table[kKinds] = {};
			return table;
		}
		std::vector<module> &list()
		{
			static std::vector<module> all;
			return all;
		}
	}

	void on_marker(uint32_t kind, marker_fn fn)
	{
		if (kind < kKinds)
			handlers()[kind] = fn;
	}
	void add(const module &m)
	{
		list().push_back(m);
	}
	bool dispatch(uint32_t kind, ID3D12GraphicsCommandList *list, const grass::list_state &bound, const world &w, uint32_t seq_low4)
	{
		if (kind >= kKinds || handlers()[kind] == nullptr)
			return false;
		handlers()[kind](list, bound, w, seq_low4);
		return true;
	}
	const std::vector<module> &modules()
	{
		return list();
	}
}
