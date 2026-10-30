#include "passes.hpp"
#include <mutex>
#include <unordered_map>

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
		// Each list's descriptor heaps (dgVoodoo's recording thread; the mutex for a list let go elsewhere)
		struct heaps
		{
			UINT count = 0;
			ID3D12DescriptorHeap *h[2] = {};
		};
		std::mutex &heaps_mutex()
		{
			static std::mutex m;
			return m;
		}
		std::unordered_map<ID3D12GraphicsCommandList *, heaps> &lists()
		{
			static std::unordered_map<ID3D12GraphicsCommandList *, heaps> all;
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
	void on_reset(ID3D12GraphicsCommandList *list)
	{
		std::lock_guard<std::mutex> lock(heaps_mutex());
		lists()[list] = heaps();
	}
	void on_heaps(ID3D12GraphicsCommandList *list, UINT count, ID3D12DescriptorHeap *const *h)
	{
		std::lock_guard<std::mutex> lock(heaps_mutex());
		heaps &x = lists()[list];
		x.count = h != nullptr ? (count < 2 ? count : 2) : 0;
		for (UINT i = 0; i < x.count; ++i)
			x.h[i] = h[i];
	}
	bool dispatch(uint32_t kind, ID3D12GraphicsCommandList *list, const grass::list_state &bound, const world &w, uint32_t seq_low4)
	{
		if (kind >= kKinds || handlers()[kind] == nullptr)
			return false;
		world with = w;
		{
			std::lock_guard<std::mutex> lock(heaps_mutex());
			const auto known = lists().find(list);
			if (known != lists().end())
			{
				with.heap_count = known->second.count;
				with.heaps[0] = known->second.h[0];
				with.heaps[1] = known->second.h[1];
			}
		}
		handlers()[kind](list, bound, with, seq_low4);
		return true;
	}
	const std::vector<module> &modules()
	{
		return list();
	}
}
