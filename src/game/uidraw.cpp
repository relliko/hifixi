/*
 * The game's screen 2D recorded at its device (uidraw.hpp; docs/own-frame/2d.md).
 *
 * The three calls (both dumps):
 *   01275E: mov edx, [wrapper]; push ebx; push 0Eh; ...; call [ecx+0C8h] (ZWRITEENABLE 0 after the resolve);
 *   012773: mov ecx, [wrapper]; call 00BB20 (an empty function: ret)      -> the window opens after it
 *   0129E2: mov eax, [esi+31Ch]; push eax; push ebx; call 00A820 (the fade after the menus)  -> kind 10 before it
 *   012AEF: mov ecx, [wrapper]; call 009740 (EndScene); mov ecx, [wrapper]; call 009720 (Present) -> kind 11 before it
 * and the screen passes' window on A (before the resolve):
 *   012452: mov ecx, [wrapper]; push eax; mov edx, [ecx+1C0h]; push edx; call 00AAB0; call 012F40 (the frozen-frame
 *           crossfade); mov ecx, esi; call 0132B0 (the wash and afterimage)    -> the window on A opens before 012F40
 *   012699: mov ecx, [wrapper]; call 00A1D0 (A popped: the back buffer again)  -> kind 8 and closed before it
 * Each call's rel32 is pointed at a stub of ours (ecx is the wrapper at all but 012F40's: the stub reads it from the
 * global 012452 loads):
 *   begin:  push ecx; call 00BB20; pop ecx; push eax/ecx/edx; push ecx; call on_begin; add esp, 4; pop; ret
 *   end:    push eax/ecx/edx; push ecx; push 10; call on_flush; add esp, 8; pop; jmp 00A820
 *   frame:  the same with 11, jmp 009740
 *   screen: push eax/ecx/edx; push [wrapper]; call on_screen; add esp, 4; pop; jmp 012F40
 *   pop:    push eax/ecx/edx; push ecx; push 108h (8, and close); call on_flush; add esp, 8; pop; jmp 00A1D0
 *
 * The opening movie (21D4C0, drawn in the window from a texture its decoder writes each frame) is left to the game: no
 * window opens while the movie player ([0x456D1C], 01298E) exists.
 *
 * The device's vtable copy: slot -1 and the 97 methods of IDirect3DDevice8 as the device has them when the window
 * opens (Ashita's, or hifps's draw tap in front of them), ours in place of the slots below. The device's own vtable
 * pointer goes back when the window closes, unless something else replaced ours meanwhile.
 */

