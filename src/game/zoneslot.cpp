/*
 * The zone pass's slot marked (zoneslot.hpp, slotmark.hpp; research/ffxi-re/notes-replace-slots.md, "Zone: 178CCE call
 * 17B510").
 *
 * The zone task (178B30, priority 5) runs the whole zone pass as one call, only while a zone is loaded:
 *   178CC1: mov eax, [ebx+150h]; pop esi; test eax, eax; je +2C; mov ecx, ebx; call 17B510; jmp +23
 * 17B510 is thiscall on the zone renderer with no stack arguments and a plain ret; this is its only caller. The call's
 * rel32 is pointed at our stub (no code moved):
 *   push ecx; push ecx; push 0; call on_slot; add esp, 8; pop ecx      the zone's begin
 *   call 17B510                                              the game's pass, as before
 *   push eax; push eax; push 1; call on_slot; add esp, 8; pop eax      its end
 *   ret
 * on_slot is cdecl C++ (ebx, esi, edi, ebp kept); ecx and eax are kept around it for the pass and its caller. Its
 * second argument is what's under the kind: at the begin the saved ecx, the renderer.
 *
 * The marker: Clear(1, {x, 0, x + 1, 1}, D3DCLEAR_STENCIL, 0, 1.0, 0) on the client device (Ashita's wrapper, [renderer
 * + 8], through whatever vtable it has now: hifps's proxy flushes its batch at a Clear), when the depth surface bound
 * now has a stencil (D15S1, D24S8, D24X4S4: the world's is D24S8). Checked at the begin; the end follows it.
 */

#include "zoneslot.hpp"
#include "slotmark.hpp"
#include "directmark.hpp"
#include <cstdio>
#include <cstring>

namespace hifi::game::zoneslot
{
	namespace
	{
		constexpr const char *kCall = "8B83500100005E85C074??8BCBE8????????EB"; // 178CC1; the call at +0x0D
		constexpr intptr_t kCallAt = 0x0D;
		// The client's IDirect3DDevice8 (Ashita's wrapper object): [renderer + 8] (as hifps's exp finds it)
		constexpr const char *kDevicePat = "8BC8E8????????85C0A3????????0F84????????8B5C241C";

		constexpr size_t kStubSize = 0x20;
		constexpr size_t kCallMark1 = 0x04, kCallPass = 0x0D, kCallMark2 = 0x16; // (the stub's three calls)

		// IDirect3DDevice8 and IDirect3DSurface8, as far as used here
		constexpr uint32_t kRelease = 2, kGetDepthStencilSurface = 33, kClear = 36, kSurfaceGetDesc = 8;
		constexpr uint32_t kClearStencil = 4;            // D3DCLEAR_STENCIL
		using release_fn = unsigned long(__stdcall *)(uintptr_t self);
		using get_surface_fn = long(__stdcall *)(uintptr_t self, uintptr_t *out);
		using get_desc_fn = long(__stdcall *)(uintptr_t self, uint32_t desc[8]); // (D3DSURFACE_DESC: Format first)
		using clear_fn = long(__stdcall *)(uintptr_t self, uint32_t count, const int32_t *rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);

		struct state
		{
			uintptr_t call = 0, stub = 0, pass = 0;
			uint8_t saved[5] = {}, ours[5] = {};
			bool in = false;
			uintptr_t renderer_ptr = 0; // (where the renderer pointer is kept, found once)
			uintptr_t device_override = 0;
			void (*sink)(uint32_t, uint32_t) = nullptr;
			void (*listener)(uint32_t, uint32_t, uintptr_t) = nullptr;
			uintptr_t renderer = 0;     // (the zone renderer, from the begin)
			volatile bool wanted = false;
			bool marking = false;       // the begin was marked: so is the end
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

		// Whether the depth surface bound now has a stencil to mark
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
			// (our own d3d8.dll: a call, no stencil pixel)
			if (hifi::game::direct_mark(kind, g.n.seq))
			{
				g.n.sent[kind]++;
				return;
			}
			const uintptr_t dev = client_device();
			if (dev == 0)
			{
				g.n.not_sent++;
				return;
			}
			if (kind == slotmark::zone_begin)
				g.marking = depth_has_stencil(dev);
			if (!g.marking)
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

