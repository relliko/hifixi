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
		// Each list's heaps (dgVoodoo's lists are reset on its own thread and, at readbacks, on the game's)
		struct heap_set
		{
			UINT count = 0;
			ID3D12DescriptorHeap *h[2] = {};
		};
		std::mutex &heaps_mutex()
		{
			static std::mutex m;
			return m;
		}
		std::unordered_map<ID3D12GraphicsCommandList *, heap_set> &heaps_of()
		{
			static std::unordered_map<ID3D12GraphicsCommandList *, heap_set> all;
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
		world with = w;
		{
			std::lock_guard<std::mutex> lock(heaps_mutex());
			const auto at = heaps_of().find(list);
			if (at != heaps_of().end())
			{
				with.heap_count = at->second.count;
				with.heaps[0] = at->second.h[0];
				with.heaps[1] = at->second.h[1];
			}
		}
		handlers()[kind](list, bound, with, seq_low4);
		return true;
	}
	const std::vector<module> &modules()
	{
		return list();
	}

	void on_heaps(ID3D12GraphicsCommandList *list, UINT count, ID3D12DescriptorHeap *const *heaps)
	{
		heap_set s;
		s.count = heaps != nullptr ? (count < 2 ? count : 2) : 0;
		for (UINT i = 0; i < s.count; ++i)
			s.h[i] = heaps[i];
		std::lock_guard<std::mutex> lock(heaps_mutex());
		heaps_of()[list] = s;
	}
	void on_reset(ID3D12GraphicsCommandList *list)
	{
		std::lock_guard<std::mutex> lock(heaps_mutex());
		heaps_of()[list] = heap_set();
	}
}
