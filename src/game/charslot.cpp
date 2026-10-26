/*
 * The character passes' slots marked (charslot.hpp, slotmark.hpp; research/ffxi-re/notes-characters.md "For a native
 * character renderer", part 1).
 *
 * The opaque actors (priority 6). CXiActorDraw's update is the whole of
 *   0817E0: call 082D70; xor al, al; ret
 * and 082D70 (the walk over the actor list, drawing each actor through vt+0x288) is called from nowhere else. It takes
 * no arguments and doesn't use ecx. The call's rel32 is pointed at a stub of ours, zoneslot's shape:
 *   push ecx; push 2; call on_slot; add esp, 4; pop ecx      the walk's begin (on_slot takes the kind alone)
 *   call 082D70                                              the game's walk, as before
 *   push eax; push 3; call on_slot; add esp, 4; pop eax      its end
 *   ret
 *
 * The see-through actors (priority 12). An actor's Update queues it in the flush's sorted list when it's see-through,
 * storing the callback for the flush to call with the actor:
 *   0C7E75: mov [esi+8D4h], esi; push eax; mov dword [esi+8D0h], 0CBD90h
 * 0CBD90 (cdecl, the actor its one argument, plain ret) checks the actor and jumps into the actor draw 0CBDE0. The
 * immediate is pointed at a wrapper of ours (the flush calls it as it called 0CBD90):
 *   push [esp+4]; call on_see_through; add esp, 4; push [esp+4]; call 0CBD90; add esp, 4
 *   push eax; call on_see_through_done; pop eax; ret
 * on_see_through marks the actor (kind 4) and sets from_flush until on_see_through_done. (The actor's queued node is
 * gone once the flush has run: the list is emptied after each walk, so putting the immediate back between frames
 * leaves nothing calling the wrapper.)
 *
 * The marker: Clear(1, {x, 0, x + 1, 1}, D3DCLEAR_STENCIL, 0, 1.0, 0) on the client device, as zoneslot.cpp sends it
 * (the same device lookup and stencil check, kept separate so the zone's stays as it is).
 */

#include "charslot.hpp"
#include "slotmark.hpp"
#include <cstdio>
#include <cstring>

namespace hifi::game::charslot
{
	namespace
	{
		// 0817D0: mov ecx, imm32; jmp; 6 x nop; then 0817E0: call 082D70; xor al, al; ret
		constexpr const char *kCall = "B9????????E9????????909090909090E8????????32C0C3";
		constexpr intptr_t kCallAt = 0x10;
		constexpr const char *kWalk = "566A3CC705????????00000000E8????????8B35????????83C40485F6"; // 082D70
		// 0C7E75: mov [esi+8D4h], esi; push eax; mov dword [esi+8D0h], imm32 (the callback)
		constexpr const char *kQueue = "89B6D408000050C786D0080000";
		constexpr intptr_t kImmediateAt = 0x0D;
		constexpr const char *kCallback = "8B4C240485C9743A6683B9B2000000007530"; // 0CBD90
		// The client's IDirect3DDevice8 (Ashita's wrapper object): [renderer + 8] (as zoneslot finds it)
		constexpr const char *kDevicePat = "8BC8E8????????85C0A3????????0F84????????8B5C241C";

		constexpr size_t kCodeSize = 0x40, kWrapperAt = 0x20;
		constexpr size_t kCallMark1 = 0x03, kCallWalk = 0x0C, kCallMark2 = 0x14; // (the walk stub's three calls)
		constexpr size_t kCallBegin = 0x04, kCallCallback = 0x10, kCallDone = 0x19; // (the wrapper's)

		constexpr uint32_t kRelease = 2, kGetDepthStencilSurface = 33, kClear = 36, kSurfaceGetDesc = 8;
		constexpr uint32_t kClearStencil = 4; // D3DCLEAR_STENCIL
		using release_fn = unsigned long(__stdcall *)(uintptr_t self);
		using get_surface_fn = long(__stdcall *)(uintptr_t self, uintptr_t *out);
		using get_desc_fn = long(__stdcall *)(uintptr_t self, uint32_t desc[8]);
		using clear_fn = long(__stdcall *)(uintptr_t self, uint32_t count, const int32_t *rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);

		struct state
		{
			uintptr_t call = 0, walk = 0, immediate = 0, callback = 0, code = 0;
			uint8_t saved_call[5] = {}, ours_call[5] = {};
			uint32_t ours_immediate = 0;
			bool in = false;
			uintptr_t renderer_ptr = 0;
			uintptr_t device_override = 0;
			void (*sink)(uint32_t, uint32_t) = nullptr;
			void (*listener)(uint32_t, uint32_t, uintptr_t) = nullptr;
			volatile bool wanted = false;
			bool marking = false;    // the walk's begin was marked: so is its end
			bool flush = false;      // in the see-through callback
			bool model_marks = false; // the model draw's stub sends the see-through markers
			counts n;
		};
		state g;

		uintptr_t client_device()
		{
			if (g.device_override != 0)
				return g.device_override;
			if (g.renderer_ptr == 0)
			{
				const uintptr_t p = find(kDevicePat);
				if (p == 0)
					return 0;
				g.renderer_ptr = word(p + 10);
			}
			const uint32_t r = word(g.renderer_ptr);
			return r != 0 ? word(r + 8) : 0;
		}