		// From the stub, on the game's thread: the zone's begin (0, with the renderer the stub saved: ecx) or end (1)
		void __cdecl on_slot(uint32_t kind, uintptr_t renderer)
		{
			if (kind == slotmark::zone_begin)
			{
				g.n.seq++;
				g.marking = false;
				g.renderer = renderer;
			}
			// (the listener before the marker: at the end, the pass's frame is published before its marker goes)
			if (g.listener != nullptr && kind < slotmark::kinds)
				g.listener(kind, g.n.seq, g.renderer);
			if (!g.wanted || kind >= slotmark::kinds)
				return;
			send(kind);
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
		const uintptr_t m = find(kCall);
		if (m == 0)
		{
			why = "the game's zone task wasn't found";
			return false;
		}
		const uintptr_t call = m + kCallAt;
		uint8_t now[5] = {};
		if (!read(call, now, 5) || now[0] != 0xE8)
		{
			why = "the zone task's call isn't as expected";
			return false;
		}
		const uintptr_t pass = rel_target(call);
		if (g.stub == 0)
		{
			g.stub = alloc_code(kStubSize);
			if (g.stub == 0)
			{
				why = "no memory for the stub";
				return false;
			}
		}
		uint8_t s[kStubSize] = {
			// (each register pushed twice: on_slot gets a copy for its second argument, which a cdecl callee may write over,
			// and the one popped back stays the caller's)
			0x51, 0x51, 0x6A, 0x00, 0, 0, 0, 0, 0, 0x83, 0xC4, 0x08, 0x59, // push ecx; push ecx; push 0; call on_slot; add esp, 8; pop ecx
			0, 0, 0, 0, 0,                                            // call the pass
			0x50, 0x50, 0x6A, 0x01, 0, 0, 0, 0, 0, 0x83, 0xC4, 0x08, 0x58, // push eax; push eax; push 1; call on_slot; add esp, 8; pop eax
			0xC3 };                                                         // ret
		const uintptr_t mark = reinterpret_cast<uintptr_t>(&on_slot);
		put_call(s + kCallMark1, g.stub + kCallMark1, mark);
		put_call(s + kCallPass, g.stub + kCallPass, pass);
		put_call(s + kCallMark2, g.stub + kCallMark2, mark);
		uint8_t ours[5];
		if (!write_code(g.stub, s, kStubSize))
		{
			why = "couldn't write the stub";
			return false;
		}
		put_call(ours, call, g.stub);
		if (!write_code(call, ours, 5))
		{
			why = "couldn't write the game's code";
			return false;
		}
		g.call = call;
		g.pass = pass;
		std::memcpy(g.saved, now, 5);
		std::memcpy(g.ours, ours, 5);
		g.in = true;
		g.n = counts();
		return true;
	}

	bool stop(std::string &why)
	{
		if (!g.in)
			return true;
		if (!code_is(g.call, g.ours, 5))
		{
			why = "something else has changed the zone task's call since: left as it is";
			return false;
		}
		if (!write_code(g.call, g.saved, 5))
		{
			why = "couldn't write the game's code";
			return false;
		}
		// (on the game's thread, as the zone task: nothing is in the stub now)
		free_code(g.stub);
		const uintptr_t device_override = g.device_override;
		void (*const sink)(uint32_t, uint32_t) = g.sink;
		void (*const listener)(uint32_t, uint32_t, uintptr_t) = g.listener;
		g = state();
		g.device_override = device_override;
		g.sink = sink;
		g.listener = listener;
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

	counts get_counts()
	{
		return g.n;
	}

	std::string status()
	{
		if (!g.in)
			return "zone slot: not marked";
		char line[200];
		std::snprintf(line, sizeof(line), "zone slot: marked at the zone task's call; %llu zone passes since, markers sent %llu begin + %llu end%s%s",
			static_cast<unsigned long long>(g.n.seq), static_cast<unsigned long long>(g.n.sent[slotmark::zone_begin]),
			static_cast<unsigned long long>(g.n.sent[slotmark::zone_end]), g.n.not_sent != 0 ? (", not sent " + std::to_string(g.n.not_sent)).c_str() : "",
			g.wanted ? "" : " (not sending: no Direct3D 12 side to see them)");
		return line;
	}

	sites where()
	{
		return { g.call, g.stub, g.pass };
	}

	void set_sink(void (*sink)(uint32_t, uint32_t))
	{
		g.sink = sink;
	}

	void set_device(uintptr_t device)
	{
		g.device_override = device;
	}

	void set_listener(void (*listener)(uint32_t kind, uint32_t seq, uintptr_t renderer))
	{
		g.listener = listener;
	}
}