#include "uidraw.hpp"
#include "slotmark.hpp"
#include "../ui2d/frame2d.hpp"
#include "../ui2d/render2d.hpp"
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace hifi::game::uidraw
{
	namespace
	{
		namespace u2 = hifi::ui2d;

		constexpr const char *kBegin = "8B15????????536A0E8B420C508B08FF91C80000008B0D????????E8????????A1????????3BC3"; // 01275E
		constexpr intptr_t kBeginAt = 0x1B;
		constexpr const char *kEnd = "8B861C0300005053E8????????A1????????33C9668B4E12"; // 0129E2
		constexpr intptr_t kEndAt = 8;
		constexpr const char *kFrame = "8B0D????????E8????????8B0D????????E8????????8B2D????????895E28"; // 012AEF
		constexpr intptr_t kFrameAt = 6;
		constexpr uint8_t kEmpty[] = { 0xC3 };                                                         // 00BB20
		constexpr uint8_t kFade[] = { 0x83, 0xEC, 0x08, 0x56, 0x57, 0x8B, 0xF1, 0xE8 };                // 00A820
		constexpr uint8_t kEndScene[] = { 0x8B, 0x41, 0x0C, 0x50, 0x8B, 0x08, 0xFF, 0x91, 0x8C, 0x00, 0x00, 0x00, 0xC3 }; // 009740
		constexpr const char *kScreen = "8B0D????????508B91C001000052E8????????E8????????8BCEE8????????A0????????3C80"; // 012452
		constexpr intptr_t kScreenWrapperAt = 2, kScreenAt = 0x13;
		constexpr const char *kPop = "E8????????33D233C0668B5610668B4612"; // 0126A1
		constexpr uint8_t kCrossfade[] = { 0xA1, 0, 0, 0, 0, 0x85, 0xC0, 0x0F, 0x84 };                 // 012F40 (mov eax, [456910]; test; je)
		constexpr uint8_t kPopA[] = { 0x56, 0x8B, 0xF1, 0x6A, 0x00, 0x6A, 0x00, 0x8B, 0x46, 0x0C, 0x50 }; // 00A1D0
		constexpr const char *kMovie = "8B0D????????3BCB7405E8????????8B0D????????E8????????8B0D????????3999E0010000"; // 01298E
		constexpr intptr_t kMovieAt = 2;
		constexpr uint32_t kClose = 0x100; // (on_flush: close the window after)

		constexpr int kCalls = 5;
		constexpr size_t kCodeSize = 0xA0, kEndStub = 0x20, kFrameStub = 0x40, kScreenStub = 0x60, kPopStub = 0x80;

		// IDirect3DDevice8's methods
		enum : uint32_t
		{
			kRelease = 2, kSetRenderTarget = 31, kGetRenderTarget = 32, kGetDepthStencilSurface = 33, kClear = 36, kSetViewport = 40, kGetViewport = 41,
			kSetRenderState = 50, kGetRenderState = 51, kApplyStateBlock = 54, kGetTexture = 60, kSetTexture = 61, kGetTextureStageState = 62,
			kSetTextureStageState = 63, kDP = 70, kDIP = 71, kDPUP = 72, kDIPUP = 73, kSetVertexShader = 76, kGetVertexShader = 77, kSetPixelShader = 88,
			kGetPixelShader = 89, kMethods = 97,
		};
		// IDirect3DTexture8's, IDirect3DSurface8's
		enum : uint32_t { kTexGetType = 10, kTexLevelCount = 13, kTexLevelDesc = 14, kTexLock = 16, kTexUnlock = 17, kSurfDesc = 8 };
		// Render states and stage states followed (D3DRENDERSTATETYPE, D3DTEXTURESTAGESTATETYPE)
		enum : uint32_t
		{
			rsZENABLE = 7, rsFILLMODE = 8, rsSHADEMODE = 9, rsALPHATESTENABLE = 15, rsSRCBLEND = 19, rsDESTBLEND = 20, rsCULLMODE = 22, rsALPHAREF = 24,
			rsALPHAFUNC = 25, rsALPHABLENDENABLE = 27, rsFOGENABLE = 28, rsSTENCILENABLE = 52, rsTEXTUREFACTOR = 60, rsCOLORWRITEENABLE = 168, rsBLENDOP = 171,
		};
		enum : uint32_t
		{
			tsCOLOROP = 1, tsCOLORARG1 = 2, tsCOLORARG2 = 3, tsALPHAOP = 4, tsALPHAARG1 = 5, tsALPHAARG2 = 6, tsTEXCOORDINDEX = 11, tsADDRESSU = 13,
			tsADDRESSV = 14, tsMAGFILTER = 16, tsMINFILTER = 17, tsMIPFILTER = 18, tsTEXTURETRANSFORMFLAGS = 24,
		};
		constexpr uint32_t kRenderStates[] = { rsZENABLE, rsFILLMODE, rsSHADEMODE, rsALPHATESTENABLE, rsSRCBLEND, rsDESTBLEND, rsCULLMODE, rsALPHAREF,
			rsALPHAFUNC, rsALPHABLENDENABLE, rsFOGENABLE, rsSTENCILENABLE, rsTEXTUREFACTOR, rsCOLORWRITEENABLE, rsBLENDOP };
		constexpr uint32_t kStageStates[] = { tsCOLOROP, tsCOLORARG1, tsCOLORARG2, tsALPHAOP, tsALPHAARG1, tsALPHAARG2, tsTEXCOORDINDEX, tsADDRESSU, tsADDRESSV,
			tsMAGFILTER, tsMINFILTER, tsMIPFILTER, tsTEXTURETRANSFORMFLAGS };

		// Why a draw went to the game (counts::why)
		enum reason : uint32_t
		{
			rOk, rPrimitive, rVertexFormat, rPixelShader, rDepth, rFog, rStencil, rFill, rStageOne, rCoordinates, rCombiner, rSampler, rTexture,
			rFull, rPipeline, rOtherDraw, rReasons
		};
		const char *const kReasonNames[rReasons] = { "recorded", "not triangles", "vertex format", "pixel shader", "Z on", "fog on", "stencil on",
			"not solid", "stage 1 on", "texture coordinates", "combiner op", "sampler", "texture", "frame full", "no pipeline", "another draw call" };

		// A texture held: its slot in the store, when it was last used (window number) and checked
		struct held
		{
			uint32_t slot = u2::kNoTexture;
			uint32_t used = 0;
			uint32_t checked = 0;
			uint64_t hash = 0;
			bool little = true;
		};

		struct device_state
		{
			uint32_t rs[256] = {};
			uint32_t tss[2][32] = {};
			uintptr_t tex[2] = {};
			uint32_t vp[4] = {}; // (X, Y, Width, Height)
			uint32_t vs = 0, ps = 0;
			uint32_t target_w = 0, target_h = 0;
			bool markable = false; // the bound depth has a stencil
		};

		struct state
		{
			uintptr_t calls[kCalls] = {}, targets[kCalls] = {}, code = 0;
			uint8_t saved[kCalls][5] = {}, ours[kCalls][5] = {};
			uintptr_t movie = 0;   // (the movie player's global)
			bool in = false;
			mode wanted = off;
			uintptr_t device_override = 0;
			void (*sink)(uint32_t, uint32_t) = nullptr;
			// The window
			bool open = false, native = false;
			uint32_t cut_kind = 10; // (the window's cuts: 10 on the back buffer, 8 on A)
			uintptr_t dev = 0;
			uint32_t source_vt = 0;
			uint32_t seq = 0;
			u2::frame *f = nullptr;
			uint32_t seg_first = 0;
			device_state st;
			std::unordered_map<uintptr_t, held> textures;
			counts n;
			std::string last_why;
		};
		state g;

		// The vtable copy (slot -1 first) and the methods it stood in for
		uint32_t g_copy[1 + kMethods] = {};
		uint32_t g_orig[kMethods] = {};

		template <typename F>
		F method(uintptr_t object, uint32_t index)
		{
			return reinterpret_cast<F>(uintptr_t(*reinterpret_cast<const uint32_t *>(*reinterpret_cast<const uint32_t *>(object) + 4 * index)));
		}
		template <typename F>
		F orig(uint32_t index)
		{
			return reinterpret_cast<F>(uintptr_t(g_orig[index]));
		}

		using release_fn = unsigned long(__stdcall *)(uintptr_t);
		using get1_fn = long(__stdcall *)(uintptr_t, uint32_t, uint32_t *);
		using get2_fn = long(__stdcall *)(uintptr_t, uint32_t, uint32_t, uint32_t *);
		using getp_fn = long(__stdcall *)(uintptr_t, uintptr_t *);
		using getp1_fn = long(__stdcall *)(uintptr_t, uint32_t, uintptr_t *);
		using getv_fn = long(__stdcall *)(uintptr_t, uint32_t *);
		using desc_fn = long(__stdcall *)(uintptr_t, uint32_t desc[8]);
		using level_desc_fn = long(__stdcall *)(uintptr_t, uint32_t level, uint32_t desc[8]);
		using count_fn = uint32_t(__stdcall *)(uintptr_t);
		using lock_fn = long(__stdcall *)(uintptr_t, uint32_t level, int32_t locked[2], const void *rect, uint32_t flags);
		using unlock_fn = long(__stdcall *)(uintptr_t, uint32_t level);
		using clear_fn = long(__stdcall *)(uintptr_t, uint32_t count, const int32_t *rects, uint32_t flags, uint32_t colour, float z, uint32_t stencil);

		void release(uintptr_t object)
		{
			if (object != 0)
				method<release_fn>(object, kRelease)(object);
		}

		// ---------------------------------------------------------------- the device's state

		bool query_target(uintptr_t dev)
		{
			uintptr_t rt = 0, ds = 0;
			uint32_t d[8] = {};
			if (orig<getp_fn>(kGetRenderTarget)(dev, &rt) < 0 || rt == 0)
				return false;
			const bool got = method<desc_fn>(rt, kSurfDesc)(rt, d) >= 0;
			release(rt);
			if (!got)
				return false;
			g.st.target_w = d[6];
			g.st.target_h = d[7];
			g.st.markable = false;
			if (orig<getp_fn>(kGetDepthStencilSurface)(dev, &ds) >= 0 && ds != 0)
			{
				uint32_t dd[8] = {};
				if (method<desc_fn>(ds, kSurfDesc)(ds, dd) >= 0)
					g.st.markable = dd[0] == 73 || dd[0] == 75 || dd[0] == 79; // D15S1, D24S8, D24X4S4
				release(ds);
			}
			return true;
		}

		bool query_state(uintptr_t dev)
		{
			for (const uint32_t s : kRenderStates)
				if (orig<get1_fn>(kGetRenderState)(dev, s, &g.st.rs[s]) < 0)
					return false;
			for (const uint32_t t : kStageStates)
				if (orig<get2_fn>(kGetTextureStageState)(dev, 0, t, &g.st.tss[0][t]) < 0)
					return false;
			if (orig<get2_fn>(kGetTextureStageState)(dev, 1, tsCOLOROP, &g.st.tss[1][tsCOLOROP]) < 0)
				return false;
			for (uint32_t stage = 0; stage < 2; ++stage)
			{
				uintptr_t t = 0;
				if (orig<getp1_fn>(kGetTexture)(dev, stage, &t) < 0)
					return false;
				g.st.tex[stage] = t;
				release(t); // (GetTexture adds a reference)
			}
			uint32_t vp[6] = {};
			if (orig<getv_fn>(kGetViewport)(dev, vp) < 0)
				return false;
			std::memcpy(g.st.vp, vp, 16);
			if (orig<getv_fn>(kGetVertexShader)(dev, &g.st.vs) < 0 || orig<getv_fn>(kGetPixelShader)(dev, &g.st.ps) < 0)
				return false;
			return query_target(dev);
		}

		// ---------------------------------------------------------------- textures

		uint64_t fnv(const uint8_t *p, size_t n, uint64_t h = 1469598103934665603ull)
		{
			for (size_t i = 0; i < n; ++i)
				h = (h ^ p[i]) * 1099511628211ull;
			return h;
		}

		// A texture's levels from the game's device (all, or level 0 only), or false: not one it can take
		bool capture(uintptr_t tex, bool all_levels, u2::texfmt::texture &out, uint32_t &level0_bytes)
		{
			if (method<count_fn>(tex, kTexGetType)(tex) != 3) // (D3DRTYPE_TEXTURE)
				return false;
			const uint32_t levels = all_levels ? method<count_fn>(tex, kTexLevelCount)(tex) : 1;
			if (levels == 0 || levels > 16)
				return false;
			out.levels.clear();
			for (uint32_t l = 0; l < levels; ++l)
			{
				uint32_t d[8] = {};
				if (method<level_desc_fn>(tex, kTexLevelDesc)(tex, l, d) < 0)
					return false;
				// (Format, Type, Usage, Pool, Size, MultiSampleType, Width, Height: a managed texture without
				// RENDERTARGET, DEPTHSTENCIL or DYNAMIC)
				if (d[3] != 1 || (d[2] & (0x1 | 0x2 | 0x200)) != 0 || !u2::texfmt::supported(d[0]))
					return false;
				out.format = d[0];
				const uint32_t w = d[6], h = d[7];
				int32_t locked[2] = {};
				if (method<lock_fn>(tex, kTexLock)(tex, l, locked, nullptr, 0x10) < 0) // (D3DLOCK_READONLY)
					return false;
				u2::texfmt::level lv;
				lv.w = w;
				lv.h = h;
				lv.pitch = u2::texfmt::row_bytes(out.format, w);
				const uint32_t rows = u2::texfmt::rows(out.format, h);
				bool ok = locked[1] != 0 && uint32_t(locked[0] < 0 ? -locked[0] : locked[0]) >= lv.pitch;
				if (ok)
				{
					lv.bytes.resize(size_t(lv.pitch) * rows);
					const uint8_t *src = reinterpret_cast<const uint8_t *>(uintptr_t(uint32_t(locked[1])));
					for (uint32_t r = 0; r < rows; ++r)
						std::memcpy(lv.bytes.data() + size_t(r) * lv.pitch, src + ptrdiff_t(r) * locked[0], lv.pitch);
				}
				method<unlock_fn>(tex, kTexUnlock)(tex, l);
				if (!ok)
					return false;
				if (l == 0)
					level0_bytes = uint32_t(lv.bytes.size());
				out.levels.push_back(std::move(lv));
			}
			return true;
		}

		// What a held texture's level 0 hashes to: all of it when it is small, else 16 rows spread over it
		uint64_t level0_hash(const u2::texfmt::texture &t, bool little)
		{
			const u2::texfmt::level &l = t.levels[0];
			if (little)
				return fnv(l.bytes.data(), l.bytes.size());
			uint64_t h = 1469598103934665603ull;
			const uint32_t rows = uint32_t(l.bytes.size() / l.pitch);
			for (uint32_t k = 0; k < 16; ++k)
			{
				const uint32_t r = rows * k / 16;
				h = fnv(l.bytes.data() + size_t(r) * l.pitch, l.pitch, h);
			}
			return h;
		}

		constexpr uint32_t kSmall = 16384;     // (level 0 bytes: checked each window it's used)
		constexpr uint32_t kLargeCheck = 180;  // (windows between checks of a large one)
		constexpr uint32_t kUnused = 300;      // (windows unused before it's let go)

		void let_go(uintptr_t tex, held &h)
		{
			u2::render::drop_texture(h.slot);
			release(tex);
		}

		// The store's slot for the texture a draw uses (taken now when it's new, or when its pixels changed)
		uint32_t texture_slot(uintptr_t tex, bool &ok)
		{
			ok = true;
			const uint32_t window = g.seq;
			const auto at = g.textures.find(tex);
			if (at != g.textures.end())
			{
				held &h = at->second;
				const bool check = h.used != window && (h.little || window - h.checked >= kLargeCheck);
				h.used = window;
				if (!check)
					return h.slot;
				h.checked = window;
				u2::texfmt::texture t;
				uint32_t bytes = 0;
				if (capture(tex, false, t, bytes) && level0_hash(t, h.little) == h.hash)
					return h.slot;
				// (new pixels, or it can't be read now: taken again from scratch)
				let_go(tex, h);
				g.textures.erase(at);
				g.n.texture_changes++;
			}
			u2::texfmt::texture t;
			uint32_t bytes = 0;
			if (!capture(tex, true, t, bytes))
			{
				ok = false;
				return u2::kNoTexture;
			}
			std::string why;
			const uint32_t slot = u2::render::add_texture(t, why);
			if (slot == u2::kNoTexture)
			{
				g.last_why = "texture: " + why;
				ok = false;
				return slot;
			}
			held h;
			h.slot = slot;
			h.used = h.checked = window;
			h.little = bytes <= kSmall;
			h.hash = level0_hash(t, h.little);
			method<release_fn>(tex, 1)(tex); // (AddRef: held while it's in the store)
			g.textures[tex] = h;
			g.n.textures++;
			return slot;
		}

		void forget_unused()
		{
			for (auto it = g.textures.begin(); it != g.textures.end();)
				if (g.seq - it->second.used > kUnused)
				{
					let_go(it->first, it->second);
					it = g.textures.erase(it);
				}
				else
					++it;
		}

		// ---------------------------------------------------------------- recording

		bool sends_marks()
		{
			return g.sink != nullptr || g.st.markable;
		}

		bool mark(uint32_t kind)
		{
			if (g.sink != nullptr)
			{
				g.sink(kind, g.seq & 15u);
				return true;
			}
			const int32_t x = int32_t(slotmark::x_of(kind, g.seq));
			const int32_t rect[4] = { x, 0, x + 1, 1 };
			return orig<clear_fn>(kClear)(g.dev, 1, rect, 4, 0, 1.0f, 0) >= 0; // (D3DCLEAR_STENCIL)
		}

		// What's recorded since the last segment published as one, and its marker sent
		void flush(uint32_t kind)
		{
			if (!g.open || g.f == nullptr)
				return;
			const uint32_t n = g.f->draw_count - g.seg_first;
			if (n == 0)
				return;
			if (u2::render::pending())
			{
				std::string why;
				if (!u2::render::upload(why))
				{
					g.n.upload_failures++;
					g.last_why = "upload: " + why;
				}
			}
			u2::segment s;
			s.first = g.seg_first;
			s.count = n;
			s.kind = kind;
			s.native = g.native;
			g.seg_first = g.f->draw_count;
			if (!u2::publish(*g.f, s))
				return;
			g.n.segments++;
			g.n.frame_segments++;
			if (mark(kind))
				g.n.markers++;
			else
				g.n.not_marked++;
		}

		void cut()
		{
			if (g.open && g.f != nullptr && g.f->draw_count != g.seg_first)
			{
				g.n.cuts++;
				flush(g.cut_kind);
			}
		}

		bool uses_texture(uint32_t op, uint32_t a1, uint32_t a2)
		{
			if (op == 1) // (DISABLE)
				return false;
			if (op == 13 || op == 15) // (BLENDTEXTUREALPHA, BLENDTEXTUREALPHAPM)
				return true;
			const bool one = (a1 & 15) == 2, two = (a2 & 15) == 2;
			return op == 2 ? one : op == 3 ? two : one || two;
		}

		uint32_t vertex_bytes(uint32_t fvf)
		{
			return fvf == 0x144 ? 28 : fvf == 0x44 ? 20 : 0;
		}

		// The state recordable as it is (the reason not, else rOk)
		reason check_state(uint32_t type)
		{
			const device_state &s = g.st;
			if (type < 4 || type > 6)
				return rPrimitive;
			if (vertex_bytes(s.vs) == 0)
				return rVertexFormat;
			if (s.ps != 0)
				return rPixelShader;
			if (s.rs[rsZENABLE] != 0)
				return rDepth;
			if (s.rs[rsFOGENABLE] != 0)
				return rFog;
			if (s.rs[rsSTENCILENABLE] != 0)
				return rStencil;
			if (s.rs[rsFILLMODE] != 3)
				return rFill;
			if (s.tss[1][tsCOLOROP] != 1 && s.tss[0][tsCOLOROP] != 1)
				return rStageOne;
			if ((s.tss[0][tsTEXCOORDINDEX] & 0xFFFF) != 0 || (s.tss[0][tsTEXCOORDINDEX] >> 16) != 0 || s.tss[0][tsTEXTURETRANSFORMFLAGS] != 0)
				return rCoordinates;
			const uint32_t co = s.tss[0][tsCOLOROP], ao = s.tss[0][tsALPHAOP];
			if (co < 1 || co > 16 || ao < 1 || ao > 16)
				return rCombiner;
			for (const uint32_t a : { s.tss[0][tsCOLORARG1], s.tss[0][tsCOLORARG2], s.tss[0][tsALPHAARG1], s.tss[0][tsALPHAARG2] })
				if ((a & 15) > 4 || (a & ~0x3Fu) != 0)
					return rCombiner;
			if (s.tss[0][tsMINFILTER] < 1 || s.tss[0][tsMINFILTER] > 2 || s.tss[0][tsMAGFILTER] < 1 || s.tss[0][tsMAGFILTER] > 2 || s.tss[0][tsMIPFILTER] > 2 ||
				s.tss[0][tsADDRESSU] < 1 || s.tss[0][tsADDRESSU] > 3 || s.tss[0][tsADDRESSV] < 1 || s.tss[0][tsADDRESSV] > 3)
				return rSampler;
			if (s.rs[rsALPHABLENDENABLE] != 0 && (s.rs[rsSRCBLEND] < 1 || s.rs[rsSRCBLEND] > 13 || s.rs[rsDESTBLEND] < 1 || s.rs[rsDESTBLEND] > 13 ||
				s.rs[rsBLENDOP] < 1 || s.rs[rsBLENDOP] > 5))
				return rPipeline;
			return rOk;
		}

		// A DrawPrimitiveUP recorded into the window's frame: rOk, or why it's the game's
		reason record(uint32_t type, uint32_t count, const void *data, uint32_t stride)
		{
			if (!g.open || g.f == nullptr || !sends_marks())
				return rFull;
			const reason r = check_state(type);
			if (r != rOk)
				return r;
			const device_state &s = g.st;
			const uint32_t vb = vertex_bytes(s.vs);
			if (stride < vb || data == nullptr || count == 0)
				return rVertexFormat;
			u2::frame &f = *g.f;
			const uint32_t corners = 3 * count;
			if (f.draw_count >= u2::kMaxDraws || f.vertex_count + corners > u2::kMaxVertices || count > u2::kMaxVertices ||
				f.segments.load(std::memory_order_relaxed) + 2 >= u2::kMaxSegments)
				return rFull;
			u2::draw d;
			d.blend.enable = s.rs[rsALPHABLENDENABLE] != 0 ? 1 : 0;
			d.blend.src = uint8_t(s.rs[rsSRCBLEND]);
			d.blend.dst = uint8_t(s.rs[rsDESTBLEND]);
			d.blend.op = uint8_t(s.rs[rsBLENDOP]);
			d.blend.write_mask = uint8_t(s.rs[rsCOLORWRITEENABLE] & 15);
			d.blend.cull = uint8_t(s.rs[rsCULLMODE] >= 1 && s.rs[rsCULLMODE] <= 3 ? s.rs[rsCULLMODE] : 1);
			if (!d.blend.enable)
				d.blend.src = 2, d.blend.dst = 1, d.blend.op = 1; // (one pipeline for every blend that's off)
			d.colour.op = uint8_t(s.tss[0][tsCOLOROP]);
			d.colour.arg1 = uint8_t(s.tss[0][tsCOLORARG1]);
			d.colour.arg2 = uint8_t(s.tss[0][tsCOLORARG2]);
			d.alpha.op = uint8_t(s.tss[0][tsALPHAOP]);
			d.alpha.arg1 = uint8_t(s.tss[0][tsALPHAARG1]);
			d.alpha.arg2 = uint8_t(s.tss[0][tsALPHAARG2]);
			d.alpha_test = s.rs[rsALPHATESTENABLE] != 0 ? 1 : 0;
			d.alpha_func = uint8_t(s.rs[rsALPHAFUNC]);
			d.alpha_ref = uint8_t(s.rs[rsALPHAREF]);
			d.flat = s.rs[rsSHADEMODE] == 1 ? 1 : 0;
			d.min_filter = uint8_t(s.tss[0][tsMINFILTER]);
			d.mag_filter = uint8_t(s.tss[0][tsMAGFILTER]);
			d.mip_filter = uint8_t(s.tss[0][tsMIPFILTER]);
			d.address_u = uint8_t(s.tss[0][tsADDRESSU]);
			d.address_v = uint8_t(s.tss[0][tsADDRESSV]);
			d.tfactor = s.rs[rsTEXTUREFACTOR];
			d.clip[0] = int32_t(s.vp[0]);
			d.clip[1] = int32_t(s.vp[1]);
			d.clip[2] = int32_t(s.vp[0] + s.vp[2]);
			d.clip[3] = int32_t(s.vp[1] + s.vp[3]);
			d.target_w = s.target_w;
			d.target_h = s.target_h;
			const bool textured = uses_texture(d.colour.op, d.colour.arg1, d.colour.arg2) ||
				(d.colour.op != 1 && uses_texture(d.alpha.op, d.alpha.arg1, d.alpha.arg2));
			if (textured && s.tex[0] != 0)
			{
				bool ok = false;
				d.texture = texture_slot(s.tex[0], ok);
				if (!ok)
					return rTexture;
			}
			else if (textured)
			{
				// (no texture set: dgVoodoo draws the stage as disabled, the diffuse colour and alpha as they are;
				// tests/ui2d_dgv.cpp "coverage", SELECTARG1, ADD and MODULATE2X of TEXTURE without one)
				d.colour.op = 1;
				d.alpha.op = 1;
			}
			// (the pipeline now, on this thread: dgVoodoo's never has to make one while it records)
			if (g.native && !u2::render::pipeline(d.blend))
				return rPipeline;
			// The vertices, a triangle list with each triangle's first vertex first (the flat-shaded colour)
			d.first = f.vertex_count;
			d.count = corners;
			const uint8_t *const p = static_cast<const uint8_t *>(data);
			const auto put = [&](uint32_t i) {
				u2::vertex &v = f.vertices[f.vertex_count++];
				const uint8_t *src = p + size_t(i) * stride;
				std::memcpy(&v, src, 20); // (x, y, z, rhw, colour)
				if (vb == 28)
					std::memcpy(&v.u, src + 20, 8);
				else
					v.u = v.v = 0.0f;
			};
			for (uint32_t t = 0; t < count; ++t)
				if (type == 4)
					put(3 * t), put(3 * t + 1), put(3 * t + 2);
				else if (type == 5)
				{
					if ((t & 1) == 0)
						put(t), put(t + 1), put(t + 2);
					else
						put(t), put(t + 2), put(t + 1);
				}
				else
					put(t + 1), put(t + 2), put(0);
			f.draws[f.draw_count++] = d;
			g.n.recorded++;
			g.n.frame_draws++;
			return rOk;
		}

		void passed(reason r)
		{
			g.n.passed++;
			g.n.why[r < rReasons ? r : 0]++;
		}

		// ---------------------------------------------------------------- the methods in the window

		long __stdcall h_set_render_state(uintptr_t dev, uint32_t s, uint32_t v)
		{
			if (s < 256)
				g.st.rs[s] = v;
			return orig<long(__stdcall *)(uintptr_t, uint32_t, uint32_t)>(kSetRenderState)(dev, s, v);
		}
		long __stdcall h_set_stage_state(uintptr_t dev, uint32_t stage, uint32_t t, uint32_t v)
		{
			if (stage < 2 && t < 32)
				g.st.tss[stage][t] = v;
			return orig<long(__stdcall *)(uintptr_t, uint32_t, uint32_t, uint32_t)>(kSetTextureStageState)(dev, stage, t, v);
		}
		long __stdcall h_set_texture(uintptr_t dev, uint32_t stage, uintptr_t tex)
		{
			if (stage < 2)
				g.st.tex[stage] = tex;
			return orig<long(__stdcall *)(uintptr_t, uint32_t, uintptr_t)>(kSetTexture)(dev, stage, tex);
		}
		long __stdcall h_set_viewport(uintptr_t dev, const uint32_t *vp)
		{
			if (vp != nullptr)
				std::memcpy(g.st.vp, vp, 16);
			return orig<long(__stdcall *)(uintptr_t, const uint32_t *)>(kSetViewport)(dev, vp);
		}
		long __stdcall h_set_vertex_shader(uintptr_t dev, uint32_t h)
		{
			g.st.vs = h;
			return orig<long(__stdcall *)(uintptr_t, uint32_t)>(kSetVertexShader)(dev, h);
		}
		long __stdcall h_set_pixel_shader(uintptr_t dev, uint32_t h)
		{
			g.st.ps = h;
			return orig<long(__stdcall *)(uintptr_t, uint32_t)>(kSetPixelShader)(dev, h);
		}
		long __stdcall h_apply_state_block(uintptr_t dev, uint32_t token)
		{
			const long r = orig<long(__stdcall *)(uintptr_t, uint32_t)>(kApplyStateBlock)(dev, token);
			if (!query_state(dev))
			{
				// (can't follow it any more: the rest of the window is the game's)
				cut();
				g.st.markable = false;
			}
			return r;
		}
		long __stdcall h_set_render_target(uintptr_t dev, uintptr_t rt, uintptr_t ds)
		{
			cut();
			const long r = orig<long(__stdcall *)(uintptr_t, uintptr_t, uintptr_t)>(kSetRenderTarget)(dev, rt, ds);
			uint32_t vp[6] = {};
			if (!query_target(dev) || orig<getv_fn>(kGetViewport)(dev, vp) < 0)
				g.st.markable = false;
			else
				std::memcpy(g.st.vp, vp, 16);
			return r;
		}
		long __stdcall h_clear(uintptr_t dev, uint32_t count, const int32_t *rects, uint32_t flags, uint32_t colour, float z, uint32_t stencil)
		{
			cut();
			return orig<clear_fn>(kClear)(dev, count, rects, flags, colour, z, stencil);
		}
		long __stdcall h_dp(uintptr_t dev, uint32_t type, uint32_t start, uint32_t count)
		{
			cut();
			passed(rOtherDraw);
			return orig<long(__stdcall *)(uintptr_t, uint32_t, uint32_t, uint32_t)>(kDP)(dev, type, start, count);
		}
		long __stdcall h_dip(uintptr_t dev, uint32_t type, uint32_t min, uint32_t vertices, uint32_t start, uint32_t count)
		{
			cut();
			passed(rOtherDraw);
			return orig<long(__stdcall *)(uintptr_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>(kDIP)(dev, type, min, vertices, start, count);
		}
		long __stdcall h_dipup(uintptr_t dev, uint32_t type, uint32_t min, uint32_t vertices, uint32_t count, const void *indices, uint32_t format,
			const void *data, uint32_t stride)
		{
			cut();
			passed(rOtherDraw);
			return orig<long(__stdcall *)(uintptr_t, uint32_t, uint32_t, uint32_t, uint32_t, const void *, uint32_t, const void *, uint32_t)>(kDIPUP)(
				dev, type, min, vertices, count, indices, format, data, stride);
		}
		long __stdcall h_dpup(uintptr_t dev, uint32_t type, uint32_t count, const void *data, uint32_t stride)
		{
			const reason r = record(type, count, data, stride);
			if (r == rOk)
			{
				if (g.native)
				{
					g.n.native++;
					return 0;
				}
			}
			else
			{
				cut();
				passed(r);
			}
			return orig<long(__stdcall *)(uintptr_t, uint32_t, uint32_t, const void *, uint32_t)>(kDPUP)(dev, type, count, data, stride);
		}

		void build_copy(uint32_t vt)
		{
			const uint32_t *const src = reinterpret_cast<const uint32_t *>(uintptr_t(vt));
			g_copy[0] = src[-1];
			for (uint32_t i = 0; i < kMethods; ++i)
				g_copy[1 + i] = g_orig[i] = src[i];
			const auto put = [](uint32_t i, auto fn) { g_copy[1 + i] = uint32_t(reinterpret_cast<uintptr_t>(fn)); };
			put(kSetRenderState, &h_set_render_state);
			put(kSetTextureStageState, &h_set_stage_state);
			put(kSetTexture, &h_set_texture);
			put(kSetViewport, &h_set_viewport);
			put(kSetVertexShader, &h_set_vertex_shader);
			put(kSetPixelShader, &h_set_pixel_shader);
			put(kApplyStateBlock, &h_apply_state_block);
			put(kSetRenderTarget, &h_set_render_target);
			put(kClear, &h_clear);
			put(kDP, &h_dp);
			put(kDIP, &h_dip);
			put(kDIPUP, &h_dipup);
			put(kDPUP, &h_dpup);
		}

		uint32_t copy_address()
		{
			return uint32_t(reinterpret_cast<uintptr_t>(&g_copy[1]));
		}

		void close()
		{
			if (!g.open)
				return;
			if (g.dev != 0)
			{
				if (word(g.dev) == copy_address())
					*reinterpret_cast<uint32_t *>(g.dev) = g.source_vt;
				else
					g.last_why = "something else replaced the device's vtable during the window: left as it is";
			}
			g.open = false;
			g.f = nullptr;
			g.dev = 0;
			if ((g.seq & 63) == 0)
				forget_unused();
			g.n.held = uint32_t(g.textures.size());
		}

		uintptr_t device_of(uintptr_t wrapper)
		{
			if (g.device_override != 0)
				return g.device_override;
			return wrapper != 0 ? word(wrapper + 0xC) : 0;
		}

		void __cdecl on_begin(uintptr_t wrapper)
		{
			window_begin(device_of(wrapper));
		}
		void __cdecl on_screen(uintptr_t wrapper)
		{
			window_begin(device_of(wrapper), true);
		}
		void __cdecl on_flush(uint32_t kind, uintptr_t)
		{
			window_flush(kind & 0xFF);
			if ((kind & kClose) != 0)
				close();
		}

		void put_rel(uint8_t *at, uintptr_t from, uint8_t opcode, uintptr_t to)
		{
			at[0] = opcode;
			put32(at + 1, uint32_t(to - (from + 5)));
		}
	}

	void window_begin(uintptr_t dev, bool screen)
	{
		if (g.open)
			window_end(); // (one not closed: closed now)
		if (g.wanted == off || dev == 0)
			return;
		if (g.movie != 0 && word(g.movie) != 0)
		{
			g.n.movie++;
			return;
		}
		const uint32_t vt = word(dev);
		if (!pointer(vt) || vt == copy_address())
		{
			g.n.no_window++;
			return;
		}
		build_copy(vt);
		g.st = device_state();
		if (!query_state(dev) || !(g.sink != nullptr || g.st.markable))
		{
			g.n.no_window++;
			return;
		}
		g.source_vt = vt;
		g.dev = dev;
		g.seq++;
		if (g.seq == 0)
			g.seq = 1;
		g.f = &u2::begin(g.seq);
		g.seg_first = 0;
		g.native = g.wanted == native;
		g.cut_kind = screen ? slotmark::screen : slotmark::ui_end;
		if (!screen)
			g.n.frame_draws = g.n.frame_segments = 0;
		g.n.windows++;
		g.n.seq = g.seq;
		*reinterpret_cast<uint32_t *>(dev) = copy_address();
		g.open = true;
		// (the back buffer's window marks its start: the Direct3D 12 side counts its segments from there; A's is named by
		// its seq alone)
		if (!screen)
			mark(slotmark::ui_begin);
	}

	void window_flush(uint32_t kind)
	{
		if (!g.open)
			return;
		flush(kind);
		if (kind == slotmark::frame_end)
			close();
	}

	void window_end()
	{
		window_flush(g.cut_kind == slotmark::screen ? slotmark::screen : slotmark::frame_end);
		close();
	}

	bool start(std::string &why)
	{
		if (g.in)
			return true;
		const uintptr_t b = find(kBegin), e = find(kEnd), fr = find(kFrame), sc = find(kScreen), po = find(kPop), mo = find(kMovie);
		if (b == 0 || e == 0 || fr == 0 || sc == 0 || po == 0 || mo == 0)
		{
			why = "the loop tail's calls weren't found";
			return false;
		}
		const uintptr_t calls[kCalls] = { b + kBeginAt, e + kEndAt, fr + kFrameAt, sc + kScreenAt, po };
		const uintptr_t wrapper_global = word(sc + kScreenWrapperAt);
		uint8_t now[kCalls][5] = {};
		uintptr_t targets[kCalls] = {};
		for (int i = 0; i < kCalls; ++i)
		{
			if (!read(calls[i], now[i], 5) || now[i][0] != 0xE8)
			{
				why = "the loop tail's calls aren't calls";
				return false;
			}
			targets[i] = rel_target(calls[i]);
		}
		uint8_t crossfade[sizeof(kCrossfade)] = {};
		read(targets[3], crossfade, sizeof(crossfade));
		std::memset(crossfade + 1, 0, 4);
		if (!code_is(targets[0], kEmpty, sizeof(kEmpty)) || !code_is(targets[1], kFade, sizeof(kFade)) || !code_is(targets[2], kEndScene, sizeof(kEndScene)) ||
			std::memcmp(crossfade, kCrossfade, sizeof(kCrossfade)) != 0 || !code_is(targets[4], kPopA, sizeof(kPopA)) || !pointer(uint32_t(wrapper_global)))
		{
			why = "the loop tail's calls don't go where they should (00BB20, the fade 00A820, EndScene 009740, the crossfade 012F40, A's pop 00A1D0)";
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
		uint8_t s[kCodeSize];
		std::memset(s, 0xCC, sizeof(s));
		// begin: push ecx; call 00BB20; pop ecx; push eax; push ecx; push edx; push ecx; call on_begin; add esp, 4; pop edx; pop ecx; pop eax; ret
		const uint8_t begin[] = { 0x51, 0, 0, 0, 0, 0, 0x59, 0x50, 0x51, 0x52, 0x51, 0, 0, 0, 0, 0, 0x83, 0xC4, 0x04, 0x5A, 0x59, 0x58, 0xC3 };
		std::memcpy(s, begin, sizeof(begin));
		put_rel(s + 1, g.code + 1, 0xE8, targets[0]);
		put_rel(s + 0xB, g.code + 0xB, 0xE8, reinterpret_cast<uintptr_t>(&on_begin));
		// end, frame: push eax; push ecx; push edx; push ecx; push kind; call on_flush; add esp, 8; pop edx; pop ecx; pop eax; jmp the game's
		for (int k = 0; k < 2; ++k)
		{
			const size_t at = k == 0 ? kEndStub : kFrameStub;
			const uint8_t stub[] = { 0x50, 0x51, 0x52, 0x51, 0x6A, uint8_t(k == 0 ? slotmark::ui_end : slotmark::frame_end), 0, 0, 0, 0, 0, 0x83, 0xC4, 0x08,
				0x5A, 0x59, 0x58, 0, 0, 0, 0, 0 };
			std::memcpy(s + at, stub, sizeof(stub));
			put_rel(s + at + 6, g.code + at + 6, 0xE8, reinterpret_cast<uintptr_t>(&on_flush));
			put_rel(s + at + 0x11, g.code + at + 0x11, 0xE9, targets[1 + k]);
		}
		// screen: push eax; push ecx; push edx; push [wrapper]; call on_screen; add esp, 4; pop edx; pop ecx; pop eax; jmp 012F40
		const uint8_t screen[] = { 0x50, 0x51, 0x52, 0xFF, 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x83, 0xC4, 0x04, 0x5A, 0x59, 0x58, 0, 0, 0, 0, 0 };
		std::memcpy(s + kScreenStub, screen, sizeof(screen));
		put32(s + kScreenStub + 5, uint32_t(wrapper_global));
		put_rel(s + kScreenStub + 9, g.code + kScreenStub + 9, 0xE8, reinterpret_cast<uintptr_t>(&on_screen));
		put_rel(s + kScreenStub + 0x14, g.code + kScreenStub + 0x14, 0xE9, targets[3]);
		// pop: push eax; push ecx; push edx; push ecx; push 108h; call on_flush; add esp, 8; pop edx; pop ecx; pop eax; jmp 00A1D0
		const uint8_t pop[] = { 0x50, 0x51, 0x52, 0x51, 0x68, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x83, 0xC4, 0x08, 0x5A, 0x59, 0x58, 0, 0, 0, 0, 0 };
		std::memcpy(s + kPopStub, pop, sizeof(pop));
		put32(s + kPopStub + 5, slotmark::screen | kClose);
		put_rel(s + kPopStub + 9, g.code + kPopStub + 9, 0xE8, reinterpret_cast<uintptr_t>(&on_flush));
		put_rel(s + kPopStub + 0x14, g.code + kPopStub + 0x14, 0xE9, targets[4]);
		if (!write_code(g.code, s, kCodeSize))
		{
			why = "couldn't write the stubs";
			return false;
		}
		const uintptr_t stubs[kCalls] = { g.code, g.code + kEndStub, g.code + kFrameStub, g.code + kScreenStub, g.code + kPopStub };
		for (int i = 0; i < kCalls; ++i)
		{
			put_rel(g.ours[i], calls[i], 0xE8, stubs[i]);
			if (!write_code(calls[i], g.ours[i], 5))
			{
				for (int k = 0; k < i; ++k)
					write_code(calls[k], now[k], 5);
				why = "couldn't write the game's code";
				return false;
			}
			std::memcpy(g.saved[i], now[i], 5);
		}
		std::memcpy(g.calls, calls, sizeof(calls));
		std::memcpy(g.targets, targets, sizeof(targets));
		g.movie = word(mo + kMovieAt);
		g.in = true;
		return true;
	}

	bool stop(std::string &why)
	{
		if (!g.in)
			return true;
		if (g.open)
			window_end();
		for (int i = 0; i < kCalls; ++i)
			if (!code_is(g.calls[i], g.ours[i], 5))
			{
				why = "something else has changed the loop tail's calls since: left as they are";
				return false;
			}
		for (int i = 0; i < kCalls; ++i)
			if (!write_code(g.calls[i], g.saved[i], 5))
			{
				why = "couldn't write the game's code";
				return false;
			}
		// (on the game's thread, between frames: no stub running)
		free_code(g.code);
		g.code = 0;
		g.in = false;
		drop_textures();
		u2::clear();
		return true;
	}

	bool in()
	{
		return g.in;
	}

	void set_mode(mode m)
	{
		g.wanted = m;
	}

	mode current()
	{
		return g.wanted;
	}

	counts get_counts()
	{
		return g.n;
	}

	const char *reason_name(uint32_t r)
	{
		return r < rReasons ? kReasonNames[r] : "?";
	}

	std::vector<std::string> status()
	{
		std::vector<std::string> lines;
		char line[400];
		if (!g.in)
		{
			lines.push_back("screen 2D: not recorded (the loop tail's calls aren't ours)");
			return lines;
		}
		std::snprintf(line, sizeof(line), "screen 2D: %s; %llu windows (%llu not opened, %llu during the movie), %llu draws recorded (%llu left out of the game's), %llu left to the game in %llu cuts; "
			"last window %u draws in %u segments; %llu segments, markers %llu sent%s",
			g.wanted == off ? "off" : g.wanted == shadow ? "recorded, the game draws (shadow)" : "HiFi XI draws it", static_cast<unsigned long long>(g.n.windows),
			static_cast<unsigned long long>(g.n.no_window), static_cast<unsigned long long>(g.n.movie), static_cast<unsigned long long>(g.n.recorded), static_cast<unsigned long long>(g.n.native),
			static_cast<unsigned long long>(g.n.passed), static_cast<unsigned long long>(g.n.cuts), g.n.frame_draws, g.n.frame_segments,
			static_cast<unsigned long long>(g.n.segments), static_cast<unsigned long long>(g.n.markers),
			g.n.not_marked != 0 ? (", " + std::to_string(g.n.not_marked) + " not").c_str() : "");
		lines.push_back(line);
		std::string why;
		for (uint32_t r = 1; r < rReasons; ++r)
			if (g.n.why[r] != 0)
				why += (why.empty() ? "" : ", ") + std::string(kReasonNames[r]) + " " + std::to_string(g.n.why[r]);
		if (!why.empty())
			lines.push_back("  left to the game: " + why);
		std::snprintf(line, sizeof(line), "  textures: %u held, %llu taken, %llu taken again (new pixels)%s", g.n.held, static_cast<unsigned long long>(g.n.textures),
			static_cast<unsigned long long>(g.n.texture_changes),
			g.n.upload_failures != 0 ? (", " + std::to_string(g.n.upload_failures) + " uploads failed").c_str() : "");
		lines.push_back(line);
		if (!g.last_why.empty())
			lines.push_back("  last: " + g.last_why);
		return lines;
	}

	void drop_textures()
	{
		for (auto &t : g.textures)
			let_go(t.first, t.second);
		g.textures.clear();
		g.n.held = 0;
	}

	sites where()
	{
		return { g.calls[0], g.targets[0], g.calls[1], g.targets[1], g.calls[2], g.targets[2], g.calls[3], g.targets[3], g.calls[4], g.targets[4],
			g.movie, g.in ? g.code : 0 };
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