		template <typename F>
		F method(uintptr_t object, uint32_t index)
		{
			return reinterpret_cast<F>(uintptr_t(*reinterpret_cast<const uint32_t *>(*reinterpret_cast<const uint32_t *>(object) + 4 * index)));
		}

		bool depth_has_stencil(uintptr_t dev)
		{
			uintptr_t surface = 0;
			if (method<get_surface_fn>(dev, kGetDepthStencilSurface)(dev, &surface) < 0 || surface == 0)
				return false;
			uint32_t desc[8] = {};
			const bool got = method<get_desc_fn>(surface, kSurfaceGetDesc)(surface, desc) >= 0;
			method<release_fn>(surface, kRelease)(surface);
			return got && (desc[0] == 73 || desc[0] == 75 || desc[0] == 79); // D15S1, D24S8, D24X4S4
		}

		void send(uint32_t kind)
		{
			if (g.sink != nullptr)
			{
				g.sink(kind, g.n.seq);
				g.n.sent[kind]++;
				return;
			}
			const uintptr_t dev = client_device();
			if (dev == 0)
			{
				g.n.not_sent++;
				return;
			}
			// (the begin checks for the walk and its end; each see-through marker checks for itself)
			bool ok = g.marking;
			if (kind == slotmark::chars_begin)
				ok = g.marking = depth_has_stencil(dev);
			else if (kind == slotmark::see_through)
				ok = depth_has_stencil(dev);
			if (!ok)
			{
				g.n.not_sent++;
				return;
			}
			const int32_t x = int32_t(slotmark::x_of(kind, g.n.seq));
			const int32_t rect[4] = { x, 0, x + 1, 1 };
			if (method<clear_fn>(dev, kClear)(dev, 1, rect, kClearStencil, 0, 1.0f, 0) >= 0)
				g.n.sent[kind]++;
			else
				g.n.not_sent++;
		}

		// From the walk's stub: its begin (2) or end (3)
		// (one argument only: the stub's saved ecx above it isn't ours to change)
		void __cdecl on_slot(uint32_t kind)
		{
			if (kind == slotmark::chars_begin)
			{
				g.n.seq++;
				g.marking = false;
				g.n.frame_see_through = 0;
			}
			if (g.listener != nullptr)
				g.listener(kind, g.n.seq, 0);
			if (g.wanted)
				send(kind);
		}

		// From the wrapper, before and after the game's callback
		void __cdecl on_see_through(uintptr_t actor)
		{
			g.flush = true;
			g.n.see_through++;
			if (++g.n.frame_see_through > g.n.most_see_through)
				g.n.most_see_through = g.n.frame_see_through;
			if (g.model_marks)
				return;
			if (g.listener != nullptr)
				g.listener(slotmark::see_through, g.n.seq, actor);
			if (g.wanted)
				send(slotmark::see_through);
		}
		void __cdecl on_see_through_done()
		{
			g.flush = false;
		}

		void put_call(uint8_t *at, uintptr_t from, uintptr_t to)
		{
			at[0] = 0xE8;
			put32(at + 1, uint32_t(to - (from + 5)));
		}
	}

