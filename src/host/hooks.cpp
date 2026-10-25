#include <d3d12.h>
#include "hooks.hpp"
#include "events.hpp"
#include "slots.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <type_traits>
#include <unordered_set>

// How the runtime dispatches (probed 2026-10-21, tests/hook_test.cpp keeps it honest): queues and devices share one
// static vtable each in D3D12Core.dll (WARP's device and the hardware's too). A command list starts on the runtime's
// static list vtable, but at its first Reset it gets a vtable of its own (on the heap, kept for its life), a copy of
// the static one with 33 slots (draws, state, root arguments...) pointing straight into the driver. So the static
// vtables are hooked (lists made later copy those hooks for the slots the runtime keeps), and each list's own vtable
// is hooked in place the first time the list is seen (its submission, or its Reset through a hook). Each list hook
// calls the function that was in the slot of the vtable it came through: a small record per list vtable, found
// through a thread-local cache.

namespace hifi::host
{
	namespace
	{
		uint32_t index[slot_count] = {};
		void *static_originals[slot_count] = {};
		void **list_vtable = nullptr;   // the runtime's static command list vtable
		void **queue_vtable = nullptr;  // its queue vtable
		void **device_vtable = nullptr; // its device vtable
		bool is_installed = false;
		std::atomic<bool> passthrough { false }; // (out: the hooks left in lists' own vtables only call on)
		std::atomic<uint32_t> counts[slot_count];
		std::atomic<const events *> sink { nullptr };
		thread_local uint32_t tl_quiet = 0;

		// A list's own vtable hooked in place, and what was in each slot before. Never freed: a list's vtable goes when
		// the list does, and its record is only read through that vtable.
		struct vtable_record
		{
			void **vtable;
			void *originals[slot_count];
		};
		constexpr uint32_t kMaxRecords = 4096;
		vtable_record *records[kMaxRecords] = {};
		std::atomic<uint32_t> record_count { 0 };
		thread_local void **tl_vtable = nullptr;
		thread_local const vtable_record *tl_record = nullptr;

		// Lists made or reset since the hooks went in (their state is known from there on); the last one each thread used
		std::mutex mutex;
		std::unordered_set<void *> lists_known;
		std::unordered_set<void *> queues;
		thread_local void *tl_list = nullptr;
		thread_local bool tl_known = false;
		std::atomic<uint32_t> executed { 0 }, foreign { 0 }, unknown_state { 0 }, vtables_hooked { 0 }, vtables_refused { 0 };
		target_seen world, depth;

		void **vtable_of(const void *object) { return *static_cast<void **const *>(object); }
		void **table_of(uint32_t s) { return is_list_slot(s) ? list_vtable : s == queue_execute ? queue_vtable : device_vtable; }

		const vtable_record *find_record(void **vt)
		{
			if (vt == tl_vtable)
				return tl_record;
			const uint32_t n = record_count.load(std::memory_order_acquire);
			for (uint32_t i = 0; i < n; ++i)
				if (records[i] != nullptr && records[i]->vtable == vt)
				{
					tl_vtable = vt;
					tl_record = records[i];
					return records[i];
				}
			return nullptr;
		}

		// The function a call through self's vtable would have reached: the record's when its vtable was hooked in
		// place, else the static one's (the static vtable itself, or a list's copy of it holding our hook there)
		template <typename F>
		F original(const void *self, slot s)
		{
			if (is_list_slot(s))
			{
				void **vt = vtable_of(self);
				if (vt != list_vtable)
					if (const vtable_record *r = find_record(vt))
						return reinterpret_cast<F>(r->originals[s]);
			}
			return reinterpret_cast<F>(static_originals[s]);
		}

		// The original called with the thread quiet: a hooked call made from inside it (the debug layer's wrapper calling
		// the object it wraps, the runtime calling itself) only calls on
		struct inside
		{
			inside() { tl_quiet++; }
			~inside() { tl_quiet--; }
		};
		template <typename F, typename... A>
		decltype(auto) call(const void *self, slot s, A... args)
		{
			inside quiet_while;
			return original<F>(self, s)(args...);
		}

		// A call on a list: counted as on a list in an unknown state when it wasn't made or reset since the hooks went in
		inline void note(void *list)
		{
			if (list != tl_list)
			{
				std::lock_guard<std::mutex> lock(mutex);
				tl_list = list;
				tl_known = lists_known.count(list) != 0;
			}
			if (!tl_known)
				unknown_state.fetch_add(1, std::memory_order_relaxed);
		}
		void know(void *list)
		{
			std::lock_guard<std::mutex> lock(mutex);
			lists_known.insert(list);
			if (tl_list == list)
				tl_known = true;
		}

