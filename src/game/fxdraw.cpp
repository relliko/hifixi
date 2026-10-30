/*
 * The effect elements handed to the native effect renderer (fxdraw.hpp; research/ffxi-re/notes-effects-draw.md).
 *
 * The flush (task priority 12): 055620: call 046380; mov ecx, [list]; call 055520 (at +0xB); mov ecx, [list]; jmp 0554B0.
 * The call's rel32 is pointed at a stub of ours (ecx, the list, kept for the walk):
 *   push ecx; push 0; call on_flush; add esp, 4; pop ecx; call 055520; push 1; call on_flush; add esp, 4; ret
 * The walk 055520 calls each queued item's callback with its argument:
 *   055533: mov eax, [esi+0Ch]; mov edi, [esi+4]; push eax; call [esi+8]; add esp, 4 (at +6, 7 bytes)
 * Those 7 bytes become call entry; nop; nop, the stub (esi the node, eax the argument; esi and edi kept by on_entry):
 *   push eax; push [esi+8]; call on_entry; add esp, 8; ret
 * An element's callback is 0555A0 (mov ecx, [esp+4]; mov eax, [ecx]; jmp [eax+2Ch]), its argument the element.
 *
 * The element draw 046070 (vt+0x2C of most classes): its colour (03E330 -> VA 0490BC0C), the hidden check (04609D,
 * gamerain's), BLENDOP ADD, vt+0x4C, then at 04612E: mov ecx, ebp; call 0467E0 (thiscall, the word 0xFFFF on the
 * stack, ret 4); test al, al; jne 046358 (past the depth pull, the class draw and 0472F0's restore). That call goes to
 * on_setup (a __fastcall with the same stack), which answers 1 for an element it recorded.
 *
 * What's recorded for an element (046070, 0467E0, the class draw), read straight from the game's memory under a
 * structured exception guard (an element whose memory faults is left to the game, nothing of it kept):
 *   - WORLD: +0xA0 after 045D40 (called here as 0467E0 calls it: the draw matrix is built only in the setup path);
 *   - TEXTUREFACTOR: VA 0490BC0C (046070's colour, alpha x fade x brightness) with 0467E0's letter changes (d, h: alpha
 *     = min(255, the letter's high byte x 2); b: the high byte) and the class's (the D3M's D: alpha >= 0x7F -> 0xFF;
 *     I: | 0xFFFFFF);
 *   - the blend by the letter (+0x16C), z write flags bit 12, PROJECTION: the depth pull (not flags bit 27, not a sky
 *     layer bit 22, +0x128 not 0, within 32 of the eye: near and far both less (1 - |z|/32)^3 x 0.03 x +0x128) or
 *     screen-fixed (bit 0, bits 6-8 clear, bits 2 and 3: field of view 2 atan2(192, 350)), each with the renderer's
 *     near/far shift (+0x39594 when +0x39598) as 046070 adds it;
 *   - fog unless flags bit 25: 18BA20 on the environment [VA 04AECB04] for the generator's area ([[+0xFC]]+0xC4) as
 *     0467E0 asks it (colour, end, start); table fog (wrapper +0x92D) by the view depth, else vertex fog (+0x92E) by
 *     the depth or the distance (RANGEFOG, +0x92F); H: the fog colour black;
 *   - TEXTURE0's offset (+0x120, +0x124) when +0x17B bits 0-1;
 *   - the class's geometry and stages.
 *
 * Classes (by the vtable's class draw, vt+0x48):
 *   040BA0 (the D3M element, vtable 0x32AD80): no model (+0x104) or a weather element (generator +0xDE bit 7) that
 *     002D80 hides: nothing drawn (taken, as the game's own early return). The model an MMB (+0x179): left to the game
 *     for now. Else the D3M (040220, the resource [[+0x104]], data at +0x30): its vertices as the load left them
 *     (03FEE0 doubled their colours in place), no vertex buffer (03FCF0 + 4): nothing; no textures: one untextured
 *     draw (352348, D: alpha x 2), but kinds 5-7 without textures are left to the game (their names begin where their
 *     vertices do, and the load stores the vertex buffer object over the first vertex's y once it has copied them;
 *     the value is only in the DAT now); else a draw per texture run (3524A8, bit 28: 3523D0; I: 352590), the texture object
 *     in each name's first dword (its resource at +8), and the untextured tail (352648) when +5. Lit (flags bit 16):
 *     left to the game for now.
 *   03EB30 (CMoD3aElem): the frame +0x170 of the D3A (record_d3a below), lit or not (03E9A0 sets LIGHTING 0).
 *   0565A0 (the rings, kind 0x24): record_ring below; lit: left to the game.
 *   Anything else: left to the game.
 * Weapon trails (CYyAfterImage) aren't elements: their flush callback 01C930 is recognised in the walk's stub and the
 * ring of samples recorded as one more element of the run (record_trail below).
 */