	bool start(std::string &why)
	{
		if (g.in)
			return true;
		const uintptr_t m = find(kCall), walk = find(kWalk), q = find(kQueue), callback = find(kCallback);
		if (m == 0 || walk == 0)
		{
			why = "the game's actor walk wasn't found";
			return false;
		}
		if (q == 0 || callback == 0)
		{
			why = "the game's see-through queue wasn't found";
			return false;
		}
		const uintptr_t call = m + kCallAt, immediate = q + kImmediateAt;
		uint8_t now[5] = {};
		if (!read(call, now, 5) || now[0] != 0xE8 || rel_target(call) != walk)
		{
			why = "CXiActorDraw's call isn't the actor walk";
			return false;
		}
		if (word(immediate) != uint32_t(callback))
		{
			why = "the see-through queue doesn't store the actor callback";
			return false;
		}
		if (g.code == 0)
		{
			g.code = alloc_code(kCodeSize);
			if (g.code == 0)
			{
				why = "no memory for the stubs";
				return false;
			}
		}
		uint8_t s[kCodeSize] = {
			0x51, 0x6A, 0x02, 0, 0, 0, 0, 0, 0x83, 0xC4, 0x04, 0x59,  // push ecx; push 2; call on_slot; add esp, 4; pop ecx
			0, 0, 0, 0, 0,                                            // call the walk
			0x50, 0x6A, 0x03, 0, 0, 0, 0, 0, 0x83, 0xC4, 0x04, 0x58,  // push eax; push 3; call on_slot; add esp, 4; pop eax
			0xC3, 0xCC, 0xCC };                                       // ret
		uint8_t *const w = s + kWrapperAt;
		// (the actor pushed again for the game's callback: a cdecl function may write over its own argument's slot)
		const uint8_t wrapper[] = {
			0xFF, 0x74, 0x24, 0x04, // push [esp+4] (the actor)
			0, 0, 0, 0, 0,          // call on_see_through
			0x83, 0xC4, 0x04,       // add esp, 4
			0xFF, 0x74, 0x24, 0x04, // push [esp+4]
			0, 0, 0, 0, 0,          // call the game's callback
			0x83, 0xC4, 0x04,       // add esp, 4
			0x50,                   // push eax
			0, 0, 0, 0, 0,          // call on_see_through_done
			0x58,                   // pop eax
			0xC3 };                 // ret
		std::memcpy(w, wrapper, sizeof(wrapper));
		const uintptr_t mark = reinterpret_cast<uintptr_t>(&on_slot), wrap = g.code + kWrapperAt;
		put_call(s + kCallMark1, g.code + kCallMark1, mark);
		put_call(s + kCallWalk, g.code + kCallWalk, walk);
		put_call(s + kCallMark2, g.code + kCallMark2, mark);
		put_call(w + kCallBegin, wrap + kCallBegin, reinterpret_cast<uintptr_t>(&on_see_through));
		put_call(w + kCallCallback, wrap + kCallCallback, callback);
		put_call(w + kCallDone, wrap + kCallDone, reinterpret_cast<uintptr_t>(&on_see_through_done));
		if (!write_code(g.code, s, kCodeSize))
		{
			why = "couldn't write the stubs";
			return false;
		}
		uint8_t ours[5];
		put_call(ours, call, g.code);
		const uint32_t ours_imm = uint32_t(wrap);
		if (!write_code(call, ours, 5))
		{
			why = "couldn't write the game's code";
			return false;
		}
		if (!write_code(immediate, &ours_imm, 4))
		{
			write_code(call, now, 5);
			why = "couldn't write the game's code";
			return false;
		}
		g.call = call;
		g.walk = walk;
		g.immediate = immediate;
		g.callback = callback;
		std::memcpy(g.saved_call, now, 5);
		std::memcpy(g.ours_call, ours, 5);
		g.ours_immediate = ours_imm;
		g.in = true;
		g.flush = false;
		g.n = counts();
		return true;
	}

	bool stop(std::string &why)
	{
		if (!g.in)
			return true;
		if (!code_is(g.call, g.ours_call, 5) || !code_is(g.immediate, &g.ours_immediate, 4))
		{
			why = "something else has changed the actor walk's call or the see-through queue since: left as they are";
			return false;
		}
		const uint32_t callback = uint32_t(g.callback);
		if (!write_code(g.call, g.saved_call, 5) || !write_code(g.immediate, &callback, 4))
		{
			why = "couldn't write the game's code";
			return false;
		}
		// (on the game's thread, between frames: no walk running and no actor queued with the wrapper)
		free_code(g.code);
		const uintptr_t device_override = g.device_override;
		void (*const sink)(uint32_t, uint32_t) = g.sink;
		void (*const listener)(uint32_t, uint32_t, uintptr_t) = g.listener;
		const bool model_marks = g.model_marks;
		g = state();
		g.device_override = device_override;
		g.sink = sink;
		g.listener = listener;
		g.model_marks = model_marks;
		return true;
	}

	bool in()
	{
		return g.in;
	}

	void set_wanted(bool wanted)
	{
		g.wanted = wanted;
	}

	bool from_flush()
	{
		return g.flush;
	}

	void set_model_marks(bool on)
	{
		g.model_marks = on;
	}

	void mark_see_through(uintptr_t actor)
	{
		if (!g.in)
			return;
		if (g.listener != nullptr)
			g.listener(slotmark::see_through, g.n.seq, actor);
		if (g.wanted)
			send(slotmark::see_through);
	}

	counts get_counts()
	{
		return g.n;
	}

	std::string status()
	{
		if (!g.in)
			return "character slots: not marked";
		char line[300];
		std::snprintf(line, sizeof(line),
			"character slots: marked at the actor walk and the see-through callback; %llu walks since, markers sent %llu begin + %llu end + %llu see-through "
			"(%llu see-through draws, most in one frame %u)%s%s",
			static_cast<unsigned long long>(g.n.seq), static_cast<unsigned long long>(g.n.sent[slotmark::chars_begin]),
			static_cast<unsigned long long>(g.n.sent[slotmark::chars_end]), static_cast<unsigned long long>(g.n.sent[slotmark::see_through]),
			static_cast<unsigned long long>(g.n.see_through), g.n.most_see_through,
			g.n.not_sent != 0 ? (", not sent " + std::to_string(g.n.not_sent)).c_str() : "", g.wanted ? "" : " (not sending: no Direct3D 12 side to see them)");
		return line;
	}

	sites where()
	{
		return { g.call, g.in ? g.code : 0, g.walk, g.immediate, g.in ? g.code + kWrapperAt : 0, g.callback };
	}

	void set_sink(void (*sink)(uint32_t, uint32_t))
	{
		g.sink = sink;
	}

	void set_device(uintptr_t device)
	{
		g.device_override = device;
	}

	void set_listener(void (*listener)(uint32_t kind, uint32_t seq, uintptr_t actor))
	{
		g.listener = listener;
	}
}