		// Counting and events: not when out, nor on a quiet thread
		inline bool live() { return tl_quiet == 0 && !passthrough.load(std::memory_order_relaxed); }
		inline void count(slot s) { counts[s].fetch_add(1, std::memory_order_relaxed); }
		inline const events *ev() { return sink.load(std::memory_order_acquire); }

		// The tracer (set_tracer): each call's arguments as numbers, handed over before the call
		std::atomic<tracer> g_tracer { nullptr };
		template <typename T>
		uint64_t as_number(T v)
		{
			if constexpr (std::is_pointer_v<T>)
				return uint64_t(reinterpret_cast<uintptr_t>(v));
			else if constexpr (std::is_same_v<T, float>)
			{
				uint32_t bits;
				std::memcpy(&bits, &v, sizeof(bits));
				return bits;
			}
			else if constexpr (std::is_same_v<T, D3D12_CPU_DESCRIPTOR_HANDLE> || std::is_same_v<T, D3D12_GPU_DESCRIPTOR_HANDLE>)
				return uint64_t(v.ptr);
			else
				return uint64_t(int64_t(v));
		}
		template <typename... A>
		inline void trace(slot s, const void *self, A... a)
		{
			const tracer t = g_tracer.load(std::memory_order_acquire);
			if (t == nullptr || passthrough.load(std::memory_order_relaxed))
				return;
			const uint64_t args[sizeof...(A) + 1] = { as_number(a)..., 0 };
			t(uint32_t(s), self, args, uint32_t(sizeof...(A)), tl_quiet != 0);
		}

		bool is_depth_format(DXGI_FORMAT f)
		{
			switch (f)
			{
			case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS:
				return true;
			default:
				return false;
			}
		}

		// (mutex held) The largest seen so far, or the same one again
		void keep_largest(target_seen &t, ID3D12Resource *r, const D3D12_RESOURCE_DESC &d, bool end)
		{
			const bool larger = uint64_t(d.Width) * d.Height > t.width * t.height;
			if (r != t.resource && !larger)
				return;
			if (r != t.resource)
				t.ends = 0;
			t.resource = r;
			t.width = d.Width;
			t.height = d.Height;
			t.format = uint32_t(d.Format);
			if (end)
				t.ends++;
		}

		// The world's target turns from a render target into something read at the world's end; its depth buffer goes
		// between depth write and read (for the counts: the backend follows the world its own way)
		void watch_barriers(UINT n, const D3D12_RESOURCE_BARRIER *b)
		{
			const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			const D3D12_RESOURCE_STATES depth_states = D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ;
			for (UINT i = 0; i < n; ++i)
			{
				if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || b[i].Transition.pResource == nullptr)
					continue;
				const D3D12_RESOURCE_TRANSITION_BARRIER &t = b[i].Transition;
				const bool end = (t.StateBefore & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0 && (t.StateAfter & read) != 0;
				const bool deep = ((t.StateBefore | t.StateAfter) & depth_states) != 0;
				if (!end && !deep)
					continue;
				const D3D12_RESOURCE_DESC d = t.pResource->GetDesc();
				if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
					continue;
				std::lock_guard<std::mutex> lock(mutex);
				if (end)
					keep_largest(world, t.pResource, d, true);
				else if (is_depth_format(d.Format))
					keep_largest(depth, t.pResource, d, false);
			}
		}

		void ensure_hooked(void **vt);

