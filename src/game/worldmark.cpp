/*
 * The world part's slot markers (worldmark.hpp). The marker: Clear(1, {x, 0, x + 1, 1}, D3DCLEAR_STENCIL, 0, 1.0, 0)
 * on the client device, as charslot.cpp sends it (the same device lookup and stencil check, kept separate so theirs
 * stay as they are).
 */

#include "worldmark.hpp"
#include "slotmark.hpp"
#include "directmark.hpp"

namespace hifi::game::worldmark
{
	namespace
	{
		// The client's IDirect3DDevice8 (Ashita's wrapper object): [renderer + 8] (as zoneslot finds it)
		constexpr const char *kDevicePat = "8BC8E8????????85C0A3????????0F84????????8B5C241C";
		constexpr uint32_t kRelease = 2, kGetDepthStencilSurface = 33, kClear = 36, kSurfaceGetDesc = 8;
		constexpr uint32_t kClearStencil = 4; // D3DCLEAR_STENCIL
		using release_fn = unsigned long(__stdcall *)(uintptr_t self);
		using get_surface_fn = long(__stdcall *)(uintptr_t self, uintptr_t *out);
		using get_desc_fn = long(__stdcall *)(uintptr_t self, uint32_t desc[8]);
		using clear_fn = long(__stdcall *)(uintptr_t self, uint32_t count, const int32_t *rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);

		struct state
		{
			uintptr_t renderer_ptr = 0, device_override = 0;
			void (*sink)(uint32_t, uint32_t) = nullptr;
			volatile bool wanted = false;
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
		// (our own d3d8.dll: a call, no stencil pixel)
		if (hifi::game::direct_mark(kind, seq))
		{
			g.n.sent[kind]++;
			return true;
		}
		const uintptr_t dev = client_device();
		if (dev == 0 || !depth_has_stencil(dev))
		{
			g.n.not_sent++;
			return false;
		}
		const int32_t x = int32_t(slotmark::x_of(kind, seq));
		const int32_t rect[4] = { x, 0, x + 1, 1 };
		if (method<clear_fn>(dev, kClear)(dev, 1, rect, kClearStencil, 0, 1.0f, 0) < 0)
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

	void set_device(uintptr_t device)
	{
		g.device_override = device;
	}
}