#include <windows.h>
#include "fxdraw.hpp"
#include "slotmark.hpp"
#include "../fx/fxframe.hpp"
#include "../fx/fxtextures.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace hifi::game::fxdraw
{
	namespace
	{
		namespace fx = hifi::fx;

		// 04610E: mov eax, [math]; push 0FFFFh; ... mov ecx, ebp; call 0467E0 (at +0x20)
		constexpr const char *kSetup = "A1????????68FFFF00008B88900A00008B9C88100A0000418988900A00008BCDE8";
		constexpr intptr_t kSetupCallAt = 0x20, kSetupMath = 1, kSetupGfx = 0x3B, kSetupMatrixCall = 0x5A, kSetupCamera = 0x8C, kSetupRenderer = 0x91;
		constexpr intptr_t kElementDrawFromSetup = 0x9E; // (046070 = 04610E - 0x9E)
		constexpr const char *kElementDraw = "83EC508D44240C558BE951";
		// 055620: call 046380; mov ecx, [list]; call 055520 (at +0xB); mov ecx, [list]; jmp 0554B0
		constexpr const char *kFlush = "E8????????8B0D????????E8????????8B0D????????E975FEFFFF";
		constexpr intptr_t kFlushCallAt = 0xB;
		constexpr const char *kWalk = "8B41045685C074268B7108C1E60503F0"; // 055520
		constexpr const char *kEntry = "8B460C8B7E0450FF560883C404C7461400000000"; // 055533, the 7 bytes at +6
		constexpr intptr_t kEntryAt = 6;
		constexpr uint8_t kEntryBytes[7] = { 0x50, 0xFF, 0x56, 0x08, 0x83, 0xC4, 0x04 };
		constexpr const char *kElementCallback = "8B4C24048B01FF602C"; // 0555A0
		constexpr const char *kD3mDraw = "568BF18B860401000085C074688B86FC000000"; // 040BA0: push colour at +0x58, call 002D80 at +0x27
		constexpr const char *kD3aDraw = "8B810401000085C0741E85C074048B00EB0233C0518B0D????????68????????518BC8E8"; // 03EB30
		constexpr const char *kTrailCallback = "8B4C240485C97405E903000000C3"; // 01C930 (-> 01C940)
		constexpr const char *kRingDraw = "83EC40535556578BF98A87B401000084C0"; // 0565A0
		constexpr intptr_t kD3mColour = 0x58, kD3mWeatherCall = 0x27;
		// 047018: mov ecx, [env]; push 0; push fog start; push fog end; push fog colour; call 18BA20 (at +0x17); call 0464D0
		constexpr const char *kFog = "8B0D????????6A0068????????68????????68????????E8????????E8";
		constexpr intptr_t kFogEnv = 2, kFogCall = 0x17;
		// The client's IDirect3DDevice8 (Ashita's wrapper object): [renderer + 8] (as zoneslot and charslot find it)
		constexpr const char *kDevicePat = "8BC8E8????????85C0A3????????0F84????????8B5C241C";

		constexpr size_t kCodeSize = 0x40, kEntryStubAt = 0x20;
		constexpr size_t kFlushMark1 = 0x03, kFlushWalk = 0x0C, kFlushMark2 = 0x13, kEntryCall = 0x24;

		constexpr uint32_t kRelease = 2, kGetDepthStencilSurface = 33, kClear = 36, kSurfaceGetDesc = 8;
		constexpr uint32_t kClearStencil = 4; // D3DCLEAR_STENCIL
		using release_fn = unsigned long(__stdcall *)(uintptr_t self);
		using get_surface_fn = long(__stdcall *)(uintptr_t self, uintptr_t *out);
		using get_desc_fn = long(__stdcall *)(uintptr_t self, uint32_t desc[8]);
		using clear_fn = long(__stdcall *)(uintptr_t self, uint32_t count, const int32_t *rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);

		using setup_fn = uint32_t(__fastcall *)(uintptr_t element, uintptr_t edx, uint32_t arg);
		using callback_fn = uint32_t(__cdecl *)(uintptr_t arg);
		using matrix_fn = void(__fastcall *)(uintptr_t element, uintptr_t edx);
		using weather_fn = float(__cdecl *)();
		using fog_fn = void(__fastcall *)(uintptr_t env, uintptr_t edx, uint32_t *colour, float *end, float *start, uint32_t none, uint32_t area);

		// Element fields (notes-effects-update.md "Fields the frame touches")
		constexpr uintptr_t kWorld = 0xA0, kGen = 0xFC, kModel = 0x104, kFlags = 0x10C, kUv = 0x120, kBias = 0x128, kLetter = 0x16C, kMmb = 0x179, kUvBits = 0x17B;

		struct state
		{
			// The sites
			uintptr_t setup_call = 0, setup = 0, flush_call = 0, walk = 0, entry_site = 0, element_cb = 0, element_draw = 0, d3m_draw = 0;
			uintptr_t draw_matrix = 0, weather = 0, fog_env = 0, d3a_draw = 0, trail_cb = 0, ring_draw = 0;
			uintptr_t colour = 0, gfx = 0, renderer = 0, camera = 0, env = 0, math = 0;
			uintptr_t code = 0;
			uint8_t saved_setup[5] = {}, ours_setup[5] = {}, saved_flush[5] = {}, ours_flush[5] = {}, ours_entry[7] = {};
			bool in = false;
			// The plugin's say
			volatile bool native = false, wanted = false, probe = false;
			double clock = 0.0;
			// The marker device
			uintptr_t renderer_ptr = 0, device_override = 0;
			void (*sink)(uint32_t, uint32_t) = nullptr;
			// The flush under way
			bool in_flush = false, taking = false, can_mark = false;
			uint32_t seq = 0;
			fx::frame *f = nullptr;
			uint32_t run_first = 0;
			counts n;
		};
		state g;

		template <typename T>
		T at(uintptr_t a)
		{
			T v;
			std::memcpy(&v, reinterpret_cast<const void *>(a), sizeof(T));
			return v;
		}

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

		// Whether this flush's markers can go out (a sink, or the device with a stencil on the bound depth)
		bool markers_possible()
		{
			if (g.sink != nullptr)
				return true;
			const uintptr_t dev = client_device();
			return dev != 0 && depth_has_stencil(dev);
		}

		void send_marker()
		{
			if (g.sink != nullptr)
			{
				g.sink(slotmark::fx_run, g.seq);
				g.n.markers++;
				return;
			}
			const uintptr_t dev = client_device();
			if (dev == 0)
			{
				g.n.not_sent++;
				return;
			}
			const int32_t x = int32_t(slotmark::x_of(slotmark::fx_run, g.seq));
			const int32_t rect[4] = { x, 0, x + 1, 1 };
			if (method<clear_fn>(dev, kClear)(dev, 1, rect, kClearStencil, 0, 1.0f, 0) >= 0)
				g.n.markers++;
			else
				g.n.not_sent++;
		}

		// The run of elements recorded since the last one published, and its marker (an empty run too, when asked)
		void close_run(bool even_empty = false)
		{
			if (g.f == nullptr || !g.can_mark)
				return;
			const uint32_t count = g.f->element_count - g.run_first;
			if (count == 0 && !even_empty)
				return;
			const uint32_t r = g.f->runs.load(std::memory_order_relaxed);
			if (r >= fx::kMaxRuns)
				return; // (never with elements in it: record() stops taking before)
			g.f->run_list[r].first = g.run_first;
			g.f->run_list[r].count = count;
			g.f->runs.store(r + 1, std::memory_order_release);
			g.run_first = g.f->element_count;
			g.n.runs++;
			send_marker();
		}

		// 0467E0's blend and its letter changes (fx::letter_blend); the subtractive letters only with the device's blend
		// ops (046550 otherwise draws a copy of the frame back: left to the game)
		bool letter_blend(uint16_t letter, uint32_t &colour, uint8_t &blend)
		{
			if (!fx::letter_blend(letter, colour, blend))
				return false;
			if (blend != fx::blend_subtract)
				return true;
			const uintptr_t gfx = word(g.gfx);
			return gfx != 0 && (word(gfx + 0xDEC) & 0x800) != 0;
		}

		// The D3M's names and vertices (03FCF0, 03FD70) from its data (resource + 0x30)
		uintptr_t d3m_names(uintptr_t data)
		{
			const uint32_t kind = at<uint8_t>(data) & 0xF;
			if (kind < 5 || kind > 7)
				return data + 0x10;
			const uintptr_t counts = data + 8;
			const uint32_t textures = at<uint8_t>(data + 4);
			if (kind == 7 && textures == 0)
				return counts;
			const uint32_t n = textures + at<uint8_t>(data + 5);
			return n % 4 != 0 ? counts + (n - n % 4) * 2 + 6 : counts + n * 2;
		}
		uintptr_t d3m_vertices(uintptr_t data)
		{
			const uint32_t kind = at<uint8_t>(data) & 0xF;
			if (kind < 5 || kind > 7)
				return data + 0x50;
			return d3m_names(data) + uintptr_t(at<uint8_t>(data + 4)) * 16;
		}

		// A texture object's texture id once it's on the GPU (kNone: not yet, or not to be had); new ones queued
		uint32_t texture_of(uintptr_t object)
		{
			if (!pointer(uint32_t(object)))
				return fx::kNone;
			const uint32_t res = word(object + 8);
			if (!pointer(res))
				return fx::kNone;
			fx::textures::key k;
			k.resource = res;
			k.handle = word(res + 0x14);
			k.header = word(res + 0x24);
			k.data = word(res + 0x30 + 6);
			uint32_t id = fx::textures::find(k, g.clock);
			if (id == fx::kNone)
			{
				const uint32_t size = ((k.header >> 7) & 0x7FFFF) * 16;
				if ((k.header & 0x7F) != 0x20 || size <= 16 || size > (8u << 20))
					return fx::kNone;
				std::vector<uint8_t> bytes(size - 16);
				if (!read(res + 0x30, bytes.data(), bytes.size()))
					return fx::kNone;
				id = fx::textures::add(k, std::move(bytes), g.clock);
			}
			return id != fx::kNone && fx::textures::ready(id) ? id : fx::kNone;
		}

		// What every class shares: WORLD, TEXTUREFACTOR, blend, z write, projection, fog, the UV offset. False: the
		// game's (a blend letter without a case)
		bool common(uintptr_t e, fx::element &out)
		{
			const uint32_t flags = at<uint32_t>(e + kFlags);
			const uint16_t letter = at<uint16_t>(e + kLetter);
			uint32_t colour = word(g.colour);
			uint8_t blend = fx::blend_add;
			if (!letter_blend(letter, colour, blend))
			{
				g.n.letter++;
				return false;
			}
			// (the draw matrix, as 0467E0 builds it)
			reinterpret_cast<matrix_fn>(g.draw_matrix)(e, 0);
			std::memcpy(out.world, reinterpret_cast<const void *>(e + kWorld), 64);
			out.tfactor = colour;
			out.blend = blend;
			out.z_write = (flags >> 12) & 1;
			// PROJECTION (046070): the depth pull, else screen-fixed
			const uintptr_t renderer = word(g.renderer), camera = word(g.camera);
			const float bias = renderer != 0 && at<uint8_t>(renderer + 0x39598) != 0 ? at<float>(renderer + 0x39594) : 0.0f;
			const float cam_near = camera != 0 ? at<float>(camera + 0x2DC) : 0.0f, cam_far = camera != 0 ? at<float>(camera + 0x2E0) : 0.0f;
			const float pull = at<float>(e + kBias);
			out.projection = fx::proj_camera;
			if ((flags & (1u << 27)) == 0 && pull != 0.0f && (flags & (1u << 22)) == 0)
			{
				const float z = std::fabs(out.world[14]);
				if (z < 32.0f && camera != 0)
				{
					const float k = 1.0f - z * 0.03125f;
					const float s = k * k * k * 0.03f * pull;
					out.projection = fx::proj_near_far;
					out.near_z = cam_near - s + bias;
					out.far_z = cam_far - s + bias;
				}
			}
			else if ((flags & 1) != 0 && (flags & 0x1C0) == 0 && (flags & 4) != 0 && (flags & 8) != 0 && camera != 0)
			{
				// (D3DXMatrixPerspectiveFovRH: _22 = cot(fov / 2) = 350 / 192, _11 = _22 / aspect, aspect = 1 / (+0x2E8 x +0x2F0))
				out.projection = fx::proj_screen;
				out.near_z = cam_near + bias;
				out.far_z = cam_far + bias;
				out.scale_y = 350.0f / 192.0f;
				out.scale_x = out.scale_y * at<float>(camera + 0x2E8) * at<float>(camera + 0x2F0);
			}
			// Fog
			out.fog = 0;
			if ((flags & (1u << 25)) == 0)
			{
				const uintptr_t gfx = word(g.gfx);
				const uintptr_t wrapper = gfx != 0 ? word(gfx + 0xDC8) : 0;
				const uint8_t table = wrapper != 0 ? at<uint8_t>(wrapper + 0x92D) : 0, vertex = wrapper != 0 ? at<uint8_t>(wrapper + 0x92E) : 0;
				const uint8_t range = wrapper != 0 ? at<uint8_t>(wrapper + 0x92F) : 0;
				const uint32_t handle = word(e + kGen);
				const uint32_t gen = handle != 0 ? word(handle) : 0;
				const uint32_t area = gen != 0 ? word(gen + 0xC4) : 0;
				uint32_t fog_colour = 0;
				float fog_end = 0.0f, fog_start = 0.0f;
				const uintptr_t env = word(g.env);
				if (env != 0)
				{
					reinterpret_cast<fog_fn>(g.fog_env)(env, 0, &fog_colour, &fog_end, &fog_start, 0, area);
					out.fog = table != 0 ? 1 : vertex != 0 ? (range != 0 ? 2 : 1) : 0;
				}
				out.fog_colour = (letter & 0xFF) == 0x48 ? 0 : fog_colour;
				out.fog_start = fog_start;
				out.fog_end = fog_end;
			}
			out.uv[0] = out.uv[1] = 0.0f;
			if ((at<uint8_t>(e + kUvBits) & 3) != 0)
			{
				out.uv[0] = at<float>(e + kUv);
				out.uv[1] = at<float>(e + kUv + 4);
			}
			return true;
		}

		// The D3M (040220) into the frame; false: the game's
		bool record_d3m(uintptr_t e, uintptr_t resource)
		{
			fx::frame &f = *g.f;
			const uintptr_t data = resource + 0x30;
			const uint32_t textures = at<uint8_t>(data + 4), tail = at<uint8_t>(data + 5), triangles = at<uint16_t>(data + 6);
			const uintptr_t names = d3m_names(data), vertices = d3m_vertices(data);
			if (word(names + 4) == 0)
			{
				g.n.empty++;
				return true; // (no vertex buffer: 040220 draws nothing)
			}
			const uint16_t letter = at<uint16_t>(e + kLetter);
			const uint32_t flags = at<uint32_t>(e + kFlags);
			// The draws, as 040220 makes them
			fx::draw draws[16];
			uint32_t count = 0, used = 0;
			if (textures == 0)
			{
				// (kinds 5-7 without textures: the names (03FCF0) begin where the vertices do, and the load stores the
				// vertex buffer object at names + 4 after copying them: the first vertex's y is gone from the chunk)
				const uint32_t kind = at<uint8_t>(data) & 0xF;
				if (kind >= 5 && kind <= 7)
				{
					g.n.overwritten++;
					return false;
				}
				draws[count++] = { 0, triangles * 3, fx::kNone, uint32_t((letter & 0xFF) == 0x44 ? fx::stage_alpha_2x : 0) };
				used = triangles * 3;
			}
			else
			{
				if (textures > 13)
				{
					g.n.unsupported++;
					return false;
				}
				const bool i_letter = (letter & 0xFF) == 0x49;
				const uint32_t stages = i_letter ? fx::stage_texture | fx::stage_colour_diffuse | fx::stage_colour_tfactor
					: fx::stage_texture | ((flags & (1u << 28)) != 0 ? fx::stage_alpha_diffuse : 0);
				uint32_t first = 0;
				for (uint32_t i = 0; i < textures; ++i)
				{
					const uint32_t object = word(names + 16 * i);
					const uint32_t id = texture_of(object);
					if (id == fx::kNone)
					{
						g.n.texture++;
						return false;
					}
					const uint32_t n = uint32_t(at<uint16_t>(data + 8 + 2 * i)) * 3;
					draws[count++] = { first, n, id, stages };
					first += n;
				}
				used = first;
				if (tail != 0)
				{
					const uint32_t rest = first / 3 <= triangles ? (triangles - first / 3) * 3 : 0;
					draws[count++] = { first, rest, fx::kNone, 0 };
					used = first + rest;
				}
			}
			if (used > triangles * 3)
				used = triangles * 3;
			if (f.element_count >= fx::kMaxElements || f.draw_count + count > fx::kMaxDraws || f.vertex_count + used > fx::kMaxVertices)
			{
				g.n.full++;
				return false;
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			if (!common(e, out))
				return false;
			// (the D3M's TEXTUREFACTOR: D's alpha from 0x7F up full; I's colour white, blended DESTCOLOR / SRCALPHA)
			if ((letter & 0xFF) == 0x44 && (out.tfactor >> 24) >= 0x7F)
				out.tfactor |= 0xFF000000u;
			if (textures != 0 && (letter & 0xFF) == 0x49)
			{
				out.tfactor |= 0x00FFFFFFu;
				out.blend = fx::blend_multiply;
			}
			std::memcpy(f.vertices + f.vertex_count, reinterpret_cast<const void *>(vertices), size_t(used) * sizeof(fx::vertex));
			out.first_draw = f.draw_count;
			for (uint32_t i = 0; i < count; ++i)
			{
				fx::draw d = draws[i];
				if (d.first + d.count > used)
					d.count = d.first < used ? used - d.first : 0;
				d.first += f.vertex_count;
				f.draws[f.draw_count++] = d;
			}
			out.draws = count;
			out.cls = fx::class_d3m;
			f.vertex_count += used;
			f.element_count++;
			g.n.taken[fx::class_d3m]++;
			return true;
		}

		// The D3A (03EB30 -> 03E910) into the frame; false: the game's. The resource keeps the texture object in the
		// name's first dword (data +8, 075520), the vertex buffer object at +0xC and the frame table at +0x14 (03E6C0:
		// per frame u16 quads, s16 first vertex); the frames' vertices stay in the chunk as the DAT has them (24 bytes:
		// x y z, D3DCOLOR, u v), the load putting them in the vertex buffer with a normal (0, 0, 1) and the UVs times 1 /
		// the texture's width and height when +0 >= 1 and +7 is 0 (the texture object's +0x24, +0x26: 075FB0, 075FD0).
		// The draw: the frame +0x170 (wrapped to 0 at the frame count, written back as 03E910 does), table 351FA0 (bit
		// 28: 351EC8; @: 352084, but @ has no blend of its own in 0467E0, so never here), TEXTUREFACTOR the colour,
		// LIGHTING 0, FOGENABLE 0, ZWRITEENABLE 0, quads x 2 triangles from the frame's first vertex.
		bool record_d3a(uintptr_t e, uintptr_t resource)
		{
			fx::frame &f = *g.f;
			const uintptr_t data = resource + 0x30;
			if (word(data + 0xC) == 0)
			{
				g.n.empty++;
				return true; // (no vertex buffer: 03E910 draws nothing)
			}
			const int16_t frames = at<int16_t>(data + 2), units = at<int16_t>(data);
			uint16_t frame = at<uint16_t>(e + 0x170);
			if (int32_t(frame) >= int32_t(frames))
			{
				frame = 0;
				*reinterpret_cast<uint16_t *>(e + 0x170) = 0;
			}
			const uintptr_t table = word(data + 0x14);
			if (table == 0 || frames <= 0)
			{
				g.n.unsupported++;
				return false;
			}
			const int32_t quads = at<int16_t>(table + 4 * uintptr_t(frame)), first = at<int16_t>(table + 4 * uintptr_t(frame) + 2);
			const uint32_t object = word(data + 8);
			const uint32_t id = texture_of(object);
			if (id == fx::kNone)
			{
				g.n.texture++;
				return false;
			}
			// (the frame's vertices in the chunk: the heads walked to it; its first vertex must be the table's)
			uintptr_t p = data + 0x18;
			int32_t at_vertex = 0;
			for (uint32_t i = 0; i < frame; ++i)
			{
				const uint32_t q = at<uint8_t>(p + 2);
				at_vertex += int32_t(q) * 6;
				p += 4 + uintptr_t(q) * 6 * 24;
			}
			const uint32_t n = uint32_t(quads) * 6;
			if (at_vertex != first || quads < 0 || at<uint8_t>(p + 2) != uint32_t(quads))
			{
				g.n.unsupported++;
				return false;
			}
			if (f.element_count >= fx::kMaxElements || f.draw_count + 1 > fx::kMaxDraws || f.vertex_count + n > fx::kMaxVertices)
			{
				g.n.full++;
				return false;
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			if (!common(e, out))
				return false;
			float su = 1.0f, sv = 1.0f;
			if (units >= 1 && at<uint8_t>(data + 7) == 0)
			{
				const uint16_t w = at<uint16_t>(object + 0x24), h = at<uint16_t>(object + 0x26);
				su = w != 0 ? 1.0f / float(w) : 1.0f;
				sv = h != 0 ? 1.0f / float(h) : 1.0f;
			}
			const uintptr_t v0 = p + 4;
			for (uint32_t k = 0; k < n; ++k)
			{
				const uintptr_t s = v0 + uintptr_t(k) * 24;
				fx::vertex &v = f.vertices[f.vertex_count + k];
				std::memcpy(v.pos, reinterpret_cast<const void *>(s), 12);
				v.normal[0] = v.normal[1] = 0.0f;
				v.normal[2] = 1.0f;
				v.colour = at<uint32_t>(s + 12);
				v.uv[0] = at<float>(s + 16) * su;
				v.uv[1] = at<float>(s + 20) * sv;
			}
			const uint32_t flags = at<uint32_t>(e + kFlags);
			out.z_write = 0;
			out.fog = 0;
			out.first_draw = f.draw_count;
			out.draws = 1;
			out.cls = fx::class_d3a;
			f.draws[f.draw_count++] = { f.vertex_count, n, id, fx::stage_texture | ((flags & (1u << 28)) != 0 ? fx::stage_alpha_diffuse : 0u) };
			f.vertex_count += n;
			f.element_count++;
			g.n.taken[fx::class_d3a]++;
			return true;
		}

		// A weapon trail (CYyAfterImage, its flush callback 01C930 -> 01C940) into the frame; false: the game's. Its
		// ring of 80 samples (head +0x14A8, count +0x14AC): tip +0x40[i], base +0x540[i] (x y z w, the world's), colour
		// A +0xA40[i] and B +0xF40[i] (r g b a floats, 0..255, truncated into a D3DCOLOR). From the newest back: +0x151C 0
		// one strip of (tip, A), (base, B) pairs; else two from the middle line (tip + base) / 2: (mid, A), (tip, A) and
		// (mid, A), (base, B). WORLD identity (VIEW the camera's), FVF 0x42, colour and alpha the vertex's, LIGHTING 0,
		// FOGENABLE 0, Z test on, ZWRITEENABLE 0, SRCALPHA / ONE, CULL NONE.
		bool record_trail(uintptr_t t)
		{
			fx::frame &f = *g.f;
			const int32_t head = at<int32_t>(t + 0x14A8), count = at<int32_t>(t + 0x14AC);
			const uint32_t mode = at<uint32_t>(t + 0x151C);
			if (count <= 1 || count > 80 || head < 0 || head > 80)
			{
				g.n.empty++;
				return true; // (fewer than two samples: no triangle; 01C940 draws 2 x count - 2 of them)
			}
			const uint32_t strips = mode != 0 ? 2 : 1;
			const uint32_t n = strips * (2 * uint32_t(count) - 2) * 3;
			if (f.element_count >= fx::kMaxElements || f.draw_count + 1 > fx::kMaxDraws || f.vertex_count + n > fx::kMaxVertices ||
				f.runs.load(std::memory_order_relaxed) + 2 >= fx::kMaxRuns)
			{
				g.n.full++;
				return false;
			}
			const auto colour = [&](uintptr_t c) {
				const auto b = [&](uintptr_t o) { return uint32_t(int32_t(at<float>(c + o))); };
				return b(0xC) << 24 | b(0) << 16 | b(4) << 8 | b(8);
			};
			fx::vertex strip[2][160];
			for (int32_t k = 0, i = head - 1; k < count; ++k, --i)
			{
				if (i < 0)
					i += 80;
				const uintptr_t tip = t + 0x40 + 16 * uintptr_t(i), base = t + 0x540 + 16 * uintptr_t(i);
				const uint32_t a = colour(t + 0xA40 + 16 * uintptr_t(i)), b = colour(t + 0xF40 + 16 * uintptr_t(i));
				float pt[3], pb[3], mid[3];
				std::memcpy(pt, reinterpret_cast<const void *>(tip), 12);
				std::memcpy(pb, reinterpret_cast<const void *>(base), 12);
				for (int c = 0; c < 3; ++c)
					mid[c] = (pt[c] + pb[c]) * 0.5f;
				const auto put = [](fx::vertex &v, const float *p, uint32_t col) {
					v = fx::vertex();
					std::memcpy(v.pos, p, 12);
					v.colour = col;
				};
				if (mode == 0)
				{
					put(strip[0][2 * k], pt, a);
					put(strip[0][2 * k + 1], pb, b);
				}
				else
				{
					put(strip[0][2 * k], mid, a);
					put(strip[0][2 * k + 1], pt, a);
					put(strip[1][2 * k], mid, a);
					put(strip[1][2 * k + 1], pb, b);
				}
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			std::memcpy(out.world, f.view, 64);
			out.blend = fx::blend_add;
			out.z_write = 0;
			out.fog = 0;
			out.projection = fx::proj_camera;
			uint32_t w = f.vertex_count;
			for (uint32_t s = 0; s < strips; ++s)
				for (int32_t k = 0; k + 2 < 2 * count; ++k)
					for (int j = 0; j < 3; ++j)
						f.vertices[w++] = strip[s][k + j];
			out.first_draw = f.draw_count;
			out.draws = 1;
			out.cls = fx::class_trail;
			f.draws[f.draw_count++] = { f.vertex_count, n, fx::kNone, fx::stage_diffuse };
			f.vertex_count += n;
			f.element_count++;
			g.n.taken[fx::class_trail]++;
			return true;
		}

		bool record_trail_guarded(uintptr_t t)
		{
			__try
			{
				return pointer(uint32_t(t)) && record_trail(t);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g.n.faulted++;
				return false;
			}
		}

		// Procedural rings (kind 0x24, 0565A0) into the frame; false: the game's. +0x1B4 segments, +0x1B5 rings (signed
		// bytes), radii +0x194 + 4i, colours +0x1A4 + 4i (bytes R G B A, made a D3DCOLOR by 03E280). Each band between
		// two rings is a strip in the element's XY plane of (sin a x r, cos a x r, 0) pairs, inner then outer, at angles
		// 0, 0, d, 2d .. (n - 2) d, then the first pair again (d = 2 pi / n): DrawPrimitive(TRIANGLESTRIP, 2n) per band,
		// FVF XYZ | DIFFUSE, table 353810 (untextured), TEXTUREFACTOR the element's own colour +0xF8 (not 046070's: no
		// fade, no brightness, no letter change).
		bool record_ring(uintptr_t e)
		{
			fx::frame &f = *g.f;
			const int32_t segments = at<int8_t>(e + 0x1B4), rings = at<int8_t>(e + 0x1B5);
			if (segments <= 0 || rings <= 1)
			{
				g.n.empty++;
				return true;
			}
			if (rings > 4)
			{
				g.n.unsupported++;
				return false;
			}
			const uint32_t bands = uint32_t(rings - 1), per_band = uint32_t(segments) * 2 * 3;
			if (f.element_count >= fx::kMaxElements || f.draw_count + 1 > fx::kMaxDraws || f.vertex_count + bands * per_band > fx::kMaxVertices)
			{
				g.n.full++;
				return false;
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			if (!common(e, out))
				return false;
			out.tfactor = at<uint32_t>(e + 0xF8);
			const float step = 6.283184f / float(segments);
			uint32_t w = f.vertex_count;
			for (uint32_t b = 0; b < bands; ++b)
			{
				const float r[2] = { at<float>(e + 0x194 + 4 * b), at<float>(e + 0x194 + 4 * (b + 1)) };
				uint32_t col[2];
				for (int k = 0; k < 2; ++k)
				{
					const uint32_t c = at<uint32_t>(e + 0x1A4 + 4 * (b + k));
					col[k] = (c & 0xFF000000u) | (c & 0xFF) << 16 | (c & 0xFF00) | ((c >> 16) & 0xFF);
				}
				// (the strip's 2n + 2 points: angle 0, then 0, d .. (n - 2) d, then angle 0 again)
				fx::vertex strip[2 * 128 + 2];
				const uint32_t points = uint32_t(segments) + 1;
				if (points > 129)
				{
					g.n.unsupported++;
					return false;
				}
				for (uint32_t p = 0; p < points; ++p)
				{
					const float a = p == 0 || p + 1 == points ? 0.0f : float(p - 1) * step;
					for (int k = 0; k < 2; ++k)
					{
						fx::vertex &v = strip[2 * p + k];
						v = fx::vertex();
						v.pos[0] = std::sin(a) * r[k];
						v.pos[1] = std::cos(a) * r[k];
						v.colour = col[k];
					}
				}
				for (uint32_t k = 0; k + 2 < 2 * points; ++k)
					for (uint32_t j = 0; j < 3; ++j)
						f.vertices[w++] = strip[k + j];
			}
			const uint32_t n = w - f.vertex_count;
			out.first_draw = f.draw_count;
			out.draws = 1;
			out.cls = fx::class_ring;
			f.draws[f.draw_count++] = { f.vertex_count, n, fx::kNone, 0 };
			f.vertex_count = w;
			f.element_count++;
			g.n.taken[fx::class_ring]++;
			return true;
		}

		// An element into the frame: true when it's taken (the game's setup and draw skipped)
		bool record(uintptr_t e)
		{
			const uintptr_t vt = word(e);
			if (vt == 0)
				return false;
			const uintptr_t cls = word(vt + 0x48);
			if (g.f->runs.load(std::memory_order_relaxed) + 2 >= fx::kMaxRuns)
			{
				g.n.full++;
				return false;
			}
			if (cls == g.d3a_draw && g.d3a_draw != 0)
			{
				const uint32_t handle = word(e + kModel);
				if (handle == 0)
				{
					g.n.empty++;
					return true;
				}
				const uint32_t resource = word(handle);
				if (resource == 0)
				{
					g.n.unsupported++;
					return false;
				}
				return record_d3a(e, resource);
			}
			if (cls == g.ring_draw && g.ring_draw != 0)
			{
				if ((at<uint8_t>(e + kFlags + 2) & 1) != 0)
				{
					g.n.lit++;
					return false;
				}
				return record_ring(e);
			}
			if (cls == g.d3m_draw)
			{
				// (lit elements: the area's lights, not reproduced yet)
				if ((at<uint8_t>(e + kFlags + 2) & 1) != 0)
				{
					g.n.lit++;
					return false;
				}
				const uint32_t handle = word(e + kModel);
				if (handle == 0)
				{
					g.n.empty++;
					return true;
				}
				const uint32_t gh = word(e + kGen);
				const uint32_t gen = gh != 0 ? word(gh) : 0;
				if (gen != 0 && (at<uint8_t>(gen + 0xDE) & 0x80) != 0 && reinterpret_cast<weather_fn>(g.weather)() == 0.0f)
				{
					g.n.empty++;
					return true;
				}
				if (at<uint8_t>(e + kMmb) != 0)
				{
					g.n.unsupported++;
					return false;
				}
				const uint32_t resource = word(handle);
				if (resource == 0)
				{
					g.n.unsupported++;
					return false;
				}
				return record_d3m(e, resource);
			}
			g.n.unsupported++;
			return false;
		}

		// record() under a guard: an element whose memory faults is the game's (nothing of it kept: the frame's counts
		// move only once it's all read)
		bool record_guarded(uintptr_t e)
		{
			__try
			{
				return record(e);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g.n.faulted++;
				return false;
			}
		}

		void put_call(uint8_t *at_, uintptr_t from, uintptr_t to)
		{
			at_[0] = 0xE8;
			put32(at_ + 1, uint32_t(to - (from + 5)));
		}
	}

	void __cdecl on_flush(uint32_t end)
	{
		if (end == 0)
		{
			g.in_flush = true;
			g.n.flushes++;
			if (++g.seq == 0)
				g.seq = 1;
			g.f = &fx::frame_slot(g.seq);
			g.f->runs.store(0, std::memory_order_relaxed);
			g.f->element_count = g.f->draw_count = g.f->vertex_count = 0;
			g.run_first = 0;
			const uintptr_t renderer = word(g.renderer);
			g.can_mark = g.wanted && markers_possible();
			g.taking = g.native && g.can_mark && renderer != 0 && word(g.camera) != 0;
			if (renderer != 0)
				std::memcpy(g.f->proj, reinterpret_cast<const void *>(renderer + 0x38DDC), 64);
			if (const uintptr_t gfx = word(g.gfx); gfx != 0)
				std::memcpy(g.f->view, reinterpret_cast<const void *>(gfx + 0xB50), 64);
			g.f->seq.store(g.seq, std::memory_order_release);
			return;
		}
		// The last run; an empty one for the textures waiting to be copied in, or as proof that markers come through
		close_run();
		if (g.f != nullptr && g.f->runs.load(std::memory_order_relaxed) == 0 && (g.probe || fx::textures::waiting() != 0))
			close_run(true);
		if (g.f != nullptr)
		{
			g.n.last_elements = g.f->element_count;
			g.n.last_runs = g.f->runs.load(std::memory_order_relaxed);
		}
		g.in_flush = false;
		g.taking = false;
		g.f = nullptr;
	}

	void __cdecl on_entry(uintptr_t callback, uintptr_t arg)
	{
		if (g.in_flush && g.taking)
		{
			// (a weapon trail: drawn by HiFi XI with the run, else the game's after it)
			if (callback == g.trail_cb && g.trail_cb != 0 && record_trail_guarded(arg))
				return;
			// (anything but an element drawn through 046070 draws now: the run so far first)
			bool element = false;
			if (callback == g.element_cb && pointer(uint32_t(arg)))
			{
				const uint32_t vt = word(arg);
				element = vt != 0 && word(vt + 0x2C) == g.element_draw;
			}
			if (!element)
				close_run();
		}
		reinterpret_cast<callback_fn>(callback)(arg);
	}

	uint32_t __fastcall on_setup(uintptr_t element, uintptr_t, uint32_t arg)
	{
		if (g.in_flush && g.taking && arg == 0xFFFF)
		{
			if (record_guarded(element))
				return 1;
			close_run();
		}
		else if (g.in_flush)
			g.n.game++;
		return reinterpret_cast<setup_fn>(g.setup)(element, 0, arg);
	}

	bool start(std::string &why)
	{
		if (g.in)
			return true;
		const uintptr_t s = find(kSetup), fl = find(kFlush), walk = find(kWalk), entry = find(kEntry), cb = find(kElementCallback), d3m = find(kD3mDraw), fog = find(kFog);
		if (s == 0 || fl == 0 || walk == 0 || entry == 0 || cb == 0 || d3m == 0 || fog == 0)
		{
			why = "the effects' flush, walk or element draw wasn't found";
			return false;
		}
		const uintptr_t setup_call = s + kSetupCallAt, flush_call = fl + kFlushCallAt, entry_site = entry + kEntryAt, element_draw = s - kElementDrawFromSetup;
		uint8_t now_setup[5] = {}, now_flush[5] = {}, now_entry[7] = {};
		if (!read(setup_call, now_setup, 5) || now_setup[0] != 0xE8 || !read(flush_call, now_flush, 5) || now_flush[0] != 0xE8 || rel_target(flush_call) != walk ||
			!read(entry_site, now_entry, 7) || std::memcmp(now_entry, kEntryBytes, 7) != 0 || !code_is(element_draw, "\x83\xEC\x50\x8D\x44\x24\x0C\x55\x8B\xE9\x51", 11))
		{
			why = "the effects' flush or element draw isn't laid out as expected (another patch there?)";
			return false;
		}
		// The globals and functions the recording reads and calls (each checked by its opcode first)
		uint8_t op[2] = {};
		const auto opcode = [&](uintptr_t a, uint8_t b0, int b1) { return read(a, op, 2) && op[0] == b0 && (b1 < 0 || op[1] == b1); };
		if (!opcode(s + kSetupGfx, 0x8B, 0x0D) || !opcode(s + kSetupCamera, 0xA1, -1) || !opcode(s + kSetupRenderer, 0x8B, 0x0D) || !opcode(s + kSetupMatrixCall, 0xE8, -1) ||
			!opcode(d3m + kD3mColour, 0x68, -1) || !opcode(d3m + kD3mWeatherCall, 0xE8, -1) || !opcode(fog + kFogCall, 0xE8, -1))
		{
			why = "the effects' globals aren't where the element draw reads them";
			return false;
		}
		state n;
		n.setup_call = setup_call;
		n.setup = rel_target(setup_call);
		n.flush_call = flush_call;
		n.walk = walk;
		n.entry_site = entry_site;
		n.element_cb = cb;
		n.element_draw = element_draw;
		n.d3m_draw = d3m;
		n.d3a_draw = find(kD3aDraw);
		n.trail_cb = find(kTrailCallback);
		n.ring_draw = find(kRingDraw);
		n.draw_matrix = rel_target(s + kSetupMatrixCall);
		n.weather = rel_target(d3m + kD3mWeatherCall);
		n.fog_env = rel_target(fog + kFogCall);
		n.math = word(s + kSetupMath);
		n.gfx = word(s + kSetupGfx + 2);
		n.camera = word(s + kSetupCamera + 1);
		n.renderer = word(s + kSetupRenderer + 2);
		n.colour = word(d3m + kD3mColour + 1);
		n.env = word(fog + kFogEnv);
		if (g.code == 0)
			g.code = alloc_code(kCodeSize);
		if (g.code == 0)
		{
			why = "no memory for the stubs";
			return false;
		}
		n.code = g.code;
		uint8_t c[kCodeSize] = {
			0x51, 0x6A, 0x00, 0, 0, 0, 0, 0, 0x83, 0xC4, 0x04, 0x59, // push ecx; push 0; call on_flush; add esp, 4; pop ecx
			0, 0, 0, 0, 0,                                           // call the walk
			0x6A, 0x01, 0, 0, 0, 0, 0, 0x83, 0xC4, 0x04,             // push 1; call on_flush; add esp, 4
			0xC3 };                                                  // ret
		const uint8_t stub[] = {
			0x50,             // push eax (the argument)
			0xFF, 0x76, 0x08, // push [esi+8] (the callback)
			0, 0, 0, 0, 0,    // call on_entry
			0x83, 0xC4, 0x08, // add esp, 8
			0xC3 };           // ret
		std::memcpy(c + kEntryStubAt, stub, sizeof(stub));
		put_call(c + kFlushMark1, g.code + kFlushMark1, reinterpret_cast<uintptr_t>(&on_flush));
		put_call(c + kFlushWalk, g.code + kFlushWalk, walk);
		put_call(c + kFlushMark2, g.code + kFlushMark2, reinterpret_cast<uintptr_t>(&on_flush));
		put_call(c + kEntryCall, g.code + kEntryCall, reinterpret_cast<uintptr_t>(&on_entry));
		if (!write_code(g.code, c, kCodeSize))
		{
			why = "couldn't write the stubs";
			return false;
		}
		put_call(n.ours_setup, setup_call, reinterpret_cast<uintptr_t>(&on_setup));
		put_call(n.ours_flush, flush_call, g.code);
		put_call(n.ours_entry, entry_site, g.code + kEntryStubAt);
		n.ours_entry[5] = n.ours_entry[6] = 0x90;
		std::memcpy(n.saved_setup, now_setup, 5);
		std::memcpy(n.saved_flush, now_flush, 5);
		// (the walk's stub first, then the flush's, then the setup: each works without the ones after it)
		if (!write_code(entry_site, n.ours_entry, 7))
		{
			why = "couldn't write the game's code";
			return false;
		}
		if (!write_code(flush_call, n.ours_flush, 5))
		{
			write_code(entry_site, kEntryBytes, 7);
			why = "couldn't write the game's code";
			return false;
		}
		if (!write_code(setup_call, n.ours_setup, 5))
		{
			write_code(flush_call, now_flush, 5);
			write_code(entry_site, kEntryBytes, 7);
			why = "couldn't write the game's code";
			return false;
		}
		n.native = g.native;
		n.wanted = g.wanted;
		n.probe = g.probe;
		n.clock = g.clock;
		n.device_override = g.device_override;
		n.sink = g.sink;
		n.in = true;
		const counts kept = g.n;
		g = n;
		g.n = kept;
		return true;
	}

	bool stop(std::string &why)
	{
		if (!g.in)
			return true;
		if (!code_is(g.setup_call, g.ours_setup, 5) || !code_is(g.flush_call, g.ours_flush, 5) || !code_is(g.entry_site, g.ours_entry, 7))
		{
			why = "something else has changed the effects' flush or element draw since: left as they are";
			return false;
		}
		if (!write_code(g.setup_call, g.saved_setup, 5) || !write_code(g.flush_call, g.saved_flush, 5) || !write_code(g.entry_site, kEntryBytes, 7))
		{
			why = "couldn't write the game's code";
			return false;
		}
		// (on the game's thread, between frames: no flush running)
		free_code(g.code);
		state n;
		n.device_override = g.device_override;
		n.sink = g.sink;
		n.native = g.native;
		n.wanted = g.wanted;
		n.n = g.n;
		g = n;
		fx::clear_frames();
		return true;
	}

	bool in()
	{
		return g.in;
	}

	void set_native(bool on)
	{
		g.native = on;
	}

	void set_wanted(bool wanted)
	{
		g.wanted = wanted;
	}

	void set_probe(bool on)
	{
		g.probe = on;
	}

	void set_clock(double now)
	{
		g.clock = now;
	}

	counts get_counts()
	{
		return g.n;
	}

	std::string status()
	{
		if (!g.in)
			return "effects: drawn by the game";
		uint64_t taken = 0;
		for (const uint64_t t : g.n.taken)
			taken += t;
		char line[600];
		std::snprintf(line, sizeof(line),
			"effects: %s; %llu flushes, %llu elements drawn by HiFi XI (D3M %llu, D3A %llu, rings %llu, weapon trails %llu), %llu with nothing to draw; the last flush %u elements in %u runs; "
			"left to the game: %llu (not taking), %llu (class not handled), %llu (lit), %llu (blend letter), %llu (texture not up yet), %llu (no room), %llu (unreadable), "
			"%llu (untextured D3M, its first vertex overwritten); "
			"%llu markers sent%s",
			g.native ? "HiFi XI draws what it can" : g.probe ? "the game draws them (waiting for the markers to come through)" : "the game draws them",
			static_cast<unsigned long long>(g.n.flushes), static_cast<unsigned long long>(taken), static_cast<unsigned long long>(g.n.taken[fx::class_d3m]),
			static_cast<unsigned long long>(g.n.taken[fx::class_d3a]), static_cast<unsigned long long>(g.n.taken[fx::class_ring]), static_cast<unsigned long long>(g.n.taken[fx::class_trail]),
			static_cast<unsigned long long>(g.n.empty), g.n.last_elements, g.n.last_runs, static_cast<unsigned long long>(g.n.game),
			static_cast<unsigned long long>(g.n.unsupported), static_cast<unsigned long long>(g.n.lit), static_cast<unsigned long long>(g.n.letter),
			static_cast<unsigned long long>(g.n.texture), static_cast<unsigned long long>(g.n.full), static_cast<unsigned long long>(g.n.faulted),
			static_cast<unsigned long long>(g.n.overwritten),
			static_cast<unsigned long long>(g.n.markers), g.n.not_sent != 0 ? (", not sent " + std::to_string(g.n.not_sent)).c_str() : "");
		return line;
	}

	sites where()
	{
		sites s;
		s.setup_call = g.setup_call;
		s.setup = g.setup;
		s.flush_call = g.flush_call;
		s.walk = g.walk;
		s.entry_site = g.entry_site;
		s.element_cb = g.element_cb;
		s.element_draw = g.element_draw;
		s.d3m_draw = g.d3m_draw;
		s.draw_matrix = g.draw_matrix;
		s.fog_env = g.fog_env;
		s.d3a_draw = g.d3a_draw;
		s.trail_cb = g.trail_cb;
		s.ring_draw = g.ring_draw;
		s.colour = g.colour;
		s.gfx = g.gfx;
		s.renderer = g.renderer;
		s.camera = g.camera;
		s.env = g.env;
		s.math = g.math;
		s.code = g.in ? g.code : 0;
		return s;
	}

	void set_sink(void (*sink)(uint32_t, uint32_t))
	{
		g.sink = sink;
	}

	void set_device(uintptr_t device)
	{
		g.device_override = device;
	}

	void set_setup(uintptr_t setup)
	{
		g.setup = setup;
	}
}