		// The hooks: each calls on through the slot it came through, counts and hands the call to the backend
		HRESULT STDMETHODCALLTYPE hk_reset(ID3D12GraphicsCommandList *self, ID3D12CommandAllocator *a, ID3D12PipelineState *p)
		{
			trace(list_reset, self, a, p);
			const HRESULT hr = call<HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *)>(self, list_reset, self, a, p);
			if (!live())
				return hr;
			// (a list's first Reset gives it its own vtable, copied from the static one: hook its driver slots too, before
			// anything is recorded on it)
			ensure_hooked(vtable_of(self));
			count(list_reset);
			know(self);
			if (SUCCEEDED(hr))
				if (const events *e = ev(); e != nullptr && e->reset != nullptr)
					e->reset(self);
			return hr;
		}
		HRESULT STDMETHODCALLTYPE hk_close(ID3D12GraphicsCommandList *self)
		{
			trace(list_close, self);
			if (live())
			{
				count(list_close);
				note(self);
				if (const events *e = ev(); e != nullptr && e->close != nullptr)
					e->close(self);
			}
			return call<HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *)>(self, list_close, self);
		}
		void STDMETHODCALLTYPE hk_barrier(ID3D12GraphicsCommandList *self, UINT n, const D3D12_RESOURCE_BARRIER *b)
		{
			trace(list_barrier, self, n, b);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *)>(self, list_barrier, self, n, b);
			if (!live())
				return;
			count(list_barrier);
			note(self);
			if (b != nullptr)
			{
				watch_barriers(n, b);
				if (const events *e = ev(); e != nullptr && e->barrier != nullptr)
					e->barrier(self, n, b);
			}
		}
		void STDMETHODCALLTYPE hk_om_targets(ID3D12GraphicsCommandList *self, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs, BOOL range, const D3D12_CPU_DESCRIPTOR_HANDLE *dsv)
		{
			trace(list_om_targets, self, n, rtvs, range, dsv);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE *)>(self, list_om_targets, self, n, rtvs, range, dsv);
			if (!live())
				return;
			count(list_om_targets);
			note(self);
			if (const events *e = ev(); e != nullptr && e->targets != nullptr)
				e->targets(self, n, rtvs, range, dsv);
		}
		void STDMETHODCALLTYPE hk_pipeline(ID3D12GraphicsCommandList *self, ID3D12PipelineState *p)
		{
			trace(list_pipeline, self, p);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12PipelineState *)>(self, list_pipeline, self, p);
			if (!live())
				return;
			count(list_pipeline);
			note(self);
			if (const events *e = ev(); e != nullptr && e->pipeline != nullptr)
				e->pipeline(self, p);
		}
		void STDMETHODCALLTYPE hk_topology(ID3D12GraphicsCommandList *self, D3D12_PRIMITIVE_TOPOLOGY t)
		{
			trace(list_topology, self, t);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, D3D12_PRIMITIVE_TOPOLOGY)>(self, list_topology, self, t);
			if (!live())
				return;
			count(list_topology);
			note(self);
			if (const events *e = ev(); e != nullptr && e->topology != nullptr)
				e->topology(self, t);
		}
		void STDMETHODCALLTYPE hk_viewports(ID3D12GraphicsCommandList *self, UINT n, const D3D12_VIEWPORT *v)
		{
			trace(list_viewports, self, n, v);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, const D3D12_VIEWPORT *)>(self, list_viewports, self, n, v);
			if (!live())
				return;
			count(list_viewports);
			note(self);
			if (const events *e = ev(); e != nullptr && e->viewports != nullptr)
				e->viewports(self, n, v);
		}
		void STDMETHODCALLTYPE hk_scissors(ID3D12GraphicsCommandList *self, UINT n, const D3D12_RECT *r)
		{
			trace(list_scissors, self, n, r);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, const D3D12_RECT *)>(self, list_scissors, self, n, r);
			if (!live())
				return;
			count(list_scissors);
			note(self);
			if (const events *e = ev(); e != nullptr && e->scissors != nullptr)
				e->scissors(self, n, r);
		}
		void STDMETHODCALLTYPE hk_root_signature(ID3D12GraphicsCommandList *self, ID3D12RootSignature *rs)
		{
			trace(list_root_signature, self, rs);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12RootSignature *)>(self, list_root_signature, self, rs);
			if (!live())
				return;
			count(list_root_signature);
			note(self);
			if (const events *e = ev(); e != nullptr && e->root_signature != nullptr)
				e->root_signature(self, rs);
		}
		void STDMETHODCALLTYPE hk_root_table(ID3D12GraphicsCommandList *self, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE h)
		{
			trace(list_root_table, self, param, h);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(self, list_root_table, self, param, h);
			if (!live())
				return;
			count(list_root_table);
			note(self);
			if (const events *e = ev(); e != nullptr && e->root_table != nullptr)
				e->root_table(self, param, h);
		}
		void STDMETHODCALLTYPE hk_root_constant(ID3D12GraphicsCommandList *self, UINT param, UINT value, UINT offset)
		{
			trace(list_root_constant, self, param, value, offset);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, UINT, UINT)>(self, list_root_constant, self, param, value, offset);
			if (!live())
				return;
			count(list_root_constant);
			note(self);
			if (const events *e = ev(); e != nullptr && e->root_constants != nullptr)
				e->root_constants(self, param, 1, &value, offset);
		}
		void STDMETHODCALLTYPE hk_root_constants(ID3D12GraphicsCommandList *self, UINT param, UINT n, const void *values, UINT offset)
		{
			trace(list_root_constants, self, param, n, values, offset);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, UINT, const void *, UINT)>(self, list_root_constants, self, param, n, values, offset);
			if (!live())
				return;
			count(list_root_constants);
			note(self);
			if (const events *e = ev(); e != nullptr && e->root_constants != nullptr && values != nullptr)
				e->root_constants(self, param, n, values, offset);
		}
		void STDMETHODCALLTYPE hk_root_cbv(ID3D12GraphicsCommandList *self, UINT param, D3D12_GPU_VIRTUAL_ADDRESS a)
		{
			trace(list_root_cbv, self, param, a);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(self, list_root_cbv, self, param, a);
			if (!live())
				return;
			count(list_root_cbv);
			note(self);
			if (const events *e = ev(); e != nullptr && e->root_cbv != nullptr)
				e->root_cbv(self, param, a);
		}
		void STDMETHODCALLTYPE hk_root_srv(ID3D12GraphicsCommandList *self, UINT param, D3D12_GPU_VIRTUAL_ADDRESS a)
		{
			trace(list_root_srv, self, param, a);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(self, list_root_srv, self, param, a);
			if (!live())
				return;
			count(list_root_srv);
			note(self);
			if (const events *e = ev(); e != nullptr && e->root_srv != nullptr)
				e->root_srv(self, param, a);
		}
		void STDMETHODCALLTYPE hk_root_uav(ID3D12GraphicsCommandList *self, UINT param, D3D12_GPU_VIRTUAL_ADDRESS a)
		{
			trace(list_root_uav, self, param, a);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(self, list_root_uav, self, param, a);
			if (!live())
				return;
			count(list_root_uav);
			note(self);
			if (const events *e = ev(); e != nullptr && e->root_uav != nullptr)
				e->root_uav(self, param, a);
		}
		void STDMETHODCALLTYPE hk_clear_depth(ID3D12GraphicsCommandList *self, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 s, UINT n, const D3D12_RECT *r)
		{
			trace(list_clear_depth, self, dsv, f, d, s, n, r);
			if (live())
			{
				count(list_clear_depth); // (before the call: where grass goes in before a mid-frame clear)
				note(self);
				if (const events *e = ev(); e != nullptr && e->clear_depth != nullptr)
					e->clear_depth(self, dsv, f, n, r);
			}
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT *)>(self, list_clear_depth, self, dsv, f, d, s, n, r);
		}
		void STDMETHODCALLTYPE hk_clear_target(ID3D12GraphicsCommandList *self, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT c[4], UINT n, const D3D12_RECT *r)
		{
			trace(list_clear_target, self, rtv, c, n, r);
			if (live())
			{
				count(list_clear_target);
				note(self);
				if (const events *e = ev(); e != nullptr && e->clear_target != nullptr)
					e->clear_target(self, rtv, c, n, r);
			}
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT *, UINT, const D3D12_RECT *)>(self, list_clear_target, self, rtv, c, n, r);
		}
		void STDMETHODCALLTYPE hk_draw(ID3D12GraphicsCommandList *self, UINT v, UINT i, UINT sv, UINT si)
		{
			trace(list_draw, self, v, i, sv, si);
			if (live())
			{
				count(list_draw);
				note(self);
				if (const events *e = ev(); e != nullptr && e->draw != nullptr)
					e->draw(self, uint64_t(v) * i);
			}
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, UINT)>(self, list_draw, self, v, i, sv, si);
		}
		void STDMETHODCALLTYPE hk_draw_indexed(ID3D12GraphicsCommandList *self, UINT n, UINT i, UINT si, INT bv, UINT sinst)
		{
			trace(list_draw_indexed, self, n, i, si, bv, sinst);
			if (live())
			{
				count(list_draw_indexed);
				note(self);
				if (const events *e = ev(); e != nullptr && e->draw != nullptr)
					e->draw(self, uint64_t(n) * i);
			}
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, INT, UINT)>(self, list_draw_indexed, self, n, i, si, bv, sinst);
		}
		// (only to see whether dgVoodoo uses these: the backend doesn't follow them)
		void STDMETHODCALLTYPE hk_execute_indirect(ID3D12GraphicsCommandList *self, ID3D12CommandSignature *sig, UINT max, ID3D12Resource *args, UINT64 args_at, ID3D12Resource *n, UINT64 n_at)
		{
			trace(list_execute_indirect, self, sig, max, args, args_at, n, n_at);
			if (live())
				count(list_execute_indirect);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12CommandSignature *, UINT, ID3D12Resource *, UINT64, ID3D12Resource *, UINT64)>(self, list_execute_indirect, self, sig, max, args, args_at, n, n_at);
		}
		void STDMETHODCALLTYPE hk_execute_bundle(ID3D12GraphicsCommandList *self, ID3D12GraphicsCommandList *bundle)
		{
			trace(list_execute_bundle, self, bundle);
			if (live())
				count(list_execute_bundle);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12GraphicsCommandList *)>(self, list_execute_bundle, self, bundle);
		}
		void STDMETHODCALLTYPE hk_clear_state(ID3D12GraphicsCommandList *self, ID3D12PipelineState *p)
		{
			trace(list_clear_state, self, p);
			if (live())
				count(list_clear_state);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12PipelineState *)>(self, list_clear_state, self, p);
		}
		void STDMETHODCALLTYPE hk_barrier7(ID3D12GraphicsCommandList7 *self, UINT32 n, const D3D12_BARRIER_GROUP *g)
		{
			trace(list_barrier7, self, n, g);
			if (live())
				count(list_barrier7);
			call<void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList7 *, UINT32, const D3D12_BARRIER_GROUP *)>(self, list_barrier7, self, n, g);
		}
		void STDMETHODCALLTYPE hk_queue_execute(ID3D12CommandQueue *self, UINT n, ID3D12CommandList *const *lists)
		{
			trace(queue_execute, self, n, lists);
			const bool counting = live();
			if (counting)
			{
				count(queue_execute);
				// Every list submitted: its own vtable hooked from here on (a list seen first here was recorded unseen)
				uint32_t unhooked = 0;
				for (UINT i = 0; i < n; ++i)
					if (lists != nullptr && lists[i] != nullptr)
					{
						void **vt = vtable_of(lists[i]);
						ensure_hooked(vt);
						unhooked += vt != list_vtable && find_record(vt) == nullptr ? 1u : 0u;
					}
				executed.fetch_add(n, std::memory_order_relaxed);
				foreign.fetch_add(unhooked, std::memory_order_relaxed);
				{
					std::lock_guard<std::mutex> lock(mutex);
					queues.insert(self);
				}
			}
			call<void(STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *)>(self, queue_execute, self, n, lists);
			if (counting)
				if (const events *e = ev(); e != nullptr && e->execute != nullptr)
					e->execute(self, n, lists);
		}
		HRESULT STDMETHODCALLTYPE hk_create_list(ID3D12Device *self, UINT mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *a, ID3D12PipelineState *p, REFIID riid, void **out)
		{
			trace(device_create_list, self, mask, type, a, p);
			const HRESULT hr = call<HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *, REFIID, void **)>(self, device_create_list, self, mask, type, a, p, riid, out);
			if (!live() || FAILED(hr) || out == nullptr || *out == nullptr || type != D3D12_COMMAND_LIST_TYPE_DIRECT)
				return hr;
			count(device_create_list);
			// (the command list interfaces inherit in one line: whichever was asked for, it's the object's start)
			auto *const list = static_cast<ID3D12GraphicsCommandList *>(*out);
			know(list);
			if (const events *e = ev(); e != nullptr && e->created != nullptr)
				e->created(list);
			return hr;
		}
		void STDMETHODCALLTYPE hk_create_rtv(ID3D12Device *self, ID3D12Resource *res, const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE at)
		{
			trace(device_create_rtv, self, res, desc, at);
			call<void(STDMETHODCALLTYPE *)(ID3D12Device *, ID3D12Resource *, const D3D12_RENDER_TARGET_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE)>(self, device_create_rtv, self, res, desc, at);
			if (!live())
				return;
			count(device_create_rtv);
			if (const events *e = ev(); e != nullptr && e->view != nullptr)
				e->view(self, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, at, res, desc != nullptr ? desc->Format : res != nullptr ? res->GetDesc().Format : DXGI_FORMAT_UNKNOWN, 0);
		}
		void STDMETHODCALLTYPE hk_create_dsv(ID3D12Device *self, ID3D12Resource *res, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE at)
		{
			trace(device_create_dsv, self, res, desc, at);
			call<void(STDMETHODCALLTYPE *)(ID3D12Device *, ID3D12Resource *, const D3D12_DEPTH_STENCIL_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE)>(self, device_create_dsv, self, res, desc, at);
			if (!live())
				return;
			count(device_create_dsv);
			if (const events *e = ev(); e != nullptr && e->view != nullptr)
				e->view(self, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, at, res, desc != nullptr ? desc->Format : res != nullptr ? res->GetDesc().Format : DXGI_FORMAT_UNKNOWN,
					desc != nullptr ? uint32_t(desc->Flags) : 0);
		}
		void STDMETHODCALLTYPE hk_copy(ID3D12Device *self, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE *ds, const UINT *dn, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE *ss, const UINT *sn, D3D12_DESCRIPTOR_HEAP_TYPE type)
		{
			trace(device_copy, self, nd, ds, dn, ns, ss, sn, type);
			call<void(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *, D3D12_DESCRIPTOR_HEAP_TYPE)>(self, device_copy, self, nd, ds, dn, ns, ss, sn, type);
			if (!live() || (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV))
				return;
			count(device_copy);
			if (const events *e = ev(); e != nullptr && e->copy != nullptr)
				e->copy(self, type, nd, ds, dn, ns, ss, sn);
		}
		void STDMETHODCALLTYPE hk_copy_simple(ID3D12Device *self, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type)
		{
			trace(device_copy_simple, self, n, dst, src, type);
			call<void(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE)>(self, device_copy_simple, self, n, dst, src, type);
			if (!live() || (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV))
				return;
			count(device_copy_simple);
			if (const events *e = ev(); e != nullptr && e->copy != nullptr)
				e->copy(self, type, 1, &dst, &n, 1, &src, &n);
		}

		void *const kHooks[slot_count] = {
			reinterpret_cast<void *>(&hk_reset), reinterpret_cast<void *>(&hk_close), reinterpret_cast<void *>(&hk_barrier),
			reinterpret_cast<void *>(&hk_om_targets), reinterpret_cast<void *>(&hk_pipeline), reinterpret_cast<void *>(&hk_topology),
			reinterpret_cast<void *>(&hk_viewports), reinterpret_cast<void *>(&hk_scissors), reinterpret_cast<void *>(&hk_root_signature),
			reinterpret_cast<void *>(&hk_root_table), reinterpret_cast<void *>(&hk_root_constant), reinterpret_cast<void *>(&hk_root_constants),
			reinterpret_cast<void *>(&hk_root_cbv), reinterpret_cast<void *>(&hk_root_srv), reinterpret_cast<void *>(&hk_root_uav),
			reinterpret_cast<void *>(&hk_clear_depth), reinterpret_cast<void *>(&hk_clear_target), reinterpret_cast<void *>(&hk_draw),
			reinterpret_cast<void *>(&hk_draw_indexed), reinterpret_cast<void *>(&hk_execute_indirect), reinterpret_cast<void *>(&hk_execute_bundle),
			reinterpret_cast<void *>(&hk_clear_state), reinterpret_cast<void *>(&hk_barrier7), reinterpret_cast<void *>(&hk_queue_execute),
			reinterpret_cast<void *>(&hk_create_list), reinterpret_cast<void *>(&hk_create_rtv), reinterpret_cast<void *>(&hk_create_dsv),
			reinterpret_cast<void *>(&hk_copy), reinterpret_cast<void *>(&hk_copy_simple),
		};

		// The module an address is in (its file name alone), empty when none
		std::wstring module_of(const void *p)
		{
			HMODULE m = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(p), &m) || m == nullptr)
				return std::wstring();
			wchar_t path[MAX_PATH] = {};
			const DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
			std::wstring s(path, n);
			const size_t slash = s.find_last_of(L"\\/");
			return slash == std::wstring::npos ? s : s.substr(slash + 1);
		}
		// Direct3D 12's own classes: the runtime's, or its debug layer's when that's on (D3D12DebugLayer=1, the tests)
		bool is_runtime(const std::wstring &name)
		{
			return _wcsicmp(name.c_str(), L"D3D12Core.dll") == 0 || _wcsicmp(name.c_str(), L"d3d12.dll") == 0 || _wcsicmp(name.c_str(), L"D3D12SDKLayers.dll") == 0;
		}
		std::string narrow(const std::wstring &w)
		{
			std::string s;
			for (wchar_t c : w)
				s += c < 128 ? char(c) : '?';
			return s;
		}

		bool swap_slot(void **table, uint32_t i, void *value, void **old_value)
		{
			MEMORY_BASIC_INFORMATION mbi = {};
			const bool writable = VirtualQuery(&table[i], &mbi, sizeof(mbi)) != 0 && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
			DWORD protect = 0;
			if (!writable && !VirtualProtect(&table[i], sizeof(void *), PAGE_READWRITE, &protect))
				return false;
			void *const was = InterlockedExchangePointer(&table[i], value);
			if (!writable)
				VirtualProtect(&table[i], sizeof(void *), protect, &protect);
			if (old_value != nullptr)
				*old_value = was;
			return true;
		}

		// A list's own vtable: hooked in place once, after checking it's the runtime's copy (on the heap, not in another
		// module; the slots the runtime doesn't give the driver holding the static vtable's functions or our hooks)
		std::mutex hook_mutex;
		std::unordered_set<void **> refused;
		void ensure_hooked(void **vt)
		{
			if (vt == list_vtable || vt == nullptr)
				return;
			if (vt == tl_vtable)
			{
				// (already hooked: but the runtime may have written a slot back since; the Reset path checks every time)
				return;
			}
			std::lock_guard<std::mutex> lock(hook_mutex);
			if (find_record(vt) != nullptr || refused.count(vt) != 0)
				return;
			bool runtime_copy = module_of(vt).empty();
			for (slot s : { list_reset, list_close, list_barrier, list_om_targets, list_clear_depth, list_clear_target })
			{
				void *const v = vt[index[s]];
				runtime_copy = runtime_copy && (v == static_originals[s] || v == kHooks[s]);
			}
			const uint32_t n = record_count.load(std::memory_order_relaxed);
			if (!runtime_copy || n == kMaxRecords)
			{
				refused.insert(vt);
				vtables_refused.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			vtable_record *r = new vtable_record();
			r->vtable = vt;
			for (uint32_t s = 0; s < queue_execute; ++s)
			{
				void *const v = vt[index[s]];
				// (a slot copied from the static vtable after it was hooked holds our hook: its original is the static one's)
				r->originals[s] = v == kHooks[s] ? static_originals[s] : v;
			}
			// Published before any slot points at a hook that reads it
			records[n] = r;
			record_count.store(n + 1, std::memory_order_release);
			for (uint32_t s = 0; s < queue_execute; ++s)
				if (vt[index[s]] != kHooks[s])
					swap_slot(vt, index[s], kHooks[s], nullptr);
			vtables_hooked.fetch_add(1, std::memory_order_relaxed);
		}
	}

	quiet::quiet() { tl_quiet++; }
	quiet::~quiet() { tl_quiet--; }
	bool is_quiet() { return tl_quiet != 0; }

	void set_events(const events *e)
	{
		sink.store(e, std::memory_order_release);
	}

	void set_tracer(tracer t)
	{
		g_tracer.store(t, std::memory_order_release);
	}

	const char *slot_name(uint32_t s)
	{
		static const char *const names[slot_count] = { "Reset", "Close", "ResourceBarrier", "OMSetRenderTargets", "SetPipelineState",
			"IASetPrimitiveTopology", "RSSetViewports", "RSSetScissorRects", "SetGraphicsRootSignature", "SetGraphicsRootDescriptorTable",
			"SetGraphicsRoot32BitConstant", "SetGraphicsRoot32BitConstants", "SetGraphicsRootConstantBufferView", "SetGraphicsRootShaderResourceView",
			"SetGraphicsRootUnorderedAccessView", "ClearDepthStencilView", "ClearRenderTargetView", "DrawInstanced", "DrawIndexedInstanced",
			"ExecuteIndirect", "ExecuteBundle", "ClearState", "Barrier (enhanced)", "ExecuteCommandLists",
			"CreateCommandList", "CreateRenderTargetView", "CreateDepthStencilView", "CopyDescriptors", "CopyDescriptorsSimple" };
		return s < slot_count ? names[s] : "?";
	}

	bool install(std::string &why, void *adapter)
	{
		if (is_installed)
			return true;
		if (list_vtable != nullptr)
		{
			// (in again after an uninstall: the static slots back in, the lists' own vtables were left hooked)
			for (uint32_t s = 0; s < slot_count; ++s)
				swap_slot(table_of(s), index[s], kHooks[s], nullptr);
			passthrough = false;
			is_installed = true;
			return true;
		}
		HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
		if (d3d12 == nullptr)
			d3d12 = LoadLibraryW(L"d3d12.dll");
		const auto create = d3d12 != nullptr ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
		if (create == nullptr)
		{
			why = "Direct3D 12 isn't there (d3d12.dll)";
			return false;
		}
		// (the adapter's device: WARP's from the starter, else the default adapter's, the one dgVoodoo made when it's on
		// it; either way the runtime's static vtables are the same; a list never reset is still on the static one)
		ID3D12Device *device = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		ID3D12CommandAllocator *allocator = nullptr;
		ID3D12GraphicsCommandList *list = nullptr;
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		HRESULT hr = create(static_cast<IUnknown *>(adapter), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
		if (SUCCEEDED(hr))
			hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
		if (SUCCEEDED(hr))
			hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
		if (SUCCEEDED(hr))
			hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
		auto release_all = [&] {
			if (list != nullptr) list->Release();
			if (allocator != nullptr) allocator->Release();
			if (queue != nullptr) queue->Release();
			if (device != nullptr) device->Release();
		};
		if (FAILED(hr))
		{
			char text[96];
			std::snprintf(text, sizeof(text), "no Direct3D 12 device, queue or list could be made (0x%08lX)", static_cast<unsigned long>(hr));
			why = text;
			release_all();
			return false;
		}
		void **lv = vtable_of(list), **qv = vtable_of(queue), **dv = vtable_of(device);
		slot_indices(index);
		// The runtime's own classes only: the static vtables in its image, every slot's function in it
		const std::wstring lm = module_of(lv), qm = module_of(qv), dm = module_of(dv);
		if (!is_runtime(lm) || !is_runtime(qm) || !is_runtime(dm))
		{
			why = "the command list's class isn't Direct3D 12's own (" + narrow(lm.empty() ? L"no module" : lm) + "): another layer (ReShade?) is in between";
			release_all();
			return false;
		}
		list_vtable = lv;
		queue_vtable = qv;
		device_vtable = dv;
		for (uint32_t s = 0; s < slot_count; ++s)
		{
			const std::wstring m = module_of(table_of(s)[index[s]]);
			if (!is_runtime(m))
			{
				why = std::string(slot_name(s)) + " isn't Direct3D 12's own (it's in " + narrow(m.empty() ? L"no module" : m) + ")";
				list_vtable = queue_vtable = device_vtable = nullptr;
				release_all();
				return false;
			}
		}
		for (uint32_t s = 0; s < slot_count; ++s)
		{
			if (!swap_slot(table_of(s), index[s], kHooks[s], &static_originals[s]))
			{
				why = std::string("couldn't write ") + slot_name(s) + "'s slot";
				for (uint32_t u = 0; u < s; ++u)
					swap_slot(table_of(u), index[u], static_originals[u], nullptr);
				list_vtable = queue_vtable = device_vtable = nullptr;
				release_all();
				return false;
			}
		}
		passthrough = false;
		is_installed = true;
		{
			// (the throwaway objects' own calls aren't the game's)
			quiet q;
			list->Close();
			release_all();
		}
		return true;
	}

	void uninstall()
	{
		if (!is_installed)
			return;
		// The static vtables back as they were (only slots that still hold ours). The lists' own vtables are never
		// written again: a list may be gone, its vtable with it. Their hooks stay, only calling on (the DLL is pinned).
		passthrough = true;
		for (uint32_t s = 0; s < slot_count; ++s)
		{
			void **table = table_of(s);
			if (static_cast<void *volatile *>(table)[index[s]] == kHooks[s])
				swap_slot(table, index[s], static_originals[s], nullptr);
		}
		is_installed = false;
	}

	bool installed() { return is_installed; }

	stats get_stats()
	{
		stats s;
		for (uint32_t i = 0; i < slot_count; ++i)
			s.calls[i] = counts[i].load(std::memory_order_relaxed);
		s.lists_executed = executed.load(std::memory_order_relaxed);
		s.lists_foreign = foreign.load(std::memory_order_relaxed);
		s.lists_unknown_state = unknown_state.load(std::memory_order_relaxed);
		s.vtables_hooked = vtables_hooked.load(std::memory_order_relaxed);
		s.vtables_refused = vtables_refused.load(std::memory_order_relaxed);
		std::lock_guard<std::mutex> lock(mutex);
		s.lists_seen = lists_known.size();
		s.world = world;
		s.depth = depth;
		s.queues_seen = uint32_t(queues.size());
		return s;
	}

	void reset_stats()
	{
		for (auto &c : counts)
			c.store(0, std::memory_order_relaxed);
		executed = 0;
		foreign = 0;
		unknown_state = 0;
		std::lock_guard<std::mutex> lock(mutex);
		world.ends = 0;
	}
}
