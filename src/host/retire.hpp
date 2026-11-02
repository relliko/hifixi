#pragma once

// GPU objects let go once the GPU is done with them: one fence on the game's direct queue, signalled at every present
// (backend::present: on_present), instead of each module guessing with a count of presents or milliseconds (zone data
// kept 240 presents, effect uploads 2 s, characters' meshes released at once when their address was reused).
//
// later(p) / later(std::move(owner)): from any thread. What is handed over may be on lists recorded this frame or
// before; those go to the GPU when the d3d8.dll presents, after the signal this present made (Ashita calls the plugin's
// present first). So an object is let go once the fence has passed two signals after the last one made: the next
// present's signal is the first behind all of this frame's lists.
//
// Before the first present (no fence yet: the tests) what's handed over is held until on_present or release_all. A new
// device: whatever the last one's was is let go at once. The device removed: the fence answers UINT64_MAX, so all of it.

#include <d3d12.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace hifi::retire
{
	namespace detail
	{
		struct item
		{
			uint64_t at = 0;
			IUnknown *p = nullptr;
			std::function<void()> owner; // (a destructor to run: what isn't one COM object)
		};
		struct state
		{
			std::mutex mutex; // (a leaf: nothing is released while it's held)
			ID3D12Device *device = nullptr;
			ID3D12Fence *fence = nullptr;
			ID3D12CommandQueue *queue = nullptr; // (the last present's, held: release_all signals on it)
			uint64_t signalled = 0;
			std::vector<item> items;
			uint64_t let_go = 0;
		};
		inline state &g()
		{
			static state s;
			return s;
		}
		inline void free(std::vector<item> &v)
		{
			for (item &i : v)
			{
				if (i.p != nullptr)
					i.p->Release();
				if (i.owner)
					i.owner();
			}
			v.clear();
		}
		inline void hold(item &&i)
		{
			state &s = g();
			std::lock_guard<std::mutex> lock(s.mutex);
			i.at = s.signalled + 2;
			s.items.push_back(std::move(i));
		}
	}

	// p released once the GPU is past every list recorded so far (nullptr: nothing)
	inline void later(IUnknown *p)
	{
		if (p == nullptr)
			return;
		detail::item i;
		i.p = p;
		detail::hold(std::move(i));
	}
	// The same for a member or slot: handed over and set to nullptr
	template <typename T>
	inline void let_go(T *&p)
	{
		later(static_cast<IUnknown *>(p));
		p = nullptr;
	}
	// An owner of GPU objects (a gpu_zone...) destroyed then
	template <typename T>
	inline void later(std::unique_ptr<T> &&owner)
	{
		if (owner == nullptr)
			return;
		detail::item i;
		std::shared_ptr<T> held(std::move(owner));
		i.owner = [held]() mutable { held.reset(); };
		detail::hold(std::move(i));
	}

	// At every present, on the game's thread, with the game's direct queue: the fence signalled, what it's past let go
	inline void on_present(ID3D12CommandQueue *queue)
	{
		if (queue == nullptr)
			return;
		detail::state &s = detail::g();
		std::vector<detail::item> done;
		{
			ID3D12Device *device = nullptr;
			if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
				return;
			device->Release(); // (the queue keeps it)
			std::lock_guard<std::mutex> lock(s.mutex);
			if (device != s.device)
			{
				if (s.device != nullptr)
					done.swap(s.items); // (the last device's objects: nothing of theirs runs on this one)
				else
					for (detail::item &i : s.items)
						i.at = 2; // (held before there was a fence: counted from this one's signals)
				if (s.fence != nullptr)
					s.fence->Release();
				s.fence = nullptr;
				s.signalled = 0;
				s.device = device;
				if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence))))
					s.fence = nullptr;
				else
					s.fence->SetName(L"hifixi: let go behind the GPU");
			}
			if (queue != s.queue)
			{
				queue->AddRef();
				if (s.queue != nullptr)
					s.queue->Release();
				s.queue = queue;
			}
			if (s.fence != nullptr)
			{
				queue->Signal(s.fence, ++s.signalled);
				const uint64_t completed = s.fence->GetCompletedValue(); // (UINT64_MAX once the device is removed)
				for (size_t k = 0; k < s.items.size();)
					if (s.items[k].at <= completed)
					{
						done.push_back(std::move(s.items[k]));
						s.items[k] = std::move(s.items.back());
						s.items.pop_back();
					}
					else
						++k;
				s.let_go += done.size();
			}
		}
		detail::free(done);
	}

	// The plugin's unload: nothing of ours is recorded after it, but the d3d8.dll's lists already sent may still be
	// running, and its open one goes after this. One more signal, waited for (at most 2 s): what it covers by the usual
	// rule is let go; what was handed over since the last present may be on the open list, and is left (never freed under
	// it). With no fence (a test, its GPU idle) everything is let go.
	inline void release_all()
	{
		detail::state &s = detail::g();
		std::vector<detail::item> done, left;
		{
			std::lock_guard<std::mutex> lock(s.mutex);
			if (s.fence != nullptr && s.queue != nullptr && SUCCEEDED(s.queue->Signal(s.fence, ++s.signalled)))
			{
				if (s.fence->GetCompletedValue() < s.signalled)
					if (HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr))
					{
						if (SUCCEEDED(s.fence->SetEventOnCompletion(s.signalled, e)))
							WaitForSingleObject(e, 2000);
						CloseHandle(e);
					}
				const uint64_t completed = s.fence->GetCompletedValue();
				for (detail::item &i : s.items)
					(i.at <= completed ? done : left).push_back(std::move(i));
			}
			else
				done.swap(s.items);
			s.items.clear();
			s.let_go += done.size();
		}
		detail::free(done);
		for (detail::item &i : left)
			if (i.owner)
				new std::function<void()>(std::move(i.owner)); // (left: never run, never freed)
	}

	// (for /hifi status and the tests) Held now, let go so far, the fence's last signal and what the GPU has passed
	struct counts
	{
		size_t held = 0;
		uint64_t let_go = 0, signalled = 0, completed = 0;
	};
	inline counts status()
	{
		detail::state &s = detail::g();
		std::lock_guard<std::mutex> lock(s.mutex);
		counts c;
		c.held = s.items.size();
		c.let_go = s.let_go;
		c.signalled = s.signalled;
		c.completed = s.fence != nullptr ? s.fence->GetCompletedValue() : 0;
		return c;
	}
}
